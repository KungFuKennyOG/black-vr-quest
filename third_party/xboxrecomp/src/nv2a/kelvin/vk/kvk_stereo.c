/*
 * kvk_stereo.c - per-eye transforms for the Kelvin Vulkan backend. See kvk_stereo.h.
 */
#include "kvk_stereo.h"

#include <math.h>
#include <string.h>

#define M(m, row, col) ((m)[(col) * 4 + (row)])

void kvk_mat4_identity(float out[16])
{
    memset(out, 0, 16 * sizeof(float));
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

void kvk_mat4_mul(float out[16], const float a[16], const float b[16])
{
    double r[16];
    int i, j, k;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            double s = 0.0;
            for (k = 0; k < 4; k++) s += (double)M(a, i, k) * (double)M(b, k, j);
            r[j * 4 + i] = s;
        }
    for (i = 0; i < 16; i++) out[i] = (float)r[i];
}

bool kvk_mat4_invert(float out[16], const float m[16])
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) { a[i][j] = M(m, i, j); a[i][j + 4] = i == j ? 1.0 : 0.0; }
    for (k = 0; k < 4; k++) {
        int piv = k;
        double best = fabs(a[k][k]), f;
        for (i = k + 1; i < 4; i++) if (fabs(a[i][k]) > best) { best = fabs(a[i][k]); piv = i; }
        if (best < 1e-300) return false;
        if (piv != k) for (j = 0; j < 8; j++) { double t = a[k][j]; a[k][j] = a[piv][j]; a[piv][j] = t; }
        f = 1.0 / a[k][k];
        for (j = 0; j < 8; j++) a[k][j] *= f;
        for (i = 0; i < 4; i++) {
            if (i == k || a[i][k] == 0.0) continue;
            f = a[i][k];
            for (j = 0; j < 8; j++) a[i][j] -= f * a[k][j];
        }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) M(out, i, j) = (float)a[i][j + 4];
    return true;
}

void kvk_mat4_apply(float out[4], const float m[16], const float v[4])
{
    double r[4];
    int i;
    for (i = 0; i < 4; i++)
        r[i] = (double)M(m, i, 0) * v[0] + (double)M(m, i, 1) * v[1] + (double)M(m, i, 2) * v[2] + (double)M(m, i, 3) * v[3];
    for (i = 0; i < 4; i++) out[i] = (float)r[i];
}

static void quat_matrix(float out[16], const VrQuat *q)
{
    double x = q->x, y = q->y, z = q->z, w = q->w, n = x * x + y * y + z * z + w * w;
    double s = n > 0.0 ? 2.0 / n : 0.0;
    kvk_mat4_identity(out);
    M(out, 0, 0) = (float)(1.0 - s * (y * y + z * z));
    M(out, 0, 1) = (float)(s * (x * y - z * w));
    M(out, 0, 2) = (float)(s * (x * z + y * w));
    M(out, 1, 0) = (float)(s * (x * y + z * w));
    M(out, 1, 1) = (float)(1.0 - s * (x * x + z * z));
    M(out, 1, 2) = (float)(s * (y * z - x * w));
    M(out, 2, 0) = (float)(s * (x * z - y * w));
    M(out, 2, 1) = (float)(s * (y * z + x * w));
    M(out, 2, 2) = (float)(1.0 - s * (x * x + y * y));
}

void kvk_pose_matrix(float out[16], const VrPose *p, float translation_scale)
{
    quat_matrix(out, &p->orientation);
    M(out, 0, 3) = p->position.x * translation_scale;
    M(out, 1, 3) = p->position.y * translation_scale;
    M(out, 2, 3) = p->position.z * translation_scale;
}

void kvk_pose_inverse_matrix(float out[16], const VrPose *p, float translation_scale)
{
    float r[16];
    double t[3] = { p->position.x * (double)translation_scale, p->position.y * (double)translation_scale,
                    p->position.z * (double)translation_scale };
    int i, j;
    quat_matrix(r, &p->orientation);
    kvk_mat4_identity(out);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) M(out, i, j) = M(r, j, i);
    for (i = 0; i < 3; i++)
        M(out, i, 3) = (float)-(M(out, i, 0) * t[0] + M(out, i, 1) * t[1] + M(out, i, 2) * t[2]);
}

void kvk_proj_from_tangents(float out[16], float l, float r, float u, float d, float zn, float zf, float fwd)
{
    double a = zf / ((double)zf - zn), b = -(double)zn * zf / ((double)zf - zn);
    memset(out, 0, 16 * sizeof(float));
    M(out, 0, 0) = (float)(2.0 / ((double)r - l));
    M(out, 0, 2) = (float)(-((double)r + l) / ((double)r - l) * fwd);
    M(out, 1, 1) = (float)(2.0 / ((double)u - d));
    M(out, 1, 2) = (float)(-((double)u + d) / ((double)u - d) * fwd);
    M(out, 2, 2) = (float)(a * fwd);
    M(out, 2, 3) = (float)b;
    M(out, 3, 2) = fwd;
}

static bool is_perspective(const float p[16])
{
    return M(p, 3, 0) == 0.0f && M(p, 3, 1) == 0.0f && M(p, 3, 3) == 0.0f && fabsf(M(p, 3, 2)) > 1e-12f &&
           M(p, 0, 0) != 0.0f && M(p, 1, 1) != 0.0f && isfinite(M(p, 0, 0)) && isfinite(M(p, 1, 1));
}

bool kvk_game_projection(float out[16], const VrGameCamera *cam, float default_near, float default_far)
{
    if (!cam || !cam->valid) return false;
    if (is_perspective(cam->proj)) {
        memcpy(out, cam->proj, 16 * sizeof(float));
        return true;
    }
    if (cam->half_fov_x > 0.0f && cam->half_fov_y > 0.0f && isfinite(cam->half_fov_x) && isfinite(cam->half_fov_y) &&
        default_far > default_near && default_near > 0.0f) {
        kvk_proj_from_tangents(out, -cam->half_fov_x, cam->half_fov_x, cam->half_fov_y, -cam->half_fov_y,
                               default_near, default_far, 1.0f);
        return true;
    }
    return false;
}

bool kvk_eye_clip_correction(float out[16], const float pe[16], const float ve[16], const float vg[16],
                             const float pg[16])
{
    float ivg[16], ipg[16], t[16];
    if (!kvk_mat4_invert(ivg, vg) || !kvk_mat4_invert(ipg, pg)) return false;
    kvk_mat4_mul(t, ivg, ipg);
    kvk_mat4_mul(t, ve, t);
    kvk_mat4_mul(out, pe, t);
    return true;
}

static float sgn(float v) { return v < 0.0f ? -1.0f : 1.0f; }

void kvk_eye_from_head(float out[16], const VrPose *head, const VrPose *eye, float world_scale, const float pg[16])
{
    float head_m[16], eye_inv[16], e[16];
    /* XR head coordinates (x right, y up, -z forward) -> the game's view axes, read off P_game */
    const float s[3] = { sgn(M(pg, 0, 0)), sgn(M(pg, 1, 1)), -sgn(M(pg, 3, 2)) };
    int i, j;
    kvk_pose_matrix(head_m, head, world_scale);
    kvk_pose_inverse_matrix(eye_inv, eye, world_scale);
    kvk_mat4_mul(e, eye_inv, head_m);                  /* head space -> eye space, XR axes */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) M(out, i, j) = s[i] * M(e, i, j) * s[j];
        M(out, i, 3) = s[i] * M(e, i, 3);
        M(out, 3, i) = 0.0f;
    }
    M(out, 3, 3) = 1.0f;
}

void kvk_eye_projection(float out[16], const VrFov *fov, const float pg[16])
{
    const double l = tan(fov->left), r = tan(fov->right), u = tan(fov->up), d = tan(fov->down);
    const double ws = fabs(M(pg, 3, 2)), fw = sgn(M(pg, 3, 2));
    const double sx = sgn(M(pg, 0, 0)), sy = sgn(M(pg, 1, 1));
    int c;
    memset(out, 0, 16 * sizeof(float));
    /* x_clip = |w| (2 tan_x - (r + l)) / (r - l) * depth, tan_x = sx * x / depth, depth = fw * z */
    M(out, 0, 0) = (float)(ws * 2.0 * sx / (r - l));
    M(out, 0, 2) = (float)(-ws * (r + l) * fw / (r - l));
    M(out, 1, 1) = (float)(ws * 2.0 * sy / (u - d));
    M(out, 1, 2) = (float)(-ws * (u + d) * fw / (u - d));
    for (c = 0; c < 4; c++) { M(out, 2, c) = M(pg, 2, c); M(out, 3, c) = M(pg, 3, c); }
}

bool kvk_eye_clip(float out[16], int form, const float pe[16], const float e[16], const float pg[16])
{
    float t[16];
    if (form == KVK_STEREO_FULL) {
        float ipg[16];
        if (!kvk_mat4_invert(ipg, pg)) return false;
        kvk_mat4_mul(t, e, ipg);
        kvk_mat4_mul(out, pe, t);
        return true;
    } else {
        /* (x_c, y_c, w_c, 1) -> (x, y, z, 1) through P_game's rows x, y and w (w = m32 z) */
        const double a0 = M(pg, 0, 0), a1 = M(pg, 0, 1), a2 = M(pg, 0, 2), a3 = M(pg, 0, 3);
        const double b0 = M(pg, 1, 0), b1 = M(pg, 1, 1), b2 = M(pg, 1, 2), b3 = M(pg, 1, 3);
        const double m32 = M(pg, 3, 2), det = a0 * b1 - a1 * b0;
        double d00, d01, d10, d11;
        float r[16];
        if (fabs(det) < 1e-30 || fabs(m32) < 1e-30) return false;
        d00 = b1 / det; d01 = -a1 / det; d10 = -b0 / det; d11 = a0 / det;
        memset(r, 0, sizeof(r));
        M(r, 0, 0) = (float)d00; M(r, 0, 1) = (float)d01;
        M(r, 0, 2) = (float)(-(d00 * a2 + d01 * b2) / m32); M(r, 0, 3) = (float)(-(d00 * a3 + d01 * b3));
        M(r, 1, 0) = (float)d10; M(r, 1, 1) = (float)d11;
        M(r, 1, 2) = (float)(-(d10 * a2 + d11 * b2) / m32); M(r, 1, 3) = (float)(-(d10 * a3 + d11 * b3));
        M(r, 2, 2) = (float)(1.0 / m32);
        M(r, 3, 3) = 1.0f;
        kvk_mat4_mul(t, e, r);
        kvk_mat4_mul(out, pe, t);
        return true;
    }
}

void kvk_stereo_default_config(KvkStereoConfig *cfg)
{
    cfg->form = KVK_STEREO_FROM_W;
    cfg->default_near = 0.1f;
    cfg->default_far = 4000.0f;
    cfg->screen_distance_m = 2.0f;
    cfg->screen_width_m = 2.4f;
}

void kvk_stereo_frame(KvkStereoFrame *out, const KvkStereoConfig *cfg, const VrFrameState *fs,
                      const VrGameCamera *cam)
{
    KvkStereoConfig def;
    float pg[16];
    int e;
    if (!cfg) { kvk_stereo_default_config(&def); cfg = &def; }
    memset(out, 0, sizeof(*out));
    for (e = 0; e < 2; e++) { kvk_mat4_identity(out->eye_clip[e]); kvk_mat4_identity(out->quad_mvp[e]); }
    out->misc[0] = (float)KVK_STEREO_FULL;
    if (!fs) return;
    /* the virtual screen: head-locked, 4:3, in front of the eyes (XR axes, metres) */
    for (e = 0; e < 2; e++) {
        float pe[16], eye_inv[16], head_m[16], eh[16], quad[16], t[16];
        const VrFov *f = &fs->eye[e].fov;
        kvk_proj_from_tangents(pe, tanf(f->left), tanf(f->right), tanf(f->up), tanf(f->down), 0.05f, 100.0f, -1.0f);
        kvk_pose_matrix(head_m, &fs->head, 1.0f);
        kvk_pose_inverse_matrix(eye_inv, &fs->eye[e].pose, 1.0f);
        kvk_mat4_mul(eh, eye_inv, head_m);
        kvk_mat4_identity(quad);
        M(quad, 0, 0) = cfg->screen_width_m;
        M(quad, 1, 1) = cfg->screen_width_m * 0.75f;
        M(quad, 2, 3) = -cfg->screen_distance_m;
        kvk_mat4_mul(t, eh, quad);
        kvk_mat4_mul(out->quad_mvp[e], pe, t);
    }
    if (!kvk_game_projection(pg, cam, cfg->default_near, cfg->default_far)) return;
    for (e = 0; e < 2; e++) {
        float pe[16], ef[16];
        const float ws = fs->world_scale > 0.0f ? fs->world_scale : 1.0f;
        kvk_eye_from_head(ef, &fs->head, &fs->eye[e].pose, ws, pg);
        kvk_eye_projection(pe, &fs->eye[e].fov, pg);
        if (!kvk_eye_clip(out->eye_clip[e], cfg->form, pe, ef, pg)) {
            kvk_mat4_identity(out->eye_clip[0]); kvk_mat4_identity(out->eye_clip[1]);
            return;
        }
    }
    out->misc[0] = (float)cfg->form;
    out->misc[1] = 1.0f;
    /* the depth row, for pre-divided screen-space vertices (KVK_DF_SCREEN_DEPTH): z_ndc = A + B / w */
    if (M(pg, 3, 2) != 0.0f && M(pg, 2, 0) == 0.0f && M(pg, 2, 1) == 0.0f && M(pg, 3, 3) == 0.0f) {
        out->misc[2] = M(pg, 2, 2) / M(pg, 3, 2);
        out->misc[3] = M(pg, 2, 3);
    }
    out->valid = 1;
}
