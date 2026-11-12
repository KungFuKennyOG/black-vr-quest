/*
 * vr_config.h - blackvr.ini: shell, input and game-hook settings.
 * Header-only and pure C (unit-tested in tests/xr_shell).
 *
 * File: <app external files dir>/blackvr.ini, e.g.
 *   /sdcard/Android/data/com.kenny.blackvr/files/blackvr.ini
 *
 *   # comments start with # or ;
 *   refresh_hz        = auto      ; auto (120 for game_hz 60, 90 for 30) | 72 | 80 | 90 | 120
 *   resolution_scale  = 0.8       ; eye swapchain (and Kelvin raster) size vs the runtime's recommendation
 *   foveation         = medium    ; off | low | medium | high | hightop: XR_FB_foveation fixed foveated rendering
 *                                 ; (hightop: high, the sharp centre moved up by VR_FOVEATION_HIGH_TOP_OFFSET_DEG)
 *   game_hz           = 60        ; 30 | 60: the game's simulation and frame rate (port/notes/perf.md)
 *   raster_workers    = 1         ; 0..4 Kelvin raster/vertex worker threads beside the GPU thread
 *   raster_spin_us    = 50        ; 0..5000 us a raster worker spins for more work before it sleeps
 *   multiview         = 1         ; 1: one 2-layer swapchain, 0: two swapchains
 *   turn              = snap      ; snap | smooth | off
 *   snap_degrees      = 30
 *   smooth_degrees_per_second = 120
 *   deadzone          = 0.15
 *   left_handed       = 0         ; 1 mirrors the controls (gun in the left hand)
 *   aim               = hand      ; hand | head
 *   locomotion        = head      ; head | aim | offhand  (the direction the move stick moves you: where you look,
 *                                 ; where the gun points, or where the off-hand controller points)
 *   vr_accuracy       = aim       ; aim | hip: BLACK's player spread has no aim term (see port/notes/xr-shell.md),
 *                                 ; so both give the game's own spread; aim never needs the game's ADS
 *   melee_swing       = on        ; on | off: a fast swing of the gun hand is a melee attack
 *   melee_swing_speed = 2.5       ; m/s (hand relative to the head) that counts as a swing
 *   duck_crouch       = on        ; on | off: ducking crouches, standing up stands
 *   duck_ratio        = 0.80      ; crouch below this x your standing head height (stand above it + 0.08)
 *   scope_raise       = on        ; on | off: raising a scoped weapon to the eye looks through the scope
 *                                 ; (off: hold the gun-hand grip for the scope)
 *   scope_raise_distance = 0.15   ; metres from the gun's rear sight to an eye
 *   two_hand          = on        ; on | off: off-hand grip near the gun steadies the aim (gun hand -> off hand)
 *   world_scale       = 1.0       ; game units per metre
 *   max_head_offset   = 0.5       ; metres the view may lean from the body
 *   aim_converge      = 10.0      ; metres: where the hand's ray crosses the game's shot
 *   hand_weapon       = 1         ; first-person weapon follows the hand (aim = hand only)
 *   weapon_offset     = 0.12 -0.18 0.30   ; weapon grip in camera space (x right, y up, z fwd), metres
 *   scope_zoom        = 2.5       ; zoom factor at which the scope view goes flat
 *   fov_margin        = 1.10      ; game frustum enlargement over the eye FOV union (late-latch room)
 *   bob_filter        = 1
 *   hud_distance      = 3.0       ; metres: the head-locked HUD / virtual screen
 *   hud_width         = 2.4       ; metres (4:3)
 *   haptic_fire       = 0.7       ; 0..1: shot pulse on the gun hand's controller (0 = off; the game's own rumble stays)
 *   gun_latch         = on        ; on | off: the first-person weapon follows the hand pose latched at display
 *                                 ; time, not the game frame's (port/notes/gun-latch.md); env
 *                                 ; BLACKVR_GUN_LATCH_DEBUG=1 tints the draws taken for the weapon
 *   settings_menu     = native    ; native: VR SETTINGS in the game's Options menu | panel: the floating panel
 *                                 ; (left stick click in a menu) | both
 *   env RECOMP_BLACK_HZ=60        ; any "env NAME=VALUE" line is exported before the game starts
 */
#ifndef VR_CONFIG_H
#define VR_CONFIG_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../vr_bridge.h"
#include "vr_input_map.h"
#include "vr_gestures.h"

#define VR_CONFIG_MAX_ENV 32

enum { VR_LOCO_HEAD = 0, VR_LOCO_AIM = 1, VR_LOCO_OFFHAND = 2 };
enum { VR_SETTINGS_MENU_NATIVE = 0, VR_SETTINGS_MENU_PANEL = 1, VR_SETTINGS_MENU_BOTH = 2 };
/* XR_FB_foveation levels (XrFoveationLevelFB values) */
enum { VR_FOVEATION_OFF = 0, VR_FOVEATION_LOW = 1, VR_FOVEATION_MEDIUM = 2, VR_FOVEATION_HIGH = 3,
       /* not an XrFoveationLevelFB: HIGH with the level profile's verticalOffset (the VrApi "HighTop" level) */
       VR_FOVEATION_HIGH_TOP = 4 };
#define VR_FOVEATION_HIGH_TOP_OFFSET_DEG 10.0f

typedef struct {
    float refresh_hz;
    float resolution_scale;
    int   multiview;
    int   locomotion;
    float hud_distance_m, hud_width_m;   /* head-locked HUD quad layer (vr-glue) */
    float haptic_fire;       /* 0..1: strength of the shot pulse on the gun hand (0 off) */
    int   foveation;         /* VR_FOVEATION_* */
    int   game_hz;           /* 30 | 60 -> RECOMP_BLACK_HZ / RECOMP_BLACK_60HZ */
    int   raster_workers;    /* -> RECOMP_KELVIN_THREADS = workers + 1 */
    int   raster_spin_us;    /* -> RECOMP_KELVIN_SPIN_US */
    int   gun_latch;         /* 1: the weapon's draws follow the late-latched hand (gun-latch) */
    int   settings_menu;     /* VR_SETTINGS_MENU_*: the native page in the game's Options, the floating panel, both */
    VrInputConfig input;
    VrGestureConfig gest;    /* melee swing, duck crouch, scope raise, two-handed aim */
    VrGameConfig game;
    int   env_count;
    char  env[VR_CONFIG_MAX_ENV][160];
    int   errors;            /* lines that did not parse */
} VrShellConfig;

static inline void vr_config_defaults(VrShellConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->refresh_hz = 0.0f;   /* 0 = auto: 120 Hz for a 60 Hz game, 90 Hz for 30 (whole display frames per game frame) */
    /* 0.8 of the runtime's recommendation (Quest 3: 2064x2208 -> 1651x1766 per eye). The title's 640x480-era assets
     * and geometry gain little from the full size, and the Kelvin main target, its offscreen chain and the eye
     * composition all scale with it (fragment work ~0.64x). */
    c->resolution_scale = 0.8f;
    c->foveation = VR_FOVEATION_MEDIUM;
    /* The game's own rate. BLACK steps its world by a fixed 1/30 s per main-loop iteration; 60 is the PC build's
     * enhancement (halved step, patched oscillator, control filter and physics thresholds, src/recomp_manual.c) and
     * doubles the game thread's work, which the Quest cannot sustain in level 1 (the game then runs in slow motion).
     * 60 was the default only while XAPI's USB driver could hang or crash at boot under CPU load; both are fixed in
     * the device model now (usb_gamepad.c GET_REPORT, ohci.c frame gating; port/notes/perf.md 4.3). */
    /* 2026-11-11, first device runs (Quest 3): 60 holds 58-60 in menus and the start of level 1 and 45-50 in busier
     * scenes, and was preferred by eye over 30, so 60 is the default again; game_hz = 30 is the steady option. */
    c->game_hz = 60;
    /* With the Vulkan backend the raster threads only shade the vertices of draws whose vertex program is not
     * compiled yet: measured ~2 ms per frame in menus, ~0 in level 1 (port/notes/perf.md). One worker, short spin. */
    c->raster_workers = 1;
    c->raster_spin_us = 50;
    c->multiview = 1;
    c->locomotion = VR_LOCO_HEAD;
    c->hud_distance_m = 3.0f;
    c->hud_width_m = 2.4f;
    c->haptic_fire = 0.7f;
    c->gun_latch = 1;
    vr_input_default_config(&c->input);
    vrg_default_config(&c->gest);
    c->game.aim_mode = VR_AIM_HAND;
    c->game.aim_hand = 1;
    c->game.units_per_metre = 1.0f;
    c->game.max_head_offset_m = 0.5f;
    c->game.aim_converge_m = 10.0f;
    c->game.vm_offset_m[0] = 0.12f;
    c->game.vm_offset_m[1] = -0.18f;
    c->game.vm_offset_m[2] = 0.30f;
    c->game.scope_zoom_min = 2.5f;
    c->game.fov_margin = 1.10f;
    c->game.bob_filter = 1;
    c->game.hand_weapon = 1;
}

static inline char *vr_cfg_trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static inline int vr_cfg_float(const char *v, float lo, float hi, float *out)
{
    char *end;
    const double d = strtod(v, &end);
    if (end == v || !(d >= lo && d <= hi)) return 0;
    *out = (float)d;
    return 1;
}
static inline int vr_cfg_bool(const char *v, int *out)
{
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes")) { *out = 1; return 1; }
    if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no")) { *out = 0; return 1; }
    return 0;
}

/* One line; returns 1 if understood (or blank/comment), 0 otherwise. */
static inline int vr_config_line(VrShellConfig *c, const char *line_in)
{
    char buf[256], *line, *eq, *key, *val, *cm;
    float f;
    int b;
    strncpy(buf, line_in, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    line = vr_cfg_trim(buf);
    if (!*line || *line == '#' || *line == ';') return 1;
    if (!strncmp(line, "env ", 4)) {
        val = vr_cfg_trim(line + 4);
        if (!strchr(val, '=') || *val == '=' || c->env_count >= VR_CONFIG_MAX_ENV ||
            strlen(val) >= sizeof(c->env[0])) return 0;
        strcpy(c->env[c->env_count++], val);
        return 1;
    }
    if ((cm = strchr(line, ';')) != NULL) *cm = 0;
    if ((cm = strchr(line, '#')) != NULL) *cm = 0;
    eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = 0;
    key = vr_cfg_trim(line);
    val = vr_cfg_trim(eq + 1);
    for (cm = val; *cm; cm++) *cm = (char)tolower((unsigned char)*cm);

    if (!strcmp(key, "refresh_hz") && !strcmp(val, "auto")) { c->refresh_hz = 0.0f; return 1; }
    if (!strcmp(key, "refresh_hz")) { if (!vr_cfg_float(val, 60, 144, &f)) return 0; c->refresh_hz = f; return 1; }
    if (!strcmp(key, "resolution_scale")) { if (!vr_cfg_float(val, 0.3f, 2.0f, &f)) return 0; c->resolution_scale = f; return 1; }
    if (!strcmp(key, "multiview")) return vr_cfg_bool(val, &c->multiview);
    if (!strcmp(key, "foveation")) {
        if (!strcmp(val, "off") || !strcmp(val, "0") || !strcmp(val, "none")) c->foveation = VR_FOVEATION_OFF;
        else if (!strcmp(val, "low") || !strcmp(val, "1")) c->foveation = VR_FOVEATION_LOW;
        else if (!strcmp(val, "medium") || !strcmp(val, "2")) c->foveation = VR_FOVEATION_MEDIUM;
        else if (!strcmp(val, "high") || !strcmp(val, "3")) c->foveation = VR_FOVEATION_HIGH;
        else if (!strcmp(val, "hightop") || !strcmp(val, "high_top") || !strcmp(val, "4")) c->foveation = VR_FOVEATION_HIGH_TOP;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "game_hz")) {
        if (!strcmp(val, "30")) c->game_hz = 30;
        else if (!strcmp(val, "60")) c->game_hz = 60;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "raster_workers")) {
        if (!vr_cfg_float(val, 0, 4, &f) || f != (float)(int)f) return 0;
        c->raster_workers = (int)f;
        return 1;
    }
    if (!strcmp(key, "raster_spin_us")) {
        if (!vr_cfg_float(val, 0, 5000, &f) || f != (float)(int)f) return 0;
        c->raster_spin_us = (int)f;
        return 1;
    }
    if (!strcmp(key, "hud_distance")) { if (!vr_cfg_float(val, 0.5f, 10.0f, &f)) return 0; c->hud_distance_m = f; return 1; }
    if (!strcmp(key, "hud_width")) { if (!vr_cfg_float(val, 0.3f, 10.0f, &f)) return 0; c->hud_width_m = f; return 1; }
    if (!strcmp(key, "haptic_fire")) { if (!vr_cfg_float(val, 0.0f, 1.0f, &f)) return 0; c->haptic_fire = f; return 1; }
    if (!strcmp(key, "gun_latch")) return vr_cfg_bool(val, &c->gun_latch);
    if (!strcmp(key, "settings_menu")) {
        if (!strcmp(val, "native") || !strcmp(val, "game")) c->settings_menu = VR_SETTINGS_MENU_NATIVE;
        else if (!strcmp(val, "panel") || !strcmp(val, "floating")) c->settings_menu = VR_SETTINGS_MENU_PANEL;
        else if (!strcmp(val, "both")) c->settings_menu = VR_SETTINGS_MENU_BOTH;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "turn")) {
        if (!strcmp(val, "snap")) c->input.turn_mode = VR_TURN_SNAP;
        else if (!strcmp(val, "smooth")) c->input.turn_mode = VR_TURN_SMOOTH;
        else if (!strcmp(val, "off")) c->input.turn_mode = VR_TURN_OFF;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "snap_degrees")) { if (!vr_cfg_float(val, 5, 180, &f)) return 0; c->input.snap_deg = f; return 1; }
    if (!strcmp(key, "smooth_degrees_per_second")) { if (!vr_cfg_float(val, 10, 720, &f)) return 0; c->input.smooth_deg_per_s = f; return 1; }
    if (!strcmp(key, "deadzone")) { if (!vr_cfg_float(val, 0, 0.9f, &f)) return 0; c->input.deadzone = f; return 1; }
    if (!strcmp(key, "left_handed")) {
        if (!vr_cfg_bool(val, &b)) return 0;
        c->input.left_handed = b; c->game.aim_hand = b ? 0 : 1; return 1;
    }
    if (!strcmp(key, "aim")) {
        if (!strcmp(val, "hand")) c->game.aim_mode = VR_AIM_HAND;
        else if (!strcmp(val, "head")) c->game.aim_mode = VR_AIM_HEAD;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "locomotion")) {
        if (!strcmp(val, "head")) c->locomotion = VR_LOCO_HEAD;
        else if (!strcmp(val, "aim")) c->locomotion = VR_LOCO_AIM;
        else if (!strcmp(val, "offhand") || !strcmp(val, "off_hand") || !strcmp(val, "controller")) c->locomotion = VR_LOCO_OFFHAND;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "vr_accuracy")) {
        if (!strcmp(val, "aim")) c->input.accuracy = VR_ACCURACY_AIM;
        else if (!strcmp(val, "hip")) c->input.accuracy = VR_ACCURACY_HIP;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "melee_swing")) return vr_cfg_bool(val, &c->gest.melee_swing);
    if (!strcmp(key, "melee_swing_speed")) { if (!vr_cfg_float(val, 1.0f, 10.0f, &f)) return 0; c->gest.swing_speed = f; return 1; }
    if (!strcmp(key, "duck_crouch")) return vr_cfg_bool(val, &c->gest.duck_crouch);
    if (!strcmp(key, "duck_ratio")) { if (!vr_cfg_float(val, 0.5f, 0.9f, &f)) return 0; c->gest.duck_ratio = f; return 1; }
    if (!strcmp(key, "scope_raise")) return vr_cfg_bool(val, &c->gest.scope_raise);
    if (!strcmp(key, "scope_raise_distance")) { if (!vr_cfg_float(val, 0.03f, 0.5f, &f)) return 0; c->gest.scope_distance = f; return 1; }
    if (!strcmp(key, "two_hand")) return vr_cfg_bool(val, &c->gest.two_hand);
    if (!strcmp(key, "world_scale")) { if (!vr_cfg_float(val, 0.001f, 1000, &f)) return 0; c->game.units_per_metre = f; return 1; }
    if (!strcmp(key, "max_head_offset")) { if (!vr_cfg_float(val, 0, 3, &f)) return 0; c->game.max_head_offset_m = f; return 1; }
    if (!strcmp(key, "aim_converge")) { if (!vr_cfg_float(val, 0.5f, 1000, &f)) return 0; c->game.aim_converge_m = f; return 1; }
    if (!strcmp(key, "hand_weapon")) return vr_cfg_bool(val, &c->game.hand_weapon);
    if (!strcmp(key, "bob_filter")) return vr_cfg_bool(val, &c->game.bob_filter);
    if (!strcmp(key, "scope_zoom")) { if (!vr_cfg_float(val, 1.0f, 100, &f)) return 0; c->game.scope_zoom_min = f; return 1; }
    if (!strcmp(key, "fov_margin")) { if (!vr_cfg_float(val, 1.0f, 2.0f, &f)) return 0; c->game.fov_margin = f; return 1; }
    if (!strcmp(key, "weapon_offset")) {
        float v[3];
        if (sscanf(val, "%f %f %f", &v[0], &v[1], &v[2]) != 3) return 0;
        for (b = 0; b < 3; b++) { if (!(v[b] >= -2.0f && v[b] <= 2.0f)) return 0; c->game.vm_offset_m[b] = v[b]; }
        return 1;
    }
    return 0;
}

/* The settings the game runtime reads from its environment, exported before the game thread starts. The explicit
 * "env NAME=VALUE" lines win (exported first, with overwrite); these then only fill what is still unset, and the
 * game's own defaults (src/main.c, setenv without overwrite) come last. setenv_fn is setenv (a parameter so the
 * unit test can record the calls). */
static inline void vr_config_export_env(const VrShellConfig *c, int (*setenv_fn)(const char *, const char *, int))
{
    char v[32];
    int i;
    for (i = 0; i < c->env_count; i++) {
        char *eq, kv[160];
        snprintf(kv, sizeof(kv), "%s", c->env[i]);
        if ((eq = strchr(kv, '=')) != NULL) { *eq = 0; setenv_fn(kv, eq + 1, 1); }
    }
    /* RECOMP_BLACK_HZ=30 is not a coupled-clock rate, so with RECOMP_BLACK_60HZ=0 the title keeps its 1/30 step */
    setenv_fn("RECOMP_BLACK_HZ", c->game_hz == 60 ? "60" : "30", 0);
    setenv_fn("RECOMP_BLACK_60HZ", c->game_hz == 60 ? "1" : "0", 0);
    snprintf(v, sizeof(v), "%d", c->raster_workers + 1);      /* counts the GPU thread */
    setenv_fn("RECOMP_KELVIN_THREADS", v, 0);
    snprintf(v, sizeof(v), "%d", c->raster_spin_us);
    setenv_fn("RECOMP_KELVIN_SPIN_US", v, 0);
}

static inline void vr_config_parse(VrShellConfig *c, const char *text)
{
    while (text && *text) {
        char line[256];
        size_t n = strcspn(text, "\r\n");
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text, n);
        line[n] = 0;
        if (!vr_config_line(c, line)) c->errors++;
        text += strcspn(text, "\r\n");
        while (*text == '\r' || *text == '\n') text++;
    }
}

#endif /* VR_CONFIG_H */
