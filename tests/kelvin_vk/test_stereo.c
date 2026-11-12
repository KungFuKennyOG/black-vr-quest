/* kvk-stereo: the per-eye clip-space correction of the Vulkan backend (kvk_stereo.c).
 *
 * Checks, for random cameras (projections in the D3D, RenderWare-like mirrored-x, and OpenGL -z conventions),
 * random heads and eyes:
 *   - FULL form == FROM_W form == the general P_eye * V_eye * inv(V_game) * inv(P_game) == direct eye projection;
 *   - the physical result: a point ahead of the head shows the expected, opposite parallax in each eye, computed
 *     by plain trigonometry independent of the matrix code; parallax vanishes at infinity;
 *   - FROM_W keeps the parallax exact when the depth row (near/far) is wrong, FULL does not (why FROM_W is used);
 *   - kvk_stereo_frame: identity when the eye equals the game camera; the virtual-screen quad's centre and edges. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvk_stereo.h"

static int s_fail, s_checks;
#define CHECK(c, ...) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double rnd(double a, double b) { return a + (b - a) * (rand() / (double)RAND_MAX); }

static VrQuat quat_axis(double ax, double ay, double az, double ang)
{
    double n = sqrt(ax * ax + ay * ay + az * az), s = sin(ang / 2) / n;
    VrQuat q = { (float)(ax * s), (float)(ay * s), (float)(az * s), (float)cos(ang / 2) };
    return q;
}

static void ndc(const float c[4], double out[3])
{
    out[0] = c[0] / c[3]; out[1] = c[1] / c[3]; out[2] = c[2] / c[3];
}

/* A game projection in one of three conventions: 0 D3D (x right, +z forward), 1 x mirrored (+z forward),
 * 2 OpenGL-like view space (-z forward) with D3D depth. */
static void game_proj(float p[16], int conv, double hx, double hy, double n, double f)
{
    kvk_proj_from_tangents(p, (float)-hx, (float)hx, (float)hy, (float)-hy, (float)n, (float)f, conv == 2 ? -1.0f : 1.0f);
    if (conv == 1) { p[0] = -p[0]; }
}

/* view-space point of the game camera for a head-space point given in XR axes (x right, y up, -z forward) */
static void xr_to_game(int conv, const double xr[3], float g[4])
{
    g[0] = (float)(conv == 1 ? -xr[0] : xr[0]);
    g[1] = (float)xr[1];
    g[2] = (float)(conv == 2 ? xr[2] : -xr[2]);
    g[3] = 1.0f;
}

static void test_inverse(void)
{
    int k, i;
    for (k = 0; k < 200; k++) {
        float m[16], inv[16], id[16];
        for (i = 0; i < 16; i++) m[i] = (float)rnd(-2, 2);
        for (i = 0; i < 4; i++) m[i * 5] += 4.0f;
        CHECK(kvk_mat4_invert(inv, m), "invert");
        kvk_mat4_mul(id, m, inv);
        for (i = 0; i < 16; i++) CHECK(fabs(id[i] - ((i % 5) == 0 ? 1.0 : 0.0)) < 1e-5, "m * inv(m) [%d] = %g", i, id[i]);
    }
}

/* Every form agrees with projecting the point straight into the eye. */
static void test_forms(void)
{
    int k, conv;
    for (conv = 0; conv < 3; conv++) {
        for (k = 0; k < 300; k++) {
            const double hx = rnd(0.4, 1.6), hy = hx * rnd(0.6, 1.0), n = rnd(0.05, 2), f = n * rnd(50, 5000);
            const float ws = (float)rnd(0.5, 40);
            float pg[16], pe[16], e[16], cf[16], cw[16], cg[16], vg[16], ve[16];
            VrPose head, eye;
            VrFov fov = { (float)-rnd(0.6, 0.9), (float)rnd(0.6, 0.9), (float)rnd(0.6, 0.9), (float)-rnd(0.6, 0.9) };
            int i;
            game_proj(pg, conv, hx, hy, n, f);
            head.orientation = quat_axis(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1), rnd(-3, 3));
            head.position.x = (float)rnd(-1, 1); head.position.y = (float)rnd(0, 2); head.position.z = (float)rnd(-1, 1);
            /* the eye: a few centimetres from the head along a random direction, slightly turned (canted displays) */
            eye.orientation = head.orientation;
            {
                VrQuat t = quat_axis(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1), rnd(-0.05, 0.05));
                VrQuat a = head.orientation;
                eye.orientation.w = a.w * t.w - a.x * t.x - a.y * t.y - a.z * t.z;
                eye.orientation.x = a.w * t.x + a.x * t.w + a.y * t.z - a.z * t.y;
                eye.orientation.y = a.w * t.y - a.x * t.z + a.y * t.w + a.z * t.x;
                eye.orientation.z = a.w * t.z + a.x * t.y - a.y * t.x + a.z * t.w;
            }
            eye.position.x = head.position.x + (float)rnd(-0.04, 0.04);
            eye.position.y = head.position.y + (float)rnd(-0.01, 0.01);
            eye.position.z = head.position.z + (float)rnd(-0.01, 0.01);
            kvk_eye_from_head(e, &head, &eye, ws, pg);
            kvk_eye_projection(pe, &fov, pg);
            CHECK(kvk_eye_clip(cf, KVK_STEREO_FULL, pe, e, pg), "full");
            CHECK(kvk_eye_clip(cw, KVK_STEREO_FROM_W, pe, e, pg), "from_w");
            /* the general form with an arbitrary game view matrix: V_eye = E * V_game */
            for (i = 0; i < 16; i++) vg[i] = 0.0f;
            {
                VrPose cam;
                cam.orientation = quat_axis(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1), rnd(-3, 3));
                cam.position.x = (float)rnd(-100, 100); cam.position.y = (float)rnd(-100, 100); cam.position.z = (float)rnd(-100, 100);
                kvk_pose_inverse_matrix(vg, &cam, 1.0f);
            }
            kvk_mat4_mul(ve, e, vg);
            CHECK(kvk_eye_clip_correction(cg, pe, ve, vg, pg), "general");
            for (i = 0; i < 20; i++) {
                /* a point in front of the eye, in game view space */
                float p[4], cgame[4], direct[4], a[4], b[4], c[4], pw[4], pe_view[4];
                double d0[3], d1[3], d2[3], d3[3];
                double xr[3] = { rnd(-3, 3), rnd(-3, 3), -rnd(0.5, 30) };
                int q;
                xr_to_game(conv, xr, p);
                for (q = 0; q < 3; q++) p[q] *= ws;
                kvk_mat4_apply(pe_view, e, p);
                kvk_mat4_apply(direct, pe, pe_view);
                if (direct[3] <= 0.0f) continue;
                kvk_mat4_apply(cgame, pg, p);
                kvk_mat4_apply(a, cf, cgame);
                pw[0] = cgame[0]; pw[1] = cgame[1]; pw[2] = cgame[3]; pw[3] = 1.0f;
                kvk_mat4_apply(b, cw, pw);
                kvk_mat4_apply(c, cg, cgame);
                ndc(direct, d0); ndc(a, d1); ndc(b, d2); ndc(c, d3);
                for (q = 0; q < 3; q++) {
                    CHECK(fabs(d0[q] - d1[q]) < 2e-3 * (1 + fabs(d0[q])), "conv %d full ndc[%d] %g vs %g", conv, q, d1[q], d0[q]);
                    CHECK(fabs(d0[q] - d2[q]) < 2e-3 * (1 + fabs(d0[q])), "conv %d from_w ndc[%d] %g vs %g", conv, q, d2[q], d0[q]);
                    CHECK(fabs(d0[q] - d3[q]) < 2e-3 * (1 + fabs(d0[q])), "conv %d general ndc[%d] %g vs %g", conv, q, d3[q], d0[q]);
                }
            }
        }
    }
}

/* The physical picture: eyes 64 mm apart, a point ahead of the head at distance D. */
static void test_parallax(void)
{
    int conv;
    for (conv = 0; conv < 3; conv++) {
        const double ipd = 0.064, ws = 10.0, tan_h = 1.0;
        float pg[16];
        double D;
        game_proj(pg, conv, tan_h, 0.75, 0.1, 10000);
        for (D = 0.3; D < 200; D *= 2.5) {
            int e;
            double x_eye[2];
            for (e = 0; e < 2; e++) {
                VrPose head = { { 0, 0, 0, 1 }, { 0, 1.6f, 0 } }, eye = head;
                VrFov fov = { (float)-atan(tan_h), (float)atan(tan_h), (float)atan(0.75), (float)-atan(0.75) };
                float e4[16], pe[16], cw[16], p[4], cg4[4], v[4], out[4];
                double xr[3] = { 0.0, 0.0, -D * ws }, d[3];
                eye.position.x = (float)(e == 0 ? -ipd / 2 : ipd / 2);
                kvk_eye_from_head(e4, &head, &eye, (float)ws, pg);
                kvk_eye_projection(pe, &fov, pg);
                kvk_eye_clip(cw, KVK_STEREO_FROM_W, pe, e4, pg);
                xr_to_game(conv, xr, p);
                kvk_mat4_apply(cg4, pg, p);
                v[0] = cg4[0]; v[1] = cg4[1]; v[2] = cg4[3]; v[3] = 1.0f;
                kvk_mat4_apply(out, cw, v);
                ndc(out, d);
                x_eye[e] = d[0];
                /* screen x of the point for that eye: tan = (ipd/2) / D, on the side away from the eye */
                CHECK(fabs(d[0] - (e == 0 ? 1 : -1) * (ipd / 2) / D / tan_h) < 1e-4,
                      "conv %d eye %d D %g: ndc x %g expected %g", conv, e, D, d[0], (e == 0 ? 1 : -1) * (ipd / 2) / D / tan_h);
                CHECK(fabs(d[1]) < 1e-5, "conv %d: no vertical parallax (%g)", conv, d[1]);
            }
            CHECK(x_eye[0] > 0 && x_eye[1] < 0, "conv %d: crossed parallax for a near point", conv);
        }
    }
    {
        /* at infinity (w huge), both eyes agree */
        float pg[16], e4[16], pe[16], cw[16], v[4], out[4];
        VrPose head = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, eye = head;
        VrFov fov = { -0.785398f, 0.785398f, 0.643501f, -0.643501f };
        game_proj(pg, 0, 1.0, 0.75, 0.1, 1e7);
        eye.position.x = -0.032f;
        kvk_eye_from_head(e4, &head, &eye, 1.0f, pg);
        kvk_eye_projection(pe, &fov, pg);
        kvk_eye_clip(cw, KVK_STEREO_FROM_W, pe, e4, pg);
        v[0] = 0.0f; v[1] = 0.0f; v[2] = 1e6f; v[3] = 1.0f;
        kvk_mat4_apply(out, cw, v);
        CHECK(fabs(out[0] / out[3]) < 1e-6, "parallax at infinity %g", out[0] / out[3]);
    }
}

/* The depth row only matters to the full form. */
static void test_wrong_depth_row(void)
{
    float pg_true[16], pg_wrong[16], e4[16], pe[16], cf[16], cw[16], p[4] = { 0, 0, 2.0f, 1 }, c[4], v[4], a[4], b[4];
    VrPose head = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, eye = head;
    VrFov fov = { -0.785398f, 0.785398f, 0.643501f, -0.643501f };
    double want;
    game_proj(pg_true, 0, 1.0, 0.75, 0.1, 1000);
    game_proj(pg_wrong, 0, 1.0, 0.75, 1.0, 50);          /* the near/far a hook might have guessed */
    eye.position.x = -0.032f;
    kvk_eye_from_head(e4, &head, &eye, 1.0f, pg_wrong);
    kvk_eye_projection(pe, &fov, pg_wrong);
    kvk_eye_clip(cf, KVK_STEREO_FULL, pe, e4, pg_wrong);
    kvk_eye_clip(cw, KVK_STEREO_FROM_W, pe, e4, pg_wrong);
    kvk_mat4_apply(c, pg_true, p);                       /* what the title's vertex program really outputs */
    kvk_mat4_apply(a, cf, c);
    v[0] = c[0]; v[1] = c[1]; v[2] = c[3]; v[3] = 1.0f;
    kvk_mat4_apply(b, cw, v);
    want = 0.032 / 2.0;
    CHECK(fabs(b[0] / b[3] - want) < 1e-5, "from_w parallax with a wrong depth row: %g want %g", b[0] / b[3], want);
    CHECK(fabs(a[0] / a[3] - want) > 1e-3, "full form unexpectedly exact with a wrong depth row (%g)", a[0] / a[3]);
}

static void test_frame(void)
{
    VrFrameState fs;
    VrGameCamera cam;
    KvkStereoFrame st;
    KvkStereoConfig cfg;
    float v[4] = { 0.3f, -0.2f, 5.0f, 1.0f }, out[4];
    int e, i;
    memset(&fs, 0, sizeof(fs));
    memset(&cam, 0, sizeof(cam));
    kvk_stereo_default_config(&cfg);
    fs.head.orientation.w = 1.0f;
    fs.world_scale = 1.0f;
    for (e = 0; e < 2; e++) {
        fs.eye[e].pose = fs.head;
        fs.eye[e].fov.left = -0.785398f; fs.eye[e].fov.right = 0.785398f;
        fs.eye[e].fov.up = 0.643501f; fs.eye[e].fov.down = -0.643501f;
    }
    /* no camera: identity, not valid */
    kvk_stereo_frame(&st, &cfg, &fs, NULL);
    CHECK(!st.valid && st.eye_clip[0][0] == 1.0f && st.eye_clip[1][5] == 1.0f, "no camera -> identity");
    /* a camera with the eyes' own field of view, eyes at the head: the correction is the identity on (x, y, w, 1) */
    cam.valid = 1;
    cam.half_fov_x = 1.0f; cam.half_fov_y = 0.75f;
    kvk_stereo_frame(&st, &cfg, &fs, &cam);
    CHECK(st.valid && st.misc[0] == (float)KVK_STEREO_FROM_W, "valid frame");
    for (e = 0; e < 2; e++) {
        float pg[16], c[4], w4[4];
        kvk_game_projection(pg, &cam, cfg.default_near, cfg.default_far);
        kvk_mat4_apply(c, pg, v);
        w4[0] = c[0]; w4[1] = c[1]; w4[2] = c[3]; w4[3] = 1.0f;
        kvk_mat4_apply(out, st.eye_clip[e], w4);
        for (i = 0; i < 4; i++) CHECK(fabs(out[i] - c[i]) < 1e-4 * (1 + fabs(c[i])), "identity eye [%d] %g vs %g", i, out[i], c[i]);
    }
    /* the virtual screen: centre ahead of the eyes, width as configured */
    {
        float centre[4] = { 0, 0, 0, 1 }, right[4] = { 0.5f, 0, 0, 1 }, top[4] = { 0, 0.5f, 0, 1 }, o[4];
        double tan_x = (cfg.screen_width_m / 2) / cfg.screen_distance_m, tan_y = tan_x * 0.75;
        kvk_mat4_apply(o, st.quad_mvp[0], centre);
        CHECK(fabs(o[0] / o[3]) < 1e-5 && fabs(o[1] / o[3]) < 1e-5 && o[3] > 0, "quad centre at the view centre");
        CHECK(o[2] / o[3] > 0 && o[2] / o[3] < 1, "quad within depth range (%g)", o[2] / o[3]);
        kvk_mat4_apply(o, st.quad_mvp[0], right);
        CHECK(fabs(o[0] / o[3] - tan_x / 1.0) < 1e-4, "quad right edge %g want %g", o[0] / o[3], tan_x);
        kvk_mat4_apply(o, st.quad_mvp[0], top);
        CHECK(fabs(o[1] / o[3] - tan_y / 0.75) < 1e-4, "quad top edge %g want %g (y up before the shader's flip)", o[1] / o[3], tan_y / 0.75);
    }
}

int main(void)
{
    srand(1234);
    test_inverse();
    test_forms();
    test_parallax();
    test_wrong_depth_row();
    test_frame();
    printf("kvk-stereo: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
