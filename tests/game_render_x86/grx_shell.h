/* grx_shell.h - tests/game_render_x86: the shell internals the harness needs (grx_shell.c). */
#ifndef GRX_SHELL_H
#define GRX_SHELL_H
#include <stdint.h>
#include "fake_openxr.h"

typedef struct GrxShellOptions {
    int multiview;
    uint32_t eye_w, eye_h;
    const char *game_root;     /* the folder holding game/ and save/ (the shell chdir()s there) */
    const char *cache_dir;     /* kgpu-cache (internalDataPath on the headset) */
    const char *ini_text;      /* extra blackvr.ini lines, or NULL */
} GrxShellOptions;

typedef struct GrxShellInfo {
    int multiview, renderer_started;
    XrSwapchain eye[2], flat, hud;
    uint32_t eye_w, eye_h, flat_w, flat_h, hud_w, hud_h;
    int submit_mode, submit_hud;
    uint32_t submit_game_frame;
    uint64_t submit_latch_serial;   /* nonzero once the renderer has released eye images */
    XrSpace view_space, world_space;
    int aim_hand;
} GrxShellInfo;

/* 1: the renderer glue attached; 0: it did not; -1: XR/Vulkan setup failed. */
int grx_shell_setup(const GrxShellOptions *o);
void grx_shell_game_started(void);
void grx_shell_frame(void);            /* one XR-thread iteration: events, xrWaitFrame ... xrEndFrame */
void grx_shell_info(GrxShellInfo *out);
#endif
