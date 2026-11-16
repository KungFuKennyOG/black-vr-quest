/* xr-shell: first-person arms (port/quest/src/vr_arms.h). A synthetic two-arm skeleton with the stock bone names (in
 * the SMG's order, which differs from most guns'), real-looking inverse-bind matrices and a posed palette. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_arms.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NEAR(a, b, e) (fabs((double)(a) - (double)(b)) <= (e))

static const char *const k_names[] = {
    "World", "Extra", "Root", "Shoulder_lft", "Elbow_lft", "Joint_wrist_twist", "Wrist_lft", "Finger1_Top_lft",
    "Thumb_Tip_lft", "Shoulder_rt", "Elbow_rt", "Wrist_rt", "Finger1_Top_rt", "SMG_Root", "SMG_Slide", "Thumb_Top_rt",
};
#define NB ((int)(sizeof(k_names) / sizeof(k_names[0])))

/* the bind-space joints (metres) */
static const float k_joint[NB][3] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {-0.34f, -0.05f, 0.12f}, {-0.10f, -0.03f, 0.12f}, {0.05f, -0.02f, 0.11f},
    {0.16f, -0.01f, 0.11f}, {0.24f, 0.02f, 0.12f}, {0.19f, -0.12f, 0.11f}, {0.34f, 0.07f, -0.13f},
    {0.10f, 0.02f, -0.12f}, {-0.12f, -0.01f, -0.16f}, {-0.24f, -0.02f, -0.12f}, {0, 0, 0}, {0, -0.05f, -0.03f},
    {-0.09f, 0.15f, -0.11f},
};

/* inverse bind in row-vector form: rotation R (rows 0..2) about z by `a`, translation -j R */
static void inverse_bind(float m[16], const float j[3], float a)
{
    const float c = cosf(a), s = sinf(a);
    const float R[3][3] = { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } };
    int r, k;
    memset(m, 0, 16 * sizeof(float));
    for (r = 0; r < 3; r++) for (k = 0; k < 3; k++) m[r * 4 + k] = R[r][k];
    for (k = 0; k < 3; k++) m[12 + k] = -(j[0] * R[0][k] + j[1] * R[1][k] + j[2] * R[2][k]);
    m[15] = 1.0f;
}

/* palette rows of bone k: an arbitrary pose (a per-bone rotation about x plus a translation) */
static void pose(float *constants, int k)
{
    const float a = 0.1f + 0.07f * (float)k, c = cosf(a), s = sinf(a);
    float *rows = constants + (VRA_PALETTE_BASE + 3 * k) * 4;
    const float R[3][4] = { { 1, 0, 0, 0.01f * k }, { 0, c, -s, -0.2f + 0.003f * k }, { 0, s, c, 0.3f - 0.002f * k } };
    memcpy(rows, R, sizeof(R));
}

static void apply_rows(const float *constants, int k, const float v[3], float out[3])
{
    const float *rows = constants + (VRA_PALETTE_BASE + 3 * k) * 4;
    int r;
    for (r = 0; r < 3; r++) out[r] = rows[r * 4] * v[0] + rows[r * 4 + 1] * v[1] + rows[r * 4 + 2] * v[2] + rows[r * 4 + 3];
}

static int idx(const char *n)
{
    int k;
    for (k = 0; k < NB; k++) if (!strcmp(k_names[k], n)) return k;
    return -1;
}

int main(void)
{
    float ib[NB][16], base[192 * 4], c[192 * 4];
    VrArmRig rig;
    int k;
    for (k = 0; k < NB; k++) inverse_bind(ib[k], k_joint[k], 0.3f * (float)k);
    memset(base, 0, sizeof(base));
    for (k = 0; k < 60; k++) base[k * 4] = (float)k;          /* other constants: must never change */
    for (k = 0; k < NB; k++) pose(base, k);

    /* the rig: names to roles and sides, the twist bone joins the left arm, joints from the inverse binds */
    CHECK(vra_build_rig(&rig, k_names, (const float (*)[16])ib, NB) == 1);
    CHECK(rig.wrist[VRA_LEFT] == idx("Wrist_lft") && rig.wrist[VRA_RIGHT] == idx("Wrist_rt"));
    CHECK(rig.role[idx("Shoulder_lft")] == VRA_UPPER && rig.side[idx("Shoulder_lft")] == VRA_LEFT);
    CHECK(rig.role[idx("Joint_wrist_twist")] == VRA_UPPER && rig.side[idx("Joint_wrist_twist")] == VRA_LEFT);
    CHECK(rig.role[idx("Thumb_Top_rt")] == VRA_HAND && rig.side[idx("Thumb_Top_rt")] == VRA_RIGHT);
    CHECK(rig.role[idx("SMG_Slide")] == VRA_OTHER && rig.side[idx("SMG_Slide")] == -1);
    CHECK(rig.role[idx("Root")] == VRA_OTHER);
    for (k = 0; k < NB; k++)
        CHECK(NEAR(rig.joint[k][0], k_joint[k][0], 1e-5) && NEAR(rig.joint[k][1], k_joint[k][1], 1e-5) &&
              NEAR(rig.joint[k][2], k_joint[k][2], 1e-5));

    /* nothing asked: nothing changes */
    memcpy(c, base, sizeof(c));
    CHECK(vra_apply(&rig, c, 0, 1, 1) == 0 && !memcmp(c, base, sizeof(c)));

    /* cut, both hands shown: each arm's shoulder, elbow (and the twist) fold into that arm's posed wrist joint; hands,
     * gun bones and every other constant unchanged */
    memcpy(c, base, sizeof(c));
    CHECK(vra_apply(&rig, c, 1, 1, 1) == 5);
    {
        float wl[3], wr[3], p[3];
        const float far_away[3] = { 5.0f, -3.0f, 2.0f };
        apply_rows(base, idx("Wrist_lft"), k_joint[idx("Wrist_lft")], wl);
        apply_rows(base, idx("Wrist_rt"), k_joint[idx("Wrist_rt")], wr);
        apply_rows(c, idx("Elbow_rt"), far_away, p);
        CHECK(NEAR(p[0], wr[0], 1e-5) && NEAR(p[1], wr[1], 1e-5) && NEAR(p[2], wr[2], 1e-5));
        apply_rows(c, idx("Shoulder_lft"), far_away, p);
        CHECK(NEAR(p[0], wl[0], 1e-5) && NEAR(p[1], wl[1], 1e-5) && NEAR(p[2], wl[2], 1e-5));
        apply_rows(c, idx("Joint_wrist_twist"), k_joint[3], p);
        CHECK(NEAR(p[0], wl[0], 1e-5) && NEAR(p[1], wl[1], 1e-5) && NEAR(p[2], wl[2], 1e-5));
        /* the wrist itself still maps its joint to the same point (the cut is seamless) */
        apply_rows(c, idx("Wrist_rt"), k_joint[idx("Wrist_rt")], p);
        CHECK(NEAR(p[0], wr[0], 1e-6) && NEAR(p[1], wr[1], 1e-6) && NEAR(p[2], wr[2], 1e-6));
    }
    for (k = 0; k < NB; k++)
        if (rig.role[k] != VRA_UPPER)
            CHECK(!memcmp(c + (VRA_PALETTE_BASE + 3 * k) * 4, base + (VRA_PALETTE_BASE + 3 * k) * 4, 12 * sizeof(float)));
    CHECK(!memcmp(c, base, VRA_PALETTE_BASE * 4 * sizeof(float)));
    CHECK(!memcmp(c + (VRA_PALETTE_BASE + 3 * NB) * 4, base + (VRA_PALETTE_BASE + 3 * NB) * 4,
                  (192 - VRA_PALETTE_BASE - 3 * NB) * 4 * sizeof(float)));

    /* off hand hidden (one-handed): the whole left arm, hand included, folds to one point: no area, nothing drawn */
    memcpy(c, base, sizeof(c));
    CHECK(vra_apply(&rig, c, 1, 0, 1) == 8);
    {
        float a[3], b[3];
        const float v1[3] = { 0.3f, 0.1f, -0.2f }, v2[3] = { -1.0f, 2.0f, 0.5f };
        apply_rows(c, idx("Finger1_Top_lft"), v1, a);
        apply_rows(c, idx("Thumb_Tip_lft"), v2, b);
        CHECK(NEAR(a[0], b[0], 1e-6) && NEAR(a[1], b[1], 1e-6) && NEAR(a[2], b[2], 1e-6));
    }
    CHECK(!memcmp(c + (VRA_PALETTE_BASE + 3 * idx("Finger1_Top_rt")) * 4,
                  base + (VRA_PALETTE_BASE + 3 * idx("Finger1_Top_rt")) * 4, 12 * sizeof(float)));

    /* arms = full but the off hand hidden: the left arm goes, the right arm stays whole */
    memcpy(c, base, sizeof(c));
    CHECK(vra_apply(&rig, c, 0, 0, 1) == 6);
    CHECK(!memcmp(c + (VRA_PALETTE_BASE + 3 * idx("Elbow_rt")) * 4, base + (VRA_PALETTE_BASE + 3 * idx("Elbow_rt")) * 4,
                  12 * sizeof(float)));

    /* the off hand at the controller (vra_place_off_hand). A mirror-symmetric rig: the left bones are the right ones
     * mirrored across model x = 0, the bones' own axes mirrored too (as the stock arms). */
    {
        float ibm[NB][16], cm[192 * 4], a[16];
        VrArmRig rm;
        int j;
        for (k = 0; k < NB; k++) memcpy(ibm[k], ib[k], sizeof(ibm[k]));
        {
            /* left wrist bind = S * right wrist bind * Q (a bone axis convention of its own: x flipped and a turn) */
            const int wl = idx("Wrist_lft"), wr = idx("Wrist_rt");
            float br[12], bl[12], q[12] = { -0.0f, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0 }, inv[12];
            q[0] = 0.0f; q[1] = -1.0f; q[4] = -1.0f; q[5] = 0.0f;           /* det -1 with S: a proper left frame */
            CHECK(vra_bind_from_inverse_bind(ib[wr], br));
            for (j = 0; j < 12; j++) bl[j] = br[j];
            for (j = 0; j < 4; j++) bl[j] = -bl[j];
            vra_m34_mul(bl, q, bl);
            CHECK(vra_m34_inv(bl, inv));
            /* back to an inverse bind in row-vector form */
            memset(ibm[wl], 0, sizeof(ibm[wl]));
            for (j = 0; j < 3; j++) { ibm[wl][j * 4 + 0] = inv[0 * 4 + j]; ibm[wl][j * 4 + 1] = inv[1 * 4 + j]; ibm[wl][j * 4 + 2] = inv[2 * 4 + j]; }
            ibm[wl][12] = inv[3]; ibm[wl][13] = inv[7]; ibm[wl][14] = inv[11]; ibm[wl][15] = 1.0f;
        }
        CHECK(vra_build_rig(&rm, k_names, (const float (*)[16])ibm, NB) == 1 && rm.mirror_ok);
        {   /* the rebuilt left bind is what was set */
            float bl2[12], br2[12];
            CHECK(vra_bind_from_inverse_bind(ibm[idx("Wrist_lft")], bl2) && vra_bind_from_inverse_bind(ibm[idx("Wrist_rt")], br2));
            CHECK(NEAR(bl2[3], -br2[3], 1e-5) && NEAR(bl2[7], br2[7], 1e-5) && NEAR(bl2[11], br2[11], 1e-5));
        }
        /* a = S (the off controller is the gun hand's mirror image across model x = 0), the right arm in its bind
         * pose: the left wrist lands exactly on its own bind frame, whatever the left arm was posed at */
        memcpy(cm, base, sizeof(cm));
        for (j = 0; j < 12; j++) vra_rows(cm, idx("Wrist_rt"))[j] = (j % 5 == 0) ? 1.0f : 0.0f;   /* identity palette */
        memset(a, 0, sizeof(a));
        a[0] = -1.0f; a[5] = 1.0f; a[10] = 1.0f; a[15] = 1.0f;
        CHECK(vra_place_off_hand(&rm, cm, a) == 1);
        {
            float fl[12];
            vra_m34_mul(vra_rows(cm, idx("Wrist_lft")), rm.bind[VRA_LEFT], fl);
            for (j = 0; j < 12; j++) CHECK(NEAR(fl[j], rm.bind[VRA_LEFT][j], 1e-4));
        }
        /* the left fingers keep their pose relative to the left wrist (moved rigidly with it) */
        {
            float before_rel[12], after_rel[12], inv_b[12], inv_a[12];
            CHECK(vra_m34_inv(vra_rows(base, idx("Wrist_lft")), inv_b) && vra_m34_inv(vra_rows(cm, idx("Wrist_lft")), inv_a));
            vra_m34_mul(inv_b, vra_rows(base, idx("Finger1_Top_lft")), before_rel);
            vra_m34_mul(inv_a, vra_rows(cm, idx("Finger1_Top_lft")), after_rel);
            for (j = 0; j < 12; j++) CHECK(NEAR(before_rel[j], after_rel[j], 1e-4));
        }
        /* the right arm and the gun are untouched */
        CHECK(!memcmp(vra_rows(cm, idx("Elbow_rt")), vra_rows(base, idx("Elbow_rt")), 12 * sizeof(float)));
        CHECK(!memcmp(vra_rows(cm, idx("SMG_Slide")), vra_rows(base, idx("SMG_Slide")), 12 * sizeof(float)));
        /* a translated off controller moves the left wrist by the same amount */
        {
            float c2[192 * 4], f1[12], f2[12];
            memcpy(c2, base, sizeof(c2));
            for (j = 0; j < 12; j++) vra_rows(c2, idx("Wrist_rt"))[j] = (j % 5 == 0) ? 1.0f : 0.0f;
            a[3] = 0.25f; a[7] = -0.1f;
            CHECK(vra_place_off_hand(&rm, c2, a) == 1);
            vra_m34_mul(vra_rows(cm, idx("Wrist_lft")), rm.bind[VRA_LEFT], f1);
            vra_m34_mul(vra_rows(c2, idx("Wrist_lft")), rm.bind[VRA_LEFT], f2);
            CHECK(NEAR(f2[3] - f1[3], 0.25, 1e-5) && NEAR(f2[7] - f1[7], -0.1, 1e-5) && NEAR(f2[11], f1[11], 1e-5));
        }
        /* gripping: the left hand is the mirror of the right one carried by a (P_lft = a * P_rt * S), so a left vertex
         * S v lands where a takes the right vertex v; twins are found by name; a bone without one takes the wrist's */
        {
            float c3[192 * 4], am[16] = { -1, 0, 0, 0.1f, 0, 1, 0, -0.05f, 0, 0, 1, 0.3f, 0, 0, 0, 1 };
            const float v[3] = { 0.12f, -0.04f, 0.2f }, sv[3] = { -0.12f, -0.04f, 0.2f };
            float l[3], rr[3], ar[3];
            CHECK(rm.pair[idx("Finger1_Top_lft")] == idx("Finger1_Top_rt") && rm.pair[idx("Wrist_lft")] == idx("Wrist_rt"));
            CHECK(rm.pair[idx("Thumb_Tip_lft")] == -1 && rm.pair[idx("Joint_wrist_twist")] == -1 && rm.pair[idx("Wrist_rt")] == -1);
            memcpy(c3, base, sizeof(c3));
            CHECK(vra_mirror_off_hand(&rm, c3, am) == 1);
            apply_rows(c3, idx("Finger1_Top_lft"), sv, l);
            apply_rows(base, idx("Finger1_Top_rt"), v, rr);
            ar[0] = -rr[0] + 0.1f; ar[1] = rr[1] - 0.05f; ar[2] = rr[2] + 0.3f;
            CHECK(NEAR(l[0], ar[0], 1e-5) && NEAR(l[1], ar[1], 1e-5) && NEAR(l[2], ar[2], 1e-5));
            apply_rows(c3, idx("Thumb_Tip_lft"), sv, l);
            apply_rows(base, idx("Wrist_rt"), v, rr);
            CHECK(NEAR(l[0], -rr[0] + 0.1f, 1e-5) && NEAR(l[1], rr[1] - 0.05f, 1e-5) && NEAR(l[2], rr[2] + 0.3f, 1e-5));
            /* the right arm and the gun are untouched */
            CHECK(!memcmp(vra_rows(c3, idx("Finger1_Top_rt")), vra_rows(base, idx("Finger1_Top_rt")), 12 * sizeof(float)));
            CHECK(!memcmp(vra_rows(c3, idx("SMG_Slide")), vra_rows(base, idx("SMG_Slide")), 12 * sizeof(float)));
        }
        /* a loose part (the gun's shell) in the game's left hand (off the gun, near that hand) is hidden while the hand is free */
        {
            static const char *const ln[] = { "World", "Shotgun_Root", "Shotgun_Shell", "Wrist_lft", "Wrist_rt" };
            float lib[5][16], c4[192 * 4], at[16] = { 1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            const float lj[5][3] = { {0,0,0}, {0,0,0}, {0.05f,-0.02f,0.30f}, {-0.15f,-0.1f,0.25f}, {0.15f,-0.1f,0.05f} };
            VrArmRig rl;
            int b2;
            for (b2 = 0; b2 < 5; b2++) inverse_bind(lib[b2], lj[b2], 0.0f);
            CHECK(vra_build_rig(&rl, ln, (const float (*)[16])lib, 5) == 1 && rl.gun_root == 1 && rl.loose[2] && !rl.loose[1]);
            memset(c4, 0, sizeof(c4));
            for (b2 = 0; b2 < 5; b2++) { float *q = vra_rows(c4, b2); q[0] = q[5] = q[10] = 1.0f; }
            /* the shell on the gun (bind place): it stays */
            CHECK(vra_place_off_hand(&rl, c4, at) == 1);
            CHECK(NEAR(vra_rows(c4, 2)[3], 0.0, 1e-6));
            /* the shell off the gun in the game's left hand (12 cm off its place): hidden (rows zeroed) */
            memset(c4, 0, sizeof(c4));
            for (b2 = 0; b2 < 5; b2++) { float *q = vra_rows(c4, b2); q[0] = q[5] = q[10] = 1.0f; }
            vra_rows(c4, 2)[3] = -0.12f;
            CHECK(vra_place_off_hand(&rl, c4, at) == 1);
            CHECK(vra_rows(c4, 2)[0] == 0.0f && vra_rows(c4, 2)[3] == 0.0f && vra_rows(c4, 2)[5] == 0.0f &&
                  vra_rows(c4, 2)[10] == 0.0f);
        }
        /* a NaN mirror or a rig without one does nothing */
        a[0] = NAN;
        memcpy(c, base, sizeof(c));
        CHECK(vra_place_off_hand(&rm, c, a) == 0 && !memcmp(c, base, sizeof(c)));
        CHECK(vra_place_off_hand(&rig, c, a) == 0);
    }

    /* a model with no arm bones (or too many bones for the palette) is left alone */
    {
        static const char *const gun_only[] = { "World", "Root", "Pistol_Root" };
        VrArmRig r2;
        CHECK(vra_build_rig(&r2, gun_only, NULL, 3) == 0);
        CHECK(vra_build_rig(&r2, k_names, NULL, VRA_MAX_BONES + 1) == 0);
    }
    /* a singular inverse bind gives a zero joint, not a crash */
    {
        float m[16] = { 0 };
        float j[3] = { 9, 9, 9 };
        CHECK(vra_joint_from_inverse_bind(m, j) == 0);
    }

    printf("xr-shell-arms: %u checks passed\n", checks);
    return 0;
}
