/*
 * fake_openxr.h - a minimal in-process OpenXR runtime for tests/vr_glue. It implements the core and extension entry
 * points the Quest shell calls (port/quest/src/xr_session.c, xr_vulkan.c, vr_bridge.c) and hands out real VkImages
 * created on the Vulkan device it makes through the XR_KHR_vulkan_enable2 functions (SwiftShader in the tests).
 * Time is a test-controlled clock (fx_now); head and eye poses come from a test-provided trajectory. xrEndFrame
 * captures the submitted layers.
 */
#ifndef FAKE_OPENXR_H
#define FAKE_OPENXR_H

#include <pthread.h>
#include <stdint.h>

#define XR_USE_GRAPHICS_API_VULKAN 1
#define XR_USE_TIMESPEC 1
#include <time.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define FX_SPACE_STAGE ((XrSpace)(uintptr_t)0x10)
#define FX_SPACE_VIEW  ((XrSpace)(uintptr_t)0x11)
#define FX_SPACE_LOCAL ((XrSpace)(uintptr_t)0x12)

#define FX_MAX_SWAPCHAINS 8
#define FX_IMAGES 3

typedef struct FxSwapchain {
    int used;
    XrSwapchainCreateInfo ci;
    VkFormat view_formats[4];
    uint32_t nview_formats;          /* XrVulkanSwapchainFormatListCreateInfoKHR, when chained */
    uint64_t foveation_flags;        /* XrSwapchainCreateInfoFoveationFB.flags, when chained */
    int foveation_level;             /* the level of the last profile set with xrUpdateSwapchainFB, -1 none */
    VkImage img[FX_IMAGES];
    VkDeviceMemory mem[FX_IMAGES];
    uint32_t next, acquired_index, last_released;
    int acquired, waited, ever_released;
    uint64_t acquires, releases;
} FxSwapchain;

typedef struct FxEndFrame {
    XrTime display_time;
    uint32_t nlayers;
    int has_proj;
    XrCompositionLayerProjection proj;
    XrCompositionLayerProjectionView views[2];
    uint32_t nquads;
    XrCompositionLayerQuad quads[4];   /* flat or HUD, the settings panel and its laser */
    uint32_t quad_image[4];            /* last released image of each quad's swapchain at xrEndFrame */
    int quad_layer_index[4];           /* its position in the frame's layer list */
    uint32_t proj_image[2];          /* last released image of the projection view's swapchain at xrEndFrame */
} FxEndFrame;

/* what the shell asked of Vulkan through XR_KHR_vulkan_enable2 */
typedef struct FxVulkan {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    uint32_t instance_api;
    int dev_multiview, dev_robust, dev_depth_clamp, dev_bc, dev_aniso;
    int dev_fdm;                     /* VK_EXT_fragment_density_map enabled on the device */
    uint32_t dev_api_ok;
} FxVulkan;

extern FxVulkan fx_vk;
extern volatile int64_t fx_now;                 /* XrTime, ns */
extern int64_t fx_period;                       /* display period */
extern void (*fx_trajectory)(int64_t t, XrPosef *head, XrPosef eye[2], XrFovf fov[2]);
extern FxEndFrame fx_last_end;
extern volatile uint64_t fx_end_count;
extern pthread_mutex_t *fx_queue_lock;          /* when set: queue-using calls must hold it */
extern int fx_check_locks;                      /* verify that (single-threaded phases only) */
extern int fx_lock_violations;
extern void (*fx_on_release)(XrSwapchain sc, uint32_t index);   /* called inside xrReleaseSwapchainImage */
extern void (*fx_on_end_frame)(const FxEndFrame *f);           /* called inside xrEndFrame */

/* input (tests/game_render_x86): when set, the state of a boolean / 2D action the shell reads, by the action name it
 * was created with; return 1 for an active action with *state filled in, 0 for inactive. Unset: nothing is active. */
extern int (*fx_input_bool)(const char *action, XrPath subaction, XrBool32 *state);
extern int (*fx_input_vec2)(const char *action, XrPath subaction, XrVector2f *state);
extern int (*fx_input_float)(const char *action, XrPath subaction, float *state);
/* hand tracking (gun latch tests): when set, the stage pose of hand 0 (left) / 1 (right), aim (grip 0) or grip space,
 * at time t; return 1 when tracked. Unset: hands are never tracked (as before). */
extern int (*fx_hand_pose)(int hand, int grip, XrTime t, XrPosef *pose);
/* optional: the hand's linear velocity (XrSpaceVelocity chained to xrLocateSpace); unset or 0: not valid */
extern int (*fx_hand_velocity)(int hand, int grip, XrTime t, XrVector3f *linear);

/* instance extensions: what xrEnumerateInstanceExtensionProperties offers (the shell's optional ones, foveation
 * included unless fx_no_foveation is set before the shell asks) and what the FB foveation calls did */
extern int fx_no_foveation;
extern int fx_foveation_profiles, fx_foveation_profiles_destroyed, fx_foveation_updates;

FxSwapchain *fx_swapchain(XrSwapchain h);
void fx_push_session_state(XrSessionState st);
XrResult fx_convert_time(XrInstance instance, const struct timespec *ts, XrTime *t);

#endif
