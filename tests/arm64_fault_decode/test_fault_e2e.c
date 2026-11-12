/*
 * test_fault_e2e -- the AArch64 fault path from end to end, on an arm64 host
 * (Quest, or this repo's QEMU executor /root/xtool/arm64run).
 *
 * A 4 GiB PROT_NONE reservation stands in for guest memory, with RAM opened
 * at the bottom and the device apertures left inaccessible, as
 * xbox_MemoryLayoutInit arranges it. Real loads and stores hit the aperture,
 * the kernel delivers SIGSEGV with a real rt_sigframe, the dispatcher in
 * arm64_fault_decode.c routes it to the handlers registered here, the
 * instruction at pc is decoded and serviced against a model device through
 * mmio_decode.h's mmio_emulate (the entry point ohci.c and kelvin_mmio.c
 * use), the registers -- including V registers via the fpsimd record -- are
 * written back, and execution resumes at pc + 4.
 *
 * The accesses come from two places:
 *   - inline asm, so the exact form is known (every class: B/H/W/X, signed,
 *     S/D/Q, LDP/STP, pre/post-index writeback, register offset, LDAR/STLR,
 *     LSE atomics, CAS, LDXR/STXR, XZR);
 *   - the runtime's own MEM8/16/32/SMEM/MEMF/MEMD macros as compiled C at
 *     whatever -O this file is built with, which is the real use.
 *
 * Plus the paths that must NOT be serviced: a fault outside every device
 * reaches the previous handler; an instruction that does not decode is
 * declined and also reaches it, with the context untouched.
 */
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "mmio_decode.h"      /* aarch64 mmio_emulate -> arm64_fault_decode */

#if !defined(__aarch64__)
#error "test_fault_e2e is an AArch64 test"
#endif

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); fflush(stdout); } } while (0)

/* ---- guest memory --------------------------------------------------------- */

#define GUEST_RAM   (1u << 20)
#define DEV_A_BASE  0xFD000000u   /* "NV2A"-like: 64 KB of registers */
#define DEV_A_SIZE  0x00010000u
#define DEV_B_BASE  0xFED00000u   /* "OHCI"-like: 4 KB */
#define DEV_B_SIZE  0x00001000u

ptrdiff_t g_xbox_mem_offset;
static unsigned char *g_base;

#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/* ---- the model device ----------------------------------------------------- */

typedef struct device {
    const char *name;
    uint32_t base, size;
    uint8_t regs[DEV_A_SIZE];
    unsigned reads, writes;
    uint32_t last_off; int last_size; int last_write;
} device;

static device g_dev_a = { "A", DEV_A_BASE, DEV_A_SIZE, {0}, 0, 0, 0, 0, 0 };
static device g_dev_b = { "B", DEV_B_BASE, DEV_B_SIZE, {0}, 0, 0, 0, 0, 0 };

/* A read of the status register at 0x100 has a side effect, so a test can
 * tell that each access really went through the model once. */
static uint64_t dev_read(void *d, uint32_t off, int size)
{
    device *dv = (device *)d;
    uint64_t v = 0;
    dv->reads++; dv->last_off = off; dv->last_size = size; dv->last_write = 0;
    if (off + (uint32_t)size > dv->size) return ~0ull;
    memcpy(&v, dv->regs + off, (size_t)size);
    if (off == 0x100 && size == 4) {
        uint32_t c; memcpy(&c, dv->regs + 0x100, 4); c++; memcpy(dv->regs + 0x100, &c, 4);
    }
    return v;
}

static void dev_write(void *d, uint32_t off, uint64_t v, int size)
{
    device *dv = (device *)d;
    dv->writes++; dv->last_off = off; dv->last_size = size; dv->last_write = 1;
    if (off + (uint32_t)size > dv->size) return;
    memcpy(dv->regs + off, &v, (size_t)size);
}

/* ---- fault handlers, as a device module registers them -------------------- */

static unsigned g_handled, g_declined;

static int dev_fault(device *dv, void *uc, uintptr_t fault)
{
    uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset);
    if (va < dv->base || va - dv->base >= dv->size)
        return 0;
    if (mmio_emulate(uc, va - dv->base, dv, dev_read, dev_write)) {
        g_handled++;
        return 1;
    }
    g_declined++;
    return 0;
}
static int fault_a(void *uc, uintptr_t f) { return dev_fault(&g_dev_a, uc, f); }
static int fault_b(void *uc, uintptr_t f) { return dev_fault(&g_dev_b, uc, f); }

/* The "previous" SIGSEGV handler (debuggerd's place): records and jumps out. */
static sigjmp_buf g_escape;
static volatile int g_prev_hits;
static volatile uintptr_t g_prev_addr, g_prev_pc;
static void prev_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    g_prev_hits++;
    g_prev_addr = (uintptr_t)si->si_addr;
    g_prev_pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc;
    siglongjmp(g_escape, 1);
}

static uintptr_t host(uint32_t va) { return (uintptr_t)g_base + va; }
/* The device model runs inside the signal handler, which the compiler cannot
 * see: without a barrier it forwards a set32() to a later get32() across the
 * faulting access (seen at -O2). A real device model shared with other
 * threads is behind a lock and has the same barrier from it. */
#define BARRIER() __asm__ volatile("" ::: "memory")
static void set32(device *d, uint32_t off, uint32_t v) { BARRIER(); memcpy(d->regs + off, &v, 4); BARRIER(); }
static void set64(device *d, uint32_t off, uint64_t v) { BARRIER(); memcpy(d->regs + off, &v, 8); BARRIER(); }
static uint32_t get32(device *d, uint32_t off) { uint32_t v; BARRIER(); memcpy(&v, d->regs + off, 4); return v; }
static uint64_t get64(device *d, uint32_t off) { uint64_t v; BARRIER(); memcpy(&v, d->regs + off, 8); return v; }

/* ---- the accesses ---------------------------------------------------------- */

static void asm_forms(void)
{
    device *A = &g_dev_a;
    uintptr_t a = host(DEV_A_BASE);
    uint64_t x0, x1, x2;
    unsigned before;

    /* LDR W, unsigned offset: zero-extends, top half of X cleared. */
    set32(A, 0x10, 0x89ABCDEFu);
    __asm__ volatile("mov x0, #-1\n ldr w0, [%1, #0x10]\n mov %0, x0"
                     : "=r"(x0) : "r"(a) : "x0", "memory");
    CHECK(x0 == 0x89ABCDEFull, "ldr w: %llx", (unsigned long long)x0);

    /* LDR X, register offset with scaled index. */
    set64(A, 0x40, 0x1122334455667788ull);
    __asm__ volatile("mov x1, #8\n ldr %0, [%1, x1, lsl #3]" : "=r"(x0) : "r"(a) : "x1", "memory");
    CHECK(x0 == 0x1122334455667788ull, "ldr x lsl3: %llx", (unsigned long long)x0);

    /* LDRSB X / LDRSH W / LDRSW / LDRB / LDRH */
    A->regs[0x20] = 0x80; set32(A, 0x24, 0x00008001u); set32(A, 0x28, 0xFFFFFFFEu);
    __asm__ volatile("ldrsb %0, [%3, #0x20]\n ldrsh %w1, [%3, #0x24]\n ldrsw %2, [%3, #0x28]"
                     : "=&r"(x0), "=&r"(x1), "=&r"(x2) : "r"(a) : "memory");
    CHECK(x0 == 0xFFFFFFFFFFFFFF80ull, "ldrsb x: %llx", (unsigned long long)x0);
    CHECK(x1 == 0xFFFF8001ull, "ldrsh w: %llx", (unsigned long long)x1);
    CHECK(x2 == 0xFFFFFFFFFFFFFFFEull, "ldrsw: %llx", (unsigned long long)x2);
    __asm__ volatile("ldrb %w0, [%2, #0x20]\n ldrh %w1, [%2, #0x24]" : "=&r"(x0), "=&r"(x1) : "r"(a) : "memory");
    CHECK(x0 == 0x80 && x1 == 0x8001, "ldrb/ldrh");

    /* STR B/H/W/X and XZR. */
    __asm__ volatile("mov x0, #0x5A\n strb w0, [%0, #0x30]\n mov x0, #0xBEEF\n strh w0, [%0, #0x32]\n"
                     "movz x0, #0xDEAD, lsl #16\n str w0, [%0, #0x34]\n"
                     "mov x0, #-2\n str x0, [%0, #0x38]\n str xzr, [%0, #0x40]"
                     :: "r"(a) : "x0", "memory");
    CHECK(A->regs[0x30] == 0x5A && get32(A, 0x30) >> 16 == 0xBEEF && get32(A, 0x34) == 0xDEAD0000u,
          "strb/strh/str w");
    CHECK(get64(A, 0x38) == 0xFFFFFFFFFFFFFFFEull && get64(A, 0x40) == 0, "str x / xzr");

    /* LDR WZR: the access happens (side effect), nothing is written. */
    set32(A, 0x100, 7); before = A->reads;
    __asm__ volatile("ldr wzr, [%0, #0x100]" :: "r"(a) : "memory");
    CHECK(A->reads == before + 1 && get32(A, 0x100) == 8, "ldr wzr side effect");

    /* Pre- and post-index writeback. */
    set32(A, 0x200, 0x11); set32(A, 0x204, 0x22);
    {
        uintptr_t p = a + 0x1FC; uint64_t v;
        __asm__ volatile("ldr %w0, [%1, #4]!" : "=r"(v), "+r"(p) :: "memory");
        CHECK(v == 0x11 && p == a + 0x200, "pre-index");
        __asm__ volatile("ldr %w0, [%1], #4" : "=r"(v), "+r"(p) :: "memory");
        CHECK(v == 0x11 && p == a + 0x204, "post-index");
        __asm__ volatile("str %w2, [%1, #-4]!\n" : "=m"(*(char *)p), "+r"(p) : "r"(0x33u) : "memory");
        CHECK(get32(A, 0x200) == 0x33 && p == a + 0x200, "str pre-index");
    }

    /* LDP / STP, W and X; LDP faulting on its first element. */
    set32(A, 0x300, 0xAAAA0001u); set32(A, 0x304, 0xBBBB0002u);
    __asm__ volatile("ldp %w0, %w1, [%2, #-4]" : "=&r"(x0), "=&r"(x1) : "r"(a + 0x304) : "memory");
    CHECK(x0 == 0xAAAA0001u && x1 == 0xBBBB0002u, "ldp w");
    __asm__ volatile("stp %1, %2, [%0, #0x10]" :: "r"(a + 0x300), "r"(0x0102030405060708ull), "r"(0x1112131415161718ull) : "memory");
    CHECK(get64(A, 0x310) == 0x0102030405060708ull && get64(A, 0x318) == 0x1112131415161718ull, "stp x");

    /* LDP whose first element is RAM and second is the device: the fault is
     * reported at the device half, and the access straddles below register
     * 0, so it must be declined (and reach the previous handler). */

    /* FP / SIMD: S, D, Q loads and stores, LDP of D, STP of Q. */
    {
        float f = 3.25f; double d = -1.5; uint64_t lo = 0x0123456789ABCDEFull, hi = 0xFEDCBA9876543210ull;
        float fr; double dr; uint64_t qlo, qhi;
        memcpy(A->regs + 0x400, &f, 4); memcpy(A->regs + 0x408, &d, 8);
        memcpy(A->regs + 0x410, &lo, 8); memcpy(A->regs + 0x418, &hi, 8);
        __asm__ volatile("ldr s3, [%2, #0x400]\n fmov %w0, s3\n ldr d4, [%2, #0x408]\n fmov %1, d4"
                         : "=&r"(x0), "=&r"(x1) : "r"(a) : "v3", "v4", "memory");
        memcpy(&fr, &x0, 4); memcpy(&dr, &x1, 8);
        CHECK(fr == 3.25f && dr == -1.5, "ldr s/d: %f %f", fr, dr);
        /* ldr s must clear the rest of V: preload v5 with ones first. */
        __asm__ volatile("movi v5.2d, #-1\n ldr s5, [%2, #0x400]\n mov %0, v5.d[0]\n mov %1, v5.d[1]"
                         : "=&r"(qlo), "=&r"(qhi) : "r"(a) : "v5", "memory");
        CHECK(qlo == 0x40500000ull && qhi == 0, "ldr s clears V: %llx %llx",
              (unsigned long long)qlo, (unsigned long long)qhi);
        __asm__ volatile("ldr q6, [%2, #0x410]\n mov %0, v6.d[0]\n mov %1, v6.d[1]"
                         : "=&r"(qlo), "=&r"(qhi) : "r"(a) : "v6", "memory");
        CHECK(qlo == lo && qhi == hi, "ldr q");
        __asm__ volatile("str q6, [%0, #0x420]\n fmov s7, #1.0\n str s7, [%0, #0x430]\n"
                         "fmov d8, #-2.0\n str d8, [%0, #0x438]"
                         :: "r"(a) : "v6", "v7", "v8", "memory");
        memcpy(&fr, A->regs + 0x430, 4); memcpy(&dr, A->regs + 0x438, 8);
        CHECK(get64(A, 0x420) == lo && get64(A, 0x428) == hi && fr == 1.0f && dr == -2.0, "str q/s/d");
        __asm__ volatile("ldp d9, d10, [%2, #8]\n fmov %0, d9\n fmov %1, d10"
                         : "=&r"(x0), "=&r"(x1) : "r"(a + 0x400) : "v9", "v10", "memory");
        CHECK(x0 == 0xBFF8000000000000ull && x1 == lo, "ldp d");
        __asm__ volatile("ldr q11, [%0, #0x410]\n ldr q12, [%0, #0x420]\n add x9, %0, #0x400\n stp q11, q12, [x9, #0x40]"
                         :: "r"(a) : "x9", "v11", "v12", "memory");
        CHECK(get64(A, 0x440) == lo && get64(A, 0x448) == hi && get64(A, 0x450) == lo, "stp q");
        /* V registers not named by the access are left alone (SVE upper
         * bits are not exercised here: no SVE on the default CPU). */
        __asm__ volatile("movi v13.2d, #-1\n ldr s14, [%2, #0x400]\n mov %0, v13.d[0]\n mov %1, v13.d[1]"
                         : "=&r"(qlo), "=&r"(qhi) : "r"(a) : "v13", "v14", "memory");
        CHECK(qlo == ~0ull && qhi == ~0ull, "unrelated V preserved");
    }

    /* Load-acquire / store-release. */
    set32(A, 0x500, 0xC0FFEEu);
    __asm__ volatile("add x9, %1, #0x500\n ldar %w0, [x9]\n" : "=r"(x0) : "r"(a) : "x9", "memory");
    CHECK(x0 == 0xC0FFEE, "ldar");
    __asm__ volatile("add x9, %0, #0x504\n mov w10, #0x77\n stlrb w10, [x9]" :: "r"(a) : "x9", "x10", "memory");
    CHECK(A->regs[0x504] == 0x77, "stlrb");

    /* LSE atomics and CAS (the CPU has LSE; -march enables it below). */
    set32(A, 0x600, 40);
    __asm__ volatile(".arch_extension lse\n add x9, %1, #0x600\n mov w10, #2\n ldaddal w10, %w0, [x9]"
                     : "=r"(x0) : "r"(a) : "x9", "x10", "memory");
    CHECK(x0 == 40 && get32(A, 0x600) == 42, "ldaddal: old %llu new %u", (unsigned long long)x0, get32(A, 0x600));
    __asm__ volatile(".arch_extension lse\n add x9, %1, #0x600\n mov w0, #42\n mov w10, #99\n casal w0, w10, [x9]\n mov %w0, w0"
                     : "=r"(x0) : "r"(a) : "x0", "x9", "x10", "memory");
    CHECK(x0 == 42 && get32(A, 0x600) == 99, "casal");
    __asm__ volatile(".arch_extension lse\n add x9, %0, #0x600\n mov w10, #1\n stset w10, [x9]"
                     :: "r"(a) : "x9", "x10", "memory");
    CHECK(get32(A, 0x600) == 99, "stset (99|1)");
    __asm__ volatile(".arch_extension lse\n add x9, %1, #0x600\n mov x10, #5\n swpal w10, %w0, [x9]"
                     : "=r"(x0) : "r"(a) : "x9", "x10", "memory");
    CHECK(x0 == 99 && get32(A, 0x600) == 5, "swpal");

    /* High registers: x28 base, x30 data. */
    set32(A, 0x700, 0x7007);
    __asm__ volatile("mov x28, %1\n ldr w30, [x28, #0x700]\n mov %0, x30" : "=r"(x0) : "r"(a) : "x28", "x30", "memory");
    CHECK(x0 == 0x7007, "x28/x30");

    /* SP as the base: point SP at the device for one instruction. The
     * signal frame is built on the faulting thread's stack, so this needs an
     * alternate signal stack -- which is what a stack overflow would need
     * too, and which the dispatcher does not use (see the header). So this
     * is a decode-only form here: covered in test_decode, not executed. */
}

/* The runtime's accessors as compiled C, guest VAs in device B. */
__attribute__((noinline)) static void accessor_forms(volatile uint32_t *scratch)
{
    device *B = &g_dev_b;
    uint32_t r = DEV_B_BASE;
    int i;

    set32(B, 0x04, 0x12345678u);
    CHECK(MEM32(r + 4) == 0x12345678u, "MEM32 read");
    CHECK(MEM16(r + 6) == 0x1234, "MEM16 read");
    CHECK(MEM8(r + 7) == 0x12, "MEM8 read");
    B->regs[0x10] = 0xFE;
    CHECK(SMEM8(r + 0x10) == -2, "SMEM8");
    set32(B, 0x12, 0x8000u);
    CHECK(SMEM16(r + 0x12) == -32768, "SMEM16");
    MEM32(r + 0x20) = 0xA5A5A5A5u;
    MEM16(r + 0x24) = 0xBEEF;
    MEM8(r + 0x26) = 0x42;
    SMEM64(r + 0x28) = -3;
    CHECK(get32(B, 0x20) == 0xA5A5A5A5u && (get32(B, 0x24) & 0xFFFFFF) == 0x42BEEF &&
          get64(B, 0x28) == (uint64_t)-3, "MEM stores");
    MEMF(r + 0x30) = 0.5f;
    MEMD(r + 0x38) = 0.25;
    CHECK(MEMF(r + 0x30) == 0.5f && MEMD(r + 0x38) == 0.25, "MEMF/MEMD");
    /* read-modify-write and an index loop */
    MEM32(r + 0x40) = 1;
    MEM32(r + 0x40) |= 0x100;
    MEM32(r + 0x40) &= ~1u;
    CHECK(get32(B, 0x40) == 0x100, "RMW");
    for (i = 0; i < 8; i++) MEM32(r + 0x80 + i * 4) = (uint32_t)i * 3;
    for (i = 0; i < 8; i++) scratch[i] = MEM32(r + 0x80 + i * 4);
    for (i = 0; i < 8; i++) CHECK(scratch[i] == (uint32_t)i * 3, "loop %d", i);
    /* a status poll that counts each read */
    set32(B, 0x100, 0);
    for (i = 0; i < 5; i++) (void)MEM32(r + 0x100);
    CHECK(get32(B, 0x100) == 5, "each poll read reached the device once: %u", get32(B, 0x100));
}

/* Faults that must not be serviced: they reach the previous handler with
 * the context unchanged. */
static void declined_paths(void)
{
    volatile uint64_t v = 0;
    unsigned before;

    /* Not a device: an unmapped guest address in the gap. */
    g_prev_hits = 0;
    if (!sigsetjmp(g_escape, 1))
        v = *(volatile uint32_t *)host(0x20000000u);
    CHECK(g_prev_hits == 1 && g_prev_addr == host(0x20000000u), "non-device fault reached previous handler");

    /* A device address, but an instruction the decoder refuses (LD1). */
    g_prev_hits = 0; before = g_declined;
    if (!sigsetjmp(g_escape, 1))
        __asm__ volatile("ld1 {v0.4s}, [%0]" :: "r"(host(DEV_A_BASE + 0x10)) : "v0", "memory");
    CHECK(g_prev_hits == 1 && g_declined == before + 1, "undecodable instruction declined");

    /* LDAXR on a device register: declined. Emulating it would leave the
     * exclusive monitor clear, the real STLXR would fail without faulting,
     * and the loop would spin forever (which is what this test did before
     * the MMIO path refused exclusives). */
    g_prev_hits = 0; before = g_declined;
    if (!sigsetjmp(g_escape, 1)) {
        uint32_t old, st;
        __asm__ volatile("1: ldaxr %w0, [%2]\n add w10, %w0, #1\n stlxr %w1, w10, [%2]\n cbnz %w1, 1b"
                         : "=&r"(old), "=&r"(st) : "r"(host(DEV_A_BASE + 0x600)) : "x10", "memory");
        v = old + st;
    }
    CHECK(g_prev_hits == 1 && g_declined == before + 1, "ldaxr on a device declined");

    /* An LDP that starts 4 bytes below device A's base: the fault is at or
     * above the base, but the access begins outside the device, so the
     * device must not be asked for it. */
    g_prev_hits = 0; before = g_declined;
    if (!sigsetjmp(g_escape, 1)) {
        uint64_t p0, p1;
        __asm__ volatile("ldp %w0, %w1, [%2]" : "=&r"(p0), "=&r"(p1) : "r"(host(DEV_A_BASE) - 4) : "memory");
        v = p0 + p1;
    }
    CHECK(g_prev_hits == 1, "straddling LDP reached previous handler");
    (void)v;
}

int main(void)
{
    struct sigaction prev;
    volatile uint32_t scratch[8];

    setvbuf(stdout, NULL, _IONBF, 0);

    /* 4 GiB of guest address space, all PROT_NONE; RAM opened at 0. */
    g_base = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_base == MAP_FAILED) { perror("mmap"); return 2; }
    if (mprotect(g_base, GUEST_RAM, PROT_READ | PROT_WRITE)) { perror("mprotect"); return 2; }
    g_xbox_mem_offset = (ptrdiff_t)g_base;

    /* The handler that was there before ours (Android: debuggerd). */
    memset(&prev, 0, sizeof prev);
    prev.sa_sigaction = prev_handler;
    prev.sa_flags = SA_SIGINFO;
    sigemptyset(&prev.sa_mask);
    sigaction(SIGSEGV, &prev, NULL);

    CHECK(a64_fault_install() == 1, "install");
    CHECK(a64_fault_install() == 1, "install is idempotent");
    CHECK(a64_fault_add_handler(fault_a) && a64_fault_add_handler(fault_b), "add handlers");

    asm_forms();
    accessor_forms(scratch);
    declined_paths();

    /* RAM still works and never faulted into us. */
    MEM32(0x1000) = 0x600D;
    CHECK(MEM32(0x1000) == 0x600D, "RAM");

    printf("handled %u faults, declined %u; device A %u reads %u writes, device B %u reads %u writes\n",
           g_handled, g_declined, g_dev_a.reads, g_dev_a.writes, g_dev_b.reads, g_dev_b.writes);
    CHECK(g_handled > 60, "too few faults handled: %u", g_handled);
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "ARM64 FAULT E2E FAIL\n" : "ARM64 FAULT E2E PASS\n");
    return failures ? 1 : 0;
}
