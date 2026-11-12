/*
 * kvk_vr_hooks.h - what the Kelvin Vulkan backend asks of a VR host glue (port/quest/src/vr_renderer_kvk.c), beyond
 * vr_bridge.h. Every hook has a weak default in kvk_vr_layers.inc that keeps the backend's own behaviour, so the
 * backend runs unchanged without a glue (flat use, tests/kelvin_vk).
 *
 * Threads: all hooks run on the Kelvin GPU thread (the thread that calls kgpu_*).
 *
 * Frame identity. The title's camera for a frame reaches the GPU thread in push-buffer order: the game hook marks the
 * push-buffer word where the world pass begins with kelvin_mark_view(va, KVK_VIEW_VR_CAMERA, slot, ...); when the
 * pusher reaches that word, kgpu_set_view() hands (mode, slot) to kvk_vr_view_mark(). The glue resolves `slot` to the
 * VrGameCamera/VrGameCameraExt it saved when it made the mark, so the eye transforms use the camera of the frame being
 * drawn, not the newest one the title has published since.
 *
 * Per-eye correction (one, owned by the backend). kvk_vr_stereo_hook() fills the KvkStereoFrame uploaded to the
 * vertex stage. Phase FIRST_DRAW: before the frame's first draw (preliminary). Phase FLIP: at the flip, after the eye
 * images were acquired (vr_acquire_eye_targets late-latched the poses): the backend rewrites this frame's stereo
 * blocks with it before the command buffer is submitted, so the frame is drawn exactly for the poses the shell will
 * submit with it (vr_render_pose()).
 */
#ifndef KVK_VR_HOOKS_H
#define KVK_VR_HOOKS_H

#include <stdbool.h>
#include <stdint.h>
#include "kvk_stereo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kelvin_mark_view mode carrying a camera identity (the view slot is the glue's camera slot). Unlike
 * KG_VIEW_VR_WORLD it does not change the draw classification (the HUD is still found automatically). */
#define KVK_VIEW_VR_CAMERA 18

enum { KVK_VR_PHASE_FIRST_DRAW = 0, KVK_VR_PHASE_FLIP = 1 };

/* True when `out` was filled (eye_clip, quad_mvp, misc, valid as kvk_stereo_frame fills them). False: the backend
 * computes it from vr_frame_state() / vr_game_camera() as before. */
bool kvk_vr_stereo_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase);

/* VR_PRESENT_* of the frame being drawn (STEREO: eye images; FLAT_*: the flat quad-layer image), -1 if unknown. */
int kvk_vr_frame_mode(void);

/* A kelvin_mark_view mark in the VR range (KG_VIEW_VR_WORLD/SCREEN, KVK_VIEW_VR_CAMERA) was reached. */
void kvk_vr_view_mark(int mode, int slot);

/* The flip's command buffer was submitted and the targets released: the next frame begins. */
void kvk_vr_frame_done(void);

/* ---- gun latch (port/notes/gun-latch.md) ----------------------------------------------------------------------------
 * The title draws its first-person weapon itself, at the hand pose of its own (30/60 Hz) frame. The backend moves
 * the weapon's draws to the hand pose latched with the eyes, the way the eye correction moves the world to the head:
 *   classification  kvk_vr_viewmodel_draw(): this world draw's position is made through (pos_consts: the program's
 *                   absolute constant registers feeding oPos, kvk_vsh_pos_consts) a weapon MVP the game hook recorded
 *                   for this frame, and the draw's constants hold it bit for bit. Unsure -> false (a world draw).
 *   correction      such draws read a second stereo block. At the first draw it equals the frame's; at the flip
 *                   kvk_vr_viewmodel_hook(FLIP) gives the eye transforms with the hand delta
 *                   (hand now * inverse(hand the game used), stage space) applied before each eye's; false: the block
 *                   keeps the frame's own transforms (no delta, or none known).
 *   debug           kvk_vr_viewmodel_tint() != 0: classified draws get a translucent tint on top (0xAABBGGRR). */
bool kvk_vr_viewmodel_draw(const float *constants, const uint32_t pos_consts[6]);
bool kvk_vr_viewmodel_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase);
uint32_t kvk_vr_viewmodel_tint(void);
/* First-person arms: a weapon draw's 192 vertex constants (a copy, editable) and its vertex data (attribute 0, element
 * 0). Returns true when it changed them (the copy is then uploaded instead). */
bool kvk_vr_viewmodel_bones(float *constants, const void *vertex_data);

/* ---- both-eye capture (port/notes/renderer.md section 12) ------------------------------------------------------
 * kvk_vr_capture_wanted(): asked once per STEREO frame at its composition; nonzero: this frame's two eye pictures
 * (exactly what goes into the eye images: one more composition pass into an image of the backend's, read back with
 * the frame's submit waited for -- the capture frame costs a stall, no other frame anything) are handed to
 * kvk_vr_capture_done() right after that submit, as tightly packed RGBA8 rows, top row first, gamma-space bytes as the
 * swapchain holds them. The callee owns rgba[0] (left) and rgba[1] (right) and frees them. */
int kvk_vr_capture_wanted(void);
void kvk_vr_capture_done(uint8_t *rgba[2], uint32_t w, uint32_t h, uint64_t frame);

#ifdef __cplusplus
}
#endif

#endif
