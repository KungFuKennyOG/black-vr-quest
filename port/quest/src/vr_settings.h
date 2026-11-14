/*
 * vr_settings.h - the in-headset settings panel's model (port/notes/settings-panel.md): which blackvr.ini settings
 * it shows, their ranges and steps, which apply live and which on restart, and the blackvr.ini writer that edits the
 * file in place (comments, unknown keys, env lines and line endings kept). Header-only, pure C, no OpenXR/Vulkan:
 * unit-tested in tests/xr_shell (test_settings.c). The panel itself is xr_panel.c (XR, Vulkan) + vr_panel_ui.cpp.
 *
 * Values are read and written as floats through vrs_get / vrs_set (an enum is its index, a bool 0/1); vrs_format is
 * the setting's blackvr.ini spelling, so "changed" always means "would be written differently".
 */
#ifndef VR_SETTINGS_H
#define VR_SETTINGS_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_config.h"

typedef enum {
    /* Graphics */
    VRS_RES_SCALE, VRS_FOVEATION, VRS_REFRESH, VRS_GAME_HZ, VRS_PERF, VRS_SPRITE_DEBUG,
    /* Comfort */
    VRS_TURN, VRS_SNAP_DEG, VRS_SMOOTH_DPS, VRS_LOCOMOTION, VRS_LEFT_HANDED, VRS_WORLD_SCALE, VRS_HUD_DIST, VRS_HUD_WIDTH,
    VRS_HUD_HEIGHT, VRS_TUTORIALS, VRS_LASER, VRS_SKIP_MOVIES,
    /* Gestures */
    VRS_MELEE, VRS_MELEE_SPEED, VRS_DUCK, VRS_DUCK_RATIO, VRS_SCOPE, VRS_SCOPE_DIST, VRS_TWO_HAND,
    VRS_COUNT
} VrSettingId;

enum { VRS_TAB_GRAPHICS, VRS_TAB_COMFORT, VRS_TAB_GESTURES, VRS_TAB_COUNT };
enum { VRS_FLOAT, VRS_ENUM, VRS_BOOL };
/* how a change takes effect */
enum {
    VRS_LIVE,             /* read by the shell every frame: copied into the live config at once */
    VRS_RESTART,          /* read once at startup (swapchain size, the game's environment) */
    VRS_LIVE_FOVEATION,   /* live when the eye swapchain was created foveated (XR_FB_foveation), else restart */
    VRS_LIVE_REFRESH,     /* live with XR_FB_display_refresh_rate */
    VRS_LIVE_PERF         /* live with XR_EXT_performance_settings */
};

typedef struct {
    const char *key;                 /* blackvr.ini key; env-backed settings: the variable, env_var = 1 */
    int env_var;
    const char *label, *help;
    int tab, type;
    float lo, hi, step;              /* VRS_FLOAT */
    const char *fmt;                 /* VRS_FLOAT: the ini spelling */
    int count;                       /* VRS_ENUM */
    const char *const *names;        /* VRS_ENUM: ini spellings */
    const char *const *labels;       /* VRS_ENUM: what the panel shows */
    int apply;
    const char *unit;
} VrSettingDesc;

static const char *const k_vrs_fov_names[] = { "off", "low", "medium", "high", "hightop" };
static const char *const k_vrs_fov_labels[] = { "Off", "Low", "Medium", "High", "Hi-Top" };
static const char *const k_vrs_refresh_names[] = { "auto", "72", "80", "90", "120" };
static const char *const k_vrs_refresh_labels[] = { "Auto", "72", "80", "90", "120" };
static const float k_vrs_refresh_hz[] = { 0.0f, 72.0f, 80.0f, 90.0f, 120.0f };
static const char *const k_vrs_hz_names[] = { "30", "60" };
static const char *const k_vrs_perf_names[] = { "sustained_high", "boost", "boost_cpu", "boost_gpu" };
static const char *const k_vrs_perf_labels[] = { "Sustained", "Boost", "CPU boost", "GPU boost" };
static const char *const k_vrs_turn_names[] = { "snap", "smooth", "off" };
static const char *const k_vrs_turn_labels[] = { "Snap", "Smooth", "Off" };
static const char *const k_vrs_loco_names[] = { "head", "aim", "offhand" };
static const char *const k_vrs_loco_labels[] = { "Head", "Gun hand", "Off hand" };

static const VrSettingDesc k_vrs[VRS_COUNT] = {
    { "resolution_scale", 0, "Resolution scale", "Eye image size vs the headset's recommendation (GPU cost ~ scale squared)",
      VRS_TAB_GRAPHICS, VRS_FLOAT, 0.6f, 1.4f, 0.05f, "%.2f", 0, NULL, NULL, VRS_RESTART, "x" },
    { "foveation", 0, "Foveation", "Fixed foveated rendering level of the eye images",
      VRS_TAB_GRAPHICS, VRS_ENUM, 0, 0, 0, NULL, 5, k_vrs_fov_names, k_vrs_fov_labels, VRS_LIVE_FOVEATION, NULL },
    { "refresh_hz", 0, "Refresh rate", "Display rate in Hz (auto: 120 for a 60 Hz game, 90 for 30)",
      VRS_TAB_GRAPHICS, VRS_ENUM, 0, 0, 0, NULL, 5, k_vrs_refresh_names, k_vrs_refresh_labels, VRS_LIVE_REFRESH, NULL },
    { "game_hz", 0, "Game rate", "The game's own frame rate: 60 is smoother, 30 the steadiest",
      VRS_TAB_GRAPHICS, VRS_ENUM, 0, 0, 0, NULL, 2, k_vrs_hz_names, k_vrs_hz_names, VRS_RESTART, NULL },
    { "BLACKVR_PERF", 1, "Performance", "CPU/GPU clock level requested from the runtime (boost may throttle)",
      VRS_TAB_GRAPHICS, VRS_ENUM, 0, 0, 0, NULL, 4, k_vrs_perf_names, k_vrs_perf_labels, VRS_LIVE_PERF, NULL },
    { "BLACKVR_SPRITE_DEBUG", 1, "Effects debug tint", "Tints the screen-space effect classification (debugging)",
      VRS_TAB_GRAPHICS, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_RESTART, NULL },

    { "turn", 0, "Turning", "Right stick left/right",
      VRS_TAB_COMFORT, VRS_ENUM, 0, 0, 0, NULL, 3, k_vrs_turn_names, k_vrs_turn_labels, VRS_LIVE, NULL },
    { "snap_degrees", 0, "Snap angle", "Degrees per snap turn",
      VRS_TAB_COMFORT, VRS_FLOAT, 10.0f, 90.0f, 5.0f, "%.0f", 0, NULL, NULL, VRS_LIVE, "deg" },
    { "smooth_degrees_per_second", 0, "Smooth turn speed", "Degrees per second at full stick",
      VRS_TAB_COMFORT, VRS_FLOAT, 30.0f, 360.0f, 10.0f, "%.0f", 0, NULL, NULL, VRS_LIVE, "deg/s" },
    { "locomotion", 0, "Move direction", "The left stick moves you where you look, where the gun points, or the off hand points",
      VRS_TAB_COMFORT, VRS_ENUM, 0, 0, 0, NULL, 3, k_vrs_loco_names, k_vrs_loco_labels, VRS_LIVE, NULL },
    { "left_handed", 0, "Left-handed", "Gun in the left hand (the controls mirror)",
      VRS_TAB_COMFORT, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },
    { "world_scale", 0, "World scale", "Game units per metre: higher makes the world feel smaller",
      VRS_TAB_COMFORT, VRS_FLOAT, 0.5f, 2.0f, 0.05f, "%.2f", 0, NULL, NULL, VRS_LIVE, "x" },
    { "hud_distance", 0, "HUD distance", "How far away the HUD, the menus and the cutscene screen float",
      VRS_TAB_COMFORT, VRS_FLOAT, 1.0f, 6.0f, 0.25f, "%.2f", 0, NULL, NULL, VRS_LIVE, "m" },
    { "hud_width", 0, "HUD size", "How wide the HUD, the menus and the cutscene screen are",
      VRS_TAB_COMFORT, VRS_FLOAT, 0.8f, 4.0f, 0.1f, "%.1f", 0, NULL, NULL, VRS_LIVE, "m" },
    { "hud_height", 0, "HUD height", "Tilts the in-game HUD below eye level (negative) or above it; menus stay level",
      VRS_TAB_COMFORT, VRS_FLOAT, -30.0f, 10.0f, 1.0f, "%.0f", 0, NULL, NULL, VRS_LIVE, "deg" },
    { "tutorial_hints", 0, "Tutorial hints", "Mission 1's control hints every time (off: only until you beat it, as the game does)",
      VRS_TAB_COMFORT, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },
    { "laser", 0, "Laser sight", "A laser from the gun's muzzle along the barrel, to where your shot goes",
      VRS_TAB_COMFORT, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },
    { "skip_movies", 0, "Skip cutscenes", "The movie cutscenes end at once (scenes played in the game's own engine still show)",
      VRS_TAB_COMFORT, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },

    { "melee_swing", 0, "Swing to melee", "A fast swing of the gun hand is a melee attack",
      VRS_TAB_GESTURES, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },
    { "melee_swing_speed", 0, "Swing speed", "Hand speed (relative to the head) that counts as a swing",
      VRS_TAB_GESTURES, VRS_FLOAT, 1.0f, 10.0f, 0.1f, "%.1f", 0, NULL, NULL, VRS_LIVE, "m/s" },
    { "duck_crouch", 0, "Duck to crouch", "Ducking crouches, standing up stands",
      VRS_TAB_GESTURES, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },
    { "duck_ratio", 0, "Duck depth", "Crouch below this fraction of your standing head height",
      VRS_TAB_GESTURES, VRS_FLOAT, 0.5f, 0.9f, 0.01f, "%.2f", 0, NULL, NULL, VRS_LIVE, "x" },
    { "scope_raise", 0, "Raise to scope", "Raising a scoped weapon to the eye looks through the scope (off: gun-hand grip)",
      VRS_TAB_GESTURES, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },
    { "scope_raise_distance", 0, "Scope distance", "Distance from the rear sight to the eye",
      VRS_TAB_GESTURES, VRS_FLOAT, 0.03f, 0.5f, 0.01f, "%.2f", 0, NULL, NULL, VRS_LIVE, "m" },
    { "two_hand", 0, "Two-handed aim", "Off-hand grip near the gun steadies the aim",
      VRS_TAB_GESTURES, VRS_BOOL, 0, 1, 1, NULL, 0, NULL, NULL, VRS_LIVE, NULL },
};

/* ---- env-backed settings: the config's "env NAME=VALUE" list ---------------------------------------------------- */

/* The value of NAME in the config's env lines (the last one wins, as setenv in order would), or NULL. */
static inline const char *vrs_env_get(const VrShellConfig *c, const char *name)
{
    const size_t n = strlen(name);
    const char *v = NULL;
    int i;
    for (i = 0; i < c->env_count; i++)
        if (!strncmp(c->env[i], name, n) && c->env[i][n] == '=') v = c->env[i] + n + 1;
    return v;
}
/* Sets NAME (every entry of it); value NULL removes them. Returns 0 when the list is full. */
static inline int vrs_env_set(VrShellConfig *c, const char *name, const char *value)
{
    const size_t n = strlen(name);
    int i, found = 0, o = 0;
    for (i = 0; i < c->env_count; i++) {
        if (!strncmp(c->env[i], name, n) && c->env[i][n] == '=') {
            if (!value) continue;
            snprintf(c->env[i], sizeof(c->env[i]), "%s=%s", name, value);
            found = 1;
        }
        if (o != i) memcpy(c->env[o], c->env[i], sizeof(c->env[o]));
        o++;
    }
    c->env_count = o;
    if (!value || found) return 1;
    if (c->env_count >= VR_CONFIG_MAX_ENV) return 0;
    snprintf(c->env[c->env_count++], sizeof(c->env[0]), "%s=%s", name, value);
    return 1;
}
static inline int vrs_enum_index(const char *v, const char *const *names, int count, int dflt)
{
    int i;
    if (!v) return dflt;
    for (i = 0; i < count; i++) if (!strcmp(v, names[i])) return i;
    return dflt;
}

/* ---- get / set / format ----------------------------------------------------------------------------------------- */

static inline float vrs_get(const VrShellConfig *c, int id)
{
    switch (id) {
    case VRS_RES_SCALE: return c->resolution_scale;
    case VRS_FOVEATION: return (float)(c->foveation >= 0 && c->foveation <= 4 ? c->foveation : 2);
    case VRS_REFRESH: {
        int i, best = 1;                 /* a rate the list lacks (e.g. 60) shows as the nearest one */
        if (c->refresh_hz <= 0.0f) return 0.0f;
        for (i = 2; i < 5; i++)
            if (fabsf(k_vrs_refresh_hz[i] - c->refresh_hz) < fabsf(k_vrs_refresh_hz[best] - c->refresh_hz)) best = i;
        return (float)best;
    }
    case VRS_GAME_HZ: return c->game_hz == 30 ? 0.0f : 1.0f;
    case VRS_PERF: return (float)vrs_enum_index(vrs_env_get(c, "BLACKVR_PERF"), k_vrs_perf_names, 4, 0);
    case VRS_SPRITE_DEBUG: { const char *v = vrs_env_get(c, "BLACKVR_SPRITE_DEBUG"); return v && *v && *v != '0' ? 1.0f : 0.0f; }
    case VRS_TURN: return (float)c->input.turn_mode;
    case VRS_SNAP_DEG: return c->input.snap_deg;
    case VRS_SMOOTH_DPS: return c->input.smooth_deg_per_s;
    case VRS_LOCOMOTION: return (float)c->locomotion;
    case VRS_LEFT_HANDED: return c->input.left_handed ? 1.0f : 0.0f;
    case VRS_WORLD_SCALE: return c->game.units_per_metre;
    case VRS_HUD_DIST: return c->hud_distance_m;
    case VRS_HUD_WIDTH: return c->hud_width_m;
    case VRS_HUD_HEIGHT: return c->hud_pitch_deg;
    case VRS_TUTORIALS: return c->game.tutorial_hints ? 1.0f : 0.0f;
    case VRS_LASER: return c->game.laser ? 1.0f : 0.0f;
    case VRS_SKIP_MOVIES: return c->game.skip_movies ? 1.0f : 0.0f;
    case VRS_MELEE: return c->gest.melee_swing ? 1.0f : 0.0f;
    case VRS_MELEE_SPEED: return c->gest.swing_speed;
    case VRS_DUCK: return c->gest.duck_crouch ? 1.0f : 0.0f;
    case VRS_DUCK_RATIO: return c->gest.duck_ratio;
    case VRS_SCOPE: return c->gest.scope_raise ? 1.0f : 0.0f;
    case VRS_SCOPE_DIST: return c->gest.scope_distance;
    case VRS_TWO_HAND: return c->gest.two_hand ? 1.0f : 0.0f;
    default: return 0.0f;
    }
}

/* A value as the panel may set it: in range, on the step grid (from lo), an enum index in range, a bool 0/1. */
static inline float vrs_clamp(int id, float v)
{
    const VrSettingDesc *d = &k_vrs[id];
    if (id < 0 || id >= VRS_COUNT || !isfinite(v)) return 0.0f;
    if (d->type == VRS_BOOL) return v != 0.0f ? 1.0f : 0.0f;
    if (d->type == VRS_ENUM) {
        const int i = (int)lroundf(v);
        return (float)(i < 0 ? 0 : i >= d->count ? d->count - 1 : i);
    }
    if (v < d->lo) v = d->lo;
    if (v > d->hi) v = d->hi;
    if (d->step > 0.0f) {
        v = d->lo + roundf((v - d->lo) / d->step) * d->step;
        if (v > d->hi) v = d->hi;
        /* the ini spelling's precision: 0.6 + 4 x 0.05 is exactly 0.80 */
        {
            char b[32];
            snprintf(b, sizeof(b), d->fmt ? d->fmt : "%g", (double)v);
            v = (float)strtod(b, NULL);
        }
    }
    return v;
}

static inline void vrs_set(VrShellConfig *c, int id, float v)
{
    v = vrs_clamp(id, v);
    switch (id) {
    case VRS_RES_SCALE: c->resolution_scale = v; break;
    case VRS_FOVEATION: c->foveation = (int)v; break;
    case VRS_REFRESH: c->refresh_hz = k_vrs_refresh_hz[(int)v]; break;
    case VRS_GAME_HZ: c->game_hz = v != 0.0f ? 60 : 30; break;
    case VRS_PERF:
        /* the default level needs no line; an existing line keeps its place with the new value */
        if ((int)v == 0 && !vrs_env_get(c, "BLACKVR_PERF")) break;
        vrs_env_set(c, "BLACKVR_PERF", k_vrs_perf_names[(int)v]);
        break;
    case VRS_SPRITE_DEBUG:
        if (v == 0.0f && !vrs_env_get(c, "BLACKVR_SPRITE_DEBUG")) break;
        vrs_env_set(c, "BLACKVR_SPRITE_DEBUG", v != 0.0f ? "1" : "0");
        break;
    case VRS_TURN: c->input.turn_mode = (VrTurnMode)(int)v; break;
    case VRS_SNAP_DEG: c->input.snap_deg = v; break;
    case VRS_SMOOTH_DPS: c->input.smooth_deg_per_s = v; break;
    case VRS_LOCOMOTION: c->locomotion = (int)v; break;
    case VRS_LEFT_HANDED: c->input.left_handed = v != 0.0f; c->game.aim_hand = v != 0.0f ? 0 : 1; break;
    case VRS_WORLD_SCALE: c->game.units_per_metre = v; break;
    case VRS_HUD_DIST: c->hud_distance_m = v; break;
    case VRS_HUD_WIDTH: c->hud_width_m = v; break;
    case VRS_HUD_HEIGHT: c->hud_pitch_deg = v; break;
    case VRS_TUTORIALS: c->game.tutorial_hints = v != 0.0f; break;
    case VRS_LASER: c->game.laser = v != 0.0f; break;
    case VRS_SKIP_MOVIES: c->game.skip_movies = v != 0.0f; break;
    case VRS_MELEE: c->gest.melee_swing = v != 0.0f; break;
    case VRS_MELEE_SPEED: c->gest.swing_speed = v; break;
    case VRS_DUCK: c->gest.duck_crouch = v != 0.0f; break;
    case VRS_DUCK_RATIO: c->gest.duck_ratio = v; break;
    case VRS_SCOPE: c->gest.scope_raise = v != 0.0f; break;
    case VRS_SCOPE_DIST: c->gest.scope_distance = v; break;
    case VRS_TWO_HAND: c->gest.two_hand = v != 0.0f; break;
    default: break;
    }
}

/* Copies one setting's exact value (not through the panel's grid: a value the file had stays what it was). */
static inline void vrs_copy(VrShellConfig *dst, const VrShellConfig *src, int id)
{
    switch (id) {
    case VRS_RES_SCALE: dst->resolution_scale = src->resolution_scale; break;
    case VRS_FOVEATION: dst->foveation = src->foveation; break;
    case VRS_REFRESH: dst->refresh_hz = src->refresh_hz; break;
    case VRS_GAME_HZ: dst->game_hz = src->game_hz; break;
    case VRS_PERF: case VRS_SPRITE_DEBUG: vrs_env_set(dst, k_vrs[id].key, vrs_env_get(src, k_vrs[id].key)); break;
    case VRS_TURN: dst->input.turn_mode = src->input.turn_mode; break;
    case VRS_SNAP_DEG: dst->input.snap_deg = src->input.snap_deg; break;
    case VRS_SMOOTH_DPS: dst->input.smooth_deg_per_s = src->input.smooth_deg_per_s; break;
    case VRS_LOCOMOTION: dst->locomotion = src->locomotion; break;
    case VRS_LEFT_HANDED: dst->input.left_handed = src->input.left_handed; dst->game.aim_hand = src->game.aim_hand; break;
    case VRS_WORLD_SCALE: dst->game.units_per_metre = src->game.units_per_metre; break;
    case VRS_HUD_DIST: dst->hud_distance_m = src->hud_distance_m; break;
    case VRS_HUD_WIDTH: dst->hud_width_m = src->hud_width_m; break;
    case VRS_HUD_HEIGHT: dst->hud_pitch_deg = src->hud_pitch_deg; break;
    case VRS_TUTORIALS: dst->game.tutorial_hints = src->game.tutorial_hints; break;
    case VRS_LASER: dst->game.laser = src->game.laser; break;
    case VRS_SKIP_MOVIES: dst->game.skip_movies = src->game.skip_movies; break;
    case VRS_MELEE: dst->gest.melee_swing = src->gest.melee_swing; break;
    case VRS_MELEE_SPEED: dst->gest.swing_speed = src->gest.swing_speed; break;
    case VRS_DUCK: dst->gest.duck_crouch = src->gest.duck_crouch; break;
    case VRS_DUCK_RATIO: dst->gest.duck_ratio = src->gest.duck_ratio; break;
    case VRS_SCOPE: dst->gest.scope_raise = src->gest.scope_raise; break;
    case VRS_SCOPE_DIST: dst->gest.scope_distance = src->gest.scope_distance; break;
    case VRS_TWO_HAND: dst->gest.two_hand = src->gest.two_hand; break;
    default: break;
    }
}

/* The setting's blackvr.ini value (the text after "key = ", or after "env NAME="). */
static inline void vrs_format(const VrShellConfig *c, int id, char *out, size_t cap)
{
    const VrSettingDesc *d = &k_vrs[id];
    if (id == VRS_REFRESH) {
        if (c->refresh_hz <= 0.0f) snprintf(out, cap, "auto");
        else snprintf(out, cap, "%g", (double)c->refresh_hz);
    } else if (id == VRS_PERF) {
        const char *v = vrs_env_get(c, "BLACKVR_PERF");
        snprintf(out, cap, "%s", v ? v : "sustained_high");
    } else if (id == VRS_SPRITE_DEBUG) {
        snprintf(out, cap, "%d", (int)vrs_get(c, id));
    } else if (d->type == VRS_FLOAT) {
        snprintf(out, cap, d->fmt, (double)vrs_get(c, id));
    } else if (d->type == VRS_BOOL) {
        if (id == VRS_LEFT_HANDED) snprintf(out, cap, "%d", (int)vrs_get(c, id));
        else snprintf(out, cap, "%s", vrs_get(c, id) != 0.0f ? "on" : "off");
    } else {
        snprintf(out, cap, "%s", d->names[(int)vrs_get(c, id)]);
    }
}

static inline int vrs_differs(const VrShellConfig *a, const VrShellConfig *b, int id)
{
    char x[64], y[64];
    vrs_format(a, id, x, sizeof(x));
    vrs_format(b, id, y, sizeof(y));
    return strcmp(x, y) != 0;
}

/* ---- live vs restart ---------------------------------------------------------------------------------------------- */

typedef struct {
    int has_refresh;          /* XR_FB_display_refresh_rate */
    int foveation_supported;  /* XR_FB_foveation (+ Vulkan, configuration, update state) and the GPU's FDM */
    int foveation_live;       /* the eye swapchain(s) were created with the foveation flag: a new level is live */
    int has_perf;             /* XR_EXT_performance_settings */
} VrsCaps;

enum { VRS_AVAILABLE = 0, VRS_UNAVAILABLE = 1 };   /* vrs_available */

static inline int vrs_is_live(int id, const VrsCaps *k)
{
    switch (k_vrs[id].apply) {
    case VRS_LIVE: return 1;
    case VRS_LIVE_FOVEATION: return k->foveation_live;
    case VRS_LIVE_REFRESH: return k->has_refresh;
    case VRS_LIVE_PERF: return k->has_perf;
    default: return 0;
    }
}
/* 0: the device has what the setting drives (a restart may still be needed), 1: it does nothing on this device */
static inline int vrs_available(int id, const VrsCaps *k)
{
    switch (id) {
    case VRS_FOVEATION: return k->foveation_supported ? VRS_AVAILABLE : VRS_UNAVAILABLE;
    case VRS_REFRESH: return k->has_refresh ? VRS_AVAILABLE : VRS_UNAVAILABLE;
    case VRS_PERF: return k->has_perf ? VRS_AVAILABLE : VRS_UNAVAILABLE;
    default: return VRS_AVAILABLE;
    }
}

/* What the shell must do after vrs_apply_live copied a value */
enum { VRS_DO_REFRESH = 1, VRS_DO_FOVEATION = 2, VRS_DO_PERF = 4, VRS_DO_HANDS = 8 };

/* Copies every live setting that differs from edit into live (the shell's running config); restart settings stay as
 * the process started. Returns the VRS_DO_* side effects. */
static inline unsigned vrs_apply_live(VrShellConfig *live, const VrShellConfig *edit, const VrsCaps *k)
{
    unsigned todo = 0;
    int id;
    for (id = 0; id < VRS_COUNT; id++) {
        if (!vrs_is_live(id, k) || !vrs_differs(live, edit, id)) continue;
        vrs_copy(live, edit, id);
        if (id == VRS_REFRESH) todo |= VRS_DO_REFRESH;
        if (id == VRS_FOVEATION) todo |= VRS_DO_FOVEATION;
        if (id == VRS_PERF) todo |= VRS_DO_PERF;
        if (id == VRS_LEFT_HANDED) todo |= VRS_DO_HANDS;
    }
    return todo;
}

/* A setting edited away from what the process started with that only a restart applies. */
static inline int vrs_pending_restart(const VrShellConfig *boot, const VrShellConfig *edit, const VrsCaps *k, int id)
{
    return !vrs_is_live(id, k) && vrs_available(id, k) == VRS_AVAILABLE && vrs_differs(boot, edit, id);
}

/* Every panel setting back to its default (everything else in the config is left alone). */
static inline void vrs_reset_defaults(VrShellConfig *c)
{
    VrShellConfig d;
    int id;
    vr_config_defaults(&d);
    for (id = 0; id < VRS_COUNT; id++) vrs_copy(c, &d, id);
}

/* ---- the blackvr.ini writer ------------------------------------------------------------------------------------- */

typedef struct { char *p; size_t n, cap; int overflow; } VrsBuf;
static inline void vrs_put(VrsBuf *b, const char *s, size_t n)
{
    if (b->overflow || b->n + n + 1 > b->cap) { b->overflow = 1; return; }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static inline void vrs_puts(VrsBuf *b, const char *s) { vrs_put(b, s, strlen(s)); }

/* Which managed setting a line sets ("key = ..." or "env NAME=..."), -1 for none; *vs / *ve: the value's span (ve
 * before any trailing spaces and comment), *ce: where the comment starts (or the line end). */
static inline int vrs_line_setting(const char *l, size_t len, size_t *vs, size_t *ve, size_t *ce)
{
    size_t i = 0, ks, ke;
    int id, env = 0;
    while (i < len && (l[i] == ' ' || l[i] == '\t')) i++;
    if (i >= len || l[i] == '#' || l[i] == ';') return -1;
    if (len - i > 4 && !strncmp(l + i, "env ", 4)) {
        env = 1;
        i += 4;
        while (i < len && (l[i] == ' ' || l[i] == '\t')) i++;
    }
    ks = i;
    while (i < len && l[i] != '=' && l[i] != ' ' && l[i] != '\t') i++;
    ke = i;
    while (i < len && (l[i] == ' ' || l[i] == '\t')) i++;
    if (i >= len || l[i] != '=' || ke == ks) return -1;
    i++;
    if (!env) while (i < len && (l[i] == ' ' || l[i] == '\t')) i++;
    *vs = i;
    if (env) {
        *ce = len;                     /* an env value runs to the end of the line (vr_config_line) */
    } else {
        size_t j = i;
        while (j < len && l[j] != ';' && l[j] != '#') j++;
        *ce = j;
    }
    *ve = *ce;
    while (*ve > *vs && (l[*ve - 1] == ' ' || l[*ve - 1] == '\t')) (*ve)--;
    for (id = 0; id < VRS_COUNT; id++)
        if (k_vrs[id].env_var == env && strlen(k_vrs[id].key) == ke - ks && !strncmp(l + ks, k_vrs[id].key, ke - ks))
            return id;
    return -1;
}

/* Rewrites blackvr.ini text `old` (NULL: no file yet) for the settings in `edit`: every panel setting whose ini
 * value differs from what `old` gives it is changed in place on each line that sets it (the key, the spacing, a
 * trailing comment and its column kept), or appended when no line sets it. Every other line -- comments, unknown or
 * other keys, env lines, blank lines -- is copied byte for byte, with its own line ending. Returns the length written
 * to out (NUL-terminated), or -1 when cap is too small. */
static inline int vrs_ini_write(const char *old, const VrShellConfig *edit, char *out, size_t cap)
{
    VrShellConfig file;
    VrsBuf b;
    int changed[VRS_COUNT], seen[VRS_COUNT], id, crlf = 0;
    const char *p = old ? old : "";
    char val[64];
    b.p = out; b.n = 0; b.cap = cap; b.overflow = 0;
    if (!cap) return -1;
    out[0] = 0;
    vr_config_defaults(&file);
    vr_config_parse(&file, p);
    for (id = 0; id < VRS_COUNT; id++) { changed[id] = vrs_differs(&file, edit, id); seen[id] = 0; }
    if (strstr(p, "\r\n")) crlf = 1;
    if (!*p) vrs_puts(&b, "# BLACK VR settings. Lines are key = value; # starts a comment.\n");
    while (*p) {
        const size_t len = strcspn(p, "\r\n");
        size_t eol = len, vs, ve, ce;
        while (p[eol] == '\r') eol++;
        if (p[eol] == '\n') eol++;
        id = vrs_line_setting(p, len, &vs, &ve, &ce);
        if (id >= 0 && changed[id]) {
            size_t nl, pad;
            seen[id] = 1;
            vrs_format(edit, id, val, sizeof(val));
            nl = strlen(val);
            vrs_put(&b, p, vs);
            vrs_put(&b, val, nl);
            if (ce < len) {
                /* keep the comment where it was when the value got shorter, one space at least */
                const size_t old_span = ce - vs;
                pad = old_span > nl ? old_span - nl : 1;
                if (pad < 1) pad = 1;
                while (pad--) vrs_put(&b, " ", 1);
                vrs_put(&b, p + ce, len - ce);
            }
            vrs_put(&b, p + len, eol - len);
        } else {
            if (id >= 0) seen[id] = 1;
            vrs_put(&b, p, eol);
        }
        p += eol;
    }
    for (id = 0; id < VRS_COUNT; id++) {
        if (!changed[id] || seen[id]) continue;
        if (b.n && b.p[b.n - 1] != '\n') vrs_puts(&b, crlf ? "\r\n" : "\n");
        vrs_format(edit, id, val, sizeof(val));
        if (k_vrs[id].env_var) { vrs_puts(&b, "env "); vrs_puts(&b, k_vrs[id].key); vrs_puts(&b, "="); }
        else { vrs_puts(&b, k_vrs[id].key); vrs_puts(&b, " = "); }
        vrs_puts(&b, val);
        vrs_puts(&b, crlf ? "\r\n" : "\n");
    }
    return b.overflow ? -1 : (int)b.n;
}


/* ---- the in-menu page (src/black_vr_menu.inc via vr_bridge.h vr_menu_*): sections, rows, value text, stepping ---- */

enum { VRS_SEC_GRAPHICS, VRS_SEC_COMFORT, VRS_SEC_GESTURES, VRS_SEC_INFO, VRS_SEC_COUNT };
static const char *const k_vrs_sec_names[VRS_SEC_COUNT] = { "GRAPHICS", "COMFORT", "GESTURES", "INFO" };

/* Value text as the game's menu shows it (capitals). restart_note: append "(RESTART)" when the change applies at the
 * next start; unavailable settings read "N/A". */
static inline void vrs_menu_value(const VrShellConfig *c, const VrShellConfig *boot, const VrsCaps *k, int id, char *out,
                                  size_t cap)
{
    const VrSettingDesc *d = &k_vrs[id];
    char v[48];
    size_t i;
    if (vrs_available(id, k) != VRS_AVAILABLE) { snprintf(out, cap, "N/A"); return; }
    if (d->type == VRS_FLOAT) {
        snprintf(v, sizeof(v), d->fmt, (double)vrs_get(c, id));
        if (d->unit && strcmp(d->unit, "x")) { size_t n = strlen(v); snprintf(v + n, sizeof(v) - n, " %s", d->unit); }
        else if (d->unit) { size_t n = strlen(v); snprintf(v + n, sizeof(v) - n, "X"); }
    } else if (d->type == VRS_BOOL) {
        snprintf(v, sizeof(v), "%s", vrs_get(c, id) != 0.0f ? "ON" : "OFF");
    } else {
        const int e = (int)vrs_get(c, id);
        snprintf(v, sizeof(v), "%s%s", d->labels[e], id == VRS_REFRESH && e ? " HZ" : "");
    }
    for (i = 0; v[i]; i++) if (v[i] >= 'a' && v[i] <= 'z') v[i] = (char)(v[i] - 32);
    if (boot && vrs_pending_restart(boot, c, k, id)) snprintf(out, cap, "%s (RESTART)", v);
    else snprintf(out, cap, "%s", v);
}

/* One menu step on a setting: dir -1 / +1 (left / right), 0 (confirm). Enums cycle (round), on/off toggles, numbers
 * move one step and stop at the ends. Returns 1 when the value changed. */
static inline int vrs_step(VrShellConfig *c, int id, int dir)
{
    const VrSettingDesc *d = &k_vrs[id];
    char before[64], after[64];
    vrs_format(c, id, before, sizeof(before));
    if (d->type == VRS_BOOL) {
        vrs_set(c, id, vrs_get(c, id) != 0.0f ? 0.0f : 1.0f);
    } else if (d->type == VRS_ENUM) {
        const int n = d->count, e = (int)vrs_get(c, id);
        vrs_set(c, id, (float)(((e + (dir < 0 ? -1 : 1)) % n + n) % n));
    } else {
        if (dir == 0) return 0;
        vrs_set(c, id, vrs_get(c, id) + (float)dir * d->step);
    }
    vrs_format(c, id, after, sizeof(after));
    return strcmp(before, after) != 0;
}

/* The settings of a section, in table order; returns the count. */
static inline int vrs_section_settings(int section, int *ids, int max)
{
    int id, n = 0;
    for (id = 0; id < VRS_COUNT && n < max; id++)
        if (k_vrs[id].tab == section) ids[n++] = id;
    return n;
}

#endif /* VR_SETTINGS_H */
