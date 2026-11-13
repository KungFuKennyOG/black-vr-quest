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
    VrMat4 w_entity, w_head, w_hand, yv;
    int yv_ok;
    VrMat4 off_rel;                   /* gun_hand_from_off_hand (LH, game units), for the first-person arms */
    VrVec3 grip;                      /* the weapon's grip offset (game units) */
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
        bvr.off_ok = 0;
        bvr.calib_this_frame = 0;
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

        vr_get_recentre(&origin);
        head_rel = vgm_relative(origin, f->head);
        head_rel.position = vgm_clamp_len(head_rel.position, cfg->max_head_offset_m);
        head_stage = vrm_pose_mul(origin, head_rel);
        bvr.w_head = vrm_m4_mul(w_body, bvr_lh(head_rel));
        hand_ok = f->hand[cfg->aim_hand & 1].valid;
        hand_rel = vgm_relative(origin, f->hand[cfg->aim_hand & 1].aim);
        bvr.w_hand = vrm_m4_mul(w_body, bvr_lh(hand_rel));
        if (cfg->hand_weapon && cfg->aim_mode == VR_AIM_HAND && hand_ok) {
            const VrVec3 grip = vrm_scale(vrm_v3(cfg->vm_offset_m[0], cfg->vm_offset_m[1], cfg->vm_offset_m[2]),
                                          cfg->units_per_metre);
            bvr.yv_ok = vgm_weapon_to_hand(bvr.w_head, bvr.w_entity, bvr.w_hand, grip, &bvr.yv);
            bvr.grip = grip;
            /* the off controller relative to the gun hand, for the first-person arms (port/notes/arms.md) */
            if (bvr.yv_ok && f->hand[(cfg->aim_hand & 1) ^ 1].valid) {
                const VrPose rel = vrm_pose_mul(vrm_pose_inv(f->hand[cfg->aim_hand & 1].aim), f->hand[(cfg->aim_hand & 1) ^ 1].aim);
                bvr.off_rel = vgm_lh_from_pose(rel, cfg->units_per_metre);
                bvr.off_ok = 1;
            }
        }

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
            if (bvr.yv_ok && bvr.hand_locked && vgm_mvp_retarget(r, bvr.yv, bvr.clip_y_sign)) {
                memcpy((void *)((uintptr_t)g_xbox_mem_offset + upload), r, sizeof(r));
                vr_mark_viewmodel_mvp(r, reg);          /* gun latch: exactly what reaches c[reg..reg+3] */
                if (have_mv && vgm_off_hand_mirror(bvr.w_head, bvr.w_entity, bvr.grip, mv, bvr.off_rel, &a)) {
                    float rm[16];
                    int i2, j2;
                    for (i2 = 0; i2 < 4; i2++) for (j2 = 0; j2 < 4; j2++) rm[i2 * 4 + j2] = VRM_AT(a, i2, j2);
                    vr_mark_viewmodel_offhand(rm);
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
