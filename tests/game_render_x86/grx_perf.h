/* grx_perf.h -- tests/game_render_x86: the headless run's performance accounting (port/notes/perf.md).
 *   x86_fault_glue.c  every serviced SIGSEGV (trapped MMIO): guest address, direction, thread, handler time
 *   grx_perf.c        thread registry (pthread_create / CreateThread are wrapped at link time), per-thread CPU time
 *                     from /proc/self/task/<tid>/schedstat, the interval report
 *   grx_kstats.c      the Kelvin engine's counters (pushbuffer words, methods, flips, draws) */
#ifndef GRX_PERF_H
#define GRX_PERF_H
#include <stdint.h>

#define GRX_FAULT_SLOTS 4096
typedef struct GrxFaultSlot {
    uint64_t key;               /* tid << 33 | guest VA << 1 | write */
    uint64_t n, ns;
} GrxFaultSlot;
extern GrxFaultSlot g_grx_faults[GRX_FAULT_SLOTS];
extern volatile uint64_t g_grx_fault_total, g_grx_fault_ns, g_grx_fault_unclaimed;

/* Kelvin counters: [0] words [1] methods [2] flips [3] draws [4] kickoffs (pusher_run calls) [5] vertices */
void grx_kelvin_counters(uint64_t out[8]);

void grx_perf_init(const char *exe);
void grx_perf_set_game_tid(void);
/* One interval line set: label (e.g. "FLAT"/"STEREO"), wall seconds since the last call. */
void grx_perf_report(const char *mode, double wall_dt);
/* grx_prof.c: GRX_PROF=<file> samples the game thread (call the first on that thread; the second from any other) */
void grx_prof_start_on_game_thread(void);
void grx_prof_flush(void);
#endif
