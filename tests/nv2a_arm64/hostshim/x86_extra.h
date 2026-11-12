/* TEST ONLY, force-included into native x86-64 Linux test builds: the primitives that
 * platform/win32_compat.h provides on aarch64 (and MSVC provides on Windows), with the
 * same SEQ_CST semantics, so the same Kelvin sources build here for the reference run. */
#ifndef NV2A_TEST_X86_EXTRA_H
#define NV2A_TEST_X86_EXTRA_H
#if !defined(__aarch64__) && !defined(_WIN32)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE     /* first: win32_compat.c needs memfd_create */
#endif
#include <immintrin.h>
#include <string.h>
#include <unistd.h>
#include "platform/xbox_winnt.h"
#ifndef __int64
#define __int64 long long
#endif
#define InterlockedIncrement64(p)       __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedExchange64(p, v)     __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64(p, v)  __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedExchange64(p, v)    __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedExchangeAdd64(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define MemoryBarrier()                 __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor()                _mm_pause()
typedef struct _SYSTEM_INFO { DWORD dwPageSize, dwNumberOfProcessors; } SYSTEM_INFO;
static inline void GetSystemInfo(SYSTEM_INFO *si)
{
    si->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
    si->dwNumberOfProcessors = (DWORD)sysconf(_SC_NPROCESSORS_ONLN);
}
#endif
#endif
