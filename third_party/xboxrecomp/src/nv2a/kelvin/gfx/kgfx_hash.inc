/* Kelvin GPU backends: the 128-bit content signature of guest texture memory. Moved unchanged from kelvin_gpu.c
 * (kelvin_gfx.h). The x86 build uses SSE4.2's CRC32 (CRC-32C) as before; arm64 has the same CRC-32C in its CRC
 * extension (__crc32cd/__crc32cb, mandatory from ARMv8.1, present on every Quest), and anything else computes it in
 * software, so a signature is the same value on every host. */
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_acle.h>
#if defined(__clang__) && !defined(__ARM_FEATURE_CRC32)
/* Android's arm64 baseline is ARMv8.0, where CRC is optional: enable it for sig_bytes alone (pop at the end) */
#pragma clang attribute push (__attribute__((target("crc"))), apply_to = function)
#define KGFX_CRC_PUSHED 1
#endif
#define _mm_crc32_u64(c, v) ((uint64_t)__crc32cd((uint32_t)(c), (uint64_t)(v)))
#define _mm_crc32_u8(c, v) __crc32cb((uint32_t)(c), (uint8_t)(v))
#elif !defined(__x86_64__) && !defined(_M_X64) && !defined(__i386__) && !defined(_M_IX86)
static uint32_t kgfx_crc32c_byte(uint32_t c, uint8_t v)
{
    int k;
    c ^= v;
    for (k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
    return c;
}
static uint64_t kgfx_crc32c_u64(uint64_t c, uint64_t v)
{
    uint32_t r = (uint32_t)c;
    int k;
    for (k = 0; k < 8; k++) r = kgfx_crc32c_byte(r, (uint8_t)(v >> (8 * k)));
    return r;
}
#define _mm_crc32_u64(c, v) kgfx_crc32c_u64((c), (v))
#define _mm_crc32_u8(c, v) kgfx_crc32c_byte((uint32_t)(c), (uint8_t)(v))
#endif
static void sig_bytes(const uint8_t *p, size_t n, uint64_t out[2])
{
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 24 <= n; i += 24) {
        uint64_t x, y, z;
        memcpy(&x, p + i, 8); memcpy(&y, p + i + 8, 8); memcpy(&z, p + i + 16, 8);
        a = _mm_crc32_u64(a, x);
        b = _mm_crc32_u64(b, y);
        c = _mm_crc32_u64(c, z);
    }
    for (; i < n; i++) {
        a = _mm_crc32_u8((unsigned int)a, p[i]);
    }
    out[0] = (a & 0xFFFFFFFFull) | (b << 32);
    out[1] = (c & 0xFFFFFFFFull) | ((uint64_t)n << 32);
}
#ifdef KGFX_CRC_PUSHED
#pragma clang attribute pop
#undef KGFX_CRC_PUSHED
#endif
