/*
 * kvk_device.h - what the Kelvin Vulkan backend needs from its host (the Quest XR shell, or a test).
 *
 * The backend never creates a VkInstance or VkDevice: OpenXR (XR_KHR_vulkan_enable2) decides them, so the shell
 * creates them and hands them over with kvk_set_device() before the Kelvin GPU thread starts (kelvin_start). Without
 * a device kgpu_init() fails and the engine keeps its CPU renderer.
 *
 * Device requirements (the shell enables these when it creates the device):
 *   Vulkan 1.1; features: multiview (required), robustBufferAccess (strongly recommended: out-of-range vertex
 *   indices then read zero as on D3D11), depthClamp, samplerAnisotropy, textureCompressionBC (each optional:
 *   say which are on in KvkDeviceInfo). No device extension is required.
 * Queue: one graphics queue. OpenXR's runtime uses the same queue inside xrEndFrame/xrReleaseSwapchainImage; when the
 * shell may call those on another thread it passes queue_lock/queue_unlock and the backend takes the lock around
 * each vkQueueSubmit/vkQueueWaitIdle.
 * Swapchain images handed out by vr_acquire_eye_targets() (vr_bridge.h) need COLOR_ATTACHMENT and TRANSFER_DST
 * usage (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT); they are expected in
 * COLOR_ATTACHMENT_OPTIMAL layout when acquired and are returned in it.
 */
#ifndef KVK_DEVICE_H
#define KVK_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KvkDeviceInfo {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    uint32_t api_version;                   /* as created (VK_API_VERSION_1_1 or later) */
    /* features enabled on `device` */
    bool multiview, robust_buffer_access, depth_clamp, sampler_anisotropy, texture_compression_bc;
    /* the eye swapchain images were created with MUTABLE_FORMAT (and a format list with the UNORM twin): the
     * backend then writes an _SRGB swapchain through a _UNORM view, so the title's gamma-space colours reach the
     * compositor unchanged. Without it they are written through the image's own format. */
    bool swapchain_mutable_format;
    /* optional external synchronisation of `queue` with the OpenXR runtime */
    void (*queue_lock)(void *ctx);
    void (*queue_unlock)(void *ctx);
    void *queue_ctx;
} KvkDeviceInfo;

/* Call before kelvin_start(); the info is copied. */
void kvk_set_device(const KvkDeviceInfo *info);

/* The host raster size of the title's full-screen (640x480) targets: per eye under VR (the recommended swapchain
 * image size, or less), any size for flat use. Default 1280x960. Takes effect at kgpu_init. */
void kvk_set_raster_size(unsigned int width, unsigned int height);

/* Stereo rendering: every Kelvin render target becomes a 2-layer array drawn with multiview (one layer per eye).
 * Default: vr_active() at kgpu_init. Takes effect at kgpu_init. */
void kvk_set_stereo(bool on);

/* Where compiled shaders (SPIR-V), the pipeline cache and the vertex-program manifest live, e.g. the app's
 * internal files directory on Android. Default: $RECOMP_KGPU_CACHE, else "kgpu-cache". */
void kvk_set_cache_dir(const char *dir);

/* Wait for the GPU, write the pipeline cache, free everything. The device itself is the shell's. */
void kvk_shutdown(void);

/* ---- diagnostics and tests ---- */

/* Copy layer `layer` of the colour surface at guest address `va` (as it is after everything submitted so far) to
 * `out` as BGRA8 rows of `pitch` bytes; *w, *h get its size. Waits for the GPU. False if there is no such
 * surface or it is not 32-bit colour. */
bool kvk_read_surface(uint32_t va, unsigned int layer, void *out, size_t pitch, unsigned int *w, unsigned int *h,
                      size_t cap);

/* Counters since start. */
typedef struct KvkStats {
    uint64_t frames, draws, gvs_draws, tris, clears, passes, pipelines, vs_compiled, ps_compiled, tex_uploads;
    uint64_t world_draws, stereo_draws, hud_draws, uber_draws, skipped;
    /* cost of first use: time in vkCreateGraphicsPipelines (any thread) and its worst single call, time in glslang
     * (compile workers), SPIR-V modules taken from the disk cache vs compiled by glslang */
    uint64_t pipe_ns, pipe_max_ns, glslang_ns, spv_cache_hits, glslang_compiles;
    uint64_t viewmodel_draws;   /* VR gun latch: draws classified as the first-person weapon (port/notes/gun-latch.md) */
    uint64_t sprite_draws;      /* stereo: screen-space effect draws corrected per eye (kvk_backend.c screen_effect) */
    uint64_t fullscreen_draws;  /* stereo: non-perspective world-pass draws left pixel-aligned as full-screen passes */
    uint64_t offscreen_stereo_draws;  /* stereo: draws into camera-aligned offscreen targets (the glare chain) corrected */
} KvkStats;
void kvk_get_stats(KvkStats *out);

/* Wait until every queued vertex/pixel program compile has finished (tests; a pre-start precompile). */
void kvk_wait_compiles(void);

#ifdef __cplusplus
}
#endif

#endif
