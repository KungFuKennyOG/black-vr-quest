/*
 * vr_panel_math.h - the settings panel's pure parts (port/notes/settings-panel.md): where the panel goes when it
 * opens, the laser pointer's hit on it, the laser beam's quad, and the open / close / pointer state machine over the
 * controllers. Header-only, pure C: unit-tested in tests/xr_shell (test_settings.c).
 */
#ifndef VR_PANEL_MATH_H
#define VR_PANEL_MATH_H

#include <math.h>
#include <string.h>
#include "vr_math.h"
#include "vr_input_map.h"
#include "vr_gestures.h"   /* vrg_look_rotation */

#define VRP_DISTANCE_M     1.2f    /* in front of the head when it opens */
#define VRP_WIDTH_M        0.8f
#define VRP_DROP_M         0.10f   /* centre below eye level */
#define VRP_OPEN_CLICK_MAX_S 0.8   /* the open click: released within this, the other stick never clicked */
#define VRP_TRIGGER_DOWN   0.55f   /* laser click hysteresis */
#define VRP_TRIGGER_UP     0.35f
#define VRP_SCROLL_DEADZONE 0.25f
#define VRP_SCROLL_RATE    7.0f    /* mouse-wheel steps per second at full stick */
#define VRP_LASER_WIDTH_M  0.004f
#define VRP_LASER_MISS_M   3.0f    /* beam length when the panel is not hit */

typedef struct {
    VrPose pose;                   /* centre; the quad's +z faces the user */
    float width_m, height_m;
} VrpQuad;

/* World-locked where it opens: VRP_DISTANCE_M ahead of the head's horizontal facing, VRP_DROP_M below the eyes,
 * upright and facing the head (an OpenXR quad layer's visible side is +z). */
static inline VrpQuad vrp_place(VrPose head, float aspect_w_over_h)
{
    VrpQuad q;
    VrVec3 f = vrm_q_rotate(head.orientation, vrm_v3(0, 0, -1));
    f.y = 0.0f;
    if (!vrm_normalize(&f)) {                       /* looking straight up/down: the head's yaw */
        const float y = vrm_q_yaw(head.orientation);
        f = vrm_v3(-sinf(y), 0.0f, -cosf(y));
    }
    q.pose.orientation = vrg_look_rotation(f, vrm_v3(0, 1, 0));
    q.pose.position = vrm_add(head.position, vrm_add(vrm_scale(f, VRP_DISTANCE_M), vrm_v3(0, -VRP_DROP_M, 0)));
    q.width_m = VRP_WIDTH_M;
    q.height_m = VRP_WIDTH_M / (aspect_w_over_h > 0.0f ? aspect_w_over_h : 4.0f / 3.0f);
    return q;
}

/* The ray (aim pose: origin, forward -z) against the quad from its front. Returns 1 on a hit inside it, with the
 * image coordinates (0..1, x right, y down) and the distance; 0 otherwise (then *t is the distance to the plane, or
 * -1 when it is behind / parallel). */
static inline int vrp_hit(const VrpQuad *q, VrPose aim, float *u, float *v, float *t)
{
    const VrVec3 d = vrm_q_rotate(aim.orientation, vrm_v3(0, 0, -1));
    const VrVec3 n = vrm_q_rotate(q->pose.orientation, vrm_v3(0, 0, 1));
    const VrVec3 r = vrm_q_rotate(q->pose.orientation, vrm_v3(1, 0, 0));
    const VrVec3 up = vrm_q_rotate(q->pose.orientation, vrm_v3(0, 1, 0));
    const float dn = vrm_dot(d, n);
    float tt, x, y;
    VrVec3 p;
    *t = -1.0f;
    if (dn > -1e-4f) return 0;                       /* parallel, or pointing at the back */
    tt = vrm_dot(vrm_sub(q->pose.position, aim.position), n) / dn;
    if (tt <= 0.0f) return 0;                        /* the panel is behind the hand */
    *t = tt;
    p = vrm_sub(vrm_add(aim.position, vrm_scale(d, tt)), q->pose.position);
    x = vrm_dot(p, r) / q->width_m + 0.5f;
    y = 0.5f - vrm_dot(p, up) / q->height_m;
    *u = x;
    *v = y;
    return x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;
}

/* The laser beam as a thin quad: from the aim origin `length` metres along the ray, its local y along the beam and its
 * face (+z) turned toward the head as far as the beam allows. *size_x / *size_y: width, length. */
static inline VrPose vrp_laser_pose(VrPose aim, float length, VrVec3 head, float *size_x, float *size_y)
{
    const VrVec3 d = vrm_q_rotate(aim.orientation, vrm_v3(0, 0, -1));
    const VrVec3 c = vrm_add(aim.position, vrm_scale(d, length * 0.5f));
    VrVec3 toward = vrm_sub(head, c), f;
    VrPose p;
    toward = vrm_sub(toward, vrm_scale(d, vrm_dot(toward, d)));
    if (!vrm_normalize(&toward)) toward = vrm_q_rotate(aim.orientation, vrm_v3(0, 1, 0));
    f = vrm_scale(toward, -1.0f);                    /* look_rotation's forward is -z: away from the head */
    p.orientation = vrg_look_rotation(f, d);         /* y along the beam */
    p.position = c;
    *size_x = VRP_LASER_WIDTH_M;
    *size_y = length;
    return p;
}

/* ---- controllers while the panel is open ---------------------------------------------------------------------- */

typedef struct {
    int open;
    /* the open click (the off hand's stick: the left one, the right one when left-handed) */
    int click_down, other_seen;
    double click_s;
    /* while open */
    int trig_down;                 /* the laser's click (gun-hand trigger, or A/X on the gun hand) */
    int b_prev, menu_prev;
    uint32_t block;                /* VRI_T_* held when it closed: kept from the game until released */
    int block_clicks;              /* stick clicks likewise */
} VrpInput;

typedef struct {
    int opened, closed;            /* this step */
    int mouse_down;                /* laser click held */
    float wheel;                   /* mouse-wheel steps this step (+ = scroll up) */
} VrpStep;

/* The click that opens it, the buttons that close it, the laser click and scroll. menu_ctx: the game is in a menu
 * (paused, the front end, no game). `game` receives what the game may see this step: everything while the panel is
 * open, neutral; after it closes the buttons that closed it stay hidden until they are released. Returns open. */
static inline int vrp_input_step(VrpInput *p, const VrTouchState *in, int left_handed, int menu_ctx, double dt,
                                 VrTouchState *game, VrpStep *st)
{
    const int open_click = left_handed ? in->rclick : in->lclick, other_click = left_handed ? in->lclick : in->rclick;
    const float gun_trig = left_handed ? in->lt : in->rt;
    const int gun_lo = left_handed ? in->x : in->a;
    memset(st, 0, sizeof(*st));
    *game = *in;
    if (!p->open) {
        if (open_click) {
            if (!p->click_down) { p->click_down = 1; p->click_s = 0.0; p->other_seen = 0; }
            p->click_s += dt;
            if (other_click) p->other_seen = 1;          /* the recentre chord, not the panel */
        } else if (p->click_down) {
            p->click_down = 0;
            if (!p->other_seen && p->click_s <= VRP_OPEN_CLICK_MAX_S && menu_ctx) {
                p->open = 1;
                st->opened = 1;
                p->b_prev = in->b;
                p->menu_prev = in->menu;
                p->trig_down = 0;
            }
        }
        if (!p->open) {
            /* buttons that closed it, hidden from the game until released */
            p->block &= vri_touch_mask(in);
            if (p->block & VRI_T_A) game->a = 0;
            if (p->block & VRI_T_B) game->b = 0;
            if (p->block & VRI_T_X) game->x = 0;
            if (p->block & VRI_T_Y) game->y = 0;
            if (p->block & VRI_T_MENU) game->menu = 0;
            if (p->block & VRI_T_LT) game->lt = 0.0f;
            if (p->block & VRI_T_RT) game->rt = 0.0f;
            if (p->block_clicks && !in->lclick && !in->rclick) p->block_clicks = 0;
            if (p->block_clicks) game->lclick = game->rclick = 0;
            return 0;
        }
    }
    /* open: the controllers are the panel's */
    {
        const int b_edge = in->b && !p->b_prev, menu_edge = in->menu && !p->menu_prev;
        p->b_prev = in->b;
        p->menu_prev = in->menu;
        if (!p->trig_down && (gun_trig > VRP_TRIGGER_DOWN || gun_lo)) p->trig_down = 1;
        else if (p->trig_down && gun_trig < VRP_TRIGGER_UP && !gun_lo) p->trig_down = 0;
        st->mouse_down = p->trig_down;
        {
            const float sy = fabsf(in->ry) > fabsf(in->ly) ? in->ry : in->ly;
            const float a = fabsf(sy);
            if (a > VRP_SCROLL_DEADZONE && isfinite(sy) && dt > 0.0 && dt < 0.25)
                st->wheel = (sy > 0 ? 1.0f : -1.0f) * (a - VRP_SCROLL_DEADZONE) / (1.0f - VRP_SCROLL_DEADZONE) *
                            VRP_SCROLL_RATE * (float)dt;
        }
        memset(game, 0, sizeof(*game));
        game->left_active = in->left_active;
        game->right_active = in->right_active;
        if (b_edge || menu_edge) {
            p->open = 0;
            st->closed = 1;
            p->block = vri_touch_mask(in);
            p->block_clicks = 1;
        }
    }
    return p->open;
}

/* The panel's own Close (a button on it): as B, the buttons held now stay from the game until released. */
static inline void vrp_input_close(VrpInput *p, const VrTouchState *in)
{
    if (!p->open) return;
    p->open = 0;
    p->block = vri_touch_mask(in);
    p->block_clicks = 1;
    p->trig_down = 0;
}

#endif /* VR_PANEL_MATH_H */
