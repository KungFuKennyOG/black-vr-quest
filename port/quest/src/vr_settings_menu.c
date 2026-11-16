/*
 * vr_settings_menu.c - the VR settings as the shell keeps them, for both front ends (port/notes/settings-panel.md):
 *   - the native page inside BLACK's own Options menu (src/black_vr_menu.inc, game thread), through vr_bridge.h's
 *     vr_menu_* functions;
 *   - the floating Dear ImGui panel (xr_panel.c, XR thread), the fallback (blackvr.ini settings_menu = panel).
 * The model and the blackvr.ini writer are pure (vr_settings.h); this file holds the state (what the process started
 * with, what is saved, what is being edited), live apply, Save, and the Info numbers. Every access to the state takes
 * vrsm_lock(). Side effects that need the XR thread's OpenXR calls (display refresh rate, foveation level,
 * performance levels) are queued here and run by vrsm_xr_frame(), which xrs_frame calls once per XR frame.
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "xr_shell.h"
#include "vr_settings_menu.h"

#ifndef BLACKVR_VERSION
#define BLACKVR_VERSION "dev"
#endif

static pthread_mutex_t g_lock;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;
static VrSettingsState S;
static int S_ready;
static volatile unsigned g_todo;          /* VRS_DO_* for the XR thread */

static void lock_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
    pthread_mutexattr_destroy(&a);
}
void vrsm_lock(void) { pthread_once(&g_lock_once, lock_init); pthread_mutex_lock(&g_lock); }
void vrsm_unlock(void) { pthread_mutex_unlock(&g_lock); }

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void vrsm_attach(XrShell *s)
{
    vrsm_lock();
    S.boot = s->cfg;
    S.saved = s->cfg;
    S.edit = s->cfg;
    S_ready = 1;
    vrsm_unlock();
}

VrSettingsState *vrsm(void)
{
    if (!S_ready) vrsm_attach(&g_xr);       /* a harness that set the config up itself */
    return &S;
}

void vrsm_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.status, sizeof(S.status), fmt, ap);
    va_end(ap);
    S.status_t = mono_s();
    XLOGI("settings: %s", S.status);
}

void vrsm_caps(XrShell *s)
{
    S.caps.has_refresh = s->has_refresh_rate && s->xrRequestDisplayRefreshRateFB && s->xrEnumerateDisplayRefreshRatesFB;
    S.caps.foveation_supported = s->has_foveation;
    S.caps.foveation_live = s->has_foveation && s->foveation_swapchain;
    S.caps.has_perf = s->has_perf_settings && s->xrPerfSettingsSetPerformanceLevelEXT;
}

void vrsm_begin_edit(XrShell *s)
{
    int id;
    vrsm();
    vrsm_caps(s);
    S.edit = s->cfg;
    /* restart settings: what is in the file (or edited earlier this run), not what the process runs with */
    for (id = 0; id < VRS_COUNT; id++)
        if (!vrs_is_live(id, &S.caps)) vrs_copy(&S.edit, &S.saved, id);
}

unsigned vrsm_apply_live(XrShell *s)
{
    const unsigned todo = vrs_apply_live(&s->cfg, &S.edit, &S.caps);
    __atomic_or_fetch(&g_todo, todo & (VRS_DO_REFRESH | VRS_DO_FOVEATION | VRS_DO_PERF), __ATOMIC_ACQ_REL);
    return todo;
}

void vrsm_xr_frame(XrShell *s)
{
    unsigned todo;
    char perf[40] = "";
    if (!g_todo) return;
    todo = __atomic_exchange_n(&g_todo, 0u, __ATOMIC_ACQ_REL);
    vrsm_lock();
    {
        const char *p = vrs_env_get(&s->cfg, "BLACKVR_PERF");
        if (p) snprintf(perf, sizeof(perf), "%s", p);
    }
    vrsm_unlock();
    if (todo & VRS_DO_REFRESH) xrs_refresh_request(s, 0);
    if (todo & VRS_DO_FOVEATION) xrs_foveation_update(s);
    if (todo & VRS_DO_PERF) xrs_perf_apply(s, perf[0] ? perf : NULL);
}

int vrsm_dirty(void)
{
    int id, d = 0;
    for (id = 0; id < VRS_COUNT; id++) d |= vrs_differs(&S.saved, &S.edit, id);
    return d;
}

void vrsm_ini_path(XrShell *s, char *p, size_t cap)
{
    snprintf(p, cap, "%s/blackvr.ini", s->game_root[0] ? s->game_root : s->files_dir);
}

int vrsm_save(XrShell *s)
{
    char path[600], tmp[620];
    char *old = NULL, *out;
    long n = 0;
    int len, pending = 0, id;
    FILE *f;
    vrsm_ini_path(s, path, sizeof(path));
    if ((f = fopen(path, "rb")) != NULL) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0 || n > 262144) { fclose(f); vrsm_status("Not saved: %s is too large", path); return 0; }
        old = (char *)calloc(1, (size_t)n + 1);
        if (!old || fread(old, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(old); vrsm_status("Not saved: cannot read %s", path); return 0; }
        fclose(f);
    }
    /* only what was changed (edit vs the stored values it started from) is written; every other setting keeps what
     * the file says now, so an edit made to the file while the game ran is not overwritten */
    {
        VrShellConfig target;
        vr_config_defaults(&target);
        vr_config_parse(&target, old ? old : "");
        for (id = 0; id < VRS_COUNT; id++)
            if (vrs_differs(&S.edit, &S.saved, id)) vrs_copy(&target, &S.edit, id);
        out = (char *)malloc((size_t)n + 8192);
        if (!out) { free(old); return 0; }
        len = vrs_ini_write(old, &target, out, (size_t)n + 8192);
    }
    free(old);
    if (len < 0) { free(out); vrsm_status("Not saved: internal error"); return 0; }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) { vrsm_status("Not saved: %s: %s", tmp, strerror(errno)); free(out); return 0; }
    if (fwrite(out, 1, (size_t)len, f) != (size_t)len || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        vrsm_status("Not saved: write %s: %s", tmp, strerror(errno));
        fclose(f);
        remove(tmp);
        free(out);
        return 0;
    }
    fclose(f);
    free(out);
    if (rename(tmp, path) != 0) { vrsm_status("Not saved: rename to %s: %s", path, strerror(errno)); remove(tmp); return 0; }
    for (id = 0; id < VRS_COUNT; id++) vrs_copy(&S.saved, &S.edit, id);
    for (id = 0; id < VRS_COUNT; id++) pending += vrs_pending_restart(&S.boot, &S.edit, &S.caps, id);
    if (pending) vrsm_status("Saved. %d setting%s at the next start", pending, pending > 1 ? "s apply" : " applies");
    else vrsm_status("Saved to %s", path);
    return 1;
}

/* ---- per-thread CPU (the HEARTBEAT threads line's method: utime + stime from /proc/self/task/<tid>/stat) ---- */

typedef struct { int tid; unsigned long long ticks; } TaskTicks;
static pthread_mutex_t g_samp = PTHREAD_MUTEX_INITIALIZER;
static int g_samp_want, g_samp_alive, g_nthreads;
static VrpThreadUse g_threads[VRP_MAX_THREADS];

static int sample_threads(TaskTicks *prev, int *nprev, double dt, VrpThreadUse *top, int max)
{
    TaskTicks cur[192];
    VrpThreadUse all[192];
    int ncur = 0, nall = 0, i, j;
    const long hz = sysconf(_SC_CLK_TCK);
    DIR *dir = opendir("/proc/self/task");
    struct dirent *e;
    if (!dir || hz <= 0) { if (dir) closedir(dir); return 0; }
    while ((e = readdir(dir)) && ncur < 192) {
        char path[64], buf[512], *p, *q;
        FILE *f;
        const int tid = atoi(e->d_name);
        unsigned long long ut = 0, st = 0;
        if (tid <= 0) continue;
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        if (!(f = fopen(path, "r"))) continue;
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
        fclose(f);
        if (!(p = strchr(buf, '(')) || !(q = strrchr(buf, ')'))) continue;
        {
            char *tok, *save = NULL;
            int field = 3;
            for (tok = strtok_r(q + 2, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
                if (field == 14) ut = strtoull(tok, NULL, 10);
                else if (field == 15) { st = strtoull(tok, NULL, 10); break; }
            }
        }
        cur[ncur].tid = tid;
        cur[ncur].ticks = ut + st;
        for (j = 0; j < *nprev; j++)
            if (prev[j].tid == tid) {
                const double pct = (double)(cur[ncur].ticks - prev[j].ticks) * 100.0 / ((double)hz * dt);
                if (pct >= 1.0) {
                    size_t nl = (size_t)(q - p - 1);
                    if (nl >= sizeof(all[0].name)) nl = sizeof(all[0].name) - 1;
                    memcpy(all[nall].name, p + 1, nl);
                    all[nall].name[nl] = 0;
                    all[nall].tid = tid;
                    all[nall].pct = (float)pct;
                    nall++;
                }
                break;
            }
        ncur++;
    }
    closedir(dir);
    memcpy(prev, cur, sizeof(cur[0]) * (size_t)ncur);
    *nprev = ncur;
    for (i = 0; i < nall; i++)                     /* busiest first */
        for (j = i + 1; j < nall; j++)
            if (all[j].pct > all[i].pct) { VrpThreadUse t = all[i]; all[i] = all[j]; all[j] = t; }
    for (i = 0; i < nall && i < max; i++) top[i] = all[i];
    return i;
}

static void *sampler_thread(void *arg)
{
    static TaskTicks prev[192];
    int nprev = 0;
    double last = mono_s();
    (void)arg;
    sample_threads(prev, &nprev, 1.0, g_threads, 0);   /* the baseline */
    for (;;) {
        VrpThreadUse top[VRP_MAX_THREADS];
        int n, k;
        double now;
        for (k = 0; k < 10; k++) {
            usleep(100000);
            pthread_mutex_lock(&g_samp);
            if (!g_samp_want) { g_samp_alive = 0; pthread_mutex_unlock(&g_samp); return NULL; }
            pthread_mutex_unlock(&g_samp);
        }
        now = mono_s();
        n = sample_threads(prev, &nprev, now - last, top, VRP_MAX_THREADS);
        last = now;
        pthread_mutex_lock(&g_samp);
        memcpy(g_threads, top, sizeof(top[0]) * (size_t)n);
        g_nthreads = n;
        pthread_mutex_unlock(&g_samp);
    }
}

/* Runs only while a settings page is shown. */
void vrsm_sampler(int want)
{
    pthread_mutex_lock(&g_samp);
    g_samp_want = want;
    if (want && !g_samp_alive) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, sampler_thread, NULL) == 0) {
            g_samp_alive = 1;
            pthread_setname_np(th, "bvr-panel-cpu");
        }
        pthread_attr_destroy(&at);
    }
    pthread_mutex_unlock(&g_samp);
}

static void info_static(XrShell *s)
{
    VkPhysicalDeviceProperties pp;
    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    VrpInfo *i = &S.info;
    memset(&pp, 0, sizeof(pp));
    if (s->vk_phys) vkGetPhysicalDeviceProperties(s->vk_phys, &pp);
    snprintf(i->gpu_name, sizeof(i->gpu_name), "%s", pp.deviceName[0] ? pp.deviceName : "?");
    if (pp.vendorID == 0x5143)                     /* Qualcomm: the driver version packs as a Vulkan version */
        snprintf(i->gpu, sizeof(i->gpu), "%s, driver %u.%u.%u, Vulkan %u.%u", i->gpu_name,
                 VK_API_VERSION_MAJOR(pp.driverVersion), VK_API_VERSION_MINOR(pp.driverVersion),
                 VK_API_VERSION_PATCH(pp.driverVersion), VK_API_VERSION_MAJOR(s->vk_api), VK_API_VERSION_MINOR(s->vk_api));
    else
        snprintf(i->gpu, sizeof(i->gpu), "%s, driver 0x%08X, Vulkan %u.%u", i->gpu_name, pp.driverVersion,
                 VK_API_VERSION_MAJOR(s->vk_api), VK_API_VERSION_MINOR(s->vk_api));
    if (s->instance && XR_SUCCEEDED(xrGetInstanceProperties(s->instance, &ip)))
        snprintf(i->runtime, sizeof(i->runtime), "%s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                 XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    else
        snprintf(i->runtime, sizeof(i->runtime), "?");
    snprintf(i->build, sizeof(i->build), "%s (shell built %s)", BLACKVR_VERSION, __DATE__);
    vrsm_ini_path(s, i->ini_path, sizeof(i->ini_path));
}

/* The Info numbers, at most every 0.5 s. */
void vrsm_info_update(XrShell *s)
{
    static uint64_t d_frames, d_xr;
    static double d_t, last;
    static int have_static;
    VrDiagCounters d;
    const double now = mono_s();
    if (last && now - last < 0.5) return;
    last = now;
    if (!have_static) { info_static(s); have_static = 1; }
    vr_diag_counters(&d);
    if (d_t > 0.0 && now - d_t > 0.05) {
        const double dt = now - d_t;
        S.info.game_fps = (float)((double)(d.eye_releases + d.flat_releases - d_frames) / dt);
        S.info.xr_fps = (float)((double)(d.xr_frames - d_xr) / dt);
    }
    d_frames = d.eye_releases + d.flat_releases;
    d_xr = d.xr_frames;
    d_t = now;
    S.info.display_hz = s->refresh_hz;
    S.info.eye_w = s->eye[0].width;
    S.info.eye_h = s->eye[0].height;
    S.info.res_scale = S.boot.resolution_scale;
    S.info.multiview = s->multiview;
    S.info.foveation_applied = s->foveation_applied;
    pthread_mutex_lock(&g_samp);
    S.info.nthreads = g_nthreads;
    memcpy(S.info.threads, g_threads, sizeof(g_threads));
    pthread_mutex_unlock(&g_samp);
}

/* ---- the native page's side (vr_bridge.h vr_menu_*; game thread) --------------------------------------------- */

/* The rows of the list of sections: the sections, then RECALIBRATE HEIGHT and RESTORE DEFAULTS. */
enum { MENU_ACT_RECAL = 100, MENU_ACT_DEFAULTS = 101 };

static int menu_items(int section, int *items, int max)
{
    int n = 0, k;
    if (section < 0) {
        for (k = 0; k < VRS_SEC_COUNT && n < max; k++) items[n++] = -1 - k;          /* -1 - section */
        if (n < max) items[n++] = MENU_ACT_RECAL;
        if (n < max) items[n++] = MENU_ACT_DEFAULTS;
        return n;
    }
    if (section == VRS_SEC_INFO) {
        for (k = 0; k < 9 && n < max; k++) items[n++] = 200 + k;                    /* info lines */
        return n;
    }
    return vrs_section_settings(section, items, max);
}

static void upper(char *t)
{
    for (; *t; t++) if (*t >= 'a' && *t <= 'z') *t = (char)(*t - 32);
}

static void info_row(int k, VrMenuRow *r)
{
    const VrpInfo *i = &S.info;
    switch (k) {
    case 0: snprintf(r->name, sizeof(r->name), "GAME"); snprintf(r->value, sizeof(r->value), "%.0f FPS", i->game_fps); break;
    case 1: snprintf(r->name, sizeof(r->name), "XR LOOP"); snprintf(r->value, sizeof(r->value), "%.0f FPS", i->xr_fps); break;
    case 2:
        snprintf(r->name, sizeof(r->name), "DISPLAY");
        if (i->display_hz > 0.0f) snprintf(r->value, sizeof(r->value), "%.0f HZ", i->display_hz);
        else snprintf(r->value, sizeof(r->value), "-");
        break;
    case 3: snprintf(r->name, sizeof(r->name), "EYE IMAGES"); snprintf(r->value, sizeof(r->value), "%uX%u", i->eye_w, i->eye_h); break;
    case 4: case 5: case 6: {
        const int t = k - 4;
        snprintf(r->name, sizeof(r->name), "CPU %d", t + 1);
        if (t < i->nthreads) snprintf(r->value, sizeof(r->value), "%.11s %.0f%%", i->threads[t].name, i->threads[t].pct);
        else snprintf(r->value, sizeof(r->value), "-");
        break;
    }
    case 7: snprintf(r->name, sizeof(r->name), "GPU"); snprintf(r->value, sizeof(r->value), "%.16s", i->gpu_name); break;
    default: snprintf(r->name, sizeof(r->name), "BUILD"); snprintf(r->value, sizeof(r->value), "%.16s", BLACKVR_VERSION); break;
    }
    upper(r->value);
    r->kind = VR_MENU_ROW_INFO;
}

void vr_menu_enter(void)
{
    XrShell *s = &g_xr;
    vrsm_lock();
    vrsm_begin_edit(s);
    vrsm_unlock();
    vrsm_sampler(1);
    XLOGI("settings: VR SETTINGS page entered");
}

int vr_menu_rows(int section, VrMenuRow *out, int max)
{
    XrShell *s = &g_xr;
    int items[32], n, k;
    vrsm_lock();
    vrsm();
    vrsm_caps(s);
    if (section == VRS_SEC_INFO) vrsm_info_update(s);
    n = menu_items(section, items, max < 32 ? max : 32);
    for (k = 0; k < n; k++) {
        VrMenuRow *r = &out[k];
        memset(r, 0, sizeof(*r));
        if (items[k] < 0) {
            snprintf(r->name, sizeof(r->name), "%s", k_vrs_sec_names[-1 - items[k]]);
            snprintf(r->value, sizeof(r->value), " ");
            r->kind = VR_MENU_ROW_SECTION;
        } else if (items[k] == MENU_ACT_RECAL) {
            snprintf(r->name, sizeof(r->name), "RECALIBRATE HEIGHT");
            snprintf(r->value, sizeof(r->value), " ");
            r->kind = VR_MENU_ROW_ACTION;
        } else if (items[k] == MENU_ACT_DEFAULTS) {
            snprintf(r->name, sizeof(r->name), "RESTORE DEFAULTS");
            snprintf(r->value, sizeof(r->value), " ");
            r->kind = VR_MENU_ROW_ACTION;
        } else if (items[k] >= 200) {
            info_row(items[k] - 200, r);
        } else {
            snprintf(r->name, sizeof(r->name), "%s", k_vrs[items[k]].label);
            upper(r->name);
            vrs_menu_value(&S.edit, &S.boot, &S.caps, items[k], r->value, sizeof(r->value));
            r->kind = vrs_available(items[k], &S.caps) == VRS_AVAILABLE ? VR_MENU_ROW_SETTING : VR_MENU_ROW_INFO;
        }
    }
    vrsm_unlock();
    return n;
}

/* A bool setting flipped from a button in the game (the gun hand's B: the laser sight), applied and saved at once. */
void vrsm_toggle(int id, const char *what)
{
    XrShell *s = &g_xr;
    vrsm_lock();
    vrsm();
    vrsm_begin_edit(s);
    vrs_set(&S.edit, id, vrs_get(&S.edit, id) != 0.0f ? 0.0f : 1.0f);
    vrsm_apply_live(s);
    if (vrsm_dirty()) vrsm_save(s);
    XLOGI("settings: %s %s (button)", what, vrs_get(&S.edit, id) != 0.0f ? "on" : "off");
    vrsm_unlock();
}

int vr_menu_activate(int section, int row, int dir)
{
    XrShell *s = &g_xr;
    int items[32], n, r = VR_MENU_NOTHING;
    vrsm_lock();
    vrsm();
    n = menu_items(section, items, 32);
    if (row >= 0 && row < n) {
        const int it = items[row];
        if (it < 0) {
            if (dir == 0) r = VR_MENU_OPEN_SECTION_0 + (-1 - it);
        } else if (it == MENU_ACT_RECAL) {
            if (dir == 0) { vr_request_recentre(); vrsm_status("height recalibrated"); r = VR_MENU_DONE; }
        } else if (it == MENU_ACT_DEFAULTS) {
            if (dir == 0) { vrs_reset_defaults(&S.edit); vrsm_apply_live(s); vrsm_status("defaults"); r = VR_MENU_CHANGED; }
        } else if (it < VRS_COUNT && vrs_available(it, &S.caps) == VRS_AVAILABLE) {
            if (vrs_step(&S.edit, it, dir)) {
                vrsm_apply_live(s);
                r = VR_MENU_CHANGED;
            }
        }
    }
    vrsm_unlock();
    return r;
}

const char *vr_menu_title(int section)
{
    return section >= 0 && section < VRS_SEC_COUNT ? k_vrs_sec_names[section] : "VR SETTINGS";
}

void vr_menu_leave(void)
{
    XrShell *s = &g_xr;
    vrsm_lock();
    if (vrsm_dirty()) vrsm_save(s);
    vrsm_unlock();
    vrsm_sampler(0);
    XLOGI("settings: VR SETTINGS page left");
}

int vr_menu_native(void)
{
    return g_xr.cfg.settings_menu != VR_SETTINGS_MENU_PANEL;
}
