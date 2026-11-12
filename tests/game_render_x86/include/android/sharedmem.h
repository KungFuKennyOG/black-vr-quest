/* tests/game_render_x86: host stand-in for <android/sharedmem.h>. memfd always works on the host, so the ashmem
 * fallback is never reached; host_stubs.c returns -1. */
#ifndef GRX_STUB_ANDROID_SHAREDMEM_H
#define GRX_STUB_ANDROID_SHAREDMEM_H
#include <stddef.h>
int ASharedMemory_create(const char *name, size_t size);
#endif
