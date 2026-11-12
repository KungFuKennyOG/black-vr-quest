/*
 * grx_perf.c -- tests/game_render_x86: per-interval performance accounting of the headless run (port/notes/perf.md).
 *
 * Threads: pthread_create and the runtime's CreateThread are wrapped at link time (-Wl,--wrap=...), so every thread
 * the game, the runtime, Kelvin, the Vulkan backend and the DSP core start is registered with its start routine; the
 * runtime's role threads also name themselves (xbox_PinThread: bx-main, bx-pusher, bx-worker, ...). CPU time per
 * thread comes from /proc/self/task/<tid>/schedstat (ns). SwiftShader's own threads (started inside the ICD, not
 * wrapped) are reported together as "driver".
 *
 * Faults: x86_fault_glue.c's table (every serviced SIGSEGV by guest address, direction and thread, with the handler
 * time). Kelvin: grx_kstats.c.
 *
 * Output, per interval (grx_perf_report), lines starting "[PERF]":
 *   [PERF] interval <mode> wall <s> flips <n> words <n> methods <n> draws <n> faults <n> fault_us <n>
 *   [PERF]   cpu <thread> <ms> (<ms/flip>)
 *   [PERF]   fault <device> <reg> <R|W> <thread> <n> (<n/flip>) <handler us/access>
 * tests/game_render_x86/perf_summary.py aggregates them by mode.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "grx_perf.h"
#if defined(GRX_COUNT_CALLS)
volatile unsigned long long g_grx_guest_calls;
unsigned int g_grx_call_hist[1u << 22];
#endif
#if defined(GRX_CALL_TRACE)
/* -DGRX_CALL_TRACE (equivalence check of a lifter/header change, port/notes/perf.md 9): on the game thread, an FNV-1a
 * hash over every guest call's target and the guest registers at the call, printed at every power of two calls. Two
 * builds that lift the same way print the same hashes for as long as the run is deterministic (until timing reaches
 * guest state; compare two runs of one build to see where that is). */
#include "recomp_types.h"
static __thread int t_trace_on;
static __thread uint64_t t_trace_h = 1469598103934665603ull, t_trace_n;
static void trace_mix(uint32_t v) { int i; for (i = 0; i < 4; i++) { t_trace_h ^= (v >> (8 * i)) & 0xFF; t_trace_h *= 1099511628211ull; } }
void grx_call_trace(uint32_t va)
{
    if (!t_trace_on) return;
    trace_mix(va); trace_mix(g_esp); trace_mix(g_eax); trace_mix(g_ecx); trace_mix(g_edx);
    trace_mix(g_ebx); trace_mix(g_esi); trace_mix(g_edi);
    if (((++t_trace_n) & (t_trace_n - 1)) == 0)
        fprintf(stderr, "[TRACE] calls %llu hash %016llx\n", (unsigned long long)t_trace_n, (unsigned long long)t_trace_h);
    {
        /* GRX_TRACE_FROM/GRX_TRACE_TO: every call in that range, with the registers */
        static unsigned long long from = ~0ull, to;
        if (from == ~0ull) { const char *a = getenv("GRX_TRACE_FROM"), *b = getenv("GRX_TRACE_TO"); from = a ? strtoull(a, 0, 0) : 0; to = b ? strtoull(b, 0, 0) : 0; }
        if (t_trace_n >= from && t_trace_n < to)
            fprintf(stderr, "[CALL] %llu %08X esp %08X eax %08X ecx %08X edx %08X ebx %08X esi %08X edi %08X\n",
                    (unsigned long long)t_trace_n, va, g_esp, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi);
    }
}
#endif

/* ---- thread registry -------------------------------------------------------------------------------------------- */
#define GRX_THREADS 512
typedef struct GrxThread {
    int tid;
    void *fn;
    char fname[48];             /* start routine, resolved lazily */
    uint64_t cpu_prev;
} GrxThread;
static GrxThread s_thr[GRX_THREADS];
static int s_nthr;
static pthread_mutex_t s_thr_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_game_tid;
static const char *s_exe;

static void thr_register(void *fn)
{
    int tid = (int)syscall(SYS_gettid), i;
    pthread_mutex_lock(&s_thr_lock);
    for (i = 0; i < s_nthr; i++) if (s_thr[i].tid == tid) break;
    if (i == s_nthr && s_nthr < GRX_THREADS) s_nthr++;
    if (i < GRX_THREADS) { s_thr[i].tid = tid; s_thr[i].fn = fn; s_thr[i].fname[0] = 0; }
    pthread_mutex_unlock(&s_thr_lock);
}

typedef struct Box { void *(*fn)(void *); void *arg; } Box;
static void *tramp_p(void *b)
{
    Box x = *(Box *)b;
    free(b);
    thr_register((void *)x.fn);
    return x.fn(x.arg);
}
int __real_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    Box *b = (Box *)malloc(sizeof(Box));
    int r;
    if (!b) return __real_pthread_create(t, a, fn, arg);
    b->fn = fn; b->arg = arg;
    r = __real_pthread_create(t, a, tramp_p, b);
    if (r) free(b);
    return r;
}

/* win32_compat's CreateThread (HANDLE, SIZE_T, DWORD are pointer / size_t / unsigned int there) */
typedef unsigned int (*W32Start)(void *);
typedef struct WBox { W32Start fn; void *arg; } WBox;
static unsigned int tramp_w(void *b)
{
    WBox x = *(WBox *)b;
    free(b);
    thr_register((void *)x.fn);
    return x.fn(x.arg);
}
void *__real_CreateThread(void *sa, size_t stack, W32Start fn, void *arg, unsigned int flags, unsigned int *id);
void *__wrap_CreateThread(void *sa, size_t stack, W32Start fn, void *arg, unsigned int flags, unsigned int *id)
{
    WBox *b = (WBox *)malloc(sizeof(WBox));
    void *h;
    if (!b) return __real_CreateThread(sa, stack, fn, arg, flags, id);
    b->fn = fn; b->arg = arg;
    h = __real_CreateThread(sa, stack, tramp_w, b, flags, id);
    if (!h) free(b);
    return h;
}

void grx_perf_set_game_tid(void)
{
    s_game_tid = (int)syscall(SYS_gettid);
#if defined(GRX_CALL_TRACE)
    t_trace_on = 1;
#endif
}
static char s_exe_buf[1024];
void grx_perf_init(const char *exe)
{
    ssize_t n = readlink(exe, s_exe_buf, sizeof(s_exe_buf) - 1);
    if (n > 0) { s_exe_buf[n] = 0; s_exe = s_exe_buf; } else s_exe = exe;
}

static void resolve(GrxThread *t)
{
    Dl_info di;
    char cmd[1400], out[256];
    FILE *p;
    if (t->fname[0]) return;
    snprintf(t->fname, sizeof(t->fname), "?");
    if (!t->fn) return;
    if (dladdr(t->fn, &di) && di.dli_sname) { snprintf(t->fname, sizeof(t->fname), "%s", di.dli_sname); return; }
    if (!s_exe || !dladdr(t->fn, &di)) return;
    snprintf(cmd, sizeof(cmd), "addr2line -f -e '%s' 0x%lx 2>/dev/null",
             di.dli_fname && di.dli_fname[0] == '/' && !strstr(di.dli_fname, "/proc/") ? di.dli_fname : s_exe,
             (unsigned long)((uintptr_t)t->fn - (uintptr_t)di.dli_fbase));
    if ((p = popen(cmd, "r")) != NULL) {
        if (fgets(out, sizeof(out), p)) {
            out[strcspn(out, "\n")] = 0;
            snprintf(t->fname, sizeof(t->fname), "%s", out);
        }
        pclose(p);
    }
}

static uint64_t task_cpu_ns(int tid, char *comm, size_t n)
{
    char path[96];
    unsigned long long ns = 0;
    FILE *f;
    comm[0] = 0;
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    if ((f = fopen(path, "r"))) { if (fgets(comm, (int)n, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
    snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    if ((f = fopen(path, "r"))) { if (fscanf(f, "%llu", &ns) != 1) ns = 0; fclose(f); }
    return ns;
}

/* A thread's reporting class: the role name it gave itself, else its start routine. */
static void thread_class(int tid, const char *comm, char *out, size_t n)
{
    int i;
    if (tid == s_game_tid) { snprintf(out, n, "game(black_game_main)"); return; }
    /* the role name a thread gave itself (bx-main, bx-pusher, ...); a thread inherits its creator's name, so the
     * start routine decides when the two disagree (the backend's compile workers are started by the pusher) */
    for (i = 0; i < s_nthr; i++)
        if (s_thr[i].tid == tid) {
            resolve(&s_thr[i]);
            if (!strncmp(comm, "bx-", 3) && (strstr(s_thr[i].fname, "kelvin_thread") || strstr(s_thr[i].fname, "kwork_thread") ||
                                            strstr(s_thr[i].fname, "nv2a_ack_thread") || strstr(s_thr[i].fname, "xbox_thread_wrapper") ||
                                            strstr(s_thr[i].fname, "vblank") || strstr(s_thr[i].fname, "timer")))
                snprintf(out, n, "%s", comm);
            else if (!strcmp(s_thr[i].fname, "xbox_thread_wrapper")) snprintf(out, n, "guest-thread");
            else snprintf(out, n, "%s", s_thr[i].fname);
            return;
        }
    if (!strncmp(comm, "bx-", 3)) { snprintf(out, n, "%s", comm); return; }
    if (tid == (int)getpid()) { snprintf(out, n, "xr-thread(harness)"); return; }
    snprintf(out, n, "driver:%s", comm);      /* SwiftShader / loader threads */
}

/* ---- the report ------------------------------------------------------------------------------------------------- */
#define CLS 64
typedef struct Agg { char name[48]; uint64_t ns; int n; } Agg;
typedef struct TidPrev { int tid; uint64_t ns; } TidPrev;
static TidPrev s_prev_cpu[1024];
static int s_nprev;
static GrxFaultSlot s_fprev[GRX_FAULT_SLOTS];
static uint64_t s_kprev[8], s_ftot_prev, s_fns_prev;

static uint64_t prev_cpu(int tid, uint64_t now)
{
    int i;
    for (i = 0; i < s_nprev; i++)
        if (s_prev_cpu[i].tid == tid) { uint64_t p = s_prev_cpu[i].ns; s_prev_cpu[i].ns = now; return p; }
    if (s_nprev < 1024) { s_prev_cpu[s_nprev].tid = tid; s_prev_cpu[s_nprev].ns = now; s_nprev++; }
    return 0;
}

static const char *device_of(uint32_t va, char *reg, size_t n)
{
    if (va >= 0xFD000000u && va < 0xFE000000u) {
        uint32_t off = va - 0xFD000000u;
        snprintf(reg, n, "0x%06X", off);
        if (off >= 0x800000 && off < 0x810000) return "NV2A-USER";       /* DMA_PUT/GET page: the PUT trap */
        if (off < 0x1000) return "NV2A-PMC";
        if (off >= 0x2000 && off < 0x4000) return "NV2A-PFIFO";
        if (off >= 0x400000 && off < 0x402000) return "NV2A-PGRAPH";
        if (off >= 0x600000 && off < 0x601000) return "NV2A-PCRTC";
        if (off >= 0x680000 && off < 0x681000) return "NV2A-PRAMDAC";
        if (off >= 0x7000 && off < 0x8000) return "NV2A-PRMDIO";
        return "NV2A";
    }
    if (va >= 0xFE800000u && va < 0xFE880000u) { snprintf(reg, n, "0x%05X", va - 0xFE800000u); return "APU"; }
    if (va >= 0xFEC00000u && va < 0xFEC01000u) { snprintf(reg, n, "0x%03X", va - 0xFEC00000u); return "AC97"; }
    if (va >= 0xFED00000u && va < 0xFED10000u) { snprintf(reg, n, "0x%05X", va - 0xFED00000u); return "OHCI"; }
    snprintf(reg, n, "0x%08X", va);
    return "other";
}

typedef struct FRow { uint64_t key, n, ns; } FRow;
static int frow_cmp(const void *a, const void *b)
{
    const FRow *x = (const FRow *)a, *y = (const FRow *)b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

void grx_perf_report(const char *mode, double wall_dt)
{
    uint64_t k[8], dk[8], ftot, fns;
    Agg agg[CLS];
    int nagg = 0, i, j;
    DIR *d;
    struct dirent *de;
    static FRow rows[GRX_FAULT_SLOTS];
    int nrows = 0;
    double flips;

    grx_kelvin_counters(k);
    for (i = 0; i < 8; i++) { dk[i] = k[i] - s_kprev[i]; s_kprev[i] = k[i]; }
    flips = (double)dk[2];
    ftot = g_grx_fault_total; fns = g_grx_fault_ns;
    fprintf(stderr, "[PERF] interval %s wall %.2f flips %llu words %llu methods %llu draws %llu vertices %llu faults %llu fault_us %.0f"
                    " unclaimed %llu\n", mode, wall_dt, (unsigned long long)dk[2], (unsigned long long)dk[0],
            (unsigned long long)dk[1], (unsigned long long)dk[3], (unsigned long long)dk[5],
            (unsigned long long)(ftot - s_ftot_prev), (fns - s_fns_prev) / 1000.0, (unsigned long long)g_grx_fault_unclaimed);
    s_ftot_prev = ftot; s_fns_prev = fns;
#if defined(GRX_COUNT_CALLS)
    {
        static unsigned long long prev;
        const unsigned long long c = g_grx_guest_calls;
        static unsigned int *hprev;
        const char *hp = getenv("GRX_CALL_HIST");
        fprintf(stderr, "[PERF]   guest direct calls %llu (%.0f per flip)\n", c - prev, flips > 0 ? (c - prev) / flips : 0.0);
        prev = c;
        /* GRX_CALL_HIST=<file>: per interval, "interval <mode> <flips>" then "<va> <calls>" for every guest function called */
        if (hp && *hp) {
            FILE *hf = fopen(hp, "a");
            unsigned int i2;
            if (!hprev) hprev = (unsigned int *)calloc(1u << 22, sizeof(unsigned int));
            if (hf && hprev) {
                fprintf(hf, "interval %s %.0f\n", mode, flips);
                for (i2 = 0; i2 < (1u << 22); i2++) {
                    const unsigned int v = g_grx_call_hist[i2];
                    if (v != hprev[i2]) { fprintf(hf, "%06X %u\n", i2, v - hprev[i2]); hprev[i2] = v; }
                }
            }
            if (hf) fclose(hf);
        }
    }
#endif
    {
        /* memory: what an Android LMK looks at (RSS/PSS; shmem = the guest RAM memfd, mapped 30 times, counted once)
         * and the address space (the 4 GB PROT_NONE guest window is VmSize only). SwiftShader's device memory is
         * part of RssAnon here; on the headset it is GPU (kgsl) memory. */
        char line[256];
        long vm_size = -1, vm_rss = -1, rss_anon = -1, rss_file = -1, rss_shmem = -1, pss = -1, maps = 0;
        FILE *f;
        if ((f = fopen("/proc/self/status", "r")) != NULL) {
            while (fgets(line, sizeof(line), f)) {
                sscanf(line, "VmSize: %ld", &vm_size); sscanf(line, "VmRSS: %ld", &vm_rss);
                sscanf(line, "RssAnon: %ld", &rss_anon); sscanf(line, "RssFile: %ld", &rss_file);
                sscanf(line, "RssShmem: %ld", &rss_shmem);
            }
            fclose(f);
        }
        if ((f = fopen("/proc/self/smaps_rollup", "r")) != NULL) {
            while (fgets(line, sizeof(line), f)) sscanf(line, "Pss: %ld", &pss);
            fclose(f);
        }
        if ((f = fopen("/proc/self/maps", "r")) != NULL) {
            while (fgets(line, sizeof(line), f)) if (strchr(line, '\n')) maps++;
            fclose(f);
        }
        fprintf(stderr, "[PERF]   mem RSS %ld MB (anon %ld, file %ld, shmem %ld) PSS %ld MB, VmSize %ld MB, %ld mappings\n",
                vm_rss / 1024, rss_anon / 1024, rss_file / 1024, rss_shmem / 1024, pss / 1024, vm_size / 1024, maps);
    }

    /* CPU per thread class */
    if ((d = opendir("/proc/self/task")) != NULL) {
        while ((de = readdir(d)) != NULL) {
            char comm[32], cls[48];
            int tid = atoi(de->d_name);
            uint64_t now, dn;
            if (tid <= 0) continue;
            now = task_cpu_ns(tid, comm, sizeof(comm));
            dn = now - prev_cpu(tid, now);
            thread_class(tid, comm, cls, sizeof(cls));
            for (j = 0; j < nagg; j++) if (!strcmp(agg[j].name, cls)) break;
            if (j == nagg) { if (nagg == CLS) continue; snprintf(agg[j].name, sizeof(agg[j].name), "%s", cls); agg[j].ns = 0; agg[j].n = 0; nagg++; }
            agg[j].ns += dn; agg[j].n++;
        }
        closedir(d);
    }
    for (i = 0; i < nagg; i++)
        if (agg[i].ns >= 1000000)
            fprintf(stderr, "[PERF]   cpu %-28s x%-2d %9.1f ms  %8.2f ms/flip  %5.1f%%\n", agg[i].name, agg[i].n, agg[i].ns / 1e6,
                    flips > 0 ? agg[i].ns / 1e6 / flips : 0.0, wall_dt > 0 ? agg[i].ns / 1e7 / wall_dt : 0.0);

    /* faults by device / register / direction / thread */
    for (i = 0; i < GRX_FAULT_SLOTS; i++) {
        const uint64_t key = __atomic_load_n(&g_grx_faults[i].key, __ATOMIC_ACQUIRE);
        uint64_t n, ns;
        if (!key) continue;
        n = __atomic_load_n(&g_grx_faults[i].n, __ATOMIC_RELAXED);
        ns = __atomic_load_n(&g_grx_faults[i].ns, __ATOMIC_RELAXED);
        if (s_fprev[i].key != key) { s_fprev[i].key = key; s_fprev[i].n = 0; s_fprev[i].ns = 0; }
        if (n > s_fprev[i].n) { rows[nrows].key = key; rows[nrows].n = n - s_fprev[i].n; rows[nrows].ns = ns - s_fprev[i].ns; nrows++; }
        s_fprev[i].n = n; s_fprev[i].ns = ns;
    }
    qsort(rows, (size_t)nrows, sizeof(rows[0]), frow_cmp);
    for (i = 0; i < nrows && i < 40; i++) {
        char reg[24], comm[32], cls[48];
        const int tid = (int)(rows[i].key >> 33);
        const uint32_t va = (uint32_t)(rows[i].key >> 1);
        const char *dev = device_of(va, reg, sizeof(reg));
        task_cpu_ns(tid, comm, sizeof(comm));
        thread_class(tid, comm, cls, sizeof(cls));
        fprintf(stderr, "[PERF]   fault %-11s %-9s %c %-24s %9llu  %9.1f/flip  %6.2f us\n", dev, reg, (rows[i].key & 1) ? 'W' : 'R',
                cls, (unsigned long long)rows[i].n, flips > 0 ? rows[i].n / flips : 0.0, rows[i].ns / 1000.0 / (double)rows[i].n);
    }
}
