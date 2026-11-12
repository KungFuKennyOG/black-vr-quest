/*
 * vr_input_map.h - Touch Plus controller state -> Xbox pad (VrPadState) and
 * host-side snap / smooth turning. Pure C, unit-tested in tests/xr_shell.
 *
 * Default layout (BLACK's default Xbox preset reads the pad, so the game's
 * own action map, Controls pages and button prompts keep working):
 *
 *   Left stick          -> left stick (move)               click -> L3 (sprint/crouch as the preset says)
 *   Right stick X       -> host turning (snap or smooth), NOT the game's look
 *   Right stick Y       -> nothing in gameplay (pitch comes from the head or the hand)
 *   Right stick click   -> R3 (melee in the default preset)
 *   Right trigger       -> right trigger (fire)
 *   Left trigger        -> left trigger (aim / zoom)
 *   A / B / X / Y       -> A / B / X / Y
 *   Right grip          -> Black (Xbox analog button 4, VR_PAD_RIGHT_SHOULDER -> BLACK)
 *   Left grip           -> White (analog 5, VR_PAD_LEFT_SHOULDER -> WHITE)
 *   Left menu button    -> START (pause)
 *   Left stick click + Y held 1 s -> BACK (objectives) ; both thumbsticks clicked 1 s -> recentre
 *   In menus (present mode != STEREO) the right stick drives the d-pad and right stick X goes
 *   to the game unchanged, so menus remain navigable.
 *
 * xinput_device.c maps LEFT_SHOULDER -> BLACK and RIGHT_SHOULDER -> WHITE
 * (the comment in its README table is the other way round); we follow the code.
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
    int   left_handed;        /* swap stick roles */
} VrInputConfig;

typedef struct {
    int    snap_armed;        /* 1 when the stick has returned to centre */
    double turn_accum_deg;    /* positive = left */
    double back_hold_s, recentre_hold_s;
    int    back_fired, recentre_fired;
    int    recentre_request;  /* consumed by the shell */
} VrInputRuntime;

static inline void vr_input_default_config(VrInputConfig *c)
{
    c->turn_mode = VR_TURN_SNAP;
    c->snap_deg = 30.0f;
    c->smooth_deg_per_s = 120.0f;
    c->deadzone = 0.15f;
    c->snap_on = 0.7f;
    c->snap_off = 0.3f;
    c->left_handed = 0;
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

/* Turning from the right stick (or left, when left-handed). Returns degrees
 * turned this step, positive = left. */
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

/* Rotates a stick vector (x right, y forward) by `deg` counter-clockwise
 * (positive = toward the left), saturating to the unit disc. */
static inline void vr_rotate_stick(float *x, float *y, double deg)
{
    const double r = deg * 3.14159265358979323846 / 180.0, c = cos(r), s = sin(r);
    const double nx = *x * c - *y * s, ny = *x * s + *y * c;
    *x = (float)nx; *y = (float)ny;
}

/* Builds the pad. `in_game` = gameplay (stereo or scope) presentation.
 * `move_rotate_deg` turns the move stick (locomotion relative to the head
 * while the game's yaw follows the hand). */
static inline void vr_map_pad(const VrInputConfig *c, VrInputRuntime *rt, const VrTouchState *t,
                              int in_game, double move_rotate_deg, double dt, VrPadState *out)
{
    float mx = t->lx, my = t->ly, lx_turn = t->rx, ly_look = t->ry;
    int move_click = t->lclick, look_click = t->rclick;
    memset(out, 0, sizeof(*out));
    if (c->left_handed) {
        mx = t->rx; my = t->ry; lx_turn = t->lx; ly_look = t->ly;
        move_click = t->rclick; look_click = t->lclick;
    }
    if (in_game && move_rotate_deg != 0.0) vr_rotate_stick(&mx, &my, move_rotate_deg);
    out->thumb_lx = vr_axis_to_s16(mx, c->deadzone);
    out->thumb_ly = vr_axis_to_s16(my, c->deadzone);
    if (in_game) {
        vr_turn_step(c, rt, lx_turn, dt);
        out->thumb_rx = 0;
        out->thumb_ry = 0;
    } else {
        /* menus: right stick passes through and also drives the d-pad */
        out->thumb_rx = vr_axis_to_s16(lx_turn, c->deadzone);
        out->thumb_ry = vr_axis_to_s16(ly_look, c->deadzone);
        if (ly_look > 0.6f) out->buttons |= VR_PAD_DPAD_UP;
        if (ly_look < -0.6f) out->buttons |= VR_PAD_DPAD_DOWN;
        if (lx_turn < -0.6f) out->buttons |= VR_PAD_DPAD_LEFT;
        if (lx_turn > 0.6f) out->buttons |= VR_PAD_DPAD_RIGHT;
        rt->snap_armed = 0;            /* no snap on the way back into the game */
    }
    out->left_trigger = vr_trigger_to_u8(t->lt);
    out->right_trigger = vr_trigger_to_u8(t->rt);
    if (t->a) out->buttons |= VR_PAD_A;
    if (t->b) out->buttons |= VR_PAD_B;
    if (t->x) out->buttons |= VR_PAD_X;
    if (t->y) out->buttons |= VR_PAD_Y;
    if (t->menu) out->buttons |= VR_PAD_START;
    if (t->rgrip > 0.6f) out->buttons |= VR_PAD_LEFT_SHOULDER;   /* -> Xbox BLACK button */
    if (t->lgrip > 0.6f) out->buttons |= VR_PAD_RIGHT_SHOULDER;  /* -> Xbox WHITE button */

    /* chords */
    if (move_click && t->y) {
        rt->back_hold_s += dt;
        if (rt->back_hold_s >= 0.6) { out->buttons |= VR_PAD_BACK; out->buttons &= (uint16_t)~VR_PAD_Y; }
    } else {
        rt->back_hold_s = 0.0;
    }
    if (move_click && look_click) {
        rt->recentre_hold_s += dt;
        if (rt->recentre_hold_s >= 1.0 && !rt->recentre_fired) { rt->recentre_request = 1; rt->recentre_fired = 1; }
        move_click = look_click = 0;        /* do not crouch/melee while recentring */
    } else {
        rt->recentre_hold_s = 0.0;
        rt->recentre_fired = 0;
    }
    if (move_click && !(out->buttons & VR_PAD_BACK)) out->buttons |= VR_PAD_LEFT_THUMB;
    if (look_click) out->buttons |= VR_PAD_RIGHT_THUMB;
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
