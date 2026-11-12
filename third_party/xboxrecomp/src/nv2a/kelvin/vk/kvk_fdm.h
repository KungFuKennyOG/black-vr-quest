/*
 * kvk_fdm.h - the fragment density map the Kelvin Vulkan backend attaches to its main passes (VK_EXT_fragment_density_map),
 * as pure functions: no Vulkan, no state, so tests/kelvin_vk/test_fdm.c checks them on any host.
 *
 * What it makes. One R8G8_UNORM texel per `texel` pixels of a main target (the device's minimum density texel size, 16x16 on
 * a Quest 3), one map layer per eye. R is the shading density along x, G along y: 1.0 is one fragment per pixel, 0.5 one
 * per two pixels along that axis (so 0.5/0.5 shades a quarter of the pixels), 0.25 one per four. The sharp region is
 * centred on the eye's projection centre (where the optical axis meets the image), from the eye's field of view, and the
 * density falls off elliptically with the distance from it. The foveation level of blackvr.ini picks how fast.
 *
 * Distance. d = hypot(dx / Rx, dy / Ry) with dx, dy the pixel offset from the projection centre and Rx, Ry the distance from
 * the centre to the farthest vertical / horizontal edge. d = 1 is therefore the middle of the farthest edge and a corner is
 * about 1.4 away: the shape follows the image whatever the field of view is. Port notes: port/notes/foveation.md.
 */
#ifndef KVK_FDM_H
#define KVK_FDM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The levels are blackvr.ini's foveation values (vr_config.h VR_FOVEATION_*, XrFoveationLevelFB). */
enum { KVK_FDM_OFF = 0, KVK_FDM_LOW = 1, KVK_FDM_MEDIUM = 2, KVK_FDM_HIGH = 3, KVK_FDM_HIGH_TOP = 4 };

/* hightop: high, with the sharp centre moved up by this many degrees (vr_config.h VR_FOVEATION_HIGH_TOP_OFFSET_DEG) */
#define KVK_FDM_HIGH_TOP_DEG 10.0f

/* The map is ceil(pixels / texel) texels along an axis. */
uint32_t kvk_fdm_dim(uint32_t pixels, uint32_t texel);

/* The projection centre inside an eye image, as fractions of its width and height, y from the top: the point where the
 * tangent plane's origin lands, from an eye's field of view (tangents of the half angles, left and down negative:
 * tan_left < 0 < tan_right, tan_down < 0 < tan_up). False when the field of view is not usable (centre set to 0.5, 0.5). */
int kvk_fdm_centre(float *cx, float *cy, float tan_left, float tan_right, float tan_up, float tan_down);

/* The density (both axes) at normalised distance d for `level`: 1.0, 0.5 or 0.25. KVK_FDM_OFF: always 1.0. */
float kvk_fdm_density(int level, float d);

typedef struct KvkFdmParams {
    uint32_t width, height;     /* the target's size in pixels (the framebuffer) */
    uint32_t texel_w, texel_h;  /* pixels per map texel (>= the device's minFragmentDensityTexelSize) */
    float cx, cy;               /* projection centre, fractions of width / height, y from the top */
    int level;                  /* KVK_FDM_* */
} KvkFdmParams;

/* Fill one layer: kvk_fdm_dim(width, texel_w) x kvk_fdm_dim(height, texel_h) texels of R8G8_UNORM (two bytes each, rows
 * tightly packed, row 0 at the top). Returns the number of bytes written (0: bad parameters). The sharp region of `high-top`
 * is moved up inside this function (KVK_FDM_HIGH_TOP_DEG, with the tangent of the half field of view `tan_up_down` =
 * tan_up - tan_down). */
uint32_t kvk_fdm_fill(uint8_t *out, uint32_t cap, const KvkFdmParams *p, float tan_up_down);

/* The share of the layer's pixels the map shades (1.0 = all of them, 0.5 = half), counting each texel's pixels by their
 * density: sum(dx * dy * area) / total. A cost estimate for the log and the tests. */
float kvk_fdm_work(const uint8_t *map, const KvkFdmParams *p);

#ifdef __cplusplus
}
#endif

#endif
