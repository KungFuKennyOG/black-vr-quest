/*
 * kvk_glsl.h - GLSL (Vulkan, #version 450 + GL_EXT_multiview) for the Kelvin Vulkan backend.
 *
 * The D3D11 backend's HLSL generators (kelvin_gpu_vsh.inc: NV2A transform programs; kelvin_gpu_pss.inc and
 * kelvin_gpu_hlsl.inc: register combiners) are mirrored here statement by statement, so a draw shades the same on
 * either backend. What differs, all of it at the very end of the vertex stage:
 *   - Vulkan clip space has y pointing down: the position's y is negated after everything else.
 *   - VR: the vertex stage multiplies the finished D3D-convention clip position by the eye's clip-space correction
 *     (kvk_stereo.h) for the view being rendered (gl_ViewIndex), when the draw's push constant says it is a world
 *     draw. The same shader serves mono and multiview passes (gl_ViewIndex is 0 without multiview).
 *   - Textures are sampler2DArray: Kelvin render targets are 2-layer arrays under VR (one layer per eye), plain
 *     textures one layer; the push constant picks the layer per stage (the eye's own, or 0).
 *   - PC extras (screen-space reflection targets, DLAA jitter) are not generated; g_jit stays in the layout, zero.
 *
 * Resource interface (descriptor set 0 = per-draw dynamic buffers, set 1 = textures):
 *   set 0 binding 0  UBO  PSConst  (gfx/kgfx_cfg_types.inc)
 *   set 0 binding 1  UBO  VSG (GPU vertex programs) or VSConst (CPU-shaded vertices)
 *   set 0 binding 2  UBO  vec4 C[192]           vertex program constants
 *   set 0 binding 3  UBO  uvec4 adesc[16]; vec4 inl[16]   attribute descriptors
 *   set 0 binding 4  SSBO uint vab[] (raw guest vertex data) or vec4 vb[] (CPU-shaded records)
 *   set 0 binding 5  UBO  KvkStereo { mat4 eye_clip[2]; mat4 quad_mvp[2]; vec4 st_misc; }
 *   set 1 binding 0..3  sampler2DArray Tex0..Tex3
 *   push constant     { vec4 p0; vec4 p1; uvec4 texlayer; uvec4 dflags; }  (64 bytes)
 *
 * Every function returns a malloc'd NUL-terminated string the caller frees, or NULL (out of memory, or a vertex
 * program the GPU cannot run: then the engine shades on the CPU, as with D3D11). Pure C, no Vulkan headers.
 */
#ifndef KVK_GLSL_H
#define KVK_GLSL_H

#include <stdint.h>
#include <stdbool.h>
#include "kvk_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Nv2aVshProgram_;

/* Push-constant dflags.x bits. */
#define KVK_DF_STEREO 1u        /* multiply the clip position by eye_clip[gl_ViewIndex] */
/* with KVK_DF_STEREO: a pre-divided screen-space vertex (CPU-projected effects: BLACK's smoke, sparks, fire, glows;
 * w = 1, the depth only in z). Its w is rebuilt from z through the game projection's depth row (st_misc.zw) first, so
 * eye_clip places it like a world vertex; without a usable z, at half the far distance with its depth value kept
 * (kvk_backend.c screen_effect). */
#define KVK_DF_SCREEN_DEPTH 2u

/* Vertex shader for an NV2A transform program (the GPU vertex path). NULL: unsupported operation. */
char *kvk_glsl_vs_program(const struct Nv2aVshProgram_ *prog);
/* Vertex shader that places CPU-shaded vertex records (kelvin_render.c's KGpuDraw.recs). */
char *kvk_glsl_vs_cpu(void);
/* Fragment shader specialised for one combiner configuration (kelvin_gpu_pss.inc's ps_generate). */
char *kvk_glsl_ps_spec(const PSConst *c);
/* The combiner interpreter (kelvin_gpu_hlsl.inc's ubershader), smooth or flat shaded. */
char *kvk_glsl_ps_uber(bool flat);
/* Same key as kelvin_gpu_pss.inc's ps_key_of: one specialised shader per configuration. */
uint64_t kvk_ps_key(const PSConst *c);

/* Internal passes. */
enum {
    KVK_FIX_FULLSCREEN_VS,      /* full-screen triangle */
    KVK_FIX_CLEAR_COLOR_PS,     /* out = p0 */
    KVK_FIX_CLEAR_DEPTH_PS,     /* depth = p1.x */
    KVK_FIX_D2C_PS,             /* depth as packed Z24S8 colour (BLACK's glare pass reads the depth buffer so) */
    KVK_FIX_C2D_PS,             /* packed Z24S8 colour as depth */
    KVK_FIX_QUAD_VS,            /* VR virtual screen: a quad through quad_mvp[gl_ViewIndex] */
    KVK_FIX_QUAD_PS,            /* ... textured from Tex0 layer p0.x: opaque, or premultiplied scaled by p0.y */
    KVK_FIX_COPY_PS,            /* eye picture from the main target (sampled, so it scales and stays UNORM) */
    KVK_FIX_LASER_VS,           /* VR laser sight: beam (p1.y 0) or dot (1) through laser_mvp[kind * 2 + view] */
    KVK_FIX_LASER_PS,           /* ... its glow, premultiplied, strength p0.x */
    KVK_FIX_COUNT
};
char *kvk_glsl_fixed(int which);

/* How a transform program forms its position's w (static data flow over the program):
 * KVK_POS_TRANSFORMED - oPos.w comes out of a dot product of input data with constants (a matrix transform: world
 *                       geometry, skinned or not, through the camera's projection);
 * KVK_POS_PASSTHROUGH - it does not (pre-transformed screen positions: 2D overlays and full-screen passes).
 * VR applies the eye correction only to TRANSFORMED draws on the main target. */
enum { KVK_POS_PASSTHROUGH = 0, KVK_POS_TRANSFORMED = 1 };
int kvk_vsh_pos_class(const struct Nv2aVshProgram_ *prog);

/* VR gun latch (port/notes/gun-latch.md): the constant registers (hardware index 0..191, absolute addressing) some
 * component of oPos depends on (static data flow), as a 192-bit set: bit r of out[r / 32]. A register reached only through relative
 * addressing (a0) is not in it. The backend moves a draw with the weapon only if the weapon's MVP registers are in
 * this set of its program, i.e. its position is really made through that matrix. */
void kvk_vsh_pos_consts(const struct Nv2aVshProgram_ *prog, uint32_t out[6]);

#ifdef __cplusplus
}
#endif

#endif
