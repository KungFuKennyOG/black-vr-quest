/*
 * game_render_x86: the real BLACK game, end to end through the production Quest graphics path, headless on x86-64
 * Linux (see port/notes/game-render-x86.md).
 *
 *   game thread   src/main.c black_game_main (BLACK_VR, the __ANDROID__ paths): crash reporter, memory layout, APU,
 *                 kernel, the recompiled title; kelvin_start -> the Kelvin GPU thread -> the Vulkan backend.
 *   GPU thread    Kelvin (KELVIN_GFX_BACKEND=vulkan): kgpu_* -> kvk_backend -> vr_renderer_kvk.c -> vr_bridge.c
 *                 (vr_acquire_eye_targets / flat / HUD targets, late latch) -> the fake runtime's VkImages.
 *   XR thread     this file: port/quest's xrs_frame (xr_session.c) at a reduced rate on tests/vr_glue's fake OpenXR
 *                 runtime; xrEndFrame's layers are captured to PNG (both eyes, the flat quad, the HUD quad).
 *   input         the fake runtime's action states (by action name) -> xr_session.c input_sync -> vr_input_map ->
 *                 VrPadState -> vr_get_pad -> xinput_device.c (BLACK_VR) -> usb_gamepad -> OHCI -> the title.
 *   head          a fixed pose, then (GRX_ROT_AT seconds, or after 30 stereo frames) a slow yaw/pitch sway.
 *
 * Usage: game_render_x86 <folder holding game/> <scratch dir> <frames dir>
 * The disc is read in place: <scratch>/root/game/<entry> are symlinks to the user's game/ entries; saves, the
 * kernel log, E:\UDATA/TDATA and the shader caches land in <scratch>/root.
 *
 * Environment (defaults):
 *   GRX_SECONDS=2400        wall-clock budget, then a final capture and exit
 *   GRX_EYE_W/GRX_EYE_H     eye image size (832 x 896)
 *   GRX_MULTIVIEW=1         one 2-layer eye swapchain (0: two swapchains)
 *   GRX_XR_HZ=30            XR-thread frames per second once the renderer runs (2 before: the test pattern)
 *   GRX_VIDEO=1             dump every newly presented frame to <frames>/video for make_video.py
 *   GRX_CAPTURE_EVERY=30    seconds between periodic captures of new content
 *   GRX_ROT_AT=900          seconds after which the head starts to sway (also after 30 stereo frames)
 *   GRX_INPUT               button script "T:button[:hold]" comma-separated; T in seconds after the first
 *                           renderer frame, button one of menu (START) a b x y up down left right (right stick)
 *                           fwd (left stick forward), rt / lt (right / left trigger: rt fires in play), lc / rc
 *                           (thumbstick clicks; both held 2.5 s: a both-eye capture to <scratch>/root/shots); hold in s (0.5);
 *                           lg / rg (grips); gestures (GRX_CONTROLS=1 tracks both controllers): duck, swing, raise,
 *                           twohand (see the controllers section)
 *   GRX_PROF=<file>         sample the game thread (grx_prof.c; GRX_PROF_US period, 250) for grx_prof.py
 *   GRX_FRAMECPU=<file>     the game thread's CPU time per Kelvin flip, for grx_framecpu.py
 *   GRX_INI                 extra blackvr.ini lines
 *   GRX_GUN_TEST=0          1: the gun-latch check (port/notes/gun-latch.md): the right controller is tracked, held
 *                           still in front of the head (no head sway); after GRX_GUN_AT (90) stereo frames with the
 *                           weapon in the hand, the controller turns GRX_GUN_YAW (5) degrees about the head between
 *                           a frame's game latch and its late latch. Saved to <frames>/gunlatch/: the two frames before
 *                           (game and late hand the same), the frame after (the late latch has the turned hand, the
 *                           game the old one), a settled frame (the game has caught up), and a debug-tint frame;
 *                           meta.json for gun_latch_check.py, which writes gunlatch_before.png / gunlatch_after.png.
 *   GRX_HAND=0              1: the right controller tracked (as GRX_GUN_TEST) without the step
 *   GRX_CONTROLS=0          1: both controllers tracked, for the VR controls' gestures (BLACKVR_INPUT_TRACE=1 logs the
 *                           game's side: intents, weapon state, magazine)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <png.h>
#include <vulkan/vulkan.h>

#include "fake_openxr.h"
#include "grx_shell.h"
#include "vr_bridge.h"
#include "kvk_device.h"
#include "kvk_stereo.h"
#include "grx_perf.h"
#include "bvr_diag.h"

extern int black_game_main(int argc, char **argv);
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;
extern void *xbox_GetMemoryBase(void);

#define NV2A_BASE 0xFD000000u

static double s_t0;
static const char *s_frames_dir;
static FILE *s_index;
static double s_limit = 2400.0, s_capture_every = 30.0, s_rot_at = 900.0, s_xr_hz = 30.0;
static uint32_t s_eye_w = 832, s_eye_h = 896;
static int s_multiview = 1;
static volatile int s_game_returned, s_game_rc;
static volatile double s_first_frame_t = -1.0;        /* wall time of the renderer's first presented frame */
static volatile int64_t s_rot_start_ns = -1;
static int s_sway = 1;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static double el(void) { return now_s() - s_t0; }
static double envf(const char *n, double d) { const char *e = getenv(n); return e && *e ? atof(e) : d; }

static uint32_t nv2a_reg(uint32_t off)
{
    if (!g_xbox_mem_offset || !xbox_GetMemoryBase()) return 0;
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + NV2A_BASE + off);
}

/* ---- head: fixed, then a slow sway (stage space, standing at the origin) ------------------------------------------ */
static const XrFovf k_fov[2] = { { -0.95f, 0.80f, 0.85f, -0.90f }, { -0.80f, 0.95f, 0.85f, -0.90f } };

static XrQuaternionf q_mul(XrQuaternionf a, XrQuaternionf b)
{
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}
static XrVector3f q_rot(XrQuaternionf q, XrVector3f v)
{
    XrQuaternionf p = { v.x, v.y, v.z, 0 }, c = { -q.x, -q.y, -q.z, q.w }, r = q_mul(q_mul(q, p), c);
    XrVector3f o = { r.x, r.y, r.z };
    return o;
}
static int button_down(const char *name);
static void head_angles(int64_t t, double *yaw, double *pitch)
{
    const int64_t r0 = s_rot_start_ns;
    double ts;
    *yaw = *pitch = 0.0;
    if (r0 < 0 || t < r0) return;
    ts = (double)(t - r0) * 1e-9;
    *yaw = 0.30 * sin(2.0 * M_PI * ts / 60.0);          /* +-17 deg over 60 s: ~0.03 rad/s peak */
    *pitch = 0.08 * sin(2.0 * M_PI * ts / 43.0);
}
static void trajectory(int64_t t, XrPosef *head, XrPosef eye[2], XrFovf fov[2])
{
    double yaw, pitch;
    XrQuaternionf qy, qx;
    int e;
    head_angles(t, &yaw, &pitch);
    qy.x = 0; qy.y = (float)sin(yaw / 2); qy.z = 0; qy.w = (float)cos(yaw / 2);
    qx.x = (float)sin(pitch / 2); qx.y = 0; qx.z = 0; qx.w = (float)cos(pitch / 2);
    head->orientation = q_mul(qy, qx);
    head->position.x = 0.0f; head->position.y = button_down("duck") ? 1.12f : 1.6f; head->position.z = 0.0f;
    for (e = 0; e < 2; e++) {
        XrVector3f off = { e == 0 ? -0.032f : 0.032f, 0.0f, 0.0f }, w = q_rot(head->orientation, off);
        eye[e].orientation = head->orientation;
        eye[e].position.x = head->position.x + w.x;
        eye[e].position.y = head->position.y + w.y;
        eye[e].position.z = head->position.z + w.z;
        fov[e] = k_fov[e];
    }
}

/* ---- the controllers (GRX_HAND / GRX_GUN_TEST / GRX_CONTROLS) ------------------------------------------------------
 * The right controller still, in front of the head; turned about the head on a gun-latch step. With GRX_CONTROLS=1
 * (or GRX_HAND) both are tracked and the script's gestures move them (port/notes/xr-shell.md "Input"):
 *   raise    the right controller at the right eye, pointing along the view (scope raise)
 *   swing    the right controller swept left at 4 m/s (pose deltas and XrSpaceVelocity), 0.6 m at most
 *   twohand  the left controller 33 cm ahead of the right one along the gun, its grip held (two-handed aim)
 *   duck     the head at 1.12 m instead of 1.6 m (trajectory above)                                                */
static int s_hand_on, s_gun_test, s_controls;
static volatile int s_gun_step;                  /* 0: the base pose; 1: turned by s_gun_yaw about the head centre */
static double s_gun_yaw = 5.0 * M_PI / 180.0;
static double s_swing_t0 = -1.0;
static int button_down(const char *name);

static void head_pose_at(XrTime t, XrPosef *head)
{
    XrPosef eye[2];
    XrFovf fov[2];
    trajectory(t, head, eye, fov);
}

static int hand_pose(int hand, int grip, XrTime t, XrPosef *pose)
{
    const XrVector3f centre = { 0.0f, 1.6f, 0.0f }, base = { 0.12f, -0.22f, -0.35f };
    XrQuaternionf q = { 0, 0, 0, 1 };
    XrVector3f r;
    (void)grip;
    if (!s_hand_on) return 0;
    if (hand == 1 && button_down("raise")) {
        /* the rear sight (aim pose + (0, 0.05, 0.06)) 8 cm in front of the right eye, along the head's view */
        XrPosef h;
        const XrVector3f off = { 0.032f, -0.05f, -0.14f };
        head_pose_at(t, &h);
        r = q_rot(h.orientation, off);
        pose->orientation = h.orientation;
        pose->position.x = h.position.x + r.x; pose->position.y = h.position.y + r.y; pose->position.z = h.position.z + r.z;
        return 1;
    }
    if (hand == 1 && s_gun_step) { q.y = (float)sin(s_gun_yaw / 2); q.w = (float)cos(s_gun_yaw / 2); }
    r = q_rot(q, base);
    pose->orientation = q;
    pose->position.x = centre.x + r.x; pose->position.y = centre.y + r.y; pose->position.z = centre.z + r.z;
    if (hand == 1) {
        if (button_down("swing")) {
            const double now = now_s();
            double d;
            if (s_swing_t0 < 0.0) s_swing_t0 = now;
            d = 4.0 * (now - s_swing_t0);
            pose->position.x -= (float)(d < 0.6 ? d : 0.6);
        } else {
            s_swing_t0 = -1.0;
        }
        return 1;
    }
    if (!s_controls) return 0;                    /* GRX_HAND / the gun-latch test: the right controller only */
    if (button_down("twohand")) {                 /* ahead of the right controller along the gun, a little up */
        pose->position.x = centre.x + base.x; pose->position.y = centre.y + base.y + 0.03f;
        pose->position.z = centre.z + base.z - 0.33f;
        return 1;
    }
    pose->position.x = centre.x - 0.15f; pose->position.y = centre.y - 0.25f; pose->position.z = centre.z - 0.30f;
    return 1;
}

static int hand_velocity(int hand, int grip, XrTime t, XrVector3f *v)
{
    (void)grip; (void)t;
    v->x = v->y = v->z = 0.0f;
    if (hand == 1 && s_swing_t0 >= 0.0 && now_s() - s_swing_t0 < 0.15) v->x = -4.0f;
    return 1;
}

/* ---- input script --------------------------------------------------------------------------------------------------- */
/* A press is held until both `hold` seconds have passed and the backend has drawn 4 more frames, so a slow XR thread
 * (it reads frames back) and a slow game frame (SwiftShader) both see it; then it is released. */
typedef struct Press { double t, hold; char button[16]; int captured_before, logged, done, exact; uint64_t frame0; double t0; } Press;
static Press s_press[256];
static int s_npress;
static int s_exact_hold;   /* GRX_EXACT_HOLD=1: release after exactly `hold` seconds (the VR controls' taps wait
                            * for the game's input updates themselves) */
static const char *k_default_script = "60:menu,120:a,180:a,240:a,300:a";

static void parse_script(const char *spec)
{
    char buf[1024], *tok, *save = NULL;
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok_r(buf, ",", &save); tok && s_npress < 64; tok = strtok_r(NULL, ",", &save)) {
        Press *p = &s_press[s_npress];
        char b[16] = "";
        double hold = 0.5, t = 0;
        if (sscanf(tok, "%lf:%15[a-z]:%lf", &t, b, &hold) >= 2) {
            p->t = t; p->hold = hold;
            snprintf(p->button, sizeof(p->button), "%s", b);
            s_npress++;
        }
    }
}

static int button_down(const char *name)
{
    const double ff = s_first_frame_t;
    double t;
    int i;
    if (ff < 0) return 0;
    t = now_s() - ff;
    for (i = 0; i < s_npress; i++)
        if (!strcmp(s_press[i].button, name) && !s_press[i].done && t >= s_press[i].t) {
            KvkStats st;
            kvk_get_stats(&st);
            if (!s_press[i].logged) {
                s_press[i].logged = 1;
                s_press[i].frame0 = st.frames;
                s_press[i].t0 = t;
                fprintf(stderr, "[GRX] t=%.1fs: input %s pressed (script +%.0fs after the first frame, backend frame %llu)\n", el(),
                        name, s_press[i].t, (unsigned long long)st.frames);
            }
            if (t - s_press[i].t0 >= s_press[i].hold && (s_exact_hold || s_press[i].exact || st.frames >= s_press[i].frame0 + 4)) {
                s_press[i].done = 1;
                fprintf(stderr, "[GRX] t=%.1fs: input %s released (backend frame %llu)\n", el(), name, (unsigned long long)st.frames);
                return 0;
            }
            return 1;
        }
    return 0;
}

static int input_bool(const char *action, XrPath sub, XrBool32 *state)
{
    static const struct { const char *action, *button; } map[] = {
        { "menu", "menu" }, { "button_a", "a" }, { "button_b", "b" }, { "button_x", "x" }, { "button_y", "y" },
    };
    unsigned i;
    /* the thumbstick clicks: "lc" / "rc" ("/user/hand/left" is XrPath 16, right 17); both held 2.5 s: a both-eye capture */
    if (!strcmp(action, "stick_click")) { *state = button_down(sub == 16 ? "lc" : "rc") ? XR_TRUE : XR_FALSE; return 1; }
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(action, map[i].action)) { *state = button_down(map[i].button) ? XR_TRUE : XR_FALSE; return 1; }
    return 0;
}

static int input_vec2(const char *action, XrPath sub, XrVector2f *state)
{
    /* the right stick drives the d-pad in menus (vr_input_map.h); the fake runtime's XrPath is strlen+1, so
     * "/user/hand/right" is 17 */
    if (strcmp(action, "stick")) return 0;
    state->x = state->y = 0.0f;
    if (sub == 16 && button_down("fwd")) state->y = 1.0f;     /* "/user/hand/left": walk forward (perf windows) */
    if (sub == 17) {
        if (button_down("up")) state->y = 1.0f;
        if (button_down("down")) state->y = -1.0f;
        if (button_down("left")) state->x = -1.0f;
        if (button_down("right")) state->x = 1.0f;
    }
    return 1;
}

/* the triggers: "rt" (right, fire in play), "lt" (left); the fake runtime's XrPath is strlen+1 */
static int input_float(const char *action, XrPath sub, float *state)
{
    if (!strcmp(action, "squeeze")) {                 /* the grips: "lg" / "rg"; "twohand" holds the left one */
        *state = (sub == 17 && button_down("rg")) || (sub == 16 && (button_down("lg") || button_down("twohand"))) ? 1.0f : 0.0f;
        return 1;
    }
    if (strcmp(action, "trigger")) return 0;
    *state = (sub == 17 && button_down("rt")) || (sub == 16 && button_down("lt")) ? 1.0f : 0.0f;
    return 1;
}

/* ---- image capture ----------------------------------------------------------------------------------------------------- */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(fx_vk.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

/* The released swapchain image, back to the CPU (images live in COLOR_ATTACHMENT_OPTIMAL between frames). Runs on the
 * XR thread inside xrEndFrame, under the (recursive) queue lock. */
static void read_image(VkImage img, uint32_t layer, uint32_t w, uint32_t h, uint8_t *out)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkCommandBufferAllocateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkMemoryRequirements mr;
    VkBufferImageCopy r;
    VkBuffer buf;
    VkDeviceMemory mem;
    VkCommandBuffer cb;
    void *p;
    bi.size = (VkDeviceSize)w * h * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(fx_vk.device, &bi, NULL, &buf);
    vkGetBufferMemoryRequirements(fx_vk.device, buf, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(fx_vk.device, &ai, NULL, &mem);
    vkBindBufferMemory(fx_vk.device, buf, mem, 0);
    ci.commandPool = fx_vk.pool; ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(fx_vk.device, &ci, &cb);
    vkBeginCommandBuffer(cb, &cbi);
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.baseArrayLayer = layer; b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.baseArrayLayer = layer; r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vr_queue_lock();
    vkQueueSubmit(fx_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(fx_vk.queue);
    vr_queue_unlock();
    vkFreeCommandBuffers(fx_vk.device, fx_vk.pool, 1, &cb);
    vkMapMemory(fx_vk.device, mem, 0, VK_WHOLE_SIZE, 0, &p);
    memcpy(out, p, (size_t)w * h * 4);
    vkUnmapMemory(fx_vk.device, mem);
    vkDestroyBuffer(fx_vk.device, buf, NULL);
    vkFreeMemory(fx_vk.device, mem, NULL);
}

static int s_png_level = 3;
static int write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, int keep_alpha)
{
    FILE *f = fopen(path, "wb");
    png_structp png;
    png_infop info;
    uint8_t *row;
    uint32_t y, x;
    if (!f) return 0;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); fclose(f); return 0; }
    png_init_io(png, f);
    png_set_compression_level(png, s_png_level);
    png_set_IHDR(png, info, w, h, 8, keep_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    row = (uint8_t *)malloc((size_t)w * 4);
    for (y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * w * 4;
        if (keep_alpha) memcpy(row, s, (size_t)w * 4);
        else for (x = 0; x < w; x++) { row[x * 3] = s[x * 4]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4 + 2]; }
        png_write_row(png, row);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    free(row);
    fclose(f);
    return 1;
}

/* Basic statistics, so the log says whether a capture is black, uniform or has content. */
static void image_stats(const uint8_t *p, uint32_t w, uint32_t h, double *mean, double *sd, double *alpha_cov)
{
    double s = 0, s2 = 0, a = 0, var;
    size_t n = 0, i;
    for (i = 0; i < (size_t)w * h; i += 7, n++) {
        const double l = 0.299 * p[i * 4] + 0.587 * p[i * 4 + 1] + 0.114 * p[i * 4 + 2];
        s += l; s2 += l * l; a += p[i * 4 + 3] > 0;
    }
    *mean = s / n;
    var = s2 / n - (*mean) * (*mean);
    *sd = sqrt(var > 0 ? var : 0);
    *alpha_cov = a / n;
}

typedef struct CapState {
    int last_kind;                 /* 1 stereo, 2 flat */
    uint64_t eye_rel, flat_rel;
    double last_t;
    unsigned seq;
    volatile int want;             /* forced by the input script / the end of the run */
    const char *volatile want_tag;
    unsigned stereo_frames_seen, flat_frames_seen;
    int hud_seen;
} CapState;
static CapState s_cap;
static uint8_t *s_img;

static void save_layer(const char *tag, const char *kind, XrSwapchain sc_h, uint32_t index, uint32_t layer, uint32_t w,
                       uint32_t h, int alpha)
{
    char path[1200], latest[1200];
    FxSwapchain *sc = fx_swapchain(sc_h);
    double mean, sd, cov;
    if (!sc || index >= FX_IMAGES) return;
    read_image(sc->img[index], layer, w, h, s_img);
    image_stats(s_img, w, h, &mean, &sd, &cov);
    snprintf(path, sizeof(path), "%s/%04u_%s_%s.png", s_frames_dir, s_cap.seq, tag, kind);
    snprintf(latest, sizeof(latest), "%s/latest_%s.png", s_frames_dir, kind);
    write_png(path, s_img, w, h, alpha);
    write_png(latest, s_img, w, h, alpha);
    fprintf(stderr, "[GRX] t=%.1fs capture %s (%ux%u): luma mean %.1f sd %.1f", el(), path, w, h, mean, sd);
    if (alpha) fprintf(stderr, ", alpha coverage %.3f", cov);
    fprintf(stderr, "\n");
    if (s_index) {
        fprintf(s_index, "%u\t%.1f\t%s\t%s\t%s\t%.1f\t%.1f\t%.3f\n", s_cap.seq, el(), tag, kind, path, mean, sd, cov);
        fflush(s_index);
    }
}


/* Where the game camera's forward axis (far away) lands in each eye image through the backend's own per-eye
 * correction (kvk_stereo_frame on the frame's render pose and the newest camera), against where the submitted pose
 * and FOV put the head's forward direction. Both eyes share the head's orientation, so with a camera driven from the
 * head the two agree, and their difference between the eyes is the projection centres' (plus vanishing parallax). */
static void stereo_diag(uint32_t w)
{
    const VrRenderPose *rp = vr_render_pose();
    const VrGameCamera *cam = vr_game_camera();
    const VrGameCameraExt *ext = vr_game_camera_ext();
    VrFrameState fs;
    KvkStereoFrame sf;
    int e;
    if (!rp->valid && !rp->serial) return;
    memset(&fs, 0, sizeof(fs));
    fs.head = rp->game_head;
    for (e = 0; e < 2; e++) { fs.eye[e].pose = rp->eye_pose[e]; fs.eye[e].fov = rp->eye_fov[e]; }
    fs.world_scale = rp->world_scale;
    fs.session_running = 1;
    kvk_stereo_frame(&sf, NULL, &fs, cam);
    fprintf(stderr, "[GRX]     stereo diag: cam valid %d half_fov %.3f %.3f, proj x row %.4f %.4f %.4f %.4f, w row %.3f %.3f %.3f %.3f, "
                    "world_scale %.3f, ext frame %u mode %d\n", cam->valid, cam->half_fov_x, cam->half_fov_y,
            cam->proj[0], cam->proj[4], cam->proj[8], cam->proj[12], cam->proj[3], cam->proj[7], cam->proj[11], cam->proj[15],
            rp->world_scale, ext->game_frame, ext->present_mode);
    for (e = 0; e < 2; e++) {
        const float *p = cam->proj, *c = sf.eye_clip[e];
        const double m32 = p[11], D = 1000.0, z = D / (m32 != 0 ? m32 : 1);
        double gc[4], ec[4];
        int i;
        const VrFov *f = &rp->eye_fov[e];
        const double l = tan(f->left), r = tan(f->right);
        for (i = 0; i < 4; i++) gc[i] = p[i + 8] * z + p[i + 12];
        if (sf.misc[0] == (float)KVK_STEREO_FROM_W) { gc[2] = gc[3]; gc[3] = 1.0; }   /* FROM_W acts on (x, y, w, 1) */
        for (i = 0; i < 4; i++) ec[i] = c[i] * gc[0] + c[i + 4] * gc[1] + c[i + 8] * gc[2] + c[i + 12] * gc[3];
        fprintf(stderr, "[GRX]     eye %d: game forward at x = %.1f px (eye clip w %.3f); head forward per the submitted FOV at "
                        "x = %.1f px; eye pos %.3f %.3f %.3f vs game head %.3f %.3f %.3f\n", e,
                ec[3] != 0 ? (ec[0] / ec[3] + 1.0) * 0.5 * w : -1.0, ec[3], (0.0 - l) / (r - l) * w,
                rp->eye_pose[e].position.x, rp->eye_pose[e].position.y, rp->eye_pose[e].position.z,
                rp->game_head.position.x, rp->game_head.position.y, rp->game_head.position.z);
    }
}


/* ---- per-frame dump for the video (frames/video/, composed by make_video.py) ----------------------------------------
 * Every newly presented frame: the layers the XR thread submitted (eye images, HUD quad, flat quad) and, in meta.jsonl,
 * how the shell placed them (eye poses and FOVs; quad space, pose and size), the backend's flip count (the game's frame
 * clock: one main-loop iteration per flip at RECOMP_BLACK_60HZ's 1/60 s step), the camera's game frame and the mode. */
static FILE *s_vmeta;
static char s_vdir[1100];
static unsigned s_vseq;
static int s_video = 1;
static uint8_t *s_vimg;

static void video_layer(const char *name, XrSwapchain sc_h, uint32_t index, uint32_t layer, uint32_t w, uint32_t h, int alpha,
                        char *out_name, size_t n)
{
    char path[1300];
    FxSwapchain *sc = fx_swapchain(sc_h);
    out_name[0] = 0;
    if (!sc || index >= FX_IMAGES) return;
    read_image(sc->img[index], layer, w, h, s_vimg);
    snprintf(out_name, n, "v%06u_%s.png", s_vseq, name);
    snprintf(path, sizeof(path), "%s/%s", s_vdir, out_name);
    s_png_level = 1;
    write_png(path, s_vimg, w, h, alpha);
    s_png_level = 3;
}

static void json_pose(FILE *o, const XrPosef *p)
{
    fprintf(o, "{\"q\":[%.6f,%.6f,%.6f,%.6f],\"p\":[%.5f,%.5f,%.5f]}", p->orientation.x, p->orientation.y, p->orientation.z,
            p->orientation.w, p->position.x, p->position.y, p->position.z);
}

static void video_dump(const FxEndFrame *f, const GrxShellInfo *si, int kind, int flat_quad, int hud_quad)
{
    char eye_l[64] = "", eye_r[64] = "", flat_n[64] = "", hud_n[64] = "";
    XrPosef eyes[2], head;
    XrFovf fov[2];
    KvkStats st;
    int e;
    uint32_t q;
    if (!s_video || !s_vmeta) return;
    s_vseq++;
    kvk_get_stats(&st);
    if (kind == 1) {
        for (e = 0; e < 2; e++) {
            const XrCompositionLayerProjectionView *v = &f->views[e];
            video_layer(e ? "R" : "L", v->subImage.swapchain, f->proj_image[e], v->subImage.imageArrayIndex,
                        (uint32_t)v->subImage.imageRect.extent.width, (uint32_t)v->subImage.imageRect.extent.height, 0,
                        e ? eye_r : eye_l, 64);
            eyes[e] = v->pose; fov[e] = v->fov;
        }
        if (hud_quad >= 0) video_layer("hud", si->hud, fx_swapchain(si->hud)->last_released, 0, si->hud_w, si->hud_h, 1, hud_n, 64);
    } else {
        trajectory(f->display_time, &head, eyes, fov);
        video_layer("flat", si->flat, fx_swapchain(si->flat)->last_released, 0, si->flat_w, si->flat_h, 0, flat_n, 64);
    }
    fprintf(s_vmeta, "{\"seq\":%u,\"t\":%.3f,\"flips\":%llu,\"game_frame\":%u,\"mode\":%d,\"kind\":\"%s\",\"eyeL\":\"%s\",\"eyeR\":\"%s\","
                     "\"eye_w\":%u,\"eye_h\":%u,\"eyes\":[", s_vseq, el(), (unsigned long long)st.frames, si->submit_game_frame,
            si->submit_mode, kind == 1 ? "stereo" : "flat", eye_l, eye_r, si->eye_w, si->eye_h);
    for (e = 0; e < 2; e++) {
        fprintf(s_vmeta, "%s{\"pose\":", e ? "," : "");
        json_pose(s_vmeta, &eyes[e]);
        fprintf(s_vmeta, ",\"fov\":[%.5f,%.5f,%.5f,%.5f]}", fov[e].angleLeft, fov[e].angleRight, fov[e].angleUp, fov[e].angleDown);
    }
    fprintf(s_vmeta, "],\"quads\":[");
    for (q = 0; q < f->nquads; q++) {
        const XrCompositionLayerQuad *qd = &f->quads[q];
        const char *img = (int)q == flat_quad ? flat_n : (int)q == hud_quad ? hud_n : "";
        fprintf(s_vmeta, "%s{\"layer\":\"%s\",\"image\":\"%s\",\"space\":\"%s\",\"premultiplied\":%d,\"size\":[%.4f,%.4f],\"pose\":",
                q ? "," : "", (int)q == flat_quad ? "flat" : "hud", img, qd->space == si->view_space ? "view" : "world",
                !(qd->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT), qd->size.width, qd->size.height);
        json_pose(s_vmeta, &qd->pose);
        fprintf(s_vmeta, "}");
    }
    fprintf(s_vmeta, "]}\n");
    fflush(s_vmeta);
}

/* ---- the gun-latch check ------------------------------------------------------------------------------------------------
 * The render pose and weapon latch of each released eye image (taken in xrReleaseSwapchainImage on the GPU thread,
 * where they still belong to that image), and the backend's weapon-draw count by then. */
typedef struct GunSnap { int used; VrRenderPose rp; VrRenderGun rg; uint64_t vm_draws, frames; } GunSnap;
static GunSnap s_snap[FX_MAX_SWAPCHAINS + 1][FX_IMAGES];
static XrSwapchain s_snap_eye;
static void on_release(XrSwapchain h, uint32_t index)
{
    const uintptr_t i = (uintptr_t)h;
    KvkStats st;
    if (h != s_snap_eye || i > FX_MAX_SWAPCHAINS || index >= FX_IMAGES) return;
    kvk_get_stats(&st);
    s_snap[i][index].rp = *vr_render_pose();
    s_snap[i][index].rg = *vr_render_gun();
    s_snap[i][index].vm_draws = st.viewmodel_draws;
    s_snap[i][index].frames = st.frames;
    s_snap[i][index].used = 1;
}

enum { GUN_WAIT, GUN_STEPPED, GUN_SETTLE, GUN_TINT, GUN_DONE };
static struct { int state, frames, tries, nsaved; uint64_t last_vm; FILE *meta; char dir[1100]; double step_t; } s_gun;

static void gun_save(const char *name, const FxEndFrame *f, const GunSnap *sn)
{
    char path[1300];
    int e;
    uint32_t w = (uint32_t)f->views[0].subImage.imageRect.extent.width, h = (uint32_t)f->views[0].subImage.imageRect.extent.height;
    for (e = 0; e < 2; e++) {
        const XrCompositionLayerProjectionView *v = &f->views[e];
        FxSwapchain *sc = fx_swapchain(v->subImage.swapchain);
        if (!sc) continue;
        read_image(sc->img[f->proj_image[e]], v->subImage.imageArrayIndex, w, h, s_img);
        snprintf(path, sizeof(path), "%s/%s_%s.png", s_gun.dir, name, e ? "R" : "L");
        write_png(path, s_img, w, h, 0);
    }
    if (s_gun.meta) {
        fprintf(s_gun.meta, "%s\"%s\":{\"w\":%u,\"h\":%u,\"game_frame\":%u,\"serial\":%llu,\"vm_draws\":%llu,\"frame\":%llu,"
                            "\"gun_valid\":%d,\"game_hand\":", s_gun.nsaved++ ? "," : "", name, w, h, sn->rp.game_frame,
                (unsigned long long)sn->rp.serial, (unsigned long long)(sn->vm_draws - s_gun.last_vm), (unsigned long long)sn->frames,
                sn->rg.valid);
        {
            XrPosef p;
            p.orientation.x = sn->rg.game_hand.orientation.x; p.orientation.y = sn->rg.game_hand.orientation.y;
            p.orientation.z = sn->rg.game_hand.orientation.z; p.orientation.w = sn->rg.game_hand.orientation.w;
            p.position.x = sn->rg.game_hand.position.x; p.position.y = sn->rg.game_hand.position.y; p.position.z = sn->rg.game_hand.position.z;
            json_pose(s_gun.meta, &p);
            fprintf(s_gun.meta, ",\"hand\":");
            p.orientation.x = sn->rg.hand.orientation.x; p.orientation.y = sn->rg.hand.orientation.y;
            p.orientation.z = sn->rg.hand.orientation.z; p.orientation.w = sn->rg.hand.orientation.w;
            p.position.x = sn->rg.hand.position.x; p.position.y = sn->rg.hand.position.y; p.position.z = sn->rg.hand.position.z;
            json_pose(s_gun.meta, &p);
        }
        fprintf(s_gun.meta, ",\"eyes\":[");
        for (e = 0; e < 2; e++) {
            fprintf(s_gun.meta, "%s{\"pose\":", e ? "," : "");
            json_pose(s_gun.meta, &f->views[e].pose);
            fprintf(s_gun.meta, ",\"fov\":[%.6f,%.6f,%.6f,%.6f]}", f->views[e].fov.angleLeft, f->views[e].fov.angleRight,
                    f->views[e].fov.angleUp, f->views[e].fov.angleDown);
        }
        fprintf(s_gun.meta, "]}\n");
        fflush(s_gun.meta);
    }
    fprintf(stderr, "[GRX] t=%.1fs GUNLATCH %s: game frame %u, weapon draws %llu, latch valid %d, hand %s the game's\n", el(), name,
            sn->rp.game_frame, (unsigned long long)(sn->vm_draws - s_gun.last_vm), sn->rg.valid,
            memcmp(&sn->rg.hand, &sn->rg.game_hand, sizeof(VrPose)) ? "differs from" : "is");
}

/* One newly presented stereo frame of the check. */
static void gun_frame(const FxEndFrame *f)
{
    const uintptr_t sci = (uintptr_t)f->views[0].subImage.swapchain;
    GunSnap sn;
    int moved, vm;
    if (!s_gun_test || s_gun.state == GUN_DONE || sci > FX_MAX_SWAPCHAINS || f->proj_image[0] >= FX_IMAGES) return;
    sn = s_snap[sci][f->proj_image[0]];
    if (!sn.used) return;
    moved = memcmp(&sn.rg.hand, &sn.rg.game_hand, sizeof(VrPose)) != 0;
    vm = sn.vm_draws > s_gun.last_vm;
    switch (s_gun.state) {
    case GUN_WAIT:
        if (!(sn.rg.valid && vm && !moved)) { s_gun.frames = 0; break; }
        ++s_gun.frames;
        /* the frame before "before" too: the game's own motion of the weapon from one frame to the next */
        if (s_gun.frames == (int)envf("GRX_GUN_AT", 90) - 1) gun_save("prev", f, &sn);
        if (s_gun.frames < (int)envf("GRX_GUN_AT", 90)) break;
        gun_save("before", f, &sn);
        s_gun_step = 1;                              /* the controller turns now: after this frame's latches */
        s_gun.step_t = el();
        s_gun.state = GUN_STEPPED;
        fprintf(stderr, "[GRX] t=%.1fs GUNLATCH: the controller turns %.1f degrees about the head\n", el(), s_gun_yaw * 180.0 / M_PI);
        break;
    case GUN_STEPPED:
        if (sn.rg.valid && moved && vm) {            /* the game's hand is the old one, the late latch's the new one */
            gun_save("after", f, &sn);
            s_gun.state = GUN_SETTLE;
        } else if (sn.rg.valid && !moved) {          /* the game caught up first: no frame showed the delta; again */
            fprintf(stderr, "[GRX] t=%.1fs GUNLATCH: no in-between frame (try %d): back to the base pose\n", el(), s_gun.tries + 1);
            s_gun_step = 0;
            s_gun.frames = 0;
            s_gun.state = ++s_gun.tries >= 4 ? GUN_DONE : GUN_WAIT;
            s_gun.nsaved = 0;
            if (s_gun.meta) { fclose(s_gun.meta); s_gun.meta = NULL; }
            if (s_gun.state == GUN_WAIT) {
                char mp[1200];
                snprintf(mp, sizeof(mp), "%s/meta.json", s_gun.dir);
                s_gun.meta = fopen(mp, "w");
                if (s_gun.meta) fprintf(s_gun.meta, "{");
            }
        }
        break;
    case GUN_SETTLE:
        if (sn.rg.valid && !moved && vm) {           /* the game now places the weapon at the turned hand itself */
            gun_save("settled", f, &sn);
            /* the debug tint (BLACKVR_GUN_LATCH_DEBUG) from the next frame on: the classified draws show */
            ((VrGunLatchConfig *)vr_gun_latch_config())->debug_tint = 0x90FF00FFu;
            s_gun.state = GUN_TINT;
            s_gun.frames = 0;
        }
        break;
    case GUN_TINT:
        if (++s_gun.frames < 2) break;               /* a frame recorded after the tint was set */
        gun_save("tint", f, &sn);
        ((VrGunLatchConfig *)vr_gun_latch_config())->debug_tint = 0;
        s_gun.state = GUN_DONE;
        if (s_gun.meta) { fprintf(s_gun.meta, "}\n"); fclose(s_gun.meta); s_gun.meta = NULL; }
        fprintf(stderr, "[GRX] t=%.1fs GUNLATCH: done (python3 tests/game_render_x86/gun_latch_check.py %s)\n", el(), s_frames_dir);
        break;
    }
    s_gun.last_vm = sn.vm_draws;
}

static void on_end_frame(const FxEndFrame *f)
{
    GrxShellInfo si;
    FxSwapchain *eye0, *flat, *hud;
    uint64_t eye_rel, flat_rel;
    int kind = 0, new_content, hud_quad = -1, flat_quad = -1;
    uint32_t q;
    const double t = el();
    const char *tag = NULL;

    grx_shell_info(&si);
    if (!si.renderer_started) return;                   /* still the test pattern */
    eye0 = fx_swapchain(si.eye[0]); flat = fx_swapchain(si.flat); hud = fx_swapchain(si.hud);
    eye_rel = eye0 ? eye0->releases : 0;
    flat_rel = flat ? flat->releases : 0;
    for (q = 0; q < f->nquads; q++) {
        if (f->quads[q].subImage.swapchain == si.flat) flat_quad = (int)q;
        if (f->quads[q].subImage.swapchain == si.hud) hud_quad = (int)q;
    }
    /* a projection layer counts once the renderer released eye images (before that it is the test pattern) */
    if (f->has_proj && si.submit_latch_serial) kind = 1; else if (!f->has_proj && flat_quad >= 0) kind = 2;
    if (!kind) return;
    if (s_first_frame_t < 0) {
        s_first_frame_t = now_s();
        fprintf(stderr, "[GRX] t=%.1fs: first renderer frame presented (%s)\n", t, kind == 1 ? "projection layer" : "flat quad");
    }
    new_content = kind == 1 ? eye_rel != s_cap.eye_rel : flat_rel != s_cap.flat_rel;
    {
        static uint64_t seen_eye, seen_flat;
        if (kind == 1 && eye_rel != seen_eye) { s_cap.stereo_frames_seen++; seen_eye = eye_rel; }
        if (kind == 2 && flat_rel != seen_flat) { s_cap.flat_frames_seen++; seen_flat = flat_rel; }
    }
    {
        static uint64_t dumped_eye, dumped_flat;
        if ((kind == 1 && eye_rel != dumped_eye) || (kind == 2 && flat_rel != dumped_flat)) {
            video_dump(f, &si, kind, flat_quad, hud_quad);
            dumped_eye = eye_rel; dumped_flat = flat_rel;
        }
    }
    {
        static uint64_t gun_seen;
        if (kind == 1 && eye_rel != gun_seen) { gun_seen = eye_rel; gun_frame(f); }
    }
    if (kind == 1 && s_sway && s_rot_start_ns < 0 && s_cap.stereo_frames_seen >= 30) {
        s_rot_start_ns = now_ns();
        fprintf(stderr, "[GRX] t=%.1fs: 30 stereo frames presented: the head starts to sway\n", t);
    }
    if (s_cap.want) tag = s_cap.want_tag;
    else if (kind != s_cap.last_kind) tag = kind == 1 ? "first-stereo" : "first-flat";
    else if (kind == 1 && hud_quad >= 0 && !s_cap.hud_seen) tag = "first-hud";
    else if (new_content && t - s_cap.last_t >= s_capture_every) tag = "periodic";
    if (!tag) return;
    if (kind != s_cap.last_kind)
        fprintf(stderr, "[GRX] t=%.1fs: present %s%s (shell submit mode %d)\n", t, kind == 1 ? "STEREO" : "FLAT",
                s_cap.last_kind ? " (mode change)" : "", si.submit_mode);
    s_cap.seq++;
    if (kind == 1) {
        int e;
        for (e = 0; e < 2; e++) {
            const XrCompositionLayerProjectionView *v = &f->views[e];
            save_layer(tag, e ? "eyeR" : "eyeL", v->subImage.swapchain, f->proj_image[e], v->subImage.imageArrayIndex,
                       (uint32_t)v->subImage.imageRect.extent.width, (uint32_t)v->subImage.imageRect.extent.height, 0);
        }
        if (hud_quad >= 0 && hud) {
            save_layer(tag, "hud", si.hud, hud->last_released, 0, si.hud_w, si.hud_h, 1);
            s_cap.hud_seen = 1;
        }
        stereo_diag((uint32_t)f->views[0].subImage.imageRect.extent.width);
        fprintf(stderr, "[GRX]     STEREO: game frame %u, head sway %s, %u layers (HUD quad %s)\n", si.submit_game_frame,
                s_rot_start_ns >= 0 ? "on" : "off", f->nlayers, hud_quad >= 0 ? "yes" : "no");
    } else {
        const XrCompositionLayerQuad *qd = &f->quads[flat_quad];
        save_layer(tag, "flat", si.flat, flat->last_released, 0, si.flat_w, si.flat_h, 0);
        fprintf(stderr, "[GRX]     FLAT: shell submit mode %d, quad in %s space, %.2f x %.2f m at z %.2f\n", si.submit_mode,
                qd->space == si.world_space ? "world" : qd->space == si.view_space ? "view" : "other", qd->size.width,
                qd->size.height, qd->pose.position.z);
    }
    s_cap.last_kind = kind;
    s_cap.eye_rel = eye_rel; s_cap.flat_rel = flat_rel;
    s_cap.last_t = t;
    s_cap.want = 0;
}

/* ---- the run -------------------------------------------------------------------------------------------------------- */
static int make_scratch_root(const char *user_root, const char *scratch, char *root, size_t n)
{
    char src[1024], game[1100], link[1400], target[1400];
    DIR *d;
    struct dirent *de;
    mkdir(scratch, 0770);
    snprintf(root, n, "%s/root", scratch);
    mkdir(root, 0770);
    snprintf(game, sizeof(game), "%s/game", root);
    mkdir(game, 0770);
    snprintf(src, sizeof(src), "%s/game", user_root);
    if (!(d = opendir(src))) { fprintf(stderr, "[GRX] FAIL: cannot open %s: %s\n", src, strerror(errno)); return -1; }
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(link, sizeof(link), "%s/%s", game, de->d_name);
        snprintf(target, sizeof(target), "%s/%s", src, de->d_name);
        if (symlink(target, link) != 0 && errno != EEXIST) {
            fprintf(stderr, "[GRX] FAIL: symlink %s: %s\n", link, strerror(errno));
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static void *game_thread(void *arg)
{
    grx_perf_set_game_tid();
    grx_prof_start_on_game_thread();
    /* as port/quest/src/xr_main.c's game thread: the larger alternate signal stack, then the trap check on it */
    bvr_thread_altstack(256 * 1024);
    if (!getenv("GRX_PREFLIGHT") || *getenv("GRX_PREFLIGHT") != '0') bvr_pf_sigsegv("sigsegv-game-thread");
    s_game_rc = black_game_main(2, (char **)arg);
    s_game_returned = 1;
    return NULL;
}

static void report_icalls(void)
{
    unsigned i, idx = g_icall_trace_idx;
    fprintf(stderr, "[GRX]     last indirect-call targets (oldest first):");
    for (i = 0; i < 16; i++) {
        uint32_t v = g_icall_trace[(idx + i) & 15];
        if (v) fprintf(stderr, " %08X", v);
    }
    fprintf(stderr, "\n[GRX]     NV2A DMA PUT=%08X GET=%08X PCRTC_START=%08X\n", nv2a_reg(0x800040), nv2a_reg(0x800044),
            nv2a_reg(0x600800));
}

static void progress(double t, KvkStats *prev, uint64_t *prev_icalls, double *prev_t)
{
    KvkStats st;
    GrxShellInfo si;
    FxSwapchain *e0, *fl, *hd;
    const double dt = t - *prev_t;
    kvk_get_stats(&st);
    grx_shell_info(&si);
    e0 = fx_swapchain(si.eye[0]); fl = fx_swapchain(si.flat); hd = fx_swapchain(si.hud);
    fprintf(stderr, "[GRX] t=%.0fs icalls=%llu (+%llu) | kvk frames %llu (+%llu) draws %llu (+%llu) world %llu stereo %llu "
                    "hud %llu uber %llu skipped %llu | pipelines %llu vs %llu ps %llu tex %llu | submit mode %d | released eye %llu "
                    "flat %llu hud %llu | xrEndFrame %llu\n",
            t, (unsigned long long)g_icall_count, (unsigned long long)(g_icall_count - *prev_icalls),
            (unsigned long long)st.frames, (unsigned long long)(st.frames - prev->frames), (unsigned long long)st.draws,
            (unsigned long long)(st.draws - prev->draws), (unsigned long long)st.world_draws,
            (unsigned long long)st.stereo_draws, (unsigned long long)st.hud_draws, (unsigned long long)st.uber_draws,
            (unsigned long long)st.skipped, (unsigned long long)st.pipelines, (unsigned long long)st.vs_compiled,
            (unsigned long long)st.ps_compiled, (unsigned long long)st.tex_uploads, si.submit_mode,
            (unsigned long long)(e0 ? e0->releases : 0), (unsigned long long)(fl ? fl->releases : 0),
            (unsigned long long)(hd ? hd->releases : 0), (unsigned long long)fx_end_count);
    fprintf(stderr, "[PERF] first-use: pipelines %llu (+%llu) in %.1f ms (+%.1f, worst %.1f ms) | glslang %llu (+%llu) in %.1f ms, "
                    "SPIR-V cache hits %llu (+%llu) | uber draws +%llu skipped +%llu\n",
            (unsigned long long)st.pipelines, (unsigned long long)(st.pipelines - prev->pipelines), st.pipe_ns / 1e6,
            (st.pipe_ns - prev->pipe_ns) / 1e6, st.pipe_max_ns / 1e6, (unsigned long long)st.glslang_compiles,
            (unsigned long long)(st.glslang_compiles - prev->glslang_compiles), (st.glslang_ns - prev->glslang_ns) / 1e6,
            (unsigned long long)st.spv_cache_hits, (unsigned long long)(st.spv_cache_hits - prev->spv_cache_hits),
            (unsigned long long)(st.uber_draws - prev->uber_draws), (unsigned long long)(st.skipped - prev->skipped));
    if (st.frames > prev->frames)
        fprintf(stderr, "[GRX]     frame time %.0f ms (wall clock, SwiftShader on this host)\n",
                1000.0 * dt / (double)(st.frames - prev->frames));
    else
        report_icalls();
    grx_perf_report(si.submit_mode == 1 ? "FLAT" : si.submit_mode == 0 && st.stereo_draws > prev->stereo_draws ? "STEREO" : "OTHER",
                    dt);
    grx_prof_flush();
    *prev = st;
    *prev_icalls = g_icall_count;
    *prev_t = t;
}

int main(int argc, char **argv)
{
    char root[1024], save[1100], cache[1100], idx_path[1200];
    static char tags[64][32];
    char *args[3];
    pthread_t game;
    pthread_attr_t at;
    GrxShellOptions so;
    int r, i;
    double next_frame = 0, next_progress = 15, prev_t = 0;
    KvkStats prev_st;
    uint64_t prev_icalls = 0;
    const char *script;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <folder holding game/> <scratch dir> <frames dir>\n", argv[0]);
        return 2;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    s_t0 = now_s();
    grx_perf_init("/proc/self/exe");
    s_limit = envf("GRX_SECONDS", s_limit);
    s_capture_every = envf("GRX_CAPTURE_EVERY", s_capture_every);
    s_rot_at = envf("GRX_ROT_AT", s_rot_at);
    s_xr_hz = envf("GRX_XR_HZ", s_xr_hz);
    s_eye_w = (uint32_t)envf("GRX_EYE_W", s_eye_w);
    s_eye_h = (uint32_t)envf("GRX_EYE_H", s_eye_h);
    s_multiview = (int)envf("GRX_MULTIVIEW", 1);
    s_gun_test = (int)envf("GRX_GUN_TEST", 0);
    s_controls = (int)envf("GRX_CONTROLS", 0) && !s_gun_test;
    s_exact_hold = (int)envf("GRX_EXACT_HOLD", 0);
    s_hand_on = s_gun_test || (int)envf("GRX_HAND", 0) || s_controls;
    s_gun_yaw = envf("GRX_GUN_YAW", 5.0) * M_PI / 180.0;
    if (s_gun_test) { s_sway = 0; if (!getenv("GRX_ROT_AT")) s_rot_at = 1e9; }   /* the head holds still */
    s_sway = (int)envf("GRX_SWAY", s_sway);
    script = getenv("GRX_INPUT") ? getenv("GRX_INPUT") : k_default_script;
    parse_script(script);
    s_frames_dir = argv[3];
    mkdir(s_frames_dir, 0770);
    snprintf(idx_path, sizeof(idx_path), "%s/index.tsv", s_frames_dir);
    s_index = fopen(idx_path, "w");
    if (s_index) fprintf(s_index, "seq\tt_s\ttag\tkind\tpath\tluma_mean\tluma_sd\talpha_cov\n");
    s_video = (int)envf("GRX_VIDEO", 1);
    if (s_video) {
        char mp[1200];
        snprintf(s_vdir, sizeof(s_vdir), "%s/video", s_frames_dir);
        mkdir(s_vdir, 0770);
        snprintf(mp, sizeof(mp), "%s/meta.jsonl", s_vdir);
        s_vmeta = fopen(mp, "w");
        s_vimg = (uint8_t *)malloc((size_t)4096 * 4096 * 4);
    }

    /* The disc in place, every write in the scratch tree (as tests/game_boot_arm64). */
    if (make_scratch_root(argv[1], argv[2], root, sizeof(root)) != 0) return 2;
    snprintf(save, sizeof(save), "%s/save", root);
    snprintf(cache, sizeof(cache), "%s/kgpu-cache", root);
    setenv("BLACKVR_SAVE_DIR", save, 1);
    setenv("BLACKVR_STDIO_LOGCAT", "0", 1);           /* stdio stays on the terminal: there is no logcat */
    if (chdir(root) != 0) { fprintf(stderr, "[GRX] FAIL: chdir %s\n", root); return 2; }
    mkdir("save", 0770);

    fprintf(stderr, "[GRX] game_render_x86: disc %s/game (in place), scratch %s, frames %s\n", argv[1], root, s_frames_dir);
    fprintf(stderr, "[GRX] eyes %ux%u %s, budget %.0fs, XR %.0f Hz, head sway at %.0fs or 30 stereo frames, input \"%s\"\n",
            s_eye_w, s_eye_h, s_multiview ? "multiview" : "two swapchains", s_limit, s_xr_hz, s_rot_at, script);

    /* xr_main.c: XR + Vulkan + session + renderer, before the game thread exists */
    fx_trajectory = trajectory;
    fx_now = now_ns();
    fx_input_bool = input_bool;
    fx_input_vec2 = input_vec2;
    fx_input_float = input_float;
    if (s_hand_on) { fx_hand_pose = hand_pose; fx_hand_velocity = hand_velocity; }
    memset(&so, 0, sizeof(so));
    so.multiview = s_multiview;
    so.eye_w = s_eye_w; so.eye_h = s_eye_h;
    so.game_root = root;
    so.cache_dir = cache;
    so.ini_text = getenv("GRX_INI");
    r = grx_shell_setup(&so);
    if (r < 0) {
        if (!fx_vk.device) { fprintf(stderr, "[GRX] no Vulkan device (VK_ICD_FILENAMES?): skipped\n"); return 77; }
        fprintf(stderr, "[GRX] FAIL: XR startup failed\n");
        return 1;
    }
    if (r != 1) { fprintf(stderr, "[GRX] FAIL: the renderer glue did not attach\n"); return 1; }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(fx_vk.phys, &pp);
        fprintf(stderr, "[GRX] Vulkan device: %s (API %u.%u.%u), multiview %d robustBufferAccess %d depthClamp %d BC %d\n",
                pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion),
                fx_vk.dev_multiview, fx_vk.dev_robust, fx_vk.dev_depth_clamp, fx_vk.dev_bc);
    }
    s_img = (uint8_t *)malloc((size_t)4096 * 4096 * 4);
    fx_on_end_frame = on_end_frame;
    if (s_gun_test) {
        GrxShellInfo gi;
        char mp[1200];
        grx_shell_info(&gi);
        s_snap_eye = gi.eye[0];
        fx_on_release = on_release;
        snprintf(s_gun.dir, sizeof(s_gun.dir), "%s/gunlatch", s_frames_dir);
        mkdir(s_gun.dir, 0770);
        snprintf(mp, sizeof(mp), "%s/meta.json", s_gun.dir);
        s_gun.meta = fopen(mp, "w");
        if (s_gun.meta) fprintf(s_gun.meta, "{");
        fprintf(stderr, "[GRX] gun-latch check: right controller tracked, head still, step of %.1f degrees after %d frames\n",
                s_gun_yaw * 180.0 / M_PI, (int)envf("GRX_GUN_AT", 90));
    }

    /* the game thread: 16 MB stack, as xr_main.c */
    args[0] = argv[0];
    args[1] = root;
    args[2] = NULL;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);
    if (pthread_create(&game, &at, game_thread, args) != 0) { fprintf(stderr, "[GRX] FAIL: game thread\n"); return 1; }
    pthread_attr_destroy(&at);
    grx_shell_game_started();
    memset(&prev_st, 0, sizeof(prev_st));

    /* the XR thread (this one) */
    for (;;) {
        const double t = el();
        GrxShellInfo si;
        fx_now = now_ns();
        if (s_rot_start_ns < 0 && t >= s_rot_at) {
            s_rot_start_ns = now_ns();
            fprintf(stderr, "[GRX] t=%.1fs: the head starts to sway (GRX_ROT_AT)\n", t);
        }
        /* a capture just before each scripted press */
        for (i = 0; i < s_npress; i++)
            if (s_first_frame_t >= 0 && !s_press[i].captured_before && now_s() - s_first_frame_t >= s_press[i].t - 1.5) {
                s_press[i].captured_before = 1;
                snprintf(tags[i], sizeof(tags[i]), "before-%s%d", s_press[i].button, i);
                s_cap.want_tag = tags[i];
                s_cap.want = 1;
            }
        {
            /* GRX_INPUT at run time: a file <frames>/press with "button" or "button:hold" per line adds presses now */
            char pp[1200], line[64];
            FILE *pf;
            snprintf(pp, sizeof(pp), "%s/press", s_frames_dir);
            if (s_first_frame_t >= 0 && (pf = fopen(pp, "r")) != NULL) {
                while (fgets(line, sizeof(line), pf) && s_npress < 256) {
                    char b[16] = "";
                    double hold = 0.0;
                    /* "button" or "button:hold" (released after exactly `hold` seconds) */
                    const int got = sscanf(line, "%15[a-z]:%lf", b, &hold);
                    if (got >= 1) {
                        Press *np = &s_press[s_npress++];
                        memset(np, 0, sizeof(*np));
                        np->t = now_s() - s_first_frame_t; np->hold = got == 2 && hold > 0.0 ? hold : 0.5; np->captured_before = 1;
                        np->exact = got == 2 && hold > 0.0;
                        snprintf(np->button, sizeof(np->button), "%s", b);
                    }
                }
                fclose(pf);
                unlink(pp);
            }
        }
        grx_shell_info(&si);
        if (t >= next_frame) {
            grx_shell_frame();
            next_frame = t + 1.0 / (si.renderer_started ? s_xr_hz : 2.0);
        }
        if (t >= next_progress) {
            progress(t, &prev_st, &prev_icalls, &prev_t);
            next_progress = t + 15.0;
        }
        {
            /* GRX_STALL_DUMP=<s>: no new backend frame for that long after the first one -> every thread's stack
             * (gdb attached to this process) into <frames>/stall.gdb, once (port/notes/perf.md) */
            static double last_change = -1, dump_after = -2;
            static uint64_t last_frames;
            KvkStats st;
            if (dump_after < -1) dump_after = envf("GRX_STALL_DUMP", 0);
            if (dump_after > 0 && s_first_frame_t >= 0) {
                kvk_get_stats(&st);
                if (st.frames != last_frames || last_change < 0) { last_frames = st.frames; last_change = t; }
                else if (t - last_change >= dump_after) {
                    char cmd[1600];
                    fprintf(stderr, "[GRX] t=%.1fs: STALL: no backend frame for %.0f s (frame %llu): dumping thread stacks\n", t,
                            dump_after, (unsigned long long)st.frames);
                    snprintf(cmd, sizeof(cmd), "timeout 180 gdb -p %d -batch -ex 'set pagination off' -ex 'thread apply all bt 30' "
                             "> '%s/stall.gdb' 2>&1", (int)getpid(), s_frames_dir);
                    if (system(cmd) != 0) fprintf(stderr, "[GRX] gdb failed\n");
                    report_icalls();
                    dump_after = 0;
                }
            }
        }
        if (s_game_returned) {
            fprintf(stderr, "[GRX] t=%.1fs: black_game_main returned %d\n", t, s_game_rc);
            break;
        }
        if (t >= s_limit) {
            fprintf(stderr, "[GRX] t=%.1fs: time budget reached\n", t);
            break;
        }
        {
            /* GRX_FRAMECPU=<file>: the game thread's CPU time per Kelvin flip (its CPU clock read at every flip change
             * this loop sees, every 5 ms): "flip cpu_ns wall_ns submit_mode" per line, for grx_framecpu.py */
            static FILE *fc;
            static int fc_init;
            static uint64_t fc_flips;
            static clockid_t fc_clk;
            if (!fc_init) {
                const char *p = getenv("GRX_FRAMECPU");
                fc_init = 1;
                if (p && *p && pthread_getcpuclockid(game, &fc_clk) == 0) fc = fopen(p, "w");
            }
            if (fc) {
                uint64_t k[8];
                grx_kelvin_counters(k);
                if (k[2] != fc_flips) {
                    struct timespec ts;
                    clock_gettime(fc_clk, &ts);
                    fc_flips = k[2];
                    fprintf(fc, "%llu %llu %llu %d\n", (unsigned long long)k[2],
                            (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec,
                            (unsigned long long)now_ns(), si.submit_mode);
                    if ((k[2] & 63) == 0) fflush(fc);
                }
            }
        }
        usleep(5000);
    }
    /* a last capture of whatever is shown */
    s_cap.want_tag = "final";
    s_cap.want = 1;
    fx_now = now_ns();
    grx_shell_frame();
    progress(el(), &prev_st, &prev_icalls, &prev_t);
    {
        KvkStats st;
        kvk_get_stats(&st);
        fprintf(stderr, "[GRX] RESULT: renderer frames %llu (draws %llu, world %llu, stereo %llu, HUD %llu, weapon %llu), first frame "
                        "at %.1fs, %u flat / %u stereo frames presented, %u captures, game %s%s\n",
                (unsigned long long)st.frames, (unsigned long long)st.draws, (unsigned long long)st.world_draws,
                (unsigned long long)st.stereo_draws, (unsigned long long)st.hud_draws, (unsigned long long)st.viewmodel_draws,
                s_first_frame_t >= 0 ? s_first_frame_t - s_t0 : -1.0, s_cap.flat_frames_seen, s_cap.stereo_frames_seen,
                s_cap.seq, s_game_returned ? "returned" : "still running",
                !s_gun_test ? "" : s_gun.state == GUN_DONE && s_gun.tries < 4 ? ", gun-latch frames saved" : ", gun-latch check INCOMPLETE");
    }
    if (s_index) fclose(s_index);
    grx_prof_flush();
    fflush(NULL);
    /* The game thread owns guest state the OS reclaims at exit (as on the headset: no orderly shutdown). */
    _exit(s_game_returned && s_game_rc != 0 ? 4 : 0);
}
