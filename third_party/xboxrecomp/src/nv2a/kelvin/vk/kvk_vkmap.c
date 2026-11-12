/*
 * kvk_vkmap.c - NV2A state values to Vulkan enums. See kvk_vkmap.h.
 */
#include "kvk_vkmap.h"

#include "../../nv2a_regs.h"

VkBlendFactor kvk_blend_factor(int f, bool alpha)
{
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_DST_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 8: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 9: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ZERO;
    }
}

VkBlendOp kvk_blend_op(int eqn)
{
    switch (eqn) {
    case 0: return VK_BLEND_OP_SUBTRACT;
    case 1: case 5: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 3: return VK_BLEND_OP_MIN;
    case 4: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp kvk_compare_op(int func)
{
    /* NV2A: NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS -- VkCompareOp's order */
    return (VkCompareOp)(func & 7);
}

VkStencilOp kvk_stencil_op(int op)
{
    /* NV2A (= D3D11_STENCIL_OP): 1 KEEP 2 ZERO 3 REPLACE 4 INCR_SAT 5 DECR_SAT 6 INVERT 7 INCR 8 DECR */
    if (op < 1 || op > 8) op = 1;
    return (VkStencilOp)(op - 1);
}

VkSamplerAddressMode kvk_address_mode(unsigned int a)
{
    switch (a) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, and the D3D11 backend's default */
    }
}

void kvk_filter(unsigned int min_filter, unsigned int mag_filter, VkFilter *min, VkFilter *mag,
                VkSamplerMipmapMode *mip, bool *lod0)
{
    bool min_lin, mip_lin;
    switch (min_filter) {
    case NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0:        min_lin = false; mip_lin = false; *lod0 = true; break;
    case NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0:       min_lin = true;  mip_lin = false; *lod0 = true; break;
    case NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD:  min_lin = false; mip_lin = false; *lod0 = false; break;
    case NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD: min_lin = true;  mip_lin = false; *lod0 = false; break;
    case NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD:    min_lin = false; mip_lin = true;  *lod0 = false; break;
    case NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD:   min_lin = true;  mip_lin = true;  *lod0 = false; break;
    default:                                       min_lin = true;  mip_lin = false; *lod0 = true; break;
    }
    *min = min_lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    *mag = mag_filter != 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    *mip = mip_lin ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkBorderColor kvk_border_color(uint32_t argb)
{
    unsigned a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    if (a < 128) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    return r + g + b >= 3 * 128 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

VkFormat kvk_color_format(unsigned int fmt, unsigned int *bpp)
{
    switch (fmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        *bpp = 2; return VK_FORMAT_A1R5G5B5_UNORM_PACK16;      /* = DXGI B5G5R5A1 */
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        *bpp = 2; return VK_FORMAT_R5G6B5_UNORM_PACK16;        /* = DXGI B5G6R5 */
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        *bpp = 1; return VK_FORMAT_R8_UNORM;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        *bpp = 2; return VK_FORMAT_R8G8_UNORM;
    default:
        *bpp = 4; return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

VkColorComponentFlags kvk_write_mask(bool r, bool g, bool b, bool a)
{
    return (r ? VK_COLOR_COMPONENT_R_BIT : 0) | (g ? VK_COLOR_COMPONENT_G_BIT : 0) |
           (b ? VK_COLOR_COMPONENT_B_BIT : 0) | (a ? VK_COLOR_COMPONENT_A_BIT : 0);
}
