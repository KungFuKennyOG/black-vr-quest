/* libkvk_linkcheck.so: links the Kelvin Vulkan backend (libkelvin_vk.a + glslang) into an Android shared object
 * with --no-undefined, so every symbol the backend needs is resolved by the NDK (libvulkan, liblog, libc, libm,
 * libc++) or by what the game's runtime provides (here: the two engine symbols, stubbed). */
#include <stddef.h>
#include <stdint.h>

#include "kelvin_gpu.h"
#include "kelvin_gfx.h"
#include "kvk_device.h"

/* from the engine (kelvin.c) in the real app */
void kelvin_unsupported(int what, const char *detail, uint32_t value) { (void)what; (void)detail; (void)value; }

typedef void (*AnyFn)(void);
__attribute__((visibility("default"))) const AnyFn kvk_linkcheck_table[] = {
    (AnyFn)kvk_set_device, (AnyFn)kvk_set_raster_size, (AnyFn)kvk_set_stereo, (AnyFn)kvk_set_cache_dir,
    (AnyFn)kvk_shutdown, (AnyFn)kvk_read_surface, (AnyFn)kvk_get_stats, (AnyFn)kvk_wait_compiles,
    (AnyFn)kvk_set_pass_merge,
    (AnyFn)kgpu_enabled, (AnyFn)kgpu_active, (AnyFn)kgpu_init, (AnyFn)kgpu_report, (AnyFn)kgpu_note_kick,
    (AnyFn)kgpu_host_size, (AnyFn)kgpu_vs_enabled, (AnyFn)kgpu_vs_ready, (AnyFn)kgpu_draw, (AnyFn)kgpu_clear,
    (AnyFn)kgpu_flip, (AnyFn)kgpu_set_viewmodel, (AnyFn)kgpu_set_view, (AnyFn)kgpu_set_ao_camera,
    (AnyFn)kgpu_request_capture, (AnyFn)kgpu_capture_result, (AnyFn)kgpu_compare_enabled, (AnyFn)kgpu_compare_frame,
    (AnyFn)kgpu_pass_check, (AnyFn)kgpu_async_active, (AnyFn)kgpu_drain, (AnyFn)kgpu_defer_call, (AnyFn)kgpu_get_bound,
};
