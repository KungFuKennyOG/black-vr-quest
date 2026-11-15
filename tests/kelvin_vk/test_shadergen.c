/* kvk-shadergen: the Vulkan backend's GLSL generators (kvk_glsl.c).
 *
 *   1. Every generated shader compiles to SPIR-V (glslang, Vulkan 1.1 rules): random combiner configurations,
 *      random NV2A vertex programs (all opcodes, swizzles, negation, write masks, relative addressing, oPos/R12
 *      aliasing), the ubershaders and the internal passes.
 *   2. Parity with the D3D11 backend: the HLSL generators are extracted from the production .inc files
 *      (extract_d3d.py) and run on the same inputs; after a fixed HLSL->GLSL spelling map, the combiner and vertex
 *      program bodies must be identical text, so the arithmetic and statement order cannot drift. The pixel shader
 *      key must be identical too, and both generators must refuse the same programs.
 *   3. Position classification (VR eye transform) on hand-written programs. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvk_glsl.h"
#include "kvk_spirv.h"
#include "nv2a_vsh_cpu/nv2a_vsh_disassembler.h"

/* what the extracted D3D11 code expects from kelvin_gpu.c */
#define _vscprintf(fmt, ap) vsnprintf(NULL, 0, fmt, ap)
#include "d3d_generators.inc"

static int s_fail, s_checks;
#define CHECK(c, ...) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rnd(uint32_t n) { return (uint32_t)rand() % n; }
static float rndf(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static bool compile(int stage, const char *src, const char *what)
{
    KvkSpirv s;
    bool ok = kvk_spirv_compile(stage, src, &s);
    if (!ok) {
        printf("---- %s did not compile:\n%s\n---- source:\n%s\n", what, s.log ? s.log : "", src);
    }
    kvk_spirv_free(&s);
    return ok;
}

/* ---- text helpers ---- */

static char *replace_all(char *s, const char *from, const char *to)
{
    size_t lf = strlen(from), lt = strlen(to), n = 0;
    const char *p;
    char *out, *o;
    for (p = s; (p = strstr(p, from)) != NULL; p += lf) n++;
    if (!n) return s;
    out = (char *)malloc(strlen(s) + n * (lt > lf ? lt - lf : 0) + 1);
    for (p = s, o = out; ; ) {
        const char *q = strstr(p, from);
        if (!q) { strcpy(o, p); break; }
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        memcpy(o, to, lt); o += lt;
        p = q + lf;
    }
    free(s);
    return out;
}

static char *dup_range(const char *s, const char *from, const char *to)
{
    const char *a = strstr(s, from), *b;
    char *r;
    if (!a) return NULL;
    b = to ? strstr(a, to) : a + strlen(a);
    if (!b) return NULL;
    r = (char *)malloc((size_t)(b - a) + 1);
    memcpy(r, a, (size_t)(b - a));
    r[b - a] = 0;
    return r;
}

/* ---- combiner configurations ---- */

static uint32_t rnd_input(void)
{
    static const uint32_t regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15 };
    return regs[rnd(14)] | (rnd(2) ? 0x10u : 0) | (rnd(8) << 5);
}

static uint32_t rnd_dest(void)
{
    static const uint32_t regs[] = { 0, 0, 4, 5, 8, 9, 10, 11, 12, 13 };
    return regs[rnd(10)];
}

static void rnd_config(PSConst *c)
{
    unsigned int i, k;
    memset(c, 0, sizeof(*c));
    c->gen[0] = 1 + rnd(8);
    c->gen[1] = rnd(3) ? 7 : rnd(8);
    c->gen[2] = rnd(256);
    c->gen[3] = rnd(4) == 0;
    c->ofs[0] = rnd(3);
    for (i = 0; i < 8; i++) {
        for (k = 0; k < 4; k++) { c->stage[i].rgb_in[k] = rnd_input(); c->stage[i].alpha_in[k] = rnd_input(); }
        c->stage[i].outs[0] = rnd_dest() | (rnd_dest() << 4) | (rnd_dest() << 8) | (rnd(256) << 12);
        c->stage[i].outs[1] = rnd_dest() | (rnd_dest() << 4) | (rnd_dest() << 8) | ((rnd(256) & 0x3C) << 12);
        for (k = 0; k < 4; k++) { c->stage[i].c0[k] = rndf(); c->stage[i].c1[k] = rndf(); }
    }
    for (k = 0; k < 4; k++) c->fin_abcd[k] = rnd_input();
    c->fin_efg[0] = rnd_input(); c->fin_efg[1] = rnd_input(); c->fin_efg[2] = rnd_input();
    c->fin_efg[3] = rnd(128);
    for (k = 0; k < 4; k++) {
        c->texmode[k] = rnd(3) ? 1 : 0;
        c->texflags[k] = (rnd(4) ? 1u : 0u) | (rnd(4) == 0 ? 2u : 0u) | (rnd(4) == 0 ? 4u : 0u) | (rnd(4) << 4);
        c->ckey[k] = (uint32_t)rand();
        c->ckeymask[k] = (uint32_t)rand();
    }
}

/* The D3D11 HLSL of a specialised combiner shader, respelled as the GLSL generator spells it. */
static char *hlsl_ps_as_glsl(const PSConst *c)
{
    char *h = ps_generate(c), *s;
    int i;
    s = dup_range(h, "float4 kg_combine(", NULL);
    free(h);
    if (!s) return NULL;
    s = replace_all(s, "float4 kg_combine(VSOut v, float4 t0, float4 t1, float4 t2, float4 t3)", "vec4 kg_combine(vec4 t0, vec4 t1, vec4 t2, vec4 t3)");
    s = replace_all(s, "float4 ps_main(VSOut v) : SV_Target {", "void main() {");
    s = replace_all(s, "    return o;\n}", "    o_color = o;\n}");
    s = replace_all(s, "kg_combine(v, t0, t1, t2, t3)", "kg_combine(t0, t1, t2, t3)");
    s = replace_all(s, "saturate(v.misc.x)", "clamp(v.misc.x, 0.0, 1.0)");
    s = replace_all(s, "v.misc", "v_misc");
    s = replace_all(s, "v.d0", "v_d0");
    s = replace_all(s, "v.d1", "v_d1");
    for (i = 0; i < 4; i++) {
        char a[64], b[96];
        snprintf(a, sizeof(a), "v.t%d", i); snprintf(b, sizeof(b), "v_t%d", i); s = replace_all(s, a, b);
        snprintf(a, sizeof(a), "Tex%d.Sample(Smp%d, uv%d)", i, i, i);
        snprintf(b, sizeof(b), "texture(Tex%d, vec3(uv%d, kvk_layer(%du)))", i, i, i);
        s = replace_all(s, a, b);
        snprintf(a, sizeof(a), "(uint4)(saturate(t%d) * 255.0 + 0.5)", i);
        snprintf(b, sizeof(b), "uvec4(clamp(t%d, 0.0, 1.0) * 255.0 + 0.5)", i);
        s = replace_all(s, a, b);
    }
    s = replace_all(s, "dot(ra0, ra1).xxx", "vec3(dot(ra0, ra1))");
    s = replace_all(s, "dot(ra2, ra3).xxx", "vec3(dot(ra2, ra3))");
    s = replace_all(s, "((((uint)(r12.a * 255.0)) & 1u) == 1u)", "((uint(max(r12.a * 255.0, 0.0)) & 1u) == 1u)");
    s = replace_all(s, "(int)floor(frag.a * 255.0 + 0.5)", "int(floor(frag.a * 255.0 + 0.5))");
    s = replace_all(s, "(int)gen.z", "int(gen.z)");
    s = replace_all(s, "saturate(frag)", "clamp(frag, 0.0, 1.0)");
    s = replace_all(s, "uint4 ", "uvec4 ");
    s = replace_all(s, "float4", "vec4");
    s = replace_all(s, "float3", "vec3");
    s = replace_all(s, "float2", "vec2");
    return s;
}

static void test_pixel_shaders(void)
{
    int n, mismatches = 0;
    for (n = 0; n < 400; n++) {
        PSConst c;
        char *g, *gpart, *h, name[64];
        rnd_config(&c);
        CHECK(kvk_ps_key(&c) == ps_key_of(&c), "pixel shader key differs from the D3D11 backend's");
        g = kvk_glsl_ps_spec(&c);
        CHECK(g != NULL, "ps_spec NULL");
        if (!g) continue;
        snprintf(name, sizeof(name), "combiner config %d", n);
        CHECK(compile(KVK_STAGE_FRAGMENT, g, name), "%s compiles", name);
        gpart = dup_range(g, "vec4 kg_combine(", NULL);
        h = hlsl_ps_as_glsl(&c);
        CHECK(gpart && h && strcmp(gpart, h) == 0, "config %d: GLSL combiner differs from the D3D11 HLSL", n);
        if (gpart && h && strcmp(gpart, h) && mismatches++ < 1) {
            size_t i = 0;
            while (gpart[i] && gpart[i] == h[i]) i++;
            printf("  first difference at %zu:\n  glsl: %.160s\n  hlsl: %.160s\n", i, gpart + (i > 40 ? i - 40 : 0), h + (i > 40 ? i - 40 : 0));
        }
        free(gpart); free(h); free(g);
    }
    {
        char *u = kvk_glsl_ps_uber(false), *f = kvk_glsl_ps_uber(true), *vc = kvk_glsl_vs_cpu();
        int i;
        CHECK(u && compile(KVK_STAGE_FRAGMENT, u, "ubershader"), "ubershader");
        CHECK(f && compile(KVK_STAGE_FRAGMENT, f, "flat ubershader"), "flat ubershader");
        CHECK(vc && compile(KVK_STAGE_VERTEX, vc, "cpu vertices"), "cpu vertex shader");
        free(u); free(f); free(vc);
        for (i = 0; i < KVK_FIX_COUNT; i++) {
            char *s = kvk_glsl_fixed(i), name[32];
            int st = i == KVK_FIX_FULLSCREEN_VS || i == KVK_FIX_QUAD_VS || i == KVK_FIX_LASER_VS ? KVK_STAGE_VERTEX
                                                                                         : KVK_STAGE_FRAGMENT;
            snprintf(name, sizeof(name), "fixed %d", i);
            CHECK(s && compile(st, s, name), "%s", name);
            free(s);
        }
    }
}

/* ---- vertex programs ---- */

static void rnd_input_operand(Nv2aVshInput *in, bool allow_rel)
{
    int k;
    switch (rnd(3)) {
    case 0: in->type = NV2ART_TEMPORARY; in->index = rnd(13); break;        /* 12 = R12 (oPos) */
    case 1: in->type = NV2ART_INPUT; in->index = rnd(16); break;
    default:
        in->type = NV2ART_CONTEXT; in->index = rnd(192);
        in->is_relative = allow_rel && rnd(4) == 0;
        break;
    }
    for (k = 0; k < 4; k++) in->swizzle[k] = (uint8_t)rnd(4);
    in->is_negated = rnd(4) == 0;
}

static void rnd_output(Nv2aVshOutput *o, bool allow_const)
{
    static const unsigned outs[] = { 0, 3, 4, 5, 9, 10, 11, 12 };
    o->writemask = (Nv2aVshWritemask)(1 + rnd(15));
    switch (rnd(allow_const ? 30 : 3)) {
    case 0: o->type = NV2ART_OUTPUT; o->index = outs[rnd(8)]; break;
    case 1: case 2: o->type = NV2ART_TEMPORARY; o->index = rnd(12); break;
    default:
        if (allow_const && rnd(10) == 0) { o->type = NV2ART_CONTEXT; o->index = rnd(192); }
        else { o->type = NV2ART_TEMPORARY; o->index = rnd(12); }
        break;
    }
}

static int nin_of(Nv2aVshOpcode op)
{
    switch (op) {
    case NV2AOP_MOV: case NV2AOP_ARL: case NV2AOP_RCP: case NV2AOP_RCC: case NV2AOP_RSQ:
    case NV2AOP_EXP: case NV2AOP_LOG: case NV2AOP_LIT: return 1;
    case NV2AOP_MAD: return 3;
    case NV2AOP_NOP: return 0;
    default: return 2;
    }
}

static Nv2aVshStep *rnd_program(int nsteps, bool allow_const)
{
    static const Nv2aVshOpcode mac[] = { NV2AOP_MOV, NV2AOP_MUL, NV2AOP_ADD, NV2AOP_MAD, NV2AOP_DP3, NV2AOP_DPH,
                                         NV2AOP_DP4, NV2AOP_DST, NV2AOP_MIN, NV2AOP_MAX, NV2AOP_SLT, NV2AOP_SGE,
                                         NV2AOP_ARL, NV2AOP_NOP };
    static const Nv2aVshOpcode ilu[] = { NV2AOP_RCP, NV2AOP_RCC, NV2AOP_RSQ, NV2AOP_EXP, NV2AOP_LOG, NV2AOP_LIT, NV2AOP_NOP };
    Nv2aVshStep *st = (Nv2aVshStep *)calloc((size_t)nsteps, sizeof(*st));
    int i, k;
    bool have_a0 = false;
    for (i = 0; i < nsteps; i++) {
        Nv2aVshOperation *m = &st[i].mac, *u = &st[i].ilu;
        m->opcode = mac[rnd(14)];
        for (k = 0; k < nin_of(m->opcode); k++) rnd_input_operand(&m->inputs[k], have_a0);
        if (m->opcode == NV2AOP_ARL) { m->outputs[0].type = NV2ART_ADDRESS; have_a0 = true; }
        else if (m->opcode != NV2AOP_NOP) {
            rnd_output(&m->outputs[0], allow_const);
            if (rnd(4) == 0) rnd_output(&m->outputs[1], false);
        }
        u->opcode = ilu[rnd(7)];
        if (u->opcode != NV2AOP_NOP) {
            rnd_input_operand(&u->inputs[0], have_a0);
            rnd_output(&u->outputs[0], false);
        }
    }
    /* a position, so the program is a plausible transform */
    st[nsteps - 1].mac.opcode = NV2AOP_DP4;
    memset(&st[nsteps - 1].mac.inputs, 0, sizeof(st[nsteps - 1].mac.inputs));
    rnd_input_operand(&st[nsteps - 1].mac.inputs[0], false);
    st[nsteps - 1].mac.inputs[1].type = NV2ART_CONTEXT; st[nsteps - 1].mac.inputs[1].index = 99;
    for (k = 0; k < 4; k++) st[nsteps - 1].mac.inputs[1].swizzle[k] = (uint8_t)k;
    st[nsteps - 1].mac.outputs[0].type = NV2ART_OUTPUT; st[nsteps - 1].mac.outputs[0].index = 0;
    st[nsteps - 1].mac.outputs[0].writemask = NV2AWM_XYZW;
    st[nsteps - 1].is_final = true;
    return st;
}

static bool has_const_write(const Nv2aVshStep *st, int n)
{
    int i, k;
    for (i = 0; i < n; i++)
        for (k = 0; k < 2; k++) if (st[i].mac.outputs[k].type == NV2ART_CONTEXT && st[i].mac.opcode != NV2AOP_NOP) return true;
    return false;
}

/* The D3D11 HLSL of a vertex program's body, respelled as the GLSL generator spells it. */
static char *hlsl_vs_body_as_glsl(const Nv2aVshProgram *prog)
{
    char *h = vsh_generate(prog), *s;
    if (!h) return NULL;
    s = dup_range(h, "    float a0 = 0.0;\n", "    float4 pp = o0;");
    free(h);
    if (!s) return NULL;
    s = replace_all(s, "(int)a0", "int(a0)");
    {
        /* C[clamp((int)N + ...  ->  C[clamp(int(N) + ... */
        char *p;
        while ((p = strstr(s, "clamp((int)")) != NULL) {
            char num[16], from[48], to[48];
            int len = 0;
            const char *d = p + strlen("clamp((int)");
            while (d[len] >= '0' && d[len] <= '9' && len < 15) { num[len] = d[len]; len++; }
            num[len] = 0;
            snprintf(from, sizeof(from), "clamp((int)%s ", num);
            snprintf(to, sizeof(to), "clamp(int(%s) ", num);
            s = replace_all(s, from, to);
        }
    }
    {
        /* ARL: floor(X).xxxx -> vec4(floor(X)) */
        const char *tags[2] = { "m", "i" };
        int t;
        for (t = 0; t < 2; t++) {
            char from[64], to[64];
            snprintf(from, sizeof(from), "= floor(%s_i0.x + 0.001).xxxx;", tags[t]);
            snprintf(to, sizeof(to), "= vec4(floor(%s_i0.x + 0.001));", tags[t]);
            s = replace_all(s, from, to);
        }
    }
    s = replace_all(s, "float4", "vec4");
    return s;
}

static void test_vertex_programs(void)
{
    int n, refused = 0, mismatches = 0;
    for (n = 0; n < 300; n++) {
        const int nsteps = 1 + (int)rnd(40);
        Nv2aVshStep *st = rnd_program(nsteps, n % 3 == 0);
        Nv2aVshProgram prog;
        char *g, *gbody, *h, name[64];
        bool want_null = has_const_write(st, nsteps);
        prog.steps = st;
        g = kvk_glsl_vs_program(&prog);
        h = hlsl_vs_body_as_glsl(&prog);
        CHECK((g == NULL) == want_null, "program %d: GLSL refusal %d, constant write %d", n, g == NULL, want_null);
        CHECK((h == NULL) == (g == NULL), "program %d: GLSL and HLSL generators disagree on support", n);
        if (!g) { refused++; free(h); free(st); continue; }
        snprintf(name, sizeof(name), "vertex program %d", n);
        CHECK(compile(KVK_STAGE_VERTEX, g, name), "%s compiles", name);
        gbody = dup_range(g, "    float a0 = 0.0;\n", "    vec4 pp = o0;");
        CHECK(gbody && h && strcmp(gbody, h) == 0, "program %d: GLSL body differs from the D3D11 HLSL", n);
        if (gbody && h && strcmp(gbody, h) && mismatches++ < 1) {
            size_t i = 0;
            while (gbody[i] && gbody[i] == h[i]) i++;
            printf("  first difference at %zu:\n  glsl: %.160s\n  hlsl: %.160s\n", i, gbody + (i > 40 ? i - 40 : 0), h + (i > 40 ? i - 40 : 0));
        }
        free(gbody); free(h); free(g); free(st);
    }
    CHECK(refused > 0, "some programs with constant writes were generated");
}

/* ---- position classification ---- */

static void op(Nv2aVshOperation *o, Nv2aVshOpcode code, Nv2aVshRegisterType ot, unsigned oi, Nv2aVshWritemask wm)
{
    int k;
    memset(o, 0, sizeof(*o));
    o->opcode = code;
    o->outputs[0].type = ot; o->outputs[0].index = oi; o->outputs[0].writemask = wm;
    for (k = 0; k < 3; k++) { int q; for (q = 0; q < 4; q++) o->inputs[k].swizzle[q] = (uint8_t)q; }
}

static void in(Nv2aVshOperation *o, int k, Nv2aVshRegisterType t, unsigned idx)
{
    o->inputs[k].type = t;
    o->inputs[k].index = idx;
}

static void test_pos_class(void)
{
    Nv2aVshStep st[8];
    Nv2aVshProgram p = { st };
    int k;
    /* the Xbox D3D transform: dp4 r0 = v0 . c[96..99]; mul r1 = r0 * c58; rcc r2.x = r0.w; mad oPos.xyz; mov oPos.w */
    memset(st, 0, sizeof(st));
    for (k = 0; k < 4; k++) {
        op(&st[k].mac, NV2AOP_DP4, NV2ART_TEMPORARY, 0, (Nv2aVshWritemask)(8 >> k));
        in(&st[k].mac, 0, NV2ART_INPUT, 0); in(&st[k].mac, 1, NV2ART_CONTEXT, 96 + k);
    }
    op(&st[4].mac, NV2AOP_MUL, NV2ART_TEMPORARY, 1, NV2AWM_XYZ);
    in(&st[4].mac, 0, NV2ART_TEMPORARY, 0); in(&st[4].mac, 1, NV2ART_CONTEXT, 58);
    op(&st[4].ilu, NV2AOP_RCC, NV2ART_TEMPORARY, 2, NV2AWM_X);
    in(&st[4].ilu, 0, NV2ART_TEMPORARY, 0);
    for (k = 0; k < 4; k++) st[4].ilu.inputs[0].swizzle[k] = 3;
    op(&st[5].mac, NV2AOP_MAD, NV2ART_OUTPUT, 0, NV2AWM_XYZ);
    in(&st[5].mac, 0, NV2ART_TEMPORARY, 1); in(&st[5].mac, 1, NV2ART_TEMPORARY, 2); in(&st[5].mac, 2, NV2ART_CONTEXT, 59);
    for (k = 0; k < 4; k++) st[5].mac.inputs[1].swizzle[k] = 0;
    op(&st[6].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_W);
    in(&st[6].mac, 0, NV2ART_TEMPORARY, 0);
    st[6].is_final = true;
    CHECK(kvk_vsh_pos_class(&p) == KVK_POS_TRANSFORMED, "Xbox D3D transform is TRANSFORMED");
    /* skinned: dp4 against a0-relative bone rows */
    st[0].mac.inputs[1].is_relative = true;
    CHECK(kvk_vsh_pos_class(&p) == KVK_POS_TRANSFORMED, "skinned transform is TRANSFORMED");
    /* pre-transformed screen positions: mov oPos, v0 */
    memset(st, 0, sizeof(st));
    op(&st[0].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_XYZW);
    in(&st[0].mac, 0, NV2ART_INPUT, 0);
    st[0].is_final = true;
    CHECK(kvk_vsh_pos_class(&p) == KVK_POS_PASSTHROUGH, "mov oPos, v0 is PASSTHROUGH");
    /* scaled screen positions: mad oPos = v0 * c[a] + c[b] */
    op(&st[0].mac, NV2AOP_MAD, NV2ART_OUTPUT, 0, NV2AWM_XYZW);
    in(&st[0].mac, 0, NV2ART_INPUT, 0); in(&st[0].mac, 1, NV2ART_CONTEXT, 10); in(&st[0].mac, 2, NV2ART_CONTEXT, 11);
    CHECK(kvk_vsh_pos_class(&p) == KVK_POS_PASSTHROUGH, "mad oPos, v0, c, c is PASSTHROUGH");
    /* a dot product of constants only (no vertex input) */
    op(&st[0].mac, NV2AOP_DP4, NV2ART_OUTPUT, 0, NV2AWM_XYZW);
    in(&st[0].mac, 0, NV2ART_CONTEXT, 3); in(&st[0].mac, 1, NV2ART_CONTEXT, 4);
    CHECK(kvk_vsh_pos_class(&p) == KVK_POS_PASSTHROUGH, "constant dot is PASSTHROUGH");
    /* the transform writes only xyz of oPos and w comes from v0: not perspective */
    memset(st, 0, sizeof(st));
    for (k = 0; k < 3; k++) {
        op(&st[k].mac, NV2AOP_DP4, NV2ART_OUTPUT, 0, (Nv2aVshWritemask)(8 >> k));
        in(&st[k].mac, 0, NV2ART_INPUT, 0); in(&st[k].mac, 1, NV2ART_CONTEXT, 96 + k);
    }
    op(&st[3].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_W);
    in(&st[3].mac, 0, NV2ART_INPUT, 0);
    st[3].is_final = true;
    CHECK(kvk_vsh_pos_class(&p) == KVK_POS_PASSTHROUGH, "w copied from the input is PASSTHROUGH");
}

int main(void)
{
    srand(42);
    if (!kvk_spirv_init()) { printf("glslang did not initialise\n"); return 1; }
    test_pixel_shaders();
    test_vertex_programs();
    test_pos_class();
    kvk_spirv_shutdown();
    printf("kvk-shadergen: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
