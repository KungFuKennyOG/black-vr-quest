/* grx_host.h - tests/game_render_x86, force-included into the runtime's NV2A, video, USB and input TUs: what
 * platform/win32_compat.h provides only under __aarch64__ (the Quest) and MSVC provides on Windows, so these Quest
 * source paths also build on the x86-64 Linux test host. tests/nv2a_arm64's x86_extra.h has the atomics,
 * MemoryBarrier and GetSystemInfo; the bounded-string variants below are win32_compat.h's aarch64 ones. */
#ifndef GRX_HOST_H
#define GRX_HOST_H
#include "x86_extra.h"
#if !defined(__aarch64__) && !defined(_WIN32)
#include <stdarg.h>
#include <stdio.h>
#ifndef _TRUNCATE
#define _TRUNCATE ((SIZE_T)-1)
#endif
#ifndef STRUNCATE
#define STRUNCATE 80
#endif
#ifndef InterlockedDecrement64
#define InterlockedDecrement64(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#endif
static inline int _snprintf_s(char *buf, SIZE_T size, SIZE_T count, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (!buf || !size) return -1;
    if (count != _TRUNCATE && count < size) size = count + 1;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[0] = 0; return -1; }
    return (SIZE_T)n >= size ? -1 : n;
}
static inline int strncpy_s(char *dst, SIZE_T size, const char *src, SIZE_T count)
{
    SIZE_T n = 0;
    if (!dst || !size) return 22;
    if (!src) { dst[0] = 0; return 22; }
    while (n + 1 < size && n < count && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
    if (src[n] && n < count) {
        if (count == _TRUNCATE) return STRUNCATE;
        dst[0] = 0;
        return 34;
    }
    return 0;
}
#endif
#endif
