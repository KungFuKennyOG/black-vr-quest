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

static void test_buttons(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrPadState p;
    uint16_t digital;
    uint8_t analog[8];
    vr_input_default_config(&c);
    memset(&rt, 0, sizeof(rt));
    memset(&t, 0, sizeof(t));
    t.a = t.y = t.menu = 1;
    t.rt = 1.0f; t.lt = 0.25f;
    t.rgrip = 0.9f;
    t.ly = 1.0f;
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK(p.buttons & VR_PAD_A);
    CHECK(p.buttons & VR_PAD_Y);
    CHECK(p.buttons & VR_PAD_START);
    CHECK(p.buttons & VR_PAD_LEFT_SHOULDER);         /* right grip -> Black */
    CHECK(!(p.buttons & VR_PAD_RIGHT_SHOULDER));
    CHECK(p.right_trigger == 255 && p.left_trigger == 64);
    CHECK(p.thumb_ly == 32767 && p.thumb_rx == 0 && p.thumb_ry == 0);
    vr_pad_to_xbox(&p, &digital, analog);
    CHECK(digital == VR_PAD_START && analog[0] == 255 && analog[3] == 255 && analog[4] == 255 && analog[5] == 0);
    CHECK(analog[6] == 64 && analog[7] == 255);
    /* head-relative locomotion: the stick turned 90 degrees left becomes strafe-left */
    vr_map_pad(&c, &rt, &t, 1, 90.0, 1.0 / 72.0, &p);
    CHECK(p.thumb_lx < -32000 && abs(p.thumb_ly) < 200);

    /* gameplay d-pad: right stick up -> d-pad RIGHT (switch weapon), down -> DOWN (health pack), no turn */
    memset(&rt, 0, sizeof(rt)); memset(&t, 0, sizeof(t));
    t.ry = 0.9f; t.rx = 0.2f;
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK((p.buttons & VR_PAD_DPAD_RIGHT) && !(p.buttons & (VR_PAD_DPAD_UP | VR_PAD_DPAD_DOWN | VR_PAD_DPAD_LEFT)));
    CHECK(rt.turn_accum_deg == 0.0 && p.thumb_ry == 0);
    t.ry = -0.9f; t.rx = 0.0f;
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK((p.buttons & VR_PAD_DPAD_DOWN) && !(p.buttons & VR_PAD_DPAD_RIGHT));
    /* left grip + right stick: the full d-pad, no turning, and no White while it is used */
    memset(&rt, 0, sizeof(rt)); memset(&t, 0, sizeof(t));
    t.lgrip = 1.0f; t.ry = 0.9f;
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK((p.buttons & VR_PAD_DPAD_UP) && !(p.buttons & VR_PAD_RIGHT_SHOULDER));
    t.ry = 0.0f; t.rx = -1.0f;
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK((p.buttons & VR_PAD_DPAD_LEFT) && rt.turn_accum_deg == 0.0);
    t.lgrip = 0.0f;                                     /* released while the stick is still pushed: no snap */
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK(rt.turn_accum_deg == 0.0 && !(p.buttons & VR_PAD_RIGHT_SHOULDER));
    /* a grip tap without the stick: White as a short pulse on release */
    memset(&rt, 0, sizeof(rt)); memset(&t, 0, sizeof(t));
    t.lgrip = 1.0f;
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK(!(p.buttons & VR_PAD_RIGHT_SHOULDER));
    t.lgrip = 0.0f;
    vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p);
    CHECK(p.buttons & VR_PAD_RIGHT_SHOULDER);
    {
        int k, on = 0;
        for (k = 0; k < 20; k++) { vr_map_pad(&c, &rt, &t, 1, 0.0, 1.0 / 72.0, &p); on += !!(p.buttons & VR_PAD_RIGHT_SHOULDER); }
        CHECK(on >= 6 && on <= 9);                      /* ~0.12 s at 72 Hz, then off */
    }
    /* menus: the left grip is White while held, as before */
    t.lgrip = 1.0f;
    vr_map_pad(&c, &rt, &t, 0, 0.0, 1.0 / 72.0, &p);
    CHECK(p.buttons & VR_PAD_RIGHT_SHOULDER);
}

static void test_snap_turn(void)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrTouchState t;
    VrPadState p;
    int i;
    vr_input_default_config(&c);
    memset(&rt, 0, sizeof(rt));
    memset(&t, 0, sizeof(t));
    vr_map_pad(&c, &rt, &t, 1, 0, 0.014, &p);           /* centred: arms */
    t.rx = 0.9f;
    for (i = 0; i < 30; i++) vr_map_pad(&c, &rt, &t, 1, 0, 0.014, &p);
    CHECK(rt.turn_accum_deg == -30.0);                  /* one snap right, held stick does not repeat */
    t.rx = 0.5f;                                        /* between off and on: still disarmed */
    vr_map_pad(&c, &rt, &t, 1, 0, 0.014, &p);
    t.rx = 0.9f;
    vr_map_pad(&c, &rt, &t, 1, 0, 0.014, &p);
    CHECK(rt.turn_accum_deg == -30.0);
    t.rx = 0.0f; vr_map_pad(&c, &rt, &t, 1, 0, 0.014, &p);
    t.rx = -1.0f; vr_map_pad(&c, &rt, &t, 1, 0, 0.014, &p);
    CHECK(rt.turn_accum_deg == 0.0);                    /* and one left */
    /* menus: right stick passes to the game and the d-pad, no turning */
    t.rx = 1.0f; t.ry = 0.0f;
    vr_map_pad(&c, &rt, &t, 0, 0, 0.014, &p);
    CHECK(rt.turn_accum_deg == 0.0 && p.thumb_rx == 32767 && (p.buttons & VR_PAD_DPAD_RIGHT));
    /* coming back from a menu with the stick held does not snap */
    vr_map_pad(&c, &rt, &t, 1, 0, 0.014, &p);
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
    memset(&rt, 0, sizeof(rt));
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
    VrPadState p;
    int i;
    vr_input_default_config(&c);
    memset(&rt, 0, sizeof(rt));
    memset(&t, 0, sizeof(t));
    t.lclick = 1;
    vr_map_pad(&c, &rt, &t, 1, 0, 0.1, &p);
    CHECK(p.buttons & VR_PAD_LEFT_THUMB);
    t.y = 1;
    for (i = 0; i < 7; i++) vr_map_pad(&c, &rt, &t, 1, 0, 0.1, &p);
    CHECK((p.buttons & VR_PAD_BACK) && !(p.buttons & VR_PAD_Y) && !(p.buttons & VR_PAD_LEFT_THUMB));
    t.y = 0;
    t.rclick = 1;
    for (i = 0; i < 9; i++) vr_map_pad(&c, &rt, &t, 1, 0, 0.1, &p);
    CHECK(!rt.recentre_request && !(p.buttons & (VR_PAD_LEFT_THUMB | VR_PAD_RIGHT_THUMB)));
    for (i = 0; i < 5; i++) vr_map_pad(&c, &rt, &t, 1, 0, 0.1, &p);
    CHECK(rt.recentre_request == 1);
    rt.recentre_request = 0;
    for (i = 0; i < 20; i++) vr_map_pad(&c, &rt, &t, 1, 0, 0.1, &p);
    CHECK(rt.recentre_request == 0);                    /* once per hold */
    /* left-handed: sticks swap */
    c.left_handed = 1;
    memset(&t, 0, sizeof(t));
    t.ry = 1.0f;
    vr_map_pad(&c, &rt, &t, 1, 0, 0.1, &p);
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

int main(void)
{
    test_axes();
    test_buttons();
    test_snap_turn();
    test_smooth_turn();
    test_chords();
    test_config();
    test_perf_config();
    printf("xr_shell input: %u checks passed\n", checks);
    return 0;
}
