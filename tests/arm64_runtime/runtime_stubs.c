/*
 * Link stubs for running runtime pieces (xr_kernel) in a standalone arm64
 * test: the symbols the renderer, video, USB and the recompiled game supply
 * in a real build. None of them is reached by the tests that link this.
 */
#include <stdint.h>
#include <stddef.h>

typedef void (*recomp_func_t)(void);
#ifndef STUBS_NO_LOOKUP   /* tests/memory_layout_posix defines its own */
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
#endif

int  kelvin_active(void) { return 0; }
void kelvin_start(void) {}
int  kelvin_irq_asserted(void) { return 0; }
void *kelvin_irq_event(void) { return NULL; }
void kelvin_vblank(void) {}
int  kelvin_owns_register(unsigned int offset) { (void)offset; return 0; }

void xbox_OhciInit(void) {}
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
int  xbox_VideoPlayFile(const char *host_path) { (void)host_path; return 0; }
int  xbox_VideoIsPlaying(void) { return 0; }

/* liblog / libandroid are shared-only in the NDK; a static test executable
 * gets these instead. Logging goes to stderr; ASharedMemory reports failure so
 * the memfd path (the one under test) is the only one that can succeed. */
#if defined(__ANDROID__)
#include <stdarg.h>
#include <stdio.h>
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%d %s] ", prio, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}
int ASharedMemory_create(const char *name, size_t size) { (void)name; (void)size; return -1; }
#endif
