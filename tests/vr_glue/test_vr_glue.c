/* vr-glue: the Quest XR shell (port/quest/src) + the renderer glue (vr_renderer_kvk.c) + the real Kelvin Vulkan
 * backend, on a real Vulkan device (SwiftShader), with OpenXR faked in-process (fake_openxr.c: real VkImages, a
 * test-controlled clock and head trajectory, xrEndFrame captured). Exit 77 (skipped) without a Vulkan device.
 *
 *   setup     the shell creates instance/device through XR_KHR_vulkan_enable2 with the renderer's features, the
 *             swapchains (MUTABLE_FORMAT + sRGB/UNORM format list; one 2-layer or two eye swapchains; flat; HUD),
 *             and attaches the glue (kvk_set_* before kgpu_init, as before kelvin_start).
 *   fallback  until the renderer delivers a frame the XR thread shows the test pattern.
 *   identity  the game publishes camera 1 and marks its world pass, then runs ahead and publishes camera 2 before the
 *             GPU thread draws frame 1; the head keeps moving between the game's latch and the render-time latch.
 *             Frame 1 must be drawn with camera 1 and for exactly the eye poses the projection layer is then
 *             submitted with: checked on the poses (bitwise) and on the pixels (a world quad lands, in each eye,
 *             where the submitted pose and FOV put it, within 1 px; where the game-time pose would put it is far).
 *   one fix   the shell's clip_from_game_clip (FULL form) == the backend's correction (FULL and FROM_W) for the same
 *             poses: the math is one correction, applied once (by the backend).
 *   HUD       the HUD goes to the HUD quad layer (premultiplied, head-locked view-space quad), not into the eyes.
 *   flat      FLAT_MENU (marked) / FLAT_SCOPE (unmarked) frames go to the flat quad-layer image only.
 *   clamp     a head beyond max_head_offset: drawn and submitted for the clamped eye.
 *   threads   an XR thread runs xrs_frame concurrently with game + GPU frames: every projection layer pairs each
 *             released image with the poses it was drawn for.
 *   gun latch the game places a "weapon" quad at its hand pose through its own MVP and records that MVP; the hand is
 *             then located again in the late latch (port/notes/gun-latch.md). Same hand: the weapon stays exactly
 *             where the game put it (no delta applied). A moved and turned hand: the weapon lands, in each eye, where
 *             the late-latched hand puts it (within 1 px) while the world quad stays where the submitted pose puts
 *             it; a draw whose program does not read the weapon's registers (stale constants) and one whose MVP
 *             differs by one ulp are world; gun_latch = off keeps the game's placement; the debug tint marks
 *             exactly the weapon's pixels (two-swapchain run).
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <vulkan/vulkan.h>

#include "kelvin_gpu.h"
#include "kelvin_gfx.h"
#include "kvk_device.h"
#include "kvk_stereo.h"
#include "kvk_vr_hooks.h"
#include "psh_regs.h"
#include "nv2a_vsh_cpu/nv2a_vsh_emulator.h"
#include "vr_bridge.h"
#include "vr_game_math.h"
#include "shell_access.h"
#include "vr_settings.h"
#include "vr_panel.h"

static int s_fail, s_checks;
#define CHECK(c, ...) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

void vr_glue_counters(uint32_t *made, uint32_t *failed, uint32_t *seen);
void vr_glue_gun_counters(uint32_t *recorded, uint32_t *dropped, uint32_t *moved);

/* ---- the engine (kelvin.c) ---- */
void kelvin_unsupported(int what, const char *detail, uint32_t value) { (void)what; (void)detail; (void)value; }
uint8_t *kelvin_guest(uint32_t va) { (void)va; return NULL; }
uint32_t kelvin_dma_va(uint32_t instance, uint32_t offset, uint32_t bytes) { (void)instance; (void)offset; (void)bytes; return 0; }

/* kelvin_mark_view as kelvin.c does it: queued by the title thread, applied when the pusher reaches the word */
static struct { uint32_t va; int mode, slot; } s_marks[16];
static int s_nmarks;
int kelvin_mark_view(uint32_t va, int mode, int slot, int x0, int y0, int x1, int y1)
{
    if (!kgpu_active() || s_nmarks >= 16 || x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0) return 0;
    s_marks[s_nmarks].va = va; s_marks[s_nmarks].mode = mode; s_marks[s_nmarks].slot = slot;
    s_nmarks++;
    return 1;
}
static void pusher_reach(uint32_t va)
{
    int i, k = 0;
    for (i = 0; i < s_nmarks; i++) {
        if (s_marks[i].va == va) kgpu_set_view(s_marks[i].mode, s_marks[i].slot, 0, 0, 640, 480);
        else s_marks[k++] = s_marks[i];
    }
    s_nmarks = k;
}

/* ---- the head: a fast, smooth trajectory (stage space) ---- */
#define T_BASE 1000000000LL
#define MS 1000000LL
static float s_jump;                      /* extra sideways lean (metres), for the clamp test */

static VrPose head_at(int64_t t)
{
    const double ts = (double)(t - T_BASE) * 1e-9;
    VrPose p;
    const VrQuat qy = vrm_q_axis_angle(vrm_v3(0, 1, 0), (float)(0.35 * sin(6.0 * ts)));
    const VrQuat qx = vrm_q_axis_angle(vrm_v3(1, 0, 0), (float)(0.12 * sin(5.0 * ts + 0.4)));
    p.orientation = vrm_q_normalize(vrm_q_mul(qy, qx));
    p.position = vrm_v3((float)(0.08 * sin(4.0 * ts)) + s_jump, (float)(1.6 + 0.02 * sin(7.0 * ts)), (float)(-0.05 * sin(3.0 * ts)));
    return p;
}
static const VrFov k_fov[2] = { { -0.95f, 0.80f, 0.85f, -0.90f }, { -0.80f, 0.95f, 0.85f, -0.90f } };

static XrPosef to_xr(VrPose p)
{
    XrPosef r;
    r.orientation.x = p.orientation.x; r.orientation.y = p.orientation.y; r.orientation.z = p.orientation.z; r.orientation.w = p.orientation.w;
    r.position.x = p.position.x; r.position.y = p.position.y; r.position.z = p.position.z;
    return r;
}
static VrPose eye_of(VrPose head, int e)
{
    VrPose off = vrm_pose_identity();
    off.position.x = e == 0 ? -0.032f : 0.032f;
    return vrm_pose_mul(head, off);
}

static void trajectory(int64_t t, XrPosef *head, XrPosef eye[2], XrFovf fov[2])
{
    const VrPose h = head_at(t);
    int e;
    *head = to_xr(h);
    for (e = 0; e < 2; e++) {
        eye[e] = to_xr(eye_of(h, e));
        fov[e].angleLeft = k_fov[e].left; fov[e].angleRight = k_fov[e].right;
        fov[e].angleUp = k_fov[e].up; fov[e].angleDown = k_fov[e].down;
    }
}

/* ---- the game side (what src/black_vr.inc does at D2F40, with the body frame at the recentre origin) ---- */
#define UPM 1.5f                                   /* game units per metre */
static const VrVec3 k_point_rel = { 0.12f, 0.05f, -1.2f };   /* the world quad, relative to the recentre origin */

typedef struct GameFrame {
    VrGameCamera cam;
    VrGameCameraExt ext;
    float mvp[16];
    VrPose eye_game[2];       /* the eye poses at the game's latch, around the camera's head */
} GameFrame;

static void game_frame(GameFrame *g, uint32_t n, int mode)
{
    const VrFrameState *f = vr_latch_frame_state();
    const VrGameConfig *cfg = vr_game_config();
    VrPose origin, head_rel, head_stage, eyes[2];
    VrFov fovs[2];
    VrMat4 w_head, view, proj, mvp;
    float a, b, hx, hy;
    int e;
    vr_get_recentre(&origin);
    head_rel = vgm_relative(origin, f->head);
    head_rel.position = vgm_clamp_len(head_rel.position, cfg->max_head_offset_m);
    head_stage = vrm_pose_mul(origin, head_rel);
    w_head = vgm_lh_from_pose(head_rel, UPM);
    vrm_m4_inverse(w_head, &view);
    for (e = 0; e < 2; e++) { eyes[e] = f->eye[e].pose; fovs[e] = f->eye[e].fov; }
    vgm_fov_union(f->head, eyes, fovs, cfg->fov_margin, &hx, &hy);
    vgm_depth_row(0.05f * UPM, 500.0f * UPM, &a, &b);
    proj = vgm_game_proj(hx, hy, a, b);
    memset(g, 0, sizeof(*g));
    memcpy(g->cam.view, view.m, sizeof(g->cam.view));
    memcpy(g->cam.proj, proj.m, sizeof(g->cam.proj));
    g->cam.half_fov_x = hx; g->cam.half_fov_y = hy;
    g->cam.valid = mode == VR_PRESENT_STEREO;
    g->ext.head_pose = head_stage;
    g->ext.pose_time = f->predicted_display_time;
    g->ext.near_z = 0.05f * UPM; g->ext.far_z = 500.0f * UPM;
    g->ext.depth_a = a; g->ext.depth_b = b;
    g->ext.game_frame = n;
    g->ext.present_mode = mode;
    mvp = vrm_m4_mul(proj, view);
    memcpy(g->mvp, mvp.m, sizeof(g->mvp));
    for (e = 0; e < 2; e++) g->eye_game[e] = vrm_pose_mul(head_stage, vrm_pose_mul(vrm_pose_inv(f->head), f->eye[e].pose));
    vr_set_present_mode(mode);
    vr_publish_game_camera_ex(&g->cam, &g->ext);
}

/* ---- the GPU side: Kelvin draws as the engine issues them ---- */
typedef struct Tgt { KelvinTarget t; uint8_t *cpu, *zcpu; } Tgt;
static Tgt s_tg;
static KPixelState s_ps_world, s_ps_hud, s_ps_flat;
static Nv2aVshStep s_st[9];
static Nv2aVshProgram s_prog = { s_st };
static float s_cons[192 * 4], s_pos[4][4], s_col[4][4];
static float s_recs[4 * 8 * 4];
static uint32_t s_idx[6] = { 0, 1, 2, 2, 1, 3 };

static void vop(Nv2aVshOperation *o, Nv2aVshOpcode code, Nv2aVshRegisterType ot, unsigned oi, Nv2aVshWritemask wm)
{
    int k, q;
    memset(o, 0, sizeof(*o));
    o->opcode = code;
    o->outputs[0].type = ot; o->outputs[0].index = oi; o->outputs[0].writemask = wm;
    for (k = 0; k < 3; k++) for (q = 0; q < 4; q++) o->inputs[k].swizzle[q] = (uint8_t)q;
}
static void vin(Nv2aVshOperation *o, int k, Nv2aVshRegisterType t, unsigned idx) { o->inputs[k].type = t; o->inputs[k].index = idx; }
static void parse_in(KCombinerInput *in, uint32_t v) { in->reg = (int)(v & 0xF); in->chan = (int)(v & 0x10); in->mod = (int)(v & 0xE0); }

static void gpu_setup(void)
{
    const unsigned w = 640, h = 480;
    int k;
    memset(&s_tg, 0, sizeof(s_tg));
    s_tg.cpu = (uint8_t *)calloc((size_t)w * h, 4);
    s_tg.zcpu = (uint8_t *)calloc((size_t)w * h, 4);
    s_tg.t.color = s_tg.cpu; s_tg.t.color_va = 0x0A000000u;
    s_tg.t.zeta = s_tg.zcpu; s_tg.t.zeta_va = 0x0A100000u;
    s_tg.t.color_bpp = 4; s_tg.t.zeta_bpp = 4;
    s_tg.t.color_pitch = w * 4; s_tg.t.zeta_pitch = w * 4;
    s_tg.t.width = w; s_tg.t.height = h; s_tg.t.aa_x = s_tg.t.aa_y = 1;
    s_tg.t.clip_x1 = (int)w; s_tg.t.clip_y1 = (int)h;
    s_tg.t.guest_width = w; s_tg.t.guest_height = h;
    s_tg.t.guest_color_pitch = w * 4; s_tg.t.guest_zeta_pitch = w * 4;
    s_tg.t.guest_color_bytes = s_tg.t.guest_zeta_bytes = (size_t)w * h * 4;

    memset(&s_ps_world, 0, sizeof(s_ps_world));
    s_ps_world.color_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    s_ps_world.zeta_format = NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
    s_ps_world.mask_r = s_ps_world.mask_g = s_ps_world.mask_b = s_ps_world.mask_a = true;
    s_ps_world.smooth = true;
    s_ps_world.zmax = 16777215.0f;
    s_ps_world.zclip_min = 0.0f; s_ps_world.zclip_max = 16777215.0f;
    s_ps_world.depth_clip_cull = true;
    s_ps_world.alpha_func = 7;
    s_ps_world.clip_region[0][2] = (int)w; s_ps_world.clip_region[0][3] = (int)h;
    s_ps_world.stencil_mask_read = s_ps_world.stencil_mask_write = 0xFF;
    s_ps_world.num_stages = 1;
    s_ps_world.final_enabled = true;                 /* final combiner: the pixel is v0 (the diffuse colour) */
    for (k = 0; k < 7; k++) parse_in(&s_ps_world.fin[k], 0);
    parse_in(&s_ps_world.fin[3], 4u);
    parse_in(&s_ps_world.fin[6], 4u | 0x10u);
    s_ps_world.depth_test = true; s_ps_world.depth_write = true; s_ps_world.depth_func = 3;
    s_ps_hud = s_ps_world;                            /* the HUD: after the world, depth ALWAYS, no write, blended */
    s_ps_hud.depth_func = 7; s_ps_hud.depth_write = false;
    s_ps_hud.blend = true; s_ps_hud.blend_sfactor = 4; s_ps_hud.blend_dfactor = 5; s_ps_hud.blend_eqn = 2;
    s_ps_flat = s_ps_world;                           /* a menu / movie picture: no depth */
    s_ps_flat.depth_test = false; s_ps_flat.depth_write = false;

    /* the Xbox D3D transform: r0 = v0 . c96..99; oPos.xyz = r0.xyz * c58 / r0.w + c59; oPos.w = r0.w; oD0 = v1 */
    memset(s_st, 0, sizeof(s_st));
    for (k = 0; k < 4; k++) { vop(&s_st[k].mac, NV2AOP_DP4, NV2ART_TEMPORARY, 0, (Nv2aVshWritemask)(8 >> k)); vin(&s_st[k].mac, 0, NV2ART_INPUT, 0); vin(&s_st[k].mac, 1, NV2ART_CONTEXT, 96 + k); }
    vop(&s_st[4].mac, NV2AOP_MUL, NV2ART_TEMPORARY, 1, NV2AWM_XYZ); vin(&s_st[4].mac, 0, NV2ART_TEMPORARY, 0); vin(&s_st[4].mac, 1, NV2ART_CONTEXT, 58);
    vop(&s_st[4].ilu, NV2AOP_RCC, NV2ART_TEMPORARY, 2, NV2AWM_X); vin(&s_st[4].ilu, 0, NV2ART_TEMPORARY, 0);
    for (k = 0; k < 4; k++) s_st[4].ilu.inputs[0].swizzle[k] = 3;
    vop(&s_st[5].mac, NV2AOP_MAD, NV2ART_OUTPUT, 0, NV2AWM_XYZ); vin(&s_st[5].mac, 0, NV2ART_TEMPORARY, 1); vin(&s_st[5].mac, 1, NV2ART_TEMPORARY, 2); vin(&s_st[5].mac, 2, NV2ART_CONTEXT, 59);
    for (k = 0; k < 4; k++) s_st[5].mac.inputs[1].swizzle[k] = 0;
    vop(&s_st[6].mac, NV2AOP_MOV, NV2ART_OUTPUT, 0, NV2AWM_W); vin(&s_st[6].mac, 0, NV2ART_TEMPORARY, 0);
    vop(&s_st[7].mac, NV2AOP_MOV, NV2ART_OUTPUT, 3, NV2AWM_XYZW); vin(&s_st[7].mac, 0, NV2ART_INPUT, 1);
    s_st[7].is_final = true;
    s_cons[58 * 4 + 0] = 320.0f; s_cons[58 * 4 + 1] = -240.0f; s_cons[58 * 4 + 2] = 16777215.0f;
    s_cons[59 * 4 + 0] = 320.0f; s_cons[59 * 4 + 1] = 240.0f;
    kgpu_vs_ready(&s_prog, 0x9000u, 0, NULL, 0);
    kvk_wait_compiles();
}

/* CPU-shaded screen-space quad (menu / HUD pictures) */
static void gpu_screen_quad(const KPixelState *ps, float x0, float y0, float x1, float y1, float r, float g, float b)
{
    KGpuDraw d;
    int v, i;
    memset(s_recs, 0, sizeof(s_recs));
    for (v = 0; v < 4; v++) {
        float *rec = s_recs + v * 8 * 4;
        rec[0] = (v & 1) ? x1 : x0; rec[1] = (v & 2) ? y1 : y0; rec[2] = 0.0f; rec[3] = 1.0f;
        rec[4] = r; rec[5] = g; rec[6] = b; rec[7] = 1.0f;
        for (i = 0; i < 4; i++) rec[16 + 4 * i + 3] = 1.0f;
    }
    memset(&d, 0, sizeof(d));
    d.t = &s_tg.t; d.ps = ps;
    d.recs = s_recs; d.vcount = 4; d.stride = 8;
    for (i = 0; i < KG_SLOTS; i++) d.slot[i] = i;
    d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
    kgpu_draw(&d);
}

static VrVec3 world_point_stage(void)
{
    VrPose origin;
    vr_get_recentre(&origin);
    return vrm_pose_apply(origin, k_point_rel);
}

static int s_with_sprites;                          /* gpu_world_frame also draws gpu_sprites (below) */
static uint64_t s_cap_frame;                        /* the renderer frame the capture test asked for */
static void gpu_sprites(const GameFrame *g);
static void gpu_sprites_pre(const GameFrame *g);

/* One frame of world through game frame g (its MVP, as the title's transform constants), from push-buffer word `va`
 * (where the camera mark is), with a HUD square on top when asked. */
static void gpu_world_frame(const GameFrame *g, uint32_t va, int hud)
{
    const float half = 0.03f * UPM;
    const VrVec3 c = vrm_v3(k_point_rel.x * UPM, k_point_rel.y * UPM, -k_point_rel.z * UPM);   /* game world: LH units */
    int k, i;
    pusher_reach(va);
    kgpu_clear(&s_tg.t, true, true, true, 0xFF000000u, 0xFFFFFF00u, 0xF, 0, 0, 640, 480, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
    if (s_with_sprites) gpu_sprites_pre(g);
    for (k = 0; k < 4; k++) for (i = 0; i < 4; i++) s_cons[(96 + k) * 4 + i] = g->mvp[i * 4 + k];   /* rows */
    for (k = 0; k < 4; k++) {
        s_pos[k][0] = c.x + ((k & 1) ? half : -half); s_pos[k][1] = c.y + ((k & 2) ? -half : half);
        s_pos[k][2] = c.z; s_pos[k][3] = 1.0f;
        s_col[k][0] = 1.0f; s_col[k][1] = 0.1f; s_col[k][2] = 0.05f; s_col[k][3] = 1.0f;
    }
    for (i = 0; i < 4; i++) {              /* four depth-writing world draws come before a HUD is recognised */
        KGpuVsDraw gv;
        KGpuDraw d;
        memset(&gv, 0, sizeof(gv));
        gv.program = &s_prog; gv.hash = 0x9000u; gv.constants = s_cons;
        gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)s_pos; gv.attr[0].hi = 64;
        gv.attr[1].fmt = 4; gv.attr[1].count = 4; gv.attr[1].stride = 16; gv.attr[1].data = (const uint8_t *)s_col; gv.attr[1].hi = 64;
        memset(&d, 0, sizeof(d));
        d.t = &s_tg.t; d.ps = &s_ps_world; d.gvs = &gv;
        d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
        kgpu_draw(&d);
    }
    if (s_with_sprites) gpu_sprites(g);
    if (hud) gpu_screen_quad(&s_ps_hud, 0, 0, 64, 48, 0.0f, 1.0f, 0.0f);
    kgpu_flip(&s_tg.t, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 0, 0);
}

static void gpu_flat_frame(uint32_t va, float r, float g, float b)
{
    if (va) pusher_reach(va);
    kgpu_clear(&s_tg.t, true, true, true, 0xFF000000u, 0xFFFFFF00u, 0xF, 0, 0, 640, 480, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
    gpu_screen_quad(&s_ps_flat, 0, 0, 640, 480, r, g, b);
    gpu_screen_quad(&s_ps_flat, 0, 0, 64, 48, 0.0f, 1.0f, 0.0f);
    kgpu_flip(&s_tg.t, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 0, 0);
}

/* ---- screen-space effect sprites (BLACK's smoke, sparks, explosions; kvk_backend.c screen_sprite) ----
 * The title projects each particle on the CPU through the camera and draws a camera-facing quad in screen space:
 * program oPos = v0 * c98 + c97 (here c121 / c120), v0 = (x / 640, y / 480, 1 / w, 1), so oPos.w = 1 and the depth
 * is only in z = zmax * (A + B / w); depth-tested (LEQUAL), not written, blended. One such sprite through that GPU
 * program (blue) and one CPU-shaded the same way (magenta, w = 1). Each must land, in each eye, where that eye sees
 * its world point -- not at the game camera's pixel in both eye layers (where one eye's depth buffer then hides it). */
#define NSPRITE 4
/* 0 blue: program, depth-tested; 1 magenta: CPU-shaded, depth-tested; 2 yellow: program, no depth test (a glow), drawn
 * before the frame's first world draw; 3 white: CPU-shaded, no depth test, z = 0 (no depth given: half the far distance) */
static const VrVec3 k_sprite_rel[NSPRITE] = { { -0.20f, -0.08f, -1.6f }, { 0.26f, -0.14f, -2.0f }, { 0.04f, 0.20f, -1.8f },
                                              { -0.32f, 0.17f, -1.4f } };
static Nv2aVshStep s_st3[4];
static Nv2aVshProgram s_prog3 = { s_st3 };
static KPixelState s_ps_sprite, s_ps_glow;
static float s_scons[192 * 4], s_spos[4][4], s_scol[4][4], s_srecs[4 * 8 * 4];

static void sprite_setup(void)
{
    memset(s_st3, 0, sizeof(s_st3));
    vop(&s_st3[0].mac, NV2AOP_MUL, NV2ART_TEMPORARY, 2, NV2AWM_XYZW); vin(&s_st3[0].mac, 0, NV2ART_INPUT, 0); vin(&s_st3[0].mac, 1, NV2ART_CONTEXT, 121);
    vop(&s_st3[1].mac, NV2AOP_ADD, NV2ART_OUTPUT, 0, NV2AWM_XYZW); vin(&s_st3[1].mac, 0, NV2ART_TEMPORARY, 2); vin(&s_st3[1].mac, 1, NV2ART_CONTEXT, 120);
    vop(&s_st3[2].mac, NV2AOP_MOV, NV2ART_OUTPUT, 3, NV2AWM_XYZW); vin(&s_st3[2].mac, 0, NV2ART_INPUT, 1);
    s_st3[2].is_final = true;
    s_ps_sprite = s_ps_world;
    s_ps_sprite.depth_write = false;                      /* LEQUAL, no write, alpha blended: as the title's */
    s_ps_sprite.blend = true; s_ps_sprite.blend_sfactor = 4; s_ps_sprite.blend_dfactor = 5; s_ps_sprite.blend_eqn = 2;
    s_ps_glow = s_ps_sprite;                              /* glows and halos: no depth test, additive */
    s_ps_glow.depth_test = false;
    s_ps_glow.blend_sfactor = 1; s_ps_glow.blend_dfactor = 1;
    kgpu_vs_ready(&s_prog3, 0x9003u, 0, NULL, 0);
    kvk_wait_compiles();
}

/* the screen-space corners of a camera-facing quad (half size `half`, game units) around game-world point c:
 * (x pixels, y pixels, z in zmax units, w = 1) */
static void sprite_corners(const GameFrame *g, VrVec3 c, float half, float out[4][4])
{
    const float *m = g->mvp, *p = g->cam.proj;
    const float v[4] = { c.x, c.y, c.z, 1.0f };
    const float A = p[2 * 4 + 2] / p[2 * 4 + 3], B = p[3 * 4 + 2], zmax = 16777215.0f;
    float clip[4];
    int r, k;
    for (r = 0; r < 4; r++) clip[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] + m[3 * 4 + r] * v[3];
    for (k = 0; k < 4; k++) {
        const float nx = clip[0] / clip[3] + ((k & 1) ? 1.0f : -1.0f) * half * p[0] / clip[3];
        const float ny = clip[1] / clip[3] + ((k & 2) ? -1.0f : 1.0f) * half * p[5] / clip[3];
        out[k][0] = (nx + 1.0f) * 320.0f;
        out[k][1] = (1.0f - ny) * 240.0f;
        out[k][2] = zmax * (A + B / clip[3]);
        out[k][3] = 1.0f;
    }
}

/* sprite k through the screen-space program (v0 = (x / 640, y / 480, 1 / w, 1)) */
static void prog_sprite(const GameFrame *g, int k, const KPixelState *ps, const float col[4])
{
    const float half = 0.05f * UPM, *p = g->cam.proj;
    const float A = p[2 * 4 + 2] / p[2 * 4 + 3], B = p[3 * 4 + 2], zmax = 16777215.0f;
    float sc[4][4];
    KGpuVsDraw gv;
    KGpuDraw d;
    int v;
    sprite_corners(g, vrm_v3(k_sprite_rel[k].x * UPM, k_sprite_rel[k].y * UPM, -k_sprite_rel[k].z * UPM), half, sc);
    memcpy(s_scons, s_cons, sizeof(s_scons));
    s_scons[120 * 4 + 0] = 0.0f; s_scons[120 * 4 + 1] = 0.0f; s_scons[120 * 4 + 2] = zmax * A; s_scons[120 * 4 + 3] = 0.0f;
    s_scons[121 * 4 + 0] = 640.0f; s_scons[121 * 4 + 1] = 480.0f; s_scons[121 * 4 + 2] = zmax * B; s_scons[121 * 4 + 3] = 1.0f;
    for (v = 0; v < 4; v++) {
        s_spos[v][0] = sc[v][0] / 640.0f; s_spos[v][1] = sc[v][1] / 480.0f;
        s_spos[v][2] = (sc[v][2] / zmax - A) / B;           /* 1 / w */
        s_spos[v][3] = 1.0f;
        memcpy(s_scol[v], col, 16);
    }
    memset(&gv, 0, sizeof(gv));
    gv.program = &s_prog3; gv.hash = 0x9003u; gv.constants = s_scons;
    gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)s_spos; gv.attr[0].hi = 64;
    gv.attr[1].fmt = 4; gv.attr[1].count = 4; gv.attr[1].stride = 16; gv.attr[1].data = (const uint8_t *)s_scol; gv.attr[1].hi = 64;
    memset(&d, 0, sizeof(d));
    d.t = &s_tg.t; d.ps = ps; d.gvs = &gv;
    d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
    kgpu_draw(&d);
}

/* CPU-shaded screen-space records (x, y, z, w = 1) with per-vertex colours */
static void cpu_quad(const KPixelState *ps, float pos[4][4], float col[4][4])
{
    KGpuDraw d;
    int v, i;
    memset(s_srecs, 0, sizeof(s_srecs));
    for (v = 0; v < 4; v++) {
        float *rec = s_srecs + v * 8 * 4;
        memcpy(rec, pos[v], 16);
        memcpy(rec + 4, col[v], 16);
        for (i = 0; i < 4; i++) rec[16 + 4 * i + 3] = 1.0f;
    }
    memset(&d, 0, sizeof(d));
    d.t = &s_tg.t; d.ps = ps;
    d.recs = s_srecs; d.vcount = 4; d.stride = 8;
    for (i = 0; i < KG_SLOTS; i++) d.slot[i] = i;
    d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
    kgpu_draw(&d);
}

static void cpu_sprite(const GameFrame *g, int k, const KPixelState *ps, const float c[4], int zero_z)
{
    float sc[4][4], col[4][4];
    int v;
    sprite_corners(g, vrm_v3(k_sprite_rel[k].x * UPM, k_sprite_rel[k].y * UPM, -k_sprite_rel[k].z * UPM), 0.05f * UPM, sc);
    for (v = 0; v < 4; v++) { memcpy(col[v], c, 16); if (zero_z) sc[v][2] = 0.0f; }
    cpu_quad(ps, sc, col);
}

/* ---- offscreen: BLACK's glare chain draws its sources into 160x120 / 80x60 images of the view (kvk_backend.c
 * eye_aligned_target); a 64x64 target (not a view image) is left alone ---- */
typedef struct OffTgt { KelvinTarget t; uint8_t *cpu; } OffTgt;
static OffTgt s_off[2];
static const VrVec3 k_off_rel[2] = { { 0.18f, -0.05f, -1.7f }, { -0.15f, 0.10f, -1.5f } };   /* sprite, perspective quad */
static float s_opos[4][4], s_ocol[4][4];
static const VrVec3 k_shaft_rel = { -1.1f, 0.0f, -2.2f };
static void project(VrPose eye, VrFov fov, VrVec3 xs, unsigned w, unsigned h, double *px, double *py);

static void off_setup(void)
{
    static const unsigned sz[2][2] = { { 160, 120 }, { 64, 64 } };
    int i;
    for (i = 0; i < 2; i++) {
        OffTgt *o = &s_off[i];
        const unsigned w = sz[i][0], h = sz[i][1];
        memset(o, 0, sizeof(*o));
        o->cpu = (uint8_t *)calloc((size_t)w * h, 4);
        o->t.color = o->cpu; o->t.color_va = 0x0A200000u + (uint32_t)i * 0x100000u;
        o->t.color_bpp = 4; o->t.color_pitch = w * 4;
        o->t.width = w; o->t.height = h; o->t.aa_x = o->t.aa_y = 1;
        o->t.clip_x1 = (int)w; o->t.clip_y1 = (int)h;
        o->t.guest_width = w; o->t.guest_height = h;
        o->t.guest_color_pitch = w * 4; o->t.guest_color_bytes = (size_t)w * h * 4;
    }
}

/* into target i: the sprite through the screen-space program (normalised x, y as the title's), and (160x120 only) a
 * perspective quad through the transform program with that target's viewport rows */
static void off_draws(const GameFrame *g, int i)
{
    static const float blue[4] = { 0.05f, 0.1f, 1.0f, 1.0f };
    KelvinTarget save = s_tg.t;
    KPixelState ps = s_ps_glow;
    float c58[4], c59[4];
    int k, j;
    ps.depth_test = false;
    s_tg.t = s_off[i].t;                                   /* prog_sprite draws into s_tg.t */
    {
        /* the program maps v0 = (x/640, y/480, 1/w, 1) through c121 = (W, H, ...): this target's pixels */
        const float *p = g->cam.proj;
        const float A = p[2 * 4 + 2] / p[2 * 4 + 3], B = p[3 * 4 + 2], zmax = 16777215.0f, half = 0.05f * UPM;
        float sc[4][4];
        KGpuVsDraw gv;
        KGpuDraw d;
        sprite_corners(g, vrm_v3(k_off_rel[0].x * UPM, k_off_rel[0].y * UPM, -k_off_rel[0].z * UPM), half, sc);
        memcpy(s_scons, s_cons, sizeof(s_scons));
        s_scons[120 * 4 + 2] = zmax * A; s_scons[120 * 4 + 0] = s_scons[120 * 4 + 1] = s_scons[120 * 4 + 3] = 0.0f;
        s_scons[121 * 4 + 0] = (float)s_off[i].t.guest_width; s_scons[121 * 4 + 1] = (float)s_off[i].t.guest_height;
        s_scons[121 * 4 + 2] = zmax * B; s_scons[121 * 4 + 3] = 1.0f;
        for (k = 0; k < 4; k++) {
            s_spos[k][0] = sc[k][0] / 640.0f; s_spos[k][1] = sc[k][1] / 480.0f;
            s_spos[k][2] = (sc[k][2] / zmax - A) / B; s_spos[k][3] = 1.0f;
            memcpy(s_scol[k], blue, 16);
        }
        memset(&gv, 0, sizeof(gv));
        gv.program = &s_prog3; gv.hash = 0x9003u; gv.constants = s_scons;
        gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)s_spos; gv.attr[0].hi = 64;
        gv.attr[1].fmt = 4; gv.attr[1].count = 4; gv.attr[1].stride = 16; gv.attr[1].data = (const uint8_t *)s_scol; gv.attr[1].hi = 64;
        memset(&d, 0, sizeof(d));
        d.t = &s_tg.t; d.ps = &ps; d.gvs = &gv;
        d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
        kgpu_clear(&s_tg.t, true, false, false, 0x00000000u, 0, 0xF, 0, 0, (int)s_tg.t.guest_width, (int)s_tg.t.guest_height,
                   NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
        kgpu_draw(&d);
    }
    if (i == 0) {
        const VrVec3 c = vrm_v3(k_off_rel[1].x * UPM, k_off_rel[1].y * UPM, -k_off_rel[1].z * UPM);
        const float half = 0.05f * UPM;
        KGpuVsDraw gv;
        KGpuDraw d;
        memcpy(c58, &s_cons[58 * 4], 16); memcpy(c59, &s_cons[59 * 4], 16);
        s_cons[58 * 4 + 0] = 80.0f; s_cons[58 * 4 + 1] = -60.0f; s_cons[59 * 4 + 0] = 80.0f; s_cons[59 * 4 + 1] = 60.0f;
        for (k = 0; k < 4; k++) {
            s_opos[k][0] = c.x + ((k & 1) ? half : -half); s_opos[k][1] = c.y + ((k & 2) ? -half : half);
            s_opos[k][2] = c.z; s_opos[k][3] = 1.0f;
            for (j = 0; j < 4; j++) s_ocol[k][j] = j == 0 || j == 3 ? 1.0f : 0.05f;
        }
        memset(&gv, 0, sizeof(gv));
        gv.program = &s_prog; gv.hash = 0x9000u; gv.constants = s_cons;
        gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)s_opos; gv.attr[0].hi = 64;
        gv.attr[1].fmt = 4; gv.attr[1].count = 4; gv.attr[1].stride = 16; gv.attr[1].data = (const uint8_t *)s_ocol; gv.attr[1].hi = 64;
        memset(&d, 0, sizeof(d));
        d.t = &s_tg.t; d.ps = &ps; d.gvs = &gv;
        d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
        kgpu_draw(&d);
        memcpy(&s_cons[58 * 4], c58, 16); memcpy(&s_cons[59 * 4], c59, 16);
    }
    s_tg.t = save;
}

/* the offscreen layers: in the 160x120 view image each eye's layer has the sprite and the quad where that eye sees
 * them; the 64x64 target has the game camera's picture in both layers */
static void check_offscreen(const char *what, const VrRenderPose *rp)
{
    uint8_t *img = (uint8_t *)malloc(160 * 120 * 4);
    VrPose origin;
    double at[2][2] = { { 0, 0 }, { 0, 0 } };
    int i, e, k;
    vr_get_recentre(&origin);
    for (i = 0; i < 2; i++)
        for (e = 0; e < 2; e++) {
            unsigned w = 0, h = 0, x, y;
            CHECK(kvk_read_surface(s_off[i].t.color_va, (unsigned)e, img, (size_t)s_off[i].t.guest_width * 4, &w, &h, 160 * 120 * 4),
                  "%s: offscreen %ux%u layer %d read back", what, s_off[i].t.guest_width, s_off[i].t.guest_height, e);
            for (k = 0; k < (i == 0 ? 2 : 1); k++) {
                const VrVec3 xs = vrm_pose_apply(origin, k_off_rel[k]);
                double sx = 0, sy = 0, wx, wy;
                int n = 0;
                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++) {
                        const uint8_t *p = img + ((size_t)y * w + x) * 4;          /* BGRA (A8R8G8B8 surface) */
                        const int hit = k == 0 ? p[0] > 200 && p[2] < 60 : p[2] > 200 && p[0] < 60;
                        if (hit) { sx += x + 0.5; sy += y + 0.5; n++; }
                    }
                if (n) { sx /= n; sy /= n; }
                if (i == 0) {
                    project(rp->eye_pose[e], rp->eye_fov[e], xs, w, h, &wx, &wy);
                    CHECK(n > 4 && fabs(sx - wx) < 0.75 && fabs(sy - wy) < 0.75,
                          "%s: glare target layer %d %s at %.2f,%.2f, eye %d sees it at %.2f,%.2f (%d px)", what, e,
                          k ? "perspective quad" : "screen-space sprite", sx, sy, e, wx, wy, n);
                    printf("%s: glare target layer %d %s %.2f,%.2f  eye sees %.2f,%.2f (%d px)\n", what, e,
                           k ? "perspective quad" : "screen-space sprite", sx, sy, wx, wy, n);
                } else {
                    CHECK(n > 0, "%s: 64x64 target layer %d sprite drawn (%d px)", what, e, n);
                    at[e][0] = sx; at[e][1] = sy;
                }
            }
        }
    CHECK(fabs(at[0][0] - at[1][0]) < 0.01 && fabs(at[0][1] - at[1][1]) < 0.01,
          "%s: the 64x64 (not a view image) target is left alone: same pixel in both layers (%.2f vs %.2f)", what, at[0][0], at[1][0]);
    free(img);
}

/* before the world: a glow */
static void gpu_sprites_pre(const GameFrame *g)
{
    static const float yellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    prog_sprite(g, 2, &s_ps_glow, yellow);
}

/* after the world: two depth-tested effect sprites, a glow without depth, then a full-screen additive pass (a
 * blue gradient 0..40 across the screen: it must stay pixel-aligned in each eye) */
static void gpu_sprites(const GameFrame *g)
{
    static const float blue[4] = { 0.05f, 0.1f, 1.0f, 1.0f }, magenta[4] = { 1.0f, 0.1f, 1.0f, 1.0f }, white[4] = { 1, 1, 1, 1 };
    float fp[4][4], fc[4][4];
    int v;
    prog_sprite(g, 0, &s_ps_sprite, blue);
    cpu_sprite(g, 1, &s_ps_sprite, magenta, 0);
    cpu_sprite(g, 3, &s_ps_glow, white, 1);
    for (v = 0; v < 4; v++) {
        fp[v][0] = (v & 1) ? 640.0f : 0.0f; fp[v][1] = (v & 2) ? 480.0f : 0.0f; fp[v][2] = 0.0f; fp[v][3] = 1.0f;
        fc[v][0] = 0.0f; fc[v][1] = 0.0f; fc[v][2] = (v & 1) ? 40.0f / 255.0f : 0.0f; fc[v][3] = 1.0f;
    }
    cpu_quad(&s_ps_glow, fp, fc);
    /* a light shaft: a tall additive column over a fire, the screen's whole height but narrow (not full-screen) */
    {
        float sc[4][4], col[4][4];
        sprite_corners(g, vrm_v3(k_shaft_rel.x * UPM, k_shaft_rel.y * UPM, -k_shaft_rel.z * UPM), 0.04f * UPM, sc);
        for (v = 0; v < 4; v++) {
            sc[v][1] = (v & 2) ? 480.0f : 0.0f;
            col[v][0] = 1.0f; col[v][1] = 0.5f; col[v][2] = 0.0f; col[v][3] = 1.0f;
        }
        cpu_quad(&s_ps_glow, sc, col);
    }
    off_draws(g, 0);
    off_draws(g, 1);
}

/* ---- reading the swapchain images back (raw bytes: RGBA8) ---- */

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(fx_vk.phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

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

/* the eye image the shell last released for eye e */
static void read_eye(int e, uint8_t *out)
{
    const int mv = shell_multiview();
    FxSwapchain *sc = fx_swapchain(shell_eye_swapchain(mv ? 0 : e));
    read_image(sc->img[sc->last_released], mv ? (uint32_t)e : 0u, shell_eye_width(), shell_eye_height(), out);
}

static void read_quad_image(XrSwapchain h, uint8_t *out)
{
    FxSwapchain *sc = fx_swapchain(h);
    read_image(sc->img[sc->last_released], 0, sc->ci.width, sc->ci.height, out);
}

/* centroid of the reddish world quad (RGBA), and the mean green of its pixels */
static int red_centroid(const uint8_t *img, unsigned w, unsigned h, double *cx, double *cy, double *green)
{
    double sx = 0, sy = 0, sg = 0;
    unsigned x, y;
    int n = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const uint8_t *p = img + ((size_t)y * w + x) * 4;
            if (p[0] > 200 && p[1] < 100 && p[2] < 100) { sx += x + 0.5; sy += y + 0.5; sg += p[1]; n++; }
        }
    if (n) { *cx = sx / n; *cy = sy / n; *green = sg / n; }
    return n;
}
static int count_green(const uint8_t *img, unsigned w, unsigned h)
{
    unsigned i;
    int n = 0;
    for (i = 0; i < w * h; i++) if (img[i * 4 + 1] > 200 && img[i * 4] < 60) n++;
    return n;
}

/* where a stage point appears in an eye image drawn for (pose, fov) */
static void project(VrPose eye, VrFov fov, VrVec3 xs, unsigned w, unsigned h, double *px, double *py)
{
    const VrVec3 p = vrm_pose_apply(vrm_pose_inv(eye), xs);
    const double tx = p.x / -p.z, ty = p.y / -p.z;
    const double l = tan(fov.left), r = tan(fov.right), u = tan(fov.up), d = tan(fov.down);
    *px = (tx - l) / (r - l) * w;
    *py = (u - ty) / (u - d) * h;
}

static int pose_equal(XrPosef a, VrPose b)
{
    const XrPosef c = to_xr(b);
    return !memcmp(&a, &c, sizeof(a));
}

/* ---- checks shared by the stereo frames ---- */

/* The eye images of the frame just presented: the world quad where the SUBMITTED poses put it. Returns the larger
 * distance (px) between the submitted-pose position and the game-time-pose position over the two eyes. */
static double check_stereo_pixels(const char *what, const VrRenderPose *rp, const GameFrame *g, int quiet)
{
    const unsigned W = shell_eye_width(), H = shell_eye_height();
    uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4);
    const VrVec3 xs = world_point_stage();
    double contrast = 0.0;
    int e;
    for (e = 0; e < 2; e++) {
        double cx = 0, cy = 0, gr = 0, wx, wy, gx, gy;
        int n;
        read_eye(e, img);
        n = red_centroid(img, W, H, &cx, &cy, &gr);
        project(rp->eye_pose[e], rp->eye_fov[e], xs, W, H, &wx, &wy);
        project(g->eye_game[e], rp->eye_fov[e], xs, W, H, &gx, &gy);
        CHECK(n > 30, "%s: eye %d world quad drawn (%d px)", what, e, n);
        CHECK(fabs(cx - wx) < 1.0 && fabs(cy - wy) < 1.0, "%s: eye %d quad at %.2f,%.2f, submitted pose puts it at %.2f,%.2f",
              what, e, cx, cy, wx, wy);
        if (hypot(gx - wx, gy - wy) > contrast) contrast = hypot(gx - wx, gy - wy);
        /* 0.1 written through the UNORM view of the sRGB image: 26, not the sRGB encoding (89) */
        CHECK(n == 0 || fabs(gr - 25.5) < 4.0, "%s: eye %d gamma-space colour kept (green %.1f, want ~26)", what, e, gr);
        CHECK(count_green(img, W, H) == 0, "%s: eye %d has no HUD pixels", what, e);
        if (!quiet)
            printf("%s: eye %d quad %.2f,%.2f  submitted pose %.2f,%.2f  game-time pose %.2f,%.2f\n", what, e, cx, cy, wx, wy, gx, gy);
    }
    free(img);
    return contrast;
}

static void check_layers_pose_identity(const char *what, const VrRenderPose *rp)
{
    int e;
    CHECK(fx_last_end.has_proj, "%s: projection layer submitted", what);
    CHECK(fx_last_end.proj.space == FX_SPACE_STAGE && fx_last_end.proj.viewCount == 2, "%s: projection layer in stage space", what);
    for (e = 0; e < 2; e++) {
        CHECK(pose_equal(fx_last_end.views[e].pose, rp->eye_pose[e]), "%s: eye %d submitted pose == rendered pose", what, e);
        CHECK(fx_last_end.views[e].fov.angleLeft == rp->eye_fov[e].left && fx_last_end.views[e].fov.angleUp == rp->eye_fov[e].up,
              "%s: eye %d submitted fov == rendered fov", what, e);
        CHECK(fx_last_end.views[e].subImage.imageArrayIndex == (uint32_t)(shell_multiview() ? e : 0), "%s: eye %d array index", what, e);
    }
    CHECK(shell_submit_latch_serial() == rp->serial, "%s: the submitted views are latch %llu (rendered with %llu)", what,
          (unsigned long long)shell_submit_latch_serial(), (unsigned long long)rp->serial);
}

/* where sprite k should be: its world point; the white one (no depth given) along the game camera's ray through it at
 * half the far distance (view depth) */
static VrVec3 sprite_expected(const GameFrame *g, int k)
{
    VrPose origin;
    VrVec3 xs;
    vr_get_recentre(&origin);
    xs = vrm_pose_apply(origin, k_sprite_rel[k]);
    if (k == 3) {
        const float *p = g->cam.proj;
        const float A = p[2 * 4 + 2] / p[2 * 4 + 3], B = p[3 * 4 + 2];
        const float wf = 0.5f * B / (1.0f - A) / UPM;                      /* metres */
        const VrPose cam = g->ext.head_pose;
        const VrVec3 pc = vrm_pose_apply(vrm_pose_inv(cam), xs);           /* camera space, -z forward */
        const float s = wf / -pc.z;
        xs = vrm_pose_apply(cam, vrm_v3(pc.x * s, pc.y * s, pc.z * s));
    }
    return xs;
}

static int sprite_hit(int k, const uint8_t *p)
{
    switch (k) {
    case 0: return p[2] > 200 && p[0] < 60 && p[1] < 60;                    /* blue */
    case 1: return p[0] > 200 && p[2] > 200 && p[1] < 60;                   /* magenta */
    case 2: return p[0] > 200 && p[1] > 200 && p[2] < 60;                   /* yellow */
    default: return p[0] > 200 && p[1] > 200 && p[2] > 200;                 /* white */
    }
}

/* the sprites of gpu_sprites(_pre): each in each eye where the submitted pose sees it; the full-screen pass
 * pixel-aligned */
static void check_sprite_pixels(const char *what, const VrRenderPose *rp, const GameFrame *g)
{
    static const char *name[NSPRITE] = { "screen-space program", "CPU-shaded", "no-depth-test glow (program, before the world)",
                                         "no-depth-test glow without z (CPU-shaded)" };
    const unsigned W = shell_eye_width(), H = shell_eye_height();
    uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4);
    double at[2][NSPRITE][2];
    int e, k;
    for (e = 0; e < 2; e++) {
        read_eye(e, img);
        for (k = 0; k < NSPRITE; k++) {
            const VrVec3 xs = sprite_expected(g, k);
            double sx = 0, sy = 0, wx, wy;
            unsigned x, y;
            int n = 0;
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++)
                    if (sprite_hit(k, img + ((size_t)y * W + x) * 4)) { sx += x + 0.5; sy += y + 0.5; n++; }
            project(rp->eye_pose[e], rp->eye_fov[e], xs, W, H, &wx, &wy);
            if (n) { sx /= n; sy /= n; }
            at[e][k][0] = sx; at[e][k][1] = sy;
            CHECK(n > 30, "%s: eye %d %s sprite drawn (%d px)", what, e, name[k], n);
            CHECK(n > 30 && fabs(sx - wx) < 1.5 && fabs(sy - wy) < 1.5,
                  "%s: eye %d %s sprite at %.2f,%.2f, the submitted pose sees it at %.2f,%.2f", what, e, name[k], sx, sy, wx, wy);
            printf("%s: eye %d sprite %d (%s) %.2f,%.2f  submitted pose %.2f,%.2f (%d px)\n", what, e, k, name[k], sx, sy,
                   wx, wy, n);
        }
        /* the full-screen additive gradient: blue 40 * x / W in every eye, on the empty top rows */
        {
            static const double fx[3] = { 0.01, 0.5, 0.99 };
            int q;
            for (q = 0; q < 3; q++) {
                const unsigned x = (unsigned)(fx[q] * W);
                const uint8_t *p = img + ((size_t)3 * W + x) * 4;
                const double want = 40.0 * (x + 0.5) / W;
                CHECK(fabs(p[2] - want) <= 3.0 && p[0] < 8 && p[1] < 8,
                      "%s: eye %d full-screen pass pixel-aligned at x %u (blue %u, want %.1f)", what, e, x, p[2], want);
            }
        }
    }
    /* uncorrected, a sprite sits at the same pixel in both eye layers: the eye images must differ by the parallax */
    for (k = 0; k < NSPRITE; k++)
        CHECK(fabs(at[0][k][0] - at[1][k][0]) > 8.0, "%s: sprite %d differs between the eyes (%.2f vs %.2f px)", what, k,
              at[0][k][0], at[1][k][0]);
    free(img);
}

/* the light shaft: corrected (it does not reach every edge), at its world point's x on that point's row, in each eye */
static void check_shaft(const char *what, const VrRenderPose *rp)
{
    const unsigned W = shell_eye_width(), H = shell_eye_height();
    uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4);
    VrPose origin;
    double xe[2] = { 0, 0 };
    int e;
    vr_get_recentre(&origin);
    for (e = 0; e < 2; e++) {
        const VrVec3 xs = vrm_pose_apply(origin, k_shaft_rel);
        double wx, wy, sx = 0;
        int n = 0, rows = 0, y, x;
        read_eye(e, img);
        project(rp->eye_pose[e], rp->eye_fov[e], xs, W, H, &wx, &wy);
        for (y = (int)wy - 2; y <= (int)wy + 2; y++) {
            if (y < 0 || y >= (int)H) continue;
            rows++;
            for (x = 0; x < (int)W; x++) {
                const uint8_t *p = img + ((size_t)y * W + x) * 4;
                if (p[0] > 200 && p[1] > 100 && p[1] < 170 && p[2] < 60) { sx += x + 0.5; n++; }
            }
        }
        if (n) sx /= n;
        xe[e] = sx;
        CHECK(n >= 3 * rows && fabs(sx - wx) < 1.5, "%s: eye %d light shaft at x %.2f, the eye sees its point at %.2f (%d px)", what,
              e, sx, wx, n);
        printf("%s: eye %d light shaft x %.2f  submitted pose %.2f (%d px in %d rows)\n", what, e, sx, wx, n, rows);
    }
    CHECK(fabs(xe[0] - xe[1]) > 8.0, "%s: the light shaft differs between the eyes (%.2f vs %.2f)", what, xe[0], xe[1]);
    free(img);
}

/* ---- both-eye capture: the PNG the glue wrote (zlib stored blocks, as vr_renderer_kvk.c png_write) ---- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint8_t *png_read_stored(const char *path, uint32_t *w, uint32_t *h)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf, *z = NULL, *out = NULL;
    long n;
    size_t zn = 0, at = 8, o = 0, zi = 2;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *w = *h = 0;
    while (at + 12 <= (size_t)n) {
        const uint32_t len = be32(buf + at);
        if (!memcmp(buf + at + 4, "IHDR", 4)) { *w = be32(buf + at + 8); *h = be32(buf + at + 12); }
        if (!memcmp(buf + at + 4, "IDAT", 4)) { z = (uint8_t *)realloc(z, zn + len); memcpy(z + zn, buf + at + 8, len); zn += len; }
        at += 12 + len;
    }
    free(buf);
    if (!*w || !*h || !z) { free(z); return NULL; }
    out = (uint8_t *)malloc(((size_t)*w * 3 + 1) * *h);
    while (zi + 5 <= zn) {                                   /* stored blocks: BFINAL/BTYPE, LEN, NLEN, data */
        const int last = z[zi] & 1;
        const uint32_t len = z[zi + 1] | (uint32_t)z[zi + 2] << 8;
        if ((z[zi] & 6) != 0 || o + len > ((size_t)*w * 3 + 1) * *h) { free(out); out = NULL; break; }
        memcpy(out + o, z + zi + 5, len);
        o += len; zi += 5 + len;
        if (last) break;
    }
    free(z);
    return out;                                              /* rows: filter byte (0) + RGB */
}

uint32_t vr_glue_captures_written(void);

static void check_capture(const char *what, uint64_t frame)
{
    const unsigned W = shell_eye_width(), H = shell_eye_height();
    uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4);
    char want[64];
    int e;
    snprintf(want, sizeof(want), "_f%llu_", (unsigned long long)frame);
    for (e = 0; e < 2; e++) {
        DIR *dp = opendir(vr_capture_dir());
        struct dirent *de;
        char path[800] = "";
        uint8_t *png;
        uint32_t pw = 0, ph = 0, x, y;
        int worst = 0;
        while (dp && (de = readdir(dp)) != NULL) {
            const size_t l = strlen(de->d_name);
            if (strstr(de->d_name, want) && l > 6 && de->d_name[l - 5] == (e ? 'R' : 'L'))
                snprintf(path, sizeof(path), "%s/%s", vr_capture_dir(), de->d_name);
        }
        if (dp) closedir(dp);
        CHECK(path[0], "%s: eye %d capture file %s*%c.png in %s", what, e, want, e ? 'R' : 'L', vr_capture_dir());
        if (!path[0]) continue;
        png = png_read_stored(path, &pw, &ph);
        CHECK(png && pw == W && ph == H, "%s: eye %d capture is a %ux%u PNG (%ux%u)", what, e, W, H, pw, ph);
        if (!png || pw != W || ph != H) { free(png); continue; }
        read_eye(e, img);
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                const uint8_t *a = png + (size_t)y * (W * 3 + 1) + 1 + 3 * x, *b = img + ((size_t)y * W + x) * 4;
                int c;
                for (c = 0; c < 3; c++) if (abs(a[c] - b[c]) > worst) worst = abs(a[c] - b[c]);
            }
        CHECK(worst <= 1, "%s: eye %d capture == the released eye image (worst %d)", what, e, worst);
        printf("%s: eye %d capture %s, worst difference to the eye image %d\n", what, e, path, worst);
        free(png);
        remove(path);
    }
    free(img);
}

/* The shell's clip_from_game_clip (FULL form, VrEyeTransforms) and the backend's correction (kvk_stereo: FULL and
 * FROM_W) for the same render pose: one correction. */
static void check_one_correction(const GameFrame *g, const VrRenderPose *rp)
{
    const VrEyeTransforms *tf = vr_eye_transforms();
    float pg[16];
    int e, k;
    double worst = 0.0;
    CHECK(tf->valid && tf->clip_valid, "shell eye transforms valid");
    CHECK(kvk_game_projection(pg, &g->cam, 0.1f, 4000.0f), "game projection usable");
    for (e = 0; e < 2; e++) {
        float E[16], PE[16], full[16], fromw[16];
        CHECK(pose_equal(to_xr(tf->pose[e]), rp->eye_pose[e]), "VrEyeTransforms.pose[%d] == VrRenderPose.eye_pose", e);
        kvk_eye_from_head(E, &rp->game_head, &rp->eye_pose[e], rp->world_scale, pg);
        kvk_eye_projection(PE, &rp->eye_fov[e], pg);
        kvk_eye_clip(full, KVK_STEREO_FULL, PE, E, pg);
        kvk_eye_clip(fromw, KVK_STEREO_FROM_W, PE, E, pg);
        for (k = 0; k < 64; k++) {
            const float z = 0.3f + (float)k * 1.7f;
            const float v[4] = { z * 0.9f * (float)sin(k * 1.3), z * 0.7f * (float)cos(k * 0.7), z, 1.0f };
            float cg[4], a[4], b[4], c[4], w1[4];
            kvk_mat4_apply(cg, pg, v);
            vrm_m4_apply(tf->clip_from_game_clip[e], cg, a);
            kvk_mat4_apply(b, full, cg);
            w1[0] = cg[0]; w1[1] = cg[1]; w1[2] = cg[3]; w1[3] = 1.0f;
            kvk_mat4_apply(c, fromw, w1);
            worst = fmax(worst, fabs(a[0] / a[3] - b[0] / b[3]));
            worst = fmax(worst, fabs(a[1] / a[3] - b[1] / b[3]));
            worst = fmax(worst, fabs(a[2] / a[3] - b[2] / b[3]));
            worst = fmax(worst, fabs(c[0] / c[3] - b[0] / b[3]));
            worst = fmax(worst, fabs(c[1] / c[3] - b[1] / b[3]));
        }
    }
    CHECK(worst < 2e-4, "shell clip_from_game_clip == backend FULL == backend FROM_W (worst NDC difference %.2e)", worst);
    printf("one correction: shell FULL vs backend FULL vs FROM_W, worst NDC difference %.2e\n", worst);
}

/* ---- gun latch ---- */

static VrPose s_hand_late;                       /* what the fake runtime reports for the right aim hand */
static int s_hand_tracked;
static int hand_pose(int hand, int grip, XrTime t, XrPosef *pose)
{
    (void)t;
    if (hand != 1 || grip || !s_hand_tracked) return 0;
    *pose = to_xr(s_hand_late);
    return 1;
}

static const VrVec3 k_grip = { 0.0f, -0.02f, -0.20f };     /* the weapon quad's centre in the aim hand's frame (m) */
static const VrVec3 k_point2_rel = { -0.25f, -0.10f, -1.5f }; /* a second world quad (drawn by a program reading c100) */
static Nv2aVshStep s_st2[9];
static Nv2aVshProgram s_prog2 = { s_st2 };
static float s_gcons[192 * 4], s_wcons2[192 * 4], s_ncons[192 * 4];
static float s_gpos[4][4], s_gcol[4][4], s_pos2[4][4], s_col2[4][4], s_npos[4][4], s_ncol[4][4];

/* a stage point as the test's game world (recentre-origin LH, game units) */
static VrVec3 game_world_of(VrVec3 stage)
{
    VrPose origin;
    VrVec3 r;
    vr_get_recentre(&origin);
    r = vrm_pose_apply(vrm_pose_inv(origin), stage);
    return vrm_v3(r.x * UPM, r.y * UPM, -r.z * UPM);
}

static void gun_setup(void)
{
    int k;
    /* the same transform with its matrix in c100..c103 */
    memcpy(s_st2, s_st, sizeof(s_st2));
    for (k = 0; k < 4; k++) s_st2[k].mac.inputs[1].index = 100 + k;
    kgpu_vs_ready(&s_prog2, 0x9002u, 0, NULL, 0);
    kvk_wait_compiles();
}

static void quad_at(float pos[4][4], VrVec3 c, float half)
{
    int k;
    for (k = 0; k < 4; k++) {
        pos[k][0] = c.x + ((k & 1) ? half : -half); pos[k][1] = c.y + ((k & 2) ? -half : half);
        pos[k][2] = c.z; pos[k][3] = 1.0f;
    }
}

static void draw_quad(const float *cons, const Nv2aVshProgram *prog, uint32_t hash, float pos[4][4], float col[4][4])
{
    KGpuVsDraw gv;
    KGpuDraw d;
    memset(&gv, 0, sizeof(gv));
    gv.program = prog; gv.hash = hash; gv.constants = cons;
    gv.attr[0].fmt = 4; gv.attr[0].count = 4; gv.attr[0].stride = 16; gv.attr[0].data = (const uint8_t *)pos; gv.attr[0].hi = 64;
    gv.attr[1].fmt = 4; gv.attr[1].count = 4; gv.attr[1].stride = 16; gv.attr[1].data = (const uint8_t *)col; gv.attr[1].hi = 64;
    memset(&d, 0, sizeof(d));
    d.t = &s_tg.t; d.ps = &s_ps_world; d.gvs = &gv;
    d.indices = s_idx; d.icount = 6; d.topology = KG_TOPO_LIST; d.index_limit = 4;
    kgpu_draw(&d);
}

/* The game side of one weapon frame: the weapon at hand_game (+ the grip), drawn through its own MVP (camera MVP *
 * the weapon's model matrix) in c96..c99; that MVP recorded after the camera mark, as black_vr.inc does. */
static void gun_game_frame(GameFrame *g, uint32_t n, VrPose hand_game, int valid, uint32_t va)
{
    VrGameGun gun;
    VrMat4 mvp, model = vrm_m4_identity(), gmvp;
    VrVec3 c;
    int k, i;
    memset(&gun, 0, sizeof(gun));
    gun.hand = hand_game; gun.hand_index = 1; gun.valid = valid;
    vr_publish_game_gun(&gun);
    game_frame(g, n, VR_PRESENT_STEREO);
    vr_mark_world_pass(va, 1);
    c = game_world_of(vrm_pose_apply(hand_game, k_grip));
    VRM_AT(model, 0, 3) = c.x; VRM_AT(model, 1, 3) = c.y; VRM_AT(model, 2, 3) = c.z;
    memcpy(mvp.m, g->mvp, sizeof(mvp.m));
    gmvp = vrm_m4_mul(mvp, model);
    memcpy(s_gcons, s_cons, sizeof(s_gcons));
    for (k = 0; k < 4; k++) for (i = 0; i < 4; i++) s_gcons[(96 + k) * 4 + i] = gmvp.m[i * 4 + k];   /* rows */
    vr_mark_viewmodel_mvp(&s_gcons[96 * 4], 96);
    /* the near miss: the weapon's MVP with its last bit changed (another object, as far as anyone can tell) */
    memcpy(s_ncons, s_gcons, sizeof(s_ncons));
    {
        uint32_t b;
        memcpy(&b, &s_ncons[96 * 4 + 3], 4); b ^= 1u; memcpy(&s_ncons[96 * 4 + 3], &b, 4);
    }
    /* the second world quad's program reads c100..c103 (the camera MVP) while c96..c99 still hold the weapon's */
    memcpy(s_wcons2, s_gcons, sizeof(s_wcons2));
    for (k = 0; k < 4; k++) for (i = 0; i < 4; i++) s_wcons2[(100 + k) * 4 + i] = g->mvp[i * 4 + k];
}

/* The GPU side: the world quads, the weapon, the stale-constant world quad, the near miss, the flip. */
static void gun_gpu_frame(const GameFrame *g, uint32_t va)
{
    const float half = 0.03f * UPM;
    const VrVec3 c2 = vrm_v3(k_point2_rel.x * UPM, k_point2_rel.y * UPM, -k_point2_rel.z * UPM);
    int k;
    pusher_reach(va);
    kgpu_clear(&s_tg.t, true, true, true, 0xFF000000u, 0xFFFFFF00u, 0xF, 0, 0, 640, 480, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8);
    {
        const VrVec3 c = vrm_v3(k_point_rel.x * UPM, k_point_rel.y * UPM, -k_point_rel.z * UPM);
        for (k = 0; k < 4; k++) for (int i = 0; i < 4; i++) s_cons[(96 + k) * 4 + i] = g->mvp[i * 4 + k];
        quad_at(s_pos, c, half);
        for (k = 0; k < 4; k++) { s_col[k][0] = 1.0f; s_col[k][1] = 0.1f; s_col[k][2] = 0.05f; s_col[k][3] = 1.0f; }
        for (k = 0; k < 4; k++) draw_quad(s_cons, &s_prog, 0x9000u, s_pos, s_col);
    }
    quad_at(s_gpos, vrm_v3(0, 0, 0), 0.012f * UPM);
    for (k = 0; k < 4; k++) { s_gcol[k][0] = 0.05f; s_gcol[k][1] = 0.1f; s_gcol[k][2] = 1.0f; s_gcol[k][3] = 1.0f; }
    draw_quad(s_gcons, &s_prog, 0x9000u, s_gpos, s_gcol);                 /* the weapon */
    quad_at(s_pos2, c2, 2.0f * half);
    for (k = 0; k < 4; k++) { s_col2[k][0] = 1.0f; s_col2[k][1] = 1.0f; s_col2[k][2] = 0.05f; s_col2[k][3] = 1.0f; }
    draw_quad(s_wcons2, &s_prog2, 0x9002u, s_pos2, s_col2);              /* world, c96..99 stale with the weapon's */
    quad_at(s_npos, vrm_v3(0.08f * UPM, 0, 0), 0.006f * UPM);
    for (k = 0; k < 4; k++) { s_ncol[k][0] = s_ncol[k][1] = s_ncol[k][2] = s_ncol[k][3] = 1.0f; }
    draw_quad(s_ncons, &s_prog, 0x9000u, s_npos, s_ncol);                 /* the near miss: world, white */
    kgpu_flip(&s_tg.t, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 0, 0);
}

static int colour_centroid(const uint8_t *img, unsigned w, unsigned h, int which, double *cx, double *cy, double *green)
{
    double sx = 0, sy = 0, sg = 0;
    unsigned x, y;
    int n = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const uint8_t *p = img + ((size_t)y * w + x) * 4;
            const int hit = which == 0 ? (p[2] > 200 && p[0] < 100)                      /* the weapon (blue) */
                                       : (p[0] > 200 && p[1] > 200 && p[2] < 100);       /* yellow world */
            if (hit) { sx += x + 0.5; sy += y + 0.5; sg += p[1]; n++; }
        }
    if (n) { *cx = sx / n; *cy = sy / n; *green = sg / n; }
    return n;
}

/* The weapon where `at` (stage) puts it in each eye, the yellow world quad where its world point does. Returns the
 * weapon's larger distance from where the game's own hand would have put it. */
static double check_gun_pixels(const char *what, const VrRenderPose *rp, VrVec3 at, VrVec3 game_at, double *tint_green)
{
    const unsigned W = shell_eye_width(), H = shell_eye_height();
    uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4);
    VrPose origin;
    double far = 0.0, gsum = 0.0;
    int e;
    vr_get_recentre(&origin);
    for (e = 0; e < 2; e++) {
        double cx = 0, cy = 0, gr = 0, wx, wy, gx, gy, yx = 0, yy = 0, yg, px2, py2;
        int n, ny;
        read_eye(e, img);
        n = colour_centroid(img, W, H, 0, &cx, &cy, &gr);
        project(rp->eye_pose[e], rp->eye_fov[e], at, W, H, &wx, &wy);
        project(rp->eye_pose[e], rp->eye_fov[e], game_at, W, H, &gx, &gy);
        CHECK(n > 20, "%s: eye %d weapon drawn (%d px)", what, e, n);
        CHECK(fabs(cx - wx) < 1.0 && fabs(cy - wy) < 1.0, "%s: eye %d weapon at %.2f,%.2f, the hand puts it at %.2f,%.2f",
              what, e, cx, cy, wx, wy);
        if (hypot(gx - wx, gy - wy) > far) far = hypot(gx - wx, gy - wy);
        gsum += gr;
        ny = colour_centroid(img, W, H, 1, &yx, &yy, &yg);
        project(rp->eye_pose[e], rp->eye_fov[e], vrm_pose_apply(origin, k_point2_rel), W, H, &px2, &py2);
        CHECK(ny > 30 && fabs(yx - px2) < 1.0 && fabs(yy - py2) < 1.0,
              "%s: eye %d world quad of a program not reading the weapon's registers stays put (%.2f,%.2f vs %.2f,%.2f, %d px)",
              what, e, yx, yy, px2, py2, ny);
        printf("%s: eye %d weapon %.2f,%.2f  hand puts it at %.2f,%.2f  game hand at %.2f,%.2f\n", what, e, cx, cy, wx, wy, gx, gy);
    }
    if (tint_green) *tint_green = gsum / 2.0;
    free(img);
    return far;
}

/* ---- the concurrent phase ---- */

typedef struct { int used; VrRenderPose rp; } Released;
static Released s_rel[FX_MAX_SWAPCHAINS + 1][FX_IMAGES];
static volatile int s_xr_stop;
static int s_pair_checked, s_pair_bad;

static void on_release(XrSwapchain h, uint32_t index)
{
    const uintptr_t i = (uintptr_t)h;
    if (i > FX_MAX_SWAPCHAINS || index >= FX_IMAGES) return;
    s_rel[i][index].used = 1;
    s_rel[i][index].rp = *vr_render_pose();      /* under the queue lock, just before the poses are committed */
}

static void on_end_frame(const FxEndFrame *f)
{
    int v;
    if (!f->has_proj) return;
    for (v = 0; v < 2; v++) {
        const uintptr_t i = (uintptr_t)f->views[v].subImage.swapchain;
        const Released *r;
        if (i > FX_MAX_SWAPCHAINS) continue;
        r = &s_rel[i][f->proj_image[v]];
        if (!r->used) continue;
        s_pair_checked++;
        if (!pose_equal(f->views[v].pose, r->rp.eye_pose[v])) s_pair_bad++;
    }
}

static void *xr_thread(void *arg)
{
    (void)arg;
    while (!s_xr_stop) { shell_xr_frame(); usleep(1500); }
    return NULL;
}

/* ---- the run ---- */


/* ---- the settings panel (port/notes/settings-panel.md) ---------------------------------------------------------- */

static struct { int lclick, rclick, b, menu, a; float rtrig; int hand_ok; VrPose hand; } s_pin;

static int pin_bool(const char *a, XrPath sub, XrBool32 *st)
{
    *st = XR_FALSE;
    if (!strcmp(a, "stick_click")) *st = (sub == 16 ? s_pin.lclick : s_pin.rclick) ? XR_TRUE : XR_FALSE;   /* 16 left, 17 right */
    else if (!strcmp(a, "button_b")) *st = s_pin.b ? XR_TRUE : XR_FALSE;
    else if (!strcmp(a, "menu")) *st = s_pin.menu ? XR_TRUE : XR_FALSE;
    else if (!strcmp(a, "button_a")) *st = s_pin.a ? XR_TRUE : XR_FALSE;
    return 1;
}
static int pin_float(const char *a, XrPath sub, float *st)
{
    *st = !strcmp(a, "trigger") && sub == 17 ? s_pin.rtrig : 0.0f;
    return 1;
}
static int pin_vec2(const char *a, XrPath sub, XrVector2f *st) { (void)a; (void)sub; st->x = st->y = 0.0f; return 1; }
static int pin_hand(int hand, int grip, XrTime t, XrPosef *pose)
{
    (void)grip; (void)t;
    if (hand != 1 || !s_pin.hand_ok) return 0;
    *pose = to_xr(s_pin.hand);
    return 1;
}

static void pframe(void) { fx_now += fx_period; shell_xr_frame(); }

static VrPose xr_to_vr(XrPosef p)
{
    VrPose r;
    r.orientation = vrm_q(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    r.position = vrm_v3(p.position.x, p.position.y, p.position.z);
    return r;
}

/* the right hand pointing at panel pixel (px, py) from 0.6 m in front of the panel, a little right of and below it */
static void aim_at(float px, float py)
{
    ShellPanel sp;
    VrPose q;
    VrVec3 r, up, n, target, hand;
    shell_panel(&sp);
    q = xr_to_vr(sp.quad_pose);
    r = vrm_q_rotate(q.orientation, vrm_v3(1, 0, 0));
    up = vrm_q_rotate(q.orientation, vrm_v3(0, 1, 0));
    n = vrm_q_rotate(q.orientation, vrm_v3(0, 0, 1));
    target = vrm_add(q.position, vrm_add(vrm_scale(r, (px / 1024.0f - 0.5f) * sp.width_m),
                                         vrm_scale(up, (0.5f - py / 768.0f) * sp.height_m)));
    hand = vrm_add(target, vrm_add(vrm_scale(n, 0.6f), vrm_add(vrm_scale(r, 0.08f), vrm_scale(up, -0.15f))));
    s_pin.hand.position = hand;
    s_pin.hand.orientation = vrg_look_rotation(vrm_sub(target, hand), vrm_v3(0, 1, 0));
    s_pin.hand_ok = 1;
}

/* point at a control drawn in the last frame, then trigger down and up */
static int click(const char *name)
{
    float rc[4];
    ShellPanel sp;
    if (!vrp_ui_test_rect(name, rc)) { CHECK(0, "panel: control %s drawn", name); return 0; }
    aim_at(0.5f * (rc[0] + rc[2]), 0.5f * (rc[1] + rc[3]));
    pframe();
    shell_panel(&sp);
    CHECK(sp.mouse_valid && fabsf(sp.mouse_x - 0.5f * (rc[0] + rc[2])) < 1.5f && fabsf(sp.mouse_y - 0.5f * (rc[1] + rc[3])) < 1.5f,
          "panel: the laser hits %s at %.1f %.1f (%.1f %.1f)", name, 0.5f * (rc[0] + rc[2]), 0.5f * (rc[1] + rc[3]),
          sp.mouse_x, sp.mouse_y);
    s_pin.rtrig = 1.0f;
    pframe();
    s_pin.rtrig = 0.0f;
    pframe();
    pframe();                                   /* the change is drawn */
    return 1;
}

static uint32_t crc_tab[256];
static uint32_t crc32_of(uint32_t c, const uint8_t *p, size_t n)
{
    size_t i;
    if (!crc_tab[1]) {
        uint32_t k, j, v;
        for (k = 0; k < 256; k++) { v = k; for (j = 0; j < 8; j++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tab[k] = v; }
    }
    c = ~c;
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(b, 1, 4, f); }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint32_t c = crc32_of(0, (const uint8_t *)type, 4);
    put32(f, n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    c = crc32_of(c, d, n);
    put32(f, c);
}
/* premultiplied RGBA composited over the dark void the panel floats in, as an RGB PNG (zlib stored blocks) */
static int png_write_panel(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    const size_t raw_n = (size_t)h * (w * 3 + 1);
    uint8_t *raw = (uint8_t *)malloc(raw_n), *z, hdr[13];
    size_t zn = 0, i, off;
    uint32_t a1 = 1, a2 = 0, x, y;
    FILE *f;
    for (y = 0; y < h; y++) {
        raw[(size_t)y * (w * 3 + 1)] = 0;
        for (x = 0; x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            static const int bg[3] = { 18, 20, 24 };
            int c;
            for (c = 0; c < 3; c++) {
                int v = p[c] + bg[c] * (255 - p[3]) / 255;
                raw[(size_t)y * (w * 3 + 1) + 1 + x * 3 + c] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
    z = (uint8_t *)malloc(raw_n + raw_n / 65535 * 5 + 64);
    z[zn++] = 0x78; z[zn++] = 0x01;
    for (off = 0; off < raw_n;) {
        const size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
        z[zn++] = off + n == raw_n ? 1 : 0;
        z[zn++] = (uint8_t)n; z[zn++] = (uint8_t)(n >> 8); z[zn++] = (uint8_t)~n; z[zn++] = (uint8_t)(~n >> 8);
        memcpy(z + zn, raw + off, n);
        zn += n;
        off += n;
    }
    for (i = 0; i < raw_n; i++) { a1 = (a1 + raw[i]) % 65521; a2 = (a2 + a1) % 65521; }
    z[zn++] = (uint8_t)(a2 >> 8); z[zn++] = (uint8_t)a2; z[zn++] = (uint8_t)(a1 >> 8); z[zn++] = (uint8_t)a1;
    if (!(f = fopen(path, "wb"))) { free(raw); free(z); return 0; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    hdr[0] = (uint8_t)(w >> 24); hdr[1] = (uint8_t)(w >> 16); hdr[2] = (uint8_t)(w >> 8); hdr[3] = (uint8_t)w;
    hdr[4] = (uint8_t)(h >> 24); hdr[5] = (uint8_t)(h >> 16); hdr[6] = (uint8_t)(h >> 8); hdr[7] = (uint8_t)h;
    hdr[8] = 8; hdr[9] = 2; hdr[10] = hdr[11] = hdr[12] = 0;
    png_chunk(f, "IHDR", hdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}

static void capture_panel(const char *what, const char *file)
{
    ShellPanel sp;
    uint8_t *img = (uint8_t *)malloc(1024 * 768 * 4);
    char path[600], cwd[400];
    unsigned x, y, red = 0, opaque = 0;
    shell_panel(&sp);
    read_quad_image(sp.panel, img);
    for (y = 0; y < 768; y++)
        for (x = 0; x < 1024; x++) {
            const uint8_t *p = img + ((size_t)y * 1024 + x) * 4;
            if (p[3] > 240) opaque++;
            if (p[0] > 150 && p[1] < 60 && p[2] < 60) red++;
        }
    CHECK(img[3] == 0 && img[(1023) * 4 + 3] == 0, "%s: the panel's rounded corners are transparent (alpha %u %u)", what, img[3], img[1023 * 4 + 3]);
    CHECK(opaque > 1024u * 768u * 9u / 10u, "%s: the panel is opaque inside (%u px)", what, opaque);
    CHECK(red > 2000, "%s: the red accents are drawn (%u px)", what, red);
    {
        const uint8_t *c = img + ((size_t)384 * 1024 + 512) * 4;
        CHECK(c[3] > 240, "%s: premultiplied colour never exceeds its alpha (centre %u %u %u / %u)", what, c[0], c[1], c[2], c[3]);
    }
    if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
    snprintf(path, sizeof(path), "%s/%s", cwd, file);
    CHECK(png_write_panel(path, img, 1024, 768), "%s: %s written", what, path);
    printf("panel: %s -> %s\n", what, path);
    free(img);
}

static const XrCompositionLayerQuad *quad_of(XrSwapchain sc, int *index)
{
    uint32_t q;
    for (q = 0; q < fx_last_end.nquads; q++)
        if (fx_last_end.quads[q].subImage.swapchain == sc) { if (index) *index = fx_last_end.quad_layer_index[q]; return &fx_last_end.quads[q]; }
    return NULL;
}

static void test_panel(void)
{
    static const char k_ini_text[] =
        "# my settings\n"
        "turn = snap              ; snap | smooth | off\n"
        "resolution_scale = 0.8\n"
        "some_future_key = 7\n"
        "env RECOMP_BLACK_HZ=60\n";
    char dir[] = "/tmp/vrglue-panel-XXXXXX", path[600], buf[4096];
    ShellPanel sp;
    VrPadState pad;
    const XrCompositionLayerQuad *q;
    int idx = -1, i;
    uint64_t acq;
    FILE *f;
    size_t n;
    XrPosef head;
    XrPosef eye[2];
    XrFovf fov[2];

    CHECK(mkdtemp(dir) != NULL, "panel: temp dir");
    /* the floating panel is the fallback front end (blackvr.ini settings_menu = panel; native: the game's Options) */
    CHECK(shell_config_line("settings_menu = panel"), "panel: settings_menu = panel parses");
    shell_set_game_root(dir);
    snprintf(path, sizeof(path), "%s/blackvr.ini", dir);
    if ((f = fopen(path, "w"))) { fputs(k_ini_text, f); fclose(f); }
    memset(&s_pin, 0, sizeof(s_pin));
    fx_input_bool = pin_bool;
    fx_input_float = pin_float;
    fx_input_vec2 = pin_vec2;
    fx_hand_pose = pin_hand;

    /* gameplay: the left stick click is Crouch, never the panel */
    shell_set_present_mode(VR_PRESENT_STEREO);
    vr_publish_game_camera(vr_game_camera());     /* a fresh camera: the controls are in gameplay */
    s_pin.lclick = 1; pframe(); pframe();
    s_pin.lclick = 0; pframe();
    shell_panel(&sp);
    CHECK(!sp.open && !sp.inited, "panel: not opened in gameplay");

    /* paused (the menus): a left stick click opens it */
    shell_set_present_mode(VR_PRESENT_FLAT_MENU);
    s_pin.lclick = 1; pframe(); pframe(); pframe();
    s_pin.lclick = 0;
    pframe();
    shell_panel(&sp);
    CHECK(sp.open && sp.inited && sp.released, "panel: opened in the menu (open %d, ready %d, drawn %d)", sp.open, sp.inited, sp.released);
    fx_trajectory(fx_now, &head, eye, fov);
    {
        const double dx = sp.quad_pose.position.x - head.position.x, dz = sp.quad_pose.position.z - head.position.z;
        CHECK(fabs(sqrt(dx * dx + dz * dz) - 1.2) < 0.05 && fabs(sp.quad_pose.position.y - (head.position.y - 0.10)) < 0.05,
              "panel: placed 1.2 m ahead, just below the eyes (%.3f m, dy %.3f)", sqrt(dx * dx + dz * dz),
              sp.quad_pose.position.y - head.position.y);
    }
    pframe();
    q = quad_of(sp.panel, &idx);
    CHECK(q != NULL, "panel: its quad layer is submitted");
    if (q) {
        CHECK(q->space == shell_world_space(), "panel: world-locked (stage space)");
        CHECK(fabsf(q->size.width - 0.8f) < 1e-5f && fabsf(q->size.height - 0.6f) < 1e-5f, "panel: 0.8 x 0.6 m (%.3f x %.3f)",
              q->size.width, q->size.height);
        CHECK(!memcmp(&q->pose, &sp.quad_pose, sizeof(q->pose)), "panel: submitted at the pose it opened with");
        CHECK(q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, "panel: alpha-blended (premultiplied)");
        CHECK(q->subImage.imageRect.extent.width == 1024 && q->subImage.imageRect.extent.height == 768, "panel: 1024x768 image");
        CHECK(idx >= 1 && (uint32_t)idx >= fx_last_end.nlayers - 2, "panel: over the game's layers (layer %d of %u)", idx, fx_last_end.nlayers);
    }
    /* the controllers are the panel's: the game sees a neutral pad even with buttons held */
    s_pin.a = 1; s_pin.rtrig = 1.0f;
    aim_at(512, 384);
    pframe();
    CHECK(vr_get_pad(&pad) && pad.buttons == 0 && pad.right_trigger == 0 && pad.left_trigger == 0, "panel: the game gets a neutral pad (%04X)", pad.buttons);
    {
        int lidx = -1;
        const XrCompositionLayerQuad *lq = quad_of(sp.laser, &lidx);
        shell_panel(&sp);
        CHECK(lq && lidx == (int)fx_last_end.nlayers - 1 && fabsf(lq->size.height - sp.laser_len) < 1e-4f && sp.laser_len > 0.5f,
              "panel: the laser beam is the last layer, as long as the hit (%.3f m)", sp.laser_len);
        CHECK(sp.mouse_valid && fabsf(sp.mouse_x - 512.0f) < 1.0f && fabsf(sp.mouse_y - 384.0f) < 1.0f, "panel: the laser hits the centre (%.1f %.1f)",
              sp.mouse_x, sp.mouse_y);
    }
    s_pin.a = 0; s_pin.rtrig = 0.0f;
    pframe();
    pframe();
    capture_panel("Graphics tab", "panel_graphics.png");

    /* a laser click changes a live setting: Comfort -> Turning: Smooth, Snap angle + */
    CHECK(shell_cfg_get(VRS_TURN) == 0.0f, "panel: turning starts as snap");
    click("tab:Comfort");
    click("seg:turn:1");
    CHECK(shell_cfg_get(VRS_TURN) == 1.0f, "panel: Smooth applied live (%g)", shell_cfg_get(VRS_TURN));
    click("plus:snap_degrees");
    CHECK(shell_cfg_get(VRS_SNAP_DEG) == 35.0f, "panel: snap angle + -> 35 (%g)", shell_cfg_get(VRS_SNAP_DEG));
    capture_panel("Comfort tab", "panel_comfort.png");
    /* a restart setting does not change the running config */
    click("tab:Graphics");
    click("seg:game_hz:0");
    CHECK(shell_cfg_get(VRS_GAME_HZ) == 1.0f, "panel: game rate 30 waits for a restart (live %g)", shell_cfg_get(VRS_GAME_HZ));
    /* foveation: SwiftShader has no VK_EXT_fragment_density_map, so the eye swapchain is not foveated and the setting
     * is N/A here (its buttons disabled); the live path (xrUpdateSwapchainFB) is the unit-tested vrs_apply_live */
    click("seg:foveation:3");
    {
        FxSwapchain *sc = fx_swapchain(shell_eye_swapchain(0));
        CHECK(shell_cfg_get(VRS_FOVEATION) == 2.0f && sc && sc->foveation_level == -1,
              "panel: foveation N/A without FDM: unchanged (%g, level %d)", shell_cfg_get(VRS_FOVEATION), sc ? sc->foveation_level : -9);
    }
    /* Performance: XR_EXT_performance_settings is not offered by the fake runtime either: N/A, nothing written */
    /* Save: blackvr.ini edited in place */
    click("btn:Save");
    n = 0;
    if ((f = fopen(path, "rb"))) { n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    buf[n] = 0;
    CHECK(strstr(buf, "# my settings\n") == buf, "save: the comment kept");
    CHECK(strstr(buf, "\nturn = smooth            ; snap | smooth | off\n") != NULL, "save: turn changed in place, comment kept");
    CHECK(strstr(buf, "\nsome_future_key = 7\n") && strstr(buf, "\nenv RECOMP_BLACK_HZ=60\n") && strstr(buf, "\nresolution_scale = 0.8\n"),
          "save: unknown keys, env lines and untouched settings kept");
    CHECK(strstr(buf, "\nsnap_degrees = 35\n") && strstr(buf, "\ngame_hz = 30\n"), "save: new values appended");
    CHECK(!strstr(buf, "world_scale") && !strstr(buf, "foveation") && !strstr(buf, "BLACKVR_PERF"),
          "save: settings not changed in the panel are not written");
    printf("panel: saved blackvr.ini:\n%s", buf);
    shell_panel(&sp);
    CHECK(strstr(sp.status, "Saved") != NULL, "save: status '%s'", sp.status);
    capture_panel("Graphics tab after Save", "panel_saved.png");
    click("tab:Gestures");
    capture_panel("Gestures tab", "panel_gestures.png");
    click("tab:Info");
    pframe();
    capture_panel("Info tab", "panel_info.png");

    /* B closes it; that B never reaches the game; closed, it costs nothing */
    s_pin.b = 1;
    pframe();
    shell_panel(&sp);
    CHECK(!sp.open, "panel: B closes it");
    CHECK(vr_get_pad(&pad) && !(pad.buttons & VR_PAD_B), "panel: the B that closed it is not the game's");
    pframe();
    CHECK(vr_get_pad(&pad) && !(pad.buttons & VR_PAD_B), "panel: ... while it is held");
    s_pin.b = 0;
    pframe();
    CHECK(quad_of(sp.panel, NULL) == NULL && quad_of(sp.laser, NULL) == NULL, "panel: no layers when closed");
    acq = fx_swapchain(sp.panel)->acquires;
    for (i = 0; i < 5; i++) pframe();
    CHECK(fx_swapchain(sp.panel)->acquires == acq, "panel: nothing drawn when closed");
    s_pin.b = 1; pframe();
    CHECK(vr_get_pad(&pad) && (pad.buttons & VR_PAD_B), "panel: a new B press is the game's again");
    s_pin.b = 0; pframe();

    /* reopen, change, Cancel: the change is undone; Menu closes */
    s_pin.lclick = 1; pframe();
    s_pin.lclick = 0; pframe();
    shell_panel(&sp);
    CHECK(sp.open, "panel: reopened");
    pframe();
    click("tab:Comfort");
    click("seg:turn:2");
    CHECK(shell_cfg_get(VRS_TURN) == 2.0f, "panel: turning off (live)");
    click("btn:Cancel");
    CHECK(shell_cfg_get(VRS_TURN) == 1.0f, "panel: Cancel restores smooth (%g)", shell_cfg_get(VRS_TURN));
    s_pin.menu = 1; pframe();
    shell_panel(&sp);
    CHECK(!sp.open, "panel: Menu closes it");
    CHECK(vr_get_pad(&pad) && !(pad.buttons & VR_PAD_START), "panel: that Menu is not START for the game");
    s_pin.menu = 0; pframe();

    fx_input_bool = NULL;
    fx_input_float = NULL;
    fx_input_vec2 = NULL;
    fx_hand_pose = NULL;
    remove(path);
    rmdir(dir);
}

static int run(int multiview)
{
    const uint32_t EW = 384, EH = 336;
    char cache[64];
    GameFrame g1, g2, g3, g4, g5;
    VrRenderPose rp;
    uint64_t eye_acq;
    uint8_t *img;
    int r;
    snprintf(cache, sizeof(cache), "vr-glue-cache-%s", multiview ? "mv" : "two");
    fx_trajectory = trajectory;
    fx_now = T_BASE;
    r = shell_setup(multiview, UPM, EW, EH, cache);
    if (r < 0 && !fx_vk.device) { printf("no Vulkan device: skipped\n"); return 77; }
    CHECK(r == 1, "the renderer glue attached (%d)", r);
    if (r != 1) return 1;

    /* device and swapchains as the backend needs them */
    {
        VkPhysicalDeviceFeatures have;
        vkGetPhysicalDeviceFeatures(fx_vk.phys, &have);
        CHECK(fx_vk.instance_api >= VK_API_VERSION_1_1, "VkInstance asks for Vulkan 1.1 (%#x)", fx_vk.instance_api);
        CHECK(fx_vk.dev_multiview, "device created with multiview");
        CHECK(fx_vk.dev_robust == (have.robustBufferAccess != 0), "robustBufferAccess enabled where present");
        CHECK(fx_vk.dev_depth_clamp == (have.depthClamp != 0), "depthClamp enabled where present");
        CHECK(fx_vk.dev_bc == (have.textureCompressionBC != 0), "textureCompressionBC enabled exactly where present");
        printf("device: robustBufferAccess %d depthClamp %d BC %d anisotropy %d (device has %d %d %d %d)\n", fx_vk.dev_robust,
               fx_vk.dev_depth_clamp, fx_vk.dev_bc, fx_vk.dev_aniso, have.robustBufferAccess, have.depthClamp,
               have.textureCompressionBC, have.samplerAnisotropy);
    }
    {
        XrSwapchain hs[4];
        int n = 0, i;
        hs[n++] = shell_eye_swapchain(0);
        if (!multiview) hs[n++] = shell_eye_swapchain(1);
        hs[n++] = shell_flat_swapchain();
        hs[n++] = shell_hud_swapchain();
        CHECK(shell_multiview() == multiview, "eye swapchain layout as configured");
        CHECK(fx_swapchain(hs[0])->ci.arraySize == (uint32_t)(multiview ? 2 : 1), "eye swapchain array size");
        for (i = 0; i < n; i++) {
            const FxSwapchain *sc = fx_swapchain(hs[i]);
            CHECK(sc && (sc->ci.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
                  (sc->ci.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT), "swapchain %d: COLOR_ATTACHMENT + MUTABLE_FORMAT", i);
            CHECK(sc && sc->ci.format == VK_FORMAT_R8G8B8A8_SRGB && sc->nview_formats == 2 &&
                  sc->view_formats[0] == VK_FORMAT_R8G8B8A8_SRGB && sc->view_formats[1] == VK_FORMAT_R8G8B8A8_UNORM,
                  "swapchain %d: sRGB with the sRGB+UNORM format list", i);
        }
        CHECK(fx_swapchain(shell_hud_swapchain())->ci.width == 1280 && vr_hud_layout()->width_m == 2.4f &&
              vr_hud_layout()->distance_m == 2.0f, "HUD swapchain and layout");
        /* fixed foveated rendering (blackvr.ini foveation, default medium): the runtime offers XR_FB_foveation(_vulkan),
         * so the eye swapchain(s) -- and only they -- are created with the fragment-density-map flag and get a level
         * profile through xrUpdateSwapchainFB; the profile is destroyed afterwards */
        /* ... and the flag needs VK_EXT_fragment_density_map enabled on the device (the fake refuses it otherwise, as
         * xr.xml requires): the shell enables the extension where the GPU has it (the Quest's Adreno) and leaves
         * foveation off where it does not (SwiftShader) */
        {
            uint32_t ne = 0, k;
            int gpu_fdm = 0;
            VkExtensionProperties *de;
            vkEnumerateDeviceExtensionProperties(fx_vk.phys, NULL, &ne, NULL);
            de = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof(*de));
            vkEnumerateDeviceExtensionProperties(fx_vk.phys, NULL, &ne, de);
            for (k = 0; k < ne; k++) if (!strcmp(de[k].extensionName, "VK_EXT_fragment_density_map")) gpu_fdm = 1;
            free(de);
            CHECK(fx_vk.dev_fdm == gpu_fdm, "VK_EXT_fragment_density_map enabled exactly where the GPU has it (%d/%d)",
                  fx_vk.dev_fdm, gpu_fdm);
            for (i = 0; i < (multiview ? 1 : 2); i++) {
                const FxSwapchain *sc = fx_swapchain(shell_eye_swapchain(i));
                CHECK(sc && sc->foveation_flags == (gpu_fdm ? XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB : 0),
                      "eye swapchain %d created %s XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB", i,
                      gpu_fdm ? "with" : "without");
                CHECK(sc && sc->foveation_level == (gpu_fdm ? XR_FOVEATION_LEVEL_MEDIUM_FB : -1),
                      "eye swapchain %d foveation level %s (%d)", i, gpu_fdm ? "medium" : "none", sc ? sc->foveation_level : -9);
            }
            CHECK(!fx_swapchain(shell_flat_swapchain())->foveation_flags && !fx_swapchain(shell_hud_swapchain())->foveation_flags &&
                  fx_swapchain(shell_hud_swapchain())->foveation_level == -1, "flat and HUD quads not foveated");
            CHECK(fx_foveation_updates == (gpu_fdm ? (multiview ? 1 : 2) : 0) &&
                  fx_foveation_profiles == fx_foveation_profiles_destroyed,
                  "one profile update per eye swapchain where foveated (%d), every profile destroyed (%d/%d)",
                  fx_foveation_updates, fx_foveation_profiles_destroyed, fx_foveation_profiles);
        }
    }
    CHECK(shell_session_running(), "session running and focused");

    /* the XR thread before any game frame: the test pattern, with the poses it was drawn for */
    fx_check_locks = 1;
    shell_xr_frame();
    CHECK(!shell_renderer_started() && fx_last_end.has_proj && fx_last_end.nlayers == 1, "test pattern until the renderer's first frame");
    {
        XrPosef h, ey[2];
        XrFovf fv[2];
        trajectory(fx_last_end.display_time, &h, ey, fv);
        CHECK(!memcmp(&fx_last_end.views[0].pose, &ey[0], sizeof(XrPosef)) && !memcmp(&fx_last_end.views[1].pose, &ey[1], sizeof(XrPosef)),
              "test pattern submitted with its own poses");
    }

    /* kelvin_start -> the GPU thread -> kgpu_init (with what vr_renderer_init gave kvk_set_*) */
    CHECK(kgpu_init(), "kgpu_init on the shell's device");
    kgpu_note_kick();
    gpu_setup();
    sprite_setup();
    off_setup();

    /* ---- frame 1: the game runs ahead (camera 2 published before frame 1 is drawn); the head keeps moving ---- */
    fx_now = T_BASE + 20 * MS;
    game_frame(&g1, 1, VR_PRESENT_STEREO);
    vr_mark_world_pass(0x80001000u, 1);
    vr_mark_world_pass(0x80001400u, 0);
    fx_now += 16 * MS;
    game_frame(&g2, 2, VR_PRESENT_STEREO);
    vr_mark_world_pass(0x80002000u, 1);
    fx_now += 40 * MS;                                 /* the GPU thread reaches frame 1 now */
    gpu_world_frame(&g1, 0x80001000u, 1);
    rp = *vr_render_pose();
    CHECK(shell_renderer_started(), "the renderer took over the display");
    CHECK(rp.valid && rp.game_frame == 1, "frame 1 drawn with camera 1 (render pose game_frame %u)", rp.game_frame);
    CHECK(!memcmp(&rp.game_head, &g1.ext.head_pose, sizeof(VrPose)), "render pose's game head == camera 1's head pose");
    CHECK(rp.present_mode == VR_PRESENT_STEREO, "frame 1 presented as STEREO");
    check_one_correction(&g1, &rp);
    fx_now += 5 * MS;
    shell_xr_frame();
    check_layers_pose_identity("frame 1", &rp);
    CHECK(fx_last_end.nlayers == 2 && fx_last_end.nquads == 1, "frame 1: projection + HUD quad (%u layers)", fx_last_end.nlayers);
    if (fx_last_end.nquads == 1) {
        const XrCompositionLayerQuad *q = &fx_last_end.quads[0];
        CHECK(q->space == shell_view_space() && q->pose.position.z == -2.0f && q->size.width == 2.4f &&
              fabsf(q->size.height - 1.8f) < 1e-6f, "HUD quad: head-locked, 2.4 x 1.8 m at 2 m");
        CHECK((q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) &&
              !(q->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT), "HUD quad: premultiplied alpha blending");
        CHECK(q->subImage.swapchain == shell_hud_swapchain(), "HUD quad shows the HUD swapchain");
    }
    {
        const double contrast = check_stereo_pixels("frame 1", &rp, &g1, 0);
        CHECK(contrast > 4.0, "the head moved between the game's latch and the render latch (%.1f px): the test can tell", contrast);
        printf("frame 1: late-latch correction %.1f px\n", contrast);
    }
    img = (uint8_t *)malloc(1280 * 960 * 4);
    read_quad_image(shell_hud_swapchain(), img);
    {
        const uint8_t *in = img + ((size_t)48 * 1280 + 64) * 4, *out = img + ((size_t)480 * 1280 + 640) * 4;
        CHECK(in[1] > 200 && in[0] < 40 && in[3] > 200, "HUD layer: the HUD square, opaque (%u %u %u %u)", in[0], in[1], in[2], in[3]);
        CHECK(out[3] == 0 && out[0] == 0 && out[1] == 0, "HUD layer: transparent elsewhere (%u %u %u %u)", out[0], out[1], out[2], out[3]);
    }

    /* ---- frame 2 (camera 2's mark was queued all along), no HUD ---- */
    fx_now += 12 * MS;
    {
        KvkStats st0, st1;
        uint32_t cap0 = vr_glue_captures_written();
        int waited;
        kvk_get_stats(&st0);
        vr_capture_request();                          /* and a both-eye capture of it */
        s_with_sprites = 1;                            /* and the title's screen-space effects */
        gpu_world_frame(&g2, 0x80002000u, 0);
        s_with_sprites = 0;
        kvk_get_stats(&st1);
        CHECK(st1.sprite_draws - st0.sprite_draws == NSPRITE + 2 && st1.fullscreen_draws - st0.fullscreen_draws == 1 &&
              st1.offscreen_stereo_draws - st0.offscreen_stereo_draws == 2 && st1.hud_draws == st0.hud_draws,
              "frame 2: %d screen-space effects corrected (%llu; the sprites, the light shaft, the glare target's), the "
              "full-screen pass left (%llu), 2 glare-target draws corrected (%llu), none as HUD", NSPRITE + 2,
              (unsigned long long)(st1.sprite_draws - st0.sprite_draws),
              (unsigned long long)(st1.fullscreen_draws - st0.fullscreen_draws),
              (unsigned long long)(st1.offscreen_stereo_draws - st0.offscreen_stereo_draws));
        for (waited = 0; waited < 500 && vr_glue_captures_written() == cap0; waited++) usleep(10000);
        CHECK(vr_glue_captures_written() == cap0 + 1, "frame 2: one both-eye capture written");
        s_cap_frame = st1.frames;
    }
    rp = *vr_render_pose();
    CHECK(rp.game_frame == 2, "frame 2 drawn with camera 2 (%u)", rp.game_frame);
    fx_now += 5 * MS;
    shell_xr_frame();
    check_layers_pose_identity("frame 2", &rp);
    CHECK(fx_last_end.nlayers == 1, "frame 2: no HUD drawn, no HUD layer (%u layers)", fx_last_end.nlayers);
    check_stereo_pixels("frame 2", &rp, &g2, 0);
    check_sprite_pixels("frame 2", &rp, &g2);
    check_capture("frame 2", s_cap_frame);
    check_offscreen("frame 2", &rp);
    check_shaft("frame 2", &rp);

    /* ---- frame 3: FLAT_MENU (marked): the flat quad layer only ---- */
    eye_acq = fx_swapchain(shell_eye_swapchain(0))->acquires;
    fx_now += 30 * MS;
    game_frame(&g3, 3, VR_PRESENT_FLAT_MENU);
    vr_mark_world_pass(0x80003000u, 1);
    gpu_flat_frame(0x80003000u, 0.0f, 0.0f, 1.0f);
    fx_now += 5 * MS;
    shell_xr_frame();
    CHECK(fx_swapchain(shell_eye_swapchain(0))->acquires == eye_acq, "flat frame: no eye image acquired");
    CHECK(shell_submit_mode() == VR_PRESENT_FLAT_MENU, "flat frame: the shell presents FLAT_MENU (%d)", shell_submit_mode());
    CHECK(fx_last_end.nlayers == 1 && !fx_last_end.has_proj && fx_last_end.nquads == 1, "menu: one quad layer, no projection layer");
    if (fx_last_end.nquads == 1)
        CHECK(fx_last_end.quads[0].space == shell_world_space() && fx_last_end.quads[0].size.width == 2.0f &&
              fx_last_end.quads[0].subImage.swapchain == shell_flat_swapchain(), "menu quad: world-locked, 2 m wide, the flat image");
    read_quad_image(shell_flat_swapchain(), img);
    {
        const uint8_t *c = img + ((size_t)480 * 1280 + 640) * 4, *k = img + ((size_t)48 * 1280 + 64) * 4;
        CHECK(c[2] > 200 && c[0] < 30 && c[1] < 30 && c[3] == 255, "menu image: the picture (%u %u %u %u)", c[0], c[1], c[2], c[3]);
        CHECK(k[1] > 200 && k[2] < 30, "menu image: its 2D overlay too (%u %u %u)", k[0], k[1], k[2]);
    }

    /* ---- frame 4: FLAT_SCOPE without a camera mark: the game's current mode decides ---- */
    fx_now += 30 * MS;
    game_frame(&g4, 4, VR_PRESENT_FLAT_SCOPE);
    gpu_flat_frame(0, 1.0f, 0.0f, 1.0f);
    fx_now += 5 * MS;
    shell_xr_frame();
    CHECK(fx_swapchain(shell_eye_swapchain(0))->acquires == eye_acq, "scope frame: no eye image acquired");
    CHECK(fx_last_end.nlayers == 1 && fx_last_end.nquads == 1, "scope: one quad layer");
    if (fx_last_end.nquads == 1)
        CHECK(fx_last_end.quads[0].space == shell_view_space() && fx_last_end.quads[0].pose.position.z == -0.9f,
              "scope quad: head-locked at 0.9 m");
    read_quad_image(shell_flat_swapchain(), img);
    {
        const uint8_t *c = img + ((size_t)480 * 1280 + 640) * 4;
        CHECK(c[0] > 200 && c[2] > 200 && c[1] < 30, "scope image: the picture (%u %u %u)", c[0], c[1], c[2]);
    }

    /* ---- frame 5: back to STEREO with the head 0.9 m beyond the recentre point: drawn and submitted clamped ---- */
    s_jump = 0.9f;
    fx_now += 30 * MS;
    game_frame(&g5, 5, VR_PRESENT_STEREO);
    vr_mark_world_pass(0x80005000u, 1);
    fx_now += 30 * MS;
    gpu_world_frame(&g5, 0x80005000u, 1);
    rp = *vr_render_pose();
    fx_now += 5 * MS;
    shell_xr_frame();
    check_layers_pose_identity("frame 5 (clamped)", &rp);
    {
        XrPosef h, ey[2];
        XrFovf fv[2];
        VrPose origin;
        vr_get_recentre(&origin);
        trajectory(rp.display_time, &h, ey, fv);
        CHECK(fabsf(rp.eye_pose[0].position.x - ey[0].position.x) > 0.3f, "frame 5: the submitted eye is the clamped one");
        CHECK(vrm_len(vrm_sub(rp.head.position, origin.position)) < 0.5f + 1e-4f, "frame 5: render head within max_head_offset");
    }
    check_stereo_pixels("frame 5 (clamped)", &rp, &g5, 0);
    s_jump = 0.0f;
    fx_check_locks = 0;
    CHECK(fx_lock_violations == 0, "every queue-using OpenXR call held the VkQueue lock (%d violations)", fx_lock_violations);

    /* ---- gun latch ---- */
    {
        GameFrame g;
        VrPose origin, hand_game, hand_late;
        VrRenderGun rg;
        KvkStats st0, st1;
        uint32_t rec0, drop0, moved0, rec1, drop1, moved1;
        double far, tint_g;
        const int tinting = !multiview;            /* BLACKVR_GUN_LATCH_DEBUG set by main() for the two-swapchain run */
        /* blackvr.ini */
        CHECK(shell_gun_latch() == 1, "gun_latch defaults to on");
        CHECK(shell_config_line("gun_latch = off") && shell_gun_latch() == 0 && shell_config_line("gun_latch = on") &&
              shell_gun_latch() == 1 && !shell_config_line("gun_latch = maybe") && shell_gun_latch() == 1,
              "gun_latch = on | off parses (and nothing else)");
        /* haptic_fire: the game's shot pulse (vr_game_haptic) is scaled by it, per hand, and taken once */
        {
            float st = -1.0f;
            uint32_t ms = 77;
            CHECK(shell_haptic_fire() == 0.7f, "haptic_fire defaults to 0.7");
            CHECK(shell_config_line("haptic_fire = 0.5") && shell_haptic_fire() == 0.5f && !shell_config_line("haptic_fire = 1.5") &&
                  !shell_config_line("haptic_fire = loud") && shell_haptic_fire() == 0.5f, "haptic_fire parses 0..1 (and nothing else)");
            vr_game_haptic(1, 1.0f, 40u);
            vr_game_haptic(1, 0.2f, 10u);                    /* a weaker, shorter one does not lower the pending pulse */
            shell_take_pulse(1, &st, &ms);
            CHECK(st == 0.5f && ms == 40u, "pulse: scaled by haptic_fire (%.2f), strongest/longest kept (%u ms)", st, ms);
            shell_take_pulse(1, &st, &ms);
            CHECK(st == 0.0f && ms == 0u, "pulse: taken once");
            vr_game_haptic(0, 1.0f, 40u);
            shell_take_pulse(1, &st, &ms);
            CHECK(st == 0.0f, "pulse: the other hand is untouched");
            shell_take_pulse(0, &st, &ms);
            CHECK(st == 0.5f, "pulse: left hand got its own");
            CHECK(shell_config_line("haptic_fire = 0"), "haptic_fire = 0 parses");
            vr_game_haptic(1, 1.0f, 40u);
            shell_take_pulse(1, &st, &ms);
            CHECK(st == 0.0f, "haptic_fire = 0: no pulse");
            shell_config_line("haptic_fire = 0.7");
        }
        /* first-person arms: arms and left_hand (port/notes/arms.md) */
        {
            int cut = -1, mode = -1, engaged = -1;
            vr_arms_state(&cut, &mode, &engaged);
            CHECK(cut == 1 && mode == VR_LEFT_HAND_TRACKED && engaged == 0, "arms: defaults hands / tracked / not gripping");
            CHECK(shell_config_line("left_hand = grip") && shell_config_line("arms = full"), "arms / left_hand parse");
            vr_arms_state(&cut, &mode, &engaged);
            CHECK(cut == 0 && mode == VR_LEFT_HAND_GRIP, "arms = full, left_hand = grip");
            CHECK(!shell_config_line("left_hand = sometimes") && !shell_config_line("arms = some"), "bad values refused");
            CHECK(shell_config_line("left_hand = always") && shell_config_line("left_hand = never") &&
                  shell_config_line("left_hand = tracked") && shell_config_line("arms = hands"), "all values parse");
            vr_arms_state(&cut, &mode, &engaged);
            CHECK(cut == 1 && mode == VR_LEFT_HAND_TRACKED, "back to the defaults");
        }
        /* auto-grip: hand_auto and wrist_offset */
        {
            int ha = -1;
            float wo[3];
            vr_arms_grip_config(&ha, wo);
            CHECK(ha == 1 && wo[0] == 0.0f && wo[1] == -0.03f && wo[2] == 0.10f, "hand_auto on, wrist_offset 0 -0.03 0.10");
            CHECK(shell_config_line("wrist_offset = 0.01 -0.05 0.12") && shell_config_line("hand_auto = off"), "parse");
            vr_arms_grip_config(&ha, wo);
            CHECK(ha == 0 && wo[0] == 0.01f && wo[1] == -0.05f && wo[2] == 0.12f, "hand_auto off, wrist_offset set");
            CHECK(!shell_config_line("wrist_offset = 0 0") && !shell_config_line("wrist_offset = 0 0 3"), "bad wrist_offset refused");
            CHECK(shell_config_line("wrist_offset = 0 -0.03 0.10") && shell_config_line("hand_auto = on"), "back to defaults");
        }
        CHECK(vr_gun_latch_config()->enabled == 1 && vr_gun_latch_config()->debug_tint == (tinting ? 0x80FFFF00u : 0u),
              "gun latch config: on, debug tint %08X", vr_gun_latch_config()->debug_tint);
        gun_setup();
        fx_hand_pose = hand_pose;
        vr_get_recentre(&origin);
        hand_game = vrm_pose_mul(origin, vrm_pose_identity());
        hand_game.position = vrm_pose_apply(origin, vrm_v3(0.12f, -0.22f, -0.30f));
        hand_game.orientation = vrm_q_normalize(vrm_q_mul(origin.orientation, vrm_q_axis_angle(vrm_v3(1, 0, 0), -0.2f)));

        /* G1: the hand did not move between the game's latch and the late latch: the weapon is where the game put it */
        s_hand_tracked = 1;
        s_hand_late = hand_game;
        vr_glue_gun_counters(&rec0, &drop0, &moved0);
        kvk_get_stats(&st0);
        fx_now += 30 * MS;
        gun_game_frame(&g, 30, hand_game, 1, 0x80030000u);
        fx_now += 30 * MS;
        gun_gpu_frame(&g, 0x80030000u);
        rp = *vr_render_pose();
        rg = *vr_render_gun();
        vr_glue_gun_counters(&rec1, &drop1, &moved1);
        kvk_get_stats(&st1);
        CHECK(rg.valid && rg.serial == rp.serial && !memcmp(&rg.game_hand, &hand_game, sizeof(VrPose)) &&
              !memcmp(&rg.hand, &hand_game, sizeof(VrPose)), "G1: the weapon hand latched with the eyes, unchanged");
        {
            const VrPose d = vrm_pose_mul(rg.hand, vrm_pose_inv(rg.game_hand));
            CHECK(vrm_len(d.position) < 1e-6f && fabsf(fabsf(d.orientation.w) - 1.0f) < 1e-6f,
                  "G1: the delta is the identity (%.2e m, w %.8f)", vrm_len(d.position), d.orientation.w);
        }
        CHECK(rec1 == rec0 + 1 && drop1 == drop0 && moved1 == moved0, "G1: one weapon MVP recorded, no delta applied (%u %u %u)",
              rec1 - rec0, drop1 - drop0, moved1 - moved0);
        CHECK(st1.viewmodel_draws == st0.viewmodel_draws + 1, "G1: exactly the weapon's draw classified (%llu)",
              (unsigned long long)(st1.viewmodel_draws - st0.viewmodel_draws));
        fx_now += 5 * MS;
        shell_xr_frame();
        check_layers_pose_identity("G1", &rp);
        check_stereo_pixels("G1", &rp, &g, 1);
        far = check_gun_pixels("G1", &rp, vrm_pose_apply(hand_game, k_grip), vrm_pose_apply(hand_game, k_grip), &tint_g);
        if (tinting) CHECK(tint_g > 110.0, "G1: the debug tint is on the weapon (green %.1f)", tint_g);
        else CHECK(tint_g < 40.0, "G1: no tint without the debug env (green %.1f)", tint_g);

        /* G2: the hand moved 6 cm and turned 12 degrees after the game's latch: the weapon follows it, the world does not */
        hand_late.orientation = vrm_q_normalize(vrm_q_mul(vrm_q_axis_angle(vrm_v3(0, 1, 0), 0.21f), hand_game.orientation));
        hand_late.position = vrm_add(hand_game.position, vrm_v3(-0.05f, 0.03f, -0.01f));
        s_hand_late = hand_late;
        vr_glue_gun_counters(&rec0, &drop0, &moved0);
        kvk_get_stats(&st0);
        fx_now += 30 * MS;
        gun_game_frame(&g, 31, hand_game, 1, 0x80031000u);
        fx_now += 30 * MS;
        gun_gpu_frame(&g, 0x80031000u);
        rp = *vr_render_pose();
        rg = *vr_render_gun();
        vr_glue_gun_counters(&rec1, &drop1, &moved1);
        kvk_get_stats(&st1);
        CHECK(rg.valid && !memcmp(&rg.hand, &hand_late, sizeof(VrPose)) && !memcmp(&rg.game_hand, &hand_game, sizeof(VrPose)),
              "G2: late latch has the moved hand, the game's hand kept");
        CHECK(moved1 == moved0 + 1 && st1.viewmodel_draws == st0.viewmodel_draws + 1,
              "G2: the delta applied to exactly the weapon's draw (%u frames, %llu draws)", moved1 - moved0,
              (unsigned long long)(st1.viewmodel_draws - st0.viewmodel_draws));
        fx_now += 5 * MS;
        shell_xr_frame();
        check_layers_pose_identity("G2", &rp);
        check_stereo_pixels("G2", &rp, &g, 1);                 /* the world quad: where the submitted pose puts it */
        far = check_gun_pixels("G2", &rp, vrm_pose_apply(hand_late, k_grip), vrm_pose_apply(hand_game, k_grip), NULL);
        CHECK(far > 10.0, "G2: the late hand is far from the game's (%.1f px): the test can tell", far);
        printf("G2: the weapon moved %.1f px with the hand; the world did not\n", far);

        /* G3: gun_latch = off: the same offset, the weapon stays where the game put it */
        shell_set_gun_latch(0);
        fx_now += 30 * MS;
        gun_game_frame(&g, 32, hand_game, 1, 0x80032000u);
        fx_now += 30 * MS;
        gun_gpu_frame(&g, 0x80032000u);
        rp = *vr_render_pose();
        CHECK(!vr_render_gun()->valid, "G3: gun_latch off: no weapon latch");
        fx_now += 5 * MS;
        shell_xr_frame();
        check_stereo_pixels("G3", &rp, &g, 1);
        check_gun_pixels("G3", &rp, vrm_pose_apply(hand_game, k_grip), vrm_pose_apply(hand_game, k_grip), NULL);
        shell_set_gun_latch(1);

        /* G4: the game did not attach the weapon to the hand (VrGameGun.valid 0): not moved; G5: hand lost: not moved */
        vr_glue_gun_counters(&rec0, &drop0, &moved0);
        kvk_get_stats(&st0);
        fx_now += 30 * MS;
        gun_game_frame(&g, 33, hand_game, 0, 0x80033000u);
        fx_now += 30 * MS;
        gun_gpu_frame(&g, 0x80033000u);
        rp = *vr_render_pose();
        kvk_get_stats(&st1);
        CHECK(!vr_render_gun()->valid && st1.viewmodel_draws == st0.viewmodel_draws, "G4: an unattached weapon is world");
        fx_now += 5 * MS;
        shell_xr_frame();
        check_gun_pixels("G4", &rp, vrm_pose_apply(hand_game, k_grip), vrm_pose_apply(hand_game, k_grip), NULL);
        s_hand_tracked = 0;
        fx_now += 30 * MS;
        gun_game_frame(&g, 34, hand_game, 1, 0x80034000u);
        fx_now += 30 * MS;
        gun_gpu_frame(&g, 0x80034000u);
        rp = *vr_render_pose();
        vr_glue_gun_counters(&rec1, &drop1, &moved1);
        CHECK(!vr_render_gun()->valid && moved1 == moved0, "G5: the hand untracked at the late latch: no delta");
        fx_now += 5 * MS;
        shell_xr_frame();
        check_gun_pixels("G5", &rp, vrm_pose_apply(hand_game, k_grip), vrm_pose_apply(hand_game, k_grip), NULL);
        fx_hand_pose = NULL;
        s_hand_tracked = 0;
    }

    /* ---- the XR thread concurrently with the game + GPU threads ---- */
    {
        pthread_t th;
        int k, bad_px = 0;
        fx_on_release = on_release;
        fx_on_end_frame = on_end_frame;
        s_xr_stop = 0;
        pthread_create(&th, NULL, xr_thread, NULL);
        for (k = 0; k < 16; k++) {
            GameFrame g;
            const uint32_t va = 0x80010000u + (uint32_t)k * 0x100u;
            const int fails = s_fail;
            fx_now += 17 * MS;
            game_frame(&g, 10 + (uint32_t)k, VR_PRESENT_STEREO);
            vr_mark_world_pass(va, 1);
            fx_now += 21 * MS;
            gpu_world_frame(&g, va, k & 1);
            rp = *vr_render_pose();
            CHECK(rp.game_frame == 10 + (uint32_t)k, "threads: frame %d drawn with its camera", k);
            check_stereo_pixels("threads", &rp, &g, 1);
            if (s_fail != fails) bad_px++;
            usleep(4000);
        }
        s_xr_stop = 1;
        pthread_join(th, NULL);
        fx_on_release = NULL;
        fx_on_end_frame = NULL;
        CHECK(s_pair_checked > 20 && s_pair_bad == 0, "threads: %d submitted views checked, %d paired with the wrong pose",
              s_pair_checked, s_pair_bad);
        printf("threads: %llu xrEndFrame in all, %d submitted views paired with their render poses, %d wrong; %d frames with pixel errors\n",
               (unsigned long long)fx_end_count, s_pair_checked, s_pair_bad, bad_px);
    }
    {
        uint32_t made, failed, seen;
        vr_glue_counters(&made, &failed, &seen);
        CHECK(failed == 0 && made == seen, "camera marks: %u made, %u reached, %u refused", made, seen, failed);
    }
    {
        KvkStats st;
        kvk_get_stats(&st);
        printf("backend: frames %llu draws %llu stereo %llu hud %llu passes %llu pipelines %llu\n", (unsigned long long)st.frames,
               (unsigned long long)st.draws, (unsigned long long)st.stereo_draws, (unsigned long long)st.hud_draws,
               (unsigned long long)st.passes, (unsigned long long)st.pipelines);
    }
    if (multiview) test_panel();
    free(img);
    vr_renderer_shutdown();
    kvk_shutdown();
    return s_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "multiview";
    int r;
    /* the two-swapchain run also checks the gun latch's debug tint (cyan, half transparent) */
    if (!strcmp(mode, "two")) setenv("BLACKVR_GUN_LATCH_DEBUG", "0x80FFFF00", 1);
    else unsetenv("BLACKVR_GUN_LATCH_DEBUG");
    r = run(strcmp(mode, "two") != 0);
    if (r == 77) return 77;
    printf("vr-glue %s: %d checks, %d failed\n", mode, s_checks, s_fail);
    return s_fail ? 1 : 0;
}
