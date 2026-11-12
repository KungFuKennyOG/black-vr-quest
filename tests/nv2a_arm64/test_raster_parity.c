/*
 * nv2a-raster-parity: Kelvin's triangle setup and multi-threaded rasterizer (kelvin_render.c),
 * arm64 vs x86, white box.
 *
 * kelvin_render.c is compiled into this file, so its own raster_triangle/flush_tris run exactly
 * as a draw runs them: tri_setup (edge functions, depth and perspective planes; plane4 is AVX on
 * the Windows build, scalar/SSE2 here), batching into row bands, kwork_submit_lanes to the raster
 * workers (RECOMP_KELVIN_THREADS, so this is also a threaded stress run of kelvin_workers.c on
 * the target), tri_scan (interp4/derivs4: AVX vs SSE2), kpix_shade8 (AVX2 vs per fragment) and
 * kelvin_texture's samplers. Then kelvin_render_sync() and a byte comparison of the colour and
 * depth surfaces with the dump from the x86-64 AVX2 build.
 *
 * 160 draws of 1-24 random triangles each (partly off-target, perspective w in [0.25, 4],
 * random varyings), each draw with its own random combiner/texture/depth/blend state, all into
 * one 192x128 target, so later draws depth-test and blend against earlier ones: ordering across
 * batches matters, and the result must not depend on the thread count (ctest runs 1 and 4).
 */
#include "parity_common.h"

#include "kelvin_render.c"

#include "kpix_random.h"

#define RW 192
#define RH 128
#define NDRAW 160

static uint8_t s_rcolor[RW * RH * 4], s_rzeta[RW * RH * 4];
static KelvinState s_state;

static void rvert(RVert *r, float *var, const KPixelState *ps)
{
    int k, i;
    memset(r, 0, sizeof(*r));
    r->x = pr_range(-24.0f, RW + 24.0f);
    r->y = pr_range(-24.0f, RH + 24.0f);
    r->lx = r->x;
    r->ly = r->y;
    r->z = pr_range(0.0f, 16777215.0f);
    r->w = pr_n(4) ? pr_range(0.25f, 4.0f) : 1.0f;
    for (k = 0; k < 16; k++) var[k] = kr_varying();
    var[16] = kr_varying();                           /* fog */
    for (i = 0; i < 4; i++) {
        const KTexture *t = &ps->tex[i];
        float sw = t->linear ? (float)t->width : 1.0f, sh = t->linear ? (float)t->height : 1.0f;
        var[17 + 4 * i + 0] = pr_range(-0.5f, 1.5f) * sw;
        var[17 + 4 * i + 1] = pr_range(-0.5f, 1.5f) * sh;
        var[17 + 4 * i + 2] = pr_range(0.0f, 1.0f);
        var[17 + 4 * i + 3] = 1.0f;
    }
    r->var = var;
}

int main(int argc, char **argv)
{
    static float vars[24 * 3][KV_VARYINGS];
    KelvinTarget t;
    DrawCtx c;
    int d, i, n, tris = 0;
    ParitySection sec[2];

    ktex_init();
    pr_seed(0x5EED0003u);
    kr_make_textures();
    for (i = 0; i < RW * RH * 4; i++) s_rcolor[i] = (uint8_t)pr_n(256);
    for (i = 0; i < RW * RH; i++) {
        uint32_t z = 0xFFFFFF00u | 0xFFu;
        memcpy(&s_rzeta[4 * i], &z, 4);
    }
    memset(&t, 0, sizeof(t));
    t.color = s_rcolor; t.color_va = 0x01000000u;
    t.zeta = s_rzeta; t.zeta_va = 0x01800000u;
    t.color_bpp = 4; t.zeta_bpp = 4;
    t.color_pitch = RW * 4; t.zeta_pitch = RW * 4;
    t.width = RW; t.height = RH; t.aa_x = t.aa_y = 1;
    t.clip_x0 = 0; t.clip_y0 = 0; t.clip_x1 = RW; t.clip_y1 = RH;
    t.guest_width = RW; t.guest_height = RH;
    t.guest_color_pitch = RW * 4; t.guest_zeta_pitch = RW * 4;
    t.guest_color_bytes = t.guest_zeta_bytes = (size_t)RW * RH * 4;

    printf("[%s] raster workers: %d\n", PARITY_HOST, kwork_workers());
    for (d = 0; d < NDRAW; d++) {
        memset(&c, 0, sizeof(c));
        c.pg = &s_state;
        c.t = t;
        kr_rnd_ps(&c.ps);
        c.ps.clip_region[0][2] = RW; c.ps.clip_region[0][3] = RH;
        for (i = 0; i < 4; i++) {
            const KTexture *tx = &c.ps.tex[i];
            c.tex_needs_lod[i] = tx->valid && !c.ps.lod_free[i] &&
                                 (tx->levels > 1 || tx->min_filter != tx->mag_filter);
        }
        skip_unread_varyings(&c);
        n = 1 + (int)pr_n(24);
        for (i = 0; i < n; i++) {
            RVert v[3];
            int k;
            for (k = 0; k < 3; k++) rvert(&v[k], vars[3 * i + k], &c.ps);
            raster_triangle(&c, &v[0], &v[1], &v[2], v[0].var, pr_n(4) != 0, 0.0);
        }
        tris += n;
        flush_tris(&c);
    }
    kelvin_render_sync();
    printf("[%s] %d draws, %d triangles, %llu pixels written, %llu batches\n", PARITY_HOST, NDRAW, tris,
           (unsigned long long)s_state.stats.pixels, (unsigned long long)s_state.stats.raster_batches);

    sec[0] = (ParitySection){ "raster colour surface", s_rcolor, sizeof(s_rcolor), 0, 0 };
    sec[1] = (ParitySection){ "raster depth/stencil", s_rzeta, sizeof(s_rzeta), 0, 0 };
    return parity_finish(argc, argv, sec, 2);
}
