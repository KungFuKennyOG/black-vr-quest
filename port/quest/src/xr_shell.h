/*
 * xr_shell.h - internal state of the Quest XR shell (not part of the VR
 * contract; other streams use port/quest/vr_bridge.h only).
 */
#ifndef XR_SHELL_H
#define XR_SHELL_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID 1
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN 1
#endif
#define XR_USE_TIMESPEC 1
#include <jni.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../vr_bridge.h"
#include "vr_config.h"
#include "vr_input_map.h"

#define XLOG_TAG "BlackVR"
#define XLOGI(...) __android_log_print(ANDROID_LOG_INFO, XLOG_TAG, __VA_ARGS__)
#define XLOGW(...) __android_log_print(ANDROID_LOG_WARN, XLOG_TAG, __VA_ARGS__)
#define XLOGE(...) __android_log_print(ANDROID_LOG_ERROR, XLOG_TAG, __VA_ARGS__)

#define XR_MAX_SWAPCHAIN_IMAGES 8

typedef struct {
    XrSwapchain handle;
    uint32_t width, height, layers;
    uint32_t image_count;
    VkImage images[XR_MAX_SWAPCHAIN_IMAGES];
    /* per image, per layer: views, framebuffers (test pattern) */
    VkImageView layer_views[XR_MAX_SWAPCHAIN_IMAGES][2];
    VkImageView array_views[XR_MAX_SWAPCHAIN_IMAGES];      /* 2D_ARRAY view of all layers (renderer) */
    VkFramebuffer framebuffers[XR_MAX_SWAPCHAIN_IMAGES][2];
    uint32_t acquired;                                      /* index, valid while held */
    int held;
} XrsSwapchain;

/* Views as the most recently released eye images were rendered, and what the
 * renderer released last (the XR thread presents exactly that). */
typedef struct {
    XrPosef pose[VR_EYE_COUNT];
    XrFovf  fov[VR_EYE_COUNT];
    uint64_t serial;
    int valid;
    /* vr-glue */
    int mode;                      /* VR_PRESENT_* of the last released content (eyes or flat image) */
    int hud;                       /* the HUD swapchain holds the HUD of the released eye images */
    uint64_t latch_serial;         /* VrRenderPose.serial of the released eye images */
    uint32_t game_frame;
} XrsSubmitViews;

typedef struct {
    struct android_app *app;
    VrShellConfig cfg;
    char files_dir[512];
    char game_root[512];
    int  game_files_present;

    /* OpenXR */
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace world_space;           /* STAGE, or LOCAL as a fallback */
    XrSpace view_space;
    int world_is_stage;
    float floor_y;                 /* test pattern floor in world space */
    XrSessionState state;
    int session_running, focused, exit_requested, resumed;
    XrViewConfigurationView config_views[VR_EYE_COUNT];
    XrTime last_predicted_time;
    XrDuration last_predicted_period;
    int has_refresh_rate, has_perf_settings, has_timespec, has_thread_settings;
    int has_format_list;           /* XR_KHR_vulkan_swapchain_format_list */
    PFN_xrConvertTimespecTimeToTimeKHR xrConvertTimespecTimeToTimeKHR;
    PFN_xrSetAndroidApplicationThreadKHR xrSetAndroidApplicationThreadKHR;
    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB;
    PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB;
    PFN_xrPerfSettingsSetPerformanceLevelEXT xrPerfSettingsSetPerformanceLevelEXT;
    float refresh_hz;
    /* XR_FB_foveation (+ _configuration, XR_FB_swapchain_update_state, XR_FB_foveation_vulkan): fixed foveated
     * rendering of the eye swapchain at blackvr.ini's level (xrs_foveation_apply) */
    int has_foveation;
    int foveation_applied;          /* the level the eye swapchain(s) got, 0 none */
    int foveation_swapchain;        /* the eye swapchain(s) were created foveated: a new level applies live */
    PFN_xrCreateFoveationProfileFB xrCreateFoveationProfileFB;
    PFN_xrDestroyFoveationProfileFB xrDestroyFoveationProfileFB;
    PFN_xrUpdateSwapchainFB xrUpdateSwapchainFB;

    /* actions */
    XrActionSet action_set;
    XrAction act_stick, act_trigger, act_squeeze, act_click, act_a, act_b, act_x, act_y,
             act_menu, act_aim_pose, act_grip_pose, act_haptic;
    XrPath hand_path[2];
    XrSpace aim_space[2], grip_space[2];

    /* Vulkan (created through XR_KHR_vulkan_enable2) */
    VkInstance vk_instance;
    VkPhysicalDevice vk_phys;
    VkDevice vk_device;
    VkQueue vk_queue;
    uint32_t vk_queue_family;
    uint32_t vk_api;
    VkFormat color_format;
    int multiview_supported;
    VrVulkanFeatures vk_features;  /* what the device was created with (vr_vulkan_features) */
    /* fragment density map support as the device reports it (xrp_vulkan_device logs it): extensions present, features
     * offered, properties, and whether the shell enabled them */
    struct {
        int ext_fdm, ext_fdm2, ext_qcom_offset, ext_fsr;   /* VK_EXT_fragment_density_map, _2, VK_QCOM_fragment_density_map_offset, VK_KHR_fragment_shading_rate */
        int have_fdm, have_dynamic, have_nonsubsampled, have_deferred;   /* features offered */
        uint32_t min_w, min_h, max_w, max_h, invocations;    /* VkPhysicalDeviceFragmentDensityMapPropertiesEXT */
        int env_off;                                          /* RECOMP_KVK_FDM=0 */
        uint32_t n_ext;                                       /* device extensions offered */
    } fdm_info;
    char cache_dir[512];           /* app-private (internalDataPath)/kgpu-cache */

    /* swapchains: eye[0] is the 2-layer array swapchain in multiview mode;
     * eye[0], eye[1] are single-layer swapchains otherwise. */
    int multiview;
    XrsSwapchain eye[VR_EYE_COUNT];
    XrsSwapchain flat;
    XrsSwapchain hud;              /* vr-glue: head-locked HUD quad layer (premultiplied alpha) */

    /* test pattern */
    VkRenderPass tp_pass;
    VkPipelineLayout tp_layout;
    VkPipeline tp_pipeline;
    VkCommandPool tp_pool;
    VkCommandBuffer tp_cmd[VR_EYE_COUNT];
    VkFence tp_fence[VR_EYE_COUNT];
    int tp_ready;

    /* renderer handoff */
    int want_renderer;              /* the game will start: offer the device to vr_renderer_init() */
    int external_renderer;          /* vr_renderer_init() succeeded */
    pthread_mutex_t queue_lock;     /* recursive: the VkQueue and swapchain release/submit pairing */
    XrsSubmitViews submit;          /* guarded by queue_lock */
    int flat_released;              /* the flat swapchain has a released image */
    float flat_aspect;
    int hud_release_pending;        /* HUD image drawn, published with the next eye release */
    /* The test pattern runs until the external renderer first acquires a target
     * (game loading, or no game frame ever): tp_lock hands the swapchains over. */
    pthread_mutex_t tp_lock;
    volatile int renderer_started;

    /* game thread */
    pthread_t game_thread;
    int game_started;

    /* device preflight (xr_preflight.c, bvr_diag.c; port/notes/device-preflight.md) */
    int preflight_on;               /* BLACKVR_PREFLIGHT=0 (a blackvr.ini env line) turns the checks off */
    volatile int pf_panel;          /* XRS_PANEL_*: a solid quad over the test pattern, until the renderer runs */

    /* the in-headset settings panel (xr_panel.c; port/notes/settings-panel.md); NULL: none in this build */
    const struct XrsPanelOps *panel;
} XrShell;

/* The settings panel's hooks into the XR thread's frame (xr_session.c calls them; all on the XR thread). */
typedef struct XrsPanelOps {
    /* after the controllers are read, before vrb_set_input: *touch becomes what the game may see (neutral while the
     * panel is open). Returns 1 while open. */
    int  (*input)(XrShell *s, VrTouchState *touch, const VrPose aim[2], const int valid[2], double dt);
    void (*draw)(XrShell *s);                                  /* after xrBeginFrame: one UI frame, when open */
    /* under the queue lock, before xrEndFrame: the panel's layers (drawn over all others), returns the count */
    uint32_t (*layers)(XrShell *s, const XrCompositionLayerBaseHeader **out, uint32_t cap);
    void (*destroy)(XrShell *s);                               /* before the device goes */
} XrsPanelOps;
/* vr_settings_menu.c: the settings state of both front ends (the native page, the floating panel) */
void vrsm_attach(XrShell *s);          /* after blackvr.ini was read: "as started" and "saved" */
void vrsm_caps(XrShell *s);
void vrsm_begin_edit(XrShell *s);      /* edit = the running config, restart settings as saved */
unsigned vrsm_apply_live(XrShell *s);  /* edit -> the running config; OpenXR side effects queued for the XR thread */
void vrsm_xr_frame(XrShell *s);        /* XR thread, every frame: runs what was queued */
int  vrsm_save(XrShell *s);
void vrsm_ini_path(XrShell *s, char *p, size_t cap);
void vrsm_sampler(int want);           /* the per-thread CPU sampler, while a settings page is shown */
void vrsm_info_update(XrShell *s);
/* xr_panel.c: the panel for this shell; call after blackvr.ini was read (it keeps that config as "as started") */
const XrsPanelOps *xrs_panel_attach(XrShell *s);
typedef struct {
    int open, inited, released;
    XrSwapchain panel, laser;
    VrPose quad_pose;
    float width_m, height_m;
    int mouse_valid;
    float mouse_x, mouse_y, laser_len;
    char status[240];
} XrsPanelTestState;
int xrs_panel_test_state(XrsPanelTestState *st);   /* tests/vr_glue */

enum { XRS_PANEL_NONE = 0, XRS_PANEL_FATAL = 1, XRS_PANEL_GAME_EXIT = 2 };

extern XrShell g_xr;

/* xr_vulkan.c */
int  xrs_vulkan_create(XrShell *s);
int  xrs_swapchain_views(XrShell *s, XrsSwapchain *sc);
int  xrs_testpattern_init(XrShell *s);
void xrs_testpattern_draw(XrShell *s, int slot, XrsSwapchain *sc, uint32_t image, uint32_t layer,
                          const XrPosef *eye, const XrFovf *fov, double time_s);
int  xrs_swapchain_create(XrShell *s, XrsSwapchain *sc, uint32_t w, uint32_t h, uint32_t layers);
/* as xrs_swapchain_create, with an extra structure chained to XrSwapchainCreateInfo (NULL: none) */
int  xrs_swapchain_create_ex(XrShell *s, XrsSwapchain *sc, uint32_t w, uint32_t h, uint32_t layers, const void *next);
void xrs_vulkan_destroy(XrShell *s);
void xrs_swapchain_destroy(XrShell *s, XrsSwapchain *sc);

/* xr_session.c */
int  xrs_instance_create(XrShell *s);
/* The optional instance extensions the shell uses: which of them the runtime offers (has_* flags), appended to want[]
 * (xrs_instance_create; the headless harness calls it on the fake runtime), and their functions once the instance
 * exists. */
void xrs_instance_extensions(XrShell *s, const XrExtensionProperties *ext, uint32_t n, const char **want, uint32_t *en);
void xrs_instance_procs(XrShell *s);
int  xrs_session_create(XrShell *s);
void xrs_poll_events(XrShell *s);
void xrs_frame(XrShell *s);
void xrs_session_destroy(XrShell *s);
int  xrs_locate_views(XrShell *s, XrTime t, XrPosef pose[VR_EYE_COUNT], XrFovf fov[VR_EYE_COUNT]);
int  xrs_locate_head(XrShell *s, XrTime t, XrPosef *head);
int  xrs_swapchain_acquire(XrShell *s, XrsSwapchain *sc, uint32_t *index);
void xrs_swapchain_release(XrShell *s, XrsSwapchain *sc);
/* live settings (the settings panel): the display rate for cfg.refresh_hz (preflight_line: also the PREFLIGHT display
 * line, at session start), cfg.foveation on the eye swapchain(s), the CPU/GPU performance levels for a BLACKVR_PERF
 * value (NULL: sustained high) */
void xrs_refresh_request(XrShell *s, int preflight_line);
int  xrs_foveation_update(XrShell *s);
void xrs_perf_apply(XrShell *s, const char *mode);

/* xr_session.c: the last failed OpenXR/Vulkan call of the startup (XRCHECK / VKCHECK), for the preflight lines */
void xrs_note_failure(const char *call, int code, int is_vk, const char *file, int line);
const char *xrs_failure_text(void);   /* "xrCreateInstance(...) -> -51 XR_ERROR_RUNTIME_UNAVAILABLE (xr_session.c:85)" */
int  xrs_failure_code(void);
void xrs_failure_clear(void);

/* xr_preflight.c: the shell's preflight lines (bvr_diag.h has the process-level checks) */
void xrp_process(XrShell *s);         /* memory (+ ActivityManager), guest window, SIGSEGV trap, JIT, syscalls, altstack */
void xrp_audio(void);                 /* AAudio as the runtime opens it */
void xrp_vulkan_probe(void);          /* a standalone VkInstance: GPU, driver, required/optional features */
void xrp_openxr(XrShell *s, int ok);  /* after xrs_instance_create */
void xrp_vulkan_device(XrShell *s, int ok);
void xrp_session(XrShell *s, int ok);

/* vr_bridge.c (shell side) */
void vrb_init(XrShell *s);
void vrb_on_xr_frame(XrShell *s, XrTime predicted, XrDuration period);
void vrb_set_session(XrShell *s, int running, int focused);
void vrb_set_input(XrShell *s, const VrTouchState *touch, const VrPose aim[2], const VrPose grip[2],
                   const int valid[2], double dt);
void vrb_recentre_from(const VrPose *head);
int  vrb_take_recentre_request(void);
void vrb_get_rumble(float *left, float *right);
int  vrb_present_mode_now(void);
void vrb_renderer_claim(XrShell *s);     /* the renderer takes the swapchains from the test pattern */
void vrb_touch_debug(VrPadState *pad);   /* latest pad for the test pattern */
/* the hands' linear velocities (stage, m/s) from XrSpaceVelocity, before vrb_set_input; valid 0: from pose deltas */
void vrb_set_hand_velocity(const VrVec3 lin[2], const int valid[2]);
int  xrs_now(XrShell *s, XrTime *t);
void vrb_head_for_testpattern(VrPose hand_aim[2], int valid[2], float trigger[2]);
/* the settings panel: the newest head pose (stage), and whether the controls are in their menu mapping (paused, the
 * front end, no game; not gameplay, not a cutscene) */
void vrb_head_now(VrPose *head);
int  vrb_menu_context(void);

#endif /* XR_SHELL_H */
