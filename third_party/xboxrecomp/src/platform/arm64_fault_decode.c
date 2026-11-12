/*
 * arm64_fault_decode.c -- AArch64 load/store decoder and emulator for trapped
 * MMIO, plus the Linux/Android signal glue. See arm64_fault_decode.h.
 *
 * Encodings follow the Arm ARM "Loads and Stores" decode tables (A64). The
 * groups handled, with the mask/value that selects each:
 *
 *   0x3B000000/0x39000000  LDR/STR (unsigned immediate), incl. B/H/SW/S/D/Q
 *   0x3B200000/0x38000000  LDUR/STUR, post-index, LDTR/STTR, pre-index (imm9)
 *   0x3B200C00/0x38200800  LDR/STR (register offset, extend/shift)
 *   0x3B200C00/0x38200000  LSE atomics (LDADD.., SWP) and LDAPR
 *   0x3A000000/0x28000000  LDP/STP/LDNP/STNP/LDPSW (W, X, S, D, Q)
 *   0x3F000000/0x08000000  LDXR/STXR/LDAXR/STLXR, LDAR/STLR, LDLAR/STLLR, CAS
 *   0x3F200C00/0x19000000  LDAPUR/STLUR (RCpc unscaled, Armv8.4)
 *
 * Refused: prefetch, SIMD structure loads (LD1..LD4, single-lane), pair
 * exclusives and CASP, MTE tag stores (STG, STGP), pointer-auth loads (LDRAA),
 * LD64B/ST64B, and every combination the architecture calls CONSTRAINED
 * UNPREDICTABLE (a writeback base that is also a data register, LDP with
 * Rt == Rt2, STXR with Rs == Rt or Rs == Rn).
 */
#include "arm64_fault_decode.h"

#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define BIT(x, n)       (((x) >> (n)) & 1u)

#define A64_ADDR_MASK 0x00FFFFFFFFFFFFFFull   /* ignore the TBI tag byte */

static int64_t sext(uint64_t v, unsigned bits)
{
    uint64_t m = 1ull << (bits - 1);
    v &= (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    return (int64_t)((v ^ m) - m);
}

static uint64_t size_mask(unsigned size)
{
    return size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1);
}

static void clear(a64_access *a)
{
    unsigned char *p = (unsigned char *)a;
    size_t i;
    for (i = 0; i < sizeof *a; i++) p[i] = 0;
    a->rt2 = A64_REG_NONE;
    a->rm  = A64_REG_NONE;
    a->rs  = A64_REG_NONE;
}

/* size/opc/V of the single-register groups -> element size, direction,
 * signedness, destination width. 0 if unallocated or a prefetch. */
static int ldst_class(unsigned size, unsigned opc, unsigned v, a64_access *a)
{
    if (v) {
        unsigned s = (size == 0 && (opc & 2)) ? 16u : (1u << size);
        if ((opc & 2) && size != 0)
            return 0;                       /* only B-size has the Q form   */
        a->is_fp    = 1;
        a->size     = (uint8_t)s;
        a->is_load  = (uint8_t)(opc & 1);
        a->is_write = (uint8_t)!(opc & 1);
        return 1;
    }
    a->size = (uint8_t)(1u << size);
    switch (opc) {
    case 0:                                 /* STR{B,H}                     */
        a->is_write = 1;
        return 1;
    case 1:                                 /* LDR{B,H}: zero-extend        */
        a->is_load  = 1;
        a->reg_bits = size == 3 ? 64 : 32;
        return 1;
    case 2:                                 /* LDRS{B,H,W} -> Xt; PRFM      */
        if (size == 3)
            return 0;
        a->is_load = 1; a->is_signed = 1; a->reg_bits = 64;
        return 1;
    default:                                /* LDRS{B,H} -> Wt              */
        if (size >= 2)
            return 0;
        a->is_load = 1; a->is_signed = 1; a->reg_bits = 32;
        return 1;
    }
}

static unsigned scale_of(const a64_access *a)
{
    switch (a->size) {
    case 1: return 0; case 2: return 1; case 4: return 2;
    case 8: return 3; default: return 4;
    }
}

/* Writeback with the base also a data register is CONSTRAINED UNPREDICTABLE
 * for GPR data. Rn == 31 is SP, never a data register. */
static int wb_overlap(const a64_access *a)
{
    if (!a->writeback || a->is_fp || a->rn == 31)
        return 0;
    return a->rn == a->rt || (a->rt2 != A64_REG_NONE && a->rn == a->rt2);
}

static int decode_single(uint32_t i, a64_access *a)
{
    unsigned size = BITS(i, 31, 30), v = BIT(i, 26), opc = BITS(i, 23, 22);

    a->kind = A64_LDST;
    a->count = 1;
    a->rt = (uint8_t)BITS(i, 4, 0);
    a->rn = (uint8_t)BITS(i, 9, 5);

    if ((i & 0x3B000000u) == 0x39000000u) {         /* unsigned immediate  */
        if (!ldst_class(size, opc, v, a)) return 0;
        a->imm = (int64_t)((uint64_t)BITS(i, 21, 10) << scale_of(a));
        return 1;
    }
    if ((i & 0x3B200000u) == 0x38000000u) {         /* imm9 forms          */
        unsigned op2 = BITS(i, 11, 10);
        if (!ldst_class(size, opc, v, a)) return 0;
        a->imm = sext(BITS(i, 20, 12), 9);
        switch (op2) {
        case 0: break;                              /* LDUR/STUR           */
        case 1: a->writeback = 1; a->post_index = 1; break;
        case 2: if (v) return 0; break;             /* LDTR/STTR           */
        case 3: a->writeback = 1; break;            /* pre-index           */
        }
        return !wb_overlap(a);
    }
    if ((i & 0x3B200C00u) == 0x38200800u) {         /* register offset     */
        unsigned option = BITS(i, 15, 13), s = BIT(i, 12);
        if (!ldst_class(size, opc, v, a)) return 0;
        switch (option) {
        case 2: a->ext = A64_EXT_UXTW; break;
        case 3: a->ext = A64_EXT_LSL;  break;
        case 6: a->ext = A64_EXT_SXTW; break;
        case 7: a->ext = A64_EXT_SXTX; break;
        default: return 0;
        }
        a->rm = (uint8_t)BITS(i, 20, 16);
        a->shift = (uint8_t)(s ? scale_of(a) : 0);
        return 1;
    }
    if ((i & 0x3B200C00u) == 0x38200000u) {         /* LSE atomics, LDAPR  */
        unsigned o3 = BIT(i, 15), aop = BITS(i, 14, 12);
        if (v) return 0;
        a->kind = A64_ATOMIC;
        a->size = (uint8_t)(1u << size);
        a->reg_bits = size == 3 ? 64 : 32;
        a->acquire = (uint8_t)BIT(i, 23);
        a->release = (uint8_t)BIT(i, 22);
        a->rs = (uint8_t)BITS(i, 20, 16);
        a->is_load = 1;
        if (!o3) {
            a->atomic_op = (uint8_t)aop;            /* ADD..UMIN = 0..7    */
            a->is_write = 1;
            return 1;
        }
        if (aop == 0) {
            a->atomic_op = A64_AT_SWP;
            a->is_write = 1;
            return 1;
        }
        if (aop == 4 && a->acquire && !a->release && a->rs == 31) {
            a->atomic_op = A64_AT_LDAPR;
            a->rs = A64_REG_NONE;
            return 1;
        }
        return 0;
    }
    return 0;
}

static int decode_pair(uint32_t i, a64_access *a)
{
    unsigned opc = BITS(i, 31, 30), v = BIT(i, 26), type = BITS(i, 24, 23);
    unsigned l = BIT(i, 22);

    a->kind = A64_PAIR;
    a->count = 2;
    a->rt  = (uint8_t)BITS(i, 4, 0);
    a->rn  = (uint8_t)BITS(i, 9, 5);
    a->rt2 = (uint8_t)BITS(i, 14, 10);
    a->is_load = (uint8_t)l;
    a->is_write = (uint8_t)!l;
    if (opc == 3) return 0;
    if (v) {
        a->is_fp = 1;
        a->size = (uint8_t)(4u << opc);             /* S, D, Q             */
    } else if (opc == 1) {
        if (!l || type == 0) return 0;              /* STGP; no LDNPSW     */
        a->size = 4; a->is_signed = 1; a->reg_bits = 64;    /* LDPSW       */
    } else {
        a->size = opc ? 8 : 4;
        a->reg_bits = opc ? 64 : 32;
    }
    a->imm = sext(BITS(i, 21, 15), 7) * (int64_t)a->size;
    switch (type) {
    case 0: case 2: break;                          /* LDNP / signed offset */
    case 1: a->writeback = 1; a->post_index = 1; break;
    case 3: a->writeback = 1; break;
    }
    if (l && a->rt == a->rt2) return 0;
    if (!l) a->reg_bits = 0;
    return !wb_overlap(a);
}

static int decode_excl(uint32_t i, a64_access *a)
{
    unsigned size = BITS(i, 31, 30), o2 = BIT(i, 23), l = BIT(i, 22);
    unsigned o1 = BIT(i, 21), o0 = BIT(i, 15);

    a->rt = (uint8_t)BITS(i, 4, 0);
    a->rn = (uint8_t)BITS(i, 9, 5);
    a->rs = (uint8_t)BITS(i, 20, 16);
    a->count = 1;
    a->size = (uint8_t)(1u << size);

    if (o2 && o1) {                                 /* CAS{A,L,AL}{B,H}    */
        if (BITS(i, 14, 10) != 31) return 0;
        a->kind = A64_CAS;
        a->acquire = (uint8_t)l;
        a->release = (uint8_t)o0;
        a->is_load = 1; a->is_write = 1;
        a->reg_bits = size == 3 ? 64 : 32;
        return 1;
    }
    if (o1) return 0;                               /* LDXP/STXP, CASP     */
    if (o2) {                                       /* LDAR/STLR/LDLAR/STLLR */
        a->kind = A64_ORDERED;
        a->rs = A64_REG_NONE;
        if (l) { a->is_load = 1; a->acquire = 1; a->reg_bits = size == 3 ? 64 : 32; }
        else   { a->is_write = 1; a->release = 1; }
        (void)o0;          /* o0=0 is the LORegion form; same access here */
        return 1;
    }
    a->kind = A64_EXCL;                             /* LDXR/LDAXR/STXR/STLXR */
    a->exclusive = 1;
    if (l) {
        a->rs = A64_REG_NONE;
        a->is_load = 1;
        a->acquire = (uint8_t)o0;
        a->reg_bits = size == 3 ? 64 : 32;
        return 1;
    }
    a->is_write = 1;
    a->release = (uint8_t)o0;
    if (a->rs == a->rt || (a->rs == a->rn && a->rn != 31)) return 0;
    return 1;
}

static int decode_rcpc_unscaled(uint32_t i, a64_access *a)
{
    unsigned size = BITS(i, 31, 30), opc = BITS(i, 23, 22);

    a->kind = A64_ORDERED;
    a->count = 1;
    a->rt = (uint8_t)BITS(i, 4, 0);
    a->rn = (uint8_t)BITS(i, 9, 5);
    a->imm = sext(BITS(i, 20, 12), 9);
    if (!ldst_class(size, opc, 0, a)) return 0;
    if (a->is_load) a->acquire = 1; else a->release = 1;
    return 1;
}

int a64_decode(uint32_t insn, a64_access *a)
{
    int ok = 0;

    clear(a);
    if ((insn & 0x0A000000u) != 0x08000000u)        /* not a load/store    */
        return 0;
    if ((insn & 0x3A000000u) == 0x38000000u)
        ok = decode_single(insn, a);
    else if ((insn & 0x3A000000u) == 0x28000000u)
        ok = decode_pair(insn, a);
    else if ((insn & 0x3F000000u) == 0x08000000u)
        ok = decode_excl(insn, a);
    else if ((insn & 0x3F200C00u) == 0x19000000u)
        ok = decode_rcpc_unscaled(insn, a);
    if (!ok)
        clear(a);
    return ok;
}

/* ===================================================================== */
/* Execution                                                             */
/* ===================================================================== */

static uint64_t xreg(const a64_regs *r, unsigned n)
{
    return n >= 31 ? 0 : r->x[n];
}

static uint64_t base_of(const a64_access *a, const a64_regs *r)
{
    return a->rn == 31 ? r->sp : r->x[a->rn];
}

uint64_t a64_address(const a64_access *a, const a64_regs *r)
{
    uint64_t base = base_of(a, r);

    if (a->rm != A64_REG_NONE) {
        uint64_t idx = xreg(r, a->rm);
        switch (a->ext) {
        case A64_EXT_UXTW: idx = (uint32_t)idx; break;
        case A64_EXT_SXTW: idx = (uint64_t)(int64_t)(int32_t)idx; break;
        default: break;
        }
        return base + (idx << a->shift);
    }
    return a->post_index ? base : base + (uint64_t)a->imm;
}

static void put_gpr(a64_regs *r, const a64_access *a, unsigned n, uint64_t v)
{
    if (n >= 31)
        return;                                     /* XZR: discarded      */
    v &= size_mask(a->size);
    if (a->is_signed)
        v = (uint64_t)sext(v, a->size * 8u);
    if (a->reg_bits == 32)
        v &= 0xFFFFFFFFull;
    r->x[n] = v;
}

static void load_elem(a64_regs *r, const a64_access *a, unsigned n,
                      uint64_t addr, const a64_mem_ops *ops, void *ctx,
                      uint64_t *lo, uint64_t *hi)
{
    (void)r; (void)n; (void)a;
    if (a->size == 16) {
        *lo = ops->read(ctx, addr, 8);
        *hi = ops->read(ctx, addr + 8, 8);
    } else {
        *lo = ops->read(ctx, addr, a->size) & size_mask(a->size);
        *hi = 0;
    }
}

static void commit_load(a64_regs *r, const a64_access *a, unsigned n,
                        uint64_t lo, uint64_t hi)
{
    if (a->is_fp) {                 /* writing Vn clears the bits above   */
        r->v[n] = lo;
        r->v_hi[n] = hi;
    } else {
        put_gpr(r, a, n, lo);
    }
}

static void store_elem(const a64_regs *r, const a64_access *a, unsigned n,
                       uint64_t addr, const a64_mem_ops *ops, void *ctx)
{
    if (a->is_fp) {
        if (a->size == 16) {
            ops->write(ctx, addr, 8, r->v[n]);
            ops->write(ctx, addr + 8, 8, r->v_hi[n]);
        } else {
            ops->write(ctx, addr, a->size, r->v[n] & size_mask(a->size));
        }
    } else {
        ops->write(ctx, addr, a->size, xreg(r, n) & size_mask(a->size));
    }
}

static uint64_t atomic_result(const a64_access *a, uint64_t old, uint64_t s)
{
    unsigned bits = a->size * 8u;
    switch (a->atomic_op) {
    case A64_AT_ADD:  return old + s;
    case A64_AT_CLR:  return old & ~s;
    case A64_AT_EOR:  return old ^ s;
    case A64_AT_SET:  return old | s;
    case A64_AT_SMAX: return sext(old, bits) > sext(s, bits) ? old : s;
    case A64_AT_SMIN: return sext(old, bits) < sext(s, bits) ? old : s;
    case A64_AT_UMAX: return old > s ? old : s;
    case A64_AT_UMIN: return old < s ? old : s;
    case A64_AT_SWP:  return s;
    default:          return old;
    }
}

int a64_execute(a64_regs *r, const a64_access *a, const a64_mem_ops *ops,
                void *ctx)
{
    uint64_t addr, m = size_mask(a->size);
    int ordered = a->acquire || a->release;

    if (a->kind == A64_NONE || !ops || !ops->read || !ops->write)
        return 0;
    addr = a64_address(a, r);
    if (ordered)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

    switch (a->kind) {
    case A64_LDST:
    case A64_ORDERED:
    case A64_PAIR: {
        unsigned k, regs[2];
        uint64_t lo[2] = {0, 0}, hi[2] = {0, 0};
        regs[0] = a->rt;
        regs[1] = a->rt2;
        if (a->is_load) {
            /* Both elements are read before either register is written:
             * LDP x0, x1, [x0] uses the old x0 for both addresses. */
            for (k = 0; k < a->count; k++)
                load_elem(r, a, regs[k], addr + (uint64_t)k * a->size, ops,
                          ctx, &lo[k], &hi[k]);
            for (k = 0; k < a->count; k++)
                commit_load(r, a, regs[k], lo[k], hi[k]);
        } else {
            for (k = 0; k < a->count; k++)
                store_elem(r, a, regs[k], addr + (uint64_t)k * a->size, ops,
                           ctx);
        }
        break;
    }
    case A64_EXCL:
        /* Single-copy: a trapped device has no exclusive monitor, and the
         * fault handler is the only path to it, so the store always
         * succeeds. */
        if (a->is_load) {
            put_gpr(r, a, a->rt, ops->read(ctx, addr, a->size));
        } else {
            ops->write(ctx, addr, a->size, xreg(r, a->rt) & m);
            if (a->rs < 31)
                r->x[a->rs] = 0;
        }
        break;
    case A64_ATOMIC: {
        uint64_t old = ops->read(ctx, addr, a->size) & m;
        if (a->atomic_op != A64_AT_LDAPR)
            ops->write(ctx, addr, a->size,
                       atomic_result(a, old, xreg(r, a->rs) & m) & m);
        put_gpr(r, a, a->rt, old);
        break;
    }
    case A64_CAS: {
        uint64_t old = ops->read(ctx, addr, a->size) & m;
        if (old == (xreg(r, a->rs) & m))
            ops->write(ctx, addr, a->size, xreg(r, a->rt) & m);
        if (a->rs < 31)
            r->x[a->rs] = old;          /* zero-extended into Ws or Xs   */
        break;
    }
    default:
        return 0;
    }

    if (ordered)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (a->writeback) {
        uint64_t nb = base_of(a, r) + (uint64_t)a->imm;
        if (a->rn == 31) r->sp = nb; else r->x[a->rn] = nb;
    }
    r->pc += 4;
    return 1;
}

int a64_emulate(a64_regs *r, uint32_t insn, const a64_mem_ops *ops, void *ctx)
{
    a64_access a;
    if (!a64_decode(insn, &a))
        return 0;
    return a64_execute(r, &a, ops, ctx);
}

/* ---- device adaptor ---------------------------------------------------- */

typedef struct mmio_ctx {
    uint64_t dev_base;              /* host address of register offset 0  */
    void *dev;
    a64_mmio_read_fn rd;
    a64_mmio_write_fn wr;
} mmio_ctx;

static uint64_t mmio_rd(void *c, uint64_t addr, int size)
{
    mmio_ctx *m = (mmio_ctx *)c;
    return m->rd(m->dev, (uint32_t)((addr & A64_ADDR_MASK) - m->dev_base),
                 size);
}

static void mmio_wr(void *c, uint64_t addr, int size, uint64_t val)
{
    mmio_ctx *m = (mmio_ctx *)c;
    m->wr(m->dev, (uint32_t)((addr & A64_ADDR_MASK) - m->dev_base), val,
          size);
}

int a64_emulate_mmio(a64_regs *r, uint32_t insn, uint64_t fault_addr,
                     uint32_t fault_off, uint32_t dev_size, void *dev,
                     a64_mmio_read_fn rd, a64_mmio_write_fn wr)
{
    static const a64_mem_ops ops = { mmio_rd, mmio_wr };
    a64_access a;
    mmio_ctx c;
    uint64_t start, span, fa;

    if (!rd || !wr || !a64_decode(insn, &a))
        return 0;
    /* Exclusives cannot be emulated one instruction at a time: an emulated
     * LDXR does not arm the CPU's exclusive monitor (and the exception
     * return clears it regardless), so the real STXR that follows fails
     * without faulting and the retry loop spins forever. Found end to end
     * under QEMU; the architecture allows the same on hardware. Refused, so
     * it is a named crash instead of a silent hang. Built for armv8.1+ (LSE),
     * atomics compile to LDADD/CAS/SWP, which are handled. */
    if (a.kind == A64_EXCL)
        return 0;
    fa    = fault_addr & A64_ADDR_MASK;
    start = a64_address(&a, r) & A64_ADDR_MASK;
    span  = a64_span(&a);
    if (fa < start || fa - start >= span)
        return 0;                   /* not this instruction's access      */
    c.dev_base = fa - fault_off;
    if (start < c.dev_base)
        return 0;                   /* begins below register offset 0     */
    if (dev_size && (start - c.dev_base) + span > dev_size)
        return 0;                   /* runs off the end of the device     */
    c.dev = dev;
    c.rd = rd;
    c.wr = wr;
    return a64_execute(r, &a, &ops, &c);
}

/* ===================================================================== */
/* Linux / Android AArch64 glue                                          */
/* ===================================================================== */
#if defined(__aarch64__) && !defined(_WIN32)

#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <asm/sigcontext.h>

#ifndef FPSIMD_MAGIC
#define FPSIMD_MAGIC 0x46508001
#endif
#ifndef EXTRA_MAGIC
#define EXTRA_MAGIC 0x45585401
#endif
#ifndef SVE_MAGIC
#define SVE_MAGIC 0x53564501
#endif

typedef struct uc_records {
    struct fpsimd_context *fp;
    unsigned char *sve;             /* sve_context record, if it has regs */
    unsigned sve_vq;
    int sve_streaming;
} uc_records;

/* Walk the __reserved record list (and the extra_context block a large
 * frame spills into). Every record is bounds-checked against its area. */
static void find_records(const ucontext_t *uc, uc_records *out)
{
    unsigned char *p   = (unsigned char *)uc->uc_mcontext.__reserved;
    unsigned char *end = p + sizeof uc->uc_mcontext.__reserved;
    unsigned char *extra = NULL;
    size_t extra_size = 0;
    int in_extra = 0;

    memset(out, 0, sizeof *out);
    for (;;) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if ((size_t)(end - p) < sizeof *h)
            break;
        if (h->magic == 0) {
            if (!in_extra && extra) {
                p = extra; end = extra + extra_size; in_extra = 1;
                continue;
            }
            break;
        }
        if (h->size < sizeof *h || h->size > (size_t)(end - p) || (h->size & 15))
            break;
        switch (h->magic) {
        case FPSIMD_MAGIC:
            if (h->size >= sizeof(struct fpsimd_context))
                out->fp = (struct fpsimd_context *)p;
            break;
        case SVE_MAGIC: {
            struct sve_context *s = (struct sve_context *)p;
            unsigned vq = s->vl / 16u;
            /* 16 = SVE_SIG_REGS_OFFSET; Z regs come first. A header-only
             * record means the live state is the fpsimd one. */
            if (vq && h->size >= 16u + 32u * 16u * vq) {
                out->sve = p;
                out->sve_vq = vq;
                out->sve_streaming = (s->flags & 1) != 0;
            }
            break;
        }
        case EXTRA_MAGIC:
            if (!in_extra) {
                struct extra_context *e = (struct extra_context *)p;
                extra = (unsigned char *)(uintptr_t)e->datap;
                extra_size = e->size;
            }
            break;
        default:
            break;
        }
        p += h->size;
    }
}

static unsigned char *sve_zreg(const uc_records *rec, unsigned n)
{
    return rec->sve + 16u + (size_t)n * 16u * rec->sve_vq;
}

int a64_uc_get(const void *ucv, a64_regs *r)
{
    const ucontext_t *uc = (const ucontext_t *)ucv;
    uc_records rec;
    int flags = 0;
    unsigned n;

    memcpy(r->x, uc->uc_mcontext.regs, sizeof r->x);
    r->sp = uc->uc_mcontext.sp;
    r->pc = uc->uc_mcontext.pc;
    memset(r->v, 0, sizeof r->v);
    memset(r->v_hi, 0, sizeof r->v_hi);

    find_records(uc, &rec);
    if (rec.sve) {
        for (n = 0; n < 32; n++) {
            memcpy(&r->v[n], sve_zreg(&rec, n), 8);
            memcpy(&r->v_hi[n], sve_zreg(&rec, n) + 8, 8);
        }
        flags |= A64_UC_HAVE_FPSIMD | A64_UC_HAVE_SVE;
        if (rec.sve_streaming) flags |= A64_UC_SVE_STREAMING;
    } else if (rec.fp) {
        for (n = 0; n < 32; n++) {
            memcpy(&r->v[n], (unsigned char *)&rec.fp->vregs[n], 8);
            memcpy(&r->v_hi[n], (unsigned char *)&rec.fp->vregs[n] + 8, 8);
        }
        flags |= A64_UC_HAVE_FPSIMD;
    }
    return flags;
}

/* Write V registers in fp_mask back to every copy the kernel may restore
 * from. A write to Vn zeroes the rest of Zn, as an FP load does. */
static int put_vregs(ucontext_t *uc, const a64_regs *r, uint32_t fp_mask)
{
    uc_records rec;
    unsigned n;

    if (!fp_mask)
        return 1;
    find_records(uc, &rec);
    if (!rec.fp && !rec.sve)
        return 0;
    for (n = 0; n < 32; n++) {
        if (!(fp_mask & (1u << n)))
            continue;
        if (rec.fp) {
            memcpy((unsigned char *)&rec.fp->vregs[n], &r->v[n], 8);
            memcpy((unsigned char *)&rec.fp->vregs[n] + 8, &r->v_hi[n], 8);
        }
        if (rec.sve) {
            unsigned char *z = sve_zreg(&rec, n);
            memset(z, 0, 16u * rec.sve_vq);
            memcpy(z, &r->v[n], 8);
            memcpy(z + 8, &r->v_hi[n], 8);
        }
    }
    return 1;
}

int a64_uc_put(void *ucv, const a64_regs *r, uint32_t fp_mask)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    if (fp_mask && !put_vregs(uc, r, fp_mask))
        return 0;
    memcpy(uc->uc_mcontext.regs, r->x, sizeof r->x);
    uc->uc_mcontext.sp = r->sp;
    uc->uc_mcontext.pc = r->pc;
    return 1;
}

uint64_t a64_uc_pc(const void *ucv)
{
    return ((const ucontext_t *)ucv)->uc_mcontext.pc;
}

uint64_t a64_uc_fault_address(const void *ucv)
{
    return ((const ucontext_t *)ucv)->uc_mcontext.fault_address;
}

uint32_t a64_uc_insn(const void *ucv)
{
    return *(const uint32_t *)(uintptr_t)a64_uc_pc(ucv);
}

int a64_uc_access_is_write(const void *ucv)
{
    a64_access a;
    if (!ucv || !a64_decode(a64_uc_insn(ucv), &a))
        return -1;
    return a.is_write ? 1 : 0;
}

int a64_uc_emulate_mmio(void *ucv, uintptr_t fault_addr, uint32_t fault_off,
                        uint32_t dev_size, void *dev,
                        a64_mmio_read_fn rd, a64_mmio_write_fn wr)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uint32_t insn;
    a64_access a;
    a64_regs r;

    if (!uc)
        return 0;
    insn = a64_uc_insn(uc);
    if (!a64_decode(insn, &a))
        return 0;
    {
        int have = a64_uc_get(uc, &r);
        if (a.is_fp && !(have & A64_UC_HAVE_FPSIMD))
            return 0;               /* cannot see the data register       */
        if (a.is_fp && (have & A64_UC_SVE_STREAMING))
            return 0;               /* SME streaming mode: not handled    */
    }
    if (!a64_emulate_mmio(&r, insn, fault_addr, fault_off, dev_size, dev,
                          rd, wr))
        return 0;
    /* The FP record was found above, so this cannot fail: the device has
     * already been accessed and the step must not be lost. */
    return a64_uc_put(uc, &r, a64_fp_dest_mask(&a));
}

/* ---- SIGSEGV dispatch -------------------------------------------------- */

#define A64_MAX_HANDLERS 16
static a64_fault_handler s_handlers[A64_MAX_HANDLERS];
static int s_nhandlers;
static struct sigaction s_prev;
static int s_installed;

static void a64_segv(int sig, siginfo_t *si, void *uc)
{
    int n = __atomic_load_n(&s_nhandlers, __ATOMIC_ACQUIRE), i;
    uintptr_t fault = (uintptr_t)si->si_addr;

    for (i = n - 1; i >= 0; i--)
        if (s_handlers[i] && s_handlers[i](uc, fault))
            return;

    /* Not ours: hand it to whoever was there before (debuggerd on Android,
     * so the tombstone is still written), or die the default way. */
    if (s_prev.sa_flags & SA_SIGINFO) {
        if (s_prev.sa_sigaction) {
            s_prev.sa_sigaction(sig, si, uc);
            return;
        }
    } else if (s_prev.sa_handler != SIG_DFL && s_prev.sa_handler != SIG_IGN) {
        s_prev.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);           /* the faulting instruction reruns     */
}

int a64_fault_add_handler(a64_fault_handler h)
{
    int n;
    if (!h)
        return 0;
    for (n = 0; n < s_nhandlers; n++)
        if (s_handlers[n] == h)
            return 1;
    n = __atomic_load_n(&s_nhandlers, __ATOMIC_ACQUIRE);
    if (n >= A64_MAX_HANDLERS)
        return 0;
    s_handlers[n] = h;
    __atomic_store_n(&s_nhandlers, n + 1, __ATOMIC_RELEASE);
    return 1;
}

int a64_fault_install(void)
{
    struct sigaction sa;

    if (__atomic_load_n(&s_installed, __ATOMIC_ACQUIRE))
        return 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = a64_segv;
    /* SA_NODEFER: a device model that itself touches a trapped aperture
     * nests, as it does under VEH, instead of the kernel killing the
     * process for a blocked synchronous signal. */
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_prev) != 0)
        return 0;
    __atomic_store_n(&s_installed, 1, __ATOMIC_RELEASE);
    return 1;
}

#endif /* __aarch64__ && !_WIN32 */
