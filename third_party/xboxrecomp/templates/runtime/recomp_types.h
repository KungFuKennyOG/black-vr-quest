/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This is a reusable template for ANY Xbox game. Game-specific
 * customization should go in separate headers.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   CreateFileMapping + MapViewOfFileEx (see xbox_memory.h).
 *   Recompiled code accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes the real guest return address (the VA of
 *   the instruction after the call); ret discards it with esp += 4.
 *   The value is never used to transfer control -- control flow is C
 *   call/return -- but it must be correct because guest code reads it
 *   (__SEH_prolog's scope table, _alloca probes, "mov eax, [esp]").
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* math.h is load-bearing, and its absence was invisible.
 *
 * The lifter emits sqrt() and fabs() for fsqrt/fabs -- 146 of them in one of
 * Halo's nine chunks alone -- and now sin/cos/tan/atan2/log2/exp2/fmod for the
 * x87 transcendentals. With no declaration in scope, C89 implicit declaration
 * makes every one of them return `int`: the caller reads EAX instead of XMM0 and
 * gets garbage, then converts that garbage to double. Vector normalisation is
 * 1/sqrt(x), so this corrupts every matrix the title builds.
 *
 * Nothing reported it because generated code is compiled with /w (see the game
 * CMakeLists) -- MSVC's C4013 was emitted and discarded. Same failure as the
 * missing stdlib.h in kernel_bridge.c, in a hotter path. */
#include <math.h>

/* ---- Host SIMD selection ------------------------------------------------
 *
 * The guest is a Pentium III, so the generated code speaks SSE: the XMM_*
 * helpers below, plus a handful of raw _mm_* calls and `.m` member accesses
 * that title-specific post-processing (scripts/generate.py) splices in.
 * Three host paths implement that vocabulary:
 *
 *   x86 / x64        the real SSE intrinsics. Unchanged.
 *   arm64            sse2neon (cdnjs-free, MIT, header-only; put the directory
 *                    holding sse2neon.h on the include path). __m128 becomes a
 *                    float32x4_t, so `.m` and the raw _mm_* calls keep working.
 *   portable         lane-wise C. Selected by RECOMP_FORCE_PORTABLE on any
 *                    host (that is how the regression in
 *                    tests/arm64_runtime checks it against SSE), and
 *                    automatically on a host with neither.
 *
 * sse2neon's default min/max are NEON fmin/fmax, which propagate NaN and order
 * -0 below +0. MINPS/MAXPS do neither -- they return the second operand when
 * the lanes are unordered or equal, and titles lean on that for clamps -- so
 * the precise variant is mandatory, not a tuning knob. */
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#  define RECOMP_HOST_X86 1
#  include <xmmintrin.h>
#  include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define RECOMP_HOST_ARM64 1
#  ifndef SSE2NEON_PRECISE_MINMAX
#    define SSE2NEON_PRECISE_MINMAX 1
#  endif
#  include "sse2neon.h"
#  if !SSE2NEON_PRECISE_MINMAX
#    error "sse2neon.h was included without SSE2NEON_PRECISE_MINMAX=1; MINPS/MAXPS need it"
#  endif
#else
#  define RECOMP_NO_M128 1
#endif
#if defined(RECOMP_FORCE_PORTABLE) || defined(RECOMP_NO_M128)
#  define RECOMP_SIMD_PORTABLE 1
#endif

/* No fused multiply-add behind the guest's back.
 *
 * x86-64 without FMA never contracts `a * b + c`; clang targeting arm64 does
 * by default (-ffp-contract=on), which rounds once instead of twice and makes
 * every lifted dot product differ from the console in the last bit -- enough
 * to flip a `fcomp; jp` and send a branch the other way. Off for the rest of
 * every translation unit that includes this header. GCC ignores the STDC form,
 * so the arm64 build also passes -ffp-contract=off (port/android). */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* Guest debug output: INT 2D, the Xbox kernel debug trap.
 *
 * eax selects the service, ecx carries its argument. Service 1 prints the
 * ANSI_STRING ecx points at -- what OutputDebugStringA compiles to. The int3
 * that always follows is the slide byte the kernel steps over, so the lifter
 * emits nothing for it; lifting it to __debugbreak() terminated the process
 * with STATUS_BREAKPOINT the first time a title tried to print. */
void recomp_debug_service(uint32_t service, uint32_t arg_va);

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions.
 * !defined(__debugbreak) is not enough on its own: MinGW declares __debugbreak
 * as a function, not a macro, so that test passes and the macro is defined
 * anyway. Any windows.h reaching this TU afterwards then fails to declare it.
 * Excluding _WIN32 outright leaves the SDK's own int3 intrinsic in place. */
#if !defined(_MSC_VER) && !defined(_WIN32) && !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during memory initialization, then read-only.
 */
#if defined(RECOMP_GENERATED_CODE) && defined(__clang__) && defined(__ANDROID__) && !defined(_WIN32) \
    && !defined(RECOMP_NO_MEMBASE_INVARIANT)
/* Generated code only (Android): declared const, which clang treats as memory no store can modify, so a function
 * loads the offset once instead of after every guest store. MEM8/16/32 are volatile and the build has
 * -fno-strict-aliasing, so with a plain declaration each guest store could alias the offset and clang reloaded it
 * before almost every guest access: ~11% of the generated arm64 instructions (port/notes/perf.md 9). The object is
 * written once, by the runtime (xbox_memory_layout.c, main.c), before any generated code runs, and never while it
 * runs; those translation units see it non-const. */
extern const ptrdiff_t g_xbox_mem_offset;
#else
extern ptrdiff_t g_xbox_mem_offset;
#endif

/* Bounds of the title's executable sections, filled in by
 * xbox_MemoryLayoutInit from the XBE's own section table. Zero means "not
 * loaded yet", which RECOMP_ICALL_IS_CODE treats as allow.
 *
 * These replace a hardcoded 0x00400000 cutoff that was only ever right for one
 * title -- see the comment where they are defined in xbox_memory_layout.c. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

/* Is this indirect-call target plausibly code?
 *
 * Synthetic kernel thunks live at 0xFE0000xx and are dispatched by
 * recomp_lookup_kernel, so they are always allowed. Everything else has to lie
 * inside an executable section of the loaded title. A target that fails this is
 * a garbage pointer that reached a call site, and calling it would be worse
 * than dropping it. */
#define RECOMP_ICALL_IS_CODE(_va) \
    ((_va) >= 0xFE000000u || g_xbox_code_hi == 0u || \
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
/* Per-thread register state.
 *
 * These started as plain globals, which works exactly as long as one thread
 * runs recompiled code. Halo is the first title to create real workers (its
 * cache/file loader), and the runtime papered over that by running every worker
 * synchronously inside PsCreateSystemThreadEx -- so a worker that blocks
 * waiting for work never returns and startup deadlocks.
 *
 * On hardware each thread has its own register set, so model it that way.
 * Thread-local costs an indirection per access; a deadlock costs the title. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

/* One TLS object for the whole register file on Android (recomp_regfile.h; port/notes/perf.md 9): a dlopen()ed
 * library only has dynamic TLS on bionic, where every separate register variable costs a TLSDESC resolver call per
 * function. The g_ names become macros for its members. Windows keeps the separate variables. */
#if !defined(RECOMP_REGFILE) && !defined(RECOMP_NO_REGFILE) && defined(__ANDROID__) && !defined(_WIN32)
#  define RECOMP_REGFILE 1
#endif
#if defined(RECOMP_REGFILE)
/* the register types first: the same definitions as below (and as src/kernel/xbox_memory_layout.h), guarded */
#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif
#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
#if defined(RECOMP_SIMD_PORTABLE)
typedef struct RecompM128 {
    float f[4] __attribute__((aligned(16)));
} RecompM128;
#  define RECOMP_M128 RecompM128
#else
#  define RECOMP_M128 __m128
#endif
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
    RECOMP_M128 m;
} RecompXmm;
#endif
#include "recomp_regfile.h"
#else
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
#endif

/* Optional BLACK diagnostic hook for unresolved call-target stubs. */
#ifdef RECOMP_TRACE_UNRESOLVED_STUBS
void recomp_unresolved_stub_log(uint32_t target_va);
#define RECOMP_UNRESOLVED_STUB_HIT(va) recomp_unresolved_stub_log(va)
#else
#define RECOMP_UNRESOLVED_STUB_HIT(va) ((void)0)
#endif

/* x87 stack. Per-thread for the same reason the integer registers are:
 * arguments are passed in st(0)/st(1) across call boundaries. */
#ifndef RECOMP_REGFILE
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
#endif

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
/* Linear base of the fs segment: where the fake TIB lives. Generated code
 * adds this to every fs-relative address, so page zero stays unmapped and a
 * null dereference faults instead of hitting the TIB. Must match
 * XBOX_FS_BASE in src/kernel/xbox_memory_layout.h.
 *
 * Per-thread, because a TIB is. fs:[0] is the SEH chain head and fs:[4]
 * reaches the CRT's per-thread data, so a single shared base makes every
 * guest thread the same thread as far as the CRT is concerned. */
#ifndef RECOMP_REGFILE
extern RECOMP_TLS uint32_t g_fs_base;
#endif
#define XBOX_FS_BASE g_fs_base

#ifndef RECOMP_REGFILE
extern RECOMP_TLS uint32_t g_seh_ebp;
#endif

/* ---- non-local jumps (setjmp / longjmp) --------------------------------
 *
 * A recompiled function is a real C function, so restoring the guest esp is
 * only half of a longjmp: the abandoned frames are still on the native stack,
 * and control returns into them once the resume point finishes. Pairing each
 * guest jmp_buf with a native one -- taken at the setjmp call site, the only
 * place a native setjmp is valid -- makes the guest jump a native jump, so the
 * frames actually unwind.
 *
 * recomp_setjmp_slot() returns the native buffer for a guest jmp_buf address,
 * allocating one on first use. recomp_guest_longjmp() restores the guest
 * registers the CRT's longjmp would and jumps; it returns 0, without jumping,
 * for a buffer that has no native counterpart, leaving the caller to fall back
 * to the guest's own longjmp.
 */
/* Atomic read-modify-write, for the lock-prefixed instructions.
 *
 * "lock xadd" and "lock cmpxchg" are what InterlockedIncrement and
 * InterlockedCompareExchange compile to, so they carry a title's reference
 * counts and its lock-free lists. They used to lift to a TODO comment, which
 * meant a refcount that never moved and a compare-and-swap that never swapped
 * -- harmless while every guest thread ran synchronously, and not once the
 * runtime began spawning real ones.
 *
 * Genuinely atomic, not merely correct in isolation: the guest's own threads
 * now run on real host threads, so a read-modify-write that races is exactly
 * the bug these instructions exist to prevent.
 */
#if defined(_MSC_VER)
#include <intrin.h>
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)_InterlockedCompareExchange((volatile long *)(p),                                            (long)(val), (long)(cmp)))
#elif defined(__aarch64__)
/* x86 executes a `lock` on any address, misaligned included (a split lock is
 * slow, not wrong). arm64 exclusives and LSE atomics need natural alignment
 * and raise SIGBUS otherwise -- Quest 3's Cortex-A78C has no LSE2 to relax
 * that. Aligned operands, the overwhelmingly common case, stay one native
 * atomic; a misaligned word inside one aligned dword is done as a CAS loop on
 * that dword; one straddling a dword boundary goes to the runtime's
 * lock-serialised helper (xbox_memory_layout.c). That last case is atomic
 * against other lock-prefixed accesses only, which is all x86 promised
 * software that never mixes plain and locked stores to one word. */
uint32_t recomp_atomic_rmw32_split(volatile uint32_t *p, int is_cas,
                                   uint32_t cmp, uint32_t val);

static inline uint32_t recomp_atomic_rmw32_unaligned(volatile uint32_t *p, int is_cas,
                                                     uint32_t cmp, uint32_t val)
{
    uintptr_t a = (uintptr_t)p;
    unsigned shift = (unsigned)(a & 7u) * 8u;
    if (shift > 32u)
        return recomp_atomic_rmw32_split(p, is_cas, cmp, val);
    volatile uint64_t *q = (volatile uint64_t *)(a & ~(uintptr_t)7);
    uint64_t mask = (uint64_t)0xFFFFFFFFu << shift;
    uint64_t old = __atomic_load_n(q, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t cur = (uint32_t)((old & mask) >> shift);
        uint32_t next = is_cas ? (cur == cmp ? val : cur) : cur + val;
        uint64_t want = (old & ~mask) | ((uint64_t)next << shift);
        if (is_cas && cur != cmp) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return cur;
        }
        if (__atomic_compare_exchange_n(q, &old, want, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return cur;
    }
}

static inline uint32_t recomp_atomic_add32(volatile uint32_t *p, uint32_t v)
{
    if (__builtin_expect(((uintptr_t)p & 3u) == 0, 1))
        return __sync_fetch_and_add(p, v);
    return recomp_atomic_rmw32_unaligned(p, 0, 0, v);
}

static inline uint32_t recomp_atomic_cas32(volatile uint32_t *p, uint32_t cmp, uint32_t val)
{
    if (__builtin_expect(((uintptr_t)p & 3u) == 0, 1))
        return __sync_val_compare_and_swap(p, cmp, val);
    return recomp_atomic_rmw32_unaligned(p, 1, cmp, val);
}
#define RECOMP_ATOMIC_ADD32(p, v)        recomp_atomic_add32((volatile uint32_t *)(p), (uint32_t)(v))
#define RECOMP_ATOMIC_CAS32(p, cmp, val) recomp_atomic_cas32((volatile uint32_t *)(p), (uint32_t)(cmp), (uint32_t)(val))
#else
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)__sync_fetch_and_add((volatile uint32_t *)(p), (uint32_t)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)__sync_val_compare_and_swap((volatile uint32_t *)(p),                                            (uint32_t)(cmp), (uint32_t)(val)))
#endif

#include <setjmp.h>
jmp_buf *recomp_setjmp_slot(uint32_t buf_va);
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);
#ifndef RECOMP_REGFILE
extern RECOMP_TLS uint32_t g_ebp;
#endif

/* EFLAGS.DF, and the signed step the string instructions take because of it.
 *
 * `cld`/`std` used to lift to a comment and every string instruction stepped
 * forwards regardless. That is right almost everywhere -- DF is 0 by ABI and
 * the compiler restores it -- but MSVC's strrchr is `std; repne scasb` from
 * the terminator backwards, so it scanned forwards off the end of the string
 * and returned a pointer into whatever followed. The Xbox Dashboard uses it to
 * split "y:\default.xip" into a mount path; with strrchr wrong the archive
 * registered itself under its own full filename, no resource in it could ever
 * be found by name, and the dashboard rebooted rather than showing a UI.
 * memmove's overlapping case is the same instruction and the same bug.
 */
#ifndef RECOMP_REGFILE
extern RECOMP_TLS int g_df;
#endif
#define RECOMP_DF_STEP(n) (g_df ? -(int32_t)(n) : (int32_t)(n))

/* x87 control and status. Thread-local for the same reason the x87 stack
   above is: one guest routine can lift to several C functions, so a compare
   and the FNSTSW that reads it can land in different bodies, and the control
   word has to survive a call. (g_fp_stack/g_fp_top are declared above.) */
#ifndef RECOMP_REGFILE
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS uint16_t g_fp_cc;
#endif
#define RECOMP_FCMP_CC(c) ((uint16_t)((c)==2 ? 0x4500u : (c)<0 ? 0x0100u : (c)>0 ? 0u : 0x4000u))
/* Round arithmetic results to the guest's 24-bit x87 significand when PC=00.
 * The precision control applies to add/subtract/multiply/divide and sqrt,
 * not loads or register copies. BLACK selects PC=00 and sorts normalized
 * byte keys using both a register result and an explicitly stored float;
 * retaining extra bits in one makes a key compare less than itself.
 *
 * A float cast would also narrow the exponent and depend on host rounding.
 * Instead round the binary64 significand using the guest RC bits. This
 * preserves the existing double stack's exponent range, signed zeros and
 * special values. PC=10/11 keep the existing binary64 approximation (not
 * 80-bit extended arithmetic). This helper rounds an already computed
 * double; it does not recover precision/range lost by the host operation
 * or emulate x87 exception flags. */
static inline double recomp_fp_precision(double value, uint16_t control) {
    uint64_t bits, magnitude, step, remainder, rounded;
    unsigned drop = 29; /* binary64 has 53 significant bits; keep 24. */
    int increment = 0;

    if (control & 0x0300u) return value;
    memcpy(&bits, &value, sizeof(bits));
    magnitude = bits & UINT64_C(0x7FFFFFFFFFFFFFFF);
    if (!magnitude ||
        (magnitude & UINT64_C(0x7FF0000000000000)) ==
                         UINT64_C(0x7FF0000000000000)) return value;

    /* Binary64 subnormals are normal values in x87's wider exponent range.
     * Count their significant bits instead of discarding 29 unconditionally. */
    if (!(magnitude & UINT64_C(0x7FF0000000000000))) {
        uint64_t significant = magnitude;
        unsigned count = 0;
        while (significant) { significant >>= 1; ++count; }
        if (count <= 24) return value;
        drop = count - 24;
    }
    step = UINT64_C(1) << drop;
    remainder = magnitude & (step - 1);
    if (!remainder) return value;
    rounded = magnitude - remainder;
    switch ((control >> 10) & 3u) {
    case 1: increment = (bits >> 63) != 0; break; /* toward -infinity */
    case 2: increment = (bits >> 63) == 0; break; /* toward +infinity */
    case 3: break;                               /* toward zero */
    default:
        increment = remainder > (step >> 1) ||
            (remainder == (step >> 1) && (rounded & step));
        break;                                  /* nearest, ties to even */
    }
    if (increment) rounded += step;
    bits = (bits & UINT64_C(0x8000000000000000)) | rounded;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Values in the existing double-backed stack are all representable as normal
 * x87 extended values, including binary64 subnormals. Empty stack tags and
 * unsupported extended encodings are not represented by this stack model. */
static inline uint16_t recomp_fxam(double value) {
    return (uint16_t)((signbit(value) ? 0x0200u : 0u) |
        (isnan(value) ? 0x0100u : isinf(value) ? 0x0500u :
         value == 0.0 ? 0x4000u : 0x0400u));
}

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case is not a curiosity: `fucompp` of a value with itself
 * followed by `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN",
 * and collapsing it to "equal" answers no every time. */
#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)
/* AH as `lahf` reads it after fcomi/fucomi set EFLAGS from a compare:
 * bit 1 always set, CF (0x01) for less, ZF (0x40) for equal, ZF|PF|CF for
 * unordered; SF and AF are cleared by those instructions. */
#define RECOMP_FCMP_LAHF(c)   ((uint8_t)(0x02u | ((c)==2 ? 0x45u : (c)<0 ? 0x01u : (c)==0 ? 0x40u : 0u)))
/* x87 integer stores use the guest RC bits, independently of host rounding.
 * Masked invalid conversions store the signed integer-indefinite value. */
static inline int64_t recomp_fist(double value, uint16_t control, unsigned bits) {
    double rounded;
    switch((control>>10)&3) {
    case 1: rounded=floor(value); break;
    case 2: rounded=ceil(value); break;
    case 3: rounded=trunc(value); break;
    default: {
        double lo=floor(value), fraction=value-lo;
        rounded=lo;
        if(fraction>0.5 || (fraction==0.5 && fmod(lo,2.0)!=0.0)) rounded=lo+1.0;
        break;
    }
    }
    double limit=ldexp(1.0,(int)bits-1);
    if(!isfinite(rounded) || rounded < -limit || rounded >= limit)
        return bits==64?INT64_MIN:-(INT64_C(1)<<(bits-1));
    return (int64_t)rounded;
}

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);

/* Report an indirect call whose target is not code (a null or wild
 * function pointer). Rate-limited per address by the implementation,
 * because these usually arrive inside a loop -- which is exactly why
 * they must be reported: silently skipping one turns a diagnosable null
 * vtable call into an unexplained hang. */
void recomp_icall_not_code_log(uint32_t va);

/* Indirect-branch target feedback. The ring buffer above is crash forensics --
 * 16 entries, overwritten constantly. This is a durable, deduplicated record of
 * every target the title ever reached, for feeding back into the next codegen
 * run (tools/recomp/icall_feedback.py).
 *
 * The header is pulled in only when the feature is on, so a default build needs
 * neither the file nor src/kernel on its include path. Disabled,
 * RECOMP_ICALL_OBSERVE discards its arguments without expanding them, so the
 * RECOMP_ICALL_SEEN_* constants need not exist either. */
#ifdef RECOMP_ICALL_FEEDBACK
#include "recomp_icall_feedback.h"
#else
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#endif

/**
 * Function entry trace, emitted only for addresses passed to
 * tools.recomp --trace-functions. Bring-up is largely "which of these
 * init calls does it not come back from", and answering that by
 * overriding a function loses the body you were trying to observe.
 */
/* The guest's time source.
 *
 * Xbox's QueryPerformanceCounter is a bare rdtsc and its
 * QueryPerformanceFrequency returns the CPU clock as a constant, so the guest
 * divides this by 733,333,333 to get seconds. Returning the host TSC would
 * make that division wrong by the ratio of the two clocks; the runtime scales
 * to the console's rate instead. */
uint64_t xbox_ReadTimeStampCounter(void);

void recomp_trace_enter(const char *name, uint32_t va);
#define RECOMP_TRACE_ENTER(name, va) recomp_trace_enter((name), (va))
void recomp_trace_exit(const char *name, uint32_t va);
#define RECOMP_TRACE_EXIT(name, va) recomp_trace_exit((name), (va))
void recomp_trace_esp(const char *name, const char *tag);
#define RECOMP_TRACE_ESP(name, tag) recomp_trace_esp((name), (tag))


/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

/**
 * Is the guest range [va, va + n) ordinary RAM rather than a device aperture?
 *
 * The block forms of `rep movs` / `rep stos` hand a whole range to the host's
 * memcpy/memset, which move 16 or 32 bytes per instruction. The device
 * apertures above RECOMP_DEVICE_BASE -- NV2A at 0xFD000000, the MCPX APU at
 * 0xFE800000, flash at 0xFF000000 -- are trapped and emulated one faulting
 * host instruction at a time, and those decoders take ordinary loads and
 * stores, not AVX. BLACK's DirectSound copies the APU's GP DSP memory
 * (0xFE830BF8) with `rep movsd`; the CRT memcpy faulted on `vmovdqu ymm0,
 * [rdx]` with nothing able to decode it. Hardware accesses such a range at the
 * element width anyway, which is what the MEM8/16/32 loops do. RAM, the
 * contiguous window at 0x80000000 and the tiled alias at 0xF0000000 all lie
 * below the base.
 */
#define RECOMP_DEVICE_BASE 0xFD000000u
#define RECOMP_RAM_SPAN(va, n) \
    ((uint64_t)(uint32_t)(va) + (uint64_t)(uint32_t)(n) \
        <= (uint64_t)RECOMP_DEVICE_BASE)

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
/* The APU's registers (0xFE800000..0xFE87FFFF): the same access as the fault handler makes, without the fault. */
uint32_t black_apu_r32(uint32_t va);
void black_apu_w32(uint32_t va, uint32_t value);
void black_copy32_slow(uint32_t dst, uint32_t src, uint32_t count);
int black_idle_yield_skip(void);
void black_level_override(uint8_t *level, uint8_t *stage);
#define APU_R32(va)       black_apu_r32(va)
#define APU_W32(va, v)    black_apu_w32((va), (v))

/** Stack accesses (esp- or ebp-based, never device memory): not volatile, so the compiler may keep stack
 * values in registers and drop dead stores. Calls still act as barriers. */
#define STK8(addr)   (*(uint8_t  *)XBOX_PTR(addr))
#define STK16(addr)  (*(uint16_t *)XBOX_PTR(addr))
#define STK32(addr)  (*(uint32_t *)XBOX_PTR(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/* ================================================================
 * SSE / XMM register state
 *
 * XMM is 128 bits of architectural state, not a scalar float. Modelling
 * it as a `float` made movaps/movups transfer 4 of 16 bytes and silently
 * drop the upper three lanes, and left the packed arithmetic with no
 * representation at all.
 *
 * The registers are global for the same reason the volatile GPRs are:
 * one guest routine can lift to several C functions, so a value produced
 * in one body and read in the next has to outlive the body that wrote it.
 * A function-local declaration would also shadow these, and the local
 * starts zeroed -- a returned float would silently read as 0.0.
 *
 * The helpers are lane-wise C rather than host intrinsics: the guest
 * semantics stay explicit (MINPS returning src on unordered, CMPNEQPS
 * being the unordered form) and the header stays portable.
 * ================================================================ */

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
#if defined(RECOMP_SIMD_PORTABLE)
/* Plain-C stand-in for __m128 when there is no SSE (or sse2neon) to lean on.
 * Same size and alignment as the real thing, so RecompXmm has one layout on
 * every host and the TLS register file is interchangeable between TUs. */
typedef struct RecompM128 {
#if defined(_MSC_VER)
    __declspec(align(16)) float f[4];
#else
    float f[4] __attribute__((aligned(16)));
#endif
} RecompM128;
#  define RECOMP_M128 RecompM128
#else
#  define RECOMP_M128 __m128
#endif
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
    RECOMP_M128 m;
} RecompXmm;
#endif

#ifndef RECOMP_REGFILE
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
#endif

/* ---- portable lane-wise reference -------------------------------------
 *
 * Always compiled, whichever host path is active: these are the guest
 * semantics written out, and the regression in tests/arm64_runtime compares
 * the active path against them. Moves go through the integer lanes so NaN
 * payloads survive bit-exactly, as they do through movaps. */

static inline RecompXmm recomp_px_lanes(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    RecompXmm r; r.u[0] = a; r.u[1] = b; r.u[2] = c; r.u[3] = d; return r;
}
static inline RecompXmm recomp_px_scalar(float v) {
    RecompXmm r = recomp_px_lanes(0, 0, 0, 0); r.f[0] = v; return r;
}
static inline RecompXmm recomp_px_scalar_double(double v) {
    RecompXmm r; r.q[1] = 0; r.d[0] = v; return r;
}
static inline RecompXmm recomp_px_movelh(RecompXmm a, RecompXmm b) {
    return recomp_px_lanes(a.u[0], a.u[1], b.u[0], b.u[1]);
}
static inline RecompXmm recomp_px_movehl(RecompXmm a, RecompXmm b) {
    return recomp_px_lanes(b.u[2], b.u[3], a.u[2], a.u[3]);
}
#define RECOMP_PX_FLOP(NAME, EXPR) \
    static inline RecompXmm NAME(RecompXmm a, RecompXmm b) { \
        RecompXmm r; int l; for (l = 0; l < 4; l++) r.f[l] = (EXPR); return r; }
RECOMP_PX_FLOP(recomp_px_add, a.f[l] + b.f[l])
RECOMP_PX_FLOP(recomp_px_sub, a.f[l] - b.f[l])
RECOMP_PX_FLOP(recomp_px_mul, a.f[l] * b.f[l])
RECOMP_PX_FLOP(recomp_px_div, a.f[l] / b.f[l])
#undef RECOMP_PX_FLOP
/* MINPS/MAXPS: the second operand on unordered or equal (so -0/+0 and NaN
 * are decided by operand order, not by IEEE minNum). */
static inline RecompXmm recomp_px_min(RecompXmm a, RecompXmm b) {
    RecompXmm r; int l;
    for (l = 0; l < 4; l++) r.u[l] = a.f[l] < b.f[l] ? a.u[l] : b.u[l];
    return r;
}
static inline RecompXmm recomp_px_max(RecompXmm a, RecompXmm b) {
    RecompXmm r; int l;
    for (l = 0; l < 4; l++) r.u[l] = a.f[l] > b.f[l] ? a.u[l] : b.u[l];
    return r;
}
#define RECOMP_PX_IOP(NAME, EXPR) \
    static inline RecompXmm NAME(RecompXmm a, RecompXmm b) { \
        RecompXmm r; int l; for (l = 0; l < 4; l++) r.u[l] = (EXPR); return r; }
RECOMP_PX_IOP(recomp_px_and,  a.u[l] & b.u[l])
RECOMP_PX_IOP(recomp_px_or,   a.u[l] | b.u[l])
RECOMP_PX_IOP(recomp_px_xor,  a.u[l] ^ b.u[l])
RECOMP_PX_IOP(recomp_px_andn, ~a.u[l] & b.u[l])
RECOMP_PX_IOP(recomp_px_cmpeq,  (a.f[l] == b.f[l]) ? 0xFFFFFFFFu : 0u)
RECOMP_PX_IOP(recomp_px_cmplt,  (a.f[l] <  b.f[l]) ? 0xFFFFFFFFu : 0u)
RECOMP_PX_IOP(recomp_px_cmple,  (a.f[l] <= b.f[l]) ? 0xFFFFFFFFu : 0u)
RECOMP_PX_IOP(recomp_px_cmpneq, !(a.f[l] == b.f[l]) ? 0xFFFFFFFFu : 0u)
RECOMP_PX_IOP(recomp_px_cmpnlt, !(a.f[l] <  b.f[l]) ? 0xFFFFFFFFu : 0u)
RECOMP_PX_IOP(recomp_px_cmpnle, !(a.f[l] <= b.f[l]) ? 0xFFFFFFFFu : 0u)
#undef RECOMP_PX_IOP
static inline uint32_t recomp_px_movemask(RecompXmm a) {
    return (a.u[0] >> 31) | ((a.u[1] >> 31) << 1) |
           ((a.u[2] >> 31) << 2) | ((a.u[3] >> 31) << 3);
}
static inline RecompXmm recomp_px_shuffle(RecompXmm a, RecompXmm b, unsigned imm) {
    return recomp_px_lanes(a.u[imm & 3], a.u[(imm >> 2) & 3],
                           b.u[(imm >> 4) & 3], b.u[(imm >> 6) & 3]);
}
static inline RecompXmm recomp_px_unpacklo(RecompXmm a, RecompXmm b) {
    return recomp_px_lanes(a.u[0], b.u[0], a.u[1], b.u[1]);
}
static inline RecompXmm recomp_px_unpackhi(RecompXmm a, RecompXmm b) {
    return recomp_px_lanes(a.u[2], b.u[2], a.u[3], b.u[3]);
}
/* The scalar compare forms (cmpltss & co.): lane 0 from the compare, lanes
 * 1-3 from the first operand. */
static inline RecompXmm recomp_px_ss(RecompXmm a, RecompXmm cmp) {
    a.u[0] = cmp.u[0]; return a;
}
static inline RecompXmm recomp_px_sqrt(RecompXmm a) {
    RecompXmm r; int l; for (l = 0; l < 4; l++) r.f[l] = sqrtf(a.f[l]); return r;
}
static inline RecompXmm recomp_px_set1(float v) {
    RecompXmm r; int l; for (l = 0; l < 4; l++) r.f[l] = v; return r;
}

/* -- construction -- */

static inline RecompXmm RecompXmmOf(RECOMP_M128 v) {
    RecompXmm r; r.m = v; return r;
}

#if defined(RECOMP_SIMD_PORTABLE)

/* Raw intrinsics that title post-processing splices into generated code
 * (scripts/generate.py repair_sse: cmp*ss, cmpnltps, rsqrtps/rcpps as
 * div/sqrt/set1). Defined only on the portable path; elsewhere they are the
 * real SSE or sse2neon functions. */
static inline RecompXmm recomp_px_of(RecompM128 v) { RecompXmm r; r.m = v; return r; }
#define RECOMP_PX_SS(NAME, CMP) \
    static inline RecompM128 NAME(RecompM128 a, RecompM128 b) { \
        RecompXmm x = recomp_px_of(a); return recomp_px_ss(x, CMP(x, recomp_px_of(b))).m; }
#define RECOMP_PX_PS(NAME, OP) \
    static inline RecompM128 NAME(RecompM128 a, RecompM128 b) { \
        return OP(recomp_px_of(a), recomp_px_of(b)).m; }
RECOMP_PX_SS(recomp_pxm_cmplt_ss,  recomp_px_cmplt)
RECOMP_PX_SS(recomp_pxm_cmple_ss,  recomp_px_cmple)
RECOMP_PX_SS(recomp_pxm_cmpeq_ss,  recomp_px_cmpeq)
RECOMP_PX_SS(recomp_pxm_cmpneq_ss, recomp_px_cmpneq)
RECOMP_PX_SS(recomp_pxm_cmpnlt_ss, recomp_px_cmpnlt)
RECOMP_PX_SS(recomp_pxm_cmpnle_ss, recomp_px_cmpnle)
RECOMP_PX_PS(recomp_pxm_cmpnlt_ps, recomp_px_cmpnlt)
RECOMP_PX_PS(recomp_pxm_cmpnle_ps, recomp_px_cmpnle)
RECOMP_PX_PS(recomp_pxm_div_ps,    recomp_px_div)
#undef RECOMP_PX_SS
#undef RECOMP_PX_PS
static inline RecompM128 recomp_pxm_sqrt_ps(RecompM128 a) { return recomp_px_sqrt(recomp_px_of(a)).m; }
static inline RecompM128 recomp_pxm_set1_ps(float v) { return recomp_px_set1(v).m; }
/* Macros so they take precedence over xmmintrin.h's functions when the
 * portable path is forced on an x86 host (the regression build). */
#define _mm_cmplt_ss  recomp_pxm_cmplt_ss
#define _mm_cmple_ss  recomp_pxm_cmple_ss
#define _mm_cmpeq_ss  recomp_pxm_cmpeq_ss
#define _mm_cmpneq_ss recomp_pxm_cmpneq_ss
#define _mm_cmpnlt_ss recomp_pxm_cmpnlt_ss
#define _mm_cmpnle_ss recomp_pxm_cmpnle_ss
#define _mm_cmpnlt_ps recomp_pxm_cmpnlt_ps
#define _mm_cmpnle_ps recomp_pxm_cmpnle_ps
#define _mm_div_ps    recomp_pxm_div_ps
#define _mm_sqrt_ps   recomp_pxm_sqrt_ps
#define _mm_set1_ps   recomp_pxm_set1_ps

static inline RecompXmm XMM_ZERO(void) { return recomp_px_lanes(0, 0, 0, 0); }
/** movss from memory: lane 0 set, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR(float v) { return recomp_px_scalar(v); }
/** movsd from memory: low double set, high double zeroed. */
static inline RecompXmm XMM_SCALAR_DOUBLE(double v) { return recomp_px_scalar_double(v); }
/** movd: 32 raw bits into lane 0, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR_BITS(uint32_t bits) { return recomp_px_lanes(bits, 0, 0, 0); }

#else /* SSE or sse2neon */

static inline RecompXmm XMM_ZERO(void) {
    return RecompXmmOf(_mm_setzero_ps());
}

/** movss from memory: lane 0 set, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR(float v) {
    return RecompXmmOf(_mm_set_ss(v));
}

/** movsd from memory: low double set, high double zeroed. */
static inline RecompXmm XMM_SCALAR_DOUBLE(double v) {
    return RecompXmmOf(_mm_castpd_ps(_mm_set_sd(v)));
}

/** movd: 32 raw bits into lane 0, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR_BITS(uint32_t bits) {
    return RecompXmmOf(_mm_castsi128_ps(_mm_cvtsi32_si128((int)bits)));
}
#endif

/* -- guest memory --
 * Addresses are guest VAs. Plain RAM is one 16-byte load or store (unaligned-safe, so movaps and movups are the
 * same); a span that reaches the device apertures goes lane by lane through MEM32, which the fault handlers decode. */

/* AArch64: a lane that goes to or comes from MEM32 passes through a general register. Left to itself clang stores
 * lane k of a vector held in a register with ST1 {v.s}[k] (and loads one with LD1), a SIMD single-structure access
 * the MMIO decoder (platform/arm64_fault_decode.c) refuses -- on exactly the device path these helpers keep for the
 * fault handlers. The empty asm only pins the value to an X/W register: one fmov/mov, no other effect. */
#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
#  define RECOMP_LANE_GPR(v) __asm__("" : "+r"(v))
#endif

static inline RecompXmm XMM_MEM(uint32_t addr) {
    RecompXmm r;
    if (RECOMP_RAM_SPAN(addr, 16u)) {
#if defined(RECOMP_SIMD_PORTABLE)
        memcpy(&r, (const void *)XBOX_PTR(addr), 16);
#else
        r.m = _mm_loadu_ps((const float *)XBOX_PTR(addr));
#endif
    } else {
#if defined(RECOMP_LANE_GPR)
        uint32_t u0 = MEM32(addr), u1 = MEM32(addr + 4), u2 = MEM32(addr + 8), u3 = MEM32(addr + 12);
        RECOMP_LANE_GPR(u0); RECOMP_LANE_GPR(u1); RECOMP_LANE_GPR(u2); RECOMP_LANE_GPR(u3);
        r.u[0] = u0; r.u[1] = u1; r.u[2] = u2; r.u[3] = u3;
#else
        r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
        r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
#endif
    }
    return r;
}

static inline void XMM_STORE(uint32_t addr, RecompXmm v) {
    if (RECOMP_RAM_SPAN(addr, 16u)) {
#if defined(RECOMP_SIMD_PORTABLE)
        memcpy((void *)XBOX_PTR(addr), &v, 16);
#else
        _mm_storeu_ps((float *)XBOX_PTR(addr), v.m);
#endif
    } else {
#if defined(RECOMP_LANE_GPR)
        uint32_t u0 = v.u[0], u1 = v.u[1], u2 = v.u[2], u3 = v.u[3];
        RECOMP_LANE_GPR(u0); RECOMP_LANE_GPR(u1); RECOMP_LANE_GPR(u2); RECOMP_LANE_GPR(u3);
        MEM32(addr) = u0; MEM32(addr + 4) = u1; MEM32(addr + 8) = u2; MEM32(addr + 12) = u3;
#else
        MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
        MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
#endif
    }
}

/* movlps/movhps move 8 bytes into or out of one half, leaving the
 * other half alone. */
#define XMM_LOAD_LOW(dst, addr)   recomp_xmm_load_half(&(dst), (addr), 0)
#define XMM_LOAD_HIGH(dst, addr)  recomp_xmm_load_half(&(dst), (addr), 1)
#define XMM_STORE_LOW(addr, src)  recomp_xmm_store_half((addr), (src), 0)
#define XMM_STORE_HIGH(addr, src) recomp_xmm_store_half((addr), (src), 1)

static inline void recomp_xmm_load_half(RecompXmm *dst, uint32_t addr,
                                        int high) {
#if defined(RECOMP_LANE_GPR)
    uint32_t lo = MEM32(addr), hi = MEM32(addr + 4);
    RECOMP_LANE_GPR(lo); RECOMP_LANE_GPR(hi);
    dst->u[high * 2] = lo; dst->u[high * 2 + 1] = hi;
#else
    dst->u[high * 2]     = MEM32(addr);
    dst->u[high * 2 + 1] = MEM32(addr + 4);
#endif
}

static inline void recomp_xmm_store_half(uint32_t addr, RecompXmm src,
                                         int high) {
#if defined(RECOMP_LANE_GPR)
    uint32_t lo = src.u[high * 2], hi = src.u[high * 2 + 1];
    RECOMP_LANE_GPR(lo); RECOMP_LANE_GPR(hi);
    MEM32(addr) = lo; MEM32(addr + 4) = hi;
#else
    MEM32(addr)     = src.u[high * 2];
    MEM32(addr + 4) = src.u[high * 2 + 1];
#endif
}

#if defined(RECOMP_SIMD_PORTABLE)

static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b) { return recomp_px_movelh(a, b); }
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b) { return recomp_px_movehl(a, b); }
static inline RecompXmm XMM_ADD(RecompXmm a, RecompXmm b) { return recomp_px_add(a, b); }
static inline RecompXmm XMM_SUB(RecompXmm a, RecompXmm b) { return recomp_px_sub(a, b); }
static inline RecompXmm XMM_MUL(RecompXmm a, RecompXmm b) { return recomp_px_mul(a, b); }
static inline RecompXmm XMM_DIV(RecompXmm a, RecompXmm b) { return recomp_px_div(a, b); }
static inline RecompXmm XMM_MIN(RecompXmm a, RecompXmm b) { return recomp_px_min(a, b); }
static inline RecompXmm XMM_MAX(RecompXmm a, RecompXmm b) { return recomp_px_max(a, b); }
static inline RecompXmm XMM_AND(RecompXmm a, RecompXmm b) { return recomp_px_and(a, b); }
static inline RecompXmm XMM_OR(RecompXmm a, RecompXmm b)  { return recomp_px_or(a, b); }
static inline RecompXmm XMM_XOR(RecompXmm a, RecompXmm b) { return recomp_px_xor(a, b); }
static inline RecompXmm XMM_ANDN(RecompXmm a, RecompXmm b) { return recomp_px_andn(a, b); }
static inline RecompXmm XMM_CMP_EQ(RecompXmm a, RecompXmm b)  { return recomp_px_cmpeq(a, b); }
static inline RecompXmm XMM_CMP_LT(RecompXmm a, RecompXmm b)  { return recomp_px_cmplt(a, b); }
static inline RecompXmm XMM_CMP_LE(RecompXmm a, RecompXmm b)  { return recomp_px_cmple(a, b); }
static inline RecompXmm XMM_CMP_NEQ(RecompXmm a, RecompXmm b) { return recomp_px_cmpneq(a, b); }
static inline uint32_t XMM_MOVEMASK(RecompXmm a) { return recomp_px_movemask(a); }
#define XMM_SHUFFLE(a, b, imm) recomp_px_shuffle((a), (b), (unsigned)((imm) & 0xFF))
static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b)  { return recomp_px_unpacklo(a, b); }
static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b) { return recomp_px_unpackhi(a, b); }

#else /* SSE or sse2neon */

/** movlhps: dst high = src low. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b) {
    return RecompXmmOf(_mm_movelh_ps(a.m, b.m));
}

/** movhlps: dst low = src high. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b) {
    return RecompXmmOf(_mm_movehl_ps(a.m, b.m));
}

/* -- packed arithmetic -- */

static inline RecompXmm XMM_ADD(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_add_ps(a.m, b.m)); }
static inline RecompXmm XMM_SUB(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_sub_ps(a.m, b.m)); }
static inline RecompXmm XMM_MUL(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_mul_ps(a.m, b.m)); }
static inline RecompXmm XMM_DIV(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_div_ps(a.m, b.m)); }
/* MINPS/MAXPS return the second operand when the lanes are unordered or equal.
 * (sse2neon: only with SSE2NEON_PRECISE_MINMAX, enforced at the top.) */
static inline RecompXmm XMM_MIN(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_min_ps(a.m, b.m)); }
static inline RecompXmm XMM_MAX(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_max_ps(a.m, b.m)); }

static inline RecompXmm XMM_AND(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_and_ps(a.m, b.m)); }
static inline RecompXmm XMM_OR(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_or_ps(a.m, b.m)); }
static inline RecompXmm XMM_XOR(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_xor_ps(a.m, b.m)); }
/* ANDNPS is ~dst & src, not dst & ~src. */
static inline RecompXmm XMM_ANDN(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_andnot_ps(a.m, b.m)); }

/* Compares produce an all-ones or all-zero mask per lane. EQ/LT/LE are
 * the ordered forms (false when either lane is NaN); NEQ is the
 * unordered form, so it is true when a lane is NaN. */
static inline RecompXmm XMM_CMP_EQ(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_cmpeq_ps(a.m, b.m)); }
static inline RecompXmm XMM_CMP_LT(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_cmplt_ps(a.m, b.m)); }
static inline RecompXmm XMM_CMP_LE(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_cmple_ps(a.m, b.m)); }
static inline RecompXmm XMM_CMP_NEQ(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_cmpneq_ps(a.m, b.m)); }

/** movmskps: the four lane sign bits, packed into the low nibble. */
static inline uint32_t XMM_MOVEMASK(RecompXmm a) {
    return (uint32_t)_mm_movemask_ps(a.m);
}

/** shufps: lanes 0-1 selected out of `a`, lanes 2-3 out of `b`. The selector is always a constant. */
#define XMM_SHUFFLE(a, b, imm) RecompXmmOf(_mm_shuffle_ps((a).m, (b).m, (imm) & 0xFF))

/** unpcklps / unpckhps: interleave the low or high halves. */
static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_unpacklo_ps(a.m, b.m)); }
static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b) { return RecompXmmOf(_mm_unpackhi_ps(a.m, b.m)); }

#endif /* RECOMP_SIMD_PORTABLE */

/* ---- scalar float -> int32 (cvttss2si / cvtss2si / cvttsd2si / cvtsd2si) --
 *
 * A C cast is undefined for NaN and out-of-range values, and the two hosts
 * disagree on what they actually do: x86 cvttss2si returns 0x80000000 (the
 * "integer indefinite"), arm64 fcvtzs saturates (NaN -> 0, +big -> INT_MAX).
 * A title that converts an uninitialised float, or clamps through an
 * overflow, then takes different paths on the two hosts.
 *
 * The lifter emits these helpers instead of a bare cast. `truncate`
 * selects cvtt* (always toward zero); otherwise the current host rounding
 * mode is used, which is where MXCSR.RC lives on x86 and where sse2neon's
 * _mm_setcsr puts it on arm64. On x86 the hardware instruction is used,
 * so behaviour there is exactly what the C cast already compiled to. */
static inline int32_t recomp_cvt_f2i32(double v, int truncate)
{
#if defined(RECOMP_HOST_X86) && !defined(RECOMP_FORCE_PORTABLE)
    return truncate ? _mm_cvttsd_si32(_mm_set_sd(v))
                    : _mm_cvtsd_si32(_mm_set_sd(v));
#else
    double rounded = truncate ? trunc(v) : nearbyint(v);
    if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0))
        return (int32_t)0x80000000u;       /* integer indefinite */
    return (int32_t)rounded;
#endif
}
/* Single-precision source (cvttss2si / cvtss2si): on x86 the very instruction
 * the old `(int32_t)float` cast compiled to, so the Windows build's code is
 * unchanged. Elsewhere it widens -- every float is exact as a double, so every
 * float-to-int32 decision is the same made on the widened value. */
static inline int32_t recomp_cvt_f2i32_ss(float v, int truncate)
{
#if defined(RECOMP_HOST_X86) && !defined(RECOMP_FORCE_PORTABLE)
    return truncate ? _mm_cvttss_si32(_mm_set_ss(v))
                    : _mm_cvtss_si32(_mm_set_ss(v));
#else
    return recomp_cvt_f2i32((double)v, truncate);
#endif
}
/* Dispatch on the operand's type: lane reads and MEMF are float, MEMD and
 * the double lanes are double (cvtt?sd2si). */
#define RECOMP_CVT_DISPATCH(v, t) _Generic((v), \
        float: recomp_cvt_f2i32_ss, \
        default: recomp_cvt_f2i32)((v), (t))
#define RECOMP_CVTT_F2I32(v) RECOMP_CVT_DISPATCH((v), 1)
#define RECOMP_CVT_F2I32(v)  RECOMP_CVT_DISPATCH((v), 0)
/* What the lifter emits for cvtss2si/cvtsd2si. Those round by MXCSR.RC on
 * hardware, but the lifter used to emit a truncating C cast for them, and
 * shipped builds were tuned against that. Truncation stays the default so a
 * rebuild changes nothing; define RECOMP_CVT_ROUND_MXCSR for the hardware
 * behaviour (BLACK has 4 such sites). */
#if defined(RECOMP_CVT_ROUND_MXCSR)
#define RECOMP_CVTR_F2I32(v) RECOMP_CVT_F2I32(v)
#else
#define RECOMP_CVTR_F2I32(v) RECOMP_CVTT_F2I32(v)
#endif

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
/* x86 evaluates the signed conditions at the operand width, not at 32 bits.
   The generated code passes LO8/HI8/LO16 sub-register reads straight in, and
   those zero-extend, so recover the width and sign-extend before comparing. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* less */
#define CMP_GE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* >= */
#define CMP_LE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* <= */
#define CMP_G(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* > */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                                     RECOMP_FLAG_WIDTH(a, b)) < 0)  /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

/* x86 masks the rotate count to 5 bits, and THEN the rotate is modulo the
 * operand's own width -- so `rol al, 16` is a rotate by zero and `rol ax, 31`
 * is a rotate by 15. A narrow rotate performed at 32 bits is not a rotate at
 * all: the bits that should wrap around at bit 7 or 15 land above the operand
 * and are discarded by the store, which turns `ror al, 2` on 0x01 into 0x00
 * where x86 gives 0x40.
 *
 * The zero case is separated out because `val >> (32 - 0)` is a shift of a
 * uint32_t by 32, which is undefined behaviour -- it happened to survive
 * because x86 masks shift counts to 5 bits and gives back `val`, but the
 * compiler is under no obligation to agree, least of all at -O2. */
static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return n ? ((val << n) | (val >> (32 - n))) : val;
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return n ? ((val >> n) | (val << (32 - n))) : val;
}

static inline uint8_t ROL8(uint8_t val, int n) {
    n = (n & 31) % 8;
    return n ? (uint8_t)((val << n) | (val >> (8 - n))) : val;
}

static inline uint8_t ROR8(uint8_t val, int n) {
    n = (n & 31) % 8;
    return n ? (uint8_t)((val >> n) | (val << (8 - n))) : val;
}

static inline uint16_t ROL16(uint16_t val, int n) {
    n = (n & 31) % 16;
    return n ? (uint16_t)((val << n) | (val >> (16 - n))) : val;
}

static inline uint16_t ROR16(uint16_t val, int n) {
    n = (n & 31) % 16;
    return n ? (uint16_t)((val >> n) | (val << (16 - n))) : val;
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    STK32(sp) = _pv; \
} while(0)

/**
 * x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits (that is what PF means). Used by the x87 float-compare idiom
 * `fnstsw ax; test ah, mask; jp/jnp`, which is how all pre-SSE code branches on
 * a float comparison. Without a real parity here the branch was hardcoded and
 * every such comparison went one fixed direction.
 */
static inline int recomp_parity8(uint32_t x) {
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = STK32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Build the flat, directly-indexed dispatch table.
 *
 * Turns recomp_lookup from a binary search over every translated function into
 * a bounds check and one indexed load -- the C form of Microsoft's
 * `jmp [table + guest_eip*8]`. Call it once at startup, before any recompiled
 * code runs.
 *
 * Entirely optional: if it is never called, or returns 0 because the allocation
 * failed, recomp_lookup keeps using the binary search and everything still
 * works. Costs 8 bytes per byte of guest code span (see
 * recomp_dispatch_flat_bytes), allocated with calloc so the untouched middle
 * stays uncommitted.
 *
 * Returns 1 if the flat table is in use, 0 if the search is.
 */
int recomp_dispatch_init(void);

/** Bytes held by the flat table, or 0 if it was never built. */
size_t recomp_dispatch_flat_bytes(void);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * RECOMP_ABI_CALL - call a resolved target, optionally checking the ABI.
 *
 * ebx, esi and edi are callee-saved on x86, and the recompiler keeps them in
 * globals rather than on the host's stack. So a lifted function that never
 * reaches its own epilogue -- the usual cause is a decode that lost sync on
 * embedded data -- does not merely lose its own state, it silently corrupts
 * every caller's.
 *
 * That failure is invisible at the crash site: the caller carries on with a
 * wrong loop cursor and simply does less work. Half-Life 2's C++ static
 * initialiser walks 5,305 constructors with esi as the cursor and edi as the
 * limit; a single callee returning with those changed ended the walk early and
 * left Source with no registered interfaces, with no error anywhere.
 *
 * Build with -DRECOMP_ABI_CHECK to have every indirect call verify them and
 * name the first offenders. esp is deliberately not checked: the convention
 * decides whether the callee pops arguments, so there is no single correct
 * value -- but there is one invariant that holds under every convention:
 * the callee at least pops its own return address, so esp must come back
 * at least 4 higher than it went in. Coming back lower means the epilogue
 * never ran, which is the failure this exists to catch.
 *
 * Covers direct calls as well as indirect: tools/recomp emits every direct
 * call through this macro, which expands to a plain call when the flag is
 * off. That matters because CRT and static-initialiser paths -- where these
 * clobbers actually bite -- are almost entirely direct calls.
 * Generated code emits a direct call as a plain C call to the symbol, with no
 * macro to hook, so a direct callee that clobbers these registers is invisible
 * here. That matters more than it sounds -- CRT and static-initialiser paths
 * are almost entirely direct calls, so this found nothing at all on Half-Life
 * 2's static init, where the clobber demonstrably exists. It is the right tool
 * for vtable-dispatch-heavy code and the wrong one for early boot.
 */
#if defined(GRX_CALL_TRACE)
/* tests/game_render_x86 equivalence build only (port/notes/perf.md 9): hash every guest call and the registers at it */
void grx_call_trace(uint32_t va);
#define RECOMP_ABI_CALL(va, fn) do { grx_call_trace((uint32_t)(va)); (fn)(); } while (0)
#elif defined(GRX_COUNT_CALLS)
/* tests/game_render_x86 measurement build only (port/notes/perf.md): count every direct guest call */
extern volatile unsigned long long g_grx_guest_calls;
extern unsigned int g_grx_call_hist[1u << 22];   /* calls per guest VA (low 22 bits), for the per-function counts */
#define RECOMP_ABI_CALL(va, fn) do { g_grx_guest_calls++; g_grx_call_hist[(uint32_t)(va) & 0x3FFFFFu]++; (fn)(); } while (0)
#elif defined(RECOMP_ABI_CHECK)
void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0);
#define RECOMP_ABI_CALL(va, fn) do { \
    uint32_t _ab = g_ebx, _as = g_esi, _ad = g_edi, _ap = g_esp; \
    (fn)(); \
    if (g_ebx != _ab || g_esi != _as || g_edi != _ad || g_esp < _ap + 4) \
        recomp_abi_violation_log((va), _ab, _as, _ad, _ap); \
} while(0)
#else
#define RECOMP_ABI_CALL(va, fn) (fn)()
#endif

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 the guest return address before this macro.
 * If not found, pops it back off to keep the stack balanced.
 *
 * RECOMP_ICALL_IS_CODE skips garbage VAs that come from uninitialized vtable
 * pointers. Kernel thunks at 0xFE000000+ are never blocked.
 *
 * Nothing to customize: the range comes from g_xbox_code_lo/g_xbox_code_hi,
 * which the memory layout fills in from the sections it actually mapped. This
 * used to say "change the VA range check to match your game's code range",
 * left over from a hardcoded 0x00400000 cutoff that was only ever right for
 * one title. Editing this header per project is no longer a thing.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); eax = 0; } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 */
#define RECOMP_ITAIL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; } \
} while(0)

/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi

/* ================================================================
 * MMX register file
 *
 * The Xbox is a Pentium III and every XDK codec leans on MMX: the WMV
 * decoder's IDCT and motion compensation are almost nothing else. Modelled the
 * same way as RecompXmm -- a union of lane views over one 64-bit register --
 * because that is what the instructions are: the same bits read as bytes,
 * words or dwords.
 *
 * mm0..mm7 alias the x87 stack on real hardware. Nothing here does, and
 * nothing needs to: a title that interleaves the two calls emms between, and
 * emms is a no-op for us. Modelling the aliasing would mean giving up the
 * separate x87 model that the FPU work depends on, to reproduce a hazard the
 * hardware exists to let software avoid.
 * ================================================================ */

#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

#ifndef RECOMP_REGFILE
extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
#endif

static inline RecompMmx MMX_ZERO(void) { RecompMmx r; r.q = 0; return r; }

/* CVTPS2PI follows MXCSR; CVTTPS2PI truncates regardless of its rounding mode.
 * Use SSE scalar conversions to avoid touching the host x87/MMX register file.
 * Non-x86 hosts use their floating-point environment for rounding instead. */
static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
    return recomp_cvt_f2i32_ss(v, truncate);
}

static inline RecompMmx MMX_FROM_PS(float lo, float hi, int truncate)
{
    RecompMmx r;
    r.d[0] = MMX_CVT_F2I(lo, truncate);
    r.d[1] = MMX_CVT_F2I(hi, truncate);
    return r;
}

/** cvtpi2ps: two signed dwords in, two singles out, into the LOW half of the
 * destination -- lanes 2 and 3 keep whatever they held. That detail is the
 * whole instruction: code that builds a float4 from two of these relies on
 * the first one surviving the second. */
static inline RecompXmm XMM_FROM_PI(RecompXmm dst, RecompMmx src)
{
    dst.f[0] = (float)src.d[0];
    dst.f[1] = (float)src.d[1];
    return dst;
}

static inline RecompMmx MMX_MEM(uint32_t addr) {
    RecompMmx r;
    memcpy(&r, (const void *)XBOX_PTR(addr), 8);
    return r;
}

static inline void MMX_STORE(uint32_t addr, RecompMmx v) {
    memcpy((void *)XBOX_PTR(addr), &v, 8);
}

static inline RecompMmx MMX_FROM32(uint32_t v) {
    RecompMmx r; r.q = 0; r.ud[0] = v; return r;
}

/* -- saturation helpers ---------------------------------------- */
static inline int16_t recomp_sat_i16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
static inline int8_t recomp_sat_i8(int32_t v) {
    return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
}
static inline uint8_t recomp_sat_u8(int32_t v) {
    return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
}

static inline uint16_t recomp_sat_u16(int32_t v) {
    return (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v));
}

/* -- integer arithmetic, lane-wise, wrapping -------------------- */
#define RECOMP_MMX_BINOP(NAME, LANES, FIELD, EXPR)                      \
    static inline RecompMmx NAME(RecompMmx a, RecompMmx b) {            \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++) { (void)b; r.FIELD[i] = (EXPR); } \
        return r;                                                       \
    }

RECOMP_MMX_BINOP(MMX_PADDB, 8, b, (int8_t)((uint8_t)a.b[i] + (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDW, 4, w, (int16_t)((uint16_t)a.w[i] + (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDD, 2, d, (int32_t)((uint32_t)a.d[i] + (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PSUBB, 8, b, (int8_t)((uint8_t)a.b[i] - (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBW, 4, w, (int16_t)((uint16_t)a.w[i] - (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBD, 2, d, (int32_t)((uint32_t)a.d[i] - (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PADDSB, 8, b, recomp_sat_i8((int32_t)a.b[i] + b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDSW, 4, w, recomp_sat_i16((int32_t)a.w[i] + b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBSB, 8, b, recomp_sat_i8((int32_t)a.b[i] - b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBSW, 4, w, recomp_sat_i16((int32_t)a.w[i] - b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] + b.ub[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] - b.ub[i]))
RECOMP_MMX_BINOP(MMX_PADDUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] + b.uw[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] - b.uw[i]))
RECOMP_MMX_BINOP(MMX_PMULLW, 4, w, (int16_t)((int32_t)a.w[i] * b.w[i]))
RECOMP_MMX_BINOP(MMX_PMULHW, 4, w, (int16_t)(((int32_t)a.w[i] * b.w[i]) >> 16))
RECOMP_MMX_BINOP(MMX_PAVGB, 8, ub, (uint8_t)(((int32_t)a.ub[i] + b.ub[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PAVGW, 4, uw, (uint16_t)(((int32_t)a.uw[i] + b.uw[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PMINSW, 4, w, (a.w[i] < b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PMAXSW, 4, w, (a.w[i] > b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PCMPEQB, 8, b, (int8_t)(a.b[i] == b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQW, 4, w, (int16_t)(a.w[i] == b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQD, 2, d, (int32_t)(a.d[i] == b.d[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTB, 8, b, (int8_t)(a.b[i] > b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTW, 4, w, (int16_t)(a.w[i] > b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTD, 2, d, (int32_t)(a.d[i] > b.d[i] ? -1 : 0))

/* pmaddwd: multiply signed words, add adjacent pairs into dwords. */
static inline RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {
    RecompMmx r;
    r.d[0] = (int32_t)a.w[0] * b.w[0] + (int32_t)a.w[1] * b.w[1];
    r.d[1] = (int32_t)a.w[2] * b.w[2] + (int32_t)a.w[3] * b.w[3];
    return r;
}

/* -- bitwise ---------------------------------------------------- */
static inline RecompMmx MMX_PAND(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q & b.q; return r; }
static inline RecompMmx MMX_PANDN(RecompMmx a, RecompMmx b) { RecompMmx r; r.q = ~a.q & b.q; return r; }
static inline RecompMmx MMX_POR(RecompMmx a, RecompMmx b)   { RecompMmx r; r.q = a.q | b.q; return r; }
static inline RecompMmx MMX_PXOR(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q ^ b.q; return r; }

/* -- shifts -----------------------------------------------------
 * A count at or past the lane width gives zero for the logical shifts and a
 * full sign fill for the arithmetic ones. The count is the whole 64-bit
 * register, not one lane, and it is unsigned -- a negative-looking count is a
 * huge one, which saturates the same way.
 */
#define RECOMP_MMX_SHIFT(NAME, LANES, FIELD, WIDTH, OP)                 \
    static inline RecompMmx NAME(RecompMmx a, uint64_t cnt) {           \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++)                                   \
            r.FIELD[i] = (cnt >= (WIDTH)) ? 0 : (OP);                   \
        return r;                                                       \
    }
RECOMP_MMX_SHIFT(MMX_PSLLW, 4, uw, 16, (uint16_t)(a.uw[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLW, 4, uw, 16, (uint16_t)(a.uw[i] >> cnt))
RECOMP_MMX_SHIFT(MMX_PSLLD, 2, ud, 32, (uint32_t)(a.ud[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLD, 2, ud, 32, (uint32_t)(a.ud[i] >> cnt))

static inline RecompMmx MMX_PSLLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q << cnt); return r;
}
static inline RecompMmx MMX_PSRLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q >> cnt); return r;
}
static inline RecompMmx MMX_PSRAW(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 16) ? 15 : cnt;
    for (i = 0; i < 4; i++) r.w[i] = (int16_t)(a.w[i] >> c);
    return r;
}
static inline RecompMmx MMX_PSRAD(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 32) ? 31 : cnt;
    for (i = 0; i < 2; i++) r.d[i] = (int32_t)(a.d[i] >> c);
    return r;
}

/* -- unpack / pack ---------------------------------------------- */
static inline RecompMmx MMX_PUNPCKLBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i]; r.ub[i*2+1] = b.ub[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i+4]; r.ub[i*2+1] = b.ub[i+4]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i]; r.uw[i*2+1] = b.uw[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i+2]; r.uw[i*2+1] = b.uw[i+2]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[0]; r.ud[1] = b.ud[0]; return r;
}
static inline RecompMmx MMX_PUNPCKHDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[1]; r.ud[1] = b.ud[1]; return r;
}
static inline RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.b[i]     = recomp_sat_i8(a.w[i]);
    for (i = 0; i < 4; i++) r.b[i + 4] = recomp_sat_i8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.ub[i]     = recomp_sat_u8(a.w[i]);
    for (i = 0; i < 4; i++) r.ub[i + 4] = recomp_sat_u8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) r.w[i]     = recomp_sat_i16(a.d[i]);
    for (i = 0; i < 2; i++) r.w[i + 2] = recomp_sat_i16(b.d[i]);
    return r;
}

/* -- word insert / extract / shuffle ----------------------------- */
static inline RecompMmx MMX_PSHUFW(RecompMmx a, uint32_t imm) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.uw[i] = a.uw[(imm >> (i * 2)) & 3];
    return r;
}
static inline RecompMmx MMX_PINSRW(RecompMmx a, uint32_t v, uint32_t imm) {
    RecompMmx r = a; r.uw[imm & 3] = (uint16_t)v; return r;
}
static inline uint32_t MMX_PEXTRW(RecompMmx a, uint32_t imm) {
    return (uint32_t)a.uw[imm & 3];
}
static inline uint32_t MMX_PMOVMSKB(RecompMmx a) {
    uint32_t m = 0; int i;
    for (i = 0; i < 8; i++) if (a.ub[i] & 0x80) m |= (1u << i);
    return m;
}
static inline RecompMmx MMX_PSADBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; uint32_t s = 0;
    for (i = 0; i < 8; i++)
        s += (uint32_t)(a.ub[i] > b.ub[i] ? a.ub[i] - b.ub[i] : b.ub[i] - a.ub[i]);
    r.q = 0; r.uw[0] = (uint16_t)s;
    return r;
}

#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

#endif /* RECOMP_TYPES_H */
