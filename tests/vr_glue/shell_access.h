/* shell_access.h - tests/vr_glue: the shell's internals the test needs (see shell_access.c). */
#ifndef SHELL_ACCESS_H
#define SHELL_ACCESS_H
#include <stdint.h>
#include "fake_openxr.h"

/* xr_main.c's startup on the fake runtime: Vulkan, session, swapchains, renderer, session running + focused.
 * Returns 1 when the renderer glue attached, 0 when it did not, -1 on failure. */
int shell_setup(int multiview, float units_per_metre, uint32_t eye_w, uint32_t eye_h, const char *cache_dir);
void shell_xr_frame(void);             /* one XR-thread frame: xrWaitFrame ... xrEndFrame */
int shell_session_running(void);
int shell_multiview(void);
int shell_renderer_started(void);
XrSwapchain shell_eye_swapchain(int i);
XrSwapchain shell_flat_swapchain(void);
XrSwapchain shell_hud_swapchain(void);
uint32_t shell_eye_width(void);
uint32_t shell_eye_height(void);
uint64_t shell_submit_latch_serial(void);
int shell_submit_mode(void);
XrSpace shell_view_space(void);
XrSpace shell_world_space(void);
/* gun latch: blackvr.ini gun_latch as parsed (vr_config_line), and set directly */
int shell_config_line(const char *line);
void shell_set_gun_latch(int on);
int shell_gun_latch(void);
/* the settings panel (port/notes/settings-panel.md) */
typedef struct {
    int open, inited, released;
    XrSwapchain panel, laser;
    XrPosef quad_pose;
    float width_m, height_m;
    int mouse_valid;
    float mouse_x, mouse_y, laser_len;
    char status[240];
} ShellPanel;
void shell_panel(ShellPanel *out);
void shell_set_game_root(const char *dir);        /* where blackvr.ini is saved */
float shell_cfg_get(int setting);                 /* vrs_get on the shell's live config (VRS_* of vr_settings.h) */
void shell_set_present_mode(int mode);
#endif
