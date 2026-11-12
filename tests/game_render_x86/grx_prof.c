/*
 * grx_prof.c -- tests/game_render_x86: a sampling profiler of the game thread (port/notes/perf.md, section 9) and,
 * optionally, of the APU frame thread (section 10).
 *
 * There is no `perf` in this container, so the game thread samples itself: a software perf event on the thread's
 * own task clock (perf_event_open PERF_COUNT_SW_TASK_CLOCK, user time only, which paranoid level 2 allows; hrtimer
 * based, so the period is exact) raises SIGPROF on that thread (F_SETOWN_EX) every GRX_PROF_US microseconds of *its* CPU
 * time (default 250). Without perf events it falls back to a POSIX CPU-clock timer, which this kernel only checks at
 * its 250 Hz tick (one sample per 4 ms whatever the period). The handler records the interrupted pc, a backtrace (libgcc's
 * unwinder over .eh_frame; the generated code keeps no frame pointers) and the Kelvin flip counter, so a profile can be
 * cut per game frame. Samples go to a static ring; the XR thread appends them to $GRX_PROF (binary) at every progress
 * report, and copies /proc/self/maps next to it ($GRX_PROF.maps) for symbolisation. grx_prof.py reads both.
 *
 * GRX_PROF_APU=<file>: the same for the APU frame thread ("mcpx.apu_thread"), into its own ring and file. The XR
 * thread finds it by name at its first flush after the APU has started, opens the event on that tid and directs the
 * signal at it; the handler picks the ring by gettid. Only user time is sampled (paranoid 2).
 * GRX_PROF_PUSH=<file>: the same for the Kelvin pusher ("bx-pusher": pushbuffer decode + the Vulkan backend), perf.md
 * section 11.
 *
 * Record: u32 magic 0x50524F46, u16 depth, u16 pad, u64 flips, u64 pc, u64 frames[depth].
 * Test-only: nothing here is linked into the Quest build.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "grx_perf.h"

#define PROF_DEPTH 40
#define PROF_RING (1u << 16)
typedef struct ProfRec {
    uint64_t flips, pc;
    uint16_t depth;
    void *fr[PROF_DEPTH];
} ProfRec;

typedef struct ProfCtx {
    ProfRec *ring;
    volatile uint32_t head;               /* written by the handler only */
    uint32_t tail;                        /* read by the flusher only */
    volatile uint64_t dropped;
    FILE *out;
    char path[1024];
    volatile int on;
    volatile int tid;                     /* the sampled thread */
} ProfCtx;
static ProfCtx s_game, s_apu, s_push;
typedef struct ProfThread { ProfCtx *c; const char *env, *comm, *what; int wanted; } ProfThread;
static ProfThread s_threads[] = {
    { &s_apu, "GRX_PROF_APU", "mcpx.apu_thread", "APU", -1 },
    { &s_push, "GRX_PROF_PUSH", "bx-pusher", "pusher", -1 },
};

static void on_prof(int sig, siginfo_t *si, void *ucv)
{
    const ucontext_t *uc = (const ucontext_t *)ucv;
    int me = (int)syscall(SYS_gettid);
    ProfCtx *c = (s_apu.on && me == s_apu.tid) ? &s_apu : (s_push.on && me == s_push.tid) ? &s_push : &s_game;
    uint32_t h;
    ProfRec *r;
    uint64_t k[8];
    int n;
    (void)sig; (void)si;
    if (!c->ring) return;
    h = c->head;
    if (h - __atomic_load_n(&c->tail, __ATOMIC_ACQUIRE) >= PROF_RING) { c->dropped++; return; }
    r = &c->ring[h & (PROF_RING - 1)];
    grx_kelvin_counters(k);
    r->flips = k[2];
    r->pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    n = backtrace(r->fr, PROF_DEPTH);
    r->depth = (uint16_t)(n > 0 ? n : 0);
    __atomic_store_n(&c->head, h + 1, __ATOMIC_RELEASE);
}

static void install_handler(void)
{
    static int done;
    struct sigaction sa;
    void *warm[4];
    if (done) return;
    done = 1;
    backtrace(warm, 4);                   /* loads libgcc_s before the first signal */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
}

static int ctx_open(ProfCtx *c, const char *path)
{
    snprintf(c->path, sizeof(c->path), "%s", path);
    c->ring = (ProfRec *)calloc(PROF_RING, sizeof(ProfRec));
    c->out = fopen(c->path, "wb");
    if (!c->ring || !c->out) { fprintf(stderr, "[PROF] cannot start (%s)\n", c->path); return 0; }
    return 1;
}

/* A user-time task-clock perf event on `tid` (0: this thread) whose overflow signal goes to that thread. */
static int open_task_clock(int tid, long period_us)
{
    struct perf_event_attr pa;
    int fd;
    memset(&pa, 0, sizeof(pa));
    pa.type = PERF_TYPE_SOFTWARE; pa.size = sizeof(pa); pa.config = PERF_COUNT_SW_TASK_CLOCK;
    pa.sample_period = (uint64_t)period_us * 1000u; pa.exclude_kernel = 1; pa.exclude_hv = 1; pa.disabled = 1;
    pa.wakeup_events = 1;
    fd = (int)syscall(SYS_perf_event_open, &pa, tid, -1, -1, 0);
    if (fd >= 0) {
        struct f_owner_ex o;
        o.type = F_OWNER_TID; o.pid = tid ? (pid_t)tid : (pid_t)syscall(SYS_gettid);
        fcntl(fd, F_SETFL, O_ASYNC); fcntl(fd, F_SETSIG, SIGPROF); fcntl(fd, F_SETOWN_EX, &o);
        ioctl(fd, PERF_EVENT_IOC_RESET, 0); ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    return fd;
}

static long period_us_env(void)
{
    const char *us = getenv("GRX_PROF_US");
    return us && *us ? atol(us) : 250;
}

void grx_prof_start_on_game_thread(void)
{
    const char *p = getenv("GRX_PROF");
    struct sigevent sev;
    struct itimerspec its;
    timer_t tm;
    long period_us = period_us_env();
    if (!p || !*p) return;
    if (!ctx_open(&s_game, p)) return;
    install_handler();
    s_game.tid = (int)syscall(SYS_gettid);
    if (open_task_clock(0, period_us) >= 0) {
        s_game.on = 1;
        fprintf(stderr, "[PROF] sampling the game thread every %ld us of its task clock (perf event) -> %s\n", period_us, s_game.path);
        return;
    }
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &tm) != 0) { perror("[PROF] timer_create"); return; }
    its.it_interval.tv_sec = period_us / 1000000; its.it_interval.tv_nsec = (period_us % 1000000) * 1000;
    its.it_value = its.it_interval;
    timer_settime(tm, 0, &its, NULL);
    s_game.on = 1;
    fprintf(stderr, "[PROF] sampling the game thread every %ld us of its CPU time -> %s\n", period_us, s_game.path);
}

static int find_thread(const char *comm_want)
{
    DIR *dir = opendir("/proc/self/task");
    struct dirent *e;
    int found = 0;
    if (!dir) return 0;
    while ((e = readdir(dir)) != NULL) {
        char path[96], comm[64];
        FILE *f;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = 0;
            if (!strcmp(comm, comm_want)) found = atoi(e->d_name);
        }
        fclose(f);
        if (found) break;
    }
    closedir(dir);
    return found;
}

static void thread_try_start(ProfThread *th)
{
    const char *p = getenv(th->env);
    ProfCtx *c = th->c;
    long period_us;
    int tid;
    if (th->wanted == 0 || c->on) return;
    if (th->wanted < 0) th->wanted = p && *p;
    if (!th->wanted) return;
    if (!(tid = find_thread(th->comm))) return;
    if (!ctx_open(c, p)) { th->wanted = 0; return; }
    install_handler();
    period_us = period_us_env();
    c->tid = tid;
    __atomic_store_n(&c->on, 1, __ATOMIC_RELEASE);
    if (open_task_clock(tid, period_us) < 0) {
        perror("[PROF] perf_event_open");
        c->on = 0; th->wanted = 0;
        return;
    }
    fprintf(stderr, "[PROF] sampling the %s thread (tid %d) every %ld us of its task clock -> %s\n", th->what, tid,
            period_us, c->path);
}

static void ctx_flush(ProfCtx *c)
{
    uint32_t h, t;
    char mp[1100], buf[4096];
    FILE *mi, *mo;
    size_t n;
    if (!c->on || !c->out) return;
    h = __atomic_load_n(&c->head, __ATOMIC_ACQUIRE);
    for (t = c->tail; t != h; t++) {
        const ProfRec *r = &c->ring[t & (PROF_RING - 1)];
        const uint32_t magic = 0x50524F46u;
        const uint16_t d = r->depth, pad = 0;
        uint64_t fr[PROF_DEPTH];
        int i;
        for (i = 0; i < d; i++) fr[i] = (uint64_t)(uintptr_t)r->fr[i];
        fwrite(&magic, 4, 1, c->out); fwrite(&d, 2, 1, c->out); fwrite(&pad, 2, 1, c->out);
        fwrite(&r->flips, 8, 1, c->out); fwrite(&r->pc, 8, 1, c->out); fwrite(fr, 8, d, c->out);
    }
    __atomic_store_n(&c->tail, h, __ATOMIC_RELEASE);
    fflush(c->out);
    snprintf(mp, sizeof(mp), "%s.maps", c->path);
    if ((mi = fopen("/proc/self/maps", "r")) && (mo = fopen(mp, "w"))) {
        while ((n = fread(buf, 1, sizeof(buf), mi)) > 0) fwrite(buf, 1, n, mo);
        fclose(mo);
    }
    if (mi) fclose(mi);
    if (c->dropped) fprintf(stderr, "[PROF] %llu samples dropped (ring full, %s)\n", (unsigned long long)c->dropped, c->path);
}

void grx_prof_flush(void)
{
    size_t i;
    for (i = 0; i < sizeof(s_threads) / sizeof(s_threads[0]); i++) thread_try_start(&s_threads[i]);
    ctx_flush(&s_game);
    ctx_flush(&s_apu);
    ctx_flush(&s_push);
}
