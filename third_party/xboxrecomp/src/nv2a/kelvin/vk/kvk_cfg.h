/* kvk_cfg.h - the shared constant layouts (gfx/kgfx_cfg_types.inc) for the Vulkan backend's modules, once. */
#ifndef KVK_CFG_H
#define KVK_CFG_H

#include <stdint.h>

#include "../gfx/kgfx_cfg_types.inc"

/* PSConst.texflags bit the D3D11 backend sets for screen-space reflections; never set by the Vulkan backend. */
#ifndef PS_TEX_SSR
#define PS_TEX_SSR 0x100u
#endif

#endif
