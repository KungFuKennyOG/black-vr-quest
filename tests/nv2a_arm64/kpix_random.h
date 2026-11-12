/*
 * Random Kelvin pixel states, textures and fragments for the NV2A parity tests
 * (deterministic: parity_common.h's generator). Adapted from tests/kelvin_vk/test_backend.c.
 */
#ifndef KPIX_RANDOM_H
#define KPIX_RANDOM_H

#include "parity_common.h"
#include "kelvin_render.h"

#define KR_NTEX 12

static KTexture kr_s_tex[KR_NTEX];
static uint8_t kr_s_texmem[KR_NTEX][4096];

static float kr_u8f(void) { return (float)pr_n(256) / 255.0f; }

static void kr_swizzle_masks(unsigned int width, unsigned int height, uint32_t *mx, uint32_t *my)
{
    uint32_t x = 0, y = 0, bit = 1, mask_bit = 1;
    int done;
    do {
        done = 1;
        if (bit < width) { x |= mask_bit; mask_bit <<= 1; done = 0; }
        if (bit < height) { y |= mask_bit; mask_bit <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mx = x; *my = y;
}

static void kr_make_textures(void)
{
    static const struct { int kind, bpp, w, h, linear; } kinds[KR_NTEX] = {
        { TK_8888, 4, 4, 4, 0 }, { TK_8888, 4, 16, 8, 0 }, { TK_8888, 4, 8, 8, 0 },
        { TK_565, 2, 8, 8, 0 }, { TK_1555, 2, 8, 16, 0 }, { TK_4444, 2, 16, 16, 0 },
        { TK_X888, 4, 8, 4, 0 }, { TK_DXT1, 0, 8, 8, 0 }, { TK_DXT3, 0, 8, 8, 0 },
        { TK_DXT5, 0, 16, 8, 0 }, { TK_8888, 4, 8, 8, 1 }, { TK_565, 2, 16, 4, 1 },
    };
    int i, k;
    for (i = 0; i < KR_NTEX; i++) {
        KTexture *t = &kr_s_tex[i];
        memset(t, 0, sizeof(*t));
        for (k = 0; k < 4096; k++) kr_s_texmem[i][k] = (uint8_t)pr_n(256);
        if (i == 2) for (k = 3; k < 256; k += 12) kr_s_texmem[i][k] = 0;   /* alpha-kill texels */
        t->valid = true;
        t->kind = (unsigned)kinds[i].kind;
        t->bpp = (unsigned)kinds[i].bpp;
        t->width = (unsigned)kinds[i].w;
        t->height = (unsigned)kinds[i].h;
        t->linear = kinds[i].linear != 0;
        t->pitch = t->linear ? t->width * t->bpp : 0;
        t->base = 0x00100000u + (uint32_t)i * 0x1000u;
        t->mem = kr_s_texmem[i];
        t->dimensionality = 2; t->depth = 1; t->levels = 1;
        t->level_w[0] = t->width; t->level_h[0] = t->height;
        kr_swizzle_masks(t->width, t->height, &t->mask_x[0], &t->mask_y[0]);
    }
}

static void kr_parse_in(KCombinerInput *in, uint32_t v) { in->reg = (int)(v & 0xF); in->chan = (int)(v & 0x10); in->mod = (int)(v & 0xE0); }
static void kr_parse_out(KCombinerOutput *o, uint32_t v)
{
    int flags = (int)(v >> 12);
    o->cd = (int)(v & 0xF); o->ab = (int)((v >> 4) & 0xF); o->muxsum = (int)((v >> 8) & 0xF); o->flags = flags;
    o->cd_op = flags & 1; o->ab_op = flags & 2; o->muxsum_op = flags & 4; o->mapping = flags & 0x38;
}
static uint32_t kr_rnd_in_stage(void)
{
    static const uint32_t regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13 };
    uint32_t r = regs[pr_n(12)];
    r |= pr_n(2) ? 0x10u : 0;
    return r | (pr_n(8) << 5);
}
static uint32_t kr_rnd_dest(void)
{
    static const uint32_t regs[] = { 0, 0, 4, 5, 8, 9, 12, 12, 13 };
    return regs[pr_n(9)];
}

static void kr_rnd_ps(KPixelState *ps)
{
    static const int factors[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15 };
    int i, k;
    memset(ps, 0, sizeof(*ps));
    ps->color_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    ps->zeta_format = NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
    ps->smooth = true;
    ps->zmax = 16777215.0f;
    ps->zclip_min = 0.0f; ps->zclip_max = 16777215.0f;
    ps->depth_clip_cull = true;
    ps->alpha_func = 7;
    ps->clip_region[0][2] = 4096; ps->clip_region[0][3] = 4096;
    ps->stencil_mask_read = ps->stencil_mask_write = 0xFF;

    ps->num_stages = 1 + (int)pr_n(4);
    ps->flags = (pr_n(2) ? PS_COMBINERCOUNT_MUX_MSB : 0);
    ps->flags |= (pr_n(2) ? PS_COMBINERCOUNT_UNIQUE_C0 : 0);
    ps->flags |= (pr_n(2) ? PS_COMBINERCOUNT_UNIQUE_C1 : 0);
    for (i = 0; i < 8; i++) {
        uint32_t o;
        for (k = 0; k < 4; k++) { kr_parse_in(&ps->stage[i].rgb[k], kr_rnd_in_stage()); kr_parse_in(&ps->stage[i].alpha[k], kr_rnd_in_stage()); }
        o = kr_rnd_dest(); o |= kr_rnd_dest() << 4; o |= kr_rnd_dest() << 8; o |= pr_n(256) << 12;
        kr_parse_out(&ps->stage[i].rgb_out, o);
        o = kr_rnd_dest(); o |= kr_rnd_dest() << 4; o |= kr_rnd_dest() << 8; o |= (pr_n(256) & 0x3C) << 12;
        kr_parse_out(&ps->stage[i].alpha_out, o);
        for (k = 0; k < 4; k++) { ps->stage[i].c0[k] = kr_u8f(); ps->stage[i].c1[k] = kr_u8f(); }
    }
    ps->final_enabled = pr_n(4) != 0;
    if (ps->final_enabled) {
        static const uint32_t regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 14, 15 };
        for (k = 0; k < 7; k++) {
            /* E and F (fin[4], fin[5]) may not read EF_PROD or V1R0_SUM, which are made from them;
             * kpix_shade would read Regs.ef uninitialised (hardware: invalid). */
            uint32_t v = regs[pr_n(k == 4 || k == 5 ? 10 : 12)];
            v |= pr_n(2) ? 0x10u : 0;
            v |= pr_n(2) ? 0x20u : 0;
            kr_parse_in(&ps->fin[k], v);
        }
        ps->clamp_sum = pr_n(2); ps->inv_v1 = pr_n(2); ps->inv_r0 = pr_n(2);
        for (k = 0; k < 4; k++) { ps->final_c0[k] = kr_u8f(); ps->final_c1[k] = kr_u8f(); }
    }
    for (k = 0; k < 3; k++) ps->fog_color[k] = kr_u8f();
    if (pr_n(4) == 0) { ps->alpha_test = true; ps->alpha_func = (int)pr_n(8); ps->alpha_ref = (int)pr_n(256); }

    for (i = 0; i < 4; i++) {
        KTexture *t = &ps->tex[i];
        if (pr_n(3) == 0) continue;
        ps->tex_mode[i] = PS_TEXTUREMODES_PROJECT2D;
        *t = kr_s_tex[pr_n(KR_NTEX)];
        t->addr[0] = 1 + pr_n(3);                      /* wrap, mirror, clamp */
        t->addr[1] = 1 + pr_n(3);
        t->addr[2] = 3;
        if (t->linear) t->addr[0] = t->addr[1] = 3;
        t->mag_filter = 1 + pr_n(2);
        t->min_filter = 1 + pr_n(2);                   /* BOX_LOD0, TENT_LOD0 */
        if (pr_n(5) == 0) t->alphakill = true;
        if (pr_n(6) == 0) {
            t->colorkey_mode = 1 + pr_n(3);
            ps->color_key_mask[i] = 0x00F0F0F0u;
            ps->color_key[i] = pr_n(2) ? 0 : pr_u32();
        }
        ps->lod_free[i] = ktex_lod_free(t);
        ps->rect_tex[i] = t->linear;
    }

    if (pr_n(2)) {
        ps->depth_test = true;
        ps->depth_write = pr_n(2);
        ps->depth_func = (int)pr_n(8);
    }
    if (pr_n(2)) {
        ps->blend = true;
        ps->blend_sfactor = factors[pr_n(15)];
        ps->blend_dfactor = factors[pr_n(15)];
        ps->blend_eqn = (int)pr_n(7);
        for (k = 0; k < 4; k++) ps->blend_color[k] = kr_u8f();
    }
    ps->mask_r = pr_n(8) != 0; ps->mask_g = pr_n(8) != 0;
    ps->mask_b = pr_n(8) != 0; ps->mask_a = pr_n(8) != 0;
}

static float kr_varying(void)
{
    switch (pr_n(16)) {
    case 0: {                                        /* anything, edges included -- */
        /* but quiet NaNs only: a fragment's varyings are interpolated (arithmetic quiets a
         * signalling NaN) and its colours were clamped (clamp_color), so kpix never sees an
         * sNaN. It matters: clang folds "x > 0 ? x : 0" / MAXPS(x, 0) into FMAXNM, which
         * differs from x86 only for an sNaN x (port/notes/nv2a-arm64.md). */
        float v = pr_value(64);
        if (isnan(v)) v = pr_bits(pr_ubits(v) | 0x00400000u);
        return v;
    }
    case 1: return pr_range(-0.25f, 1.25f);
    default: return kr_u8f();
    }
}

static void kr_rnd_fragment(const KPixelState *ps, KFragment *f, int x, int y)
{
    int i, k;
    memset(f, 0, sizeof(*f));
    for (k = 0; k < 4; k++) { f->d0[k] = kr_varying(); f->d1[k] = kr_varying(); f->b0[k] = kr_varying(); f->b1[k] = kr_varying(); }
    f->fog = kr_varying();
    for (i = 0; i < 4; i++) {
        const KTexture *t = &ps->tex[i];
        float q = pr_n(4) ? 1.0f : pr_range(0.25f, 4.0f);
        float sw = t->linear ? (float)t->width : 1.0f, sh = t->linear ? (float)t->height : 1.0f;
        f->t[i][0] = pr_range(-0.5f, 1.5f) * sw * q;
        f->t[i][1] = pr_range(-0.5f, 1.5f) * sh * q;
        f->t[i][2] = pr_range(0.0f, 1.0f);
        f->t[i][3] = q;
        if (pr_n(64) == 0) {                          /* a wild coordinate (sequenced: */
            int which = (int)pr_n(2);                 /* the index and the value must be */
            f->t[i][which] = pr_value(255);           /* drawn in the same order on every compiler) */
        }
        for (k = 0; k < 4; k++) {
            f->dtdx[i][k] = pr_range(-0.3f, 0.3f) * sw;
            f->dtdy[i][k] = pr_range(-0.3f, 0.3f) * sh;
        }
    }
    f->z = pr_n(16) ? pr_range(0.0f, 16777215.0f) : pr_value(128);
    f->x = x; f->y = y; f->front = pr_n(4) != 0;
}

#endif
