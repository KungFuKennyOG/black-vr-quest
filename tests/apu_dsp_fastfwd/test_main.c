/*
 * apu_dsp_fastfwd: the DSP idle-poll fast-forward in apu_dsp.c (port/notes/perf.md 10) leaves the DSP exactly as
 * running every cycle would.
 *
 * The production apu_dsp.c is compiled into this file. A child process runs each scenario with the fast-forward off
 * (RECOMP_APU_DSP_FASTFWD=0) and streams a digest of the whole DSP state after every 32-sample tick to the parent,
 * which runs the same scenario with it on and compares tick by tick: PC, cycle counter and budget, all 64 registers,
 * both stacks, the interrupt block, halt/power state, the model's interrupt status, pending frame starts and the words
 * the program stores. Scenarios: an EP-like program (halt, then `add #1,a ; jclr #1,x:$ffffc5,*`) on the EP's
 * one-start-in-eight schedule, which parks in the loop as the real EP does; the same with the status bit raised on
 * pseudo-random ticks (the loop exits, stores its counter and halts again); the accumulator placed just below the
 * 56-bit overflow (fast-forward must fall back); and a `b`-accumulator, `add #3`, bit-7 variant.
 * Built for x86-64 and for arm64 (NDK, run under QEMU user mode), as tests/dsp_arm64.
 */
#include "apu_dsp.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct McpxApuDebug g_dbg, g_dbg_cache;
void xbox_SetAckTick(void (*fn)(void)) { (void)fn; }
#if !defined(_WIN32)
/* The register-access paths of apu_dsp.c lock the device; this test never calls them (single-threaded). */
LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
LONG InterlockedDecrement(volatile LONG *p) { return --*p; }
VOID EnterCriticalSection(LPCRITICAL_SECTION cs) { (void)cs; }
VOID LeaveCriticalSection(LPCRITICAL_SECTION cs) { (void)cs; }
VOID WakeConditionVariable(PCONDITION_VARIABLE cv) { (void)cv; }
#endif

typedef struct Digest {
    Dsp56300State st;
    uint32_t interrupts, starts_pending, p20, p21;
    uint64_t base;
} Digest;

#define TICKS 4000

static const uint32_t prog_a[] = {
    0x08F484, 0x000001,      /* 0: movep #1,x:$ffffc4      halt                    */
    0x200013,                /* 2: clr a                                            */
    0x014180,                /* 3: add #1,a                <- head                  */
    0x0A8581, 0x000003,      /* 4: jclr #1,x:$ffffc5,$3                             */
    0x08F485, 0x000002,      /* 6: movep #2,x:$ffffc5      ack the start of frame   */
    0x07708E, 0x000020,      /* 8: movem a,p:>$20                                   */
    0x077088, 0x000021,      /* A: movem a0,p:>$21                                  */
    0x0C0000,                /* C: jmp $0                                           */
};
static const uint32_t prog_b[] = {
    0x08F484, 0x000001,      /* 0: halt                                             */
    0x20001B,                /* 2: clr b                                            */
    0x014388,                /* 3: add #3,b                <- head                  */
    0x0A8587, 0x000003,      /* 4: jclr #7,x:$ffffc5,$3    (DMA EOL bit)            */
    0x07708F, 0x000020,      /* 6: movem b,p:>$20                                   */
    0x0C0000,                /* 8: jmp $0                                           */
};

enum { SC_PARK, SC_RAISE, SC_OVERFLOW, SC_B_EOL, SC_COUNT };
static const char *const sc_name[] = { "park", "raise", "overflow", "b-eol" };

static uint32_t lcg(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 16; }

static void digest(DSPState *dsp, Digest *g)
{
    memset(g, 0, sizeof *g);
    dsp56300_get_state(dsp->engine.jit, &g->st);
    g->interrupts = dsp->interrupts;
    g->starts_pending = dsp->frame_starts_pending;
    g->p20 = dsp->core.pram[0x20];
    g->p21 = dsp->core.pram[0x21];
    g->base = dsp->cycles_base;
}

/* Runs one scenario, calling out(tick, digest) after every tick. */
static void run(int sc, void (*out)(int, const Digest *, void *), void *ctx)
{
    MCPXAPUState *apu = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    DSPState *dsp = (DSPState *)calloc(1, sizeof(DSPState));
    const uint32_t *prog = sc == SC_B_EOL ? prog_b : prog_a;
    unsigned n = sc == SC_B_EOL ? sizeof prog_b / 4 : sizeof prog_a / 4;
    uint32_t seed = 12345u + (uint32_t)sc;
    Digest g;
    dsp->apu = apu;
    dsp->is_gp = false;
    if (!(dsp->engine_ready = dsp_engine_init(dsp))) { fprintf(stderr, "engine init failed\n"); exit(2); }
    for (unsigned i = 0; i < n; ++i)
        mcpx_dsp_engine_write(&dsp->engine, MCPX_DSP_SPACE_P, i, prog[i]);
    for (int t = 0; t < TICKS; ++t) {
        if (sc == SC_RAISE && t > 16 && lcg(&seed) % 37 == 0)
            dsp->interrupts |= DSP_START_FRAME_INTERRUPT;       /* as a frame start reaching a halted core */
        if (sc == SC_B_EOL && t > 16 && lcg(&seed) % 53 == 0)
            dsp->dma.eol = true;                                 /* bit 7 of $ffffc5 */
        if (sc == SC_B_EOL && (dsp->interrupts || dsp->dma.eol) && lcg(&seed) % 3 == 0)
            dsp->dma.eol = false;
        if (sc == SC_OVERFLOW && t == 40) {
            /* a just below +2^55: the next tick's adds cross it */
            Dsp56300State s;
            memset(&s, 0, sizeof s);
            dsp56300_get_state(dsp->engine.jit, &s);
            dsp_acc_set(&s, false, ((INT64_C(1) << 55) - 1) - 20000);
            dsp56300_set_state(dsp->engine.jit, &s);
        }
        dsp_run_frame(dsp, (t % 8) == 0);
        digest(dsp, &g);
        out(t, &g, ctx);
    }
    mcpx_dsp_engine_destroy(&dsp->engine);
    free(dsp);
    free(apu);
}

static void to_pipe(int t, const Digest *g, void *ctx)
{
    (void)t;
    if (write(*(int *)ctx, g, sizeof *g) != (ssize_t)sizeof *g) _exit(3);
}

typedef struct Cmp { int fd, bad; } Cmp;
static void against_pipe(int t, const Digest *g, void *ctx)
{
    Cmp *c = (Cmp *)ctx;
    Digest ref;
    size_t got = 0;
    while (got < sizeof ref) {
        ssize_t r = read(c->fd, (char *)&ref + got, sizeof ref - got);
        if (r <= 0) { c->bad++; return; }
        got += (size_t)r;
    }
    if (memcmp(&ref, g, sizeof ref) != 0 && c->bad++ < 5)
        fprintf(stderr, "  tick %d differs: pc %06X/%06X count %u/%u budget %d/%d a1 %06X/%06X sr %06X/%06X\n",
                t, ref.st.pc, g->st.pc, ref.st.cycle_count, g->st.cycle_count, ref.st.cycle_budget,
                g->st.cycle_budget, ref.st.registers[DSP56300_REG_A1], g->st.registers[DSP56300_REG_A1],
                ref.st.registers[DSP56300_REG_SR], g->st.registers[DSP56300_REG_SR]);
}

int main(void)
{
    int fails = 0, fd[SC_COUNT][2];
    pid_t pid[SC_COUNT];
    /* The references first, before this process caches its own fast-forward setting. */
    for (int sc = 0; sc < SC_COUNT; ++sc) {
        if (pipe(fd[sc]) != 0) return 2;
        pid[sc] = fork();
        if (pid[sc] == 0) {
            for (int k = 0; k <= sc; ++k) close(fd[k][0]);
            setenv("RECOMP_APU_DSP_FASTFWD", "0", 1);
            run(sc, to_pipe, &fd[sc][1]);
            close(fd[sc][1]);
            _exit(0);
        }
        close(fd[sc][1]);
    }
    setenv("RECOMP_APU_DSP_FASTFWD", "1", 1);
    for (int sc = 0; sc < SC_COUNT; ++sc) {
        uint64_t ff0 = s_dsp_ff_ticks;
        Cmp c;
        c.fd = fd[sc][0]; c.bad = 0;
        run(sc, against_pipe, &c);
        close(fd[sc][0]);
        waitpid(pid[sc], NULL, 0);
        printf("%-9s %d ticks, %llu fast-forwarded, %s\n", sc_name[sc], TICKS,
               (unsigned long long)(s_dsp_ff_ticks - ff0), c.bad ? "DIFFERENT" : "identical");
        if (c.bad) fails++;
        /* the fast-forward must actually have been taken */
        if (s_dsp_ff_ticks - ff0 < TICKS / 4) { printf("  fast-forward not taken\n"); fails++; }
    }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
