/* arm64 build only (port/android): x86 intrinsic header -> sse2neon.
 * On the include path ahead of the compiler's own headers, which #error on
 * non-x86 targets. MINPS/MAXPS need the precise variant: see recomp_types.h. */
#ifndef XR_X86SHIM_SSE2NEON
#define XR_X86SHIM_SSE2NEON
#if !defined(__aarch64__)
#error "port/android/x86shim is for arm64 builds only"
#endif
#ifndef SSE2NEON_PRECISE_MINMAX
#define SSE2NEON_PRECISE_MINMAX 1
#endif
#include "sse2neon.h"
#endif
