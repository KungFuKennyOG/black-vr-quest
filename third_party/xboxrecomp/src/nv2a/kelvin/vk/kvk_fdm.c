/* kvk_fdm.c - the fragment density map of the Kelvin Vulkan backend, as pure functions (kvk_fdm.h). */
#include <math.h>
#include <string.h>

#include "kvk_fdm.h"

uint32_t kvk_fdm_dim(uint32_t pixels, uint32_t texel)
{
    return texel ? (pixels + texel - 1u) / texel : 0u;
}

int kvk_fdm_centre(float *cx, float *cy, float tl, float tr, float tu, float td)
{
    float x, y;
    *cx = 0.5f; *cy = 0.5f;
    if (!isfinite(tl) || !isfinite(tr) || !isfinite(tu) || !isfinite(td)) return 0;
    if (!(tr - tl > 1e-3f) || !(tu - td > 1e-3f)) return 0;
    x = -tl / (tr - tl);
    y = tu / (tu - td);
    if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)) return 0;     /* the optical axis outside the image */
    *cx = x; *cy = y;
    return 1;
}

/* Where each level's rings end, in kvk_fdm.h's normalised distance: full density up to r1, a quarter of the pixels
 * (0.5 on both axes) up to r2, a sixteenth (0.25) beyond it. A ring that is never reached is infinity. Tuned with
 * tests/kelvin_vk/test_fdm.c (it prints each level's share of shaded pixels for a Quest 3 field of view). */
static const struct { float r1, r2; } k_ring[5] = {
    { 1.0e9f, 1.0e9f },     /* off */
    { 1.05f, 1.0e9f },      /* low: only the corners at half density */
    { 0.92f, 1.0e9f },      /* medium: half density outside 0.92 */
    { 0.72f, 1.25f },       /* high: half density outside 0.72, a quarter in the far corners */
    { 0.72f, 1.25f },       /* hightop: high, centre moved up (kvk_fdm_fill) */
};

float kvk_fdm_density(int level, float d)
{
    if (level <= KVK_FDM_OFF || level > KVK_FDM_HIGH_TOP) return 1.0f;
    if (d < k_ring[level].r1) return 1.0f;
    if (d < k_ring[level].r2) return 0.5f;
    return 0.25f;
}

static uint8_t dens_byte(float dens) { return dens >= 1.0f ? 255u : dens >= 0.5f ? 128u : 64u; }

uint32_t kvk_fdm_fill(uint8_t *out, uint32_t cap, const KvkFdmParams *p, float tan_up_down)
{
    uint32_t tw, th, i, j, n;
    float cx, cy, rx, ry;
    if (!out || !p || !p->width || !p->height || !p->texel_w || !p->texel_h) return 0;
    tw = kvk_fdm_dim(p->width, p->texel_w);
    th = kvk_fdm_dim(p->height, p->texel_h);
    n = tw * th * 2u;
    if (n > cap) return 0;
    cx = p->cx; cy = p->cy;
    if (!(cx >= 0.0f && cx <= 1.0f)) cx = 0.5f;
    if (!(cy >= 0.0f && cy <= 1.0f)) cy = 0.5f;
    if (p->level == KVK_FDM_HIGH_TOP && tan_up_down > 1e-3f) {
        cy -= tanf(KVK_FDM_HIGH_TOP_DEG * 0.0174532925f) / tan_up_down;
        if (cy < 0.0f) cy = 0.0f;
    }
    rx = (cx > 1.0f - cx ? cx : 1.0f - cx) * (float)p->width;
    ry = (cy > 1.0f - cy ? cy : 1.0f - cy) * (float)p->height;
    if (rx < 1.0f) rx = 1.0f;
    if (ry < 1.0f) ry = 1.0f;
    for (j = 0; j < th; j++) {
        float py = ((float)j + 0.5f) * (float)p->texel_h;
        if (py > (float)p->height) py = (float)p->height;
        for (i = 0; i < tw; i++) {
            float px = ((float)i + 0.5f) * (float)p->texel_w;
            float ex, ey, d;
            uint8_t b;
            if (px > (float)p->width) px = (float)p->width;
            ex = (px - cx * (float)p->width) / rx;
            ey = (py - cy * (float)p->height) / ry;
            d = sqrtf(ex * ex + ey * ey);
            b = dens_byte(kvk_fdm_density(p->level, d));
            out[(j * tw + i) * 2u + 0u] = b;
            out[(j * tw + i) * 2u + 1u] = b;
        }
    }
    return n;
}

float kvk_fdm_work(const uint8_t *map, const KvkFdmParams *p)
{
    uint32_t tw, th, i, j;
    double sum = 0.0;
    if (!map || !p || !p->width || !p->height || !p->texel_w || !p->texel_h) return 0.0f;
    tw = kvk_fdm_dim(p->width, p->texel_w);
    th = kvk_fdm_dim(p->height, p->texel_h);
    for (j = 0; j < th; j++) {
        const uint32_t hh = (j + 1u) * p->texel_h > p->height ? p->height - j * p->texel_h : p->texel_h;
        for (i = 0; i < tw; i++) {
            const uint32_t ww = (i + 1u) * p->texel_w > p->width ? p->width - i * p->texel_w : p->texel_w;
            const double dx = map[(j * tw + i) * 2u + 0u] / 255.0, dy = map[(j * tw + i) * 2u + 1u] / 255.0;
            sum += dx * dy * (double)ww * (double)hh;
        }
    }
    return (float)(sum / ((double)p->width * (double)p->height));
}
