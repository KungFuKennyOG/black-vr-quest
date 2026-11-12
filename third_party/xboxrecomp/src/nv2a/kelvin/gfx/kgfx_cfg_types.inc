/* Kelvin GPU backends: the combiner/vertex constant layouts both backends share (kelvin_gfx.h).
 *
 * Included by the D3D11 backend (kelvin_gpu.c, where this block used to live, unchanged) and by the Vulkan backend
 * (vk/), whose GLSL declares the same std140 layouts (every member is a 16-byte vector, so HLSL cbuffer packing,
 * std140 and this C layout agree). Needs <stdint.h>. */
/* ---- constant buffer layouts (mirror the HLSL) ------------------------------- */

typedef struct PSStage {
    uint32_t rgb_in[4], alpha_in[4], outs[4];
    float c0[4], c1[4];
} PSStage;

typedef struct PSConst {
    PSStage stage[8];
    uint32_t fin_abcd[4];
    uint32_t fin_efg[4];
    float final_c0[4], final_c1[4];
    float fogcolor[4];
    uint32_t gen[4];
    uint32_t texmode[4];
    uint32_t texflags[4];
    float texrcp[4][4];
    uint32_t ckey[4];
    uint32_t ckeymask[4];
    uint32_t ofs[4];
} PSConst;

typedef struct VSG {
    float scale[4];
    float fog[4];
    uint32_t flags[4];
    float vm[4];                 /* x: viewmodel scale about the viewport centre (1 for none) */
    float jit[4];                /* xy: this frame's DLAA offset, clip units per unit w (kelvin_gpu_dlss.inc) */
} VSG;
