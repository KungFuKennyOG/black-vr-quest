/* grx_shell.c - tests/game_render_x86: plays port/quest/src/xr_main.c's part (android_main is not compiled here) on
 * the fake OpenXR runtime, and is the one translation unit of the harness that sees the shell's internals
 * (xr_shell.h). Order as on the headset: config, vrb_init, Vulkan through XR_KHR_vulkan_enable2, session +
 * swapchains + vr_renderer_init (want_renderer: a game will start), session READY..FOCUSED; the harness then starts
 * the game thread and runs grx_shell_frame() as the XR thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "xr_shell.h"
#include "bvr_diag.h"
#include "fake_openxr.h"
#include "grx_shell.h"

XrShell g_xr;

int grx_shell_setup(const GrxShellOptions *o)
{
    XrShell *s = &g_xr;
    static ANativeActivity act;
    static struct android_app app;
    int i;
    memset(s, 0, sizeof(*s));
    app.activity = &act;
    s->app = &app;
    /* the device preflight, in xr_main.c's order (port/notes/device-preflight.md); off with GRX_PREFLIGHT=0 */
    s->preflight_on = !getenv("GRX_PREFLIGHT") || *getenv("GRX_PREFLIGHT") != '0';
    if (s->preflight_on) {
        const char *cand[1] = { o->game_root };
        char save[600];
        BvrGameDir gd;
        bvr_pf_begin("x86 headless harness");
        bvr_pf_gamedir(cand, 1, 0, NULL, &gd);
        snprintf(save, sizeof(save), "%s/save", o->game_root);
        bvr_pf_storage(o->game_root, save, NULL);
        xrp_process(s);
        xrp_audio();
        xrp_vulkan_probe();
    }
    snprintf(s->game_root, sizeof(s->game_root), "%s", o->game_root);
    snprintf(s->files_dir, sizeof(s->files_dir), "%s", o->game_root);
    s->game_files_present = 1;
    /* blackvr.ini: the shell's defaults (xr_main.c writes them as the default ini), plus the run's own lines */
    vr_config_defaults(&s->cfg);
    /* GRX_EYE_W x GRX_EYE_H is the eye image size (frames comparable across runs); GRX_INI may still set a scale */
    s->cfg.resolution_scale = 1.0f;
    if (o->ini_text && *o->ini_text) vr_config_parse(&s->cfg, o->ini_text);
    s->cfg.multiview = o->multiview;
    vr_config_export_env(&s->cfg, setenv);     /* as xr_main.c export_config */
    vrb_init(s);
    fx_queue_lock = &s->queue_lock;
    /* what xrs_instance_create finds on the fake runtime */
    s->instance = (XrInstance)(uintptr_t)1;
    s->system = 1;
    for (i = 0; i < VR_EYE_COUNT; i++) {
        s->config_views[i].recommendedImageRectWidth = o->eye_w;
        s->config_views[i].recommendedImageRectHeight = o->eye_h;
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
    s->want_renderer = 1;              /* game files found and black_game_main linked */
    snprintf(s->cache_dir, sizeof(s->cache_dir), "%s", o->cache_dir);
    mkdir(s->cache_dir, 0770);
    {
        int ok;
        if (s->preflight_on) xrp_openxr(s, 1);
        xrs_failure_clear();
        ok = xrs_vulkan_create(s);
        if (s->preflight_on) xrp_vulkan_device(s, ok);
        if (ok) {
            xrs_failure_clear();
            ok = xrs_session_create(s);
            if (s->preflight_on) xrp_session(s, ok);
        }
        if (s->preflight_on) bvr_pf_summary_line(ok ? "starting the game" : "XR startup failed");
        if (!ok) return -1;
    }
    fx_push_session_state(XR_SESSION_STATE_READY);
    fx_push_session_state(XR_SESSION_STATE_SYNCHRONIZED);
    fx_push_session_state(XR_SESSION_STATE_VISIBLE);
    fx_push_session_state(XR_SESSION_STATE_FOCUSED);
    xrs_poll_events(s);
    return s->external_renderer;
}

void grx_shell_game_started(void) { g_xr.game_started = 1; }

void grx_shell_frame(void)
{
    xrs_poll_events(&g_xr);
    if (g_xr.session_running) xrs_frame(&g_xr);
}

void grx_shell_info(GrxShellInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->multiview = g_xr.multiview;
    out->renderer_started = g_xr.renderer_started;
    out->eye[0] = g_xr.eye[0].handle;
    out->eye[1] = g_xr.eye[1].handle;
    out->flat = g_xr.flat.handle;
    out->hud = g_xr.hud.handle;
    out->eye_w = g_xr.eye[0].width;
    out->eye_h = g_xr.eye[0].height;
    out->flat_w = g_xr.flat.width;
    out->flat_h = g_xr.flat.height;
    out->hud_w = g_xr.hud.width;
    out->hud_h = g_xr.hud.height;
    out->submit_mode = g_xr.submit.mode;
    out->submit_hud = g_xr.submit.hud;
    out->submit_game_frame = g_xr.submit.game_frame;
    out->submit_latch_serial = g_xr.submit.latch_serial;
    out->view_space = g_xr.view_space;
    out->world_space = g_xr.world_space;
    out->aim_hand = g_xr.cfg.game.aim_mode == VR_AIM_HAND;
}
