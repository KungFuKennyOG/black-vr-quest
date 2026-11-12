/* kvk-backend: the Kelvin Vulkan backend driven through kelvin_gpu.h exactly as the engine drives it, on a real
 * Vulkan device (SwiftShader works; any ICD with Vulkan 1.1 + multiview). Exit 77 (skipped) without a device.
 *
 *   cpu-parity  512 random register-combiner configurations with random textures (swizzled A8R8G8B8, linear,
 *               DXT1), colours, fog, projective coordinates, alpha test, alpha kill, colour keys -- one pixel each --
 *               drawn by the backend with the specialised shaders and by Kelvin's CPU renderer (kpix_fragment,
 *               ktex_*), compared byte by byte. 512 random NV2A vertex programs on the GPU vertex path compared with
 *               the nv2a_vsh_cpu emulator. Every attribute format. Depth testing; partial and channel-masked clears;
 *               the depth buffer read as packed Z24S8 colour.
 *   uber        the combiner comparison with the combiner interpreter (RECOMP_KGPU_PS_SPEC=0), DXT decoded on the
 *               CPU (as on Quest, which has no BC formats).
 *   stereo      multiview stereo: a world quad through an Xbox-style transform program lands in each eye where plain
 *               trigonometry puts it; the frame is composed into the eye images (vr_acquire_eye_targets) with the
 *               HUD redirected to the virtual screen; a frame without world shows on the virtual screen.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "kelvin_gpu.h"
#include "kelvin_gfx.h"
#include "kvk_device.h"
#include "kvk_stereo.h"
#include "psh_regs.h"
#include "nv2a_vsh_cpu/nv2a_vsh_emulator.h"

static int s_fail, s_checks;
#define CHECK(c, ...) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- what the CPU reference and the backend need from the engine ---- */
static int s_unsupported;
void kelvin_unsupported(int what, const char *detail, uint32_t value) { (void)what; (void)detail; (void)value; s_unsupported++; }
uint8_t *kelvin_guest(uint32_t va) { (void)va; return NULL; }
uint32_t kelvin_dma_va(uint32_t instance, uint32_t offset, uint32_t bytes) { (void)instance; (void)offset; (void)bytes; return 0; }

/* ---- the XR shell, faked (strong definitions replace the backend's weak ones) ---- */
static int g_vr_on, g_eye_ok, g_released;
static VrFrameState g_fs;
static VrGameCamera g_cam;
static VrEyeTarget g_eyes[2];
int vr_active(void) { return g_vr_on; }
const VrFrameState *vr_frame_state(void) { return g_vr_on ? &g_fs : NULL; }
const VrGameCamera *vr_game_camera(void) { return g_vr_on ? &g_cam : NULL; }
int vr_acquire_eye_targets(VrEyeTarget out[VR_EYE_COUNT]) { if (!g_eye_ok) return 0; out[0] = g_eyes[0]; out[1] = g_eyes[1]; return 1; }
void vr_release_eye_targets(void) { g_released++; }

static uint32_t rnd(uint32_t n) { return (uint32_t)rand() % n; }
static float rnd255(void) { return (float)rnd(256) / 255.0f; }

/* ---- device ---- */
static VkInstance s_inst;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static uint32_t s_qf;
static VkCommandPool s_pool;

static bool make_device(bool *bc, bool allow_bc)
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
    uint32_t n = 1, i;
    float prio = 1.0f;
    ai.apiVersion = VK_API_VERSION_1_1;
    if (getenv("KVK_TEST_BDA") && *getenv("KVK_TEST_BDA") == '1') ai.apiVersion = VK_API_VERSION_1_2;  /* see below */
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
    en.textureCompressionBC = allow_bc ? f2.features.textureCompressionBC : VK_FALSE;   /* Quest has no BC */
    *bc = en.textureCompressionBC != 0;
    memset(&mv, 0, sizeof(mv));
    mv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    mv.multiview = VK_TRUE;
    qi.queueFamilyIndex = s_qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    di.pNext = &mv;
    {
        /* KVK_TEST_BDA=1: also enable VK_KHR_buffer_device_address, which the validation layer's GPU-assisted mode
         * needs for its instrumentation (the backend itself does not use it). Validation runs only. */
        static VkPhysicalDeviceBufferDeviceAddressFeatures bda = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
        const char *e = getenv("KVK_TEST_BDA");
        if (e && *e == '1') {                    /* a Vulkan 1.2 instance: the feature is core there */
            bda.bufferDeviceAddress = VK_TRUE;
            mv.pNext = &bda;
        }
    }
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &en;
    if (vkCreateDevice(s_phys, &di, NULL, &s_dev) != VK_SUCCESS) return false;
    vkGetDeviceQueue(s_dev, s_qf, 0, &s_queue);
    pi.queueFamilyIndex = s_qf;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(s_dev, &pi, NULL, &s_pool);
    {
        KvkDeviceInfo info;
        memset(&info, 0, sizeof(info));
        info.instance = s_inst; info.physical = s_phys; info.device = s_dev;
        info.queue_family = s_qf; info.queue = s_queue; info.api_version = VK_API_VERSION_1_1;
        info.multiview = true;
        info.robust_buffer_access = en.robustBufferAccess != 0;
        info.depth_clamp = en.depthClamp != 0;
        info.texture_compression_bc = en.textureCompressionBC != 0;
        kvk_set_device(&info);
    }
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

/* ---- Kelvin state helpers ---- */

typedef struct Tgt { KelvinTarget t; uint8_t *cpu, *zcpu; } Tgt;

static void tgt_init(Tgt *g, uint32_t va, uint32_t zva, unsigned int w, unsigned int h)
{
    memset(g, 0, sizeof(*g));
    g->cpu = (uint8_t *)calloc((size_t)w * h, 4);
    g->zcpu = (uint8_t *)calloc((size_t)w * h, 4);
    g->t.color = g->cpu; g->t.color_va = va;
    g->t.zeta = zva ? g->zcpu : NULL; g->t.zeta_va = zva;
    g->t.color_bpp = 4; g->t.zeta_bpp = 4;
    g->t.color_pitch = w * 4; g->t.zeta_pitch = w * 4;
    g->t.width = w; g->t.height = h; g->t.aa_x = g->t.aa_y = 1;
    g->t.clip_x0 = 0; g->t.clip_y0 = 0; g->t.clip_x1 = (int)w; g->t.clip_y1 = (int)h;
    g->t.guest_width = w; g->t.guest_height = h;
    g->t.guest_color_pitch = w * 4; g->t.guest_zeta_pitch = w * 4;
    g->t.guest_color_bytes = g->t.guest_zeta_bytes = (size_t)w * h * 4;
}

static void ps_base(KPixelState *ps, unsigned int w, unsigned int h)
{
    memset(ps, 0, sizeof(*ps));
    ps->color_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    ps->zeta_format = NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
    ps->mask_r = ps->mask_g = ps->mask_b = ps->mask_a = true;
    ps->smooth = true;
    ps->zmax = 16777215.0f;
    ps->zclip_min = 0.0f; ps->zclip_max = 16777215.0f;
    ps->depth_clip_cull = true;
    ps->alpha_func = 7;
    ps->clip_region[0][2] = (int)w; ps->clip_region[0][3] = (int)h;
    ps->stencil_mask_read = ps->stencil_mask_write = 0xFF;
}

static void parse_in(KCombinerInput *in, uint32_t v) { in->reg = (int)(v & 0xF); in->chan = (int)(v & 0x10); in->mod = (int)(v & 0xE0); }
static void parse_out(KCombinerOutput *o, uint32_t v)
{
    int flags = (int)(v >> 12);
    o->cd = (int)(v & 0xF); o->ab = (int)((v >> 4) & 0xF); o->muxsum = (int)((v >> 8) & 0xF); o->flags = flags;
    o->cd_op = flags & 1; o->ab_op = flags & 2; o->muxsum_op = flags & 4; o->mapping = flags & 0x38;
}

/* the final combiner as "D = register `reg`, G = its alpha": the pixel is that register */
static void final_passthrough(KPixelState *ps, int reg)
{
    int k;
    ps->final_enabled = true;
    for (k = 0; k < 7; k++) parse_in(&ps->fin[k], 0);
    parse_in(&ps->fin[3], (uint32_t)reg);
    parse_in(&ps->fin[6], (uint32_t)reg | 0x10u);
}

/* a quad of CPU-shaded vertex records (pos, d0, d1, fog, t0..t3), constant attributes */
typedef struct Quad { float x0, y0, x1, y1, z; float d0[4], d1[4], fog, t[4][4]; } Quad;
static float s_recs[4 * 8 * 4];
static uint32_t s_idx[6] = { 0, 1, 2, 2, 1, 3 };

static void gpu_quad(const Tgt *g, const KPixelState *ps, const Quad *q)
{
    KGpuDraw d;
    int v, i;
    for (v = 0; v < 4; v++) {
        float *r = s_recs + v * 8 * 4;
        r[0] = (v & 1) ? q->x1 : q->x0; r[1] = (v & 2) ? q->y1 : q->y0; r[2] = q->z; r[3] = 1.0f;
        memcpy(r + 4, q->d0, 16); memcpy(r + 8, q->d1, 16);
        r[12] = q->fog; r[13] = r[14] = r[15] = 0.0f;
        for (i = 0; i < 4; i++) memcpy(r + 16 + 4 * i, q->t[i], 16);
    }
    memset(&d, 0, sizeof(d));
    d.t = &g->t; d.ps = ps;
    d.recs = s_recs; d.vcount = 4; d.stride = 8;
    for (i = 0; i < KG_SLOTS; i++) d.slot[i] = i;
    d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
    kgpu_draw(&d);
}

/* The CPU renderer's pixels for the same quad (axis-aligned, constant attributes: every covered pixel the same). */
static void cpu_quad(const Tgt *g, const KPixelState *ps, const Quad *q)
{
    int x, y, i;
    for (y = (int)ceilf(q->y0 - 0.5f); y < (int)ceilf(q->y1 - 0.5f); y++)
        for (x = (int)ceilf(q->x0 - 0.5f); x < (int)ceilf(q->x1 - 0.5f); x++) {
            KFragment f;
            memset(&f, 0, sizeof(f));
            memcpy(f.d0, q->d0, 16); memcpy(f.d1, q->d1, 16);
            f.fog = q->fog;
            for (i = 0; i < 4; i++) memcpy(f.t[i], q->t[i], 16);
            f.z = q->z;
            f.x = x; f.y = y; f.front = true;
            kpix_fragment(ps, &g->t, &f);
        }
}

static void clear_both(Tgt *g, uint32_t argb, unsigned int w, unsigned int h)
{
    unsigned int i;
    kgpu_clear(&g->t, true, g->t.zeta != NULL, g->t.zeta != NULL, argb, 0xFFFFFF00u, 0xF, 0, 0, (int)w, (int)h,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
    for (i = 0; i < w * h; i++) { memcpy(g->cpu + i * 4, &argb, 4); ((uint32_t *)g->zcpu)[i] = 0xFFFFFF00u; }
}

/* pixels where the backend's surface and the CPU buffer differ by more than `tol` in a channel */
static int s_badpix[4096], s_nbad;
static int compare(const Tgt *g, unsigned int w, unsigned int h, int tol, const char *what, int *maxd)
{
    uint8_t *gpu = (uint8_t *)malloc((size_t)w * h * 4);
    unsigned int gw = 0, gh = 0, i;
    int bad = 0, shown = 0;
    *maxd = 0;
    if (!kvk_read_surface(g->t.color_va, 0, gpu, w * 4, &gw, &gh, (size_t)w * h * 4) || gw != w || gh != h) {
        CHECK(0, "%s: readback", what);
        free(gpu);
        return (int)(w * h);
    }
    for (i = 0; i < w * h; i++) {
        int c, d = 0;
        for (c = 0; c < 4; c++) { int e = abs((int)gpu[i * 4 + c] - (int)g->cpu[i * 4 + c]); if (e > d) d = e; }
        if (d > *maxd) *maxd = d;
        if (d > tol) {
            if (s_nbad < 4096) s_badpix[s_nbad++] = (int)i;
            bad++;
            if (shown++ < 4) printf("  %s pixel %u (%u,%u): gpu %02X%02X%02X%02X cpu %02X%02X%02X%02X (ARGB)\n", what, i, i % w, i / w,
                                    gpu[i * 4 + 3], gpu[i * 4 + 2], gpu[i * 4 + 1], gpu[i * 4], g->cpu[i * 4 + 3], g->cpu[i * 4 + 2], g->cpu[i * 4 + 1], g->cpu[i * 4]);
        }
    }
    free(gpu);
    return bad;
}

/* ---- guest textures ---- */

#define NTEX 9
static KTexture s_tex[NTEX];
static uint8_t s_texmem[NTEX][1024];

static void swizzle_masks(unsigned int width, unsigned int height, uint32_t *mx, uint32_t *my)
{
    uint32_t x = 0, y = 0, bit = 1, mask_bit = 1;
    int done;
    do {
        done = 1;
        if (bit < width) { x |= mask_bit; mask_bit <<= 1; done = 0; }
        if (bit < height) { y |= mask_bit; mask_bit <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mx = x; *my = y;
}

static void make_textures(void)
{
    int i, k;
    for (i = 0; i < NTEX; i++) {
        KTexture *t = &s_tex[i];
        memset(t, 0, sizeof(*t));
        for (k = 0; k < 1024; k++) s_texmem[i][k] = (uint8_t)rnd(256);
        t->valid = true;
        t->base = 0x00100000u + (uint32_t)i * 0x1000u;
        t->mem = s_texmem[i];
        t->dimensionality = 2; t->depth = 1; t->levels = 1;
        t->addr[0] = t->addr[1] = 1; t->addr[2] = 3;
        t->min_filter = NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0; t->mag_filter = 1;
        if (i < 6) {                                   /* 4x4 swizzled A8R8G8B8 */
            t->kind = TK_8888; t->color_format = 0x06; t->bpp = 4;
            t->width = t->height = 4; t->pitch = 16;
            if (i == 1) for (k = 3; k < 64; k += 12) s_texmem[i][k] = 0;    /* transparent texels, for alpha kill */
        } else if (i < 8) {                            /* 8x8 DXT1 */
            t->kind = TK_DXT1; t->color_format = 0x0C; t->bpp = 0;
            t->width = t->height = 8; t->pitch = 0;
        } else {                                       /* 4x4 linear (rectangle) A8R8G8B8: texel coordinates */
            t->kind = TK_8888; t->color_format = 0x12; t->bpp = 4; t->linear = true;
            t->width = t->height = 4; t->pitch = 16;
            t->addr[0] = t->addr[1] = 3;
        }
        t->level_w[0] = t->width; t->level_h[0] = t->height;
        swizzle_masks(t->width, t->height, &t->mask_x[0], &t->mask_y[0]);
    }
}

/* ---- combiners against the CPU renderer ---- */

static uint32_t rnd_in_stage(void)
{
    static const uint32_t regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13 };
    return regs[rnd(12)] | (rnd(2) ? 0x10u : 0) | (rnd(8) << 5);
}

static uint32_t rnd_dest(void)
{
    static const uint32_t regs[] = { 0, 0, 4, 5, 8, 9, 12, 12, 13 };
    return regs[rnd(9)];
}

static void rnd_ps(KPixelState *ps, Quad *q, unsigned int w, unsigned int h, int px, int py)
{
    int i, k;
    ps_base(ps, w, h);
    ps->num_stages = 1 + (int)rnd(4);
    ps->flags = (rnd(2) ? PS_COMBINERCOUNT_MUX_MSB : 0) | (rnd(2) ? PS_COMBINERCOUNT_UNIQUE_C0 : 0) |
                (rnd(2) ? PS_COMBINERCOUNT_UNIQUE_C1 : 0);
    for (i = 0; i < 8; i++) {
        for (k = 0; k < 4; k++) { parse_in(&ps->stage[i].rgb[k], rnd_in_stage()); parse_in(&ps->stage[i].alpha[k], rnd_in_stage()); }
        parse_out(&ps->stage[i].rgb_out, rnd_dest() | (rnd_dest() << 4) | (rnd_dest() << 8) | (rnd(256) << 12));
        parse_out(&ps->stage[i].alpha_out, rnd_dest() | (rnd_dest() << 4) | (rnd_dest() << 8) | ((rnd(256) & 0x3C) << 12));
        for (k = 0; k < 4; k++) { ps->stage[i].c0[k] = rnd255(); ps->stage[i].c1[k] = rnd255(); }
    }
    ps->final_enabled = rnd(4) != 0;
    if (ps->final_enabled) {
        for (k = 0; k < 7; k++) {
            static const uint32_t regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 14, 15 };
            parse_in(&ps->fin[k], regs[rnd(12)] | (rnd(2) ? 0x10u : 0) | (rnd(2) ? 0x20u : 0));
            /* E and F (fin[4], fin[5]) make EF_PROD, so they cannot read it (NV_register_combiners rejects it as
             * circular). Its value there is undefined: the GPU shaders read the 0 they start r15 with, the CPU
             * renderer an uninitialised Regs.ef, so the comparison depended on stack garbage (and ASLR). ZERO is
             * what the GPU reads there anyway; same rnd() sequence, so every other configuration is unchanged. */
            if ((k == 4 || k == 5) && ps->fin[k].reg == 0xF) ps->fin[k].reg = 0;
        }
        ps->clamp_sum = rnd(2); ps->inv_v1 = rnd(2); ps->inv_r0 = rnd(2);
        for (k = 0; k < 4; k++) { ps->final_c0[k] = rnd255(); ps->final_c1[k] = rnd255(); }
    }
    for (k = 0; k < 3; k++) ps->fog_color[k] = rnd255();
    if (rnd(4) == 0) { ps->alpha_test = true; ps->alpha_func = (int)rnd(8); ps->alpha_ref = (int)rnd(256); }
    memset(q, 0, sizeof(*q));
    q->x0 = (float)px; q->y0 = (float)py; q->x1 = q->x0 + 1.0f; q->y1 = q->y0 + 1.0f;
    for (k = 0; k < 4; k++) { q->d0[k] = rnd255(); q->d1[k] = rnd255(); }
    q->fog = rnd255();
    for (i = 0; i < 4; i++) {
        KTexture *t = &ps->tex[i];
        unsigned int tx = rnd(4), ty = rnd(4);
        q->t[i][3] = 1.0f;
        if (rnd(3) == 0) continue;
        ps->tex_mode[i] = PS_TEXTUREMODES_PROJECT2D;
        *t = s_tex[rnd(NTEX)];
        if (rnd(5) == 0) t->alphakill = true;
        if (rnd(6) == 0) {
            t->colorkey_mode = 1 + rnd(3);
            ps->color_key_mask[i] = 0x00F0F0F0u;
            ps->color_key[i] = rnd(2) ? 0 : (uint32_t)rand();
        }
        ps->lod_free[i] = ktex_lod_free(t);
        ps->rect_tex[i] = t->linear;                    /* as kpix_setup derives it */
        if (t->linear) { q->t[i][0] = (float)tx + 0.5f; q->t[i][1] = (float)ty + 0.5f; }
        else { q->t[i][0] = ((float)tx + 0.5f) / (float)t->width; q->t[i][1] = ((float)ty + 0.5f) / (float)t->height; }
        if (rnd(4) == 0) { float qq = 0.5f + (float)rnd(4); q->t[i][0] *= qq; q->t[i][1] *= qq; q->t[i][3] = qq; }
    }
}

#define PW 32
#define PH 12

/* msb: every configuration selects its muxes on r0.a's top bit. With the LSB select ((uint)(r0.a * 255) & 1, the
 * NV2A default) a 1-ulp difference between the GPU's UNORM conversion and the CPU's k * (1/255) flips the mux now and
 * then -- the D3D11 backend shares that -- so the strict comparison uses MSB and the LSB run allows 2%. */
static void test_combiners(bool spec, bool msb)
{
    Tgt g;
    static KPixelState ps[PW * PH];
    static Quad q[PW * PH];
    KvkStats a, b;
    int i, bad, maxd, round;
    tgt_init(&g, 0x01000000u, 0, PW, PH);
    srand(7);
    for (i = 0; i < PW * PH; i++) rnd_ps(&ps[i], &q[i], PW, PH, i % PW, i / PW);
    if (msb) for (i = 0; i < PW * PH; i++) ps[i].flags |= PS_COMBINERCOUNT_MUX_MSB;
    for (round = 0; round < (spec ? 2 : 1); round++) {
        kvk_get_stats(&a);
        clear_both(&g, 0xFF102030u, PW, PH);
        for (i = 0; i < PW * PH; i++) {
            gpu_quad(&g, &ps[i], &q[i]);
            cpu_quad(&g, &ps[i], &q[i]);
        }
        kvk_get_stats(&b);
        printf("combiners: round %d: %llu of %d draws used the interpreter\n", round,
               (unsigned long long)(b.uber_draws - a.uber_draws), PW * PH);
        if (spec && round == 0) kvk_wait_compiles();       /* the next round draws with the specialised shaders */
    }
    if (spec) CHECK(b.uber_draws == a.uber_draws, "the second round used only specialised shaders");
    s_nbad = 0;
    bad = compare(&g, PW, PH, 2, spec ? "specialised combiners" : "combiner interpreter", &maxd);
    if (getenv("KVK_TEST_VERBOSE"))
        for (i = 0; i < s_nbad; i++) {
            const KPixelState *p = &ps[s_badpix[i]];
            int k;
            printf("  pixel %d: stages %d final %d atest %d/%d/%d flags %x:", s_badpix[i], p->num_stages, p->final_enabled,
                   p->alpha_test, p->alpha_func, p->alpha_ref, p->flags);
            for (k = 0; k < 4; k++)
                if (p->tex_mode[k]) printf(" t%d[kind %u lin %d ak %d ck %u q %g]", k, p->tex[k].kind, p->tex[k].linear, p->tex[k].alphakill,
                                           p->tex[k].colorkey_mode, q[s_badpix[i]].t[k][3]);
            printf("\n");
        }
    printf("combiners (%s, %s mux): %d of %d pixels differ from the CPU renderer by more than 2/255 (max %d)\n",
           spec ? "specialised" : "ubershader", msb ? "MSB" : "random", bad, PW * PH, maxd);
    if (msb) CHECK(bad == 0, "%s: %d pixels differ from the CPU renderer", spec ? "specialised" : "ubershader", bad);
    else CHECK(bad <= PW * PH / 50, "%s: %d pixels differ from the CPU renderer", spec ? "specialised" : "ubershader", bad);
    free(g.cpu); free(g.zcpu);
}

/* ---- vertex programs against the nv2a_vsh_cpu emulator ---- */

static void vop(Nv2aVshOperation *o, Nv2aVshOpcode code, Nv2aVshRegisterType ot, unsigned oi, Nv2aVshWritemask wm)
{
    int k, q;
    memset(o, 0, sizeof(*o));
    o->opcode = code;
    o->outputs[0].type = ot; o->outputs[0].index = oi; o->outputs[0].writemask = wm;
    for (k = 0; k < 3; k++) for (q = 0; q < 4; q++) o->inputs[k].swizzle[q] = (uint8_t)q;
}

static void vin(Nv2aVshOperation *o, int k, Nv2aVshRegisterType t, unsigned idx)
{
    o->inputs[k].type = t; o->inputs[k].index = idx;
}

static int nin_of(Nv2aVshOpcode op)
{
    switch (op) {
    case NV2AOP_MOV: case NV2AOP_ARL: case NV2AOP_RCP: case NV2AOP_RCC: case NV2AOP_RSQ:
    case NV2AOP_EXP: case NV2AOP_LOG: case NV2AOP_LIT: return 1;
    case NV2AOP_MAD: return 3;
    default: return 2;
    }
}

/* oPos = v0; random arithmetic over v1..v3, c0..c15 and r0..r5; an a0-relative read; oD0 = the result */
static void rnd_vprog(Nv2aVshStep *st)
{
    static const Nv2aVshOpcode mac[] = { NV2AOP_MOV, NV2AOP_MUL, NV2AOP_ADD, NV2AOP_MAD, NV2AOP_DP3, NV2AOP_DPH,
                                         NV2AOP_DP4, NV2AOP_DST, NV2AOP_MIN, NV2AOP_MAX, NV2AOP_SLT, NV2AOP_SGE };
    /* LOG is left out: nv2a_vsh_cpu_log computes the exponent as uint32_t, so for |x| < 1 the emulator's .x is
     * about 4.3e9 where the D3D11 HLSL (and this GLSL) give the signed exponent (see notes/renderer.md);
     * test_log covers LOG where the two agree. */
    static const Nv2aVshOpcode ilu[] = { NV2AOP_RCP, NV2AOP_RCC, NV2AOP_RSQ, NV2AOP_EXP, NV2AOP_LIT, NV2AOP_RCP };
    int n = 2 + (int)rnd(12), i, k, q;
    memset(st, 0, sizeof(Nv2aVshStep) * (size_t)(n + 3));
    vop(&st[0].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_XYZW);
    vin(&st[0].mac, 0, NV2ART_INPUT, 0);
    for (i = 1; i <= n; i++) {
        Nv2aVshOperation *m = &st[i].mac, *u = &st[i].ilu;
        vop(m, mac[rnd(12)], NV2ART_TEMPORARY, rnd(4), (Nv2aVshWritemask)(1 + rnd(15)));
        for (k = 0; k < nin_of(m->opcode); k++) {
            int r = (int)rnd(3);
            vin(m, k, r == 0 ? NV2ART_INPUT : r == 1 ? NV2ART_CONTEXT : NV2ART_TEMPORARY, r == 0 ? 1 + rnd(3) : r == 1 ? rnd(16) : rnd(4));
            for (q = 0; q < 4; q++) m->inputs[k].swizzle[q] = (uint8_t)rnd(4);
            m->inputs[k].is_negated = rnd(4) == 0;
        }
        if (rnd(3) == 0) {
            vop(u, ilu[rnd(6)], NV2ART_TEMPORARY, 4 + rnd(2), (Nv2aVshWritemask)(1 + rnd(15)));
            vin(u, 0, rnd(2) ? NV2ART_INPUT : NV2ART_TEMPORARY, rnd(2) ? 1 + rnd(3) : rnd(4));
            for (q = 0; q < 4; q++) u->inputs[0].swizzle[q] = (uint8_t)rnd(4);
        }
    }
    vop(&st[n + 1].mac, NV2AOP_ARL, NV2ART_ADDRESS, 0, NV2AWM_X);
    vin(&st[n + 1].mac, 0, NV2ART_CONTEXT, 16);                       /* a0 = c16.x = 3 */
    vop(&st[n + 2].mac, NV2AOP_MAD, NV2ART_OUTPUT, 3, NV2AWM_XYZW);
    vin(&st[n + 2].mac, 0, NV2ART_TEMPORARY, rnd(4));
    vin(&st[n + 2].mac, 1, NV2ART_CONTEXT, 4); st[n + 2].mac.inputs[1].is_relative = true;
    vin(&st[n + 2].mac, 2, NV2ART_TEMPORARY, 4 + rnd(2));
    st[n + 2].is_final = true;
}

static float clamp_color1(float c) { return isnan(c) ? 1.0f : c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c; }

static void test_vertex_programs(void)
{
    Tgt g;
    KPixelState ps;
    static Nv2aVshStep steps[PW * PH][20];
    static Nv2aVshProgram prog[PW * PH];
    static float cons[192 * 4], vdata[PW * PH][4][4][4];   /* [program][attribute v0..v3][vertex][component] */
    static uint8_t expect[PW * PH][4];
    uint8_t *gpu = (uint8_t *)malloc(PW * PH * 4);
    unsigned int gw, gh;
    int i, k, v, bad = 0, maxd = 0, pass;
    tgt_init(&g, 0x02000000u, 0, PW, PH);
    ps_base(&ps, PW, PH);
    ps.num_stages = 1;
    final_passthrough(&ps, 4);                              /* the pixel shows the vertex's diffuse colour */
    srand(11);
    for (i = 0; i < 192 * 4; i++) cons[i] = (float)((int)rnd(2001) - 1000) / 1000.0f;
    cons[16 * 4] = 3.0f;
    for (i = 0; i < PW * PH; i++) {
        rnd_vprog(steps[i]);
        prog[i].steps = steps[i];
        for (v = 0; v < 4; v++) {
            vdata[i][0][v][0] = (float)(i % PW) + ((v & 1) ? 1.0f : 0.0f);
            vdata[i][0][v][1] = (float)(i / PW) + ((v & 2) ? 1.0f : 0.0f);
            vdata[i][0][v][2] = 0.0f; vdata[i][0][v][3] = 1.0f;
        }
        for (k = 1; k < 4; k++) {
            float a[4];
            int c;
            for (c = 0; c < 4; c++) a[c] = (float)((int)rnd(2001) - 1000) / 1000.0f;
            for (v = 0; v < 4; v++) memcpy(vdata[i][k][v], a, 16);
        }
        {
            Nv2aVshCPUFullExecutionState es;
            Nv2aVshExecutionState s = nv2a_vsh_emu_initialize_full_execution_state(&es);
            memcpy(es.context_regs, cons, sizeof(cons));
            for (k = 0; k < 4; k++) memcpy(&es.input_regs[k * 4], vdata[i][k][0], 16);
            nv2a_vsh_emu_execute(&s, &prog[i]);
            for (k = 0; k < 4; k++) expect[i][k] = (uint8_t)floorf(clamp_color1(es.output_regs[3 * 4 + k]) * 255.0f + 0.5f);
        }
    }
    for (pass = 0; pass < 2; pass++) {
        int drawn = 0;
        clear_both(&g, 0xFF000000u, PW, PH);
        for (i = 0; i < PW * PH; i++) {
            KGpuVsDraw gv;
            KGpuDraw d;
            if (!kgpu_vs_ready(&prog[i], 0x5000u + (uint32_t)i, 0, NULL, 0)) continue;
            memset(&gv, 0, sizeof(gv));
            gv.program = &prog[i]; gv.hash = 0x5000u + (uint32_t)i; gv.constants = cons;
            for (k = 0; k < 4; k++) {
                gv.attr[k].fmt = 4; gv.attr[k].count = 4; gv.attr[k].stride = 16;
                gv.attr[k].data = (const uint8_t *)vdata[i][k]; gv.attr[k].lo = 0; gv.attr[k].hi = 64;
            }
            memset(&d, 0, sizeof(d));
            d.t = &g.t; d.ps = &ps; d.gvs = &gv;
            d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
            kgpu_draw(&d);
            drawn++;
        }
        if (pass == 0) { kvk_wait_compiles(); continue; }
        CHECK(drawn == PW * PH, "every vertex program compiled (%d of %d)", drawn, PW * PH);
    }
    CHECK(kvk_read_surface(g.t.color_va, 0, gpu, PW * 4, &gw, &gh, PW * PH * 4), "readback");
    for (i = 0; i < PW * PH; i++) {
        static const int bgra[4] = { 2, 1, 0, 3 };
        int d = 0, c;
        for (c = 0; c < 4; c++) { int e = abs((int)gpu[i * 4 + bgra[c]] - (int)expect[i][c]); if (e > d) d = e; }
        if (d > maxd) maxd = d;
        if (d > 2) {
            if (bad < 4) printf("  program %d: gpu %02X %02X %02X %02X want %02X %02X %02X %02X (RGBA)\n", i, gpu[i * 4 + 2], gpu[i * 4 + 1],
                                gpu[i * 4], gpu[i * 4 + 3], expect[i][0], expect[i][1], expect[i][2], expect[i][3]);
            if (getenv("KVK_TEST_VERBOSE")) {
                const Nv2aVshStep *q;
                for (q = steps[i];; q++) {
                    printf("    mac %d -> %d.%d wm %d | ilu %d -> %d.%d wm %d\n", q->mac.opcode, q->mac.outputs[0].type, q->mac.outputs[0].index,
                           q->mac.outputs[0].writemask, q->ilu.opcode, q->ilu.outputs[0].type, q->ilu.outputs[0].index, q->ilu.outputs[0].writemask);
                    if (q->is_final) break;
                }
            }
            bad++;
        }
    }
    printf("vertex programs: %d of %d differ from the nv2a_vsh_cpu emulator by more than 2/255 (max %d)\n", bad, PW * PH, maxd);
    CHECK(bad <= PW * PH / 100, "vertex programs: %d differ", bad);
    free(gpu); free(g.cpu); free(g.zcpu);
}

/* LOG where the emulator is right (|x| >= 1); below 1 it is not (see rnd_vprog), which is printed, not failed */
static void test_log(void)
{
    static const float xs[8] = { 1.0f, 2.0f, 3.5f, 7.9f, 100.0f, -12.0f, 0.5f, 0.01f };
    Tgt g;
    KPixelState ps;
    static Nv2aVshStep st[3];
    static Nv2aVshProgram prog = { st };
    static float cons[192 * 4], pos[8][4][4], val[8][4][4];
    uint8_t gpu[8 * 4];
    unsigned int gw, gh;
    int i, v, k;
    tgt_init(&g, 0x06000000u, 0, 8, 1);
    ps_base(&ps, 8, 1);
    ps.num_stages = 1;
    final_passthrough(&ps, 4);
    vop(&st[0].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_XYZW); vin(&st[0].mac, 0, NV2ART_INPUT, 0);
    vop(&st[0].ilu, NV2AOP_LOG, NV2ART_TEMPORARY, 0, NV2AWM_XYZW); vin(&st[0].ilu, 0, NV2ART_INPUT, 1);
    vop(&st[1].mac, NV2AOP_MUL, NV2ART_OUTPUT, 3, NV2AWM_XYZW); vin(&st[1].mac, 0, NV2ART_TEMPORARY, 0); vin(&st[1].mac, 1, NV2ART_CONTEXT, 0);
    st[1].is_final = true;
    cons[0] = 0.125f; cons[1] = 0.5f; cons[2] = 0.125f; cons[3] = 1.0f;
    clear_both(&g, 0xFF000000u, 8, 1);
    kgpu_vs_ready(&prog, 0x7100u, 0, NULL, 0);
    kvk_wait_compiles();
    for (i = 0; i < 8; i++) {
        KGpuVsDraw gv;
        KGpuDraw d;
        for (v = 0; v < 4; v++) {
            pos[i][v][0] = (float)i + ((v & 1) ? 1.0f : 0.0f); pos[i][v][1] = (v & 2) ? 1.0f : 0.0f; pos[i][v][2] = 0; pos[i][v][3] = 1;
            val[i][v][0] = xs[i]; val[i][v][1] = val[i][v][2] = 0; val[i][v][3] = 1;
        }
        memset(&gv, 0, sizeof(gv));
        gv.program = &prog; gv.hash = 0x7100u; gv.constants = cons;
        gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)pos[i]; gv.attr[0].hi = 64;
        gv.attr[1].fmt = 4; gv.attr[1].count = 4; gv.attr[1].stride = 16; gv.attr[1].data = (const uint8_t *)val[i]; gv.attr[1].hi = 64;
        memset(&d, 0, sizeof(d));
        d.t = &g.t; d.ps = &ps; d.gvs = &gv;
        d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
        kgpu_draw(&d);
    }
    CHECK(kvk_read_surface(g.t.color_va, 0, gpu, 8 * 4, &gw, &gh, sizeof(gpu)), "readback");
    for (i = 0; i < 8; i++) {
        Nv2aVshCPUFullExecutionState es;
        Nv2aVshExecutionState s = nv2a_vsh_emu_initialize_full_execution_state(&es);
        int d = 0;
        memcpy(es.context_regs, cons, sizeof(cons));
        memcpy(&es.input_regs[0], pos[i][0], 16);
        memcpy(&es.input_regs[4], val[i][0], 16);
        nv2a_vsh_emu_execute(&s, &prog);
        for (k = 0; k < 3; k++) {
            static const int bgra[3] = { 2, 1, 0 };
            const int w8 = (int)floorf(clamp_color1(es.output_regs[3 * 4 + k]) * 255.0f + 0.5f);
            const int e = abs((int)gpu[i * 4 + bgra[k]] - w8);
            if (e > d) d = e;
        }
        if (fabsf(xs[i]) >= 1.0f) CHECK(d <= 1, "LOG(%g): GPU and emulator differ by %d", xs[i], d);
        else printf("LOG(%g): GPU exponent/8 clamped %.3f, emulator %.3f (its unsigned exponent)\n", xs[i], gpu[i * 4 + 2] / 255.0,
                    (double)clamp_color1(es.output_regs[12]));
    }
    free(g.cpu); free(g.zcpu);
}

/* every attribute format (FetchAttr) against its NV2A definition */
static void test_attribute_formats(void)
{
    Tgt g;
    KPixelState ps;
    static Nv2aVshStep st[2];
    static Nv2aVshProgram prog = { st };
    static float cons[192 * 4], pos[6][4][4];
    static uint8_t packed[6][64];
    uint8_t raw[6][16], gpu[8 * 4 * 4];
    float want[6][4];
    unsigned int gw, gh;
    int f, v, k;
    static const uint32_t fmt[6] = { 6, 1, 2, 3, 4, 5 }, cnt[6] = { 3, 4, 4, 4, 4, 4 }, stride[6] = { 4, 4, 4, 8, 16, 8 };
    tgt_init(&g, 0x03000000u, 0, 8, 4);
    ps_base(&ps, 8, 4);
    ps.num_stages = 1;
    final_passthrough(&ps, 4);
    vop(&st[0].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_XYZW); vin(&st[0].mac, 0, NV2ART_INPUT, 0);
    vop(&st[1].mac, NV2AOP_MOV, NV2ART_OUTPUT, 3, NV2AWM_XYZW); vin(&st[1].mac, 0, NV2ART_INPUT, 1);
    st[1].is_final = true;
    memset(raw, 0, sizeof(raw));
    {   /* 6 CMP: x 11 bits, y 11 bits, z 10 bits, signed */
        uint32_t w = 512u | (1023u << 11) | (255u << 22);
        memcpy(raw[0], &w, 4);
        want[0][0] = 512 / 1023.0f; want[0][1] = 1.0f; want[0][2] = 255 / 511.0f; want[0][3] = 1.0f;
    }
    raw[1][0] = 0x40; raw[1][1] = 0x80; raw[1][2] = 0xC0; raw[1][3] = 0xFF;          /* 1 UB_D3D: B G R A */
    want[1][0] = 0xC0 / 255.0f; want[1][1] = 0x80 / 255.0f; want[1][2] = 0x40 / 255.0f; want[1][3] = 1.0f;
    raw[2][0] = 0x10; raw[2][1] = 0x20; raw[2][2] = 0x30; raw[2][3] = 0xF0;          /* 2 UB_OGL: R G B A */
    want[2][0] = 0x10 / 255.0f; want[2][1] = 0x20 / 255.0f; want[2][2] = 0x30 / 255.0f; want[2][3] = 0xF0 / 255.0f;
    {   /* 3 S1: normalised shorts, -1 at the bottom */
        int16_t s[4] = { 16384, 8191, 32767, -5 };
        memcpy(raw[3], s, 8);
        want[3][0] = 16384 / 32767.0f; want[3][1] = 8191 / 32767.0f; want[3][2] = 1.0f; want[3][3] = 0.0f;
    }
    {   /* 4 F */
        float fl[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        memcpy(raw[4], fl, 16);
        memcpy(want[4], fl, 16);
    }
    {   /* 5 S32K: shorts as they are */
        int16_t s[4] = { 0, 1, 1, 0 };
        memcpy(raw[5], s, 8);
        want[5][0] = 0; want[5][1] = 1; want[5][2] = 1; want[5][3] = 0;
    }
    clear_both(&g, 0xFF000000u, 8, 4);
    kgpu_vs_ready(&prog, 0x7000u, 0, NULL, 0);
    kvk_wait_compiles();
    for (f = 0; f < 6; f++) {
        KGpuVsDraw gv;
        KGpuDraw d;
        for (v = 0; v < 4; v++) {
            pos[f][v][0] = (float)f + ((v & 1) ? 1.0f : 0.0f); pos[f][v][1] = (v & 2) ? 1.0f : 0.0f;
            pos[f][v][2] = 0.0f; pos[f][v][3] = 1.0f;
            memcpy(packed[f] + v * stride[f], raw[f], stride[f]);   /* packed at its stride: unaligned for short formats */
        }
        memset(&gv, 0, sizeof(gv));
        gv.program = &prog; gv.hash = 0x7000u; gv.constants = cons;
        gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)pos[f]; gv.attr[0].hi = 64;
        gv.attr[1].fmt = fmt[f]; gv.attr[1].count = cnt[f]; gv.attr[1].stride = stride[f];
        gv.attr[1].data = packed[f]; gv.attr[1].hi = 4 * stride[f];
        memset(&d, 0, sizeof(d));
        d.t = &g.t; d.ps = &ps; d.gvs = &gv;
        d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
        kgpu_draw(&d);
    }
    CHECK(kvk_read_surface(g.t.color_va, 0, gpu, 8 * 4, &gw, &gh, sizeof(gpu)), "readback");
    for (f = 0; f < 6; f++)
        for (k = 0; k < 4; k++) {
            static const int bgra[4] = { 2, 1, 0, 3 };
            const int w8 = (int)floorf(clamp_color1(want[f][k]) * 255.0f + 0.5f);
            CHECK(abs((int)gpu[f * 4 + bgra[k]] - w8) <= 1, "attribute format %u component %d: %d want %d", fmt[f], k, gpu[f * 4 + bgra[k]], w8);
        }
    free(g.cpu); free(g.zcpu);
}

/* depth test and write, partial and masked clears, against the CPU renderer */
static void test_depth_and_clears(void)
{
    Tgt g;
    KPixelState ps;
    Quad q;
    int bad, maxd, i, x, y;
    tgt_init(&g, 0x04000000u, 0x04100000u, 64, 48);
    clear_both(&g, 0xFF000000u, 64, 48);
    ps_base(&ps, 64, 48);
    ps.num_stages = 1;
    final_passthrough(&ps, 4);
    ps.depth_test = true; ps.depth_write = true; ps.depth_func = 1;          /* LESS */
    memset(&q, 0, sizeof(q));
    for (i = 0; i < 4; i++) q.t[i][3] = 1.0f;
    q.d0[0] = 1.0f; q.d0[3] = 1.0f;
    q.x0 = 4; q.y0 = 4; q.x1 = 40; q.y1 = 30; q.z = 0.3f * 16777215.0f;
    gpu_quad(&g, &ps, &q); cpu_quad(&g, &ps, &q);
    q.d0[0] = 0.0f; q.d0[2] = 1.0f;                                          /* behind: hidden where they overlap */
    q.x0 = 20; q.y0 = 10; q.x1 = 60; q.y1 = 44; q.z = 0.6f * 16777215.0f;
    gpu_quad(&g, &ps, &q); cpu_quad(&g, &ps, &q);
    ps.depth_func = 4;                                                       /* GREATER */
    q.d0[1] = 1.0f;
    q.x0 = 0; q.y0 = 20; q.x1 = 64; q.y1 = 24; q.z = 0.45f * 16777215.0f;
    gpu_quad(&g, &ps, &q); cpu_quad(&g, &ps, &q);
    bad = compare(&g, 64, 48, 1, "depth", &maxd);
    CHECK(bad == 0, "depth test: %d pixels differ (max %d)", bad, maxd);
    /* red and green only (channel mask B=1 G=2 R=4 A=8) over a rectangle, then a full partial clear */
    kgpu_clear(&g.t, true, false, false, 0xFF80FF80u, 0, 4 | 2, 8, 8, 24, 16, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
    for (y = 8; y < 16; y++) for (x = 8; x < 24; x++) { uint8_t *p = g.cpu + (y * 64 + x) * 4; p[2] = 0x80; p[1] = 0xFF; }
    kgpu_clear(&g.t, true, false, false, 0x11223344u, 0, 0xF, 30, 30, 50, 40, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
    {
        const uint32_t v = 0x11223344u;
        for (y = 30; y < 40; y++) for (x = 30; x < 50; x++) memcpy(g.cpu + (y * 64 + x) * 4, &v, 4);
    }
    bad = compare(&g, 64, 48, 1, "clears", &maxd);
    CHECK(bad == 0, "clears: %d pixels differ (max %d)", bad, maxd);
    free(g.cpu); free(g.zcpu);
}

/* BLACK's glare pass reads the depth buffer as an ARGB texture of packed Z24S8 words */
static void test_depth_as_color(void)
{
    Tgt a, b;
    KPixelState ps;
    Quad q;
    uint8_t px[16 * 16 * 4];
    unsigned int w, h;
    int i;
    tgt_init(&a, 0x05000000u, 0x05100000u, 16, 16);
    tgt_init(&b, 0x05200000u, 0, 16, 16);
    clear_both(&a, 0xFF000000u, 16, 16);
    clear_both(&b, 0xFF000000u, 16, 16);
    ps_base(&ps, 16, 16);
    ps.num_stages = 1;
    final_passthrough(&ps, 4);
    ps.depth_test = true; ps.depth_write = true; ps.depth_func = 7;
    memset(&q, 0, sizeof(q));
    for (i = 0; i < 4; i++) q.t[i][3] = 1.0f;
    q.x0 = 0; q.y0 = 0; q.x1 = 16; q.y1 = 16; q.z = 0.25f * 16777215.0f;
    gpu_quad(&a, &ps, &q);
    ps_base(&ps, 16, 16);
    ps.num_stages = 1;
    final_passthrough(&ps, 8);                                               /* the pixel is texture 0 */
    ps.tex_mode[0] = PS_TEXTUREMODES_PROJECT2D;
    memset(&ps.tex[0], 0, sizeof(ps.tex[0]));
    ps.tex[0].valid = true; ps.tex[0].kind = TK_8888; ps.tex[0].bpp = 4; ps.tex[0].linear = true;
    ps.tex[0].base = a.t.zeta_va; ps.tex[0].width = 16; ps.tex[0].height = 16; ps.tex[0].pitch = 16 * 4;
    ps.tex[0].levels = 1; ps.tex[0].level_w[0] = 16; ps.tex[0].level_h[0] = 16; ps.tex[0].mem = a.zcpu;
    ps.tex[0].addr[0] = ps.tex[0].addr[1] = 3; ps.tex[0].min_filter = 1; ps.tex[0].mag_filter = 1;
    ps.rect_tex[0] = true;
    q.t[0][0] = 8.5f; q.t[0][1] = 8.5f; q.t[0][3] = 1.0f;
    q.z = 0;
    gpu_quad(&b, &ps, &q);
    CHECK(kvk_read_surface(b.t.color_va, 0, px, 16 * 4, &w, &h, sizeof(px)), "readback");
    /* z24 = round(0.25 * (2^24 - 1)) = 0x400000: A = z >> 16, R = (z >> 8) & 255, G = z & 255, B = 0 */
    CHECK(px[3] == 0x40 && px[2] == 0x00 && px[1] == 0x00 && px[0] == 0x00, "depth as colour: ARGB %02X%02X%02X%02X want 40000000",
          px[3], px[2], px[1], px[0]);
    free(a.cpu); free(a.zcpu); free(b.cpu); free(b.zcpu);
}

/* ---- stereo ---- */

static VkImage s_eye_img;
static VkDeviceMemory s_eye_mem;

static void make_eye_targets(unsigned int w, unsigned int h)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkCommandBuffer cb;
    int e;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 2; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(s_dev, &ci, NULL, &s_eye_img);
    vkGetImageMemoryRequirements(s_dev, s_eye_img, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(s_dev, &ai, NULL, &s_eye_mem);
    vkBindImageMemory(s_dev, s_eye_img, s_eye_mem, 0);
    cb = one_shot_begin();
    layout(cb, s_eye_img, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    one_shot_end(cb);
    for (e = 0; e < 2; e++) {
        g_eyes[e].image = (uint64_t)(uintptr_t)s_eye_img;
        g_eyes[e].width = w; g_eyes[e].height = h;
        g_eyes[e].format = VK_FORMAT_R8G8B8A8_UNORM;
        g_eyes[e].array_layer = (uint32_t)e;
    }
    g_eye_ok = 1;
}

/* RGBA8 of one eye layer (left in COLOR_ATTACHMENT_OPTIMAL, as the OpenXR runtime expects) */
static void read_eye(int e, unsigned int w, unsigned int h, uint8_t *out)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkBufferImageCopy r;
    VkCommandBuffer cb;
    void *p;
    bi.size = (VkDeviceSize)w * h * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(s_dev, &bi, NULL, &buf);
    vkGetBufferMemoryRequirements(s_dev, buf, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(s_dev, &ai, NULL, &mem);
    vkBindBufferMemory(s_dev, buf, mem, 0);
    cb = one_shot_begin();
    layout(cb, s_eye_img, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.baseArrayLayer = (uint32_t)e; r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, s_eye_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    layout(cb, s_eye_img, 2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    one_shot_end(cb);
    vkMapMemory(s_dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
    memcpy(out, p, (size_t)w * h * 4);
    vkUnmapMemory(s_dev, mem);
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
}

/* centroid of the pixels where channel c is bright and the other two colour channels dark */
static int centroid(const uint8_t *img, unsigned int w, unsigned int h, int c, double *cx, double *cy)
{
    double sx = 0, sy = 0;
    unsigned int x, y;
    int n = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const uint8_t *p = img + (y * w + x) * 4;
            if (p[c] > 128 && p[(c + 1) % 3] < 100 && p[(c + 2) % 3] < 100) { sx += x + 0.5; sy += y + 0.5; n++; }
        }
    if (n) { *cx = sx / n; *cy = sy / n; }
    return n;
}

static void test_stereo(void)
{
    const unsigned int W = 256, H = 192;
    const double ipd = 0.064, depth = 0.5, half = 0.05;
    Tgt g;
    KPixelState ps, hud;
    static Nv2aVshStep st[9];
    static Nv2aVshProgram prog = { st };
    static float cons[192 * 4], pos[4][4], col[4][4];
    float pg[16];
    uint8_t *eye = (uint8_t *)malloc(W * H * 4), *mainpx = (uint8_t *)malloc(W * H * 4);
    unsigned int gw, gh;
    int e, k, i;
    KvkStats stats;

    /* the shell's frame: head at the origin, eyes 64 mm apart, 90 x 73.7 degrees; one game unit is a metre */
    memset(&g_fs, 0, sizeof(g_fs));
    g_fs.head.orientation.w = 1.0f;
    g_fs.world_scale = 1.0f;
    g_fs.session_running = 1;
    for (e = 0; e < 2; e++) {
        g_fs.eye[e].pose = g_fs.head;
        g_fs.eye[e].pose.position.x = (float)(e == 0 ? -ipd / 2 : ipd / 2);
        g_fs.eye[e].fov.left = -0.785398f; g_fs.eye[e].fov.right = 0.785398f;
        g_fs.eye[e].fov.up = 0.643501f; g_fs.eye[e].fov.down = -0.643501f;
    }
    /* the game camera: the same field of view, D3D convention (+z forward), at the head */
    memset(&g_cam, 0, sizeof(g_cam));
    kvk_proj_from_tangents(pg, -1.0f, 1.0f, 0.75f, -0.75f, 0.1f, 100.0f, 1.0f);
    memcpy(g_cam.proj, pg, sizeof(pg));
    kvk_mat4_identity(g_cam.view);
    g_cam.half_fov_x = 1.0f; g_cam.half_fov_y = 0.75f;
    g_cam.valid = 1;
    make_eye_targets(W, H);

    /* the Xbox D3D transform: r0 = v0 . c96..99; oPos.xyz = r0.xyz * c58 / r0.w + c59; oPos.w = r0.w; oD0 = v1 */
    memset(st, 0, sizeof(st));
    for (k = 0; k < 4; k++) { vop(&st[k].mac, NV2AOP_DP4, NV2ART_TEMPORARY, 0, (Nv2aVshWritemask)(8 >> k)); vin(&st[k].mac, 0, NV2ART_INPUT, 0); vin(&st[k].mac, 1, NV2ART_CONTEXT, 96 + k); }
    vop(&st[4].mac, NV2AOP_MUL, NV2ART_TEMPORARY, 1, NV2AWM_XYZ); vin(&st[4].mac, 0, NV2ART_TEMPORARY, 0); vin(&st[4].mac, 1, NV2ART_CONTEXT, 58);
    vop(&st[4].ilu, NV2AOP_RCC, NV2ART_TEMPORARY, 2, NV2AWM_X); vin(&st[4].ilu, 0, NV2ART_TEMPORARY, 0);
    for (k = 0; k < 4; k++) st[4].ilu.inputs[0].swizzle[k] = 3;
    vop(&st[5].mac, NV2AOP_MAD, NV2ART_OUTPUT, 0, NV2AWM_XYZ); vin(&st[5].mac, 0, NV2ART_TEMPORARY, 1); vin(&st[5].mac, 1, NV2ART_TEMPORARY, 2); vin(&st[5].mac, 2, NV2ART_CONTEXT, 59);
    for (k = 0; k < 4; k++) st[5].mac.inputs[1].swizzle[k] = 0;
    vop(&st[6].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_W); vin(&st[6].mac, 0, NV2ART_TEMPORARY, 0);
    vop(&st[7].mac, NV2AOP_MOV, NV2ART_OUTPUT, 3, NV2AWM_XYZW); vin(&st[7].mac, 0, NV2ART_INPUT, 1);
    st[7].is_final = true;
    for (k = 0; k < 4; k++) for (i = 0; i < 4; i++) cons[(96 + k) * 4 + i] = pg[i * 4 + k];        /* rows of P */
    cons[58 * 4 + 0] = 320.0f; cons[58 * 4 + 1] = -240.0f; cons[58 * 4 + 2] = 16777215.0f;
    cons[59 * 4 + 0] = 320.0f; cons[59 * 4 + 1] = 240.0f;
    for (k = 0; k < 4; k++) {
        pos[k][0] = (float)((k & 1) ? half : -half); pos[k][1] = (float)((k & 2) ? -half : half);
        pos[k][2] = (float)depth; pos[k][3] = 1.0f;
        col[k][0] = 1.0f; col[k][1] = 0.1f; col[k][2] = 0.05f; col[k][3] = 1.0f;
    }

    tgt_init(&g, 0x0A000000u, 0x0A100000u, 640, 480);
    ps_base(&ps, 640, 480);
    ps.num_stages = 1;
    final_passthrough(&ps, 4);
    ps.depth_test = true; ps.depth_write = true; ps.depth_func = 3;
    hud = ps;
    hud.depth_func = 7; hud.depth_write = false;
    hud.blend = true; hud.blend_sfactor = 4; hud.blend_dfactor = 5; hud.blend_eqn = 2;

    kgpu_vs_ready(&prog, 0x9000u, 0, NULL, 0);
    kvk_wait_compiles();
    /* frame 1: the world quad (four depth-writing draws: the world comes before a HUD is recognised), then a HUD
     * square in the top-left corner */
    kgpu_clear(&g.t, true, true, true, 0xFF000000u, 0xFFFFFF00u, 0xF, 0, 0, 640, 480, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
    for (i = 0; i < 4; i++) {
        KGpuVsDraw gv;
        KGpuDraw d;
        memset(&gv, 0, sizeof(gv));
        gv.program = &prog; gv.hash = 0x9000u; gv.constants = cons;
        gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)pos; gv.attr[0].hi = 64;
        gv.attr[1].fmt = 4; gv.attr[1].count = 4; gv.attr[1].stride = 16; gv.attr[1].data = (const uint8_t *)col; gv.attr[1].hi = 64;
        memset(&d, 0, sizeof(d));
        d.t = &g.t; d.ps = &ps; d.gvs = &gv;
        d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
        kgpu_draw(&d);
    }
    {
        Quad q;
        memset(&q, 0, sizeof(q));
        for (k = 0; k < 4; k++) q.t[k][3] = 1.0f;
        q.x0 = 0; q.y0 = 0; q.x1 = 64; q.y1 = 48;
        q.d0[1] = 1.0f; q.d0[3] = 1.0f;
        gpu_quad(&g, &hud, &q);
    }
    for (e = 0; e < 2; e++) {
        /* the main target's layers, before the composition: the quad where the eye sees it */
        double cx = 0, cy = 0, want = (0.5 + 0.5 * (e == 0 ? 1 : -1) * (ipd / 2) / depth) * W;
        int n;
        CHECK(kvk_read_surface(g.t.color_va, (unsigned)e, mainpx, W * 4, &gw, &gh, W * H * 4), "main layer %d readback", e);
        n = centroid(mainpx, W, H, 2, &cx, &cy);            /* BGRA: red is byte 2 */
        CHECK(n > 100, "eye %d: world quad drawn (%d px)", e, n);
        CHECK(fabs(cx - want) < 1.0 && fabs(cy - H / 2.0) < 1.0, "eye %d: world quad centre %.2f,%.2f want %.2f,%.2f", e, cx, cy, want, H / 2.0);
        printf("stereo: layer %d world quad centre x %.2f (trigonometry: %.2f), %d px\n", e, cx, want, n);
        CHECK(mainpx[(10 * W + 10) * 4 + 1] < 50, "eye %d: the HUD did not go into the world target", e);
    }
    kgpu_flip(&g.t, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 0, 0);
    CHECK(g_released == 1, "eye targets released after the flip (%d)", g_released);
    kvk_read_surface(g.t.color_va, 0, mainpx, W * 4, &gw, &gh, W * H * 4);     /* waits for the GPU */
    for (e = 0; e < 2; e++) {
        double cx = 0, cy = 0, want = (0.5 + 0.5 * (e == 0 ? 1 : -1) * (ipd / 2) / depth) * W;
        int n;
        read_eye(e, W, H, eye);
        n = centroid(eye, W, H, 0, &cx, &cy);               /* RGBA */
        CHECK(n > 100 && fabs(cx - want) < 1.0, "eye image %d: world quad centre %.2f want %.2f (%d px)", e, cx, want, n);
        n = centroid(eye, W, H, 1, &cx, &cy);
        {
            /* the virtual screen is 2.4 m wide at 2 m: the HUD square (the top-left tenth) spans tan x -0.6..-0.48,
             * tan y 0.45..0.36, plus the eye's parallax at 2 m */
            const double par = (e == 0 ? 1 : -1) * (ipd / 2) / 2.0;
            const double wx = (0.5 + 0.5 * (-0.54 + par)) * W, wy = (0.5 - 0.5 * (0.405 / 0.75)) * H;
            CHECK(n > 20 && fabs(cx - wx) < 1.5 && fabs(cy - wy) < 1.5, "eye image %d: HUD centre %.2f,%.2f want %.2f,%.2f (%d px)", e, cx, cy, wx, wy, n);
            printf("stereo: eye image %d HUD centre %.2f,%.2f (expected %.2f,%.2f)\n", e, cx, cy, wx, wy);
        }
    }
    /* frame 2: no world, a full-screen blue picture (a menu): shown on the virtual screen */
    {
        KPixelState flat = ps;
        Quad q;
        flat.depth_test = false; flat.depth_write = false;
        memset(&q, 0, sizeof(q));
        for (k = 0; k < 4; k++) q.t[k][3] = 1.0f;
        q.x0 = 0; q.y0 = 0; q.x1 = 640; q.y1 = 480;
        q.d0[2] = 1.0f; q.d0[3] = 1.0f;
        gpu_quad(&g, &flat, &q);
        kgpu_flip(&g.t, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 0, 0);
        kvk_read_surface(g.t.color_va, 0, mainpx, W * 4, &gw, &gh, W * H * 4);
        for (e = 0; e < 2; e++) {
            read_eye(e, W, H, eye);
            CHECK(eye[(H / 2 * W + W / 2) * 4 + 2] > 200 && eye[(H / 2 * W + W / 2) * 4] < 50, "menu frame: eye %d centre shows the picture", e);
            CHECK(eye[(2 * W + 2) * 4 + 2] < 20, "menu frame: eye %d corner outside the virtual screen is black", e);
        }
    }
    kvk_get_stats(&stats);
    printf("stereo: draws %llu (stereo %llu, hud %llu), render passes %llu, pipelines %llu\n", (unsigned long long)stats.draws,
           (unsigned long long)stats.stereo_draws, (unsigned long long)stats.hud_draws, (unsigned long long)stats.passes,
           (unsigned long long)stats.pipelines);
    CHECK(stats.stereo_draws == 4 && stats.hud_draws == 1, "draw classification (stereo %llu, hud %llu)",
          (unsigned long long)stats.stereo_draws, (unsigned long long)stats.hud_draws);
    free(eye); free(mainpx); free(g.cpu); free(g.zcpu);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "cpu-parity";
    bool bc = false;
    char dir[256];
    if (!make_device(&bc, strcmp(mode, "uber") != 0)) { printf("no Vulkan 1.1 device with multiview: skipped\n"); return 77; }
    snprintf(dir, sizeof(dir), "kvk-test-cache-%s", mode);
    kvk_set_cache_dir(dir);
    ktex_init();
    srand(3);
    make_textures();
    if (!strcmp(mode, "stereo")) {
        g_vr_on = 1;
        kvk_set_stereo(true);
        kvk_set_raster_size(256, 192);
    } else {
        kvk_set_stereo(false);
        kvk_set_raster_size(640, 480);
    }
    if (!strcmp(mode, "uber")) setenv("RECOMP_KGPU_PS_SPEC", "0", 1);
    if (!kgpu_init()) { printf("kgpu_init failed\n"); return 1; }
    kgpu_note_kick();
    if (!strcmp(mode, "stereo")) {
        test_stereo();
    } else if (!strcmp(mode, "uber")) {
        printf("device BC textures: %s\n", bc ? "native" : "decoded on the CPU");
        test_combiners(false, true);
        test_combiners(false, false);
    } else if (!strcmp(mode, "attr")) {
        test_attribute_formats();
    } else if (!strcmp(mode, "comb")) {
        test_combiners(true, true);
    } else if (!strcmp(mode, "vprog")) {
        test_vertex_programs();
    } else {
        printf("device BC textures: %s\n", bc ? "native" : "decoded on the CPU");
        test_combiners(true, true);
        test_combiners(true, false);
        test_vertex_programs();
        test_log();
        test_attribute_formats();
        test_depth_and_clears();
        test_depth_as_color();
    }
    kgpu_report();
    kvk_shutdown();
    printf("kvk-backend %s: %d checks, %d failed\n", mode, s_checks, s_fail);
    return s_fail ? 1 : 0;
}
