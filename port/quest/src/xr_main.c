/*
 * xr_main.c - NativeActivity entry point for BLACK VR (com.kenny.blackvr).
 *
 * Startup
 *   1. Locate the game folder (see find_game_root) and read blackvr.ini.
 *   2. OpenXR instance -> Vulkan through XR_KHR_vulkan_enable2 -> session,
 *      swapchains, actions; when the game will start, vr_renderer_init() is
 *      offered the Vulkan context (the Kelvin backend: kvk_set_* before the
 *      game thread exists, so before kelvin_start). Otherwise the test pattern.
 *   3. If the recompiled game is linked in (black_game_main is defined) and
 *      game/default.xbe exists, start the game on its own thread.
 *   4. The XR loop runs on this thread for the life of the app.
 *
 * Threading model (see port/notes/xr-shell.md, "Threading"):
 *   The game runs decoupled at its own ~60 Hz. It never waits on the XR
 *   frame loop; it samples the newest predicted head pose when it produces
 *   its world camera. The renderer late-latches eye poses again when it
 *   acquires the swapchain image, and the shell submits each projection
 *   layer with exactly the poses the image was rendered with, so the
 *   compositor's reprojection (ATW) corrects the remaining rotation error at
 *   72/90 Hz no matter how slowly the game runs.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "xr_shell.h"
#include "bvr_diag.h"

XrShell g_xr;

/* The XBE the APK's recompiled code was generated from (port/quest/CMakeLists.txt reads game/default.xbe at configure
 * time for a --with-game build): the preflight compares the headset's copy with it. */
#ifndef BLACKVR_XBE_SIZE
#define BLACKVR_XBE_SIZE 0
#endif
#ifndef BLACKVR_XBE_SIG
#define BLACKVR_XBE_SIG ""
#endif
#ifndef BLACKVR_VERSION
#define BLACKVR_VERSION "dev"
#endif

/* Defined by the game build (src/main.c under __ANDROID__); weak so the smoke
 * build links without the runtime. */
extern int black_game_main(int argc, char **argv) __attribute__((weak));

static const char k_default_ini[] =
    "# BLACK VR settings. Lines are key = value; # starts a comment.\n"
    "refresh_hz = auto        ; auto (120 for game_hz 60, 90 for 30) or 72, 80, 90, 120\n"
    "resolution_scale = 0.8   ; eye image size vs the headset's recommendation\n"
    "foveation = medium       ; off | low | medium | high | hightop (fixed foveated rendering)\n"
    "game_hz = 60             ; 60 (smoother, twice the CPU) | 30 (the game's own rate, steadiest)\n"
    "raster_workers = 1       ; 0..4 CPU vertex/raster threads beside the GPU thread\n"
    "raster_spin_us = 50      ; 0..5000: how long an idle raster thread spins before sleeping\n"
    "multiview = 1\n"
    "turn = snap              ; snap | smooth | off\n"
    "snap_degrees = 30\n"
    "smooth_degrees_per_second = 120\n"
    "deadzone = 0.15\n"
    "left_handed = 0          ; 1: gun in the left hand (the controls mirror)\n"
    "aim = hand               ; hand | head\n"
    "locomotion = head        ; head | aim | offhand (move where you look / the gun points / the off hand points)\n"
    "vr_accuracy = aim        ; aim | hip (BLACK's spread has no aim term: both are the game's own)\n"
    "melee_swing = on         ; a fast swing of the gun hand is a melee attack (B too)\n"
    "melee_swing_speed = 2.5  ; m/s\n"
    "duck_crouch = on         ; duck to crouch, stand up to stand (the stick click still toggles)\n"
    "duck_ratio = 0.80        ; crouch below 0.80 x standing head height, stand above 0.88\n"
    "scope_raise = on         ; raise a scoped weapon to your eye to look through the scope (off: gun-hand grip)\n"
    "scope_raise_distance = 0.15 ; metres from the rear sight to the eye\n"
    "two_hand = on            ; off-hand grip near the gun: two-handed aim\n"
    "world_scale = 1.0        ; game units per metre (tune if the world feels too big/small)\n"
    "max_head_offset = 0.5\n"
    "aim_converge = 10.0\n"
    "hand_weapon = 1\n"
    "weapon_offset = 0.12 -0.18 0.30\n"
    "scope_zoom = 2.5\n"
    "fov_margin = 1.10\n"
    "bob_filter = 1\n"
    "hud_distance = 2.0       ; metres: the head-locked HUD\n"
    "hud_width = 2.4          ; metres (4:3)\n"
    "arms = hands             ; hands: the arms end at the wrists | full: the game's whole arms\n"
    "left_hand = tracked      ; tracked: follows your off controller | grip | always | never\n"
    "haptic_fire = 0.7        ; 0..1: shot pulse on the gun hand (0 = off); the game's own rumble (hits) stays\n"
    "gun_latch = on           ; on | off: the weapon follows the controller at display time\n"
    "# env RECOMP_BLACK_HZ=60\n"
    "# In the headset: Options -> VR SETTINGS (settings_menu = panel: the floating panel instead)\n";

static const char k_where_txt[] =
    "BLACK VR - put your own BLACK (original Xbox, NTSC-U) disc files here:\n\n"
    "  game/default.xbe and the rest of the extracted disc, in a folder named 'game'\n"
    "  next to this file. The XBE must be the one the APK was generated from.\n\n"
    "blackvr.ini next to this file holds the VR settings.\n";

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static void write_if_missing(const char *dir, const char *name, const char *text)
{
    char p[600];
    FILE *f;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    if (file_exists(p) || !(f = fopen(p, "w"))) return;
    fputs(text, f);
    fclose(f);
}

static void load_config(XrShell *s)
{
    char p[600];
    FILE *f;
    long n;
    char *text;
    vr_config_defaults(&s->cfg);
    snprintf(p, sizeof(p), "%s/blackvr.ini", s->game_root);
    if (!(f = fopen(p, "rb"))) return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < 65536 && (text = (char *)calloc(1, (size_t)n + 1))) {
        if (fread(text, 1, (size_t)n, f) == (size_t)n) vr_config_parse(&s->cfg, text);
        free(text);
    }
    fclose(f);
}

/* After load_config, before the game thread: the runtime's environment (also with no blackvr.ini: the defaults). */
static void export_config(XrShell *s)
{
    vr_config_export_env(&s->cfg, setenv);
    XLOGI("blackvr.ini: %d lines not understood; aim %s, turn %d, %.0f Hz display, game %d Hz, resolution x%.2f, "
          "foveation %d, raster workers %d (spin %d us)",
          s->cfg.errors, s->cfg.game.aim_mode == VR_AIM_HAND ? "hand" : "head", (int)s->cfg.input.turn_mode,
          s->cfg.refresh_hz, s->cfg.game_hz, s->cfg.resolution_scale, s->cfg.foveation, s->cfg.raster_workers,
          s->cfg.raster_spin_us);
    {   /* which file, and its env lines: a log shows whether an edit reached the running game */
        char envs[900] = "";
        size_t l = 0;
        int i;
        for (i = 0; i < s->cfg.env_count && l + 4 < sizeof(envs); i++)
            l += (size_t)snprintf(envs + l, sizeof(envs) - l, "%s%s", i ? ", " : "", s->cfg.env[i]);
        if (l >= sizeof(envs)) l = sizeof(envs) - 1;
        XLOGI("blackvr.ini: %s/blackvr.ini, %d env line(s)%s%s", s->game_root, s->cfg.env_count, l ? ": " : "", envs);
    }
}

/* JNI: on Android 11+ reading a game folder outside the app's own (/sdcard/BlackVR, /sdcard/Download/BlackVR,
 * /sdcard/BLACK) needs "All files access". Opens the system page for it once, when such a folder exists but is
 * unreadable. */
static void request_all_files_access(XrShell *s)
{
    JavaVM *vm = s->app->activity->vm;
    JNIEnv *env = NULL;
    jclass build_ver, environment, intent_cls, uri_cls, activity_cls;
    jfieldID sdk_f;
    jint sdk;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    build_ver = (*env)->FindClass(env, "android/os/Build$VERSION");
    sdk_f = build_ver ? (*env)->GetStaticFieldID(env, build_ver, "SDK_INT", "I") : NULL;
    sdk = sdk_f ? (*env)->GetStaticIntField(env, build_ver, sdk_f) : 0;
    if (sdk >= 30) {
        environment = (*env)->FindClass(env, "android/os/Environment");
        if (environment) {
            jmethodID is_mgr = (*env)->GetStaticMethodID(env, environment, "isExternalStorageManager", "()Z");
            if (is_mgr && !(*env)->CallStaticBooleanMethod(env, environment, is_mgr)) {
                jstring action = (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
                jstring pkg = (*env)->NewStringUTF(env, "package:com.kenny.blackvr");
                uri_cls = (*env)->FindClass(env, "android/net/Uri");
                intent_cls = (*env)->FindClass(env, "android/content/Intent");
                activity_cls = (*env)->GetObjectClass(env, s->app->activity->clazz);
                if (uri_cls && intent_cls && activity_cls) {
                    jmethodID parse = (*env)->GetStaticMethodID(env, uri_cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
                    jmethodID ctor = (*env)->GetMethodID(env, intent_cls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
                    jmethodID start = (*env)->GetMethodID(env, activity_cls, "startActivity", "(Landroid/content/Intent;)V");
                    jobject uri = parse ? (*env)->CallStaticObjectMethod(env, uri_cls, parse, pkg) : NULL;
                    jobject intent = (uri && ctor) ? (*env)->NewObject(env, intent_cls, ctor, action, uri) : NULL;
                    if (intent && start) {
                        (*env)->CallVoidMethod(env, s->app->activity->clazz, start, intent);
                        XLOGI("asked for All files access (for a game folder outside the app's own)");
                    }
                }
            }
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
}

/* Game folder, first match (every path tried is logged on the PREFLIGHT gamedir line):
 *   1. <app external files dir>/game/default.xbe
 *        /sdcard/Android/data/com.kenny.blackvr/files/game  (no permission needed;
 *        reachable over USB from a PC, or adb push; mac_install.sh copies there)
 *   2. the same folder by its /sdcard name, if the activity reported another
 *   3. /sdcard/BlackVR/game, 4. /sdcard/Download/BlackVR/game, 5. /sdcard/BLACK/game
 *        (need All files access; reachable from on-headset file managers / browser downloads)
 * The chosen folder becomes the working directory, so the runtime's relative "game" and "save" paths resolve
 * there. */
static void find_game_root(XrShell *s)
{
    const char *ext = s->app->activity->externalDataPath, *in = s->app->activity->internalDataPath;
    const char *cands[5];
    char save[600];
    BvrGameDir gd;
    snprintf(s->files_dir, sizeof(s->files_dir), "%s", ext ? ext : "/sdcard/Android/data/com.kenny.blackvr/files");
    mkdir(s->files_dir, 0770);
    write_if_missing(s->files_dir, "blackvr.ini", k_default_ini);
    write_if_missing(s->files_dir, "WHERE_TO_PUT_GAME_FILES.txt", k_where_txt);
    cands[0] = s->files_dir;
    cands[1] = "/sdcard/Android/data/com.kenny.blackvr/files";
    cands[2] = "/sdcard/BlackVR";
    cands[3] = "/sdcard/Download/BlackVR";
    cands[4] = "/sdcard/BLACK";
    bvr_pf_gamedir(cands, 5, (long long)BLACKVR_XBE_SIZE, BLACKVR_XBE_SIG, &gd);
    s->game_files_present = gd.found;
    snprintf(s->game_root, sizeof(s->game_root), "%s", gd.found ? gd.root : s->files_dir);
    if (!gd.found && gd.unreadable_seen) request_all_files_access(s);
    if (chdir(s->game_root) != 0) XLOGW("chdir %s: %s", s->game_root, strerror(errno));
    mkdir("save", 0770);
    snprintf(save, sizeof(save), "%s/save", s->game_root);
    bvr_pf_storage(s->game_root, save, in && *in ? in : NULL);
    XLOGI("game folder %s/game: %s", s->game_root, s->game_files_present ? "found" : "MISSING (test pattern)");
}

/* The shader cache shipped in the APK (assets/kgpu-cache: SPIR-V for every vertex program and combiner the headless
 * run met in the menus and level 1, the vertex-program and pipeline manifests; port/notes/perf.md), into the app's
 * own cache dir before the renderer starts, so a first boot neither runs glslang nor meets a pipeline for the first
 * time at a draw (the backend creates the manifest's pipelines on its workers at start). SPIR-V files are copied
 * when missing; the manifests' records are appended once per shipped cache (seed.stamp: file count and bytes), the
 * backend ignores duplicates. The driver's VkPipelineCache is not shipped: it is the device's own. */
static void seed_shader_cache(XrShell *s)
{
    AAssetManager *am = s->app && s->app->activity ? s->app->activity->assetManager : NULL;
    AAssetDir *dir;
    const char *name;
    char stamp_path[700], stamp[64], old[64] = "";
    long long total = 0;
    int count = 0, copied = 0, merged = 0;
    FILE *f;
    if (!am || !(dir = AAssetManager_openDir(am, "kgpu-cache"))) return;
    while ((name = AAssetDir_getNextFileName(dir)) != NULL) {
        char src[300];
        AAsset *a;
        snprintf(src, sizeof(src), "kgpu-cache/%s", name);
        if ((a = AAssetManager_open(am, src, AASSET_MODE_UNKNOWN)) != NULL) {
            total += (long long)AAsset_getLength64(a);
            count++;
            AAsset_close(a);
        }
    }
    if (!count) { AAssetDir_close(dir); return; }
    snprintf(stamp, sizeof(stamp), "%d %lld", count, total);
    snprintf(stamp_path, sizeof(stamp_path), "%s/seed.stamp", s->cache_dir);
    if ((f = fopen(stamp_path, "r")) != NULL) {
        if (!fgets(old, sizeof(old), f)) old[0] = 0;
        old[strcspn(old, "\n")] = 0;
        fclose(f);
    }
    if (!strcmp(old, stamp)) { AAssetDir_close(dir); return; }
    AAssetDir_rewind(dir);
    while ((name = AAssetDir_getNextFileName(dir)) != NULL) {
        char src[300], dst[700], tmp[720];
        const int manifest = !strncmp(name, "vk_", 3) && strstr(name, "manifest") != NULL;
        AAsset *a;
        FILE *o;
        if (strchr(name, '/') || !strcmp(name, "vk_pipeline_cache.bin")) continue;
        snprintf(src, sizeof(src), "kgpu-cache/%s", name);
        snprintf(dst, sizeof(dst), "%s/%s", s->cache_dir, name);
        if (!manifest && file_exists(dst)) continue;
        if (!(a = AAssetManager_open(am, src, AASSET_MODE_STREAMING))) continue;
        /* SPIR-V: written whole, then renamed into place; manifests: appended */
        snprintf(tmp, sizeof(tmp), "%s.seed", dst);
        if ((o = fopen(manifest ? dst : tmp, manifest ? "ab" : "wb")) != NULL) {
            char buf[16384];
            int n, ok = 1;
            while ((n = AAsset_read(a, buf, sizeof(buf))) > 0)
                if (fwrite(buf, 1, (size_t)n, o) != (size_t)n) { ok = 0; break; }
            if (fclose(o) != 0) ok = 0;
            if (manifest) merged += ok;
            else if (ok && rename(tmp, dst) == 0) copied++;
            else remove(tmp);
        }
        AAsset_close(a);
    }
    AAssetDir_close(dir);
    if ((f = fopen(stamp_path, "w")) != NULL) { fprintf(f, "%s\n", stamp); fclose(f); }
    XLOGI("shader cache: %d SPIR-V files seeded from the APK, %d manifests merged (%d assets, %lld bytes)", copied,
          merged, count, total);
}

static void *game_thread(void *arg)
{
    static char a0[] = "black", *argv[] = { a0, NULL };
    XrShell *s = (XrShell *)arg;
    size_t alt;
    int rc;
    /* the SIGSEGV handlers (every trapped device register, the crash report) run on the alternate signal stack in
     * an app (ART's libsigchain installs SA_ONSTACK); bionic's is 32 KB */
    alt = bvr_thread_altstack(256 * 1024);
    XLOGI("game thread starting (alternate signal stack %zu KB)", alt / 1024);
    if (s->preflight_on) bvr_pf_sigsegv("sigsegv-game-thread");
    rc = black_game_main(1, argv);
    if (rc != 0) {
        XLOGE("PREFLIGHT game-exit: FAIL the game returned %d during startup (the [BOOT]/Recomp lines above say why: "
              "default.xbe unreadable, memory layout, ...); orange panel shown", rc);
        s->pf_panel = XRS_PANEL_GAME_EXIT;
    } else {
        XLOGI("game thread returned 0");
    }
    return NULL;
}

static void on_cmd(struct android_app *app, int32_t cmd)
{
    XrShell *s = (XrShell *)app->userData;
    switch (cmd) {
    case APP_CMD_RESUME: s->resumed = 1; break;
    case APP_CMD_PAUSE: s->resumed = 0; break;
    case APP_CMD_DESTROY: s->exit_requested = 1; break;
    default: break;
    }
}

void android_main(struct android_app *app)
{
    XrShell *s = &g_xr;
    prctl(PR_SET_NAME, "bvr-xr", 0, 0, 0);   /* the XR loop; threads it starts are named by their own code */
    memset(s, 0, sizeof(*s));
    s->app = app;
    app->userData = s;
    app->onAppCmd = on_cmd;

    /* Device preflight (port/notes/device-preflight.md): one "PREFLIGHT <check>: OK|WARN|FAIL" line per check, tag
     * BlackVR, before anything can crash for a device-only reason. The game folder and storage are checked while
     * finding the folder; the rest after blackvr.ini, whose "env BLACKVR_PREFLIGHT=0" line skips them. */
    bvr_pf_begin(BLACKVR_VERSION);
    find_game_root(s);
    load_config(s);
    export_config(s);
    s->panel = xrs_panel_attach(s);          /* the in-headset settings panel (port/notes/settings-panel.md) */
    {
        const char *pf = getenv("BLACKVR_PREFLIGHT");
        s->preflight_on = !(pf && *pf == '0');
    }
    if (s->preflight_on) {
        xrp_process(s);
        xrp_audio();
        xrp_vulkan_probe();
    } else {
        XLOGW("PREFLIGHT skipped (blackvr.ini env BLACKVR_PREFLIGHT=0)");
    }
    vrb_init(s);
    {
        const char *in = app->activity->internalDataPath;
        snprintf(s->cache_dir, sizeof(s->cache_dir), "%s/kgpu-cache", in && *in ? in : s->files_dir);
        mkdir(s->cache_dir, 0770);
        seed_shader_cache(s);
    }
    {
        /* a fatal check (the guest window, the SIGSEGV trap path) means the game would crash at its first device
         * access: it is not started, a red panel shows; "env BLACKVR_PREFLIGHT_IGNORE=1" starts it anyway */
        const char *ign = getenv("BLACKVR_PREFLIGHT_IGNORE");
        const int fatal = bvr_pf_summary()->fatal > 0 && !(ign && *ign == '1');
        if (fatal) s->pf_panel = XRS_PANEL_FATAL;
        /* the renderer only drives the display when a game frame can come; otherwise the test pattern */
        s->want_renderer = s->game_files_present && black_game_main != NULL && !fatal;
    }
    {
        int ok;
        xrs_failure_clear();
        ok = xrs_instance_create(s);
        xrp_openxr(s, ok);
        if (ok) {
            xrs_failure_clear();
            ok = xrs_vulkan_create(s);
            xrp_vulkan_device(s, ok);
        }
        if (ok) {
            xrs_failure_clear();
            ok = xrs_session_create(s);
            xrp_session(s, ok);
        }
        if (!ok) {
            bvr_pf_summary_line("XR startup failed: the app exits");
            ANativeActivity_finish(app->activity);
            goto drain;
        }
    }

    if (s->game_files_present && black_game_main && s->pf_panel != XRS_PANEL_FATAL) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 16u << 20);   /* deep lifted call chains */
        if (pthread_create(&s->game_thread, &at, game_thread, s) == 0) s->game_started = 1;
        pthread_attr_destroy(&at);
        bvr_pf_summary_line(s->game_started ? "starting the game" : "pthread_create of the game thread FAILED");
    } else if (s->pf_panel == XRS_PANEL_FATAL) {
        bvr_pf_summary_line("NOT starting the game (a fatal check failed: red panel; blackvr.ini env "
                            "BLACKVR_PREFLIGHT_IGNORE=1 starts it anyway)");
    } else {
        XLOGI("no game %s: smoke test only", black_game_main ? "files" : "code linked");
        bvr_pf_summary_line(black_game_main ? "no game files: test pattern" : "smoke build (no game linked): test pattern");
    }

    while (!app->destroyRequested && !s->exit_requested) {
        int events;
        struct android_poll_source *src;
        /* block only while the session is idle and the activity is paused */
        const int timeout = (s->session_running || s->resumed) ? 0 : 250;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested || timeout) break;
        }
        xrs_poll_events(s);
        if (s->session_running) xrs_frame(s);
    }
    if (s->session && s->session_running) xrRequestExitSession(s->session);
    /* The game thread owns guest state the OS reclaims at process exit; the
     * recompiled runtime has no orderly cross-thread shutdown. */
    xrs_session_destroy(s);
    ANativeActivity_finish(app->activity);
drain:
    while (!app->destroyRequested) {
        int events;
        struct android_poll_source *src;
        if (ALooper_pollOnce(250, NULL, &events, (void **)&src) >= 0 && src) src->process(app, src);
    }
    if (s->game_started) _exit(0);
}
