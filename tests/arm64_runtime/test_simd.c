/*
 * arm64-runtime regression: the recomp_types.h SIMD / conversion / atomic
 * helpers must give the same bits on every host path.
 *
 * Three builds of this one file:
 *   x86-64, default          real SSE path (the Windows build's semantics)
 *   x86-64, -DRECOMP_FORCE_PORTABLE   lane-wise C path
 *   arm64 (NDK, static, run under /root/xtool/arm64run)   sse2neon path
 *
 * Each build prints a digest of every result and checks it against golden
 * values captured from real SSE hardware semantics (hard-coded below), so a
 * divergence on any path fails here rather than as a wrong branch in-game.
 * Every helper is also checked against the always-compiled recomp_px_*
 * reference lanes.
 *
 * No guest memory is mapped: g_xbox_mem_offset is set so XBOX_PTR maps guest
 * VA 0 onto a host buffer.
 */
#define RECOMP_GENERATED_CODE 1
#define RECOMP_NO_MEMBASE_INVARIANT 1   /* this file defines and sets g_xbox_mem_offset itself */
#include <stdio.h>
#include <stdlib.h>
#include <fenv.h>
#include <pthread.h>
#include "recomp_types.h"

/* Storage the generated code would get from the runtime. */
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
#if defined(RECOMP_REGFILE)   /* Android: the register file is one TLS object (recomp_regfile.h) */
RECOMP_TLS RecompRegFile g_recomp_regs;
#else
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
#endif
uint32_t black_apu_r32(uint32_t va) { (void)va; return 0; }
void black_apu_w32(uint32_t va, uint32_t value) { (void)va; (void)value; }

#if defined(__aarch64__)
/* The runtime's lock-serialised fallback for a dword straddling 8 bytes. */
static pthread_mutex_t s_split = PTHREAD_MUTEX_INITIALIZER;
uint32_t recomp_atomic_rmw32_split(volatile uint32_t *p, int is_cas, uint32_t cmp, uint32_t val)
{
    uint32_t cur;
    pthread_mutex_lock(&s_split);
    memcpy(&cur, (const void *)p, 4);
    if (is_cas) { if (cur == cmp) memcpy((void *)p, &val, 4); }
    else { uint32_t n = cur + val; memcpy((void *)p, &n, 4); }
    pthread_mutex_unlock(&s_split);
    return cur;
}
#endif

static unsigned s_checks, s_fail;
static uint64_t s_digest = 1469598103934665603ull;

static void mix(uint32_t v) { s_digest = (s_digest ^ v) * 1099511628211ull; }
static void mixx(RecompXmm v) { mix(v.u[0]); mix(v.u[1]); mix(v.u[2]); mix(v.u[3]); }

static void check_eq(const char *what, RecompXmm got, RecompXmm want, int i, int j)
{
    ++s_checks;
    if (memcmp(&got, &want, 16)) {
        if (++s_fail < 40)
            printf("FAIL %s [%d,%d]: got %08X %08X %08X %08X want %08X %08X %08X %08X\n", what, i, j,
                   got.u[0], got.u[1], got.u[2], got.u[3], want.u[0], want.u[1], want.u[2], want.u[3]);
    }
}
static void check_u(const char *what, uint32_t got, uint32_t want, int i)
{
    ++s_checks;
    if (got != want && ++s_fail < 40)
        printf("FAIL %s [%d]: got %08X want %08X\n", what, i, got, want);
}

/* A spread of lane values that exercises the edges: signed zeros, NaNs with
 * payloads, infinities, denormals, ties, and values outside int32. */
static const uint32_t k_bits[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0xFFC00001u, 0x7F800001u, 0x00000001u, 0x80000001u, 0x007FFFFFu,
    0x3F000000u, 0x3FC00000u, 0x40200000u, 0xC0200000u, 0x4F000000u, 0xCF000000u,
    0x4EFFFFFFu, 0xCF000001u, 0x7F7FFFFFu, 0x42F6E979u, 0xC2F6E979u, 0x3EAAAAABu,
};
#define NBITS (int)(sizeof(k_bits) / sizeof(k_bits[0]))

static RecompXmm lanes_from(int i, int j)
{
    return recomp_px_lanes(k_bits[i % NBITS], k_bits[(i + j) % NBITS],
                           k_bits[(i * 7 + 3) % NBITS], k_bits[(j * 5 + 1) % NBITS]);
}

static void test_packed(void)
{
    for (int i = 0; i < NBITS; i++) {
        for (int j = 0; j < NBITS; j++) {
            RecompXmm a = lanes_from(i, j), b = lanes_from(j, i + 1);
#define BIN(H, R) do { RecompXmm g = H(a, b); check_eq(#H, g, R(a, b), i, j); mixx(g); } while (0)
            BIN(XMM_AND, recomp_px_and);  BIN(XMM_OR, recomp_px_or);
            BIN(XMM_XOR, recomp_px_xor);  BIN(XMM_ANDN, recomp_px_andn);
            BIN(XMM_MIN, recomp_px_min);  BIN(XMM_MAX, recomp_px_max);
            BIN(XMM_CMP_EQ, recomp_px_cmpeq);   BIN(XMM_CMP_LT, recomp_px_cmplt);
            BIN(XMM_CMP_LE, recomp_px_cmple);   BIN(XMM_CMP_NEQ, recomp_px_cmpneq);
            BIN(XMM_UNPACK_LOW, recomp_px_unpacklo); BIN(XMM_UNPACK_HIGH, recomp_px_unpackhi);
            BIN(XMM_MOVE_LOW_TO_HIGH, recomp_px_movelh); BIN(XMM_MOVE_HIGH_TO_LOW, recomp_px_movehl);
#undef BIN
            /* Arithmetic: NaN results may legitimately differ in payload
             * between x86 (first-operand quiet NaN, else default -NaN) and
             * NEON (default NaN when FPCR.DN). Compare non-NaN lanes exactly
             * and NaN-ness otherwise; the digest only takes the class. */
#define ARITH(H, R) do { RecompXmm g = H(a, b), w = R(a, b); \
                for (int l = 0; l < 4; l++) { \
                    int gn = isnan(g.f[l]), wn = isnan(w.f[l]); ++s_checks; \
                    if (gn != wn || (!gn && g.u[l] != w.u[l])) { if (++s_fail < 40) \
                        printf("FAIL %s [%d,%d] lane %d: got %08X want %08X\n", #H, i, j, l, g.u[l], w.u[l]); } \
                    mix(gn ? 0x7FC00000u : g.u[l]); } } while (0)
            ARITH(XMM_ADD, recomp_px_add); ARITH(XMM_SUB, recomp_px_sub);
            ARITH(XMM_MUL, recomp_px_mul); ARITH(XMM_DIV, recomp_px_div);
#undef ARITH
            check_u("XMM_MOVEMASK", XMM_MOVEMASK(a), recomp_px_movemask(a), i);
            mix(XMM_MOVEMASK(a));
            {
                RecompXmm g;
#define SHUF(IMM) g = XMM_SHUFFLE(a, b, IMM); check_eq("XMM_SHUFFLE", g, recomp_px_shuffle(a, b, IMM), i, IMM); mixx(g)
                SHUF(0x00); SHUF(0x1B); SHUF(0x44); SHUF(0xE4); SHUF(0xB1); SHUF(0xFF); SHUF(0x4E);
#undef SHUF
            }
            /* The raw intrinsics scripts/generate.py splices in. */
            {
                RecompXmm g;
#define RAW(EXPR, REF) g = RecompXmmOf(EXPR); check_eq(#EXPR, g, REF, i, j); mixx(g)
                RAW(_mm_cmplt_ss(a.m, b.m),  recomp_px_ss(a, recomp_px_cmplt(a, b)));
                RAW(_mm_cmpnle_ss(a.m, b.m), recomp_px_ss(a, recomp_px_cmpnle(a, b)));
                RAW(_mm_cmpeq_ss(a.m, b.m),  recomp_px_ss(a, recomp_px_cmpeq(a, b)));
                RAW(_mm_cmpneq_ss(a.m, b.m), recomp_px_ss(a, recomp_px_cmpneq(a, b)));
                RAW(_mm_cmpnlt_ps(a.m, b.m), recomp_px_cmpnlt(a, b));
#undef RAW
                g = RecompXmmOf(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(a.m)));
                {
                    RecompXmm w = recomp_px_div(recomp_px_set1(1.0f), recomp_px_sqrt(a));
                    for (int l = 0; l < 4; l++) {
                        int gn = isnan(g.f[l]), wn = isnan(w.f[l]);
                        ++s_checks;
                        if (gn != wn || (!gn && g.u[l] != w.u[l])) {
                            if (++s_fail < 40) printf("FAIL rsqrt-div [%d] lane %d: %08X vs %08X\n", i, l, g.u[l], w.u[l]);
                        }
                        mix(gn ? 0x7FC00000u : g.u[l]);
                    }
                }
            }
        }
    }
}

/* Golden x86 semantics for the conversions, independent of the reference. */
static void test_convert(void)
{
    static const struct { uint32_t bits; int32_t trunc, near, down, up; } k[] = {
        {0x00000000u, 0, 0, 0, 0},
        {0x80000000u, 0, 0, 0, 0},
        {0x3F000000u, 0, 0, 0, 1},              /* 0.5 */
        {0x3FC00000u, 1, 2, 1, 2},              /* 1.5 */
        {0x40200000u, 2, 2, 2, 3},              /* 2.5 */
        {0xC0200000u, -2, -2, -3, -2},          /* -2.5 */
        {0x4EFFFFFFu, 2147483520, 2147483520, 2147483520, 2147483520},
        {0x4F000000u, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN}, /* 2^31: indefinite */
        {0xCF000000u, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN}, /* -2^31: exact */
        {0xCF000001u, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN}, /* below: indefinite */
        {0x7F800000u, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
        {0xFF800000u, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
        {0x7FC00000u, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
        {0x42F6E979u, 123, 123, 123, 124},      /* 123.456 */
        {0xC2F6E979u, -123, -123, -124, -123},
    };
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD};
    for (unsigned n = 0; n < sizeof(k) / sizeof(k[0]); n++) {
        volatile float f; memcpy((void *)&f, &k[n].bits, 4);
        check_u("cvttss2si", (uint32_t)RECOMP_CVTT_F2I32(f), (uint32_t)k[n].trunc, (int)n);
        check_u("cvttsd2si", (uint32_t)RECOMP_CVTT_F2I32((double)f), (uint32_t)k[n].trunc, (int)n);
        mix((uint32_t)RECOMP_CVTT_F2I32(f));
        for (int m = 0; m < 3; m++) {
            int32_t want = m == 0 ? k[n].near : m == 1 ? k[n].down : k[n].up;
            fesetround(modes[m]);
            /* The SSE path reads MXCSR.RC; set it the way the guest's ldmxcsr
             * would. On arm64 sse2neon maps both onto FPCR.RMode. */
#if !defined(RECOMP_SIMD_PORTABLE)
            _MM_SET_ROUNDING_MODE(m == 0 ? _MM_ROUND_NEAREST : m == 1 ? _MM_ROUND_DOWN : _MM_ROUND_UP);
#endif
            int32_t got = RECOMP_CVT_F2I32(f);
            fesetround(FE_TONEAREST);
#if !defined(RECOMP_SIMD_PORTABLE)
            _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
#endif
            check_u("cvtss2si", (uint32_t)got, (uint32_t)want, (int)(n * 4 + m));
            mix((uint32_t)got);
        }
        {
            RecompMmx r = MMX_FROM_PS(f, -f, 1);
            check_u("cvttps2pi.lo", r.ud[0], (uint32_t)k[n].trunc, (int)n);
            mix(r.ud[0]); mix(r.ud[1]);
        }
    }
}

/* MINPS/MAXPS operand-order rules, against golden x86 answers. */
static void test_minmax_golden(void)
{
    RecompXmm a = recomp_px_lanes(0x80000000u, 0x7FC00000u, 0x3F800000u, 0x00000000u);
    RecompXmm b = recomp_px_lanes(0x00000000u, 0x3F800000u, 0x7FC00001u, 0x80000000u);
    RecompXmm mn = XMM_MIN(a, b), mx = XMM_MAX(a, b);
    /* -0 vs +0 -> second; NaN anywhere -> second operand. */
    check_eq("minps golden", mn, recomp_px_lanes(0x00000000u, 0x3F800000u, 0x7FC00001u, 0x80000000u), 0, 0);
    check_eq("maxps golden", mx, recomp_px_lanes(0x00000000u, 0x3F800000u, 0x7FC00001u, 0x80000000u), 0, 1);
}

static void test_memory_and_atomics(void)
{
    static uint8_t ram[256] __attribute__((aligned(64)));
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)ram;
    for (int i = 0; i < 256; i++) ram[i] = (uint8_t)(i * 37 + 11);
    for (uint32_t off = 0; off < 32; off++) {
        RecompXmm v = XMM_MEM(off), w;
        memcpy(&w, ram + off, 16);
        check_eq("XMM_MEM unaligned", v, w, (int)off, 0);
        XMM_STORE(off + 100, v);
        check_u("XMM_STORE", (uint32_t)memcmp(ram + off + 100, &w, 16), 0, (int)off);
        RecompMmx m = MMX_MEM(off);
        MMX_STORE(off + 200, m);
        check_u("MMX roundtrip", (uint32_t)memcmp(ram + off, ram + off + 200, 8), 0, (int)off);
    }
    /* lock xadd / lock cmpxchg at every byte offset: x86 accepts them all,
     * arm64 SIGBUSes on a plain atomic at a misaligned address. */
    for (uint32_t off = 0; off < 16; off++) {
        uint32_t base = 0x40 + off, init = 0x01020304u * (off + 1), old;
        for (int b = 0; b < 256; b++) ram[b] = (uint8_t)(b * 37 + 11);
        memcpy(ram + base, &init, 4);
        old = RECOMP_ATOMIC_ADD32(XBOX_PTR(base), 0x10u);
        check_u("xadd old", old, init, (int)off);
        memcpy(&old, ram + base, 4);
        check_u("xadd new", old, init + 0x10u, (int)off);
        old = RECOMP_ATOMIC_CAS32(XBOX_PTR(base), 0xDEADBEEFu, 1u);      /* fails */
        check_u("cas miss", old, init + 0x10u, (int)off);
        old = RECOMP_ATOMIC_CAS32(XBOX_PTR(base), init + 0x10u, 0xCAFEF00Du); /* succeeds */
        check_u("cas hit old", old, init + 0x10u, (int)off);
        memcpy(&old, ram + base, 4);
        check_u("cas hit new", old, 0xCAFEF00Du, (int)off);
        /* neighbours untouched: the arm64 path rewrites the containing
         * aligned dword(s), so the bytes either side must survive it */
        check_u("neighbour lo", ram[base - 1], (uint8_t)((base - 1) * 37 + 11), (int)off);
        check_u("neighbour hi", ram[base + 4], (uint8_t)((base + 4) * 37 + 11), (int)off);
        mix(old);
    }
}

/* Contended misaligned xadd from several threads must not lose increments. */
#define NTHREADS 4
#define NITER 20000
static uint8_t s_shared[64] __attribute__((aligned(64)));
static void *adder(void *arg)
{
    uint32_t off = (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < NITER; i++) RECOMP_ATOMIC_ADD32(s_shared + off, 1u);
    return NULL;
}
static void test_contended(uint32_t off)
{
    pthread_t t[NTHREADS];
    uint32_t v = 0;
    memset(s_shared, 0, sizeof(s_shared));
    for (int i = 0; i < NTHREADS; i++) pthread_create(&t[i], NULL, adder, (void *)(uintptr_t)off);
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
    memcpy(&v, s_shared + off, 4);
    check_u("contended xadd", v, NTHREADS * NITER, (int)off);
}

static void test_fp_contract(void)
{
    /* a*b+c where fusing changes the answer: (1+2^-12)^2 - 1 - 2^-11. */
    volatile float a = 1.0f + 0x1p-12f, c = -(1.0f + 0x1p-11f);
    float r = a * a + c;          /* unfused: 0, fused: 2^-24 */
    check_u("no FMA contraction", (uint32_t)(r == 0.0f), 1u, 0);
}

int main(void)
{
    const char *path =
#if defined(RECOMP_SIMD_PORTABLE)
        "portable C";
#elif defined(RECOMP_HOST_ARM64)
        "sse2neon (NEON)";
#else
        "SSE";
#endif
    test_packed();
    test_convert();
    test_minmax_golden();
    test_memory_and_atomics();
    for (uint32_t off = 0; off < 8; off++) test_contended(off);
    test_fp_contract();
    printf("arm64_runtime SIMD [%s]: %u checks, %u failures, digest %016llX\n",
           path, s_checks, s_fail, (unsigned long long)s_digest);
    /* Every path must produce the same digest; the CMake test compares the
     * printed value against the expected one passed in as an argument. */
    return s_fail != 0;
}
