/*
 * test_decode -- arm64_fault_decode's decoder and emulator, on any host.
 *
 * Three parts:
 *
 *  1. Ground truth. vectors_gen.h holds instruction words assembled and
 *     compiled by the NDK's clang, with the expected decode derived from
 *     llvm-objdump's disassembly (gen_vectors.py). Every field must match,
 *     every "refuse" word must be refused, and every load/store the compiler
 *     emitted for the runtime's accessor macros must decode.
 *
 *  2. Behaviour of every accepted vector. Each one is executed against a
 *     recording memory with random registers, and what it touched is checked
 *     against a reference computed here from the objdump-derived fields:
 *     the address, the number and size of accesses, the values stored, the
 *     register a load wrote (sign/zero extension, W clearing the top half,
 *     XZR discarding), writeback, and pc += 4.
 *
 *  3. Hand-written semantics: the edge cases a generic model checks least
 *     well (LDP into its own base, Q halves, FP loads clearing the top of V,
 *     atomics and CAS, STXR status, MMIO offset derivation and refusals).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "arm64_fault_decode.h"
#include "vectors_gen.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* ---- recording memory ---------------------------------------------------- */

typedef struct acc { int write; uint64_t addr; int size; uint64_t val; } acc;
static acc g_log[16];
static int g_nlog;
static uint64_t g_seed = 0x0123456789ABCDEFull;

static uint64_t rnd(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}

static uint64_t mask_of(int size) { return size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1); }

/* What "memory" holds at an address: deterministic, never all-zero, with the
 * sign bit of every width set at some addresses and clear at others. */
static uint64_t pattern(uint64_t addr, int size)
{
    uint64_t v = (addr + 0x1F) * 0x9E3779B97F4A7C15ull;
    v ^= v >> 29;
    if (addr & 4) v |= 0x8080808080808080ull;
    return v & mask_of(size);
}

static uint64_t m_read(void *c, uint64_t addr, int size)
{
    (void)c;
    if (g_nlog < 16) g_log[g_nlog++] = (acc){0, addr, size, pattern(addr, size)};
    /* garbage above the access size: the emulator must mask it */
    return pattern(addr, size) | (size < 8 ? (0xA5A5A5A5A5A5A5A5ull & ~mask_of(size)) : 0);
}

static void m_write(void *c, uint64_t addr, int size, uint64_t val)
{
    (void)c;
    if (g_nlog < 16) g_log[g_nlog++] = (acc){1, addr, size, val};
}

static const a64_mem_ops k_ops = { m_read, m_write };

/* ---- part 1 + 2 ---------------------------------------------------------- */

static int64_t sx(uint64_t v, int bits)
{
    uint64_t m = 1ull << (bits - 1);
    v &= bits == 64 ? ~0ull : ((1ull << bits) - 1);
    return (int64_t)((v ^ m) - m);
}

static void random_regs(a64_regs *r)
{
    int i;
    for (i = 0; i < 31; i++) r->x[i] = rnd();
    for (i = 0; i < 32; i++) { r->v[i] = rnd(); r->v_hi[i] = rnd(); }
    r->sp = rnd() & ~15ull;
    r->pc = 0x400000;
    /* keep a few index registers small and signed so sxtw/uxtw differ */
    for (i = 0; i < 31; i += 3) r->x[i] = (uint64_t)(int64_t)(int32_t)rnd();
}

static uint64_t gpr(const a64_regs *r, int n) { return n >= 31 ? 0 : r->x[n]; }

static uint64_t ref_address(const struct vexp *e, const a64_regs *r)
{
    uint64_t base = e->rn == 31 ? r->sp : r->x[e->rn];
    if (e->rm != A64_REG_NONE) {
        uint64_t idx = gpr(r, (int)e->rm);
        if (e->ext == A64_EXT_UXTW) idx = (uint32_t)idx;
        if (e->ext == A64_EXT_SXTW) idx = (uint64_t)(int64_t)(int32_t)idx;
        return base + (idx << e->shift);
    }
    return e->post_index ? base : base + (uint64_t)e->imm;
}

static uint64_t ref_loaded(const struct vexp *e, uint64_t mem)
{
    uint64_t v = mem & mask_of((int)e->size);
    if (e->is_signed) v = (uint64_t)sx(v, (int)e->size * 8);
    if (e->reg_bits == 32) v &= 0xFFFFFFFFull;
    return v;
}

static void check_vector(const struct vec *t)
{
    a64_access a;
    int ok = a64_decode(t->insn, &a);

    if (!t->accept) {
        CHECK(!ok, "%08X %s: decoded, must be refused", t->insn, t->text);
        CHECK(a.kind == A64_NONE, "%08X: refused but kind=%d", t->insn, a.kind);
        return;
    }
    CHECK(ok, "%08X %s: not decoded", t->insn, t->text);
    if (!ok) return;

#define F(name) CHECK((long long)a.name == t->e.name, "%08X %s: " #name " = %lld, want %lld", \
                      t->insn, t->text, (long long)a.name, t->e.name)
    F(kind); F(size); F(count); F(is_load); F(is_write); F(is_fp); F(is_signed);
    F(reg_bits); F(rt); F(rt2); F(rn); F(rm); F(rs); F(ext); F(shift);
    F(writeback); F(post_index); F(acquire); F(release); F(exclusive);
    F(atomic_op); F(imm);
#undef F

    /* ---- part 2: execute it ---- */
    {
        const struct vexp *e = &t->e;
        a64_regs r0, r;
        uint64_t addr;
        int round;
        for (round = 0; round < 4; round++) {
            int k, n = 0, el, halves = e->size == 16 ? 2 : 1;
            random_regs(&r0);
            if (e->kind == A64_CAS && (round & 1)) {
                /* make the compare succeed: Rs equals memory */
                uint64_t want = pattern(ref_address(e, &r0), (int)e->size);
                if (e->rs < 31) r0.x[e->rs] = want | (rnd() << (e->size * 8 % 64) & ~mask_of((int)e->size));
            }
            r = r0;
            addr = ref_address(e, &r0);
            g_nlog = 0;
            CHECK(a64_execute(&r, &a, &k_ops, NULL) == 1, "%08X: execute refused", t->insn);
            CHECK(r.pc == r0.pc + 4, "%08X: pc not advanced by 4", t->insn);
            CHECK(a64_address(&a, &r0) == addr, "%08X %s: address %llx want %llx", t->insn,
                  t->text, (unsigned long long)a64_address(&a, &r0), (unsigned long long)addr);

            if (e->kind == A64_ATOMIC || e->kind == A64_CAS) {
                uint64_t m = mask_of((int)e->size), old = pattern(addr, (int)e->size);
                int expect_write = (e->kind == A64_ATOMIC) ? (e->atomic_op != A64_AT_LDAPR)
                                 : (old == (gpr(&r0, (int)e->rs) & m));
                CHECK(g_nlog == 1 + expect_write, "%08X %s: %d accesses", t->insn, t->text, g_nlog);
                CHECK(!g_log[0].write && g_log[0].addr == addr && g_log[0].size == e->size,
                      "%08X: atomic read wrong", t->insn);
                if (e->kind == A64_CAS) {
                    if (expect_write)
                        CHECK(g_log[1].val == (gpr(&r0, (int)e->rt) & m), "%08X: CAS stored wrong", t->insn);
                    if (e->rs < 31)
                        CHECK(r.x[e->rs] == old, "%08X: CAS Rs not old value", t->insn);
                } else if (e->rt < 31) {
                    CHECK(r.x[e->rt] == old, "%08X %s: atomic Rt %llx want %llx", t->insn, t->text,
                          (unsigned long long)r.x[e->rt], (unsigned long long)old);
                }
                continue;
            }

            CHECK(g_nlog == (int)e->count * halves, "%08X %s: %d accesses, want %d", t->insn,
                  t->text, g_nlog, (int)(e->count * halves));
            for (el = 0; el < (int)e->count; el++) {
                int reg = el ? (int)e->rt2 : (int)e->rt;
                uint64_t ea = addr + (uint64_t)el * e->size;
                for (k = 0; k < halves; k++, n++) {
                    int sz = halves == 2 ? 8 : (int)e->size;
                    uint64_t ka = ea + (uint64_t)k * 8;
                    if (n >= g_nlog) break;
                    CHECK(g_log[n].addr == ka && g_log[n].size == sz && g_log[n].write == !e->is_load,
                          "%08X %s: access %d is %s %llx/%d, want %s %llx/%d", t->insn, t->text, n,
                          g_log[n].write ? "W" : "R", (unsigned long long)g_log[n].addr, g_log[n].size,
                          e->is_load ? "R" : "W", (unsigned long long)ka, sz);
                    if (!e->is_load) {
                        uint64_t src = e->is_fp ? (k ? r0.v_hi[reg] : r0.v[reg]) : gpr(&r0, reg);
                        CHECK(g_log[n].val == (src & mask_of(sz)), "%08X %s: stored %llx", t->insn,
                              t->text, (unsigned long long)g_log[n].val);
                    }
                }
                if (e->is_load) {
                    if (e->is_fp) {
                        uint64_t lo = pattern(ea, halves == 2 ? 8 : (int)e->size);
                        uint64_t hi = halves == 2 ? pattern(ea + 8, 8) : 0;
                        CHECK(r.v[reg] == lo && r.v_hi[reg] == hi, "%08X %s: V%d = %llx:%llx", t->insn,
                              t->text, reg, (unsigned long long)r.v_hi[reg], (unsigned long long)r.v[reg]);
                    } else if (reg < 31) {
                        uint64_t want = ref_loaded(e, pattern(ea, (int)e->size));
                        CHECK(r.x[reg] == want, "%08X %s: X%d = %llx want %llx", t->insn, t->text,
                              reg, (unsigned long long)r.x[reg], (unsigned long long)want);
                    }
                }
            }
            /* Registers nobody should have touched. */
            {
                int i;
                for (i = 0; i < 31; i++) {
                    int is_dst = e->is_load && !e->is_fp && (i == e->rt || i == e->rt2);
                    int is_wb = e->writeback && e->rn == i;
                    int is_status = e->kind == A64_EXCL && e->is_write && e->rs == i;
                    if (!is_dst && !is_wb && !is_status)
                        CHECK(r.x[i] == r0.x[i], "%08X %s: X%d clobbered", t->insn, t->text, i);
                }
                for (i = 0; i < 32; i++) {
                    int is_dst = e->is_load && e->is_fp && (i == e->rt || i == e->rt2);
                    if (!is_dst)
                        CHECK(r.v[i] == r0.v[i] && r.v_hi[i] == r0.v_hi[i], "%08X: V%d clobbered", t->insn, i);
                }
                if (!(e->writeback && e->rn == 31))
                    CHECK(r.sp == r0.sp, "%08X: SP clobbered", t->insn);
            }
            if (e->writeback) {
                uint64_t base0 = e->rn == 31 ? r0.sp : r0.x[e->rn];
                uint64_t now = e->rn == 31 ? r.sp : r.x[e->rn];
                CHECK(now == base0 + (uint64_t)e->imm, "%08X %s: writeback", t->insn, t->text);
            }
            if (e->kind == A64_EXCL && e->is_write && e->rs < 31)
                CHECK(r.x[e->rs] == 0, "%08X: STXR status not 0 (success)", t->insn);
        }
    }
}

/* ---- part 3: hand-written semantics -------------------------------------- */

static uint8_t g_mem[256];
static uint64_t mem_rd(void *c, uint64_t addr, int size)
{
    uint64_t v = 0; (void)c;
    memcpy(&v, g_mem + (addr & 0xFF), (size_t)size);
    return v;
}
static void mem_wr(void *c, uint64_t addr, int size, uint64_t v)
{
    (void)c; memcpy(g_mem + (addr & 0xFF), &v, (size_t)size);
}
static const a64_mem_ops k_mem = { mem_rd, mem_wr };

/* A device: 0x1000 registers, offsets logged. */
static struct { uint32_t off[8]; int size[8]; int w[8]; uint64_t val[8]; int n; } dev;
static uint64_t dev_rd(void *d, uint32_t off, int size)
{
    (void)d;
    if (dev.n < 8) { dev.off[dev.n] = off; dev.size[dev.n] = size; dev.w[dev.n] = 0; dev.n++; }
    return 0x1000000000000000ull + off;
}
static void dev_wr(void *d, uint32_t off, uint64_t v, int size)
{
    (void)d;
    if (dev.n < 8) { dev.off[dev.n] = off; dev.size[dev.n] = size; dev.w[dev.n] = 1; dev.val[dev.n] = v; dev.n++; }
}

static void semantics(void)
{
    a64_regs r;
    uint64_t base = 0x7F0000001000ull;

    /* LDP x1, x0, [x0]: both addresses use the old x0. 0xA9400001 */
    memset(&r, 0, sizeof r);
    { uint64_t a = 0x11, b = 0x22; memcpy(g_mem + 0x40, &a, 8); memcpy(g_mem + 0x48, &b, 8); }
    r.x[0] = 0x40;
    CHECK(a64_emulate(&r, 0xA9400001u, &k_mem, NULL), "ldp x1,x0,[x0]");
    CHECK(r.x[1] == 0x11 && r.x[0] == 0x22, "ldp x1,x0,[x0]: x1=%llx x0=%llx",
          (unsigned long long)r.x[1], (unsigned long long)r.x[0]);

    /* LDR s0 clears bits 32..127 of V0; LDR q0 sets all 128. */
    memset(&r, 0, sizeof r);
    r.v[0] = ~0ull; r.v_hi[0] = ~0ull; r.x[1] = 0x80;
    memset(g_mem + 0x80, 0x3F, 16);
    CHECK(a64_emulate(&r, 0xBD400020u, &k_mem, NULL), "ldr s0,[x1]");       /* ldr s0, [x1] */
    CHECK(r.v[0] == 0x3F3F3F3Full && r.v_hi[0] == 0, "ldr s0 did not clear upper bits");
    CHECK(a64_emulate(&r, 0x3DC00020u, &k_mem, NULL), "ldr q0,[x1]");       /* ldr q0, [x1] */
    CHECK(r.v[0] == 0x3F3F3F3F3F3F3F3Full && r.v_hi[0] == 0x3F3F3F3F3F3F3F3Full, "ldr q0");

    /* LDRSB w0 vs x0 of 0x80. */
    memset(&r, 0, sizeof r);
    g_mem[0x10] = 0x80; r.x[1] = 0x10; r.x[0] = ~0ull;
    CHECK(a64_emulate(&r, 0x39C00020u, &k_mem, NULL), "ldrsb w0");          /* ldrsb w0, [x1] */
    CHECK(r.x[0] == 0xFFFFFF80ull, "ldrsb w0 = %llx", (unsigned long long)r.x[0]);
    CHECK(a64_emulate(&r, 0x39800020u, &k_mem, NULL), "ldrsb x0");          /* ldrsb x0, [x1] */
    CHECK(r.x[0] == 0xFFFFFFFFFFFFFF80ull, "ldrsb x0 = %llx", (unsigned long long)r.x[0]);

    /* ldr w0 clears the top half of x0. */
    memset(&r, 0, sizeof r);
    r.x[0] = ~0ull; r.x[1] = 0x20; memset(g_mem + 0x20, 0x12, 4);
    CHECK(a64_emulate(&r, 0xB9400020u, &k_mem, NULL) && r.x[0] == 0x12121212ull, "ldr w0 upper half");

    /* ldr xzr: discarded; str xzr: writes zero. */
    memset(&r, 0, sizeof r);
    r.x[1] = 0x30; memset(g_mem + 0x30, 0xEE, 8);
    CHECK(a64_emulate(&r, 0xF940003Fu, &k_mem, NULL), "ldr xzr");           /* ldr xzr, [x1] */
    CHECK(a64_emulate(&r, 0xF900003Fu, &k_mem, NULL) && g_mem[0x30] == 0 && g_mem[0x37] == 0,
          "str xzr writes zero");                                         /* str xzr, [x1] */

    /* LDADD, LDSMAX (signed), LDUMIN, SWP, CAS fail/succeed, STXR status. */
    memset(&r, 0, sizeof r);
    { uint32_t v = 0xFFFFFFF0u; memcpy(g_mem + 0x50, &v, 4); }
    r.x[2] = 0x50; r.x[0] = 0x20;
    CHECK(a64_emulate(&r, 0xB8200041u, &k_mem, NULL), "ldadd w0,w1,[x2]");
    CHECK(r.x[1] == 0xFFFFFFF0ull && *(uint32_t *)(g_mem + 0x50) == 0x10, "ldadd result");
    r.x[0] = 5;        /* smax(16, 5) = 16 */
    CHECK(a64_emulate(&r, 0xB8204041u, &k_mem, NULL) && *(uint32_t *)(g_mem + 0x50) == 0x10, "ldsmax");
    r.x[0] = 0xFFFFFFFFu;  /* smax(16, -1) = 16, umax = 0xFFFFFFFF */
    CHECK(a64_emulate(&r, 0xB8204041u, &k_mem, NULL) && *(uint32_t *)(g_mem + 0x50) == 0x10, "ldsmax signed");
    CHECK(a64_emulate(&r, 0xB8206041u, &k_mem, NULL) && *(uint32_t *)(g_mem + 0x50) == 0xFFFFFFFFu, "ldumax");
    r.x[0] = 7;
    CHECK(a64_emulate(&r, 0xB8208041u, &k_mem, NULL) && r.x[1] == 0xFFFFFFFFull &&
          *(uint32_t *)(g_mem + 0x50) == 7, "swp");
    r.x[0] = 6; r.x[1] = 99;  /* cas w0, w1, [x2]: 7 != 6 -> no store, w0 = 7 */
    CHECK(a64_emulate(&r, 0x88A07C41u, &k_mem, NULL) && r.x[0] == 7 && *(uint32_t *)(g_mem + 0x50) == 7, "cas fail");
    CHECK(a64_emulate(&r, 0x88A07C41u, &k_mem, NULL) && r.x[0] == 7 && *(uint32_t *)(g_mem + 0x50) == 99, "cas ok");
    r.x[11] = 5; r.x[12] = 0x1234; r.x[13] = 0x50;  /* stxr w11, w12, [x13] */
    CHECK(a64_emulate(&r, 0x880B7DACu, &k_mem, NULL) && r.x[11] == 0 &&
          *(uint32_t *)(g_mem + 0x50) == 0x1234, "stxr status");

    /* Pre/post-index on SP. str x30, [sp, #-16]!  / ldr x30, [sp], #16 */
    memset(&r, 0, sizeof r);
    r.sp = 0xA0; r.x[30] = 0xCAFE;
    CHECK(a64_emulate(&r, 0xF81F0FFEu, &k_mem, NULL) && r.sp == 0x90 && *(uint64_t *)(g_mem + 0x90) == 0xCAFE, "pre");
    r.x[30] = 0;
    CHECK(a64_emulate(&r, 0xF84107FEu, &k_mem, NULL) && r.sp == 0xA0 && r.x[30] == 0xCAFE, "post");

    /* ---- MMIO adaptor ---- */
    memset(&r, 0, sizeof r);
    /* ldr w0, [x1, #4] with x1 = base: fault at base+4, register 0x104 */
    r.x[1] = base; dev.n = 0;
    CHECK(a64_emulate_mmio(&r, 0xB9400420u, base + 4, 0x104, 0x1000, NULL, dev_rd, dev_wr), "mmio ldr");
    CHECK(dev.n == 1 && dev.off[0] == 0x104 && dev.size[0] == 4 && r.x[0] == 0x104, "mmio ldr offset");
    /* ldp w0, w1, [x2] faulting on the SECOND element: offsets 0x200, 0x204 */
    r.x[2] = base; dev.n = 0;
    CHECK(a64_emulate_mmio(&r, 0x29400440u, base + 4, 0x204, 0x1000, NULL, dev_rd, dev_wr), "mmio ldp");
    CHECK(dev.n == 2 && dev.off[0] == 0x200 && dev.off[1] == 0x204, "ldp offsets %x %x", dev.off[0], dev.off[1]);
    CHECK(r.x[0] == 0x200 && r.x[1] == 0x204, "ldp values");
    /* stp q0, q1, [x0]: four 8-byte writes at +0, +8, +16, +24 */
    r.x[0] = base; r.v[0] = 1; r.v_hi[0] = 2; r.v[1] = 3; r.v_hi[1] = 4; dev.n = 0;
    CHECK(a64_emulate_mmio(&r, 0xAD000400u, base, 0x40, 0x1000, NULL, dev_rd, dev_wr), "mmio stp q");
    CHECK(dev.n == 4 && dev.off[3] == 0x58 && dev.val[0] == 1 && dev.val[1] == 2 && dev.val[2] == 3 &&
          dev.val[3] == 4 && dev.size[2] == 8, "stp q halves");
    /* Tagged pointer (TBI): top byte differs between base and fault address. */
    r.x[1] = base | 0xB400000000000000ull; dev.n = 0;
    CHECK(a64_emulate_mmio(&r, 0xB9400420u, base + 4, 0x104, 0x1000, NULL, dev_rd, dev_wr) &&
          dev.off[0] == 0x104, "TBI");
    /* Refusals: fault not inside the access; access starts below register 0
     * (an LDP straddling RAM and the aperture); runs off the device's end;
     * undecodable instruction. None may touch the device or the registers. */
    {
        a64_regs before;
        r.x[1] = base; r.x[2] = base - 4; dev.n = 0; before = r;
        CHECK(!a64_emulate_mmio(&r, 0xB9400420u, base + 64, 0x104, 0x1000, NULL, dev_rd, dev_wr), "outside");
        CHECK(!a64_emulate_mmio(&r, 0x29400440u, base, 0x0, 0x1000, NULL, dev_rd, dev_wr), "straddle low");
        r.x[2] = base + 0xFFC; before = r;
        CHECK(!a64_emulate_mmio(&r, 0x29400440u, base + 0xFFC, 0xFFC, 0x1000, NULL, dev_rd, dev_wr), "past end");
        CHECK(a64_emulate_mmio(&r, 0x29400440u, base + 0xFFC, 0xFFC, 0, NULL, dev_rd, dev_wr), "size 0 = unbounded");
        r = before; dev.n = 0;
        CHECK(!a64_emulate_mmio(&r, 0xD503201Fu, base, 0, 0x1000, NULL, dev_rd, dev_wr), "nop refused");
        CHECK(!a64_emulate_mmio(&r, 0xF9800000u, base, 0, 0x1000, NULL, dev_rd, dev_wr), "prfm refused");
        CHECK(dev.n == 0 && memcmp(&r, &before, sizeof r) == 0, "refusal touched state");
    }
}

/* Every 32-bit word in a large sample: decode must never claim a word whose
 * top-level class is not a load/store, and must be deterministic. */
static void sweep(void)
{
    uint32_t w;
    unsigned long decoded = 0, n = 0;
    for (w = 0; ; w += 0x10001u) {          /* ~65 k words, all bit positions */
        a64_access a;
        int ok = a64_decode(w, &a);
        n++;
        if (ok) {
            decoded++;
            CHECK((w & 0x0A000000u) == 0x08000000u, "%08X decoded outside ld/st class", w);
            CHECK(a.size == 1 || a.size == 2 || a.size == 4 || a.size == 8 || a.size == 16,
                  "%08X size %d", w, a.size);
            CHECK(!(a.writeback && !a.is_fp && a.rn != 31 && (a.rn == a.rt || a.rn == a.rt2)),
                  "%08X writeback overlap accepted", w);
        }
        if (w > 0xFFFFFFFFu - 0x10001u) break;
    }
    printf("sweep: %lu words, %lu decoded\n", n, decoded);
}

int main(void)
{
    size_t i, n = sizeof k_vectors / sizeof k_vectors[0];
    int compiled = 0, refused = 0;

    for (i = 0; i < n; i++) {
        check_vector(&k_vectors[i]);
        compiled += k_vectors[i].origin_compiled;
        refused += !k_vectors[i].accept;
    }
    semantics();
    sweep();
    printf("%zu vectors (%d distinct compiler-emitted words from %d load/stores in %d configs, %d refusals)\n",
           n, compiled, VEC_HARVESTED, VEC_CONFIGS, refused);
    CHECK(compiled > 50, "too few compiled vectors: %d", compiled);
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "ARM64 DECODE TEST FAIL\n" : "ARM64 DECODE TEST PASS\n");
    return failures ? 1 : 0;
}
