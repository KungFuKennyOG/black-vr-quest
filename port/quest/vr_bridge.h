/*
 * vr_bridge.h - contract between the Quest XR shell, the renderer and the
 * game-side camera/input hooks. Plain C, no platform headers.
 *
 * Owners:
 *   XR shell (port/quest)      writes: frame poses, eye matrices, pad state
 *   Renderer (kelvin/vk)       reads : eye matrices, swapchain targets
 *   Game hooks (src/black_vr)  reads : head/hand poses, writes game camera info
 *
 * Conventions: right-handed, metres, OpenXR stage space, column-major 4x4
 * float matrices (m[col*4 + row]), quaternions as x,y,z,w.
 */
#ifndef VR_BRIDGE_H
#define VR_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VR_EYE_COUNT 2

typedef struct { float x, y, z; } VrVec3;
typedef struct { float x, y, z, w; } VrQuat;
typedef struct { VrQuat orientation; VrVec3 position; } VrPose;
typedef struct { float m[16]; } VrMat4;

/* Asymmetric field of view in radians, as OpenXR reports it. */
typedef struct { float left, right, up, down; } VrFov;

typedef struct {
    VrPose pose;        /* eye pose in stage space */
    VrFov  fov;
    VrMat4 view;        /* inverse of pose */
    VrMat4 proj;        /* built for the renderer's clip conventions */
} VrEye;

/* Hand input, mapped by the shell from OpenXR actions. */
typedef struct {
    VrPose aim;         /* aim pose (pointing ray) */
    VrPose grip;
    int    valid;
} VrHand;

/* Xbox gamepad state, the shape the runtime's XInput path consumes.
 * Buttons use XINPUT_GAMEPAD_* bit values; analog in Xbox ranges. */
typedef struct {
    uint16_t buttons;
    uint8_t  left_trigger, right_trigger;   /* 0..255 */
    int16_t  thumb_lx, thumb_ly, thumb_rx, thumb_ry;
} VrPadState;

typedef struct {
    int64_t    predicted_display_time;  /* XrTime, ns */
    VrPose     head;
    VrEye      eye[VR_EYE_COUNT];
    VrHand     hand[2];                 /* 0 = left, 1 = right */
    VrPadState pad;
    float      ipd_m;
    float      world_scale;             /* game units per metre */
    uint32_t   frame_index;
    int        session_running;         /* 0 while paused / not focused */
} VrFrameState;

/* Game camera as captured at the world-camera hook (game units/space). */
typedef struct {
    float view[16];       /* game view matrix (world -> camera), column-major */
    float proj[16];       /* game projection used for this frame */
    float half_fov_x, half_fov_y;
    int   valid;          /* 1 when the player world camera produced this frame */
} VrGameCamera;

/* Shell: latest frame state, stable for the duration of a game frame. */
const VrFrameState *vr_frame_state(void);

/* Game hooks -> renderer: the camera the frame was rendered from. */
void vr_publish_game_camera(const VrGameCamera *cam);
const VrGameCamera *vr_game_camera(void);

/* Renderer: eye render targets for the current XR frame. Handles are
 * backend-specific (VkImage / VkImageView as uint64_t). */
typedef struct {
    uint64_t image, view;
    uint32_t width, height;
    uint32_t format;        /* VkFormat */
    uint32_t array_layer;   /* layer when using a multiview array swapchain */
} VrEyeTarget;

int  vr_acquire_eye_targets(VrEyeTarget out[VR_EYE_COUNT]);
void vr_release_eye_targets(void);

/* Shell: true when VR presentation is active (false = flat fallback). */
int vr_active(void);

/* ======================================================================
 * Additions (xr-shell stream). Everything above is unchanged; everything
 * below is new. See port/notes/xr-shell.md for the rationale.
 * ====================================================================== */

/* --- VrGameCamera companion: what the game hook used for this frame ------
 * Kept as a separate struct (VrGameCamera above is frozen) and published
 * together with it by vr_publish_game_camera_ex(). */
typedef struct {
    VrPose   head_pose;      /* stage-space head pose the game camera was built from */
    int64_t  pose_time;      /* XrTime the pose was predicted for */
    float    near_z, far_z;  /* game units; 0 when unknown */
    float    depth_a, depth_b; /* game clip z = a*z_view + b (D3D LH), 0 when unknown */
    uint32_t game_frame;     /* increments once per published world camera */
    int      present_mode;   /* VR_PRESENT_* chosen by the game hook */
} VrGameCameraExt;

void vr_publish_game_camera_ex(const VrGameCamera *cam, const VrGameCameraExt *ext);
const VrGameCameraExt *vr_game_camera_ext(void);

/* --- Presentation mode ---------------------------------------------------
 * STEREO : world drawn per eye into the projection layer (vr_acquire_eye_targets).
 * FLAT_* : the whole frame drawn once, flat, into the quad layer
 *          (vr_acquire_flat_target): menus/pause/FMV (MENU), in-engine
 *          scripted cameras (CINEMA, world-locked screen), sniper scope (SCOPE,
 *          head-locked window). */
enum {
    VR_PRESENT_STEREO      = 0,
    VR_PRESENT_FLAT_MENU   = 1,
    VR_PRESENT_FLAT_CINEMA = 2,
    VR_PRESENT_FLAT_SCOPE  = 3
};
void vr_set_present_mode(int mode);
int  vr_present_mode(void);

/* --- Per-eye transforms for the renderer ---------------------------------
 * Valid between vr_acquire_eye_targets() and vr_release_eye_targets(); the
 * shell late-latches the head pose inside vr_acquire_eye_targets().
 *
 * "Game view space" is the game camera's space as published in
 * VrGameCamera: left-handed, x = screen right, y = up, z = forward, game
 * units (D3D convention). "Game clip space" is what the game's own
 * MVP produces (D3D: x,y in [-w,w], z in [0,w], w = z_view).
 *
 *   eye_from_game_view[i]  rigid transform game view -> eye i view (same
 *                          LH convention), includes IPD and late-latch delta.
 *   proj[i]                eye i projection, LH D3D convention, z in [0,1],
 *                          built with the game's near/far.
 *   clip_from_game_clip[i] proj[i] * eye_from_game_view[i] * inverse(game proj)
 *                          -> multiply any world-camera clip position by this
 *                          (e.g. in the vertex shader epilogue, indexed by
 *                          gl_ViewIndex) to get eye i's clip position. Valid
 *                          only when clip_valid (needs depth_a/depth_b).
 *   pose[i] / fov[i]       exactly what the shell submits for eye i.
 */
typedef struct {
    VrMat4  eye_from_game_view[VR_EYE_COUNT];
    VrMat4  proj[VR_EYE_COUNT];
    VrMat4  clip_from_game_clip[VR_EYE_COUNT];
    VrPose  pose[VR_EYE_COUNT];
    VrFov   fov[VR_EYE_COUNT];
    int64_t display_time;
    int     clip_valid;
    int     valid;
} VrEyeTransforms;
const VrEyeTransforms *vr_eye_transforms(void);

/* Flat (quad layer) target: one 2D image, 4:3. */
int  vr_acquire_flat_target(VrEyeTarget *out);
void vr_release_flat_target(void);

/* --- Vulkan context (created by the shell through XR_KHR_vulkan_enable2) -- */
typedef struct {
    uint64_t instance, physical_device, device, queue;  /* Vk handles */
    uint32_t queue_family, queue_index;
    uint32_t api_version;
    uint32_t multiview;        /* 1: one 2-layer array swapchain; 0: two swapchains */
    uint32_t color_format;     /* VkFormat of the eye and flat swapchains */
    uint32_t eye_width, eye_height;
    void    *get_instance_proc_addr;   /* PFN_vkGetInstanceProcAddr */
} VrVulkanContext;
const VrVulkanContext *vr_vulkan_context(void);

/* The OpenXR runtime submits on the same VkQueue inside xrEndFrame and
 * xrReleaseSwapchainImage; VkQueue access must be externally synchronised.
 * Every vkQueueSubmit / vkQueuePresent / vkQueueWaitIdle on the shared queue
 * must be bracketed by these. */
void vr_queue_lock(void);
void vr_queue_unlock(void);

/* --- Renderer integration (the renderer defines these; the shell has weak
 * defaults that run the built-in test pattern). --------------------------- */
typedef struct {
    const char *const *instance_extensions; uint32_t instance_extension_count;
    const char *const *device_extensions;   uint32_t device_extension_count;
    const void *device_features_pnext;      /* chain appended to VkDeviceCreateInfo.pNext */
    const void *device_features10;          /* VkPhysicalDeviceFeatures* or NULL */
    uint32_t api_version;                   /* minimum; shell uses >= VK 1.1 */
} VrVulkanRequirements;
void vr_renderer_get_requirements(VrVulkanRequirements *req);  /* before Vulkan is created */
int  vr_renderer_init(const VrVulkanContext *ctx);            /* after; 0 = failure */
void vr_renderer_shutdown(void);

/* --- Game frame latch -----------------------------------------------------
 * vr_frame_state() returns the snapshot taken by the most recent
 * vr_latch_frame_state() (double-buffered: a pointer stays valid for at
 * least one further latch). The world-camera hook latches once per game
 * frame; if nobody has latched yet, vr_frame_state() latches itself. */
const VrFrameState *vr_latch_frame_state(void);

/* Fresh head pose predicted for a game frame starting now (game thread). */
int vr_sample_head(VrPose *head, int64_t *display_time);

/* Recentre: the stage-space yaw/position treated as "forward / neutral". */
void vr_get_recentre(VrPose *origin);
void vr_request_recentre(void);

/* --- Host-side turning and aim for the game hooks ------------------------- */
/* Degrees of yaw turned since the last call, game convention (positive =
 * left). Accumulated by the shell from the right stick (snap or smooth). */
double vr_consume_turn(void);

enum { VR_AIM_HEAD = 0, VR_AIM_HAND = 1 };
typedef struct {
    int   aim_mode;            /* VR_AIM_* */
    int   aim_hand;            /* 0 left, 1 right */
    float units_per_metre;     /* == VrFrameState.world_scale */
    float max_head_offset_m;   /* clamp for positional head offset */
    float aim_converge_m;      /* hand-aim convergence distance */
    float vm_offset_m[3];      /* viewmodel natural offset in aim space (x right, y up, z fwd) */
    float scope_zoom_min;      /* zoom factor at/above which the scope goes flat */
    float fov_margin;          /* game FOV enlargement factor over the eye FOV union */
    int   bob_filter;          /* 1: low-pass the camera's vertical position */
    int   hand_weapon;         /* 1: move first-person weapon to the aim hand */
} VrGameConfig;
const VrGameConfig *vr_game_config(void);

/* Rumble from the game (XInput motor speeds 0..65535). */
void vr_set_rumble(uint16_t left_motor, uint16_t right_motor);
/* A short haptic pulse from the game (game thread): hand 0 left / 1 right, strength 0..1 (scaled by the ini's
 * haptic_fire), milliseconds. Used for the player's shots (the muzzle flash draw). */
void vr_game_haptic(int hand, float strength, uint32_t ms);
/* Pad state for the runtime's XInput path; 0 when not connected. */
int vr_get_pad(VrPadState *out);

/* Multiview: VkImageView (2D_ARRAY, both layers, UNORM view of the sRGB
 * swapchain image) for the image acquired by vr_acquire_eye_targets(); 0 when
 * the shell runs two swapchains. VrEyeTarget.view is always a single-layer
 * 2D view of that eye. Both are UNORM views: write display-referred
 * (gamma-encoded) colour, as the Xbox frame already is; the compositor reads
 * the image as sRGB. */
uint64_t vr_eye_array_view(void);

/* Optional: the renderer's submitting thread; lets the runtime schedule it
 * as the renderer main thread (XR_KHR_android_thread_settings). */
void vr_register_render_thread(void);

/* World-pass marker (renderer implements; the shell has a no-op stub).
 * The game hook calls it with the D3D push-buffer position (guest VA, as
 * kelvin_mark_view takes it) where the world camera's draws begin
 * (begin = 1) and end (begin = 0), so the renderer knows which draws the
 * per-eye clip_from_game_clip transform applies to. HUD / 2D draws lie
 * outside the bracket. */
void vr_mark_world_pass(uint32_t pushbuffer_va, int begin);

/* Game -> shell: true once the game thread has published at least one
 * world camera in the last 250 ms (diagnostics / flat fallback). */
int vr_game_camera_fresh(void);

/* XInput button bits used in VrPadState.buttons (same values as XINPUT_GAMEPAD_*). */
#define VR_PAD_DPAD_UP        0x0001
#define VR_PAD_DPAD_DOWN      0x0002
#define VR_PAD_DPAD_LEFT      0x0004
#define VR_PAD_DPAD_RIGHT     0x0008
#define VR_PAD_START          0x0010
#define VR_PAD_BACK           0x0020
#define VR_PAD_LEFT_THUMB     0x0040
#define VR_PAD_RIGHT_THUMB    0x0080
#define VR_PAD_LEFT_SHOULDER  0x0100
#define VR_PAD_RIGHT_SHOULDER 0x0200
#define VR_PAD_A              0x1000
#define VR_PAD_B              0x2000
#define VR_PAD_X              0x4000
#define VR_PAD_Y              0x8000

/* ======================================================================
 * Additions (vr-glue stream: the Kelvin Vulkan backend joined to the
 * shell). Everything above is unchanged. See port/notes/vr-glue.md.
 * ====================================================================== */

/* --- Contract wording ------------------------------------------------------
 * VrGameCamera.proj: the projection that maps the game camera's view space to
 *   exactly what the title's vertex programs output for world draws before
 *   the viewport transform: D3D clip, x right, y up, z/w in [0,1], w = view
 *   depth. Its x, y and w rows must be the title's own (the per-eye
 *   correction recovers view positions through them); the depth row only
 *   decides the depth values written. When proj is not a perspective matrix
 *   the backend builds one from half_fov_x/y (tangents) and a default range.
 * VrFrameState.world_scale, VrGameConfig.units_per_metre, VrRenderPose
 *   .world_scale: game units per metre.
 *
 * --- Who applies the per-eye correction -------------------------------------
 * Exactly one place: the Kelvin backend's vertex stage (kvk_stereo.c, the
 * FROM_W form), from vr_render_pose() of the frame. The shell owns the poses:
 * it latches them once (in vr_acquire_eye_targets), the backend draws with
 * them, and the projection layer is submitted with them. VrEyeTransforms
 * .clip_from_game_clip is the same correction in the FULL form (needs the
 * exact depth row) for other renderers and diagnostics; a renderer applies
 * one of the two, never both. */

/* --- Frame identity --------------------------------------------------------
 * The poses one rendered frame stands for. Filled by vr_acquire_eye_targets()
 * (the late latch) from the camera given with vr_set_render_frame(); valid
 * until vr_release_eye_targets(), which submits eye_pose/eye_fov unchanged. */
typedef struct {
    uint32_t game_frame;                /* VrGameCameraExt.game_frame of the camera drawn (0: unknown) */
    int      present_mode;              /* VR_PRESENT_* this frame is presented as */
    int64_t  display_time;              /* XrTime the eye poses are predicted for */
    VrPose   game_head;                 /* stage head pose the game camera was built from (VrGameCameraExt.head_pose) */
    VrPose   head;                      /* stage head pose of the late latch, clamped as the game clamps it */
    VrPose   eye_pose[VR_EYE_COUNT];    /* stage eye poses the frame is drawn for == what is submitted */
    VrFov    eye_fov[VR_EYE_COUNT];
    float    world_scale;               /* game units per metre */
    uint64_t serial;                    /* increments with every late latch */
    int      valid;
} VrRenderPose;

/* Renderer (render thread): the frame being drawn uses this game camera (NULL:
 * the newest published) and is presented as present_mode (< 0: the game's
 * current vr_present_mode()). Takes copies. Call before acquiring that
 * frame's targets; it stays until the next call. */
void vr_set_render_frame(const VrGameCamera *cam, const VrGameCameraExt *ext, int present_mode);
const VrRenderPose *vr_render_pose(void);

/* --- HUD quad layer --------------------------------------------------------
 * One 2D image (4:3, VrHudLayout.pixel size) shown as an XrCompositionLayerQuad
 * locked to the head, in front of the projection layer, premultiplied alpha
 * (transparent where the HUD drew nothing). Acquire it in a STEREO frame
 * while the eye targets are held; vr_release_hud_target() hands it back but
 * the shell publishes it with the next vr_release_eye_targets(), so a HUD is
 * never shown over another frame's world. A STEREO frame that does not
 * acquire it shows no HUD. Images in/out in COLOR_ATTACHMENT_OPTIMAL; UNORM
 * views of the sRGB image when VrVulkanFeatures.swapchain_mutable_format. */
int  vr_acquire_hud_target(VrEyeTarget *out);
void vr_release_hud_target(void);

typedef struct {
    float distance_m;      /* view-space distance of the HUD quad */
    float width_m;         /* its width; height = width * 3/4 */
    uint32_t width, height;/* image size (1280x960) */
} VrHudLayout;
const VrHudLayout *vr_hud_layout(void);

/* --- Vulkan features the shell enabled on the device (renderer request 3) --
 * The shell enables what vr_renderer_get_requirements() asked for, where the
 * device has it (requests the device lacks are logged and left off). */
typedef struct {
    uint32_t api_version;
    uint32_t multiview, robust_buffer_access, depth_clamp, sampler_anisotropy, texture_compression_bc;
    uint32_t swapchain_mutable_format;  /* swapchains created MUTABLE_FORMAT; views may use the UNORM twin */
    uint32_t swapchain_format_list;     /* ... with XR_KHR_vulkan_swapchain_format_list (sRGB + UNORM) */
} VrVulkanFeatures;
const VrVulkanFeatures *vr_vulkan_features(void);

/* App-private directory for shader / pipeline caches (created; never NULL). */
const char *vr_cache_dir(void);

/* --- Diagnostics (device-preflight): what the shell has presented, for the game's heartbeat / stall watchdog and
 * crash report (src/main.c, __ANDROID__). Counters only grow; times are CLOCK_MONOTONIC ns, 0 = never. Any thread. */
typedef struct {
    uint64_t xr_frames;              /* XR frame loop iterations (xrWaitFrame returned) */
    uint64_t eye_releases;           /* stereo frames the renderer handed to the shell */
    uint64_t flat_releases;          /* flat (menu / cinema / scope) frames */
    int64_t  last_xr_frame_ns, last_eye_release_ns, last_flat_release_ns;
    uint32_t last_game_frame;        /* VrGameCameraExt.game_frame of the last stereo frame */
    int32_t  present_mode;           /* vr_present_mode() now */
    int32_t  session_running, renderer_started;
} VrDiagCounters;
void vr_diag_counters(VrDiagCounters *out);

/* ======================================================================
 * Additions (gun-latch stream: the first-person weapon follows the hand
 * pose latched at display time). Everything above is unchanged. See
 * port/notes/gun-latch.md.
 * ====================================================================== */

/* --- What the game hook placed the weapon at (game thread) -----------------
 * Published right before vr_publish_game_camera_ex() and stored with that
 * camera (the renderer's camera mark carries both). valid = 1 only when this
 * frame's first-person weapon was re-attached to `hand` (hand weapon on,
 * hand tracked, conventions locked); otherwise the weapon is not moved. */
typedef struct {
    VrPose   hand;          /* stage aim pose of the weapon hand (VrFrameState.hand[hand_index].aim of the latch used) */
    int      hand_index;    /* 0 left, 1 right */
    int      valid;
} VrGameGun;
void vr_publish_game_gun(const VrGameGun *gun);
const VrGameGun *vr_game_gun(void);   /* the one stored with the newest camera (vr_game_camera_ext) */

/* Game thread, after the weapon's MVP upload was re-targeted to the hand:
 * the exact 16 floats uploaded (rows, as the transform constants hold them)
 * and the first constant register written (hardware index: 10 for the mesh,
 * 112 for the muzzle flash). Recorded with the frame's camera mark; the
 * renderer moves a draw only when its position is made through one of
 * these bit for bit. The renderer implements it; the shell has a no-op. */
void vr_mark_viewmodel_mvp(const float mvp[16], int constant_register);
/* Game thread, with the weapon mesh's MVP (after vr_mark_viewmodel_mvp): the off-hand mirror A (row-major 4x4,
 * column vectors) in the weapon's model space, for the first-person arms (vgm_off_hand_mirror). */
void vr_mark_viewmodel_offhand(const float a[16]);

/* --- The weapon's late latch (render thread) --------------------------------
 * vr_set_render_gun(): what the frame being drawn placed its weapon at
 * (NULL: nothing); call it with vr_set_render_frame(). vr_acquire_eye_targets()
 * then locates that hand at the eyes' display time, in the same latch, into
 * VrRenderGun. valid: both poses known and gun_latch on; the renderer moves
 * the weapon's draws by hand * inverse(game_hand) (stage space). */
typedef struct {
    VrPose   game_hand;     /* VrGameGun.hand of the frame */
    VrPose   hand;          /* the late latch's */
    int      hand_index;
    int      valid;
    uint64_t serial;        /* == VrRenderPose.serial of the latch */
} VrRenderGun;
void vr_set_render_gun(const VrGameGun *gun);
const VrRenderGun *vr_render_gun(void);

/* blackvr.ini gun_latch (default on) and the debug env BLACKVR_GUN_LATCH_DEBUG
 * (1, or a colour 0xAABBGGRR: tint the draws taken for the weapon). */
typedef struct {
    int      enabled;
    uint32_t debug_tint;    /* 0: off */
} VrGunLatchConfig;
const VrGunLatchConfig *vr_gun_latch_config(void);

enum { VR_LEFT_HAND_GRIP = 0, VR_LEFT_HAND_ALWAYS = 1, VR_LEFT_HAND_NEVER = 2, VR_LEFT_HAND_TRACKED = 3 };
/* First-person arms (render thread; port/notes/arms.md): cut = blackvr.ini arms (hands), mode = left_hand
 * (VR_LEFT_HAND_*), engaged = the two-handed grip is on now. */
void vr_arms_state(int *cut, int *mode, int *engaged);

/* --- Both-eye capture (debug; port/notes/renderer.md section 12) -------------
 * A request (both thumbsticks held VR_CAPTURE_HOLD_S -- past the 1 s recentre
 * chord --, every BLACKVR_CAPTURE_EVERY seconds, or vr_capture_request())
 * makes the renderer save the next STEREO frame's two eye images as PNG to
 * vr_capture_dir() (<game root>/shots). vr_capture_take(): render thread, once
 * per STEREO frame; nonzero once per request. */
#define VR_CAPTURE_HOLD_S 2.5
void vr_capture_request(void);
int vr_capture_take(void);
const char *vr_capture_dir(void);

/* ======================================================================
 * Additions (VR controls: actions resolved through the game's own action
 * map, body gestures; port/notes/xr-shell.md "Input"). Everything above is
 * unchanged.
 * ====================================================================== */

/* Game thread, once: the live action map's resolver (slot 0..36 -> BLACK's
 * control id 0..27, -1 none; black_pc_input_resolver). The shell resolves the
 * VR actions through it (XR thread); until it is set, gameplay sends nothing. */
void vr_set_action_resolver(int (*resolve)(int slot));

/* Game thread, each player input update (1EC450): the game's own state the
 * VR controls keep in step with. */
typedef struct {
    int      valid;          /* the player's input component was seen */
    int      crouched;       /* component +40: the game's crouch */
    int      crouch_toggle;  /* component +102: the profile's Toggle Crouch */
    int      aiming;         /* weapon +105: aim / scope engaged */
    int      scoped;         /* the weapon's descriptor +C0: a scoped weapon */
    float    max_zoom;       /* descriptor +B4 */
    float    zoom;           /* actor +2A4 (1 = none) */
    uint32_t weapon;         /* guest address of the weapon in hand */
    uint32_t serial;         /* increments per publish */
    /* diagnostics (logged with BLACKVR_INPUT_TRACE=1): the component's intents +40..+4F (+41 fire, +42/+43
     * grenade, +45 aim, +4B melee), the weapon's state +D8 and its magazine [[weapon+F4]+18] */
    uint8_t  intent[16];
    uint8_t  inventory[2];   /* actor +2BA weapon slots, +2BB the slot in hand */
    uint8_t  latch[16];      /* +100..+10F: options (+101..+103) and edge latches (+104 aim, +105 crouch, +107 suppressor, +108 health pack, +10B/+10C use) */
    uint32_t weapon_state;
    int      clip;
    /* the shot spread's inputs (1CEF40): weapon +E4 (scale), +E0 (time firing); the shot record's
     * [[weapon+F4]+C] params +8 (Max Spread Angle), +28 / +2C (Accuracy Fall Off Over Time / Time) */
    float    spread_scale, fire_time, max_spread, spread_extra, spread_time;
} VrGameInputState;
void vr_publish_game_input(const VrGameInputState *s);

/* Game thread, same place: the aim (scope) the VR controls want: -1 leave it
 * to the game's own input, 0 off, 1 on. Written into the component's aim
 * intent (+45, with the toggle latch +104 held) before the native update. */
int vr_game_aim_request(void);


/* ---- VR SETTINGS inside the game's Options menu (port/notes/settings-panel.md) -----------------------------------
 * Game thread (src/black_vr_menu.inc, the front-end update). The shell keeps the settings (vr_settings_menu.c): the
 * page asks it for its rows' text and hands it the player's left / right / confirm on a row. Section -1 is the list
 * of sections (GRAPHICS, COMFORT, GESTURES, INFO, then RECALIBRATE HEIGHT and RESTORE DEFAULTS). Texts are capitals,
 * ASCII. Live settings apply at once; the file is written when the page is left (vr_menu_leave). */
enum { VR_MENU_ROW_SETTING = 0, VR_MENU_ROW_SECTION = 1, VR_MENU_ROW_ACTION = 2, VR_MENU_ROW_INFO = 3 };
typedef struct {
    char name[40];
    char value[48];
    int  kind;               /* VR_MENU_ROW_*: INFO rows (and settings this headset lacks) do not change */
} VrMenuRow;
/* vr_menu_activate results */
enum { VR_MENU_NOTHING = 0, VR_MENU_CHANGED = 1, VR_MENU_DONE = 2, VR_MENU_OPEN_SECTION_0 = 16 };
int  vr_menu_native(void);                               /* blackvr.ini settings_menu: the page is offered */
void vr_menu_enter(void);                                /* the page opened: edits start from the running config */
int  vr_menu_rows(int section, VrMenuRow *out, int max); /* the rows' texts now; returns the count */
int  vr_menu_activate(int section, int row, int dir);    /* dir -1 left, +1 right, 0 confirm */
const char *vr_menu_title(int section);                  /* -1: "VR SETTINGS" */
void vr_menu_leave(void);                                /* left the page: saved when changed */

#ifdef __cplusplus
}
#endif

#endif /* VR_BRIDGE_H */
