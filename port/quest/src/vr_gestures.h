/*
 * vr_gestures.h - the body gestures of the VR controls, as pure state machines over poses and time
 * (unit-tested in tests/xr_shell/test_input.c). Nothing here reads OpenXR or game memory: the shell
 * (vr_bridge.c) feeds stage-space poses, velocities and the game's own state, and turns the results
 * into game actions (vr_input_map.h).
 *
 *   swing      a fast swing of the gun hand -> one Melee, with a cooldown. Speed is the hand's linear
 *              speed RELATIVE TO THE HEAD (walking, leaning and turning in place move both), from the
 *              runtime's XrSpaceVelocity when valid, else from pose deltas; it must stay above the
 *              threshold for a minimum time and number of samples. Tracking jumps are rejected.
 *   duck       head height against the standing height calibrated at game start / recentre (and raised
 *              slowly when the player stands taller): crouch below duck_ratio x standing, stand above
 *              (duck_ratio + 0.08) x standing. Without a floor-referenced space (standing height < 1 m)
 *              the same thresholds apply to an assumed 1.65 m standing height below the calibrated one.
 *   crouch sync  what to press so the game's crouch follows the body: in Toggle Crouch mode one press on
 *              each duck/stand edge when the game's state (read from game memory) differs, re-tried if
 *              the game did not follow; in Hold mode the crouch control is held while ducked.
 *   scope      the gun's rear sight within scope_raise_distance of an eye and the gun pointing along the
 *              view -> engaged; disengaged beyond 1.5x the distance or 40 degrees (hysteresis). With free_aim
 *              (the 3D scope of a two-handed gun) only the distance counts, as in DrBeef's ports.
 *   scope aim  what the 3D scope looks and shoots along (port/notes/scope.md): a virtual stock, the line from
 *              the head to the off hand, turned from where the head faced when the scope came up.
 *   two hand   the off hand's grip held near the gun hand (or near a point 30 cm ahead of it along the
 *              gun) engages; the aim then points from the gun hand to the off hand, with the gun hand's
 *              roll, blended in over 0.1 s; only letting go of the grip (or the off hand going behind the gun hand)
 *              disengages.
 */
#ifndef VR_GESTURES_H
#define VR_GESTURES_H

#include <math.h>
#include <string.h>
#include "vr_math.h"

/* ---- config ------------------------------------------------------------------------------------------------- */
typedef struct {
    int   melee_swing;           /* on/off */
    float swing_speed;           /* m/s, 2.5 */
    float swing_min_s;           /* seconds above the speed, 0.035 */
    int   swing_min_samples;     /* samples above the speed, 2 */
    float swing_cooldown_s;      /* 0.6 */
    float swing_glitch_speed;    /* m/s above which a sample is a tracking jump, 15 */
    int   duck_crouch;           /* on/off */
    float duck_ratio;            /* 0.80 (stand back up above duck_ratio + 0.08) */
    int   scope_raise;           /* on/off */
    float scope_distance;        /* m, 0.15 */
    float scope_lift_m;          /* m, 0.12: the off hand sits this far under the scope's line of sight */
    float scope_smooth_s;        /* s, 0.05: the low-pass on the scope's aim (hand tremor is magnified with the view) */
    int   two_hand;              /* on/off */
    float two_hand_engage_m;     /* 0.60, within 30 deg of the gun's ray (0.20 caught a hand on a handguard only now and then) */
} VrGestureConfig;

static inline void vrg_default_config(VrGestureConfig *c)
{
    c->melee_swing = 1;
    c->swing_speed = 2.5f;
    c->swing_min_s = 0.035f;
    c->swing_min_samples = 2;
    c->swing_cooldown_s = 0.6f;
    c->swing_glitch_speed = 15.0f;
    c->duck_crouch = 1;
    c->duck_ratio = 0.80f;
    c->scope_raise = 1;
    c->scope_distance = 0.15f;
    c->scope_lift_m = 0.12f;
    c->scope_smooth_s = 0.05f;
    c->two_hand = 1;
    c->two_hand_engage_m = 0.60f;
}

/* ---- swing -> melee ---------------------------------------------------------------------------------------- */
typedef struct {
    int    have_prev;
    VrVec3 prev_hand, prev_head; /* stage positions of the last sample */
    double above_s;              /* time continuously above the threshold */
    int    above_n;
    double cooldown_s;
    float  peak;                 /* fastest speed of the current run (logged) */
    float  last_speed;           /* the trigger's measured speed (for the log) */
} VrSwing;

/* One sample. hand/head: stage positions; vel_*: runtime linear velocities (m/s) when *_vel_ok (a velocity
 * missing on one side is taken from that side's pose delta). Pose deltas need dt <= 0.25 s; runtime velocities
 * do not depend on dt. Returns 1 when a swing completes (Melee). */
static inline int vrg_swing_update(VrSwing *s, const VrGestureConfig *c, int tracked, VrVec3 hand, VrVec3 head,
                                   int hand_vel_ok, VrVec3 hand_vel, int head_vel_ok, VrVec3 head_vel, double dt)
{
    const int deltas = s->have_prev && dt > 0.0 && dt <= 0.25;
    VrVec3 vh, vd;
    float speed;
    int fired = 0;
    if (s->cooldown_s > 0.0) s->cooldown_s -= dt;
    if (!c->melee_swing || !tracked || !(dt > 0.0) || (!deltas && !hand_vel_ok)) {
        s->have_prev = tracked && dt <= 0.25;
        s->prev_hand = hand; s->prev_head = head;
        s->above_s = 0.0; s->above_n = 0; s->peak = 0.0f;
        return 0;
    }
    vh = hand_vel_ok ? hand_vel : vrm_scale(vrm_sub(hand, s->prev_hand), (float)(1.0 / dt));
    vd = head_vel_ok ? head_vel : deltas ? vrm_scale(vrm_sub(head, s->prev_head), (float)(1.0 / dt)) : vrm_v3(0, 0, 0);
    s->prev_hand = hand; s->prev_head = head;
    s->have_prev = 1;
    speed = vrm_len(vrm_sub(vh, vd));            /* the hand relative to the head: walking and leaning cancel */
    if (!isfinite(speed) || speed > c->swing_glitch_speed) {       /* a tracking jump, not a swing */
        s->above_s = 0.0; s->above_n = 0; s->peak = 0.0f;
        return 0;
    }
    if (speed >= c->swing_speed) {
        s->above_s += dt < 0.1 ? dt : 0.1;
        s->above_n++;
        if (speed > s->peak) s->peak = speed;
        if (s->cooldown_s <= 0.0 && s->above_s >= c->swing_min_s && s->above_n >= c->swing_min_samples) {
            fired = 1;
            s->last_speed = s->peak;
            s->cooldown_s = c->swing_cooldown_s;
            s->above_s = -1e9;                      /* one Melee per run: wait for the speed to drop */
        }
    } else {
        s->above_s = 0.0; s->above_n = 0; s->peak = 0.0f;
    }
    return fired;
}

/* ---- duck -> crouch ---------------------------------------------------------------------------------------- */
typedef struct {
    int    calibrated;
    float  standing;             /* standing head height (stage y) */
    int    ducked;
    float  last_y, threshold_lo, threshold_hi;
} VrDuck;

static inline void vrg_duck_calibrate(VrDuck *d, float head_y)
{
    d->calibrated = isfinite(head_y);
    d->standing = head_y;
    d->ducked = 0;
}

static inline void vrg_duck_thresholds(const VrDuck *d, const VrGestureConfig *c, float *lo, float *hi)
{
    if (d->standing >= 1.0f) {
        *lo = d->standing * c->duck_ratio;
        *hi = d->standing * (c->duck_ratio + 0.08f);
    } else {                                         /* no floor: an assumed 1.65 m body below the calibration */
        *lo = d->standing - 1.65f * (1.0f - c->duck_ratio);
        *hi = d->standing - 1.65f * (1.0f - c->duck_ratio - 0.08f);
    }
}

/* Returns the body's crouch (1 ducked, 0 standing). The standing height rises slowly (2 s) toward a taller
 * stance, so a calibration taken while bent over corrects itself; it never sinks. */
static inline int vrg_duck_update(VrDuck *d, const VrGestureConfig *c, int tracked, float head_y, double dt)
{
    float lo, hi;
    if (!c->duck_crouch || !tracked || !isfinite(head_y)) return d->ducked = 0;
    if (!d->calibrated) vrg_duck_calibrate(d, head_y);
    if (head_y > d->standing && dt > 0.0) {
        const double k = dt / 2.0 < 1.0 ? dt / 2.0 : 1.0;
        d->standing += (float)((head_y - d->standing) * k);
    }
    vrg_duck_thresholds(d, c, &lo, &hi);
    d->threshold_lo = lo; d->threshold_hi = hi; d->last_y = head_y;
    if (!d->ducked && head_y < lo) d->ducked = 1;
    else if (d->ducked && head_y > hi) d->ducked = 0;
    return d->ducked;
}

/* ---- crouch sync (body -> the game's crouch) ----------------------------------------------------------------
 * game_crouched: the game's crouch state, -1 unknown. toggle_mode: the profile's Toggle Crouch (1), hold (0),
 * -1 unknown (treated as toggle). Returns 1 while the crouch control is to be held this frame. */
typedef struct {
    int    have, want;           /* the body's last state */
    int    need;                 /* a press is owed for the last body edge */
    double press_s;              /* remaining press time */
    double gap_s;                /* release time after a press */
    double verify_s;             /* waiting for the game to follow a press */
    uint32_t tick0;              /* the game's input-update count at the press / its release (+1; 0 none) */
    int    tries;
    int    presses;              /* total presses made (diagnostics / tests) */
} VrCrouchSync;

#define VRG_CROUCH_PRESS_S 0.12
#define VRG_CROUCH_VERIFY_S 0.6
#define VRG_CROUCH_TRIES 3
#define VRG_CROUCH_TICKS 2u      /* a press also lasts this many game input updates, the verify window 3 x that */

/* Only body EDGES act: a crouch toggled by hand (stick click) while standing is left alone until the body
 * itself ducks or stands up. With the game's state known, a press the game did not follow (mid-animation,
 * a menu frame) is repeated up to 3 times; with it unknown, one press per edge. ticks: the game's input-update
 * count (ticks_valid 0: time only), so a slow game frame neither misses a press nor fails its check early. */
static inline int vrg_crouch_sync(VrCrouchSync *s, int body_ducked, int game_crouched, int toggle_mode,
                                  int ticks_valid, uint32_t ticks, double dt)
{
    const uint32_t since = s->tick0 ? ticks - (s->tick0 - 1u) : 0xFFFFFFFFu;
    if (!s->have) { s->have = 1; s->want = body_ducked; s->need = body_ducked; s->tries = 0; }
    else if (body_ducked != s->want) { s->want = body_ducked; s->need = 1; s->tries = 0; s->verify_s = 0.0; }
    if (toggle_mode == 0) {                           /* hold mode: held while ducked */
        s->need = 0; s->press_s = s->gap_s = s->verify_s = 0.0; s->tick0 = 0;
        return body_ducked;
    }
    if (s->press_s > 0.0) {
        s->press_s -= dt;
        if (s->press_s > 0.0 || (ticks_valid && since < VRG_CROUCH_TICKS)) { if (s->press_s < 0.0) s->press_s = 1e-9; return 1; }
        s->press_s = 0.0;
        s->gap_s = 0.1; s->verify_s = VRG_CROUCH_VERIFY_S;
        s->tick0 = ticks + 1u;
        return 0;
    }
    if (s->gap_s > 0.0) s->gap_s -= dt;
    if (s->verify_s > 0.0) {
        s->verify_s -= dt;
        if (game_crouched >= 0 && game_crouched == s->want) { s->need = 0; s->verify_s = 0.0; }
        else if (s->verify_s > 0.0 || (ticks_valid && since < 3u * VRG_CROUCH_TICKS)) { if (s->verify_s <= 0.0) s->verify_s = 1e-9; return 0; }
        else { s->verify_s = 0.0; s->need = game_crouched >= 0 && s->tries < VRG_CROUCH_TRIES; }   /* not followed */
    }
    if (!s->need || s->gap_s > 0.0) return 0;
    if (game_crouched >= 0 && game_crouched == s->want) { s->need = 0; return 0; }   /* already there */
    s->tries++;
    s->presses++;
    if (game_crouched < 0) s->need = 0;               /* unknown state: one press per edge */
    s->press_s = VRG_CROUCH_PRESS_S - dt;
    if (s->press_s <= 0.0) s->press_s = 1e-9;
    s->tick0 = ticks + 1u;
    return 1;
}

/* ---- scope raise ------------------------------------------------------------------------------------------- */
typedef struct {
    int   engaged;
    float last_dist, last_angle_deg;
    int   free_aim;              /* set by the caller: the distance alone decides (no angle to the view) */
} VrScope;

/* The rear sight of the gun: a little above and behind the aim pose's origin (the controller's tip). */
static inline VrVec3 vrg_sight_point(VrPose aim)
{
    return vrm_pose_apply(aim, vrm_v3(0.0f, 0.05f, 0.06f));
}

/* eye[2]: stage eye positions; head_fwd: the view's forward. Returns 1 while engaged. */
static inline int vrg_scope_update(VrScope *s, const VrGestureConfig *c, int tracked, VrPose aim, const VrVec3 eye[2],
                                   VrVec3 head_fwd)
{
    const VrVec3 sight = vrg_sight_point(aim);
    VrVec3 gf = vrm_q_rotate(aim.orientation, vrm_v3(0, 0, -1)), hf = head_fwd;
    float d0, d1, d, cosang;
    if (!c->scope_raise || !tracked || !vrm_normalize(&gf) || !vrm_normalize(&hf)) return s->engaged = 0;
    d0 = vrm_len(vrm_sub(sight, eye[0]));
    d1 = vrm_len(vrm_sub(sight, eye[1]));
    d = d0 < d1 ? d0 : d1;
    cosang = vrm_dot(gf, hf);
    s->last_dist = d;
    s->last_angle_deg = (float)(acos(cosang > 1.0f ? 1.0f : cosang < -1.0f ? -1.0f : cosang) / VRM_DEG);
    if (!s->engaged) {
        if (d <= c->scope_distance && (s->free_aim || s->last_angle_deg <= 25.0f)) s->engaged = 1;
    } else if (d > (s->free_aim ? fmaxf(c->scope_distance * 1.5f, 0.25f) : c->scope_distance * 1.5f) ||
               (!s->free_aim && s->last_angle_deg > 40.0f)) {
        /* with the stock (free_aim) the cheek may leave the gun hand 25 cm (DrBeef: 0.25-0.26 m to the headset) */
        s->engaged = 0;
    }
    return s->engaged;
}

/* ---- two-handed aim ---------------------------------------------------------------------------------------- */
/* An orientation (forward -z) with forward f and the up vector closest to `up` (the gun hand's, for its roll). */
static inline VrQuat vrg_look_rotation(VrVec3 f, VrVec3 up)
{
    VrVec3 X, Y, Z;
    float tr, s;
    VrQuat q;
    if (!vrm_normalize(&f)) return vrm_q_identity();
    Y = vrm_sub(up, vrm_scale(f, vrm_dot(up, f)));
    if (!vrm_normalize(&Y)) {                        /* up along the aim: any perpendicular */
        Y = fabsf(f.y) < 0.9f ? vrm_v3(0, 1, 0) : vrm_v3(1, 0, 0);
        Y = vrm_sub(Y, vrm_scale(f, vrm_dot(Y, f)));
        vrm_normalize(&Y);
    }
    Z = vrm_scale(f, -1.0f);
    X = vrm_cross(Y, Z);
    /* rotation matrix columns X Y Z -> quaternion */
    tr = X.x + Y.y + Z.z;
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q = vrm_q((Y.z - Z.y) / s, (Z.x - X.z) / s, (X.y - Y.x) / s, 0.25f * s);
    } else if (X.x > Y.y && X.x > Z.z) {
        s = sqrtf(1.0f + X.x - Y.y - Z.z) * 2.0f;
        q = vrm_q(0.25f * s, (Y.x + X.y) / s, (Z.x + X.z) / s, (Y.z - Z.y) / s);
    } else if (Y.y > Z.z) {
        s = sqrtf(1.0f + Y.y - X.x - Z.z) * 2.0f;
        q = vrm_q((Y.x + X.y) / s, 0.25f * s, (Z.y + Y.z) / s, (Z.x - X.z) / s);
    } else {
        s = sqrtf(1.0f + Z.z - X.x - Y.y) * 2.0f;
        q = vrm_q((Z.x + X.z) / s, (Z.y + Y.z) / s, 0.25f * s, (X.y - Y.x) / s);
    }
    return vrm_q_normalize(q);
}

static inline VrQuat vrg_nlerp(VrQuat a, VrQuat b, float t)
{
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w, s = d < 0.0f ? -1.0f : 1.0f;
    return vrm_q_normalize(vrm_q(a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t,
                                 a.z + (s * b.z - a.z) * t, a.w + (s * b.w - a.w) * t));
}

/* The two-handed aim pose: at the gun hand, pointing at the off hand, the gun hand's roll kept. */
static inline VrPose vrg_two_hand_pose(VrPose gun, VrVec3 off_pos)
{
    VrPose p = gun;
    const VrVec3 up = vrm_q_rotate(gun.orientation, vrm_v3(0, 1, 0));
    p.orientation = vrg_look_rotation(vrm_sub(off_pos, gun.position), up);
    return p;
}

typedef struct {
    int   engaged;
    float blend;                 /* 0 one-handed .. 1 two-handed */
    float last_dist;
    int   miss;                  /* updates in a row the hold's inputs dropped while engaged (held through 3) */
    VrVec3 last_off;             /* the off hand's last position while engaged */
} VrTwoHand;

/* Updates the engagement from the off hand's grip; returns the aim pose to use for the gun hand. */
static inline VrPose vrg_two_hand_update(VrTwoHand *s, const VrGestureConfig *c, int gun_ok, VrPose gun, int off_ok,
                                         VrVec3 off_pos, int off_grip, double dt)
{
    VrVec3 fwd = vrm_q_rotate(gun.orientation, vrm_v3(0, 0, -1)), d;
    float along, dist;
    if (!c->two_hand || !gun_ok || !off_ok || !off_grip) {
        /* a dropped input for a tick or two (a tracking blip, a grip reading) keeps the hold on the off hand's last
         * place: let go for one update, the aim snapped to the one-hand ray and back (the gun flicked) */
        if (s->engaged && c->two_hand && gun_ok && s->miss < 3) {
            s->miss++;
            return vrg_two_hand_pose(gun, s->last_off);
        }
        s->miss = 0;
        s->engaged = 0;
        s->blend = 0.0f;
        return gun;
    }
    d = vrm_sub(off_pos, gun.position);
    dist = vrm_len(d);
    along = vrm_dot(d, fwd);
    s->last_dist = dist;
    /* the off hand must be on the gun's line ahead (within 30 deg of the controller's ray): the aim turns toward it, so a
     * hand off to the side within reach turned the gun sideways (the shotgun held across the view on the headset) */
    if (!s->engaged) {
        if (dist <= c->two_hand_engage_m && dist > 0.08f && along > dist * 0.866f)
            s->engaged = 1;
    } else if (along < 0.0f || dist < 0.05f || dist > 1.2f) {
        /* held, it stays held wherever the hand goes (the gun follows it), unless it goes behind the gun hand's ray
         * (the aim would point back), onto the gun hand, or past any reach (tracking lost). It let go 45 deg off the
         * controller's ray, but a two-handed hold turns the gun wrist that far: Kenny, "ungrips when my left hand
         * strays a bit too far even though im still gripping", 2026-11-14 */
        s->engaged = 0;
    }
    if (!s->engaged) { s->blend = 0.0f; return gun; }
    s->miss = 0;
    s->last_off = off_pos;
    s->blend += (float)(dt / 0.1);
    if (s->blend > 1.0f) s->blend = 1.0f;
    {
        VrPose two = vrg_two_hand_pose(gun, off_pos);
        if (s->blend < 1.0f) two.orientation = vrg_nlerp(gun.orientation, two.orientation, s->blend);
        return two;
    }
}

/* ---- the scope's aim (the 3D scope view; port/notes/scope.md) ---------------------------------------------------
 * How DrBeef's ports aim a scoped rifle (RTCWQuest, JKXR: read for the method, none of their code is used):
 *   - a virtual stock: the aim is the line from the head to the off hand on the handguard, not the gun hand's
 *     controller. The pivot is the head, the lever is the whole arm (about 45 cm, not the 20-30 cm between two hands
 *     held at the face), and the wrist of the gun hand, which sits at the cheek at whatever angle, does not matter.
 *     The off hand is taken scope_lift_m higher: a rifle's handguard is that far under the line of sight (JKXR lifts
 *     the off hand 12 cm, RTCWQuest lowers the eye point 10 cm);
 *   - the off hand smoothed (JKXR averages its last 5 frames): here a low-pass on the head -> hand vector;
 *   - the yaw relative: the view starts on the head's yaw as the scope comes up and turns by as much as the aim has
 *     turned since. Nothing jumps sideways on the way in; the pitch is the aim's own;
 *   - no roll in the aim (the renderer gives the picture the head's roll, so the horizon stays level).
 * A gun with no off hand on it (off_ok 0 from the start) uses the gun hand's own ray the same way. An off hand lost
 * while engaged holds the last aim. */
typedef struct {
    int    active;
    int    stock;                /* the virtual stock (head -> off hand); 0: the gun hand's ray */
    float  yaw0_view, yaw0_aim;  /* rad, positive left: the head's and the aim's yaw as the scope came up */
    VrVec3 v;                    /* the smoothed aim vector (stage) */
    VrQuat q;                    /* the aim: forward -z along it, level */
    float  yaw, pitch;           /* rad: the aim's (yaw positive left, pitch positive up) */
} VrScopeAim;

/* yaw (positive left) and pitch (positive up) of a direction; 0 when it has no horizontal part to speak of */
static inline int vrg_dir_yaw_pitch(VrVec3 d, float *yaw, float *pitch)
{
    const float h = sqrtf(d.x * d.x + d.z * d.z);
    if (!(h > 1e-4f) || h * h < 0.0003f * vrm_dot(d, d)) return 0;      /* within 1 deg of straight up or down */
    *yaw = atan2f(-d.x, -d.z);
    *pitch = atan2f(d.y, h);
    return 1;
}

/* the level orientation looking along yaw / pitch */
static inline VrQuat vrg_yaw_pitch_rotation(float yaw, float pitch)
{
    const float cp = cosf(pitch);
    return vrg_look_rotation(vrm_v3(-sinf(yaw) * cp, sinf(pitch), -cosf(yaw) * cp), vrm_v3(0, 1, 0));
}

/* The head's roll (rad): how far its right vector is turned up out of the horizontal about its forward; 0 when it
 * looks straight up or down. */
static inline float vrg_head_roll(VrQuat head)
{
    const VrVec3 f = vrm_q_rotate(head, vrm_v3(0, 0, -1)), r = vrm_q_rotate(head, vrm_v3(1, 0, 0));
    VrVec3 lr = vrm_cross(f, vrm_v3(0, 1, 0)), lu;
    if (!vrm_normalize(&lr)) return 0.0f;
    lu = vrm_cross(lr, f);
    return atan2f(vrm_dot(r, lu), vrm_dot(r, lr));
}

/* `aim` (level) with the head's roll: what the scoped picture is drawn with. Shown head-locked, its horizon then
 * lies level in the world whichever way the head leans over the stock. */
static inline VrQuat vrg_scope_view_rotation(VrQuat aim, VrQuat head)
{
    VrVec3 f = vrm_q_rotate(aim, vrm_v3(0, 0, -1));
    float yaw, pitch;
    if (!vrm_normalize(&f) || !vrg_dir_yaw_pitch(f, &yaw, &pitch)) return aim;
    return vrm_q_normalize(vrm_q_mul(vrg_yaw_pitch_rotation(yaw, pitch),
                                     vrm_q_axis_angle(vrm_v3(0, 0, 1), vrg_head_roll(head))));
}

/* One input update. on: the scope is up; gun: the gun hand's aim pose (two-handed aim included); off_ok / off_pos:
 * the off hand holds the gun, and where. Returns the gun hand's aim orientation to use (gun's own when off). */
static inline VrQuat vrg_scope_aim_update(VrScopeAim *s, const VrGestureConfig *c, int on, VrPose head, VrPose gun,
                                          int off_ok, VrVec3 off_pos, double dt)
{
    VrVec3 raw;
    float yaw, pitch, hy, hp;
    int have_raw = 1;
    if (!on) { s->active = 0; return gun.orientation; }
    if (off_ok) {
        raw = vrm_sub(vrm_add(off_pos, vrm_v3(0.0f, c->scope_lift_m, 0.0f)), head.position);
    } else if (s->active && s->stock) {
        raw = s->v;                                   /* the off hand let go or lost: the last aim is held */
        have_raw = 0;
    } else {
        raw = vrm_q_rotate(gun.orientation, vrm_v3(0, 0, -1));
    }
    if (!s->active) {
        if (!vrg_dir_yaw_pitch(raw, &yaw, &pitch)) return gun.orientation;     /* no aim to start from yet */
        s->v = raw;
        s->stock = off_ok != 0;
        s->yaw0_aim = yaw;
        s->yaw0_view = vrg_dir_yaw_pitch(vrm_q_rotate(head.orientation, vrm_v3(0, 0, -1)), &hy, &hp) ? hy : yaw;
        s->active = 1;
    } else if (have_raw) {
        const float tau = c->scope_smooth_s;
        const float k = tau > 1e-4f && dt > 0.0 ? 1.0f - expf(-(float)dt / tau) : 1.0f;
        if (off_ok && !s->stock) {                    /* the off hand came on later: the stock takes over where the */
            float y0, p0, y1, p1;                     /* aim is (no jump): its yaw is counted from here */
            if (vrg_dir_yaw_pitch(s->v, &y0, &p0) && vrg_dir_yaw_pitch(raw, &y1, &p1)) {
                s->yaw0_view += (float)vrm_wrap_rad((double)(y0 - s->yaw0_aim));
                s->yaw0_aim = y1;
                s->v = raw;
                s->stock = 1;
            }
        }
        s->v = vrm_add(s->v, vrm_scale(vrm_sub(raw, s->v), k));
    }
    if (vrg_dir_yaw_pitch(s->v, &yaw, &pitch)) {
        const float lim = 80.0f * (float)VRM_DEG;
        s->yaw = (float)vrm_wrap_rad((double)s->yaw0_view + vrm_wrap_rad((double)(yaw - s->yaw0_aim)));
        s->pitch = pitch > lim ? lim : pitch < -lim ? -lim : pitch;
        s->q = vrg_yaw_pitch_rotation(s->yaw, s->pitch);
    }
    return s->q;
}

#endif /* VR_GESTURES_H */
