/*
 * What the NV2A tests link instead of the rest of the runtime: guest memory, the
 * unimplemented-feature counter, and an inactive GPU backend (the CPU renderer draws).
 * TEST_STUB_GEOMETRY: kelvin_render_geometry_sync, for tests that do not compile
 * kelvin_render.c in.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kelvin_int.h"
#include "kelvin_gpu.h"

static uint8_t s_guest[1 << 16];

uint8_t *kelvin_guest(uint32_t va) { return s_guest + (va & 0xFFF0u); }
uint32_t kelvin_dma_va(uint32_t instance, uint32_t offset, uint32_t bytes)
{
    (void)instance; (void)offset; (void)bytes;
    return 0;
}
void kelvin_unsupported(int what, const char *detail, uint32_t value)
{
    static int said[KU_COUNT];
    if (what >= 0 && what < KU_COUNT && !said[what]++) {
        fprintf(stderr, "  [stub] unsupported %d (%s, 0x%08X)\n", what, detail ? detail : "", value);
    }
}
void xbox_PinThread(const char *role) { (void)role; }

#if defined(TEST_STUB_GEOMETRY)
void kelvin_render_geometry_sync(void) {}
#endif

#if defined(TEST_STUB_GPU)
long long g_kgpu_ticks[KGP_COUNT];
bool kgpu_enabled(void) { return false; }
bool kgpu_active(void) { return false; }
bool kgpu_vs_enabled(void) { return false; }
bool kgpu_vs_ready(const void *p, uint32_t h, uint32_t s, const uint32_t *w, uint32_t n)
{
    (void)p; (void)h; (void)s; (void)w; (void)n;
    return false;
}
void kgpu_draw(const KGpuDraw *d) { (void)d; }
void kgpu_clear(const KelvinTarget *t, bool c, bool d, bool s, uint32_t cv, uint32_t zv, unsigned int m,
                int x0, int y0, int x1, int y1, unsigned int f)
{
    (void)t; (void)c; (void)d; (void)s; (void)cv; (void)zv; (void)m; (void)x0; (void)y0; (void)x1; (void)y1; (void)f;
}
bool kgpu_compare_enabled(void) { return false; }
void kgpu_compare_frame(const KelvinTarget *t, unsigned int f) { (void)t; (void)f; }
void kgpu_pass_check(void) {}
bool kgpu_async_active(void) { return false; }
void kgpu_drain(void) {}
void kgpu_defer_call(void (*fn)(uintptr_t, uintptr_t), uintptr_t a, uintptr_t b) { fn(a, b); }
uint32_t kgpu_get_bound(uint32_t get) { return get; }
void kgpu_flip(const KelvinTarget *t, unsigned int f, uint32_t w, uint32_t h) { (void)t; (void)f; (void)w; (void)h; }
void xbox_FramebufferPublishAspect(const void *s, size_t n, uint32_t va, uint32_t w, uint32_t h, uint32_t p,
                                   uint32_t f, uint32_t a, uint32_t b)
{
    (void)s; (void)n; (void)va; (void)w; (void)h; (void)p; (void)f; (void)a; (void)b;
}
void xbox_FramebufferWindowStart(void) {}
#endif
