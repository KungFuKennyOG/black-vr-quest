/*
 * nv2a-kwork: the raster worker pool's cross-thread handoffs (kelvin_workers.c).
 *
 * Every piece of data here is passed between threads with PLAIN loads and stores, exactly as
 * the renderer passes triangles and pixels; only the pool's own protocol may order them:
 *   lanes     job j's lane l reads what job j-1 (the previous job listing l) wrote there;
 *   barriers  a barrier job's begin callback sees every lane of every earlier job;
 *   exclusive kwork_submit's shares see everything before them, and later jobs see them;
 *   waits     after kwork_wait(job) / kwork_drain the submitter reads what the job wrote;
 *   parallel  after kwork_parallel returns, every item's writes;
 *   tasks     phase p+1 items read every item of phase p; ktask_wait sees the last phase.
 * A stale read is counted. Run on arm64 (QEMU: functional, threads really run in parallel) and
 * under ThreadSanitizer on x86-64, which checks the same handoffs against the C11 model -- the
 * model ARM hardware actually exercises, where x86's TSO would hide a missing barrier.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kelvin_workers.h"
#include "parity_common.h"

#define NLANES 32
#define WORDS 48
#define NJOBS 4000
#define RING 256

static uint32_t s_mem[NLANES][WORDS];          /* plain */
static long s_owner[NLANES];                   /* submitter's view: last job id per lane */
static atomic_long s_errors, s_lane_runs, s_begin_runs;

typedef struct Job {
    long id;
    long prev[NLANES];                         /* writer before this job, per lane */
    uint8_t listed[NLANES];                    /* the lanes this job runs */
} Job;
static Job s_jobs[RING];

static uint32_t pat(long id, int lane, int k) { return (uint32_t)(id * 2654435761u) ^ (uint32_t)(lane * 40503 + k * 97); }

static void check_lane(long prev, int l)
{
    int k;
    for (k = 0; k < WORDS; k++) {
        if (s_mem[l][k] != pat(prev, l, k)) {
            if (atomic_fetch_add(&s_errors, 1) < 5) {
                fprintf(stderr, "  stale lane %d word %d: %08X, expected job %ld's %08X\n", l, k, s_mem[l][k], prev,
                        pat(prev, l, k));
            }
            return;
        }
    }
}

static void run_lane(void *arg, int lane, int worker)
{
    Job *j = (Job *)arg;
    int k;
    (void)worker;
    check_lane(j->prev[lane], lane);
    for (k = 0; k < WORDS; k++) s_mem[lane][k] = pat(j->id, lane, k);
    atomic_fetch_add(&s_lane_runs, 1);
}

/* A barrier waits for every earlier job. It does not hold back later jobs on lanes it does not
 * list (they may already be running), so its begin callback checks the lanes it lists; the
 * whole state is checked at the drains. */
static void barrier_begin(void *arg)
{
    Job *j = (Job *)arg;
    int l;
    for (l = 0; l < NLANES; l++) if (j->listed[l]) check_lane(j->prev[l], l);
    atomic_fetch_add(&s_begin_runs, 1);
}

/* kwork_submit: one share per worker, on lanes 0..workers-1; share s owns memory lane s. */
static void run_share(void *arg, int share, int shares)
{
    (void)shares;
    run_lane(arg, share, share);
}

/* ---- parallel loops and tasks -------------------------------------------------------- */
#define PITEMS 200
static uint32_t s_par[PITEMS];
static uint32_t s_par_round;
static void par_item(void *arg, int item, int worker)
{
    (void)arg; (void)worker;
    s_par[item] = s_par_round * 7919u + (uint32_t)item;
}

#define TPHASES 4
#define TITEMS 24
typedef struct Task { uint32_t data[TPHASES][TITEMS]; uint32_t seed; } Task;
static Task s_tasks[KTASK_QUEUE];
static void task_item(void *arg, int item, int worker);
static int task_step(void *arg, int phase, void (**run)(void *arg, int item, int worker))
{
    Task *t = (Task *)arg;
    (void)t;                                  /* runs on whichever thread ended the phase */
    if (phase >= TPHASES) return 0;
    *run = task_item;
    return TITEMS;
}
static void task_item(void *arg, int item, int worker)
{
    Task *t = (Task *)arg;
    int p, k;
    (void)worker;
    /* which phase: the first whose row this item has not written */
    for (p = 0; p < TPHASES && t->data[p][item] != 0; p++) {}
    if (p == TPHASES) { atomic_fetch_add(&s_errors, 1); return; }
    if (p > 0) {
        for (k = 0; k < TITEMS; k++) {
            if (t->data[p - 1][k] != t->seed + (uint32_t)((p - 1) * 1000 + k)) {
                if (atomic_fetch_add(&s_errors, 1) < 5) fprintf(stderr, "  task phase %d item %d saw a stale phase %d\n", p, item, p - 1);
                break;
            }
        }
    }
    t->data[p][item] = t->seed + (uint32_t)(p * 1000 + item);
}

int main(void)
{
    int n, workers = kwork_workers();
    long last = -1, jobs = 0;
    long tasks[KTASK_QUEUE];
    int ntask = 0, i;

    printf("[%s] %d raster workers\n", PARITY_HOST, workers);
    if (workers < 1) {
        printf("no workers: set RECOMP_KELVIN_THREADS >= 2\n");
        return 1;
    }
    pr_seed(0x5EED0004u);
    for (i = 0; i < NLANES; i++) {
        int k;
        s_owner[i] = -1;
        for (k = 0; k < WORDS; k++) s_mem[i][k] = pat(-1, i, k);
    }
    for (n = 0; n < NJOBS; n++) {
        Job *j = &s_jobs[n % RING];
        uint8_t lanes[NLANES];
        int nl = 0, l, kind = (int)pr_n(16);
        unsigned flags = 0;

        j->id = n;
        memset(j->listed, 0, sizeof(j->listed));
        for (l = 0; l < NLANES; l++) j->prev[l] = s_owner[l];
        if (kind == 0) {
            /* exclusive: lanes 0..workers-1, one share each */
            for (l = 0; l < workers && l < NLANES; l++) { j->listed[l] = 1; s_owner[l] = n; }
            kwork_submit(barrier_begin, run_share, j);
            jobs++;
        } else {
            for (l = 0; l < NLANES; l++) {
                if (pr_n(4) == 0) { lanes[nl++] = (uint8_t)l; s_owner[l] = n; j->listed[l] = 1; }
            }
            if (kind == 1) flags = KWORK_BARRIER;
            last = kwork_submit_lanes(kind <= 2 ? barrier_begin : NULL, run_lane, j, lanes, nl, flags);
            jobs++;
            if (nl && pr_n(32) == 0) {
                /* the submitter reads the job's lanes once it has finished */
                kwork_wait(last);
                for (l = 0; l < nl; l++) check_lane(n, lanes[l]);
            }
        }
        if (pr_n(64) == 0) {
            s_par_round++;
            kwork_parallel(par_item, NULL, PITEMS);
            for (i = 0; i < PITEMS; i++) {
                if (s_par[i] != s_par_round * 7919u + (uint32_t)i && atomic_fetch_add(&s_errors, 1) < 5)
                    fprintf(stderr, "  parallel item %d stale after kwork_parallel\n", i);
            }
        }
        if (pr_n(48) == 0) {
            Task *t;
            if (ntask == KTASK_QUEUE) {
                for (i = 0; i < ntask; i++) {
                    Task *u = &s_tasks[i];
                    int k;
                    ktask_wait(tasks[i]);
                    for (k = 0; k < TITEMS; k++)
                        if (u->data[TPHASES - 1][k] != u->seed + (uint32_t)((TPHASES - 1) * 1000 + k)) {
                            atomic_fetch_add(&s_errors, 1);
                            break;
                        }
                }
                ntask = 0;
            }
            t = &s_tasks[ntask];
            memset(t, 0, sizeof(*t));
            t->seed = 0x10000u + (uint32_t)n * 64u;
            tasks[ntask++] = ktask_submit(task_step, t);
        }
        if (pr_n(128) == 0) {
            kwork_drain();
            for (l = 0; l < NLANES; l++) check_lane(s_owner[l], l);
        }
    }
    kwork_drain();
    for (i = 0; i < NLANES; i++) check_lane(s_owner[i], i);
    for (i = 0; i < ntask; i++) {
        Task *u = &s_tasks[i];
        int k;
        ktask_wait(tasks[i]);
        for (k = 0; k < TITEMS; k++)
            if (u->data[TPHASES - 1][k] != u->seed + (uint32_t)((TPHASES - 1) * 1000 + k)) { atomic_fetch_add(&s_errors, 1); break; }
    }
    printf("[%s] %ld jobs, %ld lane runs, %ld begin callbacks, %u parallel loops: %ld stale reads\n", PARITY_HOST, jobs,
           (long)atomic_load(&s_lane_runs), (long)atomic_load(&s_begin_runs), s_par_round, (long)atomic_load(&s_errors));
    printf("[%s] %s\n", PARITY_HOST, atomic_load(&s_errors) ? "FAIL" : "PASS");
    return atomic_load(&s_errors) != 0;
}
