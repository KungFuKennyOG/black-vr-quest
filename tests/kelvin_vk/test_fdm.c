/* kvk-fdm: the fragment density map generator of the Vulkan backend (kvk_fdm.c): pure functions, any host.
 * Checks the projection centre from a field of view, the map's size, symmetry and monotonicity (density only falls with the
 * distance from the centre, centre texel always full), the work share per level (off 1.0 > low > medium > high), that
 * hightop moves the sharp region up, and that bad parameters write nothing. Prints each level's share for tuning. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvk_fdm.h"

static int s_fail, s_checks;
#define CHECK(c, ...) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* a Quest 3 left eye, about: 54/45 degrees left/right, 48/52 up/down (tangents, left and down negative) */
    const float tl = -1.38f, tr = 1.0f, tu = 1.11f, td = -1.28f;
    KvkFdmParams p;
    static uint8_t map[512 * 512 * 2];
    float cx, cy, work[5], w0;
    uint32_t n, tw, th, i, j, lv;

    CHECK(kvk_fdm_dim(1680, 16) == 105 && kvk_fdm_dim(1760, 16) == 110 && kvk_fdm_dim(17, 16) == 2 && kvk_fdm_dim(5, 0) == 0, "dim");
    CHECK(kvk_fdm_centre(&cx, &cy, tl, tr, tu, td) == 1, "centre ok");
    CHECK(fabsf(cx - 1.38f / 2.38f) < 1e-5f && fabsf(cy - 1.11f / 2.39f) < 1e-5f, "centre %f %f", cx, cy);
    CHECK(kvk_fdm_centre(&cx, &cy, 0.2f, 1.0f, 1.0f, -1.0f) == 0 && cx == 0.5f && cy == 0.5f, "axis outside the image");
    CHECK(kvk_fdm_centre(&cx, &cy, NAN, 1.0f, 1.0f, -1.0f) == 0, "nan fov");
    CHECK(kvk_fdm_density(KVK_FDM_OFF, 5.0f) == 1.0f && kvk_fdm_density(KVK_FDM_HIGH, 0.0f) == 1.0f &&
          kvk_fdm_density(KVK_FDM_HIGH, 1.3f) == 0.25f && kvk_fdm_density(99, 1.0f) == 1.0f, "density");

    kvk_fdm_centre(&cx, &cy, tl, tr, tu, td);
    p.width = 1680; p.height = 1760; p.texel_w = p.texel_h = 16; p.cx = cx; p.cy = cy;
    tw = kvk_fdm_dim(p.width, p.texel_w); th = kvk_fdm_dim(p.height, p.texel_h);
    for (lv = 0; lv <= 4; lv++) {
        float prev = 2.0f;
        p.level = (int)lv;
        memset(map, 0xEE, sizeof(map));
        n = kvk_fdm_fill(map, sizeof(map), &p, tu - td);
        CHECK(n == tw * th * 2u, "level %u wrote %u, want %u", lv, n, tw * th * 2u);
        work[lv] = kvk_fdm_work(map, &p);
        printf("level %u: shades %.1f%% of the pixels\n", lv, work[lv] * 100.0f);
        /* the texel nearest the centre is at full density, density is one of the three tiers, R == G */
        {
            const uint32_t ci = (uint32_t)(cx * (float)tw), cj = (uint32_t)(cy * (float)th);
            CHECK(map[(cj * tw + ci) * 2] == 255 || (lv == KVK_FDM_HIGH_TOP), "level %u: centre texel not full", lv);
        }
        for (j = 0; j < th; j++)
            for (i = 0; i < tw; i++) {
                const uint8_t r = map[(j * tw + i) * 2], g = map[(j * tw + i) * 2 + 1];
                if (!(r == g && (r == 255 || r == 128 || r == 64))) { CHECK(0, "level %u texel %u,%u = %u,%u", lv, i, j, r, g); i = tw; j = th; }
            }
        /* along the row through the centre, density never rises going away from it */
        if (lv != KVK_FDM_HIGH_TOP) {
            const uint32_t cj = (uint32_t)(cy * (float)th), ci = (uint32_t)(cx * (float)tw);
            prev = 2.0f;
            for (i = ci; i < tw; i++) { float v = map[(cj * tw + i) * 2] / 255.0f; CHECK(v <= prev + 1e-6f, "level %u row rises at %u", lv, i); prev = v; }
            prev = 2.0f;
            for (j = cj; j < th; j++) { float v = map[(j * tw + ci) * 2] / 255.0f; CHECK(v <= prev + 1e-6f, "level %u column rises at %u", lv, j); prev = v; }
        }
    }
    CHECK(work[0] == 1.0f, "off shades everything (%f)", work[0]);
    CHECK(work[1] < work[0] && work[2] < work[1] && work[3] < work[2], "work falls with the level: %f %f %f %f", work[0], work[1], work[2], work[3]);
    CHECK(work[1] > 0.85f && work[2] > 0.70f && work[3] > 0.45f, "levels are not absurd: %f %f %f", work[1], work[2], work[3]);
    CHECK(fabsf(work[4] - work[3]) < 0.08f, "hightop costs about what high costs (%f vs %f)", work[4], work[3]);

    /* hightop: the sharp region sits higher, so rows above the optical axis are sharper than rows below it */
    {
        uint8_t hi[512 * 512 * 2];
        uint32_t above = 0, below = 0, cj = (uint32_t)(cy * (float)th), k = 12;
        p.level = KVK_FDM_HIGH; kvk_fdm_fill(hi, sizeof(hi), &p, tu - td);
        p.level = KVK_FDM_HIGH_TOP; kvk_fdm_fill(map, sizeof(map), &p, tu - td);
        for (i = 0; i < tw; i++) {
            above += map[((cj - k) * tw + i) * 2] > hi[((cj - k) * tw + i) * 2];
            below += map[((cj + k) * tw + i) * 2] < hi[((cj + k) * tw + i) * 2];
        }
        CHECK(above > 0 && below > 0, "hightop moves the sharp region up (%u texels sharper above, %u softer below)", above, below);
    }

    /* an eye at the exact image centre is left-right and top-bottom symmetric */
    w0 = 0;
    p.cx = 0.5f; p.cy = 0.5f; p.width = 1600; p.height = 1600; p.level = KVK_FDM_MEDIUM;
    tw = kvk_fdm_dim(p.width, 16); th = kvk_fdm_dim(p.height, 16);
    kvk_fdm_fill(map, sizeof(map), &p, tu - td);
    for (j = 0; j < th; j++) for (i = 0; i < tw; i++)
        if (map[(j * tw + i) * 2] != map[(j * tw + (tw - 1 - i)) * 2] || map[(j * tw + i) * 2] != map[((th - 1 - j) * tw + i) * 2]) w0 += 1;
    CHECK(w0 == 0, "centred map is symmetric (%g texels differ)", w0);

    /* partial texels: 17 x 17 pixels is 2 x 2 texels, the work share counts the partial ones by their pixels */
    p.width = p.height = 17; p.level = KVK_FDM_OFF;
    CHECK(kvk_fdm_fill(map, sizeof(map), &p, 2.0f) == 8u, "17x17 map size");
    CHECK(fabsf(kvk_fdm_work(map, &p) - 1.0f) < 1e-6f, "17x17 work");

    /* bad parameters write nothing */
    CHECK(kvk_fdm_fill(map, 4, &p, 2.0f) == 0, "capacity too small");
    p.texel_w = 0; CHECK(kvk_fdm_fill(map, sizeof(map), &p, 2.0f) == 0, "texel 0");
    CHECK(kvk_fdm_fill(NULL, 100, &p, 2.0f) == 0 && kvk_fdm_fill(map, 100, NULL, 2.0f) == 0, "null");

    printf("kvk-fdm: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
