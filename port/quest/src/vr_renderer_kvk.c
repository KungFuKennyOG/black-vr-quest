/*
 * vr_renderer_kvk.c - the shell's renderer hooks (vr_bridge.h: vr_renderer_get_requirements, vr_renderer_init,
 * vr_renderer_shutdown, vr_mark_world_pass) implemented with the Kelvin Vulkan backend
 * (third_party/xboxrecomp/src/nv2a/kelvin/vk), and the backend's VR hooks (kvk_vr_hooks.h). Linked into the game
 * build only (port/quest/CMakeLists.txt: black_vk_renderer, whole archive, so these strong definitions replace the
 * shell's and the backend's weak ones). Notes: port/notes/vr-glue.md.
 *
 * Call order and threads
 *   XR thread    vr_renderer_get_requirements()     before the VkInstance exists
 *                [shell: instance, device with those features (where present), session, swapchains]
 *                vr_renderer_init(ctx)              kvk_set_device/raster_size/stereo/cache_dir; the shell calls it
 *                                                    only when the game will start, before the game thread exists
 *   game thread  black_game_main -> ... -> kelvin_start() -> Kelvin GPU thread -> kgpu_init() (takes what kvk_set_*
 *                stored). Per game frame: the BLACK_VR camera hook publishes VrGameCamera + Ext (head pose,
 *                game_frame, present mode); the world pass calls vr_mark_world_pass(va, 1): the camera is copied to
 *                a slot of this file's ring and kelvin_mark_view(va, KVK_VIEW_VR_CAMERA, slot) puts it in the push
 *                buffer at the world's first word.
 *   GPU thread   kgpu_set_view(KVK_VIEW_VR_CAMERA, slot) -> kvk_vr_view_mark(): this frame's camera.
 *                first draw  -> kvk_vr_stereo_hook(FIRST_DRAW): preliminary eye transforms (the game-time eye
 *                               offsets around the camera's head pose).
 *                flip        -> kvk_vr_frame_mode(): STEREO -> vr_acquire_eye_targets() latches the eye poses
 *                               (VrRenderPose) -> kvk_vr_stereo_hook(FLIP): the transforms for exactly those poses,
 *                               written into the frame's stereo blocks before the submit -> vr_release_eye_targets()
 *                               submits exactly those poses. FLAT_* -> vr_acquire_flat_target(), no eye images.
 *                after       -> kvk_vr_frame_done().
 *
 * The one per-eye correction: C_i = P_eye_i * E_i * R(P_game) (kvk_stereo.c, FROM_W form) with
 *   E_i = inverse(eye_i) * game_head   (stage poses, axes of the game's view space, translation in game units),
 * eye_i = VrRenderPose.eye_pose[i] (what the shell submits) and game_head = the pose the game built the camera from.
 * The head motion between the game's latch and the render-time latch is in E_i once; what is left between the
 * render-time latch and display is the compositor's (it is given eye_i). Nothing else corrects the world.
 *
 * Gun latch (port/notes/gun-latch.md). The title draws its first-person weapon at the hand pose of its own frame. The
 * game hook re-targets the weapon's MVP uploads to the hand (black_vr.inc) and records, with the frame's camera mark,
 * the hand pose it used (VrGameGun) and the exact MVPs it uploaded (vr_mark_viewmodel_mvp). A world draw whose position
 * is made through one of those MVPs, bit for bit (kvk_vr_viewmodel_draw), reads the backend's second stereo block;
 * at the flip it gets E'_i = inverse(eye_i) * D * game_head with D = hand_late * inverse(hand_game) (stage, the hand
 * located in the same latch as eye_i, VrRenderGun): the weapon is drawn where the controller is at display time.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <vulkan/vulkan.h>

#include "kvk_device.h"
#include "kvk_stereo.h"
#include "kvk_vr_hooks.h"
#include "vr_arms.h"
#include "vr_game_math.h"
#include "vr_bridge.h"
#include "vr_math.h"

#ifdef __ANDROID__
#include <android/log.h>
#define GLOG(...) __android_log_print(ANDROID_LOG_INFO, "BlackVR", __VA_ARGS__)
#else
#define GLOG(...) do { fprintf(stderr, "[vr-glue] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

/* the Kelvin engine (kelvin.h) */
extern int kelvin_mark_view(uint32_t va, int mode, int slot, int x0, int y0, int x1, int y1);

#define GLUE_RING 16   /* > kelvin_mark_view's queue (16 marks): a slot cannot be reused before its mark is reached */
#define GLUE_VM_MAX 8  /* weapon MVPs per frame (BLACK: the mesh queue's first-person entries and the muzzle flash) */

typedef struct GlueVm { float mvp[16]; int reg; } GlueVm;
typedef struct GlueCam {
    VrGameCamera cam;
    VrGameCameraExt ext;
    VrGameGun gun;           /* gun latch: where the frame's weapon was placed */
    uint32_t serial;         /* the mark's number (a slot is checked against it before its live MVP list is read) */
    int nvm;                 /* weapon MVPs recorded so far for this frame (grows after the mark, read live) */
    GlueVm vm[GLUE_VM_MAX];
    float offhand[16];       /* first-person arms: K = hand_from_model (vr_mark_viewmodel_offhand) */
    int offhand_ok;
} GlueCam;

static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static GlueCam g_ring[GLUE_RING];
static uint32_t g_ring_next;
static int g_ring_last = -1;          /* the slot of the newest mark: the weapon MVPs that follow belong to it */
static volatile int g_on;

/* GPU thread: the frame being drawn */
static struct {
    int marked;          /* a camera mark arrived this frame */
    int slot;            /* ... in this ring slot */
    GlueCam mark;
    int chosen;          /* `cur` decided (and handed to the shell) */
    GlueCam cur;
    int mode;
    int registered;
} F;

/* counters (diagnostics, tests) */
static volatile uint32_t g_marks_made, g_marks_failed, g_marks_seen, g_hud_marks;
static volatile uint32_t g_vm_recorded, g_vm_dropped, g_vm_frames;
static uint32_t g_captures;          /* both-eye captures written */

/* ---- vr_bridge.h: renderer side -------------------------------------------------------------------------------- */

void vr_renderer_get_requirements(VrVulkanRequirements *req)
{
    static VkPhysicalDeviceFeatures f;
    memset(req, 0, sizeof(*req));
    memset(&f, 0, sizeof(f));
    f.robustBufferAccess = VK_TRUE;     /* out-of-range vertex reads return 0, as D3D11's typed buffers */
    f.depthClamp = VK_TRUE;
    f.samplerAnisotropy = VK_TRUE;      /* optional */
    f.textureCompressionBC = VK_TRUE;   /* optional (Quest has none: DXT decoded on the CPU) */
    req->device_features10 = &f;        /* the shell enables these where the device has them; multiview always */
    req->api_version = VK_API_VERSION_1_1;
}

static void glue_queue_lock(void *ctx) { (void)ctx; vr_queue_lock(); }
static void glue_queue_unlock(void *ctx) { (void)ctx; vr_queue_unlock(); }

int vr_renderer_init(const VrVulkanContext *ctx)
{
    const VrVulkanFeatures *f = vr_vulkan_features();
    KvkDeviceInfo di;
    if (!ctx || !ctx->device || !ctx->queue) return 0;
    if (!f->multiview) {
        GLOG("the device has no multiview: the Kelvin backend cannot draw stereo; test pattern stays");
        return 0;
    }
    memset(&di, 0, sizeof(di));
    di.instance = (VkInstance)(uintptr_t)ctx->instance;
    di.physical = (VkPhysicalDevice)(uintptr_t)ctx->physical_device;
    di.device = (VkDevice)(uintptr_t)ctx->device;
    di.queue = (VkQueue)(uintptr_t)ctx->queue;
    di.queue_family = ctx->queue_family;
    di.api_version = f->api_version ? f->api_version : ctx->api_version;
    di.multiview = f->multiview != 0;
    di.robust_buffer_access = f->robust_buffer_access != 0;
    di.depth_clamp = f->depth_clamp != 0;
    di.sampler_anisotropy = f->sampler_anisotropy != 0;
    di.texture_compression_bc = f->texture_compression_bc != 0;
    di.swapchain_mutable_format = f->swapchain_mutable_format != 0;
    /* xrEndFrame / xrAcquire/ReleaseSwapchainImage use the queue on the XR thread */
    di.queue_lock = glue_queue_lock;
    di.queue_unlock = glue_queue_unlock;
    /* all of this is read by kgpu_init() on the Kelvin GPU thread, which kelvin_start() creates later */
    kvk_set_device(&di);
    kvk_set_raster_size(ctx->eye_width, ctx->eye_height);   /* the 640x480 targets at eye resolution, per eye */
    kvk_set_stereo(true);
    kvk_set_cache_dir(vr_cache_dir());
    setenv("RECOMP_NV2A_KELVIN", "1", 0);                    /* the NV2A engine (and so this backend), unless set */
    g_on = 1;
    GLOG("Kelvin Vulkan backend attached: raster %ux%u per eye, %s, cache %s", ctx->eye_width, ctx->eye_height,
         ctx->multiview ? "one 2-layer swapchain" : "two swapchains", vr_cache_dir());
    return 1;
}

void vr_renderer_shutdown(void)
{
    /* Presentation stops here (the hooks stand down). kvk_shutdown() is not called: the game's Kelvin GPU thread
     * may still be inside the backend, and the process exits right after (xr_main.c). The pipeline cache is
     * saved by the backend every 1800 frames. */
    g_on = 0;
    GLOG("renderer detached: %u camera marks made (%u refused), %u reached, %u HUD marks; gun latch: %u weapon MVPs "
         "recorded (%u dropped), %u frames moved the weapon", g_marks_made, g_marks_failed, g_marks_seen, g_hud_marks,
         g_vm_recorded,
         g_vm_dropped, g_vm_frames);
}

/* Game thread, at the world pass (src/black_vr.inc black_vr_world_mark). begin 1: the world camera's draws start;
 * 2: the game starts drawing its HUD (scripts/generate.py hooks the world draw there), so everything after is HUD
 * whatever its depth state; 0: the end, no mark. Without the 2 the backend finds the HUD by its first depth-ALWAYS
 * draw, and HUD boxes drawn before that one (the pickup caption, secondary-objective messages: depth test off)
 * were taken for world effects and pushed far out into the scene. */
#define GLUE_VIEW_VR_SCREEN 17          /* kelvin_gfx.h KG_VIEW_VR_SCREEN */
void vr_mark_world_pass(uint32_t pushbuffer_va, int begin)
{
    uint32_t k;
    if (!g_on || !begin) return;
    if (begin == 2) {
        if (kelvin_mark_view(pushbuffer_va, GLUE_VIEW_VR_SCREEN, 0, 0, 0, 640, 480)) g_hud_marks++;
        return;
    }
    pthread_mutex_lock(&g_ring_lock);
    k = g_ring_next++ % GLUE_RING;
    g_ring[k].cam = *vr_game_camera();  /* what the world pass is drawn with: the newest published camera */
    g_ring[k].ext = *vr_game_camera_ext();
    g_ring[k].gun = *vr_game_gun();     /* and where its weapon was placed */
    g_ring[k].serial = g_ring_next;
    g_ring[k].nvm = 0;
    g_ring[k].offhand_ok = 0;
    g_ring_last = -1;
    pthread_mutex_unlock(&g_ring_lock);
    if (kelvin_mark_view(pushbuffer_va, KVK_VIEW_VR_CAMERA, (int)k, 0, 0, 640, 480)) {
        g_marks_made++;
        pthread_mutex_lock(&g_ring_lock);
        g_ring_last = (int)k;           /* a refused mark gets no weapon MVPs: its frame is never matched */
        pthread_mutex_unlock(&g_ring_lock);
    } else {
        g_marks_failed++;
    }
}

/* Game thread, after a weapon MVP upload was re-targeted (black_vr.inc): it belongs to the newest mark's frame. Its
 * constants reach the push buffer after this call, so the GPU thread never sees the draw before the record. */
void vr_mark_viewmodel_mvp(const float mvp[16], int constant_register)
{
    GlueCam *c;
    if (!g_on || constant_register < 0 || constant_register > 188) return;
    pthread_mutex_lock(&g_ring_lock);
    if (g_ring_last >= 0 && (c = &g_ring[g_ring_last])->nvm < GLUE_VM_MAX) {
        memcpy(c->vm[c->nvm].mvp, mvp, sizeof(c->vm[c->nvm].mvp));
        c->vm[c->nvm].reg = constant_register;
        c->nvm++;
        g_vm_recorded++;
    } else {
        g_vm_dropped++;
    }
    pthread_mutex_unlock(&g_ring_lock);
}

/* Game thread, after the weapon mesh's MVP: its off-hand mirror, into the same (newest accepted) mark's slot. */
void vr_mark_viewmodel_offhand(const float a[16])
{
    if (!g_on || !a) return;
    pthread_mutex_lock(&g_ring_lock);
    if (g_ring_last >= 0) {
        memcpy(g_ring[g_ring_last].offhand, a, sizeof(g_ring[g_ring_last].offhand));
        g_ring[g_ring_last].offhand_ok = 1;
    }
    pthread_mutex_unlock(&g_ring_lock);
}

/* ---- kvk_vr_hooks.h (GPU thread) ------------------------------------------------------------------------------- */

static int ext_mode(const VrGameCameraExt *e)
{
    return e->game_frame && e->present_mode >= VR_PRESENT_STEREO && e->present_mode <= VR_PRESENT_FLAT_SCOPE
        ? e->present_mode : -1;
}

/* The camera and presentation of the frame being drawn, decided once (again after a camera mark) and handed to the
 * shell, which latches the eye poses against it. Without a mark (no world pass this frame, or a game hook that
 * marks STEREO frames only): the newest published camera and the game's current mode. */
static void choose_frame(void)
{
    int m;
    if (F.chosen) return;
    if (F.marked) {
        F.cur = F.mark;
    } else {
        F.cur.cam = *vr_game_camera();
        F.cur.ext = *vr_game_camera_ext();
        if (!F.cur.ext.game_frame) F.cur.ext.head_pose = vrm_pose_identity();
    }
    m = F.marked ? ext_mode(&F.cur.ext) : -1;
    F.mode = m >= 0 ? m : vr_present_mode();
    F.chosen = 1;
    vr_set_render_gun(F.marked ? &F.cur.gun : NULL);   /* gun latch: unmarked frames have no weapon pose */
    vr_set_render_frame(&F.cur.cam, &F.cur.ext, F.mode);
}

void kvk_vr_view_mark(int mode, int slot)
{
    if (mode != KVK_VIEW_VR_CAMERA || slot < 0 || slot >= GLUE_RING) return;
    pthread_mutex_lock(&g_ring_lock);
    F.mark = g_ring[slot];
    pthread_mutex_unlock(&g_ring_lock);
    F.slot = slot;
    F.marked = 1;
    F.chosen = 0;
    g_marks_seen++;
}

int kvk_vr_frame_mode(void)
{
    if (!g_on) return -1;
    choose_frame();
    return F.mode;
}

void kvk_vr_frame_done(void)
{
    F.marked = 0;
    F.chosen = 0;
}

/* eye_clip for (head, eyes) through the frame's camera; quad_mvp: the virtual screen (only drawn when there is no
 * HUD layer) locked to `screen_head`, placed as the shell places its HUD quad. */
static void stereo_for(KvkStereoFrame *out, const KvkStereoConfig *cfg, const VrFrameState *f, const VrPose *screen_head)
{
    const VrHudLayout *hl = vr_hud_layout();
    KvkStereoConfig qc = *cfg;
    KvkStereoFrame q;
    VrFrameState fq = *f;
    kvk_stereo_frame(out, cfg, f, &F.cur.cam);
    qc.screen_distance_m = hl->distance_m;
    qc.screen_width_m = hl->width_m;
    fq.head = *screen_head;
    kvk_stereo_frame(&q, &qc, &fq, &F.cur.cam);
    memcpy(out->quad_mvp, q.quad_mvp, sizeof(out->quad_mvp));
}

bool kvk_vr_stereo_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase)
{
    VrFrameState f;
    int i;
    if (!g_on) return false;
    if (!F.registered) { vr_register_render_thread(); F.registered = 1; }
    choose_frame();
    if (phase == KVK_VR_PHASE_FLIP) {
        /* the poses vr_acquire_eye_targets() just latched for this frame; submitted unchanged */
        const VrRenderPose *rp = vr_render_pose();
        if (!rp->valid) return false;
        memset(&f, 0, sizeof(f));
        f.head = rp->game_head;
        for (i = 0; i < VR_EYE_COUNT; i++) { f.eye[i].pose = rp->eye_pose[i]; f.eye[i].fov = rp->eye_fov[i]; }
        f.world_scale = rp->world_scale;
        f.predicted_display_time = rp->display_time;
        f.session_running = 1;
        stereo_for(out, cfg, &f, &rp->head);
        return true;
    }
    /* first draw (preliminary, replaced at the flip): the game latch's eye offsets around the camera's head */
    {
        const VrFrameState *fs = vr_frame_state();
        VrPose head_g, to_g;
        if (!fs) return false;
        f = *fs;
        head_g = F.cur.ext.game_frame ? F.cur.ext.head_pose : fs->head;
        to_g = vrm_pose_mul(head_g, vrm_pose_inv(fs->head));
        for (i = 0; i < VR_EYE_COUNT; i++) f.eye[i].pose = vrm_pose_mul(to_g, fs->eye[i].pose);
        f.head = head_g;
        stereo_for(out, cfg, &f, &head_g);
        return true;
    }
}

/* ---- gun latch (kvk_vr_hooks.h; port/notes/gun-latch.md) ------------------------------------------------------- */

/* A draw is the weapon when its position reads, by absolute address, the four registers one of this frame's weapon
 * MVPs was uploaded to, and they hold that MVP bit for bit: geometry placed through the matrix made for the weapon.
 * Only marked frames, only the live list of the mark's own slot (checked by serial); anything else is the world. */
bool kvk_vr_viewmodel_draw(const float *constants, const uint32_t pos_consts[6])
{
    const VrGunLatchConfig *gc;
    const GlueCam *c;
    bool hit = false;
    int i, k;
    if (!g_on || !F.marked || !constants || !pos_consts) return false;
    gc = vr_gun_latch_config();
    if (!gc->enabled && !gc->debug_tint) return false;
    pthread_mutex_lock(&g_ring_lock);
    c = &g_ring[F.slot];
    if (c->serial == F.mark.serial && c->gun.valid) {
        for (i = 0; i < c->nvm && !hit; i++) {
            const int r = c->vm[i].reg;
            for (k = 0; k < 4; k++) if (!(pos_consts[(r + k) >> 5] & (1u << ((r + k) & 31)))) break;
            if (k == 4 && !memcmp(constants + (size_t)r * 4, c->vm[i].mvp, sizeof(c->vm[i].mvp))) hit = true;
        }
    }
    pthread_mutex_unlock(&g_ring_lock);
    return hit;
}

/* ---- first-person arms (vr_arms.h, port/notes/arms.md) -------------------------------------------------------
 * The weapon's vertex data lies inside its loaded MODEL resource (docs/file-formats/models.md): the model root P sits
 * before it, marked by the GtID MODEL (0x95820AD4B8000000 at P+10) and version 0x23 (P+18). P+5C is the bone count,
 * P+60 the bone-name pointer table and P+54 the bone matrices (inverse bind). The loader may have relocated these
 * offsets to guest addresses, so both forms are accepted. Results are cached per vertex pointer. Guest memory is read
 * only inside the mapped windows (as black_pc_ram_span). */
extern ptrdiff_t g_xbox_mem_offset __attribute__((weak));
void *xbox_GetMemoryBase(void) __attribute__((weak));
size_t xbox_GetMappedSize(void) __attribute__((weak));
void *xbox_GetPhysicalMemoryBase(void) __attribute__((weak));

#define ARMS_CONTIG_BASE 0x80000000u
#define ARMS_CONTIG_SIZE (64u * 1024u * 1024u)
#define ARMS_SCAN_BYTES  (256u * 1024u)
#define ARMS_CACHE       8

static int arms_span(uint64_t va, uint64_t bytes)
{
    const uint64_t end = va + bytes;
    if (!&g_xbox_mem_offset || !xbox_GetMemoryBase || !xbox_GetMappedSize) return 0;
    if (va >= 0x10000u && xbox_GetMemoryBase() != NULL && end <= (uint64_t)xbox_GetMappedSize()) return 1;
    return va >= ARMS_CONTIG_BASE && end <= (uint64_t)ARMS_CONTIG_BASE + ARMS_CONTIG_SIZE && xbox_GetPhysicalMemoryBase &&
           xbox_GetPhysicalMemoryBase() != NULL;
}

static uint32_t arms_u32(uint64_t va)
{
    uint32_t v;
    memcpy(&v, (const void *)((uintptr_t)g_xbox_mem_offset + (uintptr_t)va), 4);
    return v;
}

/* A field of the model root: an offset relative to P, or (relocated) a guest address. 0 if neither is readable. */
static uint64_t arms_ptr(uint64_t P, uint32_t field, uint32_t bytes)
{
    const uint32_t v = arms_u32(P + field);
    if (v < 0x00400000u && arms_span(P + v, bytes)) return P + v;
    if (arms_span(v, bytes)) return v;
    return 0;
}

typedef struct {
    const void *vdata;          /* key: the draw's vertex data */
    int ok;                     /* 1: rig valid, 0: not a known model (do nothing) */
    uint64_t P;                 /* the model root (rechecked each use: memory is reused across levels) */
    unsigned retry;             /* a miss is retried after this many uses */
    float corr[3];              /* auto-grip correction for this weapon (game units) */
    unsigned corr_n;            /* measurements so far */
    VrArmRig rig;
} ArmsEntry;

static ArmsEntry g_arms[ARMS_CACHE];
static unsigned g_arms_next;
static unsigned g_arms_log;

static int arms_load(ArmsEntry *e, const void *vdata)
{
    const uint64_t va = (uint64_t)((uintptr_t)vdata - (uintptr_t)g_xbox_mem_offset);
    uint64_t P = 0, o;
    const char *names[VRA_MAX_BONES];
    float mats[VRA_MAX_BONES][16];
    char store[VRA_MAX_BONES][32];
    uint32_t n, k;
    uint64_t ntab, mtab;
    if (!arms_span(va, 4)) return 0;
    for (o = 0; o + 0x70 <= ARMS_SCAN_BYTES && o <= va; o += 4) {
        const uint64_t c = (va - o) & ~3ull;
        if (!arms_span(c, 0x70)) break;
        if (arms_u32(c + 0x10) == 0xB8000000u && arms_u32(c + 0x14) == 0x95820AD4u && arms_u32(c + 0x18) == 0x23u) { P = c; break; }
    }
    if (!P) return 0;
    n = arms_u32(P + 0x5C);
    if (n == 0 || n > VRA_MAX_BONES) return 0;
    ntab = arms_ptr(P, 0x60, n * 4u);
    mtab = arms_ptr(P, 0x54, n * 64u);
    if (!ntab || !mtab) return 0;
    for (k = 0; k < n; k++) {
        const uint32_t v = arms_u32(ntab + 4u * k);
        uint64_t sp = v < 0x00400000u && arms_span(P + v, 32) ? P + v : (arms_span(v, 32) ? v : 0);
        unsigned i;
        if (!sp) return 0;
        for (i = 0; i < 31; i++) {
            const char ch = *(const char *)((uintptr_t)g_xbox_mem_offset + (uintptr_t)sp + i);
            if (!ch) break;
            if (ch < 32 || ch > 126) return 0;
            store[k][i] = ch;
        }
        store[k][i] = 0;
        names[k] = store[k];
        memcpy(mats[k], (const void *)((uintptr_t)g_xbox_mem_offset + (uintptr_t)(mtab + 64u * k)), 64);
    }
    if (!vra_build_rig(&e->rig, names, (const float (*)[16])mats, (int)n)) return 0;
    e->P = P;
    if (g_arms_log++ < 16)
        GLOG("arms: model at %08llX, %u bones, wrists %d/%d", (unsigned long long)P, n, e->rig.wrist[0], e->rig.wrist[1]);
    return 1;
}

/* The posed joint of bone k (model space) */
static void arms_joint(const VrArmRig *rig, const float *constants, int k, float out[3])
{
    const float *r = vra_rows((float *)constants, k);
    int i;
    for (i = 0; i < 3; i++)
        out[i] = r[i * 4] * rig->joint[k][0] + r[i * 4 + 1] * rig->joint[k][1] + r[i * 4 + 2] * rig->joint[k][2] + r[i * 4 + 3];
}

/* Auto-grip: where the game's right wrist is in the gun hand's aim frame (via K) vs where it should be (wrist_offset);
 * the error is integrated per weapon into the grip correction the game thread adds to the weapon's grip offset. */
static void arms_auto_grip(ArmsEntry *e, const float *constants, VrMat4 k)
{
    int hand_auto, i;
    float wo[3], p[3], h[3], target[3], err[3], gain;
    const float upm = vr_game_config()->units_per_metre;
    vr_arms_grip_config(&hand_auto, wo);
    if (!hand_auto) {
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        vr_set_grip_correction(zero);
        return;
    }
    arms_joint(&e->rig, constants, e->rig.wrist[VRA_RIGHT], p);
    for (i = 0; i < 3; i++) h[i] = VRM_AT(k, i, 0) * p[0] + VRM_AT(k, i, 1) * p[1] + VRM_AT(k, i, 2) * p[2] + VRM_AT(k, i, 3);
    target[0] = wo[0] * upm; target[1] = wo[1] * upm; target[2] = -wo[2] * upm;   /* OpenXR z back -> LH z forward */
    /* fast for the first ~2 s of a weapon, then slow and limited so recoil and reload animations do not drag the gun */
    gain = e->corr_n < 120u ? 0.08f : 0.01f;
    for (i = 0; i < 3; i++) {
        err[i] = h[i] - target[i];
        if (e->corr_n >= 120u) {
            const float lim = 0.05f * upm;
            if (err[i] > lim) err[i] = lim;
            if (err[i] < -lim) err[i] = -lim;
        }
        if (!(err[i] == err[i])) return;
        e->corr[i] += gain * err[i];
        if (e->corr[i] > 0.6f * upm) e->corr[i] = 0.6f * upm;
        if (e->corr[i] < -0.6f * upm) e->corr[i] = -0.6f * upm;
    }
    e->corr_n++;
    vr_set_grip_correction(e->corr);
    if (e->corr_n == 1u || e->corr_n == 120u || e->corr_n % 3600u == 0u)
        GLOG("arms: auto-grip n %u wrist in hand frame %.3f %.3f %.3f m (target %.3f %.3f %.3f) correction %.3f %.3f %.3f m",
             e->corr_n, h[0] / upm, h[1] / upm, h[2] / upm, target[0] / upm, target[1] / upm, target[2] / upm,
             e->corr[0] / upm, e->corr[1] / upm, e->corr[2] / upm);
}

bool kvk_vr_viewmodel_bones(float *constants, const void *vertex_data)
{
    ArmsEntry *e = NULL;
    int cut, mode, engaged, show_off, have_k = 0, place = 0, i;
    static int last_engaged;
    float kf[16], off[16];
    VrMat4 k;
    if (!g_on || !constants || !vertex_data || !&g_xbox_mem_offset) return false;
    vr_arms_state(&cut, &mode, &engaged);
    /* the off hand: tracked -> at the off controller (late latch), snapped onto the gun line while gripping two-handed;
     * grip -> the game's pose, only while gripping; always -> the game's pose; never -> hidden */
    show_off = mode == VR_LEFT_HAND_ALWAYS || (mode == VR_LEFT_HAND_GRIP && engaged);
    if (F.marked) {
        pthread_mutex_lock(&g_ring_lock);
        if (g_ring[F.slot].serial == F.mark.serial && g_ring[F.slot].offhand_ok) {
            memcpy(kf, g_ring[F.slot].offhand, sizeof(kf));
            have_k = 1;
        }
        pthread_mutex_unlock(&g_ring_lock);
    }
    if (have_k) {
        int r, c;
        for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) VRM_AT(k, r, c) = kf[r * 4 + c];
    }
    if (engaged && !last_engaged) {   /* the off hand takes the gun: a short tick on it */
        const VrRenderGun *g = vr_render_gun();
        vr_game_haptic((g->hand_index & 1) ^ 1, 0.8f, 30u);
    }
    last_engaged = engaged;
    for (i = 0; i < ARMS_CACHE; i++) if (g_arms[i].vdata == vertex_data) { e = &g_arms[i]; break; }
    if (e && e->ok && !(arms_span(e->P, 0x70) && arms_u32(e->P + 0x10) == 0xB8000000u &&
                        arms_u32(e->P + 0x14) == 0x95820AD4u && arms_u32(e->P + 0x5C) == (uint32_t)e->rig.n))
        e->ok = 0, e->retry = 0;                        /* the memory now holds something else: look again */
    if (e && !e->ok && e->retry-- == 0) { e->ok = arms_load(e, vertex_data); e->retry = 120; }
    if (!e) {
        e = &g_arms[g_arms_next++ % ARMS_CACHE];
        memset(e, 0, sizeof(*e));
        e->vdata = vertex_data;
        e->ok = arms_load(e, vertex_data);
        e->retry = 120;
    }
    if (!e->ok) return false;
    if (have_k && e->rig.wrist[VRA_RIGHT] >= 0) arms_auto_grip(e, constants, k);
    if (mode == VR_LEFT_HAND_TRACKED && have_k) {
        const VrRenderGun *g = vr_render_gun();
        VrMat4 a;
        if (g->valid && g->off_valid &&
            vgm_off_hand_mirror(k, vgm_off_hand_rel(g->hand, g->off, engaged, vr_game_config()->units_per_metre), &a)) {
            int r, c;
            for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) off[r * 4 + c] = VRM_AT(a, r, c);
            place = 1;
        }
    }
    if (!cut && show_off && !place) return false;      /* the game's arms, unchanged */
    {
        static unsigned logged, changes;
        static int last_logged_engaged = -1;
        int placed = place && vra_place_off_hand(&e->rig, constants, off);
        if (engaged != last_logged_engaged && changes < 16) { changes++; last_logged_engaged = engaged; logged = 0; }
        if (logged < 3) {
            float pl[3], pr[3];
            logged++;
            arms_joint(&e->rig, constants, e->rig.wrist[0], pl);
            arms_joint(&e->rig, constants, e->rig.wrist[1], pr);
            GLOG("arms: off hand mode %d gripping %d hand frame %d placed %d | wrists (model) L %.3f %.3f %.3f  R %.3f %.3f %.3f",
                 mode, engaged, have_k, placed, pl[0], pl[1], pl[2], pr[0], pr[1], pr[2]);
        }
        if (placed) show_off = 1;
    }
    /* the model's right arm holds the gun (the gun hand, whichever controller it is on); its left is the off hand */
    return vra_apply(&e->rig, constants, cut, show_off, 1) > 0 || place;
}

/* The hand delta in stage space, D = hand_late * inverse(hand_game), and whether it is applied: gun_latch on, both
 * poses from this latch, not the identity, and within what a hand does between two latches (a larger step is a
 * tracking glitch or a recentre: the game's own placement is kept). */
#define GUN_MAX_STEP_M   0.30f
#define GUN_MAX_TURN_COS 0.93969262f        /* cos(40 deg / 2): a unit quaternion's |w| for a 40 degree turn */
static int gun_delta(const VrRenderGun *g, const VrRenderPose *rp, VrPose *d)
{
    if (!g->valid || !rp->valid || g->serial != rp->serial) return 0;
    if (!memcmp(&g->hand, &g->game_hand, sizeof(VrPose))) return 0;     /* unchanged: exactly the frame's transforms */
    *d = vrm_pose_mul(g->hand, vrm_pose_inv(g->game_hand));
    if (!(vrm_len(d->position) <= GUN_MAX_STEP_M) || !(fabsf(d->orientation.w) >= GUN_MAX_TURN_COS)) return 0;
    return 1;
}

/* Phase FLIP: the eye transforms for eye'_i = inverse(D) * eye_i, i.e. E'_i = inverse(eye_i) * D * game_head. */
bool kvk_vr_viewmodel_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase)
{
    const VrRenderPose *rp;
    VrFrameState f;
    VrPose d, dinv;
    int i;
    if (!g_on || phase != KVK_VR_PHASE_FLIP || !vr_gun_latch_config()->enabled) return false;
    rp = vr_render_pose();
    if (!gun_delta(vr_render_gun(), rp, &d)) return false;
    dinv = vrm_pose_inv(d);
    memset(&f, 0, sizeof(f));
    f.head = rp->game_head;
    for (i = 0; i < VR_EYE_COUNT; i++) { f.eye[i].pose = vrm_pose_mul(dinv, rp->eye_pose[i]); f.eye[i].fov = rp->eye_fov[i]; }
    f.world_scale = rp->world_scale;
    f.predicted_display_time = rp->display_time;
    f.session_running = 1;
    stereo_for(out, cfg, &f, &rp->head);
    if (!out->valid) return false;
    g_vm_frames++;
    return true;
}

uint32_t kvk_vr_viewmodel_tint(void) { return g_on ? vr_gun_latch_config()->debug_tint : 0; }

/* ---- both-eye capture (kvk_vr_hooks.h; vr_bridge.h vr_capture_*) ---------------------------------------------- */

int kvk_vr_capture_wanted(void) { return g_on && vr_capture_take(); }

static uint32_t png_crc_table[256];
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n)
{
    size_t i;
    if (!png_crc_table[1]) {
        uint32_t k, j, v;
        for (k = 0; k < 256; k++) { v = k; for (j = 0; j < 8; j++) v = (v & 1u) ? 0xEDB88320u ^ (v >> 1) : v >> 1; png_crc_table[k] = v; }
    }
    c = ~c;
    for (i = 0; i < n; i++) c = png_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t h[8], c[4];
    uint32_t crc;
    put32(h, n);
    memcpy(h + 4, type, 4);
    crc = png_crc(0, h + 4, 4);
    if (n) crc = png_crc(crc, data, n);
    put32(c, crc);
    return fwrite(h, 1, 8, f) == 8 && (!n || fwrite(data, 1, n, f) == n) && fwrite(c, 1, 4, f) == 4;
}

/* RGBA8 rows -> an RGB PNG (zlib "stored" blocks: no compression library needed; ~5.7 MB for a Quest 3 eye) */
static int png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    const size_t row = (size_t)w * 3 + 1, raw_n = row * h;
    const size_t nblk = (raw_n + 65534) / 65535, z_n = 2 + raw_n + nblk * 5 + 4;
    uint8_t *raw = (uint8_t *)malloc(raw_n), *z = (uint8_t *)malloc(z_n), ihdr[13];
    uint32_t a = 1, b = 0, x, y;
    size_t i, o = 0, at = 0;
    FILE *f;
    int ok = 0;
    if (!raw || !z) { free(raw); free(z); return 0; }
    for (y = 0; y < h; y++) {
        uint8_t *r = raw + row * y;
        const uint8_t *s = rgba + (size_t)w * 4 * y;
        r[0] = 0;
        for (x = 0; x < w; x++) { r[1 + 3 * x] = s[4 * x]; r[2 + 3 * x] = s[4 * x + 1]; r[3 + 3 * x] = s[4 * x + 2]; }
    }
    for (i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    z[o++] = 0x78; z[o++] = 0x01;
    while (at < raw_n) {
        const uint32_t len = (uint32_t)(raw_n - at > 65535 ? 65535 : raw_n - at);
        z[o++] = at + len >= raw_n ? 1 : 0;
        z[o++] = (uint8_t)len; z[o++] = (uint8_t)(len >> 8);
        z[o++] = (uint8_t)~len; z[o++] = (uint8_t)(~len >> 8);
        memcpy(z + o, raw + at, len);
        o += len; at += len;
    }
    put32(z + o, (b << 16) | a); o += 4;
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;      /* 8-bit RGB */
    if ((f = fopen(path, "wb")) != NULL) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        ok = fwrite(sig, 1, 8, f) == 8 && png_chunk(f, "IHDR", ihdr, 13) && png_chunk(f, "IDAT", z, (uint32_t)o) &&
             png_chunk(f, "IEND", NULL, 0);
        if (fclose(f)) ok = 0;
    }
    free(raw); free(z);
    return ok;
}

typedef struct CapJob { uint8_t *px[2]; uint32_t w, h; uint64_t frame; } CapJob;
static void *capture_writer(void *arg)
{
    CapJob *j = (CapJob *)arg;
    const char *dir = vr_capture_dir();
    char path[2][700];
    time_t t = time(NULL);
    struct tm tm;
    int e, ok = 1;
    localtime_r(&t, &tm);
    mkdir(dir, 0775);
    for (e = 0; e < 2; e++) {
        snprintf(path[e], sizeof(path[e]), "%s/eye_%04d%02d%02d_%02d%02d%02d_f%llu_%c.png", dir, tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned long long)j->frame, e ? 'R' : 'L');
        if (!png_write(path[e], j->px[e], j->w, j->h)) ok = 0;
        free(j->px[e]);
    }
    if (ok) GLOG("CAPTURE saved %s %s (%ux%u, renderer frame %llu)", path[0], path[1], j->w, j->h, (unsigned long long)j->frame);
    else GLOG("CAPTURE failed writing %s (%s)", path[0], dir);
    __atomic_add_fetch(&g_captures, 1, __ATOMIC_RELAXED);
    free(j);
    return NULL;
}

/* GPU thread: the pictures are written on a thread of their own (a few MB of file I/O off the render path). */
void kvk_vr_capture_done(uint8_t *rgba[2], uint32_t w, uint32_t h, uint64_t frame)
{
    CapJob *j = (CapJob *)malloc(sizeof(*j));
    pthread_t th;
    if (!j) { free(rgba[0]); free(rgba[1]); return; }
    j->px[0] = rgba[0]; j->px[1] = rgba[1]; j->w = w; j->h = h; j->frame = frame;
    if (pthread_create(&th, NULL, capture_writer, j) == 0) pthread_detach(th);
    else capture_writer(j);
}

uint32_t vr_glue_captures_written(void) { return __atomic_load_n(&g_captures, __ATOMIC_RELAXED); }

/* tests / diagnostics */
void vr_glue_gun_counters(uint32_t *recorded, uint32_t *dropped, uint32_t *moved)
{
    *recorded = g_vm_recorded; *dropped = g_vm_dropped; *moved = g_vm_frames;
}

void vr_glue_counters(uint32_t *made, uint32_t *failed, uint32_t *seen)
{
    *made = g_marks_made; *failed = g_marks_failed; *seen = g_marks_seen;
}
