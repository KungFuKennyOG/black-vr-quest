/* xr-shell: Touch -> Xbox pad mapping, snap/smooth turning, chords, blackvr.ini. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_config.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void test_axes(void)
{
    CHECK(vr_axis_to_s16(0.1f, 0.15f) == 0);
    CHECK(vr_axis_to_s16(1.0f, 0.15f) == 32767);
    CHECK(vr_axis_to_s16(-1.0f, 0.15f) == -32767);
    CHECK(vr_axis_to_s16(2.0f, 0.15f) == 32767);
    CHECK(vr_axis_to_s16(0.575f, 0.15f) > 16000 && vr_axis_to_s16(0.575f, 0.15f) < 16800);
    CHECK(vr_axis_to_s16(0.0f / 0.0f, 0.15f) == 0);
    CHECK(vr_trigger_to_u8(0.0f) == 0 && vr_trigger_to_u8(1.0f) == 255 && vr_trigger_to_u8(0.5f) == 128);
}

/* A layout as the game's live action map gives it (slot -> control, BLACK's numbering): the default preset's
 * button actions, and a Southpaw-like one (move on the right stick, crouch and aim swapped to the other clicks). */
static int s_layout;
static int resolve(int slot)
{
    switch (slot) {
    case 1: return s_layout ? 8 : 4;              /* move forward: left (4) / right (8) stick up */
    case 9: return 23;                            /* fire: RT */
    case 11: return 16;                           /* reload: A */
    case 12: return s_layout ? 15 : 14;           /* crouch: L3 / R3 */
    case 13: return 18;                           /* use: X */
    case 14: return 3; case 15: return 2;         /* switch weapon: d-pad right / left */
    case 25: return 12; case 36: return 13;       /* pause START, back BACK */
    case 27: return 22;                           /* grenade: LT */
    case 30: return 0; case 31: return 19;        /* fire mode d-pad up, suppressor Y */
    case 32: return 17; case 33: return 1;        /* melee B, health pack d-pad down */
    case 35: return 0;                            /* the scope's zoom level: d-pad up */
    default: return -1;
    }
}

static VrPadState run(const VrInputConfig *c, VrInputRuntime *rt, const VrTouchState *t, const VrInputContext *x,
                      double dt, VrActionState *a_out)
{
    VrActionState a;
    VrPadState p;
    vr_map_actions(c, rt, t, x, dt, &a);
    vr_actions_to_pad(&a, resolve, &p);
    if (a_out) *a_out = a;
    return p;
}
/* a runtime already in gameplay (buttons held at a mode change are ignored until released) */
static void reset_rt(VrInputRuntime *rt) { memset(rt, 0, sizeof(*rt)); rt->was_in_game = 1; }
static VrInputContext game_ctx(void) { VrInputContext x; memset(&x, 0, sizeof(x)); x.in_game = 1; return x; }
/* frames until a held/tap action shows, how many frames it stays */
static int held_frames(const VrInputConfig *c, VrInputRuntime *rt, VrTouchState *t, const VrInputContext *x, double dt,
                       int frames, uint16_t bit)
{
    int i, on = 0;
    for (i = 0; i < frames; i++) { VrPadState p = run(c, rt, t, x, dt, NULL); on += !!(p.buttons & bit); }
    return on;
}

static void test_buttons(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrInputContext x = game_ctx();
    VrActionState a;
    VrPadState p;
    uint16_t digital;
    uint8_t analog[8];
    vr_input_default_config(&c);
    reset_rt(&rt);
    memset(&t, 0, sizeof(t));
    s_layout = 0;
    /* right hand: trigger fire (analog), A reload, B nothing (melee is the swing), grip nothing; left: trigger grenade,
     * X health pack */
    t.rt = 1.0f; t.lt = 0.25f; t.a = 1; t.b = 1; t.x = 1; t.rgrip = 1.0f; t.lgrip = 1.0f;
    t.ly = 1.0f;
    p = run(&c, &rt, &t, &x, 1.0 / 72.0, &a);
    CHECK(a.in_game && (a.held & VRA_BIT(VRA_FIRE)) && (a.held & VRA_BIT(VRA_RELOAD)) && !(a.held & VRA_BIT(VRA_MELEE)));
    CHECK((a.held & VRA_BIT(VRA_GRENADE)) && (a.held & VRA_BIT(VRA_HEALTH_PACK)));
    CHECK(p.right_trigger == 255 && p.left_trigger == 64);
    CHECK((p.buttons & VR_PAD_A) && (p.buttons & VR_PAD_DPAD_DOWN));
    CHECK(!(p.buttons & (VR_PAD_B | VR_PAD_X | VR_PAD_Y | VR_PAD_LEFT_SHOULDER | VR_PAD_RIGHT_SHOULDER | VR_PAD_START)));
    CHECK(p.thumb_ly == 32767 && p.thumb_rx == 0 && p.thumb_ry == 0);
    vr_pad_to_xbox(&p, &digital, analog);
    CHECK(digital == VR_PAD_DPAD_DOWN && analog[0] == 255 && analog[1] == 0 && analog[2] == 0 && analog[4] == 0);
    CHECK(analog[6] == 64 && analog[7] == 255);
    /* B (held since the run above) toggled the laser sight once, on its press only */
    {
        VrActionState a2;
        run(&c, &rt, &t, &x, 1.0 / 72.0, &a2);
        CHECK(!a2.laser_toggle);
        t.b = 0;
        run(&c, &rt, &t, &x, 1.0 / 72.0, &a2);
        CHECK(!a2.laser_toggle);
        t.b = 1;
        run(&c, &rt, &t, &x, 1.0 / 72.0, &a2);
        CHECK(a2.laser_toggle);
    }
    /* head-relative locomotion: the stick turned 90 degrees left becomes strafe-left */
    x.move_rotate_deg = 90.0;
    p = run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    CHECK(p.thumb_lx < -32000 && abs(p.thumb_ly) < 200);
    x.move_rotate_deg = 0.0;
    /* Southpaw-like layout: the move goes to the right stick, crouch to R3; actions follow the live map */
    s_layout = 1;
    memset(&t, 0, sizeof(t));
    reset_rt(&rt);
    t.ly = 1.0f; t.lclick = 1;
    held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 6, 0);
    p = run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    CHECK(p.thumb_ry == 32767 && p.thumb_ly == 0 && (p.buttons & VR_PAD_RIGHT_THUMB) && !(p.buttons & VR_PAD_LEFT_THUMB));
    s_layout = 0;
    /* fire on a button (a remapped layout): pressed past half the trigger only */
    {
        VrActionState fa;
        memset(&fa, 0, sizeof(fa));
        fa.in_game = 1; fa.held = VRA_BIT(VRA_FIRE); fa.fire = 0.3f;
        s_layout = 0;
        vr_actions_to_pad(&fa, resolve, &p);
        CHECK(p.right_trigger == 77);                 /* analog on RT */
    }
    /* no resolver yet: nothing pressed, the move still on the left stick */
    vr_actions_to_pad(&a, NULL, &p);
    CHECK(p.buttons == 0 && p.right_trigger == 0 && p.thumb_ly == 32767);
}

/* stick click = Use (right) / Crouch (left), Y tap / hold, Menu tap / hold */
static void test_taps(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrInputContext x = game_ctx();
    const double dt = 1.0 / 72.0;
    int on, i;
    vr_input_default_config(&c);
    reset_rt(&rt);
    memset(&t, 0, sizeof(t));
    /* right stick click: Use (X in this layout), after the chord delay, held while clicked */
    t.rclick = 1;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 4, VR_PAD_X) == 0);          /* 4 frames < 60 ms */
    CHECK(held_frames(&c, &rt, &t, &x, dt, 20, VR_PAD_X) == 20);
    t.rclick = 0;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 2, VR_PAD_X) == 0);
    /* left stick click: Crouch (L3) */
    t.lclick = 1;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 10, VR_PAD_LEFT_THUMB) >= 5);
    t.lclick = 0;
    run(&c, &rt, &t, &x, dt, NULL);
    /* Y tap: Suppressor (Y here) on release, a pulse of ~0.15 s; no Fire mode */
    t.y = 1;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 10, VR_PAD_Y | VR_PAD_DPAD_UP) == 0);
    t.y = 0;
    on = held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_Y);
    CHECK(on >= 10 && on <= 12);
    CHECK(held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_DPAD_UP) == 0);
    /* Y held 0.5 s: Fire mode (d-pad up) once at 0.5 s, nothing on release */
    t.y = 1;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 35, VR_PAD_DPAD_UP) == 0);   /* 0.49 s */
    on = held_frames(&c, &rt, &t, &x, dt, 60, VR_PAD_DPAD_UP);
    CHECK(on >= 10 && on <= 12);
    t.y = 0;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_Y | VR_PAD_DPAD_UP) == 0);
    /* Menu tap: Pause (START); Menu held 1 s: Back, no START */
    t.menu = 1;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 20, VR_PAD_START | VR_PAD_BACK) == 0);
    t.menu = 0;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 20, VR_PAD_START) >= 10);
    t.menu = 1;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 71, VR_PAD_BACK) == 0);
    CHECK(held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_BACK) >= 10);
    t.menu = 0;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_START | VR_PAD_BACK) == 0);
    /* a melee swing: one B pulse; two swings in a row are two presses */
    x.melee_swing = 1;
    run(&c, &rt, &t, &x, dt, NULL);
    run(&c, &rt, &t, &x, dt, NULL);
    x.melee_swing = 0;
    {
        int edges = 0, last = 1;
        for (i = 0; i < 60; i++) {
            const int b = !!(run(&c, &rt, &t, &x, dt, NULL).buttons & VR_PAD_B);
            if (b && !last) edges++;
            last = b;
        }
        CHECK(edges == 1);                           /* the first pulse began above; the queued one follows a gap */
    }
    /* a slow game (2 s per frame): a tap lasts until the game has run 2 input updates, and so does its gap */
    {
        int on1 = 0, on2 = 0, on3 = 0;
        x.ticks_valid = 1; x.game_ticks = 100;
        t.y = 1; run(&c, &rt, &t, &x, dt, NULL); t.y = 0;           /* a Suppressor tap */
        on1 = held_frames(&c, &rt, &t, &x, dt, 100, VR_PAD_Y);      /* 1.4 s, no game update: held all along */
        x.game_ticks++;
        on2 = held_frames(&c, &rt, &t, &x, dt, 10, VR_PAD_Y);       /* one update: still held */
        x.game_ticks++;
        on3 = held_frames(&c, &rt, &t, &x, dt, 10, VR_PAD_Y);       /* two: released */
        CHECK(on1 == 100 && on2 == 10 && on3 == 0);
        x.ticks_valid = 0;
    }
}

/* the right stick: x turns, up/down = one weapon step per push with hysteresis; in the scope: its zoom */
static void test_weapon_step(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrInputContext x = game_ctx();
    const double dt = 1.0 / 72.0;
    vr_input_default_config(&c);
    reset_rt(&rt);
    memset(&t, 0, sizeof(t));
    run(&c, &rt, &t, &x, dt, NULL);                  /* centred: armed */
    t.ry = 0.8f; t.rx = 0.2f;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 60, VR_PAD_DPAD_RIGHT) >= 10);   /* next weapon, once */
    CHECK(held_frames(&c, &rt, &t, &x, dt, 60, VR_PAD_DPAD_RIGHT) == 0);    /* held: no repeat */
    CHECK(rt.turn_accum_deg == 0.0);                 /* a vertical push never turns */
    t.ry = 0.5f;                                     /* not below the re-arm line */
    held_frames(&c, &rt, &t, &x, dt, 5, 0);
    t.ry = 0.8f;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_DPAD_RIGHT) == 0);
    t.ry = 0.1f; held_frames(&c, &rt, &t, &x, dt, 2, 0);
    t.ry = -0.9f;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_DPAD_LEFT) >= 10);    /* previous weapon */
    t.ry = 0.0f; held_frames(&c, &rt, &t, &x, dt, 2, 0);
    /* two quick pushes: two steps */
    {
        int i, edges = 0, last = 0;
        for (i = 0; i < 120; i++) {
            int b;
            t.ry = (i < 3 || (i >= 6 && i < 9)) ? 0.9f : 0.0f;
            b = !!(run(&c, &rt, &t, &x, dt, NULL).buttons & VR_PAD_DPAD_RIGHT);
            if (b && !last) edges++;
            last = b;
        }
        CHECK(edges == 2);
    }
    /* the scope is up: up steps its zoom level (slot 35, d-pad up here), not the weapon */
    x.scope_up = 1;
    t.ry = 0.9f;
    CHECK(held_frames(&c, &rt, &t, &x, dt, 30, VR_PAD_DPAD_UP) >= 10);
    x.scope_up = 0;
    t.ry = 0.0f;
    held_frames(&c, &rt, &t, &x, dt, 30, 0);
    /* sideways: turns as before */
    t.rx = 0.9f;
    held_frames(&c, &rt, &t, &x, dt, 5, 0);
    CHECK(rt.turn_accum_deg == -30.0);
    /* the 3D scope view: no stick turning, the walk at a third */
    t.rx = 0.0f;
    held_frames(&c, &rt, &t, &x, dt, 10, 0);
    x.scope_view = 1;
    t.rx = 0.9f;
    held_frames(&c, &rt, &t, &x, dt, 10, 0);
    CHECK(rt.turn_accum_deg == -30.0);
    t.rx = 0.0f;
    t.ly = 0.9f;
    {
        VrActionState a;
        VrPadState p = run(&c, &rt, &t, &x, dt, &a);
        CHECK(a.move_y > 0 && a.move_y < 32767 / 2);
        (void)p;
        x.scope_view = 0;
        run(&c, &rt, &t, &x, dt, &a);
        CHECK(a.move_y > 32767 * 3 / 4);
    }
    t.ly = 0.0f;
}

static void test_snap_turn(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrInputContext x = game_ctx(), menu;
    VrPadState p;
    int i;
    vr_input_default_config(&c);
    reset_rt(&rt);
    memset(&t, 0, sizeof(t));
    memset(&menu, 0, sizeof(menu));
    run(&c, &rt, &t, &x, 0.014, NULL);              /* centred: arms */
    t.rx = 0.9f;
    for (i = 0; i < 30; i++) run(&c, &rt, &t, &x, 0.014, NULL);
    CHECK(rt.turn_accum_deg == -30.0);                  /* one snap right, held stick does not repeat */
    t.rx = 0.5f;                                        /* between off and on: still disarmed */
    run(&c, &rt, &t, &x, 0.014, NULL);
    t.rx = 0.9f;
    run(&c, &rt, &t, &x, 0.014, NULL);
    CHECK(rt.turn_accum_deg == -30.0);
    t.rx = 0.0f; run(&c, &rt, &t, &x, 0.014, NULL);
    t.rx = -1.0f; run(&c, &rt, &t, &x, 0.014, NULL);
    CHECK(rt.turn_accum_deg == 0.0);                    /* and one left */
    /* menus: right stick passes to the game and the d-pad, no turning */
    t.rx = 1.0f; t.ry = 0.0f;
    p = run(&c, &rt, &t, &menu, 0.014, NULL);
    CHECK(rt.turn_accum_deg == 0.0 && p.thumb_rx == 32767 && (p.buttons & VR_PAD_DPAD_RIGHT));
    /* coming back from a menu with the stick held does not snap */
    run(&c, &rt, &t, &x, 0.014, NULL);
    CHECK(rt.turn_accum_deg == 0.0);
}

static void test_smooth_turn(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    double total = 0;
    int i;
    vr_input_default_config(&c);
    c.turn_mode = VR_TURN_SMOOTH;
    reset_rt(&rt);
    for (i = 0; i < 72; i++) total += vr_turn_step(&c, &rt, -1.0f, 1.0 / 72.0);
    CHECK(total > 119.0 && total < 121.0);              /* full left for a second = 120 degrees left */
    CHECK(vr_turn_step(&c, &rt, -1.0f, 1.0) == 0.0);    /* a hitch does not spin the player */
    c.turn_mode = VR_TURN_OFF;
    CHECK(vr_turn_step(&c, &rt, 1.0f, 0.1) == 0.0);
}

static void test_chords(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrInputContext x = game_ctx();
    VrPadState p;
    int i;
    vr_input_default_config(&c);
    reset_rt(&rt);
    memset(&t, 0, sizeof(t));
    /* both clicks together (one a frame early): neither Crouch nor Use ever reaches the pad; recentre at 1 s */
    t.lclick = 1;
    p = run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    CHECK(!(p.buttons & VR_PAD_LEFT_THUMB));
    t.rclick = 1;
    for (i = 0; i < 70; i++) {
        p = run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
        CHECK(!(p.buttons & (VR_PAD_LEFT_THUMB | VR_PAD_X)));
    }
    CHECK(!rt.recentre_request);
    for (i = 0; i < 4; i++) run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    CHECK(rt.recentre_request == 1);
    rt.recentre_request = 0;
    for (i = 0; i < 100; i++) run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    CHECK(rt.recentre_request == 0);                    /* once per hold */
    t.rclick = 0;                                       /* one let go first: the other still does nothing */
    for (i = 0; i < 20; i++) CHECK(!(run(&c, &rt, &t, &x, 1.0 / 72.0, NULL).buttons & VR_PAD_LEFT_THUMB));
    t.lclick = 0;
    run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    /* the chord in a menu: no L3/R3 either */
    {
        VrInputContext menu;
        memset(&menu, 0, sizeof(menu));
        t.lclick = t.rclick = 1;
        for (i = 0; i < 80; i++) CHECK(!(run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL).buttons & (VR_PAD_LEFT_THUMB | VR_PAD_RIGHT_THUMB)));
        CHECK(rt.recentre_request == 1);
        rt.recentre_request = 0;
        t.lclick = t.rclick = 0;
        run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL);
    }
}

/* left_handed mirrors the scheme */
static void test_left_handed(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrInputContext x = game_ctx();
    VrActionState a;
    VrPadState p;
    vr_input_default_config(&c);
    c.left_handed = 1;
    reset_rt(&rt);
    memset(&t, 0, sizeof(t));
    t.lt = 1.0f;          /* gun hand trigger: fire */
    t.rt = 0.5f;          /* off hand trigger: grenade */
    t.x = 1;              /* gun hand lower button: reload */
    t.a = 1;              /* off hand lower button: health pack */
    t.ry = 1.0f;          /* off hand stick: move */
    p = run(&c, &rt, &t, &x, 1.0 / 72.0, &a);
    CHECK((a.held & VRA_BIT(VRA_FIRE)) && a.fire == 1.0f && a.grenade == 0.5f);
    CHECK((a.held & VRA_BIT(VRA_RELOAD)) && (a.held & VRA_BIT(VRA_HEALTH_PACK)) && !(a.held & VRA_BIT(VRA_MELEE)));
    CHECK(p.right_trigger == 255 && p.left_trigger == 128 && p.thumb_ly == 32767);
    /* the gun hand's stick (left) steps weapons and turns */
    memset(&t, 0, sizeof(t));
    run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    t.ly = 0.9f;
    CHECK(held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 20, VR_PAD_DPAD_RIGHT) >= 10);
    t.ly = 0.0f;
    run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);          /* centred: snap re-armed */
    rt.turn_accum_deg = 0.0;
    t.lx = 1.0f;
    held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 3, 0);
    CHECK(rt.turn_accum_deg == -30.0);
    /* gun hand click (left) = Use, off hand click (right) = Crouch */
    memset(&t, 0, sizeof(t));
    t.lclick = 1;
    CHECK(held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 10, VR_PAD_X) >= 5);
    t.lclick = 0; run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    t.rclick = 1;
    CHECK(held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 10, VR_PAD_LEFT_THUMB) >= 5);
    t.rclick = 0;
    /* B (off hand upper) tapped: suppressor */
    run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    t.b = 1; held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 5, 0); t.b = 0;
    CHECK(held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 20, VR_PAD_Y) >= 10);
}

/* menus: the old button pad; buttons held across a mode change act only after a release */
static void test_menus(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrInputContext x = game_ctx(), menu;
    VrPadState p;
    vr_input_default_config(&c);
    memset(&rt, 0, sizeof(rt));
    memset(&t, 0, sizeof(t));
    memset(&menu, 0, sizeof(menu));
    t.a = t.b = t.x = t.y = t.menu = 1; t.rt = 1.0f; t.lgrip = 1.0f; t.rgrip = 1.0f; t.ry = 1.0f; t.lx = -1.0f;
    p = run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL);
    CHECK((p.buttons & (VR_PAD_A | VR_PAD_B | VR_PAD_X | VR_PAD_Y | VR_PAD_START)) == (VR_PAD_A | VR_PAD_B | VR_PAD_X | VR_PAD_Y | VR_PAD_START));
    CHECK((p.buttons & VR_PAD_LEFT_SHOULDER) && (p.buttons & VR_PAD_RIGHT_SHOULDER) && (p.buttons & VR_PAD_DPAD_UP));
    CHECK(p.right_trigger == 255 && p.thumb_ry == 32767 && p.thumb_lx == -32767);
    /* into gameplay with A held (it confirmed "resume"): no reload until A is let go and pressed again */
    memset(&t, 0, sizeof(t));
    t.a = 1;
    run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL);
    CHECK(!(run(&c, &rt, &t, &x, 1.0 / 72.0, NULL).buttons & VR_PAD_A));
    t.a = 0; run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    t.a = 1;
    CHECK(run(&c, &rt, &t, &x, 1.0 / 72.0, NULL).buttons & VR_PAD_A);
    /* Menu held for Back in gameplay; the objectives screen (a menu) appears while it is still held: no START */
    memset(&t, 0, sizeof(t));
    run(&c, &rt, &t, &x, 1.0 / 72.0, NULL);
    t.menu = 1;
    CHECK(held_frames(&c, &rt, &t, &x, 1.0 / 72.0, 80, VR_PAD_BACK) > 0);
    CHECK(!(run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL).buttons & VR_PAD_START));
    t.menu = 0; run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL);
    t.menu = 1;
    CHECK(run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL).buttons & VR_PAD_START);
    /* left-handed menus: sticks swap */
    c.left_handed = 1;
    memset(&t, 0, sizeof(t));
    t.ry = 1.0f;
    p = run(&c, &rt, &t, &menu, 1.0 / 72.0, NULL);
    CHECK(p.thumb_ly == 32767);
}

static void test_config(void)
{
    VrShellConfig c;
    vr_config_defaults(&c);
    CHECK(c.refresh_hz == 0.0f && c.input.turn_mode == VR_TURN_SNAP && c.game.aim_mode == VR_AIM_HAND);
    vr_config_parse(&c,
        "# comment\r\n"
        "refresh_hz = 90 ; inline\n"
        "turn = Smooth\n"
        "aim = head\n"
        "left_handed = yes\n"
        "world_scale = 2.54\n"
        "weapon_offset = 0.1 -0.2 0.35\n"
        "env RECOMP_BLACK_HZ=60\n"
        "bogus = 1\n"
        "snap_degrees = 999\n"
        "\n");
    CHECK(c.refresh_hz == 90.0f);
    CHECK(c.input.turn_mode == VR_TURN_SMOOTH);
    CHECK(c.game.aim_mode == VR_AIM_HEAD);
    CHECK(c.input.left_handed == 1 && c.game.aim_hand == 0);
    CHECK(c.game.units_per_metre > 2.53f && c.game.units_per_metre < 2.55f);
    CHECK(c.game.vm_offset_m[1] == -0.2f && c.game.vm_offset_m[2] == 0.35f);
    CHECK(c.env_count == 1 && !strcmp(c.env[0], "RECOMP_BLACK_HZ=60"));
    CHECK(c.errors == 2 && c.input.snap_deg == 30.0f);   /* unknown key + out-of-range value */
}

/* the perf knobs (port/notes/perf.md): defaults, parsing, and what reaches the game's environment */
static struct { char k[64], v[64]; int ow; } s_env[16];
static int s_nenv;
static int rec_setenv(const char *k, const char *v, int ow)
{
    int i;
    for (i = 0; i < s_nenv; i++)
        if (!strcmp(s_env[i].k, k)) { if (ow) snprintf(s_env[i].v, sizeof(s_env[i].v), "%s", v); return 0; }
    if (s_nenv < 16) {
        snprintf(s_env[s_nenv].k, 64, "%s", k); snprintf(s_env[s_nenv].v, 64, "%s", v); s_env[s_nenv].ow = ow; s_nenv++;
    }
    return 0;
}
static const char *env_of(const char *k)
{
    int i;
    for (i = 0; i < s_nenv; i++) if (!strcmp(s_env[i].k, k)) return s_env[i].v;
    return "";
}

static void test_perf_config(void)
{
    VrShellConfig c;
    vr_config_defaults(&c);
    CHECK(c.resolution_scale == 0.8f && c.foveation == VR_FOVEATION_MEDIUM && c.game_hz == 60);
    CHECK(c.raster_workers == 1 && c.raster_spin_us == 50);
    s_nenv = 0;
    vr_config_export_env(&c, rec_setenv);
    CHECK(!strcmp(env_of("RECOMP_BLACK_HZ"), "60") && !strcmp(env_of("RECOMP_BLACK_60HZ"), "1"));
    CHECK(!strcmp(env_of("RECOMP_KELVIN_THREADS"), "2") && !strcmp(env_of("RECOMP_KELVIN_SPIN_US"), "50"));

    vr_config_parse(&c, "refresh_hz = 90\nrefresh_hz = auto\n");
    CHECK(c.errors == 0 && c.refresh_hz == 0.0f);
    vr_config_parse(&c, "game_hz = 30\n");
    CHECK(c.errors == 0 && c.game_hz == 30);
    s_nenv = 0;
    vr_config_export_env(&c, rec_setenv);
    CHECK(!strcmp(env_of("RECOMP_BLACK_HZ"), "30") && !strcmp(env_of("RECOMP_BLACK_60HZ"), "0"));

    vr_config_parse(&c, "foveation = HIGH\ngame_hz = 60\nraster_workers = 3\nraster_spin_us = 250\n"
                        "resolution_scale = 0.7\n");
    CHECK(c.errors == 0 && c.foveation == VR_FOVEATION_HIGH && c.game_hz == 60);
    CHECK(c.raster_workers == 3 && c.raster_spin_us == 250 && c.resolution_scale == 0.7f);
    s_nenv = 0;
    vr_config_export_env(&c, rec_setenv);
    CHECK(!strcmp(env_of("RECOMP_BLACK_HZ"), "60") && !strcmp(env_of("RECOMP_BLACK_60HZ"), "1"));
    CHECK(!strcmp(env_of("RECOMP_KELVIN_THREADS"), "4") && !strcmp(env_of("RECOMP_KELVIN_SPIN_US"), "250"));

    vr_config_parse(&c, "foveation = off\ngame_hz = 45\nraster_workers = 5\nraster_spin_us = 1.5\n"
                        "env RECOMP_KELVIN_THREADS=1\n");
    CHECK(c.foveation == VR_FOVEATION_OFF && c.errors == 3);          /* 45 Hz, 5 workers, 1.5 us refused */
    CHECK(c.game_hz == 60 && c.raster_workers == 3 && c.raster_spin_us == 250);
    s_nenv = 0;
    vr_config_export_env(&c, rec_setenv);
    CHECK(!strcmp(env_of("RECOMP_KELVIN_THREADS"), "1"));             /* an explicit env line wins */
}

/* ---- gestures (vr_gestures.h) ---- */
static VrVec3 V(float x, float y, float z) { return vrm_v3(x, y, z); }

static void test_swing(void)
{
    VrGestureConfig gc;
    VrSwing sw;
    const double dt = 1.0 / 72.0;
    VrVec3 head = V(0, 1.6f, 0), hand = V(0.2f, 1.3f, -0.3f);
    int i, fired = 0, k;
    vrg_default_config(&gc);
    memset(&sw, 0, sizeof(sw));
    /* still: nothing */
    for (i = 0; i < 50; i++) fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0, 0, 0), 0, V(0, 0, 0), dt);
    CHECK(fired == 0);
    /* a 4 m/s swing for 0.15 s: one melee */
    for (i = 0; i < 11; i++) {
        hand.x -= (float)(4.0 * dt);
        fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0, 0, 0), 0, V(0, 0, 0), dt);
    }
    CHECK(fired == 1 && sw.last_speed > 3.9f && sw.last_speed < 4.1f);
    /* a second swing within the cooldown: nothing; after it: one more */
    for (i = 0; i < 11; i++) { hand.x += (float)(4.0 * dt); fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt); }
    CHECK(fired == 1);
    for (i = 0; i < 50; i++) vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    for (i = 0; i < 11; i++) { hand.x -= (float)(4.0 * dt); fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt); }
    CHECK(fired == 2);
    /* the runtime's velocity, when given, is used (a 3 m/s sample run, positions still) */
    for (i = 0; i < 50; i++) vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    for (i = 0; i < 5; i++) fired += vrg_swing_update(&sw, &gc, 1, hand, head, 1, V(0, 3, 0), 1, V(0, 0, 0), dt);
    CHECK(fired == 3);
    /* false positives: walking (head and hand move together at 1.5 m/s), a fast in-place turn (the hand sweeps at
     * 0.5 m radius, 180 deg/s = 1.6 m/s relative to the head), a reload-like move (1.8 m/s for 0.3 s), a one-sample
     * jitter spike (5 m/s for one 72 Hz frame), a tracking jump (40 cm in one frame) */
    for (i = 0; i < 50; i++) vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    for (i = 0; i < 144; i++) {
        head.z -= (float)(1.5 * dt); hand.z -= (float)(1.5 * dt);
        fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    }
    for (i = 0; i < 144; i++) {
        const double a = i * 3.14159265 * dt;
        const VrVec3 hp = V(head.x + 0.5f * (float)sin(a), 1.3f, head.z - 0.5f * (float)cos(a));
        fired += vrg_swing_update(&sw, &gc, 1, hp, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    }
    hand = V(head.x + 0.2f, 1.3f, head.z - 0.3f);
    for (i = 0; i < 10; i++) vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    for (i = 0; i < 22; i++) { hand.y += (float)(1.8 * dt); fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt); }
    for (k = 0; k < 3; k++) {
        hand.x += (float)(5.0 * dt);
        fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
        for (i = 0; i < 10; i++) fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    }
    hand.x += 0.4f;
    fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt);
    CHECK(fired == 3);
    /* untracked / off: nothing */
    gc.melee_swing = 0;
    for (i = 0; i < 11; i++) { hand.x -= (float)(4.0 * dt); fired += vrg_swing_update(&sw, &gc, 1, hand, head, 0, V(0,0,0), 0, V(0,0,0), dt); }
    CHECK(fired == 3);
}

static void test_duck(void)
{
    VrGestureConfig gc;
    VrDuck d;
    VrCrouchSync cs;
    int i, game = 0, hold;
    vrg_default_config(&gc);
    memset(&d, 0, sizeof(d));
    vrg_duck_calibrate(&d, 1.70f);
    CHECK(!vrg_duck_update(&d, &gc, 1, 1.60f, 0.014));          /* a nod: standing */
    CHECK(vrg_duck_update(&d, &gc, 1, 1.35f, 0.014));           /* < 0.80 x 1.70 = 1.36: ducked */
    CHECK(vrg_duck_update(&d, &gc, 1, 1.45f, 0.014));           /* between: still ducked (hysteresis) */
    CHECK(vrg_duck_update(&d, &gc, 1, 1.49f, 0.014));           /* 0.88 x 1.70 = 1.496 */
    CHECK(!vrg_duck_update(&d, &gc, 1, 1.50f, 0.014));
    CHECK(!vrg_duck_update(&d, &gc, 1, 1.37f, 0.014));
    /* calibrated while bent over (1.40): standing up taller raises the standing height (time constant 2 s) */
    vrg_duck_calibrate(&d, 1.40f);
    for (i = 0; i < 600; i++) vrg_duck_update(&d, &gc, 1, 1.75f, 0.014);
    CHECK(d.standing > 1.73f && d.standing <= 1.75f);
    CHECK(vrg_duck_update(&d, &gc, 1, 1.38f, 0.014));
    /* recalibration (recentre) */
    vrg_duck_calibrate(&d, 1.60f);
    CHECK(!d.ducked && !vrg_duck_update(&d, &gc, 1, 1.40f, 0.014) && vrg_duck_update(&d, &gc, 1, 1.27f, 0.014));
    /* no floor (LOCAL space, calibrated at ~0): an assumed 1.65 m body */
    vrg_duck_calibrate(&d, 0.0f);
    CHECK(!vrg_duck_update(&d, &gc, 1, -0.25f, 0.014) && vrg_duck_update(&d, &gc, 1, -0.34f, 0.014));
    CHECK(vrg_duck_update(&d, &gc, 1, -0.22f, 0.014) && !vrg_duck_update(&d, &gc, 1, -0.19f, 0.014));
    gc.duck_crouch = 0;
    CHECK(!vrg_duck_update(&d, &gc, 1, -1.0f, 0.014));

    /* crouch sync, Toggle Crouch: one press per body edge, only when the game differs */
    memset(&cs, 0, sizeof(cs));
    for (i = 0; i < 10; i++) CHECK(!vrg_crouch_sync(&cs, 0, game, 1, 0, 0u, 0.014));
    hold = 0;
    for (i = 0; i < 30; i++) { const int h = vrg_crouch_sync(&cs, 1, game, 1, 0, 0u, 0.014); hold += h; if (h) game = 1; }
    CHECK(cs.presses == 1 && hold >= 8 && hold <= 9 && game == 1);
    for (i = 0; i < 100; i++) CHECK(!vrg_crouch_sync(&cs, 1, game, 1, 0, 0u, 0.014));
    /* the player stands up with the stick click while still ducked: left alone */
    game = 0;
    for (i = 0; i < 100; i++) CHECK(!vrg_crouch_sync(&cs, 1, game, 1, 0, 0u, 0.014));
    /* standing up physically: the game is already standing, no press */
    for (i = 0; i < 100; i++) CHECK(!vrg_crouch_sync(&cs, 0, game, 1, 0, 0u, 0.014));
    CHECK(cs.presses == 1);
    /* ducking again, and the game misses the first press: pressed again after the verify window, max 3 tries */
    for (i = 0; i < 200; i++) vrg_crouch_sync(&cs, 1, game, 1, 0, 0u, 0.014);
    CHECK(cs.presses == 4);
    /* unknown game state: one press per edge */
    memset(&cs, 0, sizeof(cs));
    vrg_crouch_sync(&cs, 0, -1, -1, 0, 0u, 0.014);
    for (i = 0; i < 200; i++) vrg_crouch_sync(&cs, 1, -1, -1, 0, 0u, 0.014);
    for (i = 0; i < 200; i++) vrg_crouch_sync(&cs, 0, -1, -1, 0, 0u, 0.014);
    CHECK(cs.presses == 2);
    /* hold mode: held while ducked */
    memset(&cs, 0, sizeof(cs));
    CHECK(!vrg_crouch_sync(&cs, 0, 0, 0, 0, 0u, 0.014) && vrg_crouch_sync(&cs, 1, 0, 0, 0, 0u, 0.014) && vrg_crouch_sync(&cs, 1, 1, 0, 0, 0u, 0.014));
    CHECK(!vrg_crouch_sync(&cs, 0, 1, 0, 0, 0u, 0.014));
    /* a slow game: the press lasts 2 game updates, and the check waits 6 before pressing again */
    memset(&cs, 0, sizeof(cs));
    vrg_crouch_sync(&cs, 0, 0, 1, 1, 10u, 0.014);
    for (i = 0, hold = 0; i < 100; i++) hold += vrg_crouch_sync(&cs, 1, 0, 1, 1, 10u, 0.014);
    CHECK(hold == 100 && cs.presses == 1);
    hold = vrg_crouch_sync(&cs, 1, 0, 1, 1, 11u, 0.014);
    CHECK(hold == 1);
    CHECK(!vrg_crouch_sync(&cs, 1, 0, 1, 1, 12u, 0.014));          /* released after 2 updates */
    for (i = 0; i < 100; i++) CHECK(!vrg_crouch_sync(&cs, 1, 0, 1, 1, 13u, 0.014));   /* verifying (updates) */
    for (i = 0; i < 20; i++) hold = vrg_crouch_sync(&cs, 1, 0, 1, 1, 19u, 0.014);
    CHECK(cs.presses == 2);                                          /* not followed after 6 updates: again */
    /* starting a level already ducked: crouches once */
    memset(&cs, 0, sizeof(cs));
    for (i = 0; i < 30; i++) vrg_crouch_sync(&cs, 1, 0, 1, 0, 0u, 0.014);
    CHECK(cs.presses == 1);
}

static VrPose pose_look(VrVec3 pos, float yaw_deg, float pitch_deg)
{
    VrPose p;
    p.position = pos;
    p.orientation = vrm_q_mul(vrm_q_from_yaw((float)(yaw_deg * VRM_DEG)),
                              vrm_q_axis_angle(vrm_v3(1, 0, 0), (float)(pitch_deg * VRM_DEG)));
    return p;
}

static void test_scope(void)
{
    VrGestureConfig gc;
    VrScope sc;
    VrVec3 eye[2] = { { -0.032f, 1.6f, 0.0f }, { 0.032f, 1.6f, 0.0f } };
    const VrVec3 fwd = V(0, 0, -1);
    vrg_default_config(&gc);
    memset(&sc, 0, sizeof(sc));
    /* hip: no */
    CHECK(!vrg_scope_update(&sc, &gc, 1, pose_look(V(0.2f, 1.3f, -0.35f), 0, 0), eye, fwd));
    /* raised: the rear sight 8 cm in front of the right eye, pointing forward */
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 0, 0), eye, fwd));
    CHECK(sc.last_dist < 0.15f && sc.last_angle_deg < 1.0f);
    /* hysteresis: a bit further (18 cm) stays up; 25 cm drops it */
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.24f), 0, 0), eye, fwd));
    CHECK(!vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.31f), 0, 0), eye, fwd));
    /* close to the eye but pointing 45 degrees aside: no */
    CHECK(!vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 45, 0), eye, fwd));
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 10, 0), eye, fwd));
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 35, 0), eye, fwd));   /* engaged: 40 */
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 0, 0), eye, fwd));
    gc.scope_raise = 0;
    CHECK(!vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 0, 0), eye, fwd));
}

/* The 3D scope's aim (DrBeef's virtual stock) and its release by distance alone once up. */
static void test_scope_aim(void)
{
    VrGestureConfig gc;
    VrScopeAim sa;
    VrScope sc;
    VrPose head = pose_look(V(0.0f, 1.6f, 0.0f), 30, 0), gun = pose_look(V(0.03f, 1.55f, -0.12f), 0, 0);
    VrVec3 off, f, r, eye[2] = { { -0.032f, 1.6f, 0.0f }, { 0.032f, 1.6f, 0.0f } };
    VrQuat q, v;
    float yaw, pitch;
    int i;
    vrg_default_config(&gc);
    memset(&sa, 0, sizeof(sa));
    /* off: the gun hand's own orientation, nothing active */
    q = vrg_scope_aim_update(&sa, &gc, 0, head, gun, 1, V(0, 1.48f, -0.45f), 1.0 / 72.0);
    CHECK(!sa.active && fabsf(q.w - gun.orientation.w) < 1e-6f);
    /* up with the head turned 30 deg left and the stock straight ahead: it starts on the head's yaw (no jump) and is
     * level (the off hand 12 cm under the eye's line: the lift) */
    off = V(0.0f, 1.6f - 0.12f, -0.45f);
    q = vrg_scope_aim_update(&sa, &gc, 1, head, gun, 1, off, 1.0 / 72.0);
    CHECK(sa.active && sa.stock);
    f = vrm_q_rotate(q, V(0, 0, -1));
    CHECK(vrg_dir_yaw_pitch(f, &yaw, &pitch));
    CHECK(fabsf(yaw - (float)(30 * VRM_DEG)) < 1e-3f && fabsf(pitch) < 1e-3f);
    r = vrm_q_rotate(q, V(1, 0, 0));
    CHECK(fabsf(r.y) < 1e-5f);                                 /* no roll in the aim */
    /* the stock turned 10 deg right (the off hand moved): the aim turns 10 deg right from where it began, after the
     * smoothing settles; the gun hand's own turn does not count */
    off = vrm_add(V(0.0f, 1.6f, 0.0f), V(sinf((float)(10 * VRM_DEG)) * 0.45f, -0.12f, -cosf((float)(10 * VRM_DEG)) * 0.45f));
    gun = pose_look(gun.position, -40, 15);
    for (i = 0; i < 100; i++) q = vrg_scope_aim_update(&sa, &gc, 1, head, gun, 1, off, 1.0 / 72.0);
    f = vrm_q_rotate(q, V(0, 0, -1));
    CHECK(vrg_dir_yaw_pitch(f, &yaw, &pitch));
    CHECK(fabsf(yaw - (float)(20 * VRM_DEG)) < 2e-3f && fabsf(pitch) < 2e-3f);
    /* the smoothing: one update after a 2 deg step of the off hand moves the aim only part of the way */
    {
        float y0 = yaw, y1;
        const VrVec3 off2 = vrm_add(V(0.0f, 1.6f, 0.0f), V(sinf((float)(12 * VRM_DEG)) * 0.45f, -0.12f, -cosf((float)(12 * VRM_DEG)) * 0.45f));
        q = vrg_scope_aim_update(&sa, &gc, 1, head, gun, 1, off2, 1.0 / 72.0);
        CHECK(vrg_dir_yaw_pitch(vrm_q_rotate(q, V(0, 0, -1)), &y1, &pitch));
        CHECK(y1 < y0 && y1 > y0 - (float)(2 * VRM_DEG) * 0.5f);
        off = off2;
    }
    /* the off hand lost: the last aim is held */
    {
        float y0, y1;
        CHECK(vrg_dir_yaw_pitch(vrm_q_rotate(q, V(0, 0, -1)), &y0, &pitch));
        q = vrg_scope_aim_update(&sa, &gc, 1, head, gun, 0, V(5, 5, 5), 1.0 / 72.0);
        CHECK(vrg_dir_yaw_pitch(vrm_q_rotate(q, V(0, 0, -1)), &y1, &pitch));
        CHECK(fabsf(y1 - y0) < 1e-5f);
    }
    /* down and up again: it starts from the head's yaw again */
    vrg_scope_aim_update(&sa, &gc, 0, head, gun, 1, off, 1.0 / 72.0);
    CHECK(!sa.active);
    /* one-handed (no stock): the gun hand's ray, relative too */
    q = vrg_scope_aim_update(&sa, &gc, 1, head, pose_look(gun.position, 0, 5), 0, off, 1.0 / 72.0);
    CHECK(sa.active && !sa.stock);
    CHECK(vrg_dir_yaw_pitch(vrm_q_rotate(q, V(0, 0, -1)), &yaw, &pitch));
    CHECK(fabsf(yaw - (float)(30 * VRM_DEG)) < 1e-3f && fabsf(pitch - (float)(5 * VRM_DEG)) < 1e-3f);
    /* the view: the aim with the head's roll (the head rolled 20 deg), its horizon level in the world */
    head.orientation = vrm_q_mul(pose_look(V(0, 0, 0), 30, 0).orientation, vrm_q_axis_angle(V(0, 0, 1), (float)(20 * VRM_DEG)));
    CHECK(fabsf(vrg_head_roll(head.orientation) - (float)(20 * VRM_DEG)) < 1e-4f);
    v = vrg_scope_view_rotation(q, head.orientation);
    CHECK(vrm_dot(vrm_q_rotate(v, V(0, 0, -1)), vrm_q_rotate(q, V(0, 0, -1))) > 0.99999f);
    CHECK(fabsf(vrg_head_roll(v) - (float)(20 * VRM_DEG)) < 1e-4f);
    /* release: up (with the angle rule), then free_aim: the head turned 60 deg off the gun keeps it; distance drops it */
    memset(&sc, 0, sizeof(sc));
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 0, 0), eye, V(0, 0, -1)));
    sc.free_aim = 1;
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.14f), 60, 0), eye, V(0, 0, -1)));
    CHECK(vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.26f), 0, 0), eye, V(0, 0, -1)));   /* 0.24 m */
    CHECK(!vrg_scope_update(&sc, &gc, 1, pose_look(V(0.032f, 1.55f, -0.34f), 0, 0), eye, V(0, 0, -1)));
}

static void test_two_hand(void)
{
    VrGestureConfig gc;
    VrTwoHand th;
    VrPose gun = pose_look(V(0.15f, 1.3f, -0.3f), -10, 0), out;
    /* the gun hand rolled 30 degrees */
    VrVec3 off, f, up;
    int i;
    gun.orientation = vrm_q_mul(gun.orientation, vrm_q_axis_angle(V(0, 0, 1), (float)(30 * VRM_DEG)));
    vrg_default_config(&gc);
    memset(&th, 0, sizeof(th));
    /* the off hand 35 cm ahead and a little up/left of the gun: grip held engages (within 20 cm of the point
     * 30 cm ahead) */
    off = vrm_add(gun.position, V(-0.05f, 0.08f, -0.33f));
    for (i = 0; i < 20; i++) out = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 1, 1.0 / 72.0);
    CHECK(th.engaged && th.blend == 1.0f);
    f = vrm_q_rotate(out.orientation, V(0, 0, -1));
    {
        VrVec3 want = vrm_sub(off, gun.position);
        vrm_normalize(&want);
        CHECK(vrm_dot(f, want) > 0.99999f);
    }
    /* the roll is the gun hand's: the new up is the gun's up made perpendicular to the new forward */
    up = vrm_q_rotate(out.orientation, V(0, 1, 0));
    {
        VrVec3 gu = vrm_q_rotate(gun.orientation, V(0, 1, 0));
        gu = vrm_sub(gu, vrm_scale(f, vrm_dot(gu, f)));
        vrm_normalize(&gu);
        CHECK(vrm_dot(up, gu) > 0.9999f && fabsf(vrm_dot(up, f)) < 1e-4f);
    }
    CHECK(vrm_len(vrm_sub(out.position, gun.position)) < 1e-6f);
    /* jitter of the gun hand's orientation does not move the two-handed aim */
    {
        VrPose j = gun;
        VrVec3 f2;
        j.orientation = vrm_q_mul(gun.orientation, vrm_q_axis_angle(V(0, 1, 0), (float)(2 * VRM_DEG)));
        out = vrg_two_hand_update(&th, &gc, 1, j, 1, off, 1, 1.0 / 72.0);
        f2 = vrm_q_rotate(out.orientation, V(0, 0, -1));
        CHECK(vrm_dot(f, f2) > 0.99999f);
    }
    /* held, the off hand strays 70 deg off the controller's ray and to 1 m: still held, the aim follows it */
    {
        const VrVec3 far = vrm_add(gun.position, vrm_q_rotate(gun.orientation, V(-0.94f, 0.0f, -0.34f)));
        VrVec3 want = vrm_sub(far, gun.position), f3;
        out = vrg_two_hand_update(&th, &gc, 1, gun, 1, far, 1, 1.0 / 72.0);
        f3 = vrm_q_rotate(out.orientation, V(0, 0, -1));
        vrm_normalize(&want);
        CHECK(th.engaged && vrm_dot(f3, want) > 0.9999f);
        out = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 1, 1.0 / 72.0);
        CHECK(th.engaged);
    }
    /* a one-update drop (a tracking blip, the AK's flick on the headset): still held, on the off hand's last place */
    {
        const VrPose before = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 1, 1.0 / 72.0);
        out = vrg_two_hand_update(&th, &gc, 1, gun, 0, V(0, 0, 0), 1, 1.0 / 72.0);
        CHECK(th.engaged && vrm_dot(vrm_q_rotate(out.orientation, V(0, 0, -1)),
                                    vrm_q_rotate(before.orientation, V(0, 0, -1))) > 0.99999f);
        out = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 1, 1.0 / 72.0);
        CHECK(th.engaged && th.miss == 0 && th.blend == 1.0f);
    }
    /* released: one-handed again after 3 updates (25 ms at 120 Hz) */
    for (i = 0; i < 3; i++) out = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 0, 1.0 / 72.0);
    CHECK(th.engaged);
    out = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 0, 1.0 / 72.0);
    CHECK(!th.engaged && !memcmp(&out, &gun, sizeof(out)));
    /* grip held far away (60 cm to the side): no */
    for (i = 0; i < 5; i++) out = vrg_two_hand_update(&th, &gc, 1, gun, 1, vrm_add(gun.position, V(-0.6f, 0, 0)), 1, 1.0 / 72.0);
    CHECK(!th.engaged);
    /* close (12 cm) and ahead: engages, blending over 0.1 s */
    off = vrm_add(gun.position, vrm_q_rotate(gun.orientation, V(0, 0.02f, -0.12f)));
    out = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 1, 1.0 / 72.0);
    CHECK(th.engaged && th.blend > 0.1f && th.blend < 0.2f);
    /* the off hand slides behind the gun hand: disengages */
    out = vrg_two_hand_update(&th, &gc, 1, gun, 1, vrm_add(gun.position, vrm_q_rotate(gun.orientation, V(0, 0, 0.1f))), 1, 1.0 / 72.0);
    CHECK(!th.engaged);
    gc.two_hand = 0;
    out = vrg_two_hand_update(&th, &gc, 1, gun, 1, off, 1, 1.0 / 72.0);
    CHECK(!th.engaged);
    /* look rotation basics: forward -z, up +y is the identity */
    {
        const VrQuat q = vrg_look_rotation(V(0, 0, -1), V(0, 1, 0));
        CHECK(fabsf(q.w) > 0.99999f);
        const VrQuat q2 = vrg_look_rotation(V(1, 0, 0), V(0, 1, 0));
        CHECK(vrm_dot(vrm_q_rotate(q2, V(0, 0, -1)), V(1, 0, 0)) > 0.99999f);
    }
}

static void test_gestures(void)
{
    test_swing();
    test_duck();
    test_scope();
    test_scope_aim();
    test_two_hand();
}

static void test_new_config(void)
{
    VrShellConfig c;
    vr_config_defaults(&c);
    CHECK(c.locomotion == VR_LOCO_HEAD && c.input.accuracy == VR_ACCURACY_AIM);
    CHECK(c.gest.melee_swing && c.gest.swing_speed == 2.5f && c.gest.duck_crouch && c.gest.duck_ratio == 0.80f);
    CHECK(c.gest.scope_raise && c.gest.scope_distance == 0.15f && c.gest.two_hand);
    vr_config_parse(&c,
        "locomotion = offhand\n"
        "vr_accuracy = hip\n"
        "melee_swing = off\n"
        "melee_swing_speed = 3.2\n"
        "duck_crouch = 0\n"
        "duck_ratio = 0.75 ; comment\n"
        "scope_raise = no\n"
        "scope_raise_distance = 0.2\n"
        "two_hand = off\n");
    CHECK(c.errors == 0 && c.locomotion == VR_LOCO_OFFHAND && c.input.accuracy == VR_ACCURACY_HIP);
    CHECK(!c.gest.melee_swing && c.gest.swing_speed == 3.2f && !c.gest.duck_crouch && c.gest.duck_ratio == 0.75f);
    CHECK(!c.gest.scope_raise && c.gest.scope_distance == 0.2f && !c.gest.two_hand);
    vr_config_parse(&c, "locomotion = aim\nmelee_swing_speed = 50\nduck_ratio = 0.2\nvr_accuracy = laser\n");
    CHECK(c.errors == 3 && c.locomotion == VR_LOCO_AIM && c.gest.swing_speed == 3.2f);
}

int main(void)
{
    test_axes();
    test_buttons();
    test_taps();
    test_weapon_step();
    test_snap_turn();
    test_smooth_turn();
    test_chords();
    test_left_handed();
    test_menus();
    test_gestures();
    test_new_config();
    test_config();
    test_perf_config();
    printf("xr_shell input: %u checks passed\n", checks);
    return 0;
}
