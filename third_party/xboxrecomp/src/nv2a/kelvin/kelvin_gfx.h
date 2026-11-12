/*
 * kelvin_gfx.h - the seam between the Kelvin NV2A engine and a host graphics backend.
 *
 * The engine (kelvin.c, kelvin_render.c, kelvin_pgraph.c, kelvin_workers.c, kelvin_mmio.c) reaches the host GPU only
 * through the kgpu_* functions declared in kelvin_gpu.h. That API is already backend-neutral: it speaks in guest
 * terms (KelvinTarget, KPixelState, NV2A vertex programs and raw vertex arrays, NV2A clear words) and never hands
 * out a host object. A backend is therefore one link-time implementation of kelvin_gpu.h:
 *
 *   kelvin_gpu.c (+ kelvin_gpu_*.inc)   Direct3D 11, Windows. Unchanged; also carries the PC extras below.
 *   vk/kvk_backend.c (+ vk/kvk_*.c)      Vulkan 1.1 + multiview, Android/Quest (and Linux for tests).
 *
 * Exactly one is linked (src/nv2a/CMakeLists.txt: KELVIN_GFX_BACKEND = d3d11 | vulkan). Code that both share and
 * that has no host API in it lives in gfx/ and is #included by each backend:
 *
 *   gfx/kgfx_cfg_types.inc   PSConst / VSG constant layouts (HLSL cbuffer == GLSL std140 == these structs)
 *   gfx/kgfx_cfg_fill.inc    KPixelState -> PSConst
 *   gfx/kgfx_hash.inc        texture content signature (CRC-32C: SSE4.2 / ARMv8 CRC / software, same values)
 *   gfx/kgfx_texdecode.inc   guest texel formats, swizzle, YUV and DXT -> RGBA8
 *
 * Contract every backend implements (see kelvin_gpu.h for the full semantics):
 *   lifecycle     kgpu_enabled kgpu_init kgpu_active kgpu_report kgpu_note_kick kgpu_host_size
 *   drawing       kgpu_draw kgpu_clear kgpu_flip kgpu_set_view kgpu_set_viewmodel
 *   vertex progs  kgpu_vs_enabled kgpu_vs_ready  (false = the engine shades vertices on the CPU for this draw)
 *   ordering      kgpu_async_active kgpu_drain kgpu_defer_call kgpu_get_bound
 *                 (a synchronous backend: false / no-op / call now / get unchanged)
 *   diagnostics   kgpu_compare_enabled kgpu_compare_frame kgpu_pass_check g_kgpu_ticks
 *   title thread  kgpu_set_ao_camera, kgpu_request_capture, kgpu_capture_result (below)
 *
 * PC-only extras that stay in the D3D11 backend and are compiled out of the Vulkan one (their entry points are
 * stubs there): screen-space reflections (kelvin_gpu_ssr.inc), ambient occlusion / depth of field / FXAA / colour
 * passes (kelvin_gpu_post.inc), DLSS/DLAA (kelvin_dlss.c, kelvin_gpu_dlss.inc), D3D12 ray tracing (kelvin_rt.c,
 * kelvin_gpu_rt.inc), replacement texture packs (kelvin_texpack.c), the profilers and slow-frame samplers
 * (kelvin_gpu_prof.inc, kelvin_gpu_time.inc, kelvin_gpu_slow.inc, kelvin_gpu_incl.inc), split-screen views,
 * shared-texture presentation (fb_present) and the asynchronous submission thread (kelvin_gpu_async.inc).
 */
#ifndef XBOX_NV2A_KELVIN_GFX_H
#define XBOX_NV2A_KELVIN_GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defined by every backend, called from the game (src/recomp_manual.c) on the title thread. */
void kgpu_set_ao_camera(float half_x, float half_y);
void kgpu_request_capture(const char *path);       /* photo mode: the next presented frame to a file */
int kgpu_capture_result(void);                     /* 0 pending/none, 1 written, -1 failed */

/* kelvin_mark_view modes beyond kelvin_gpu.h's KG_VIEW_FULL/FRAME/FIT, for VR (game hooks -> Vulkan backend).
 * The D3D11 backend never receives them. WORLD: what follows is drawn through the player's world camera (draws on
 * the main target get the per-eye transform). SCREEN: what follows is HUD/menu/2D and goes to the flat
 * virtual-screen layer. Each flip returns to automatic classification (see vk/kvk_backend.c). */
enum { KG_VIEW_VR_WORLD = 16, KG_VIEW_VR_SCREEN = 17 };

#ifdef __cplusplus
}
#endif

#endif
