/* BEGIN BLACK_VR
 * Quest 3 VR hooks over the lifted game (port/quest; notes in port/notes/xr-shell.md).
 * Built only with -DBLACK_VR and -I port/quest -I port/quest/src. Included by
 * recomp_manual.c after BLACK_SPLITSCREEN, so the world-camera, photo-mode and
 * first-person helpers above are in scope. Runs on the guest thread only.
 *
 * Model
 *   The game keeps its own aim: its look object's yaw/pitch (the look integrator
 *   sub_001BD3F0, ESI: yaw +18, pitch +1C, yaw cache +10, pitch clamp
 *   [2888C8]/[295E38]) is driven from the VR aim (right hand, or the head) plus a
 *   host-side body yaw B (snap/smooth turn). The world camera
 *   renderer[2D67F4]+D450 is still produced by D2F40 from that aim, and is then
 *   replaced (both RwCamera frame matrices, as photo mode does) by
 *   body frame * tracked head pose. The view's rotation therefore comes only
 *   from B and the head: camera shake, recoil kick and scripted roll in the game
 *   camera's rotation never reach the eyes; vertical head bob is low-passed.
 *   The lens window (+70/+74) is widened for D2F40 so the game frustum (culling,
 *   +48/+4C) covers both eyes; native aim zoom is cancelled by the same
 *   window (no FOV change in stereo); high zoom (scopes) switches to a flat,
 *   head-locked window with the game's own lens.
 *   The first-person weapon is drawn by the game where its camera entity
 *   (the aim) is. Its MVP uploads (16F610 -> c10..c13, flash D4A50 -> c112..c115,
 *   owner 1D25D0 / actor+2C8, as BLACK_VIEWMODEL_OWNER finds them) are re-targeted
 *   so the weapon sits in the aim hand.
 *   Gun latch (port/notes/gun-latch.md): the hand pose the weapon was placed at goes with the frame's camera
 *   (vr_publish_game_gun) and every re-targeted MVP is recorded with the frame's camera mark
 *   (vr_mark_viewmodel_mvp), so the renderer can move exactly those draws to the hand pose latched at display time.
 *
 * Presentation: STEREO only while the player world camera is bound to the
 * player's camera entity outside scripted mode (+16A4), the game is not paused
 * (black_pc_input_gameplay_blocked_query), the look integrator ran in the last
 * 250 ms, and neither photo mode nor split-screen is on. Scripted cameras show
 * as a flat world-locked screen (FLAT_CINEMA); pause, menus and movies as
 * FLAT_MENU.
 *
 * Self-checks (logged): the RenderWare frame's right row is the screen's left
 * (docs/photo-mode.md) -- verified from world draws' MVPs and flipped if wrong;
 * the game pitch grows downward (pc_input.h) -- verified against the camera
 * matrix and flipped if wrong; the depth row is taken from the MVPs.
 */
#include <time.h>
#include "vr_game_math.h"

#define BVR_RENDERER 0x002D67F4u
#define BVR_OWNER 0x002D12B8u
#define BVR_PITCH_UP_VA 0x002888C8u     /* +70: the game's pitch clamp */
#define BVR_PITCH_DOWN_VA 0x00295E38u   /* -70 */
#define BVR_VM_STACK 16

typedef struct BlackVr {
    int mode;                         /* VR_PRESENT_* chosen for this world camera, -1 = VR off */
    int world_pass;                   /* the D2F40 in progress produces the world camera */
    uint32_t lens_camera, saved70, saved74;
    int lens;
    /* aim */
    uint32_t look;                    /* look object (sub_001BD3F0 ESI) */
    double look_time;
    double body_yaw, aim_yaw_prev;    /* game degrees, positive = left */
    int have_body;
    double pitch_sign;                /* game pitch = -pitch_up * sign */
    int pitch_votes;
    VgmYawCal cal;
    VgmBob bob;
    double last_frame_time;
    /* this frame */
    VrMat4 w_entity, w_head, w_hand, yv, w_body;
    int yv_ok;
    VrVec3 grip;                      /* the weapon's grip offset (game units) */
    VrMat4 yv_fx;                     /* yv for the muzzle flash draw: with the arms' wrist lock */
    int yv_fx_ok;
    float grip_corr[3];               /* the auto-grip correction inside grip (game units) */
    int off_ok;
    float hx, hy;
    float depth_a, depth_b;
    int depth_ok, calib_this_frame;
    double p00, p02, p11, p12;        /* the world projection, factored out of world MVPs (bvr_learn) */
    int proj_ok, proj_fov_logged;
    float screen_sign;
    double clip_y_sign;
    int hand_votes, hand_locked;
    uint32_t game_frame;
    VrVec3 aim_d;                     /* diagnostics: the aim direction asked of the look (recentre space, OpenXR axes) */
    VrQuat head_q;                    /* ... and the head's turn from the recentre origin this frame */
} BlackVr;

static BlackVr bvr = { .mode = -1, .pitch_sign = 1.0, .screen_sign = VGM_SCREEN_RIGHT_SIGN, .clip_y_sign = 1.0 };
static uint8_t bvr_vm_stack[BVR_VM_STACK];
static int bvr_vm_depth;
static int black_vr_vm_draw;          /* the current 172EB0 draw is the player's first-person mesh */

#define BVR_FP_BEGIN fenv_t bvr_env_; const unsigned bvr_csr_ = _mm_getcsr(); fegetenv(&bvr_env_); \
    fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u)
#define BVR_FP_END fesetenv(&bvr_env_); _mm_setcsr(bvr_csr_)

static double black_vr_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int bvr_f32_ok(uint32_t va) { return (GUEST32(va) & 0x7FFFFFFFu) < 0x7F800000u; }
static float bvr_f32(uint32_t va) { float v; memcpy(&v, (const void *)((uintptr_t)g_xbox_mem_offset + va), 4); return v; }
static void bvr_put(uint32_t va, float v) { memcpy((void *)((uintptr_t)g_xbox_mem_offset + va), &v, 4); }

/* The world camera's RenderWare frame (modelling matrix at +10, LTM at +50), 0 if unreadable. */
static uint32_t bvr_frame(uint32_t camera)
{
    uint32_t rwcam, frame;
    if (!black_pc_ram_span(camera, 0x60u)) return 0;
    rwcam = GUEST32(camera + 0x58u);
    if (!black_pc_ram_span(rwcam, 8u)) return 0;
    frame = GUEST32(rwcam + 4u);
    return black_pc_ram_span(frame, 0x90u) ? frame : 0;
}

/* Native aim zoom, [[look+8C]+2A4], 1 unzoomed. */
static float bvr_zoom(void)
{
    uint32_t z;
    float f;
    if (!bvr.look || !black_pc_ram_span(bvr.look, 0x90u)) return 1.0f;
    z = GUEST32(bvr.look + 0x8Cu);
    if (!black_pc_ram_span(z, 0x2A8u) || !bvr_f32_ok(z + 0x2A4u)) return 1.0f;
    f = bvr_f32(z + 0x2A4u);
    return f >= 1.0f && f < 100.0f ? f : 1.0f;
}

static VrMat4 bvr_lh(VrPose p) { return vgm_lh_from_pose(p, vr_game_config()->units_per_metre); }

/* ---- first-person draw provenance (called from BLACK_VIEWMODEL_OWNER) ------------------ */

static void black_vr_vm_push(int is_vm)
{
    if (bvr_vm_depth < BVR_VM_STACK) bvr_vm_stack[bvr_vm_depth] = (uint8_t)(is_vm != 0);
    bvr_vm_depth++;
    black_vr_vm_draw = is_vm != 0;
}

static void black_vr_vm_pop(void)
{
    if (bvr_vm_depth > 0) bvr_vm_depth--;
    black_vr_vm_draw = bvr_vm_depth > 0 && bvr_vm_depth <= BVR_VM_STACK ? bvr_vm_stack[bvr_vm_depth - 1] : 0;
}

static int black_vr_stereo(void) { return bvr.mode == VR_PRESENT_STEREO; }

/* ---- presentation choice ---------------------------------------------------------------- */

/* -2: not the world camera (state untouched); -1: VR presentation inactive. */
static int black_vr_decide(uint32_t camera)
{
    uint32_t renderer;
    if (!black_pc_ram_span(BVR_RENDERER, 4u)) return -2;
    renderer = GUEST32(BVR_RENDERER);
    if (!black_pc_ram_span(renderer, 0xD4F0u) || camera != renderer + 0xD450u) return -2;
    if (!vr_active()) return -1;
    if (pc_photo_active() || pc_split_enabled() || black_split_pass) return VR_PRESENT_FLAT_MENU;
    if (!black_photo_world_camera()) return VR_PRESENT_FLAT_CINEMA;     /* scripted / not the player's view */
    if (black_pc_input_gameplay_blocked_query()) return VR_PRESENT_FLAT_MENU;
    if (!bvr.look || black_vr_now() - bvr.look_time > 0.25) return VR_PRESENT_FLAT_MENU;
    if (bvr_zoom() >= vr_game_config()->scope_zoom_min) return VR_PRESENT_FLAT_SCOPE;
    return VR_PRESENT_STEREO;
}

/* ---- before D2F40: lens window covering both eyes ----------------------------------------- */

static void black_vr_camera_begin(uint32_t camera)
{
    uint32_t owner;
    float angle, t, window, aspect;
    const VrFrameState *f;
    BVR_FP_BEGIN;
    bvr.lens = 0;
    {
        const int m = black_vr_decide(camera);
        bvr.world_pass = m != -2;
        if (m == -2) goto done;
        bvr.mode = m;
    }
    if (bvr.mode >= 0) vr_set_present_mode(bvr.mode);
    if (bvr.mode != VR_PRESENT_STEREO || !black_pc_ram_span(BVR_OWNER, 4u)) goto done;
    owner = GUEST32(BVR_OWNER);
    if (!black_pc_ram_span(owner, 0x754u) || !bvr_f32_ok(owner + 0x750u)) goto done;
    angle = bvr_f32(owner + 0x750u);
    if (!(angle > 1.0f && angle < 179.0f)) goto done;
    f = vr_frame_state();
    {
        VrPose eye[VR_EYE_COUNT];
        VrFov fov[VR_EYE_COUNT];
        int i;
        for (i = 0; i < VR_EYE_COUNT; i++) { eye[i] = f->eye[i].pose; fov[i] = f->eye[i].fov; }
        vgm_fov_union(f->head, eye, fov, vr_game_config()->fov_margin, &bvr.hx, &bvr.hy);
    }
    t = tanf(angle * 0.5f * (float)VRM_DEG);
    window = bvr.hx / t;
    aspect = bvr.hx / bvr.hy;
    if (!(window > 0.0f && window < 1e4f && aspect > 0.05f && aspect < 20.0f)) goto done;
    bvr.lens_camera = camera;
    bvr.saved70 = GUEST32(camera + 0x70u);
    bvr.saved74 = GUEST32(camera + 0x74u);
    bvr_put(camera + 0x70u, window);
    bvr_put(camera + 0x74u, aspect);
    bvr.lens = 1;
    /* the weapon goes to the hand at true perspective: no wide-FOV framing scale */
    black_vm_scale_bits = 0x3F800000u;
done:
    BVR_FP_END;
}

/* Right after D2F40's original: the lens fields go back before the other policies restore theirs. */
static void black_vr_camera_restore(uint32_t camera)
{
    if (!bvr.lens || camera != bvr.lens_camera) return;
    GUEST32(camera + 0x70u) = bvr.saved70;
    GUEST32(camera + 0x74u) = bvr.saved74;
    bvr.lens = 0;
}

/* ---- the look integrator: the game's aim follows the VR aim ------------------------------- */

static void black_vr_look(uint32_t look)
{
    const VrGameConfig *cfg;
    const VrFrameState *f;
    VrPose origin, head_rel, hand_rel;
    VrVec3 d;
    double ay, ap, g, turn, yaw, pitch, upper, lower;
    int hand_mode;
    if (!vr_active() || !black_pc_ram_span(look, 0xA8u)) return;
    {
        BVR_FP_BEGIN;
        bvr.look = look;
        bvr.look_time = black_vr_now();
        cfg = vr_game_config();
        f = vr_latch_frame_state();                     /* this game frame's head and hands */
        turn = vr_consume_turn();
        if (bvr.mode != VR_PRESENT_STEREO && bvr.mode != VR_PRESENT_FLAT_SCOPE) goto done;
        if (!bvr_f32_ok(look + 0x18u) || !bvr_f32_ok(BVR_PITCH_UP_VA) || !bvr_f32_ok(BVR_PITCH_DOWN_VA)) goto done;
        vr_get_recentre(&origin);
        head_rel = vgm_relative(origin, f->head);
        head_rel.position = vgm_clamp_len(head_rel.position, cfg->max_head_offset_m);
        hand_rel = vgm_relative(origin, f->hand[cfg->aim_hand & 1].aim);
        hand_mode = cfg->aim_mode == VR_AIM_HAND && f->hand[cfg->aim_hand & 1].valid;
        d = vgm_aim_dir(hand_mode, head_rel, hand_rel, vrm_v3(0, 0, 0), cfg->aim_converge_m);
        bvr.aim_d = d;
        vgm_dir_angles(d, &ay, &ap);
        g = bvr_f32(look + 0x18u);
        bvr.body_yaw = vgm_body_yaw_step(bvr.body_yaw, bvr.have_body, g, bvr.aim_yaw_prev, turn, 20.0);
        bvr.have_body = 1;
        bvr.aim_yaw_prev = ay;
        yaw = vrm_wrap_deg(bvr.body_yaw + ay);
        pitch = -ap * bvr.pitch_sign;
        upper = bvr_f32(BVR_PITCH_UP_VA);
        lower = bvr_f32(BVR_PITCH_DOWN_VA);
        if (lower < upper) { if (pitch > upper) pitch = upper; if (pitch < lower) pitch = lower; }
        bvr_put(look + 0x18u, (float)yaw);
        bvr_put(look + 0x10u, (float)yaw);
        bvr_put(look + 0x1Cu, (float)pitch);
        GUEST32(look + 0xA0u) = 0u;                     /* the pad's smoothing history (as mouse look) */
        GUEST32(look + 0xA4u) = 0u;
    done:
        BVR_FP_END;
    }
}

/* ---- the player's input update (1EC450, ECX = the input component): VR controls ------------------- *
 * Component (1EC450 self): +8C actor, +40 crouch (the game's own: toggled on a press edge when +102, the
 * profile's Toggle Crouch, is set, else held), +45 aim intent with toggle latch +104, +102 Toggle Crouch.
 * Actor +29C weapon, +2A4 zoom. Weapon +EC descriptor (+B4 first zoom, +B8 second, +C0 scoped: 1EC450 then
 * always toggles the aim and reads slot 35 as the scope's zoom level), +105 aim engaged (1BB110).
 * The VR layer resolves its actions through the live map (black_pc_input_resolver) and keeps crouch and scope
 * in step with these; the scope's aim intent is written here, before the native update (as BLACK_PC_AIM's Hold
 * mode seeds it): with the latch held the native toggle cannot flip it, and non-scoped weapons never aim. */
int black_pc_input_resolver(int slot);
static uint32_t bvr_input_serial;

static void black_vr_input(uint32_t comp)
{
    VrGameInputState st;
    uint32_t actor, weapon, desc;
    int request;
    static int logged_resolver;
    if (!vr_active() || !black_pc_ram_span(comp, 0x108u)) return;
    if (!logged_resolver) {
        logged_resolver = 1;
        vr_set_action_resolver(black_pc_input_resolver);
        fprintf(stderr, "[BLACK_VR] VR actions resolve through the live action map: fire %d reload %d crouch %d use %d "
                "weapon %d/%d grenade %d melee %d health %d suppressor %d fire mode %d pause %d back %d\n",
                black_pc_input_resolver(9), black_pc_input_resolver(11), black_pc_input_resolver(12),
                black_pc_input_resolver(13), black_pc_input_resolver(14), black_pc_input_resolver(15),
                black_pc_input_resolver(27), black_pc_input_resolver(32), black_pc_input_resolver(33),
                black_pc_input_resolver(31), black_pc_input_resolver(30), black_pc_input_resolver(25),
                black_pc_input_resolver(36));
    }
    actor = GUEST32(comp + 0x8Cu);
    /* player one only: the actor the look object (the VR aim's) belongs to */
    if (!bvr.look || !black_pc_ram_span(bvr.look, 0x90u) || GUEST32(bvr.look + 0x8Cu) != actor ||
        !black_pc_ram_span(actor, 0x2A8u))
        return;
    memset(&st, 0, sizeof(st));
    st.valid = 1;
    st.crouched = GUEST8(comp + 0x40u) != 0;
    st.crouch_toggle = GUEST8(comp + 0x102u) != 0;
    st.zoom = bvr_f32_ok(actor + 0x2A4u) ? bvr_f32(actor + 0x2A4u) : 1.0f;
    memcpy(st.intent, (const void *)((uintptr_t)g_xbox_mem_offset + comp + 0x40u), sizeof(st.intent));
    memcpy(st.latch, (const void *)((uintptr_t)g_xbox_mem_offset + comp + 0x100u), sizeof(st.latch));
    st.clip = -1;
    weapon = GUEST32(actor + 0x29Cu);
    if (black_pc_ram_span(actor, 0x2BCu)) { st.inventory[0] = GUEST8(actor + 0x2BAu); st.inventory[1] = GUEST8(actor + 0x2BBu); }
    if (black_pc_ram_span(weapon, 0x108u)) {
        st.weapon = weapon;
        st.aiming = GUEST8(weapon + 0x105u) != 0;
        st.weapon_state = GUEST32(weapon + 0xD8u);
        {
            const uint32_t mag = GUEST32(weapon + 0xF4u);
            if (black_pc_ram_span(mag, 0x1Au)) st.clip = *(const uint16_t *)((uintptr_t)g_xbox_mem_offset + mag + 0x18u);
            if (black_pc_ram_span(mag, 0x10u)) {
                const uint32_t prm = GUEST32(mag + 0xCu);
                if (black_pc_ram_span(prm, 0x30u)) {
                    st.max_spread = bvr_f32(prm + 8u);
                    st.spread_extra = bvr_f32(prm + 0x28u);
                    st.spread_time = bvr_f32(prm + 0x2Cu);
                }
            }
        }
        st.spread_scale = bvr_f32(weapon + 0xE4u);
        st.fire_time = bvr_f32(weapon + 0xE0u);
        desc = GUEST32(weapon + 0xECu);
        if (black_pc_ram_span(desc, 0xC8u)) {
            st.scoped = GUEST8(desc + 0xC0u) != 0;
            st.max_zoom = bvr_f32_ok(desc + 0xB4u) ? bvr_f32(desc + 0xB4u) : 1.0f;
        }
    }
    st.serial = ++bvr_input_serial;
    vr_publish_game_input(&st);
    request = vr_game_aim_request();
    if (request >= 0 && (bvr.mode == VR_PRESENT_STEREO || bvr.mode == VR_PRESENT_FLAT_SCOPE)) {
        GUEST8(comp + 0x45u) = (uint8_t)(request != 0);
        GUEST8(comp + 0x104u) = 1u;
    }
}

/* ---- after D2F40: the view becomes body * head ---------------------------------------------- */

static void bvr_publish(uint32_t camera, int valid, const VrMat4 *w_view, const VrPose *head_stage, int64_t t)
{
    VrGameCamera cam;
    VrGameCameraExt ext;
    VrMat4 view = vrm_m4_identity(), proj;
    uint32_t rwcam;
    memset(&cam, 0, sizeof(cam));
    memset(&ext, 0, sizeof(ext));
    cam.half_fov_x = bvr_f32_ok(camera + 0x48u) ? bvr_f32(camera + 0x48u) : 0.0f;
    cam.half_fov_y = bvr_f32_ok(camera + 0x4Cu) ? bvr_f32(camera + 0x4Cu) : 0.0f;
    if (w_view) vrm_m4_inverse(*w_view, &view);
    proj = vgm_game_proj(cam.half_fov_x > 0.0f ? cam.half_fov_x : 1.0f, cam.half_fov_y > 0.0f ? cam.half_fov_y : 1.0f,
                         bvr.depth_ok ? bvr.depth_a : 1.0f, bvr.depth_ok ? bvr.depth_b : -0.1f);
    /* Once the conventions are locked, publish the projection the title's world draws actually use (learned
     * from their MVPs), with its clip-y sign, rather than the one rebuilt from the half-frustum. */
    if (bvr.proj_ok && bvr.hand_locked) {
        memset(proj.m, 0, sizeof(proj.m));
        VRM_AT(proj,0,0) = (float)bvr.p00;                     VRM_AT(proj,0,2) = (float)bvr.p02;
        VRM_AT(proj,1,1) = (float)(bvr.clip_y_sign * bvr.p11); VRM_AT(proj,1,2) = (float)bvr.p12;
        VRM_AT(proj,2,2) = bvr.depth_a; VRM_AT(proj,2,3) = bvr.depth_b; VRM_AT(proj,3,2) = 1.0f;
        if (!bvr.proj_fov_logged && cam.half_fov_x > 0.0f && bvr.p00 > 0.0 &&
            fabs(1.0 / bvr.p00 - cam.half_fov_x) > 0.01 * cam.half_fov_x) {
            bvr.proj_fov_logged = 1;
            fprintf(stderr, "[BLACK_VR] learned projection 1/p00 %.6g differs from half_fov_x %.6g by more than 1%%\n",
                    1.0 / bvr.p00, (double)cam.half_fov_x);
        }
    }
    memcpy(cam.view, view.m, sizeof(cam.view));
    memcpy(cam.proj, proj.m, sizeof(cam.proj));
    cam.valid = valid;
    if (head_stage) ext.head_pose = *head_stage;
    else ext.head_pose = vrm_pose_identity();
    ext.pose_time = t;
    /* RwCamera near/far (+80/+84), trusted only when its view window (+68/+6C) is the half-frustum D2F40 just made */
    rwcam = black_pc_ram_span(camera, 0x60u) ? GUEST32(camera + 0x58u) : 0;
    if (black_pc_ram_span(rwcam, 0x88u) && GUEST32(rwcam + 0x68u) == GUEST32(camera + 0x48u) &&
        GUEST32(rwcam + 0x6Cu) == GUEST32(camera + 0x4Cu) && bvr_f32_ok(rwcam + 0x80u) && bvr_f32_ok(rwcam + 0x84u)) {
        const float n = bvr_f32(rwcam + 0x80u), fa = bvr_f32(rwcam + 0x84u);
        if (n > 0.0f && fa > n) { ext.near_z = n; ext.far_z = fa; }
    }
    if (bvr.depth_ok) { ext.depth_a = bvr.depth_a; ext.depth_b = bvr.depth_b; }
    else if (ext.near_z > 0.0f) vgm_depth_row(ext.near_z, ext.far_z, &ext.depth_a, &ext.depth_b);
    ext.game_frame = ++bvr.game_frame;
    ext.present_mode = bvr.mode;
    vr_publish_game_camera_ex(&cam, &ext);
}

static void black_vr_camera_after_world(uint32_t camera);
static void bvr_fx_capture(void);
static void bvr_fx_invalidate(void);
static void black_vr_camera_after(uint32_t camera)
{
    if (!bvr.world_pass) return;
    bvr.world_pass = 0;
    black_vr_camera_after_world(camera);
}

static void black_vr_camera_after_world(uint32_t camera)
{
    const VrGameConfig *cfg;
    const VrFrameState *f;
    uint32_t frame, row;
    float m[4][3];
    VrVec3 F0, S0, U0, P;
    VrPose origin, head_rel, hand_rel, head_stage;
    VrMat4 w_body;
    double now, dt;
    int k, hand_ok;
    if (bvr.mode < 0) return;
    {
        BVR_FP_BEGIN;
        bvr.yv_ok = 0;
        bvr.yv_fx_ok = 0;
        bvr.off_ok = 0;
        bvr.calib_this_frame = 0;
        bvr_fx_invalidate();
        frame = bvr_frame(camera);
        if (bvr.mode != VR_PRESENT_STEREO || !frame || !bvr.look || !bvr_f32_ok(bvr.look + 0x18u)) {
            if (frame) {
                for (row = 0; row < 4u; row++)
                    for (k = 0; k < 3; k++) m[row][k] = bvr_f32(frame + 0x10u + row * 0x10u + (uint32_t)k * 4u);
                bvr.w_entity = vgm_frame_to_view(m, bvr.screen_sign);
                bvr_publish(camera, 0, &bvr.w_entity, NULL, 0);
            } else {
                bvr_publish(camera, 0, NULL, NULL, 0);
            }
            if (bvr.mode != VR_PRESENT_FLAT_SCOPE) vgm_yaw_cal_reset(&bvr.cal);
            goto done;
        }
        cfg = vr_game_config();
        f = vr_frame_state();
        now = black_vr_now();
        dt = bvr.last_frame_time > 0.0 ? now - bvr.last_frame_time : 1.0 / 60.0;
        bvr.last_frame_time = now;
        for (row = 0; row < 4u; row++)
            for (k = 0; k < 3; k++) {
                const uint32_t va = frame + 0x10u + row * 0x10u + (uint32_t)k * 4u;
                if (!bvr_f32_ok(va)) goto done;
                m[row][k] = bvr_f32(va);
            }
        bvr.w_entity = vgm_frame_to_view(m, bvr.screen_sign);
        S0 = vgm_col(bvr.w_entity, 0); U0 = vgm_col(bvr.w_entity, 1);
        F0 = vgm_col(bvr.w_entity, 2); P = vgm_col(bvr.w_entity, 3);
        if (!vrm_normalize(&F0) || !vrm_normalize(&S0)) goto done;

        /* body yaw in the world: the game's yaw degrees -> world yaw, learned and low-passed */
        {
            const double g = bvr_f32(bvr.look + 0x18u);
            vgm_yaw_cal_update(&bvr.cal, vgm_world_yaw(vrm_v3(F0.x, 0.0f, F0.z)), g, vgm_yaw_sign(F0, S0), dt);
        }
        /* pitch convention check: positive game pitch should look down */
        if (bvr_f32_ok(bvr.look + 0x1Cu)) {
            const float p = bvr_f32(bvr.look + 0x1Cu), up = F0.y * (U0.y >= 0.0f ? 1.0f : -1.0f);
            if (fabsf(p) > 10.0f && fabsf(up) > 0.17f) {
                bvr.pitch_votes += ((p > 0.0f) == (up < 0.0f)) ? 1 : -1;
                if (bvr.pitch_votes > 64) bvr.pitch_votes = 64;
                if (bvr.pitch_votes < -24) {
                    bvr.pitch_sign = -bvr.pitch_sign;
                    bvr.pitch_votes = 0;
                    fprintf(stderr, "[BLACK_VR] game pitch grows upward here: pitch sign flipped\n");
                }
            }
        }
        if (cfg->bob_filter && U0.y > 0.5f) P.y = vgm_bob_filter(&bvr.bob, P.y, dt, 0.2, 0.6f * cfg->units_per_metre);
        w_body = vgm_body_frame(vgm_yaw_cal_world(&bvr.cal, bvr.body_yaw), P, F0, S0, U0);
        bvr.w_body = w_body;

        vr_get_recentre(&origin);
        head_rel = vgm_relative(origin, f->head);
        head_rel.position = vgm_clamp_len(head_rel.position, cfg->max_head_offset_m);
        head_stage = vrm_pose_mul(origin, head_rel);
        bvr.head_q = head_rel.orientation;
        bvr.w_head = vrm_m4_mul(w_body, bvr_lh(head_rel));
        hand_ok = f->hand[cfg->aim_hand & 1].valid;
        hand_rel = vgm_relative(origin, f->hand[cfg->aim_hand & 1].aim);
        bvr.w_hand = vrm_m4_mul(w_body, bvr_lh(hand_rel));
        if (cfg->hand_weapon && cfg->aim_mode == VR_AIM_HAND && hand_ok) {
            float corr[3];
            VrVec3 grip;
            vr_grip_correction(corr);                 /* first-person arms: the measured auto-grip (game units) */
            memcpy(bvr.grip_corr, corr, sizeof(bvr.grip_corr));
            grip = vrm_add(vrm_scale(vrm_v3(cfg->vm_offset_m[0], cfg->vm_offset_m[1], cfg->vm_offset_m[2]),
                                     cfg->units_per_metre), vrm_v3(corr[0], corr[1], corr[2]));
            bvr.yv_ok = vgm_weapon_to_hand(bvr.w_head, bvr.w_entity, bvr.w_hand, grip, &bvr.yv);
            bvr.grip = grip;
            bvr.off_ok = bvr.yv_ok;               /* first-person arms: the hand frame is published with the MVP */
            {
                /* the muzzle flash draw (its own MVP, not skinned with the weapon): where the arms' per-draw wrist
                 * lock put the drawn gun last render (grip - lock), so the flash sits at the drawn muzzle */
                float lock[3];
                vr_grip_lock(lock);
                bvr.yv_fx_ok = bvr.yv_ok && vgm_weapon_to_hand(bvr.w_head, bvr.w_entity, bvr.w_hand,
                                                               vrm_sub(grip, vrm_v3(lock[0], lock[1], lock[2])), &bvr.yv_fx);
            }
        }
        bvr_fx_capture();                     /* the held effects' next update starts from this */

        /* both RwCamera frame matrices, as photo mode replaces them */
        vgm_view_to_frame(bvr.w_head, bvr.screen_sign, m);
        for (row = 0; row < 4u; row++)
            for (k = 0; k < 3; k++) {
                bvr_put(frame + 0x10u + row * 0x10u + (uint32_t)k * 4u, m[row][k]);
                bvr_put(frame + 0x50u + row * 0x10u + (uint32_t)k * 4u, m[row][k]);
            }
        {
            /* gun latch: the hand this frame's weapon is attached to (the renderer moves it to the late-latched one) */
            VrGameGun gun;
            memset(&gun, 0, sizeof(gun));
            gun.hand = f->hand[cfg->aim_hand & 1].aim;
            gun.hand_index = cfg->aim_hand & 1;
            gun.valid = bvr.yv_ok && bvr.hand_locked;
            vr_publish_game_gun(&gun);
        }
        bvr_publish(camera, 1, &bvr.w_head, &head_stage, f->predicted_display_time);
    done:
        BVR_FP_END;
    }
}

/* ---- every MVP upload (16F610 c10..c13, flash D4A50 c112..c115) --------------------------- */

/* World draws (not the weapon) teach three things, from draws whose model rotation is identity
 * (their view rows are exactly the head frame we wrote): the depth row (a, b), whether the
 * RenderWare right row is the screen's left (screen_sign), and the clip-space y direction. */
static void bvr_learn(const float r[16])
{
    VgmClipCal c;
    VrVec3 x, y, z;
    const VrVec3 S = vgm_col(bvr.w_head, 0), U = vgm_col(bvr.w_head, 1), A = vgm_col(bvr.w_head, 2);
    VrVec3 Sn = S, Un = U, An = A;
    if (!vgm_clip_calibrate(r, &c) || !(c.a > 0.0) || !(c.b < 0.0)) return;
    if (!vgm_mvp_axes(r, &x, &y, &z) || !vrm_normalize(&Sn) || !vrm_normalize(&Un) || !vrm_normalize(&An)) return;
    if (vrm_dot(z, An) < 0.9999f) return;                         /* a rotated model: no evidence */
    bvr.depth_a = (float)c.a;
    bvr.depth_b = (float)c.b;
    bvr.depth_ok = 1;
    bvr.p00 = c.p00; bvr.p02 = c.p02; bvr.p11 = c.p11; bvr.p12 = c.p12; bvr.proj_ok = 1;
    bvr.clip_y_sign = vrm_dot(y, Un) >= 0.0f ? 1.0 : -1.0;
    if (bvr.hand_locked) return;
    {
        const float sx = vrm_dot(x, Sn);
        if (sx > 0.99f) bvr.hand_votes++;
        else if (sx < -0.99f) bvr.hand_votes--;
        if (bvr.hand_votes >= 32) {
            bvr.hand_locked = 1;
            fprintf(stderr, "[BLACK_VR] screen-right convention confirmed (sign %+.0f), clip y %+.0f, depth %.6g z %+.6g\n",
                    bvr.screen_sign, bvr.clip_y_sign, bvr.depth_a, bvr.depth_b);
        } else if (bvr.hand_votes <= -8) {
            bvr.screen_sign = -bvr.screen_sign;
            bvr.hand_votes = 0;
            vgm_yaw_cal_reset(&bvr.cal);
            fprintf(stderr, "[BLACK_VR] screen-right convention flipped (sign now %+.0f)\n", bvr.screen_sign);
        }
    }
}

/* BLACKVR_AIM_TRACE=1 (diagnostic): at each muzzle-flash upload, the flash's model frame (origin and axes, from P^-1 *
 * MVP) before and after the re-target, the game camera entity (the game's aim: origin and forward) and the aim hand, all
 * in the head view (x right, y up, z forward; game units). */
static int bvr_aim_trace(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("BLACKVR_AIM_TRACE"); on = e && *e ? atoi(e) : 0; if (on < 0) on = 0; }
    return on;
}
static int bvr_view_of(const float r[16], double vw[3][4])
{
    VgmClipCal c;
    int j;
    if (!vgm_clip_calibrate(r, &c)) return 0;
    if (bvr.clip_y_sign < 0.0) c.p11 = -c.p11;
    for (j = 0; j < 4; j++) {
        vw[0][j] = (r[j] - c.p02 * r[12 + j]) / c.p00;
        vw[1][j] = (r[4 + j] - c.p12 * r[12 + j]) / c.p11;
        vw[2][j] = r[12 + j];
    }
    return 1;
}
static void bvr_aim_log(const float before[16], const float after[16])
{
    double b[3][4], a[3][4];
    VrMat4 hv, e, h;
    VrVec3 eo, ef, ho, hf;
    if (!bvr_view_of(before, b) || !bvr_view_of(after, a) || !vrm_m4_inverse(bvr.w_head, &hv)) return;
    e = vrm_m4_mul(hv, bvr.w_entity);
    h = vrm_m4_mul(hv, bvr.w_hand);
    eo = vgm_col(e, 3); ef = vgm_col(e, 2); ho = vgm_col(h, 3); hf = vgm_col(h, 2);
    {
        /* the aim the look was given (vgm_aim_dir), in the head view (LH: z forward) */
        const VrVec3 d = vrm_q_rotate(vrm_q_conj(bvr.head_q), bvr.aim_d);
        fprintf(stderr, "[BLACK_VR] AIM gf %u asked %.5f %.5f %.5f\n", bvr.game_frame, (double)d.x, (double)d.y, (double)-d.z);
    }
    {
        /* the shot's aim points: 1DBD50 fires from the actor's eye matrix toward weapon+10 (with spread) or, at the
         * descriptor's +A8 odds, exactly toward weapon+20 (1CEF40); weapon = actor+29C */
        const uint32_t actor = black_pc_ram_span(0x2D45D4u, 4u) ? GUEST32(0x2D45D4u) + 0x30u : 0u;
        const uint32_t weapon = actor && black_pc_ram_span(actor, 0x2A0u) ? GUEST32(actor + 0x29Cu) : 0u;
        if (black_pc_ram_span(weapon, 0x30u)) {
            float p[4], q[4], r[4];
            int k;
            for (k = 0; k < 3; k++) p[k] = bvr_f32(weapon + 0x10u + (uint32_t)k * 4u);
            p[3] = 1.0f;
            vrm_m4_apply(hv, p, q);
            for (k = 0; k < 3; k++) p[k] = bvr_f32(weapon + 0x20u + (uint32_t)k * 4u);
            vrm_m4_apply(hv, p, r);
            fprintf(stderr, "[BLACK_VR] AIM gf %u aim points (head view) w+10 %.4f %.4f %.4f w+20 %.4f %.4f %.4f\n",
                    bvr.game_frame, (double)q[0], (double)q[1], (double)q[2], (double)r[0], (double)r[1], (double)r[2]);
        }
    }
    fprintf(stderr, "[BLACK_VR] AIM gf %u flash before o %.4f %.4f %.4f z %.4f %.4f %.4f | after o %.4f %.4f %.4f "
            "x %.4f %.4f %.4f y %.4f %.4f %.4f z %.4f %.4f %.4f | entity o %.4f %.4f %.4f f %.5f %.5f %.5f | "
            "hand o %.4f %.4f %.4f f %.5f %.5f %.5f\n", bvr.game_frame,
            b[0][3], b[1][3], b[2][3], b[0][2], b[1][2], b[2][2],
            a[0][3], a[1][3], a[2][3], a[0][0], a[1][0], a[2][0], a[0][1], a[1][1], a[2][1], a[0][2], a[1][2], a[2][2],
            (double)eo.x, (double)eo.y, (double)eo.z, (double)ef.x, (double)ef.y, (double)ef.z,
            (double)ho.x, (double)ho.y, (double)ho.z, (double)hf.x, (double)hf.y, (double)hf.z);
}

/* ---- the first-person weapon's held effects follow it to the hand ----------------------------------------------------
 * The effects the characters hold ([2D1990]+65690, recomp_manual.c BLACK_FX_AT): 0x50 slots of 0x70 bytes (+0..+1C
 * its emitters, +20 the matrix it follows), live ones listed at +3440 (u16, +3448 of them), owner +2300, where on it
 * +2440 (0..7 a record of the owner's first-person model, 8 the owner itself). A shot (1DBD50 -> 169960) puts one on
 * the player's muzzle record (5): the muzzle's glow and smoke. Their particles are spawned in the world where the
 * held effect's update (176BD0) finds the record -- [[actor+328]+30+where*4]+40, a RenderWare matrix -- and drawn as
 * screen-space sprites (the glow into the glare chain's 160x120 source image, blurred and added over the view). The
 * record is where the flat game holds its weapon, so with the weapon in the hand they made a soft orange glow floating
 * where the flat gun would be (port/notes/gun-latch.md).
 * Every gun's shot does this (pistol, UZI, shotgun alike); the shotgun has no flash draw of its own (its flash entry's
 * texture is 0, so 16B1E0 skips it), its fireball is this held effect.
 * So around the held effects' update (sub_00176BD0 in recomp_manual.c) the player's records 0..7 are moved by the
 * transform the next render will give the weapon (world X = w_head * yv * w_head^-1 = w_hand * grip^-1 *
 * w_entity^-1), built for the update (black_vr_fx_update_begin), and put back afterwards: the effects that follow them
 * start at the hand-held muzzle and leave along the hand-held barrel. The records themselves are left to the game
 * (the flash, 16B1E0, reads record 5 at the render and is re-targeted with its MVP). BLACKVR_GUN_FX=0: the effects
 * follow the game's records as before. */
#define BVR_FX_AT 0x65690u
#define BVR_FX_SLOTS 0x50u
static uint32_t bvr_fx(void)
{
    uint32_t fx;
    if (!black_pc_ram_span(0x002D1990u, 4u)) return 0;
    fx = GUEST32(0x002D1990u) + BVR_FX_AT;
    if (!black_pc_ram_span(fx, 0x3460u) || !black_pc_ram_span(GUEST32(fx + 0x3440u), BVR_FX_SLOTS * 2u) ||
        *(uint16_t *)((uintptr_t)g_xbox_mem_offset + fx + 0x3448u) > BVR_FX_SLOTS)
        return 0;
    return fx;
}
static int bvr_gun_fx(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("BLACKVR_GUN_FX"); on = !e || *e != '0'; }
    return on;
}

/* The player's first-person model record r's matrix, as 176BD0 finds it (0 when the record is absent). */
static uint32_t bvr_fp_record(unsigned r)
{
    uint32_t core, model, cnt, rec;
    if (r > 7u || !black_pc_ram_span(0x2D45D4u, 4u)) return 0;
    core = GUEST32(0x2D45D4u);
    if (!black_pc_ram_span(core + 0x358u, 4u)) return 0;
    model = GUEST32(core + 0x358u);
    if (!black_pc_ram_span(model, 0x54u)) return 0;
    cnt = GUEST32(model + 0x50u);
    if (!black_pc_ram_span(cnt + 0x38u + r * 4u, 4u) || (int32_t)GUEST32(cnt + 0x38u + r * 4u) < 0) return 0;
    rec = GUEST32(model + 0x30u + r * 4u);
    return black_pc_ram_span(rec + 0x40u, 0x40u) ? rec + 0x40u : 0;
}
/* A RenderWare matrix (rows right, up, at, pos; 4th words left alone) <-> VrMat4 (those rows as columns). */
static int bvr_rw_read(uint32_t va, VrMat4 *m)
{
    int j, k;
    *m = vrm_m4_identity();
    for (j = 0; j < 4; j++)
        for (k = 0; k < 3; k++) {
            const uint32_t at = va + (uint32_t)j * 0x10u + (uint32_t)k * 4u;
            if (!bvr_f32_ok(at)) return 0;
            VRM_AT(*m, k, j) = bvr_f32(at);
        }
    return 1;
}
static void bvr_rw_write(uint32_t va, const VrMat4 *m)
{
    int j, k;
    for (j = 0; j < 4; j++)
        for (k = 0; k < 3; k++) bvr_put(va + (uint32_t)j * 0x10u + (uint32_t)k * 4u, VRM_AT(*m, k, j));
}

/* The player's aim matrix, actor+D0 ([2D45D4]+100; RenderWare rows right, up, at, pos): the game's update poses the
 * first-person model on it, the camera entity copies it later in the frame (so at the held effects' update the camera
 * entity is still the last frame's: with a shot's recoil, core+4F4 degrees of pitch, 8.1 for the shotgun, the two
 * differ by the recoil's change). As a view (the frame's screen sign), as w_entity is read; 0 if unreadable. */
static uint32_t bvr_aim_va(void)
{
    uint32_t core;
    if (!black_pc_ram_span(0x2D45D4u, 4u)) return 0;
    core = GUEST32(0x2D45D4u);
    return black_pc_ram_span(core + 0x100u, 0x40u) ? core + 0x100u : 0;
}
static int bvr_aim_view(uint32_t va, VrMat4 *w)
{
    float rows[4][3];
    int r, k;
    for (r = 0; r < 4; r++)
        for (k = 0; k < 3; k++) {
            const uint32_t at = va + (uint32_t)r * 0x10u + (uint32_t)k * 4u;
            if (!bvr_f32_ok(at)) return 0;
            rows[r][k] = bvr_f32(at);
        }
    *w = vgm_frame_to_view(rows, bvr.screen_sign);
    return 1;
}
static int bvr_m4_near(VrMat4 a, VrMat4 b, float rot, float pos)
{
    int r, c;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 3; r++)
            if (!(fabsf(VRM_AT(a, r, c) - VRM_AT(b, r, c)) <= (c == 3 ? pos : rot))) return 0;
    return 1;
}

static struct {
    int ok;
    uint32_t frame, aim;                      /* aim: the player's aim matrix, when it was this render's camera entity */
    VrMat4 x, back, w_body, w_hand, w_entity;
    uint32_t at[8];
    VrMat4 l[8];
} bvr_fxl;                                    /* the last render's */
static struct { int ok, fresh; uint32_t frame, at; VrVec3 p, p_old; } bvr_fxd;   /* trace: the last update's muzzle */
static uint32_t bvr_fxu_at[8];
static uint8_t bvr_fxu_kept[8][0x40];
static unsigned bvr_fxu_n;
static uint32_t bvr_fxu_updates;

static void bvr_fx_invalidate(void) { bvr_fxl.ok = 0; }
/* camera_after_world, once the weapon's re-target for this render is known (yv_ok) */
static void bvr_fx_capture(void)
{
    const VrGameConfig *cfg = vr_game_config();
    VrMat4 hinv, a;
    unsigned r;
    uint32_t va;
    bvr_fxl.ok = 0;
    if (!bvr.yv_ok || !bvr.hand_locked || !vrm_m4_inverse(bvr.w_head, &hinv)) { bvr_fxd.ok = 0; return; }
    bvr_fxl.x = vrm_m4_mul(bvr.w_head, vrm_m4_mul(bvr.yv, hinv));
    bvr_fxl.back = vrm_m4_identity();                 /* the grip, as vgm_weapon_to_hand takes it off */
    {
        /* the grip this render used (vm_offset + the first-person arms' auto-grip baseline), less the arms' per-draw
         * wrist lock (last render's), so the effects start where the drawn gun's muzzle is (port/notes/arms.md) */
        float lock[3];
        vr_grip_lock(lock);
        VRM_AT(bvr_fxl.back, 0, 3) = -(bvr.grip.x - lock[0]);
        VRM_AT(bvr_fxl.back, 1, 3) = -(bvr.grip.y - lock[1]);
        VRM_AT(bvr_fxl.back, 2, 3) = -(bvr.grip.z - lock[2]);
        (void)cfg;
    }
    bvr_fxl.w_body = bvr.w_body;
    bvr_fxl.w_hand = bvr.w_hand;
    bvr_fxl.w_entity = bvr.w_entity;
    va = bvr_aim_va();
    bvr_fxl.aim = va && bvr_aim_view(va, &a) && bvr_m4_near(a, bvr.w_entity, 2e-3f, 2e-3f * cfg->units_per_metre) ? va : 0;
    {
        static int logged;
        if (!logged && !bvr_fxl.aim) {
            logged = 1;
            fprintf(stderr, "[BLACK_VR] gun fx: the player's aim matrix is not this render's camera (logged once): the held "
                    "effects use the last render's transform\n");
        }
    }
    for (r = 0; r < 8u; r++)
        if ((bvr_fxl.at[r] = bvr_fp_record(r)) != 0 && !bvr_rw_read(bvr_fxl.at[r], &bvr_fxl.l[r])) bvr_fxl.at[r] = 0;
    bvr_fxl.frame = bvr.game_frame;
    bvr_fxl.ok = 1;
    /* trace: where the last update started the muzzle's effects vs where this render puts the muzzle in the hand (the
     * late latch's own move at display aside), and where e856ad7 started them */
    if (bvr_aim_trace() && bvr_fxd.ok && bvr_fxl.at[5] && bvr_fxd.at == bvr_fxl.at[5]) {
        float p4[4], b[4];
        VrVec3 t;
        p4[0] = VRM_AT(bvr_fxl.l[5], 0, 3); p4[1] = VRM_AT(bvr_fxl.l[5], 1, 3); p4[2] = VRM_AT(bvr_fxl.l[5], 2, 3); p4[3] = 1.0f;
        vrm_m4_apply(bvr_fxl.x, p4, b);
        t = vrm_v3(b[0], b[1], b[2]);
        fprintf(stderr, "[BLACK_VR] FXC gf %u muzzle effects started at update after gf %u: %.4f from the hand-held muzzle "
                "(e856ad7: %.4f)%s\n", bvr.game_frame, bvr_fxd.frame,
                (double)vrm_len(vrm_sub(bvr_fxd.p, t)), (double)vrm_len(vrm_sub(bvr_fxd.p_old, t)),
                bvr_fxd.fresh ? "" : ", no aim matrix");
    }
    bvr_fxd.ok = 0;
}

/* Before the held effects' update (176BD0; VR STEREO, the weapon in the hand at the last render): the player's
 * first-person records to the hand, by the transform the next render will give the weapon (X = w_hand * grip^-1 *
 * w_entity^-1), built now: the camera entity it will have is the player's aim matrix now (the records are posed on it;
 * the camera entity itself still has the last frame's), the hand is this game frame's latch, on the last render's
 * body moved as the aim matrix moved. Without the aim matrix: the last render's X, carried by the hand's move. */
static void black_vr_fx_update_begin(void)
{
    unsigned r, j;
    bvr_fxu_n = 0;
    if (!bvr_gun_fx() || bvr.mode != VR_PRESENT_STEREO || !bvr_fxl.ok) {
        if (bvr_aim_trace() && bvr.mode == VR_PRESENT_STEREO)
            fprintf(stderr, "[BLACK_VR] FXU gf %u skipped (on %d, render captured %d, yv_ok %d, hand_locked %d)\n",
                    bvr.game_frame, bvr_gun_fx(), bvr_fxl.ok, bvr.yv_ok, bvr.hand_locked);
        return;
    }
    {
        BVR_FP_BEGIN;
        const VrGameConfig *cfg = vr_game_config();
        const VrFrameState *f = vr_frame_state();
        VrMat4 h_now = bvr_fxl.w_hand, x = bvr_fxl.x, hinv, m_now, minv;
        VrVec3 dp = vrm_v3(0, 0, 0);
        int fresh = 0;
        if (f && f->hand[cfg->aim_hand & 1].valid) {
            VrPose origin;
            VrMat4 h;
            vr_get_recentre(&origin);
            h = vrm_m4_mul(bvr_fxl.w_body, bvr_lh(vgm_relative(origin, f->hand[cfg->aim_hand & 1].aim)));
            if (vrm_len(vrm_sub(vgm_col(h, 3), vgm_col(bvr_fxl.w_hand, 3))) <= 0.30f * cfg->units_per_metre)
                h_now = h;                                                   /* a recentre or a glitch: none */
        }
        if (bvr_fxl.aim && bvr_aim_va() == bvr_fxl.aim && bvr_aim_view(bvr_fxl.aim, &m_now) && vrm_m4_inverse(m_now, &minv)) {
            dp = vrm_sub(vgm_col(m_now, 3), vgm_col(bvr_fxl.w_entity, 3));
            if (vrm_len(dp) <= 1.0f * cfg->units_per_metre) {                /* a teleport: as without it */
                VRM_AT(h_now, 0, 3) += dp.x; VRM_AT(h_now, 1, 3) += dp.y; VRM_AT(h_now, 2, 3) += dp.z;
                x = vrm_m4_mul(h_now, vrm_m4_mul(bvr_fxl.back, minv));
                fresh = 1;
            } else {
                dp = vrm_v3(0, 0, 0);
            }
        }
        if (!fresh && vrm_m4_inverse(bvr_fxl.w_hand, &hinv)) x = vrm_m4_mul(h_now, vrm_m4_mul(hinv, bvr_fxl.x));
        for (r = 0; r < 8u; r++) {
            const uint32_t at = bvr_fp_record(r);
            VrMat4 now, m;
            int dup = 0;
            if (!at || !bvr_rw_read(at, &now)) continue;
            for (j = 0; j < bvr_fxu_n; j++) if (bvr_fxu_at[j] == at) dup = 1;
            if (dup) continue;
            m = vrm_m4_mul(x, now);
            if (r == 5u && bvr_aim_trace()) {
                /* trace: what e856ad7 did (the last render's X on the game's record, its motion since as a translation) */
                const VrVec3 p0 = vgm_col(bvr_fxl.l[r], 3), p1 = vgm_col(now, 3);
                float a[4], b[4];
                a[0] = p0.x; a[1] = p0.y; a[2] = p0.z; a[3] = 1.0f;
                vrm_m4_apply(bvr_fxl.x, a, b);
                bvr_fxd.ok = bvr_fxl.at[r] == at; bvr_fxd.fresh = fresh; bvr_fxd.frame = bvr_fxl.frame; bvr_fxd.at = at;
                bvr_fxd.p = vgm_col(m, 3);
                bvr_fxd.p_old = vrm_v3(b[0] + (p1.x - p0.x), b[1] + (p1.y - p0.y), b[2] + (p1.z - p0.z));
            }
            bvr_fxu_at[bvr_fxu_n] = at;
            memcpy(bvr_fxu_kept[bvr_fxu_n], (uint8_t *)(uintptr_t)g_xbox_mem_offset + at, 0x40u);
            bvr_fxu_n++;
            bvr_rw_write(at, &m);
        }
        if (bvr_aim_trace())
            fprintf(stderr, "[BLACK_VR] FXU gf %u moved %u records (render gf %u) hand move %.4f %.4f %.4f (body step %.4f %.4f "
                    "%.4f), recoil %.3f deg%s\n", bvr.game_frame, bvr_fxu_n, bvr_fxl.frame,
                    (double)(VRM_AT(h_now, 0, 3) - VRM_AT(bvr_fxl.w_hand, 0, 3)),
                    (double)(VRM_AT(h_now, 1, 3) - VRM_AT(bvr_fxl.w_hand, 1, 3)),
                    (double)(VRM_AT(h_now, 2, 3) - VRM_AT(bvr_fxl.w_hand, 2, 3)), (double)dp.x, (double)dp.y, (double)dp.z,
                    bvr_fxl.aim && black_pc_ram_span(bvr_fxl.aim + 0x3F4u, 4u) && bvr_f32_ok(bvr_fxl.aim + 0x3F4u)
                        ? (double)bvr_f32(bvr_fxl.aim + 0x3F4u) : 0.0,
                    fresh ? "" : ", no aim matrix");
        if (bvr_fxu_n && (++bvr_fxu_updates == 1u || !(bvr_fxu_updates & 4095u)))
            fprintf(stderr, "[BLACK_VR] gun fx: the weapon's held effects follow it to the hand (%u updates%s)\n",
                    bvr_fxu_updates, fresh ? "" : ", without the aim matrix");
        BVR_FP_END;
    }
}

/* After it: the records back as the game left them. */
static void black_vr_fx_update_end(void)
{
    unsigned k;
    for (k = 0; k < bvr_fxu_n; k++)
        memcpy((uint8_t *)(uintptr_t)g_xbox_mem_offset + bvr_fxu_at[k], bvr_fxu_kept[k], 0x40u);
    bvr_fxu_n = 0;
}

/* ---- harness only: RECOMP_BLACK_TEST_LOADOUT=<gun> (smg, shg, pst, asr, ...) ------------------------------------------
 * At the player's loadout initialization (1B7800, the black_test_weapon_loadout hook) the primary weapon token becomes
 * BG1_<GUN>: the gun table's tokens are 12-character names packed base 40, MSB first ('0'-'9' = 3..12, 'A'-'Z' = 13..38,
 * '_' = 39, 0 pads), e.g. 54461524B8230000 = BG1_SNR, 5446151EBC278000 = BG1_SHG (shotgun), 5446152380DB8000 = BG1_SMG
 * (the UZI). Only when the token is in the loaded gun table exactly once with a sane descriptor; native construction
 * follows. A test control: it does not represent a natural pickup. */
static uint64_t bvr_gun_token(const char *gun)
{
    char name[13];
    uint64_t n = 0;
    int i;
    if (!gun || strlen(gun) > 8u) return 0;
    snprintf(name, sizeof(name), "BG1_%s", gun);
    for (i = 0; i < 12; i++) {
        const char ch = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
        unsigned c;
        if (!ch) c = 0;
        else if (ch >= '0' && ch <= '9') c = 3u + (unsigned)(ch - '0');
        else if (ch >= 'A' && ch <= 'Z') c = 13u + (unsigned)(ch - 'A');
        else if (ch == '_') c = 39u;
        else return 0;
        n = n * 40u + c;
        if (!ch) { for (i++; i < 12; i++) n *= 40u; break; }
    }
    return n;
}
static void black_vr_test_loadout(uint32_t record)
{
    static int done;
    const char *gun = getenv("RECOMP_BLACK_TEST_LOADOUT");
    const uint64_t tok = gun && *gun ? bvr_gun_token(gun) : 0;
    uint32_t core, table, header, entries, count, i, matches = 0;
    if (!tok || done >= 8 || !black_pc_ram_span(0x2D45D4u, 4u) || !black_pc_ram_span(0x2D12D0u, 4u)) return;
    core = GUEST32(0x2D45D4u);
    if (!black_pc_ram_span(core, 0x5CB2u) || record != core + 0x3F0u || !black_pc_ram_span(record, 0x50u) ||
        GUEST8(core + 0x5CB1u))
        return;
    table = GUEST32(0x2D12D0u);
    header = black_pc_ram_span(table, 4u) ? GUEST32(table) : 0;
    if (!black_pc_ram_span(header, 8u)) return;
    count = GUEST8(header + 1u);
    entries = GUEST32(header + 4u);
    if (!count || count > 127u || !black_pc_ram_span(entries, count * 32u)) return;
    for (i = 0; i < count; i++) {
        const uint32_t e = entries + i * 32u, cfg = GUEST32(e + 8u);
        if (GUEST32(e) != (uint32_t)tok || GUEST32(e + 4u) != (uint32_t)(tok >> 32)) continue;
        if (!black_pc_ram_span(cfg, 0x65u) || !GUEST32(cfg + 0x60u) || GUEST32(cfg + 0x60u) > 65535u ||
            GUEST8(cfg + 0x64u) >= 10u) return;
        matches++;
    }
    done++;
    fprintf(stderr, "[BLACK_VR] test loadout: BG1_%s token %08X%08X %s (mission %u, was %08X%08X)\n", gun,
            (uint32_t)(tok >> 32), (uint32_t)tok, matches == 1u ? "accepted" : "not in the gun table",
            GUEST8(core + 0x5AECu), GUEST32(record + 4u), GUEST32(record));
    if (matches != 1u) return;
    GUEST32(record) = (uint32_t)tok;
    GUEST32(record + 4u) = (uint32_t)(tok >> 32);
}

/* BLACKVR_AIM_TRACE=1, at the world draw: every live held effect's emitters in the head view (FX lines) and, every 8th
 * frame, the muzzle record relative to the game's camera entity (MUZZLE lines: the barrel's idle tilt). */
static void bvr_fx_trace(void)
{
    uint32_t fx;
    VrMat4 hv;
    unsigned k, n, e;
    if (!bvr_aim_trace() || bvr.mode != VR_PRESENT_STEREO || !(fx = bvr_fx()) || !vrm_m4_inverse(bvr.w_head, &hv)) return;
    /* every 8th frame: the muzzle locator 16B1E0 draws the flash at ([[core+358]+44]+40, a RenderWare matrix in the
     * world), relative to the game's camera entity, i.e. where the latch puts it relative to the hand (plus the grip) */
    if ((bvr_aim_trace() >= 2 || !(bvr.game_frame & 7u)) && black_pc_ram_span(0x2D45D4u, 4u)) {
        const uint32_t core = GUEST32(0x2D45D4u);
        const uint32_t a = black_pc_ram_span(core, 0x35Cu) ? GUEST32(core + 0x358u) : 0u;
        const uint32_t t = black_pc_ram_span(a, 0x48u) ? GUEST32(a + 0x44u) : 0u;
        VrMat4 ei;
        if (black_pc_ram_span(t + 0x40u, 0x40u) && vrm_m4_inverse(bvr.w_entity, &ei)) {
            float r[4][4], q[4][4];
            int j, c;
            for (j = 0; j < 4; j++) {
                for (c = 0; c < 3; c++) r[j][c] = bvr_f32(t + 0x40u + (uint32_t)j * 0x10u + (uint32_t)c * 4u);
                r[j][3] = j == 3 ? 1.0f : 0.0f;
                vrm_m4_apply(ei, r[j], q[j]);
            }
            fprintf(stderr, "[BLACK_VR] MUZZLE gf %u entity-local right %.4f %.4f %.4f up %.4f %.4f %.4f at %.4f %.4f %.4f "
                    "pos %.4f %.4f %.4f\n", bvr.game_frame, (double)q[0][0], (double)q[0][1], (double)q[0][2],
                    (double)q[1][0], (double)q[1][1], (double)q[1][2], (double)q[2][0], (double)q[2][1], (double)q[2][2],
                    (double)q[3][0], (double)q[3][1], (double)q[3][2]);
        }
    }
    n = *(uint16_t *)((uintptr_t)g_xbox_mem_offset + fx + 0x3448u);
    for (k = 0; k < n; k++) {
        const unsigned i = *(uint16_t *)((uintptr_t)g_xbox_mem_offset + GUEST32(fx + 0x3440u) + k * 2u);
        if (i >= BVR_FX_SLOTS) continue;
        for (e = 0; e < 8u; e++) {
            const uint32_t em = GUEST32(fx + i * 0x70u + e * 4u);
            float p[4], q[4];
            if (em <= 1u || !black_pc_ram_span(em, 0xA0u)) continue;
            p[0] = bvr_f32(em + 0x30u); p[1] = bvr_f32(em + 0x34u); p[2] = bvr_f32(em + 0x38u); p[3] = 1.0f;
            vrm_m4_apply(hv, p, q);
            fprintf(stderr, "[BLACK_VR] FX gf %u slot %u owner %08X (player %08X) where %u emitter %u %08X head-view %.3f %.3f %.3f\n",
                    bvr.game_frame, i, GUEST32(fx + 0x2300u + i * 4u),
                    black_pc_ram_span(0x2D45D4u, 4u) ? GUEST32(0x2D45D4u) + 0x30u : 0u, GUEST32(fx + 0x2440u + i * 4u), e, em,
                    (double)q[0], (double)q[1], (double)q[2]);
        }
    }
}

/* `reg`: the first constant register the upload goes to (10: 16F610's mesh MVP, 112: the muzzle flash's). */
static void black_vr_upload_at(uint32_t upload, int reg)
{
    uint32_t renderer;
    float r[16];
    unsigned i;
    if (bvr.mode != VR_PRESENT_STEREO || !black_pc_ram_span(upload, 64u) || !black_pc_ram_span(BVR_RENDERER, 4u)) return;
    renderer = GUEST32(BVR_RENDERER);
    if (!black_pc_ram_span(renderer, 0xD594u) || GUEST32(renderer + 0xD590u) != renderer + 0xD450u) return;
    if (!black_vr_vm_draw && bvr.calib_this_frame >= 48) return;
    for (i = 0; i < 16u; ++i) if (!bvr_f32_ok(upload + i * 4u)) return;
    memcpy(r, (const void *)((uintptr_t)g_xbox_mem_offset + upload), sizeof(r));
    {
        BVR_FP_BEGIN;
        if (black_vr_vm_draw) {
            VrMat4 mv, a;
            const int have_mv = reg == 10 && bvr.off_ok && vgm_mvp_modelview(r, bvr.clip_y_sign, &mv);
            float before[16];
            memcpy(before, r, sizeof(before));
            if (bvr.yv_ok && bvr.hand_locked &&
                vgm_mvp_retarget(r, reg != 10 && bvr.yv_fx_ok ? bvr.yv_fx : bvr.yv, bvr.clip_y_sign)) {
                if (reg == 0x70 && bvr_aim_trace()) bvr_aim_log(before, r);
                memcpy((void *)((uintptr_t)g_xbox_mem_offset + upload), r, sizeof(r));
                vr_mark_viewmodel_mvp(r, reg);          /* gun latch: exactly what reaches c[reg..reg+3] */
                if (have_mv && vgm_weapon_hand_frame(bvr.w_head, bvr.w_entity, bvr.grip, mv, &a)) {
                    float rm[16];
                    int i2, j2;
                    for (i2 = 0; i2 < 4; i2++) for (j2 = 0; j2 < 4; j2++) rm[i2 * 4 + j2] = VRM_AT(a, i2, j2);
                    vr_mark_viewmodel_offhand(rm, bvr.grip_corr);
                }
            }
        } else {
            bvr.calib_this_frame++;
            bvr_learn(r);
        }
        BVR_FP_END;
    }
}

static void black_vr_upload(uint32_t upload) { black_vr_upload_at(upload, 10); }

/* The player's muzzle flash is being drawn (a shot): a short haptic pulse on the gun hand's controller. */
static void black_vr_shot_pulse(void) { vr_game_haptic(vr_game_config()->aim_hand & 1, 1.0f, 40u); }
/* ---- the world pass (1EE820 draws the world through the world camera) ------------------------ */

/* Tells the renderer where in the push buffer the world camera's draws begin and end, the way
 * split-screen marks its views (kelvin_mark_view): at the D3D device's push pointer [27BFF8]+0. */
static void black_vr_world_mark(int begin)
{
    uint32_t device, physical;
    if (bvr.mode < 0 || !black_pc_ram_span(0x0027BFF8u, 4u)) return;
    device = GUEST32(0x0027BFF8u);
    if (!black_pc_ram_span(device, 4u)) return;
    physical = xbox_PhysicalAddressOf(GUEST32(device));
    if (physical >= 0x08000000u) return;
    vr_mark_world_pass(0x80000000u + physical, begin);
}
/* END BLACK_VR */
