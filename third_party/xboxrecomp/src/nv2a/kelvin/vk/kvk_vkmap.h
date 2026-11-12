/*
 * kvk_vkmap.h - NV2A state values (as KPixelState / KTexture hold them) to Vulkan enums.
 *
 * Each mirrors the D3D11 backend's mapping in kelvin_gpu.c (map_blend, get_blend, get_depth, get_raster,
 * get_sampler, color_dxgi). Where D3D11 had to approximate and Vulkan does not, Vulkan follows the CPU reference
 * (kelvin_pixel.c): blend factors 14/15 are the constant's alpha (D3D11 used the whole constant colour).
 */
#ifndef KVK_VKMAP_H
#define KVK_VKMAP_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

VkBlendFactor kvk_blend_factor(int nv2a_factor, bool alpha);
VkBlendOp kvk_blend_op(int nv2a_eqn);
VkCompareOp kvk_compare_op(int nv2a_func);              /* depth, stencil (NEVER..ALWAYS, same order) */
VkStencilOp kvk_stencil_op(int nv2a_op);                /* 1 KEEP .. 8 DECR; 0 is KEEP */
VkSamplerAddressMode kvk_address_mode(unsigned int nv2a_addr);
/* min/mag filter and mip mode; *lod0: sample level 0 only (maxLod = minLod) */
void kvk_filter(unsigned int min_filter, unsigned int mag_filter, VkFilter *min, VkFilter *mag,
                VkSamplerMipmapMode *mip, bool *lod0);
/* Vulkan 1.1 has three border colours: the nearest of transparent black, opaque black, opaque white. */
VkBorderColor kvk_border_color(uint32_t argb);
/* Surface colour formats (NV097_SET_SURFACE_FORMAT_COLOR_*), with bytes per pixel; BGRA8 for anything 32-bit. */
VkFormat kvk_color_format(unsigned int nv2a_fmt, unsigned int *bpp);
/* Colour write mask: R G B A bits as D3D11 (and Vulkan) number them, from KPixelState's masks. */
VkColorComponentFlags kvk_write_mask(bool r, bool g, bool b, bool a);

#ifdef __cplusplus
}
#endif

#endif
