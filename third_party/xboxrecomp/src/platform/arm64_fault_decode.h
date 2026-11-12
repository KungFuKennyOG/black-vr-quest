/*
 * arm64_fault_decode.h -- the AArch64 counterpart of mmio_decode.h.
 *
 * Trapped device registers work the same way on an AArch64 host as on x86-64:
 * the aperture is left inaccessible, the access faults, the faulting
 * instruction is decoded and serviced against the device model, and execution
 * resumes at the next instruction. What changes is the decoder. AArch64 has a
 * fixed 4-byte instruction, so there is no length to work out, but there are
 * more addressing forms (register offset with extend, pre/post-index
 * writeback, pairs) and the destination may be a SIMD/FP register.
 *
 * Two layers:
 *
 *  1. Pure (no OS headers, builds and unit-tests on any host):
 *       a64_decode()        one 32-bit instruction word -> a64_access
 *       a64_execute()       perform it on an a64_regs through callbacks
 *       a64_emulate()       decode + execute, pc += 4
 *       a64_emulate_mmio()  the same against a device: callbacks take the
 *                           register offset, as mmio_decode.h's do
 *
 *  2. Linux/Android AArch64 glue (compiled only under __aarch64__ && !_WIN32):
 *       a64_uc_get/put()    ucontext_t <-> a64_regs, including V0..V31 from
 *                           the fpsimd_context record (and SVE Z regs when the
 *                           kernel saved them, since it then restores from
 *                           there and ignores the fpsimd copy)
 *       a64_uc_emulate_mmio()  what a fault handler calls
 *       a64_fault_install() / a64_fault_add_handler()
 *                           a SIGSEGV dispatcher, because win32_compat's
 *                           AddVectoredExceptionHandler never fires on POSIX
 *
 * Anything outside the decoded set is refused (returns 0) before any device
 * callback runs: stepping over an instruction that was not understood
 * corrupts the guest silently, which is far worse than a crash naming it.
 *
 * Register 31: as Rt/Rt2/Rm/Rs it is XZR/WZR (reads 0, writes discarded); as
 * Rn (the base) it is SP. ucontext's regs[] has only 31 entries for exactly
 * this reason.
 */
#ifndef ARM64_FAULT_DECODE_H
#define ARM64_FAULT_DECODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The register state the emulator works on. */
typedef struct a64_regs {
    uint64_t x[31];        /* X0..X30                                       */
    uint64_t sp;
    uint64_t pc;
    uint64_t v[32];        /* V0..V31, bits 0..63                           */
    uint64_t v_hi[32];     /* V0..V31, bits 64..127 (Q accesses; a scalar
                              FP load clears them, as the hardware does)    */
} a64_regs;

enum a64_kind {
    A64_NONE = 0,
    A64_LDST,      /* LDR/STR family, one register                       */
    A64_PAIR,      /* LDP/STP/LDNP/STNP/LDPSW                            */
    A64_ORDERED,   /* LDAR/STLR/LDLAR/STLLR                              */
    A64_EXCL,      /* LDXR/LDAXR/STXR/STLXR (single register)            */
    A64_ATOMIC,    /* LSE LDADD/LDCLR/LDEOR/LDSET/LD{S,U}{MAX,MIN}/SWP,
                      and LDAPR (RCpc), which lives in the same group     */
    A64_CAS        /* LSE CAS/CASA/CASL/CASAL (single register)          */
};

enum a64_ext {     /* register-offset forms                              */
    A64_EXT_NONE = 0,
    A64_EXT_UXTW,  /* [Xn, Wm, uxtw #s]                                  */
    A64_EXT_LSL,   /* [Xn, Xm{, lsl #s}]                                 */
    A64_EXT_SXTW,  /* [Xn, Wm, sxtw #s]                                  */
    A64_EXT_SXTX   /* [Xn, Xm, sxtx #s]                                  */
};

enum a64_atomic_op {
    A64_AT_ADD = 0, A64_AT_CLR, A64_AT_EOR, A64_AT_SET,
    A64_AT_SMAX, A64_AT_SMIN, A64_AT_UMAX, A64_AT_UMIN,
    A64_AT_SWP, A64_AT_LDAPR
};

#define A64_REG_NONE 0xFF

/* One decoded memory access. */
typedef struct a64_access {
    uint8_t kind;          /* enum a64_kind                                  */
    uint8_t size;          /* bytes per element: 1, 2, 4, 8 or 16 (Q)        */
    uint8_t count;         /* elements: 2 for pairs, else 1                  */
    uint8_t is_load;       /* a register receives data from memory           */
    uint8_t is_write;      /* memory is written (stores; atomics are both)   */
    uint8_t is_fp;         /* Rt/Rt2 are V registers                         */
    uint8_t is_signed;     /* loaded value is sign-extended                  */
    uint8_t reg_bits;      /* width of the GPR a load writes: 32 (Wt, upper
                              half cleared) or 64; 0 for FP                   */
    uint8_t rt, rt2;       /* data registers (rt2 = A64_REG_NONE if unused)  */
    uint8_t rn;            /* base: 31 means SP                              */
    uint8_t rm;            /* index register (31 = XZR) or A64_REG_NONE      */
    uint8_t rs;            /* status/compare/operand (EXCL, ATOMIC, CAS)     */
    uint8_t ext;           /* enum a64_ext                                   */
    uint8_t shift;         /* left shift applied to the extended index       */
    uint8_t writeback;     /* base register updated                          */
    uint8_t post_index;    /* access at base, then base += imm               */
    uint8_t acquire, release, exclusive;
    uint8_t atomic_op;     /* enum a64_atomic_op                             */
    int64_t imm;           /* byte offset (already scaled)                   */
} a64_access;

/* 1 if insn is a load/store this module emulates, with *a filled in;
 * 0 otherwise (*a then has kind A64_NONE). */
int a64_decode(uint32_t insn, a64_access *a);

/* Address of the first element (pre-index applied, post-index not). */
uint64_t a64_address(const a64_access *a, const a64_regs *r);

/* The V registers a load writes (bit n = Vn); 0 for anything else. */
static inline uint32_t a64_fp_dest_mask(const a64_access *a)
{
    uint32_t m = 0;
    if (a->is_fp && a->is_load) {
        m |= 1u << a->rt;
        if (a->rt2 != A64_REG_NONE) m |= 1u << a->rt2;
    }
    return m;
}

/* Total bytes touched starting at a64_address(). */
static inline unsigned a64_span(const a64_access *a)
{
    return (unsigned)a->size * a->count;
}

/* Memory callbacks for a64_execute. Sizes are 1, 2, 4 or 8; a Q (16-byte)
 * element is two 8-byte calls, low half first. read() returns the value in
 * the low `size` bytes. */
typedef struct a64_mem_ops {
    uint64_t (*read)(void *ctx, uint64_t addr, int size);
    void     (*write)(void *ctx, uint64_t addr, int size, uint64_t val);
} a64_mem_ops;

/* Perform a decoded access on r through ops, apply writeback, pc += 4.
 * Returns 1, or 0 for a combination refused as unpredictable (nothing is
 * touched in that case). */
int a64_execute(a64_regs *r, const a64_access *a, const a64_mem_ops *ops,
                void *ctx);

/* a64_decode + a64_execute. */
int a64_emulate(a64_regs *r, uint32_t insn, const a64_mem_ops *ops,
                void *ctx);

/* Device callbacks, the same shape as mmio_decode.h's mmio_read_fn /
 * mmio_write_fn, so a device model serves both hosts unchanged. */
typedef uint64_t (*a64_mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*a64_mmio_write_fn)(void *dev, uint32_t off, uint64_t val,
                                      int size);

/* Emulate insn against a device whose register offset fault_off is at host
 * address fault_addr. The offset of every element is derived from its own
 * address relative to the fault (so LDP/STP, Q halves and a fault reported
 * on the second element all land right). Refused, with no callback made,
 * when the instruction is not decoded, when fault_addr is not inside the
 * access, or (dev_size != 0) when any byte falls outside [0, dev_size),
 * and always for LDXR/STXR (exclusive monitor; see the .c file) -- build
 * with LSE (-march=armv8.1-a or later) so atomics are LDADD/CAS/SWP.
 * Top-byte (TBI) tags are ignored when comparing addresses. */
int a64_emulate_mmio(a64_regs *r, uint32_t insn, uint64_t fault_addr,
                     uint32_t fault_off, uint32_t dev_size, void *dev,
                     a64_mmio_read_fn rd, a64_mmio_write_fn wr);

/* ===================================================================== */
/* Linux / Android AArch64 glue                                          */
/* ===================================================================== */
/* XR_HOST_FAULT_GLUE: a non-AArch64 POSIX build (tests/game_render_x86, x86-64 Linux) that supplies this glue
 * itself, so the runtime's POSIX fault paths -- written against this API -- run on that host too. Never set by
 * the Quest or Windows builds. */
#if (defined(__aarch64__) || defined(XR_HOST_FAULT_GLUE)) && !defined(_WIN32)

/* ucontext_t (passed as void * so callers need not include <ucontext.h>;
 * PCONTEXT is void * on POSIX) -> registers. Returns a bit set:
 * A64_UC_HAVE_FPSIMD if V0..V31 were loaded (else they read as zero), and
 * A64_UC_HAVE_SVE if live SVE register data was found as well. */
#define A64_UC_HAVE_FPSIMD 1
#define A64_UC_HAVE_SVE    2
#define A64_UC_SVE_STREAMING 4
int a64_uc_get(const void *uc, a64_regs *r);

/* registers -> ucontext_t: X0..X30, SP, PC, and the vector registers whose
 * bits are set in fp_mask (bit n = Vn), written to the fpsimd record and, if
 * present, the SVE Z registers (low 128 bits, the rest cleared, as an FP load
 * does). Only pass the registers that changed: rewriting the others would
 * drop their SVE upper bits. a64_fp_dest_mask() gives that set for an
 * access. Returns 0 if fp_mask is nonzero and there is nowhere to put it. */
int a64_uc_put(void *uc, const a64_regs *r, uint32_t fp_mask);

uint64_t a64_uc_pc(const void *uc);
uint64_t a64_uc_fault_address(const void *uc);   /* uc_mcontext.fault_address */
uint32_t a64_uc_insn(const void *uc);            /* *(uint32_t *)pc */

/* What Windows reports as ExceptionInformation[0]: 1 if the faulting
 * instruction writes memory (stores; atomics count as writes), 0 for a
 * load, -1 if it does not decode. Taken from the instruction, not from an
 * esr_context record, which not every kernel (or QEMU) provides. */
int a64_uc_access_is_write(const void *uc);

/* The whole job of an MMIO fault handler: decode the instruction at the
 * context's pc, service it through rd/wr, write the registers back and step
 * over it. Returns 1 if handled (resume), 0 if not (nothing changed). */
int a64_uc_emulate_mmio(void *uc, uintptr_t fault_addr, uint32_t fault_off,
                        uint32_t dev_size, void *dev,
                        a64_mmio_read_fn rd, a64_mmio_write_fn wr);

/* SIGSEGV dispatch. A handler returns nonzero if it serviced the fault (the
 * context is resumed as the handler left it), 0 to pass it on. The most
 * recently added handler runs first, like AddVectoredExceptionHandler(1, ..).
 * fault_addr is si_addr (tag bits cleared by the kernel). An unclaimed fault
 * goes to whatever handler was installed before (Android's debuggerd, so the
 * tombstone still happens), or to the default action.
 *
 * Handlers run in signal context on the faulting thread's own stack (no
 * SA_ONSTACK: device models can be stack-hungry and bionic's per-thread
 * alternate stack is small). Up to 16 handlers; add them before the faults
 * can happen, removal is not supported. */
typedef int (*a64_fault_handler)(void *uc, uintptr_t fault_addr);
int a64_fault_add_handler(a64_fault_handler h);
int a64_fault_install(void);     /* idempotent; 1 on success */

#endif /* (__aarch64__ || XR_HOST_FAULT_GLUE) && !_WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* ARM64_FAULT_DECODE_H */
