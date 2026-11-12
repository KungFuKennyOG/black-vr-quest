/*
 * accessor_sample.c -- the guest-memory access shapes the generated code and
 * the runtime use, compiled by gen_vectors.py with the NDK clang at several
 * optimisation levels. Every load and store the compiler emits from this file
 * must decode (test_decode checks that), because any of them can land on a
 * trapped device aperture.
 *
 * The macros are copied from templates/runtime/recomp_types.h rather than
 * included: that header pulls <emmintrin.h>, which is the x86 host's.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esi, g_edi, g_ebx;

#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define STK8(addr)   (*(uint8_t  *)XBOX_PTR(addr))
#define STK16(addr)  (*(uint16_t *)XBOX_PTR(addr))
#define STK32(addr)  (*(uint32_t *)XBOX_PTR(addr))
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

uint32_t s_r8(uint32_t a)   { return MEM8(a); }
uint32_t s_r16(uint32_t a)  { return MEM16(a); }
uint32_t s_r32(uint32_t a)  { return MEM32(a); }
int32_t  s_rs8(uint32_t a)  { return SMEM8(a); }
int32_t  s_rs16(uint32_t a) { return SMEM16(a); }
int64_t  s_rs8q(uint32_t a) { return SMEM8(a); }
int64_t  s_rs16q(uint32_t a){ return SMEM16(a); }
int64_t  s_rs32q(uint32_t a){ return SMEM32(a); }
int64_t  s_rs64(uint32_t a) { return SMEM64(a); }
float    s_rf(uint32_t a)   { return MEMF(a); }
double   s_rd(uint32_t a)   { return MEMD(a); }
double   s_rfd(uint32_t a)  { return (double)MEMF(a); }

void s_w8(uint32_t a, uint32_t v)  { MEM8(a) = (uint8_t)v; }
void s_w16(uint32_t a, uint32_t v) { MEM16(a) = (uint16_t)v; }
void s_w32(uint32_t a, uint32_t v) { MEM32(a) = v; }
void s_w64(uint32_t a, int64_t v)  { SMEM64(a) = v; }
void s_wf(uint32_t a, float v)     { MEMF(a) = v; }
void s_wd(uint32_t a, double v)    { MEMD(a) = v; }
void s_wz(uint32_t a)              { MEM32(a) = 0; MEM16(a + 4) = 0; MEM8(a + 6) = 0; }
void s_wfz(uint32_t a)             { MEMF(a) = 0.0f; MEMD(a + 8) = 0.0; }
void s_wimm(uint32_t a)            { MEM32(a) = 0x80000001u; MEM8(a + 1) = 0x5A; }

/* Constant displacements, as the lifter emits for [reg+disp]. */
uint32_t s_disp(void)  { return MEM32(g_ecx + 0x10) + MEM32(g_ecx + 0x1804) + MEM16(g_ecx + 2) + MEM8(g_ecx - 1); }
void     s_dispw(void) { MEM32(g_ecx + 0x10) = g_eax; MEM32(g_ecx + 0x14) = g_edx; MEMF(g_ecx + 0x18) = (float)g_eax; }
/* Absolute guest addresses (device registers). */
uint32_t s_abs(void)   { return MEM32(0xFD001804u) | MEM32(0xFE820010u); }
void     s_absw(void)  { MEM32(0xFD400100u) = 1; MEM32(0xFED00008u) = g_eax; }
/* Scaled index, [base+index*4+disp]. */
uint32_t s_idx(void)   { return MEM32(g_ecx + g_eax * 4 + 8); }
void     s_idxw(void)  { MEM32(g_ecx + g_eax * 4) = g_edx; MEM16(g_ecx + g_eax * 2) = (uint16_t)g_edx; }
/* Read-modify-write and poll loops. */
void     s_rmw(void)   { MEM32(g_ecx + 4) |= 0x100; MEM32(g_ecx + 8) &= ~1u; MEM8(g_ecx) ^= 1; MEM16(g_ecx + 2) += 3; }
void     s_poll(void)  { while (!(MEM32(0xFD400700u) & 1)) {} }
uint32_t s_pollcnt(uint32_t n) { uint32_t c = 0; while ((MEM32(0xFE820010u) & 0x10) && n--) c++; return c; }
/* rep movsd / rep stosd as element loops (the device-aperture fallback). */
void s_movsd(uint32_t d, uint32_t s, uint32_t n) { while (n--) { MEM32(d) = MEM32(s); d += 4; s += 4; } }
void s_stosd(uint32_t d, uint32_t v, uint32_t n) { while (n--) { MEM32(d) = v; d += 4; } }
void s_movsb(uint32_t d, uint32_t s, uint32_t n) { while (n--) { MEM8(d++) = MEM8(s++); } }
void s_movsw(uint32_t d, uint32_t s, uint32_t n) { while (n--) { MEM16(d) = MEM16(s); d += 2; s += 2; } }
/* XMM_MEM / XMM_STORE device-span fallback, lane by lane. */
typedef union { float f[4]; uint32_t u[4]; uint64_t q[2]; double d[2]; } X;
X s_xmm(uint32_t a) { X r; r.u[0] = MEM32(a); r.u[1] = MEM32(a + 4); r.u[2] = MEM32(a + 8); r.u[3] = MEM32(a + 12); return r; }
void s_xmmw(uint32_t a, X v) { MEM32(a) = v.u[0]; MEM32(a + 4) = v.u[1]; MEM32(a + 8) = v.u[2]; MEM32(a + 12) = v.u[3]; }
void s_xmmlo(X *d, uint32_t a) { d->u[0] = MEM32(a); d->u[1] = MEM32(a + 4); }
/* movss/movsd and fld/fstp shapes. */
void s_fpu(uint32_t a, uint32_t b) { MEMF(b) = MEMF(a) * 2.0f + MEMF(a + 4); MEMD(b + 8) = MEMD(a + 8) + (double)MEMF(a + 16); }
/* 8-byte memcpy accessor (recomp_types.h line ~1100) and the non-volatile
 * stack forms, which clang is free to pair up. */
uint64_t s_q(uint32_t a) { uint64_t r; memcpy(&r, (const void *)XBOX_PTR(a), 8); return r; }
void s_qw(uint32_t a, uint64_t v) { memcpy((void *)XBOX_PTR(a), &v, 8); }
uint32_t s_stk(void) { return STK32(g_esi) + STK32(g_esi + 4) + STK16(g_esi + 8) + STK8(g_esi + 10); }
void s_stkw(uint32_t a, uint32_t b) { STK32(g_esi) = a; STK32(g_esi + 4) = b; STK32(g_esi + 8) = 0; STK32(g_esi + 12) = 0; }
void s_stkcpy(uint32_t d, uint32_t s) { memcpy((void *)XBOX_PTR(d), (const void *)XBOX_PTR(s), 16); }
void s_stkcpy32(uint32_t d, uint32_t s) { memcpy((void *)XBOX_PTR(d), (const void *)XBOX_PTR(s), 32); }
/* Guest atomics (RECOMP_ATOMIC_* on POSIX). */
uint32_t s_xadd(uint32_t a, uint32_t v) { return __sync_fetch_and_add((volatile uint32_t *)XBOX_PTR(a), v); }
uint32_t s_acq(uint32_t a) { return __atomic_load_n((volatile uint32_t *)XBOX_PTR(a), __ATOMIC_ACQUIRE); }
void     s_rel(uint32_t a, uint32_t v) { __atomic_store_n((volatile uint32_t *)XBOX_PTR(a), v, __ATOMIC_RELEASE); }
uint8_t  s_acq8(uint32_t a) { return __atomic_load_n((volatile uint8_t *)XBOX_PTR(a), __ATOMIC_SEQ_CST); }
/* A many-register function, to get callee-saved spills (stp/ldp pre/post). */
extern void sink(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, double, double);
void s_many(uint32_t a) {
    uint32_t v0 = MEM32(a), v1 = MEM32(a + 4), v2 = MEM32(a + 8), v3 = MEM32(a + 12);
    uint32_t v4 = MEM32(a + 16), v5 = MEM32(a + 20), v6 = MEM32(a + 24), v7 = MEM32(a + 28);
    double d0 = MEMD(a + 32), d1 = MEMD(a + 40);
    sink(v0, v1, v2, v3, v4, v5, v6, v7, d0, d1);
    sink(v7, v6, v5, v4, v3, v2, v1, v0, d1, d0);
    MEM32(a) = v0 + v7; MEMD(a + 32) = d0 * d1;
}
