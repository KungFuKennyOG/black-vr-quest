/*
 * nv2a-pixel-parity: Kelvin's CPU pixel pipeline (kelvin_pixel.c, kelvin_texture.c), arm64 vs x86.
 *
 * The CPU renderer is what draws when the Vulkan backend is unavailable, and the reference the
 * backend is tested against. Windows (MSVC /arch:AVX2) shades a raster block of up to eight
 * fragments with kpix_shade8's AVX2 path (blend_factor8, ktex_sample_2d8, GATHER8); arm64 has no
 * AVX2 and runs kpix_shade once per fragment (SSE through sse2neon: eval_rgb, blend_factor4,
 * MAXPS/MINPS). The source documents both as bit-identical.
 *
 * 600 random configurations: 1-4 register-combiner stages with random inputs, mappings and
 * outputs, the final combiner, MSB/LSB mux, fog, alpha test, colour keys and alpha kill; up to
 * four textures (swizzled A8R8G8B8/R5G6B5/A1R5G5B5/A4R4G4B4, DXT1/3/5, linear), nearest and
 * linear filters, wrap/clamp/mirror addressing, projective coordinates and LOD from
 * derivatives; depth test/write with every compare; blending with every factor and equation and
 * channel masks. Each configuration shades eight fragments of one row, with random varyings
 * (sometimes outside [0, 1], sometimes non-finite) over a random background.
 *
 * In process: kpix_keep + kpix_shade8 (the raster threads' entry) == kpix_fragment one at a
 * time. Across hosts: the colour and depth surfaces, byte for byte.
 */
#include "parity_common.h"

#include "kelvin_render.h"

#define NCFG 600
#define W 64
#define H ((NCFG + 7) / 8)              /* eight configurations a row, eight pixels each */
#include "kpix_random.h"

static uint8_t s_color[2][W * H * 4], s_zeta[2][W * H * 4];

/* ktex_sample_2d directly: every texture, filter and addressing mode, over coordinates that
 * include +-0, +-inf, NaN, denormals and huge values (the float->int texel addressing). */
#define NSAMP 96
static float s_samples[KR_NTEX][2][9][NSAMP][4];
static int s_sample_self_bad;

static void sample_grid(void)
{
    int i, f, a, n, k;
    for (i = 0; i < KR_NTEX; i++) {
        for (f = 0; f < 2; f++) {
            for (a = 0; a < 9; a++) {
                KTexture t = kr_s_tex[i];
                float s[NSAMP], tc[NSAMP], lod[NSAMP];
                t.addr[0] = 1 + (unsigned)(a % 3);
                t.addr[1] = 1 + (unsigned)(a / 3);
                t.addr[2] = 3;
                if (t.linear) t.addr[0] = t.addr[1] = 3;
                t.mag_filter = t.min_filter = 1 + (unsigned)f;
                for (n = 0; n < NSAMP; n++) {
                    s[n] = n < 32 ? pr_value(96) : pr_range(-2.0f, 3.0f);
                    tc[n] = n % 3 ? pr_range(-2.0f, 3.0f) : pr_value(96);
                    lod[n] = pr_range(-1.0f, 1.0f);
                    ktex_sample_2d(&t, 0, s[n], tc[n], lod[n], s_samples[i][f][a][n]);
                }
#if defined(__AVX2__)
                /* the Windows build's eight-wide sampler, against the scalar one */
                for (n = 0; n + 8 <= NSAMP; n += 8) {
                    float out[4][8];
                    int l;
                    ktex_sample_2d8_lod(&t, &s[n], &tc[n], &lod[n], 0xFFu, out);
                    for (l = 0; l < 8; l++) {
                        for (k = 0; k < 4; k++) {
                            float x = out[k][l], y = s_samples[i][f][a][n + l][k];
                            if (pr_ubits(x) != pr_ubits(y) && !(isnan(x) && isnan(y)) && s_sample_self_bad++ < 5) {
                                printf("  ktex_sample_2d8_lod != ktex_sample_2d: tex %d filt %d addr %d s %08X t %08X\n",
                                       i, f, a, pr_ubits(s[n + l]), pr_ubits(tc[n + l]));
                            }
                        }
                    }
                }
#endif
            }
        }
    }
    (void)k;
}

static void target(KelvinTarget *t, int which)
{
    memset(t, 0, sizeof(*t));
    t->color = s_color[which]; t->color_va = 0x01000000u;
    t->zeta = s_zeta[which]; t->zeta_va = 0x01800000u;
    t->color_bpp = 4; t->zeta_bpp = 4;
    t->color_pitch = W * 4; t->zeta_pitch = W * 4;
    t->width = W; t->height = H; t->aa_x = t->aa_y = 1;
    t->clip_x0 = 0; t->clip_y0 = 0; t->clip_x1 = W; t->clip_y1 = H;
    t->guest_width = W; t->guest_height = H;
    t->guest_color_pitch = W * 4; t->guest_zeta_pitch = W * 4;
    t->guest_color_bytes = t->guest_zeta_bytes = (size_t)W * H * 4;
}

int main(int argc, char **argv)
{
    static KPixelState ps;
    KelvinTarget ta, tb;
    int c, i, written8 = 0, written1 = 0, kept = 0;
    ParitySection sec[3];

    ktex_init();
    pr_seed(0x5EED0002u);
    kr_make_textures();
    for (i = 0; i < W * H * 4; i++) s_color[0][i] = s_color[1][i] = (uint8_t)pr_n(256);
    for (i = 0; i < W * H; i++) {
        uint32_t z = pr_u32() | 0xFFu;                 /* stencil 0xFF */
        memcpy(&s_zeta[0][4 * i], &z, 4);
        memcpy(&s_zeta[1][4 * i], &z, 4);
    }
    target(&ta, 0);
    target(&tb, 1);
    sample_grid();
    printf("[%s] sampler: %d samples%s\n", PARITY_HOST, KR_NTEX * 2 * 9 * NSAMP,
#if defined(__AVX2__)
           s_sample_self_bad ? ", ktex_sample_2d8_lod DIFFERS" : ", ktex_sample_2d8_lod == ktex_sample_2d"
#else
           ""
#endif
    );

    for (c = 0; c < NCFG; c++) {
        KFragment f[8], fa[8], fb;
        float zv[8];
        int n = 0, x0 = (c % 8) * 8, y = c / 8;
        kr_rnd_ps(&ps);
        for (i = 0; i < 8; i++) kr_rnd_fragment(&ps, &f[i], x0 + i, y);
        for (i = 1; i < 8; i++) f[i].front = f[0].front;     /* one triangle */
        /* the raster threads' way: early tests, then up to eight shaded together */
        for (i = 0; i < 8; i++) {
            float z = f[i].z;
            if (kpix_keep(&ps, &ta, f[i].x, f[i].y, &z)) { fa[n] = f[i]; zv[n] = z; n++; }
        }
        kept += n;
        written8 += kpix_shade8(&ps, &ta, fa, zv, n);
        /* one fragment at a time */
        for (i = 0; i < 8; i++) {
            fb = f[i];
            written1 += kpix_fragment(&ps, &tb, &fb);
        }
    }
    printf("[%s] %d configurations x 8 fragments: %d kept, %d written (block) / %d (one at a time)\n",
           PARITY_HOST, NCFG, kept, written8, written1);
    {
        int bad = memcmp(s_color[0], s_color[1], sizeof(s_color[0])) || memcmp(s_zeta[0], s_zeta[1], sizeof(s_zeta[0]))
                  || written8 != written1 || s_sample_self_bad;
        printf("[%s] in-process: kpix_shade8 block %s kpix_fragment one at a time\n", PARITY_HOST,
               bad ? "DIFFERS FROM" : "==");
        for (i = 0; i < W * H && bad; i++) {
            static int shown;
            if ((memcmp(&s_color[0][4 * i], &s_color[1][4 * i], 4) || memcmp(&s_zeta[0][4 * i], &s_zeta[1][4 * i], 4)) && shown++ < 6)
                printf("    pixel %d (config %d lane %d): block %08X/%08X single %08X/%08X\n", i, (i / W) * 8 + (i % W) / 8, i % 8,
                       *(uint32_t *)&s_color[0][4 * i], *(uint32_t *)&s_zeta[0][4 * i], *(uint32_t *)&s_color[1][4 * i], *(uint32_t *)&s_zeta[1][4 * i]);
        }
        sec[0] = (ParitySection){ "colour surface", s_color[0], sizeof(s_color[0]), 0, 0 };
        sec[1] = (ParitySection){ "depth/stencil surface", s_zeta[0], sizeof(s_zeta[0]), 0, 0 };
        sec[2] = (ParitySection){ "texture samples", s_samples, sizeof(s_samples), 1, 0 };
        {
            int rc = parity_finish(argc, argv, sec, 3);
            return bad ? 1 : rc;
        }
    }
}
