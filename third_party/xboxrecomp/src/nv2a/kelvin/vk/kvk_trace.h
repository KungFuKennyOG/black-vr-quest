/*
 * kvk_trace.h - the Kelvin Vulkan backend's call trace (RECOMP_KVK_TRACE, host builds with KVK_TRACE only).
 *
 * A trace is everything the backend was given for a run of whole frames: every kgpu_draw / kgpu_clear / kgpu_flip /
 * kgpu_note_kick / kgpu_set_view / kgpu_set_viewmodel call with deep copies of the memory it reads (vertex records or
 * vertex arrays, indices, constants, texture and palette bytes), the vertex programs' microcode, and what the VR glue
 * answered (the stereo transforms of each phase, the frame mode, the gun-latch classification, which shell targets
 * the flip got). tests/kelvin_vk/kvk_replay.c feeds it to a fresh backend with the glue's answers replayed, so the same
 * frames can be drawn deterministically through two backend configurations (RECOMP_KVK_PASS_MERGE=0 and 1) and the
 * pictures compared bit for bit (port/notes/renderer.md section 13).
 *
 * File: KvkTrHeader, then records { uint32 tag, uint32 bytes, payload } in call order. A record that a call causes
 * (blobs, programs, stereo transforms) comes before the call's own record, which is written when the call returns.
 * Structures are stored raw (same compiler and ABI on both sides; the header checks the sizes); pointers inside them
 * are meaningless in the file and are replaced on replay by blob ids stored beside them.
 */
#ifndef KVK_TRACE_H
#define KVK_TRACE_H

#include <stdint.h>
#include "../kelvin_gpu.h"
#include "kvk_stereo.h"
#include "vr_bridge.h"

#define KVK_TR_MAGIC   0x3152544Bu      /* "KTR1" */
#define KVK_TR_VERSION 1u

typedef struct KvkTrHeader {
    uint32_t magic, version;
    uint32_t host_w, host_h, layers, hud_w, hud_h;
    uint32_t sz_draw, sz_ps, sz_target, sz_vsdraw, sz_stereo, sz_gamecfg;
    uint64_t first_frame;               /* the backend's frame number of the first traced frame */
    uint32_t frames;                    /* flips in the file (rewritten at the end) */
    uint32_t pad;
} KvkTrHeader;

enum {
    KVK_TR_BLOB = 1,        /* uint32 id, uint32 pad, uint64 bytes, then the bytes */
    KVK_TR_VSPROG,          /* uint32 hash, start, nwords, then nwords words */
    KVK_TR_DRAW,            /* KvkTrDraw */
    KVK_TR_CLEAR,           /* KvkTrClear */
    KVK_TR_FLIP,            /* KvkTrFlip */
    KVK_TR_KICK,            /* nothing */
    KVK_TR_VIEW,            /* int32 mode, slot, x0, y0, x1, y1 */
    KVK_TR_VIEWMODEL,       /* int32 count, float scale, uint32 hashes[8] */
    KVK_TR_STEREO,          /* KvkStereoFrame: the first-draw transforms stereo_prepare settled on */
    KVK_TR_GAMECFG,         /* uint32 have, VrGameConfig */
};

/* the frame mode and glue answers in force for a call */
typedef struct KvkTrCtx {
    int32_t frame_mode;     /* kvk_vr_frame_mode() */
    uint32_t vr_active;
} KvkTrCtx;

typedef struct KvkTrDraw {
    KvkTrCtx ctx;
    uint32_t vm;            /* kvk_vr_viewmodel_draw() answered true for this draw */
    uint32_t has_color, has_zeta, has_gvs;
    uint32_t recs_blob, idx_blob, const_blob;
    uint32_t tex_blob[4], pal_blob[4];      /* 0: none (no texture memory) */
    uint32_t attr_blob[16];                 /* the bytes [lo, hi + 16) of attr[a] */
    KGpuDraw d;
    KelvinTarget t;
    KPixelState ps;
    KGpuVsDraw gv;
} KvkTrDraw;

typedef struct KvkTrClear {
    KvkTrCtx ctx;
    uint32_t has_color, has_zeta;
    KelvinTarget t;
    uint32_t color, depth, stencil, color_value, zs_value, channel_mask, color_format;
    int32_t x0, y0, x1, y1;
} KvkTrClear;

typedef struct KvkTrFlip {
    KvkTrCtx ctx;
    uint32_t has_color, has_zeta;
    KelvinTarget t;
    uint32_t color_format, aspect_w, aspect_h;
    /* what the flip got from the glue */
    uint32_t flat_target, eye_targets, hud_target;
    uint32_t relatch_ok, relatch_vm_ok;     /* kvk_vr_stereo_hook / kvk_vr_viewmodel_hook (phase FLIP) answered */
    KvkStereoFrame relatch, relatch_vm;
} KvkTrFlip;

#endif
