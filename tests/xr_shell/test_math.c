/* xr-shell: pose, camera, projection, aim and weapon math (port/quest/src/vr_game_math.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vr_game_math.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

static int vnear(VrVec3 a, VrVec3 b, double eps) { return NEAR(a.x, b.x, eps) && NEAR(a.y, b.y, eps) && NEAR(a.z, b.z, eps); }
static int mnear(VrMat4 a, VrMat4 b, double eps)
{
    int i;
    for (i = 0; i < 16; i++) if (!NEAR(a.m[i], b.m[i], eps)) return 0;
    return 1;
}
static VrPose pose(float yaw_deg, float pitch_deg, float roll_deg, float x, float y, float z)
{
    VrPose p;
    const VrQuat qy = vrm_q_axis_angle(vrm_v3(0, 1, 0), (float)(yaw_deg * VRM_DEG));
    const VrQuat qp = vrm_q_axis_angle(vrm_v3(1, 0, 0), (float)(pitch_deg * VRM_DEG));
    const VrQuat qr = vrm_q_axis_angle(vrm_v3(0, 0, 1), (float)(roll_deg * VRM_DEG));
    p.orientation = vrm_q_mul(qy, vrm_q_mul(qp, qr));
    p.position = vrm_v3(x, y, z);
    return p;
}
static void apply4(VrMat4 m, VrVec3 v, float w, float out[4])
{
    const float in[4] = { v.x, v.y, v.z, w };
    vrm_m4_apply(m, in, out);
}

static void test_quat_pose(void)
{
    const VrPose a = pose(30, -10, 5, 1, 2, 3), b = pose(-70, 20, 0, -0.5f, 0.1f, 0.7f);
    const VrPose ab = vrm_pose_mul(a, b), back = vrm_pose_mul(vrm_pose_inv(a), ab);
    const VrVec3 v = vrm_v3(0.3f, -0.2f, 0.9f);
    VrMat4 inv;
    CHECK(vnear(back.position, b.position, 1e-5));
    CHECK(vnear(vrm_q_rotate(back.orientation, v), vrm_q_rotate(b.orientation, v), 1e-5));
    /* matrix of a pose and its inverse */
    CHECK(vrm_m4_inverse(vrm_m4_from_pose(a), &inv));
    CHECK(mnear(inv, vrm_m4_view_from_pose(a), 1e-5));
    CHECK(mnear(vrm_m4_mul(vrm_m4_from_pose(a), vrm_m4_from_pose(b)), vrm_m4_from_pose(ab), 1e-5));
    /* yaw: +90 about +y turns forward (-z) to -x, which is "left" */
    CHECK(NEAR(vrm_q_yaw(pose(90, 0, 0, 0, 0, 0).orientation) / VRM_DEG, 90.0, 1e-4));
    CHECK(NEAR(vrm_q_yaw(pose(-35, 40, 10, 0, 0, 0).orientation) / VRM_DEG, -35.0, 1e-3));
    CHECK(vnear(vrm_q_rotate(pose(90, 0, 0, 0, 0, 0).orientation, vrm_v3(0, 0, -1)), vrm_v3(-1, 0, 0), 1e-6));
    CHECK(NEAR(vrm_wrap_deg(190.0), -170.0, 1e-9) && NEAR(vrm_wrap_deg(-180.0), -180.0, 1e-9));
}

static void test_projection(void)
{
    VrFov f = { -0.9f, 0.7f, 0.8f, -0.85f };
    const VrMat4 p = vrm_proj_vulkan(f, 0.1f, 100.0f);
    float o[4];
    /* the right edge at depth 2 (view -z) maps to NDC x = +1; the top edge to Vulkan y = -1 */
    apply4(p, vrm_v3(tanf(f.right) * 2.0f, 0, -2.0f), 1, o);
    CHECK(NEAR(o[0] / o[3], 1.0, 1e-5));
    apply4(p, vrm_v3(0, tanf(f.up) * 2.0f, -2.0f), 1, o);
    CHECK(NEAR(o[1] / o[3], -1.0, 1e-5));
    apply4(p, vrm_v3(0, 0, -0.1f), 1, o);
    CHECK(NEAR(o[2] / o[3], 0.0, 1e-5));
    apply4(p, vrm_v3(0, 0, -100.0f), 1, o);
    CHECK(NEAR(o[2] / o[3], 1.0, 1e-5));
    /* D3D LH: +z forward, clip y up, z = a + b/z */
    {
        float a, b;
        VrMat4 d;
        CHECK(vgm_depth_row(0.5f, 500.0f, &a, &b));
        d = vrm_proj_d3d_lh(f, a, b);
        apply4(d, vrm_v3(tanf(f.left) * 3.0f, tanf(f.up) * 3.0f, 3.0f), 1, o);
        CHECK(NEAR(o[0] / o[3], -1.0, 1e-5) && NEAR(o[1] / o[3], 1.0, 1e-5));
        apply4(d, vrm_v3(0, 0, 0.5f), 1, o);
        CHECK(NEAR(o[2] / o[3], 0.0, 1e-5));
        apply4(d, vrm_v3(0, 0, 500.0f), 1, o);
        CHECK(NEAR(o[2] / o[3], 1.0, 1e-4));
        CHECK(!vgm_depth_row(0.0f, 1.0f, &a, &b) && !vgm_depth_row(2.0f, 1.0f, &a, &b));
    }
}

/* A world-space point seen by the game camera, then by each eye: the per-eye
 * transform must move it exactly as the eye's own view does. */
static void test_eye_transforms(void)
{
    const float upm = 2.5f;
    const VrPose head = pose(20, -5, 0, 0.1f, 1.6f, -0.2f);
    VrPose eye[2];
    VrFov fov[2] = { { -0.95f, 0.8f, 0.85f, -0.9f }, { -0.8f, 0.95f, 0.85f, -0.9f } };
    VrGameCamera cam;
    VrGameCameraExt ext;
    VrEyeTransforms tf;
    VrPose origin = vgm_recentre_origin(head);
    int i;
    eye[0] = vrm_pose_mul(head, pose(0, 0, 0, -0.032f, 0, 0));
    eye[1] = vrm_pose_mul(head, pose(0, 0, 0, 0.032f, 0, 0));
    memset(&cam, 0, sizeof(cam));
    memset(&ext, 0, sizeof(ext));
    cam.valid = 1;
    cam.half_fov_x = 1.4f;
    cam.half_fov_y = 1.3f;
    ext.head_pose = head;
    CHECK(vgm_depth_row(0.3f, 300.0f, &ext.depth_a, &ext.depth_b));
    vgm_eye_transforms(&cam, &ext, origin, 0.5f, head, eye, fov, upm, &tf);
    CHECK(tf.valid && tf.clip_valid);
    for (i = 0; i < 2; i++) {
        /* a point 3 m in front-left of the head, in head-local OpenXR metres */
        const VrVec3 p_head = vrm_v3(-0.4f, 0.2f, -3.0f);
        const VrVec3 p_stage = vrm_pose_apply(head, p_head);
        const VrVec3 p_eye = vrm_pose_apply(vrm_pose_inv(eye[i]), p_stage);
        float in[4], out[4], clip_g[4], clip_e[4];
        /* game view LH units: flip z, scale */
        const VrVec3 g = vrm_v3(p_head.x * upm, p_head.y * upm, -p_head.z * upm);
        apply4(tf.eye_from_game_view[i], g, 1, out);
        CHECK(NEAR(out[0], p_eye.x * upm, 1e-4) && NEAR(out[1], p_eye.y * upm, 1e-4) && NEAR(out[2], -p_eye.z * upm, 1e-4));
        /* clip_from_game_clip: game clip -> eye clip */
        apply4(vgm_game_proj(cam.half_fov_x, cam.half_fov_y, ext.depth_a, ext.depth_b), g, 1, clip_g);
        vrm_m4_apply(tf.clip_from_game_clip[i], clip_g, clip_e);
        in[0] = out[0]; in[1] = out[1]; in[2] = out[2]; in[3] = 1;
        vrm_m4_apply(tf.proj[i], in, out);
        CHECK(NEAR(clip_e[0] / clip_e[3], out[0] / out[3], 1e-4));
        CHECK(NEAR(clip_e[1] / clip_e[3], out[1] / out[3], 1e-4));
        CHECK(NEAR(clip_e[2] / clip_e[3], out[2] / out[3], 1e-4));
    }
    /* stereo: the two eyes see the point at different x (disparity), same y */
    {
        float a[4], b[4];
        const VrVec3 g = vrm_v3(0, 0, 2.0f * upm);
        apply4(tf.eye_from_game_view[0], g, 1, a);
        apply4(tf.eye_from_game_view[1], g, 1, b);
        CHECK(NEAR(a[0] - b[0], 0.064 * upm, 1e-4) && NEAR(a[1], b[1], 1e-5));
    }
    /* late latch: a head that moved 10 cm right after the game frame shifts the world 10 cm left */
    {
        VrPose head2 = vrm_pose_mul(head, pose(0, 0, 0, 0.1f, 0, 0)), eye2[2];
        float a[4];
        eye2[0] = vrm_pose_mul(head2, pose(0, 0, 0, -0.032f, 0, 0));
        eye2[1] = vrm_pose_mul(head2, pose(0, 0, 0, 0.032f, 0, 0));
        vgm_eye_transforms(&cam, &ext, origin, 0.5f, head2, eye2, fov, upm, &tf);
        apply4(tf.eye_from_game_view[0], vrm_v3(0, 0, 2.0f * upm), 1, a);
        CHECK(NEAR(a[0], (0.032 - 0.1) * upm, 1e-4));
        /* and the head clamp holds at render time: a 2 m lean stops at 0.5 m */
        head2 = vrm_pose_mul(origin, pose(0, 0, 0, 2.0f, 0, 0));
        eye2[0] = eye2[1] = head2;
        vgm_eye_transforms(&cam, &ext, origin, 0.5f, head2, eye2, fov, upm, &tf);
        CHECK(NEAR(vrm_len(vgm_clamp_head(origin, head2, 0.5f).position) , vrm_len(vrm_add(origin.position, vrm_q_rotate(origin.orientation, vrm_v3(0.5f, 0, 0)))), 1e-4));
    }
    /* no depth row: placeholder transform, flagged invalid */
    ext.depth_a = ext.depth_b = 0.0f;
    vgm_eye_transforms(&cam, &ext, origin, 0.5f, head, eye, fov, upm, &tf);
    CHECK(!tf.clip_valid && mnear(tf.clip_from_game_clip[0], vrm_m4_identity(), 0));
}

static void test_frame_rows(void)
{
    /* RenderWare rows right/up/at/pos with right = screen LEFT (sign -1) */
    const float rows[4][3] = { { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 5, 6, 7 } };
    float back[4][3];
    VrMat4 w = vgm_frame_to_view(rows, -1.0f);
    int r, k;
    CHECK(vnear(vgm_col(w, 0), vrm_v3(1, 0, 0), 0) && vnear(vgm_col(w, 3), vrm_v3(5, 6, 7), 0));
    vgm_view_to_frame(w, -1.0f, back);
    for (r = 0; r < 4; r++) for (k = 0; k < 3; k++) CHECK(back[r][k] == rows[r][k]);
    /* LH from pose: the OpenXR forward (-z) becomes +z, metres become units */
    w = vgm_lh_from_pose(pose(90, 0, 0, 1, 2, -3), 10.0f);
    CHECK(vnear(vgm_col(w, 3), vrm_v3(10, 20, 30), 1e-5));
    /* yaw +90 (left): the LH forward column must point to OpenXR -x, which is LH -x */
    CHECK(vnear(vgm_col(w, 2), vrm_v3(-1, 0, 0), 1e-6));
    CHECK(vnear(vgm_col(w, 0), vrm_v3(0, 0, 1), 1e-6));   /* screen right: OpenXR -z -> LH +z */
}

/* The body frame keeps the game's handedness and an exactly level horizon. */
static void test_body_and_yaw(void)
{
    int hand;
    for (hand = 0; hand < 2; hand++) {
        /* a game world of either handedness: screen right = +-(cos, 0, -sin) of the forward yaw */
        const double hs = hand ? -1.0 : 1.0, psi0 = 0.7;
        const VrVec3 F0 = vgm_dir_from_world_yaw(psi0);
        const VrVec3 S0 = vrm_v3((float)(hs * cos(psi0)), 0, (float)(-hs * sin(psi0)));
        const VrVec3 U0 = vrm_v3(0, 1, 0);
        const double sign = vgm_yaw_sign(F0, S0);
        VrMat4 b = vgm_body_frame(1.2, vrm_v3(1, 2, 3), F0, S0, U0);
        VgmYawCal c;
        double g = 37.0;
        int i;
        CHECK(vnear(vgm_col(b, 2), vgm_dir_from_world_yaw(1.2), 1e-6));
        CHECK(NEAR(vgm_col(b, 0).y, 0, 0) && NEAR(vgm_col(b, 1).y, 1, 0));
        /* same handedness as the game: screen right x up . forward has the game's sign */
        CHECK((vrm_dot(vrm_cross(vgm_col(b, 0), vgm_col(b, 1)), vgm_col(b, 2)) > 0) ==
              (vrm_dot(vrm_cross(S0, U0), F0) > 0));
        /* a left turn (game yaw grows) moves the forward toward the screen's left */
        {
            const VrVec3 f1 = vgm_dir_from_world_yaw(psi0 + sign * 5.0 * VRM_DEG);
            CHECK(vrm_dot(vrm_sub(f1, F0), S0) < 0.0f);
        }
        /* calibration: world_yaw = sign * g + off, with noise (shake) */
        vgm_yaw_cal_reset(&c);
        for (i = 0; i < 400; i++) {
            const double off = 1.0, noise = 0.02 * sin(i * 1.7);
            g = vrm_wrap_deg(g + 0.9);
            vgm_yaw_cal_update(&c, vrm_wrap_rad(sign * g * VRM_DEG + off + noise), g, sign, 1.0 / 60.0);
        }
        CHECK(NEAR(vrm_wrap_rad(vgm_yaw_cal_world(&c, g) - vrm_wrap_rad(sign * g * VRM_DEG + 1.0)), 0, 0.02));
        /* an offset near an axis locks exactly */
        vgm_yaw_cal_reset(&c);
        for (i = 0; i < 200; i++) vgm_yaw_cal_update(&c, vrm_wrap_rad(sign * g * VRM_DEG + VRM_PI / 2 + 0.01), g, sign, 1.0 / 60.0);
        CHECK(c.locked && NEAR(c.off, VRM_PI / 2, 1e-12));
        /* a persistent jump (camera re-seeded) is re-learned within a quarter second */
        for (i = 0; i < 20; i++) vgm_yaw_cal_update(&c, vrm_wrap_rad(sign * g * VRM_DEG - 2.0), g, sign, 1.0 / 60.0);
        CHECK(NEAR(vrm_wrap_rad(c.off + 2.0), 0, 1e-6) && !c.locked);
    }
}

static void test_body_yaw_step(void)
{
    double B = vgm_body_yaw_step(0, 0, 50.0, 10.0, 0.0, 20.0);
    CHECK(NEAR(B, 40.0, 1e-12));
    /* small drift from the game (aim assist): ignored */
    B = vgm_body_yaw_step(B, 1, 50.0 + 3.0, 10.0, 0.0, 20.0);
    CHECK(NEAR(B, 40.0, 1e-12));
    /* a big change (respawn facing): absorbed */
    B = vgm_body_yaw_step(B, 1, 50.0 + 90.0, 10.0, 0.0, 20.0);
    CHECK(NEAR(B, 130.0, 1e-12));
    /* snap turn and wrap */
    B = vgm_body_yaw_step(170.0, 1, 180.0, 10.0, 30.0, 20.0);
    CHECK(NEAR(B, -160.0, 1e-12));
}

static void test_aim(void)
{
    const VrPose head = pose(0, 0, 0, 0, 0, 0);
    /* a hand 25 cm right and 30 cm low pointing straight ahead, converging at 10 m */
    const VrPose hand = pose(0, 0, 0, 0.25f, -0.3f, 0);
    double y, p;
    VrVec3 d = vgm_aim_dir(1, head, hand, vrm_v3(0, 0, 0), 10.0f);
    vgm_dir_angles(d, &y, &p);
    CHECK(NEAR(y, -atan2(0.25, 10.0) / VRM_DEG, 1e-4));        /* slightly right: negative = right */
    CHECK(NEAR(p, atan2(-0.3, sqrt(100.0 + 0.0625)) / VRM_DEG, 1e-4));
    /* the shot (from the head) crosses the hand's ray at 10 m */
    {
        const VrVec3 hit = vrm_scale(d, sqrtf(10.0f * 10.0f + 0.25f * 0.25f + 0.3f * 0.3f));
        CHECK(vnear(hit, vrm_v3(0.25f, -0.3f, -10.0f), 1e-4));
    }
    /* head mode ignores the hand */
    vgm_dir_angles(vgm_aim_dir(0, pose(-30, 15, 0, 0, 0, 0), hand, vrm_v3(0, 0, 0), 10.0f), &y, &p);
    CHECK(NEAR(y, -30.0, 1e-3) && NEAR(p, 15.0, 1e-3));
    /* a hand turned 40 degrees left */
    vgm_dir_angles(vgm_aim_dir(1, head, pose(40, 0, 0, 0, 0, 0), vrm_v3(0, 0, 0), 10.0f), &y, &p);
    CHECK(NEAR(y, 40.0, 1e-3));
}

static void test_fov_union(void)
{
    VrPose eye[2];
    VrFov f[2] = { { -0.9f, 0.7f, 0.8f, -0.85f }, { -0.7f, 0.9f, 0.8f, -0.85f } };
    float hx, hy;
    eye[0] = pose(0, 0, 0, -0.03f, 0, 0);
    eye[1] = pose(0, 0, 0, 0.03f, 0, 0);
    vgm_fov_union(pose(0, 0, 0, 0, 0, 0), eye, f, 1.0f, &hx, &hy);
    CHECK(NEAR(hx, tan(0.9), 1e-5) && NEAR(hy, tan(0.85), 1e-5));
    vgm_fov_union(pose(0, 0, 0, 0, 0, 0), eye, f, 1.1f, &hx, &hy);
    CHECK(NEAR(hx, tan(0.9) * 1.1, 1e-5));
    /* canted displays: each eye yawed outward 5 degrees widens the union */
    eye[0] = pose(5, 0, 0, -0.03f, 0, 0);
    eye[1] = pose(-5, 0, 0, 0.03f, 0, 0);
    vgm_fov_union(pose(0, 0, 0, 0, 0, 0), eye, f, 1.0f, &hx, &hy);
    CHECK(hx > tan(0.9) * 1.05);
}

static void test_bob(void)
{
    VgmBob b = { 0, 0 };
    float y = 0;
    int i;
    double peak = 0;
    vgm_bob_filter(&b, 10.0f, 1.0 / 60.0, 0.2, 5.0f);
    /* 2 Hz, 4 cm bob is attenuated by more than half (first-order, tau 0.2 s: to 37%) */
    for (i = 0; i < 600; i++) {
        y = vgm_bob_filter(&b, 10.0f + 0.04f * sinf((float)(i / 60.0 * 2.0 * VRM_PI * 2.0)), 1.0 / 60.0, 0.2, 5.0f);
        if (i > 120 && fabs(y - 10.0) > peak) peak = fabs(y - 10.0);
    }
    CHECK(peak < 0.02);
    /* a step down a ledge (> snap) is taken at once */
    CHECK(vgm_bob_filter(&b, 3.0f, 1.0 / 60.0, 0.2, 5.0f) == 3.0f);
    /* a crouch (smaller than snap) is followed within a second */
    for (i = 0; i < 60; i++) y = vgm_bob_filter(&b, 2.0f, 1.0 / 60.0, 0.2, 5.0f);
    CHECK(fabs(y - 2.0f) < 0.02f);
}

/* A synthetic first-person MVP: P * V * M; re-targeting moves the weapon by Yv in view space. */
static void test_weapon_retarget(void)
{
    float a, b, r[16], r2[16];
    VrMat4 P, V, M, mvp, yv, expect;
    VgmClipCal c;
    int i, j;
    CHECK(vgm_depth_row(0.1f, 400.0f, &a, &b));
    P = vgm_game_proj(1.2f, 0.9f, a, b);
    V = vrm_m4_view_from_pose(pose(30, 10, 0, 4, 5, 6));
    M = vrm_m4_from_pose(pose(-20, 5, 3, 4.2f, 4.8f, 6.5f));
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) VRM_AT(M, i, j) *= 0.75f;   /* uniform model scale */
    mvp = vrm_m4_mul(P, vrm_m4_mul(V, M));
    /* the upload is the transposed MVP: row-major rows x, y, z, w */
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) r[i * 4 + j] = VRM_AT(mvp, i, j);
    CHECK(vgm_clip_calibrate(r, &c));
    CHECK(NEAR(c.p00, 1.0 / 1.2, 1e-5) && NEAR(c.p11, 1.0 / 0.9, 1e-5) && NEAR(c.a, a, 1e-4) && NEAR(c.b, b, 1e-4));
    yv = vrm_m4_from_pose(pose(15, -8, 4, 0.2f, -0.3f, 0.1f));
    memcpy(r2, r, sizeof(r));
    CHECK(vgm_mvp_retarget(r2, yv, 1.0));
    expect = vrm_m4_mul(P, vrm_m4_mul(yv, vrm_m4_mul(V, M)));
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) CHECK(NEAR(r2[i * 4 + j], VRM_AT(expect, i, j), 2e-4 * (1 + fabs(VRM_AT(expect, i, j)))));
    /* identity re-target leaves the upload as it was */
    memcpy(r2, r, sizeof(r));
    CHECK(vgm_mvp_retarget(r2, vrm_m4_identity(), 1.0));
    for (i = 0; i < 16; i++) CHECK(NEAR(r2[i], r[i], 1e-4 * (1 + fabs(r[i]))));
    /* clip y pointing down (if the game's matrices do that): learned sign keeps it right */
    {
        VrMat4 Pd = P, expect_d;
        VRM_AT(Pd, 1, 1) = -VRM_AT(Pd, 1, 1);
        mvp = vrm_m4_mul(Pd, vrm_m4_mul(V, M));
        for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) r[i * 4 + j] = VRM_AT(mvp, i, j);
        CHECK(vgm_mvp_retarget(r, yv, -1.0));
        expect_d = vrm_m4_mul(Pd, vrm_m4_mul(yv, vrm_m4_mul(V, M)));
        for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) CHECK(NEAR(r[i * 4 + j], VRM_AT(expect_d, i, j), 2e-4 * (1 + fabs(VRM_AT(expect_d, i, j)))));
    }
    /* a non-perspective (orthographic HUD) matrix is refused */
    {
        float ortho[16] = { 0.01f, 0, 0, -1, 0, -0.01f, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1 };
        CHECK(!vgm_mvp_retarget(ortho, yv, 1.0));
    }
    /* axes of a static draw are the view's own axes */
    {
        VrVec3 x, y, z;
        mvp = vrm_m4_mul(P, V);
        for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) r[i * 4 + j] = VRM_AT(mvp, i, j);
        CHECK(vgm_mvp_axes(r, &x, &y, &z));
        CHECK(vnear(x, vrm_v3(VRM_AT(V, 0, 0), VRM_AT(V, 0, 1), VRM_AT(V, 0, 2)), 1e-5));
        CHECK(vnear(z, vrm_v3(VRM_AT(V, 2, 0), VRM_AT(V, 2, 1), VRM_AT(V, 2, 2)), 1e-5));
    }
}

/* The weapon carried by the entity camera lands at the hand with its grip offset removed. */
static void test_weapon_to_hand(void)
{
    const VrMat4 w_head = vgm_lh_from_pose(pose(10, 0, 0, 1, 1.7f, 2), 1.0f);
    const VrMat4 w_entity = vgm_lh_from_pose(pose(25, -12, 0, 1, 1.7f, 2), 1.0f);
    const VrMat4 w_hand = vgm_lh_from_pose(pose(40, -20, 10, 1.3f, 1.3f, 1.7f), 1.0f);
    const VrVec3 grip = vrm_v3(0.12f, -0.18f, 0.3f);
    VrMat4 yv, head_from_world, world_from_head_view, moved;
    float o[4];
    CHECK(vgm_weapon_to_hand(w_head, w_entity, w_hand, grip, &yv));
    CHECK(vrm_m4_inverse(w_head, &head_from_world));
    /* a weapon vertex at the grip point in entity space ends at the hand origin in world space */
    {
        float in[4], v[4];
        const VrMat4 entity_vertex_in_head = vrm_m4_mul(head_from_world, w_entity);
        in[0] = grip.x; in[1] = grip.y; in[2] = grip.z; in[3] = 1;
        vrm_m4_apply(entity_vertex_in_head, in, v);     /* where the game drew it, head view */
        vrm_m4_apply(yv, v, o);                         /* after re-targeting */
        world_from_head_view = w_head;
        vrm_m4_apply(world_from_head_view, o, v);
        CHECK(NEAR(v[0], 1.3, 1e-5) && NEAR(v[1], 1.3, 1e-5) && NEAR(v[2], -1.7, 1e-5));
    }
    /* orientation: the entity's forward becomes the hand's forward */
    moved = vrm_m4_mul(w_head, vrm_m4_mul(yv, vrm_m4_mul(head_from_world, w_entity)));
    CHECK(vnear(vgm_col(moved, 2), vgm_col(w_hand, 2), 1e-5));
}

int main(void)
{
    test_quat_pose();
    test_projection();
    test_frame_rows();
    test_eye_transforms();
    test_body_and_yaw();
    test_body_yaw_step();
    test_aim();
    test_fov_union();
    test_bob();
    test_weapon_retarget();
    test_weapon_to_hand();
    printf("xr_shell math: %u checks passed\n", checks);
    return 0;
}
