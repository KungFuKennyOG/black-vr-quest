/*
 * Shared helpers for the NV2A arm64-vs-x86 parity tests.
 *
 * Each test computes its outputs (raw float/byte arrays, in sections), then either
 *   --dump FILE    writes them (the x86-64 AVX2 build: the Windows build's code paths), or
 *   --check FILE   compares them, section by section, with a dump made elsewhere
 *                  (the x86-64 SSE-only build, and the arm64 build under QEMU).
 * Comparison is bit for bit. A section may allow two named exceptions, counted separately:
 *   NaN bits   both values are NaN but the payload/sign differs (x86's default NaN is
 *              0xFFC00000, AArch64's 0x7FC00000; no stage here looks at NaN bits);
 *   ulp(k)     libm results (powf, log2f, exp2f) may differ by up to k ulp between the
 *              C libraries (MSVC UCRT, glibc, bionic). SIMD code never gets this allowance.
 * The process exits non-zero on any other difference.
 */
#ifndef NV2A_PARITY_COMMON_H
#define NV2A_PARITY_COMMON_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#define PARITY_HOST "arm64"
#elif defined(__AVX2__)
#define PARITY_HOST "x86-64-avx2"
#else
#define PARITY_HOST "x86-64-sse"
#endif

/* xorshift64*: identical sequences on every host. */
static uint64_t pr_state = 0x9E3779B97F4A7C15ull;
static inline void pr_seed(uint64_t s) { pr_state = s ? s : 1; }
static inline uint32_t pr_u32(void)
{
    pr_state ^= pr_state >> 12;
    pr_state ^= pr_state << 25;
    pr_state ^= pr_state >> 27;
    return (uint32_t)((pr_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static inline uint32_t pr_n(uint32_t n) { return n ? pr_u32() % n : 0; }
static inline float pr_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t pr_ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
/* A float in [lo, hi). */
static inline float pr_range(float lo, float hi)
{
    return lo + (hi - lo) * (float)(pr_u32() >> 8) * (1.0f / 16777216.0f);
}

/* Edge values: signed zeros, infinities, quiet and signalling NaNs with payloads,
 * denormals, the largest/smallest normals, ties, integers around 2^24 and 2^31. */
static const uint32_t pr_edges[] = {
    0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
    0x7FC12345u, 0x7FA00001u, 0xFF812345u, 0x00000001u, 0x80000001u, 0x007FFFFFu,
    0x00400000u, 0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F800000u,
    0xBF800000u, 0x3F000000u, 0x3EFFFFFFu, 0x3F7FFFFFu, 0x4B800000u, 0x4B800001u,
    0x4F000000u, 0xCF000000u, 0x4F000001u, 0x3A83126Fu, 0x1F800000u, 0x5F800000u,
    0x3DCCCCCDu, 0x447A0000u,
};
#define PR_NEDGES ((int)(sizeof(pr_edges) / sizeof(pr_edges[0])))

/* Mostly ordinary values, sometimes an edge value. */
static inline float pr_value(int edge_per_256)
{
    if ((int)pr_n(256) < edge_per_256) return pr_bits(pr_edges[pr_n(PR_NEDGES)]);
    switch (pr_n(4)) {
    case 0: return pr_range(-1.0f, 1.0f);
    case 1: return pr_range(-1000.0f, 1000.0f);
    case 2: return pr_range(0.0f, 1.0f);
    default: return (float)((int)pr_n(17) - 8) * 0.25f;
    }
}

/* ---- dump / check ------------------------------------------------------------------ */

typedef struct ParitySection {
    const char *name;
    const void *data;
    size_t bytes;
    int is_float;      /* compare as float32 words (NaN rule applies) */
    int ulp_allowed;   /* libm-only sections: up to this many ulp */
} ParitySection;

static inline uint64_t pr_fnv(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static inline int64_t pr_ulp_dist(uint32_t a, uint32_t b)
{
    int64_t ia = (a & 0x80000000u) ? -(int64_t)(a & 0x7FFFFFFFu) : (int64_t)a;
    int64_t ib = (b & 0x80000000u) ? -(int64_t)(b & 0x7FFFFFFFu) : (int64_t)b;
    return ia > ib ? ia - ib : ib - ia;
}

/* Returns the process exit code. */
static inline int parity_finish(int argc, char **argv, const ParitySection *s, int n)
{
    const char *mode = argc > 2 ? argv[1] : "";
    const char *path = argc > 2 ? argv[2] : NULL;
    int i, failed = 0;
    uint64_t h = 0xCBF29CE484222325ull;

    for (i = 0; i < n; i++) h = pr_fnv(s[i].data, s[i].bytes, h);
    printf("[%s] %d sections, digest %016llX\n", PARITY_HOST, n, (unsigned long long)h);

    if (!strcmp(mode, "--dump")) {
        FILE *f = fopen(path, "wb");
        if (!f) { perror(path); return 2; }
        for (i = 0; i < n; i++) {
            uint64_t b = s[i].bytes;
            fwrite(&b, 8, 1, f);
            fwrite(s[i].data, 1, s[i].bytes, f);
        }
        fclose(f);
        printf("[%s] wrote %s\n", PARITY_HOST, path);
        return 0;
    }
    if (strcmp(mode, "--check")) return 0;      /* digest only */

    {
        FILE *f = fopen(path, "rb");
        if (!f) { perror(path); return 2; }
        for (i = 0; i < n; i++) {
            uint64_t b = 0;
            uint8_t *ref;
            size_t k, words, exact = 0, nanbits = 0, ulp = 0, bad = 0;
            int64_t worst = 0;
            if (fread(&b, 8, 1, f) != 1 || b != s[i].bytes) {
                printf("  %-28s size mismatch\n", s[i].name);
                fclose(f);
                return 1;
            }
            ref = (uint8_t *)malloc(b ? b : 1);
            if (fread(ref, 1, b, f) != b) { free(ref); fclose(f); return 2; }
            if (s[i].is_float) {
                const uint8_t *me = (const uint8_t *)s[i].data;
                words = b / 4;
                for (k = 0; k < words; k++) {
                    uint32_t x, y;
                    memcpy(&x, me + 4 * k, 4);
                    memcpy(&y, ref + 4 * k, 4);
                    if (x == y) { exact++; continue; }
                    if (isnan(pr_bits(x)) && isnan(pr_bits(y))) { nanbits++; continue; }
                    if (s[i].ulp_allowed && !isnan(pr_bits(x)) && !isnan(pr_bits(y)) &&
                        pr_ulp_dist(x, y) <= s[i].ulp_allowed) {
                        ulp++;
                        if (pr_ulp_dist(x, y) > worst) worst = pr_ulp_dist(x, y);
                        continue;
                    }
                    if (bad++ < 5) {
                        printf("    %s[%zu]: mine %08X (%g) reference %08X (%g)\n", s[i].name, k,
                               x, pr_bits(x), y, pr_bits(y));
                    }
                }
            } else {
                words = b;
                for (k = 0; k < b; k++) {
                    if (((const uint8_t *)s[i].data)[k] == ref[k]) exact++;
                    else if (bad++ < 5) printf("    %s[%zu]: mine %02X reference %02X\n", s[i].name, k,
                                               ((const uint8_t *)s[i].data)[k], ref[k]);
                }
            }
            printf("  %-28s %8zu %s: %zu identical, %zu NaN-bits only, %zu within %d ulp (max %lld), %zu DIFFER\n",
                   s[i].name, words, s[i].is_float ? "floats" : "bytes", exact, nanbits, ulp,
                   s[i].ulp_allowed, (long long)worst, bad);
            failed |= bad != 0;
            free(ref);
        }
        fclose(f);
    }
    printf("[%s] %s\n", PARITY_HOST, failed ? "FAIL" : "PASS: matches the reference");
    return failed;
}

#endif
