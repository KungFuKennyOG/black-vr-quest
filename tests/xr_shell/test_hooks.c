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
#include <time.h>
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
static int offhand_count;
void vr_mark_viewmodel_offhand(const float a[16], const float c[3]) { (void)a; (void)c; offhand_count++; }
void vr_grip_correction(float c[3]) { c[0] = c[1] = c[2] = 0.0f; }
void vr_set_gun_barrel(const float d[3], int valid) { (void)d; (void)valid; }
void vr_set_gun_laser(const float m[3], const float d[3], float l, int v) { (void)m; (void)d; (void)l; (void)v; }
void vr_set_scope_view(float z) { (void)z; }
float vr_scope_view(void) { return 0.0f; }
int vr_scope_aim(VrQuat *q) { (void)q; return 0; }
void vr_game_haptic(int hand, float strength, uint32_t ms) { (void)hand; (void)strength; (void)ms; }
unsigned vr_button_presses(void) { return 0; }
void vr_grip_lock(float d[16]) { int i; for (i = 0; i < 16; i++) d[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
void vr_note_shot(void) { }
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

/* The player's aim matrix ([2D45D4]+100, RenderWare rows): the game poses the first-person model on it in its update
 * and the camera entity copies it later in the frame; written from the frame's rows when aim_on. */
static int aim_on;                 /* 1: the aim matrix is the camera entity; 2: the entity is A * O (aim_oinv = O^-1) */
static VrMat4 aim_oinv;
static void aim_from_rows(const float rows[4][3])
{
    const uint32_t a = GUEST32(0x002D45D4u) + 0x100u;
    int r, k;
    for (r = 0; r < 4; r++) for (k = 0; k < 3; k++) memcpy(ram + a + r * 0x10u + k * 4u, &rows[r][k], 4);
}
static void aim_from_view(VrMat4 w)
{
    float rows[4][3];
    vgm_view_to_frame(w, TEST_RIGHT_ROW_SIGN, rows);
    aim_from_rows(rows);
}

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
    if (aim_on) {
        float fr[4][3];
        for (r = 0; r < 4; r++) for (k = 0; k < 3; k++) fr[r][k] = getf(FRAME + 0x10u + r * 0x10u + k * 4u);
        if (aim_on == 2) aim_from_view(vrm_m4_mul(vgm_frame_to_view(fr, TEST_RIGHT_ROW_SIGN), aim_oinv));
        else aim_from_rows(fr);
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
    /* the first STEREO frame already faces the game's way: no turn on the frame after (sweep issue 1: the look
     * integrator skipped while the view was flat, so the body yaw was still unset) */
    {
        VrVec3 f1;
        frame();
        CHECK(present == VR_PRESENT_STEREO);
        f1 = frame_forward();
        frame();
        CHECK(vrm_dot(f1, frame_forward()) > 0.99999f);
    }
    for (i = 0; i < 88; i++) frame();
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
    /* a scoped weapon goes flat from 2.0 (the sniper's X2 is 2.33, under scope_zoom 2.5; sweep issue 2); another
     * weapon at that zoom stays STEREO, and so does the scoped one at the raise animation's 1.10 */
    {
#define SCOPE_WEAPON 0x00500000u
#define SCOPE_DESC 0x00510000u
        GUEST32(ZOOMOBJ + 0x29Cu) = SCOPE_WEAPON;
        GUEST32(SCOPE_WEAPON + 0xECu) = SCOPE_DESC;
        GUEST8(SCOPE_DESC + 0xC0u) = 0;
        putf(ZOOMOBJ + 0x2A4u, 2.33f);
        frame();
        CHECK(present == VR_PRESENT_STEREO);
        GUEST8(SCOPE_DESC + 0xC0u) = 1;
        frame();
        CHECK(present == VR_PRESENT_FLAT_SCOPE && !pub_cam.valid);
        putf(ZOOMOBJ + 0x2A4u, 1.10f);
        frame();
        CHECK(present == VR_PRESENT_STEREO);
        gcfg.scope_zoom_min = 1.5f;                     /* a lower scope_zoom in blackvr.ini still wins */
        putf(ZOOMOBJ + 0x2A4u, 1.8f);
        frame();
        CHECK(present == VR_PRESENT_FLAT_SCOPE);
        gcfg.scope_zoom_min = 2.5f;
        putf(ZOOMOBJ + 0x2A4u, 1.0f);
        GUEST8(SCOPE_DESC + 0xC0u) = 0;
        GUEST32(ZOOMOBJ + 0x29Cu) = 0;
        frame();
        CHECK(present == VR_PRESENT_STEREO);
    }
    /* a hitch (300 ms between the look and the world camera) stays STEREO (sweep issue 3: the old 250 ms rule made
     * it one flat frame); the look integrator stopping goes flat within a few world cameras */
    {
        const struct timespec ts = { 0, 300000000L };
        black_vr_look(LOOK);
        nanosleep(&ts, NULL);
        d2f40();
        CHECK(present == VR_PRESENT_STEREO && pub_cam.valid);
        for (i = 0; i < 5; i++) d2f40();               /* six world cameras since the look: still STEREO */
        CHECK(present == VR_PRESENT_STEREO);
        d2f40();                                        /* the seventh: flat */
        CHECK(present == VR_PRESENT_FLAT_MENU && !pub_cam.valid);
        frame();
        CHECK(present == VR_PRESENT_STEREO);
        frame();
    }
    /* back from a flat stretch with the game turned meanwhile (a checkpoint, a level change): the first STEREO frame
     * faces the game's new way (sweep issue 1) */
    {
        VrVec3 f_before, f1;
        f_before = frame_forward();
        blocked = 1; frame(); CHECK(present == VR_PRESENT_FLAT_MENU);
        putf(LOOK + 0x18u, vrm_wrap_deg(getf(LOOK + 0x18u) + 90.0));
        blocked = 0;
        frame();
        CHECK(present == VR_PRESENT_STEREO);
        f1 = frame_forward();
        CHECK(NEAR(acos(vrm_dot(f_before, f1)) / VRM_DEG, 90.0, 0.5));
        frame();
        CHECK(vrm_dot(f1, frame_forward()) > 0.99999f);
    }
    /* a stop-the-game tutorial hint (Kenny 2026-11-13): the game stops its clock and the look integrator, and keeps
     * drawing the frozen world; VR stays STEREO. The pause menu over it is flat; the frame after the hint is not. */
    {
        static const uint32_t stop_ids[5] = { 12, 13, 23, 87, 24 };
        const uint32_t el = 0x520000u, gm = 0x600000u;
        for (i = 0; i < 5; i++) GUEST32(0x0028AA80u + 4u * (uint32_t)i) = stop_ids[i];
        GUEST32(0x002D45F0u) = gm;
        bvr_hint_el = el;
        GUEST32(el + 0x2110u) = 8u; GUEST32(el + 0x22A4u) = 0xFFFFFFFFu; GUEST8(el + 0x1Bu) = 7u;
        blocked = 1; d2f40(); CHECK(present == VR_PRESENT_FLAT_MENU);      /* a hint that does not stop the game */
        blocked = 0; frame(); CHECK(present == VR_PRESENT_STEREO);
        GUEST32(el + 0x22A4u) = 13u; GUEST8(el + 0x1Bu) = 5u;               /* pending, fading in */
        blocked = 1;
        for (i = 0; i < 20; i++) { d2f40(); CHECK(present == VR_PRESENT_STEREO && pub_cam.valid); }
        GUEST32(el + 0x2110u) = 13u; GUEST32(el + 0x22A4u) = 0xFFFFFFFFu; GUEST8(el + 0x1Bu) = 7u;
        d2f40(); CHECK(present == VR_PRESENT_STEREO);
        GUEST8(gm + 0x210C8u) = 1; d2f40(); CHECK(present == VR_PRESENT_FLAT_MENU);   /* the pause menu over it */
        GUEST8(gm + 0x210C8u) = 0; d2f40(); CHECK(present == VR_PRESENT_STEREO);
        GUEST32(el + 0x2110u) = 0xFFFFFFFFu; GUEST8(el + 0x1Bu) = 0u; blocked = 0;  /* gone, no look yet */
        d2f40(); CHECK(present == VR_PRESENT_STEREO);
        frame(); CHECK(present == VR_PRESENT_STEREO);
        GUEST32(el + 0x2110u) = 13u; GUEST8(el + 0x1Bu) = 7u; blocked = 1;
        d2f40(); CHECK(present == VR_PRESENT_STEREO);
        GUEST32(el + 0x2110u) = 0xFFFFFFFFu; GUEST8(el + 0x1Bu) = 0u; blocked = 0;
        bvr_hint_el = 0; GUEST32(0x002D45F0u) = 0;
        frame(); CHECK(present == VR_PRESENT_STEREO);
    }
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
    /* the weapon's held effects (the muzzle glow and smoke a shot puts on the player's first-person record 5) follow
     * the records 0..7 in their update (176BD0); around it the records are where the last render drew them in the hand,
     * carried by the hand's move since; afterwards they are the game's again */
    {
#define FXCORE 0x003B0000u
#define FXMODEL 0x003C0000u
#define FXCNT 0x003C1000u
#define FXREC 0x003D0000u
        uint8_t kept[8][0x40];
        VrMat4 he, hh, lprev[8], x, hinv;
        unsigned r, j;
        gcfg.vm_offset_m[0] = 0.12f; gcfg.vm_offset_m[1] = -0.18f; gcfg.vm_offset_m[2] = 0.30f;
        fs.hand[1].aim = P(25, -10, 0.25f, 1.25f, -0.35f);
        GUEST32(0x002D45D4u) = FXCORE;
        GUEST32(FXCORE + 0x358u) = FXMODEL;
        GUEST32(FXMODEL + 0x50u) = FXCNT;
        for (i = 0; i < 4; i++) frame();
        CHECK(bvr.yv_ok && bvr.hand_locked && bvr.mode == VR_PRESENT_STEREO);
        for (r = 0; r < 8u; r++) {
            /* records 2 and 5 present (the muzzle: 0.39 forward, 0.09 right, 0.06 down of the game's camera entity) */
            const VrMat4 m = vrm_m4_mul(bvr.w_entity, vrm_m4_from_pose(r == 5 ? P(3, 2, 0.09f, -0.06f, 0.39f)
                                                                              : P(-8, 5, 0.02f, -0.12f, 0.20f)));
            const uint32_t at = FXREC + r * 0x100u + 0x40u;
            GUEST32(FXCNT + 0x38u + r * 4u) = (r == 2 || r == 5) ? 0u : 0xFFFFFFFFu;
            GUEST32(FXMODEL + 0x30u + r * 4u) = FXREC + r * 0x100u;
            for (j = 0; j < 4; j++) {
                putf(at + j * 0x10u, VRM_AT(m, 0, j)); putf(at + j * 0x10u + 4u, VRM_AT(m, 1, j));
                putf(at + j * 0x10u + 8u, VRM_AT(m, 2, j)); GUEST32(at + j * 0x10u + 12u) = 0x5A5A0000u + r * 16u + j;
            }
            lprev[r] = m;
        }
        frame();                                              /* the render captures X and the records */
        CHECK(bvr_fxl.ok && bvr_fxl.at[5] == FXREC + 0x540u && bvr_fxl.at[2] == FXREC + 0x240u && !bvr_fxl.at[3]);
        CHECK(!bvr_fxl.aim);                                  /* no aim matrix (it is not the camera): the fallback */
        CHECK(vrm_m4_inverse(bvr.w_entity, &he) && vrm_m4_inverse(bvr.w_hand, &hh) && vrm_m4_inverse(bvr.w_head, &hinv));
        x = vrm_m4_mul(bvr.w_head, vrm_m4_mul(bvr.yv, hinv));
        /* the next update: the game moved record 5 (the player stepped 5 cm right, 8 cm back; the weapon tilted 4 deg);
         * the hand did not move */
        {
            const VrMat4 step = vrm_m4_from_pose(P(0, 0, 0.05f, 0.0f, -0.08f));
            const VrMat4 m = vrm_m4_mul(step, vrm_m4_mul(lprev[5], vrm_m4_from_pose(P(0, 4, 0, 0, 0))));
            const uint32_t at = FXREC + 0x540u;
            for (j = 0; j < 4; j++) {
                putf(at + j * 0x10u, VRM_AT(m, 0, j)); putf(at + j * 0x10u + 4u, VRM_AT(m, 1, j));
                putf(at + j * 0x10u + 8u, VRM_AT(m, 2, j));
            }
            for (r = 0; r < 8u; r++) memcpy(kept[r], ram + FXREC + r * 0x100u + 0x40u, 0x40u);
            black_vr_fx_update_begin();
            /* the rows: turned by X (the last render's re-target of the weapon), the game's own tilt kept */
            for (j = 0; j < 3; j++) {
                float a[4] = { VRM_AT(m, 0, j), VRM_AT(m, 1, j), VRM_AT(m, 2, j), 0.0f }, b[4];
                vrm_m4_apply(x, a, b);
                CHECK(NEAR(getf(at + j * 0x10u), b[0], 2e-5) && NEAR(getf(at + j * 0x10u + 4u), b[1], 2e-5) &&
                      NEAR(getf(at + j * 0x10u + 8u), b[2], 2e-5));
                CHECK(GUEST32(at + j * 0x10u + 12u) == 0x5A5A0000u + 5u * 16u + j);
            }
            /* the position, too: the game's record by X; relative to the hand, the game's relative to the render's camera
             * entity less the grip */
            {
                float a[4] = { VRM_AT(m, 0, 3), VRM_AT(m, 1, 3), VRM_AT(m, 2, 3), 1.0f }, b[4], c[4];
                vrm_m4_apply(x, a, b);
                CHECK(NEAR(getf(at + 0x30u), b[0], 2e-5) && NEAR(getf(at + 0x34u), b[1], 2e-5) &&
                      NEAR(getf(at + 0x38u), b[2], 2e-5));
                vrm_m4_apply(hh, b, c);
                vrm_m4_apply(he, a, b);
                CHECK(NEAR(c[0], b[0] - 0.12f, 2e-5) && NEAR(c[1], b[1] + 0.18f, 2e-5) && NEAR(c[2], b[2] - 0.30f, 2e-5));
            }
            /* record 2, unmoved since the render: exactly the weapon's re-target */
            {
                const uint32_t at2 = FXREC + 0x240u;
                float a[4] = { VRM_AT(lprev[2], 0, 3), VRM_AT(lprev[2], 1, 3), VRM_AT(lprev[2], 2, 3), 1.0f }, b[4], c[4];
                vrm_m4_apply(he, a, b);
                c[0] = getf(at2 + 0x30u); c[1] = getf(at2 + 0x34u); c[2] = getf(at2 + 0x38u); c[3] = 1.0f;
                vrm_m4_apply(hh, c, a);
                CHECK(NEAR(a[0], b[0] - 0.12f, 2e-5) && NEAR(a[1], b[1] + 0.18f, 2e-5) && NEAR(a[2], b[2] - 0.30f, 2e-5));
            }
            for (r = 0; r < 8u; r++)
                if (r != 2 && r != 5) CHECK(!memcmp(ram + FXREC + r * 0x100u + 0x40u, kept[r], 0x40u));   /* absent */
            black_vr_fx_update_end();
            for (r = 0; r < 8u; r++) CHECK(!memcmp(ram + FXREC + r * 0x100u + 0x40u, kept[r], 0x40u));
        }
        /* nothing moves outside STEREO, or after a render whose weapon stayed with the game (hand untracked) */
        bvr.mode = VR_PRESENT_FLAT_MENU;
        black_vr_fx_update_begin();
        for (r = 0; r < 8u; r++) CHECK(!memcmp(ram + FXREC + r * 0x100u + 0x40u, kept[r], 0x40u));
        black_vr_fx_update_end();
        bvr.mode = VR_PRESENT_STEREO;
        fs.hand[1].valid = 0;
        frame();
        CHECK(!bvr_fxl.ok);
        black_vr_fx_update_begin();
        for (r = 0; r < 8u; r++) CHECK(!memcmp(ram + FXREC + r * 0x100u + 0x40u, kept[r], 0x40u));
        black_vr_fx_update_end();
        fs.hand[1].valid = 1;
        gcfg.vm_offset_m[0] = gcfg.vm_offset_m[1] = gcfg.vm_offset_m[2] = 0.0f;
        fs.hand[1].aim = P(0, 0, 0.2f, 1.3f, -0.3f);
        for (i = 0; i < 4; i++) frame();
    }
    /* ... with the player's aim matrix readable ([2D45D4]+100, the render's camera entity): the transform is built for
     * the update from the aim matrix as it is then (the camera entity the next render will have) and the hand this game
     * frame latched. A shot kicks the aim matrix up (the records posed on it) while the camera entity stays the last
     * frame's until later in the frame; relative to the hand of the next render, every record is then its pose on the
     * aim matrix, less the grip: the recoil is not in it, the animation is */
    {
        VrMat4 lrec[8], e0, e1, body, hand_next, hinv2, want, got;
        VrPose o;
        unsigned r, j, c;
        uint8_t kept2[8][0x40];
        aim_on = 1;
        gcfg.vm_offset_m[0] = 0.12f; gcfg.vm_offset_m[1] = -0.18f; gcfg.vm_offset_m[2] = 0.30f;
        fs.hand[1].aim = P(25, -10, 0.25f, 1.25f, -0.35f);
        for (i = 0; i < 4; i++) frame();
        CHECK(bvr.yv_ok && bvr.hand_locked && bvr.mode == VR_PRESENT_STEREO);
        for (r = 0; r < 8u; r++) {
            lrec[r] = vrm_m4_from_pose(r == 5 ? P(3, 2, 0.09f, -0.06f, 0.39f) : P(-8, 5, 0.02f, -0.12f, 0.20f));
            GUEST32(FXCNT + 0x38u + r * 4u) = (r == 2 || r == 5) ? 0u : 0xFFFFFFFFu;
        }
        /* the records as the game places them for the render: on the entity it is about to build the frame from */
        frame();
        for (r = 0; r < 8u; r++) {
            const VrMat4 m = vrm_m4_mul(bvr.w_entity, lrec[r]);
            const uint32_t at = FXREC + r * 0x100u + 0x40u;
            for (j = 0; j < 4; j++) {
                putf(at + j * 0x10u, VRM_AT(m, 0, j)); putf(at + j * 0x10u + 4u, VRM_AT(m, 1, j));
                putf(at + j * 0x10u + 8u, VRM_AT(m, 2, j)); GUEST32(at + j * 0x10u + 12u) = 0x6B6B0000u + r * 16u + j;
            }
        }
        frame();                                              /* the render: X, the records, the aim matrix */
        CHECK(bvr_fxl.ok && bvr_fxl.aim == FXCORE + 0x100u);
        {
            /* the shot's frame: the player stepped 5 cm right and the aim matrix kicked 8 deg up about the eye; the game
             * posed the records on it, record 5 with its animation 3 deg on, and record 3 appeared; the hand moved 4 cm
             * left and turned 5 deg since the render */
            const VrMat4 kick = vrm_m4_from_pose(P(0, 8, 0, 0, 0));
            e0 = bvr.w_entity;
            body = bvr.w_body;
            e1 = vrm_m4_mul(vrm_m4_from_pose(P(0, 0, 0.05f, 0, 0)), vrm_m4_mul(e0, kick));
            aim_from_view(e1);
            GUEST32(FXCNT + 0x38u + 3u * 4u) = 0u;
            lrec[3] = vrm_m4_from_pose(P(0, 0, -0.05f, -0.10f, 0.25f));
            for (r = 0; r < 8u; r++) {
                const VrMat4 m = vrm_m4_mul(e1, r == 5 ? vrm_m4_mul(lrec[5], vrm_m4_from_pose(P(0, 3, 0, 0, 0))) : lrec[r]);
                const uint32_t at = FXREC + r * 0x100u + 0x40u;
                for (j = 0; j < 4; j++) {
                    putf(at + j * 0x10u, VRM_AT(m, 0, j)); putf(at + j * 0x10u + 4u, VRM_AT(m, 1, j));
                    putf(at + j * 0x10u + 8u, VRM_AT(m, 2, j));
                }
                GUEST32(at + 12u) = 0x6B6B0000u + r * 16u;
                memcpy(kept2[r], ram + at, 0x40u);
            }
            fs.hand[1].aim = P(30, -10, 0.21f, 1.25f, -0.35f);
            black_vr_fx_update_begin();
            /* the next render's hand: this latch, on the body moved with the aim matrix */
            vr_get_recentre(&o);
            VRM_AT(body, 0, 3) += VRM_AT(e1, 0, 3) - VRM_AT(e0, 0, 3);
            VRM_AT(body, 1, 3) += VRM_AT(e1, 1, 3) - VRM_AT(e0, 1, 3);
            VRM_AT(body, 2, 3) += VRM_AT(e1, 2, 3) - VRM_AT(e0, 2, 3);
            hand_next = vrm_m4_mul(body, vgm_lh_from_pose(vgm_relative(o, fs.hand[1].aim), 1.0f));
            CHECK(vrm_m4_inverse(hand_next, &hinv2));
            for (r = 0; r < 8u; r++) {
                const uint32_t at = FXREC + r * 0x100u + 0x40u;
                if (r != 2 && r != 3 && r != 5) { CHECK(!memcmp(ram + at, kept2[r], 0x40u)); continue; }
                got = vrm_m4_identity();
                for (j = 0; j < 4; j++) for (c = 0; c < 3; c++) VRM_AT(got, c, j) = getf(at + j * 0x10u + c * 4u);
                got = vrm_m4_mul(hinv2, got);
                want = r == 5 ? vrm_m4_mul(lrec[5], vrm_m4_from_pose(P(0, 3, 0, 0, 0))) : lrec[r];
                VRM_AT(want, 0, 3) -= 0.12f; VRM_AT(want, 1, 3) += 0.18f; VRM_AT(want, 2, 3) -= 0.30f;
                for (j = 0; j < 4; j++) for (c = 0; c < 3; c++) CHECK(NEAR(VRM_AT(got, c, j), VRM_AT(want, c, j), 5e-5));
                CHECK(GUEST32(at + 12u) == 0x6B6B0000u + r * 16u);
            }
            /* e856ad7 (the last render's X on the update's record, plus its motion since) started the muzzle's effects
             * with the recoil in: over 5 cm off here */
            {
                float a[4], b[4], d[3];
                const uint32_t at = FXREC + 0x540u;
                a[0] = VRM_AT(bvr_fxl.l[5], 0, 3); a[1] = VRM_AT(bvr_fxl.l[5], 1, 3); a[2] = VRM_AT(bvr_fxl.l[5], 2, 3); a[3] = 1;
                vrm_m4_apply(bvr_fxl.x, a, b);
                for (c = 0; c < 3; c++) {
                    float now;
                    memcpy(&now, kept2[5] + 0x30u + c * 4u, 4);
                    d[c] = b[c] + (now - a[c]) - getf(at + 0x30u + c * 4u);
                }
                CHECK(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > 0.05f);
            }
            black_vr_fx_update_end();
            for (r = 0; r < 8u; r++) CHECK(!memcmp(ram + FXREC + r * 0x100u + 0x40u, kept2[r], 0x40u));
            /* the aim matrix unreadable: the last render's X on the game's record, moved with the hand */
            putf(FXCORE + 0x100u, NAN);
            black_vr_fx_update_begin();
            {
                const uint32_t at = FXREC + 0x540u;
                VrMat4 h0 = vrm_m4_mul(bvr.w_body, vgm_lh_from_pose(vgm_relative(o, fs.hand[1].aim), 1.0f)), hi, ei;
                CHECK(vrm_m4_inverse(h0, &hi) && vrm_m4_inverse(e0, &ei));
                got = vrm_m4_identity();
                for (j = 0; j < 4; j++) for (c = 0; c < 3; c++) VRM_AT(got, c, j) = getf(at + j * 0x10u + c * 4u);
                got = vrm_m4_mul(hi, got);
                want = vrm_m4_identity();
                for (j = 0; j < 4; j++) for (c = 0; c < 3; c++) { float v; memcpy(&v, kept2[5] + j * 16u + c * 4u, 4); VRM_AT(want, c, j) = v; }
                want = vrm_m4_mul(ei, want);
                VRM_AT(want, 0, 3) -= 0.12f; VRM_AT(want, 1, 3) += 0.18f; VRM_AT(want, 2, 3) -= 0.30f;
                for (j = 0; j < 4; j++) for (c = 0; c < 3; c++) CHECK(NEAR(VRM_AT(got, c, j), VRM_AT(want, c, j), 5e-5));
            }
            black_vr_fx_update_end();
            for (r = 0; r < 8u; r++) CHECK(!memcmp(ram + FXREC + r * 0x100u + 0x40u, kept2[r], 0x40u));
            GUEST32(FXCNT + 0x38u + 3u * 4u) = 0xFFFFFFFFu;
        }
        aim_on = 0;
        memset(ram + FXCORE + 0x100u, 0, 0x40u);
        gcfg.vm_offset_m[0] = gcfg.vm_offset_m[1] = gcfg.vm_offset_m[2] = 0.0f;
        fs.hand[1].aim = P(0, 0, 0.2f, 1.3f, -0.3f);
        for (i = 0; i < 4; i++) frame();
    }
    /* ... with the camera entity off the aim matrix (E = A * O: a melee lunge, a menu's camera; the self-check above
     * fails, as the sweep's harness run showed after the objectives screen and a melee): the shot's recoil still stays
     * out of the spawn, the camera's offset carried into the next render; the last render's X alone (the fallback
     * before) put the muzzle over 3 cm off */
    {
        VrMat4 l5, l5s, e0, a0, a1, e1, body, hand_next, hinv2, want, got, oinv;
        VrPose o;
        unsigned r, j, c;
        uint8_t kept3[8][0x40];
        const VrMat4 off = vrm_m4_from_pose(P(2, -3, 0.0f, -0.02f, 0.01f));
        CHECK(vrm_m4_inverse(off, &oinv));
        aim_oinv = oinv;
        aim_on = 2;
        gcfg.vm_offset_m[0] = 0.12f; gcfg.vm_offset_m[1] = -0.18f; gcfg.vm_offset_m[2] = 0.30f;
        fs.hand[1].aim = P(25, -10, 0.25f, 1.25f, -0.35f);
        for (i = 0; i < 4; i++) frame();
        CHECK(bvr.yv_ok && bvr.hand_locked && bvr.mode == VR_PRESENT_STEREO);
        l5 = vrm_m4_from_pose(P(3, 2, 0.09f, -0.06f, 0.39f));
        for (r = 0; r < 8u; r++) GUEST32(FXCNT + 0x38u + r * 4u) = r == 5 ? 0u : 0xFFFFFFFFu;
        frame();
        {
            /* the record as the game places it for the render: on the aim matrix */
            const VrMat4 m = vrm_m4_mul(vrm_m4_mul(bvr.w_entity, oinv), l5);
            const uint32_t at = FXREC + 0x540u;
            for (j = 0; j < 4; j++) {
                putf(at + j * 0x10u, VRM_AT(m, 0, j)); putf(at + j * 0x10u + 4u, VRM_AT(m, 1, j));
                putf(at + j * 0x10u + 8u, VRM_AT(m, 2, j)); GUEST32(at + j * 0x10u + 12u) = 0x7C7C0000u + j;
            }
        }
        frame();                                              /* the render: the aim matrix is not its camera */
        CHECK(bvr_fxl.ok && !bvr_fxl.aim && bvr_fxl.aim_off == FXCORE + 0x100u);
        {
            /* the shot's frame: the player stepped 5 cm right, the aim matrix kicked 8 deg up, record 5 animated 3 deg
             * on it; the hand moved 4 cm left and turned 5 deg */
            const VrMat4 kick = vrm_m4_from_pose(P(0, 8, 0, 0, 0)), anim = vrm_m4_from_pose(P(0, 3, 0, 0, 0));
            const uint32_t at = FXREC + 0x540u;
            float a[4], b[4], d;
            VrMat4 h0, hl_inv, x_old;
            e0 = bvr.w_entity;
            body = bvr.w_body;
            a0 = vrm_m4_mul(e0, oinv);
            a1 = vrm_m4_mul(vrm_m4_from_pose(P(0, 0, 0.05f, 0, 0)), vrm_m4_mul(a0, kick));
            e1 = vrm_m4_mul(a1, off);                         /* the camera the next render will have */
            aim_from_view(a1);
            l5s = vrm_m4_mul(l5, anim);
            {
                const VrMat4 m = vrm_m4_mul(a1, l5s);
                for (j = 0; j < 4; j++) {
                    putf(at + j * 0x10u, VRM_AT(m, 0, j)); putf(at + j * 0x10u + 4u, VRM_AT(m, 1, j));
                    putf(at + j * 0x10u + 8u, VRM_AT(m, 2, j));
                }
            }
            for (r = 0; r < 8u; r++) memcpy(kept3[r], ram + FXREC + r * 0x100u + 0x40u, 0x40u);
            fs.hand[1].aim = P(30, -10, 0.21f, 1.25f, -0.35f);
            black_vr_fx_update_begin();
            vr_get_recentre(&o);
            h0 = vrm_m4_mul(body, vgm_lh_from_pose(vgm_relative(o, fs.hand[1].aim), 1.0f));
            VRM_AT(body, 0, 3) += VRM_AT(e1, 0, 3) - VRM_AT(e0, 0, 3);
            VRM_AT(body, 1, 3) += VRM_AT(e1, 1, 3) - VRM_AT(e0, 1, 3);
            VRM_AT(body, 2, 3) += VRM_AT(e1, 2, 3) - VRM_AT(e0, 2, 3);
            hand_next = vrm_m4_mul(body, vgm_lh_from_pose(vgm_relative(o, fs.hand[1].aim), 1.0f));
            CHECK(vrm_m4_inverse(hand_next, &hinv2));
            got = vrm_m4_identity();
            for (j = 0; j < 4; j++) for (c = 0; c < 3; c++) VRM_AT(got, c, j) = getf(at + j * 0x10u + c * 4u);
            got = vrm_m4_mul(hinv2, got);
            /* relative to the next render's hand: the record's pose on that render's camera entity, less the grip */
            want = vrm_m4_mul(oinv, l5s);
            VRM_AT(want, 0, 3) -= 0.12f; VRM_AT(want, 1, 3) += 0.18f; VRM_AT(want, 2, 3) -= 0.30f;
            for (j = 0; j < 4; j++) for (c = 0; c < 3; c++) CHECK(NEAR(VRM_AT(got, c, j), VRM_AT(want, c, j), 5e-5));
            CHECK(GUEST32(at + 12u) == 0x7C7C0000u);
            /* the fallback before: the last render's X, moved with the hand, on the game's record */
            CHECK(vrm_m4_inverse(bvr_fxl.w_hand, &hl_inv));
            x_old = vrm_m4_mul(h0, vrm_m4_mul(hl_inv, bvr_fxl.x));
            a[0] = VRM_AT(a1, 0, 3); a[3] = 1.0f;
            {
                const VrMat4 g = vrm_m4_mul(a1, l5s), w = vrm_m4_mul(hand_next, want);
                a[0] = VRM_AT(g, 0, 3); a[1] = VRM_AT(g, 1, 3); a[2] = VRM_AT(g, 2, 3);
                vrm_m4_apply(x_old, a, b);
                d = vrm_len(vrm_sub(vrm_v3(b[0], b[1], b[2]), vgm_col(w, 3)));
                CHECK(d > 0.03f);
            }
            black_vr_fx_update_end();
            for (r = 0; r < 8u; r++) CHECK(!memcmp(ram + FXREC + r * 0x100u + 0x40u, kept3[r], 0x40u));
        }
        aim_on = 0;
        memset(ram + FXCORE + 0x100u, 0, 0x40u);
        gcfg.vm_offset_m[0] = gcfg.vm_offset_m[1] = gcfg.vm_offset_m[2] = 0.0f;
        fs.hand[1].aim = P(0, 0, 0.2f, 1.3f, -0.3f);
        for (i = 0; i < 4; i++) frame();
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
