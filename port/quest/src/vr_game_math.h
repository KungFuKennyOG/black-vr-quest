/*
 * vr_game_math.h - the pure math behind the BLACK VR camera, aim and
 * first-person weapon hooks (src/black_vr.inc) and the shell's per-eye
 * transforms. Header-only, no guest memory, no OpenXR: everything here is
 * unit-tested natively in tests/xr_shell.
 *
 * Spaces
 *   tracking : OpenXR stage space or a pose relative to the recentre origin.
 *              Right-handed, metres, +y up, -z forward.
 *   game     : BLACK world (RenderWare), game units, world +y up. The
 *              RwCamera frame stores rows right / up / at / pos; the photo
 *              mode work established that the frame's "right" row is the
 *              screen's LEFT (VGM_SCREEN_RIGHT_SIGN = -1).
 *   view LH  : a camera-local space with x = screen right, y = up,
 *              z = forward, game units. This is the space of
 *              VrGameCamera.view and of the D3D-style clip the game's own
 *              MVP produces. world_from_view_LH has columns S, U, A, P.
 *
 * Handedness: nothing here assumes the game world is right- or left-handed.
 * Directions are built from the game's own vectors, and the yaw sign is
 * learned (VgmYawCal), so both conventions behave the same way.
 */
#ifndef VR_GAME_MATH_H
#define VR_GAME_MATH_H

#include "vr_math.h"

#ifndef VGM_SCREEN_RIGHT_SIGN
#define VGM_SCREEN_RIGHT_SIGN (-1.0f)   /* RwCamera right row = screen left (docs/photo-mode.md, pc_photo.c) */
#endif

/* ---- frames --------------------------------------------------------------- */

static inline VrMat4 vgm_world_from_view(VrVec3 S, VrVec3 U, VrVec3 A, VrVec3 P)
{
    VrMat4 m = vrm_m4_identity();
    VRM_AT(m, 0, 0) = S.x; VRM_AT(m, 1, 0) = S.y; VRM_AT(m, 2, 0) = S.z;
    VRM_AT(m, 0, 1) = U.x; VRM_AT(m, 1, 1) = U.y; VRM_AT(m, 2, 1) = U.z;
    VRM_AT(m, 0, 2) = A.x; VRM_AT(m, 1, 2) = A.y; VRM_AT(m, 2, 2) = A.z;
    VRM_AT(m, 0, 3) = P.x; VRM_AT(m, 1, 3) = P.y; VRM_AT(m, 2, 3) = P.z;
    return m;
}
static inline VrVec3 vgm_col(VrMat4 m, int c) { return vrm_v3(VRM_AT(m, 0, c), VRM_AT(m, 1, c), VRM_AT(m, 2, c)); }

/* RwCamera frame rows (right, up, at, pos) <-> world_from_view_LH. */
static inline VrMat4 vgm_frame_to_view(const float rows[4][3], float screen_right_sign)
{
    const VrVec3 S = vrm_scale(vrm_v3(rows[0][0], rows[0][1], rows[0][2]), screen_right_sign);
    return vgm_world_from_view(S, vrm_v3(rows[1][0], rows[1][1], rows[1][2]),
                               vrm_v3(rows[2][0], rows[2][1], rows[2][2]),
                               vrm_v3(rows[3][0], rows[3][1], rows[3][2]));
}
static inline void vgm_view_to_frame(VrMat4 w, float screen_right_sign, float rows[4][3])
{
    int c, k;
    for (c = 0; c < 4; c++) {
        const VrVec3 v = vgm_col(w, c);
        const float s = c == 0 ? screen_right_sign : 1.0f;
        rows[c][0] = v.x * s; rows[c][1] = v.y * s; rows[c][2] = v.z * s;
        for (k = 0; k < 3; k++) if (!isfinite(rows[c][k])) rows[c][k] = 0.0f;
    }
}

/* A tracking-space (RH, metres) pose as a view-LH-to-parent-LH transform in
 * game units: C(T) = F * S * M(T) * S^-1 * F. */
static inline VrMat4 vgm_lh_from_pose(VrPose t, float units_per_metre)
{
    VrMat4 m = vrm_m4_from_pose(t);
    /* conjugating with F flips the signs of entries mixing z with x/y */
    VRM_AT(m, 0, 2) = -VRM_AT(m, 0, 2); VRM_AT(m, 1, 2) = -VRM_AT(m, 1, 2);
    VRM_AT(m, 2, 0) = -VRM_AT(m, 2, 0); VRM_AT(m, 2, 1) = -VRM_AT(m, 2, 1);
    VRM_AT(m, 0, 3) *= units_per_metre;
    VRM_AT(m, 1, 3) *= units_per_metre;
    VRM_AT(m, 2, 3) *= -units_per_metre;
    return m;
}

/* ---- yaw in the game world ------------------------------------------------- */

/* Yaw of a world direction about world +y, measured from +z toward +x. A pure
 * parametrisation of horizontal directions; its relation to the game's own
 * yaw degrees (sign and offset) is learned by VgmYawCal. */
static inline double vgm_world_yaw(VrVec3 v) { return atan2((double)v.x, (double)v.z); }
static inline VrVec3 vgm_dir_from_world_yaw(double psi) { return vrm_v3((float)sin(psi), 0.0f, (float)cos(psi)); }

/* The game's yaw degrees grow to the LEFT (pc_input.h, BLACK's own
 * convention). Which way that turns world_yaw depends on the handedness of
 * the world, which the camera's own screen-right vector tells: returns the
 * sign s with world_yaw = s * radians(game_yaw) + offset. */
static inline double vgm_yaw_sign(VrVec3 F0, VrVec3 S0)
{
    const double psi0 = vgm_world_yaw(vrm_v3(F0.x, 0, F0.z));
    const VrVec3 p0 = vrm_v3((float)cos(psi0), 0.0f, (float)-sin(psi0));   /* d dir / d psi */
    return vrm_dot(p0, S0) >= 0.0f ? -1.0 : 1.0;   /* psi grows to the right -> left turn lowers it */
}

/* offset = world_yaw - sign * radians(game_yaw), low-passed so that yaw
 * shake/recoil in the camera matrix never reaches the VR body frame, while a
 * persistent change (a re-seeded camera) is taken after a short while. */
typedef struct {
    double off;          /* radians */
    double sign;
    int    samples;
    int    miss;         /* consecutive samples far from the estimate */
    int    locked;       /* off snapped to a multiple of 90 degrees */
} VgmYawCal;

static inline void vgm_yaw_cal_reset(VgmYawCal *c) { memset(c, 0, sizeof(*c)); }

static inline void vgm_yaw_cal_update(VgmYawCal *c, double world_yaw, double game_yaw_deg, double sign, double dt)
{
    const double inst = vrm_wrap_rad(world_yaw - sign * game_yaw_deg * VRM_DEG);
    double e;
    if (c->samples == 0 || c->sign != sign) {
        c->off = inst; c->sign = sign; c->samples = 1; c->miss = 0;
        return;
    }
    e = vrm_wrap_rad(inst - c->off);
    if (fabs(e) < 10.0 * VRM_DEG) {
        if (!c->locked) {
            const double alpha = c->samples < 8 ? 0.5 : 1.0 - exp(-(dt > 0.0 ? dt : 1.0 / 60.0) / 1.5);
            const double q = floor(c->off / (VRM_PI / 2.0) + 0.5) * (VRM_PI / 2.0);
            c->off = vrm_wrap_rad(c->off + e * alpha);
            /* a game's yaw origin is an axis: once settled near one, hold it exactly */
            if (c->samples >= 60 && fabs(vrm_wrap_rad(c->off - q)) < 3.0 * VRM_DEG) {
                c->off = vrm_wrap_rad(q);
                c->locked = 1;
            }
        }
        c->miss = 0;
    } else if (++c->miss >= 15) {       /* a quarter second of disagreement: re-seed */
        c->off = inst;
        c->miss = 0;
        c->locked = 0;
        c->samples = 1;
    }
    c->samples++;
}
static inline double vgm_yaw_cal_world(const VgmYawCal *c, double game_yaw_deg)
{
    return vrm_wrap_rad(c->sign * game_yaw_deg * VRM_DEG + c->off);
}

/* Body yaw B (game degrees). The game camera is driven to G = B + a, where a
 * is the aim's yaw relative to the recentred forward. What the game changed
 * on its own since our last write (G_now - (B + a_prev)) is absorbed into B
 * when it is a real turn (>= absorb_deg: respawn, scripted facing, mouse),
 * and dropped when it is drift (aim assist nudges), so the VR world never
 * creeps. Host turns (snap/smooth) are added. */
static inline double vgm_body_yaw_step(double B_prev, int have_prev, double G_now, double a_prev,
                                       double turn_deg, double absorb_deg)
{
    double B;
    if (!have_prev) return vrm_wrap_deg(G_now - a_prev + turn_deg);
    {
        const double change = vrm_wrap_deg(G_now - (B_prev + a_prev));
        B = B_prev + (fabs(change) >= absorb_deg ? change : 0.0) + turn_deg;
    }
    return vrm_wrap_deg(B);
}

/* ---- body frame ------------------------------------------------------------ */

/* The level body frame (world_from_body_LH) at position P, facing world yaw
 * psi, with screen right chosen on the same side as the game camera's own
 * screen right S0 (handedness taken from the game) and up = world +y on the
 * side of the game's up vector U0. */
static inline VrMat4 vgm_body_frame(double psi, VrVec3 P, VrVec3 F0, VrVec3 S0, VrVec3 U0)
{
    const VrVec3 Fb = vgm_dir_from_world_yaw(psi);
    VrVec3 p0, Sb, Ub = vrm_v3(0, U0.y >= 0.0f ? 1.0f : -1.0f, 0);
    const double psi0 = vgm_world_yaw(vrm_v3(F0.x, 0, F0.z));
    float h;
    p0 = vrm_v3((float)cos(psi0), 0.0f, (float)-sin(psi0));
    h = vrm_dot(p0, S0) >= 0.0f ? 1.0f : -1.0f;
    Sb = vrm_scale(vrm_v3((float)cos(psi), 0.0f, (float)-sin(psi)), h);
    return vgm_world_from_view(Sb, Ub, Fb, P);
}

/* ---- head / hand poses relative to the recentre origin ---------------------- */

static inline VrPose vgm_relative(VrPose origin, VrPose stage) { return vrm_pose_mul(vrm_pose_inv(origin), stage); }

/* Recentre origin from a head pose: yaw only, full position. */
static inline VrPose vgm_recentre_origin(VrPose head)
{
    VrPose o;
    o.orientation = vrm_q_from_yaw(vrm_q_yaw(head.orientation));
    o.position = head.position;
    return o;
}

static inline VrVec3 vgm_clamp_len(VrVec3 v, float max_len)
{
    const float l = vrm_len(v);
    if (max_len > 0.0f && l > max_len) return vrm_scale(v, max_len / l);
    return v;
}

/* ---- aim ------------------------------------------------------------------- */

/* Aim direction in the recentred tracking space. HEAD: the head's forward.
 * HAND: from `from` (where the game's own shot starts: the recentre origin,
 * which the game camera position stands for) toward the point `converge_m`
 * metres along the hand's aim ray, so the game's camera-centred shot crosses
 * the hand's ray there. */
static inline VrVec3 vgm_aim_dir(int hand_mode, VrPose head_rel, VrPose hand_aim_rel, VrVec3 from, float converge_m)
{
    VrVec3 d;
    if (!hand_mode) return vrm_q_rotate(head_rel.orientation, vrm_v3(0, 0, -1));
    {
        const VrVec3 f = vrm_q_rotate(hand_aim_rel.orientation, vrm_v3(0, 0, -1));
        const VrVec3 target = vrm_add(hand_aim_rel.position, vrm_scale(f, converge_m > 0.05f ? converge_m : 0.05f));
        d = vrm_sub(target, from);
        if (!vrm_normalize(&d)) d = f;
    }
    return d;
}
/* Relative yaw (degrees, positive = left) and pitch (degrees, positive = up). */
static inline void vgm_dir_angles(VrVec3 d, double *yaw_left_deg, double *pitch_up_deg)
{
    const double h = sqrt((double)d.x * d.x + (double)d.z * d.z);
    *yaw_left_deg = atan2(-(double)d.x, -(double)d.z) / VRM_DEG;
    *pitch_up_deg = atan2((double)d.y, h) / VRM_DEG;
}

/* ---- head bob filter -------------------------------------------------------- */

typedef struct { float y; int init; } VgmBob;
/* Low-passes the vertical position; jumps larger than snap_units are taken at once. */
static inline float vgm_bob_filter(VgmBob *b, float y, double dt, double tau, float snap_units)
{
    if (!b->init || !(fabsf(y - b->y) < snap_units) || !isfinite(b->y)) { b->y = y; b->init = 1; return y; }
    if (tau > 0.0 && dt > 0.0) b->y += (float)((y - b->y) * (1.0 - exp(-dt / tau)));
    else b->y = y;
    return b->y;
}

/* ---- lens: tangent extents of both eyes, as seen from the head ------------------ */

/* Half-extents (tangents at unit forward distance, head space) that cover
 * every eye's frustum, enlarged by `margin`. The eyes' orientation relative
 * to the head (canted displays) is taken into account. */
static inline void vgm_fov_union(const VrPose head, const VrPose eye[VR_EYE_COUNT], const VrFov fov[VR_EYE_COUNT],
                                 float margin, float *half_x, float *half_y)
{
    float hx = 0.0f, hy = 0.0f;
    int i, c;
    for (i = 0; i < VR_EYE_COUNT; i++) {
        const VrQuat rel = vrm_q_mul(vrm_q_conj(head.orientation), eye[i].orientation);
        const float tx[2] = { tanf(fov[i].left), tanf(fov[i].right) };
        const float ty[2] = { tanf(fov[i].down), tanf(fov[i].up) };
        for (c = 0; c < 4; c++) {
            VrVec3 d = vrm_q_rotate(rel, vrm_v3(tx[c & 1], ty[c >> 1], -1.0f));
            if (d.z < -1e-3f) {
                const float ax = fabsf(d.x / -d.z), ay = fabsf(d.y / -d.z);
                if (ax > hx) hx = ax;
                if (ay > hy) hy = ay;
            }
        }
    }
    if (!(hx > 0.05f)) hx = 1.0f;
    if (!(hy > 0.05f)) hy = 1.0f;
    *half_x = hx * margin;
    *half_y = hy * margin;
}

/* D3D-style symmetric LH projection from half-extents and depth row. */
static inline VrMat4 vgm_game_proj(float half_x, float half_y, float a, float b)
{
    VrFov f;
    f.left = -atanf(half_x); f.right = atanf(half_x); f.up = atanf(half_y); f.down = -atanf(half_y);
    return vrm_proj_d3d_lh(f, a, b);
}
/* RenderWare D3D depth row for near/far planes: z_clip = f/(f-n) * (z - n). */
static inline int vgm_depth_row(float near_z, float far_z, float *a, float *b)
{
    if (!(near_z > 0.0f) || !(far_z > near_z * 1.0001f) || !isfinite(far_z)) return 0;
    *a = far_z / (far_z - near_z);
    *b = -near_z * far_z / (far_z - near_z);
    return 1;
}

/* ---- per-eye transforms (shell) ------------------------------------------- */

/* eye_from_game_view = S F inv(E) Hg F S^-1 : game view LH (units) of the
 * head pose Hg the game camera was built from, to eye E's LH view (units). */
static inline VrMat4 vgm_eye_from_game_view(VrPose eye_stage, VrPose head_stage, float units_per_metre)
{
    return vgm_lh_from_pose(vrm_pose_mul(vrm_pose_inv(eye_stage), head_stage), units_per_metre);
}

/* Head pose with its offset from the recentre origin limited to max_m
 * (orientation untouched). Both the game hook and the shell's late latch use
 * this, so the limit holds at render time too. */
static inline VrPose vgm_clamp_head(VrPose origin, VrPose head_stage, float max_m)
{
    VrPose rel = vgm_relative(origin, head_stage);
    rel.position = vgm_clamp_len(rel.position, max_m);
    return vrm_pose_mul(origin, rel);
}

/* All per-eye renderer transforms (see VrEyeTransforms in vr_bridge.h).
 * head_game: the (clamped) stage head pose the game camera stands for.
 * eye/fov  : late-latched eye poses (stage) and FOVs, as submitted.
 * head_late: the late-latched head pose (stage), used to clamp the eyes the
 *            same way. */
static inline void vgm_eye_transforms(const VrGameCamera *cam, const VrGameCameraExt *ext,
                                      VrPose origin, float max_head_m, VrPose head_late,
                                      const VrPose eye[VR_EYE_COUNT], const VrFov fov[VR_EYE_COUNT],
                                      float units_per_metre, VrEyeTransforms *out)
{
    const VrPose head_eff = vgm_clamp_head(origin, head_late, max_head_m);
    const VrPose late_to_eff = vrm_pose_mul(head_eff, vrm_pose_inv(head_late));
    float a = ext->depth_a, b = ext->depth_b;
    VrMat4 game_proj, inv_game_proj;
    int i, have_depth = a != 0.0f && b != 0.0f && isfinite(a) && isfinite(b);
    memset(out, 0, sizeof(*out));
    if (!have_depth) { a = 1.0001f; b = -0.1f; }       /* placeholder near 0.1, far ~1000 */
    game_proj = vgm_game_proj(cam->half_fov_x, cam->half_fov_y, a, b);
    out->clip_valid = have_depth && cam->valid && cam->half_fov_x > 0.0f && cam->half_fov_y > 0.0f &&
                      vrm_m4_inverse(game_proj, &inv_game_proj);
    for (i = 0; i < VR_EYE_COUNT; i++) {
        const VrPose eye_eff = vrm_pose_mul(late_to_eff, eye[i]);
        out->pose[i] = eye_eff;          /* the pose drawn for == the pose submitted (clamped like the head) */
        out->fov[i] = fov[i];
        out->eye_from_game_view[i] = vgm_eye_from_game_view(eye_eff, ext->head_pose, units_per_metre);
        out->proj[i] = vrm_proj_d3d_lh(fov[i], a, b);
        out->clip_from_game_clip[i] = out->clip_valid
            ? vrm_m4_mul(out->proj[i], vrm_m4_mul(out->eye_from_game_view[i], inv_game_proj))
            : vrm_m4_identity();
    }
    out->valid = cam->valid;
}

/* ---- first-person weapon: re-attach the camera-borne weapon to the hand ------- */

/* Clip-space MVP rows as uploaded to c10..c13 (row0 = x, row1 = y, row2 = z,
 * row3 = w; 4 floats each). The game projection is factored out of the MVP
 * itself, assuming only a D3D-style perspective projection with
 * w = view z and an affine model-view:
 *   row0 = p00*x + p02*w, row1 = p11*y + p12*w, row2 = a*w + b*(0,0,0,1).
 * The model-view rows are orthogonal with equal norms (rotation * uniform
 * scale), which recovers p00, p02, p11, p12, a and b exactly per draw. */
typedef struct { double p00, p02, p11, p12, a, b; } VgmClipCal;

static inline int vgm_clip_calibrate(const float r[16], VgmClipCal *c)
{
    double w2, x0[3], y0[3], wx, k;
    int i, best = 0;
    w2 = (double)r[12] * r[12] + (double)r[13] * r[13] + (double)r[14] * r[14];
    if (!(w2 > 1e-20) || !isfinite(w2)) return 0;
    c->p02 = ((double)r[0] * r[12] + (double)r[1] * r[13] + (double)r[2] * r[14]) / w2;
    c->p12 = ((double)r[4] * r[12] + (double)r[5] * r[13] + (double)r[6] * r[14]) / w2;
    for (i = 0; i < 3; i++) { x0[i] = r[i] - c->p02 * r[12 + i]; y0[i] = r[4 + i] - c->p12 * r[12 + i]; }
    c->p00 = sqrt((x0[0] * x0[0] + x0[1] * x0[1] + x0[2] * x0[2]) / w2);
    c->p11 = sqrt((y0[0] * y0[0] + y0[1] * y0[1] + y0[2] * y0[2]) / w2);
    if (!(c->p00 > 1e-6) || !(c->p11 > 1e-6)) return 0;
    /* the depth row is a*w (+ b in the translation column) */
    for (i = 1; i < 3; i++) if (fabs(r[12 + i]) > fabs(r[12 + best])) best = i;
    c->a = (double)r[8 + best] / (double)r[12 + best];
    c->b = (double)r[11] - c->a * (double)r[15];
    /* verify the model-view is orthogonal-uniform and the depth row matches */
    wx = (x0[0] * r[12] + x0[1] * r[13] + x0[2] * r[14]) / (c->p00 * w2);
    k = fabs((double)r[8] - c->a * r[12]) + fabs((double)r[9] - c->a * r[13]) + fabs((double)r[10] - c->a * r[14]);
    if (fabs(wx) > 1e-3 || k > 1e-3 * (1.0 + fabs(c->a) * sqrt(w2))) return 0;
    if (!isfinite(c->a) || !isfinite(c->b)) return 0;
    return 1;
}

/* new MVP = P * Yv * (P^-1 * MVP), with P from the calibration and Yv a
 * view-space (LH, game units, y up) rigid transform. `clip_y_sign` is the
 * sign of P's y scale (+1: clip y up, as D3D; -1 if the game's matrices put
 * screen-down in clip y), learned from static draws by the caller. */
static inline int vgm_mvp_retarget(float r[16], VrMat4 yv, double clip_y_sign)
{
    VgmClipCal c;
    double vw[4][4], out[4][4];
    int i, j, k;
    if (!vgm_clip_calibrate(r, &c)) return 0;
    if (clip_y_sign < 0.0) c.p11 = -c.p11;
    for (j = 0; j < 4; j++) {
        vw[0][j] = (r[j] - c.p02 * r[12 + j]) / c.p00;
        vw[1][j] = (r[4 + j] - c.p12 * r[12 + j]) / c.p11;
        vw[2][j] = r[12 + j];
        vw[3][j] = j == 3 ? 1.0 : 0.0;
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double sum = 0.0;
            for (k = 0; k < 4; k++) sum += (double)VRM_AT(yv, i, k) * vw[k][j];
            out[i][j] = sum;
        }
    for (j = 0; j < 4; j++) {
        const double x = c.p00 * out[0][j] + c.p02 * out[2][j];
        const double y = c.p11 * out[1][j] + c.p12 * out[2][j];
        const double z = c.a * out[2][j] + (j == 3 ? c.b : 0.0);
        const double w = out[2][j];
        if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(w)) return 0;
        r[j] = (float)x; r[4 + j] = (float)y; r[8 + j] = (float)z; r[12 + j] = (float)w;
    }
    return 1;
}

/* The model-view (view LH from model, game units) a weapon MVP was built with: P^-1 * MVP, as vgm_mvp_retarget
 * factors it. Returns 0 if the MVP does not calibrate. */
static inline int vgm_mvp_modelview(const float r[16], double clip_y_sign, VrMat4 *mv)
{
    VgmClipCal c;
    int j;
    if (!vgm_clip_calibrate(r, &c)) return 0;
    if (clip_y_sign < 0.0) c.p11 = -c.p11;
    *mv = vrm_m4_identity();
    for (j = 0; j < 4; j++) {
        VRM_AT(*mv, 0, j) = (float)(((double)r[j] - c.p02 * r[12 + j]) / c.p00);
        VRM_AT(*mv, 1, j) = (float)(((double)r[4 + j] - c.p12 * r[12 + j]) / c.p11);
        VRM_AT(*mv, 2, j) = r[12 + j];
        VRM_AT(*mv, 3, j) = j == 3 ? 1.0f : 0.0f;
    }
    return 1;
}

/* The gun hand's aim frame from the weapon's model space (first-person arms, port/notes/arms.md): K = hand_from_model
 * (LH, game units) = back(grip) * entity_from_world * world_from_head * MV, the chain vgm_weapon_to_hand re-targets
 * the weapon with (a model point p lands at world_from_hand * K * p). */
static inline int vgm_weapon_hand_frame(VrMat4 world_from_head, VrMat4 world_from_entity, VrVec3 grip_units, VrMat4 mv,
                                        VrMat4 *k)
{
    VrMat4 entity_from_world, back = vrm_m4_identity();
    if (!vrm_m4_inverse(world_from_entity, &entity_from_world)) return 0;
    VRM_AT(back, 0, 3) = -grip_units.x; VRM_AT(back, 1, 3) = -grip_units.y; VRM_AT(back, 2, 3) = -grip_units.z;
    *k = vrm_m4_mul(back, vrm_m4_mul(entity_from_world, vrm_m4_mul(world_from_head, mv)));
    return 1;
}

/* The off-hand mirror in model space: A = K^-1 * rel * S * K, rel = gun_hand_from_off_hand (LH, game units),
 * S = diag(-1, 1, 1): takes the gun hand's pose to the off hand's, mirrored left/right. */
static inline int vgm_off_hand_mirror(VrMat4 k, VrMat4 rel, VrMat4 *a)
{
    VrMat4 k_inv, s = vrm_m4_identity();
    if (!vrm_m4_inverse(k, &k_inv)) return 0;
    VRM_AT(s, 0, 0) = -1.0f;
    *a = vrm_m4_mul(k_inv, vrm_m4_mul(rel, vrm_m4_mul(s, k)));
    return 1;
}

/* The off hand relative to the gun hand (OpenXR stage poses, both at one time), for the arms: snapped onto the gun
 * line while gripping two-handed (straight ahead of the gun hand, same orientation: the off hand then mirrors the
 * gun hand's grip, further along the gun), as LH game units. */
static inline VrMat4 vgm_off_hand_rel(VrPose gun, VrPose off, int gripping, float units_per_metre)
{
    VrPose rel = vrm_pose_mul(vrm_pose_inv(gun), off);
    if (gripping) {
        const float ahead = rel.position.z < -0.08f ? rel.position.z : -0.08f;   /* OpenXR: forward is -z */
        rel.position = vrm_v3(0.0f, 0.0f, ahead);
        rel.orientation = vrm_q(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return vgm_lh_from_pose(rel, units_per_metre);
}

/* The view axes a world MVP was built with (unit vectors in the draw's model
 * space): x = screen right, y = clip +y, z = forward. For a draw whose model
 * rotation is identity these are the camera's world axes. */
static inline int vgm_mvp_axes(const float r[16], VrVec3 *x, VrVec3 *y, VrVec3 *z)
{
    VgmClipCal c;
    if (!vgm_clip_calibrate(r, &c)) return 0;
    *x = vrm_v3(r[0] - (float)c.p02 * r[12], r[1] - (float)c.p02 * r[13], r[2] - (float)c.p02 * r[14]);
    *y = vrm_v3(r[4] - (float)c.p12 * r[12], r[5] - (float)c.p12 * r[13], r[6] - (float)c.p12 * r[14]);
    *z = vrm_v3(r[12], r[13], r[14]);
    return vrm_normalize(x) && vrm_normalize(y) && vrm_normalize(z);
}

/* Yv: in the head's LH view, the rigid move that takes a weapon carried by the
 * game camera entity (world_from_entity_LH, the frame before VR replaced it)
 * to the hand (world_from_hand_LH), with the weapon's natural grip offset
 * `grip_units` (entity LH local) landing on the hand origin. */
static inline int vgm_weapon_to_hand(VrMat4 world_from_head, VrMat4 world_from_entity, VrMat4 world_from_hand,
                                     VrVec3 grip_units, VrMat4 *yv)
{
    VrMat4 head_from_world, entity_from_world, x, back = vrm_m4_identity();
    if (!vrm_m4_inverse(world_from_head, &head_from_world) || !vrm_m4_inverse(world_from_entity, &entity_from_world))
        return 0;
    VRM_AT(back, 0, 3) = -grip_units.x; VRM_AT(back, 1, 3) = -grip_units.y; VRM_AT(back, 2, 3) = -grip_units.z;
    x = vrm_m4_mul(world_from_hand, vrm_m4_mul(back, entity_from_world));
    *yv = vrm_m4_mul(head_from_world, vrm_m4_mul(x, world_from_head));
    return 1;
}

#endif /* VR_GAME_MATH_H */
