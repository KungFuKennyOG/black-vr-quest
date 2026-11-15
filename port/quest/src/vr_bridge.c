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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include "xr_shell.h"
#include "vr_settings.h"
#include "vr_game_math.h"
#include "vr_gestures.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- the shell's log lines: logcat, and the game's log file (xr_shell.h XLOGI) ---------------------------- */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static VrLogSink g_log_sink;
static char g_log_early[65536];      /* lines before the file opened: "(hh:mm:ss.mmm) text\n" each */
static size_t g_log_early_n;

void xlog_line(int prio, const char *fmt, ...)
{
    char line[1024];
    VrLogSink sink = NULL;
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    __android_log_print(prio, XLOG_TAG, "%s", line);
    if (pthread_mutex_trylock(&g_log_lock) != 0) return;   /* never wait (a crash report may log from a handler) */
    if (g_log_sink) {
        sink = g_log_sink;
    } else if (g_log_early_n + (size_t)n + 32u < sizeof(g_log_early)) {
        struct timespec ts;
        struct tm tmv;
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tmv);
        g_log_early_n += (size_t)snprintf(g_log_early + g_log_early_n, sizeof(g_log_early) - g_log_early_n,
                                          "(%02d:%02d:%02d.%03ld) %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                                          ts.tv_nsec / 1000000, line);
    }
    pthread_mutex_unlock(&g_log_lock);
    if (sink) sink(XLOG_TAG, line, (size_t)n);
}

void vr_log_sink(VrLogSink sink)
{
    char *early = NULL;
    size_t n = 0;
    pthread_mutex_lock(&g_log_lock);
    g_log_sink = sink;
    if (sink && g_log_early_n && (early = malloc(g_log_early_n))) {
        memcpy(early, g_log_early, g_log_early_n);
        n = g_log_early_n;
    }
    g_log_early_n = 0;
    pthread_mutex_unlock(&g_log_lock);
    if (early) {
        size_t at = 0;
        while (at < n) {
            const char *nl = memchr(early + at, '\n', n - at);
            const size_t len = nl ? (size_t)(nl - (early + at)) : n - at;
            sink(XLOG_TAG, early + at, len);
            at += len + 1u;
        }
        free(early);
    }
}

/* ---- live state (XR thread writes) ---------------------------------------- */
static VrFrameState g_live;
static VrTouchState g_touch;
static VrInputRuntime g_in_rt;
static float g_trigger[2];
static int g_have_origin, g_recentre_pending;
static VrPose g_origin;
static float g_rumble[2];
static float g_pulse[2];          /* pending game haptic pulse (strength, already scaled), taken by the XR frame */
static uint32_t g_pulse_ms[2];

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
static uint32_t g_cam_count;                    /* cameras published: the game's rendered frames (VR controls' taps) */
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

/* ---- VR controls (XR thread; the game's state from the game thread) ------------------------------------------- */
static int (*volatile g_resolver)(int slot);
static VrGameInputState g_game_in;
static int64_t g_game_in_ns;
static volatile int g_aim_request = -1;
static VrSwing g_swing;
static VrDuck g_duck;
static VrCrouchSync g_csync;
static VrScope g_scope;
static VrTwoHand g_two;
static VrVec3 g_hand_vel[2];
static int g_hand_vel_ok[2];
static int g_duck_recal = 1, g_was_in_game;
static VrInputContext g_last_ctx;
static unsigned g_button_presses;                /* fresh button presses (vr_button_presses) */
static int g_scope_one_hand;                     /* frames the 3D scope has been held one-handed */
static int g_gun_two_handed;                     /* the drawn gun takes the off hand (set by the renderer) */
static int g_hold_on;                            /* the pistol's look-only hold: grip held, hands close */
static int g_two_hand_on;                       /* the two-handed aim of the last input step (latch_gun) */

void vr_set_action_resolver(int (*resolve)(int slot)) { g_resolver = resolve; }

void vr_publish_game_input(const VrGameInputState *st)
{
    static VrGameInputState prev;
    static int trace = -1, have;
    if (trace < 0) { const char *e = getenv("BLACKVR_INPUT_TRACE"); trace = e && *e && strcmp(e, "0"); }
    pthread_mutex_lock(&g_lock);
    if (st) g_game_in = *st;
    else memset(&g_game_in, 0, sizeof(g_game_in));
    g_game_in_ns = mono_ns();
    pthread_mutex_unlock(&g_lock);
    if (!st) return;
    /* the game's side of the VR controls, on change (device logs tune the gestures against these) */
    if (!have || st->crouched != prev.crouched || st->crouch_toggle != prev.crouch_toggle || st->weapon != prev.weapon ||
        st->aiming != prev.aiming || st->scoped != prev.scoped || memcmp(st->inventory, prev.inventory, 2))
        XLOGI("game: crouch %d (toggle mode %d), weapon %08X (slot %u of %u) scoped %d (zoom %.2f), aiming %d",
              st->crouched, st->crouch_toggle, st->weapon, st->inventory[1], st->inventory[0], st->scoped, st->max_zoom,
              st->aiming);
    if (trace && have && (memcmp(st->intent, prev.intent, sizeof(st->intent)) || st->weapon_state != prev.weapon_state ||
                          st->clip != prev.clip || memcmp(st->latch, prev.latch, sizeof(st->latch)))) {
        char hex[3 * 16 + 1], hex2[3 * 16 + 1];
        int i;
        for (i = 0; i < 16; i++) { snprintf(hex + 3 * i, 4, "%02X ", st->intent[i]); snprintf(hex2 + 3 * i, 4, "%02X ", st->latch[i]); }
        XLOGI("game trace: +100..10F %s| intents +40..4F %s| weapon state %u, magazine %d, zoom %.2f, aiming %d, crouch %d, spread "
              "scale %.3f max %.4f +%.4f over %.3f s (firing %.2f s)", hex2, hex, st->weapon_state, st->clip, st->zoom,
              st->aiming, st->crouched, st->spread_scale, st->max_spread, st->spread_extra, st->spread_time, st->fire_time);
    }
    prev = *st;
    have = 1;
}

int vr_game_aim_request(void) { return g_aim_request; }

void vrb_set_hand_velocity(const VrVec3 lin[2], const int valid[2])
{
    int h;
    pthread_mutex_lock(&g_lock);
    for (h = 0; h < 2; h++) { g_hand_vel[h] = lin[h]; g_hand_vel_ok[h] = valid[h]; }
    pthread_mutex_unlock(&g_lock);
}

static int resolve_slot(int slot)
{
    int (*r)(int) = g_resolver;
    return r ? r(slot) : -1;
}

/* the yaw of a pose's forward in stage space (radians, positive = left), 0 when it points straight up/down */
static int horizontal_yaw(VrQuat q, double *yaw)
{
    const VrVec3 f = vrm_q_rotate(q, vrm_v3(0, 0, -1));
    if (f.x * f.x + f.z * f.z < 0.04f) return 0;
    *yaw = atan2(-f.x, -f.z);
    return 1;
}

void vrb_set_input(XrShell *s, const VrTouchState *touch, const VrPose aim_in[2], const VrPose grip[2],
                   const int valid[2], double dt)
{
    int h, gun, off, swing = 0, body_ducked = 0, game_known, scope = 0;
    VrPose aim[2];
    VrGameInputState gin;
    VrInputContext ctx;
    VrActionState act;
    double move_rot = 0.0;
    /* gameplay for the controls: the mode the game hook chose, while the game has published a camera in the last
     * 3 s (the display's 250 ms freshness would flap the controls between gameplay and menus on a hitch) */
    const int mode = g_present_mode;
    int in_game;
    const VrGestureConfig *gc = &s->cfg.gest;
    pthread_mutex_lock(&g_lock);
    in_game = (mode == VR_PRESENT_STEREO || mode == VR_PRESENT_FLAT_SCOPE) && g_cam_time_ns &&
              mono_ns() - g_cam_time_ns < 3000000000LL;
    g_touch = *touch;
    gun = s->cfg.game.aim_hand & 1;
    off = gun ^ 1;
    for (h = 0; h < 2; h++) aim[h] = aim_in[h];
    gin = g_game_in;
    game_known = gin.valid && g_game_in_ns && mono_ns() - g_game_in_ns < 3000000000LL;   /* as in_game */

    /* two-handed aim: the gun hand's aim pose points at the off hand while its grip is held near the gun */
    {
        const int was = g_two.engaged;
        const float off_grip = gun ? touch->lgrip : touch->rgrip;
        const int gun_hands = __atomic_load_n(&g_gun_two_handed, __ATOMIC_RELAXED);   /* 0 none, 1 hold only, 2 aim */
        const VrPose two_aim = vrg_two_hand_update(&g_two, gc, valid[gun] && in_game, aim[gun], valid[off], aim[off].position,
                                       /* held: takes the gun at 50 %, lets go only under 25 % (a relaxed hold on a
                                        * foregrip hovers around one threshold, and dropped the grip every second) */
                                       (g_two.engaged ? off_grip > 0.15f : off_grip > 0.5f) &&
                                           gun_hands == 2, dt);   /* the pistol: the hold below, not this test */
        if (gun_hands == 2) aim[gun] = two_aim;          /* a hold only (the pistol's cupped hand): the look, not the aim */
        if (g_two.engaged != was)
            XLOGI("two-hand %s (hands %.2f m apart)", g_two.engaged ? "on" : "off", g_two.last_dist);
        g_two_hand_on = g_two.engaged && gun_hands == 2;
        /* the look-only hold (the pistol): the two-hand test wants the off hand ahead on the gun line; the pistol's
         * hold is under and behind the grip, so it is just the grip held with the hands close (25 cm, kept to 35) */
        {
            const int was_hold = g_hold_on;
            const VrVec3 dv = vrm_sub(aim[off].position, aim[gun].position);
            const float dist = vrm_len(dv);
            g_hold_on = gun_hands == 1 && valid[gun] && valid[off] && in_game &&
                        (g_hold_on ? off_grip > 0.15f && dist < 0.45f : off_grip > 0.5f && dist < 0.35f);
            if (g_hold_on != was_hold) XLOGI("hold %s (hands %.2f m apart)", g_hold_on ? "on" : "off", dist);
        }
    }
    for (h = 0; h < 2; h++) {
        g_live.hand[h].aim = aim[h];
        g_live.hand[h].grip = grip[h];
        g_live.hand[h].valid = valid[h];
    }
    g_trigger[0] = touch->lt;
    g_trigger[1] = touch->rt;

    /* locomotion: the move stick turned from the game's yaw (the aim) to the head's or the off hand's */
    if (in_game && g_have_origin && s->cfg.locomotion != VR_LOCO_AIM) {
        const VrPose head_rel = vgm_relative(g_origin, g_live.head);
        double ay = 0.0, ap, ry;
        int ok = 1;
        if (s->cfg.game.aim_mode == VR_AIM_HAND && valid[gun]) {
            const VrPose hand_rel = vgm_relative(g_origin, aim[gun]);
            vgm_dir_angles(vgm_aim_dir(1, head_rel, hand_rel, vrm_v3(0, 0, 0), s->cfg.game.aim_converge_m), &ay, &ap);
        } else {
            vgm_dir_angles(vgm_aim_dir(0, head_rel, head_rel, vrm_v3(0, 0, 0), 1.0f), &ay, &ap);
        }
        if (s->cfg.locomotion == VR_LOCO_OFFHAND) {
            const VrPose off_rel = vgm_relative(g_origin, aim[off]);
            double oy = 0.0;
            ok = valid[off] && horizontal_yaw(off_rel.orientation, &oy);
            ry = oy / VRM_DEG;
            if (!ok) {                                  /* off hand untracked or pointing down: the head */
                vgm_dir_angles(vgm_aim_dir(0, head_rel, head_rel, vrm_v3(0, 0, 0), 1.0f), &ry, &ap);
                ok = 1;
            }
        } else {
            vgm_dir_angles(vgm_aim_dir(0, head_rel, head_rel, vrm_v3(0, 0, 0), 1.0f), &ry, &ap);
        }
        if (ok) move_rot = vrm_wrap_deg(ry - ay);
    }

    /* gestures (gameplay only) */
    if (in_game && !g_was_in_game && !g_duck.calibrated) g_duck_recal = 1;
    if (in_game) {
        const VrVec3 head_pos = g_live.head.position;
        if (g_duck_recal && g_have_origin) {
            vrg_duck_calibrate(&g_duck, head_pos.y);
            g_duck_recal = 0;
            XLOGI("duck: standing head height %.2f m (crouch below %.2f, stand above %.2f)", head_pos.y,
                  head_pos.y >= 1.0f ? head_pos.y * gc->duck_ratio : head_pos.y - 1.65f * (1.0f - gc->duck_ratio),
                  head_pos.y >= 1.0f ? head_pos.y * (gc->duck_ratio + 0.08f) : head_pos.y - 1.65f * (0.92f - gc->duck_ratio));
        }
        {
            const int was = g_duck.ducked;
            body_ducked = vrg_duck_update(&g_duck, gc, g_have_origin && !g_duck_recal, head_pos.y, dt);
            if (body_ducked != was)
                XLOGI("duck: %s (head %.2f m, standing %.2f, thresholds %.2f / %.2f, game crouch %d)",
                      body_ducked ? "crouch" : "stand", g_duck.last_y, g_duck.standing, g_duck.threshold_lo,
                      g_duck.threshold_hi, game_known ? gin.crouched : -1);
        }
        swing = vrg_swing_update(&g_swing, gc, valid[gun], aim_in[gun].position, head_pos, g_hand_vel_ok[gun],
                                 g_hand_vel[gun], 0, vrm_v3(0, 0, 0), dt);
        if (swing) XLOGI("melee swing: %.2f m/s (threshold %.2f)", g_swing.last_speed, gc->swing_speed);
        {
            VrVec3 eye[2], right = vrm_q_rotate(g_live.head.orientation, vrm_v3(1, 0, 0));
            const VrVec3 fwd = vrm_q_rotate(g_live.head.orientation, vrm_v3(0, 0, -1));
            const float half_ipd = (g_live.ipd_m > 0.0f ? g_live.ipd_m : 0.064f) * 0.5f;
            const int was = g_scope.engaged;
            eye[0] = vrm_sub(head_pos, vrm_scale(right, half_ipd));
            eye[1] = vrm_add(head_pos, vrm_scale(right, half_ipd));
            scope = vrg_scope_update(&g_scope, gc, valid[gun], aim[gun], eye, fwd);
            /* the 3D scope view, as in DrBeef's ports: a two-handed gun looks through its scope only with both hands on */
            if (scope && s->cfg.game.scope_3d && __atomic_load_n(&g_gun_two_handed, __ATOMIC_RELAXED) == 2 &&
                !g_two.engaged) {
                if (!was) g_scope.engaged = 0;
                else if (++g_scope_one_hand > 30) g_scope.engaged = 0, g_scope_one_hand = 0;  /* let go for 30 frames */
                scope = g_scope.engaged;
            } else {
                g_scope_one_hand = 0;
            }
            if (scope != was)
                XLOGI("scope raise: %s (sight %.3f m from the eye, %.1f deg off the view, weapon scoped %d, aiming %d)",
                      scope ? "up" : "down", g_scope.last_dist, g_scope.last_angle_deg, game_known ? gin.scoped : -1,
                      game_known ? gin.aiming : -1);
        }
    } else {
        g_scope.engaged = 0;
    }
    g_was_in_game = in_game;

    /* the aim intent the game hook writes: scoped weapons only; scope_raise off -> the gun hand's grip holds it */
    if (!in_game || !game_known) g_aim_request = -1;
    else if (!gin.scoped) g_aim_request = 0;
    else if (gc->scope_raise) g_aim_request = scope;
    else g_aim_request = (gun ? touch->rgrip : touch->lgrip) > 0.6f;

    {
        /* any button pressed (a fresh press), for the movie skip (vr_button_presses) */
        static unsigned prev;
        const unsigned now = (touch->a ? 1u : 0u) | (touch->b ? 2u : 0u) | (touch->x ? 4u : 0u) | (touch->y ? 8u : 0u) |
                             (touch->menu ? 16u : 0u) | (touch->rt > 0.5f ? 32u : 0u) | (touch->lt > 0.5f ? 64u : 0u);
        if (now & ~prev) __atomic_add_fetch(&g_button_presses, 1u, __ATOMIC_RELAXED);
        if ((now ^ prev) & 15u) {
            /* the face buttons as the headset reports them (rare: one line per press and release) */
            static const char *const k_face[4] = { "A", "B", "X", "Y" };
            int k;
            for (k = 0; k < 4; k++)
                if ((now ^ prev) & (1u << k))
                    XLOGI("button %s %s (in game %d, two-hand %d, left grip %.2f)", k_face[k], now & (1u << k) ? "down" : "up",
                          in_game, g_two.engaged, touch->lgrip);
        }
        prev = now;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.in_game = in_game;
    ctx.move_rotate_deg = move_rot;
    ctx.melee_swing = swing;
    ctx.scope_up = in_game && game_known && gin.scoped && gin.aiming;
    /* the 3D scope view: a third of the walk, no stick turning (RTCWQuest's comfort rules) */
    ctx.scope_view = in_game && vr_scope_view() > 1.0f;
    /* taps last 2 of the game's rendered frames (each has >= 1 input update; pause and menus read once a frame) */
    ctx.ticks_valid = game_known;
    ctx.game_ticks = g_cam_count;
    {
        const int before = g_csync.presses;
        ctx.crouch_hold = in_game && gc->duck_crouch &&
            vrg_crouch_sync(&g_csync, body_ducked, game_known ? gin.crouched : -1,
                            game_known ? gin.crouch_toggle : -1, game_known, g_cam_count, dt);
        if (g_csync.presses != before)
            XLOGI("duck: crouch pressed (body %s, game crouch %d, try %d)", body_ducked ? "ducked" : "standing",
                  game_known ? gin.crouched : -1, g_csync.tries);
    }
    vr_map_actions(&s->cfg.input, &g_in_rt, touch, &ctx, dt, &act);
    if (act.laser_toggle) vrsm_toggle(VRS_LASER, "laser sight");
    if (g_in_rt.fired_mask) {
        char line[160];
        int i, n = 0;
        line[0] = 0;
        for (i = 0; i < VRA_COUNT && n < (int)sizeof(line) - 16; i++)
            if (g_in_rt.fired_mask & VRA_BIT(i)) n += snprintf(line + n, sizeof(line) - (size_t)n, " %s->%d", k_vra_name[i],
                                                             resolve_slot(k_vra_slot[i]));
        XLOGI("action tap:%s", line);
    }
    /* gameplay needs the game's map; until its resolver is installed, a neutral pad */
    if (act.in_game && !g_resolver) memset(&g_live.pad, 0, sizeof(g_live.pad));
    else vr_actions_to_pad(&act, resolve_slot, &g_live.pad);
    /* experiments (env BLACKVR_DEBUG_GRIPS=1, gameplay): right grip = the game's own Aim control (no VR scope
     * logic), left grip = Black, both = White; the VR scheme leaves these unmapped */
    if (act.in_game && g_resolver) {
        static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("BLACKVR_DEBUG_GRIPS"); dbg = e && *e && strcmp(e, "0"); }
        if (dbg) {
            const int rg = touch->rgrip > 0.6f, lg = touch->lgrip > 0.6f;
            if (rg && lg) vr_pad_press(&g_live.pad, 21, 1.0f);
            else if (rg) { vr_pad_press(&g_live.pad, resolve_slot(16), 1.0f); g_aim_request = -1; }
            else if (lg) vr_pad_press(&g_live.pad, 20, 1.0f);
        }
    }
    g_last_ctx = ctx;
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
            g_duck_recal = 1;                    /* the standing height is taken again */
            XLOGI("recentred at %.2f %.2f %.2f yaw %.1f", h.position.x, h.position.y, h.position.z,
                  vrm_q_yaw(h.orientation) / VRM_DEG);
        }
        pthread_mutex_unlock(&g_lock);
    }
}

void vrb_recentre_from(const VrPose *head)
{
    pthread_mutex_lock(&g_lock);
    if (head) { g_origin = vgm_recentre_origin(*head); g_have_origin = 1; g_duck_recal = 1; }
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

void vrb_head_now(VrPose *head)
{
    pthread_mutex_lock(&g_lock);
    *head = g_live.head;
    pthread_mutex_unlock(&g_lock);
}

/* The settings panel opens only here: the controls' menu mapping (vrb_set_input's in_game is 0: paused, the front
 * end, loading, no game), and not a cutscene (FLAT_CINEMA). */
int vrb_menu_context(void)
{
    const int mode = g_present_mode;
    int in_game;
    pthread_mutex_lock(&g_lock);
    in_game = (mode == VR_PRESENT_STEREO || mode == VR_PRESENT_FLAT_SCOPE) && g_cam_time_ns &&
              mono_ns() - g_cam_time_ns < 3000000000LL;
    pthread_mutex_unlock(&g_lock);
    return !in_game && mode != VR_PRESENT_FLAT_CINEMA;
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

void vr_game_haptic(int hand, float strength, uint32_t ms)
{
    const float k = g_xr.cfg.haptic_fire;
    const int h = hand & 1;
    float a = strength * k;
    if (!(a > 0.0f)) return;
    if (a > 1.0f) a = 1.0f;
    if (ms < 5u) ms = 5u;
    if (ms > 200u) ms = 200u;
    pthread_mutex_lock(&g_lock);
    if (a > g_pulse[h]) g_pulse[h] = a;
    if (ms > g_pulse_ms[h]) g_pulse_ms[h] = ms;
    pthread_mutex_unlock(&g_lock);
}

/* XR thread: the pending pulse of a hand (0 when none), cleared. */
void vrb_take_pulse(int hand, float *strength, uint32_t *ms)
{
    const int h = hand & 1;
    pthread_mutex_lock(&g_lock);
    *strength = g_pulse[h];
    *ms = g_pulse_ms[h];
    g_pulse[h] = 0.0f;
    g_pulse_ms[h] = 0;
    pthread_mutex_unlock(&g_lock);
}

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
    g_cam_count++;
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

static float g_grip_corr[3];

void vr_set_grip_correction(const float c[3])
{
    pthread_mutex_lock(&g_lock);
    memcpy(g_grip_corr, c, sizeof(g_grip_corr));
    pthread_mutex_unlock(&g_lock);
}

void vr_grip_correction(float c[3])
{
    pthread_mutex_lock(&g_lock);
    memcpy(c, g_grip_corr, sizeof(g_grip_corr));
    pthread_mutex_unlock(&g_lock);
}

static float g_grip_lock[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static int64_t g_last_shot_ns;

void vr_set_grip_lock(const float l[16])
{
    pthread_mutex_lock(&g_lock);
    memcpy(g_grip_lock, l, sizeof(g_grip_lock));
    pthread_mutex_unlock(&g_lock);
}

void vr_grip_lock(float l[16])
{
    pthread_mutex_lock(&g_lock);
    memcpy(l, g_grip_lock, sizeof(g_grip_lock));
    pthread_mutex_unlock(&g_lock);
}

void vr_set_gun_two_handed(int two) { __atomic_store_n(&g_gun_two_handed, two < 0 ? 0 : two > 2 ? 2 : two, __ATOMIC_RELAXED); }

unsigned vr_button_presses(void) { return __atomic_load_n(&g_button_presses, __ATOMIC_RELAXED); }
static uint32_t g_scope_view_bits;
void vr_set_scope_view(float zoom)
{
    uint32_t b;
    if (!(zoom > 1.0f)) zoom = 0.0f;
    memcpy(&b, &zoom, sizeof(b));
    __atomic_store_n(&g_scope_view_bits, b, __ATOMIC_RELAXED);
}
float vr_scope_view(void)
{
    const uint32_t b = __atomic_load_n(&g_scope_view_bits, __ATOMIC_RELAXED);
    float z;
    memcpy(&z, &b, sizeof(z));
    return z;
}
static float g_laser_p[3], g_laser_d[3], g_laser_len;
static int64_t g_laser_ns;
void vr_set_gun_laser(const float muzzle[3], const float dir[3], float length_m, int valid)
{
    pthread_mutex_lock(&g_lock);
    if (valid) {
        memcpy(g_laser_p, muzzle, sizeof(g_laser_p));
        memcpy(g_laser_d, dir, sizeof(g_laser_d));
        g_laser_len = length_m;
        g_laser_ns = mono_ns();
    } else {
        g_laser_ns = 0;
    }
    pthread_mutex_unlock(&g_lock);
}
int vr_gun_laser(float muzzle[3], float dir[3], float *length_m)
{
    int ok;
    pthread_mutex_lock(&g_lock);
    memcpy(muzzle, g_laser_p, sizeof(g_laser_p));
    memcpy(dir, g_laser_d, sizeof(g_laser_d));
    *length_m = g_laser_len;
    {
        /* fresh: the game is in gameplay (BLACKVR_LASER_FRESH_MS: the x86 harness runs at ~1 frame/s) */
        static long long fresh_ns = -1;
        if (fresh_ns < 0) {
            const char *e = getenv("BLACKVR_LASER_FRESH_MS");
            fresh_ns = (e && atoll(e) > 0 ? atoll(e) : 250LL) * 1000000LL;
        }
        ok = g_laser_ns && mono_ns() - g_laser_ns < fresh_ns;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
static float g_gun_barrel[3];
static int g_gun_barrel_ok;
void vr_set_gun_barrel(const float d[3], int valid)
{
    pthread_mutex_lock(&g_lock);
    if (valid) memcpy(g_gun_barrel, d, sizeof(g_gun_barrel));
    g_gun_barrel_ok = valid;
    pthread_mutex_unlock(&g_lock);
}
int vr_gun_barrel(float d[3])
{
    int ok;
    pthread_mutex_lock(&g_lock);
    memcpy(d, g_gun_barrel, sizeof(g_gun_barrel));
    ok = g_gun_barrel_ok;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void vr_note_shot(void) { __atomic_store_n(&g_last_shot_ns, mono_ns(), __ATOMIC_RELAXED); }
int64_t vr_last_shot_ns(void) { return __atomic_load_n(&g_last_shot_ns, __ATOMIC_RELAXED); }
float vr_recoil_scale(void) { return g_xr.cfg.recoil; }

void vr_arms_grip_config(int *hand_auto, float wrist_offset_m[3])
{
    *hand_auto = g_xr.cfg.hand_auto;
    memcpy(wrist_offset_m, g_xr.cfg.wrist_offset_m, 3 * sizeof(float));
}

void vr_arms_state(int *cut, int *mode, int *engaged)
{
    *cut = g_xr.cfg.arms_cut;
    *mode = g_xr.cfg.left_hand;
    *engaged = __atomic_load_n(&g_two.engaged, __ATOMIC_RELAXED) != 0 || __atomic_load_n(&g_hold_on, __ATOMIC_RELAXED) != 0;
}

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
    {
        /* the off hand at the same display time (first-person arms) */
        XrSpaceLocation lo = { XR_TYPE_SPACE_LOCATION };
        const int off = (in.hand_index & 1) ^ 1;
        if (s->aim_space[off] && XR_SUCCEEDED(xrLocateSpace(s->aim_space[off], s->world_space, t, &lo)) &&
            (lo.locationFlags & need) == need) {
            g_rgun.off = pose_from_xr(&lo.pose);
            g_rgun.off_valid = 1;
        }
    }
    {
        /* two-handed aim: the weapon follows the gun hand -> off hand line at display time too */
        int two;
        float blend;
        const int off = (in.hand_index & 1) ^ 1;
        XrSpaceLocation lo = { XR_TYPE_SPACE_LOCATION };
        pthread_mutex_lock(&g_lock);
        two = g_two_hand_on;
        blend = g_two.blend;
        pthread_mutex_unlock(&g_lock);
        if (two && s->aim_space[off] && XR_SUCCEEDED(xrLocateSpace(s->aim_space[off], s->world_space, t, &lo)) &&
            (lo.locationFlags & need) == need) {
            const VrPose offp = pose_from_xr(&lo.pose);
            VrPose p = vrg_two_hand_pose(g_rgun.hand, offp.position);
            if (blend < 1.0f) p.orientation = vrg_nlerp(g_rgun.hand.orientation, p.orientation, blend);
            g_rgun.hand = p;
        }
    }
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
    l.pitch_deg = s->cfg.hud_pitch_deg >= -45.0f && s->cfg.hud_pitch_deg <= 30.0f ? s->cfg.hud_pitch_deg : 0.0f;
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
