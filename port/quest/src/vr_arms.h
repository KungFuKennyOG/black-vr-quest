/* vr_arms.h: first-person arms for VR (port/notes/arms.md).
 *
 * BLACK's first-person weapon is one skinned model per gun (levels/<level>/fpguns/bg1_<gun>.db): the gun mesh and a
 * shared arms mesh, skinned on the GPU by vertex program 463DBDF0 from a bone palette in the vertex constants. Bone k
 * of the model's bone-name table is the 3-row (4x3) matrix at c[60 + 3k] (rows: x, y, z; w = translation), and its
 * inverse-bind matrix is the model's matrix k (row-vector form: rotation rows 0..2, translation row 3).
 *
 * In VR the gun sits in the player's real hand, so the game's forearms point at where the flat-screen camera expected
 * the arms to be. Collapsing a bone's palette matrix to a single point folds every vertex it drives into that point:
 *   - the forearm and upper arm (Shoulder_*, Elbow_*, Joint_wrist_twist) fold into the wrist joint: the hand stays,
 *     cut at the wrist;
 *   - a hidden off hand: every bone of that arm folds into its wrist, so its triangles have no area and nothing draws.
 * Bones are matched by name, so every gun model works whatever its bone order (they differ: the SMG's right shoulder
 * is bone 20, most guns' 21, the magnums' arms start at bone 10).
 *
 * Pure functions; no I/O. Unit tests: tests/xr_shell/test_arms.c. */
#ifndef VR_ARMS_H
#define VR_ARMS_H

#include <math.h>
#include <string.h>

#define VRA_MAX_BONES 64
#define VRA_PALETTE_BASE 60     /* c60: the palette's first row (bone 0) */

enum { VRA_OTHER = 0, VRA_UPPER = 1, VRA_HAND = 2 };   /* role[] */
enum { VRA_LEFT = 0, VRA_RIGHT = 1 };                   /* side[] / wrist[] */

typedef struct {
    int   n;                         /* bones */
    unsigned char role[VRA_MAX_BONES];
    signed char side[VRA_MAX_BONES]; /* -1: neither arm */
    int   wrist[2];                  /* bone index of Wrist_lft / Wrist_rt, -1 if missing */
    float joint[VRA_MAX_BONES][3];   /* the bone's joint in model (bind) space */
    float bind[2][12];               /* the wrists' bind frames (3x4 affine, rows; column vectors) */
    float mirror[12];                /* bind_rt^-1 * S * bind_lft, S = diag(-1, 1, 1): the bind pose's left/right
                                      * mirror in the right wrist's frame (the stock arms mirror across model x = 0) */
    int   mirror_ok;
    /* loose parts the game's left hand carries in reload animations (shell, magazine, clip, bullets, rocket...):
     * they follow the moved left hand while they are off their place on the gun */
    unsigned char loose[VRA_MAX_BONES];
    int have_loose_rest;                    /* the loose parts' rest places on the gun (vra_loose_rest), else the bind's */
    float loose_rest[VRA_MAX_BONES][3];
    int   gun_root;                  /* the gun's *_Root bone, -1 if none */
    float bindk[VRA_MAX_BONES][12];  /* every bone's bind frame (3x4) */
    signed char pair[VRA_MAX_BONES]; /* a left-arm bone's right twin (same name, _rt), -1 if none */
} VrArmRig;

/* a gun part that can come off the gun in the game's hands */
static inline int vra_is_loose(const char *s)
{
    static const char *const k[] = { "Shell", "Magazine", "Clip", "Bullet", "Rocket", "Case", "Lead", "loader", "Cap" };
    unsigned i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) if (strstr(s, k[i])) return 1;
    return 0;
}

/* ---- 3x4 affine helpers (rows; column vectors: p' = M p) ---- */
static inline void vra_m34_mul(const float a[12], const float b[12], float o[12])
{
    float t[12];
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 4; c++)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + (c == 3 ? a[r * 4 + 3] : 0.0f);
    }
    memcpy(o, t, sizeof(t));
}

static inline int vra_m34_inv(const float m[12], float o[12])
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    float t[12];
    int r;
    if (!(det > 1e-12f || det < -1e-12f)) return 0;
    t[0] = A / det; t[1] = -(b * i - c * h) / det; t[2] = (b * f - c * e) / det;
    t[4] = B / det; t[5] = (a * i - c * g) / det;  t[6] = -(a * f - c * d) / det;
    t[8] = C / det; t[9] = -(a * h - b * g) / det; t[10] = (a * e - b * d) / det;
    for (r = 0; r < 3; r++) t[r * 4 + 3] = -(t[r * 4] * m[3] + t[r * 4 + 1] * m[7] + t[r * 4 + 2] * m[11]);
    memcpy(o, t, sizeof(t));
    return 1;
}

/* the bind frame (bone to model) of an inverse-bind matrix in row-vector form (v' = v R + t) */
static inline int vra_bind_from_inverse_bind(const float m[16], float o[12])
{
    const float inv_col[12] = { m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14] };
    return vra_m34_inv(inv_col, o);
}

static inline int vra_ends_with(const char *s, const char *suffix)
{
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static inline int vra_starts_with(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

/* The joint of an inverse-bind matrix m (row-vector form v' = v R + t): the point it takes to the origin,
 * j = -t R^-1. Returns 0 if R is singular. */
static inline int vra_joint_from_inverse_bind(const float m[16], float j[3])
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    float inv[9], t0 = -m[12], t1 = -m[13], t2 = -m[14];
    if (!(det > 1e-8f || det < -1e-8f)) return 0;
    inv[0] = A / det; inv[1] = -(b * i - c * h) / det; inv[2] = (b * f - c * e) / det;
    inv[3] = B / det; inv[4] = (a * i - c * g) / det;  inv[5] = -(a * f - c * d) / det;
    inv[6] = C / det; inv[7] = -(a * h - b * g) / det; inv[8] = (a * e - b * d) / det;
    /* row vector times matrix */
    j[0] = t0 * inv[0] + t1 * inv[3] + t2 * inv[6];
    j[1] = t0 * inv[1] + t1 * inv[4] + t2 * inv[7];
    j[2] = t0 * inv[2] + t1 * inv[5] + t2 * inv[8];
    return 1;
}

/* Builds the rig from the model's bone names and inverse-bind matrices. Returns 1 when at least one wrist is found
 * (only then can anything be cut). A bone without a side (Joint_wrist_twist) belongs to the arm of the nearest
 * preceding sided bone, as in the stock models (it sits between Elbow_lft and Wrist_lft). */
static inline int vra_build_rig(VrArmRig *rig, const char *const *names, const float (*inverse_bind)[16], int n)
{
    int k, last_side = -1;
    memset(rig, 0, sizeof(*rig));
    rig->wrist[0] = rig->wrist[1] = -1;
    rig->gun_root = -1;
    if (n <= 0 || n > VRA_MAX_BONES) return 0;
    rig->n = n;
    for (k = 0; k < n; k++) {
        const char *s = names[k] ? names[k] : "";
        int side = vra_ends_with(s, "_lft") ? VRA_LEFT : vra_ends_with(s, "_rt") ? VRA_RIGHT : -1;
        int role = VRA_OTHER;
        if (vra_starts_with(s, "Shoulder_") || vra_starts_with(s, "Elbow_")) role = VRA_UPPER;
        else if (vra_starts_with(s, "Joint_wrist_twist")) { role = VRA_UPPER; side = last_side; }
        else if (vra_starts_with(s, "Wrist_") || vra_starts_with(s, "Finger") || vra_starts_with(s, "Thumb_")) role = VRA_HAND;
        if (side < 0) role = VRA_OTHER;
        rig->role[k] = (unsigned char)role;
        rig->side[k] = (signed char)(role == VRA_OTHER ? -1 : side);
        if (role != VRA_OTHER) last_side = side;
        if (role == VRA_HAND && vra_starts_with(s, "Wrist_")) rig->wrist[side] = k;
        if (!inverse_bind || !vra_joint_from_inverse_bind(inverse_bind[k], rig->joint[k]))
            rig->joint[k][0] = rig->joint[k][1] = rig->joint[k][2] = 0.0f;
        if (role == VRA_OTHER) {
            const size_t len = strlen(s);
            if (len > 5 && !strcmp(s + len - 5, "_Root") && rig->gun_root < 0) rig->gun_root = k;
            else if (vra_is_loose(s)) rig->loose[k] = 1;
        }
        if (!inverse_bind || !vra_bind_from_inverse_bind(inverse_bind[k], rig->bindk[k])) {
            memset(rig->bindk[k], 0, sizeof(rig->bindk[k]));
            rig->loose[k] = 0;
            if (rig->gun_root == k) rig->gun_root = -1;
        }
    }
    for (k = 0; k < n; k++) {
        const char *s = names[k] ? names[k] : "";
        const size_t len = strlen(s);
        int j;
        rig->pair[k] = -1;
        if (rig->side[k] != VRA_LEFT || len < 5 || len > 40) continue;
        for (j = 0; j < n; j++) {
            const char *t = names[j] ? names[j] : "";
            if (strlen(t) == len - 1 && !strncmp(s, t, len - 4) && !strcmp(t + len - 4, "_rt")) { rig->pair[k] = (signed char)j; break; }
        }
    }
    if (inverse_bind && rig->wrist[0] >= 0 && rig->wrist[1] >= 0 &&
        vra_bind_from_inverse_bind(inverse_bind[rig->wrist[0]], rig->bind[0]) &&
        vra_bind_from_inverse_bind(inverse_bind[rig->wrist[1]], rig->bind[1])) {
        float r_inv[12], sl[12];
        int c;
        memcpy(sl, rig->bind[0], sizeof(sl));
        for (c = 0; c < 4; c++) sl[c] = -sl[c];                 /* S * bind_lft */
        rig->mirror_ok = vra_m34_inv(rig->bind[1], r_inv);
        if (rig->mirror_ok) vra_m34_mul(r_inv, sl, rig->mirror);
    }
    return rig->wrist[0] >= 0 || rig->wrist[1] >= 0;
}

/* Palette rows of bone k: constants[(VRA_PALETTE_BASE + 3k + r) * 4 .. +3], r = 0..2 */
static inline float *vra_rows(float *constants, int k) { return constants + (VRA_PALETTE_BASE + 3 * k) * 4; }

/* Edits the palette in place. cut: fold both forearms into the wrists. show_left / show_right: that hand stays (else
 * the whole arm folds away). Returns the number of bones changed. */
static inline int vra_apply(const VrArmRig *rig, float *constants, int cut, int show_left, int show_right)
{
    float p[2][3];
    int have[2] = { 0, 0 }, s, k, changed = 0;
    if (rig->n <= 0 || VRA_PALETTE_BASE + 3 * rig->n > 192) return 0;
    for (s = 0; s < 2; s++) {
        const int w = rig->wrist[s];
        int r;
        if (w < 0) continue;
        for (r = 0; r < 3; r++) {
            const float *row = vra_rows(constants, w) + r * 4;
            p[s][r] = row[0] * rig->joint[w][0] + row[1] * rig->joint[w][1] + row[2] * rig->joint[w][2] + row[3];
        }
        have[s] = 1;
    }
    for (k = 0; k < rig->n; k++) {
        const int side = rig->side[k];
        const int show = side == VRA_LEFT ? show_left : show_right;
        float *rows;
        int r;
        if (side < 0 || !have[side]) continue;
        if (rig->role[k] == VRA_UPPER ? !(cut || !show) : show) continue;   /* hand kept when shown; upper kept uncut */
        rows = vra_rows(constants, k);
        for (r = 0; r < 3; r++) { rows[r * 4 + 0] = rows[r * 4 + 1] = rows[r * 4 + 2] = 0.0f; rows[r * 4 + 3] = p[side][r]; }
        changed++;
    }
    return changed;
}

/* Places the game's left (off) hand at the off controller: a[16] (row-major 4x4, column vectors; the game thread's
 * vgm_off_hand_mirror) takes the gun hand's pose to the off hand's, mirrored, in model space. The left wrist goes to
 * a * (posed right wrist) * mirror, and every left-arm bone moves rigidly with it, so the game's own finger pose is
 * kept. Returns 1 when it moved the hand. */
/* The loose parts the game's left hand holds (off their place on the gun by 3+ cm, nearer the left wrist than the
 * right: the shotgun's spare shell, parked there even at rest and through the raise and cock) are hidden: their rows
 * zeroed, their triangles collapse to a point. Returns how many. */
static inline int vra_hide_left_loose(const VrArmRig *rig, float *constants)
{
    const int wl = rig->wrist[VRA_LEFT], wr = rig->wrist[VRA_RIGHT];
    float fr[12], fl[12], root_now[12], root_inv[12], root_bind_inv[12];
    int k, j, n = 0;
    if (wl < 0 || wr < 0 || rig->gun_root < 0 || VRA_PALETTE_BASE + 3 * rig->n > 192) return 0;
    vra_m34_mul(vra_rows(constants, wr), rig->bind[VRA_RIGHT], fr);
    vra_m34_mul(vra_rows(constants, wl), rig->bind[VRA_LEFT], fl);
    vra_m34_mul(vra_rows(constants, rig->gun_root), rig->bindk[rig->gun_root], root_now);
    if (!vra_m34_inv(root_now, root_inv) || !vra_m34_inv(rig->bindk[rig->gun_root], root_bind_inv)) return 0;
    for (k = 0; k < rig->n; k++) {
        float now[12], rel_now[12], rel_bind[12], dx, dy, dz, dl, dr, *rows;
        if (!rig->loose[k]) continue;
        rows = vra_rows(constants, k);
        vra_m34_mul(rows, rig->bindk[k], now);
        vra_m34_mul(root_inv, now, rel_now);
        vra_m34_mul(root_bind_inv, rig->bindk[k], rel_bind);
        dx = rel_now[3] - rel_bind[3]; dy = rel_now[7] - rel_bind[7]; dz = rel_now[11] - rel_bind[11];
        dl = (now[3] - fl[3]) * (now[3] - fl[3]) + (now[7] - fl[7]) * (now[7] - fl[7]) + (now[11] - fl[11]) * (now[11] - fl[11]);
        dr = (now[3] - fr[3]) * (now[3] - fr[3]) + (now[7] - fr[7]) * (now[7] - fr[7]) + (now[11] - fr[11]) * (now[11] - fr[11]);
        if (!(sqrtf(dx * dx + dy * dy + dz * dz) > 0.03f) || !(dl < dr)) continue;
        for (j = 0; j < 12; j++) rows[j] = 0.0f;
        n++;
    }
    return n;
}

static inline int vra_place_off_hand(const VrArmRig *rig, float *constants, const float a[16])
{
    const int wl = rig->wrist[VRA_LEFT], wr = rig->wrist[VRA_RIGHT];
    const float a34[12] = { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11] };
    float fr[12], fl[12], target[12], fl_inv[12], g[12];
    int k;
    if (!rig->mirror_ok || wl < 0 || wr < 0 || VRA_PALETTE_BASE + 3 * rig->n > 192) return 0;
    for (k = 0; k < 12; k++) if (!(a[k] == a[k])) return 0;
    vra_m34_mul(vra_rows(constants, wr), rig->bind[VRA_RIGHT], fr);     /* posed right wrist frame */
    vra_m34_mul(vra_rows(constants, wl), rig->bind[VRA_LEFT], fl);      /* posed left wrist frame */
    vra_m34_mul(a34, fr, target);
    vra_m34_mul(target, rig->mirror, target);
    if (!vra_m34_inv(fl, fl_inv)) return 0;
    vra_m34_mul(target, fl_inv, g);
    /* loose parts first (they are judged against the game's own left hand, before it moves): one off its place on the
     * gun by more than 3 cm and nearer the game's left wrist than its right is in the game's left hand (the shotgun
     * keeps a shell there even at rest). The free hand at the controller does no reloading (the game's animations
     * show the game's own hand), so such a part is hidden (its rows zeroed: its triangles collapse to a point), not
     * carried under your hand */
    if (rig->gun_root >= 0) {
        float root_now[12], root_inv[12], root_bind_inv[12];
        vra_m34_mul(vra_rows(constants, rig->gun_root), rig->bindk[rig->gun_root], root_now);
        if (vra_m34_inv(root_now, root_inv) && vra_m34_inv(rig->bindk[rig->gun_root], root_bind_inv)) {
            for (k = 0; k < rig->n; k++) {
                float now[12], rel_now[12], rel_bind[12], moved[12], dx, dy, dz, off, dl, dr, w;
                float *rows;
                int j;
                if (!rig->loose[k]) continue;
                rows = vra_rows(constants, k);
                vra_m34_mul(rows, rig->bindk[k], now);
                vra_m34_mul(root_inv, now, rel_now);
                vra_m34_mul(root_bind_inv, rig->bindk[k], rel_bind);
                dx = rel_now[3] - rel_bind[3]; dy = rel_now[7] - rel_bind[7]; dz = rel_now[11] - rel_bind[11];
                off = sqrtf(dx * dx + dy * dy + dz * dz);
                dl = (now[3] - fl[3]) * (now[3] - fl[3]) + (now[7] - fl[7]) * (now[7] - fl[7]) + (now[11] - fl[11]) * (now[11] - fl[11]);
                dr = (now[3] - fr[3]) * (now[3] - fr[3]) + (now[7] - fr[7]) * (now[7] - fr[7]) + (now[11] - fr[11]) * (now[11] - fr[11]);
                if (!(off > 0.03f) || !(dl < dr)) continue;
                (void)w; (void)moved;
                for (j = 0; j < 12; j++) rows[j] = 0.0f;
            }
        }
    }
    for (k = 0; k < rig->n; k++) {
        float *rows;
        if (rig->side[k] != VRA_LEFT) continue;
        rows = vra_rows(constants, k);
        vra_m34_mul(g, rows, rows);
    }
    return 1;
}

/* How far the furthest loose part (magazine, shell, clip, rocket...) is off its place on the gun (metres in model
 * space; 0 without a gun root or loose parts). Over a few cm: the game is reloading (or loading) with its left hand. */
/* Each loose part's position on the gun (relative to the gun root) now: its place at rest. Some guns keep a part away
 * from its bind place at rest (the AK: 0.22 m), which is no animation. */
static inline void vra_loose_rest(VrArmRig *rig, const float *constants)
{
    float root_now[12], root_inv[12];
    int k;
    if (rig->gun_root < 0 || VRA_PALETTE_BASE + 3 * rig->n > 192) return;
    vra_m34_mul(vra_rows((float *)constants, rig->gun_root), rig->bindk[rig->gun_root], root_now);
    if (!vra_m34_inv(root_now, root_inv)) return;
    for (k = 0; k < rig->n; k++) {
        float now[12], rel[12];
        if (!rig->loose[k]) continue;
        vra_m34_mul(vra_rows((float *)constants, k), rig->bindk[k], now);
        vra_m34_mul(root_inv, now, rel);
        rig->loose_rest[k][0] = rel[3]; rig->loose_rest[k][1] = rel[7]; rig->loose_rest[k][2] = rel[11];
    }
    rig->have_loose_rest = 1;
}

static inline float vra_loose_off(const VrArmRig *rig, const float *constants)
{
    float root_now[12], root_inv[12], root_bind_inv[12], best = 0.0f;
    int k;
    if (rig->gun_root < 0 || VRA_PALETTE_BASE + 3 * rig->n > 192) return 0.0f;
    vra_m34_mul(vra_rows((float *)constants, rig->gun_root), rig->bindk[rig->gun_root], root_now);
    if (!vra_m34_inv(root_now, root_inv) || !vra_m34_inv(rig->bindk[rig->gun_root], root_bind_inv)) return 0.0f;
    for (k = 0; k < rig->n; k++) {
        float now[12], rel_now[12], rel_bind[12], dx, dy, dz, off;
        if (!rig->loose[k]) continue;
        vra_m34_mul(vra_rows((float *)constants, k), rig->bindk[k], now);
        vra_m34_mul(root_inv, now, rel_now);
        vra_m34_mul(root_bind_inv, rig->bindk[k], rel_bind);
        if (rig->have_loose_rest) {
            dx = rel_now[3] - rig->loose_rest[k][0]; dy = rel_now[7] - rig->loose_rest[k][1];
            dz = rel_now[11] - rig->loose_rest[k][2];
        } else {
            dx = rel_now[3] - rel_bind[3]; dy = rel_now[7] - rel_bind[7]; dz = rel_now[11] - rel_bind[11];
        }
        off = sqrtf(dx * dx + dy * dy + dz * dz);
        if (off == off && off > best) best = off;
    }
    return best;
}

/* Gripping two-handed: the left hand becomes the exact mirror of the game's right hand (its fingers closed around the
 * grip), carried by a (as vra_place_off_hand) onto the gun. The stock arms' bind pose is mirror-symmetric across model
 * x = 0 (S), so a left vertex S v skins to a * P_rt * v: P_lft = a * P_rt * S. A left bone without a twin (the twist)
 * takes the left wrist's. Returns 1 when it did. */
static inline int vra_mirror_off_hand(const VrArmRig *rig, float *constants, const float a[16])
{
    const float a34[12] = { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11] };
    float out[VRA_MAX_BONES][12];
    int k, have = 0;
    if (rig->wrist[VRA_LEFT] < 0 || rig->wrist[VRA_RIGHT] < 0 || VRA_PALETTE_BASE + 3 * rig->n > 192) return 0;
    for (k = 0; k < 12; k++) if (!(a[k] == a[k])) return 0;
    for (k = 0; k < rig->n; k++) {
        const int twin = rig->side[k] != VRA_LEFT ? -1 : rig->pair[k] >= 0 ? rig->pair[k] : rig->wrist[VRA_RIGHT];
        float ps[12];
        int r;
        if (twin < 0) continue;
        memcpy(ps, vra_rows(constants, twin), sizeof(ps));
        for (r = 0; r < 3; r++) ps[r * 4] = -ps[r * 4];                    /* P_rt * S */
        vra_m34_mul(a34, ps, out[k]);
        have = 1;
    }
    for (k = 0; k < rig->n; k++)
        if (rig->side[k] == VRA_LEFT) memcpy(vra_rows(constants, k), out[k], sizeof(out[k]));
    return have;
}

#endif /* VR_ARMS_H */
