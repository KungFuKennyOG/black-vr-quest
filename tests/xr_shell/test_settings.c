/* xr-shell: the settings panel's pure parts: blackvr.ini edit + save (vr_settings.h), clamping, live vs restart,
 * placement / laser / open-close (vr_panel_math.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_settings.h"
#include "vr_panel_math.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NEAR(a, b, e) (fabs((double)(a) - (double)(b)) <= (e))

static const char k_ini[] =
    "# BLACK VR settings. Lines are key = value; # starts a comment.\n"
    "refresh_hz = auto        ; auto (120 for game_hz 60, 90 for 30) or 72, 80, 90, 120\n"
    "resolution_scale = 0.8   ; eye image size vs the headset's recommendation\n"
    "foveation = medium       ; off | low | medium | high\n"
    "\n"
    "; a comment line with = in it: turn = smooth\n"
    "turn = snap              ; snap | smooth | off\n"
    "snap_degrees = 30\n"
    "my_unknown_key = 42      # kept as it is\n"
    "left_handed = 0          ; 1: gun in the left hand\n"
    "   world_scale=1.0\n"
    "env RECOMP_BLACK_HZ=60\n"
    "env BLACKVR_PERF=boost\n"
    "# env BLACKVR_SPRITE_DEBUG=1\n"
    "melee_swing = on";       /* no line ending at the end */

/* the line of `text` that starts with `prefix` (after leading spaces), copied out; "" when none */
static const char *line_of(const char *text, const char *prefix)
{
    static char out[256];
    const char *p = text;
    out[0] = 0;
    while (*p) {
        const size_t n = strcspn(p, "\r\n");
        const char *q = p;
        while (*q == ' ') q++;
        if (!strncmp(q, prefix, strlen(prefix))) { memcpy(out, p, n < 255 ? n : 255); out[n < 255 ? n : 255] = 0; return out; }
        p += n;
        while (*p == '\r' || *p == '\n') p++;
    }
    return out;
}

static void test_clamp(void)
{
    CHECK(NEAR(vrs_clamp(VRS_RES_SCALE, 0.3f), 0.60, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_RES_SCALE, 9.0f), 1.40, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_RES_SCALE, 0.82f), 0.80, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_RES_SCALE, 0.83f), 0.85, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_RES_SCALE, 0.6f + 4 * 0.05f), 0.80, 1e-7));       /* on the grid exactly as the ini spells it */
    CHECK(NEAR(vrs_clamp(VRS_SNAP_DEG, 47.0f), 45.0, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_SNAP_DEG, 1.0f), 10.0, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_DUCK_RATIO, 0.954f), 0.90, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_DUCK_RATIO, 0.8149f), 0.81, 1e-6));
    CHECK(NEAR(vrs_clamp(VRS_MELEE_SPEED, 2.47f), 2.5, 1e-6));
    CHECK(vrs_clamp(VRS_FOVEATION, 7.0f) == 4.0f && vrs_clamp(VRS_FOVEATION, -2.0f) == 0.0f);
    CHECK(vrs_clamp(VRS_REFRESH, 2.4f) == 2.0f);
    CHECK(vrs_clamp(VRS_TWO_HAND, 0.3f) == 1.0f && vrs_clamp(VRS_TWO_HAND, 0.0f) == 0.0f);
    CHECK(vrs_clamp(VRS_WORLD_SCALE, 0.0f / 0.0f) == 0.0f);                      /* NaN: never stored as NaN */
    /* every float setting's own range: the ends stay, the parser accepts them */
    {
        int id;
        for (id = 0; id < VRS_COUNT; id++) {
            VrShellConfig c;
            char v[64], line[128];
            if (k_vrs[id].type != VRS_FLOAT) continue;
            vr_config_defaults(&c);
            vrs_set(&c, id, k_vrs[id].hi + 100.0f);
            CHECK(NEAR(vrs_get(&c, id), k_vrs[id].hi, 1e-5));
            vrs_set(&c, id, k_vrs[id].lo - 100.0f);
            CHECK(NEAR(vrs_get(&c, id), k_vrs[id].lo, 1e-5));
            vrs_format(&c, id, v, sizeof(v));
            snprintf(line, sizeof(line), "%s = %s", k_vrs[id].key, v);
            CHECK(vr_config_line(&c, line));
        }
    }
}

static void test_get_set(void)
{
    VrShellConfig c;
    char v[64];
    vr_config_defaults(&c);
    CHECK(vrs_get(&c, VRS_REFRESH) == 0.0f);
    vrs_set(&c, VRS_REFRESH, 3);
    CHECK(c.refresh_hz == 90.0f);
    c.refresh_hz = 60.0f;                               /* a rate the list lacks shows as the nearest */
    CHECK(vrs_get(&c, VRS_REFRESH) == 1.0f);
    vrs_format(&c, VRS_REFRESH, v, sizeof(v));
    CHECK(!strcmp(v, "60"));
    vrs_set(&c, VRS_LEFT_HANDED, 1);
    CHECK(c.input.left_handed == 1 && c.game.aim_hand == 0);
    vrs_set(&c, VRS_FOVEATION, 4);
    vrs_format(&c, VRS_FOVEATION, v, sizeof(v));
    CHECK(c.foveation == VR_FOVEATION_HIGH_TOP && !strcmp(v, "hightop"));
    CHECK(vr_config_line(&c, "foveation = highTop") && c.foveation == VR_FOVEATION_HIGH_TOP);
    /* env-backed: BLACKVR_PERF / BLACKVR_SPRITE_DEBUG live in the env lines */
    CHECK(vrs_get(&c, VRS_PERF) == 0.0f);
    vrs_set(&c, VRS_PERF, 0);
    CHECK(c.env_count == 0);                            /* the default needs no line */
    vrs_set(&c, VRS_PERF, 2);
    CHECK(c.env_count == 1 && !strcmp(c.env[0], "BLACKVR_PERF=boost_cpu") && vrs_get(&c, VRS_PERF) == 2.0f);
    vrs_set(&c, VRS_PERF, 0);
    CHECK(c.env_count == 1 && !strcmp(c.env[0], "BLACKVR_PERF=sustained_high"));
    vrs_set(&c, VRS_SPRITE_DEBUG, 1);
    CHECK(vrs_get(&c, VRS_SPRITE_DEBUG) == 1.0f && !strcmp(vrs_env_get(&c, "BLACKVR_SPRITE_DEBUG"), "1"));
    vrs_env_set(&c, "BLACKVR_SPRITE_DEBUG", NULL);
    CHECK(c.env_count == 1 && vrs_get(&c, VRS_SPRITE_DEBUG) == 0.0f);
    /* reset: every panel setting to its default, other keys left alone */
    c.game.fov_margin = 1.5f;
    vrs_set(&c, VRS_SNAP_DEG, 45);
    vrs_reset_defaults(&c);
    CHECK(c.input.snap_deg == 30.0f && c.game.fov_margin == 1.5f && c.input.left_handed == 0 && c.game.aim_hand == 1);
    CHECK(vrs_env_get(&c, "BLACKVR_PERF") == NULL);
}

static void test_ini_roundtrip(void)
{
    VrShellConfig file, edit, back;
    char out[8192];
    const char *l;
    int n, id;
    vr_config_defaults(&file);
    vr_config_parse(&file, k_ini);
    CHECK(file.errors == 1);                            /* my_unknown_key */
    edit = file;
    vrs_set(&edit, VRS_RES_SCALE, 1.05f);
    vrs_set(&edit, VRS_TURN, 1);                       /* smooth */
    vrs_set(&edit, VRS_SNAP_DEG, 45);
    vrs_set(&edit, VRS_LEFT_HANDED, 1);
    vrs_set(&edit, VRS_WORLD_SCALE, 1.25f);
    vrs_set(&edit, VRS_REFRESH, 4);                    /* 120 */
    vrs_set(&edit, VRS_PERF, 3);                       /* boost_gpu: the existing env line */
    vrs_set(&edit, VRS_SPRITE_DEBUG, 1);               /* only in a comment: appended */
    vrs_set(&edit, VRS_DUCK_RATIO, 0.75f);             /* not in the file: appended */
    vrs_set(&edit, VRS_MELEE, 0);                      /* the last line, no line ending */
    n = vrs_ini_write(k_ini, &edit, out, sizeof(out));
    CHECK(n > 0 && (size_t)n == strlen(out));
    /* comments, blank lines, unknown keys and other env lines byte for byte */
    CHECK(strstr(out, "# BLACK VR settings. Lines are key = value; # starts a comment.\n") == out);
    CHECK(strstr(out, "\n\n; a comment line with = in it: turn = smooth\n"));
    CHECK(strstr(out, "\nmy_unknown_key = 42      # kept as it is\n"));
    CHECK(strstr(out, "\nenv RECOMP_BLACK_HZ=60\n"));
    CHECK(strstr(out, "\n# env BLACKVR_SPRITE_DEBUG=1\n"));
    CHECK(strstr(out, "\nfoveation = medium       ; off | low | medium | high\n"));     /* unchanged setting untouched */
    /* changed in place: the key, its spacing and the comment's column kept */
    l = line_of(out, "resolution_scale");
    CHECK(!strcmp(l, "resolution_scale = 1.05  ; eye image size vs the headset's recommendation"));
    CHECK(strchr(l, ';') - l == strchr(line_of(k_ini, "resolution_scale"), ';') - line_of(k_ini, "resolution_scale"));
    CHECK(!strcmp(line_of(out, "turn"), "turn = smooth            ; snap | smooth | off"));
    CHECK(!strcmp(line_of(out, "refresh_hz"), "refresh_hz = 120         ; auto (120 for game_hz 60, 90 for 30) or 72, 80, 90, 120"));
    CHECK(!strcmp(line_of(out, "snap_degrees"), "snap_degrees = 45"));
    CHECK(!strcmp(line_of(out, "left_handed"), "left_handed = 1          ; 1: gun in the left hand"));
    CHECK(!strcmp(line_of(out, "world_scale"), "   world_scale=1.25"));
    CHECK(!strcmp(line_of(out, "env BLACKVR_PERF"), "env BLACKVR_PERF=boost_gpu"));
    CHECK(!strcmp(line_of(out, "melee_swing"), "melee_swing = off"));
    /* appended, after a line ending was added to the last line */
    CHECK(strstr(out, "melee_swing = off\n"));
    CHECK(!strcmp(line_of(out, "env BLACKVR_SPRITE_DEBUG"), "env BLACKVR_SPRITE_DEBUG=1"));
    CHECK(!strcmp(line_of(out, "duck_ratio"), "duck_ratio = 0.75"));
    CHECK(out[n - 1] == '\n');
    /* read back: every panel setting as edited, everything else as the file had it */
    vr_config_defaults(&back);
    vr_config_parse(&back, out);
    CHECK(back.errors == 1);
    for (id = 0; id < VRS_COUNT; id++) CHECK(!vrs_differs(&back, &edit, id));
    CHECK(!strcmp(vrs_env_get(&back, "RECOMP_BLACK_HZ"), "60"));
    CHECK(back.game.aim_hand == 0);
    /* a second save of the same values changes nothing */
    {
        char again[8192];
        CHECK(vrs_ini_write(out, &edit, again, sizeof(again)) == n && !strcmp(again, out));
    }
    /* reverting to what the file had: the edited lines get the file's values back (its own spelling where the
     * panel spells it the same: 0.8 -> 0.80 is not, 1.0 -> 1.00 neither) */
    {
        char rev[8192];
        VrShellConfig r;
        vrs_ini_write(out, &file, rev, sizeof(rev));
        CHECK(!strcmp(line_of(rev, "resolution_scale"), "resolution_scale = 0.80  ; eye image size vs the headset's recommendation"));
        CHECK(!strcmp(line_of(rev, "turn"), line_of(k_ini, "turn")));
        CHECK(!strcmp(line_of(rev, "left_handed"), line_of(k_ini, "left_handed")));
        CHECK(!strcmp(line_of(rev, "env BLACKVR_PERF"), "env BLACKVR_PERF=boost"));
        CHECK(!strcmp(line_of(rev, "env BLACKVR_SPRITE_DEBUG"), "env BLACKVR_SPRITE_DEBUG=0"));
        vr_config_defaults(&r);
        vr_config_parse(&r, rev);
        for (id = 0; id < VRS_COUNT; id++) CHECK(!vrs_differs(&r, &file, id));
    }
    /* too small a buffer: refused, never truncated */
    CHECK(vrs_ini_write(k_ini, &edit, out, 64) == -1);
}

static void test_ini_crlf_and_new(void)
{
    static const char crlf[] = "turn = snap ; t\r\n# c\r\nenv A=1\r\n";
    VrShellConfig e;
    char out[2048];
    int n;
    vr_config_defaults(&e);
    vrs_set(&e, VRS_TURN, 2);
    vrs_set(&e, VRS_TWO_HAND, 0);
    n = vrs_ini_write(crlf, &e, out, sizeof(out));
    CHECK(n > 0);
    CHECK(!strcmp(out, "turn = off  ; t\r\n# c\r\nenv A=1\r\ntwo_hand = off\r\n"));
    /* no file yet: a header line and only what differs from the defaults */
    vr_config_defaults(&e);
    vrs_set(&e, VRS_GAME_HZ, 0);
    n = vrs_ini_write(NULL, &e, out, sizeof(out));
    CHECK(n > 0 && !strcmp(out, "# BLACK VR settings. Lines are key = value; # starts a comment.\ngame_hz = 30\n"));
    vr_config_defaults(&e);
    n = vrs_ini_write("", &e, out, sizeof(out));
    CHECK(n > 0 && !strcmp(out, "# BLACK VR settings. Lines are key = value; # starts a comment.\n"));
    /* the same key twice: both lines carry the new value (the last one wins in the parser either way) */
    vrs_set(&e, VRS_SNAP_DEG, 60);
    n = vrs_ini_write("snap_degrees = 30\nsnap_degrees = 45\n", &e, out, sizeof(out));
    CHECK(!strcmp(out, "snap_degrees = 60\nsnap_degrees = 60\n"));
}

static void test_live(void)
{
    VrShellConfig boot, live, edit;
    VrsCaps all = { 1, 1, 1, 1 }, none = { 0, 0, 0, 0 }, unfoveated = { 1, 1, 0, 1 };
    unsigned todo;
    vr_config_defaults(&boot);
    live = boot;
    edit = boot;
    vrs_set(&edit, VRS_RES_SCALE, 1.2f);
    vrs_set(&edit, VRS_GAME_HZ, 0);
    vrs_set(&edit, VRS_SPRITE_DEBUG, 1);
    vrs_set(&edit, VRS_FOVEATION, 3);
    vrs_set(&edit, VRS_REFRESH, 2);
    vrs_set(&edit, VRS_PERF, 1);
    vrs_set(&edit, VRS_TURN, 1);
    vrs_set(&edit, VRS_LEFT_HANDED, 1);
    vrs_set(&edit, VRS_SCOPE_DIST, 0.2f);
    todo = vrs_apply_live(&live, &edit, &all);
    CHECK(todo == (VRS_DO_REFRESH | VRS_DO_FOVEATION | VRS_DO_PERF | VRS_DO_HANDS));
    CHECK(live.resolution_scale == boot.resolution_scale && live.game_hz == boot.game_hz);      /* restart */
    CHECK(vrs_env_get(&live, "BLACKVR_SPRITE_DEBUG") == NULL);
    CHECK(live.foveation == VR_FOVEATION_HIGH && live.refresh_hz == 80.0f && live.input.turn_mode == VR_TURN_SMOOTH);
    CHECK(!strcmp(vrs_env_get(&live, "BLACKVR_PERF"), "boost"));
    CHECK(live.input.left_handed && live.game.aim_hand == 0 && NEAR(live.gest.scope_distance, 0.2, 1e-6));
    CHECK(vrs_apply_live(&live, &edit, &all) == 0);                                            /* nothing left */
    CHECK(vrs_pending_restart(&boot, &edit, &all, VRS_RES_SCALE) && vrs_pending_restart(&boot, &edit, &all, VRS_GAME_HZ));
    CHECK(vrs_pending_restart(&boot, &edit, &all, VRS_SPRITE_DEBUG));
    CHECK(!vrs_pending_restart(&boot, &edit, &all, VRS_FOVEATION) && !vrs_pending_restart(&boot, &edit, &all, VRS_TURN));
    /* an eye swapchain made without the foveation flag: the level waits for a restart */
    live = boot;
    todo = vrs_apply_live(&live, &edit, &unfoveated);
    CHECK(!(todo & VRS_DO_FOVEATION) && live.foveation == boot.foveation);
    CHECK(vrs_pending_restart(&boot, &edit, &unfoveated, VRS_FOVEATION));
    /* a runtime without the extensions: refresh / foveation / perf do nothing (N/A), never "pending" */
    live = boot;
    todo = vrs_apply_live(&live, &edit, &none);
    CHECK(todo == VRS_DO_HANDS && live.refresh_hz == boot.refresh_hz);
    CHECK(vrs_available(VRS_REFRESH, &none) == VRS_UNAVAILABLE && !vrs_pending_restart(&boot, &edit, &none, VRS_REFRESH));
    CHECK(vrs_is_live(VRS_TURN, &none) && !vrs_is_live(VRS_RES_SCALE, &all));
    /* Cancel: applying the snapshot taken at open restores every live value */
    {
        VrShellConfig orig = boot;
        live = boot;
        vrs_apply_live(&live, &edit, &all);
        todo = vrs_apply_live(&live, &orig, &all);
        CHECK(todo & VRS_DO_REFRESH);
        {
            int id;
            for (id = 0; id < VRS_COUNT; id++) CHECK(!vrs_differs(&live, &boot, id));
        }
    }
}

static VrPose pose_at(float x, float y, float z, float yaw_deg)
{
    VrPose p;
    p.position = vrm_v3(x, y, z);
    p.orientation = vrm_q_axis_angle(vrm_v3(0, 1, 0), yaw_deg * 3.14159265f / 180.0f);
    return p;
}

static void test_placement(void)
{
    VrpQuad q = vrp_place(pose_at(0.3f, 1.7f, -0.2f, 0), 4.0f / 3.0f);
    VrVec3 n;
    float u, v, t;
    CHECK(NEAR(q.pose.position.x, 0.3, 1e-5) && NEAR(q.pose.position.y, 1.6, 1e-5) && NEAR(q.pose.position.z, -1.4, 1e-5));
    CHECK(NEAR(q.width_m, 0.8, 1e-6) && NEAR(q.height_m, 0.6, 1e-6));
    n = vrm_q_rotate(q.pose.orientation, vrm_v3(0, 0, 1));
    CHECK(NEAR(n.z, 1.0, 1e-5));                                         /* faces the user */
    /* turned 90 deg left, looking down 60 deg: still level, 1.2 m along the horizontal facing */
    {
        VrPose h = pose_at(0, 1.6f, 0, 90);
        VrVec3 up;
        h.orientation = vrm_q_mul(h.orientation, vrm_q_axis_angle(vrm_v3(1, 0, 0), -60.0f * 3.14159265f / 180.0f));
        q = vrp_place(h, 4.0f / 3.0f);
        CHECK(NEAR(q.pose.position.x, -1.2, 1e-4) && NEAR(q.pose.position.z, 0.0, 1e-4) && NEAR(q.pose.position.y, 1.5, 1e-5));
        up = vrm_q_rotate(q.pose.orientation, vrm_v3(0, 1, 0));
        n = vrm_q_rotate(q.pose.orientation, vrm_v3(0, 0, 1));
        CHECK(NEAR(up.y, 1.0, 1e-5) && NEAR(n.x, 1.0, 1e-4));
    }
    /* the laser: the centre, the top-left corner, off the panel, from behind */
    q = vrp_place(pose_at(0, 1.6f, 0, 0), 4.0f / 3.0f);
    CHECK(vrp_hit(&q, pose_at(0, 1.5f, -0.3f, 0), &u, &v, &t) && NEAR(u, 0.5, 1e-5) && NEAR(v, 0.5, 1e-5) && NEAR(t, 0.9, 1e-5));
    {
        VrPose a = pose_at(-0.4f + 0.04f, 1.5f + 0.3f - 0.03f, -0.4f, 0);
        CHECK(vrp_hit(&q, a, &u, &v, &t) && NEAR(u, 0.05, 1e-4) && NEAR(v, 0.05, 1e-4));
        a = pose_at(0.5f, 1.5f, -0.4f, 0);
        CHECK(!vrp_hit(&q, a, &u, &v, &t) && u > 1.0f && t > 0.0f);
        a = pose_at(0, 1.5f, -0.4f, 180);                                   /* pointing away */
        CHECK(!vrp_hit(&q, a, &u, &v, &t) && t < 0.0f);
        a = pose_at(0, 1.5f, -2.0f, 180);                                   /* behind the panel, facing its back */
        CHECK(!vrp_hit(&q, a, &u, &v, &t));
    }
    /* pointing 10 deg right of centre from 0.9 m: u = 0.5 + tan(10)*0.9/0.8 */
    {
        VrPose a = pose_at(0, 1.5f, -0.3f, -10);
        CHECK(vrp_hit(&q, a, &u, &v, &t) && NEAR(u, 0.5 + tan(10 * 3.14159265 / 180) * 0.9 / 0.8, 1e-4) && NEAR(v, 0.5, 1e-5));
    }
    /* the beam quad: along the ray from the hand, its face turned to the head */
    {
        VrPose a = pose_at(0.2f, 1.2f, -0.3f, 0), lp;
        float w, h;
        VrVec3 y, z, toward;
        lp = vrp_laser_pose(a, 0.9f, vrm_v3(0, 1.6f, 0), &w, &h);
        y = vrm_q_rotate(lp.orientation, vrm_v3(0, 1, 0));
        z = vrm_q_rotate(lp.orientation, vrm_v3(0, 0, 1));
        CHECK(NEAR(y.z, -1.0, 1e-5) && NEAR(lp.position.z, -0.75, 1e-5) && NEAR(lp.position.x, 0.2, 1e-5));
        CHECK(NEAR(w, VRP_LASER_WIDTH_M, 1e-7) && NEAR(h, 0.9, 1e-6));
        toward = vrm_sub(vrm_v3(0, 1.6f, 0), lp.position);
        toward.z = 0;
        vrm_normalize(&toward);
        CHECK(vrm_dot(z, toward) > 0.999f && fabsf(vrm_dot(z, y)) < 1e-5f);
    }
}

static VrTouchState none_pressed(void) { VrTouchState t; memset(&t, 0, sizeof(t)); t.left_active = t.right_active = 1; return t; }

static void test_open_close(void)
{
    VrpInput p;
    VrpStep st;
    VrTouchState t = none_pressed(), g;
    const double dt = 1.0 / 72.0;
    int i;
    memset(&p, 0, sizeof(p));
    /* a left stick click in a menu: passes to the game while held, opens on release */
    t.lclick = 1;
    for (i = 0; i < 10; i++) { CHECK(!vrp_input_step(&p, &t, 0, 1, dt, &g, &st)); CHECK(g.lclick == 1); }
    t.lclick = 0;
    CHECK(vrp_input_step(&p, &t, 0, 1, dt, &g, &st) && st.opened);
    /* open: the game sees nothing */
    t.rt = 1.0f; t.a = 1; t.lx = 0.9f; t.menu = 0; t.x = 1;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(g.rt == 0.0f && !g.a && g.lx == 0.0f && !g.x && g.left_active && st.mouse_down);
    /* trigger hysteresis (A released) */
    t.a = 0; t.rt = 0.45f;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(st.mouse_down);
    t.rt = 0.3f;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(!st.mouse_down);
    t.rt = 0.5f;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(!st.mouse_down);
    t.rt = 0.0f;
    /* scroll: either stick, past the dead zone */
    t.lx = 0; t.ry = 1.0f;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(NEAR(st.wheel, VRP_SCROLL_RATE * dt, 1e-5));
    t.ry = 0; t.ly = -0.2f;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(st.wheel == 0.0f);
    t.ly = 0; t.x = 0;
    /* B closes; B stays hidden from the game until it is released */
    t.b = 1;
    CHECK(!vrp_input_step(&p, &t, 0, 1, dt, &g, &st) && st.closed);
    CHECK(!g.b);
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(!g.b);
    t.b = 0;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    t.b = 1;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    CHECK(g.b);                                          /* a new press reaches the game */
    t.b = 0;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    /* Menu closes too */
    t.lclick = 1; vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    t.lclick = 0; CHECK(vrp_input_step(&p, &t, 0, 1, dt, &g, &st));
    t.menu = 1;
    CHECK(!vrp_input_step(&p, &t, 0, 1, dt, &g, &st) && st.closed && !g.menu);
    t.menu = 0;
    vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    /* not in gameplay (menu_ctx 0): the click is the game's (Crouch) */
    t.lclick = 1; vrp_input_step(&p, &t, 0, 0, dt, &g, &st);
    t.lclick = 0; CHECK(!vrp_input_step(&p, &t, 0, 0, dt, &g, &st) && !st.opened);
    /* the recentre chord (both clicks), or a long hold: not the panel */
    t.lclick = 1; vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    t.rclick = 1; for (i = 0; i < 80; i++) vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    t.rclick = 0; vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    t.lclick = 0; CHECK(!vrp_input_step(&p, &t, 0, 1, dt, &g, &st));
    t.lclick = 1; for (i = 0; i < 72; i++) vrp_input_step(&p, &t, 0, 1, dt, &g, &st);
    t.lclick = 0; CHECK(!vrp_input_step(&p, &t, 0, 1, dt, &g, &st));
    /* left-handed: the off hand is the right one */
    t.lclick = 1; vrp_input_step(&p, &t, 1, 1, dt, &g, &st);
    t.lclick = 0; CHECK(!vrp_input_step(&p, &t, 1, 1, dt, &g, &st));
    t.rclick = 1; vrp_input_step(&p, &t, 1, 1, dt, &g, &st);
    t.rclick = 0; CHECK(vrp_input_step(&p, &t, 1, 1, dt, &g, &st));
    /* left-handed laser click: the left trigger */
    t.lt = 1.0f;
    vrp_input_step(&p, &t, 1, 1, dt, &g, &st);
    CHECK(st.mouse_down && g.lt == 0.0f);
    /* the Close button: the trigger that clicked it stays from the game */
    vrp_input_close(&p, &t);
    CHECK(!p.open);
    vrp_input_step(&p, &t, 1, 1, dt, &g, &st);
    CHECK(g.lt == 0.0f);
    t.lt = 0.0f;
    vrp_input_step(&p, &t, 1, 1, dt, &g, &st);
    t.lt = 1.0f;
    vrp_input_step(&p, &t, 1, 1, dt, &g, &st);
    CHECK(g.lt == 1.0f);
}


static void test_menu_model(void)
{
    VrShellConfig c, boot;
    VrsCaps all = { 1, 1, 1, 1 }, none = { 0, 0, 0, 0 };
    char v[64];
    int ids[32], n;
    vr_config_defaults(&c);
    boot = c;
    /* values as the game's menu shows them */
    vrs_menu_value(&c, &boot, &all, VRS_RES_SCALE, v, sizeof(v));   CHECK(!strcmp(v, "0.80X"));
    vrs_menu_value(&c, &boot, &all, VRS_SNAP_DEG, v, sizeof(v));    CHECK(!strcmp(v, "30 DEG"));
    vrs_menu_value(&c, &boot, &all, VRS_MELEE_SPEED, v, sizeof(v)); CHECK(!strcmp(v, "2.5 M/S"));
    vrs_menu_value(&c, &boot, &all, VRS_REFRESH, v, sizeof(v));     CHECK(!strcmp(v, "AUTO"));
    vrs_menu_value(&c, &boot, &all, VRS_FOVEATION, v, sizeof(v));   CHECK(!strcmp(v, "MEDIUM"));
    vrs_menu_value(&c, &boot, &all, VRS_TWO_HAND, v, sizeof(v));    CHECK(!strcmp(v, "ON"));
    vrs_menu_value(&c, &boot, &none, VRS_REFRESH, v, sizeof(v));    CHECK(!strcmp(v, "N/A"));
    /* stepping: enums go round both ways, on/off toggles (A too), numbers stop at their ends */
    CHECK(vrs_step(&c, VRS_REFRESH, 1) && c.refresh_hz == 72.0f);
    vrs_menu_value(&c, &boot, &all, VRS_REFRESH, v, sizeof(v));     CHECK(!strcmp(v, "72 HZ"));
    CHECK(vrs_step(&c, VRS_REFRESH, -1) && vrs_step(&c, VRS_REFRESH, -1) && c.refresh_hz == 120.0f);
    CHECK(vrs_step(&c, VRS_TURN, 0) && c.input.turn_mode == VR_TURN_SMOOTH);
    CHECK(vrs_step(&c, VRS_TWO_HAND, 1) && !c.gest.two_hand && vrs_step(&c, VRS_TWO_HAND, 0) && c.gest.two_hand);
    CHECK(!vrs_step(&c, VRS_SNAP_DEG, 0));
    CHECK(vrs_step(&c, VRS_SNAP_DEG, 1) && c.input.snap_deg == 35.0f);
    c.resolution_scale = 1.4f;
    CHECK(!vrs_step(&c, VRS_RES_SCALE, 1) && vrs_step(&c, VRS_RES_SCALE, -1) && NEAR(c.resolution_scale, 1.35, 1e-6));
    vrs_menu_value(&c, &boot, &all, VRS_RES_SCALE, v, sizeof(v));   CHECK(!strcmp(v, "1.35X (RESTART)"));
    /* sections */
    n = vrs_section_settings(VRS_SEC_GRAPHICS, ids, 32);
    CHECK(n == 6 && ids[0] == VRS_RES_SCALE && ids[5] == VRS_SPRITE_DEBUG);
    CHECK(vrs_section_settings(VRS_SEC_COMFORT, ids, 32) == 12 && vrs_section_settings(VRS_SEC_GESTURES, ids, 32) == 7);
    CHECK(vrs_section_settings(VRS_SEC_COMFORT, ids, 32) == 12 && ids[6] == VRS_HUD_DIST && ids[7] == VRS_HUD_WIDTH &&
          ids[8] == VRS_HUD_HEIGHT && ids[9] == VRS_TUTORIALS && ids[10] == VRS_LASER && ids[11] == VRS_SKIP_MOVIES);
    /* the laser sight: off by default, live, blackvr.ini laser */
    {
        VrShellConfig lc;
        char lv[32];
        vr_config_defaults(&lc);
        CHECK(lc.game.laser == 0 && vrs_get(&lc, VRS_LASER) == 0.0f && vrs_is_live(VRS_LASER, &(VrsCaps){0}));
        vrs_set(&lc, VRS_LASER, 1.0f);
        CHECK(lc.game.laser == 1);
        vrs_format(&lc, VRS_LASER, lv, sizeof(lv));
        CHECK(!strcmp(lv, "on"));
        CHECK(vr_config_line(&lc, "laser = off") && lc.game.laser == 0);
        CHECK(lc.game.skip_movies == 0 && vr_config_line(&lc, "skip_movies = on") && vrs_get(&lc, VRS_SKIP_MOVIES) == 1.0f);
    }
    /* tutorial hints: on by default, live, the ini key the shell reads */
    {
        VrShellConfig t;
        char tv[16];
        vr_config_defaults(&t);
        CHECK(t.game.tutorial_hints == 1 && vrs_get(&t, VRS_TUTORIALS) == 1.0f && vrs_is_live(VRS_TUTORIALS, &(VrsCaps){0}));
        vrs_set(&t, VRS_TUTORIALS, 0.0f);
        CHECK(t.game.tutorial_hints == 0);
        vrs_format(&t, VRS_TUTORIALS, tv, sizeof(tv));
        CHECK(!strcmp(tv, "off"));
        CHECK(vr_config_line(&t, "tutorial_hints = on") && t.game.tutorial_hints == 1);
        CHECK(vr_config_line(&t, "tutorial_hints = off") && t.game.tutorial_hints == 0);
    }
    /* the HUD rows: live, written as the ini keys the shell reads */
    vr_config_defaults(&c);
    CHECK(vrs_step(&c, VRS_HUD_DIST, 1) && NEAR(c.hud_distance_m, 2.25, 1e-6));
    CHECK(vrs_step(&c, VRS_HUD_WIDTH, -1) && NEAR(c.hud_width_m, 2.3, 1e-6));
    CHECK(vr_config_line(&c, "hud_distance = 3.0") && NEAR(c.hud_distance_m, 3.0, 1e-6));
    /* HUD height: the in-game HUD tilted below eye level by default (BLACK's HUD is along the top of its image) */
    CHECK(NEAR(c.hud_pitch_deg, -12.0, 1e-6));
    CHECK(vrs_step(&c, VRS_HUD_HEIGHT, -1) && NEAR(c.hud_pitch_deg, -13.0, 1e-6));
    CHECK(vr_config_line(&c, "hud_height = -20") && NEAR(c.hud_pitch_deg, -20.0, 1e-6));
    CHECK(vr_config_line(&c, "hud_height = 5") && NEAR(c.hud_pitch_deg, 5.0, 1e-6));
    CHECK(!vr_config_line(&c, "hud_height = -60") && NEAR(c.hud_pitch_deg, 5.0, 1e-6));
    c.hud_pitch_deg = -30.0f;
    CHECK(!vrs_step(&c, VRS_HUD_HEIGHT, -1) && vrs_step(&c, VRS_HUD_HEIGHT, 1) && NEAR(c.hud_pitch_deg, -29.0, 1e-6));
    CHECK(vrs_section_settings(VRS_SEC_INFO, ids, 32) == 0);
    /* settings_menu */
    vr_config_defaults(&c);
    CHECK(c.settings_menu == VR_SETTINGS_MENU_NATIVE);
    CHECK(vr_config_line(&c, "settings_menu = panel") && c.settings_menu == VR_SETTINGS_MENU_PANEL);
    CHECK(vr_config_line(&c, "settings_menu = both") && c.settings_menu == VR_SETTINGS_MENU_BOTH);
    CHECK(!vr_config_line(&c, "settings_menu = nope"));
}

int main(void)
{
    test_menu_model();
    test_clamp();
    test_get_set();
    test_ini_roundtrip();
    test_ini_crlf_and_new();
    test_live();
    test_placement();
    test_open_close();
    printf("xr_shell settings: %u checks passed\n", checks);
    return 0;
}
