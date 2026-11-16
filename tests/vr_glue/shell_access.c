/* shell_access.c - tests/vr_glue: the one translation unit that sees the shell's internals (xr_shell.h); the test
 * itself only sees vr_bridge.h and these helpers. Plays xr_main.c's part (android_main is not compiled here). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "xr_shell.h"
#include "fake_openxr.h"
#include "shell_access.h"
#include "vr_settings.h"

XrShell g_xr;

static int s_verbose = -1;

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    if (s_verbose < 0) s_verbose = getenv("VRGLUE_VERBOSE") != NULL;
    if (!s_verbose && prio < ANDROID_LOG_WARN) return 0;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}

int shell_setup(int multiview, float units_per_metre, uint32_t eye_w, uint32_t eye_h, const char *cache_dir)
{
    static ANativeActivity act;
    static struct android_app app;
    XrShell *s = &g_xr;
    int i;
    memset(s, 0, sizeof(*s));
    app.activity = &act;
    s->app = &app;
    vr_config_defaults(&s->cfg);
    s->cfg.resolution_scale = 1.0f;          /* eye_w x eye_h is the eye image size the test checks against */
    s->cfg.multiview = multiview;
    s->cfg.game.units_per_metre = units_per_metre;
    vrb_init(s);
    s->panel = xrs_panel_attach(s);          /* xr_main.c attaches the settings panel after blackvr.ini */
    fx_queue_lock = &s->queue_lock;
    /* what xrs_instance_create would have found */
    s->instance = (XrInstance)(uintptr_t)1;
    s->system = 1;
    for (i = 0; i < VR_EYE_COUNT; i++) {
        s->config_views[i].recommendedImageRectWidth = eye_w;
        s->config_views[i].recommendedImageRectHeight = eye_h;
        s->config_views[i].maxImageRectWidth = 4096;
        s->config_views[i].maxImageRectHeight = 4096;
    }
    s->has_timespec = 1;
    s->xrConvertTimespecTimeToTimeKHR = fx_convert_time;
    s->has_format_list = 1;
    /* the optional instance extensions, as xrs_instance_create negotiates them with the (fake) runtime */
    {
        XrExtensionProperties ext[16];
        const char *want[24];
        uint32_t n = 0, en = 0, k;
        for (k = 0; k < 16; k++) { memset(&ext[k], 0, sizeof(ext[k])); ext[k].type = XR_TYPE_EXTENSION_PROPERTIES; }
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 16, &n, ext))) xrs_instance_extensions(s, ext, n, want, &en);
        xrs_instance_procs(s);
    }
    /* xr_main.c: a game will start, so the renderer is offered the device */
    s->want_renderer = 1;
    snprintf(s->cache_dir, sizeof(s->cache_dir), "%s", cache_dir);
    mkdir(s->cache_dir, 0770);
    if (!xrs_vulkan_create(s) || !xrs_session_create(s)) return -1;
    fx_push_session_state(XR_SESSION_STATE_READY);
    fx_push_session_state(XR_SESSION_STATE_SYNCHRONIZED);
    fx_push_session_state(XR_SESSION_STATE_VISIBLE);
    fx_push_session_state(XR_SESSION_STATE_FOCUSED);
    xrs_poll_events(s);
    return s->external_renderer;
}

void shell_xr_frame(void) { xrs_frame(&g_xr); }
int shell_session_running(void) { return g_xr.session_running && g_xr.focused; }
int shell_multiview(void) { return g_xr.multiview; }
int shell_renderer_started(void) { return g_xr.renderer_started; }
XrSwapchain shell_eye_swapchain(int i) { return g_xr.eye[i].handle; }
XrSwapchain shell_flat_swapchain(void) { return g_xr.flat.handle; }
XrSwapchain shell_hud_swapchain(void) { return g_xr.hud.handle; }
uint32_t shell_eye_width(void) { return g_xr.eye[0].width; }
uint32_t shell_eye_height(void) { return g_xr.eye[0].height; }
uint64_t shell_submit_latch_serial(void) { return g_xr.submit.latch_serial; }
int shell_submit_mode(void) { return g_xr.submit.mode; }
XrSpace shell_view_space(void) { return g_xr.view_space; }
XrSpace shell_world_space(void) { return g_xr.world_space; }

int shell_config_line(const char *line) { return vr_config_line(&g_xr.cfg, line); }
void shell_set_gun_latch(int on) { g_xr.cfg.gun_latch = on; }
int shell_gun_latch(void) { return g_xr.cfg.gun_latch; }
float shell_haptic_fire(void) { return g_xr.cfg.haptic_fire; }
void shell_take_pulse(int hand, float *strength, uint32_t *ms) { vrb_take_pulse(hand, strength, ms); }

void shell_panel(ShellPanel *out)
{
    XrsPanelTestState st;
    xrs_panel_test_state(&st);
    memset(out, 0, sizeof(*out));
    out->open = st.open; out->inited = st.inited; out->released = st.released;
    out->panel = st.panel; out->laser = st.laser;
    out->quad_pose.orientation.x = st.quad_pose.orientation.x; out->quad_pose.orientation.y = st.quad_pose.orientation.y;
    out->quad_pose.orientation.z = st.quad_pose.orientation.z; out->quad_pose.orientation.w = st.quad_pose.orientation.w;
    out->quad_pose.position.x = st.quad_pose.position.x; out->quad_pose.position.y = st.quad_pose.position.y;
    out->quad_pose.position.z = st.quad_pose.position.z;
    out->width_m = st.width_m; out->height_m = st.height_m;
    out->mouse_valid = st.mouse_valid; out->mouse_x = st.mouse_x; out->mouse_y = st.mouse_y; out->laser_len = st.laser_len;
    snprintf(out->status, sizeof(out->status), "%s", st.status);
}
void shell_set_game_root(const char *dir) { snprintf(g_xr.game_root, sizeof(g_xr.game_root), "%s", dir); }
float shell_cfg_get(int setting) { return vrs_get(&g_xr.cfg, setting); }
void shell_set_present_mode(int mode) { vr_set_present_mode(mode); }
