/* xr-shell: runs the exact src/black_vr.inc hooks over synthetic guest RAM.
 * No game code or assets: the "game" here is a few words of memory laid out the
 * way recomp_manual.c documents (renderer[2D67F4]+D450, owner [2D12B8]+740/+750/
 * +16A4, RwCamera frame [cam+58]+4 rows at +10/+50, look object yaw +18 / pitch
 * +1C, zoom [[look+8C]+2A4]) and a stand-in for D2F40 that builds the frame from
 * the look angles the way a game would. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <xmmintrin.h>
#include "vr_bridge.h"

#ifndef TEST_RIGHT_ROW_SIGN
#define TEST_RIGHT_ROW_SIGN (-1.0f)        /* BLACK: RW right row = screen left (docs/photo-mode.md) */
#endif
#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE 0x04000000u
#define RAM_BYTES 0x00800000u
static unsigned char ram[RAM_BYTES];
static ptrdiff_t g_xbox_mem_offset;
static void *xbox_GetMemoryBase(void) { return ram; }
static size_t xbox_GetMappedSize(void) { return RAM_BYTES; }
static void *xbox_GetPhysicalMemoryBase(void) { return NULL; }
#define GUEST32(a) (*(uint32_t *)(ram + (a)))
#define GUEST8(a) (*(uint8_t *)(ram + (a)))
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NEAR(a, b, e) (fabs((double)(a) - (double)(b)) <= (e))

/* ---- what recomp_manual.c provides around the include ---------------------------------- */
static uint32_t black_vm_scale_bits = 0x3F800000u;
static int black_split_pass;
static int photo_on, split_on, blocked;
static int pc_photo_active(void) { return photo_on; }
static int pc_split_enabled(void) { return split_on; }
static int black_pc_input_gameplay_blocked_query(void) { return blocked; }
#define RENDERER 0x00200000u
#define OWNER 0x00300000u
#define LOOK 0x00310000u
#define ZOOMOBJ 0x00320000u
#define RWCAM 0x00330000u
#define FRAME 0x00340000u
#define CAMERA (RENDERER + 0xD450u)
static uint32_t black_photo_world_camera(void)
{
    if (GUEST32(CAMERA + 0x68u) != OWNER + 0x740u || !GUEST8(OWNER + 0x16A4u)) return 0;
    return CAMERA;
}
static uint32_t xbox_PhysicalAddressOf(uint32_t va);
void vr_mark_world_pass(uint32_t va, int begin);
/* the live action map's resolver (recomp_manual.c): slot -> control, a fixed table here */
int black_pc_input_resolver(int slot) { return slot == 12 ? 14 : slot == 9 ? 23 : -1; }
#include "black_vr.extracted.inc"

/* ---- the shell, faked ------------------------------------------------------------------- */
static VrFrameState fs;
static VrGameCamera pub_cam;
static VrGameCameraExt pub_ext;
static int pub_count, present;
static double pending_turn;
static VrGameConfig gcfg;
static VrPose origin;
const VrFrameState *vr_frame_state(void) { return &fs; }
const VrFrameState *vr_latch_frame_state(void) { return &fs; }
int vr_active(void) { return 1; }
void vr_set_present_mode(int m) { present = m; }
double vr_consume_turn(void) { const double t = pending_turn; pending_turn = 0; return t; }
const VrGameConfig *vr_game_config(void) { return &gcfg; }
void vr_get_recentre(VrPose *o) { *o = origin; }
static int marks;
void vr_mark_world_pass(uint32_t va, int begin) { (void)va; (void)begin; marks++; }
static uint32_t xbox_PhysicalAddressOf(uint32_t va) { return va & 0x07FFFFFFu; }
void vr_publish_game_camera_ex(const VrGameCamera *c, const VrGameCameraExt *e) { pub_cam = *c; pub_ext = *e; pub_count++; }
/* gun latch (port/notes/gun-latch.md): the weapon pose published with the camera, the re-targeted MVPs recorded */
static VrGameGun pub_gun;
static int gun_count, vm_count, vm_reg = -1;
static float vm_mvp[16];
void vr_publish_game_gun(const VrGameGun *g) { pub_gun = *g; gun_count++; }
void vr_mark_viewmodel_mvp(const float m[16], int reg) { memcpy(vm_mvp, m, sizeof(vm_mvp)); vm_reg = reg; vm_count++; }
/* VR controls: the game's input state out, the scope's aim request in */
static int (*got_resolver)(int);
static VrGameInputState pub_in;
static int pub_in_count, aim_request = -1;
void vr_set_action_resolver(int (*r)(int)) { got_resolver = r; }
void vr_publish_game_input(const VrGameInputState *st) { pub_in = *st; pub_in_count++; }
int vr_game_aim_request(void) { return aim_request; }

static float depth_a, depth_b;
static void putf(uint32_t va, float v) { memcpy(ram + va, &v, 4); }
static float getf(uint32_t va) { float v; memcpy(&v, ram + va, 4); return v; }

/* World convention of this synthetic game: right-handed world, +y up; game yaw (degrees)
 * grows to the left; forward = (sin y, 0, cos y) rotated; the RW "right" row is the
 * screen's LEFT, as BLACK's is. */
static void game_produce(void)
{
    const double yaw = getf(LOOK + 0x18u) * VRM_DEG, pitch = getf(LOOK + 0x1Cu) * VRM_DEG;
    /* world yaw of forward = -game yaw + 0.3 (an arbitrary origin) */
    const double psi = -yaw + 0.3;
    VrVec3 F = vrm_v3((float)(cos(-pitch) * sin(psi)), (float)sin(-pitch), (float)(cos(-pitch) * cos(psi)));
    VrVec3 S, U, rows[4];
    const VrVec3 up = vrm_v3(0, 1, 0);
    int r, k;
    /* screen right of a right-handed, y-up world looking along F: F x up */
    S = vrm_cross(F, up); vrm_normalize(&S);
    U = vrm_cross(S, F);
    rows[0] = vrm_scale(S, TEST_RIGHT_ROW_SIGN);
    rows[1] = U; rows[2] = F; rows[3] = vrm_v3(10, 1.7f, -4);
    for (r = 0; r < 4; r++)
        for (k = 0; k < 3; k++) {
            const float v = k == 0 ? rows[r].x : k == 1 ? rows[r].y : rows[r].z;
            putf(FRAME + 0x10u + r * 0x10u + k * 4u, v);
            putf(FRAME + 0x50u + r * 0x10u + k * 4u, v);
        }
    /* the lens: halfX = window70 * tan(angle/2), halfY = halfX / aspect74 */
    {
        const float hx = getf(CAMERA + 0x70u) * tanf(getf(OWNER + 0x750u) * 0.5f * (float)VRM_DEG);
        putf(CAMERA + 0x48u, hx);
        putf(CAMERA + 0x4Cu, hx / getf(CAMERA + 0x74u));
    }
}

static void d2f40(void)
{
    black_vr_camera_begin(CAMERA);
    game_produce();
    black_vr_camera_restore(CAMERA);
    black_vr_camera_after(CAMERA);
}

static VrPose P(float yaw_deg, float pitch_deg, float x, float y, float z)
{
    VrPose p;
    p.orientation = vrm_q_mul(vrm_q_axis_angle(vrm_v3(0, 1, 0), (float)(yaw_deg * VRM_DEG)),
                              vrm_q_axis_angle(vrm_v3(1, 0, 0), (float)(pitch_deg * VRM_DEG)));
    p.position = vrm_v3(x, y, z);
    return p;
}

static void set_head(VrPose h)
{
    int i;
    fs.head = h;
    for (i = 0; i < 2; i++) {
        fs.eye[i].pose = vrm_pose_mul(h, P(0, 0, i ? 0.032f : -0.032f, 0, 0));
        fs.eye[i].fov.left = -0.9f; fs.eye[i].fov.right = 0.9f; fs.eye[i].fov.up = 0.85f; fs.eye[i].fov.down = -0.85f;
    }
}

static void setup(void)
{
    memset(ram, 0, sizeof(ram));
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)ram;
    GUEST32(0x002D67F4u) = RENDERER;
    GUEST32(0x002D12B8u) = OWNER;
    GUEST32(CAMERA + 0x68u) = OWNER + 0x740u;
    GUEST8(OWNER + 0x16A4u) = 1;
    putf(OWNER + 0x750u, 70.0f);
    putf(CAMERA + 0x70u, 1.0f);
    putf(CAMERA + 0x74u, 4.0f / 3.0f);
    GUEST32(CAMERA + 0x58u) = RWCAM;
    GUEST32(RWCAM + 4u) = FRAME;
    GUEST32(LOOK + 0x8Cu) = ZOOMOBJ;
    putf(ZOOMOBJ + 0x2A4u, 1.0f);
    putf(0x002888C8u, 70.0f);
    putf(0x00295E38u, -70.0f);
    GUEST32(RENDERER + 0xD590u) = CAMERA;               /* the world camera is the one being drawn */
    vgm_depth_row(0.1f, 300.0f, &depth_a, &depth_b);
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.aim_mode = VR_AIM_HAND;
    gcfg.aim_hand = 1;
    gcfg.units_per_metre = 1.0f;
    gcfg.max_head_offset_m = 0.5f;
    gcfg.aim_converge_m = 10.0f;
    gcfg.scope_zoom_min = 2.5f;
    gcfg.fov_margin = 1.1f;
    gcfg.bob_filter = 0;
    gcfg.hand_weapon = 1;
    memset(&fs, 0, sizeof(fs));
    origin = P(0, 0, 0, 1.6f, 0);
    set_head(origin);
    fs.hand[1].aim = P(0, 0, 0.2f, 1.3f, -0.3f);
    fs.hand[1].valid = 1;
}

static VrVec3 frame_forward(void) { return vrm_v3(getf(FRAME + 0x30u), getf(FRAME + 0x34u), getf(FRAME + 0x38u)); }
static VrVec3 frame_screen_right(void) { return vrm_scale(vrm_v3(getf(FRAME + 0x10u), getf(FRAME + 0x14u), getf(FRAME + 0x18u)), TEST_RIGHT_ROW_SIGN); }

#define UPLOAD 0x00350000u
/* What the game's renderer uploads for a static world draw: the transposed MVP of the camera
 * frame as it renders it (screen right = the right row times this game's convention). */
static void world_mvp(float r[16])
{
    VrMat4 w_view, view_from_world, mvp;
    int k, j;
    w_view = vgm_world_from_view(frame_screen_right(), vrm_v3(getf(FRAME + 0x20u), getf(FRAME + 0x24u), getf(FRAME + 0x28u)),
                                 frame_forward(), vrm_v3(getf(FRAME + 0x40u), getf(FRAME + 0x44u), getf(FRAME + 0x48u)));
    vrm_m4_inverse(w_view, &view_from_world);
    mvp = vrm_m4_mul(vgm_game_proj(getf(CAMERA + 0x48u), getf(CAMERA + 0x4Cu), depth_a, depth_b), view_from_world);
    for (k = 0; k < 4; k++) for (j = 0; j < 4; j++) r[k * 4 + j] = VRM_AT(mvp, k, j);
}

/* One game frame: the look integrator, the world camera, then a world draw. */
static void frame(void)
{
    float r[16];
    const int vm_before = vm_count;
    black_vr_look(LOOK);
    d2f40();
    world_mvp(r);
    memcpy(ram + UPLOAD, r, sizeof(r));
    black_vr_upload(UPLOAD);
    CHECK(!memcmp(ram + UPLOAD, r, sizeof(r)));       /* world draws are never changed */
    CHECK(vm_count == vm_before);                     /* ... nor recorded as the weapon */
}

int main(void)
{
    int i;
    double yaw0;
    setup();
    putf(LOOK + 0x18u, 20.0f);
    putf(LOOK + 0x1Cu, 0.0f);
    /* the first world camera has no look yet: flat */
    d2f40();
    CHECK(present == VR_PRESENT_FLAT_MENU && pub_count == 1 && !pub_cam.valid);
    for (i = 0; i < 90; i++) frame();
    CHECK(present == VR_PRESENT_STEREO && pub_cam.valid);
    CHECK(bvr.hand_locked && bvr.screen_sign == TEST_RIGHT_ROW_SIGN);
    /* the lens was widened for D2F40 and restored afterwards */
    CHECK(getf(CAMERA + 0x70u) == 1.0f && NEAR(getf(CAMERA + 0x74u), 4.0 / 3.0, 1e-6));
    CHECK(NEAR(getf(CAMERA + 0x48u), tan(0.9) * 1.1, 1e-4) && NEAR(getf(CAMERA + 0x4Cu), tan(0.85) * 1.1, 1e-4));
    CHECK(black_vm_scale_bits == 0x3F800000u);

    /* the view is level and looks where the head looks, whatever the hand does */
    {
        const VrVec3 f = frame_forward();
        CHECK(NEAR(f.y, 0.0, 1e-5));
    }
    yaw0 = getf(LOOK + 0x18u);
    /* the hand points 30 degrees left: the game's aim yaw follows; the head view does not */
    {
        const VrVec3 f_before = frame_forward();
        fs.hand[1].aim = P(30, 0, 0.2f, 1.3f, -0.3f);
        for (i = 0; i < 10; i++) frame();
        CHECK(NEAR(getf(LOOK + 0x18u) - yaw0, 30.0, 1.0));
        CHECK(vrm_dot(f_before, frame_forward()) > 0.99999f);
    }
    /* the head turns 40 degrees left: the view follows, to the left on screen */
    {
        const VrVec3 f_before = frame_forward(), s_before = frame_screen_right();
        set_head(vrm_pose_mul(origin, P(40, 0, 0, 0, 0)));
        for (i = 0; i < 3; i++) frame();
        CHECK(NEAR(acos(vrm_dot(f_before, frame_forward())) / VRM_DEG, 40.0, 0.05));
        CHECK(vrm_dot(vrm_sub(frame_forward(), f_before), s_before) < 0.0f);
        set_head(origin);
        for (i = 0; i < 3; i++) frame();
    }
    /* the head looks up 25 degrees: the view pitches up; a pitch-only head does not move the aim */
    {
        const float aim_pitch = getf(LOOK + 0x1Cu);
        set_head(vrm_pose_mul(origin, P(0, 25, 0, 0, 0)));
        for (i = 0; i < 3; i++) frame();
        CHECK(NEAR(asin(frame_forward().y) / VRM_DEG, 25.0, 0.05));
        CHECK(NEAR(getf(LOOK + 0x1Cu), aim_pitch, 0.6));      /* hand-aim convergence moves slightly with the head */
        set_head(origin);
    }
    /* the hand (at the head) points 20 degrees down: the game pitch is +20 (down grows) */
    fs.hand[1].aim = P(0, -20, 0.0f, 1.6f, 0.0f);
    for (i = 0; i < 3; i++) frame();
    CHECK(NEAR(getf(LOOK + 0x1Cu), 20.0, 0.01));
    fs.hand[1].aim = P(0, 0, 0.2f, 1.3f, -0.3f);
    for (i = 0; i < 3; i++) frame();

    /* a snap turn: 30 degrees left in both the view and the aim */
    {
        const VrVec3 f_before = frame_forward(), s_before = frame_screen_right();
        const float aim_before = getf(LOOK + 0x18u);
        pending_turn = 30.0;
        for (i = 0; i < 3; i++) frame();
        CHECK(NEAR(acos(vrm_dot(f_before, frame_forward())) / VRM_DEG, 30.0, 0.05));
        CHECK(vrm_dot(vrm_sub(frame_forward(), f_before), s_before) < 0.0f);
        CHECK(NEAR(vrm_wrap_deg(getf(LOOK + 0x18u) - aim_before), 30.0, 0.01));
    }
    /* camera shake in the game's matrix (a roll and a yaw jitter) never reaches the view */
    {
        const VrVec3 f_before = frame_forward();
        for (i = 0; i < 30; i++) {
            black_vr_look(LOOK);
            black_vr_camera_begin(CAMERA);
            game_produce();
            putf(FRAME + 0x30u, getf(FRAME + 0x30u) + 0.03f * (float)sin(i));   /* shake the at row */
            putf(FRAME + 0x14u, 0.05f);                                         /* and roll the right row */
            black_vr_camera_restore(CAMERA);
            black_vr_camera_after(CAMERA);
        }
        CHECK(vrm_dot(f_before, frame_forward()) > 0.9995f);
        CHECK(NEAR(getf(FRAME + 0x14u), 0.0, 1e-6));    /* horizon level */
    }
    /* the scope: high zoom goes flat and leaves the frame to the game */
    putf(ZOOMOBJ + 0x2A4u, 4.0f);
    frame();
    CHECK(present == VR_PRESENT_FLAT_SCOPE && !pub_cam.valid);
    putf(ZOOMOBJ + 0x2A4u, 1.0f);
    frame();
    CHECK(present == VR_PRESENT_STEREO);
    /* pause, photo mode, scripted camera */
    blocked = 1; frame(); CHECK(present == VR_PRESENT_FLAT_MENU); blocked = 0;
    photo_on = 1; frame(); CHECK(present == VR_PRESENT_FLAT_MENU); photo_on = 0;
    GUEST8(OWNER + 0x16A4u) = 0; frame(); CHECK(present == VR_PRESENT_FLAT_CINEMA); GUEST8(OWNER + 0x16A4u) = 1;
    for (i = 0; i < 70; i++) frame();
    CHECK(present == VR_PRESENT_STEREO);
    /* a non-world camera leaves everything alone */
    {
        const int before = pub_count;
        black_vr_camera_begin(RENDERER + 0xD4F0u);
        black_vr_camera_restore(RENDERER + 0xD4F0u);
        black_vr_camera_after(RENDERER + 0xD4F0u);
        CHECK(pub_count == before && present == VR_PRESENT_STEREO);
    }

    /* the published camera: its view is the inverse of the head frame and the near/far guard holds */
    {
        VrMat4 v, w;
        memcpy(v.m, pub_cam.view, sizeof(v.m));
        CHECK(vrm_m4_inverse(v, &w));
        CHECK(vrm_dot(vgm_col(w, 2), frame_forward()) > 0.99999f);
        CHECK(NEAR(VRM_AT(w, 0, 3), 10.0, 1e-3) && NEAR(VRM_AT(w, 1, 3), 1.7, 1e-3));
        CHECK(pub_ext.head_pose.position.y == fs.head.position.y);
    }

    /* the world pass is marked for the renderer in every VR present mode (stereo and the flat ones: the
     * renderer picks the frame's camera from the mark), and not at all with VR off */
    GUEST32(0x0027BFF8u) = 0x00360000u;
    GUEST32(0x00360000u) = 0x80012340u;
    black_vr_world_mark(1); black_vr_world_mark(0);
    CHECK(marks == 2);
    blocked = 1; frame(); black_vr_world_mark(1); CHECK(marks == 3); blocked = 0;
    {
        const int saved_mode = bvr.mode;
        bvr.mode = -1; black_vr_world_mark(1); CHECK(marks == 3); bvr.mode = saved_mode;
    }
    for (i = 0; i < 3; i++) frame();

    /* learned from the world uploads: depth row, screen-right convention, clip y; then the weapon moves */
    {
        float r[16];
        CHECK(bvr.depth_ok && NEAR(bvr.depth_a, depth_a, 1e-3) && NEAR(bvr.depth_b, depth_b, 1e-3));
        CHECK(bvr.hand_locked && bvr.screen_sign == TEST_RIGHT_ROW_SIGN && bvr.clip_y_sign == 1.0);
        CHECK(bvr.yv_ok);
        world_mvp(r);
        memcpy(ram + UPLOAD, r, sizeof(r));
        black_vr_vm_push(1);
        black_vr_upload(UPLOAD);
        black_vr_vm_pop();
        CHECK(memcmp(ram + UPLOAD, r, sizeof(r)) != 0);
        CHECK(black_vr_vm_draw == 0);
        /* gun latch: exactly the uploaded (re-targeted) MVP is recorded, for c10; the flash's for c112 */
        CHECK(vm_count == 1 && vm_reg == 10 && !memcmp(vm_mvp, ram + UPLOAD, sizeof(vm_mvp)));
        world_mvp(r);
        memcpy(ram + UPLOAD, r, sizeof(r));
        black_vr_vm_push(1);
        black_vr_upload_at(UPLOAD, 0x70);
        black_vr_vm_pop();
        CHECK(vm_count == 2 && vm_reg == 112 && !memcmp(vm_mvp, ram + UPLOAD, sizeof(vm_mvp)));
        /* the camera went out with the hand pose the weapon was attached to */
        CHECK(gun_count > 0 && pub_gun.valid && pub_gun.hand_index == 1 &&
              !memcmp(&pub_gun.hand, &fs.hand[1].aim, sizeof(VrPose)));
        /* an untracked hand: the weapon stays with the game (not re-targeted, nothing recorded, the pose invalid) */
        fs.hand[1].valid = 0;
        frame();
        CHECK(!pub_gun.valid && !bvr.yv_ok);
        world_mvp(r);
        memcpy(ram + UPLOAD, r, sizeof(r));
        black_vr_vm_push(1);
        black_vr_upload(UPLOAD);
        black_vr_vm_pop();
        CHECK(vm_count == 2 && !memcmp(ram + UPLOAD, r, sizeof(r)));
        fs.hand[1].valid = 1;
    }
    /* the player's input update (1EC450): the game's crouch / Toggle Crouch / scope state goes out, the scope's
     * aim intent comes in (latched against the native toggle); another player's component is left alone */
    {
#define COMP 0x00380000u
#define WEAPON 0x00390000u
#define DESC 0x003A0000u
        CHECK(GUEST32(LOOK + 0x8Cu) == ZOOMOBJ);
        GUEST32(COMP + 0x8Cu) = ZOOMOBJ;
        GUEST32(ZOOMOBJ + 0x29Cu) = WEAPON;
        GUEST32(WEAPON + 0xECu) = DESC;
        GUEST8(COMP + 0x40u) = 1; GUEST8(COMP + 0x102u) = 1;
        GUEST8(WEAPON + 0x105u) = 0; GUEST8(DESC + 0xC0u) = 1; putf(DESC + 0xB4u, 4.0f);
        GUEST8(COMP + 0x45u) = 0; GUEST8(COMP + 0x104u) = 0;
        aim_request = -1;
        black_vr_input(COMP);
        CHECK(got_resolver == black_pc_input_resolver && pub_in_count == 1);
        CHECK(pub_in.valid && pub_in.crouched == 1 && pub_in.crouch_toggle == 1 && pub_in.scoped == 1 && !pub_in.aiming);
        CHECK(pub_in.weapon == WEAPON && pub_in.max_zoom == 4.0f && pub_in.zoom == 1.0f);
        CHECK(GUEST8(COMP + 0x45u) == 0 && GUEST8(COMP + 0x104u) == 0);    /* -1: the game's own */
        aim_request = 1;
        black_vr_input(COMP);
        CHECK(GUEST8(COMP + 0x45u) == 1 && GUEST8(COMP + 0x104u) == 1);
        aim_request = 0;
        GUEST8(COMP + 0x104u) = 0;
        black_vr_input(COMP);
        CHECK(GUEST8(COMP + 0x45u) == 0 && GUEST8(COMP + 0x104u) == 1 && pub_in.serial == 3);
        GUEST32(COMP + 0x8Cu) = ZOOMOBJ + 0x1000u;                         /* player two's actor */
        aim_request = 1;
        black_vr_input(COMP);
        CHECK(pub_in_count == 3 && GUEST8(COMP + 0x45u) == 0);
        aim_request = -1;
    }
    printf("xr_shell hooks: %u checks passed\n", checks);
    return 0;
}
