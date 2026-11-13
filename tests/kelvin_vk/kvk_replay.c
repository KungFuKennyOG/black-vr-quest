/*
 * kvk-replay: draws a recorded backend call trace (kvk_trace.h, RECOMP_KVK_TRACE in game_render_x86) on a fresh Kelvin
 * Vulkan backend, deterministically: the VR glue's answers are replayed from the trace, shaders and pipelines are
 * created synchronously (RECOMP_KVK_SYNC_COMPILE=1, RECOMP_KGPU_PIPE_ASYNC=0), and after every flip the pictures are
 * read back: both eye layers, the flat and HUD layer images, and both layers of the flipped main colour surface.
 *
 *   kvk-replay <trace.ktr> <outdir>                 write <outdir>/f<NNN>_<image>.raw (the reference run)
 *   kvk-replay <trace.ktr> <outdir> --compare <ref> compare each picture with <ref>'s, report the largest channel
 *                                                   difference and the differing pixels per picture; exit 1 on any
 *   kvk-replay <trace.ktr> --info                   what the trace holds
 *   --ppm                                           also <outdir>/f<NNN>_<image>.ppm (RGB) of the first flip
 *
 * The configuration under test comes from the environment (RECOMP_KVK_PASS_MERGE=0/1, RECOMP_KVK_POISON=1, ...), so
 * run it twice: once to write a reference, once with --compare (port/notes/renderer.md section 13).
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <vulkan/vulkan.h>

#include "kelvin_gpu.h"
#include "kelvin_gfx.h"
#include "kvk_device.h"
#include "kvk_stereo.h"
#include "kvk_vr_hooks.h"
#include "kvk_trace.h"
#include "vr_bridge.h"
#include "nv2a_vsh_cpu/nv2a_vsh_disassembler.h"

void kelvin_unsupported(int what, const char *detail, uint32_t value) { (void)what; (void)detail; (void)value; }

/* ---- the trace ---- */
static uint8_t *s_buf;
static size_t s_len;
static const KvkTrHeader *s_hdr;
static const uint8_t **s_blob;
static uint64_t *s_blob_len;
static uint32_t s_nblob;

typedef struct Prog { uint32_t hash, start, nwords; const uint32_t *words; Nv2aVshProgram prog; bool ok; } Prog;
static Prog s_prog[2048];
static unsigned s_nprog;

static const uint8_t *blob(uint32_t id, uint64_t *len)
{
    if (!id || id > s_nblob || !s_blob[id]) { if (len) *len = 0; return NULL; }
    if (len) *len = s_blob_len[id];
    return s_blob[id];
}

/* ---- the glue's answers ---- */
static KvkTrCtx g_ctx = { -1, 1 };
static bool g_vm;
static bool g_have_st;
static KvkStereoFrame g_st;
static KvkTrFlip g_flip;
static bool g_have_cfg;
static VrGameConfig g_cfg;
static int g_rel_eye, g_rel_flat, g_rel_hud;

int vr_active(void) { return (int)g_ctx.vr_active; }
const VrFrameState *vr_frame_state(void) { return NULL; }
const VrGameCamera *vr_game_camera(void) { return NULL; }
const VrGameConfig *vr_game_config(void) { return g_have_cfg ? &g_cfg : NULL; }

bool kvk_vr_stereo_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase)
{
    (void)cfg;
    if (phase == KVK_VR_PHASE_FIRST_DRAW) { if (!g_have_st) return false; *out = g_st; return true; }
    if (!g_flip.relatch_ok) return false;
    *out = g_flip.relatch;
    return true;
}
int kvk_vr_frame_mode(void) { return g_ctx.frame_mode; }
void kvk_vr_view_mark(int mode, int slot) { (void)mode; (void)slot; }
void kvk_vr_frame_done(void) {}
bool kvk_vr_viewmodel_draw(const float *constants, const uint32_t pos_consts[6]) { (void)constants; (void)pos_consts; return g_vm; }
bool kvk_vr_viewmodel_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase)
{
    (void)cfg;
    if (phase != KVK_VR_PHASE_FLIP || !g_flip.relatch_vm_ok) return false;
    *out = g_flip.relatch_vm;
    return true;
}
uint32_t kvk_vr_viewmodel_tint(void) { return 0; }
int kvk_vr_capture_wanted(void) { return 0; }
void kvk_vr_capture_done(uint8_t *rgba[2], uint32_t w, uint32_t h, uint64_t frame) { (void)w; (void)h; (void)frame; free(rgba[0]); free(rgba[1]); }

/* ---- Vulkan ---- */
static VkInstance s_inst;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static uint32_t s_qf;
static VkCommandPool s_pool;

typedef struct Img { VkImage img; VkDeviceMemory mem; uint32_t w, h, layers; } Img;
static Img s_eye, s_flat, s_hud;

static bool make_device(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceMultiviewFeatures mv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceFeatures en;
    VkQueueFamilyProperties qp[16];
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    KvkDeviceInfo info;
    uint32_t n = 1, i;
    float prio = 1.0f;
    ai.apiVersion = VK_API_VERSION_1_1;
    ci.pApplicationInfo = &ai;
    if (vkCreateInstance(&ci, NULL, &s_inst) != VK_SUCCESS) return false;
    if (vkEnumeratePhysicalDevices(s_inst, &n, &s_phys) < 0 || !n) return false;
    n = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &n, qp);
    for (i = 0; i < n; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (i == n) return false;
    s_qf = i;
    f2.pNext = &mv;
    vkGetPhysicalDeviceFeatures2(s_phys, &f2);
    if (!mv.multiview) return false;
    memset(&en, 0, sizeof(en));
    en.robustBufferAccess = f2.features.robustBufferAccess;
    en.depthClamp = f2.features.depthClamp;
    en.samplerAnisotropy = f2.features.samplerAnisotropy;
    en.textureCompressionBC = f2.features.textureCompressionBC;     /* as game_render_x86 (BC 1 on SwiftShader) */
    memset(&mv, 0, sizeof(mv));
    mv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    mv.multiview = VK_TRUE;
    qi.queueFamilyIndex = s_qf; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    di.pNext = &mv;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.pEnabledFeatures = &en;
    if (vkCreateDevice(s_phys, &di, NULL, &s_dev) != VK_SUCCESS) return false;
    vkGetDeviceQueue(s_dev, s_qf, 0, &s_queue);
    pi.queueFamilyIndex = s_qf;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(s_dev, &pi, NULL, &s_pool);
    memset(&info, 0, sizeof(info));
    info.instance = s_inst; info.physical = s_phys; info.device = s_dev;
    info.queue_family = s_qf; info.queue = s_queue; info.api_version = VK_API_VERSION_1_1;
    info.multiview = true;
    info.robust_buffer_access = en.robustBufferAccess != 0;
    info.depth_clamp = en.depthClamp != 0;
    info.sampler_anisotropy = en.samplerAnisotropy != 0;
    info.texture_compression_bc = en.textureCompressionBC != 0;
    kvk_set_device(&info);
    return true;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(s_phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

static VkCommandBuffer one_shot_begin(void)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkCommandBuffer cb;
    ai.commandPool = s_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(s_dev, &ai, &cb);
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void one_shot_end(VkCommandBuffer cb)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    vkEndCommandBuffer(cb);
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(s_queue);
    vkFreeCommandBuffers(s_dev, s_pool, 1, &cb);
}

static void layout(VkCommandBuffer cb, VkImage img, uint32_t layers, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = layers;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* a target image of the shell's, in COLOR_ATTACHMENT_OPTIMAL, filled with a marker colour (a picture the backend
 * does not draw shows as such) */
static void make_target(Img *g, uint32_t w, uint32_t h, uint32_t layers)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkClearColorValue c = { { 1.0f, 0.0f, 1.0f, 1.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 0 };
    VkCommandBuffer cb;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCreateImage(s_dev, &ci, NULL, &g->img);
    vkGetImageMemoryRequirements(s_dev, g->img, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(s_dev, &ai, NULL, &g->mem);
    vkBindImageMemory(s_dev, g->img, g->mem, 0);
    g->w = w; g->h = h; g->layers = layers;
    r.layerCount = layers;
    cb = one_shot_begin();
    layout(cb, g->img, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cb, g->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
    layout(cb, g->img, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    one_shot_end(cb);
}

static void read_target(const Img *g, uint32_t layer, uint8_t *out)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkBufferImageCopy r;
    VkCommandBuffer cb;
    void *p;
    bi.size = (VkDeviceSize)g->w * g->h * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(s_dev, &bi, NULL, &buf);
    vkGetBufferMemoryRequirements(s_dev, buf, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(s_dev, &ai, NULL, &mem);
    vkBindBufferMemory(s_dev, buf, mem, 0);
    cb = one_shot_begin();
    layout(cb, g->img, g->layers, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.baseArrayLayer = layer; r.imageSubresource.layerCount = 1;
    r.imageExtent.width = g->w; r.imageExtent.height = g->h; r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, g->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    layout(cb, g->img, g->layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    one_shot_end(cb);
    vkMapMemory(s_dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
    memcpy(out, p, (size_t)g->w * g->h * 4);
    vkUnmapMemory(s_dev, mem);
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
}

static void target_of(const Img *g, uint32_t layer, VrEyeTarget *t)
{
    memset(t, 0, sizeof(*t));
    t->image = (uint64_t)(uintptr_t)g->img;
    t->width = g->w; t->height = g->h;
    t->format = VK_FORMAT_R8G8B8A8_UNORM;
    t->array_layer = layer;
}

int vr_acquire_eye_targets(VrEyeTarget out[VR_EYE_COUNT])
{
    if (!g_flip.eye_targets) return 0;
    target_of(&s_eye, 0, &out[0]);
    target_of(&s_eye, s_eye.layers > 1 ? 1 : 0, &out[1]);
    return 1;
}
void vr_release_eye_targets(void) { g_rel_eye++; }
int vr_acquire_flat_target(VrEyeTarget *out) { if (!g_flip.flat_target) return 0; target_of(&s_flat, 0, out); return 1; }
void vr_release_flat_target(void) { g_rel_flat++; }
int vr_acquire_hud_target(VrEyeTarget *out) { if (!g_flip.hud_target) return 0; target_of(&s_hud, 0, out); return 1; }
void vr_release_hud_target(void) { g_rel_hud++; }

/* ---- pictures ---- */
static const char *s_out, *s_ref;
static bool s_ppm;
static int s_bad_pictures;
static int s_frame;
static int s_worst_all;

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "kvk-replay: %s: %s\n", path, strerror(errno)); exit(2); }
    fwrite(p, 1, n, f);
    fclose(f);
}

static void write_ppm(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, bool bgra)
{
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        const uint8_t *p = rgba + 4 * (size_t)i;
        uint8_t rgb[3] = { bgra ? p[2] : p[0], p[1], bgra ? p[0] : p[2] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* this picture: written (reference run) or compared with the reference's */
static void picture(const char *what, const uint8_t *px, uint32_t w, uint32_t h, bool bgra)
{
    char path[1024];
    const size_t n = (size_t)w * h * 4;
    snprintf(path, sizeof(path), "%s/f%03d_%s.raw", s_out, s_frame, what);
    if (s_ppm && s_frame == 0) {
        char pp[1024];
        snprintf(pp, sizeof(pp), "%s/f%03d_%s.ppm", s_out, s_frame, what);
        write_ppm(pp, px, w, h, bgra);
    }
    if (!s_ref) { write_file(path, px, n); return; }
    {
        char rp[1024];
        FILE *f;
        uint8_t *ref = (uint8_t *)malloc(n);
        size_t i, got;
        int worst = 0;
        unsigned long long npx = 0, first = ~0ull;
        snprintf(rp, sizeof(rp), "%s/f%03d_%s.raw", s_ref, s_frame, what);
        f = fopen(rp, "rb");
        got = f ? fread(ref, 1, n, f) : 0;
        if (f) fclose(f);
        if (got != n) {
            printf("  frame %d %-8s %ux%u: no reference picture of that size (%s)\n", s_frame, what, w, h, rp);
            s_bad_pictures++;
            free(ref);
            return;
        }
        for (i = 0; i < n; i += 4) {
            int k, d = 0;
            for (k = 0; k < 4; k++) { const int e = abs((int)px[i + k] - (int)ref[i + k]); if (e > d) d = e; }
            if (d) { npx++; if (first == ~0ull) first = i / 4; }
            if (d > worst) worst = d;
        }
        if (worst > s_worst_all) s_worst_all = worst;
        if (worst) {
            char dp[1024];
            s_bad_pictures++;
            printf("  frame %d %-8s %ux%u: max channel difference %d, %llu pixels differ (first at %llu,%llu)\n", s_frame,
                   what, w, h, worst, npx, first % w, first / w);
            snprintf(dp, sizeof(dp), "%s/f%03d_%s_diff.ppm", s_out, s_frame, what);
            for (i = 0; i < n; i += 4) {
                const int d = abs((int)px[i] - (int)ref[i]) + abs((int)px[i + 1] - (int)ref[i + 1]) + abs((int)px[i + 2] - (int)ref[i + 2]);
                ref[i] = ref[i + 1] = ref[i + 2] = (uint8_t)(d ? (d * 8 > 255 ? 255 : d * 8 + 40) : 0);
            }
            write_ppm(dp, ref, w, h, false);
        } else {
            printf("  frame %d %-8s %ux%u: identical\n", s_frame, what, w, h);
        }
        free(ref);
    }
}

static void pictures_of_flip(uint32_t main_va)
{
    static uint8_t *px;
    static size_t cap;
    uint32_t l;
    const size_t need = (size_t)4096 * 4096 * 4;
    char name[32];
    if (!px) { cap = need; px = (uint8_t *)malloc(cap); }
    if (g_rel_eye) {
        for (l = 0; l < s_eye.layers; l++) {
            read_target(&s_eye, l, px);
            snprintf(name, sizeof(name), "eye%u", l);
            picture(name, px, s_eye.w, s_eye.h, false);
        }
    }
    if (g_rel_flat) { read_target(&s_flat, 0, px); picture("flat", px, s_flat.w, s_flat.h, false); }
    if (g_rel_hud) { read_target(&s_hud, 0, px); picture("hud", px, s_hud.w, s_hud.h, false); }
    for (l = 0; l < s_hdr->layers; l++) {
        unsigned w = 0, h = 0;
        if (!kvk_read_surface(main_va, l, px, (size_t)4096 * 4, &w, &h, cap)) break;
        {
            /* tightly packed for the file */
            uint32_t y;
            for (y = 1; y < h; y++) memmove(px + (size_t)y * w * 4, px + (size_t)y * 4096 * 4, (size_t)w * 4);
        }
        snprintf(name, sizeof(name), "main%u", l);
        picture(name, px, w, h, true);
    }
}

/* ---- replay ---- */
static uint8_t s_dummy_surface[64];
static unsigned long long s_skipped_draws;

static const Prog *prog_find(uint32_t hash, uint32_t start)
{
    unsigned i;
    for (i = 0; i < s_nprog; i++) if (s_prog[i].hash == hash && s_prog[i].start == start) return &s_prog[i];
    return NULL;
}

static void fix_target(KelvinTarget *t, uint32_t has_color, uint32_t has_zeta)
{
    t->color = has_color ? s_dummy_surface : NULL;
    t->zeta = has_zeta ? s_dummy_surface : NULL;
}

static void replay_draw(const KvkTrDraw *r)
{
    KGpuDraw d = r->d;
    KelvinTarget t = r->t;
    KPixelState ps = r->ps;
    KGpuVsDraw gv = r->gv;
    int i;
    g_ctx = r->ctx;
    g_vm = r->vm != 0;
    fix_target(&t, r->has_color, r->has_zeta);
    for (i = 0; i < 4; i++) {
        ps.tex[i].mem = blob(r->tex_blob[i], NULL);
        ps.tex[i].palette_mem = blob(r->pal_blob[i], NULL);
        ps.tex[i].host = NULL;
    }
    d.t = &t;
    d.ps = &ps;
    d.indices = (const uint32_t *)blob(r->idx_blob, NULL);
    d.recs = (const float *)blob(r->recs_blob, NULL);
    d.gvs = NULL;
    if (r->has_gvs) {
        const Prog *p = prog_find(gv.hash, gv.start);
        int tries;
        if (!p || !p->ok) { s_skipped_draws++; return; }
        gv.program = &p->prog;
        gv.constants = (const float *)blob(r->const_blob, NULL);
        for (i = 0; i < 16; i++) {
            const uint8_t *b = blob(r->attr_blob[i], NULL);
            gv.attr[i].data = b ? b - gv.attr[i].lo : NULL;
        }
        d.gvs = &gv;
        for (tries = 0; tries < 3 && !kgpu_vs_ready(gv.program, gv.hash, gv.start, p->words, p->nwords); tries++)
            kvk_wait_compiles();
        if (tries == 3) { s_skipped_draws++; return; }
    }
    kgpu_draw(&d);
}

static int info_only(void)
{
    size_t at = sizeof(KvkTrHeader);
    unsigned long long n[16] = { 0 }, bytes[16] = { 0 };
    static const char *const names[16] = { "", "blob", "vsprog", "draw", "clear", "flip", "kick", "view", "viewmodel",
                                           "stereo", "gamecfg" };
    int i;
    printf("trace: host %ux%u, %u layers, HUD %ux%u, first frame %llu, %u flips\n", s_hdr->host_w, s_hdr->host_h,
           s_hdr->layers, s_hdr->hud_w, s_hdr->hud_h, (unsigned long long)s_hdr->first_frame, s_hdr->frames);
    while (at + 8 <= s_len) {
        uint32_t tag, sz;
        memcpy(&tag, s_buf + at, 4); memcpy(&sz, s_buf + at + 4, 4);
        if (tag < 16) { n[tag]++; bytes[tag] += sz; }
        at += 8 + sz;
    }
    for (i = 1; i < 11; i++) printf("  %-10s %8llu records, %10.1f KB\n", names[i], n[i], bytes[i] / 1024.0);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    size_t at;
    int i, frames = 0;
    const char *trace;
    bool info = false;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <trace.ktr> <outdir> [--compare <refdir>] [--ppm] | <trace.ktr> --info\n", argv[0]);
        return 2;
    }
    trace = argv[1];
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--compare") && i + 1 < argc) s_ref = argv[++i];
        else if (!strcmp(argv[i], "--ppm")) s_ppm = true;
        else if (!strcmp(argv[i], "--info")) info = true;
        else s_out = argv[i];
    }
    f = fopen(trace, "rb");
    if (!f) { fprintf(stderr, "kvk-replay: %s: %s\n", trace, strerror(errno)); return 2; }
    fseek(f, 0, SEEK_END);
    s_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_buf = (uint8_t *)malloc(s_len);
    if (!s_buf || fread(s_buf, 1, s_len, f) != s_len) { fprintf(stderr, "kvk-replay: read failed\n"); return 2; }
    fclose(f);
    s_hdr = (const KvkTrHeader *)s_buf;
    if (s_len < sizeof(*s_hdr) || s_hdr->magic != KVK_TR_MAGIC || s_hdr->version != KVK_TR_VERSION ||
        s_hdr->sz_draw != sizeof(KGpuDraw) || s_hdr->sz_ps != sizeof(KPixelState) || s_hdr->sz_target != sizeof(KelvinTarget) ||
        s_hdr->sz_vsdraw != sizeof(KGpuVsDraw) || s_hdr->sz_stereo != sizeof(KvkStereoFrame) ||
        s_hdr->sz_gamecfg != sizeof(VrGameConfig)) {
        fprintf(stderr, "kvk-replay: %s is not a trace of this build's structures\n", trace);
        return 2;
    }
    if (info) return info_only();
    if (!s_out) { fprintf(stderr, "kvk-replay: no output directory\n"); return 2; }
    mkdir(s_out, 0775);
    /* index the blobs and parse the programs */
    for (at = sizeof(KvkTrHeader); at + 8 <= s_len;) {
        uint32_t tag, sz;
        memcpy(&tag, s_buf + at, 4); memcpy(&sz, s_buf + at + 4, 4);
        if (tag == KVK_TR_BLOB) {
            uint32_t id;
            uint64_t bytes;
            memcpy(&id, s_buf + at + 8, 4); memcpy(&bytes, s_buf + at + 16, 8);
            if (id >= s_nblob) {
                const uint32_t nn = id + 1024;
                s_blob = (const uint8_t **)realloc((void *)s_blob, nn * sizeof(*s_blob));
                s_blob_len = (uint64_t *)realloc(s_blob_len, nn * sizeof(*s_blob_len));
                memset((void *)(s_blob + s_nblob), 0, (nn - s_nblob) * sizeof(*s_blob));
                s_nblob = nn;
            }
            s_blob[id] = s_buf + at + 24;
            s_blob_len[id] = bytes;
        } else if (tag == KVK_TR_VSPROG && s_nprog < 2048) {
            Prog *p = &s_prog[s_nprog++];
            const uint32_t *w = (const uint32_t *)(s_buf + at + 8);
            p->hash = w[0]; p->start = w[1]; p->nwords = w[2]; p->words = w + 3;
            p->ok = nv2a_vsh_parse_program(&p->prog, p->words, p->nwords / 4) == NV2AVPR_SUCCESS;
        }
        at += 8 + sz;
    }
    if (!make_device()) { fprintf(stderr, "kvk-replay: no Vulkan device with multiview\n"); return 77; }
    kvk_set_raster_size(s_hdr->host_w, s_hdr->host_h);
    kvk_set_stereo(s_hdr->layers > 1);
    if (!kgpu_init()) { fprintf(stderr, "kvk-replay: the backend did not start\n"); return 2; }
    make_target(&s_eye, s_hdr->host_w, s_hdr->host_h, s_hdr->layers > 1 ? 2 : 1);
    make_target(&s_flat, 1280, 960, 1);
    make_target(&s_hud, s_hdr->hud_w, s_hdr->hud_h, 1);
    printf("kvk-replay: %s, %ux%u x%u, %u flips%s%s\n", trace, s_hdr->host_w, s_hdr->host_h, s_hdr->layers, s_hdr->frames,
           s_ref ? ", comparing with " : "", s_ref ? s_ref : "");
    for (at = sizeof(KvkTrHeader); at + 8 <= s_len;) {
        uint32_t tag, sz;
        const uint8_t *p;
        memcpy(&tag, s_buf + at, 4); memcpy(&sz, s_buf + at + 4, 4);
        p = s_buf + at + 8;
        switch (tag) {
        case KVK_TR_GAMECFG: { uint32_t have; memcpy(&have, p, 4); g_have_cfg = have != 0; memcpy(&g_cfg, p + 4, sizeof(g_cfg)); break; }
        case KVK_TR_STEREO: memcpy(&g_st, p, sizeof(g_st)); g_have_st = true; break;
        case KVK_TR_KICK: kgpu_note_kick(); break;
        case KVK_TR_VIEW: { int32_t v[6]; memcpy(v, p, sizeof(v)); kgpu_set_view(v[0], v[1], v[2], v[3], v[4], v[5]); break; }
        case KVK_TR_VIEWMODEL: {
            struct { int32_t count; float scale; uint32_t h[8]; } r;
            memcpy(&r, p, sizeof(r));
            kgpu_set_viewmodel(r.h, r.count, r.scale);
            break;
        }
        case KVK_TR_DRAW: replay_draw((const KvkTrDraw *)p); break;
        case KVK_TR_CLEAR: {
            KvkTrClear c;
            memcpy(&c, p, sizeof(c));
            g_ctx = c.ctx;
            fix_target(&c.t, c.has_color, c.has_zeta);
            kgpu_clear(&c.t, c.color != 0, c.depth != 0, c.stencil != 0, c.color_value, c.zs_value, c.channel_mask, c.x0, c.y0,
                       c.x1, c.y1, c.color_format);
            break;
        }
        case KVK_TR_FLIP: {
            memcpy(&g_flip, p, sizeof(g_flip));
            g_ctx = g_flip.ctx;
            fix_target(&g_flip.t, g_flip.has_color, g_flip.has_zeta);
            g_rel_eye = g_rel_flat = g_rel_hud = 0;
            kgpu_flip(&g_flip.t, g_flip.color_format, g_flip.aspect_w, g_flip.aspect_h);
            if (s_ref) printf(" flip %d (mode %d):\n", s_frame, g_flip.ctx.frame_mode);
            pictures_of_flip(g_flip.t.color_va);
            s_frame++;
            frames++;
            break;
        }
        default: break;
        }
        at += 8 + sz;
    }
    {
        KvkStats st;
        kvk_get_stats(&st);
        printf("kvk-replay: %d flips, %llu draws (%llu not replayable), %llu passes, %llu skipped by the backend\n", frames,
               (unsigned long long)st.draws, s_skipped_draws, (unsigned long long)st.passes, (unsigned long long)st.skipped);
    }
    kgpu_report();
    if (s_ref) {
        printf("kvk-replay: %s: %d pictures differ, largest channel difference %d\n", s_bad_pictures ? "DIFFERENT" : "IDENTICAL",
               s_bad_pictures, s_worst_all);
        return s_bad_pictures ? 1 : 0;
    }
    return 0;
}
