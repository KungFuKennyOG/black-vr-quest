/*
 * vr_math.h - small header-only vector/quaternion/matrix helpers for the
 * Quest shell and the BLACK VR game hooks. Pure C99, no platform headers,
 * so the same code is compiled into the APK, into recomp_manual.c (through
 * src/black_vr.inc) and into the native tests (tests/xr_shell).
 *
 * Conventions (same as vr_bridge.h):
 *   - VrMat4 is column-major: m[col*4 + row]; vectors are columns (M * v).
 *   - quaternions x,y,z,w; rotation of v by q is q * v * q^-1.
 *   - OpenXR spaces are right-handed: +x right, +y up, -z forward.
 */
#ifndef VR_MATH_H
#define VR_MATH_H

#include <math.h>
#include <string.h>
#include "../vr_bridge.h"

#ifndef VRM_PI
#define VRM_PI 3.14159265358979323846
#endif
#define VRM_DEG (VRM_PI / 180.0)

static inline VrVec3 vrm_v3(float x, float y, float z) { VrVec3 v; v.x = x; v.y = y; v.z = z; return v; }
static inline VrVec3 vrm_add(VrVec3 a, VrVec3 b) { return vrm_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline VrVec3 vrm_sub(VrVec3 a, VrVec3 b) { return vrm_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline VrVec3 vrm_scale(VrVec3 a, float s) { return vrm_v3(a.x * s, a.y * s, a.z * s); }
static inline float  vrm_dot(VrVec3 a, VrVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline VrVec3 vrm_cross(VrVec3 a, VrVec3 b)
{
    return vrm_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float vrm_len(VrVec3 a) { return sqrtf(vrm_dot(a, a)); }
static inline int vrm_normalize(VrVec3 *a)
{
    const float l = vrm_len(*a);
    if (!(l > 1e-9f) || !isfinite(l)) return 0;
    *a = vrm_scale(*a, 1.0f / l);
    return 1;
}

/* ---- quaternions -------------------------------------------------------- */

static inline VrQuat vrm_q(float x, float y, float z, float w) { VrQuat q; q.x = x; q.y = y; q.z = z; q.w = w; return q; }
static inline VrQuat vrm_q_identity(void) { return vrm_q(0, 0, 0, 1); }
static inline VrQuat vrm_q_mul(VrQuat a, VrQuat b)
{
    return vrm_q(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                 a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
static inline VrQuat vrm_q_conj(VrQuat q) { return vrm_q(-q.x, -q.y, -q.z, q.w); }
static inline VrQuat vrm_q_normalize(VrQuat q)
{
    const float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 1e-9f) || !isfinite(l)) return vrm_q_identity();
    return vrm_q(q.x / l, q.y / l, q.z / l, q.w / l);
}
static inline VrQuat vrm_q_axis_angle(VrVec3 axis, float radians)
{
    const float s = sinf(radians * 0.5f);
    vrm_normalize(&axis);
    return vrm_q(axis.x * s, axis.y * s, axis.z * s, cosf(radians * 0.5f));
}
static inline VrVec3 vrm_q_rotate(VrQuat q, VrVec3 v)
{
    /* v + 2w(u x v) + 2 u x (u x v) */
    const VrVec3 u = vrm_v3(q.x, q.y, q.z);
    const VrVec3 t = vrm_scale(vrm_cross(u, v), 2.0f);
    return vrm_add(vrm_add(v, vrm_scale(t, q.w)), vrm_cross(u, t));
}
/* Yaw about +y (radians, positive = counter-clockwise seen from above =
 * turning left) of an orientation whose forward is -z. */
static inline float vrm_q_yaw(VrQuat q)
{
    const VrVec3 f = vrm_q_rotate(q, vrm_v3(0, 0, -1));
    return atan2f(-f.x, -f.z);
}
static inline VrQuat vrm_q_from_yaw(float radians) { return vrm_q_axis_angle(vrm_v3(0, 1, 0), radians); }

/* ---- poses -------------------------------------------------------------- */

static inline VrPose vrm_pose_identity(void) { VrPose p; p.orientation = vrm_q_identity(); p.position = vrm_v3(0, 0, 0); return p; }
/* a * b: apply b, then a (b is expressed in a's frame). */
static inline VrPose vrm_pose_mul(VrPose a, VrPose b)
{
    VrPose r;
    r.orientation = vrm_q_normalize(vrm_q_mul(a.orientation, b.orientation));
    r.position = vrm_add(a.position, vrm_q_rotate(a.orientation, b.position));
    return r;
}
static inline VrPose vrm_pose_inv(VrPose a)
{
    VrPose r;
    r.orientation = vrm_q_conj(a.orientation);
    r.position = vrm_scale(vrm_q_rotate(r.orientation, a.position), -1.0f);
    return r;
}
static inline VrVec3 vrm_pose_apply(VrPose a, VrVec3 v) { return vrm_add(a.position, vrm_q_rotate(a.orientation, v)); }

/* ---- 4x4 matrices (column-major) ----------------------------------------- */

#define VRM_AT(M, row, col) ((M).m[(col) * 4 + (row)])

static inline VrMat4 vrm_m4_identity(void)
{
    VrMat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}
static inline VrMat4 vrm_m4_mul(VrMat4 a, VrMat4 b)
{
    VrMat4 r;
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double s = 0.0;
            for (k = 0; k < 4; k++) s += (double)VRM_AT(a, i, k) * (double)VRM_AT(b, k, j);
            VRM_AT(r, i, j) = (float)s;
        }
    return r;
}
static inline void vrm_m4_apply(VrMat4 m, const float in[4], float out[4])
{
    int i;
    for (i = 0; i < 4; i++)
        out[i] = VRM_AT(m, i, 0) * in[0] + VRM_AT(m, i, 1) * in[1] + VRM_AT(m, i, 2) * in[2] + VRM_AT(m, i, 3) * in[3];
}
static inline VrMat4 vrm_m4_from_pose(VrPose p)
{
    VrMat4 m = vrm_m4_identity();
    const VrVec3 x = vrm_q_rotate(p.orientation, vrm_v3(1, 0, 0));
    const VrVec3 y = vrm_q_rotate(p.orientation, vrm_v3(0, 1, 0));
    const VrVec3 z = vrm_q_rotate(p.orientation, vrm_v3(0, 0, 1));
    VRM_AT(m, 0, 0) = x.x; VRM_AT(m, 1, 0) = x.y; VRM_AT(m, 2, 0) = x.z;
    VRM_AT(m, 0, 1) = y.x; VRM_AT(m, 1, 1) = y.y; VRM_AT(m, 2, 1) = y.z;
    VRM_AT(m, 0, 2) = z.x; VRM_AT(m, 1, 2) = z.y; VRM_AT(m, 2, 2) = z.z;
    VRM_AT(m, 0, 3) = p.position.x; VRM_AT(m, 1, 3) = p.position.y; VRM_AT(m, 2, 3) = p.position.z;
    return m;
}
/* General inverse (cofactors, double precision). Returns 0 if singular. */
static inline int vrm_m4_inverse(VrMat4 a, VrMat4 *out)
{
    double m[16], inv[16], det;
    int i;
    for (i = 0; i < 16; i++) m[i] = a.m[i];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-20) || !isfinite(det)) return 0;
    for (i = 0; i < 16; i++) out->m[i] = (float)(inv[i] / det);
    return 1;
}
/* Rigid inverse of a pose matrix: view = inverse(pose). */
static inline VrMat4 vrm_m4_view_from_pose(VrPose p) { return vrm_m4_from_pose(vrm_pose_inv(p)); }

/* Vulkan-convention projection for OpenXR stage-space rendering (right-handed
 * view, -z forward): clip y points down, depth 0 (near) .. 1 (far). Same
 * as hello_xr's XrMatrix4x4f_CreateProjectionFov(GRAPHICS_VULKAN). */
static inline VrMat4 vrm_proj_vulkan(VrFov fov, float near_z, float far_z)
{
    VrMat4 m;
    const float tl = tanf(fov.left), tr = tanf(fov.right), tu = tanf(fov.up), td = tanf(fov.down);
    const float w = tr - tl, h = td - tu;     /* h < 0: clip +y points down */
    memset(&m, 0, sizeof(m));
    VRM_AT(m, 0, 0) = 2.0f / w;
    VRM_AT(m, 0, 2) = (tr + tl) / w;
    VRM_AT(m, 1, 1) = 2.0f / h;
    VRM_AT(m, 1, 2) = (tu + td) / h;
    VRM_AT(m, 2, 2) = -far_z / (far_z - near_z);
    VRM_AT(m, 2, 3) = -(far_z * near_z) / (far_z - near_z);
    VRM_AT(m, 3, 2) = -1.0f;
    return m;
}

/* D3D-convention, left-handed projection (+z forward, clip y up, w = z),
 * with an asymmetric FOV and an explicit depth row z_clip = a*z + b*w.
 * This is the convention BLACK's own world projection uses (see
 * vr_game_math.h), so a per-eye projection built here keeps the game's depth
 * semantics. */
static inline VrMat4 vrm_proj_d3d_lh(VrFov fov, float a, float b)
{
    VrMat4 m;
    const float tl = tanf(fov.left), tr = tanf(fov.right), tu = tanf(fov.up), td = tanf(fov.down);
    memset(&m, 0, sizeof(m));
    VRM_AT(m, 0, 0) = 2.0f / (tr - tl);
    VRM_AT(m, 0, 2) = -(tr + tl) / (tr - tl);
    VRM_AT(m, 1, 1) = 2.0f / (tu - td);
    VRM_AT(m, 1, 2) = -(tu + td) / (tu - td);
    VRM_AT(m, 2, 2) = a;
    VRM_AT(m, 2, 3) = b;
    VRM_AT(m, 3, 2) = 1.0f;
    return m;
}

/* The flip between an OpenXR right-handed frame and the matching
 * left-handed (D3D) frame: (x, y, z) -> (x, y, -z). Its own inverse. */
static inline VrMat4 vrm_m4_flip_z(void)
{
    VrMat4 m = vrm_m4_identity();
    VRM_AT(m, 2, 2) = -1.0f;
    return m;
}

static inline double vrm_wrap_deg(double d)
{
    d = fmod(d + 180.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}
static inline double vrm_wrap_rad(double r)
{
    r = fmod(r + VRM_PI, 2.0 * VRM_PI);
    if (r < 0.0) r += 2.0 * VRM_PI;
    return r - VRM_PI;
}
static inline float vrm_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

#endif /* VR_MATH_H */
