/*
 * xr_panel.c - the in-headset settings panel: OpenXR quad layers, input routing, live apply, blackvr.ini save
 * (port/notes/settings-panel.md). The UI and its drawing are vr_panel_ui.cpp (Dear ImGui); the pure parts are
 * vr_settings.h (model, ini writer) and vr_panel_math.h (placement, laser, open/close).
 *
 * Everything runs on the XR thread (xr_session.c calls the XrsPanelOps):
 *   input  (input_sync, before vrb_set_input): the open click / close buttons, the laser, the scroll; while open
 *          the game gets a neutral controller (it stays paused) and the buttons that closed the panel stay hidden
 *          from it until released.
 *   draw   (after xrBeginFrame): one UI frame into the panel swapchain: own command pool, 2 command buffers with
 *          fences, submitted under vr_queue_lock like the shell's test pattern; the image is released at once.
 *   layers (under the queue lock, before xrEndFrame): the panel quad (world-locked where it opened) and the laser
 *          beam quad, after every other layer, so the compositor draws them on top.
 * Closed, none of this does any work beyond the open-click check: nothing is drawn, acquired or submitted, and the
 * swapchains and Vulkan objects are made the first time it opens.
 */
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "xr_shell.h"
#include "vr_panel.h"
#include "vr_panel_math.h"
#include "vr_settings_menu.h"

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

#ifndef BLACKVR_VERSION
#define BLACKVR_VERSION "dev"
#endif

#define PANEL_W 1024u
#define PANEL_H 768u
#define LASER_W 8u
#define LASER_H 64u

static struct {
    int inited, init_failed;
    XrsSwapchain sc, laser;
    int released, laser_released;
    VkCommandPool pool;
    VkCommandBuffer cmd[2], laser_cmd;
    VkFence fence[2], laser_fence;
    int slot;
    VrpQuad quad;
    VrpInput in;
    VrpStep step;
    VrTouchState touch;                   /* the controllers as read this frame */
    VrPose aim, head;
    int aim_valid, mouse_valid;
    float mouse_x, mouse_y, laser_len;
    double dt;
    VrShellConfig orig;                   /* Cancel goes back here (the state at open) */
    XrCompositionLayerQuad q[2];
} P;

static VkFormat unorm_view_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

static XrPosef to_xr(VrPose p)
{
    XrPosef r;
    r.orientation.x = p.orientation.x; r.orientation.y = p.orientation.y;
    r.orientation.z = p.orientation.z; r.orientation.w = p.orientation.w;
    r.position.x = p.position.x; r.position.y = p.position.y; r.position.z = p.position.z;
    return r;
}

/* ---- Vulkan / swapchains (the first time it opens) ---- */

static int panel_init(XrShell *s)
{
    VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkCommandBuffer cbs[3];
    VrpVkInit vi;
    int i;
    if (P.inited) return 1;
    if (P.init_failed) return 0;
    P.init_failed = 1;
    if (!s->vk_device || !s->session) return 0;
    /* made mid-session, while the renderer may be submitting: under the queue lock in case the runtime touches the
     * queue to create the images (recursive; xrs_swapchain_create takes nothing else) */
    {
        int ok;
        vr_queue_lock();
        ok = xrs_swapchain_create(s, &P.sc, PANEL_W, PANEL_H, 1) && xrs_swapchain_create(s, &P.laser, LASER_W, LASER_H, 1);
        vr_queue_unlock();
        if (!ok) { XLOGE("settings panel: swapchain refused"); return 0; }
    }
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = s->vk_queue_family;
    if (vkCreateCommandPool(s->vk_device, &cp, NULL, &P.pool) != VK_SUCCESS) return 0;
    ca.commandPool = P.pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 3;
    if (vkAllocateCommandBuffers(s->vk_device, &ca, cbs) != VK_SUCCESS) return 0;
    P.cmd[0] = cbs[0]; P.cmd[1] = cbs[1]; P.laser_cmd = cbs[2];
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (i = 0; i < 2; i++) if (vkCreateFence(s->vk_device, &fi, NULL, &P.fence[i]) != VK_SUCCESS) return 0;
    if (vkCreateFence(s->vk_device, &fi, NULL, &P.laser_fence) != VK_SUCCESS) return 0;
    vi.phys = s->vk_phys;
    vi.device = s->vk_device;
    vi.format = unorm_view_format(s->color_format);
    if (!vrp_ui_init(&vi)) { XLOGE("settings panel: UI init failed"); return 0; }
    P.inited = 1;
    P.init_failed = 0;
    XLOGI("settings panel ready: %ux%u swapchain, %.2f x %.2f m quad", PANEL_W, PANEL_H, VRP_WIDTH_M,
          VRP_WIDTH_M * PANEL_H / PANEL_W);
    return 1;
}

static void submit(XrShell *s, VkCommandBuffer cmd, VkFence fence)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vr_queue_lock();
    vkQueueSubmit(s->vk_queue, 1, &si, fence);
    vr_queue_unlock();
}

/* the beam's image: one colour, premultiplied (the shell's red, 85% opaque); drawn once */
static void laser_fill(XrShell *s)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkClearColorValue c;
    uint32_t im;
    if (P.laser_released || !xrs_swapchain_acquire(s, &P.laser, &im)) return;
    vkWaitForFences(s->vk_device, 1, &P.laser_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(s->vk_device, 1, &P.laser_fence);
    vkResetCommandBuffer(P.laser_cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(P.laser_cmd, &bi);
    mb.srcAccessMask = 0;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = P.laser.images[im];
    mb.subresourceRange = rr;
    vkCmdPipelineBarrier(P.laser_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    /* sRGB image, cleared through its own (sRGB) format: linear values; (0.9, 0.08, 0.06) display red x 0.85 */
    c.float32[0] = 0.66f; c.float32[1] = 0.006f; c.float32[2] = 0.003f; c.float32[3] = 0.85f;
    vkCmdClearColorImage(P.laser_cmd, P.laser.images[im], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rr);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(P.laser_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    vkEndCommandBuffer(P.laser_cmd);
    submit(s, P.laser_cmd, P.laser_fence);
    xrs_swapchain_release(s, &P.laser);
    P.laser_released = 1;
}

/* ---- the ops ---- */

static void on_open(XrShell *s)
{
    VrSettingsState *S = vrsm();
    vrsm_lock();
    vrsm_begin_edit(s);
    P.orig = S->edit;
    S->status[0] = 0;
    vrsm_unlock();
    P.quad = vrp_place(P.head, (float)PANEL_W / (float)PANEL_H);
    P.released = 0;
    vrsm_sampler(1);
    XLOGI("settings panel open at %.2f %.2f %.2f (head %.2f %.2f %.2f)", P.quad.pose.position.x, P.quad.pose.position.y,
          P.quad.pose.position.z, P.head.position.x, P.head.position.y, P.head.position.z);
}

static int op_input(XrShell *s, VrTouchState *touch, const VrPose aim[2], const int valid[2], double dt)
{
    VrTouchState game;
    const int gun = s->cfg.game.aim_hand & 1;
    const int was = P.in.open;
    P.touch = *touch;
    P.dt = dt;
    vrb_head_now(&P.head);
    /* blackvr.ini settings_menu = panel | both: the floating panel opens from any menu */
    vrp_input_step(&P.in, touch, s->cfg.input.left_handed,
                   vrb_menu_context() && s->cfg.settings_menu != VR_SETTINGS_MENU_NATIVE, dt, &game, &P.step);
    *touch = game;
    if (P.step.opened) {
        if (!panel_init(s)) { P.in.open = 0; P.in.block = vri_touch_mask(&P.touch); return 0; }
        on_open(s);
    }
    if (was && !P.in.open) { vrsm_sampler(0); XLOGI("settings panel closed"); }
    P.aim = aim[gun];
    P.aim_valid = valid[gun];
    P.mouse_valid = 0;
    P.laser_len = VRP_LASER_MISS_M;
    if (P.in.open && P.aim_valid) {
        float u, v, t;
        if (vrp_hit(&P.quad, P.aim, &u, &v, &t)) {
            P.mouse_valid = 1;
            P.mouse_x = u * (float)PANEL_W;
            P.mouse_y = v * (float)PANEL_H;
            P.laser_len = t;
        } else if (t > 0.0f && t < VRP_LASER_MISS_M) {
            P.laser_len = t;
        }
    }
    return P.in.open;
}

static void op_draw(XrShell *s)
{
    VrpUiIn ui;
    VrpUiOut out;
    VrSettingsState *S = vrsm();
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    uint32_t im;
    if (!P.in.open || !P.inited) return;
    vrsm_lock();
    vrsm_info_update(s);
    if (S->status[0] && mono_now() - S->status_t > 6.0) S->status[0] = 0;
    vrsm_caps(s);
    memset(&ui, 0, sizeof(ui));
    ui.width = PANEL_W;
    ui.height = PANEL_H;
    ui.mouse_x = P.mouse_x;
    ui.mouse_y = P.mouse_y;
    ui.mouse_valid = P.mouse_valid;
    ui.mouse_down = P.step.mouse_down;
    ui.wheel = P.step.wheel;
    ui.dt = P.dt;
    ui.edit = &S->edit;
    ui.boot = &S->boot;
    ui.saved = &S->saved;
    ui.caps = &S->caps;
    ui.info = &S->info;
    ui.status = S->status;
    S->info.mode = vrb_menu_context() ? "menu" : "gameplay";
    vrp_ui_frame(&ui, &out);
    if (out.changed) vrsm_apply_live(s);
    if (out.cancel) {
        S->edit = P.orig;
        vrsm_apply_live(s);
        vrsm_status("Changes undone");
    }
    if (out.reset) {
        vrs_reset_defaults(&S->edit);
        vrsm_apply_live(s);
        vrsm_status("Defaults (not saved yet)");
    }
    if (out.save) vrsm_save(s);
    if (out.recalibrate) {
        vr_request_recentre();
        vrsm_status("Height recalibrated, view recentred");
    }
    vrsm_unlock();
    vrsm_xr_frame(s);                         /* this frame's refresh / foveation / performance changes */
    if (out.close) {
        vrp_input_close(&P.in, &P.touch);
        vrsm_sampler(0);
        XLOGI("settings panel closed (Close button)");
        return;
    }
    laser_fill(s);
    if (!xrs_swapchain_acquire(s, &P.sc, &im)) return;
    vkWaitForFences(s->vk_device, 1, &P.fence[P.slot], VK_TRUE, UINT64_MAX);
    vkResetFences(s->vk_device, 1, &P.fence[P.slot]);
    vkResetCommandBuffer(P.cmd[P.slot], 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(P.cmd[P.slot], &bi);
    vrp_ui_record(P.cmd[P.slot], P.sc.layer_views[im][0], PANEL_W, PANEL_H, P.slot);
    vkEndCommandBuffer(P.cmd[P.slot]);
    submit(s, P.cmd[P.slot], P.fence[P.slot]);
    xrs_swapchain_release(s, &P.sc);
    P.slot ^= 1;
    P.released = 1;
}

static uint32_t op_layers(XrShell *s, const XrCompositionLayerBaseHeader **out, uint32_t cap)
{
    uint32_t n = 0;
    if (!P.in.open || !P.released || cap < 1) return 0;
    memset(P.q, 0, sizeof(P.q));
    P.q[0].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    P.q[0].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   /* premultiplied */
    P.q[0].space = s->world_space;
    P.q[0].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    P.q[0].subImage.swapchain = P.sc.handle;
    P.q[0].subImage.imageRect.extent.width = (int32_t)P.sc.width;
    P.q[0].subImage.imageRect.extent.height = (int32_t)P.sc.height;
    P.q[0].pose = to_xr(P.quad.pose);
    P.q[0].size.width = P.quad.width_m;
    P.q[0].size.height = P.quad.height_m;
    out[n++] = (const XrCompositionLayerBaseHeader *)&P.q[0];
    if (P.aim_valid && P.laser_released && cap >= 2) {
        float w, h;
        const VrPose lp = vrp_laser_pose(P.aim, P.laser_len, P.head.position, &w, &h);
        P.q[1] = P.q[0];
        P.q[1].subImage.swapchain = P.laser.handle;
        P.q[1].subImage.imageRect.extent.width = (int32_t)P.laser.width;
        P.q[1].subImage.imageRect.extent.height = (int32_t)P.laser.height;
        P.q[1].pose = to_xr(lp);
        P.q[1].size.width = w;
        P.q[1].size.height = h;
        out[n++] = (const XrCompositionLayerBaseHeader *)&P.q[1];
    }
    return n;
}

static void op_destroy(XrShell *s)
{
    int i;
    vrsm_sampler(0);
    if (!P.inited || !s->vk_device) return;
    vkWaitForFences(s->vk_device, 2, P.fence, VK_TRUE, UINT64_MAX);
    vkWaitForFences(s->vk_device, 1, &P.laser_fence, VK_TRUE, UINT64_MAX);
    vrp_ui_shutdown();
    for (i = 0; i < 2; i++) vkDestroyFence(s->vk_device, P.fence[i], NULL);
    vkDestroyFence(s->vk_device, P.laser_fence, NULL);
    vkDestroyCommandPool(s->vk_device, P.pool, NULL);
    xrs_swapchain_destroy(s, &P.sc);
    xrs_swapchain_destroy(s, &P.laser);
    P.inited = 0;
}


/* ---- the laser sight (blackvr.ini laser; VR SETTINGS > Comfort) ------------------------------------------------------
 * A real-looking laser sight: the beam barely there (a red laser in the air shows only faintly, more near the muzzle),
 * soft-edged and fading along its length; the dot small and bright with a soft glow where the shot goes. From the
 * drawn gun's muzzle along its barrel (the game thread publishes both in the gun hand's aim space and the length:
 * vr_set_gun_laser), placed from the same latched hand the drawn gun is (VrRenderGun.hand: two-handed aim included).
 * Composition quads, drawn over the scene (not hidden by walls). One 64x128 swapchain, painted once: the beam's
 * texture in x 0..15 (alpha falling along y), the dot's in x 32..63, y 0..31. */
#define GL_W 64u
#define GL_H 128u
static struct {
    int inited, failed, filled;
    XrsSwapchain sc;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buf;
    VkDeviceMemory mem;
    XrCompositionLayerQuad q[2];
} G;

static int gun_laser_init(XrShell *s)
{
    VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements mr;
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    int ok;
    if (G.inited) return 1;
    if (G.failed || !s->vk_device || !s->session) return 0;
    G.failed = 1;
    vr_queue_lock();
    ok = xrs_swapchain_create(s, &G.sc, GL_W, GL_H, 1);
    vr_queue_unlock();
    if (!ok) { XLOGE("laser sight: swapchain refused"); return 0; }
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = s->vk_queue_family;
    if (vkCreateCommandPool(s->vk_device, &cp, NULL, &G.pool) != VK_SUCCESS) return 0;
    ca.commandPool = G.pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(s->vk_device, &ca, &G.cmd) != VK_SUCCESS) return 0;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(s->vk_device, &fi, NULL, &G.fence) != VK_SUCCESS) return 0;
    bc.size = GL_W * GL_H * 4u;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s->vk_device, &bc, NULL, &G.buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(s->vk_device, G.buf, &mr);
    vkGetPhysicalDeviceMemoryProperties(s->vk_phys, &mp);
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = UINT32_MAX;
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { ma.memoryTypeIndex = i; break; }
    if (ma.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(s->vk_device, &ma, NULL, &G.mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(s->vk_device, G.buf, G.mem, 0) != VK_SUCCESS) return 0;
    G.inited = 1;
    G.failed = 0;
    XLOGI("laser sight ready");
    return 1;
}

/* sRGB encode of a linear 0..1 value */
static uint8_t gl_srgb(float v)
{
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(v * 255.0f + 0.5f);
}

/* the textures, premultiplied (linear colour x alpha, sRGB-encoded for an sRGB swapchain) */
static void gl_paint(uint8_t *px, int srgb, int bgra)
{
    const float red[3] = { 1.0f, 0.03f, 0.02f };
    uint32_t x, y;
    memset(px, 0, GL_W * GL_H * 4u);
    for (y = 0; y < GL_H; y++)
        for (x = 0; x < GL_W; x++) {
            float a = 0.0f, core = 0.0f, c[3];
            int k;
            if (x < 16u) {
                /* the beam: across, a soft line (gaussian); along, faint and fading out from the muzzle (the bottom
                 * row: a quad's image top is its +y edge, the beam's far end) */
                const float u = ((float)x + 0.5f) / 16.0f - 0.5f, v = 1.0f - ((float)y + 0.5f) / (float)GL_H;
                a = expf(-u * u / (2.0f * 0.13f * 0.13f)) * (0.22f * (1.0f - v) * (1.0f - v) + 0.04f * (1.0f - v));
            } else if (x >= 32u && y < 32u) {
                /* the dot: a hot small core in a soft red glow */
                const float dx = ((float)(x - 32u) + 0.5f) / 32.0f - 0.5f, dy = ((float)y + 0.5f) / 32.0f - 0.5f;
                const float r2 = dx * dx + dy * dy;
                core = expf(-r2 / (2.0f * 0.045f * 0.045f));
                a = core + 0.45f * expf(-r2 / (2.0f * 0.14f * 0.14f));
                if (a > 1.0f) a = 1.0f;
            }
            for (k = 0; k < 3; k++) c[k] = (red[k] + (1.0f - red[k]) * 0.55f * core) * a;   /* the core whitens a little */
            {
                uint8_t *o = px + (y * GL_W + x) * 4u;
                const uint8_t r = srgb ? gl_srgb(c[0]) : (uint8_t)(c[0] * 255.0f + 0.5f);
                const uint8_t g = srgb ? gl_srgb(c[1]) : (uint8_t)(c[1] * 255.0f + 0.5f);
                const uint8_t b = srgb ? gl_srgb(c[2]) : (uint8_t)(c[2] * 255.0f + 0.5f);
                o[0] = bgra ? b : r; o[1] = g; o[2] = bgra ? r : b;
                o[3] = (uint8_t)(a * 255.0f + 0.5f);
            }
        }
}

static void gun_laser_fill(XrShell *s)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkBufferImageCopy bic;
    void *map;
    uint32_t im;
    if (G.filled || !xrs_swapchain_acquire(s, &G.sc, &im)) return;
    if (vkMapMemory(s->vk_device, G.mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) { xrs_swapchain_release(s, &G.sc); return; }
    gl_paint((uint8_t *)map, s->color_format == VK_FORMAT_R8G8B8A8_SRGB || s->color_format == VK_FORMAT_B8G8R8A8_SRGB,
             s->color_format == VK_FORMAT_B8G8R8A8_SRGB || s->color_format == VK_FORMAT_B8G8R8A8_UNORM);
    vkUnmapMemory(s->vk_device, G.mem);
    vkWaitForFences(s->vk_device, 1, &G.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(s->vk_device, 1, &G.fence);
    vkResetCommandBuffer(G.cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(G.cmd, &bi);
    mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = G.sc.images[im];
    mb.subresourceRange = rr;
    vkCmdPipelineBarrier(G.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    memset(&bic, 0, sizeof(bic));
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = GL_W;
    bic.imageExtent.height = GL_H;
    bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(G.cmd, G.buf, G.sc.images[im], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(G.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    vkEndCommandBuffer(G.cmd);
    submit(s, G.cmd, G.fence);
    xrs_swapchain_release(s, &G.sc);
    G.filled = 1;
}

static VrPose from_xr(XrPosef p)
{
    VrPose v;
    v.position = vrm_v3(p.position.x, p.position.y, p.position.z);
    v.orientation.x = p.orientation.x; v.orientation.y = p.orientation.y;
    v.orientation.z = p.orientation.z; v.orientation.w = p.orientation.w;
    return v;
}

uint32_t xrs_gun_laser_layers(XrShell *s, XrTime t, const XrCompositionLayerBaseHeader **out, uint32_t cap)
{
    const VrGameConfig *cfg = vr_game_config();
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    const VrRenderGun *g = vr_render_gun();
    XrSpaceLocation vl = { XR_TYPE_SPACE_LOCATION };
    float pm[3], am[3], len, w, h;
    VrPose hand, head, aim;
    VrVec3 muzzle, dir, up, endp;
    if (!cfg->laser || cap < 2 || !vr_gun_laser(pm, am, &len) || !g->valid || !s->view_space) return 0;
    if (!gun_laser_init(s)) return 0;
    gun_laser_fill(s);
    if (!G.filled) return 0;
    if (XR_FAILED(xrLocateSpace(s->view_space, s->world_space, t, &vl)) || (vl.locationFlags & need) != need) return 0;
    hand = g->hand;                                       /* the drawn gun's hand: latched, two-handed aim included */
    head = from_xr(vl.pose);
    muzzle = vrm_add(hand.position, vrm_q_rotate(hand.orientation, vrm_v3(pm[0], pm[1], pm[2])));
    dir = vrm_q_rotate(hand.orientation, vrm_v3(am[0], am[1], am[2]));
    if (!vrm_normalize(&dir)) return 0;
    up = vrm_q_rotate(hand.orientation, vrm_v3(0, 1, 0));
    aim.position = muzzle;
    aim.orientation = vrg_look_rotation(dir, up);
    memset(G.q, 0, sizeof(G.q));
    G.q[0].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    G.q[0].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    G.q[0].space = s->world_space;
    G.q[0].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    G.q[0].subImage.swapchain = G.sc.handle;
    /* the beam: the quad's +y runs from the muzzle out (vrp_laser_pose); its image is painted bright at the bottom */
    G.q[0].subImage.imageRect.offset.x = 0;
    G.q[0].subImage.imageRect.extent.width = 16;
    G.q[0].subImage.imageRect.extent.height = (int32_t)GL_H;
    G.q[0].pose = to_xr(vrp_laser_pose(aim, len, head.position, &w, &h));
    G.q[0].size.width = 0.006f;                           /* the soft edges: the visible line ~1.5 mm */
    G.q[0].size.height = h;
    out[0] = (const XrCompositionLayerBaseHeader *)&G.q[0];
    /* the dot: facing the eye, about 0.35 deg across with its glow */
    endp = vrm_add(muzzle, vrm_scale(dir, len));
    G.q[1] = G.q[0];
    G.q[1].subImage.imageRect.offset.x = 32;
    G.q[1].subImage.imageRect.offset.y = 0;
    G.q[1].subImage.imageRect.extent.width = 32;
    G.q[1].subImage.imageRect.extent.height = 32;
    {
        VrVec3 f = vrm_sub(endp, head.position);
        const float d = vrm_len(f);
        VrPose dp;
        if (!vrm_normalize(&f)) return 1;
        dp.position = endp;
        dp.orientation = vrg_look_rotation(f, vrm_q_rotate(head.orientation, vrm_v3(0, 1, 0)));
        G.q[1].pose = to_xr(dp);
        G.q[1].size.width = G.q[1].size.height = d * 0.006f < 0.006f ? 0.006f : d * 0.006f;
    }
    out[1] = (const XrCompositionLayerBaseHeader *)&G.q[1];
    return 2;
}

static const XrsPanelOps k_ops = { op_input, op_draw, op_layers, op_destroy };

const XrsPanelOps *xrs_panel_attach(XrShell *s)
{
    /* after blackvr.ini was read (xr_main.c load_config): what the process runs with */
    vrsm_attach(s);
    return &k_ops;
}

/* ---- tests (tests/vr_glue) ---- */
int xrs_panel_test_state(XrsPanelTestState *st)
{
    memset(st, 0, sizeof(*st));
    st->open = P.in.open;
    st->inited = P.inited;
    st->released = P.released;
    st->panel = P.sc.handle;
    st->laser = P.laser.handle;
    st->quad_pose = P.quad.pose;
    st->width_m = P.quad.width_m;
    st->height_m = P.quad.height_m;
    st->mouse_valid = P.mouse_valid;
    st->mouse_x = P.mouse_x;
    st->mouse_y = P.mouse_y;
    st->laser_len = P.laser_len;
    snprintf(st->status, sizeof(st->status), "%s", vrsm()->status);
    return 1;
}
