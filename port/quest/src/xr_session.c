/*
 * xr_session.c - OpenXR instance, session, spaces, actions and the frame loop.
 *
 * Frame loop (XR thread, android_main):
 *   xrWaitFrame -> input sync / poses -> xrBeginFrame -> layers -> xrEndFrame
 *
 * Layers
 *   STEREO      projection layer from the eye swapchain(s). With the renderer
 *               running, the shell submits the most recently RELEASED eye
 *               images with the poses they were rendered with (the renderer
 *               runs at the game's rate, the compositor reprojects to the
 *               current head pose every display frame), plus the HUD quad
 *               layer released with them (head-locked, premultiplied alpha).
 *               With the renderer stubbed, or until it delivers its first
 *               frame, the shell draws the test pattern itself each frame.
 *   FLAT_MENU   quad layer, world-locked 2.2 m in front of the recentre
 *   FLAT_CINEMA origin, 2.0 m wide (4:3), black void around it.
 *   FLAT_SCOPE  quad layer, head-locked 0.9 m ahead (scope view).
 * With the renderer running, the mode is that of what it released last
 * (eye images or the flat image), not the game's current wish: the layers
 * always show one rendered frame, never a mix.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "xr_shell.h"
#include "vr_math.h"
#include "xr_result_names.h"

#define XRCHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { XLOGE("%s failed: %d %s (%s:%d)", #x, r_, xr_result_name(r_), __FILE__, __LINE__); xrs_note_failure(#x, (int)r_, 0, __FILE__, __LINE__); return 0; } } while (0)
#define XRWARN(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) XLOGW("%s failed: %d", #x, r_); } while (0)

/* The last failed call of the startup, for the PREFLIGHT openxr / vulkan-device / openxr-session lines. */
static char g_fail_text[320];
static int g_fail_code;

void xrs_note_failure(const char *call, int code, int is_vk, const char *file, int line)
{
    const char *f = strrchr(file, '/');
    char c[160];
    snprintf(c, sizeof(c), "%s", call);
    if (strlen(call) >= sizeof(c)) strcpy(c + sizeof(c) - 4, "...");
    snprintf(g_fail_text, sizeof(g_fail_text), "%s -> %d %s (%s:%d)", c, code,
             is_vk ? vk_result_name(code) : xr_result_name(code), f ? f + 1 : file, line);
    g_fail_code = code;
}
const char *xrs_failure_text(void) { return g_fail_text; }
int xrs_failure_code(void) { return g_fail_code; }
void xrs_failure_clear(void) { g_fail_text[0] = 0; g_fail_code = 0; }

static int ext_present(const XrExtensionProperties *e, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static XrPosef identity_pose(void)
{
    XrPosef p;
    memset(&p, 0, sizeof(p));
    p.orientation.w = 1.0f;
    return p;
}

int xrs_instance_create(XrShell *s)
{
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    XrExtensionProperties *ext;
    uint32_t n = 0, i, en = 0;
    const char *want[24];
    XrInstanceCreateInfoAndroidKHR ia = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    XrSystemGetInfo sg = { XR_TYPE_SYSTEM_GET_INFO };
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };

    /* XR_KHR_loader_init_android: the loader needs the JavaVM and the Activity */
    XRCHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader));
    li.applicationVM = s->app->activity->vm;
    li.applicationContext = s->app->activity->clazz;
    XRCHECK(init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li));

    XRCHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL));
    ext = (XrExtensionProperties *)calloc(n ? n : 1, sizeof(*ext));
    for (i = 0; i < n; i++) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    XRCHECK(xrEnumerateInstanceExtensionProperties(NULL, n, &n, ext));
    if (!ext_present(ext, n, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) { XLOGE("runtime lacks XR_KHR_vulkan_enable2"); free(ext); return 0; }
    want[en++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    if (ext_present(ext, n, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) want[en++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    xrs_instance_extensions(s, ext, n, want, &en);
    free(ext);

    ia.applicationVM = s->app->activity->vm;
    ia.applicationActivity = s->app->activity->clazz;
    ci.next = &ia;
    strncpy(ci.applicationInfo.applicationName, "BLACK VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "xboxrecomp", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = en;
    ci.enabledExtensionNames = want;
    XRCHECK(xrCreateInstance(&ci, &s->instance));

    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(xrGetSystem(s->instance, &sg, &s->system));
    XRCHECK(xrGetSystemProperties(s->instance, s->system, &sp));
    XLOGI("OpenXR system: %s (vendor %u), max layers %u", sp.systemName, sp.vendorId, sp.graphicsProperties.maxLayerCount);

    {
        uint32_t vn = 0;
        XRCHECK(xrEnumerateViewConfigurationViews(s->instance, s->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vn, NULL));
        if (vn != VR_EYE_COUNT) { XLOGE("expected 2 views, got %u", vn); return 0; }
        for (i = 0; i < VR_EYE_COUNT; i++) s->config_views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        XRCHECK(xrEnumerateViewConfigurationViews(s->instance, s->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vn, &vn, s->config_views));
    }
    xrs_instance_procs(s);
    return 1;
}

void xrs_instance_extensions(XrShell *s, const XrExtensionProperties *ext, uint32_t n, const char **want, uint32_t *en)
{
    if ((s->has_refresh_rate = ext_present(ext, n, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))) want[(*en)++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    if ((s->has_perf_settings = ext_present(ext, n, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))) want[(*en)++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    if ((s->has_timespec = ext_present(ext, n, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME))) want[(*en)++] = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
    if ((s->has_thread_settings = ext_present(ext, n, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))) want[(*en)++] = XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME;
    if (ext_present(ext, n, XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME)) want[(*en)++] = XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME;
    if ((s->has_format_list = ext_present(ext, n, XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))) want[(*en)++] = XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME;
    /* fixed foveated rendering: the profile (level) is the _configuration extension's, it reaches a swapchain through
     * _swapchain_update_state; a Vulkan swapchain needs the fragment-density-map variant (_vulkan) */
    s->has_foveation = ext_present(ext, n, XR_FB_FOVEATION_EXTENSION_NAME) &&
                       ext_present(ext, n, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) &&
                       ext_present(ext, n, XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) &&
                       ext_present(ext, n, XR_FB_FOVEATION_VULKAN_EXTENSION_NAME);
    if (s->has_foveation) {
        want[(*en)++] = XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME;
        want[(*en)++] = XR_FB_FOVEATION_EXTENSION_NAME;
        want[(*en)++] = XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME;
        want[(*en)++] = XR_FB_FOVEATION_VULKAN_EXTENSION_NAME;
    }
}

void xrs_instance_procs(XrShell *s)
{
    if (s->has_refresh_rate) {
        xrGetInstanceProcAddr(s->instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&s->xrRequestDisplayRefreshRateFB);
        xrGetInstanceProcAddr(s->instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&s->xrEnumerateDisplayRefreshRatesFB);
    }
    if (s->has_perf_settings)
        xrGetInstanceProcAddr(s->instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&s->xrPerfSettingsSetPerformanceLevelEXT);
    if (s->has_timespec)
        xrGetInstanceProcAddr(s->instance, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction *)&s->xrConvertTimespecTimeToTimeKHR);
    if (s->has_thread_settings)
        xrGetInstanceProcAddr(s->instance, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&s->xrSetAndroidApplicationThreadKHR);
    if (s->has_foveation) {
        xrGetInstanceProcAddr(s->instance, "xrCreateFoveationProfileFB", (PFN_xrVoidFunction *)&s->xrCreateFoveationProfileFB);
        xrGetInstanceProcAddr(s->instance, "xrDestroyFoveationProfileFB", (PFN_xrVoidFunction *)&s->xrDestroyFoveationProfileFB);
        xrGetInstanceProcAddr(s->instance, "xrUpdateSwapchainFB", (PFN_xrVoidFunction *)&s->xrUpdateSwapchainFB);
        if (!s->xrCreateFoveationProfileFB || !s->xrDestroyFoveationProfileFB || !s->xrUpdateSwapchainFB) s->has_foveation = 0;
    }
}

/* blackvr.ini foveation on an eye swapchain created with the fragment-density-map flag: a level profile (no dynamic
 * foveation, no vertical offset) set through xrUpdateSwapchainFB. Failure only logs: the swapchain works without. */
static void xrs_foveation_apply(XrShell *s, XrsSwapchain *sc)
{
    XrFoveationLevelProfileCreateInfoFB lv = { XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB };
    XrFoveationProfileCreateInfoFB pci = { XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB };
    XrSwapchainStateFoveationFB st = { XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB };
    XrFoveationProfileFB profile = XR_NULL_HANDLE;
    XrResult r;
    /* hightop (the VrApi level): high, with the sharp centre moved up (where the HUD and the horizon are) */
    lv.level = s->cfg.foveation == VR_FOVEATION_HIGH_TOP ? XR_FOVEATION_LEVEL_HIGH_FB : (XrFoveationLevelFB)s->cfg.foveation;
    lv.verticalOffset = s->cfg.foveation == VR_FOVEATION_HIGH_TOP ? VR_FOVEATION_HIGH_TOP_OFFSET_DEG : 0.0f;
    lv.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
    pci.next = &lv;
    r = s->xrCreateFoveationProfileFB(s->session, &pci, &profile);
    if (XR_FAILED(r)) { XLOGW("xrCreateFoveationProfileFB: %d (no foveation)", (int)r); return; }
    st.flags = 0;
    st.profile = profile;
    r = s->xrUpdateSwapchainFB(sc->handle, (const XrSwapchainStateBaseHeaderFB *)&st);
    if (XR_FAILED(r)) XLOGW("xrUpdateSwapchainFB (foveation): %d", (int)r);
    else s->foveation_applied = s->cfg.foveation;
    s->xrDestroyFoveationProfileFB(profile);         /* the swapchain keeps its state */
}

/* The settings panel: blackvr.ini foveation again, on the eye swapchain(s) created foveated (level off = a NONE
 * profile). Under the queue lock, so it never overlaps the renderer's acquire / release of the same swapchain. */
int xrs_foveation_update(XrShell *s)
{
    int i, n = s->multiview ? 1 : VR_EYE_COUNT;
    if (!s->has_foveation || !s->foveation_swapchain) return 0;
    pthread_mutex_lock(&s->queue_lock);
    for (i = 0; i < n; i++) if (s->eye[i].handle) xrs_foveation_apply(s, &s->eye[i]);
    pthread_mutex_unlock(&s->queue_lock);
    XLOGI("foveation now %d (applied %d)", s->cfg.foveation, s->foveation_applied);
    return s->foveation_applied == s->cfg.foveation;
}

int xrs_now(XrShell *s, XrTime *t)
{
    struct timespec ts;
    if (!s->xrConvertTimespecTimeToTimeKHR) return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return XR_SUCCEEDED(s->xrConvertTimespecTimeToTimeKHR(s->instance, &ts, t));
}

/* ---- actions ------------------------------------------------------------------ */

static XrPath path(XrShell *s, const char *str)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(s->instance, str, &p);
    return p;
}

static XrAction make_action(XrShell *s, XrActionType type, const char *name, const char *loc, int per_hand)
{
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    XrAction a = XR_NULL_HANDLE;
    ci.actionType = type;
    strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(ci.localizedActionName, loc, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (per_hand) { ci.countSubactionPaths = 2; ci.subactionPaths = s->hand_path; }
    if (XR_FAILED(xrCreateAction(s->action_set, &ci, &a))) XLOGE("xrCreateAction %s failed", name);
    return a;
}

static int suggest(XrShell *s, const char *profile, int with_buttons)
{
    XrActionSuggestedBinding b[32];
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    uint32_t n = 0;
    static const char *hand[2] = { "/user/hand/left", "/user/hand/right" };
    char buf[128];
    int h;
#define BIND(act, suffix) do { snprintf(buf, sizeof(buf), "%s%s", hand[h], suffix); b[n].action = (act); b[n].binding = path(s, buf); n++; } while (0)
    for (h = 0; h < 2; h++) {
        BIND(s->act_aim_pose, "/input/aim/pose");
        BIND(s->act_grip_pose, "/input/grip/pose");
        BIND(s->act_haptic, "/output/haptic");
        if (with_buttons) {
            BIND(s->act_stick, "/input/thumbstick");
            BIND(s->act_trigger, "/input/trigger/value");
            BIND(s->act_squeeze, "/input/squeeze/value");
            BIND(s->act_click, "/input/thumbstick/click");
        } else {
            BIND(s->act_trigger, "/input/select/click");
            if (h == 0) BIND(s->act_menu, "/input/menu/click");
        }
    }
    if (with_buttons) {
        h = 0; BIND(s->act_x, "/input/x/click"); BIND(s->act_y, "/input/y/click"); BIND(s->act_menu, "/input/menu/click");
        h = 1; BIND(s->act_a, "/input/a/click"); BIND(s->act_b, "/input/b/click");
    }
#undef BIND
    sb.interactionProfile = path(s, profile);
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = n;
    if (XR_FAILED(xrSuggestInteractionProfileBindings(s->instance, &sb))) {
        XLOGW("bindings for %s rejected", profile);
        return 0;
    }
    return 1;
}

static int actions_create(XrShell *s)
{
    XrActionSetCreateInfo ac = { XR_TYPE_ACTION_SET_CREATE_INFO };
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    int h;
    strcpy(ac.actionSetName, "gameplay");
    strcpy(ac.localizedActionSetName, "Gameplay");
    XRCHECK(xrCreateActionSet(s->instance, &ac, &s->action_set));
    s->hand_path[0] = path(s, "/user/hand/left");
    s->hand_path[1] = path(s, "/user/hand/right");
    s->act_stick = make_action(s, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick", 1);
    s->act_trigger = make_action(s, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", 1);
    s->act_squeeze = make_action(s, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip", 1);
    s->act_click = make_action(s, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click", 1);
    s->act_a = make_action(s, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A", 0);
    s->act_b = make_action(s, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", 0);
    s->act_x = make_action(s, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X", 0);
    s->act_y = make_action(s, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y", 0);
    s->act_menu = make_action(s, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", 0);
    s->act_aim_pose = make_action(s, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim pose", 1);
    s->act_grip_pose = make_action(s, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip pose", 1);
    s->act_haptic = make_action(s, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Rumble", 1);

    suggest(s, "/interaction_profiles/oculus/touch_controller", 1);
    suggest(s, "/interaction_profiles/meta/touch_controller_plus", 1);   /* ignored without the extension */
    suggest(s, "/interaction_profiles/khr/simple_controller", 0);

    for (h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sc = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sc.subactionPath = s->hand_path[h];
        sc.poseInActionSpace = identity_pose();
        sc.action = s->act_aim_pose;
        XRCHECK(xrCreateActionSpace(s->session, &sc, &s->aim_space[h]));
        sc.action = s->act_grip_pose;
        XRCHECK(xrCreateActionSpace(s->session, &sc, &s->grip_space[h]));
    }
    at.countActionSets = 1;
    at.actionSets = &s->action_set;
    XRCHECK(xrAttachSessionActionSets(s->session, &at));
    return 1;
}

static float get_float(XrShell *s, XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    gi.action = a;
    gi.subactionPath = h >= 0 ? s->hand_path[h] : XR_NULL_PATH;
    if (XR_FAILED(xrGetActionStateFloat(s->session, &gi, &st)) || !st.isActive) return 0.0f;
    return st.currentState;
}
static int get_bool(XrShell *s, XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    gi.action = a;
    gi.subactionPath = h >= 0 ? s->hand_path[h] : XR_NULL_PATH;
    if (XR_FAILED(xrGetActionStateBoolean(s->session, &gi, &st)) || !st.isActive) return 0;
    return st.currentState ? 1 : 0;
}
static XrVector2f get_vec2(XrShell *s, XrAction a, int h, int *active)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    XrVector2f z = { 0, 0 };
    gi.action = a;
    gi.subactionPath = s->hand_path[h];
    if (XR_FAILED(xrGetActionStateVector2f(s->session, &gi, &st)) || !st.isActive) { *active = 0; return z; }
    *active = 1;
    return st.currentState;
}

static VrPose to_vr(const XrPosef *p)
{
    VrPose r;
    r.orientation = vrm_q(p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w);
    r.position = vrm_v3(p->position.x, p->position.y, p->position.z);
    return r;
}

static void input_sync(XrShell *s, XrTime t, double dt)
{
    XrActiveActionSet aas = { s->action_set, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    VrTouchState ts;
    VrPose aim[2], grip[2];
    VrVec3 lin[2];
    int valid[2], vel_ok[2], h, active;
    XrVector2f v;
    float rumble[2];

    memset(&ts, 0, sizeof(ts));
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    if (!s->focused || XR_FAILED(xrSyncActions(s->session, &si))) {
        memset(aim, 0, sizeof(aim));
        memset(grip, 0, sizeof(grip));
        valid[0] = valid[1] = 0;
        vrb_set_input(s, &ts, aim, grip, valid, dt);
        return;
    }
    v = get_vec2(s, s->act_stick, 0, &active); ts.lx = v.x; ts.ly = v.y; ts.left_active = active;
    v = get_vec2(s, s->act_stick, 1, &active); ts.rx = v.x; ts.ry = v.y; ts.right_active = active;
    ts.lt = get_float(s, s->act_trigger, 0);
    ts.rt = get_float(s, s->act_trigger, 1);
    ts.lgrip = get_float(s, s->act_squeeze, 0);
    ts.rgrip = get_float(s, s->act_squeeze, 1);
    ts.lclick = get_bool(s, s->act_click, 0);
    ts.rclick = get_bool(s, s->act_click, 1);
    ts.a = get_bool(s, s->act_a, -1);
    ts.b = get_bool(s, s->act_b, -1);
    ts.x = get_bool(s, s->act_x, -1);
    ts.y = get_bool(s, s->act_y, -1);
    ts.menu = get_bool(s, s->act_menu, -1);
    for (h = 0; h < 2; h++) {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        XrSpaceVelocity vel = { XR_TYPE_SPACE_VELOCITY };
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        valid[h] = 0;
        vel_ok[h] = 0;
        lin[h] = vrm_v3(0, 0, 0);
        aim[h] = grip[h] = vrm_pose_identity();
        loc.next = &vel;                         /* the melee swing detector's speed (vr_gestures.h) */
        if (XR_SUCCEEDED(xrLocateSpace(s->aim_space[h], s->world_space, t, &loc)) && (loc.locationFlags & need) == need) {
            aim[h] = to_vr(&loc.pose);
            valid[h] = 1;
            if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
                lin[h] = vrm_v3(vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z);
                vel_ok[h] = 1;
            }
        }
        loc.next = NULL;
        loc.locationFlags = 0;
        if (XR_SUCCEEDED(xrLocateSpace(s->grip_space[h], s->world_space, t, &loc)) && (loc.locationFlags & need) == need)
            grip[h] = to_vr(&loc.pose);
    }
    /* the settings panel takes the controllers while it is open (the game then sees a neutral pad) */
    if (s->panel) s->panel->input(s, &ts, aim, valid, dt);
    vrb_set_hand_velocity(lin, vel_ok);
    vrb_set_input(s, &ts, aim, grip, valid, dt);

    /* rumble: the game's two motors -> left / right controller */
    vrb_get_rumble(&rumble[0], &rumble[1]);
    for (h = 0; h < 2; h++) {
        static int buzzing[2];
        XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
        hi.action = s->act_haptic;
        hi.subactionPath = s->hand_path[h];
        float pulse = 0.0f;
        uint32_t pulse_ms = 0;
        float amp;
        vrb_take_pulse(h, &pulse, &pulse_ms);
        amp = pulse > rumble[h] ? pulse : rumble[h];   /* a shot pulse rides on top of the game's rumble */
        if (amp > 0.02f) {
            XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
            vib.amplitude = amp > 1.0f ? 1.0f : amp;
            vib.duration = pulse_ms > 50u && pulse > 0.02f ? (XrDuration)pulse_ms * 1000000 : 50000000;   /* 50 ms, refreshed every frame while held */
            vib.frequency = XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(s->session, &hi, (const XrHapticBaseHeader *)&vib);
            buzzing[h] = 1;
        } else if (buzzing[h]) {
            xrStopHapticFeedback(s->session, &hi);
            buzzing[h] = 0;
        }
    }
}

/* ---- session ------------------------------------------------------------------- */

void xrs_refresh_request(XrShell *s, int preflight_line)
{
    float rates[16], all[64];
    uint32_t n = 0, total = 0, i;
    float best = 0.0f;
    /* auto (0): a whole number of display frames per game frame, so each game frame is shown equally long */
    const float want = s->cfg.refresh_hz > 0.0f ? s->cfg.refresh_hz : (s->cfg.game_hz == 60 ? 120.0f : 90.0f);
    if (!s->has_refresh_rate || !s->xrEnumerateDisplayRefreshRatesFB || !s->xrRequestDisplayRefreshRateFB) return;
    /* two-call idiom: the capacity passed must be the count the runtime gave (Quest 3 offers 8) */
    if (XR_FAILED(s->xrEnumerateDisplayRefreshRatesFB(s->session, 0, &total, NULL)) || total == 0 || total > 64) return;
    if (XR_FAILED(s->xrEnumerateDisplayRefreshRatesFB(s->session, total, &total, all))) return;
    n = total < 16 ? total : 16;
    for (i = 0; i < n; i++) rates[i] = all[i];
    for (i = 0; i < n; i++) {
        if (preflight_line) XLOGI("display offers %.1f Hz", rates[i]);
        if (fabsf(rates[i] - want) < fabsf(best - want)) best = rates[i];
    }
    if (best > 0.0f && XR_SUCCEEDED(s->xrRequestDisplayRefreshRateFB(s->session, best))) {
        s->refresh_hz = best;
        XLOGI("refresh rate %.1f Hz", best);
    }
    if (preflight_line) {
        char list[96] = "";
        for (i = 0; i < n && i < 16; i++) {
            size_t l = strlen(list);
            snprintf(list + l, sizeof(list) - l, "%s%.0f", i ? "/" : "", rates[i]);
        }
        XLOGI("PREFLIGHT display: %s %.0f Hz requested of %s (blackvr.ini refresh_hz %.0f, 0 = auto)", s->refresh_hz > 0 ? "OK" : "WARN",
              s->refresh_hz, list, s->cfg.refresh_hz);
    }
}

int xrs_session_create(XrShell *s)
{
    XrGraphicsBindingVulkan2KHR gb = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
    XrSessionCreateInfo sc = { XR_TYPE_SESSION_CREATE_INFO };
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    XrReferenceSpaceType *spaces;
    uint32_t n = 0, i;
    int64_t *formats;
    uint32_t w, h;
    VrVulkanContext ctx;

    gb.instance = s->vk_instance;
    gb.physicalDevice = s->vk_phys;
    gb.device = s->vk_device;
    gb.queueFamilyIndex = s->vk_queue_family;
    gb.queueIndex = 0;
    sc.next = &gb;
    sc.systemId = s->system;
    XRCHECK(xrCreateSession(s->instance, &sc, &s->session));

    /* stage (floor-level, guardian-centred) when available, local otherwise */
    /* two-call idiom: a runtime offering more than the array holds answers SIZE_INSUFFICIENT to a fixed capacity */
    /* the capacity passed must be the count the runtime gave: a smaller one is XR_ERROR_SIZE_INSUFFICIENT */
    XRCHECK(xrEnumerateReferenceSpaces(s->session, 0, &n, NULL));
    spaces = (XrReferenceSpaceType *)calloc(n ? n : 1, sizeof(*spaces));
    if (!spaces || XR_FAILED(xrEnumerateReferenceSpaces(s->session, n, &n, spaces))) n = 0;
    s->world_is_stage = 0;
    for (i = 0; i < n; i++) if (spaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE) s->world_is_stage = 1;
    free(spaces);
    rs.poseInReferenceSpace = identity_pose();
    rs.referenceSpaceType = s->world_is_stage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace(s->session, &rs, &s->world_space))) {
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        s->world_is_stage = 0;
        XRCHECK(xrCreateReferenceSpace(s->session, &rs, &s->world_space));
    }
    s->floor_y = s->world_is_stage ? 0.0f : -1.6f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHECK(xrCreateReferenceSpace(s->session, &rs, &s->view_space));
    XLOGI("world space: %s", s->world_is_stage ? "STAGE" : "LOCAL (fallback)");

    /* swapchain format: sRGB RGBA8 preferred (the compositor expects sRGB) */
    XRCHECK(xrEnumerateSwapchainFormats(s->session, 0, &n, NULL));
    if (n == 0) { XLOGE("the runtime offers no swapchain format"); xrs_note_failure("xrEnumerateSwapchainFormats: none", 0, 0, __FILE__, __LINE__); return 0; }
    formats = (int64_t *)calloc(n, sizeof(*formats));   /* Quest 3 offers more than 64 */
    if (!formats) return 0;
    XRCHECK(xrEnumerateSwapchainFormats(s->session, n, &n, formats));   /* a failure here ends the startup: the buffer is not freed */
    XLOGI("swapchain formats offered: %u", n);
    s->color_format = VK_FORMAT_UNDEFINED;
    for (i = 0; i < n && s->color_format == VK_FORMAT_UNDEFINED; i++)
        if (formats[i] == VK_FORMAT_R8G8B8A8_SRGB) s->color_format = VK_FORMAT_R8G8B8A8_SRGB;
    for (i = 0; i < n && s->color_format == VK_FORMAT_UNDEFINED; i++)
        if (formats[i] == VK_FORMAT_B8G8R8A8_SRGB) s->color_format = VK_FORMAT_B8G8R8A8_SRGB;
    if (s->color_format == VK_FORMAT_UNDEFINED) s->color_format = (VkFormat)formats[0];
    free(formats);

    w = (uint32_t)(s->config_views[0].recommendedImageRectWidth * s->cfg.resolution_scale + 0.5f);
    h = (uint32_t)(s->config_views[0].recommendedImageRectHeight * s->cfg.resolution_scale + 0.5f);
    if (w > s->config_views[0].maxImageRectWidth) w = s->config_views[0].maxImageRectWidth;
    if (h > s->config_views[0].maxImageRectHeight) h = s->config_views[0].maxImageRectHeight;
    w &= ~1u; h &= ~1u;
    s->multiview = s->cfg.multiview && s->multiview_supported;
    {
        /* fixed foveated rendering (blackvr.ini foveation) on the eye swapchain(s) only: the flat and HUD quads are
         * small and magnified */
        XrSwapchainCreateInfoFoveationFB fov = { XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB };
        const int foveate = s->has_foveation && s->cfg.foveation != VR_FOVEATION_OFF;
        fov.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
        /* foveation must never stop the launch: a runtime that refuses the foveated swapchain gets a plain one */
        for (i = 0; i < (s->multiview ? 1u : (uint32_t)VR_EYE_COUNT); i++) {
            const uint32_t layers = s->multiview ? 2u : 1u;
            const int fv = foveate && s->has_foveation;
            if (xrs_swapchain_create_ex(s, &s->eye[i], w, h, layers, fv ? &fov : NULL)) {
                if (fv) xrs_foveation_apply(s, &s->eye[i]);
                s->foveation_swapchain = fv;
                continue;
            }
            if (!fv) return 0;
            XLOGW("the foveated eye swapchain was refused: retrying without foveation");
            s->has_foveation = 0;
            if (!xrs_swapchain_create_ex(s, &s->eye[i], w, h, layers, NULL)) return 0;
            s->foveation_swapchain = 0;
        }
        XLOGI("foveation: %s", !s->has_foveation ? "not used (no XR_FB_foveation_vulkan, no VK_EXT_fragment_density_map, "
                                                   "or the foveated swapchain was refused)"
                               : !foveate ? "off (blackvr.ini)"
                               : s->foveation_applied ? "fixed, level set on the eye swapchain" : "request failed");
    }
    /* flat content: the game's 640x480 frame, upscaled 2x by the renderer */
    if (!xrs_swapchain_create(s, &s->flat, 1280, 960, 1)) return 0;
    s->flat_aspect = 4.0f / 3.0f;
    /* the HUD drawn over the stereo world, same size and aspect (vr_hud_layout) */
    if (!xrs_swapchain_create(s, &s->hud, 1280, 960, 1)) return 0;
    XLOGI("eye swapchain %ux%u (%s), flat + HUD 1280x960, format %d%s", w, h,
          s->multiview ? "2-layer multiview" : "2 swapchains", (int)s->color_format,
          s->has_format_list ? ", sRGB+UNORM format list" : "");

    if (!actions_create(s)) return 0;

    /* the test pattern always: it shows until the renderer delivers its first frame */
    if (!xrs_testpattern_init(s)) return 0;
    ctx = *vr_vulkan_context();
    s->external_renderer = s->want_renderer ? vr_renderer_init(&ctx) : 0;
    XLOGI(s->external_renderer ? "renderer attached (test pattern until its first frame)"
                               : "renderer stubbed or no game: drawing the test pattern");
    return 1;
}

int xrs_locate_views(XrShell *s, XrTime t, XrPosef pose[VR_EYE_COUNT], XrFovf fov[VR_EYE_COUNT])
{
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView views[VR_EYE_COUNT];
    uint32_t n = 0, i;
    if (!s->session || t <= 0) return 0;
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = s->world_space;
    for (i = 0; i < VR_EYE_COUNT; i++) { views[i].type = XR_TYPE_VIEW; views[i].next = NULL; }
    if (XR_FAILED(xrLocateViews(s->session, &li, &vs, VR_EYE_COUNT, &n, views)) || n != VR_EYE_COUNT) return 0;
    if ((vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) return 0;
    for (i = 0; i < VR_EYE_COUNT; i++) { pose[i] = views[i].pose; fov[i] = views[i].fov; }
    return 1;
}

int xrs_locate_head(XrShell *s, XrTime t, XrPosef *head)
{
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (!s->session || t <= 0) return 0;
    if (XR_FAILED(xrLocateSpace(s->view_space, s->world_space, t, &loc))) return 0;
    if ((loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) return 0;
    *head = loc.pose;
    return 1;
}

int xrs_swapchain_acquire(XrShell *s, XrsSwapchain *sc, uint32_t *index)
{
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    XrResult r;
    if (sc->held) return 0;
    /* XR_KHR_vulkan_enable2: xrAcquireSwapchainImage may use the VkQueue (as xrBeginFrame, xrEndFrame and
     * xrReleaseSwapchainImage do), so it is externally synchronised with every other queue user. The wait below
     * does not touch the queue and runs unlocked. */
    pthread_mutex_lock(&s->queue_lock);
    r = xrAcquireSwapchainImage(sc->handle, &ai, index);
    pthread_mutex_unlock(&s->queue_lock);
    if (XR_FAILED(r)) return 0;
    wi.timeout = 100000000;   /* 100 ms */
    r = xrWaitSwapchainImage(sc->handle, &wi);
    if (r == XR_TIMEOUT_EXPIRED) r = xrWaitSwapchainImage(sc->handle, &wi);
    if (XR_FAILED(r) || r == XR_TIMEOUT_EXPIRED) {
        XLOGW("xrWaitSwapchainImage: %d", r);
        return 0;
    }
    sc->acquired = *index;
    sc->held = 1;
    return 1;
}

void xrs_swapchain_release(XrShell *s, XrsSwapchain *sc)
{
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (!sc->held) return;
    pthread_mutex_lock(&s->queue_lock);
    xrReleaseSwapchainImage(sc->handle, &ri);
    sc->held = 0;
    pthread_mutex_unlock(&s->queue_lock);
}

/* ---- events -------------------------------------------------------------------- */

/* XR_EXT_performance_settings: the CPU and GPU levels for a BLACKVR_PERF value (session start: the environment; the
 * settings panel: its Performance setting, live) */
void xrs_perf_apply(XrShell *s, const char *mode)
{
    /* BLACKVR_PERF (blackvr.ini "env BLACKVR_PERF=..."): sustained_high (default) | boost | boost_cpu |
     * boost_gpu. Boost asks for the highest clocks; the runtime may cap or throttle it (watch the
     * VrApi "CPU%d/GPU=%d/%d" lines and thermal warnings in the log). */
    const char *pe = mode;
    XrPerfSettingsLevelEXT cpu = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT, gpu = cpu;
    XrResult rc, rg;
    if (!s->has_perf_settings || !s->xrPerfSettingsSetPerformanceLevelEXT || !s->session) return;
    if (pe && (!strcmp(pe, "boost") || !strcmp(pe, "boost_cpu"))) cpu = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
    if (pe && (!strcmp(pe, "boost") || !strcmp(pe, "boost_gpu"))) gpu = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
    rc = s->xrPerfSettingsSetPerformanceLevelEXT(s->session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, cpu);
    rg = s->xrPerfSettingsSetPerformanceLevelEXT(s->session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, gpu);
    XLOGI("performance levels: CPU %s (%d), GPU %s (%d)",
          cpu == XR_PERF_SETTINGS_LEVEL_BOOST_EXT ? "boost" : "sustained high", (int)rc,
          gpu == XR_PERF_SETTINGS_LEVEL_BOOST_EXT ? "boost" : "sustained high", (int)rg);
}

static void session_state(XrShell *s, XrSessionState st)
{
    s->state = st;
    switch (st) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(s->session, &bi))) {
            s->session_running = 1;
            xrs_refresh_request(s, 1);
            xrs_perf_apply(s, getenv("BLACKVR_PERF"));
            if (s->has_thread_settings && s->xrSetAndroidApplicationThreadKHR)
                s->xrSetAndroidApplicationThreadKHR(s->session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, gettid());
        }
        break;
    }
    case XR_SESSION_STATE_FOCUSED: s->focused = 1; break;
    case XR_SESSION_STATE_VISIBLE: s->focused = 0; break;
    case XR_SESSION_STATE_STOPPING:
        s->focused = 0;
        xrEndSession(s->session);
        s->session_running = 0;
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        s->exit_requested = 1;
        break;
    default: break;
    }
    vrb_set_session(s, s->session_running, s->focused);
    XLOGI("session state %d (running %d focused %d)", (int)st, s->session_running, s->focused);
}

void xrs_poll_events(XrShell *s)
{
    XrEventDataBuffer ev;
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (xrPollEvent(s->instance, &ev) != XR_SUCCESS) break;
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            session_state(s, ((XrEventDataSessionStateChanged *)&ev)->state);
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            s->exit_requested = 1;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            /* the user re-centred from the system menu: follow it */
            vrb_recentre_from(NULL);
            break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
            s->refresh_hz = ((XrEventDataDisplayRefreshRateChangedFB *)&ev)->toDisplayRefreshRate;
            XLOGI("refresh rate now %.1f Hz", s->refresh_hz);
            break;
        default: break;
        }
    }
}

/* ---- frame ------------------------------------------------------------------------ */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void flat_clear(XrShell *s, float r, float g, float b)
{
    /* test-pattern mode: proves the quad layer path with a solid colour */
    uint32_t im;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkClearColorValue c;
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBuffer cmd = s->tp_cmd[1];
    if (!xrs_swapchain_acquire(s, &s->flat, &im)) return;
    vkWaitForFences(s->vk_device, 1, &s->tp_fence[1], VK_TRUE, UINT64_MAX);
    vkResetFences(s->vk_device, 1, &s->tp_fence[1]);
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &bi);
    mb.srcAccessMask = 0;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = s->flat.images[im];
    mb.subresourceRange = rr;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    c.float32[0] = r; c.float32[1] = g; c.float32[2] = b; c.float32[3] = 1.0f;
    vkCmdClearColorImage(cmd, s->flat.images[im], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rr);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    vkEndCommandBuffer(cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vr_queue_lock();
    vkQueueSubmit(s->vk_queue, 1, &si, s->tp_fence[1]);
    vr_queue_unlock();
    xrs_swapchain_release(s, &s->flat);
    s->flat_released = 1;
}

static XrPosef quad_pose_world(XrShell *s, float dist, float yoff)
{
    VrPose o;
    XrPosef p;
    VrVec3 pos;
    vr_get_recentre(&o);
    pos = vrm_add(o.position, vrm_q_rotate(o.orientation, vrm_v3(0, yoff, -dist)));
    p.orientation.x = o.orientation.x; p.orientation.y = o.orientation.y;
    p.orientation.z = o.orientation.z; p.orientation.w = o.orientation.w;
    p.position.x = pos.x; p.position.y = pos.y; p.position.z = pos.z;
    (void)s;
    return p;
}

void xrs_frame(XrShell *s)
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[VR_EYE_COUNT];
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad hudq = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader *layers[10];  /* projection, scope surround, HUD or flat, settings panel + its laser */
    uint32_t nl = 0, i;
    static double last_t;
    static double t0;
    double t, dt;
    int mode = VR_PRESENT_STEREO, show_quad_too = 0, renderer;

    if (XR_FAILED(xrWaitFrame(s->session, &wi, &fs))) return;
    s->last_predicted_time = fs.predictedDisplayTime;
    s->last_predicted_period = fs.predictedDisplayPeriod;
    t = now_s();
    if (!t0) t0 = t;
    dt = last_t > 0.0 ? t - last_t : 0.0;
    last_t = t;
    vrb_on_xr_frame(s, fs.predictedDisplayTime, fs.predictedDisplayPeriod);
    input_sync(s, fs.predictedDisplayTime, dt);

    vr_queue_lock();
    xrBeginFrame(s->session, &bi);
    vr_queue_unlock();
    vrsm_xr_frame(s);                    /* settings changed on the game's VR SETTINGS page: refresh, foveation, perf */
    if (s->panel && fs.shouldRender) s->panel->draw(s);      /* nothing while the panel is closed */

    memset(pv, 0, sizeof(pv));

    if (fs.shouldRender && !(s->external_renderer && s->renderer_started)) {
        /* no renderer, or none of its frames yet: the shell renders the smoke test itself */
        pthread_mutex_lock(&s->tp_lock);
        if (!s->renderer_started) {
            XrPosef pose[VR_EYE_COUNT];
            XrFovf fov[VR_EYE_COUNT];
            uint32_t idx[VR_EYE_COUNT];
            if (xrs_locate_views(s, fs.predictedDisplayTime, pose, fov)) {
                int ok = 1, n = s->multiview ? 1 : VR_EYE_COUNT, e;
                for (e = 0; e < n && ok; e++) ok = xrs_swapchain_acquire(s, &s->eye[e], &idx[e]);
                if (ok) {
                    for (e = 0; e < VR_EYE_COUNT; e++) {
                        XrsSwapchain *sc = s->multiview ? &s->eye[0] : &s->eye[e];
                        xrs_testpattern_draw(s, e, sc, s->multiview ? idx[0] : idx[e],
                                             s->multiview ? (uint32_t)e : 0u, &pose[e], &fov[e], t - t0);
                    }
                    for (e = 0; e < n; e++) xrs_swapchain_release(s, &s->eye[e]);
                    pthread_mutex_lock(&s->queue_lock);
                    for (e = 0; e < VR_EYE_COUNT; e++) { s->submit.pose[e] = pose[e]; s->submit.fov[e] = fov[e]; }
                    s->submit.valid = 1;
                    s->submit.serial++;
                    s->submit.mode = VR_PRESENT_STEREO;
                    s->submit.hud = 0;
                    pthread_mutex_unlock(&s->queue_lock);
                } else {
                    for (e = 0; e < n; e++) xrs_swapchain_release(s, &s->eye[e]);
                }
                /* quad layer check: hold X to show a 4:3 panel that cycles colour. A failed preflight (or a game that
                 * exited early) shows a solid panel instead: red = the game was not started (fatal PREFLIGHT line),
                 * orange = the game thread returned; the log says why (port/notes/device-preflight.md). */
                {
                    VrPadState pad;
                    vrb_touch_debug(&pad);
                    if (s->pf_panel) {
                        if (s->pf_panel == XRS_PANEL_FATAL) flat_clear(s, 0.75f, 0.02f, 0.02f);
                        else flat_clear(s, 0.85f, 0.40f, 0.02f);
                        mode = VR_PRESENT_FLAT_MENU;
                        show_quad_too = 1;
                    } else if (pad.buttons & VR_PAD_X) {
                        const float k = (float)fmod(t - t0, 3.0) / 3.0f;
                        flat_clear(s, 0.2f + 0.6f * k, 0.3f, 0.8f - 0.6f * k);
                        mode = VR_PRESENT_FLAT_MENU;
                        show_quad_too = 1;          /* the quad over the scene */
                    }
                }
            }
        }
        pthread_mutex_unlock(&s->tp_lock);
    }
    /* held through xrEndFrame: the renderer's release + pose commit cannot slip between */
    pthread_mutex_lock(&s->queue_lock);
    renderer = s->external_renderer && s->renderer_started;
    if (renderer) mode = s->submit.mode;           /* what the renderer released last */
    if (fs.shouldRender) {
        if ((mode == VR_PRESENT_STEREO || show_quad_too) && s->submit.valid) {
            for (i = 0; i < VR_EYE_COUNT; i++) {
                XrsSwapchain *sc = s->multiview ? &s->eye[0] : &s->eye[i];
                pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pv[i].pose = s->submit.pose[i];
                pv[i].fov = s->submit.fov[i];
                pv[i].subImage.swapchain = sc->handle;
                pv[i].subImage.imageRect.extent.width = (int32_t)sc->width;
                pv[i].subImage.imageRect.extent.height = (int32_t)sc->height;
                pv[i].subImage.imageArrayIndex = s->multiview ? i : 0;
            }
            proj.space = s->world_space;
            proj.viewCount = VR_EYE_COUNT;
            proj.views = pv;
            /* the 3D scope view: the game's own scope (on the HUD) over a black surround, both centred on the view */
            const int scoped = vr_scope_view() > 1.0f;
            if (renderer && mode == VR_PRESENT_STEREO && !show_quad_too && scoped && s->view_space) {
                /* The scoped picture is fixed to the head, as the reticle and the surround on it are (view space): what
                 * it shows follows the aim, not where the head faces. Submitted in the world it was re-aimed by the
                 * compositor for every head turn since it was drawn, and on a repeated frame slid under the reticle
                 * (Kenny 2026-11-16: "it's not aligned with the cross air"). The eyes relative to the head they were
                 * drawn for: the first eye's orientation at their midpoint. */
                const VrQuat hq = vrm_q(s->submit.pose[0].orientation.x, s->submit.pose[0].orientation.y,
                                        s->submit.pose[0].orientation.z, s->submit.pose[0].orientation.w);
                const VrQuat hi = vrm_q_conj(hq);
                const VrVec3 mid = vrm_v3(0.5f * (s->submit.pose[0].position.x + s->submit.pose[1].position.x),
                                          0.5f * (s->submit.pose[0].position.y + s->submit.pose[1].position.y),
                                          0.5f * (s->submit.pose[0].position.z + s->submit.pose[1].position.z));
                for (i = 0; i < VR_EYE_COUNT; i++) {
                    const XrPosef *ep = &s->submit.pose[i];
                    const VrQuat q = vrm_q_normalize(vrm_q_mul(hi, vrm_q(ep->orientation.x, ep->orientation.y,
                                                                         ep->orientation.z, ep->orientation.w)));
                    const VrVec3 t = vrm_q_rotate(hi, vrm_sub(vrm_v3(ep->position.x, ep->position.y, ep->position.z), mid));
                    pv[i].pose.orientation.x = q.x; pv[i].pose.orientation.y = q.y;
                    pv[i].pose.orientation.z = q.z; pv[i].pose.orientation.w = q.w;
                    pv[i].pose.position.x = t.x; pv[i].pose.position.y = t.y; pv[i].pose.position.z = t.z;
                }
                proj.space = s->view_space;
            }
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
            if (renderer && mode == VR_PRESENT_STEREO && !show_quad_too && scoped)
                nl += xrs_scope_layers(s, layers + nl, (uint32_t)(sizeof(layers) / sizeof(layers[0])) - nl);
            if (renderer && mode == VR_PRESENT_STEREO && s->submit.hud && s->hud.handle) {
                /* the HUD: head-locked (the compositor keeps it on the current head pose), premultiplied alpha */
                const VrHudLayout *hl = vr_hud_layout();
                hudq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudq.space = s->view_space;
                hudq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudq.subImage.swapchain = s->hud.handle;
                hudq.subImage.imageRect.extent.width = (int32_t)s->hud.width;
                hudq.subImage.imageRect.extent.height = (int32_t)s->hud.height;
                hudq.pose = identity_pose();
                if (scoped) {
                    /* the scope's picture centred where the renderer shows the aim (the middle of the headset's view,
                     * xrs_scope_drop; its centre is the zoomed gun line), far out (the eyes look through it at the
                     * distance), XRS_SCOPE_HUD_TAN wide, facing the eyes */
                    const float a = xrs_scope_drop(s);
                    hudq.pose.orientation.x = sinf(0.5f * a);
                    hudq.pose.orientation.w = cosf(0.5f * a);
                    hudq.pose.position.y = XRS_SCOPE_HUD_DIST * sinf(a);
                    hudq.pose.position.z = -XRS_SCOPE_HUD_DIST * cosf(a);
                } else {
                    /* turned about the eyes' x axis by hud_height, still facing them: BLACK's HUD lives along the
                     * top of its image, 22 deg above the view centre when level */
                    const float a = hl->pitch_deg * 0.017453292f;
                    hudq.pose.orientation.x = sinf(0.5f * a);
                    hudq.pose.orientation.w = cosf(0.5f * a);
                    hudq.pose.position.y = hl->distance_m * sinf(a);
                    hudq.pose.position.z = -hl->distance_m * cosf(a);
                }
                hudq.size.width = scoped ? 2.0f * XRS_SCOPE_HUD_TAN * XRS_SCOPE_HUD_DIST : hl->width_m;
                hudq.size.height = hudq.size.width * (float)s->hud.height / (float)s->hud.width;
                layers[nl++] = (const XrCompositionLayerBaseHeader *)&hudq;
            }
        }
        if (mode != VR_PRESENT_STEREO && s->flat_released) {
            quad.layerFlags = 0;           /* opaque: the Xbox frame's alpha is not coverage */
            quad.subImage.swapchain = s->flat.handle;
            quad.subImage.imageRect.extent.width = (int32_t)s->flat.width;
            quad.subImage.imageRect.extent.height = (int32_t)s->flat.height;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            if (mode == VR_PRESENT_FLAT_SCOPE) {
                quad.space = s->view_space;
                quad.pose = identity_pose();
                quad.pose.position.z = -0.9f;
                quad.size.width = 0.75f;
            } else {
                quad.space = s->world_space;
                quad.pose = quad_pose_world(s, 2.2f, 0.0f);
                quad.size.width = 2.0f;
            }
            quad.size.height = quad.size.width / s->flat_aspect;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&quad;
        }
        /* the settings panel over everything (world-locked quad + laser beam) */
        if (s->panel) nl += s->panel->layers(s, layers + nl, (uint32_t)(sizeof(layers) / sizeof(layers[0])) - nl);
    }
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl;
    ei.layers = layers;
    xrEndFrame(s->session, &ei);
    pthread_mutex_unlock(&s->queue_lock);
}

/* xr_panel.c's scope surround; a build without the panel draws none */
__attribute__((weak)) uint32_t xrs_scope_layers(XrShell *s, const XrCompositionLayerBaseHeader **out, uint32_t cap)
{
    (void)s; (void)out; (void)cap;
    return 0;
}

void xrs_session_destroy(XrShell *s)
{
    int h;
    if (s->external_renderer) {
        vr_renderer_shutdown();
        if (s->game_started) {
            /* The game's GPU thread may still be submitting on this device (the recompiled runtime has no orderly
             * shutdown): leave Vulkan and OpenXR to the process exit that follows (xr_main.c _exit). */
            XLOGI("game running: Vulkan/OpenXR objects left to process exit");
            return;
        }
    }
    if (s->panel && s->panel->destroy) s->panel->destroy(s);
    xrs_vulkan_destroy(s);
    for (h = 0; h < 2; h++) {
        if (s->aim_space[h]) xrDestroySpace(s->aim_space[h]);
        if (s->grip_space[h]) xrDestroySpace(s->grip_space[h]);
    }
    if (s->action_set) xrDestroyActionSet(s->action_set);
    if (s->view_space) xrDestroySpace(s->view_space);
    if (s->world_space) xrDestroySpace(s->world_space);
    if (s->session) xrDestroySession(s->session);
    if (s->instance) xrDestroyInstance(s->instance);
    memset(&s->instance, 0, sizeof(s->instance));
    s->session = XR_NULL_HANDLE;
}
