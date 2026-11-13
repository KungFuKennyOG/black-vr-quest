/*
 * kvk_backend.c - the Kelvin GPU backend on Vulkan (Android / Quest; Linux for tests). Implements kelvin_gpu.h.
 *
 * The D3D11 backend (kelvin_gpu.c and its .inc files) is the model: the same surfaces keyed by guest address, the
 * same texture cache with per-kick revalidation, the same per-draw combiner constants and shader specialisation, the
 * same depth <-> colour reinterpretations BLACK needs. What is different:
 *
 *  Device       given by the XR shell (kvk_device.h); one graphics queue; Vulkan 1.1 + multiview.
 *  Submission   synchronous like D3D11 with RECOMP_KGPU_ASYNC=0: the Kelvin GPU thread records one command buffer per
 *               frame (plus one for texture uploads, submitted ahead of it) and submits at the flip; KVK_FRAMES frames
 *               in flight, each with its own ring of host-visible memory for constants, vertex data, indices and
 *               upload staging, and a fence. Objects released mid-frame are destroyed when that frame retires.
 *  Passes       render passes are opened lazily at the first draw on a (colour, depth) pair and kept until a draw
 *               needs another pair, a texture read of an attachment, or the flip. A full clear while no pass is open
 *               becomes the next pass's loadOp CLEAR (no load/clear/store round trip on a tiled GPU).
 *  Pipelines    keyed on (vertex module, fragment module, compatible render pass, packed blend/depth/stencil/raster
 *               state); viewport, scissor, blend constants, stencil masks/reference and depth bias are dynamic.
 *               Created on first use on the GPU thread through a VkPipelineCache persisted on disk.
 *  Shaders      kvk_glsl.c generates GLSL for each NV2A vertex program and combiner configuration; worker threads
 *               compile it with glslang (kvk_spirv.c) into SPIR-V cached on disk. Until a vertex program is ready,
 *               kgpu_vs_ready() says no and the engine shades that draw's vertices on the CPU; until a combiner's
 *               specialised shader is ready the draw uses the combiner interpreter (ubershader), as on D3D11.
 *  Stereo (VR)  with kvk_set_stereo(true) every render target is a 2-layer image array and every pass is multiview
 *               (view mask 0b11): the frame is drawn once per eye by the GPU. World draws on the main target get the
 *               eye's clip-space correction in their vertex stage (kvk_stereo.h). After the world, HUD/menu draws on
 *               the main target are redirected to a mono HUD surface; at the flip the eye images are composed from
 *               the main target's layers plus the HUD on a virtual screen (or, for a frame without world, the whole
 *               picture on the virtual screen).
 *
 * Not carried over (PC extras, kelvin_gfx.h): SSR, AO/DOF/FXAA/colour passes, DLSS, ray tracing, texture packs,
 * profilers, split-screen views, shared-texture presentation, the asynchronous submission thread.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <vulkan/vulkan.h>

#include "../kelvin_gpu.h"
#include "../kelvin_gfx.h"
#include "../psh_regs.h"
#include "kvk_cfg.h"

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#endif
#include "../gfx/kgfx_cfg_fill.inc"
#include "../gfx/kgfx_hash.inc"
#include "../gfx/kgfx_texdecode.inc"

#include "../nv2a_vsh_cpu/nv2a_vsh_disassembler.h"
#include "../nv2a_vsh_cpu/nv2a_vsh_emulator.h"     /* VR: where a screen-space draw lands (np_pos) */
#include "kvk_device.h"
#include "kvk_glsl.h"
#include "kvk_spirv.h"
#include "kvk_stereo.h"
#include "kvk_vkmap.h"
#include "kvk_vr_hooks.h"      /* VR host glue hooks; defaults and the quad layers in kvk_vr_layers.inc */
#include "vr_bridge.h"

#define KVK_FRAMES 2
#define KVK_CHUNK_SIZE ((VkDeviceSize)32 << 20)
#define KVK_VAB_RANGE ((VkDeviceSize)4 << 20)     /* the vertex-data window one draw may read */
#define KVK_CHUNKS 16
#define KVK_MAX_SURF 64
#define KVK_TEX_SLOTS 4096
#define KVK_VS_MAX 1024
#define KVK_PS_MAX 2048
#define KVK_PIPE_MAX 4096
#define KVK_FB_MAX 128
#define KVK_RP_MAX 64
#define KVK_SMP_MAX 256
/* texture descriptor sets, kept across frames (port/notes/perf.md section 11): 4-way buckets, one pool that can free */
#define KVK_TSET_WAYS 4
#define KVK_TSET_BUCKETS 1024
#define KVK_TSET_CACHE (KVK_TSET_BUCKETS * KVK_TSET_WAYS)
#define KVK_TSET_POOL_SETS (KVK_TSET_CACHE + 4096)     /* the cache, plus evicted sets waiting for their frame */
#define KVK_EYEVIEW_MAX 16
#define KVK_STEREO_BYTES 272u

#if defined(_MSC_VER)
#define KVK_RESTRICT __restrict
#else
#define KVK_RESTRICT __restrict__
#endif
#define LOG(...) do { fprintf(stderr, "[KVK] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VKOK(expr) ((expr) == VK_SUCCESS)

/* ---- host configuration ------------------------------------------------------------------------------------- */

static KvkDeviceInfo s_dev;
static bool s_have_dev;
static unsigned int s_raster_w = 1280, s_raster_h = 960;
static int s_stereo_req = -1;
static char s_cache_dir[512];

void kvk_set_device(const KvkDeviceInfo *info)
{
    if (info) { s_dev = *info; s_have_dev = info->device != VK_NULL_HANDLE; }
    else { memset(&s_dev, 0, sizeof(s_dev)); s_have_dev = false; }
}

void kvk_set_raster_size(unsigned int w, unsigned int h)
{
    if (w >= 64 && h >= 64 && w <= 8192 && h <= 8192) { s_raster_w = w; s_raster_h = h; }
}

void kvk_set_stereo(bool on) { s_stereo_req = on ? 1 : 0; }

void kvk_set_cache_dir(const char *dir)
{
    snprintf(s_cache_dir, sizeof(s_cache_dir), "%s", dir ? dir : "");
}

static const char *cache_dir(void)
{
    const char *e;
    if (s_cache_dir[0]) return s_cache_dir;
    e = getenv("RECOMP_KGPU_CACHE");
    return e && *e ? e : "kgpu-cache";
}

/* ---- the XR shell's side of vr_bridge.h, when no shell is linked (flat use, tests) ---------------------------- */

__attribute__((weak)) const VrFrameState *vr_frame_state(void) { return NULL; }
__attribute__((weak)) const VrGameCamera *vr_game_camera(void) { return NULL; }
__attribute__((weak)) int vr_acquire_eye_targets(VrEyeTarget out[VR_EYE_COUNT]) { (void)out; return 0; }
__attribute__((weak)) void vr_release_eye_targets(void) {}
__attribute__((weak)) int vr_active(void) { return 0; }
__attribute__((weak)) const VrGameConfig *vr_game_config(void) { return NULL; }

/* ---- objects ------------------------------------------------------------------------------------------------ */

typedef struct KvkBuf { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size; bool coherent; } KvkBuf;

/* buf_create's memory: device-local, CPU-written (uploads: uncached write-combined preferred), CPU-read (readback:
 * cached preferred, so the CPU's reads of it are not uncached on a tiler; coherent if there is such a type) */
enum { BUF_DEVICE = 0, BUF_UPLOAD = 1, BUF_READBACK = 2 };

typedef struct KvkImage {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;       /* sampled: 2D array of every layer (depth: the depth aspect) */
    VkImageView att;        /* attachment: every layer, every aspect (== view for colour) */
    VkFormat fmt;
    uint32_t w, h, layers, levels;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
} KvkImage;

enum { DEAD_IMAGE = 1, DEAD_BUF, DEAD_FB, DEAD_SAMPLER, DEAD_TSET };
typedef struct KvkDead { int kind; KvkImage im; KvkBuf b; uint64_t h; } KvkDead;

typedef struct KvkChunk {
    KvkBuf b; VkDescriptorSet set0; VkDeviceSize pos; bool stereo; VkDeviceSize stereo_off;
    bool stereo_vm; VkDeviceSize stereo_vm_off;     /* VR gun latch: the weapon's stereo block (kvk_vr_hooks.h) */
} KvkChunk;

typedef struct KvkFrame {
    VkCommandPool pool;
    VkCommandBuffer cb, ucb;
    VkFence fence;
    bool pending, ucb_open;
    VkDescriptorPool tpool[8];
    int ntpool, curtpool;
    KvkChunk chunk[KVK_CHUNKS];
    int nchunk, cur;
    KvkDead *dead;
    int ndead, capdead;
} KvkFrame;

/* pending clears of a surface not in an open pass: applied as the next pass's loadOp */
enum { PC_COLOR = 1, PC_DEPTH = 2, PC_STENCIL = 4 };

typedef struct KvkSurf {
    bool used, depth, swz, hud;
    uint32_t va, pitch, format;
    unsigned int gw, gh, hw, hh, bpp;
    KvkImage im;
    uint64_t clock, ver, wseq;
    KvkImage conv;              /* depth read as packed Z24S8 colour */
    uint64_t conv_ver;
    bool from_color, real;
    uint64_t src_ver;
    int pending;
    VkClearValue pending_val;
} KvkSurf;

typedef struct KvkTex {
    bool used;
    uint32_t base, fmt, w, h, pitch, levels, palette, palette_len, span;
    uint64_t sig[2], epoch, clock;
    size_t bytes;
    KvkImage im;
} KvkTex;

typedef struct KvkVs {
    bool used;
    uint32_t hash, start;
    int state;                  /* 0 compiling, 1 ready, 2 unusable (atomic) */
    VkShaderModule mod;
    char *src;
    uint64_t sid;               /* the GLSL's hash and length: its SPIR-V cache name, its pipelines' stable key */
    uint32_t slen;
    int pos_class;
    uint32_t pos_consts[6];     /* VR gun latch: constant registers feeding oPos (kvk_vsh_pos_consts) */
    uint32_t *words, nwords;
} KvkVs;

typedef struct KvkPs {
    bool used;
    uint64_t key;
    int state;
    VkShaderModule mod;
    char *src;
    uint64_t sid;               /* as KvkVs */
    uint32_t slen;
} KvkPs;

/* A pipeline's key. Shaders and the render pass are named by stable identities where there are some (the GLSL's hash
 * and length, KVK_SID_* for the built-in vertex/uber shaders, the pass's formats and views), so the same key comes
 * back in a later run and the pipeline manifest can precreate it; internal passes use their handles. */
typedef struct KvkPipeKey {
    uint64_t vs, fs, rp;
    uint32_t blend, ds, rs, pad;
    uint32_t vlen, flen;
} KvkPipeKey;

/* state: 0 being created on a compile worker, 1 ready, 2 failed (atomic). The GPU thread owns the table; a worker
 * only fills in pipe and state of an entry the GPU thread inserted. */
typedef struct KvkPipe { bool used; KvkPipeKey key; VkPipeline pipe; int state; } KvkPipe;

typedef struct KvkRp { bool used; VkFormat cf, df; uint32_t views; int cload, dload; VkRenderPass rp; } KvkRp;
typedef struct KvkFb { bool used; VkImageView c, d; VkRenderPass rp; uint32_t w, h; VkFramebuffer fb; } KvkFb;
typedef struct KvkSmp { bool used; uint64_t key; VkSampler s; } KvkSmp;
typedef struct KvkTset { bool used; VkImageView v[4]; VkSampler s[4]; VkDescriptorSet set; uint64_t stamp; } KvkTset;
typedef struct KvkEyeView { VkImage img; uint32_t layer, layers; VkFormat fmt; VkImageView view; VkFramebuffer fb[2]; VkRenderPass rp[2]; uint32_t w, h; } KvkEyeView;

static struct {
    int tried, ok;
    VkDevice dev;
    VkPhysicalDevice phys;
    VkQueue queue;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memp;
    VkFormat depth_fmt;
    uint32_t layers;            /* 2 under stereo */
    VkDeviceSize ubo_align, ssbo_align;
    VkDescriptorSetLayout dsl0, dsl1;
    VkPipelineLayout layout;
    VkDescriptorPool set0_pool;
    VkPipelineCache pcache;
    uint64_t pipes_at_save, pipes_seen, pipes_quiet_from;
    KvkFrame frame[KVK_FRAMES];
    int fi;
    VkShaderModule vs_cpu, ps_uber, ps_uber_flat;
    VkShaderModule fix[KVK_FIX_COUNT];
    KvkImage dummy;
    VkSampler smp_nearest, smp_linear;
    KvkSurf surf[KVK_MAX_SURF];
    KvkTex *tex;
    KvkVs vs[KVK_VS_MAX];
    KvkPs ps[KVK_PS_MAX];
    KvkPipe *pipe;
    KvkRp rp[KVK_RP_MAX];
    KvkFb fb[KVK_FB_MAX];
    KvkSmp smp[KVK_SMP_MAX];
    KvkTset tset[KVK_TSET_CACHE];
    VkDescriptorPool tset_pool;     /* the cached sets (FREE_DESCRIPTOR_SET); per-frame pools are the fallback */
    uint64_t tset_stamp;
    KvkEyeView eyeview[KVK_EYEVIEW_MAX];
    KvkImage fb_copy;
    uint64_t epoch, clock, wseq, surf_gen;
    uint64_t surf_va_gen, surf_va_bits[4];  /* a filter of the used surfaces' addresses, as of surf_gen (surf_va_maybe) */
    KvkSurf *surf_last[2];                   /* surf_get_ex's last colour / depth result */
    unsigned int host_w, host_h, hud_w, hud_h;
    KvkStats st;
    pthread_mutex_t qlock_internal;
} G;

/* the open render pass and what is bound on the frame's command buffer */
static struct {
    bool active;
    KvkSurf *cs, *zs;
    uint32_t views;
    VkPipeline pipe;
    VkDescriptorSet set0, set1;
    uint32_t offs[6];
    bool bound0;
    VkViewport vp;
    VkRect2D sc;
    float bc[4];
    uint32_t sref, smr, smw;
    float zb, zsl;
    bool dyn;
    VkBuffer ib;
    VkDeviceSize ib_off;
    VkIndexType ibt;
} P;

/* VR per frame */
enum { REGION_AUTO = 0, REGION_WORLD, REGION_SCREEN };
static struct {
    bool prepared;
    KvkStereoFrame st;
    KvkStereoFrame st_vm;       /* VR gun latch: what the weapon's draws read (== st until the flip's hand delta) */
    KvkStereoConfig cfg;
    int region;
    bool hud, hud_used;
    uint64_t world_draws, stereo_draws;
} V;

/* last uploads (per chunk), so unchanged constants are not copied again */
static struct { int chunk; VkDeviceSize off; bool have; } s_last_ps, s_last_vsg, s_last_c;
static PSConst s_last_ps_val;
static float s_last_vsg_val[20];
static float s_last_c_val[192 * 4];

static volatile uint32_t s_vm_hash[8];
static volatile int s_vm_count;
static volatile float s_vm_scale = 1.0f;
long long g_kgpu_ticks[KGP_COUNT];
static int s_enabled = -1, s_vs_enabled = -1, s_ps_spec = -1, s_dbg = -1;

/* RECOMP_KGPU_DRAWLOG=1: why a draw was dropped */
static void skip_note(const char *why)
{
    if (s_dbg < 0) { const char *e = getenv("RECOMP_KGPU_DRAWLOG"); s_dbg = e && *e && *e != '0'; }
    G.st.skipped++;
    if (s_dbg) LOG("draw skipped: %s", why);
}

static void queue_lock(void)
{
    if (s_dev.queue_lock) s_dev.queue_lock(s_dev.queue_ctx);
    else pthread_mutex_lock(&G.qlock_internal);
}

static void queue_unlock(void)
{
    if (s_dev.queue_unlock) s_dev.queue_unlock(s_dev.queue_ctx);
    else pthread_mutex_unlock(&G.qlock_internal);
}

/* ---- memory, buffers, images ----------------------------------------------------------------------------------- */

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    uint32_t i;
    for (i = 0; i < G.memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (G.memp.memoryTypes[i].propertyFlags & want) == want &&
            !(G.memp.memoryTypes[i].propertyFlags & avoid)) return i;
    if (avoid) return mem_type(bits, want, 0);
    return UINT32_MAX;
}

static bool buf_create(KvkBuf *b, VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    memset(b, 0, sizeof(*b));
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!VKOK(vkCreateBuffer(G.dev, &bi, NULL, &b->buf))) return false;
    vkGetBufferMemoryRequirements(G.dev, b->buf, &mr);
    ai.allocationSize = mr.size;
    if (host == BUF_READBACK) {
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        if (ai.memoryTypeIndex == UINT32_MAX)
            ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
        if (ai.memoryTypeIndex == UINT32_MAX)
            ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    } else if (host) {
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    } else {
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    }
    if (ai.memoryTypeIndex == UINT32_MAX || !VKOK(vkAllocateMemory(G.dev, &ai, NULL, &b->mem))) {
        vkDestroyBuffer(G.dev, b->buf, NULL);
        memset(b, 0, sizeof(*b));
        return false;
    }
    vkBindBufferMemory(G.dev, b->buf, b->mem, 0);
    if (host && !VKOK(vkMapMemory(G.dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->map))) {
        vkDestroyBuffer(G.dev, b->buf, NULL);
        vkFreeMemory(G.dev, b->mem, NULL);
        memset(b, 0, sizeof(*b));
        return false;
    }
    b->size = size;
    b->coherent = !host || (G.memp.memoryTypes[ai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return true;
}

static void buf_destroy(KvkBuf *b)
{
    if (b->buf) vkDestroyBuffer(G.dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(G.dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static VkImageAspectFlags format_aspect(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

static bool img_create(KvkImage *im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, uint32_t levels,
                       VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    memset(im, 0, sizeof(*im));
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!VKOK(vkCreateImage(G.dev, &ci, NULL, &im->img))) return false;
    vkGetImageMemoryRequirements(G.dev, im->img, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || !VKOK(vkAllocateMemory(G.dev, &ai, NULL, &im->mem))) {
        vkDestroyImage(G.dev, im->img, NULL);
        memset(im, 0, sizeof(*im));
        return false;
    }
    vkBindImageMemory(G.dev, im->img, im->mem, 0);
    im->fmt = fmt; im->w = w; im->h = h; im->layers = layers; im->levels = levels;
    im->aspect = format_aspect(fmt);
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = im->aspect;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = layers;
    if (!VKOK(vkCreateImageView(G.dev, &vi, NULL, &im->att))) goto fail;
    if (im->aspect != VK_IMAGE_ASPECT_COLOR_BIT && (usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (!VKOK(vkCreateImageView(G.dev, &vi, NULL, &im->view))) goto fail;
    } else {
        im->view = im->att;
    }
    return true;
fail:
    if (im->att) vkDestroyImageView(G.dev, im->att, NULL);
    vkDestroyImage(G.dev, im->img, NULL);
    vkFreeMemory(G.dev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
    return false;
}

static void img_destroy_now(KvkImage *im)
{
    if (im->view && im->view != im->att) vkDestroyImageView(G.dev, im->view, NULL);
    if (im->att) vkDestroyImageView(G.dev, im->att, NULL);
    if (im->img) vkDestroyImage(G.dev, im->img, NULL);
    if (im->mem) vkFreeMemory(G.dev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

static KvkFrame *cur_frame(void) { return &G.frame[G.fi]; }

static void defer(const KvkDead *d)
{
    KvkFrame *f = cur_frame();
    if (f->ndead == f->capdead) {
        int cap = f->capdead ? f->capdead * 2 : 64;
        KvkDead *g = (KvkDead *)realloc(f->dead, (size_t)cap * sizeof(*g));
        if (!g) return;                     /* leaked rather than destroyed early */
        f->dead = g; f->capdead = cap;
    }
    f->dead[f->ndead++] = *d;
}

static void fb_forget_view(VkImageView v);
static void tset_forget(VkImageView v, VkSampler s);

static void img_release(KvkImage *im)
{
    KvkDead d;
    if (!im->img) return;
    fb_forget_view(im->att);
    if (im->view != im->att) fb_forget_view(im->view);
    tset_forget(im->view, VK_NULL_HANDLE);
    if (im->att && im->att != im->view) tset_forget(im->att, VK_NULL_HANDLE);
    memset(&d, 0, sizeof(d));
    d.kind = DEAD_IMAGE;
    d.im = *im;
    defer(&d);
    memset(im, 0, sizeof(*im));
}

static void run_dead(KvkFrame *f)
{
    int i;
    for (i = 0; i < f->ndead; i++) {
        KvkDead *d = &f->dead[i];
        switch (d->kind) {
        case DEAD_IMAGE: img_destroy_now(&d->im); break;
        case DEAD_BUF: buf_destroy(&d->b); break;
        case DEAD_FB: vkDestroyFramebuffer(G.dev, (VkFramebuffer)(uintptr_t)d->h, NULL); break;
        case DEAD_SAMPLER: vkDestroySampler(G.dev, (VkSampler)(uintptr_t)d->h, NULL); break;
        case DEAD_TSET: { VkDescriptorSet ds = (VkDescriptorSet)(uintptr_t)d->h;
                          vkFreeDescriptorSets(G.dev, G.tset_pool, 1, &ds); } break;
        }
    }
    f->ndead = 0;
}

/* ---- layouts ------------------------------------------------------------------------------------------------- */

static void pass_end(void);

static void layout_use(VkImageLayout l, VkPipelineStageFlags *stage, VkAccessFlags *access)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    default:
        *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        *access = 0;
        break;
    }
}

static void barrier(VkCommandBuffer cb, KvkImage *im, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkPipelineStageFlags ss, ds;
    VkAccessFlags sa, da;
    if (im->layout == to) return;
    /* barriers and transfers belong outside a render pass */
    if (cb == G.frame[G.fi].cb && P.active) pass_end();
    layout_use(im->layout, &ss, &sa);
    layout_use(to, &ds, &da);
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = im->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange.aspectMask = im->aspect;
    b.subresourceRange.levelCount = im->levels;
    b.subresourceRange.layerCount = im->layers;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = to;
}

/* ---- frames and the ring -------------------------------------------------------------------------------------- */

static void pass_end(void);

static bool chunk_create(KvkChunk *c)
{
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorBufferInfo bi[6];
    VkWriteDescriptorSet w[6];
    static const VkDeviceSize ranges[6] = { sizeof(PSConst), 80, 192 * 16, 512, KVK_VAB_RANGE, KVK_STEREO_BYTES };
    int i;
    memset(c, 0, sizeof(*c));
    if (!buf_create(&c->b, KVK_CHUNK_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BUF_UPLOAD)) return false;
    ai.descriptorPool = G.set0_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &G.dsl0;
    if (!VKOK(vkAllocateDescriptorSets(G.dev, &ai, &c->set0))) { buf_destroy(&c->b); return false; }
    for (i = 0; i < 6; i++) {
        bi[i].buffer = c->b.buf; bi[i].offset = 0; bi[i].range = ranges[i];
        memset(&w[i], 0, sizeof(w[i]));
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = c->set0;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(G.dev, 6, w, 0, NULL);
    return true;
}

static void last_reset(void)
{
    s_last_ps.have = s_last_vsg.have = s_last_c.have = false;
}

/* Room for `bytes` (with alignment slack) in the current chunk, moving on to the next one when it is full. */
static bool ring_reserve(VkDeviceSize bytes)
{
    KvkFrame *f = cur_frame();
    KvkChunk *c = &f->chunk[f->cur];
    const VkDeviceSize usable = KVK_CHUNK_SIZE - KVK_VAB_RANGE;
    if (bytes > usable) return false;
    if (c->pos + bytes <= usable) return true;
    if (f->cur + 1 >= KVK_CHUNKS) return false;
    if (f->cur + 1 >= f->nchunk) {
        if (!chunk_create(&f->chunk[f->nchunk])) return false;
        f->nchunk++;
    }
    f->cur++;
    f->chunk[f->cur].pos = 0;
    f->chunk[f->cur].stereo = false;
    f->chunk[f->cur].stereo_vm = false;
    last_reset();
    P.bound0 = false;
    return true;
}

static uint8_t *ring_take(VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off)
{
    KvkFrame *f = cur_frame();
    KvkChunk *c = &f->chunk[f->cur];
    VkDeviceSize at = (c->pos + align - 1) / align * align;
    c->pos = at + size;
    *off = at;
    return c->b.map + at;
}

static VkDeviceSize ualign(void) { return G.ubo_align; }

static VkCommandBuffer cmd(void) { return cur_frame()->cb; }

static VkCommandBuffer upload_cmd(void)
{
    KvkFrame *f = cur_frame();
    if (!f->ucb_open) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f->ucb, &bi);
        f->ucb_open = true;
    }
    return f->ucb;
}

/* A failed submit or fence wait is logged (the first few): VK_ERROR_DEVICE_LOST otherwise only shows as frames that
 * stop changing, which on the headset reads as a hang (port/notes/device-preflight.md). */
static void vk_frame_error(const char *what, VkResult r)
{
    static int reported;
    if (r == VK_SUCCESS || reported >= 4) return;
    reported++;
    LOG("%s failed: %d%s", what, (int)r,
        r == VK_ERROR_DEVICE_LOST ? " = VK_ERROR_DEVICE_LOST: the GPU hung or its driver reset; nothing more will draw"
        : r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? " = VK_ERROR_OUT_OF_DEVICE_MEMORY" : r == VK_ERROR_OUT_OF_HOST_MEMORY
        ? " = VK_ERROR_OUT_OF_HOST_MEMORY" : "");
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "BlackVR", "GPU %s failed: VkResult %d%s", what, (int)r,
                        r == VK_ERROR_DEVICE_LOST ? " (VK_ERROR_DEVICE_LOST: GPU hang / driver reset)" : "");
#endif
}

static void frame_begin(void)
{
    KvkFrame *f = cur_frame();
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    int i;
    if (f->pending) {
        vk_frame_error("vkWaitForFences (frame)", vkWaitForFences(G.dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
        vkResetFences(G.dev, 1, &f->fence);
        f->pending = false;
    }
    run_dead(f);
    for (i = 0; i < f->ntpool; i++) vkResetDescriptorPool(G.dev, f->tpool[i], 0);
    f->curtpool = 0;
    for (i = 0; i < f->nchunk; i++) { f->chunk[i].pos = 0; f->chunk[i].stereo = false; f->chunk[i].stereo_vm = false; }
    f->cur = 0;
    vkResetCommandPool(G.dev, f->pool, 0);
    f->ucb_open = false;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f->cb, &bi);
    last_reset();
    memset(&P, 0, sizeof(P));
}

/* End the frame's recording and submit it (texture uploads first). */
static void frame_submit(bool wait)
{
    KvkFrame *f = cur_frame();
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    pass_end();
    if (f->ucb_open) { vkEndCommandBuffer(f->ucb); cbs[n++] = f->ucb; }
    vkEndCommandBuffer(f->cb);
    cbs[n++] = f->cb;
    si.commandBufferCount = n;
    si.pCommandBuffers = cbs;
    queue_lock();
    vk_frame_error("vkQueueSubmit (frame)", vkQueueSubmit(G.queue, 1, &si, f->fence));
    queue_unlock();
    f->pending = true;
    if (wait) {
        vkWaitForFences(G.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        vkResetFences(G.dev, 1, &f->fence);
        f->pending = false;
    }
    G.fi = (G.fi + 1) % KVK_FRAMES;
    frame_begin();
}

/* ---- disk -------------------------------------------------------------------------------------------------------- */

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = buf ? (size_t)n : 0;
    return buf;
}

static void write_file(const char *path, const void *p, size_t n)
{
    char tmp[700];
    FILE *f;
    mkdir(cache_dir(), 0755);
    snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)pthread_self() & 0xFFFFFF);
    f = fopen(tmp, "wb");
    if (!f) return;
    if (fwrite(p, 1, n, f) != n) { fclose(f); remove(tmp); return; }
    fclose(f);
    if (rename(tmp, path)) remove(tmp);
}

/* ---- shader modules -------------------------------------------------------------------------------------------- */

static uint64_t kvk_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static VkShaderModule module_from_words(const uint32_t *w, size_t count)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = count * 4;
    ci.pCode = w;
    if (!VKOK(vkCreateShaderModule(G.dev, &ci, NULL, &m))) return VK_NULL_HANDLE;
    return m;
}

/* SPIR-V for a source: from the disk cache, else compiled (and cached). */
static VkShaderModule module_from_glsl(int stage, const char *src, bool use_cache)
{
    char path[640];
    size_t n = strlen(src), size = 0;
    uint32_t *words;
    KvkSpirv sp;
    VkShaderModule m;
    uint64_t t0;
    bool ok;
    snprintf(path, sizeof(path), "%s/vk_%c_%016llx_%05zx.spv", cache_dir(), stage == KVK_STAGE_VERTEX ? 'v' : 'f',
             (unsigned long long)kvk_hash64(src, n), n);
    if (use_cache && (words = (uint32_t *)read_file(path, &size)) != NULL) {
        if (size >= 20 && size % 4 == 0 && words[0] == 0x07230203u) {
            m = module_from_words(words, size / 4);
            free(words);
            if (m) { __atomic_add_fetch(&G.st.spv_cache_hits, 1, __ATOMIC_RELAXED); return m; }
        } else {
            free(words);
        }
    }
    t0 = kvk_now_ns();
    ok = kvk_spirv_compile(stage, src, &sp);
    __atomic_add_fetch(&G.st.glslang_ns, kvk_now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&G.st.glslang_compiles, 1, __ATOMIC_RELAXED);
    if (!ok) {
        LOG("shader did not compile: %s", sp.log ? sp.log : "(no log)");
        if (getenv("RECOMP_KGPU_SHADER_LOG")) fprintf(stderr, "%s\n", src);
        kvk_spirv_free(&sp);
        return VK_NULL_HANDLE;
    }
    m = module_from_words(sp.words, sp.count);
    if (m && use_cache) write_file(path, sp.words, sp.count * 4);
    kvk_spirv_free(&sp);
    return m;
}

/* ---- compile workers -------------------------------------------------------------------------------------------- */

typedef struct KvkJob { int kind; void *p; } KvkJob;
#define KVK_JOBS 4096
static KvkJob s_jobs[KVK_JOBS];
static int s_jhead, s_jtail, s_jbusy, s_nworkers;
static pthread_mutex_t s_jlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_jcond = PTHREAD_COND_INITIALIZER, s_jdone = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_manifest_lock = PTHREAD_MUTEX_INITIALIZER;

static void manifest_add(const KvkVs *e);
struct KvkPipeJob;
static void pipe_job_run(struct KvkPipeJob *pj);

static void job_run(KvkJob j)
{
    if (j.kind == 0) {
        KvkVs *e = (KvkVs *)j.p;
        VkShaderModule m = module_from_glsl(KVK_STAGE_VERTEX, e->src, true);
        if (m) {
            e->mod = m;
            __atomic_store_n(&e->state, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&G.st.vs_compiled, 1, __ATOMIC_RELAXED);
            manifest_add(e);
        } else {
            __atomic_store_n(&e->state, 2, __ATOMIC_RELEASE);
        }
    } else if (j.kind == 1) {
        KvkPs *e = (KvkPs *)j.p;
        VkShaderModule m = module_from_glsl(KVK_STAGE_FRAGMENT, e->src, true);
        if (m) {
            e->mod = m;
            __atomic_store_n(&e->state, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&G.st.ps_compiled, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_store_n(&e->state, 2, __ATOMIC_RELEASE);
        }
    } else if (j.kind == 2) {
        /* precompile from the manifest: only fills the disk cache */
        char *src = (char *)j.p;
        VkShaderModule m = module_from_glsl(KVK_STAGE_VERTEX, src, true);
        if (m) vkDestroyShaderModule(G.dev, m, NULL);
        free(src);
    } else if (j.kind == 3) {
        pipe_job_run((struct KvkPipeJob *)j.p);      /* a pipeline (the pipelines section) */
    }
}

static void *worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        KvkJob j;
        pthread_mutex_lock(&s_jlock);
        while (s_jhead == s_jtail) pthread_cond_wait(&s_jcond, &s_jlock);
        j = s_jobs[s_jtail % KVK_JOBS];
        s_jtail++;
        s_jbusy++;
        pthread_mutex_unlock(&s_jlock);
        job_run(j);
        pthread_mutex_lock(&s_jlock);
        s_jbusy--;
        pthread_cond_broadcast(&s_jdone);
        pthread_mutex_unlock(&s_jlock);
    }
    return NULL;
}

static void job_post(int kind, void *p)
{
    bool run_now = false;
    pthread_mutex_lock(&s_jlock);
    if (!s_nworkers) {
        const char *e = getenv("RECOMP_KGPU_VS_THREADS");
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        int n = e && *e ? atoi(e) : (int)(ncpu / 2), i;
        if (n > 4) n = 4;
        if (n < 1) n = 1;
        for (i = 0; i < n; i++) {
            pthread_t th;
            if (pthread_create(&th, NULL, worker_main, NULL) == 0) { pthread_detach(th); s_nworkers++; }
        }
        if (!s_nworkers) s_nworkers = -1;
    }
    if (s_nworkers < 0 || s_jhead - s_jtail >= KVK_JOBS) {
        run_now = true;
    } else {
        s_jobs[s_jhead % KVK_JOBS].kind = kind;
        s_jobs[s_jhead % KVK_JOBS].p = p;
        s_jhead++;
        pthread_cond_signal(&s_jcond);
    }
    pthread_mutex_unlock(&s_jlock);
    if (run_now) { KvkJob j; j.kind = kind; j.p = p; job_run(j); }
}

void kvk_wait_compiles(void)
{
    pthread_mutex_lock(&s_jlock);
    while (s_jhead != s_jtail || s_jbusy) pthread_cond_wait(&s_jdone, &s_jlock);
    pthread_mutex_unlock(&s_jlock);
}

/* ---- the vertex-program manifest (the D3D11 backend's vs_manifest.bin record format, a file of its own) -------- */

#define KVK_MAN_MAGIC 0x3153564Bu
static uint64_t *s_man_keys;
static int s_man_n, s_man_cap;
static bool s_man_loaded;

static void manifest_path(char *out, size_t cap) { snprintf(out, cap, "%s/vk_vs_manifest.bin", cache_dir()); }

static bool manifest_has(uint64_t k)
{
    int i;
    for (i = 0; i < s_man_n; i++) if (s_man_keys[i] == k) return true;
    return false;
}

static void manifest_note(uint64_t k)
{
    if (manifest_has(k)) return;
    if (s_man_n == s_man_cap) {
        int cap = s_man_cap ? s_man_cap * 2 : 256;
        uint64_t *g = (uint64_t *)realloc(s_man_keys, (size_t)cap * sizeof(*g));
        if (!g) return;
        s_man_keys = g; s_man_cap = cap;
    }
    s_man_keys[s_man_n++] = k;
}

static uint32_t fnv32(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* Every program met in an earlier run: its GLSL compiled into the disk cache on the workers, now, so a level's first
 * frames do not wait for glslang (the driver's pipeline compile still happens at first use; VkPipelineCache keeps
 * that across runs). */
static void manifest_load_and_precompile(void)
{
    char path[640];
    size_t n = 0, at = 0;
    uint8_t *buf;
    int queued = 0;
    manifest_path(path, sizeof(path));
    buf = (uint8_t *)read_file(path, &n);
    s_man_loaded = true;
    if (!buf) return;
    while (at + 16 <= n) {
        const uint32_t *hdr = (const uint32_t *)(buf + at);
        Nv2aVshProgram prog;
        char *src;
        if (hdr[0] != KVK_MAN_MAGIC || !hdr[3] || hdr[3] % 4 || hdr[3] > 136 * 4 || at + 16 + (size_t)hdr[3] * 4 > n) break;
        manifest_note(((uint64_t)hdr[1] << 32) | hdr[2]);
        memset(&prog, 0, sizeof(prog));
        if (nv2a_vsh_parse_program(&prog, hdr + 4, hdr[3] / 4) == NV2AVPR_SUCCESS) {
            src = kvk_glsl_vs_program(&prog);
            nv2a_vsh_program_destroy(&prog);
            if (src) { job_post(2, src); queued++; }
        }
        at += 16 + (size_t)hdr[3] * 4;
    }
    free(buf);
    if (queued) LOG("vertex program manifest: %d programs queued for the shader cache", queued);
}

static void manifest_add(const KvkVs *e)
{
    uint64_t key;
    uint32_t hdr[4];
    char path[640];
    FILE *f;
    if (!e->words || !e->nwords || e->nwords % 4) return;
    hdr[1] = fnv32(e->src, strlen(e->src));
    hdr[2] = (uint32_t)strlen(e->src);
    key = ((uint64_t)hdr[1] << 32) | hdr[2];
    pthread_mutex_lock(&s_manifest_lock);
    if (!manifest_has(key)) {
        manifest_note(key);
        manifest_path(path, sizeof(path));
        mkdir(cache_dir(), 0755);
        f = fopen(path, "ab");
        if (f) {
            hdr[0] = KVK_MAN_MAGIC; hdr[3] = e->nwords;
            fwrite(hdr, sizeof(hdr), 1, f);
            fwrite(e->words, 4, e->nwords, f);
            fclose(f);
        }
    }
    pthread_mutex_unlock(&s_manifest_lock);
}

/* ---- vertex programs and combiner shaders ------------------------------------------------------------------------- */

static KvkVs *vs_lookup(const void *program, uint32_t hash, uint32_t start, const uint32_t *words, uint32_t nwords)
{
    KvkVs *e = NULL;
    unsigned int i, slot = (hash ^ (start * 40503u)) % KVK_VS_MAX;
    for (i = 0; i < KVK_VS_MAX; i++, slot = (slot + 1) % KVK_VS_MAX) {
        KvkVs *q = &G.vs[slot];
        if (!q->used) { e = q; break; }
        if (q->hash == hash && q->start == start) return __atomic_load_n(&q->state, __ATOMIC_ACQUIRE) == 1 ? q : NULL;
    }
    if (!e || !program) return NULL;
    e->used = true; e->hash = hash; e->start = start; e->state = 0;
    e->src = kvk_glsl_vs_program((const Nv2aVshProgram *)program);
    if (!e->src) {
        e->state = 2;
        LOG("vertex program %08X/%u runs on the CPU (unsupported operation)", hash, start);
        return NULL;
    }
    e->slen = (uint32_t)strlen(e->src);
    e->sid = kvk_hash64(e->src, e->slen);
    e->pos_class = kvk_vsh_pos_class((const Nv2aVshProgram *)program);
    kvk_vsh_pos_consts((const Nv2aVshProgram *)program, e->pos_consts);
    if (words && nwords) {
        e->words = (uint32_t *)malloc((size_t)nwords * 4);
        if (e->words) { memcpy(e->words, words, (size_t)nwords * 4); e->nwords = nwords; }
    }
    job_post(0, e);
    return __atomic_load_n(&e->state, __ATOMIC_ACQUIRE) == 1 ? e : NULL;
}

static VkShaderModule ps_lookup(const PSConst *c, uint64_t *sid, uint32_t *slen)
{
    uint64_t key;
    unsigned int i, slot;
    KvkPs *e = NULL;
    if (s_ps_spec < 0) {
        const char *env = getenv("RECOMP_KGPU_PS_SPEC");
        s_ps_spec = !(env && *env == '0');
    }
    if (!s_ps_spec) return VK_NULL_HANDLE;
    key = kvk_ps_key(c);
    slot = (unsigned int)(key % KVK_PS_MAX);
    for (i = 0; i < KVK_PS_MAX; i++, slot = (slot + 1) % KVK_PS_MAX) {
        KvkPs *q = &G.ps[slot];
        if (!q->used) { e = q; break; }
        if (q->key == key) {
            if (__atomic_load_n(&q->state, __ATOMIC_ACQUIRE) != 1) return VK_NULL_HANDLE;
            *sid = q->sid; *slen = q->slen;
            return q->mod;
        }
    }
    if (!e) return VK_NULL_HANDLE;
    e->used = true; e->key = key; e->state = 0;
    e->src = kvk_glsl_ps_spec(c);
    if (!e->src) { e->state = 2; return VK_NULL_HANDLE; }
    e->slen = (uint32_t)strlen(e->src);
    e->sid = kvk_hash64(e->src, e->slen);
    job_post(1, e);
    if (__atomic_load_n(&e->state, __ATOMIC_ACQUIRE) != 1) return VK_NULL_HANDLE;
    *sid = e->sid; *slen = e->slen;
    return e->mod;
}

/* ---- render passes, framebuffers -------------------------------------------------------------------------------- */

enum { LOAD_LOAD = 0, LOAD_CLEAR, LOAD_DONTCARE };

static VkRenderPass rp_get(VkFormat cf, VkFormat df, uint32_t views, int cload, int dload)
{
    VkAttachmentDescription at[2];
    VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dr;
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkRenderPassMultiviewCreateInfo mv = { VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO };
    uint32_t mask = (1u << views) - 1u, n = 0;
    static const VkAttachmentLoadOp ops[3] = { VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    int i;
    KvkRp *free_slot = NULL;
    for (i = 0; i < KVK_RP_MAX; i++) {
        KvkRp *r = &G.rp[i];
        if (!r->used) { if (!free_slot) free_slot = r; continue; }
        if (r->cf == cf && r->df == df && r->views == views && r->cload == cload && r->dload == dload) return r->rp;
    }
    if (!free_slot) return VK_NULL_HANDLE;
    memset(at, 0, sizeof(at));
    memset(&sp, 0, sizeof(sp));
    memset(dep, 0, sizeof(dep));
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (cf != VK_FORMAT_UNDEFINED) {
        at[n].format = cf;
        at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = ops[cload];
        at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[n].initialLayout = at[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cr.attachment = n++;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &cr;
    }
    if (df != VK_FORMAT_UNDEFINED) {
        at[n].format = df;
        at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = ops[dload];
        at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = ops[dload];
        at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].initialLayout = at[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dr.attachment = n++;
        dr.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sp.pDepthStencilAttachment = &dr;
    }
    /* everything before and after the pass that touches its attachments: earlier passes, transfers, clears.
     * The subpass side of each dependency may only name graphics stages (VUID-VkRenderPassCreateInfo-
     * pDependencies-00837/00838): TRANSFER and its accesses go on the external side only. */
    for (i = 0; i < 2; i++) {
        const VkPipelineStageFlags gfx = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        const VkAccessFlags att_w = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        const VkAccessFlags att_rw = att_w | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        dep[i].srcSubpass = i == 0 ? VK_SUBPASS_EXTERNAL : 0;
        dep[i].dstSubpass = i == 0 ? 0 : VK_SUBPASS_EXTERNAL;
        dep[i].srcStageMask = i == 0 ? gfx | VK_PIPELINE_STAGE_TRANSFER_BIT : gfx;
        dep[i].dstStageMask = i == 0 ? gfx : gfx | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[i].srcAccessMask = i == 0 ? att_w | VK_ACCESS_TRANSFER_WRITE_BIT : att_w;
        dep[i].dstAccessMask = i == 0 ? att_rw : att_rw | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    ci.attachmentCount = n;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = dep;
    if (views > 1) {
        mv.subpassCount = 1;
        mv.pViewMasks = &mask;
        mv.correlationMaskCount = 1;
        mv.pCorrelationMasks = &mask;
        ci.pNext = &mv;
    }
    if (!VKOK(vkCreateRenderPass(G.dev, &ci, NULL, &free_slot->rp))) return VK_NULL_HANDLE;
    free_slot->used = true;
    free_slot->cf = cf; free_slot->df = df; free_slot->views = views;
    free_slot->cload = cload; free_slot->dload = dload;
    return free_slot->rp;
}

static VkFramebuffer fb_get(VkRenderPass compat, VkImageView c, VkImageView d, uint32_t w, uint32_t h)
{
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView v[2];
    uint32_t n = 0;
    int i;
    KvkFb *slot = NULL;
    for (i = 0; i < KVK_FB_MAX; i++) {
        KvkFb *f = &G.fb[i];
        if (!f->used) { if (!slot) slot = f; continue; }
        if (f->c == c && f->d == d && f->rp == compat && f->w == w && f->h == h) return f->fb;
    }
    if (!slot) {
        /* full: drop one (its frame may still use it: deferred) */
        static unsigned rr;
        KvkDead dd;
        slot = &G.fb[rr++ % KVK_FB_MAX];
        memset(&dd, 0, sizeof(dd));
        dd.kind = DEAD_FB; dd.h = (uint64_t)(uintptr_t)slot->fb;
        defer(&dd);
        slot->used = false;
    }
    if (c) v[n++] = c;
    if (d) v[n++] = d;
    ci.renderPass = compat;
    ci.attachmentCount = n;
    ci.pAttachments = v;
    ci.width = w; ci.height = h; ci.layers = 1;
    if (!VKOK(vkCreateFramebuffer(G.dev, &ci, NULL, &slot->fb))) return VK_NULL_HANDLE;
    slot->used = true; slot->c = c; slot->d = d; slot->rp = compat; slot->w = w; slot->h = h;
    return slot->fb;
}

static void fb_forget_view(VkImageView v)
{
    int i;
    if (!v) return;
    for (i = 0; i < KVK_FB_MAX; i++) {
        KvkFb *f = &G.fb[i];
        if (f->used && (f->c == v || f->d == v)) {
            KvkDead dd;
            memset(&dd, 0, sizeof(dd));
            dd.kind = DEAD_FB; dd.h = (uint64_t)(uintptr_t)f->fb;
            defer(&dd);
            memset(f, 0, sizeof(*f));
        }
    }
}

/* ---- pipelines ---------------------------------------------------------------------------------------------------- */

/* packed state */
#define BL_EN(x)      ((uint32_t)(x) & 1u)
static uint32_t pack_blend(bool en, VkBlendFactor s, VkBlendFactor d, VkBlendOp op, VkBlendFactor sa,
                           VkBlendFactor da, VkBlendOp opa, VkColorComponentFlags mask)
{
    return (en ? 1u : 0u) | ((uint32_t)s << 1) | ((uint32_t)d << 6) | ((uint32_t)op << 11) | ((uint32_t)sa << 14) |
           ((uint32_t)da << 19) | ((uint32_t)opa << 24) | ((uint32_t)mask << 27);
}

enum { RS_LIST = 0, RS_STRIP = 1, RS_QUADSTRIP = 2 };

typedef struct KvkPipeDesc {
    VkShaderModule vs, fs;
    VkRenderPass rp;
    uint32_t blend;             /* pack_blend; 0 with has_color false */
    bool has_color;
    bool ztest, zwrite, stencil;
    int zfunc, sfunc, sfail, szfail, szpass;
    int topo;                   /* RS_* */
    VkCullModeFlags cull;
    bool ccw, clamp;
    /* Stable identities (title draws): the shaders' KvkVs/KvkPs sid+slen or KVK_SID_*, and the compatible render
     * pass's formats and views (pass_compat_desc). All set: the pipeline may be created asynchronously and goes into
     * the pipeline manifest. Zero (internal passes): keyed by handle, created at once. */
    uint64_t vs_sid, fs_sid;
    uint32_t vs_len, fs_len;
    VkFormat cf, df;
    uint32_t views;
} KvkPipeDesc;

/* the built-in shaders' identities (a GLSL hash is never this small) */
#define KVK_SID_VS_CPU    1u
#define KVK_SID_UBER      2u
#define KVK_SID_UBER_FLAT 3u
/* the backend's depth format, whichever the device has (D24S8 on a Quest, D32S8 on SwiftShader), in the manifest */
#define KVK_FMT_THE_DEPTH 0x7FFFFFF0u

static uint64_t rp_sid(VkFormat cf, VkFormat df, uint32_t views)
{
    uint32_t w[3];
    w[0] = (uint32_t)cf;
    w[1] = df == G.depth_fmt && df != VK_FORMAT_UNDEFINED ? KVK_FMT_THE_DEPTH : (uint32_t)df;
    w[2] = views;
    return kvk_hash64(w, sizeof(w)) | 1u;
}

static bool pipe_stable(const KvkPipeDesc *d) { return d->vs_sid && d->fs_sid && d->views; }

static void pipe_key(const KvkPipeDesc *d, KvkPipeKey *k)
{
    memset(k, 0, sizeof(*k));
    if (pipe_stable(d)) {
        k->vs = d->vs_sid; k->vlen = d->vs_len;
        k->fs = d->fs_sid; k->flen = d->fs_len;
        k->rp = rp_sid(d->cf, d->df, d->views);
    } else {
        k->vs = (uint64_t)(uintptr_t)d->vs; k->fs = (uint64_t)(uintptr_t)d->fs; k->rp = (uint64_t)(uintptr_t)d->rp;
    }
    k->blend = d->has_color ? d->blend | 0x80000000u : 0;
    k->ds = (d->ztest ? 1u : 0u) | (d->zwrite ? 2u : 0u) | ((uint32_t)(d->zfunc & 7) << 2) | (d->stencil ? 32u : 0u) |
            ((uint32_t)(d->sfunc & 7) << 6) | ((uint32_t)(d->sfail & 15) << 9) | ((uint32_t)(d->szfail & 15) << 13) |
            ((uint32_t)(d->szpass & 15) << 17);
    k->rs = (uint32_t)d->topo | ((uint32_t)d->cull << 2) | (d->ccw ? 16u : 0u) | (d->clamp ? 32u : 0u);
}

/* The inverse of pipe_key's state words (the manifest stores the key). */
static void pipe_desc_from_key(const KvkPipeKey *k, KvkPipeDesc *d)
{
    d->has_color = (k->blend & 0x80000000u) != 0;
    d->blend = k->blend & 0x7FFFFFFFu;
    d->ztest = (k->ds & 1u) != 0; d->zwrite = (k->ds & 2u) != 0; d->zfunc = (int)((k->ds >> 2) & 7);
    d->stencil = (k->ds & 32u) != 0; d->sfunc = (int)((k->ds >> 6) & 7); d->sfail = (int)((k->ds >> 9) & 15);
    d->szfail = (int)((k->ds >> 13) & 15); d->szpass = (int)((k->ds >> 17) & 15);
    d->topo = (int)(k->rs & 3); d->cull = (VkCullModeFlags)((k->rs >> 2) & 3); d->ccw = (k->rs & 16u) != 0;
    d->clamp = (k->rs & 32u) != 0;
}

/* RECOMP_KGPU_PIPE_ASYNC: title-draw pipelines are created on the compile workers; until one is ready the draw uses
 * the ubershader's pipeline with the same state if that one is ready, else it is skipped (a frame or two at a
 * pipeline's first use). Default on Android (the Quest), off elsewhere (the parity tests draw once and compare). */
static int s_pipe_async = -1;
static bool pipe_async(void)
{
    if (s_pipe_async < 0) {
        const char *e = getenv("RECOMP_KGPU_PIPE_ASYNC");
#if defined(__ANDROID__)
        s_pipe_async = !(e && *e == '0');
#else
        s_pipe_async = e && *e && *e != '0';
#endif
    }
    return s_pipe_async != 0;
}

static VkResult pipe_create(const KvkPipeDesc *d, VkPipeline *out)
{
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkStencilOpState so;
    VkResult r;
    uint64_t t0, dt, mx;

    memset(st, 0, sizeof(st));
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d->vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = d->fs; st[1].pName = "main";
    ia.topology = d->topo == RS_LIST ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    ia.primitiveRestartEnable = d->topo == RS_STRIP ? VK_TRUE : VK_FALSE;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d->cull;
    rs.frontFace = d->ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthClampEnable = d->clamp && s_dev.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.depthBiasEnable = VK_TRUE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ds.depthTestEnable = d->ztest || d->zwrite ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d->zwrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = d->ztest ? kvk_compare_op(d->zfunc) : VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = d->stencil ? VK_TRUE : VK_FALSE;
    memset(&so, 0, sizeof(so));
    so.failOp = kvk_stencil_op(d->sfail);
    so.depthFailOp = kvk_stencil_op(d->szfail);
    so.passOp = kvk_stencil_op(d->szpass);
    so.compareOp = kvk_compare_op(d->sfunc);
    ds.front = ds.back = so;
    ds.maxDepthBounds = 1.0f;
    memset(&ba, 0, sizeof(ba));
    ba.blendEnable = BL_EN(d->blend) ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = (VkBlendFactor)((d->blend >> 1) & 31);
    ba.dstColorBlendFactor = (VkBlendFactor)((d->blend >> 6) & 31);
    ba.colorBlendOp = (VkBlendOp)((d->blend >> 11) & 7);
    ba.srcAlphaBlendFactor = (VkBlendFactor)((d->blend >> 14) & 31);
    ba.dstAlphaBlendFactor = (VkBlendFactor)((d->blend >> 19) & 31);
    ba.alphaBlendOp = (VkBlendOp)((d->blend >> 24) & 7);
    ba.colorWriteMask = (VkColorComponentFlags)((d->blend >> 27) & 15);
    cb.attachmentCount = d->has_color ? 1 : 0;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof(dyn) / sizeof(dyn[0]));
    dy.pDynamicStates = dyn;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = G.layout;
    ci.renderPass = d->rp;
    /* any thread: the device and the (internally synchronised) VkPipelineCache allow it */
    t0 = kvk_now_ns();
    *out = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(G.dev, G.pcache, 1, &ci, NULL, out);
    dt = kvk_now_ns() - t0;
    __atomic_add_fetch(&G.st.pipe_ns, dt, __ATOMIC_RELAXED);
    mx = __atomic_load_n(&G.st.pipe_max_ns, __ATOMIC_RELAXED);
    while (dt > mx && !__atomic_compare_exchange_n(&G.st.pipe_max_ns, &mx, dt, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
    if (!VKOK(r)) {
        LOG("pipeline creation failed (%d)", (int)r);
        *out = VK_NULL_HANDLE;
        return r;
    }
    __atomic_add_fetch(&G.st.pipelines, 1, __ATOMIC_RELAXED);
    return r;
}

/* ---- the pipeline manifest: every title-draw pipeline met in a run, by stable key, so a later run (or a fresh
 * install with a shipped cache, port/notes/perf.md) creates them on the workers at start instead of at first use. --- */
#define KVK_PMAN_MAGIC 0x50564B4Bu          /* "KKVP" */
typedef struct KvkPipeRec {
    uint32_t magic, cf, df, views;          /* df KVK_FMT_THE_DEPTH: the backend's depth format */
    KvkPipeKey key;
} KvkPipeRec;

static void pipe_manifest_path(char *out, size_t cap) { snprintf(out, cap, "%s/vk_pipe_manifest.bin", cache_dir()); }

static void pipe_manifest_add(const KvkPipeDesc *d, const KvkPipeKey *k)
{
    KvkPipeRec r;
    char path[640];
    FILE *f;
    memset(&r, 0, sizeof(r));
    r.magic = KVK_PMAN_MAGIC;
    r.cf = (uint32_t)d->cf;
    r.df = d->df == G.depth_fmt && d->df != VK_FORMAT_UNDEFINED ? KVK_FMT_THE_DEPTH : (uint32_t)d->df;
    r.views = d->views;
    r.key = *k;
    pipe_manifest_path(path, sizeof(path));
    pthread_mutex_lock(&s_manifest_lock);
    mkdir(cache_dir(), 0755);
    if ((f = fopen(path, "ab")) != NULL) {
        fwrite(&r, sizeof(r), 1, f);
        fclose(f);
    }
    pthread_mutex_unlock(&s_manifest_lock);
}

/* A compile worker's pipeline job. Null modules are loaded from the SPIR-V cache by identity (manifest records). */
typedef struct KvkPipeJob {
    KvkPipe *e;
    KvkPipeDesc d;
    KvkPipeKey k;
    bool record;                            /* a pipeline met for the first time: add it to the manifest */
} KvkPipeJob;

static VkShaderModule module_from_cache(int stage, uint64_t sid, uint32_t len)
{
    char path[640];
    size_t size = 0;
    uint32_t *words;
    VkShaderModule m = VK_NULL_HANDLE;
    snprintf(path, sizeof(path), "%s/vk_%c_%016llx_%05x.spv", cache_dir(), stage == KVK_STAGE_VERTEX ? 'v' : 'f',
             (unsigned long long)sid, (unsigned)len);
    if ((words = (uint32_t *)read_file(path, &size)) != NULL) {
        if (size >= 20 && size % 4 == 0 && words[0] == 0x07230203u) m = module_from_words(words, size / 4);
        free(words);
    }
    return m;
}

static void pipe_job_run(KvkPipeJob *pj)
{
    KvkPipeDesc d = pj->d;
    VkShaderModule lv = VK_NULL_HANDLE, lf = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool ok;
    if (!d.vs) d.vs = lv = module_from_cache(KVK_STAGE_VERTEX, d.vs_sid, d.vs_len);
    if (!d.fs) d.fs = lf = module_from_cache(KVK_STAGE_FRAGMENT, d.fs_sid, d.fs_len);
    ok = d.vs && d.fs && VKOK(pipe_create(&d, &pipe));
    if (lv) vkDestroyShaderModule(G.dev, lv, NULL);
    if (lf) vkDestroyShaderModule(G.dev, lf, NULL);
    pj->e->pipe = pipe;
    __atomic_store_n(&pj->e->state, ok ? 1 : 2, __ATOMIC_RELEASE);
    if (ok && pj->record) pipe_manifest_add(&pj->d, &pj->k);
    free(pj);
}

static VkPipeline pipe_get(const KvkPipeDesc *d)
{
    KvkPipeKey k;
    uint64_t h;
    unsigned int i, slot;
    KvkPipe *e = NULL;
    const bool stable = pipe_stable(d);

    pipe_key(d, &k);
    h = kvk_hash64(&k, sizeof(k));
    slot = (unsigned int)(h % KVK_PIPE_MAX);
    for (i = 0; i < KVK_PIPE_MAX; i++, slot = (slot + 1) % KVK_PIPE_MAX) {
        KvkPipe *q = &G.pipe[slot];
        if (!q->used) { e = q; break; }
        if (!memcmp(&q->key, &k, sizeof(k)))
            return __atomic_load_n(&q->state, __ATOMIC_ACQUIRE) == 1 ? q->pipe : VK_NULL_HANDLE;
    }
    if (!e) return VK_NULL_HANDLE;
    if (stable && pipe_async()) {
        KvkPipeJob *pj = (KvkPipeJob *)calloc(1, sizeof(*pj));
        if (!pj) return VK_NULL_HANDLE;
        e->used = true; e->key = k; e->state = 0; e->pipe = VK_NULL_HANDLE;
        pj->e = e; pj->d = *d; pj->k = k; pj->record = true;
        job_post(3, pj);
        return __atomic_load_n(&e->state, __ATOMIC_ACQUIRE) == 1 ? e->pipe : VK_NULL_HANDLE;
    }
    if (!VKOK(pipe_create(d, &e->pipe))) return VK_NULL_HANDLE;   /* not inserted: retried at the next use, as before */
    e->used = true;
    e->key = k;
    e->state = 1;
    if (stable) pipe_manifest_add(d, &k);
    return e->pipe;
}

static VkRenderPass rp_get(VkFormat cf, VkFormat df, uint32_t views, int cload, int dload);

/* At start (GPU thread, async mode): every pipeline of the manifest goes into the table as "being created" and onto
 * the workers, its shaders read from the SPIR-V cache. Records whose formats this device cannot render to, or whose
 * shaders are not in the cache, are left out (a draw then creates its pipeline at first use, as without a manifest). */
static void pipe_manifest_precreate(void)
{
    char path[640];
    size_t n = 0, at;
    uint8_t *buf;
    int queued = 0, skipped = 0;
    if (!pipe_async()) return;
    pipe_manifest_path(path, sizeof(path));
    if (!(buf = (uint8_t *)read_file(path, &n))) return;
    for (at = 0; at + sizeof(KvkPipeRec) <= n; at += sizeof(KvkPipeRec)) {
        KvkPipeRec r;
        KvkPipeDesc d;
        KvkPipeJob *pj;
        KvkPipe *e = NULL;
        VkFormatProperties fp;
        unsigned int i, slot;
        uint64_t h;
        memcpy(&r, buf + at, sizeof(r));
        if (r.magic != KVK_PMAN_MAGIC || !r.views || r.views > G.layers || !r.key.vs || !r.key.fs) { skipped++; continue; }
        memset(&d, 0, sizeof(d));
        pipe_desc_from_key(&r.key, &d);
        d.cf = (VkFormat)r.cf;
        d.df = r.df == KVK_FMT_THE_DEPTH ? G.depth_fmt : (VkFormat)r.df;
        d.views = r.views;
        if (d.cf != VK_FORMAT_UNDEFINED) {
            vkGetPhysicalDeviceFormatProperties(G.phys, d.cf, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) { skipped++; continue; }
        }
        if (d.df != VK_FORMAT_UNDEFINED) {
            vkGetPhysicalDeviceFormatProperties(G.phys, d.df, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) { skipped++; continue; }
        }
        if (d.cf == VK_FORMAT_UNDEFINED && d.df == VK_FORMAT_UNDEFINED) { skipped++; continue; }
        d.vs_sid = r.key.vs; d.vs_len = r.key.vlen;
        d.fs_sid = r.key.fs; d.fs_len = r.key.flen;
        if (d.vs_sid == KVK_SID_VS_CPU) d.vs = G.vs_cpu;
        if (d.fs_sid == KVK_SID_UBER) d.fs = G.ps_uber;
        if (d.fs_sid == KVK_SID_UBER_FLAT) d.fs = G.ps_uber_flat;
        d.rp = rp_get(d.cf, d.df, d.views, LOAD_LOAD, LOAD_LOAD);
        if (!d.rp) { skipped++; continue; }
        {
            KvkPipeKey k;
            pipe_key(&d, &k);                   /* this device's key (its depth format) */
            h = kvk_hash64(&k, sizeof(k));
            slot = (unsigned int)(h % KVK_PIPE_MAX);
            for (i = 0; i < KVK_PIPE_MAX; i++, slot = (slot + 1) % KVK_PIPE_MAX) {
                KvkPipe *q = &G.pipe[slot];
                if (!q->used) { e = q; break; }
                if (!memcmp(&q->key, &k, sizeof(k))) break;     /* a duplicate record */
            }
            if (!e) continue;
            if (!(pj = (KvkPipeJob *)calloc(1, sizeof(*pj)))) break;
            e->used = true; e->key = k; e->state = 0; e->pipe = VK_NULL_HANDLE;
            pj->e = e; pj->d = d; pj->k = k; pj->record = false;
            job_post(3, pj);
            queued++;
        }
    }
    free(buf);
    LOG("pipeline manifest: %d pipelines queued for creation on the workers (%d records not usable here)", queued, skipped);
}

static void pcache_save(void)
{
    size_t n = 0;
    void *p;
    char path[640];
    if (!G.pcache || !VKOK(vkGetPipelineCacheData(G.dev, G.pcache, &n, NULL)) || !n) return;
    p = malloc(n);
    if (!p) return;
    if (VKOK(vkGetPipelineCacheData(G.dev, G.pcache, &n, p))) {
        snprintf(path, sizeof(path), "%s/vk_pipeline_cache.bin", cache_dir());
        write_file(path, p, n);
    }
    free(p);
    G.pipes_at_save = G.st.pipelines;
}

/* ---- samplers and texture descriptor sets ------------------------------------------------------------------------ */

static VkSampler sampler_get(const KTexture *t)
{
    uint64_t key;
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFilter mn, mg;
    VkSamplerMipmapMode mip;
    bool lod0;
    int i;
    KvkSmp *slot = NULL;
    key = (uint64_t)(t->min_filter & 15) | ((uint64_t)(t->mag_filter & 15) << 4) |
          ((uint64_t)(t->addr[0] & 7) << 8) | ((uint64_t)(t->addr[1] & 7) << 11) |
          ((uint64_t)(t->levels & 31) << 14) | ((uint64_t)(int)(t->lod_bias * 256) << 20) |
          ((uint64_t)(unsigned)(t->min_lod * 16) << 36) | ((uint64_t)(unsigned)(t->max_lod * 16) << 44);
    key ^= (uint64_t)kvk_border_color(t->border_color) * 0x9E3779B97F4A7C15ull;
    for (i = 0; i < KVK_SMP_MAX; i++) {
        KvkSmp *s = &G.smp[i];
        if (!s->used) { if (!slot) slot = s; continue; }
        if (s->key == key) return s->s;
    }
    if (!slot) {
        static unsigned rr;
        KvkDead dd;
        slot = &G.smp[rr++ % KVK_SMP_MAX];
        memset(&dd, 0, sizeof(dd));
        dd.kind = DEAD_SAMPLER; dd.h = (uint64_t)(uintptr_t)slot->s;
        defer(&dd);
        tset_forget(VK_NULL_HANDLE, slot->s);
        slot->used = false;
    }
    kvk_filter(t->min_filter, t->mag_filter, &mn, &mg, &mip, &lod0);
    ci.magFilter = mg;
    ci.minFilter = mn;
    ci.mipmapMode = mip;
    ci.addressModeU = kvk_address_mode(t->addr[0]);
    ci.addressModeV = kvk_address_mode(t->addr[1]);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.mipLodBias = t->lod_bias;              /* |bias| <= maxSamplerLodBias (VUID-VkSamplerCreateInfo-mipLodBias-01069) */
    if (ci.mipLodBias > G.props.limits.maxSamplerLodBias) ci.mipLodBias = G.props.limits.maxSamplerLodBias;
    if (ci.mipLodBias < -G.props.limits.maxSamplerLodBias) ci.mipLodBias = -G.props.limits.maxSamplerLodBias;
    ci.maxAnisotropy = 1.0f;
    ci.compareOp = VK_COMPARE_OP_NEVER;
    ci.minLod = t->min_lod;
    ci.maxLod = lod0 ? t->min_lod : t->max_lod;
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    ci.borderColor = kvk_border_color(t->border_color);
    if (!VKOK(vkCreateSampler(G.dev, &ci, NULL, &slot->s))) return G.smp_linear;
    slot->used = true;
    slot->key = key;
    return slot->s;
}

/* A set from the frame's own pools (reset when the frame slot is reused): the fallback when the cache's pool is full. */
static VkDescriptorSet tset_alloc_frame(void)
{
    KvkFrame *f = cur_frame();
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet set = VK_NULL_HANDLE;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &G.dsl1;
    for (;;) {
        if (f->curtpool >= f->ntpool) {
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * 2048 };
            VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            if (f->ntpool >= 8) return VK_NULL_HANDLE;
            pi.maxSets = 2048;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            if (!VKOK(vkCreateDescriptorPool(G.dev, &pi, NULL, &f->tpool[f->ntpool]))) return VK_NULL_HANDLE;
            f->ntpool++;
        }
        ai.descriptorPool = f->tpool[f->curtpool];
        if (VKOK(vkAllocateDescriptorSets(G.dev, &ai, &set))) return set;
        f->curtpool++;
    }
}

static void tset_drop(KvkTset *e)
{
    KvkDead dd;
    memset(&dd, 0, sizeof(dd));
    dd.kind = DEAD_TSET; dd.h = (uint64_t)(uintptr_t)e->set;   /* frames in flight may still use it */
    defer(&dd);
    memset(e, 0, sizeof(*e));
}

/* A view or sampler is going away (its destruction is deferred until the frames using it retire, after which the
 * driver may hand its handle out again): no cached set naming it may be used from now on. */
static void tset_forget(VkImageView v, VkSampler s)
{
    unsigned int i, k;
    if (!G.tset_pool || (!v && !s)) return;
    for (i = 0; i < KVK_TSET_CACHE; i++) {
        KvkTset *e = &G.tset[i];
        if (!e->used) continue;
        for (k = 0; k < 4; k++) {
            if ((v && e->v[k] == v) || (s && e->s[k] == s)) { tset_drop(e); G.st.tset_flushes++; break; }
        }
    }
}

/* The texture descriptor set for these views and samplers. Sets are immutable once written and live in a cache that
 * survives frames (before, the cache was emptied at every frame and a set written again for nearly every draw: ~260
 * vkAllocateDescriptorSets + vkUpdateDescriptorSets per level-1 frame). An entry is dropped when a view or sampler it
 * names is released (tset_forget) or when its bucket is full (least recently used); dropped sets are freed when the
 * frame retires. */
/* RECOMP_KVK_TSET_VERIFY=1: every cache hit checks that each view and sampler of the set is still alive (owned by a
 * surface, texture, the backend or an eye view): a stale handle would mean a set outlived what it names. */
static int s_tset_verify = -1;
static uint64_t s_tset_checked, s_tset_stale;
static bool tset_view_alive(VkImageView v)
{
    unsigned int i;
    if (v == G.dummy.view || v == G.fb_copy.view) return true;
    for (i = 0; i < KVK_MAX_SURF; i++)
        if (G.surf[i].used && (G.surf[i].im.view == v || G.surf[i].im.att == v || G.surf[i].conv.view == v)) return true;
    for (i = 0; i < KVK_TEX_SLOTS; i++) if (G.tex[i].used && G.tex[i].im.view == v) return true;
    for (i = 0; i < KVK_EYEVIEW_MAX; i++) if (G.eyeview[i].view == v) return true;
    return false;
}
static bool tset_sampler_alive(VkSampler s)
{
    unsigned int i;
    if (s == G.smp_linear || s == G.smp_nearest) return true;
    for (i = 0; i < KVK_SMP_MAX; i++) if (G.smp[i].used && G.smp[i].s == s) return true;
    return false;
}
static void tset_verify_hit(const KvkTset *q)
{
    unsigned int k;
    bool ok = true;
    for (k = 0; k < 4; k++) ok = ok && tset_view_alive(q->v[k]) && tset_sampler_alive(q->s[k]);
    s_tset_checked++;
    if (!ok && s_tset_stale++ < 8) LOG("TSET verify: cached set %p names a released view or sampler", (void *)q->set);
    if (!(s_tset_checked & 0x3FFFu))
        LOG("TSET verify: %llu hits checked, %llu stale", (unsigned long long)s_tset_checked, (unsigned long long)s_tset_stale);
}

static VkDescriptorSet tset_get(const VkImageView v[4], const VkSampler s[4])
{
    uint64_t h = kvk_hash64(v, 4 * sizeof(v[0])) ^ (kvk_hash64(s, 4 * sizeof(s[0])) * 31u);
    KvkTset *b = &G.tset[(unsigned int)(h % KVK_TSET_BUCKETS) * KVK_TSET_WAYS], *e = NULL;
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool cached = false;
    unsigned int i;
    for (i = 0; i < KVK_TSET_WAYS; i++) {
        KvkTset *q = &b[i];
        if (q->used && !memcmp(q->v, v, sizeof(q->v)) && !memcmp(q->s, s, sizeof(q->s))) {
            if (s_tset_verify < 0) { const char *ev = getenv("RECOMP_KVK_TSET_VERIFY"); s_tset_verify = ev && *ev && *ev != '0'; }
            if (s_tset_verify > 0) tset_verify_hit(q);
            q->stamp = ++G.tset_stamp;
            return q->set;
        }
    }
    if (G.tset_pool) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        for (i = 0; i < KVK_TSET_WAYS; i++) {
            if (!b[i].used) { e = &b[i]; break; }
            if (!e || b[i].stamp < e->stamp) e = &b[i];
        }
        if (e->used) tset_drop(e);
        ai.descriptorPool = G.tset_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &G.dsl1;
        cached = VKOK(vkAllocateDescriptorSets(G.dev, &ai, &set));
    }
    if (!cached && !(set = tset_alloc_frame())) return VK_NULL_HANDLE;
    for (i = 0; i < 4; i++) {
        ii[i].sampler = s[i];
        ii[i].imageView = v[i];
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 4;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(G.dev, 1, &w, 0, NULL);
    G.st.tset_writes++;
    if (cached) {
        e->used = true;
        memcpy(e->v, v, sizeof(e->v));
        memcpy(e->s, s, sizeof(e->s));
        e->set = set;
        e->stamp = ++G.tset_stamp;
    }
    return set;
}

/* ---- surfaces ---------------------------------------------------------------------------------------------------- */

static bool is_main(unsigned int gw, unsigned int gh) { return gw == 640 && gh == 480; }

static void surf_free(KvkSurf *s)
{
    if (P.active && (P.cs == s || P.zs == s)) pass_end();
    img_release(&s->im);
    img_release(&s->conv);
    memset(s, 0, sizeof(*s));
    G.surf_gen++;
}

/* what a colour surface needs: drawn to with blending, sampled with linear filtering, cleared and copied */
#define KVK_SURF_FEATURES (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | \
                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

static VkFormat surf_color_format(unsigned int format, unsigned int *bpp)
{
    VkFormat f = kvk_color_format(format, bpp);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(G.phys, f, &fp);
    if ((fp.optimalTilingFeatures & KVK_SURF_FEATURES) != KVK_SURF_FEATURES) {
        *bpp = 4;
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
    return f;
}

static KvkSurf *surf_get_ex(const KelvinTarget *t, bool depth, unsigned int format, bool hud)
{
    uint32_t va = depth ? t->zeta_va : t->color_va;
    uint32_t pitch = t->swizzled ? 0 : depth ? t->guest_zeta_pitch : t->guest_color_pitch;
    unsigned int gw = t->guest_width, gh = t->guest_height, hw, hh, bpp = 4, i, layers;
    VkFormat fmt;
    KvkSurf *s = NULL, *victim = NULL;
    if (!va) return NULL;
    if (t->aa_x != 1 || t->aa_y != 1) {
        kelvin_unsupported(KU_SURFACE_FORMAT, "GPU surface with anti-aliasing", va);
        return NULL;
    }
    if (!is_main(gw, gh)) hud = false;
    if (is_main(gw, gh)) { hw = hud ? G.hud_w : G.host_w; hh = hud ? G.hud_h : G.host_h; }
    else { hw = gw; hh = gh; }
    layers = hud ? 1 : G.layers;
    {
        /* The same surface as the last call (most draws): at most one used entry has this va/depth/hud, so the
         * scan below would return this one. */
        KvkSurf *q = G.surf_last[depth];
        if (q && q->used && q->va == va && q->depth == depth && q->hud == hud && q->pitch == pitch && q->gw == gw &&
            q->gh == gh && q->format == (depth ? 0 : format) && q->swz == t->swizzled && q->hw == hw && q->hh == hh) {
            q->clock = ++G.clock;
            return q;
        }
    }
    for (i = 0; i < KVK_MAX_SURF; i++) {
        KvkSurf *q = &G.surf[i];
        if (!q->used) { if (!s) s = q; continue; }
        if (q->va == va && q->depth == depth && q->hud == hud) {
            if (q->pitch == pitch && q->gw == gw && q->gh == gh && q->format == (depth ? 0 : format) &&
                q->swz == t->swizzled && q->hw == hw && q->hh == hh) {
                q->clock = ++G.clock;
                G.surf_last[depth] = q;
                return q;
            }
            surf_free(q);
            s = q;
            break;
        }
        if (!victim || q->clock < victim->clock) victim = q;
    }
    if (!s) { surf_free(victim); s = victim; }
    fmt = depth ? G.depth_fmt : surf_color_format(format, &bpp);    /* only a new surface needs it */
    if (!img_create(&s->im, fmt, hw, hh, layers, 1,
                    depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOG("surface %ux%u create failed", hw, hh);
        memset(s, 0, sizeof(*s));
        return NULL;
    }
    s->used = true; s->depth = depth; s->hud = hud; s->swz = t->swizzled; s->va = va; s->pitch = pitch;
    s->format = depth ? 0 : format;
    s->gw = gw; s->gh = gh; s->hw = hw; s->hh = hh; s->bpp = bpp;
    s->clock = ++G.clock;
    G.surf_last[depth] = s;
    /* a fresh colour surface starts black (transparent for the HUD), depth at the far plane */
    if (depth) {
        s->pending = PC_DEPTH | PC_STENCIL;
        s->pending_val.depthStencil.depth = 1.0f;
        s->pending_val.depthStencil.stencil = 0;
        s->ver++;
    } else {
        s->pending = PC_COLOR;
        memset(&s->pending_val, 0, sizeof(s->pending_val));
    }
    G.surf_gen++;
    LOG("%s%s surface 0x%08X: guest %ux%u pitch %u -> host %ux%u x%u", hud ? "HUD " : "", depth ? "depth" : "colour",
        va, gw, gh, pitch, hw, hh, layers);
    return s;
}

static KvkSurf *surf_get(const KelvinTarget *t, bool depth, unsigned int format)
{
    return surf_get_ex(t, depth, format, V.hud);
}

/* A pending clear, now (the surface is about to be read or copied outside a pass). */
static void surf_resolve_clear(KvkSurf *s)
{
    VkImageSubresourceRange r;
    VkCommandBuffer cb = cmd();
    if (!s->pending) return;
    pass_end();
    barrier(cb, &s->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof(r));
    r.levelCount = 1;
    r.layerCount = s->im.layers;
    if (s->depth) {
        r.aspectMask = ((s->pending & PC_DEPTH) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                       ((s->pending & PC_STENCIL) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        vkCmdClearDepthStencilImage(cb, s->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &s->pending_val.depthStencil, 1, &r);
    } else {
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdClearColorImage(cb, s->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &s->pending_val.color, 1, &r);
    }
    s->pending = 0;
}

/* The surface ready to be sampled. */
static void surf_for_read(KvkSurf *s)
{
    surf_resolve_clear(s);
    if (P.active && (P.cs == s || P.zs == s)) pass_end();
    barrier(cmd(), &s->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* Whether a used surface may have this address (no false negatives): most textures are not render targets, and both
 * lookups below would otherwise scan the whole table for each texture stage of each draw. */
static unsigned int surf_va_bit(uint32_t va) { return (va * 2654435761u) >> 24; }
static bool surf_va_maybe(uint32_t va)
{
    const unsigned int b = surf_va_bit(va);
    if (G.surf_va_gen != G.surf_gen) {
        unsigned int i;
        memset(G.surf_va_bits, 0, sizeof(G.surf_va_bits));
        for (i = 0; i < KVK_MAX_SURF; i++)
            if (G.surf[i].used) { const unsigned int k = surf_va_bit(G.surf[i].va); G.surf_va_bits[k >> 6] |= 1ull << (k & 63); }
        G.surf_va_gen = G.surf_gen;
    }
    return (G.surf_va_bits[b >> 6] >> (b & 63)) & 1u;
}

static KvkSurf *surf_for_texture(const KTexture *t)
{
    unsigned int i;
    if (!surf_va_maybe(t->base)) return NULL;
    for (i = 0; i < KVK_MAX_SURF; i++) {
        KvkSurf *s = &G.surf[i];
        if (!s->used || s->depth || s->va != t->base || s->hud != (V.hud && is_main(s->gw, s->gh))) continue;
        if (t->cubemap || t->levels != 1 || t->width != s->gw || t->height != s->gh) continue;
        if (s->swz) {
            if (t->linear || t->kind != TK_8888 || s->bpp != 4) continue;
        } else if (!t->linear || t->pitch != s->pitch) {
            continue;
        }
        return s;
    }
    return NULL;
}

static KvkSurf *surf_find(uint32_t va, bool depth)
{
    unsigned int i;
    if (!surf_va_maybe(va)) return NULL;
    for (i = 0; i < KVK_MAX_SURF; i++) {
        KvkSurf *s = &G.surf[i];
        if (s->used && s->va == va && s->depth == depth && s->hud == (V.hud && is_main(s->gw, s->gh))) return s;
    }
    return NULL;
}

static KvkSurf *surf_depth_for_texture(const KTexture *t)
{
    KvkSurf *s = surf_find(t->base, true);
    if (!s || s->swz || !t->linear || t->cubemap || t->levels != 1) return NULL;
    if (t->width != s->gw || t->height != s->gh || t->pitch != s->pitch) return NULL;
    return s;
}

/* ---- render passes ------------------------------------------------------------------------------------------------ */

static void pass_end(void)
{
    if (!P.active) return;
    vkCmdEndRenderPass(cmd());
    P.active = false;
    P.cs = P.zs = NULL;
    G.st.passes++;
}

/* After vkCmdBeginRenderPass: with multiview, everything that is not render-pass state (pipeline, descriptor sets,
 * index buffer, dynamic state, push constants) is undefined at the start of each subpass, so nothing bound earlier
 * may be relied on (VUID-vkCmdDrawIndexed-None-07834 and friends; tilers really drop it). Push constants are written
 * before every draw. Done for every pass, single-view too: it costs a few binds per pass. */
static void state_forget(void)
{
    P.pipe = VK_NULL_HANDLE;
    P.set0 = P.set1 = VK_NULL_HANDLE;
    P.bound0 = false;
    P.dyn = false;
    P.ib = VK_NULL_HANDLE;
}

static bool pass_begin(KvkSurf *cs, KvkSurf *zs)
{
    VkCommandBuffer cb = cmd();
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv[2];
    VkRenderPass rp, compat;
    VkFramebuffer fb;
    int cload = LOAD_LOAD, dload = LOAD_LOAD;
    uint32_t n = 0, w, h, views;
    const KvkSurf *ref = cs ? cs : zs;
    if (P.active && P.cs == cs && P.zs == zs) return true;
    pass_end();
    if (!ref) return false;
    w = ref->hw; h = ref->hh; views = ref->im.layers;
    memset(cv, 0, sizeof(cv));
    if (cs) {
        if (cs->pending) { cload = LOAD_CLEAR; cv[n] = cs->pending_val; cs->pending = 0; }
        barrier(cb, &cs->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        n++;
    }
    if (zs) {
        if (zs->pending) {
            /* depth and stencil clear together or not at all as a load op */
            if ((zs->pending & (PC_DEPTH | PC_STENCIL)) == (PC_DEPTH | PC_STENCIL)) {
                dload = LOAD_CLEAR;
                cv[n] = zs->pending_val;
                zs->pending = 0;
            } else {
                surf_resolve_clear(zs);
            }
        }
        barrier(cb, &zs->im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        n++;
    }
    rp = rp_get(cs ? cs->im.fmt : VK_FORMAT_UNDEFINED, zs ? zs->im.fmt : VK_FORMAT_UNDEFINED, views, cload, dload);
    compat = rp_get(cs ? cs->im.fmt : VK_FORMAT_UNDEFINED, zs ? zs->im.fmt : VK_FORMAT_UNDEFINED, views, LOAD_LOAD, LOAD_LOAD);
    fb = compat ? fb_get(compat, cs ? cs->im.att : VK_NULL_HANDLE, zs ? zs->im.att : VK_NULL_HANDLE, w, h) : VK_NULL_HANDLE;
    if (!rp || !fb) return false;
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea.extent.width = w;
    bi.renderArea.extent.height = h;
    bi.clearValueCount = n;
    bi.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    state_forget();
    P.active = true;
    P.cs = cs; P.zs = zs;
    P.views = views;
    return true;
}

static VkRenderPass pass_compat(void)
{
    return rp_get(P.cs ? P.cs->im.fmt : VK_FORMAT_UNDEFINED, P.zs ? P.zs->im.fmt : VK_FORMAT_UNDEFINED, P.views,
                  LOAD_LOAD, LOAD_LOAD);
}

/* ---- per-draw buffers ---------------------------------------------------------------------------------------------- */

static VkDeviceSize stereo_block(void)
{
    KvkFrame *f = cur_frame();
    KvkChunk *c = &f->chunk[f->cur];
    if (!c->stereo) {
        VkDeviceSize off;
        uint8_t *p = ring_take(KVK_STEREO_BYTES, ualign(), &off);
        memcpy(p, V.st.eye_clip, 128);
        memcpy(p + 128, V.st.quad_mvp, 128);
        memcpy(p + 256, V.st.misc, 16);
        c->stereo = true;
        c->stereo_off = off;
    }
    return c->stereo_off;
}

/* VR gun latch: the weapon draws' block of this chunk (rewritten at the flip with the hand delta, vrl_relatch) */
static VkDeviceSize stereo_block_vm(void)
{
    KvkFrame *f = cur_frame();
    KvkChunk *c = &f->chunk[f->cur];
    if (!c->stereo_vm) {
        VkDeviceSize off;
        uint8_t *p = ring_take(KVK_STEREO_BYTES, ualign(), &off);
        memcpy(p, V.st_vm.eye_clip, 128);
        memcpy(p + 128, V.st_vm.quad_mvp, 128);
        memcpy(p + 256, V.st_vm.misc, 16);
        c->stereo_vm = true;
        c->stereo_vm_off = off;
    }
    return c->stereo_vm_off;
}

static void stereo_prepare(void)
{
    const VrFrameState *fs;
    if (V.prepared) return;
    V.prepared = true;
    if (!kvk_vr_stereo_hook(&V.st, &V.cfg, KVK_VR_PHASE_FIRST_DRAW)) {   /* the VR glue: this frame's camera */
        fs = vr_frame_state();
        kvk_stereo_frame(&V.st, &V.cfg, fs, vr_game_camera());
    }
    V.st_vm = V.st;                                 /* gun latch: no hand delta before the flip's latch */
    {
        KvkFrame *f = cur_frame();
        int i;
        for (i = 0; i < f->nchunk; i++) f->chunk[i].stereo = f->chunk[i].stereo_vm = false;
    }
}

/* Constants: a fresh copy unless the last one in this chunk is the same. */
static VkDeviceSize upload_dedup(const void *data, size_t n, void *last_val, int *last_chunk, VkDeviceSize *last_off,
                                 bool *have)
{
    KvkFrame *f = cur_frame();
    VkDeviceSize off;
    uint8_t *p;
    if (*have && *last_chunk == f->cur && !memcmp(last_val, data, n)) return *last_off;
    p = ring_take(n, ualign(), &off);
    memcpy(p, data, n);
    memcpy(last_val, data, n);
    *last_chunk = f->cur; *last_off = off; *have = true;
    return off;
}

static void bind_set0(const uint32_t offs[6])
{
    KvkFrame *f = cur_frame();
    VkDescriptorSet s0 = f->chunk[f->cur].set0;
    if (P.bound0 && P.set0 == s0 && !memcmp(P.offs, offs, sizeof(P.offs))) return;
    vkCmdBindDescriptorSets(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 1, &s0, 6, offs);
    P.set0 = s0;
    memcpy(P.offs, offs, sizeof(P.offs));
    P.bound0 = true;
}

static void bind_set1(VkDescriptorSet s1)
{
    if (P.set1 == s1) return;
    vkCmdBindDescriptorSets(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 1, 1, &s1, 0, NULL);
    P.set1 = s1;
}

static void bind_pipe(VkPipeline p)
{
    if (P.pipe == p) return;
    vkCmdBindPipeline(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    P.pipe = p;
}

static void set_dynamic(const VkViewport *vp, const VkRect2D *sc, const float bc[4], uint32_t sref, uint32_t smr,
                        uint32_t smw, float zb, float zsl)
{
    VkCommandBuffer cb = cmd();
    if (!P.dyn || memcmp(&P.vp, vp, sizeof(*vp))) { vkCmdSetViewport(cb, 0, 1, vp); P.vp = *vp; }
    if (!P.dyn || memcmp(&P.sc, sc, sizeof(*sc))) { vkCmdSetScissor(cb, 0, 1, sc); P.sc = *sc; }
    if (!P.dyn || memcmp(P.bc, bc, sizeof(P.bc))) { vkCmdSetBlendConstants(cb, bc); memcpy(P.bc, bc, sizeof(P.bc)); }
    if (!P.dyn || P.sref != sref) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sref); P.sref = sref; }
    if (!P.dyn || P.smr != smr) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smr); P.smr = smr; }
    if (!P.dyn || P.smw != smw) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smw); P.smw = smw; }
    if (!P.dyn || P.zb != zb || P.zsl != zsl) { vkCmdSetDepthBias(cb, zb, 0.0f, zsl); P.zb = zb; P.zsl = zsl; }
    P.dyn = true;
}

typedef struct KvkPush { float p0[4], p1[4]; uint32_t texlayer[4], dflags[4]; } KvkPush;

static void push(const KvkPush *pc)
{
    vkCmdPushConstants(cmd(), G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*pc), pc);
}

/* ---- textures ------------------------------------------------------------------------------------------------------ */

static void tex_free(KvkTex *e)
{
    img_release(&e->im);                    /* forgets the sets naming its view (tset_forget) */
    memset(e, 0, sizeof(*e));
}

static void tex_signature(const KTexture *t, uint32_t span, uint64_t sig[2])
{
    G.st.tex_sig_bytes += span;
    sig_bytes(t->mem, span, sig);
    if (t->kind == TK_I8 && t->palette_mem) {
        uint64_t p[2];
        sig_bytes(t->palette_mem, t->palette_len * 4, p);
        sig[0] ^= p[0] * 0x9E3779B97F4A7C15ull;
        sig[1] ^= p[1];
    }
}

/* Create or refresh the cached GPU copy of a guest texture (kelvin_gpu.c tex_get). A changed content gets a new
 * image, so draws recorded before keep the old one (it is destroyed when this frame retires), and every upload can
 * run in the frame's upload command buffer ahead of all its draws. */
static VkImageView tex_get(const KTexture *t)
{
    uint32_t span, h;
    unsigned int slot, probes, level;
    KvkTex *e = NULL;
    uint64_t sig[2];
    bool dxt = kind_is_dxt(t->kind), native = false;
    unsigned int levels = t->levels ? t->levels : 1;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize total = 0, off, at[KTEX_MAX_LEVELS];
    VkBufferImageCopy reg[KTEX_MAX_LEVELS];
    VkCommandBuffer ucb;
    KvkFrame *f;
    uint8_t *p;

    if (!t->valid || !t->mem || t->cubemap || !t->width || !t->height) return VK_NULL_HANDLE;
    span = texture_span(t);
    h = (t->base * 2654435761u) ^ (t->color_format * 40503u) ^ (t->width * 977u) ^ (t->height * 131u);
    /* Mixed down before the modulo: texture addresses are 4 KiB aligned, so the product above has zero low bits and
     * the home slot depended only on format and size -- long probe runs for every draw (perf.md section 11). Where an
     * entry lives does not change what a lookup returns. */
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    for (probes = 0, slot = h % KVK_TEX_SLOTS; probes < KVK_TEX_SLOTS; probes++, slot = (slot + 1) % KVK_TEX_SLOTS) {
        KvkTex *q = &G.tex[slot];
        if (!q->used) { if (!e) e = q; break; }
        if (q->base == t->base && q->fmt == t->color_format && q->w == t->width && q->h == t->height &&
            q->pitch == t->pitch && q->levels == levels && q->palette == t->palette) { e = q; break; }
    }
    if (!e) return VK_NULL_HANDLE;
    if (e->used) {
        if (e->epoch == G.epoch) { e->clock = G.clock; return e->im.view; }
        tex_signature(t, span, sig);
        e->epoch = G.epoch;
        e->clock = G.clock;
        if (sig[0] == e->sig[0] && sig[1] == e->sig[1] && e->span == span) return e->im.view;
        tex_free(e);
    } else {
        tex_signature(t, span, sig);
    }
    if (dxt && s_dev.texture_compression_bc && t->width % 4 == 0 && t->height % 4 == 0) {
        native = true;
        fmt = t->kind == TK_DXT1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : t->kind == TK_DXT3 ? VK_FORMAT_BC2_UNORM_BLOCK
                                                                                    : VK_FORMAT_BC3_UNORM_BLOCK;
    }
    for (level = 0; level < levels; level++) {
        size_t lw = t->level_w[level], lh = t->level_h[level];
        size_t bytes = native ? ((lw + 3) / 4) * ((lh + 3) / 4) * (t->kind == TK_DXT1 ? 8 : 16) : lw * lh * 4;
        at[level] = total;
        total += (bytes + 15) & ~(size_t)15;
    }
    if (!ring_reserve(total + 64)) return VK_NULL_HANDLE;
    p = ring_take(total, 16, &off);
    for (level = 0; level < levels; level++) {
        uint8_t *dst = p + at[level];
        if (native) {
            unsigned int bw = (t->level_w[level] + 3) / 4, bh = (t->level_h[level] + 3) / 4;
            memcpy(dst, t->mem + t->level_offset[level], (size_t)bw * bh * (t->kind == TK_DXT1 ? 8 : 16));
        } else if (dxt) {
            decode_dxt_level(t, level, (uint32_t *)dst);
        } else {
            decode_level(t, level, (uint32_t *)dst);
        }
        memset(&reg[level], 0, sizeof(reg[level]));
        reg[level].bufferOffset = off + at[level];
        reg[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        reg[level].imageSubresource.mipLevel = level;
        reg[level].imageSubresource.layerCount = 1;
        reg[level].imageExtent.width = t->level_w[level];
        reg[level].imageExtent.height = t->level_h[level];
        reg[level].imageExtent.depth = 1;
    }
    if (!img_create(&e->im, fmt, t->width, t->height, 1, levels, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        static int said;
        if (said++ < 8) LOG("texture 0x%08X fmt 0x%02X %ux%u create failed", t->base, t->color_format, t->width, t->height);
        return VK_NULL_HANDLE;
    }
    f = cur_frame();
    ucb = upload_cmd();
    barrier(ucb, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(ucb, f->chunk[f->cur].b.buf, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, reg);
    barrier(ucb, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    e->used = true;
    e->base = t->base; e->fmt = t->color_format; e->w = t->width; e->h = t->height;
    e->pitch = t->pitch; e->levels = levels; e->palette = t->palette; e->palette_len = t->palette_len;
    e->span = span; e->sig[0] = sig[0]; e->sig[1] = sig[1];
    e->epoch = G.epoch; e->clock = G.clock;
    e->bytes = (size_t)total;
    G.st.tex_uploads++;
    return e->im.view;
}

/* ---- internal passes ---------------------------------------------------------------------------------------------- */

static VkPipeline fixed_pipe(VkShaderModule vs, VkShaderModule fs, VkRenderPass compat, bool color, bool zwrite,
                             int topo, uint32_t blend)
{
    KvkPipeDesc d;
    memset(&d, 0, sizeof(d));
    d.vs = vs; d.fs = fs; d.rp = compat;
    d.has_color = color;
    d.blend = blend;
    d.ztest = zwrite; d.zwrite = zwrite; d.zfunc = 7;
    d.topo = topo;
    d.cull = VK_CULL_MODE_NONE;
    return pipe_get(&d);
}

static uint32_t blend_none(VkColorComponentFlags mask)
{
    return pack_blend(false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                      VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, mask);
}

/* A full-target triangle in the open pass, with this pipeline, textures and push constants. */
static void fullscreen_draw(VkPipeline pipe, VkDescriptorSet set1, const KvkPush *pc, uint32_t w, uint32_t h)
{
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    static const float bc[4] = { 0, 0, 0, 0 };
    uint32_t offs[6] = { 0, 0, 0, 0, 0, 0 };
    if (!ring_reserve(KVK_STEREO_BYTES + 256)) return;
    offs[5] = (uint32_t)stereo_block();
    bind_pipe(pipe);
    bind_set0(offs);
    if (set1) bind_set1(set1);
    set_dynamic(&vp, &sc, bc, 0, 0xFF, 0xFF, 0.0f, 0.0f);
    push(pc);
    vkCmdDraw(cmd(), 3, 1, 0, 0);
}

static VkDescriptorSet tset_single(VkImageView v, VkSampler s)
{
    VkImageView vv[4] = { v, G.dummy.view, G.dummy.view, G.dummy.view };
    VkSampler ss[4] = { s, G.smp_nearest, G.smp_nearest, G.smp_nearest };
    return tset_get(vv, ss);
}

/* The depth buffer as an RGBA8 texture of packed Z24S8 words (kelvin_gpu.c depth_as_color). */
static VkImageView depth_as_color(KvkSurf *d)
{
    KvkSurf *keep_cs = P.cs, *keep_zs = P.zs;
    VkRenderPass rp, compat;
    VkFramebuffer fb;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    KvkPush pc;
    VkPipeline pipe;
    VkDescriptorSet s1;
    (void)keep_cs; (void)keep_zs;
    if (!d->conv.img) {
        if (!img_create(&d->conv, VK_FORMAT_R8G8B8A8_UNORM, d->hw, d->hh, d->im.layers, 1,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return VK_NULL_HANDLE;
        d->conv_ver = ~(uint64_t)0;
    }
    if (d->conv_ver != d->ver) {
        surf_for_read(d);
        pass_end();
        barrier(cmd(), &d->conv, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rp = rp_get(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, d->conv.layers, LOAD_DONTCARE, LOAD_LOAD);
        compat = rp_get(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, d->conv.layers, LOAD_LOAD, LOAD_LOAD);
        fb = fb_get(compat, d->conv.att, VK_NULL_HANDLE, d->hw, d->hh);
        pipe = fixed_pipe(G.fix[KVK_FIX_FULLSCREEN_VS], G.fix[KVK_FIX_D2C_PS], compat, true, false, RS_LIST, blend_none(15));
        s1 = tset_single(d->im.view, G.smp_nearest);
        if (!rp || !fb || !pipe || !s1) return VK_NULL_HANDLE;
        bi.renderPass = rp;
        bi.framebuffer = fb;
        bi.renderArea.extent.width = d->hw;
        bi.renderArea.extent.height = d->hh;
        vkCmdBeginRenderPass(cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        state_forget();
        memset(&pc, 0, sizeof(pc));
        fullscreen_draw(pipe, s1, &pc, d->hw, d->hh);
        vkCmdEndRenderPass(cmd());
        barrier(cmd(), &d->conv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        d->conv_ver = d->ver;
    } else if (d->conv.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        pass_end();
        barrier(cmd(), &d->conv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return d->conv.view;
}

/* A colour surface's packed Z24S8 words as the contents of a depth surface (kelvin_gpu.c depth_from_color). */
static void depth_from_color(KvkSurf *z, KvkSurf *c)
{
    VkRenderPass rp, compat;
    VkFramebuffer fb;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkPipeline pipe;
    VkDescriptorSet s1;
    KvkPush pc;
    if (z->im.layers != c->im.layers || z->hw != c->hw || z->hh != c->hh) return;
    surf_for_read(c);
    pass_end();
    z->pending = 0;
    barrier(cmd(), &z->im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    rp = rp_get(VK_FORMAT_UNDEFINED, z->im.fmt, z->im.layers, LOAD_LOAD, LOAD_LOAD);
    compat = rp;
    fb = rp ? fb_get(compat, VK_NULL_HANDLE, z->im.att, z->hw, z->hh) : VK_NULL_HANDLE;
    pipe = rp ? fixed_pipe(G.fix[KVK_FIX_FULLSCREEN_VS], G.fix[KVK_FIX_C2D_PS], compat, false, true, RS_LIST, 0) : VK_NULL_HANDLE;
    s1 = tset_single(c->im.view, G.smp_nearest);
    if (!fb || !pipe || !s1) return;
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea.extent.width = z->hw;
    bi.renderArea.extent.height = z->hh;
    vkCmdBeginRenderPass(cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
    state_forget();
    memset(&pc, 0, sizeof(pc));
    fullscreen_draw(pipe, s1, &pc, z->hw, z->hh);
    vkCmdEndRenderPass(cmd());
    z->src_ver = c->ver;
    z->ver++;
    z->wseq = ++G.wseq;
}

static void depth_surface_refresh(KvkSurf *z)
{
    KvkSurf *c = surf_find(z->va, false);
    if (!c || c->gw != z->gw || c->gh != z->gh) { z->from_color = false; return; }
    if (c->wseq > z->wseq) {
        z->from_color = true;
        depth_from_color(z, c);
    }
}

/* A copy of the target's pre-draw contents, for a draw that samples what it draws to. */
static VkImageView feedback_copy(KvkSurf *s)
{
    VkImageCopy r;
    VkCommandBuffer cb;
    if (G.fb_copy.img && (G.fb_copy.w != s->hw || G.fb_copy.h != s->hh || G.fb_copy.fmt != s->im.fmt ||
                          G.fb_copy.layers != s->im.layers)) img_release(&G.fb_copy);
    if (!G.fb_copy.img && !img_create(&G.fb_copy, s->im.fmt, s->hw, s->hh, s->im.layers, 1,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_NULL_HANDLE;
    surf_resolve_clear(s);
    pass_end();
    cb = cmd();
    barrier(cb, &s->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, &G.fb_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof(r));
    r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.srcSubresource.layerCount = r.dstSubresource.layerCount = s->im.layers;
    r.extent.width = s->hw; r.extent.height = s->hh; r.extent.depth = 1;
    vkCmdCopyImage(cb, s->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.fb_copy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(cb, &G.fb_copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return G.fb_copy.view;
}

/* ---- draws ---------------------------------------------------------------------------------------------------------- */

static float viewmodel_scale(uint32_t vs_hash)
{
    int i, n = s_vm_count;
    for (i = 0; i < n; i++) if (s_vm_hash[i] == vs_hash) return s_vm_scale;
    return 1.0f;
}

/* CPU-shaded vertices are perspective when their w varies (a pre-transformed overlay has w = 1). */
static bool cpu_w_perspective(const KGpuDraw *d)
{
    uint32_t i;
    if (d->slot[KG_POS] < 0) return false;
    for (i = 0; i < d->vcount; i++) {
        float w = d->recs[((size_t)i * d->stride + (size_t)d->slot[KG_POS]) * 4 + 3];
        if (fabsf(w - 1.0f) > 1e-5f) return true;
    }
    return false;
}

/* ---- VR: screen-space effects (port/notes/renderer.md section 11) ---------------------------------------------------
 * BLACK projects its effects on the CPU and draws them in screen space: smoke, dust, sparks, fire, explosions, glows
 * and flash halos are camera-facing quads made by programs such as oPos = v0 * c98 + c97 (v0 = (x/640, y/480, 1/w, 1)),
 * so oPos.w = 1 and the depth, when there is one, is only in z. Neither kvk_vsh_pos_class nor cpu_w_perspective calls
 * them perspective. Left alone, both eye layers get the game camera's picture of such a draw at the same pixel -- a
 * different direction in each eye, and tested against a depth buffer that is not the game camera's, so the effect
 * shows in one eye. Every non-perspective draw of a stereo frame's world pass is therefore corrected like a world
 * vertex (KVK_DF_SCREEN_DEPTH: kvk_glsl.c rebuilds w from z through the depth row; without a usable z, at half the far
 * distance with its depth value kept), except full-screen passes (fades, flashes, colour filters, glare composites),
 * which stay pixel-aligned per eye. The HUD (hud_starts) is never in the world pass. */

/* One attribute element as the vertex stage fetches it (kvk_glsl.c FetchAttr), from guest memory. */
static void np_attr(const KGpuAttrSrc *as, uint32_t e, float r[4])
{
    const int64_t at = (int64_t)e * as->stride;
    const uint8_t *q;
    uint32_t i, w;
    r[0] = r[1] = r[2] = 0.0f; r[3] = 1.0f;
    if (!as->fmt) { memcpy(r, as->inline_value, 16); return; }
    if (!as->data || at < as->lo || at + 4 > as->hi + 1) { r[3] = 0.0f; return; }
    q = as->data + at;
    switch (as->fmt) {
    case 1: memcpy(&w, q, 4); r[0] = ((w >> 16) & 255u) / 255.0f; r[1] = ((w >> 8) & 255u) / 255.0f;
            r[2] = (w & 255u) / 255.0f; r[3] = (w >> 24) / 255.0f; break;
    case 2: for (i = 0; i < 4 && i < as->count; i++) r[i] = q[i] / 255.0f; break;
    case 3: for (i = 0; i < 4 && i < as->count && at + 2 * i + 2 <= as->hi + 1; i++) {
                int16_t v; memcpy(&v, q + 2 * i, 2); r[i] = v / 32767.0f; if (r[i] < -1.0f) r[i] = -1.0f; } break;
    case 4: for (i = 0; i < 4 && i < as->count && at + 4 * i + 4 <= as->hi + 1; i++) memcpy(&r[i], q + 4 * i, 4); break;
    case 5: for (i = 0; i < 4 && i < as->count && at + 2 * i + 2 <= as->hi + 1; i++) {
                int16_t v; memcpy(&v, q + 2 * i, 2); r[i] = v; } break;
    case 6: memcpy(&w, q, 4);
            r[0] = (float)((int32_t)(w << 21) >> 21) / 1023.0f; r[1] = (float)((int32_t)((w >> 11) << 21) >> 21) / 1023.0f;
            r[2] = (float)((int32_t)w >> 22) / 511.0f; break;
    default: break;
    }
}

/* What a non-perspective draw covers: its oPos (guest pixels x, y; z in zmax units) over a sample of its triangles. */
typedef struct NpExt { float x0, y0, x1, y1, z0, z1, area, tri_max; uint32_t n; } NpExt;
#define NP_MAX_TRIS 32

/* The reference evaluation of one element's oPos: a fresh, zeroed emulator state with the draw's 192 constants
 * copied in and all 16 inputs fetched (what np_extent did per vertex before the fast path; RECOMP_KVK_NP_VERIFY). */
static bool np_pos_ref(const KGpuDraw *d, uint32_t e, float o[4])
{
    if (!d->gvs) {
        const float *p;
        if (d->slot[KG_POS] < 0 || e >= d->vcount) return false;
        p = &d->recs[((size_t)e * d->stride + (size_t)d->slot[KG_POS]) * 4];
        memcpy(o, p, 16);
        return true;
    } else {
        Nv2aVshCPUFullExecutionState fs;
        Nv2aVshExecutionState st;
        int a;
        memset(&fs, 0, sizeof(fs));
        st = nv2a_vsh_emu_initialize_full_execution_state(&fs);
        for (a = 0; a < 16; a++) np_attr(&d->gvs->attr[a], e, &fs.input_regs[a * 4]);
        memcpy(fs.context_regs, d->gvs->constants, sizeof(fs.context_regs));
        nv2a_vsh_emu_execute(&st, (const Nv2aVshProgram *)d->gvs->program);
        memcpy(o, &fs.output_regs[0], 16);
        return isfinite(o[0]) && isfinite(o[1]);
    }
}

/* One draw's evaluator for np_extent (port/notes/perf.md section 11). The reference above clears 4.3 KB of emulator
 * state, copies the 3 KB of constants and fetches all 16 inputs for every vertex. For a program that never writes a
 * constant and never reads one relatively (so every constant it reads is inside c0..c191), the emulator reads the
 * constants in place; per vertex only the registers a program can read before writing them are reset to the
 * reference's zeros (temporaries, outputs, a0), and only the inputs it reads are fetched -- the same values in, the
 * same operations in the same order, so the same oPos bit for bit. Any other program takes the reference path. An
 * element's result is reused within the draw (indexed lists name a quad's corners twice). */
#define NP_MEMO 64
typedef struct NpEval {
    const KGpuDraw *d;
    bool fast;
    uint32_t inputs;                         /* bit a: the program reads v<a> */
    Nv2aVshCPUFullExecutionState fs;         /* fast path: input/output/temp/a0 only (context_regs unused) */
    Nv2aVshExecutionState st;
    uint32_t memo_e[NP_MEMO];
    float memo_o[NP_MEMO][4];
    bool memo_ok[NP_MEMO], memo_used[NP_MEMO];
} NpEval;

static int s_np_verify = -1;                /* RECOMP_KVK_NP_VERIFY=1: every fast result checked against the reference */
static uint64_t s_np_verified, s_np_mismatch;

static void np_eval_init(NpEval *ev, const KGpuDraw *d)
{
    const Nv2aVshStep *step;
    ev->d = d;
    ev->fast = false;
    ev->inputs = 0;
    memset(ev->memo_used, 0, sizeof(ev->memo_used));
    if (s_np_verify < 0) { const char *e = getenv("RECOMP_KVK_NP_VERIFY"); s_np_verify = e && *e && *e != '0'; }
    if (!d->gvs || !d->gvs->program || !((const Nv2aVshProgram *)d->gvs->program)->steps) return;
    ev->fast = true;
    for (step = ((const Nv2aVshProgram *)d->gvs->program)->steps;; step++) {
        const Nv2aVshOperation *ops[2];
        int o, k;
        ops[0] = &step->mac; ops[1] = &step->ilu;
        for (o = 0; o < 2; o++) {
            if (!ops[o]->opcode) continue;
            for (k = 0; k < 2; k++) if (ops[o]->outputs[k].type == NV2ART_CONTEXT) ev->fast = false;
            for (k = 0; k < 3; k++) {
                const Nv2aVshInput *in = &ops[o]->inputs[k];
                if (o == 1 && k > 0) break;                          /* the ILU reads one input */
                if (in->type == NV2ART_NONE) { if (o == 0) break; continue; }
                if (in->type == NV2ART_CONTEXT && (in->is_relative || in->index >= 192)) ev->fast = false;
                if (in->type == NV2ART_INPUT) ev->inputs |= 1u << (in->index & 15);
            }
        }
        if (step->is_final) break;
    }
    if (ev->fast) {
        ev->st = nv2a_vsh_emu_initialize_full_execution_state(&ev->fs);
        ev->st.context_regs = (float *)(uintptr_t)d->gvs->constants;   /* read only: no CONTEXT output (above) */
    }
}

static bool np_eval(NpEval *ev, uint32_t e, float o[4])
{
    const KGpuDraw *d = ev->d;
    const unsigned m = (e ^ (e >> 6)) & (NP_MEMO - 1);
    bool ok;
    if (!d->gvs) return np_pos_ref(d, e, o);
    if (ev->memo_used[m] && ev->memo_e[m] == e) {
        memcpy(o, ev->memo_o[m], 16);
        G.st.np_cache_hits++;
        return ev->memo_ok[m];
    }
    G.st.np_vertices++;
    if (ev->fast) {
        uint32_t in = ev->inputs;
        memset(ev->fs.output_regs, 0, sizeof(ev->fs.output_regs));
        memset(ev->fs.temp_regs, 0, sizeof(ev->fs.temp_regs));
        memset(ev->fs.address_reg, 0, sizeof(ev->fs.address_reg));
        while (in) {
            const int a = __builtin_ctz(in);
            in &= in - 1;
            np_attr(&d->gvs->attr[a], e, &ev->fs.input_regs[a * 4]);
        }
        nv2a_vsh_emu_execute(&ev->st, (const Nv2aVshProgram *)d->gvs->program);
        memcpy(o, &ev->fs.output_regs[0], 16);
        ok = isfinite(o[0]) && isfinite(o[1]);
        if (s_np_verify > 0) {
            float r[4];
            const bool rok = np_pos_ref(d, e, r);
            s_np_verified++;
            if (rok != ok || memcmp(r, o, 16)) {
                if (s_np_mismatch++ < 8)
                    LOG("NP verify: vs %08X element %u: fast %d (%g %g %g %g) reference %d (%g %g %g %g)", d->gvs->hash, e,
                        ok, (double)o[0], (double)o[1], (double)o[2], (double)o[3], rok, (double)r[0], (double)r[1],
                        (double)r[2], (double)r[3]);
            }
            if (!(s_np_verified & 0xFFFFu))
                LOG("NP verify: %llu vertices checked, %llu mismatches", (unsigned long long)s_np_verified,
                    (unsigned long long)s_np_mismatch);
        }
    } else {
        ok = np_pos_ref(d, e, o);
    }
    ev->memo_used[m] = true; ev->memo_e[m] = e; ev->memo_ok[m] = ok;
    memcpy(ev->memo_o[m], o, 16);
    return ok;
}

static bool np_extent(const KGpuDraw *d, NpExt *x)
{
    const bool strip = d->topology == KG_TOPO_STRIP;
    uint32_t i, ntri = 0, tri_total, k;
    float v[3][4];
    int have = 0;
    static NpEval ev;                                                /* pusher thread only */
    memset(x, 0, sizeof(*x));
    x->x0 = x->y0 = x->z0 = 1e30f; x->x1 = x->y1 = x->z1 = -1e30f;
    if (!d->indices || d->icount < 3 || (d->gvs && !d->gvs->constants)) return false;
    tri_total = strip ? d->icount - 2 : d->icount / 3;
    np_eval_init(&ev, d);
    for (i = 0; i < d->icount && ntri < NP_MAX_TRIS; i++) {
        const uint32_t e = d->indices[i];
        float o[4];
        if (e == 0xFFFFFFFFu) { have = 0; continue; }                 /* strip restart */
        if (!np_eval(&ev, e, o)) return false;
        if (o[0] < x->x0) x->x0 = o[0];
        if (o[0] > x->x1) x->x1 = o[0];
        if (o[1] < x->y0) x->y0 = o[1];
        if (o[1] > x->y1) x->y1 = o[1];
        if (o[2] < x->z0) x->z0 = o[2];
        if (o[2] > x->z1) x->z1 = o[2];
        x->n++;
        if (strip) { if (have == 3) { memcpy(v[0], v[1], 16); memcpy(v[1], v[2], 16); have = 2; } }
        memcpy(v[have++], o, 16);
        if (have == 3) {
            const float ta = 0.5f * fabsf((v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]));
            x->area += ta;
            if (ta > x->tri_max) x->tri_max = ta;
            ntri++;
            if (!strip) have = 0;
        }
    }
    if (!ntri) return false;
    k = tri_total > ntri ? tri_total : ntri;
    x->area *= (float)k / (float)ntri;
    return true;
}

/* A full-screen pass (fade, flash, colour filter, glare composite or blur step): it reaches all four edges of the
 * target (within a pixel) and covers it -- one triangle of at least 45 % of it (a quad's half, or the one big
 * triangle such passes use), or a mesh at one depth whose triangles sum to 98 % of it. A large effect that does not
 * reach every edge (a light shaft over a fire, an explosion filling most of the view) is corrected. */
static bool np_fullscreen(const NpExt *x, const KelvinTarget *t)
{
    const float w = (float)t->guest_width, h = (float)t->guest_height;
    if (!(x->x0 <= 1.0f && x->y0 <= 1.0f && x->x1 >= w - 1.0f && x->y1 >= h - 1.0f)) return false;
    return x->tri_max >= 0.45f * w * h || (x->area >= 0.98f * w * h && x->z1 - x->z0 <= 1.0f);
}

/* RECOMP_KVK_SPRITELOG=1: one line per non-perspective world-pass draw of a stereo frame (what screen_effect decided);
 * 2: also the blended perspective ones (persp_fx_log) */
static int s_spritelog = -1;
static bool spritelog(void)
{
    if (s_spritelog < 0) { const char *e = getenv("RECOMP_KVK_SPRITELOG"); s_spritelog = e && *e ? atoi(e) : 0; }
    return s_spritelog > 0;
}

/* RECOMP_KVK_SPRITELOG=2 also logs every blended perspective world-pass draw on the main target: where it lands in the
 * game camera's picture (x/w, y/w in guest pixels over a sample of its vertices), its view depth w, the program and the
 * constant registers its position reads, and whether the gun latch took it (vm). Finds effects drawn through a matrix. */
/* A guest pixel (x, y) at view depth w through the frame's preliminary eye transforms (FROM_W form, as kvk_finish):
 * each eye's NDC (x right, y up). Diagnostics only. */
static void eye_ndc(float px, float py, float w, const KelvinTarget *t, float e0[2], float e1[2])
{
    float v[4], c[4];
    int e;
    v[0] = (px * 2.0f / (float)t->guest_width - 1.0f) * w;
    v[1] = (1.0f - py * 2.0f / (float)t->guest_height) * w;
    v[2] = w; v[3] = 1.0f;
    for (e = 0; e < 2; e++) {
        float *o = e ? e1 : e0;
        kvk_mat4_apply(c, V.st.eye_clip[e], v);
        o[0] = c[3] != 0.0f ? c[0] / c[3] : 0.0f;
        o[1] = c[3] != 0.0f ? c[1] / c[3] : 0.0f;
    }
}

static void persp_fx_log(const KGpuDraw *d, const KvkVs *ve, bool vm)
{
    const KPixelState *ps = d->ps;
    static NpEval s_ev;                                              /* pusher thread only */
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f, e0[2], e1[2];
    char regs[64] = "";
    uint32_t i, n = 0;
    int r, k = 0;
    if (!d->indices || !d->icount || (d->gvs && !d->gvs->constants)) return;
    np_eval_init(&s_ev, d);
    /* the program's oPos is screen space: x, y in guest pixels, w the view depth */
    for (i = 0; i < d->icount && i < 3u * NP_MAX_TRIS; i++) {
        float o[4];
        if (d->indices[i] == 0xFFFFFFFFu || !np_eval(&s_ev, d->indices[i], o) || !(o[3] > 0.0f)) continue;
        if (o[0] < x0) x0 = o[0];
        if (o[0] > x1) x1 = o[0];
        if (o[1] < y0) y0 = o[1];
        if (o[1] > y1) y1 = o[1];
        if (o[3] < w0) w0 = o[3];
        if (o[3] > w1) w1 = o[3];
        n++;
    }
    if (!n) return;
    eye_ndc(0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (w0 + w1), d->t, e0, e1);
    for (r = 0; ve && r < 192 && k < 56; r++) {
        int e2 = r;
        if (!(ve->pos_consts[r >> 5] & (1u << (r & 31)))) continue;
        while (e2 + 1 < 192 && (ve->pos_consts[(e2 + 1) >> 5] & (1u << ((e2 + 1) & 31)))) e2++;
        k += snprintf(regs + k, sizeof(regs) - (size_t)k, "%s%d-%d", k ? "," : "", r, e2);
        r = e2;
    }
    LOG("PFX f %llu vs %08X ic %u vm %d pos %s | px x %.1f..%.1f y %.1f..%.1f w %.4g..%.4g (%u v) | z t%d w%d f%d blend %d %d %d"
        " | eyeL %.3f %.3f eyeR %.3f %.3f",
        (unsigned long long)G.st.frames, d->gvs ? d->gvs->hash : 0u, d->icount, vm, regs, (double)x0, (double)x1, (double)y0, (double)y1,
        (double)w0, (double)w1, n, ps->depth_test, ps->depth_write, ps->depth_func, ps->blend, ps->blend_sfactor,
        ps->blend_dfactor, (double)e0[0], (double)e0[1], (double)e1[0], (double)e1[1]);
}

static bool screen_effect(const KGpuDraw *d, const KvkVs *ve, bool *fullscreen)
{
    const KPixelState *ps = d->ps;
    NpExt x;
    bool fs, ok;
    *fullscreen = false;
    ok = np_extent(d, &x);
    fs = ok && np_fullscreen(&x, d->t);
    if (spritelog()) {
        const float zm = ps->zmax > 0.0f ? ps->zmax : 16777215.0f;
        float e0[2] = { 0, 0 }, e1[2] = { 0, 0 }, vw = 0.0f;
        if (ok && s_spritelog >= 2) {
            /* where the extent's centre lands in each eye (eye NDC, y up), as kvk_finish places it */
            const float zn = 0.5f * (x.z0 + x.z1) / zm, A = V.st.misc[2], B = V.st.misc[3];
            vw = B / (zn - A);
            if (!(zn > 0.0f && vw > 0.0f && isfinite(vw))) vw = 0.5f * B / (1.0f - A);
            eye_ndc(0.5f * (x.x0 + x.x1), 0.5f * (x.y0 + x.y1), vw, d->t, e0, e1);
        }
        LOG("SPRITE f %llu vs %08X ic %u -> %s | x %.1f..%.1f y %.1f..%.1f z %.6g..%.6g area %.0f | z t%d w%d f%d blend %d %d %d"
            " | w %.3g eyeL %.3f %.3f eyeR %.3f %.3f",
            (unsigned long long)G.st.frames, d->gvs ? d->gvs->hash : 0u, d->icount, !ok ? "unknown (left)" : fs ? "full-screen (left)" : "corrected",
            (double)x.x0, (double)x.x1, (double)x.y0, (double)x.y1, (double)(x.z0 / zm),
            (double)(x.z1 / zm), (double)x.area, ps->depth_test, ps->depth_write,
            ps->depth_func, ps->blend, ps->blend_sfactor, ps->blend_dfactor, (double)vw, (double)e0[0], (double)e0[1],
            (double)e1[0], (double)e1[1]);
    }
    (void)ve;
    *fullscreen = fs;
    return ok && !fs;
}

/* An offscreen target that is an image of the camera's view at a lower resolution: BLACK's glare chain (sources and
 * fire/smoke sprites drawn into 160x120 and 80x60, blurred into streaks, added back over the main target full-screen:
 * the light shafts over fires). 4:3 like the main target and smaller; shadow maps and other square or odd targets
 * are not. Its draws of a stereo frame's world pass (before the HUD) get the eye correction like the main target's,
 * so each eye layer holds that eye's glare; its blur and copy steps are full-screen passes and stay pixel-aligned. */
static bool eye_aligned_target(const KvkSurf *ref)
{
    return ref && !ref->depth && !is_main(ref->gw, ref->gh) && ref->gw * 3u == ref->gh * 4u && ref->gw >= 32u &&
           ref->gw < 640u && ref->im.layers > 1;
}

/* The world pass of a stereo frame: the glue says STEREO, or (no glue) a world draw came first. */
static bool stereo_world_pass(void)
{
    const int fm = kvk_vr_frame_mode();
    return fm == VR_PRESENT_STEREO || (fm < 0 && V.stereo_draws > 0);
}

static bool hud_starts(const KGpuDraw *d)
{
    const KPixelState *ps = d->ps;
    if (G.layers < 2 || V.hud || V.region == REGION_WORLD) return false;
    if (!d->t->color || !is_main(d->t->guest_width, d->t->guest_height)) return false;
    if (V.region == REGION_SCREEN) return true;
    /* kelvin_gpu_post.inc's rule: after the world, the HUD and menus test depth, always pass, write nothing */
    return V.world_draws >= 4 && ps->depth_test && ps->depth_func == 7 && !ps->depth_write;
}

/* The title's aiming dot: a small HUD draw at the centre of the 640x480 screen (kelvin_gpu_draw.inc kg_hud_is_reticle,
 * the same test on the same KGpuDraw). */
static bool hud_is_reticle(const KGpuDraw *d)
{
    const KGpuAttrSrc *as;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    uint32_t i;
    if (!d->gvs || !d->indices || !d->icount || d->icount > 24) return false;
    as = &d->gvs->attr[0];
    if (as->fmt != 4 || as->count < 2 || !as->data || !as->stride) return false;
    for (i = 0; i < d->icount; i++) {
        const int64_t at = (int64_t)d->indices[i] * as->stride;
        const float *p;
        if (d->indices[i] == 0xFFFFFFFFu) continue;
        if (at < as->lo || at + 8 > as->hi + 1) return false;
        p = (const float *)(as->data + at);
        if (p[0] < x0) x0 = p[0];
        if (p[0] > x1) x1 = p[0];
        if (p[1] < y0) y0 = p[1];
        if (p[1] > y1) y1 = p[1];
    }
    return x1 > x0 && y1 > y0 && x0 >= 306.0f && x1 <= 334.0f && y0 >= 226.0f && y1 <= 254.0f;
}

/* VR hand aiming in a stereo frame: the player aims with the controller, so the game's dot in the middle of the
 * head-locked HUD is wrong -- hidden, as the D3D11 backend hides it under the PC crosshair (kelvin_gpu_draw.inc,
 * pc_crosshair_replaces_dot). vr_game_config() is the shell's (weak NULL default above: no shell, no hiding). */
static bool vr_hides_aim_dot(void)
{
    const VrGameConfig *gc;
    int fm;
    if (G.layers < 2) return false;
    fm = kvk_vr_frame_mode();
    if (fm >= 0 && fm != VR_PRESENT_STEREO) return false;
    gc = vr_game_config();
    return gc && gc->aim_mode == VR_AIM_HAND;
}

/* ---- VR gun latch: debug tint and the per-draw log (port/notes/gun-latch.md) ---------------------------------------- */

/* BLACKVR_SPRITE_DEBUG=1 (blackvr.ini "env BLACKVR_SPRITE_DEBUG=1"): tint the screen-space effect classification */
static bool sprite_debug(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("BLACKVR_SPRITE_DEBUG"); on = e && *e && *e != '0'; }
    return on != 0;
}

/* BLACKVR_GUN_LATCH_DEBUG (the glue's kvk_vr_viewmodel_tint): the draw again, flat translucent colour, depth-tested
 * against itself, so the classified draws show. A debug aid only (synchronous pipeline, not in the manifest). */
static void draw_tint(const KvkPipeDesc *pd0, const KvkPush *pc0, uint32_t icount, uint32_t first, uint32_t c)
{
    KvkPipeDesc pd;
    KvkPush pc;
    VkPipeline pipe;
    if (!c || !pd0->has_color) return;
    pd = *pd0;
    pd.fs = G.fix[KVK_FIX_CLEAR_COLOR_PS];
    pd.vs_sid = pd.fs_sid = 0; pd.vs_len = pd.fs_len = 0;       /* keyed by handle, created now */
    pd.blend = pack_blend(true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                          VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 7);
    pd.zwrite = false;
    if (pd.zfunc == 1) pd.zfunc = 3;                            /* LESS -> LEQUAL: the same depth passes */
    if (pd.zfunc == 4) pd.zfunc = 6;                            /* GREATER -> GEQUAL */
    pd.stencil = false;
    if (!(pipe = pipe_get(&pd))) return;
    pc = *pc0;
    pc.p0[0] = (float)(c & 0xFF) / 255.0f; pc.p0[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    pc.p0[2] = (float)((c >> 16) & 0xFF) / 255.0f; pc.p0[3] = (float)(c >> 24) / 255.0f;
    bind_pipe(pipe);
    push(&pc);
    vkCmdDrawIndexed(cmd(), icount, 1, first, 0, 0);
}

/* RECOMP_KVK_VMLOG=<n>: for n stereo frames, from the one after the first frame with a weapon draw (or the 60th), one line per draw
 * on the main target with what could tell the weapon apart (its classification, the program and the constants its
 * position reads, the viewport rows c58/c59, depth and raster state). The study behind the discriminator
 * (gun-latch.md). */
static int s_vmlog = -1;
static uint64_t s_vmlog_frames;
static bool s_vmlog_armed;
static int s_vmlog_wait;
static void vm_log_init(void)
{
    const char *e = getenv("RECOMP_KVK_VMLOG");
    s_vmlog = e && *e ? atoi(e) : 0;
    if (s_vmlog < 0) s_vmlog = 0;
}
static void vm_log_draw(const KGpuDraw *d, const KvkVs *ve, bool stereo, bool vm, const KvkSurf *cs, const KvkSurf *zs)
{
    const KPixelState *ps = d->ps;
    const float *c = d->gvs ? d->gvs->constants : NULL;
    char regs[64] = "";
    int r, n = 0;
    if (!s_vmlog_armed || !cs || !is_main(cs->gw, cs->gh) || cs->hud || (uint64_t)s_vmlog_frames >= (uint64_t)s_vmlog) return;
    if (ve) {
        /* the absolute constant ranges the position reads, as lo-hi */
        for (r = 0; r < 192 && n < 56; r++) {
            if (!(ve->pos_consts[r >> 5] & (1u << (r & 31)))) continue;
            {
                int e2 = r;
                while (e2 + 1 < 192 && (ve->pos_consts[(e2 + 1) >> 5] & (1u << ((e2 + 1) & 31)))) e2++;
                n += snprintf(regs + n, sizeof(regs) - (size_t)n, "%s%d-%d", n ? "," : "", r, e2);
                r = e2;
            }
        }
    }
    LOG("VMLOG f %llu %s vm %d st %d vs %08X pos %s ic %u | c10 %.4g %.4g %.4g %.4g w %.4g %.4g %.4g %.4g | c112w %.4g %.4g "
        "%.4g %.4g | vp c58 %.5g %.5g %.6g %.4g c59 %.5g %.5g %.6g %.4g | z t%d w%d f%d zb %.3g zclip %.6g..%.6g clipcull %d | "
        "blend %d %d %d cull %d %d tex0 %d | zs %d",
        (unsigned long long)G.st.frames, V.hud ? "hud" : "wld", vm, stereo, d->gvs ? d->gvs->hash : 0u, regs, d->icount,
        c ? c[40] : 0.f, c ? c[41] : 0.f, c ? c[42] : 0.f, c ? c[43] : 0.f, c ? c[52] : 0.f, c ? c[53] : 0.f,
        c ? c[54] : 0.f, c ? c[55] : 0.f, c ? c[460] : 0.f, c ? c[461] : 0.f, c ? c[462] : 0.f, c ? c[463] : 0.f,
        c ? c[232] : 0.f, c ? c[233] : 0.f, c ? c[234] : 0.f, c ? c[235] : 0.f, c ? c[236] : 0.f, c ? c[237] : 0.f,
        c ? c[238] : 0.f, c ? c[239] : 0.f, ps->depth_test, ps->depth_write, ps->depth_func, (double)d->zbias,
        (double)ps->zclip_min, (double)ps->zclip_max, ps->depth_clip_cull, ps->blend, ps->blend_sfactor, ps->blend_dfactor,
        d->cull_enable, d->cull_ctrl, (int)(ps->tex_mode[0] == PS_TEXTUREMODES_PROJECT2D && ps->tex[0].valid), zs != NULL);
}

static void draw_now(const KGpuDraw *d)
{
    const KelvinTarget *t = d->t;
    const KPixelState *ps = d->ps;
    KvkSurf *cs = NULL, *zs = NULL;
    const KvkSurf *ref;
    VkImageView views[4];
    VkSampler smps[4];
    uint32_t texlayer = 0, offs[6];
    PSConst pc;
    unsigned int alpha_mode = 0, i;
    bool has_alpha, short_idx, stereo = false, sprite = false, np_tint = false, hud, vm = false;
    VkShaderModule fs, vsm;
    uint64_t fs_sid = 0;
    uint32_t fs_len = 0;
    bool uber = false;
    KvkVs *ve = NULL;
    KvkPipeDesc pd;
    VkPipeline pipe;
    VkDescriptorSet s1;
    VkDeviceSize total, vab_bytes = 0, off, ib_off;
    uint32_t vab_words = 0, ib_first = 0;
    KvkPush push_c;
    VkViewport vp;
    VkRect2D sc;
    double sx, sy;
    float zb_f;

    if (!G.ok || !d->icount || (!d->gvs && !d->vcount)) return;
    stereo_prepare();
    if (hud_starts(d)) {
        static unsigned long long hud_logged;   /* every frame has one: log the first, then every 3600th */
        V.hud = true;
        if (!hud_logged || G.st.frames - hud_logged >= 3600u || spritelog()) {
            LOG("frame %llu: HUD begins", (unsigned long long)G.st.frames);
            hud_logged = G.st.frames ? G.st.frames : 1u;
        }
        if (spritelog()) {
            NpExt x;
            const bool ok = np_extent(d, &x);
            LOG("SPRITE f %llu HUD begins at vs %08X ic %u x %.1f..%.1f y %.1f..%.1f blend %d %d %d (%d)",
                (unsigned long long)G.st.frames, d->gvs ? d->gvs->hash : 0u, d->icount, (double)x.x0, (double)x.x1,
                (double)x.y0, (double)x.y1, ps->blend, ps->blend_sfactor, ps->blend_dfactor, ok);
        }
    }
    if (t->color) cs = surf_get(t, false, ps->color_format);
    if (t->zeta && (ps->depth_test || ps->stencil_test)) zs = surf_get(t, true, ps->zeta_format);
    if ((!cs && !zs) || (cs && zs && (cs->hw != zs->hw || cs->hh != zs->hh || cs->im.layers != zs->im.layers))) {
        skip_note("no target, or colour and depth of different sizes");
        return;
    }
    if (d->cull_enable && d->cull_ctrl == NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK) return;
    ref = cs ? cs : zs;
    hud = ref->hud;
    if (hud && cs && is_main(cs->gw, cs->gh) && hud_is_reticle(d) && vr_hides_aim_dot()) return;

    if (d->gvs) {
        ve = vs_lookup(d->gvs->program, d->gvs->hash, d->gvs->start, NULL, 0);
        if (!ve || !ve->mod) { skip_note("vertex program not ready"); return; }
    }
    /* VR: world draws on the main target get the eye transform */
    if (G.layers > 1 && !hud && V.st.valid && V.region != REGION_SCREEN &&
        (is_main(ref->gw, ref->gh) || (!V.hud && cs && eye_aligned_target(cs) && stereo_world_pass()))) {
        stereo = d->gvs ? ve->pos_class == KVK_POS_TRANSFORMED : cpu_w_perspective(d);
        /* a screen-space effect of a stereo frame's world pass (a menu or a movie frame stays on the virtual screen) */
        if (!stereo && stereo_world_pass()) {
            bool fullscreen;
            if (screen_effect(d, ve, &fullscreen)) stereo = sprite = true;
            else if (fullscreen) G.st.fullscreen_draws++;
            np_tint = ps->blend && !sprite;
        }
    }
    /* VR gun latch: a world draw whose position is made through this frame's weapon MVP reads the weapon's block */
    if (stereo && !sprite && is_main(ref->gw, ref->gh) && d->gvs && d->gvs->constants)
        vm = kvk_vr_viewmodel_draw(d->gvs->constants, ve->pos_consts);
    if (stereo && !sprite && ps->blend && is_main(ref->gw, ref->gh) && spritelog() && s_spritelog >= 2 && stereo_world_pass())
        persp_fx_log(d, ve, vm);
    if (stereo && !is_main(ref->gw, ref->gh)) G.st.offscreen_stereo_draws++;
    if (s_vmlog > 0 && V.st.valid) vm_log_draw(d, ve, stereo, vm, cs, zs);
    /* RECOMP_KVK_VMATTR=<n> (debug): the first n weapon draws' vertex attributes and a checksum of their data */
    if (vm && d->gvs) {
        static int left = -1;
        if (left < 0) { const char *e = getenv("RECOMP_KVK_VMATTR"); left = e ? atoi(e) : 0; }
        if (left > 0) {
            int a;
            left--;
            LOG("VMATTR f %llu ic %u vs %08X", (unsigned long long)G.st.frames, d->icount, d->gvs->hash);
            if (left == 7) {   /* once: constants, a4 value histogram per draw, the program's relative reads */
                const float *k = d->gvs->constants;
                int r;
                for (r = 0; r < 192; r++)
                    if (k[r*4] != 0.0f || k[r*4+1] != 0.0f || k[r*4+2] != 0.0f || k[r*4+3] != 0.0f)
                        LOG("VMCONST c%d %.5g %.5g %.5g %.5g", r, k[r*4], k[r*4+1], k[r*4+2], k[r*4+3]);
            }
            if (left >= 6) {
                const KGpuAttrSrc *a4 = &d->gvs->attr[4], *a1 = &d->gvs->attr[1];
                unsigned hist[512];
                int64_t o;
                char line[2048];
                int n = 0, v;
                memset(hist, 0, sizeof(hist));
                if (a4->fmt == 5 && a4->data)
                    for (o = a4->lo; o + 8 <= a4->hi; o += a4->stride) {
                        const int16_t *q = (const int16_t *)(a4->data + o);
                        int j;
                        for (j = 0; j < 4; j++) { v = q[j] + 256; if (v >= 0 && v < 512) hist[v]++; }
                    }
                for (v = 0; v < 512 && n < 2000; v++) if (hist[v]) n += snprintf(line + n, sizeof(line) - n, " %d:%u", v - 256, hist[v]);
                LOG("VMA4 ic %u values%s", d->icount, line);
                if (a1->data) {
                    const uint8_t *q = a1->data + a1->lo;
                    LOG("VMA1 first vertices %u %u %u %u | %u %u %u %u | a4 %d %d %d %d", q[0], q[1], q[2], q[3], q[36], q[37], q[38], q[39],
                        ((const int16_t *)(a4->data + a4->lo))[0], ((const int16_t *)(a4->data + a4->lo))[1],
                        ((const int16_t *)(a4->data + a4->lo))[2], ((const int16_t *)(a4->data + a4->lo))[3]);
                }
            }
            for (a = 0; a < 16; a++) {
                const KGpuAttrSrc *at = &d->gvs->attr[a];
                uint32_t sum = 0;
                int64_t i;
                if (!at->fmt || !at->data) continue;
                for (i = at->lo; i < at->hi && i < at->lo + 4096; i++) sum = sum * 31u + at->data[i];
                LOG("VMATTR   a%d fmt %u n %u stride %u data %p lo %lld hi %lld sum %08X", a, at->fmt, at->count, at->stride,
                    (const void *)at->data, (long long)at->lo, (long long)at->hi, sum);
            }
        }
    }
    /* RECOMP_KVK_SPRITELOG: blended draws into other targets of a stereo frame (effects drawn offscreen) */
    if (G.layers > 1 && V.st.valid && ps->blend && !is_main(ref->gw, ref->gh) && spritelog() && stereo_world_pass())
        LOG("SPRITE f %llu offscreen %ux%u va %08X vs %08X ic %u %s z t%d w%d f%d blend %d %d %d", (unsigned long long)G.st.frames,
            ref->gw, ref->gh, ref->va, d->gvs ? d->gvs->hash : 0u, d->icount,
            stereo ? (sprite ? "corrected (screen-space)" : "corrected (perspective)") : "left", ps->depth_test,
            ps->depth_write, ps->depth_func, ps->blend, ps->blend_sfactor, ps->blend_dfactor);
    if (G.layers > 1 && V.st.valid && hud && spritelog() && stereo_world_pass() && cs && is_main(cs->gw, cs->gh))
        LOG("SPRITE f %llu hud vs %08X ic %u z t%d w%d f%d blend %d %d %d", (unsigned long long)G.st.frames,
            d->gvs ? d->gvs->hash : 0u, d->icount, ps->depth_test, ps->depth_write, ps->depth_func, ps->blend,
            ps->blend_sfactor, ps->blend_dfactor);
    if (cs && is_main(cs->gw, cs->gh) && !hud && ps->depth_write && zs) V.world_draws++;
    if (stereo) { V.stereo_draws++; G.st.stereo_draws++; }
    if (sprite) G.st.sprite_draws++;
    if (vm) G.st.viewmodel_draws++;
    if (hud) { V.hud_used = true; G.st.hud_draws++; }

    /* textures (may end the pass: layout changes and conversions happen outside it) */
    for (i = 0; i < 4; i++) {
        const KTexture *tx = &ps->tex[i];
        KvkSurf *s2;
        views[i] = G.dummy.view;
        smps[i] = G.smp_linear;
        if (ps->tex_mode[i] != PS_TEXTUREMODES_PROJECT2D || !tx->valid) continue;
        s2 = surf_for_texture(tx);
        if (s2) {
            VkImageView v = VK_NULL_HANDLE;
            if (s2 == cs) v = feedback_copy(s2);
            else { surf_for_read(s2); v = s2->im.view; }
            if (v) { views[i] = v; if (s2->im.layers > 1) texlayer |= 1u << i; }
        } else {
            KvkSurf *ds = surf_depth_for_texture(tx);
            VkImageView v = ds ? depth_as_color(ds) : tex_get(tx);
            if (v) { views[i] = v; if (ds && ds->im.layers > 1) texlayer |= 1u << i; }
        }
        smps[i] = sampler_get(tx);
    }
    if (zs) {
        if (ps->depth_write || ps->stencil_write) zs->real = true;
        depth_surface_refresh(zs);
    }

    /* combiner constants and shaders */
    has_alpha = ps->color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 ||
                ps->color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8 ||
                ps->color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8;
    if (ps->color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8) alpha_mode = 1;
    if (ps->color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) alpha_mode = 2;
    if (hud) {
        /* the HUD surface keeps coverage in alpha: real alpha when blending, opaque when not */
        has_alpha = true;
        alpha_mode = ps->blend ? 0 : 1;
    }
    fill_ps_const(&pc, ps, alpha_mode);
    fs = ps_lookup(&pc, &fs_sid, &fs_len);
    if (!fs) {
        fs = ps->smooth ? G.ps_uber : G.ps_uber_flat;
        fs_sid = ps->smooth ? KVK_SID_UBER : KVK_SID_UBER_FLAT;
        fs_len = 0;
        uber = true;
    }
    vsm = d->gvs ? ve->mod : G.vs_cpu;

    if (!pass_begin(cs, zs)) { skip_note("render pass"); return; }

    /* pipeline */
    memset(&pd, 0, sizeof(pd));
    pd.vs = vsm; pd.fs = fs; pd.rp = pass_compat();
    pd.vs_sid = d->gvs ? ve->sid : KVK_SID_VS_CPU;
    pd.vs_len = d->gvs ? ve->slen : 0;
    pd.fs_sid = fs_sid; pd.fs_len = fs_len;
    pd.cf = P.cs ? P.cs->im.fmt : VK_FORMAT_UNDEFINED;
    pd.df = P.zs ? P.zs->im.fmt : VK_FORMAT_UNDEFINED;
    pd.views = P.views;
    pd.has_color = cs != NULL;
    if (cs) {
        VkColorComponentFlags mask = kvk_write_mask(ps->mask_r, ps->mask_g, ps->mask_b, ps->mask_a && has_alpha);
        VkBlendFactor sf = VK_BLEND_FACTOR_ONE, df = VK_BLEND_FACTOR_ZERO, sfa = VK_BLEND_FACTOR_ONE, dfa = VK_BLEND_FACTOR_ZERO;
        VkBlendOp op = VK_BLEND_OP_ADD, opa = VK_BLEND_OP_ADD;
        bool en = ps->blend;
        if (alpha_mode) mask |= VK_COLOR_COMPONENT_A_BIT;
        if (ps->blend) {
            op = opa = kvk_blend_op(ps->blend_eqn);
            sf = kvk_blend_factor(ps->blend_sfactor, false);
            df = kvk_blend_factor(ps->blend_dfactor, false);
            sfa = kvk_blend_factor(ps->blend_sfactor, true);
            dfa = kvk_blend_factor(ps->blend_dfactor, true);
            if (op == VK_BLEND_OP_MIN || op == VK_BLEND_OP_MAX) sf = df = sfa = dfa = VK_BLEND_FACTOR_ONE;
        }
        if (alpha_mode && !hud) {
            /* X8R8G8B8: the shader writes the forced alpha and the alpha channel passes it through */
            en = true;
            sfa = VK_BLEND_FACTOR_ONE; dfa = VK_BLEND_FACTOR_ZERO; opa = VK_BLEND_OP_ADD;
        }
        if (hud && ps->blend && op != VK_BLEND_OP_MIN && op != VK_BLEND_OP_MAX) {
            /* coverage accumulates as premultiplied alpha over the transparent HUD surface */
            sfa = VK_BLEND_FACTOR_ONE; dfa = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; opa = VK_BLEND_OP_ADD;
            mask |= VK_COLOR_COMPONENT_A_BIT;
        }
        pd.blend = pack_blend(en, sf, df, op, sfa, dfa, opa, mask);
    }
    {
        bool test = zs && ps->depth_test;
        bool sten = zs && ps->stencil_test && ps->zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
        pd.ztest = test;
        pd.zwrite = test && ps->depth_write;
        pd.zfunc = ps->depth_func & 7;
        pd.stencil = sten;
        pd.sfunc = ps->stencil_func;
        pd.sfail = ps->stencil_op_fail;
        pd.szfail = ps->stencil_op_zfail;
        pd.szpass = ps->stencil_op_zpass;
    }
    pd.topo = d->topology == KG_TOPO_STRIP ? RS_STRIP : RS_LIST;
    pd.cull = VK_CULL_MODE_NONE;
    if (d->cull_enable) pd.cull = d->cull_ctrl == NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    pd.ccw = d->ccw_front;
    pd.clamp = !ps->depth_clip_cull;
    pipe = pipe_get(&pd);
    if (!pipe && !uber) {
        /* the specialised combiner's pipeline is still being created: the ubershader's, if that one is ready */
        pd.fs = ps->smooth ? G.ps_uber : G.ps_uber_flat;
        pd.fs_sid = ps->smooth ? KVK_SID_UBER : KVK_SID_UBER_FLAT;
        pd.fs_len = 0;
        pipe = pipe_get(&pd);
        uber = pipe != VK_NULL_HANDLE;
    }
    if (!pipe) { skip_note("pipeline"); return; }
    if (uber) G.st.uber_draws++;
    s1 = tset_get(views, smps);
    if (!s1) { skip_note("texture descriptors"); return; }

    /* uploads: constants, vertex data, indices, all from one chunk */
    short_idx = d->index_limit && d->index_limit <= 0xFFFFu;
    if (d->gvs) {
        int a;
        for (a = 0; a < 16; a++) if (d->gvs->attr[a].fmt) vab_bytes += (VkDeviceSize)(d->gvs->attr[a].hi - d->gvs->attr[a].lo) + 64;
    } else {
        vab_bytes = (VkDeviceSize)d->vcount * d->stride * 16;
    }
    total = 4 * 4096 + KVK_STEREO_BYTES + vab_bytes + (VkDeviceSize)d->icount * 4 + 1024;
    if (vab_bytes > KVK_VAB_RANGE || !ring_reserve(total)) {
        static int said;
        if (said++ < 8) LOG("draw too large for the ring (%llu bytes of vertex data)", (unsigned long long)vab_bytes);
        return;
    }
    memset(offs, 0, sizeof(offs));
    offs[5] = (uint32_t)(vm ? stereo_block_vm() : stereo_block());
    offs[0] = (uint32_t)upload_dedup(&pc, sizeof(pc), &s_last_ps_val, &s_last_ps.chunk, &s_last_ps.off, &s_last_ps.have);
    sx = (double)ref->hw / (double)ref->gw;
    sy = (double)ref->hh / (double)ref->gh;
    if (d->gvs) {
        typedef struct Rng { const uint8_t *lo, *hi; size_t off; } Rng;
        const KGpuVsDraw *gv = d->gvs;
        Rng rg[16], mg[16];
        int nr = 0, nm = 0, a, j;
        size_t vtotal = 0;
        uint32_t acb[32][4];
        float vsg[20];
        uint8_t *p;
        memset(acb, 0, sizeof(acb));
        for (a = 0; a < 16; a++) {
            const KGpuAttrSrc *as = &gv->attr[a];
            memcpy(acb[16 + a], as->inline_value, 16);
            if (!as->fmt) continue;
            rg[nr].lo = as->data + as->lo;
            rg[nr].hi = as->data + as->hi;
            rg[nr].off = (size_t)a;
            for (j = nr; j > 0 && rg[j - 1].lo > rg[j].lo; j--) { Rng t2 = rg[j]; rg[j] = rg[j - 1]; rg[j - 1] = t2; }
            nr++;
        }
        for (a = 0; a < nr; a++) {
            if (nm && rg[a].lo <= mg[nm - 1].hi + 64) { if (rg[a].hi > mg[nm - 1].hi) mg[nm - 1].hi = rg[a].hi; }
            else mg[nm++] = rg[a];
        }
        for (a = 0; a < nm; a++) {
            mg[a].off = vtotal;
            vtotal += ((size_t)(mg[a].hi - mg[a].lo) + 15) & ~(size_t)15;     /* hi is one past the last byte */
        }
        if (!vtotal) vtotal = 16;
        p = ring_take(vtotal + 16, G.ssbo_align, &off);
        for (a = 0; a < nm; a++) memcpy(p + mg[a].off, mg[a].lo, (size_t)(mg[a].hi - mg[a].lo));
        memset(p + vtotal, 0, 16);
        offs[4] = (uint32_t)off;
        vab_words = (uint32_t)(vtotal / 4);
        for (a = 0; a < 16; a++) {
            const KGpuAttrSrc *as = &gv->attr[a];
            const uint8_t *lo;
            if (!as->fmt) continue;
            lo = as->data + as->lo;
            for (j = 0; j < nm; j++) if (lo >= mg[j].lo && lo < mg[j].hi + 1) break;
            if (j == nm) continue;
            acb[a][0] = (uint32_t)((int64_t)mg[j].off + (int64_t)(as->data - mg[j].lo));
            acb[a][1] = as->stride;
            acb[a][2] = as->fmt;
            acb[a][3] = as->count;
        }
        memset(vsg, 0, sizeof(vsg));
        vsg[0] = 2.0f / (float)ref->gw;
        vsg[1] = 2.0f / (float)ref->gh;
        vsg[2] = 1.0f / (ps->zmax > 0.0f ? ps->zmax : 16777215.0f);
        vsg[3] = 18446744073709551616.0f;
        vsg[4] = gv->fog_param[0]; vsg[5] = gv->fog_param[1];
        {
            uint32_t fl[4] = { gv->fog_enable ? 1u : 0u, (uint32_t)gv->fog_mode, gv->specular_enable ? 1u : 0u,
                               gv->ignore_specular_alpha ? 1u : 0u };
            memcpy(&vsg[8], fl, 16);
        }
        vsg[12] = viewmodel_scale(gv->hash);
        offs[1] = (uint32_t)upload_dedup(vsg, sizeof(vsg), s_last_vsg_val, &s_last_vsg.chunk, &s_last_vsg.off, &s_last_vsg.have);
        {
            /* VR first-person arms: the weapon's bone palette, edited in a copy (kvk_vr_viewmodel_bones) */
            const float *cv = gv->constants;
            float vmk[192 * 4];
            if (vm) {
                memcpy(vmk, gv->constants, sizeof(vmk));
                if (kvk_vr_viewmodel_bones(vmk, gv->attr[0].data)) cv = vmk;
            }
            offs[2] = (uint32_t)upload_dedup(cv, 192 * 16, s_last_c_val, &s_last_c.chunk, &s_last_c.off, &s_last_c.have);
        }
        {
            uint8_t *q = ring_take(sizeof(acb), ualign(), &off);
            memcpy(q, acb, sizeof(acb));
            offs[3] = (uint32_t)off;
        }
        G.st.gvs_draws++;
    } else {
        float vc[20];
        uint32_t rec[12];
        uint8_t *p = ring_take((VkDeviceSize)d->vcount * d->stride * 16, G.ssbo_align, &off);
        memcpy(p, d->recs, (size_t)d->vcount * d->stride * 16);
        offs[4] = (uint32_t)off;
        memset(vc, 0, sizeof(vc));
        vc[0] = 2.0f / (float)ref->gw;
        vc[1] = 2.0f / (float)ref->gh;
        vc[2] = 1.0f / (ps->zmax > 0.0f ? ps->zmax : 16777215.0f);
        vc[3] = 18446744073709551616.0f;
        memset(rec, 0, sizeof(rec));
        for (i = 0; i < KG_SLOTS; i++) {
            uint32_t s = d->slot[i] < 0 ? 0xFFFFFFFFu : (uint32_t)d->slot[i];
            static const int where[KG_SLOTS] = { 2, 3, 4, 5, 6, 7, 8, 9 };
            rec[where[i]] = s;
        }
        rec[0] = 0;
        rec[1] = d->stride;
        memcpy(&vc[4], rec, sizeof(rec));
        {
            uint8_t *q = ring_take(sizeof(vc), ualign(), &off);
            memcpy(q, vc, sizeof(vc));
            offs[1] = (uint32_t)off;
        }
    }
    {
        size_t ib_bytes = (size_t)d->icount * (short_idx ? 2 : 4);
        uint8_t *p = ring_take(ib_bytes, 4, &ib_off);
        if (short_idx) {
            uint16_t *KVK_RESTRICT w = (uint16_t *)p;               /* the ring never overlaps the caller's indices */
            const uint32_t *KVK_RESTRICT src = d->indices;
            const uint32_t n = d->icount;
            uint32_t k;
            for (k = 0; k < n; k++) w[k] = (uint16_t)src[k];
        } else {
            memcpy(p, d->indices, ib_bytes);
        }
    }

    /* state, then the draw */
    bind_pipe(pipe);
    bind_set0(offs);
    bind_set1(s1);
    vp.x = vp.y = 0.0f; vp.width = (float)ref->hw; vp.height = (float)ref->hh; vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    {
        long x0 = (long)floor(t->clip_x0 * sx), y0 = (long)floor(t->clip_y0 * sy);
        long x1 = (long)ceil(t->clip_x1 * sx), y1 = (long)ceil(t->clip_y1 * sy);
        if (x1 > (long)ref->hw) x1 = (long)ref->hw;
        if (y1 > (long)ref->hh) y1 = (long)ref->hh;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = (uint32_t)(x1 - x0); sc.extent.height = (uint32_t)(y1 - y0);
    }
    {
        /* a whole number of the depth buffer's smallest steps (2^-24: one zeta unit for D24, and for float depth in
         * [0.5, 1)), as kelvin_gpu.c get_raster */
        double zm = ps->zmax > 0.0f ? (double)ps->zmax : 16777215.0;
        double u = (double)d->zbias * (16777216.0 / zm);
        zb_f = (float)(int)(u < 0.0 ? u - 0.5 : u + 0.5);
    }
    set_dynamic(&vp, &sc, ps->blend_color, (uint32_t)ps->stencil_ref, (uint32_t)ps->stencil_mask_read & 0xFF,
                ps->stencil_write ? (uint32_t)ps->stencil_mask_write & 0xFF : 0, zb_f, d->zslope);
    memset(&push_c, 0, sizeof(push_c));
    push_c.texlayer[0] = texlayer;
    push_c.dflags[0] = (stereo ? KVK_DF_STEREO : 0u) | (sprite ? KVK_DF_SCREEN_DEPTH : 0u);
    push_c.dflags[1] = vab_words;
    push(&push_c);
    {
        /* The chunk is bound once at offset 0 (again only for another chunk, index type or pass) and each draw names
         * its indices by firstIndex: ib_off is a multiple of 4, so of either index size. One vkCmdBindIndexBuffer per
         * draw less (port/notes/perf.md section 11). */
        VkBuffer ib = cur_frame()->chunk[cur_frame()->cur].b.buf;
        VkIndexType it = short_idx ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (P.ib != ib || P.ib_off != 0 || P.ibt != it) {
            vkCmdBindIndexBuffer(cmd(), ib, 0, it);
            P.ib = ib; P.ib_off = 0; P.ibt = it;
        }
        ib_first = (uint32_t)(ib_off / (short_idx ? 2u : 4u));
    }
    vkCmdDrawIndexed(cmd(), d->icount, 1, ib_first, 0, 0);
    if (vm && cs) {
        /* RECOMP_KVK_VMSPLIT=1 (debug): each weapon draw of a frame gets its own tint, so the parts can be told apart */
        static int split = -1, ord; static unsigned long long ord_frame = ~0ull;
        uint32_t tint = kvk_vr_viewmodel_tint();
        if (split < 0) { const char *e = getenv("RECOMP_KVK_VMSPLIT"); split = e && *e == '1'; }
        if (split) {
            static const uint32_t k_split[4] = { 0x80FF00FFu, 0x80FFFF00u, 0x8000FF00u, 0x800000FFu };
            if (ord_frame != (unsigned long long)G.st.frames) { ord_frame = G.st.frames; ord = 0; }
            if (!tint) tint = k_split[0];
            tint = k_split[ord++ & 3];
        }
        draw_tint(&pd, &push_c, d->icount, ib_first, tint);
    }
    /* BLACKVR_SPRITE_DEBUG=1: corrected screen-space effects green, blended non-perspective world-pass draws left as
     * they are red (full-screen passes among them; the HUD is never tinted) */
    if (cs && is_main(cs->gw, cs->gh) && (sprite || np_tint) && sprite_debug()) draw_tint(&pd, &push_c, d->icount, ib_first, sprite ? 0x6000FF00u : 0x600000FFu);
    if (cs) { cs->ver++; cs->wseq = ++G.wseq; }
    if (zs && (ps->depth_write || ps->stencil_write)) { zs->ver++; zs->wseq = ++G.wseq; }
    G.st.draws++;
    G.st.tris += d->topology == KG_TOPO_STRIP ? (d->icount > 2 ? d->icount - 2 : 0) : d->icount / 3;
}

/* ---- clears ---------------------------------------------------------------------------------------------------------- */

static void clear_now(const KelvinTarget *t, bool color, bool depth, bool stencil, uint32_t color_value,
                      uint32_t zs_value, unsigned int channel_mask, int x0, int y0, int x1, int y1,
                      unsigned int color_format)
{
    KvkSurf *cs = NULL, *zs = NULL;
    double sx = 1.0, sy = 1.0;
    unsigned int zfmt = 0;
    if (s_vmlog > 0 && s_vmlog_armed && s_vmlog_frames < (uint64_t)s_vmlog && is_main(t->guest_width, t->guest_height))
        LOG("VMLOG f %llu clear colour %d depth %d stencil %d rect %d,%d-%d,%d", (unsigned long long)G.st.frames, color, depth,
            stencil, x0, y0, x1, y1);
    if (!G.ok) return;
    G.st.clears++;
    stereo_prepare();
    if (color && t->color) cs = surf_get(t, false, color_format);
    if ((depth || stencil) && t->zeta) {
        zfmt = t->zeta_bpp == 2 ? NV097_SET_SURFACE_FORMAT_ZETA_Z16 : NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
        zs = surf_get(t, true, zfmt);
    }
    if (cs) { sx = (double)cs->hw / cs->gw; sy = (double)cs->hh / cs->gh; }
    else if (zs) { sx = (double)zs->hw / zs->gw; sy = (double)zs->hh / zs->gh; }
    if (cs) { cs->ver++; cs->wseq = ++G.wseq; }
    if (zs) { zs->real = true; zs->from_color = false; zs->ver++; zs->wseq = ++G.wseq; }
    x0 = (int)floor(x0 * sx); y0 = (int)floor(y0 * sy);
    x1 = (int)ceil(x1 * sx); y1 = (int)ceil(y1 * sy);
    if (cs) {
        bool full = x0 <= 0 && y0 <= 0 && x1 >= (int)cs->hw && y1 >= (int)cs->hh;
        VkClearValue v;
        memset(&v, 0, sizeof(v));
        v.color.float32[0] = ((color_value >> 16) & 0xFF) / 255.0f;
        v.color.float32[1] = ((color_value >> 8) & 0xFF) / 255.0f;
        v.color.float32[2] = (color_value & 0xFF) / 255.0f;
        v.color.float32[3] = ((color_value >> 24) & 0xFF) / 255.0f;
        if (color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) v.color.float32[3] = 0.0f;
        if (color_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8) v.color.float32[3] = 1.0f;
        if (cs->hud) memset(&v, 0, sizeof(v));          /* the HUD surface stays transparent under its clears */
        if (full && channel_mask == 0xF && !(P.active && P.cs == cs)) {
            cs->pending = PC_COLOR;
            cs->pending_val = v;
        } else if (x1 > x0 && y1 > y0) {
            VkClearRect r;
            if (!(P.active && P.cs == cs) && !pass_begin(cs, P.active && P.cs == cs ? P.zs : NULL)) return;
            r.rect.offset.x = x0 < 0 ? 0 : x0; r.rect.offset.y = y0 < 0 ? 0 : y0;
            r.rect.extent.width = (uint32_t)((x1 > (int)cs->hw ? (int)cs->hw : x1) - r.rect.offset.x);
            r.rect.extent.height = (uint32_t)((y1 > (int)cs->hh ? (int)cs->hh : y1) - r.rect.offset.y);
            r.baseArrayLayer = 0;
            r.layerCount = 1;
            if (channel_mask == 0xF) {
                VkClearAttachment a;
                a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                a.colorAttachment = 0;
                a.clearValue = v;
                vkCmdClearAttachments(cmd(), 1, &a, 1, &r);
            } else {
                /* masked: a rectangle drawn with a colour write mask (bits B=1 G=2 R=4 A=8) */
                VkColorComponentFlags m = kvk_write_mask((channel_mask & 4) != 0, (channel_mask & 2) != 0,
                                                         (channel_mask & 1) != 0, (channel_mask & 8) != 0);
                VkPipeline pipe = fixed_pipe(G.fix[KVK_FIX_FULLSCREEN_VS], G.fix[KVK_FIX_CLEAR_COLOR_PS], pass_compat(),
                                             true, false, RS_LIST, blend_none(m));
                KvkPush pc;
                VkViewport vp = { 0.0f, 0.0f, (float)cs->hw, (float)cs->hh, 0.0f, 1.0f };
                static const float bc[4] = { 0, 0, 0, 0 };
                uint32_t offs[6] = { 0, 0, 0, 0, 0, 0 };
                if (!pipe || !ring_reserve(1024)) return;
                offs[5] = (uint32_t)stereo_block();
                memset(&pc, 0, sizeof(pc));
                memcpy(pc.p0, v.color.float32, 16);
                bind_pipe(pipe);
                bind_set0(offs);
                bind_set1(tset_single(G.dummy.view, G.smp_nearest));
                set_dynamic(&vp, &r.rect, bc, 0, 0xFF, 0xFF, 0.0f, 0.0f);
                push(&pc);
                vkCmdDraw(cmd(), 3, 1, 0, 0);
            }
        }
    }
    if (zs) {
        bool full = x0 <= 0 && y0 <= 0 && x1 >= (int)zs->hw && y1 >= (int)zs->hh;
        float zv = zfmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? (float)(zs_value & 0xFFFF) / 65535.0f
                                                           : (float)(zs_value >> 8) / 16777215.0f;
        int what = (depth ? PC_DEPTH : 0) | (stencil ? PC_STENCIL : 0);
        if (full && !(P.active && P.zs == zs)) {
            zs->pending |= what;
            if (depth) zs->pending_val.depthStencil.depth = zv;
            if (stencil) zs->pending_val.depthStencil.stencil = zs_value & 0xFF;
        } else if (x1 > x0 && y1 > y0) {
            VkClearAttachment a;
            VkClearRect r;
            if (!(P.active && P.zs == zs) && !pass_begin(P.active && P.zs == zs ? P.cs : NULL, zs)) return;
            a.aspectMask = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            a.colorAttachment = 0;
            a.clearValue.depthStencil.depth = zv;
            a.clearValue.depthStencil.stencil = zs_value & 0xFF;
            r.rect.offset.x = x0 < 0 ? 0 : x0; r.rect.offset.y = y0 < 0 ? 0 : y0;
            r.rect.extent.width = (uint32_t)((x1 > (int)zs->hw ? (int)zs->hw : x1) - r.rect.offset.x);
            r.rect.extent.height = (uint32_t)((y1 > (int)zs->hh ? (int)zs->hh : y1) - r.rect.offset.y);
            r.baseArrayLayer = 0;
            r.layerCount = 1;
            vkCmdClearAttachments(cmd(), 1, &a, 1, &r);
        }
    }
}

/* ---- VR composition and the flip --------------------------------------------------------------------------------- */

static VkFormat unorm_twin(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

static KvkEyeView *eye_view(VkImage img, uint32_t layer, uint32_t layers, VkFormat fmt, uint32_t w, uint32_t h)
{
    int i;
    KvkEyeView *e = NULL;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    for (i = 0; i < KVK_EYEVIEW_MAX; i++) {
        KvkEyeView *q = &G.eyeview[i];
        if (q->view && q->img == img && q->layer == layer && q->layers == layers && q->fmt == fmt && q->w == w && q->h == h) return q;
        if (!q->view && !e) e = q;
    }
    if (!e) {
        /* full (the shell recreated its swapchains): start over; the old views are not in flight past a frame.
         * vkDeviceWaitIdle externally synchronizes every queue of the device, so it takes the shell's queue lock. */
        queue_lock();
        vkDeviceWaitIdle(G.dev);
        queue_unlock();
        for (i = 0; i < KVK_EYEVIEW_MAX; i++) {
            KvkEyeView *q = &G.eyeview[i];
            if (q->fb[0]) vkDestroyFramebuffer(G.dev, q->fb[0], NULL);
            if (q->fb[1]) vkDestroyFramebuffer(G.dev, q->fb[1], NULL);
            if (q->view) { tset_forget(q->view, VK_NULL_HANDLE); vkDestroyImageView(G.dev, q->view, NULL); }
            memset(q, 0, sizeof(*q));
        }
        e = &G.eyeview[0];
    }
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = layer;
    vi.subresourceRange.layerCount = layers;
    if (!VKOK(vkCreateImageView(G.dev, &vi, NULL, &e->view))) { memset(e, 0, sizeof(*e)); return NULL; }
    e->img = img; e->layer = layer; e->layers = layers; e->fmt = fmt; e->w = w; e->h = h;
    return e;
}

/* One pass over an eye image view (both eyes under multiview, or eye `eye`): the world from the main target's
 * layers, or a frame without world on the virtual screen; then the HUD on the virtual screen. */
static void compose_pass(KvkEyeView *ev, int eye, KvkSurf *main_cs, KvkSurf *hud_cs, bool world)
{
    const uint32_t views = ev->layers;
    const int which = world ? 0 : 1;
    VkRenderPass rp = rp_get(ev->fmt, VK_FORMAT_UNDEFINED, views, world ? LOAD_DONTCARE : LOAD_CLEAR, LOAD_LOAD);
    VkRenderPass compat = rp_get(ev->fmt, VK_FORMAT_UNDEFINED, views, LOAD_LOAD, LOAD_LOAD);
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv;
    KvkPush pc;
    VkViewport vp = { 0.0f, 0.0f, (float)ev->w, (float)ev->h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { ev->w, ev->h } };
    static const float bc[4] = { 0, 0, 0, 0 };
    uint32_t offs[6] = { 0, 0, 0, 0, 0, 0 };
    if (!rp || !compat) return;
    if (!ev->fb[which]) {
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = compat;
        fi.attachmentCount = 1;
        fi.pAttachments = &ev->view;
        fi.width = ev->w; fi.height = ev->h; fi.layers = 1;
        if (!VKOK(vkCreateFramebuffer(G.dev, &fi, NULL, &ev->fb[which]))) return;
        ev->rp[which] = compat;
    }
    if (!ring_reserve(KVK_STEREO_BYTES + 256)) return;
    offs[5] = (uint32_t)stereo_block();
    memset(&cv, 0, sizeof(cv));
    bi.renderPass = rp;
    bi.framebuffer = ev->fb[which];
    bi.renderArea.extent.width = ev->w;
    bi.renderArea.extent.height = ev->h;
    bi.clearValueCount = 1;
    bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
    P.pipe = VK_NULL_HANDLE; P.set1 = VK_NULL_HANDLE; P.bound0 = false; P.dyn = false;
    memset(&pc, 0, sizeof(pc));
    if (main_cs && world) {
        VkPipeline pipe = fixed_pipe(G.fix[KVK_FIX_FULLSCREEN_VS], G.fix[KVK_FIX_COPY_PS], compat, true, false, RS_LIST, blend_none(15));
        if (pipe) {
            pc.p0[0] = views > 1 ? -1.0f : (float)(main_cs->im.layers > 1 ? eye : 0);
            pc.p0[2] = 1.0f / (float)ev->w;
            pc.p0[3] = 1.0f / (float)ev->h;
            bind_pipe(pipe);
            bind_set0(offs);
            bind_set1(tset_single(main_cs->im.view, G.smp_linear));
            set_dynamic(&vp, &sc, bc, 0, 0xFF, 0xFF, 0.0f, 0.0f);
            push(&pc);
            vkCmdDraw(cmd(), 3, 1, 0, 0);
        }
    }
    {
        KvkSurf *quads[2];
        int nq = 0, q;
        if (main_cs && !world) quads[nq++] = main_cs;
        if (hud_cs) quads[nq++] = hud_cs;
        for (q = 0; q < nq; q++) {
            const bool opaque = quads[q] == main_cs && !world;
            uint32_t bl = opaque ? blend_none(15)
                                 : pack_blend(true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                              VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 15);
            VkPipeline pipe = fixed_pipe(G.fix[KVK_FIX_QUAD_VS], G.fix[KVK_FIX_QUAD_PS], compat, true, false, RS_QUADSTRIP, bl);
            if (!pipe) continue;
            memset(&pc, 0, sizeof(pc));
            pc.p0[0] = 0.0f;
            pc.p0[1] = opaque ? -1.0f : 1.0f;
            pc.p1[0] = views > 1 ? -1.0f : (float)eye;
            bind_pipe(pipe);
            bind_set0(offs);
            bind_set1(tset_single(quads[q]->im.view, G.smp_linear));
            set_dynamic(&vp, &sc, bc, 0, 0xFF, 0xFF, 0.0f, 0.0f);
            push(&pc);
            vkCmdDraw(cmd(), 4, 1, 0, 0);
        }
    }
    vkCmdEndRenderPass(cmd());
    P.pipe = VK_NULL_HANDLE; P.set1 = VK_NULL_HANDLE; P.bound0 = false; P.dyn = false;
}

#include "kvk_vr_layers.inc"

static bool compose_eyes(KvkSurf *main_cs)
{
    VrEyeTarget eyes[VR_EYE_COUNT];
    KvkSurf *hud_cs = NULL;
    const bool world = V.stereo_draws > 0;
    unsigned int i;
    if (vrl_compose_flat(main_cs)) return false;        /* FLAT_* frame: the flat quad layer, no eye images */
    if (!vr_active() || !vr_acquire_eye_targets(eyes)) return false;
    stereo_prepare();
    vrl_relatch();                                      /* the transforms for the poses just latched */
    if (V.hud_used) {
        for (i = 0; i < KVK_MAX_SURF; i++)
            if (G.surf[i].used && G.surf[i].hud && !G.surf[i].depth && G.surf[i].va == main_cs->va) hud_cs = &G.surf[i];
    }
    surf_for_read(main_cs);
    if (hud_cs) surf_for_read(hud_cs);
    pass_end();
    if (vrl_hud_layer(hud_cs)) hud_cs = NULL;           /* the HUD on its own quad layer */
    vrl_capture_record(main_cs, hud_cs, world, eyes[0].width, eyes[0].height);   /* asked for: the eye pictures read back */
    {
        const VkFormat vf0 = s_dev.swapchain_mutable_format ? unorm_twin((VkFormat)eyes[0].format) : (VkFormat)eyes[0].format;
        if (G.layers > 1 && eyes[0].image == eyes[1].image && eyes[0].array_layer == 0 && eyes[1].array_layer == 1 &&
            eyes[0].width == eyes[1].width && eyes[0].height == eyes[1].height) {
            KvkEyeView *ev = eye_view((VkImage)(uintptr_t)eyes[0].image, 0, 2, vf0, eyes[0].width, eyes[0].height);
            if (ev) compose_pass(ev, 0, main_cs, hud_cs, world);
        } else {
            int e;
            for (e = 0; e < VR_EYE_COUNT; e++) {
                const VkFormat vf = s_dev.swapchain_mutable_format ? unorm_twin((VkFormat)eyes[e].format) : (VkFormat)eyes[e].format;
                KvkEyeView *ev = eye_view((VkImage)(uintptr_t)eyes[e].image, eyes[e].array_layer, 1, vf, eyes[e].width, eyes[e].height);
                if (ev) compose_pass(ev, e, main_cs, hud_cs, world);
            }
        }
    }
    return true;
}

static void flip_now(const KelvinTarget *t, unsigned int color_format)
{
    KvkSurf *cs;
    bool composed = false;
    unsigned int i;
    if (!G.ok) return;
    cs = surf_get_ex(t, false, color_format, false);
    if (cs) composed = compose_eyes(cs);
    G.st.frames++;
    G.st.world_draws += V.world_draws;
    frame_submit(s_cap.on);                             /* a capture frame waits for its readback */
    vrl_capture_deliver(G.st.frames);
    vrl_after_submit(composed);                         /* flat / HUD / eye images back to the shell */
    /* the next frame: automatic classification, transparent HUD surfaces. Every HUD colour surface: the title
     * double-buffers its back buffer, so there is one per back buffer, and one left uncleared keeps whatever opaque
     * full-screen draw it last had (a black intro card) under every later HUD drawn into it -- an opaque HUD layer
     * on every other frame (game_render_x86, port/notes/game-render-x86.md). */
    for (i = 0; i < KVK_MAX_SURF; i++)
        if (G.surf[i].used && G.surf[i].hud && !G.surf[i].depth) {
            G.surf[i].pending = PC_COLOR;
            memset(&G.surf[i].pending_val, 0, sizeof(G.surf[i].pending_val));
        }
    for (i = 0; i < KVK_MAX_SURF; i++)
        if (G.surf[i].used && G.surf[i].hud && G.surf[i].depth) {
            G.surf[i].pending = PC_DEPTH | PC_STENCIL;
            G.surf[i].pending_val.depthStencil.depth = 1.0f;
            G.surf[i].pending_val.depthStencil.stencil = 0;
        }
    if (s_vmlog > 0 && V.stereo_draws) {
        if (s_vmlog_armed) s_vmlog_frames++;
        else if (G.st.viewmodel_draws || ++s_vmlog_wait >= 60) s_vmlog_armed = true;   /* or 60 stereo frames in */
    }
    V.prepared = false;
    V.region = REGION_AUTO;
    V.hud = V.hud_used = false;
    V.world_draws = V.stereo_draws = 0;
    /* the driver's pipeline cache: every 1800 frames, and 300 frames after the last new pipeline (a level's set is
     * then in it even if the app is killed soon after: the process ends without a shutdown on the headset) */
    {
        const uint64_t np = __atomic_load_n(&G.st.pipelines, __ATOMIC_RELAXED);
        if (np != G.pipes_seen) { G.pipes_seen = np; G.pipes_quiet_from = G.st.frames; }
        if (np > G.pipes_at_save && ((G.st.frames % 1800) == 0 || G.st.frames - G.pipes_quiet_from == 300)) pcache_save();
    }
}

/* ---- initialisation ------------------------------------------------------------------------------------------------ */

static VkShaderModule fixed_module(int which)
{
    char *src = kvk_glsl_fixed(which);
    VkShaderModule m;
    if (!src) return VK_NULL_HANDLE;
    m = module_from_glsl(which == KVK_FIX_FULLSCREEN_VS || which == KVK_FIX_QUAD_VS ? KVK_STAGE_VERTEX : KVK_STAGE_FRAGMENT,
                         src, true);
    free(src);
    return m;
}

static bool init_layouts(void)
{
    VkDescriptorSetLayoutBinding b0[6], b1[4];
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(KvkPush) };
    VkDescriptorSetLayout sl[2];
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    int i;
    memset(b0, 0, sizeof(b0));
    for (i = 0; i < 6; i++) {
        b0[i].binding = (uint32_t)i;
        b0[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = 6;
    li.pBindings = b0;
    if (!VKOK(vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.dsl0))) return false;
    memset(b1, 0, sizeof(b1));
    for (i = 0; i < 4; i++) {
        b1[i].binding = (uint32_t)i;
        b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b1[i].descriptorCount = 1;
        b1[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = 4;
    li.pBindings = b1;
    if (!VKOK(vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.dsl1))) return false;
    sl[0] = G.dsl0; sl[1] = G.dsl1;
    pi.setLayoutCount = 2;
    pi.pSetLayouts = sl;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (!VKOK(vkCreatePipelineLayout(G.dev, &pi, NULL, &G.layout))) return false;
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; ps[0].descriptorCount = 5 * KVK_FRAMES * KVK_CHUNKS;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; ps[1].descriptorCount = KVK_FRAMES * KVK_CHUNKS;
    dpi.maxSets = KVK_FRAMES * KVK_CHUNKS;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (!VKOK(vkCreateDescriptorPool(G.dev, &dpi, NULL, &G.set0_pool))) return false;
    {
        /* the texture-set cache's pool; without it every set comes from the per-frame pools (tset_alloc_frame) */
        VkDescriptorPoolSize tp = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * KVK_TSET_POOL_SETS };
        VkDescriptorPoolCreateInfo ti = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        ti.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ti.maxSets = KVK_TSET_POOL_SETS;
        ti.poolSizeCount = 1;
        ti.pPoolSizes = &tp;
        if (!VKOK(vkCreateDescriptorPool(G.dev, &ti, NULL, &G.tset_pool))) G.tset_pool = VK_NULL_HANDLE;
    }
    return true;
}

static bool init_frames(void)
{
    int i;
    for (i = 0; i < KVK_FRAMES; i++) {
        KvkFrame *f = &G.frame[i];
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkCommandBuffer cbs[2];
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = s_dev.queue_family;
        if (!VKOK(vkCreateCommandPool(G.dev, &pi, NULL, &f->pool))) return false;
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        if (!VKOK(vkAllocateCommandBuffers(G.dev, &ai, cbs))) return false;
        f->cb = cbs[0]; f->ucb = cbs[1];
        if (!VKOK(vkCreateFence(G.dev, &fi, NULL, &f->fence))) return false;
        if (!chunk_create(&f->chunk[0])) return false;
        f->nchunk = 1;
    }
    return true;
}

static bool init_misc(void)
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    char path[640];
    size_t n = 0;
    void *data;
    int i;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    si.maxLod = 0.0f;
    if (!VKOK(vkCreateSampler(G.dev, &si, NULL, &G.smp_nearest))) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod = 16.0f;
    if (!VKOK(vkCreateSampler(G.dev, &si, NULL, &G.smp_linear))) return false;
    snprintf(path, sizeof(path), "%s/vk_pipeline_cache.bin", cache_dir());
    data = read_file(path, &n);
    ci.initialDataSize = n;
    ci.pInitialData = data;
    if (!VKOK(vkCreatePipelineCache(G.dev, &ci, NULL, &G.pcache))) {
        ci.initialDataSize = 0; ci.pInitialData = NULL;           /* a stale or foreign cache */
        if (!VKOK(vkCreatePipelineCache(G.dev, &ci, NULL, &G.pcache))) G.pcache = VK_NULL_HANDLE;
    }
    free(data);
    G.pipe = (KvkPipe *)calloc(KVK_PIPE_MAX, sizeof(KvkPipe));
    G.tex = (KvkTex *)calloc(KVK_TEX_SLOTS, sizeof(KvkTex));
    if (!G.pipe || !G.tex) return false;
    for (i = 0; i < KVK_FIX_COUNT; i++) if (!(G.fix[i] = fixed_module(i))) return false;
    {
        char *s = kvk_glsl_vs_cpu();
        G.vs_cpu = s ? module_from_glsl(KVK_STAGE_VERTEX, s, true) : VK_NULL_HANDLE;
        free(s);
        s = kvk_glsl_ps_uber(false);
        G.ps_uber = s ? module_from_glsl(KVK_STAGE_FRAGMENT, s, true) : VK_NULL_HANDLE;
        free(s);
        s = kvk_glsl_ps_uber(true);
        G.ps_uber_flat = s ? module_from_glsl(KVK_STAGE_FRAGMENT, s, true) : VK_NULL_HANDLE;
        free(s);
        if (!G.vs_cpu || !G.ps_uber || !G.ps_uber_flat) return false;
    }
    /* the 1x1 white texture unused stages see (D3D11's dummy SRV) */
    if (!img_create(&G.dummy, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    {
        VkClearColorValue white = { { 1.0f, 1.0f, 1.0f, 1.0f } };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkCommandBuffer ucb = upload_cmd();
        barrier(ucb, &G.dummy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(ucb, G.dummy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &r);
        barrier(ucb, &G.dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

/* Every format, limit and feature the backend relies on, from the device rather than assumed: logged once (the first
 * headset run then records what the Quest's driver reports), false if something required is missing. */
static bool caps_check(void)
{
    static const struct { VkFormat f; VkFormatFeatureFlags need; const char *name; } fmts[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, KVK_SURF_FEATURES, "B8G8R8A8_UNORM (surfaces)" },
        { VK_FORMAT_R8G8B8A8_UNORM, KVK_SURF_FEATURES, "R8G8B8A8_UNORM (textures, depth-as-colour)" },
        { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0, "A1R5G5B5_UNORM_PACK16 (surfaces, else BGRA8)" },
        { VK_FORMAT_R5G6B5_UNORM_PACK16, 0, "R5G6B5_UNORM_PACK16 (surfaces, else BGRA8)" },
        { VK_FORMAT_R8_UNORM, 0, "R8_UNORM (surfaces, else BGRA8)" },
        { VK_FORMAT_R8G8_UNORM, 0, "R8G8_UNORM (surfaces, else BGRA8)" },
        { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0, "BC1 (native DXT1, else decoded)" },
        { VK_FORMAT_BC2_UNORM_BLOCK, 0, "BC2 (native DXT3, else decoded)" },
        { VK_FORMAT_BC3_UNORM_BLOCK, 0, "BC3 (native DXT5, else decoded)" },
        { VK_FORMAT_D24_UNORM_S8_UINT, 0, "D24_UNORM_S8_UINT (depth, first choice)" },
        { VK_FORMAT_D32_SFLOAT_S8_UINT, 0, "D32_SFLOAT_S8_UINT (depth, second choice)" },
    };
    const VkPhysicalDeviceLimits *L = &G.props.limits;
    bool ok = true;
    unsigned int i;
    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(G.phys, fmts[i].f, &fp);
        LOG("format %-44s optimal features 0x%08X%s", fmts[i].name, (unsigned)fp.optimalTilingFeatures,
            (fp.optimalTilingFeatures & fmts[i].need) != fmts[i].need ? "  MISSING REQUIRED" : "");
        if ((fp.optimalTilingFeatures & fmts[i].need) != fmts[i].need) ok = false;
    }
    /* set 0: 5 dynamic UBOs + 1 dynamic SSBO, set 1: 4 combined samplers; vertex and fragment both see set 0 */
    LOG("limits: push constants %u (uses %u), bound sets %u (2), per stage UBO %u (5) SSBO %u (1) samplers %u (4) "
        "sampled images %u (4) resources %u (10), set UBO dyn %u (5) SSBO dyn %u (1), UBO range %u (3072), "
        "SSBO range %u (%u), lod bias %.2f, allocations %u, samplers %u, framebuffer %ux%u layers %u",
        L->maxPushConstantsSize, (unsigned)sizeof(KvkPush), L->maxBoundDescriptorSets,
        L->maxPerStageDescriptorUniformBuffers, L->maxPerStageDescriptorStorageBuffers, L->maxPerStageDescriptorSamplers,
        L->maxPerStageDescriptorSampledImages, L->maxPerStageResources, L->maxDescriptorSetUniformBuffersDynamic,
        L->maxDescriptorSetStorageBuffersDynamic, L->maxUniformBufferRange, L->maxStorageBufferRange,
        (unsigned)KVK_VAB_RANGE, L->maxSamplerLodBias, L->maxMemoryAllocationCount, L->maxSamplerAllocationCount,
        L->maxFramebufferWidth, L->maxFramebufferHeight, L->maxFramebufferLayers);
    if (L->maxPushConstantsSize < sizeof(KvkPush) || L->maxBoundDescriptorSets < 2 ||
        L->maxPerStageDescriptorUniformBuffers < 5 || L->maxPerStageDescriptorStorageBuffers < 1 ||
        L->maxPerStageDescriptorSamplers < 4 || L->maxPerStageDescriptorSampledImages < 4 ||
        L->maxPerStageResources < 10 || L->maxDescriptorSetUniformBuffersDynamic < 5 ||
        L->maxDescriptorSetStorageBuffersDynamic < 1 || L->maxUniformBufferRange < 192 * 16 ||
        L->maxStorageBufferRange < KVK_VAB_RANGE) {
        LOG("a descriptor or push-constant limit is below what the backend's layouts use");
        ok = false;
    }
    for (i = 0; i < G.memp.memoryTypeCount; i++)
        LOG("memory type %u: heap %u flags 0x%X%s%s%s%s%s", i, G.memp.memoryTypes[i].heapIndex,
            (unsigned)G.memp.memoryTypes[i].propertyFlags,
            (G.memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "",
            (G.memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
            (G.memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " COHERENT" : "",
            (G.memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " CACHED" : "",
            (G.memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? " LAZY" : "");
    return ok;
}

static VkFormat pick_depth_format(void)
{
    static const VkFormat cands[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    int i;
    for (i = 0; i < 2; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(G.phys, cands[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) return cands[i];
    }
    return VK_FORMAT_UNDEFINED;
}

/* ---- kelvin_gpu.h --------------------------------------------------------------------------------------------------- */

bool kgpu_enabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("RECOMP_KELVIN_GPU");
        s_enabled = e && *e ? *e != '0' : s_have_dev;
    }
    return s_enabled != 0;
}

bool kgpu_active(void) { return G.ok != 0; }

bool kgpu_init(void)
{
    if (G.tried) return G.ok != 0;
    G.tried = 1;
    if (!kgpu_enabled() || !s_have_dev) { LOG("no Vulkan device from the host: the CPU renderer stays"); return false; }
    if (!s_dev.multiview) LOG("multiview not enabled on the device: stereo is off");
    pthread_mutex_init(&G.qlock_internal, NULL);
    G.dev = s_dev.device;
    G.phys = s_dev.physical;
    G.queue = s_dev.queue;
    vkGetPhysicalDeviceProperties(G.phys, &G.props);
    vkGetPhysicalDeviceMemoryProperties(G.phys, &G.memp);
    G.ubo_align = G.props.limits.minUniformBufferOffsetAlignment > 16 ? G.props.limits.minUniformBufferOffsetAlignment : 16;
    G.ssbo_align = G.props.limits.minStorageBufferOffsetAlignment > 16 ? G.props.limits.minStorageBufferOffsetAlignment : 16;
    if (!caps_check()) { LOG("the device lacks a format or limit the backend needs"); return false; }
    G.depth_fmt = pick_depth_format();
    if (G.depth_fmt == VK_FORMAT_UNDEFINED) { LOG("no usable depth/stencil format"); return false; }
    G.layers = (s_stereo_req < 0 ? vr_active() != 0 : s_stereo_req != 0) && s_dev.multiview ? 2 : 1;
    G.host_w = s_raster_w; G.host_h = s_raster_h;
    G.hud_w = 1280; G.hud_h = 960;
    kvk_stereo_default_config(&V.cfg);
    vm_log_init();
    if (!kvk_spirv_init()) { LOG("glslang did not initialise"); return false; }
    if (!init_layouts() || !init_frames()) { LOG("Vulkan objects could not be created"); return false; }
    G.fi = 0;
    frame_begin();
    if (!init_misc()) { LOG("internal shaders or caches could not be created"); return false; }
    manifest_load_and_precompile();
    pipe_manifest_precreate();
    G.ok = 1;
    LOG("Vulkan backend on %s: host raster %ux%u, %s, depth %s, ring %u MiB x %d frames", G.props.deviceName,
        G.host_w, G.host_h, G.layers > 1 ? "stereo (multiview, 2 layers)" : "mono",
        G.depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", (unsigned)(KVK_CHUNK_SIZE >> 20), KVK_FRAMES);
    return true;
}

void kgpu_report(void)
{
    if (!G.ok) return;
    LOG("frames %llu draws %llu (gpu vertices %llu, ubershader %llu, world %llu, stereo %llu, sprites %llu, hud %llu, "
        "skipped %llu) tris %llu clears %llu passes %llu pipelines %llu vs %llu ps %llu uploads %llu | tsets %llu "
        "(dropped %llu) np vertices %llu (reused %llu) tex hashed %llu MB",
        (unsigned long long)G.st.frames, (unsigned long long)G.st.draws, (unsigned long long)G.st.gvs_draws,
        (unsigned long long)G.st.uber_draws, (unsigned long long)G.st.world_draws,
        (unsigned long long)G.st.stereo_draws, (unsigned long long)G.st.sprite_draws, (unsigned long long)G.st.hud_draws,
        (unsigned long long)G.st.skipped, (unsigned long long)G.st.tris, (unsigned long long)G.st.clears, (unsigned long long)G.st.passes,
        (unsigned long long)G.st.pipelines, (unsigned long long)G.st.vs_compiled, (unsigned long long)G.st.ps_compiled,
        (unsigned long long)G.st.tex_uploads, (unsigned long long)G.st.tset_writes, (unsigned long long)G.st.tset_flushes,
        (unsigned long long)G.st.np_vertices, (unsigned long long)G.st.np_cache_hits,
        (unsigned long long)(G.st.tex_sig_bytes >> 20));
}

void kgpu_note_kick(void) { G.epoch++; }

void kgpu_host_size(unsigned int *w, unsigned int *h) { *w = G.host_w; *h = G.host_h; }

bool kgpu_vs_enabled(void)
{
    if (s_vs_enabled < 0) {
        const char *e = getenv("RECOMP_KGPU_CPU_VERTICES");
        s_vs_enabled = !(e && *e && *e != '0');
    }
    return s_vs_enabled != 0 && G.ok;
}

bool kgpu_vs_ready(const void *program, uint32_t hash, uint32_t start, const uint32_t *words, uint32_t nwords)
{
    return vs_lookup(program, hash, start, words, nwords) != NULL;
}

void kgpu_draw(const KGpuDraw *d) { draw_now(d); }

void kgpu_clear(const KelvinTarget *t, bool color, bool depth, bool stencil, uint32_t color_value, uint32_t zs_value,
                unsigned int channel_mask, int x0, int y0, int x1, int y1, unsigned int color_format)
{
    clear_now(t, color, depth, stencil, color_value, zs_value, channel_mask, x0, y0, x1, y1, color_format);
}

void kgpu_flip(const KelvinTarget *t, unsigned int color_format, uint32_t aspect_w, uint32_t aspect_h)
{
    (void)aspect_w; (void)aspect_h;
    flip_now(t, color_format);
}

void kgpu_set_viewmodel(const uint32_t *vs_hashes, int count, float scale)
{
    int i;
    if (count > 8) count = 8;
    s_vm_count = 0;
    for (i = 0; i < count; i++) s_vm_hash[i] = vs_hashes[i];
    s_vm_scale = scale;
    s_vm_count = count < 0 ? 0 : count;
}

void kgpu_set_view(int mode, int slot, int x0, int y0, int x1, int y1)
{
    (void)slot; (void)x0; (void)y0; (void)x1; (void)y1;
    if (mode >= KG_VIEW_VR_WORLD) kvk_vr_view_mark(mode, slot);
    if (mode == KVK_VIEW_VR_CAMERA) { V.prepared = false; return; }   /* this frame's camera: re-prepare */
    if (mode == KG_VIEW_VR_WORLD) { V.region = REGION_WORLD; return; }
    if (mode == KG_VIEW_VR_SCREEN) { V.region = REGION_SCREEN; if (G.layers > 1) V.hud = true; return; }
    if (mode != KG_VIEW_FULL) {
        static int said;
        if (!said++) LOG("split-screen views are not supported by the Vulkan backend");
    }
}

void kgpu_set_ao_camera(float half_x, float half_y) { (void)half_x; (void)half_y; }

static volatile int s_capture_result;
void kgpu_request_capture(const char *path) { (void)path; s_capture_result = -1; }
int kgpu_capture_result(void) { return s_capture_result; }

bool kgpu_compare_enabled(void) { return false; }
void kgpu_compare_frame(const KelvinTarget *t, unsigned int color_format) { (void)t; (void)color_format; }
void kgpu_pass_check(void) {}
bool kgpu_async_active(void) { return false; }
void kgpu_drain(void) {}
void kgpu_defer_call(void (*fn)(uintptr_t, uintptr_t), uintptr_t a, uintptr_t b) { fn(a, b); }
uint32_t kgpu_get_bound(uint32_t get) { return get; }
void kgpu_symbol_name(uint64_t addr, char *out, size_t cap) { snprintf(out, cap, "%llx", (unsigned long long)addr); }

/* ---- kvk_device.h ---------------------------------------------------------------------------------------------------- */

void kvk_get_stats(KvkStats *out) { *out = G.st; }

bool kvk_read_surface(uint32_t va, unsigned int layer, void *out, size_t pitch, unsigned int *w, unsigned int *h,
                      size_t cap)
{
    KvkSurf *s = NULL;
    KvkBuf b;
    VkBufferImageCopy r;
    unsigned int i, y;
    if (!G.ok) return false;
    for (i = 0; i < KVK_MAX_SURF; i++)
        if (G.surf[i].used && !G.surf[i].depth && !G.surf[i].hud && G.surf[i].va == va) s = &G.surf[i];
    if (!s || s->bpp != 4 || layer >= s->im.layers) return false;
    *w = s->hw; *h = s->hh;
    if (pitch < (size_t)s->hw * 4 || cap < pitch * s->hh) return false;
    if (!buf_create(&b, (VkDeviceSize)s->hw * s->hh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BUF_READBACK)) return false;
    surf_resolve_clear(s);
    pass_end();
    barrier(cmd(), &s->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.baseArrayLayer = layer;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = s->hw; r.imageExtent.height = s->hh; r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd(), s->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buf, 1, &r);
    {
        /* the copy's writes made visible to the host: a fence wait alone covers only device accesses */
        VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = b.buf;
        hb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
    }
    frame_submit(true);
    if (!b.coherent) {
        VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        mr.memory = b.mem;
        mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(G.dev, 1, &mr);
    }
    for (y = 0; y < s->hh; y++) memcpy((uint8_t *)out + (size_t)y * pitch, b.map + (size_t)y * s->hw * 4, (size_t)s->hw * 4);
    buf_destroy(&b);
    return true;
}

void kvk_shutdown(void)
{
    int i;
    if (!G.ok) return;
    kvk_wait_compiles();
    pass_end();
    frame_submit(true);
    queue_lock();
    vkQueueWaitIdle(G.queue);
    queue_unlock();
    pcache_save();
    for (i = 0; i < KVK_FRAMES; i++) run_dead(&G.frame[i]);
    for (i = 0; i < KVK_MAX_SURF; i++) if (G.surf[i].used) { img_destroy_now(&G.surf[i].im); img_destroy_now(&G.surf[i].conv); }
    for (i = 0; i < KVK_TEX_SLOTS; i++) if (G.tex[i].used) img_destroy_now(&G.tex[i].im);
    for (i = 0; i < KVK_FRAMES; i++) run_dead(&G.frame[i]);
    img_destroy_now(&G.fb_copy);
    img_destroy_now(&G.dummy);
    for (i = 0; i < KVK_FB_MAX; i++) if (G.fb[i].used) vkDestroyFramebuffer(G.dev, G.fb[i].fb, NULL);
    for (i = 0; i < KVK_EYEVIEW_MAX; i++) {
        if (G.eyeview[i].fb[0]) vkDestroyFramebuffer(G.dev, G.eyeview[i].fb[0], NULL);
        if (G.eyeview[i].fb[1]) vkDestroyFramebuffer(G.dev, G.eyeview[i].fb[1], NULL);
        if (G.eyeview[i].view) vkDestroyImageView(G.dev, G.eyeview[i].view, NULL);
    }
    for (i = 0; i < KVK_RP_MAX; i++) if (G.rp[i].used) vkDestroyRenderPass(G.dev, G.rp[i].rp, NULL);
    for (i = 0; i < KVK_PIPE_MAX; i++) if (G.pipe[i].used) vkDestroyPipeline(G.dev, G.pipe[i].pipe, NULL);
    for (i = 0; i < KVK_SMP_MAX; i++) if (G.smp[i].used) vkDestroySampler(G.dev, G.smp[i].s, NULL);
    for (i = 0; i < KVK_VS_MAX; i++) if (G.vs[i].mod) vkDestroyShaderModule(G.dev, G.vs[i].mod, NULL);
    for (i = 0; i < KVK_PS_MAX; i++) if (G.ps[i].mod) vkDestroyShaderModule(G.dev, G.ps[i].mod, NULL);
    for (i = 0; i < KVK_FIX_COUNT; i++) if (G.fix[i]) vkDestroyShaderModule(G.dev, G.fix[i], NULL);
    vkDestroyShaderModule(G.dev, G.vs_cpu, NULL);
    vkDestroyShaderModule(G.dev, G.ps_uber, NULL);
    vkDestroyShaderModule(G.dev, G.ps_uber_flat, NULL);
    vkDestroySampler(G.dev, G.smp_nearest, NULL);
    vkDestroySampler(G.dev, G.smp_linear, NULL);
    for (i = 0; i < KVK_FRAMES; i++) {
        KvkFrame *f = &G.frame[i];
        int k;
        for (k = 0; k < f->nchunk; k++) buf_destroy(&f->chunk[k].b);
        for (k = 0; k < f->ntpool; k++) vkDestroyDescriptorPool(G.dev, f->tpool[k], NULL);
        vkDestroyFence(G.dev, f->fence, NULL);
        vkDestroyCommandPool(G.dev, f->pool, NULL);
        free(f->dead);
    }
    if (G.pcache) vkDestroyPipelineCache(G.dev, G.pcache, NULL);
    vkDestroyDescriptorPool(G.dev, G.set0_pool, NULL);
    if (G.tset_pool) vkDestroyDescriptorPool(G.dev, G.tset_pool, NULL);
    vkDestroyPipelineLayout(G.dev, G.layout, NULL);
    vkDestroyDescriptorSetLayout(G.dev, G.dsl0, NULL);
    vkDestroyDescriptorSetLayout(G.dev, G.dsl1, NULL);
    free(G.pipe);
    free(G.tex);
    memset(&G, 0, sizeof(G));
}
