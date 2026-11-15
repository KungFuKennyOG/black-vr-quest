/*
 * kvk_stereo.h - per-eye transforms for the Kelvin Vulkan backend (pure C math, no Vulkan).
 *
 * The title renders one picture through its own camera: every world vertex leaves the vertex stage as
 *     clip_game = P_game * V_game * M * x          (D3D clip convention: x right, y up, z/w in [0, 1])
 * whatever program made it (rigid, skinned, CPU-shaded, the first-person weapon). The eye wants
 *     clip_eye  = P_eye * V_eye * M * x
 * and since V_eye = E * V_game (E: eye from game camera), clip_eye = C * clip_game with
 *     C = P_eye * V_eye * inverse(V_game) * inverse(P_game) = P_eye * E * inverse(P_game).
 * So the eye transform is one 4x4 per eye applied to the finished clip position in the vertex stage (kvk_glsl.c's
 * kvk_finish, selected by gl_ViewIndex under multiview) -- no vertex program, constant or matrix of the title is
 * found or patched.
 *
 * Two forms of C are provided:
 *   KVK_STEREO_FULL   C acts on clip (x, y, z, w). Exact when P_game is exactly the title's projection, including its
 *                     depth row; a wrong depth row (near/far) mis-scales the eye separation.
 *   KVK_STEREO_FROM_W C acts on (x, y, w, 1). The view-space point of a vertex is recovered from x, y and w alone
 *                     (w is view depth for any perspective projection; the vertex's homogeneous 1 is implied), so
 *                     the parallax is exact whatever the title's depth range; P_game's depth row only sets the depth
 *                     values written (taken from the camera when known). This is what the backend uses.
 * Both agree when P_game is exact (tests/kelvin_vk).
 *
 * Conventions: matrices column-major, m[col * 4 + row], as vr_bridge.h. "Game view space" is whatever P_game maps;
 * its axis signs (x right or left, y up or down, +z or -z forward) are read off P_game, so the OpenXR poses (x right,
 * y up, -z forward, metres) are converted without knowing the title's handedness.
 */
#ifndef KVK_STEREO_H
#define KVK_STEREO_H

#include <stdbool.h>
#include <stdint.h>

#include "vr_bridge.h"     /* port/quest (the XR shell owns it); the build adds its directory */

#ifdef __cplusplus
extern "C" {
#endif

void kvk_mat4_identity(float out[16]);
void kvk_mat4_mul(float out[16], const float a[16], const float b[16]);     /* out = a * b (out may alias) */
bool kvk_mat4_invert(float out[16], const float m[16]);                      /* in double precision */
void kvk_mat4_apply(float out[4], const float m[16], const float v[4]);
/* A pose (rotation, translation) as the matrix taking its local coordinates to its parent's; and its inverse. */
void kvk_pose_matrix(float out[16], const VrPose *p, float translation_scale);
void kvk_pose_inverse_matrix(float out[16], const VrPose *p, float translation_scale);

/* A perspective projection in the D3D convention (x right, y up, z/w 0 at near and 1 at far), from the tangents of
 * the field-of-view half angles (OpenXR's XrFovf: left and down negative), for a view space looking along
 * `forward_sign` z (+1: +z forward, as D3D; -1: -z forward, as OpenXR). */
void kvk_proj_from_tangents(float out[16], float tan_left, float tan_right, float tan_up, float tan_down,
                            float znear, float zfar, float forward_sign);
/* The camera the title's world was drawn with, as a projection. Uses cam->proj when valid (its row 3 must be
 * (0, 0, +-1, 0): a perspective), else builds one from half_fov_x/y (tangents) with +z forward and the given
 * near/far. False when neither is usable. */
bool kvk_game_projection(float out[16], const VrGameCamera *cam, float default_near, float default_far);

/* The general form: C = P_eye * V_eye * inverse(V_game) * inverse(P_game). */
bool kvk_eye_clip_correction(float out[16], const float proj_eye[16], const float view_eye[16],
                             const float view_game[16], const float proj_game[16]);

/* Eye from game camera, in game view space: the eye's offset from the head (OpenXR stage poses, metres), scaled to
 * game units, with the axes of the view space P_game maps. Assumes the title's camera is the head (the game hooks
 * drive it from the HMD pose), so only the eye-relative part is applied here. */
void kvk_eye_from_head(float out[16], const VrPose *head, const VrPose *eye, float world_scale,
                       const float proj_game[16]);
/* P_eye in the game's view space: the eye's field of view (tangents) with P_game's depth and w rows. */
void kvk_eye_projection(float out[16], const VrFov *fov, const float proj_game[16]);

enum { KVK_STEREO_FULL = 0, KVK_STEREO_FROM_W = 1 };

/* C for one eye. FULL: P_eye * E * inverse(P_game). FROM_W: P_eye * E * R where R takes (x, y, w, 1) to the view
 * point (x, y, z, 1) through P_game's x, y and w rows. */
bool kvk_eye_clip(float out[16], int form, const float proj_eye[16], const float eye_from_game[16],
                  const float proj_game[16]);

/* Everything the backend uploads for a frame (the GLSL KvkStereo block). */
typedef struct KvkStereoFrame {
    float eye_clip[2][16];      /* per eye C, form `form` */
    float quad_mvp[2][16];      /* the HUD/menu virtual screen per eye (Vulkan clip before its y flip) */
    float misc[4];              /* x: form; y: 1 when valid; z, w: P_game depth row A = m22/m32, B = m23 (z/w = A + B/w) */
    float laser_mvp[4][16];     /* VR laser sight, drawn in the world: [kind * 2 + eye], kind 0 the beam (unit quad x
                                 * -0.5..0.5 across, y 0..1 along), 1 the dot (x, y -0.5..0.5); Vulkan clip before y flip */
    float laser_misc[4];        /* x: 1 to draw it; y: beam strength; z: dot strength; w: the beam's width at its end
                                 * over its width at the muzzle */
    int valid;
} KvkStereoFrame;

typedef struct KvkStereoConfig {
    int form;                   /* KVK_STEREO_FROM_W by default */
    float default_near, default_far;  /* game units, when the camera has no projection matrix */
    float screen_distance_m;    /* the virtual screen: distance and width (metres), 4:3 */
    float screen_width_m;
} KvkStereoConfig;

void kvk_stereo_default_config(KvkStereoConfig *cfg);
/* From the shell's frame state and the camera the game hooks published. Without a valid game camera the eye
 * transforms are identity (mono picture in both eyes) and valid is 0; the quad is always built. */
void kvk_stereo_frame(KvkStereoFrame *out, const KvkStereoConfig *cfg, const VrFrameState *fs,
                      const VrGameCamera *cam);

#ifdef __cplusplus
}
#endif

#endif
