/*
 * x86_fault_glue.c -- tests/game_render_x86: the POSIX fault glue of platform/arm64_fault_decode.h
 * (a64_fault_install / a64_fault_add_handler / a64_uc_*), implemented for x86-64 Linux.
 *
 * On the Quest, every trapped device (Kelvin's PMC/PGRAPH/PCRTC, the APU, OHCI, the crash reporter) registers a
 * SIGSEGV handler with that API, and the AArch64 decoder in arm64_fault_decode.c services the faulting load or
 * store. This test runs the same game, runtime and handlers natively on x86-64 Linux, where no such path exists
 * (the desktop build plays under Wine). The production sources take their AArch64 POSIX branches here because the
 * test build defines XR_HOST_FAULT_GLUE; this file supplies what those branches call:
 *
 *   - the dispatcher: the same semantics as arm64_fault_decode.c's a64_segv (newest handler first, unclaimed
 *     faults to the previous action, SA_NODEFER so a device model may nest);
 *   - a64_uc_emulate_mmio: the x86-64 instruction at RIP is decoded and serviced by platform/mmio_decode.h's
 *     x86-64 decoder -- the one the Windows build uses from its VEHs -- over a CONTEXT-shaped copy of the
 *     ucontext_t's general registers, written back afterwards;
 *   - a64_uc_pc / fault_address (CR2) / insn (the first 4 bytes at RIP, for logs) / access_is_write (the page
 *     fault error code's W bit).
 *
 * Test-only. Nothing here is linked into the Quest app.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

#include "arm64_fault_decode.h"

#if !defined(__x86_64__) || !defined(__linux__) || !defined(XR_HOST_FAULT_GLUE)
#error "x86_fault_glue.c is the x86-64 Linux glue for tests/game_render_x86 (build with -DXR_HOST_FAULT_GLUE)"
#endif

/* ---- the production x86-64 decoder, over a CONTEXT-shaped register file ---------------------------------------- */
typedef struct GrxX86Context {
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    uint32_t EFlags;
} GrxX86Context;

/* mmio_decode.h's decoder is compiled under _WIN32 against PCONTEXT; glue_inc/windows.h gives it DWORD. */
#define _WIN32 1
#define PCONTEXT GrxX86Context *
#include "mmio_decode.h"
#undef PCONTEXT
#undef _WIN32

static void ctx_from_uc(const ucontext_t *uc, GrxX86Context *c)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = (uint64_t)g[REG_RAX]; c->Rcx = (uint64_t)g[REG_RCX]; c->Rdx = (uint64_t)g[REG_RDX];
    c->Rbx = (uint64_t)g[REG_RBX]; c->Rsp = (uint64_t)g[REG_RSP]; c->Rbp = (uint64_t)g[REG_RBP];
    c->Rsi = (uint64_t)g[REG_RSI]; c->Rdi = (uint64_t)g[REG_RDI];
    c->R8 = (uint64_t)g[REG_R8];   c->R9 = (uint64_t)g[REG_R9];   c->R10 = (uint64_t)g[REG_R10];
    c->R11 = (uint64_t)g[REG_R11]; c->R12 = (uint64_t)g[REG_R12]; c->R13 = (uint64_t)g[REG_R13];
    c->R14 = (uint64_t)g[REG_R14]; c->R15 = (uint64_t)g[REG_R15];
    c->Rip = (uint64_t)g[REG_RIP];
    c->EFlags = (uint32_t)g[REG_EFL];
}

static void ctx_to_uc(const GrxX86Context *c, ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = (greg_t)c->Rax; g[REG_RCX] = (greg_t)c->Rcx; g[REG_RDX] = (greg_t)c->Rdx;
    g[REG_RBX] = (greg_t)c->Rbx; g[REG_RSP] = (greg_t)c->Rsp; g[REG_RBP] = (greg_t)c->Rbp;
    g[REG_RSI] = (greg_t)c->Rsi; g[REG_RDI] = (greg_t)c->Rdi;
    g[REG_R8] = (greg_t)c->R8;   g[REG_R9] = (greg_t)c->R9;   g[REG_R10] = (greg_t)c->R10;
    g[REG_R11] = (greg_t)c->R11; g[REG_R12] = (greg_t)c->R12; g[REG_R13] = (greg_t)c->R13;
    g[REG_R14] = (greg_t)c->R14; g[REG_R15] = (greg_t)c->R15;
    g[REG_RIP] = (greg_t)c->Rip;
    /* only the arithmetic flags the decoder may set; keep TF, IF, DF and the rest as they were */
    g[REG_EFL] = (greg_t)(((uint64_t)g[REG_EFL] & ~(uint64_t)0x8D5u) | (c->EFlags & 0x8D5u));
}

uint64_t a64_uc_pc(const void *ucv)
{
    return (uint64_t)((const ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
}

uint64_t a64_uc_fault_address(const void *ucv)
{
    return (uint64_t)((const ucontext_t *)ucv)->uc_mcontext.gregs[REG_CR2];
}

uint32_t a64_uc_insn(const void *ucv)
{
    uint32_t w;
    memcpy(&w, (const void *)(uintptr_t)a64_uc_pc(ucv), sizeof(w));
    /* byte order as they appear in memory, most significant first, for the logs */
    return __builtin_bswap32(w);
}

int a64_uc_access_is_write(const void *ucv)
{
    /* x86 page-fault error code: bit 1 = write access */
    return (((const ucontext_t *)ucv)->uc_mcontext.gregs[REG_ERR] & 2) ? 1 : 0;
}

/* Wrap the device callbacks: x86 accesses here are a single element, so the element's offset is the fault's. */
typedef struct GrxDev {
    void *dev;
    a64_mmio_read_fn rd;
    a64_mmio_write_fn wr;
    uint32_t dev_size;
    int bad;
} GrxDev;

static uint64_t grx_rd(void *d, uint32_t off, int size)
{
    GrxDev *g = (GrxDev *)d;
    if (g->dev_size && (uint64_t)off + (uint64_t)size > g->dev_size) { g->bad = 1; return 0; }
    return g->rd(g->dev, off, size);
}

static void grx_wr(void *d, uint32_t off, uint64_t v, int size)
{
    GrxDev *g = (GrxDev *)d;
    if (g->dev_size && (uint64_t)off + (uint64_t)size > g->dev_size) { g->bad = 1; return; }
    g->wr(g->dev, off, v, size);
}

int a64_uc_emulate_mmio(void *ucv, uintptr_t fault_addr, uint32_t fault_off, uint32_t dev_size, void *dev,
                        a64_mmio_read_fn rd, a64_mmio_write_fn wr)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    GrxX86Context c;
    GrxDev g;
    (void)fault_addr;
    if (!uc || !rd || !wr || (dev_size && fault_off >= dev_size))
        return 0;
    g.dev = dev; g.rd = rd; g.wr = wr; g.dev_size = dev_size; g.bad = 0;
    ctx_from_uc(uc, &c);
    if (!mmio_emulate(&c, fault_off, &g, grx_rd, grx_wr) || g.bad)
        return 0;
    ctx_to_uc(&c, uc);
    return 1;
}

/* ---- SIGSEGV dispatch (as arm64_fault_decode.c's a64_segv) ---------------------------------------------------- */
#define GRX_MAX_HANDLERS 16
static a64_fault_handler s_handlers[GRX_MAX_HANDLERS];
static int s_nhandlers;
static struct sigaction s_prev;
static int s_installed;

/* ---- per-access accounting for the perf report (grx_perf.c): every serviced fault, by guest address, direction and
 * thread, with the time spent in the handlers (the device model, its locks, the decode). Lock-free. */
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "grx_perf.h"
extern ptrdiff_t g_xbox_mem_offset;
GrxFaultSlot g_grx_faults[GRX_FAULT_SLOTS];
volatile uint64_t g_grx_fault_total, g_grx_fault_ns, g_grx_fault_unclaimed;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fault_account(uintptr_t fault, int write, uint64_t ns)
{
    static __thread int tid;
    uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset) & ~3u;
    uint64_t key;
    unsigned i, slot;
    if (!tid) tid = (int)syscall(SYS_gettid);
    key = ((uint64_t)(uint32_t)tid << 33) | ((uint64_t)va << 1) | (uint64_t)(write & 1);
    slot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 40) % GRX_FAULT_SLOTS;
    for (i = 0; i < GRX_FAULT_SLOTS; i++, slot = (slot + 1) % GRX_FAULT_SLOTS) {
        uint64_t k = __atomic_load_n(&g_grx_faults[slot].key, __ATOMIC_ACQUIRE), z = 0;
        if (k == key || (!k && (__atomic_compare_exchange_n(&g_grx_faults[slot].key, &z, key, 0, __ATOMIC_ACQ_REL,
                                                              __ATOMIC_ACQUIRE) || z == key))) {
            __atomic_add_fetch(&g_grx_faults[slot].n, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_grx_faults[slot].ns, ns, __ATOMIC_RELAXED);
            break;
        }
    }
    __atomic_add_fetch(&g_grx_fault_total, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_grx_fault_ns, ns, __ATOMIC_RELAXED);
}

static void grx_segv(int sig, siginfo_t *si, void *uc)
{
    int n = __atomic_load_n(&s_nhandlers, __ATOMIC_ACQUIRE), i;
    uintptr_t fault = (uintptr_t)si->si_addr;
    const uint64_t t0 = mono_ns();
    for (i = n - 1; i >= 0; i--)
        if (s_handlers[i] && s_handlers[i](uc, fault)) {
            fault_account(fault, a64_uc_access_is_write(uc), mono_ns() - t0);
            return;
        }
    __atomic_add_fetch(&g_grx_fault_unclaimed, 1, __ATOMIC_RELAXED);
    if (s_prev.sa_flags & SA_SIGINFO) {
        if (s_prev.sa_sigaction) { s_prev.sa_sigaction(sig, si, uc); return; }
    } else if (s_prev.sa_handler != SIG_DFL && s_prev.sa_handler != SIG_IGN) {
        s_prev.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

int a64_fault_add_handler(a64_fault_handler h)
{
    int n;
    if (!h) return 0;
    for (n = 0; n < s_nhandlers; n++)
        if (s_handlers[n] == h) return 1;
    n = __atomic_load_n(&s_nhandlers, __ATOMIC_ACQUIRE);
    if (n >= GRX_MAX_HANDLERS) return 0;
    s_handlers[n] = h;
    __atomic_store_n(&s_nhandlers, n + 1, __ATOMIC_RELEASE);
    return 1;
}

int a64_fault_install(void)
{
    struct sigaction sa;
    if (__atomic_load_n(&s_installed, __ATOMIC_ACQUIRE)) return 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = grx_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_prev) != 0) return 0;
    __atomic_store_n(&s_installed, 1, __ATOMIC_RELEASE);
    return 1;
}

/* a64_uc_get / a64_uc_put are AArch64 register files; nothing on this host's enabled paths calls them (the AC97
 * and memory-watch traps keep their __aarch64__-only branches). Defined so a stray call is loud, not a link error. */
int a64_uc_get(const void *uc, a64_regs *r)
{
    (void)uc; memset(r, 0, sizeof(*r));
    fprintf(stderr, "[GRX] a64_uc_get called on x86-64: not supported\n");
    return 0;
}

int a64_uc_put(void *uc, const a64_regs *r, uint32_t fp_mask)
{
    (void)uc; (void)r; (void)fp_mask;
    fprintf(stderr, "[GRX] a64_uc_put called on x86-64: not supported\n");
    return 0;
}
