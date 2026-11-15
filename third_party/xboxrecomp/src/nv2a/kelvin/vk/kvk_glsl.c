/*
 * kvk_glsl.c - GLSL generators of the Kelvin Vulkan backend. See kvk_glsl.h.
 *
 * Mirrors, in order: kelvin_gpu_vsh.inc (kgpu_vsh_head, vsh_operand, vsh_op, vsh_writes, vsh_generate),
 * kelvin_gpu_pss.inc (ps_map .. ps_generate, without the SSR variant), kelvin_gpu_hlsl.inc (the CPU-vertex vertex
 * shader and the combiner ubershader) and kelvin_gpu.c's kgpu_hlsl_clear. Keep them in step: a change to the
 * arithmetic there belongs here too (tests/kelvin_vk checks both against the same CPU references).
 */
#include "kvk_glsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nv2a_vsh_cpu/nv2a_vsh_disassembler.h"

/* ---- string building ------------------------------------------------------------------------------------ */

typedef struct KvkStr { char *p; size_t n, cap; bool bad; } KvkStr;

static void ks_add(KvkStr *b, const char *fmt, ...)
{
    va_list ap;
    int need;
    if (b->bad) return;
    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { b->bad = true; return; }
    if (b->n + (size_t)need + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *g;
        while (cap < b->n + (size_t)need + 1) cap *= 2;
        g = (char *)realloc(b->p, cap);
        if (!g) { b->bad = true; return; }
        b->p = g; b->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    b->n += (size_t)need;
}

static char *ks_take(KvkStr *b)
{
    if (b->bad || !b->p) { free(b->p); return NULL; }
    return b->p;
}

/* ---- declarations shared by the stages --------------------------------------------------------------------- */

static const char kvk_head[] =
"#version 450\n"
"#extension GL_EXT_multiview : require\n";

static const char kvk_push[] =
"layout(push_constant) uniform KvkPush { vec4 p0; vec4 p1; uvec4 texlayer; uvec4 dflags; } pc;\n";

static const char kvk_stereo[] =
"layout(std140, set = 0, binding = 5) uniform KvkStereo { mat4 eye_clip[2]; mat4 quad_mvp[2]; vec4 st_misc;\n"
"    mat4 laser_mvp[4]; vec4 laser_misc; };\n"
/* The D3D-convention clip position (y up, z in [0, 1]) as Vulkan wants it: the eye's correction for world draws,
 * then y down. */
"vec4 kvk_finish(vec4 p) {\n"
"    bool eye = (pc.dflags.x & 1u) != 0u, keepz = false;\n"
"    float zn = 0.0;\n"
/* KVK_DF_SCREEN_DEPTH: a pre-divided screen-space vertex (w carries no depth). Its view depth from z through the game
 * projection's depth row, w = B / (z/w - A) (st_misc.z = A = m22/m32, st_misc.w = B = m23), so the clip position is the
 * game camera's. Without a usable z (z <= 0: no depth was given; or beyond the far plane): at half the far distance
 * B / (1 - A), in its direction, with its own depth value kept (keepz). No depth row: the game's position. */
"    if (eye && (pc.dflags.x & 2u) != 0u) {\n"
"        float w;\n"
"        zn = p.z / p.w;\n"
"        w = st_misc.w / (zn - st_misc.z);\n"
"        if (!(zn > 0.0 && w > 0.0 && !isinf(w) && !isnan(w))) { w = 0.5 * st_misc.w / (1.0 - st_misc.z); keepz = true; }\n"
"        if (w > 0.0 && !isinf(w) && !isnan(w)) p = vec4(p.xy * (w / p.w), zn * w, w); else eye = false;\n"
"    }\n"
"    if (eye)\n"
/* st_misc.x: 0 the full form acts on (x, y, z, w); 1 the from-w form on (x, y, w, 1) (kvk_stereo.h) */
"        p = eye_clip[gl_ViewIndex] * (st_misc.x != 0.0 ? vec4(p.x, p.y, p.w, 1.0) : p);\n"
"    if (eye && keepz) p.z = zn * p.w;\n"
"    p.y = -p.y;\n"
"    return p;\n"
"}\n";

static const char kvk_ps_const[] =
"struct StageCfg {\n"
"    uvec4 rgb_in;\n"
"    uvec4 alpha_in;\n"
"    uvec4 outs;\n"
"    vec4  c0;\n"
"    vec4  c1;\n"
"};\n"
"layout(std140, set = 0, binding = 0) uniform PSConst {\n"
"    StageCfg stage[8];\n"
"    uvec4 fin_abcd;\n"
"    uvec4 fin_efg;\n"
"    vec4  final_c0;\n"
"    vec4  final_c1;\n"
"    vec4  fogcolor;\n"
"    uvec4 gen;\n"
"    uvec4 texmode;\n"
"    uvec4 texflags;\n"
"    vec4  texrcp[4];\n"
"    uvec4 ckey;\n"
"    uvec4 ckeymask;\n"
"    uvec4 ofs;\n"
"};\n";

/* The varyings, in kelvin_gpu_hlsl.inc's VSOut order (TEXCOORD0..6 -> locations 0..6). */
static const char kvk_vs_out[] =
"layout(location = 0) out vec4 v_d0;\n"
"layout(location = 1) out vec4 v_d1;\n"
"layout(location = 2) out vec4 v_misc;\n"
"layout(location = 3) out vec4 v_t0;\n"
"layout(location = 4) out vec4 v_t1;\n"
"layout(location = 5) out vec4 v_t2;\n"
"layout(location = 6) out vec4 v_t3;\n";

/* Flat shading (the KG_FLAT variants): the colours come from the provoking (first) vertex, as with D3D11. */
static const char kvk_fs_in[] =
"#ifdef KG_FLAT\n"
"#define KG_NI flat\n"
"#else\n"
"#define KG_NI\n"
"#endif\n"
"layout(location = 0) KG_NI in vec4 v_d0;\n"
"layout(location = 1) KG_NI in vec4 v_d1;\n"
"layout(location = 2) in vec4 v_misc;\n"
"layout(location = 3) in vec4 v_t0;\n"
"layout(location = 4) in vec4 v_t1;\n"
"layout(location = 5) in vec4 v_t2;\n"
"layout(location = 6) in vec4 v_t3;\n"
"layout(location = 0) out vec4 o_color;\n";

static const char kvk_textures[] =
"layout(set = 1, binding = 0) uniform sampler2DArray Tex0;\n"
"layout(set = 1, binding = 1) uniform sampler2DArray Tex1;\n"
"layout(set = 1, binding = 2) uniform sampler2DArray Tex2;\n"
"layout(set = 1, binding = 3) uniform sampler2DArray Tex3;\n"
/* texlayer bit k: stage k's texture is a per-eye render target (sample this view's layer) */
"float kvk_layer(uint k) { return ((pc.texlayer.x >> k) & 1u) != 0u ? float(gl_ViewIndex) : 0.0; }\n";

/* ---- vertex programs (kelvin_gpu_vsh.inc) ------------------------------------------------------------------- */

static const char kvk_vsh_head[] =
"layout(std140, set = 0, binding = 1) uniform VSG {\n"
"    vec4  g_scale;\n"       /* x 2/guestW, y 2/guestH, z 1/zmax, w 2^64 */
"    vec4  g_fog;\n"
"    uvec4 g_flags;\n"       /* x fog enable, y fog mode, z specular enable, w ignore specular alpha */
"    vec4  g_vm;\n"
"    vec4  g_jit;\n"
"};\n"
"layout(std140, set = 0, binding = 2) uniform VCB { vec4 C[192]; };\n"
"layout(std140, set = 0, binding = 3) uniform ACB { uvec4 adesc[16]; vec4 inl[16]; };\n"
"layout(std430, set = 0, binding = 4) readonly buffer VAB { uint vab[]; };\n"
"\n"
/* Out-of-range reads give zero, as D3D11's typed buffer does (the draw's index may exceed its arrays). */
"uint LoadW(uint i) { return i < uint(vab.length()) ? vab[i] : 0u; }\n"
"uint LoadU32(uint addr) {\n"
"    uint a0 = addr & ~3u;\n"
"    uint sh = (addr & 3u) * 8u;\n"
"    uint lo = LoadW(a0 >> 2);\n"
"    if (sh == 0u) return lo;\n"
"    uint hi = LoadW((a0 >> 2) + 1u);\n"
"    return (lo >> sh) | (hi << (32u - sh));\n"
"}\n"
"uint LoadU8(uint addr) { return (LoadW(addr >> 2) >> ((addr & 3u) * 8u)) & 0xFFu; }\n"
"int LoadS16(uint addr) { return int(LoadU32(addr) << 16) >> 16; }\n"
"vec4 FetchAttr(uint a, uint e) {\n"
"    uvec4 d = adesc[a];\n"
"    vec4 r = vec4(0.0, 0.0, 0.0, 1.0);\n"
"    if (d.z == 0u) return inl[a];\n"
"    uint addr = d.x + e * d.y;\n"
"    if (d.z == 1u) {\n"
"        uint w = LoadU32(addr);\n"
"        r = vec4(float((w >> 16) & 255u), float((w >> 8) & 255u), float(w & 255u), float(w >> 24)) / 255.0;\n"
"    } else if (d.z == 2u) {\n"
"        for (uint i = 0u; i < 4u; i++) if (i < d.w) r[i] = float(LoadU8(addr + i)) / 255.0;\n"
"    } else if (d.z == 3u) {\n"
"        for (uint i = 0u; i < 4u; i++) if (i < d.w) r[i] = max(-1.0, float(LoadS16(addr + i * 2u)) / 32767.0);\n"
"    } else if (d.z == 4u) {\n"
"        for (uint i = 0u; i < 4u; i++) if (i < d.w) r[i] = uintBitsToFloat(LoadU32(addr + i * 4u));\n"
"    } else if (d.z == 5u) {\n"
"        for (uint i = 0u; i < 4u; i++) if (i < d.w) r[i] = float(LoadS16(addr + i * 2u));\n"
"    } else if (d.z == 6u) {\n"
"        uint v = LoadU32(addr);\n"
"        int x = int(v << 21) >> 21;\n"
"        int y = int((v >> 11) << 21) >> 21;\n"
"        int z = int(v) >> 22;\n"
"        r = vec4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);\n"
"    }\n"
"    return r;\n"
"}\n"
"\n"
"float vm(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
"vec4 vmul(vec4 a, vec4 b) { return vec4(vm(a.x, b.x), vm(a.y, b.y), vm(a.z, b.z), vm(a.w, b.w)); }\n"
"vec4 vdp3(vec4 a, vec4 b) { return vec4(vm(a.x, b.x) + vm(a.y, b.y) + vm(a.z, b.z)); }\n"
"vec4 vdph(vec4 a, vec4 b) { return vec4(vm(a.x, b.x) + vm(a.y, b.y) + vm(a.z, b.z) + b.w); }\n"
"vec4 vdp4(vec4 a, vec4 b) { return vec4(vm(a.x, b.x) + vm(a.y, b.y) + vm(a.z, b.z) + vm(a.w, b.w)); }\n"
"vec4 vdst(vec4 a, vec4 b) { return vec4(1.0, vm(a.y, b.y), a.z, b.w); }\n"
"vec4 vmin(vec4 a, vec4 b) { return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
"vec4 vmax(vec4 a, vec4 b) { return vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w); }\n"
"float lt1(float a, float b) { return (a < b || (floatBitsToUint(a) == 0x80000000u && floatBitsToUint(b) == 0u)) ? 1.0 : 0.0; }\n"
"vec4 vslt(vec4 a, vec4 b) { return vec4(lt1(a.x, b.x), lt1(a.y, b.y), lt1(a.z, b.z), lt1(a.w, b.w)); }\n"
"vec4 vsge(vec4 a, vec4 b) { return 1.0 - vslt(a, b); }\n"
"vec4 vrcp(vec4 a) {\n"
"    float x = a.x, r;\n"
"    if (x == 1.0) r = 1.0;\n"
"    else if (x == 0.0) r = (floatBitsToUint(x) & 0x80000000u) != 0u ? -uintBitsToFloat(0x7F800000u) : uintBitsToFloat(0x7F800000u);\n"
"    else r = 1.0 / x;\n"
"    return vec4(r);\n"
"}\n"
"vec4 vrcc(vec4 a) {\n"
"    float x = a.x, r;\n"
"    if (x == 1.0) r = 1.0;\n"
"    else {\n"
"        r = 1.0 / x;\n"
"        if (r > 0.0) {\n"
"            if (r < 5.42101e-020) r = 5.42101e-020;\n"
"            else if (r > 1.884467e+019) r = uintBitsToFloat(0x5F800000u);\n"
"        } else {\n"
"            if (r < -1.884467e+019) r = uintBitsToFloat(0xDF800000u);\n"
"            else if (r > -5.42101e-020) r = -5.42101e-020;\n"
"        }\n"
"    }\n"
"    return vec4(r);\n"
"}\n"
"vec4 vrsq(vec4 a) {\n"
"    float x = abs(a.x), r;\n"
"    if (x == 1.0) r = 1.0;\n"
"    else if (x == 0.0) r = uintBitsToFloat(0x7F800000u);\n"
"    else r = 1.0 / sqrt(x);\n"
"    return vec4(r);\n"
"}\n"
"vec4 vexp(vec4 a) {\n"
"    float t = floor(a.x);\n"
"    return vec4(exp2(t), a.x - t, exp2(a.x), 1.0);\n"
"}\n"
"vec4 vlog(vec4 a) {\n"
"    float t = abs(a.x);\n"
"    if (t == 0.0) return vec4(-uintBitsToFloat(0x7F800000u), 1.0, -uintBitsToFloat(0x7F800000u), 1.0);\n"
"    if (isinf(t)) return vec4(uintBitsToFloat(0x7F800000u), 1.0, uintBitsToFloat(0x7F800000u), 1.0);\n"
"    uint bits = floatBitsToUint(t);\n"
"    float e = float(int((bits >> 23) & 0xFFu) - 127);\n"
"    float m = uintBitsToFloat((bits & 0x7FFFFFu) | 0x3F800000u);\n"
"    return vec4(e, m, log2(t), 1.0);\n"
"}\n"
"vec4 vlit(vec4 a) {\n"
"    vec4 r = vec4(1.0, 0.0, 0.0, 1.0);\n"
"    float power = clamp(a.w, -127.9961, 127.9961);\n"
"    if (a.x > 0.0) {\n"
"        r.y = a.x;\n"
"        if (a.y > 0.0) r.z = pow(a.y, power);\n"
"    }\n"
"    return r;\n"
"}\n"
"float clamp_away(float t) {\n"
"    float lo = uintBitsToFloat(0x1F800000u), hi = uintBitsToFloat(0x5F800000u);\n"
"    if (t > 0.0 || floatBitsToUint(t) == 0u) return t < lo ? lo : (t > hi ? hi : t);\n"
"    return t < -hi ? -hi : (t > -lo ? -lo : t);\n"
"}\n"
"vec4 clamp_color(vec4 c) {\n"
"    vec4 r = c;\n"
"    r.x = isnan(c.x) ? 1.0 : clamp(c.x, 0.0, 1.0);\n"
"    r.y = isnan(c.y) ? 1.0 : clamp(c.y, 0.0, 1.0);\n"
"    r.z = isnan(c.z) ? 1.0 : clamp(c.z, 0.0, 1.0);\n"
"    r.w = isnan(c.w) ? 1.0 : clamp(c.w, 0.0, 1.0);\n"
"    return r;\n"
"}\n"
"float fog_factor(float d) {\n"
"    uint m = g_flags.y;\n"
"    float f = 1.0, infr = 0.0, nanr = 0.0;\n"
"    if (m == 0u || m == 4u) { infr = 1.0; nanr = 1.0; f = g_fog.x + d * g_fog.y - 1.0; }\n"
"    else if (m == 1u) { infr = 1.0; nanr = 1.0; f = g_fog.x + exp2(d * g_fog.y * 16.0) - 1.5; }\n"
"    else if (m == 5u) { f = g_fog.x + exp2(d * g_fog.y * 16.0) - 1.5; }\n"
"    else if (m == 3u || m == 7u) { f = g_fog.x + exp2(-d * d * g_fog.y * g_fog.y * 32.0) - 1.5; }\n"
"    if (m == 4u || m == 5u || m == 7u) f = abs(f);\n"
"    if (isinf(d)) return infr;\n"
"    if (isnan(f)) f = nanr;\n"
"    return clamp(f, -3.402823466e38, 3.402823466e38);\n"
"}\n";

/* An operand as a GLSL expression (vsh_operand). */
static bool vsh_operand(const Nv2aVshInput *in, char *out, size_t cap, uint32_t *attr_mask)
{
    char base[64];
    static const char sw[4] = { 'x', 'y', 'z', 'w' };
    switch (in->type) {
    case NV2ART_TEMPORARY:
        if (in->index == 12) snprintf(base, sizeof(base), "o0");
        else if (in->index < 12) snprintf(base, sizeof(base), "r%u", in->index);
        else return false;
        break;
    case NV2ART_INPUT:
        if (in->index > 15) return false;
        snprintf(base, sizeof(base), "v%u", in->index);
        *attr_mask |= 1u << in->index;
        break;
    case NV2ART_CONTEXT:
        if (in->index > 191) return false;
        if (in->is_relative) snprintf(base, sizeof(base), "C[clamp(int(%u) + int(a0), 0, 191)]", in->index);
        else snprintf(base, sizeof(base), "C[%u]", in->index);
        break;
    default:
        return false;
    }
    snprintf(out, cap, "%s(%s.%c%c%c%c)", in->is_negated ? "-" : "", base,
             sw[in->swizzle[0] & 3], sw[in->swizzle[1] & 3], sw[in->swizzle[2] & 3], sw[in->swizzle[3] & 3]);
    return true;
}

static bool vsh_op(KvkStr *b, const Nv2aVshOperation *op, const char *tag, uint32_t *attr_mask)
{
    char in[3][96];
    const char *fn = NULL;
    int nin = 0, k;
    if (op->opcode == NV2AOP_NOP) return true;
    switch (op->opcode) {
    case NV2AOP_MOV: case NV2AOP_ARL: case NV2AOP_RCP: case NV2AOP_RCC: case NV2AOP_RSQ:
    case NV2AOP_EXP: case NV2AOP_LOG: case NV2AOP_LIT:
        nin = 1; break;
    case NV2AOP_MUL: case NV2AOP_ADD: case NV2AOP_DP3: case NV2AOP_DPH: case NV2AOP_DP4:
    case NV2AOP_DST: case NV2AOP_MIN: case NV2AOP_MAX: case NV2AOP_SLT: case NV2AOP_SGE:
        nin = 2; break;
    case NV2AOP_MAD:
        nin = 3; break;
    default:
        return false;
    }
    for (k = 0; k < nin; k++)
        if (!vsh_operand(&op->inputs[k], in[k], sizeof(in[k]), attr_mask)) return false;
    for (k = 0; k < nin; k++) ks_add(b, "        vec4 %s_i%d = %s;\n", tag, k, in[k]);
    switch (op->opcode) {
    case NV2AOP_MOV: ks_add(b, "        vec4 %s_res = %s_i0;\n", tag, tag); break;
    case NV2AOP_ARL: ks_add(b, "        vec4 %s_res = vec4(floor(%s_i0.x + 0.001));\n", tag, tag); break;
    case NV2AOP_MUL: fn = "vmul"; break;
    case NV2AOP_ADD: ks_add(b, "        vec4 %s_res = %s_i0 + %s_i1;\n", tag, tag, tag); break;
    case NV2AOP_MAD: ks_add(b, "        vec4 %s_res = vmul(%s_i0, %s_i1) + %s_i2;\n", tag, tag, tag, tag); break;
    case NV2AOP_DP3: fn = "vdp3"; break;
    case NV2AOP_DPH: fn = "vdph"; break;
    case NV2AOP_DP4: fn = "vdp4"; break;
    case NV2AOP_DST: fn = "vdst"; break;
    case NV2AOP_MIN: fn = "vmin"; break;
    case NV2AOP_MAX: fn = "vmax"; break;
    case NV2AOP_SLT: fn = "vslt"; break;
    case NV2AOP_SGE: fn = "vsge"; break;
    case NV2AOP_RCP: ks_add(b, "        vec4 %s_res = vrcp(%s_i0);\n", tag, tag); break;
    case NV2AOP_RCC: ks_add(b, "        vec4 %s_res = vrcc(%s_i0);\n", tag, tag); break;
    case NV2AOP_RSQ: ks_add(b, "        vec4 %s_res = vrsq(%s_i0);\n", tag, tag); break;
    case NV2AOP_EXP: ks_add(b, "        vec4 %s_res = vexp(%s_i0);\n", tag, tag); break;
    case NV2AOP_LOG: ks_add(b, "        vec4 %s_res = vlog(%s_i0);\n", tag, tag); break;
    case NV2AOP_LIT: ks_add(b, "        vec4 %s_res = vlit(%s_i0);\n", tag, tag); break;
    default: return false;
    }
    if (fn) ks_add(b, "        vec4 %s_res = %s(%s_i0, %s_i1);\n", tag, fn, tag, tag);
    return true;
}

static bool vsh_writes(KvkStr *b, const Nv2aVshOperation *op, const char *tag)
{
    int k;
    if (op->opcode == NV2AOP_NOP) return true;
    for (k = 0; k < 2; k++) {
        const Nv2aVshOutput *o = &op->outputs[k];
        char name[16];
        switch (o->type) {
        case NV2ART_NONE: continue;
        case NV2ART_ADDRESS:
            ks_add(b, "        a0 = %s_res.x;\n", tag);
            continue;
        case NV2ART_OUTPUT:
            if (o->index > 12) return false;
            snprintf(name, sizeof(name), "o%u", o->index);
            break;
        case NV2ART_TEMPORARY:
            if (o->index > 11) return false;
            snprintf(name, sizeof(name), "r%u", o->index);
            break;
        default:
            return false;       /* constant writes: the CPU stage runs this program */
        }
        if (o->writemask & NV2AWM_X) ks_add(b, "        %s.x = %s_res.x;\n", name, tag);
        if (o->writemask & NV2AWM_Y) ks_add(b, "        %s.y = %s_res.y;\n", name, tag);
        if (o->writemask & NV2AWM_Z) ks_add(b, "        %s.z = %s_res.z;\n", name, tag);
        if (o->writemask & NV2AWM_W) ks_add(b, "        %s.w = %s_res.w;\n", name, tag);
    }
    return true;
}

char *kvk_glsl_vs_program(const Nv2aVshProgram *prog)
{
    KvkStr b = { 0 }, body = { 0 };
    uint32_t attr_mask = 0;
    const Nv2aVshStep *st;
    int step = 0, i;
    if (!prog || !prog->steps) return NULL;
    for (st = prog->steps;; st++, step++) {
        ks_add(&body, "    { // step %d\n", step);
        if (!vsh_op(&body, &st->mac, "m", &attr_mask) || !vsh_op(&body, &st->ilu, "i", &attr_mask) ||
            !vsh_writes(&body, &st->mac, "m") || !vsh_writes(&body, &st->ilu, "i")) {
            free(body.p);
            return NULL;
        }
        ks_add(&body, "    }\n");
        if (st->is_final || step > 400) break;
    }
    ks_add(&b, "%s%s%s%s%s", kvk_head, kvk_push, kvk_stereo, kvk_vsh_head, kvk_vs_out);
    ks_add(&b, "void main() {\n    uint vid = uint(gl_VertexIndex);\n");
    for (i = 0; i < 16; i++)
        if (attr_mask & (1u << i)) ks_add(&b, "    vec4 v%d = FetchAttr(%du, vid);\n", i, i);
    for (i = 0; i < 12; i++) ks_add(&b, "    vec4 r%d = vec4(0.0, 0.0, 0.0, 0.0);\n", i);
    for (i = 0; i < 13; i++) ks_add(&b, "    vec4 o%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
    ks_add(&b, "    float a0 = 0.0;\n");
    ks_add(&b, "%s", body.p ? body.p : "");
    if (body.bad) b.bad = true;
    free(body.p);
    /* kelvin_vsh.c's epilogue, as vsh_generate writes it; then kvk_finish (VR eye, Vulkan y) */
    ks_add(&b,
        "    vec4 pp = o0;\n"
        "    if (g_vm.x != 1.0) pp.xy = C[59].xy + (pp.xy - C[59].xy) * g_vm.x;\n"
        "    pp.x = trunc(pp.x * 16.0) / 16.0;\n"
        "    pp.y = trunc(pp.y * 16.0) / 16.0;\n"
        "    pp.w = clamp_away(pp.w);\n"
        "    float fogv = g_flags.x != 0u ? fog_factor(o5.x) : 1.0;\n"
        "    vec4 c0 = clamp_color(o3);\n"
        "    vec4 c1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    if (g_flags.z != 0u) { c1 = clamp_color(o4); if (g_flags.w != 0u) c1.w = 1.0; }\n"
        "    float w = pp.w;\n"
        "    if (abs(w) < 1.0e-12) w *= g_scale.w;\n"
        "    vec2 ndc = vec2(pp.x * g_scale.x - 1.0, 1.0 - pp.y * g_scale.y);\n"
        "    vec4 pos = vec4(ndc * w, pp.z * g_scale.z * w, w);\n"
        "    pos.xy += g_jit.xy * w;\n"
        "    gl_Position = kvk_finish(pos);\n"
        "    v_d0 = c0;\n"
        "    v_d1 = c1;\n"
        "    v_misc = vec4(fogv, 0.0, 0.0, 0.0);\n"
        "    v_t0 = o9;\n"
        "    v_t1 = o10;\n"
        "    v_t2 = o11;\n"
        "    v_t3 = o12;\n"
        "}\n");
    return ks_take(&b);
}

/* ---- CPU-shaded vertices (kelvin_gpu_hlsl.inc kgpu_hlsl_vs) --------------------------------------------- */

char *kvk_glsl_vs_cpu(void)
{
    KvkStr b = { 0 };
    ks_add(&b, "%s%s%s", kvk_head, kvk_push, kvk_stereo);
    ks_add(&b,
        "layout(std140, set = 0, binding = 1) uniform VSConst {\n"
        "    vec4  vs_scale;\n"      /* x 2/guestW, y 2/guestH, z 1/zmax, w w-normalise factor */
        "    uvec4 vs_rec0;\n"       /* x base, y stride, z pos slot, w d0 slot */
        "    uvec4 vs_rec1;\n"       /* x d1 slot, y fog slot, z t0 slot, w t1 slot */
        "    uvec4 vs_rec2;\n"       /* x t2 slot, y t3 slot */
        "    vec4  vs_misc;\n"
        "};\n"
        "layout(std430, set = 0, binding = 4) readonly buffer VB { vec4 vb[]; };\n");
    ks_add(&b, "%s", kvk_vs_out);
    ks_add(&b,
        "vec4 FetchRec(uint base, uint slot, vec4 dflt) {\n"
        "    return slot == 0xFFFFFFFFu ? dflt : vb[base + slot];\n"
        "}\n"
        "void main() {\n"
        "    uint vid = uint(gl_VertexIndex);\n"
        "    uint base = vs_rec0.x + vid * vs_rec0.y;\n"
        "    vec4 p = vb[base + vs_rec0.z];\n"
        "    float w = p.w;\n"
        "    if (abs(w) < 1.0e-12) w *= vs_scale.w;\n"
        "    vec2 ndc = vec2(p.x * vs_scale.x - 1.0, 1.0 - p.y * vs_scale.y);\n"
        "    float zn = p.z * vs_scale.z;\n"
        "    gl_Position = kvk_finish(vec4(ndc * w, zn * w, w));\n"
        "    v_d0 = FetchRec(base, vs_rec0.w, vec4(0, 0, 0, 1));\n"
        "    v_d1 = FetchRec(base, vs_rec1.x, vec4(0, 0, 0, 1));\n"
        "    v_misc = FetchRec(base, vs_rec1.y, vec4(1, 0, 0, 0));\n"
        "    v_t0 = FetchRec(base, vs_rec1.z, vec4(0, 0, 0, 1));\n"
        "    v_t1 = FetchRec(base, vs_rec1.w, vec4(0, 0, 0, 1));\n"
        "    v_t2 = FetchRec(base, vs_rec2.x, vec4(0, 0, 0, 1));\n"
        "    v_t3 = FetchRec(base, vs_rec2.y, vec4(0, 0, 0, 1));\n"
        "}\n");
    return ks_take(&b);
}

/* ---- specialised combiners (kelvin_gpu_pss.inc) ------------------------------------------------------------ */

static void ps_map(char *out, size_t cap, const char *x, uint32_t mod)
{
    switch (mod & 0xE0u) {
    case 0x00: snprintf(out, cap, "max(%s, 0.0)", x); break;
    case 0x20: snprintf(out, cap, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;
    case 0x40: snprintf(out, cap, "(2.0 * max(%s, 0.0) - 1.0)", x); break;
    case 0x60: snprintf(out, cap, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;
    case 0x80: snprintf(out, cap, "(max(%s, 0.0) - 0.5)", x); break;
    case 0xA0: snprintf(out, cap, "(-max(%s, 0.0) + 0.5)", x); break;
    case 0xE0: snprintf(out, cap, "(-%s)", x); break;
    default:   snprintf(out, cap, "%s", x); break;
    }
}

static void ps_mapout(char *out, size_t cap, const char *x, uint32_t m)
{
    switch (m & 0x38u) {
    case 0x08: snprintf(out, cap, "(%s - 0.5)", x); break;
    case 0x10: snprintf(out, cap, "(%s * 2.0)", x); break;
    case 0x18: snprintf(out, cap, "((%s - 0.5) * 2.0)", x); break;
    case 0x20: snprintf(out, cap, "(%s * 4.0)", x); break;
    case 0x30: snprintf(out, cap, "(%s * 0.5)", x); break;
    default:   snprintf(out, cap, "%s", x); break;
    }
}

static bool ps_writable(uint32_t r) { return r == 4u || r == 5u || (r >= 8u && r <= 13u); }

static void ps_input3(char *out, size_t cap, uint32_t in)
{
    char x[40];
    snprintf(x, sizeof(x), "r%u.%s", in & 15u, (in & 0x10u) ? "aaa" : "rgb");
    ps_map(out, cap, x, in & 0xE0u);
}

static void ps_input1(char *out, size_t cap, uint32_t in)
{
    char x[40];
    snprintf(x, sizeof(x), "r%u.%s", in & 15u, (in & 0x10u) ? "a" : "b");
    ps_map(out, cap, x, in & 0xE0u);
}

uint64_t kvk_ps_key(const PSConst *c)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t nst = c->gen[0] > 8 ? 8 : c->gen[0];
    unsigned int i, k;
#define PS_MIX(v) h = (h ^ (uint64_t)(uint32_t)(v)) * 1099511628211ULL
    PS_MIX(nst); PS_MIX(c->gen[1]); PS_MIX(c->ofs[0]); PS_MIX(c->gen[3]);
    for (k = 0; k < 4; k++) { PS_MIX(c->texmode[k]); PS_MIX(c->texflags[k]); PS_MIX(c->fin_abcd[k]); PS_MIX(c->fin_efg[k]); }
    for (i = 0; i < nst; i++) {
        for (k = 0; k < 4; k++) { PS_MIX(c->stage[i].rgb_in[k]); PS_MIX(c->stage[i].alpha_in[k]); }
        PS_MIX(c->stage[i].outs[0]); PS_MIX(c->stage[i].outs[1]);
    }
#undef PS_MIX
    return h ? h : 1;
}

char *kvk_glsl_ps_spec(const PSConst *c)
{
    KvkStr b = { 0 }, mn = { 0 }, cmb = { 0 }, tl = { 0 };
    uint32_t nst = c->gen[0] > 8 ? 8 : c->gen[0], fflags = c->fin_efg[3], func = c->gen[1];
    unsigned int i, k;
    char ex[128], ey[128];
    char *out;

    ks_add(&b, "%s", kvk_head);
    if (c->gen[3]) ks_add(&b, "#define KG_FLAT 1\n");
    ks_add(&b, "%s%s%s%s", kvk_push, kvk_ps_const, kvk_fs_in, kvk_textures);
    /* textures: only the ones in use are sampled, outside any branch (defined derivatives) */
    for (i = 0; i < 4; i++) {
        uint32_t mode = c->texmode[i], fl = c->texflags[i];
        const char *comp = i == 0 ? "x" : i == 1 ? "y" : i == 2 ? "z" : "w";
        if (mode == 1u && (fl & 1u)) {
            ks_add(&mn,
                "    vec2 uv%u = vec2(0.0, 0.0);\n"
                "    if (v_t%u.w != 0.0) uv%u = v_t%u.xy / v_t%u.w;\n", i, i, i, i, i);
            if (fl & 2u) ks_add(&mn, "    uv%u *= texrcp[%u].xy;\n", i, i);
            ks_add(&mn,
                "    vec4 s%u = texture(Tex%u, vec3(uv%u, kvk_layer(%uu)));\n"
                "    vec4 t%u = vec4(0.0, 0.0, 0.0, 1.0);\n"
                "    if (v_t%u.w != 0.0) {\n"
                "        t%u = s%u;\n", i, i, i, i, i, i, i, i);
            if (fl & 4u) ks_add(&mn, "        if (t%u.a == 0.0) killed = true;\n", i);
            if ((fl >> 4) & 3u) {
                uint32_t ckm = (fl >> 4) & 3u;
                ks_add(&mn,
                    "        {\n"
                    "            uvec4 q = uvec4(clamp(t%u, 0.0, 1.0) * 255.0 + 0.5);\n"
                    "            uint cc = (q.a << 24) | (q.r << 16) | (q.g << 8) | q.b;\n"
                    "            if ((cc & ckeymask.%s) == (ckey.%s & ckeymask.%s)) {\n", i, comp, comp, comp);
                if (ckm == 3u) ks_add(&mn, "                killed = true;\n");
                else if (ckm == 1u) ks_add(&mn, "                t%u.a = 0.0;\n", i);
                else ks_add(&mn, "                t%u = vec4(0.0, 0.0, 0.0, 0.0);\n", i);
                ks_add(&mn, "            }\n        }\n");
            }
            ks_add(&mn, "    }\n");
        } else {
            ks_add(&mn, "    vec4 t%u = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
        }
    }
    ks_add(&cmb,
        "    vec4 r0 = vec4(0, 0, 0, 0);\n"
        "    vec4 r1 = stage[0].c0;\n"
        "    vec4 r2 = stage[0].c1;\n"
        "    vec4 r3 = vec4(fogcolor.rgb, clamp(v_misc.x, 0.0, 1.0));\n"
        "    vec4 r4 = v_d0;\n"
        "    vec4 r5 = v_d1;\n"
        "    vec4 r6 = vec4(0, 0, 0, 0);\n"
        "    vec4 r7 = vec4(0, 0, 0, 0);\n"
        "    vec4 r8 = t0; vec4 r9 = t1; vec4 r10 = t2; vec4 r11 = t3;\n"
        "    vec4 r12 = vec4(0.0, 0.0, 0.0, %s);\n"
        "    vec4 r13 = vec4(0, 0, 0, 0);\n"
        "    vec4 r14 = vec4(0, 0, 0, 0);\n"
        "    vec4 r15 = vec4(0, 0, 0, 0);\n", c->texmode[0] != 0u ? "t0.a" : "1.0");
    for (i = 0; i < nst; i++) {
        uint32_t ro = c->stage[i].outs[0], ao = c->stage[i].outs[1];
        uint32_t rcd = ro & 15u, rab = (ro >> 4) & 15u, rmx = (ro >> 8) & 15u, rfl = ro >> 12, rmap = rfl & 0x38u;
        uint32_t acd = ao & 15u, aab = (ao >> 4) & 15u, amx = (ao >> 8) & 15u, afl = ao >> 12, amap = afl & 0x38u;
        ks_add(&cmb, "    {\n");
        ks_add(&cmb, "        r1 = stage[%u].c0;\n", (fflags & 0x20u) ? i : 0u);
        ks_add(&cmb, "        r2 = stage[%u].c1;\n", (fflags & 0x40u) ? i : 0u);
        ks_add(&cmb, "        {\n            vec3 a1 = %s;\n            vec3 b1 = %s;\n            vec3 sm = a1 + b1;\n",
               (fflags & 4u) ? "1.0 - r5.rgb" : "r5.rgb", (fflags & 8u) ? "1.0 - r12.rgb" : "r12.rgb");
        if (fflags & 2u) ks_add(&cmb, "            sm = clamp(sm, 0.0, 1.0);\n");
        ks_add(&cmb, "            r14 = vec4(sm, 0.0);\n        }\n");
        for (k = 0; k < 4; k++) {
            ps_input3(ex, sizeof(ex), c->stage[i].rgb_in[k]);
            ps_input1(ey, sizeof(ey), c->stage[i].alpha_in[k]);
            ks_add(&cmb, "        vec3 ra%u = %s;\n        float aa%u = %s;\n", k, ex, k, ey);
        }
        ks_add(&cmb, "        vec3 ab3 = %s;\n", (rfl & 2u) ? "vec3(dot(ra0, ra1))" : "ra0 * ra1");
        ks_add(&cmb, "        vec3 cd3 = %s;\n", (rfl & 1u) ? "vec3(dot(ra2, ra3))" : "ra2 * ra3");
        if ((rfl & 4u) || (afl & 4u)) {
            if (fflags & 0x10u) ks_add(&cmb, "        bool selr = r12.a >= 0.5;\n");
            /* D3D's float -> uint conversion takes negatives to zero; GLSL leaves them undefined */
            else ks_add(&cmb, "        bool selr = ((uint(max(r12.a * 255.0, 0.0)) & 1u) == 1u);\n");
        }
        ks_add(&cmb, "        vec3 mx3 = %s;\n", (rfl & 4u) ? "(selr ? cd3 : ab3)" : "(ab3 + cd3)");
        ps_mapout(ex, sizeof(ex), "ab3", rmap); ks_add(&cmb, "        vec3 rab3 = clamp(%s, -1.0, 1.0);\n", ex);
        ps_mapout(ex, sizeof(ex), "cd3", rmap); ks_add(&cmb, "        vec3 rcd3 = clamp(%s, -1.0, 1.0);\n", ex);
        ps_mapout(ex, sizeof(ex), "mx3", rmap); ks_add(&cmb, "        vec3 rmx3 = clamp(%s, -1.0, 1.0);\n", ex);
        ks_add(&cmb, "        float ab1 = aa0 * aa1;\n        float cd1 = aa2 * aa3;\n");
        ks_add(&cmb, "        float mx1 = %s;\n", (afl & 4u) ? "(selr ? cd1 : ab1)" : "(ab1 + cd1)");
        ps_mapout(ex, sizeof(ex), "ab1", amap); ks_add(&cmb, "        float rab1 = clamp(%s, -1.0, 1.0);\n", ex);
        ps_mapout(ex, sizeof(ex), "cd1", amap); ks_add(&cmb, "        float rcd1 = clamp(%s, -1.0, 1.0);\n", ex);
        ps_mapout(ex, sizeof(ex), "mx1", amap); ks_add(&cmb, "        float rmx1 = clamp(%s, -1.0, 1.0);\n", ex);
        if (ps_writable(rab)) {
            ks_add(&cmb, "        r%u.rgb = rab3;\n", rab);
            if (rfl & 0x80u) ks_add(&cmb, "        r%u.a = rab3.b;\n", rab);
        }
        if (ps_writable(rcd)) {
            ks_add(&cmb, "        r%u.rgb = rcd3;\n", rcd);
            if (rfl & 0x40u) ks_add(&cmb, "        r%u.a = rcd3.b;\n", rcd);
        }
        if (ps_writable(rmx)) ks_add(&cmb, "        r%u.rgb = rmx3;\n", rmx);
        if (ps_writable(aab)) ks_add(&cmb, "        r%u.a = rab1;\n", aab);
        if (ps_writable(acd)) ks_add(&cmb, "        r%u.a = rcd1;\n", acd);
        if (ps_writable(amx)) ks_add(&cmb, "        r%u.a = rmx1;\n", amx);
        ks_add(&cmb, "    }\n");
    }
    ks_add(&cmb, "    vec4 frag;\n");
    if (fflags & 1u) {
        uint32_t ine = c->fin_efg[0], inf = c->fin_efg[1], ing = c->fin_efg[2];
        ks_add(&cmb, "    {\n        r1 = final_c0;\n        r2 = final_c1;\n");
        ks_add(&cmb, "        {\n            vec3 a1 = %s;\n            vec3 b1 = %s;\n            vec3 sm = a1 + b1;\n",
               (fflags & 4u) ? "1.0 - r5.rgb" : "r5.rgb", (fflags & 8u) ? "1.0 - r12.rgb" : "r12.rgb");
        if (fflags & 2u) ks_add(&cmb, "            sm = clamp(sm, 0.0, 1.0);\n");
        ks_add(&cmb, "            r14 = vec4(sm, 0.0);\n        }\n");
        ps_input3(ex, sizeof(ex), ine); ps_input3(ey, sizeof(ey), inf);
        ks_add(&cmb, "        vec3 e3 = %s;\n        vec3 f3 = %s;\n        r15 = vec4(e3 * f3, 0.0);\n", ex, ey);
        ps_input3(ex, sizeof(ex), c->fin_abcd[0]); ks_add(&cmb, "        vec3 a3 = %s;\n", ex);
        ps_input3(ex, sizeof(ex), c->fin_abcd[1]); ks_add(&cmb, "        vec3 b3 = %s;\n", ex);
        ps_input3(ex, sizeof(ex), c->fin_abcd[2]); ks_add(&cmb, "        vec3 c3 = %s;\n", ex);
        ps_input3(ex, sizeof(ex), c->fin_abcd[3]); ks_add(&cmb, "        vec3 d3 = %s;\n", ex);
        ps_input1(ex, sizeof(ex), ing); ks_add(&cmb, "        float g1 = %s;\n", ex);
        ks_add(&cmb, "        frag = vec4(d3 + c3 * (1.0 - a3) + b3 * a3, g1);\n    }\n");
    } else {
        ks_add(&cmb, "    frag = r12;\n");
    }
    if (func != 7u) {
        static const char *const cmp[] = { NULL, "fa < ref", "fa == ref", "fa <= ref", "fa > ref", "fa != ref", "fa >= ref" };
        if (func == 0u) {
            ks_add(&tl, "    discard;\n");
        } else if (func < 7u) {
            ks_add(&tl, "    {\n        int fa = int(floor(frag.a * 255.0 + 0.5));\n        int ref = int(gen.z);\n"
                       "        if (!(%s)) discard;\n    }\n", cmp[func]);
        }
    }
    ks_add(&tl, "    vec4 o = clamp(frag, 0.0, 1.0);\n");
    if (c->ofs[0] == 1u) ks_add(&tl, "    o.a = 1.0;\n");
    else if (c->ofs[0] == 2u) ks_add(&tl, "    o.a = 0.0;\n");
    ks_add(&b, "vec4 kg_combine(vec4 t0, vec4 t1, vec4 t2, vec4 t3) {\n%s    return frag;\n}\n", cmb.p ? cmb.p : "");
    ks_add(&b, "void main() {\n    bool killed = false;\n%s    if (killed) discard;\n"
               "    vec4 frag = kg_combine(t0, t1, t2, t3);\n%s    o_color = o;\n}\n",
           mn.p ? mn.p : "", tl.p ? tl.p : "");
    if (mn.bad || cmb.bad || tl.bad) b.bad = true;
    free(mn.p); free(cmb.p); free(tl.p);
    out = ks_take(&b);
    return out;
}

/* ---- the combiner interpreter (kelvin_gpu_hlsl.inc kgpu_hlsl_ps) ------------------------------------------- */

static const char kvk_ps_uber_body[] =
"vec3 map3(vec3 x, uint mod) {\n"
"    if (mod == 0x00u) return max(x, 0.0);\n"
"    if (mod == 0x20u) return 1.0 - clamp(x, 0.0, 1.0);\n"
"    if (mod == 0x40u) return 2.0 * max(x, 0.0) - 1.0;\n"
"    if (mod == 0x60u) return -2.0 * max(x, 0.0) + 1.0;\n"
"    if (mod == 0x80u) return max(x, 0.0) - 0.5;\n"
"    if (mod == 0xA0u) return -max(x, 0.0) + 0.5;\n"
"    if (mod == 0xE0u) return -x;\n"
"    return x;\n"
"}\n"
"float map1(float x, uint mod) { return map3(vec3(x, x, x), mod).x; }\n"
"vec3 mapout3(vec3 x, uint m) {\n"
"    if (m == 0x08u) return x - 0.5;\n"
"    if (m == 0x10u) return x * 2.0;\n"
"    if (m == 0x18u) return (x - 0.5) * 2.0;\n"
"    if (m == 0x20u) return x * 4.0;\n"
"    if (m == 0x30u) return x * 0.5;\n"
"    return x;\n"
"}\n"
"float mapout1(float x, uint m) { return mapout3(vec3(x, x, x), m).x; }\n"
"vec3 clamp1_3(vec3 x) { return clamp(x, -1.0, 1.0); }\n"
"float clamp1_1(float x) { return clamp(x, -1.0, 1.0); }\n"
"bool Writable(uint r) { return r == 4u || r == 5u || (r >= 8u && r <= 13u); }\n"
"\n"
"void main() {\n"
"    vec4 R[16];\n"
"    vec4 T[4];\n"
"    bool killed = false;\n"
"    vec4 vt[4] = vec4[4](v_t0, v_t1, v_t2, v_t3);\n"
"    vec4 rcp[4] = vec4[4](texrcp[0], texrcp[1], texrcp[2], texrcp[3]);\n"
"    vec4 s0, s1, s2, s3;\n"
"    {\n"
"        vec2 uv0 = vec2(0.0, 0.0); vec2 uv1 = uv0; vec2 uv2 = uv0; vec2 uv3 = uv0;\n"
"        if (vt[0].w != 0.0) uv0 = vt[0].xy / vt[0].w;\n"
"        if (vt[1].w != 0.0) uv1 = vt[1].xy / vt[1].w;\n"
"        if (vt[2].w != 0.0) uv2 = vt[2].xy / vt[2].w;\n"
"        if (vt[3].w != 0.0) uv3 = vt[3].xy / vt[3].w;\n"
"        if ((texflags.x & 2u) != 0u) uv0 *= rcp[0].xy;\n"
"        if ((texflags.y & 2u) != 0u) uv1 *= rcp[1].xy;\n"
"        if ((texflags.z & 2u) != 0u) uv2 *= rcp[2].xy;\n"
"        if ((texflags.w & 2u) != 0u) uv3 *= rcp[3].xy;\n"
"        s0 = texture(Tex0, vec3(uv0, kvk_layer(0u)));\n"
"        s1 = texture(Tex1, vec3(uv1, kvk_layer(1u)));\n"
"        s2 = texture(Tex2, vec3(uv2, kvk_layer(2u)));\n"
"        s3 = texture(Tex3, vec3(uv3, kvk_layer(3u)));\n"
"    }\n"
"    vec4 samp[4] = vec4[4](s0, s1, s2, s3);\n"
"    for (uint ti = 0u; ti < 4u; ti++) {\n"
"        vec4 t = vec4(0.0, 0.0, 0.0, 1.0);\n"
"        uint mode = ti == 0u ? texmode.x : ti == 1u ? texmode.y : ti == 2u ? texmode.z : texmode.w;\n"
"        uint fl = ti == 0u ? texflags.x : ti == 1u ? texflags.y : ti == 2u ? texflags.z : texflags.w;\n"
"        uint key = ti == 0u ? ckey.x : ti == 1u ? ckey.y : ti == 2u ? ckey.z : ckey.w;\n"
"        uint kmask = ti == 0u ? ckeymask.x : ti == 1u ? ckeymask.y : ti == 2u ? ckeymask.z : ckeymask.w;\n"
"        if (mode == 1u && (fl & 1u) != 0u && vt[ti].w != 0.0) {\n"
"            t = samp[ti];\n"
"            if ((fl & 4u) != 0u && t.a == 0.0) killed = true;\n"
"            uint ckm = (fl >> 4) & 3u;\n"
"            if (ckm != 0u) {\n"
"                uvec4 q = uvec4(clamp(t, 0.0, 1.0) * 255.0 + 0.5);\n"
"                uint c = (q.a << 24) | (q.r << 16) | (q.g << 8) | q.b;\n"
"                if ((c & kmask) == (key & kmask)) {\n"
"                    if (ckm == 3u) killed = true;\n"
"                    else if (ckm == 1u) t.a = 0.0;\n"
"                    else t = vec4(0.0, 0.0, 0.0, 0.0);\n"
"                }\n"
"            }\n"
"        } else if (mode == 1u) {\n"
"            t = vec4(0.0, 0.0, 0.0, 1.0);\n"
"        }\n"
"        T[ti] = t;\n"
"    }\n"
"    if (killed) discard;\n"
"\n"
"    uint fflags = fin_efg.w;\n"
"    R[0] = vec4(0, 0, 0, 0);\n"
"    R[1] = stage[0].c0;\n"
"    R[2] = stage[0].c1;\n"
"    R[3] = vec4(fogcolor.rgb, clamp(v_misc.x, 0.0, 1.0));\n"
"    R[4] = v_d0;\n"
"    R[5] = v_d1;\n"
"    R[6] = vec4(0, 0, 0, 0);\n"
"    R[7] = vec4(0, 0, 0, 0);\n"
"    R[8] = T[0]; R[9] = T[1]; R[10] = T[2]; R[11] = T[3];\n"
"    R[12] = vec4(0.0, 0.0, 0.0, texmode.x != 0u ? T[0].a : 1.0);\n"
"    R[13] = vec4(0, 0, 0, 0);\n"
"    R[14] = vec4(0, 0, 0, 0);\n"
"    R[15] = vec4(0, 0, 0, 0);\n"
"\n"
"    uint nst = gen.x;\n"
"    for (uint i = 0u; i < nst; i++) {\n"
"        StageCfg s = stage[i];\n"
"        if ((fflags & 0x20u) != 0u) R[1] = s.c0; else R[1] = stage[0].c0;\n"
"        if ((fflags & 0x40u) != 0u) R[2] = s.c1; else R[2] = stage[0].c1;\n"
"        {\n"
"            vec3 a1 = (fflags & 4u) != 0u ? 1.0 - R[5].rgb : R[5].rgb;\n"
"            vec3 b1 = (fflags & 8u) != 0u ? 1.0 - R[12].rgb : R[12].rgb;\n"
"            vec3 sm = a1 + b1;\n"
"            if ((fflags & 2u) != 0u) sm = clamp(sm, 0.0, 1.0);\n"
"            R[14] = vec4(sm, 0.0);\n"
"        }\n"
"        vec3 ra[4];\n"
"        float aa[4];\n"
"        uint rin[4] = uint[4](s.rgb_in.x, s.rgb_in.y, s.rgb_in.z, s.rgb_in.w);\n"
"        uint ain[4] = uint[4](s.alpha_in.x, s.alpha_in.y, s.alpha_in.z, s.alpha_in.w);\n"
"        for (uint k = 0u; k < 4u; k++) {\n"
"            vec4 rv = R[rin[k] & 15u];\n"
"            ra[k] = (rin[k] & 0x10u) != 0u ? map3(rv.aaa, rin[k] & 0xE0u) : map3(rv.rgb, rin[k] & 0xE0u);\n"
"            vec4 av = R[ain[k] & 15u];\n"
"            aa[k] = map1((ain[k] & 0x10u) != 0u ? av.a : av.b, ain[k] & 0xE0u);\n"
"        }\n"
"        uint ro = s.outs.x;\n"
"        uint rcd = ro & 15u, rab = (ro >> 4) & 15u, rmx = (ro >> 8) & 15u, rfl = ro >> 12;\n"
"        uint rmap = rfl & 0x38u;\n"
"        vec3 ab3 = (rfl & 2u) != 0u ? vec3(dot(ra[0], ra[1])) : ra[0] * ra[1];\n"
"        vec3 cd3 = (rfl & 1u) != 0u ? vec3(dot(ra[2], ra[3])) : ra[2] * ra[3];\n"
"        bool selr = ((fflags & 0x10u) != 0u) ? (R[12].a >= 0.5) : ((uint(max(R[12].a * 255.0, 0.0)) & 1u) == 1u);\n"
"        vec3 mx3 = (rfl & 4u) != 0u ? (selr ? cd3 : ab3) : (ab3 + cd3);\n"
"        vec3 rab3 = clamp1_3(mapout3(ab3, rmap));\n"
"        vec3 rcd3 = clamp1_3(mapout3(cd3, rmap));\n"
"        vec3 rmx3 = clamp1_3(mapout3(mx3, rmap));\n"
"        uint ao = s.outs.y;\n"
"        uint acd = ao & 15u, aab = (ao >> 4) & 15u, amx = (ao >> 8) & 15u, afl = ao >> 12;\n"
"        uint amap = afl & 0x38u;\n"
"        float ab1 = aa[0] * aa[1];\n"
"        float cd1 = aa[2] * aa[3];\n"
"        float mx1 = (afl & 4u) != 0u ? (selr ? cd1 : ab1) : (ab1 + cd1);\n"
"        float rab1 = clamp1_1(mapout1(ab1, amap));\n"
"        float rcd1 = clamp1_1(mapout1(cd1, amap));\n"
"        float rmx1 = clamp1_1(mapout1(mx1, amap));\n"
"        if (Writable(rab)) { R[rab].rgb = rab3; if ((rfl & 0x80u) != 0u) R[rab].a = rab3.b; }\n"
"        if (Writable(rcd)) { R[rcd].rgb = rcd3; if ((rfl & 0x40u) != 0u) R[rcd].a = rcd3.b; }\n"
"        if (Writable(rmx)) { R[rmx].rgb = rmx3; }\n"
"        if (Writable(aab)) { R[aab].a = rab1; }\n"
"        if (Writable(acd)) { R[acd].a = rcd1; }\n"
"        if (Writable(amx)) { R[amx].a = rmx1; }\n"
"    }\n"
"\n"
"    vec4 frag;\n"
"    if ((fflags & 1u) != 0u) {\n"
"        R[1] = final_c0;\n"
"        R[2] = final_c1;\n"
"        {\n"
"            vec3 a1 = (fflags & 4u) != 0u ? 1.0 - R[5].rgb : R[5].rgb;\n"
"            vec3 b1 = (fflags & 8u) != 0u ? 1.0 - R[12].rgb : R[12].rgb;\n"
"            vec3 sm = a1 + b1;\n"
"            if ((fflags & 2u) != 0u) sm = clamp(sm, 0.0, 1.0);\n"
"            R[14] = vec4(sm, 0.0);\n"
"        }\n"
"        uint ine = fin_efg.x, inf = fin_efg.y, ing = fin_efg.z;\n"
"        vec4 ev = R[ine & 15u], fv = R[inf & 15u];\n"
"        vec3 e3 = (ine & 0x10u) != 0u ? map3(ev.aaa, ine & 0xE0u) : map3(ev.rgb, ine & 0xE0u);\n"
"        vec3 f3 = (inf & 0x10u) != 0u ? map3(fv.aaa, inf & 0xE0u) : map3(fv.rgb, inf & 0xE0u);\n"
"        R[15] = vec4(e3 * f3, 0.0);\n"
"        uint fa = fin_abcd.x, fb = fin_abcd.y, fc = fin_abcd.z, fd = fin_abcd.w;\n"
"        vec4 av = R[fa & 15u], bv = R[fb & 15u], cv = R[fc & 15u], dv = R[fd & 15u];\n"
"        vec3 a3 = (fa & 0x10u) != 0u ? map3(av.aaa, fa & 0xE0u) : map3(av.rgb, fa & 0xE0u);\n"
"        vec3 b3 = (fb & 0x10u) != 0u ? map3(bv.aaa, fb & 0xE0u) : map3(bv.rgb, fb & 0xE0u);\n"
"        vec3 c3 = (fc & 0x10u) != 0u ? map3(cv.aaa, fc & 0xE0u) : map3(cv.rgb, fc & 0xE0u);\n"
"        vec3 d3 = (fd & 0x10u) != 0u ? map3(dv.aaa, fd & 0xE0u) : map3(dv.rgb, fd & 0xE0u);\n"
"        vec4 gv = R[ing & 15u];\n"
"        float g1 = map1((ing & 0x10u) != 0u ? gv.a : gv.b, ing & 0xE0u);\n"
"        frag = vec4(d3 + c3 * (1.0 - a3) + b3 * a3, g1);\n"
"    } else {\n"
"        frag = R[12];\n"
"    }\n"
"\n"
"    uint func = gen.y;\n"
"    if (func != 7u) {\n"
"        int fa = int(floor(frag.a * 255.0 + 0.5));\n"
"        int ref = int(gen.z);\n"
"        bool apass = true;\n"
"        if (func == 0u) apass = false;\n"
"        else if (func == 1u) apass = fa < ref;\n"
"        else if (func == 2u) apass = fa == ref;\n"
"        else if (func == 3u) apass = fa <= ref;\n"
"        else if (func == 4u) apass = fa > ref;\n"
"        else if (func == 5u) apass = fa != ref;\n"
"        else if (func == 6u) apass = fa >= ref;\n"
"        if (!apass) discard;\n"
"    }\n"
"    vec4 o = clamp(frag, 0.0, 1.0);\n"
"    if (ofs.x == 1u) o.a = 1.0; else if (ofs.x == 2u) o.a = 0.0;\n"
"    o_color = o;\n"
"}\n";

char *kvk_glsl_ps_uber(bool flat)
{
    KvkStr b = { 0 };
    ks_add(&b, "%s", kvk_head);
    if (flat) ks_add(&b, "#define KG_FLAT 1\n");
    ks_add(&b, "%s%s%s%s%s", kvk_push, kvk_ps_const, kvk_fs_in, kvk_textures, kvk_ps_uber_body);
    return ks_take(&b);
}

/* ---- internal passes (kelvin_gpu.c kgpu_hlsl_clear, and VR composition) ------------------------------------- */

char *kvk_glsl_fixed(int which)
{
    KvkStr b = { 0 };
    ks_add(&b, "%s%s", kvk_head, kvk_push);
    switch (which) {
    case KVK_FIX_FULLSCREEN_VS:
        ks_add(&b,
            "void main() {\n"
            "    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
            "    gl_Position = vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n");
        break;
    case KVK_FIX_CLEAR_COLOR_PS:
        ks_add(&b, "layout(location = 0) out vec4 o_color;\nvoid main() { o_color = pc.p0; }\n");
        break;
    case KVK_FIX_CLEAR_DEPTH_PS:
        ks_add(&b, "void main() { gl_FragDepth = pc.p1.x; }\n");
        break;
    case KVK_FIX_D2C_PS:
        /* above 2^23 a float holds whole numbers only (see kgpu_hlsl_clear) */
        ks_add(&b,
            "layout(set = 1, binding = 0) uniform sampler2DArray DepthTex;\n"
            "layout(location = 0) out vec4 o_color;\n"
            "void main() {\n"
            "    float s = clamp(texelFetch(DepthTex, ivec3(ivec2(gl_FragCoord.xy), int(gl_ViewIndex)), 0).r, 0.0, 1.0) * 16777215.0;\n"
            "    uint z = s >= 8388608.0 ? uint(s) : uint(s + 0.5);\n"
            "    o_color = vec4(float((z >> 8) & 255u) / 255.0, float(z & 255u) / 255.0, 0.0, float((z >> 16) & 255u) / 255.0);\n"
            "}\n");
        break;
    case KVK_FIX_C2D_PS:
        ks_add(&b,
            "layout(set = 1, binding = 0) uniform sampler2DArray ColorTex;\n"
            "void main() {\n"
            "    vec4 c = texelFetch(ColorTex, ivec3(ivec2(gl_FragCoord.xy), int(gl_ViewIndex)), 0);\n"
            "    uint z = (uint(c.a * 255.0 + 0.5) << 16) | (uint(c.r * 255.0 + 0.5) << 8) | uint(c.g * 255.0 + 0.5);\n"
            "    gl_FragDepth = float(z) / 16777215.0;\n"
            "}\n");
        break;
    case KVK_FIX_QUAD_VS:
        ks_add(&b, "%s", kvk_stereo);
        ks_add(&b,
            "layout(location = 0) out vec2 v_uv;\n"
            "void main() {\n"
            "    vec2 c = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));\n"
            /* a unit quad in its own plane, x right, y up; quad_mvp places it in front of each eye */
            /* p1.x: the eye of a single-view pass, or -1 under multiview (the view's own) */
            "    int e = pc.p1.x < 0.0 ? int(gl_ViewIndex) : int(pc.p1.x);\n"
            "    gl_Position = quad_mvp[e] * vec4(c.x - 0.5, 0.5 - c.y, 0.0, 1.0);\n"
            "    gl_Position.y = -gl_Position.y;\n"
            "    v_uv = c;\n"
            "}\n");
        break;
    case KVK_FIX_QUAD_PS:
        ks_add(&b,
            "layout(set = 1, binding = 0) uniform sampler2DArray Tex0;\n"
            "layout(location = 0) in vec2 v_uv;\n"
            "layout(location = 0) out vec4 o_color;\n"
            "void main() {\n"
            /* p0.x layer; p0.y < 0: opaque (a menu frame), else premultiplied colour scaled by p0.y (the HUD) */
            "    vec4 c = texture(Tex0, vec3(v_uv, pc.p0.x));\n"
            "    o_color = pc.p0.y < 0.0 ? vec4(c.rgb, 1.0) : c * pc.p0.y;\n"
            "}\n");
        break;
    case KVK_FIX_LASER_VS:
        ks_add(&b, "%s", kvk_stereo);
        ks_add(&b,
            "layout(location = 0) out vec2 v_uv;\n"
            "void main() {\n"
            "    vec2 c = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));\n"
            "    int k = int(pc.p1.y);\n"
            "    int e = pc.p1.x < 0.0 ? int(gl_ViewIndex) : int(pc.p1.x);\n"
            /* the beam: x across (-0.5..0.5), y from the muzzle (0) to the hit (1); the dot: centred */
            /* the beam widens toward its end (laser_misc.w: the width there over the width at the muzzle) */
            "    float wx = (c.x - 0.5) * (k == 0 ? mix(1.0, max(laser_misc.w, 1.0), c.y) : 1.0);\n"
            "    gl_Position = laser_mvp[k * 2 + e] * vec4(wx, k == 0 ? c.y : c.y - 0.5, 0.0, 1.0);\n"
            "    gl_Position.y = -gl_Position.y;\n"
            "    v_uv = c;\n"
            "}\n");
        break;
    case KVK_FIX_LASER_PS:
        ks_add(&b,
            "layout(location = 0) in vec2 v_uv;\n"
            "layout(location = 0) out vec4 o_color;\n"
            "void main() {\n"
            "    vec3 red = vec3(1.0, 0.10, 0.06);\n"
            "    float a;\n"
            "    if (pc.p1.y < 0.5) {\n"
            /* a thin hot core in a faint glow, fading in off the muzzle and away down the beam */
            "        float x = (v_uv.x - 0.5) * 2.0;\n"
            "        a = exp(-x * x * 40.0) * 0.8 + exp(-x * x * 5.0) * 0.2;\n"
            "        a *= smoothstep(0.0, 0.02, v_uv.y) * mix(1.0, 0.25, v_uv.y);\n"
            "    } else {\n"
            "        vec2 d = (v_uv - 0.5) * 2.0;\n"
            "        float r2 = dot(d, d);\n"
            "        a = exp(-r2 * 25.0) + exp(-r2 * 5.0) * 0.35;\n"
            "    }\n"
            "    a = clamp(a * pc.p0.x, 0.0, 1.0);\n"
            "    o_color = vec4(mix(red, vec3(1.0, 0.85, 0.8), a * a * 0.5) * a, a);\n"
            "}\n");
        break;
    case KVK_FIX_COPY_PS:
        /* the eye picture: the main target's layer (p0.x, or the view's own when negative) scaled to the target */
        ks_add(&b,
            "layout(set = 1, binding = 0) uniform sampler2DArray Tex0;\n"
            "layout(location = 0) out vec4 o_color;\n"
            "void main() {\n"
            "    float layer = pc.p0.x < 0.0 ? float(gl_ViewIndex) : pc.p0.x;\n"
            "    o_color = vec4(texture(Tex0, vec3(gl_FragCoord.xy * pc.p0.zw, layer)).rgb, 1.0);\n"
            "}\n");
        break;
    default:
        free(b.p);
        return NULL;
    }
    return ks_take(&b);
}

/* ---- position classification ----------------------------------------------------------------------------- */

/* Per component of every register: bit 0 the value depends on vertex input; bit 1 it went through a dot product of
 * input-dependent data with a constant register (a matrix row). */
#define PF_INPUT 1u
#define PF_XFORM 2u

static unsigned pf_operand(const Nv2aVshInput *in, int comp, const unsigned temp[13][4], unsigned a0)
{
    unsigned f = 0;
    int c = in->swizzle[comp] & 3;
    if (in->type == NV2ART_INPUT) f = PF_INPUT;
    else if (in->type == NV2ART_TEMPORARY && in->index <= 12) f = temp[in->index][c];
    else if (in->type == NV2ART_CONTEXT && in->is_relative) f = a0;
    return f;
}

static void pf_op(const Nv2aVshOperation *op, unsigned temp[13][4], unsigned out0[4], unsigned *a0,
                  unsigned res[4])
{
    int nin = 0, k, c;
    bool dot = false, has_const = false;
    switch (op->opcode) {
    case NV2AOP_NOP: return;
    case NV2AOP_MOV: case NV2AOP_ARL: case NV2AOP_RCP: case NV2AOP_RCC: case NV2AOP_RSQ:
    case NV2AOP_EXP: case NV2AOP_LOG: case NV2AOP_LIT: nin = 1; break;
    case NV2AOP_MAD: nin = 3; break;
    case NV2AOP_DP3: case NV2AOP_DP4: case NV2AOP_DPH: nin = 2; dot = true; break;
    default: nin = 2; break;
    }
    for (k = 0; k < nin; k++) if (op->inputs[k].type == NV2ART_CONTEXT) has_const = true;
    for (c = 0; c < 4; c++) {
        unsigned f = 0;
        if (dot || (nin == 1 && op->opcode != NV2AOP_MOV)) {
            /* a dot product (or a scalar unit op) reads several components into each result */
            for (k = 0; k < nin; k++) {
                int q;
                for (q = 0; q < 4; q++) f |= pf_operand(&op->inputs[k], q, temp, *a0);
            }
        } else {
            for (k = 0; k < nin; k++) f |= pf_operand(&op->inputs[k], c, temp, *a0);
        }
        if (dot && has_const && (f & PF_INPUT)) f |= PF_XFORM;
        res[c] = f;
    }
    (void)out0;
}

static void pf_store(const Nv2aVshOperation *op, const unsigned res[4], unsigned temp[13][4], unsigned out0[4],
                     unsigned *a0)
{
    int k, c;
    if (op->opcode == NV2AOP_NOP) return;
    for (k = 0; k < 2; k++) {
        const Nv2aVshOutput *o = &op->outputs[k];
        unsigned *dst = NULL;
        if (o->type == NV2ART_TEMPORARY && o->index <= 12) dst = temp[o->index];
        else if (o->type == NV2ART_OUTPUT && o->index == 0) dst = out0;
        else if (o->type == NV2ART_ADDRESS) { *a0 = res[0]; continue; }
        if (!dst) continue;
        for (c = 0; c < 4; c++) if (o->writemask & (1u << (3 - c))) dst[c] = res[c];
        /* oPos and R12 are one register */
        if (dst == out0) for (c = 0; c < 4; c++) temp[12][c] = out0[c];
        else if (dst == temp[12]) for (c = 0; c < 4; c++) out0[c] = temp[12][c];
    }
}

int kvk_vsh_pos_class(const Nv2aVshProgram *prog)
{
    unsigned temp[13][4], out0[4], a0 = 0, rm[4], ri[4];
    const Nv2aVshStep *st;
    int step = 0;
    if (!prog || !prog->steps) return KVK_POS_PASSTHROUGH;
    memset(temp, 0, sizeof(temp));
    memset(out0, 0, sizeof(out0));
    for (st = prog->steps;; st++, step++) {
        unsigned a0_before = a0;
        memset(rm, 0, sizeof(rm)); memset(ri, 0, sizeof(ri));
        /* both halves read before either writes, as the hardware does */
        pf_op(&st->mac, temp, out0, &a0_before, rm);
        pf_op(&st->ilu, temp, out0, &a0_before, ri);
        pf_store(&st->mac, rm, temp, out0, &a0);
        pf_store(&st->ilu, ri, temp, out0, &a0);
        if (st->is_final || step > 400) break;
    }
    return (out0[3] & PF_XFORM) ? KVK_POS_TRANSFORMED : KVK_POS_PASSTHROUGH;
}

/* ---- the constants a position is made from (VR gun latch) ------------------------------------------------------ */

typedef struct { uint32_t b[6]; } PcSet;

static void pc_or(PcSet *d, const PcSet *s) { int i; for (i = 0; i < 6; i++) d->b[i] |= s->b[i]; }

static void pc_operand(const Nv2aVshInput *in, int comp, PcSet temp[13][4], PcSet *acc)
{
    const int c = in->swizzle[comp] & 3;
    if (in->type == NV2ART_TEMPORARY && in->index <= 12) pc_or(acc, &temp[in->index][c]);
    else if (in->type == NV2ART_CONTEXT && !in->is_relative && in->index < 192) acc->b[in->index >> 5] |= 1u << (in->index & 31);
}

static void pc_op(const Nv2aVshOperation *op, PcSet temp[13][4], PcSet res[4])
{
    int nin, k, c;
    bool all;
    memset(res, 0, 4 * sizeof(PcSet));
    switch (op->opcode) {
    case NV2AOP_NOP: return;
    case NV2AOP_MOV: case NV2AOP_ARL: case NV2AOP_RCP: case NV2AOP_RCC: case NV2AOP_RSQ:
    case NV2AOP_EXP: case NV2AOP_LOG: case NV2AOP_LIT: nin = 1; break;
    case NV2AOP_MAD: nin = 3; break;
    default: nin = 2; break;
    }
    all = op->opcode == NV2AOP_DP3 || op->opcode == NV2AOP_DP4 || op->opcode == NV2AOP_DPH ||
          (nin == 1 && op->opcode != NV2AOP_MOV);
    for (c = 0; c < 4; c++)
        for (k = 0; k < nin; k++) {
            if (all) { int q; for (q = 0; q < 4; q++) pc_operand(&op->inputs[k], q, temp, &res[c]); }
            else pc_operand(&op->inputs[k], c, temp, &res[c]);
        }
}

static void pc_store(const Nv2aVshOperation *op, const PcSet res[4], PcSet temp[13][4], PcSet out0[4])
{
    int k, c;
    if (op->opcode == NV2AOP_NOP) return;
    for (k = 0; k < 2; k++) {
        const Nv2aVshOutput *o = &op->outputs[k];
        PcSet *dst = NULL;
        if (o->type == NV2ART_TEMPORARY && o->index <= 12) dst = temp[o->index];
        else if (o->type == NV2ART_OUTPUT && o->index == 0) dst = out0;
        if (!dst) continue;
        for (c = 0; c < 4; c++) if (o->writemask & (1u << (3 - c))) dst[c] = res[c];
        if (dst == out0) for (c = 0; c < 4; c++) temp[12][c] = out0[c];        /* oPos and R12 are one register */
        else if (dst == temp[12]) for (c = 0; c < 4; c++) out0[c] = temp[12][c];
    }
}

void kvk_vsh_pos_consts(const Nv2aVshProgram *prog, uint32_t out[6])
{
    PcSet temp[13][4], out0[4], rm[4], ri[4];
    const Nv2aVshStep *st;
    int step = 0, i, c;
    memset(out, 0, 6 * sizeof(uint32_t));
    if (!prog || !prog->steps) return;
    memset(temp, 0, sizeof(temp));
    memset(out0, 0, sizeof(out0));
    for (st = prog->steps;; st++, step++) {
        pc_op(&st->mac, temp, rm);          /* both halves read before either writes */
        pc_op(&st->ilu, temp, ri);
        pc_store(&st->mac, rm, temp, out0);
        pc_store(&st->ilu, ri, temp, out0);
        if (st->is_final || step > 400) break;
    }
    /* the union over x, y, z, w: an MVP's rows each feed one component */
    for (i = 0; i < 6; i++) {
        uint32_t m = 0;
        for (c = 0; c < 4; c++) m |= out0[c].b[i];
        out[i] = m;
    }
}
