/*
 * Thread placement on hybrid CPUs.
 *
 * The recompiled game runs a handful of threads that each need a whole fast core (the guest main thread, the NV2A
 * pusher and its D3D submitter) next to threads that poll or sleep (the register acknowledger, the vblank timer,
 * audio). On a CPU with performance and efficiency cores, and with SMT, the scheduler is free to put two of the busy
 * ones on the two hardware threads of one core, or one of them on an efficiency core: either costs a large part of
 * a frame's CPU time. xbox_PinThread(role) gives each role a core of its own.
 *
 *   RECOMP_THREAD_PLACEMENT=0       leave placement to the scheduler
 *   RECOMP_PIN_<ROLE>=<mask>        explicit affinity mask (hex, group 0) for a role, overriding the automatic choice
 *
 * Roles: MAIN, PUSHER, GPU, VBLANK, ACK, WORKER. Automatic placement needs a CPU with at least two efficiency
 * classes (a hybrid part): the busiest roles take performance cores in that order, one physical core each (both of its
 * hardware threads stay in the mask, so the thread may move between them but nothing else is put on that core on
 * purpose), and the polling roles take the efficiency cores.
 */
#if !defined(_WIN32)
/* ---- POSIX (Linux / Android) -------------------------------------------
 *
 * Same roles and the same RECOMP_PIN_<ROLE>=<hex mask> override, with the
 * topology read from cpufreq: on a big.LITTLE SoC the cores with the highest
 * cpuinfo_max_freq are the performance cluster. Quest 3's XR2 Gen 2 is
 * 1 + 4 + 2 (prime/gold/silver); the three busy roles take the fastest cores in
 * descending order, the polling roles share the slowest cluster.
 *
 * On Android the XR runtime also wants its own threads on the big cores, and
 * the OS may refuse a mask outside the app's cpuset; sched_setaffinity then
 * fails and the thread is left where the scheduler put it, which is the
 * pre-existing behaviour. RECOMP_THREAD_PLACEMENT=0 disables it entirely. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define TP_MAX_CPUS 64
static int s_ncpu;
static long s_freq[TP_MAX_CPUS];
static int s_order[TP_MAX_CPUS];        /* cpus, fastest first */
static int s_ready, s_mode = -1;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static void topology_init(void)
{
    s_ready = 1;
    s_ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (s_ncpu > TP_MAX_CPUS) s_ncpu = TP_MAX_CPUS;
    for (int i = 0; i < s_ncpu; i++) {
        char path[96];
        FILE *f;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        s_freq[i] = 0;
        if ((f = fopen(path, "r"))) {
            if (fscanf(f, "%ld", &s_freq[i]) != 1) s_freq[i] = 0;
            fclose(f);
        }
        s_order[i] = i;
    }
    /* stable sort by frequency, descending; ties keep cpu order */
    for (int i = 1; i < s_ncpu; i++)
        for (int j = i; j > 0 && s_freq[s_order[j]] > s_freq[s_order[j - 1]]; j--) {
            int t = s_order[j]; s_order[j] = s_order[j - 1]; s_order[j - 1] = t;
        }
}

static const char *const s_roles[] = { "MAIN", "PUSHER", "GPU", "VBLANK", "ACK", "WORKER" };

void xbox_PinThread(const char *role)
{
    char name[48];
    const char *e;
    unsigned long long mask = 0;
    int r = -1;

    if (s_mode < 0) {
        const char *m = getenv("RECOMP_THREAD_PLACEMENT");
        s_mode = !(m && *m == '0');
    }
    for (int i = 0; i < (int)(sizeof(s_roles) / sizeof(s_roles[0])); i++)
        if (!strcmp(role, s_roles[i])) r = i;
    {
        /* The OS thread name ("bx-pusher", ...), for top/perfetto/simpleperf on the device and the headless
         * run's per-thread CPU accounting (15 characters at most). */
        char tn[16];
        snprintf(tn, sizeof(tn), "bx-%s", role);
        for (int k = 3; tn[k]; k++)
            if (tn[k] >= 'A' && tn[k] <= 'Z') tn[k] = (char)(tn[k] - 'A' + 'a');
        pthread_setname_np(pthread_self(), tn);
    }
    snprintf(name, sizeof(name), "RECOMP_PIN_%s", role);
    e = getenv(name);
    if (e && *e) {
        mask = strtoull(e, NULL, 16);
    } else if (s_mode && r >= 0) {
        pthread_mutex_lock(&s_lock);
        if (!s_ready) topology_init();
        /* Only meaningful on a heterogeneous part. */
        if (s_ncpu > 1 && s_freq[s_order[0]] > s_freq[s_order[s_ncpu - 1]]) {
            long slow = s_freq[s_order[s_ncpu - 1]];
            if (r <= 2 && r < s_ncpu) {
                mask = 1ull << s_order[r];          /* MAIN, PUSHER, GPU */
            } else {
                for (int i = 0; i < s_ncpu; i++)     /* the efficiency cluster */
                    if (s_freq[i] == slow) mask |= 1ull << i;
            }
        }
        pthread_mutex_unlock(&s_lock);
    }
    if (mask) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int i = 0; i < TP_MAX_CPUS; i++)
            if (mask & (1ull << i)) CPU_SET(i, &set);
        int ok = sched_setaffinity(0, sizeof(set), &set) == 0;
        static int said[8];
        if (r >= 0 && r < 8 && !said[r]) {
            said[r] = 1;
            fprintf(stderr, "[THREAD] %s on cpus 0x%llX%s\n", role, mask, ok ? "" : " (refused)");
        }
    }
}

#else /* _WIN32 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PCore { KAFFINITY mask; BYTE eff; } PCore;

static PCore s_core[128];
static int s_ncores;
static BYTE s_max_eff, s_min_eff;
static int s_ready;
static int s_mode = -1;                 /* 0 off, 1 automatic */
static KAFFINITY s_pmask, s_emask;      /* every performance / efficiency hardware thread */
static int s_next_perf;

static void topology_init(void)
{
    DWORD len = 0;
    BYTE *buf, *p;
    s_ready = 1;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (!len) return;
    buf = (BYTE *)malloc(len);
    if (!buf) return;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf);
        return;
    }
    s_min_eff = 255;
    for (p = buf; p < buf + len;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
        if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1 &&
            e->Processor.GroupMask[0].Group == 0 && s_ncores < 128) {
            s_core[s_ncores].mask = e->Processor.GroupMask[0].Mask;
            s_core[s_ncores].eff = e->Processor.EfficiencyClass;
            if (e->Processor.EfficiencyClass > s_max_eff) s_max_eff = e->Processor.EfficiencyClass;
            if (e->Processor.EfficiencyClass < s_min_eff) s_min_eff = e->Processor.EfficiencyClass;
            s_ncores++;
        }
        p += e->Size;
    }
    free(buf);
    if (s_max_eff > s_min_eff) {
        int i;
        for (i = 0; i < s_ncores; i++) {
            if (s_core[i].eff == s_max_eff) s_pmask |= s_core[i].mask;
            else s_emask |= s_core[i].mask;
        }
        /* The performance cores in logical-processor order. */
        for (i = 0; i < s_ncores; i++) {
            int j;
            for (j = i + 1; j < s_ncores; j++) {
                if (s_core[j].mask < s_core[i].mask) { PCore t = s_core[i]; s_core[i] = s_core[j]; s_core[j] = t; }
            }
        }
    }
}

static const char *const s_roles[] = { "MAIN", "PUSHER", "GPU", "VBLANK", "ACK", "WORKER" };

void xbox_PinThread(const char *role)
{
    char name[48];
    const char *e;
    KAFFINITY mask = 0;
    int r = -1, i;

    if (s_mode < 0) {
        const char *m = getenv("RECOMP_THREAD_PLACEMENT");
        s_mode = !(m && *m == '0');
    }
    for (i = 0; i < (int)(sizeof(s_roles) / sizeof(s_roles[0])); i++) if (!strcmp(role, s_roles[i])) r = i;
    snprintf(name, sizeof(name), "RECOMP_PIN_%s", role);
    e = getenv(name);
    if (e && *e) {
        mask = (KAFFINITY)strtoull(e, NULL, 16);
    } else if (s_mode) {
        static volatile LONG lock;
        while (InterlockedExchange(&lock, 1)) Sleep(0);
        if (!s_ready) topology_init();
        for (i = 0; i < (int)(sizeof(s_roles) / sizeof(s_roles[0])); i++) if (!strcmp(role, s_roles[i])) r = i;
        if (s_pmask && s_emask && r >= 0) {
            /* performance cores in order for the three busy roles; the rest share the efficiency cores */
            if (r <= 3) {
                int k = 0, want = r;      /* MAIN 0, PUSHER 1, GPU 2, VBLANK 3 */
                for (i = 0; i < s_ncores; i++) {
                    if (s_core[i].eff != s_max_eff) continue;
                    if (k++ == want) { mask = s_core[i].mask; break; }
                }
            } else {
                mask = s_emask;
            }
        }
        InterlockedExchange(&lock, 0);
    }
    if (mask) {
        KAFFINITY old = SetThreadAffinityMask(GetCurrentThread(), mask);
        static int said[8];
        if (r >= 0 && r < 8 && !said[r]) {
            said[r] = 1;
            fprintf(stderr, "[THREAD] %s on logical processors 0x%llX%s\n", role, (unsigned long long)mask, old ? "" : " (failed)");
        }
    }
}

#endif /* _WIN32 */
