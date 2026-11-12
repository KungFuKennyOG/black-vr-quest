/*
 * black_android.h - what BLACK's own sources (main.c, recomp_manual.c and its
 * .inc files) need from <windows.h> on the Quest (aarch64 Android) build.
 *
 * Included only under __ANDROID__, in place of <windows.h>. The Windows build
 * never sees this file. The Win32 calls themselves come from the runtime's
 * POSIX Win32 layer (platform/win32_compat.h) plus port/android/include/
 * win32_android.h (CreateDirectoryA, DeleteFileA, MoveFileExA, ...).
 */
#ifndef BLACK_ANDROID_H
#define BLACK_ANDROID_H

#if !defined(__ANDROID__)
#error "black_android.h is for the Android (Quest) build only"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#include "win32_android.h"

/* MSVC storage-class spellings used in the game files. Only `thread` occurs:
 * __declspec(thread) -> C11 _Thread_local, the same TLS model as the runtime's
 * RECOMP_TLS (__thread off MSVC), so redeclarations agree. This also covers the
 * generated black_xmv_idct.inc's `extern __declspec(thread) uint32_t g_ebp`. */
#ifndef __declspec
#define __declspec(x) BLACK_DECLSPEC_##x
#define BLACK_DECLSPEC_thread _Thread_local
#endif
/* Same spelling recomp_types.h uses; the .inc files included before it need it. */
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

#endif /* BLACK_ANDROID_H */
