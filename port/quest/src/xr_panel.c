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
/* A small texture painted once on the CPU (paint: premultiplied RGBA8 rows, sRGB-encoded for an sRGB swapchain,
 * BGRA for a BGRA one) into its own one-image swapchain, for composition quads. */
typedef struct {
    const char *name;
    uint32_t w, h;
    void (*paint)(uint8_t *px, int srgb, int bgra);
    int inited, failed, filled;
    XrsSwapchain sc;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buf;
    VkDeviceMemory mem;
} PaintedTex;

static int ptex_init(XrShell *s, PaintedTex *T)
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
    if (T->inited) return 1;
    if (T->failed || !s->vk_device || !s->session) return 0;
    T->failed = 1;
    vr_queue_lock();
    ok = xrs_swapchain_create(s, &T->sc, T->w, T->h, 1);
    vr_queue_unlock();
    if (!ok) { XLOGE("%s: swapchain refused", T->name); return 0; }
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = s->vk_queue_family;
    if (vkCreateCommandPool(s->vk_device, &cp, NULL, &T->pool) != VK_SUCCESS) return 0;
    ca.commandPool = T->pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(s->vk_device, &ca, &T->cmd) != VK_SUCCESS) return 0;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(s->vk_device, &fi, NULL, &T->fence) != VK_SUCCESS) return 0;
    bc.size = T->w * T->h * 4u;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s->vk_device, &bc, NULL, &T->buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(s->vk_device, T->buf, &mr);
    vkGetPhysicalDeviceMemoryProperties(s->vk_phys, &mp);
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = UINT32_MAX;
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { ma.memoryTypeIndex = i; break; }
    if (ma.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(s->vk_device, &ma, NULL, &T->mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(s->vk_device, T->buf, T->mem, 0) != VK_SUCCESS) return 0;
    T->inited = 1;
    T->failed = 0;
    XLOGI("%s ready", T->name);
    return 1;
}

static void ptex_fill(XrShell *s, PaintedTex *T)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkBufferImageCopy bic;
    void *map;
    uint32_t im;
    if (T->filled || !xrs_swapchain_acquire(s, &T->sc, &im)) return;
    if (vkMapMemory(s->vk_device, T->mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) { xrs_swapchain_release(s, &T->sc); return; }
    T->paint((uint8_t *)map, s->color_format == VK_FORMAT_R8G8B8A8_SRGB || s->color_format == VK_FORMAT_B8G8R8A8_SRGB,
             s->color_format == VK_FORMAT_B8G8R8A8_SRGB || s->color_format == VK_FORMAT_B8G8R8A8_UNORM);
    vkUnmapMemory(s->vk_device, T->mem);
    vkWaitForFences(s->vk_device, 1, &T->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(s->vk_device, 1, &T->fence);
    vkResetCommandBuffer(T->cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(T->cmd, &bi);
    mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = T->sc.images[im];
    mb.subresourceRange = rr;
    vkCmdPipelineBarrier(T->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    memset(&bic, 0, sizeof(bic));
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = T->w;
    bic.imageExtent.height = T->h;
    bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(T->cmd, T->buf, T->sc.images[im], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(T->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    vkEndCommandBuffer(T->cmd);
    submit(s, T->cmd, T->fence);
    xrs_swapchain_release(s, &T->sc);
    T->filled = 1;
}


/* (The laser sight is drawn in the world by the renderer since 2026-11-15: vr_renderer_kvk.c laser_fill.) */

/* ---- the 3D scope view's surround (port/notes/scope.md) ------------------------------------------------------------
 * The renderer draws the zoomed world over the whole view (vr_renderer_kvk.c scope_eyes) and the game draws its own
 * scope (sniperhud: the lens vignette, reticle and X2/X5 marks) on the HUD layer, which xr_session.c then centres on
 * the view (XRS_SCOPE_HUD_*). That picture ends at the HUD quad's edges (tan +-0.80 x +-0.60); this black surround,
 * under the HUD layer, covers the rest of the view: transparent to tan 0.51, black by tan 0.58, out to tan +-2
 * (63 deg). One 512x512 swapchain, painted once. */
#define SR_TEX 512u
#define SR_APERTURE_TAN (0.633f * XRS_SCOPE_HUD_TAN)   /* black by 0.73x, inside the quad's 0.75x height */
#define SR_RING_TAN 2.0f

static void sr_paint(uint8_t *px, int srgb, int bgra)
{
    const float half = (float)SR_TEX * 0.5f, r_ap = SR_APERTURE_TAN / SR_RING_TAN;
    uint32_t x, y;
    (void)srgb; (void)bgra;             /* black: the same bytes in every format */
    for (y = 0; y < SR_TEX; y++)
        for (x = 0; x < SR_TEX; x++) {
            const float u = ((float)x + 0.5f - half) / half, v = ((float)y + 0.5f - half) / half;
            float t = (sqrtf(u * u + v * v) - r_ap) / (r_ap * 0.15f);
            uint8_t *o = px + (y * SR_TEX + x) * 4u;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            o[0] = o[1] = o[2] = 0;
            o[3] = (uint8_t)(t * t * (3.0f - 2.0f * t) * 255.0f + 0.5f);
        }
}

static struct {
    PaintedTex t;
    XrCompositionLayerQuad q;
} SR = { { "scope surround", SR_TEX, SR_TEX, sr_paint } };

uint32_t xrs_scope_layers(XrShell *s, const XrCompositionLayerBaseHeader **out, uint32_t cap)
{
    const float dist = 10.0f;
    if (cap < 1 || !(vr_scope_view() > 1.0f) || !s->view_space || !ptex_init(s, &SR.t)) return 0;
    ptex_fill(s, &SR.t);
    if (!SR.t.filled) return 0;
    memset(&SR.q, 0, sizeof(SR.q));
    SR.q.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    SR.q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    SR.q.space = s->view_space;
    SR.q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    SR.q.subImage.swapchain = SR.t.sc.handle;
    SR.q.subImage.imageRect.extent.width = (int32_t)SR_TEX;
    SR.q.subImage.imageRect.extent.height = (int32_t)SR_TEX;
    SR.q.pose.orientation.w = 1.0f;
    SR.q.pose.position.z = -dist;
    SR.q.size.width = SR.q.size.height = 2.0f * SR_RING_TAN * dist;
    out[0] = (const XrCompositionLayerBaseHeader *)&SR.q;
    return 1;
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
