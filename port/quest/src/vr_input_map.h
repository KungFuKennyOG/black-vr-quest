/*
 * vr_input_map.h - Touch Plus controllers -> BLACK's game ACTIONS -> the Xbox pad, and host-side snap / smooth
 * turning. Pure C, unit-tested in tests/xr_shell (test_input.c). Gestures (swing, duck, scope raise, two hands)
 * are in vr_gestures.h; the shell (vr_bridge.c) runs both.
 *
 * Gameplay (present mode STEREO or FLAT_SCOPE): the controllers produce actions, and each action is pressed on
 * whatever Xbox control the game's LIVE action map ([0x2F3BD4], 37 slots) gives its slot, so all four presets and
 * the player's own layout keep working (vr_actions_to_pad, with the game's resolver). Right-handed:
 *
 *   right trigger       Fire (analog when Fire is on a trigger)       left stick        move (locomotion = head |
 *   right A             Reload                                                          aim | offhand)
 *   right B             Melee (also: a fast swing of the gun hand)    left stick click  Crouch (as the game's
 *   right stick x       turn (snap / smooth)                                            Toggle Crouch says)
 *   right stick up/down next / previous weapon, one per push        left trigger      Grenade
 *                       (while the scope is up: the scope's zoom)     left grip         two-handed aim (vr_gestures.h)
 *   right stick click   Use / pick up / doors                         left X            Health pack
 *   right grip          nothing (scope_raise = off: holds the scope)  left Y            tap: Suppressor, hold 0.5 s:
 *   body: duck = crouch, raise the gun to the eye = scope (scoped                         Fire mode
 *         weapons only)                                               left Menu         tap: Pause, hold 1 s: Back
 *                                                                                       (objectives)
 *   left_handed = 1 mirrors it: left trigger fires, X reloads, Y melees, A health pack, B suppressor / fire mode.
 *   Both stick clicks held 1 s: recentre (the clicks then do neither Crouch nor Use: a click only becomes an action
 *   once the other stick has stayed up for VRI_CLICK_DELAY_S).
 *
 * Menus and flat modes (vr_map_menu_pad): the pad as buttons, as before: right stick passes through and also drives
 * the d-pad, face buttons, triggers and Menu (START) pass through, right grip -> Black, left grip -> White.
 *
 * xinput_device.c maps LEFT_SHOULDER -> BLACK and RIGHT_SHOULDER -> WHITE (the comment in its README table is the
 * other way round); we follow the code.
 */
#ifndef VR_INPUT_MAP_H
#define VR_INPUT_MAP_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "../vr_bridge.h"

typedef struct {
    float lx, ly, rx, ry;          /* -1..1, +y up */
    float lt, rt, lgrip, rgrip;    /* 0..1 */
    int a, b, x, y, menu, lclick, rclick;
    int left_active, right_active;
} VrTouchState;

typedef enum { VR_TURN_SNAP = 0, VR_TURN_SMOOTH = 1, VR_TURN_OFF = 2 } VrTurnMode;

typedef struct {
    VrTurnMode turn_mode;
    float snap_deg;           /* 30 or 45 */
    float smooth_deg_per_s;   /* 120 */
    float deadzone;           /* 0.15 */
    float snap_on, snap_off;  /* hysteresis, 0.7 / 0.3 */
    int   left_handed;        /* mirror the scheme */
    int   accuracy;           /* VR_ACCURACY_* (blackvr.ini vr_accuracy) */
} VrInputConfig;

enum { VR_ACCURACY_AIM = 0, VR_ACCURACY_HIP = 1 };

/* The game actions and BLACK's action slots they read (pc_input.c's table; the Controls page's list at 0x2927B4;
 * slot 35 = the scope's zoom level, read by 1EC450 for weapons whose descriptor +C0 says scoped). */
typedef enum {
    VRA_FIRE, VRA_RELOAD, VRA_MELEE, VRA_USE, VRA_CROUCH, VRA_NEXT_WEAPON, VRA_PREV_WEAPON, VRA_GRENADE,
    VRA_HEALTH_PACK, VRA_SUPPRESSOR, VRA_FIRE_MODE, VRA_PAUSE, VRA_BACK, VRA_SCOPE_ZOOM, VRA_COUNT
} VrAction;
static const uint8_t k_vra_slot[VRA_COUNT] = { 9, 11, 32, 13, 12, 14, 15, 27, 33, 31, 30, 25, 36, 35 };
static const char *const k_vra_name[VRA_COUNT] = {
    "fire", "reload", "melee", "use", "crouch", "next_weapon", "prev_weapon", "grenade", "health_pack",
    "suppressor", "fire_mode", "pause", "back", "scope_zoom" };
#define VRA_BIT(a) (1u << (a))
enum { VRA_SLOT_MOVE_FORWARD = 1 };

#define VRI_PULSE_S 0.15          /* a tap action is pressed this long (>= 4 game frames at 30 Hz) */
#define VRI_PULSE_GAP_S 0.10      /* and released this long before the same action again */
#define VRI_Y_HOLD_S 0.5          /* Y: tap = Suppressor, held this long = Fire mode */
#define VRI_MENU_HOLD_S 1.0       /* Menu: tap = Pause, held this long = Back */
#define VRI_CLICK_DELAY_S 0.06    /* a stick click waits this long for the other (the recentre chord) */
#define VRI_RECENTRE_S 1.0
#define VRI_STEP_ON 0.7f          /* weapon step: push past, back under VRI_STEP_OFF to re-arm */
#define VRI_STEP_OFF 0.3f

typedef struct {
    int    snap_armed;        /* 1 when the stick has returned to centre */
    double turn_accum_deg;    /* positive = left */
    double recentre_hold_s;
    int    recentre_fired;
    int    recentre_request;  /* consumed by the shell */
    /* gameplay */
    int    chord;             /* both clicks have been down together: neither acts until both are up */
    double click_s[2];        /* how long each stick (0 left, 1 right) has been clicked */
    int    click_on[2];       /* the click acted last step */
    int    step_armed;
    int    y_down, y_fired, menu_down, menu_fired;
    double y_s, menu_s;
    double pulse_s[VRA_COUNT], gap_s[VRA_COUNT];
    uint32_t pulse_tick[VRA_COUNT], gap_tick[VRA_COUNT];   /* the game's input-update count when they began */
    uint8_t pending[VRA_COUNT];
    uint32_t fired_mask;      /* actions whose tap was started this step (logs / tests) */
    int    was_in_game;       /* the mode of the previous step */
    uint32_t block;           /* VRI_T_* held across a gameplay <-> menu change: ignored until released */
} VrInputRuntime;

/* Buttons held while the game enters or leaves gameplay act only after a release: the A that confirmed "resume"
 * does not reload, the Menu held for Back does not press START in the objectives screen. */
enum { VRI_T_A = 1, VRI_T_B = 2, VRI_T_X = 4, VRI_T_Y = 8, VRI_T_MENU = 16, VRI_T_LT = 128, VRI_T_RT = 256 };
/* (the stick clicks are not blocked: the recentre chord works in any mode) */
static inline uint32_t vri_touch_mask(const VrTouchState *t)
{
    return (t->a ? (uint32_t)VRI_T_A : 0u) | (t->b ? (uint32_t)VRI_T_B : 0u) | (t->x ? (uint32_t)VRI_T_X : 0u) |
           (t->y ? (uint32_t)VRI_T_Y : 0u) | (t->menu ? (uint32_t)VRI_T_MENU : 0u) |
           (t->lt > 0.3f ? (uint32_t)VRI_T_LT : 0u) | (t->rt > 0.3f ? (uint32_t)VRI_T_RT : 0u);
}
static inline void vri_filter(VrInputRuntime *rt, int in_game, const VrTouchState *in, VrTouchState *t)
{
    const uint32_t now = vri_touch_mask(in);
    if (in_game != rt->was_in_game) { rt->block = now; rt->was_in_game = in_game; }
    rt->block &= now;                                  /* released: no longer blocked */
    *t = *in;
    if (rt->block & VRI_T_A) t->a = 0;
    if (rt->block & VRI_T_B) t->b = 0;
    if (rt->block & VRI_T_X) t->x = 0;
    if (rt->block & VRI_T_Y) t->y = 0;
    if (rt->block & VRI_T_MENU) t->menu = 0;
    if (rt->block & VRI_T_LT) t->lt = 0.0f;
    if (rt->block & VRI_T_RT) t->rt = 0.0f;
}

/* What the shell knows this frame beyond the buttons (gestures, the game's state). */
typedef struct {
    int    in_game;           /* gameplay presentation (STEREO / FLAT_SCOPE) */
    double move_rotate_deg;   /* turns the move stick (locomotion relative to the head / off hand) */
    int    melee_swing;       /* the swing detector fired */
    int    crouch_hold;       /* the crouch sync wants the crouch control down */
    int    scope_up;          /* the scope is engaged (right stick up/down then steps its zoom) */
    int      ticks_valid;     /* game_ticks counts the game's input updates (vr_publish_game_input) */
    uint32_t game_ticks;      /* a tap lasts until the game has also run VRI_PULSE_TICKS updates: a slow or
                               * hitching game frame never misses it */
} VrInputContext;

#define VRI_PULSE_TICKS 2u

/* The actions of one frame. */
typedef struct {
    int      in_game;
    uint32_t held;            /* VRA_BIT()s */
    float    fire, grenade;   /* analog 0..1 (pressed: > 0.5 when on a button) */
    int16_t  move_x, move_y;  /* the move stick, dead zone applied */
    VrPadState menu;          /* !in_game: the pad as-is */
} VrActionState;

static inline void vr_input_default_config(VrInputConfig *c)
{
    c->turn_mode = VR_TURN_SNAP;
    c->snap_deg = 30.0f;
    c->smooth_deg_per_s = 120.0f;
    c->deadzone = 0.15f;
    c->snap_on = 0.7f;
    c->snap_off = 0.3f;
    c->left_handed = 0;
    c->accuracy = VR_ACCURACY_AIM;
}

static inline int16_t vr_axis_to_s16(float v, float deadzone)
{
    float a = fabsf(v);
    if (!isfinite(v) || a <= deadzone) return 0;
    if (a > 1.0f) a = 1.0f;
    a = (a - deadzone) / (1.0f - deadzone);          /* rescale past the dead zone */
    {
        const float s = v < 0.0f ? -a : a;
        long r = lroundf(s * 32767.0f);
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        return (int16_t)r;
    }
}
static inline uint8_t vr_trigger_to_u8(float v)
{
    if (!isfinite(v) || v <= 0.02f) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)lroundf(v * 255.0f);
}

/* Turning from the gun hand's stick. Returns degrees turned this step, positive = left. */
static inline double vr_turn_step(const VrInputConfig *c, VrInputRuntime *rt, float stick_x, double dt)
{
    double d = 0.0;
    const float ax = fabsf(stick_x);
    if (c->turn_mode == VR_TURN_OFF) return 0.0;
    if (c->turn_mode == VR_TURN_SNAP) {
        if (ax < c->snap_off) rt->snap_armed = 1;
        if (rt->snap_armed && ax >= c->snap_on) {
            d = stick_x > 0.0f ? -c->snap_deg : c->snap_deg;
            rt->snap_armed = 0;
        }
    } else if (ax > c->deadzone && dt > 0.0 && dt < 0.25) {
        const double mag = (ax - c->deadzone) / (1.0 - c->deadzone);
        d = -(stick_x > 0.0f ? 1.0 : -1.0) * mag * c->smooth_deg_per_s * dt;
    }
    rt->turn_accum_deg += d;
    return d;
}

/* Rotates a stick vector (x right, y forward) by `deg` counter-clockwise (positive = toward the left). */
static inline void vr_rotate_stick(float *x, float *y, double deg)
{
    const double r = deg * 3.14159265358979323846 / 180.0, c = cos(r), s = sin(r);
    const double nx = *x * c - *y * s, ny = *x * s + *y * c;
    *x = (float)nx; *y = (float)ny;
}

/* ---- taps: a pulse of VRI_PULSE_S, then a gap, queued (two quick steps are two presses) ---- */
static inline void vri_tap(VrInputRuntime *rt, VrAction a)
{
    if (rt->pending[a] < 4) rt->pending[a]++;
}
static inline uint32_t vri_pulses(VrInputRuntime *rt, double dt, int ticks_valid, uint32_t ticks)
{
    uint32_t held = 0;
    int i;
    rt->fired_mask = 0;
    for (i = 0; i < VRA_COUNT; i++) {
        if (rt->pulse_s[i] > 0.0 || (rt->pulse_tick[i] != 0u && ticks_valid)) {
            rt->pulse_s[i] -= dt;
            if (rt->pulse_s[i] > 0.0 || (ticks_valid && ticks - (rt->pulse_tick[i] - 1u) < VRI_PULSE_TICKS)) {
                held |= VRA_BIT(i);
                continue;
            }
            rt->pulse_s[i] = 0.0;
            rt->pulse_tick[i] = 0u;
            rt->gap_s[i] = VRI_PULSE_GAP_S;
            rt->gap_tick[i] = ticks + 1u;
        }
        if (rt->gap_s[i] > 0.0 || rt->gap_tick[i] != 0u) {
            rt->gap_s[i] -= dt;
            if (rt->gap_s[i] > 0.0 || (ticks_valid && ticks - (rt->gap_tick[i] - 1u) < VRI_PULSE_TICKS)) continue;
            rt->gap_s[i] = 0.0;
            rt->gap_tick[i] = 0u;
        }
        if (rt->pending[i]) {
            rt->pending[i]--;
            rt->pulse_s[i] = VRI_PULSE_S;
            rt->pulse_tick[i] = ticks + 1u;          /* +1: 0 means "none" */
            held |= VRA_BIT(i);
            rt->fired_mask |= VRA_BIT(i);
        }
    }
    return held;
}

/* Both sticks' clicks: the recentre chord, and the click actions only once the chord is ruled out. */
static inline void vri_clicks(VrInputRuntime *rt, int lclick, int rclick, double dt, int *left_on, int *right_on)
{
    const int both = lclick && rclick;
    if (both) {
        rt->chord = 1;
        rt->recentre_hold_s += dt;
        if (rt->recentre_hold_s >= VRI_RECENTRE_S && !rt->recentre_fired) { rt->recentre_request = 1; rt->recentre_fired = 1; }
    } else {
        rt->recentre_hold_s = 0.0;
        rt->recentre_fired = 0;
    }
    if (!lclick && !rclick) rt->chord = 0;
    rt->click_s[0] = lclick ? rt->click_s[0] + dt : 0.0;
    rt->click_s[1] = rclick ? rt->click_s[1] + dt : 0.0;
    /* a click already acting before the chord keeps acting while held (a held crouch); a new one waits out the
     * chord delay, and none starts while the chord lasts (until both sticks are up) */
    *left_on = lclick && (rt->click_on[0] || (!rt->chord && rt->click_s[0] >= VRI_CLICK_DELAY_S));
    *right_on = rclick && (rt->click_on[1] || (!rt->chord && rt->click_s[1] >= VRI_CLICK_DELAY_S));
    rt->click_on[0] = *left_on;
    rt->click_on[1] = *right_on;
}

/* Menus / flat modes: the pad as buttons (unchanged from the button layout). */
static inline void vr_map_menu_pad(const VrInputConfig *c, VrInputRuntime *rt, const VrTouchState *t, double dt,
                                   VrPadState *out)
{
    float mx = t->lx, my = t->ly, sx = t->rx, sy = t->ry;
    int lc, rc, mclick = t->lclick, oclick = t->rclick;
    memset(out, 0, sizeof(*out));
    if (c->left_handed) { mx = t->rx; my = t->ry; sx = t->lx; sy = t->ly; mclick = t->rclick; oclick = t->lclick; }
    out->thumb_lx = vr_axis_to_s16(mx, c->deadzone);
    out->thumb_ly = vr_axis_to_s16(my, c->deadzone);
    out->thumb_rx = vr_axis_to_s16(sx, c->deadzone);
    out->thumb_ry = vr_axis_to_s16(sy, c->deadzone);
    if (sy > 0.6f) out->buttons |= VR_PAD_DPAD_UP;
    if (sy < -0.6f) out->buttons |= VR_PAD_DPAD_DOWN;
    if (sx < -0.6f) out->buttons |= VR_PAD_DPAD_LEFT;
    if (sx > 0.6f) out->buttons |= VR_PAD_DPAD_RIGHT;
    rt->snap_armed = 0;                    /* no snap on the way back into the game */
    rt->step_armed = 0;
    out->left_trigger = vr_trigger_to_u8(t->lt);
    out->right_trigger = vr_trigger_to_u8(t->rt);
    if (t->a) out->buttons |= VR_PAD_A;
    if (t->b) out->buttons |= VR_PAD_B;
    if (t->x) out->buttons |= VR_PAD_X;
    if (t->y) out->buttons |= VR_PAD_Y;
    if (t->menu) out->buttons |= VR_PAD_START;
    if (t->rgrip > 0.6f) out->buttons |= VR_PAD_LEFT_SHOULDER;    /* -> Xbox BLACK */
    if (t->lgrip > 0.6f) out->buttons |= VR_PAD_RIGHT_SHOULDER;   /* -> Xbox WHITE */
    vri_clicks(rt, mclick, oclick, dt, &lc, &rc);
    if (lc) out->buttons |= VR_PAD_LEFT_THUMB;
    if (rc) out->buttons |= VR_PAD_RIGHT_THUMB;
    /* gameplay's tap/hold state does not carry into a menu */
    rt->y_down = rt->menu_down = 0;
}

/* Gameplay: the actions of this frame. */
static inline void vr_map_actions(const VrInputConfig *c, VrInputRuntime *rt, const VrTouchState *t_in,
                                  const VrInputContext *ctx, double dt, VrActionState *out)
{
    VrTouchState tf;
    const VrTouchState *t = (vri_filter(rt, ctx->in_game, t_in, &tf), &tf);
    const int lh = c->left_handed;
    /* the gun hand (right unless left-handed) and the off hand */
    const float gun_trig = lh ? t->lt : t->rt, off_trig = lh ? t->rt : t->lt;
    const float gx = lh ? t->lx : t->rx, gy = lh ? t->ly : t->ry;
    float mx = lh ? t->rx : t->lx, my = lh ? t->ry : t->ly;
    const int gun_lo = lh ? t->x : t->a, gun_hi = lh ? t->y : t->b;
    const int off_lo = lh ? t->a : t->x, off_hi = lh ? t->b : t->y;
    int lclick_on, rclick_on, gun_click, off_click;
    uint32_t held;
    memset(out, 0, sizeof(*out));
    if (!ctx->in_game) {
        vr_map_menu_pad(c, rt, t, dt, &out->menu);
        memset(rt->pending, 0, sizeof(rt->pending));
        memset(rt->pulse_s, 0, sizeof(rt->pulse_s));
        memset(rt->pulse_tick, 0, sizeof(rt->pulse_tick));
        memset(rt->gap_s, 0, sizeof(rt->gap_s));
        memset(rt->gap_tick, 0, sizeof(rt->gap_tick));
        return;
    }
    out->in_game = 1;

    /* move */
    if (ctx->move_rotate_deg != 0.0) vr_rotate_stick(&mx, &my, ctx->move_rotate_deg);
    out->move_x = vr_axis_to_s16(mx, c->deadzone);
    out->move_y = vr_axis_to_s16(my, c->deadzone);

    /* gun stick: up / down = one weapon step per push (or the scope's zoom), sideways = turn */
    {
        const float ax = fabsf(gx), ay = fabsf(gy);
        if (ay < VRI_STEP_OFF) rt->step_armed = 1;
        if (ay > 0.6f && ay > ax) {
            rt->snap_armed = 0;                     /* a vertical push never also snaps */
            if (rt->step_armed && ay >= VRI_STEP_ON && ay > ax * 1.2f) {
                rt->step_armed = 0;
                if (ctx->scope_up) vri_tap(rt, VRA_SCOPE_ZOOM);
                else vri_tap(rt, gy > 0.0f ? VRA_NEXT_WEAPON : VRA_PREV_WEAPON);
            }
        } else {
            vr_turn_step(c, rt, gx, dt);
        }
    }

    /* Y: tap Suppressor / hold Fire mode;  Menu: tap Pause / hold Back */
    if (off_hi) {
        if (!rt->y_down) { rt->y_down = 1; rt->y_s = 0.0; rt->y_fired = 0; }
        rt->y_s += dt;
        if (!rt->y_fired && rt->y_s >= VRI_Y_HOLD_S) { rt->y_fired = 1; vri_tap(rt, VRA_FIRE_MODE); }
    } else if (rt->y_down) {
        rt->y_down = 0;
        if (!rt->y_fired) vri_tap(rt, VRA_SUPPRESSOR);
    }
    if (t->menu) {
        if (!rt->menu_down) { rt->menu_down = 1; rt->menu_s = 0.0; rt->menu_fired = 0; }
        rt->menu_s += dt;
        if (!rt->menu_fired && rt->menu_s >= VRI_MENU_HOLD_S) { rt->menu_fired = 1; vri_tap(rt, VRA_BACK); }
    } else if (rt->menu_down) {
        rt->menu_down = 0;
        if (!rt->menu_fired) vri_tap(rt, VRA_PAUSE);
    }
    if (ctx->melee_swing) vri_tap(rt, VRA_MELEE);

    held = vri_pulses(rt, dt, ctx->ticks_valid, ctx->game_ticks);
    vri_clicks(rt, t->lclick, t->rclick, dt, &lclick_on, &rclick_on);
    gun_click = lh ? lclick_on : rclick_on;
    off_click = lh ? rclick_on : lclick_on;
    if (gun_lo) held |= VRA_BIT(VRA_RELOAD);
    if (gun_hi) held |= VRA_BIT(VRA_MELEE);
    if (gun_click) held |= VRA_BIT(VRA_USE);
    if (off_click || ctx->crouch_hold) held |= VRA_BIT(VRA_CROUCH);
    if (off_lo) held |= VRA_BIT(VRA_HEALTH_PACK);
    out->fire = gun_trig > 0.0f && isfinite(gun_trig) ? (gun_trig > 1.0f ? 1.0f : gun_trig) : 0.0f;
    out->grenade = off_trig > 0.0f && isfinite(off_trig) ? (off_trig > 1.0f ? 1.0f : off_trig) : 0.0f;
    if (out->fire > 0.02f) held |= VRA_BIT(VRA_FIRE);
    if (out->grenade > 0.02f) held |= VRA_BIT(VRA_GRENADE);
    out->held = held;
}

/* One control (BLACK's numbering: pc_controls.c k_pad) pressed on the pad. `analog` 0..1 for the triggers. */
static inline void vr_pad_press(VrPadState *p, int control, float analog)
{
    static const uint16_t bits[] = { VR_PAD_DPAD_UP, VR_PAD_DPAD_DOWN, VR_PAD_DPAD_LEFT, VR_PAD_DPAD_RIGHT };
    const uint8_t a8 = vr_trigger_to_u8(analog);
    switch (control) {
    case 0: case 1: case 2: case 3: p->buttons |= bits[control]; break;
    case 4: p->thumb_ly = 32767; break;
    case 5: p->thumb_ly = -32767; break;
    case 6: p->thumb_lx = -32767; break;
    case 7: p->thumb_lx = 32767; break;
    case 8: p->thumb_ry = 32767; break;
    case 9: p->thumb_ry = -32767; break;
    case 10: p->thumb_rx = -32767; break;
    case 11: p->thumb_rx = 32767; break;
    case 12: p->buttons |= VR_PAD_START; break;
    case 13: p->buttons |= VR_PAD_BACK; break;
    case 14: p->buttons |= VR_PAD_LEFT_THUMB; break;
    case 15: p->buttons |= VR_PAD_RIGHT_THUMB; break;
    case 16: p->buttons |= VR_PAD_A; break;
    case 17: p->buttons |= VR_PAD_B; break;
    case 18: p->buttons |= VR_PAD_X; break;
    case 19: p->buttons |= VR_PAD_Y; break;
    case 20: p->buttons |= VR_PAD_LEFT_SHOULDER; break;    /* Black */
    case 21: p->buttons |= VR_PAD_RIGHT_SHOULDER; break;   /* White */
    case 22: if (a8 > p->left_trigger) p->left_trigger = a8; break;
    case 23: if (a8 > p->right_trigger) p->right_trigger = a8; break;
    default: break;                                         /* none / axes: not a button */
    }
}

/* Actions -> the pad, each action on the control the live map gives its slot. resolve(slot): control 0..27, or
 * -1 for none. The move vector goes to the stick the map gives "move forward" (slot 1; the right one in the
 * Southpaw preset); the other stick stays centred (look comes from the head and hands). */
typedef int (*VrSlotResolver)(int slot);
static inline void vr_actions_to_pad(const VrActionState *a, VrSlotResolver resolve, VrPadState *out)
{
    int i, c;
    if (!a->in_game) { *out = a->menu; return; }
    memset(out, 0, sizeof(*out));
    c = resolve ? resolve(VRA_SLOT_MOVE_FORWARD) : -1;
    if ((c >= 8 && c <= 11) || c == 26 || c == 27) { out->thumb_rx = a->move_x; out->thumb_ry = a->move_y; }
    else { out->thumb_lx = a->move_x; out->thumb_ly = a->move_y; }
    if (!resolve) return;
    for (i = 0; i < VRA_COUNT; i++) {
        float v = 1.0f;
        if (!(a->held & VRA_BIT(i))) continue;
        c = resolve(k_vra_slot[i]);
        if (c < 0) continue;
        if (i == VRA_FIRE) v = a->fire;
        else if (i == VRA_GRENADE) v = a->grenade;
        if ((i == VRA_FIRE || i == VRA_GRENADE) && c != 22 && c != 23 && v <= 0.5f) continue;  /* button: past half */
        vr_pad_press(out, c, (c == 22 || c == 23) ? v : 1.0f);
    }
}

/* VrPadState -> the 8 Xbox analog buttons + digital word, as xinput_device.c
 * builds them from XInput. Shared with the runtime hook so both agree. */
static inline void vr_pad_to_xbox(const VrPadState *p, uint16_t *digital, uint8_t analog[8])
{
    *digital = (uint16_t)(p->buttons & 0x00FF);
    analog[0] = (p->buttons & VR_PAD_A) ? 255 : 0;
    analog[1] = (p->buttons & VR_PAD_B) ? 255 : 0;
    analog[2] = (p->buttons & VR_PAD_X) ? 255 : 0;
    analog[3] = (p->buttons & VR_PAD_Y) ? 255 : 0;
    analog[4] = (p->buttons & VR_PAD_LEFT_SHOULDER) ? 255 : 0;   /* BLACK */
    analog[5] = (p->buttons & VR_PAD_RIGHT_SHOULDER) ? 255 : 0;  /* WHITE */
    analog[6] = p->left_trigger;
    analog[7] = p->right_trigger;
}

#endif /* VR_INPUT_MAP_H */
