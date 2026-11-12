/*
 * vr_bridge.c - the shell's side of port/quest/vr_bridge.h.
 *
 * Threads
 *   XR thread    (android_main): xrWaitFrame/BeginFrame/EndFrame, input sync,
 *                haptics. Owns g_live (hands, pad, session flags).
 *   game thread  (black_game_main): latches a VrFrameState once per world
 *                camera (vr_latch_frame_state), publishes VrGameCamera,
 *                consumes host turns.
 *   render thread (renderer): vr_set_render_frame (the camera of the frame it
 *                draws), vr_acquire_eye_targets (late-latches the eye poses
 *                into a VrRenderPose), draws for exactly those poses,
 *                vr_release_eye_targets (submits exactly those poses).
 *   OHCI thread  (runtime USB): vr_get_pad, vr_set_rumble.
 * All shared state below is guarded by g_lock (short critical sections, no
 * blocking calls inside). The VkQueue has its own recursive lock.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "xr_shell.h"
#include "vr_game_math.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- live state (XR thread writes) ---------------------------------------- */
static VrFrameState g_live;
static VrTouchState g_touch;
static VrInputRuntime g_in_rt;
static float g_trigger[2];
static int g_have_origin, g_recentre_pending;
static VrPose g_origin;
static float g_rumble[2];

/* ---- latched frames (game thread) ------------------------------------------ */
#define LATCH_RING 3
static VrFrameState g_latched[LATCH_RING];
static volatile int g_latch_idx = -1;

/* ---- game camera ------------------------------------------------------------ */
/* deeper than the latch ring: the renderer may draw a frame or two behind the game */
#define CAM_RING 8
static VrGameCamera g_cam[CAM_RING];
static VrGameCameraExt g_cam_ext[CAM_RING];
static VrGameGun g_cam_gun[CAM_RING];           /* gun latch: the weapon pose published with each camera */
static VrGameGun g_gun_pending;                 /* vr_publish_game_gun, taken by the next camera */
static volatile int g_cam_idx = -1;
static int64_t g_cam_time_ns;
static volatile int g_present_mode = VR_PRESENT_FLAT_MENU;

/* ---- render side -------------------------------------------------------------- */
static VrEyeTransforms g_eye_tf;
static XrPosef g_pending_pose[VR_EYE_COUNT];   /* poses of the images held by the renderer */
static XrFovf g_pending_fov[VR_EYE_COUNT];
static VrVulkanContext g_vk_ctx;
static uint64_t g_array_view;
/* the frame being rendered (render thread; vr_set_render_frame) */
static VrGameCamera g_rcam;
static VrGameCameraExt g_rext;
static int g_rhave, g_rmode = -1;
static VrRenderPose g_rpose;
static uint64_t g_rserial;
static VrGameGun g_rgun_in;                     /* gun latch: the weapon pose of the frame being drawn */
static VrRenderGun g_rgun;

/* ---- diagnostics (vr_diag_counters) ------------------------------------------------------------------------- */
static volatile uint64_t g_d_xr_frames, g_d_eye, g_d_flat;
static volatile int64_t g_d_xr_ns, g_d_eye_ns, g_d_flat_ns;
static volatile uint32_t g_d_game_frame;

/* both-eye capture (vr_capture_*): the thumbstick chord (input thread, under g_lock) and the pending request */
static double g_cap_hold_s;
static int g_cap_fired;
static int g_cap_request;

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static VrPose pose_from_xr(const XrPosef *p)
{
    VrPose r;
    r.orientation = vrm_q(p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w);
    r.position = vrm_v3(p->position.x, p->position.y, p->position.z);
    return r;
}
static VrFov fov_from_xr(const XrFovf *f)
{
    VrFov r;
    r.left = f->angleLeft; r.right = f->angleRight; r.up = f->angleUp; r.down = f->angleDown;
    return r;
}

void vrb_init(XrShell *s)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s->queue_lock, &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_init(&s->tp_lock, NULL);
    memset(&g_live, 0, sizeof(g_live));
    g_live.head = vrm_pose_identity();
    g_live.world_scale = s->cfg.game.units_per_metre;
    g_live.ipd_m = 0.064f;
    g_origin = vrm_pose_identity();
}

/* ---- XR-thread updates ------------------------------------------------------ */

void vrb_set_session(XrShell *s, int running, int focused)
{
    (void)s;
    pthread_mutex_lock(&g_lock);
    g_live.session_running = running && focused;
    pthread_mutex_unlock(&g_lock);
}

void vrb_set_input(XrShell *s, const VrTouchState *touch, const VrPose aim[2], const VrPose grip[2],
                   const int valid[2], double dt)
{
    int h;
    double move_rot = 0.0;
    const int mode = vrb_present_mode_now();
    const int in_game = mode == VR_PRESENT_STEREO || mode == VR_PRESENT_FLAT_SCOPE;
    pthread_mutex_lock(&g_lock);
    g_touch = *touch;
    for (h = 0; h < 2; h++) {
        g_live.hand[h].aim = aim[h];
        g_live.hand[h].grip = grip[h];
        g_live.hand[h].valid = valid[h];
    }
    g_trigger[0] = touch->lt;
    g_trigger[1] = touch->rt;
    /* head-relative locomotion while the game's yaw follows the aim hand */
    if (in_game && s->cfg.locomotion == VR_LOCO_HEAD && s->cfg.game.aim_mode == VR_AIM_HAND &&
        valid[s->cfg.game.aim_hand] && g_have_origin) {
        const VrPose head_rel = vgm_relative(g_origin, g_live.head);
        const VrPose hand_rel = vgm_relative(g_origin, aim[s->cfg.game.aim_hand]);
        double hy, hp, ay, ap;
        vgm_dir_angles(vgm_aim_dir(0, head_rel, hand_rel, vrm_v3(0, 0, 0), 1.0f), &hy, &hp);
        vgm_dir_angles(vgm_aim_dir(1, head_rel, hand_rel, vrm_v3(0, 0, 0), s->cfg.game.aim_converge_m), &ay, &ap);
        move_rot = vrm_wrap_deg(hy - ay);
    }
    vr_map_pad(&s->cfg.input, &g_in_rt, touch, in_game, move_rot, dt, &g_live.pad);
    if (g_in_rt.recentre_request) { g_in_rt.recentre_request = 0; g_recentre_pending = 1; }
    /* both-eye capture: both thumbsticks still held VR_CAPTURE_HOLD_S (the recentre chord fired at 1 s) */
    if (touch->lclick && touch->rclick) {
        g_cap_hold_s += dt;
        if (g_cap_hold_s >= VR_CAPTURE_HOLD_S && !g_cap_fired) { g_cap_fired = 1; vr_capture_request(); }
    } else {
        g_cap_hold_s = 0.0;
        g_cap_fired = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void vrb_on_xr_frame(XrShell *s, XrTime predicted, XrDuration period)
{
    XrPosef head;
    g_d_xr_frames++;
    g_d_xr_ns = mono_ns();
    pthread_mutex_lock(&g_lock);
    g_live.predicted_display_time = predicted;
    g_live.world_scale = s->cfg.game.units_per_metre;
    g_live.frame_index++;
    pthread_mutex_unlock(&g_lock);
    (void)period;
    if (xrs_locate_head(s, predicted, &head)) {
        const VrPose h = pose_from_xr(&head);
        pthread_mutex_lock(&g_lock);
        g_live.head = h;
        if (!g_have_origin || g_recentre_pending) {
            g_origin = vgm_recentre_origin(h);
            g_have_origin = 1;
            g_recentre_pending = 0;
            XLOGI("recentred at %.2f %.2f %.2f yaw %.1f", h.position.x, h.position.y, h.position.z,
                  vrm_q_yaw(h.orientation) / VRM_DEG);
        }
        pthread_mutex_unlock(&g_lock);
    }
}

void vrb_recentre_from(const VrPose *head)
{
    pthread_mutex_lock(&g_lock);
    if (head) { g_origin = vgm_recentre_origin(*head); g_have_origin = 1; }
    else g_recentre_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

int vrb_take_recentre_request(void)
{
    int r;
    pthread_mutex_lock(&g_lock);
    r = g_recentre_pending;
    pthread_mutex_unlock(&g_lock);
    return r;
}

void vrb_get_rumble(float *left, float *right)
{
    pthread_mutex_lock(&g_lock);
    *left = g_rumble[0];
    *right = g_rumble[1];
    pthread_mutex_unlock(&g_lock);
}

void vrb_touch_debug(VrPadState *pad)
{
    pthread_mutex_lock(&g_lock);
    *pad = g_live.pad;
    pthread_mutex_unlock(&g_lock);
}

void vrb_head_for_testpattern(VrPose hand_aim[2], int valid[2], float trigger[2])
{
    int h;
    pthread_mutex_lock(&g_lock);
    for (h = 0; h < 2; h++) {
        hand_aim[h] = g_live.hand[h].aim;
        valid[h] = g_live.hand[h].valid;
        trigger[h] = g_trigger[h];
    }
    pthread_mutex_unlock(&g_lock);
}

int vrb_present_mode_now(void)
{
    const int m = g_present_mode;
    if (m == VR_PRESENT_STEREO || m == VR_PRESENT_FLAT_SCOPE)
        return vr_game_camera_fresh() ? m : VR_PRESENT_FLAT_MENU;
    return m;
}

/* ---- contract: frame state ------------------------------------------------------ */

static XrTime game_predict_time(XrShell *s)
{
    XrTime now;
    const XrDuration period = s->last_predicted_period > 0 ? s->last_predicted_period : 13888888;
    if (xrs_now(s, &now)) {
        /* a game frame started now is displayed ~1 game frame + 1 render frame later */
        return now + period * 2;
    }
    return s->last_predicted_time + period;
}

int vr_sample_head(VrPose *head, int64_t *display_time)
{
    XrShell *s = &g_xr;
    XrPosef p;
    XrTime t;
    if (!s->session_running) return 0;
    t = game_predict_time(s);
    if (!xrs_locate_head(s, t, &p)) return 0;
    *head = pose_from_xr(&p);
    if (display_time) *display_time = t;
    return 1;
}

const VrFrameState *vr_latch_frame_state(void)
{
    XrShell *s = &g_xr;
    const int next = (g_latch_idx + 1 + LATCH_RING) % LATCH_RING;
    VrFrameState *f = &g_latched[next];
    XrPosef pose[VR_EYE_COUNT];
    XrFovf fov[VR_EYE_COUNT];
    XrTime t = 0;
    int i;

    pthread_mutex_lock(&g_lock);
    *f = g_live;
    pthread_mutex_unlock(&g_lock);
    if (s->session_running) {
        t = game_predict_time(s);
        if (xrs_locate_views(s, t, pose, fov)) {
            VrPose head;
            XrPosef hp;
            for (i = 0; i < VR_EYE_COUNT; i++) {
                f->eye[i].pose = pose_from_xr(&pose[i]);
                f->eye[i].fov = fov_from_xr(&fov[i]);
                f->eye[i].view = vrm_m4_view_from_pose(f->eye[i].pose);
                f->eye[i].proj = vrm_proj_vulkan(f->eye[i].fov, 0.05f, 1000.0f);
            }
            f->ipd_m = vrm_len(vrm_sub(f->eye[1].pose.position, f->eye[0].pose.position));
            if (xrs_locate_head(s, t, &hp)) {
                head = pose_from_xr(&hp);
                f->head = head;
            }
            f->predicted_display_time = t;
        }
    }
    g_latch_idx = next;
    return f;
}

const VrFrameState *vr_frame_state(void)
{
    const int i = g_latch_idx;
    if (i < 0) return vr_latch_frame_state();
    return &g_latched[i];
}

void vr_get_recentre(VrPose *origin)
{
    pthread_mutex_lock(&g_lock);
    *origin = g_origin;
    pthread_mutex_unlock(&g_lock);
}

void vr_request_recentre(void)
{
    pthread_mutex_lock(&g_lock);
    g_recentre_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

double vr_consume_turn(void)
{
    double d;
    pthread_mutex_lock(&g_lock);
    d = g_in_rt.turn_accum_deg;
    g_in_rt.turn_accum_deg = 0.0;
    pthread_mutex_unlock(&g_lock);
    return d;
}

const VrGameConfig *vr_game_config(void) { return &g_xr.cfg.game; }

void vr_set_rumble(uint16_t left_motor, uint16_t right_motor)
{
    pthread_mutex_lock(&g_lock);
    g_rumble[0] = left_motor / 65535.0f;
    g_rumble[1] = right_motor / 65535.0f;
    pthread_mutex_unlock(&g_lock);
}

int vr_get_pad(VrPadState *out)
{
    int connected;
    pthread_mutex_lock(&g_lock);
    connected = g_live.session_running;
    if (connected) *out = g_live.pad;
    else memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&g_lock);
    return connected;
}

int vr_active(void) { return g_xr.session_running && g_xr.vk_device != VK_NULL_HANDLE; }

/* ---- contract: game camera ------------------------------------------------------- */

void vr_publish_game_camera_ex(const VrGameCamera *cam, const VrGameCameraExt *ext)
{
    int next;
    pthread_mutex_lock(&g_lock);
    next = (g_cam_idx + 1 + CAM_RING) % CAM_RING;
    g_cam[next] = *cam;
    if (ext) g_cam_ext[next] = *ext;
    else memset(&g_cam_ext[next], 0, sizeof(g_cam_ext[next]));
    g_cam_gun[next] = g_gun_pending;             /* the weapon pose goes with this camera, once */
    memset(&g_gun_pending, 0, sizeof(g_gun_pending));
    g_cam_idx = next;
    g_cam_time_ns = mono_ns();
    pthread_mutex_unlock(&g_lock);
}

void vr_publish_game_camera(const VrGameCamera *cam) { vr_publish_game_camera_ex(cam, NULL); }

void vr_publish_game_gun(const VrGameGun *gun)
{
    pthread_mutex_lock(&g_lock);
    if (gun) g_gun_pending = *gun;
    else memset(&g_gun_pending, 0, sizeof(g_gun_pending));
    pthread_mutex_unlock(&g_lock);
}

const VrGameGun *vr_game_gun(void)
{
    static const VrGameGun none;
    const int i = g_cam_idx;
    return i < 0 ? &none : &g_cam_gun[i];
}

const VrGameCamera *vr_game_camera(void)
{
    static const VrGameCamera none;
    const int i = g_cam_idx;
    return i < 0 ? &none : &g_cam[i];
}

const VrGameCameraExt *vr_game_camera_ext(void)
{
    static const VrGameCameraExt none;
    const int i = g_cam_idx;
    return i < 0 ? &none : &g_cam_ext[i];
}

int vr_game_camera_fresh(void)
{
    int64_t t;
    pthread_mutex_lock(&g_lock);
    t = g_cam_time_ns;
    pthread_mutex_unlock(&g_lock);
    return t != 0 && mono_ns() - t < 250000000LL;
}

void vr_set_present_mode(int mode)
{
    if (mode >= VR_PRESENT_STEREO && mode <= VR_PRESENT_FLAT_SCOPE) g_present_mode = mode;
}
int vr_present_mode(void) { return vrb_present_mode_now(); }

/* ---- contract: render targets ------------------------------------------------------ */

const VrVulkanContext *vr_vulkan_context(void)
{
    XrShell *s = &g_xr;
    g_vk_ctx.instance = (uint64_t)(uintptr_t)s->vk_instance;
    g_vk_ctx.physical_device = (uint64_t)(uintptr_t)s->vk_phys;
    g_vk_ctx.device = (uint64_t)(uintptr_t)s->vk_device;
    g_vk_ctx.queue = (uint64_t)(uintptr_t)s->vk_queue;
    g_vk_ctx.queue_family = s->vk_queue_family;
    g_vk_ctx.queue_index = 0;
    g_vk_ctx.api_version = s->vk_api;
    g_vk_ctx.multiview = (uint32_t)s->multiview;
    g_vk_ctx.color_format = (uint32_t)s->color_format;
    g_vk_ctx.eye_width = s->eye[0].width;
    g_vk_ctx.eye_height = s->eye[0].height;
    g_vk_ctx.get_instance_proc_addr = (void *)vkGetInstanceProcAddr;
    return &g_vk_ctx;
}

void vr_queue_lock(void) { pthread_mutex_lock(&g_xr.queue_lock); }
void vr_queue_unlock(void) { pthread_mutex_unlock(&g_xr.queue_lock); }

static XrTime render_predict_time(XrShell *s)
{
    XrTime now;
    const XrDuration period = s->last_predicted_period > 0 ? s->last_predicted_period : 13888888;
    XrTime t = s->last_predicted_time;
    if (xrs_now(s, &now)) {
        /* the next display time at least one period after now (render time) */
        if (t <= 0) return now + period * 2;
        while (t < now + period) t += period;
    }
    return t;
}

void vrb_renderer_claim(XrShell *s)
{
    if (s->renderer_started) return;
    pthread_mutex_lock(&s->tp_lock);         /* waits for a test-pattern frame in flight */
    s->renderer_started = 1;
    pthread_mutex_unlock(&s->tp_lock);
    XLOGI("renderer took over the display");
}

void vr_set_render_frame(const VrGameCamera *cam, const VrGameCameraExt *ext, int present_mode)
{
    pthread_mutex_lock(&g_lock);
    g_rhave = cam != NULL;
    if (cam) g_rcam = *cam;
    if (cam && ext) g_rext = *ext;
    else memset(&g_rext, 0, sizeof(g_rext));
    g_rmode = present_mode;
    pthread_mutex_unlock(&g_lock);
}

const VrRenderPose *vr_render_pose(void) { return &g_rpose; }

void vr_set_render_gun(const VrGameGun *gun)
{
    pthread_mutex_lock(&g_lock);
    if (gun) g_rgun_in = *gun;
    else memset(&g_rgun_in, 0, sizeof(g_rgun_in));
    pthread_mutex_unlock(&g_lock);
}

const VrRenderGun *vr_render_gun(void) { return &g_rgun; }

const VrGunLatchConfig *vr_gun_latch_config(void)
{
    static VrGunLatchConfig c;
    static int init;
    if (!init) {
        const char *e = getenv("BLACKVR_GUN_LATCH_DEBUG");
        c.debug_tint = 0;
        if (e && *e && strcmp(e, "0")) {
            char *end;
            const unsigned long v = strtoul(e, &end, 0);
            c.debug_tint = v > 1 && *end == 0 ? (uint32_t)v : 0x80FF00FFu;   /* "1": half-transparent magenta */
        }
        init = 1;
    }
    c.enabled = g_xr.cfg.gun_latch;
    return &c;
}

/* Gun latch, inside the late latch: the weapon hand at the eyes' display time t (same latch serial). */
static void latch_gun(XrShell *s, XrTime t)
{
    VrGameGun in;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    pthread_mutex_lock(&g_lock);
    in = g_rgun_in;
    pthread_mutex_unlock(&g_lock);
    memset(&g_rgun, 0, sizeof(g_rgun));
    g_rgun.serial = g_rpose.serial;
    g_rgun.game_hand = in.hand;
    g_rgun.hand = in.hand;
    g_rgun.hand_index = in.hand_index & 1;
    if (!in.valid || !s->cfg.gun_latch || !s->session || t <= 0 || !s->aim_space[in.hand_index & 1]) return;
    if (XR_FAILED(xrLocateSpace(s->aim_space[in.hand_index & 1], s->world_space, t, &loc)) ||
        (loc.locationFlags & need) != need)
        return;
    g_rgun.hand = pose_from_xr(&loc.pose);
    g_rgun.valid = 1;
}

/* The renderer's frame: its camera (or the newest), and its presentation mode. */
static void render_frame(VrGameCamera *cam, VrGameCameraExt *ext, int *mode)
{
    const int now_mode = vrb_present_mode_now();      /* takes g_lock itself */
    pthread_mutex_lock(&g_lock);
    if (g_rhave) {
        *cam = g_rcam;
        *ext = g_rext;
    } else if (g_cam_idx >= 0) {
        *cam = g_cam[g_cam_idx];
        *ext = g_cam_ext[g_cam_idx];
    } else {
        memset(cam, 0, sizeof(*cam));
        memset(ext, 0, sizeof(*ext));
        ext->head_pose = vrm_pose_identity();
    }
    *mode = g_rmode >= 0 ? g_rmode : now_mode;
    pthread_mutex_unlock(&g_lock);
}

int vr_acquire_eye_targets(VrEyeTarget out[VR_EYE_COUNT])
{
    XrShell *s = &g_xr;
    XrPosef pose[VR_EYE_COUNT], hp;
    XrFovf fov[VR_EYE_COUNT];
    VrPose eye[VR_EYE_COUNT], head, origin;
    VrFov vf[VR_EYE_COUNT];
    VrGameCamera cam;
    VrGameCameraExt ext;
    uint32_t idx[VR_EYE_COUNT];
    int i, mode, n = s->multiview ? 1 : VR_EYE_COUNT;
    XrTime t;

    if (!vr_active() || !s->eye[0].handle) return 0;
    vrb_renderer_claim(s);
    for (i = 0; i < n; i++)
        if (!xrs_swapchain_acquire(s, &s->eye[i], &idx[i])) {
            while (--i >= 0) xrs_swapchain_release(s, &s->eye[i]);
            return 0;
        }
    /* late latch: the newest prediction for when this image will be shown */
    t = render_predict_time(s);
    if (!xrs_locate_views(s, t, pose, fov) || !xrs_locate_head(s, t, &hp)) {
        const VrFrameState *f = vr_frame_state();
        for (i = 0; i < VR_EYE_COUNT; i++) {
            pose[i].orientation.x = f->eye[i].pose.orientation.x; pose[i].orientation.y = f->eye[i].pose.orientation.y;
            pose[i].orientation.z = f->eye[i].pose.orientation.z; pose[i].orientation.w = f->eye[i].pose.orientation.w;
            pose[i].position.x = f->eye[i].pose.position.x; pose[i].position.y = f->eye[i].pose.position.y;
            pose[i].position.z = f->eye[i].pose.position.z;
            fov[i].angleLeft = f->eye[i].fov.left; fov[i].angleRight = f->eye[i].fov.right;
            fov[i].angleUp = f->eye[i].fov.up; fov[i].angleDown = f->eye[i].fov.down;
        }
        hp.orientation.x = f->head.orientation.x; hp.orientation.y = f->head.orientation.y;
        hp.orientation.z = f->head.orientation.z; hp.orientation.w = f->head.orientation.w;
        hp.position.x = f->head.position.x; hp.position.y = f->head.position.y; hp.position.z = f->head.position.z;
        t = f->predicted_display_time;
    }
    for (i = 0; i < VR_EYE_COUNT; i++) { eye[i] = pose_from_xr(&pose[i]); vf[i] = fov_from_xr(&fov[i]); }
    head = pose_from_xr(&hp);
    vr_get_recentre(&origin);
    render_frame(&cam, &ext, &mode);
    /* one latch: the transforms, the poses the renderer draws for, the poses submitted */
    vgm_eye_transforms(&cam, &ext, origin, s->cfg.game.max_head_offset_m, head,
                       eye, vf, s->cfg.game.units_per_metre, &g_eye_tf);
    g_eye_tf.display_time = t;
    memset(&g_rpose, 0, sizeof(g_rpose));
    g_rpose.game_frame = ext.game_frame;
    g_rpose.present_mode = mode;
    g_rpose.display_time = t;
    g_rpose.game_head = ext.head_pose;
    g_rpose.head = vgm_clamp_head(origin, head, s->cfg.game.max_head_offset_m);
    for (i = 0; i < VR_EYE_COUNT; i++) { g_rpose.eye_pose[i] = g_eye_tf.pose[i]; g_rpose.eye_fov[i] = vf[i]; }
    g_rpose.world_scale = s->cfg.game.units_per_metre;
    g_rpose.serial = ++g_rserial;
    g_rpose.valid = 1;
    latch_gun(s, t);                                    /* the weapon hand, in the same latch */

    for (i = 0; i < VR_EYE_COUNT; i++) {
        XrsSwapchain *sc = s->multiview ? &s->eye[0] : &s->eye[i];
        const uint32_t im = s->multiview ? idx[0] : idx[i];
        const uint32_t layer = s->multiview ? (uint32_t)i : 0u;
        out[i].image = (uint64_t)(uintptr_t)sc->images[im];
        out[i].view = (uint64_t)(uintptr_t)sc->layer_views[im][layer];
        out[i].width = sc->width;
        out[i].height = sc->height;
        out[i].format = (uint32_t)s->color_format;
        out[i].array_layer = layer;
    }
    g_array_view = s->multiview ? (uint64_t)(uintptr_t)s->eye[0].array_views[idx[0]] : 0;
    /* committed to the projection layer only when these images are released: exactly the drawn poses */
    for (i = 0; i < VR_EYE_COUNT; i++) {
        const VrPose *p = &g_rpose.eye_pose[i];
        g_pending_pose[i].orientation.x = p->orientation.x; g_pending_pose[i].orientation.y = p->orientation.y;
        g_pending_pose[i].orientation.z = p->orientation.z; g_pending_pose[i].orientation.w = p->orientation.w;
        g_pending_pose[i].position.x = p->position.x; g_pending_pose[i].position.y = p->position.y;
        g_pending_pose[i].position.z = p->position.z;
        g_pending_fov[i] = fov[i];
    }
    return 1;
}

void vr_release_eye_targets(void)
{
    XrShell *s = &g_xr;
    int i, n = s->multiview ? 1 : VR_EYE_COUNT, hud = 0;
    /* release and pose commit are one step for the XR thread, which builds the
     * projection layer and calls xrEndFrame under the same lock; the HUD drawn
     * for these images is published in the same step */
    pthread_mutex_lock(&s->queue_lock);
    for (i = 0; i < n; i++) xrs_swapchain_release(s, &s->eye[i]);
    if (s->hud_release_pending) {
        xrs_swapchain_release(s, &s->hud);
        s->hud_release_pending = 0;
        hud = 1;
    }
    for (i = 0; i < VR_EYE_COUNT; i++) { s->submit.pose[i] = g_pending_pose[i]; s->submit.fov[i] = g_pending_fov[i]; }
    s->submit.serial++;
    s->submit.valid = 1;
    s->submit.mode = VR_PRESENT_STEREO;
    s->submit.hud = hud;
    s->submit.latch_serial = g_rpose.serial;
    s->submit.game_frame = g_rpose.game_frame;
    pthread_mutex_unlock(&s->queue_lock);
    g_d_game_frame = g_rpose.game_frame;
    g_d_eye_ns = mono_ns();
    g_d_eye++;
}

const VrEyeTransforms *vr_eye_transforms(void) { return &g_eye_tf; }
uint64_t vr_eye_array_view(void) { return g_array_view; }

int vr_acquire_flat_target(VrEyeTarget *out)
{
    XrShell *s = &g_xr;
    uint32_t im;
    if (!vr_active() || !s->flat.handle) return 0;
    vrb_renderer_claim(s);
    if (!xrs_swapchain_acquire(s, &s->flat, &im)) return 0;
    out->image = (uint64_t)(uintptr_t)s->flat.images[im];
    out->view = (uint64_t)(uintptr_t)s->flat.layer_views[im][0];
    out->width = s->flat.width;
    out->height = s->flat.height;
    out->format = (uint32_t)s->color_format;
    out->array_layer = 0;
    return 1;
}

void vr_release_flat_target(void)
{
    XrShell *s = &g_xr;
    VrGameCamera cam;
    VrGameCameraExt ext;
    int mode;
    render_frame(&cam, &ext, &mode);
    if (mode <= VR_PRESENT_STEREO || mode > VR_PRESENT_FLAT_SCOPE) mode = VR_PRESENT_FLAT_MENU;
    pthread_mutex_lock(&s->queue_lock);
    xrs_swapchain_release(s, &s->flat);
    s->flat_released = 1;
    s->submit.mode = mode;         /* the flat image is what the renderer shows now */
    pthread_mutex_unlock(&s->queue_lock);
    g_d_flat_ns = mono_ns();
    g_d_flat++;
}

int vr_acquire_hud_target(VrEyeTarget *out)
{
    XrShell *s = &g_xr;
    uint32_t im;
    if (!vr_active() || !s->hud.handle) return 0;
    vrb_renderer_claim(s);
    if (!xrs_swapchain_acquire(s, &s->hud, &im)) return 0;
    out->image = (uint64_t)(uintptr_t)s->hud.images[im];
    out->view = (uint64_t)(uintptr_t)s->hud.layer_views[im][0];
    out->width = s->hud.width;
    out->height = s->hud.height;
    out->format = (uint32_t)s->color_format;
    out->array_layer = 0;
    return 1;
}

void vr_release_hud_target(void)
{
    XrShell *s = &g_xr;
    pthread_mutex_lock(&s->queue_lock);
    if (s->eye[0].held) s->hud_release_pending = 1;    /* published with the eye images */
    else xrs_swapchain_release(s, &s->hud);           /* no eye images: not shown */
    pthread_mutex_unlock(&s->queue_lock);
}

const VrHudLayout *vr_hud_layout(void)
{
    static VrHudLayout l;
    XrShell *s = &g_xr;
    l.distance_m = s->cfg.hud_distance_m > 0.0f ? s->cfg.hud_distance_m : 2.0f;
    l.width_m = s->cfg.hud_width_m > 0.0f ? s->cfg.hud_width_m : 2.4f;
    l.width = s->hud.width ? s->hud.width : 1280u;
    l.height = s->hud.height ? s->hud.height : 960u;
    return &l;
}

const VrVulkanFeatures *vr_vulkan_features(void) { return &g_xr.vk_features; }

const char *vr_cache_dir(void) { return g_xr.cache_dir[0] ? g_xr.cache_dir : "kgpu-cache"; }

/* ---- both-eye capture (vr_bridge.h) ---- */
void vr_capture_request(void) { __atomic_store_n(&g_cap_request, 1, __ATOMIC_RELEASE); }

int vr_capture_take(void)
{
    static int every_s = -1;
    static int64_t next_ns;
    if (every_s < 0) {
        const char *e = getenv("BLACKVR_CAPTURE_EVERY");
        every_s = e && *e ? atoi(e) : 0;
        if (every_s < 0) every_s = 0;
        if (every_s) XLOGI("capture: both eye images every %d s (BLACKVR_CAPTURE_EVERY) to %s", every_s, vr_capture_dir());
    }
    if (every_s) {
        const int64_t now = mono_ns();
        if (!next_ns) next_ns = now + (int64_t)every_s * 1000000000LL;
        else if (now >= next_ns) { next_ns = now + (int64_t)every_s * 1000000000LL; return 1; }
    }
    return __atomic_exchange_n(&g_cap_request, 0, __ATOMIC_ACQ_REL) != 0;
}

const char *vr_capture_dir(void)
{
    static char dir[600];
    if (!dir[0]) snprintf(dir, sizeof(dir), "%s/shots", g_xr.game_root[0] ? g_xr.game_root : ".");
    return dir;
}

void vr_register_render_thread(void)
{
    XrShell *s = &g_xr;
    if (s->has_thread_settings && s->xrSetAndroidApplicationThreadKHR && s->session)
        s->xrSetAndroidApplicationThreadKHR(s->session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, gettid());
}

void vr_diag_counters(VrDiagCounters *out)
{
    memset(out, 0, sizeof(*out));
    out->xr_frames = g_d_xr_frames;
    out->eye_releases = g_d_eye;
    out->flat_releases = g_d_flat;
    out->last_xr_frame_ns = g_d_xr_ns;
    out->last_eye_release_ns = g_d_eye_ns;
    out->last_flat_release_ns = g_d_flat_ns;
    out->last_game_frame = g_d_game_frame;
    out->present_mode = g_present_mode;
    out->session_running = g_xr.session_running;
    out->renderer_started = g_xr.renderer_started;
}
