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
    unsigned look_cams;               /* world cameras decided since the look integrator last ran */
    double body_yaw, aim_yaw_prev;    /* game degrees, positive = left */
    int have_body;
    int body_fresh;                   /* the body yaw was stepped (or anchored) since the view last turned STEREO */
    double pitch_sign;                /* game pitch = -pitch_up * sign */
    int pitch_votes;
    VgmYawCal cal;
    VgmBob bob;
    double last_frame_time;
    /* this frame */
    VrMat4 w_entity, w_head, w_hand, yv, w_body;
    int yv_ok;
    VrVec3 grip;                      /* the weapon's grip offset (game units) */
    unsigned barrel_ok;               /* frames aimed along the drawn barrel (log) */
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

/* Test only: BLACKVR_TEST_LOOK_STALE_MS=<ms> adds a wall-clock limit on the look integrator's age (the rule before
 * 2026-11-13 was 250 ms; the decision now counts world cameras, BVR_LOOK_STALE_CAMS). Unset: 0, no wall-clock limit. */
static double bvr_look_stale_s(void)
{
    static double s = -1.0;
    if (s < 0.0) {
        const char *e = getenv("BLACKVR_TEST_LOOK_STALE_MS");
        const double ms = e && *e ? atof(e) : 0.0;
        s = ms > 0.0 && ms < 600000.0 ? ms / 1000.0 : 0.0;
        if (s > 0.0) fprintf(stderr, "[BLACK_VR] test: look staleness limit %.0f ms\n", s * 1000.0);
    }
    return s;
}

/* STEREO needs the look integrator to have run within the last few world cameras: it stops in cutscenes and menus.
 * Counted in world cameras, not wall time, so a hitch (a long game frame) cannot turn one gameplay frame flat
 * (sweep 2026-11-13, issue 3: the old 250 ms rule popped a frame to the flat quad after hitches of 0.25 s or more).
 * 6: two frames of level_07, which builds its world camera three times per frame. */
#define BVR_LOOK_STALE_CAMS 6u

/* The current weapon has a scope ([[look+8C]+29C] weapon, +EC descriptor, +C0). */
static int bvr_weapon_scoped(void)
{
    uint32_t actor, weapon, desc;
    if (!bvr.look || !black_pc_ram_span(bvr.look, 0x90u)) return 0;
    actor = GUEST32(bvr.look + 0x8Cu);
    if (!black_pc_ram_span(actor, 0x2A0u)) return 0;
    weapon = GUEST32(actor + 0x29Cu);
    if (!black_pc_ram_span(weapon, 0xF0u)) return 0;
    desc = GUEST32(weapon + 0xECu);
    return black_pc_ram_span(desc, 0xC8u) && GUEST8(desc + 0xC0u) != 0;
}

/* The zoom at which the view goes flat for the scope: scope_zoom (2.5), and for a scoped weapon at most 2.0, so the
 * sniper's X2 (2.33) shows the game's scope view, not its full-screen scope mask as a black panel on the HUD quad
 * (sweep issue 2). The 1.10 of the raise animation stays STEREO. */
static float bvr_scope_zoom_min(void)
{
    const float z = vr_game_config()->scope_zoom_min;
    return bvr_weapon_scoped() && z > 2.0f ? 2.0f : z;
}

/* The tutorial hint element (ecx of its update, 1314E0), noted by black_vr_hint_probe each HUD update. */
static uint32_t bvr_hint_el;
static int bvr_hint_was;      /* black_vr_decide: the last world camera was kept STEREO for a hint */

/* A tutorial hint that stops the game is up: the element's pending (+22A4) or active (+2110) hint is one of the five
 * that 12B3C0 gives a "PRESS A TO CONTINUE" line (the id list at 28AA80: 12, 13, 23, 87, 24), while its state byte
 * (+1B) is 5..8 (fading in, shown, fading out), and the pause menu (main +210C8) is not open over it. The game stops
 * its world clock for these (black_pc_input_gameplay_blocked_query) but keeps drawing the frozen world each frame
 * (blurred, the hint at the bottom), so VR stays STEREO with the head tracked: Kenny 2026-11-13, "needs to be changed
 * into smt that doesnt change you to flat screen", "has to look like the stop-the-game hints in the original game". */
static int bvr_hint_stop(void)
{
    const uint32_t el = bvr_hint_el;
    uint32_t a, p, st, i;
    if (!el || !black_pc_ram_span(el, 0x2300u) || !black_pc_ram_span(0x0028AA80u, 20u)) return 0;
    st = GUEST8(el + 0x1Bu);
    if (st < 5u || st > 8u) return 0;
    a = GUEST32(el + 0x2110u);
    p = GUEST32(el + 0x22A4u);
    if (black_pc_ram_span(0x002D45F0u, 4u)) {
        const uint32_t main = GUEST32(0x002D45F0u);
        if (black_pc_ram_span(main, 0x210C9u) && GUEST8(main + 0x210C8u)) return 0;
    }
    for (i = 0; i < 5u; i++) {
        const uint32_t id = GUEST32(0x0028AA80u + i * 4u);
        if (id == a || id == p) return 1;
    }
    return 0;
}

/* -2: not the world camera (state untouched); -1: VR presentation inactive. */
static int black_vr_decide(uint32_t camera)
{
    uint32_t renderer;
    int hint;
    if (!black_pc_ram_span(BVR_RENDERER, 4u)) return -2;
    renderer = GUEST32(BVR_RENDERER);
    if (!black_pc_ram_span(renderer, 0xD4F0u) || camera != renderer + 0xD450u) return -2;
    if (!vr_active()) return -1;
    if (bvr.look_cams < 1000000u) bvr.look_cams++;
    if (pc_photo_active() || pc_split_enabled() || black_split_pass) return VR_PRESENT_FLAT_MENU;
    if (!black_photo_world_camera()) return VR_PRESENT_FLAT_CINEMA;     /* scripted / not the player's view */
    /* a stop-the-game tutorial hint: the clock is stopped and the look integrator with it; the world stays 3D */
    hint = bvr.look && bvr_hint_stop();
    /* the hint just went: the look integrator runs again from the next game frame; a fresh count of cameras, so the
     * frame between does not drop to the flat quad (harness 2026-11-13: one flat frame there) */
    if (bvr_hint_was && !hint) bvr.look_cams = 0;
    bvr_hint_was = hint;
    if (!hint && black_pc_input_gameplay_blocked_query()) return VR_PRESENT_FLAT_MENU;
    if (!bvr.look || (!hint && bvr.look_cams > BVR_LOOK_STALE_CAMS)) return VR_PRESENT_FLAT_MENU;
    if (!hint && bvr_look_stale_s() > 0.0 && black_vr_now() - bvr.look_time > bvr_look_stale_s()) return VR_PRESENT_FLAT_MENU;
    if (bvr_zoom() >= bvr_scope_zoom_min()) return VR_PRESENT_FLAT_SCOPE;
    return VR_PRESENT_STEREO;
}

/* ---- harness only: BLACKVR_DECIDE_TRACE=1 ------------------------------------------------------------------------
 * Logs why the presentation leaves STEREO (each STEREO -> other change and the change back): the inputs of
 * black_vr_decide. Diagnostics for the sweep (build-vrmenu-x86/sweep/REPORT.md); without the variable, nothing. */
/* Every line the game puts on its HUD message list goes to the log, once. The list is an 8-slot ring of a title and
 * a body (+10E4 + slot * 100 and +80 from it, 7F bytes each, written by 12B790: "SECONDARY OBJECTIVES TARGET UPDATE"
 * / "0/3 SECONDARY OBJECTIVES COMPLETED", the recon and intel objectives' "recon objective complete" / "you found
 * ...") in the HUD element of vtable 290F58 (its update 1337C0 posts through 12FE30), found in the HUD's element
 * lists ([2D12E4] +A0 + layer * A8, next at +8). Called each frame from the messages element's update (1314E0).
 * Kenny 2026-11-13: no message seen on picking up a recon map; this tells a message the game never posted from one
 * that was posted but not seen. */
static void black_vr_msg_watch(void);

/* Tutorial hints (the ones that stop the game): sub_0012F650 queues hint esi through 12B3C0 only if its "shown" byte
 * ([2DB670+id]) is clear, and, for the in-mission callers (args 0,1), only if the frontend's level selection
 * ([2D45F0]+2020C) is 0 and the level's record ([2D0D84 + difficulty*0x100 + index*0x20], the menu reads 2 there as
 * "completed on this difficulty") is 0 on every difficulty. This logs those inputs when they change, and every hint
 * whose shown byte gets set (= the game queued it). Kenny 2026-11-13: no stop-the-game hint on either difficulty. */
static void black_vr_hint_probe(uint32_t el)
{
    static uint32_t last[8];
    static uint8_t shown[0x40];
    static int init;
    uint32_t now[8];
    unsigned i, n = 0, idx = 0xFFu;
    if (!black_pc_ram_span(0x002D45F0u, 4u) || !black_pc_ram_span(0x002DB670u, 0x40u) ||
        !black_pc_ram_span(0x002D0D84u, 0x400u) || !black_pc_ram_span(0x00288648u, 8u)) return;
    {
        const uint32_t g = GUEST32(0x002D45F0u);
        if (!g || !black_pc_ram_span(g + 0x2014Cu, 4u) || !black_pc_ram_span(g + 0x2020Cu, 1u)) return;
        now[0] = GUEST8(g + 0x2020Cu);
        now[1] = GUEST32(g + 0x2014Cu);
    }
    for (i = 0; i < 8u; i++) if ((uint32_t)(int32_t)(int8_t)GUEST8(0x00288648u + i) == now[0]) { idx = i; break; }
    now[2] = idx;
    {   /* harness only: BLACKVR_TEST_RECORD=v writes v into this level's Easy completion record once (a save that has
         * beaten mission 1 on Easy: 2) */
        static int tr = -2;
        if (tr == -2) { const char *e = getenv("BLACKVR_TEST_RECORD"); tr = e && *e && !getenv("BLACKVR_TEST_RECORD_EARLY") ? atoi(e) : -1000; }
        if (tr != -1000 && idx < 8u) {
            const char *d = getenv("BLACKVR_TEST_RECORD_EARLY") ? NULL : getenv("BLACKVR_TEST_DIFFICULTY");
            if (d && *d) GUEST32(GUEST32(0x002D45F0u) + 0x2014Cu) = (uint32_t)atoi(d);
            GUEST32(0x002D0D84u + idx * 0x20u) = (uint32_t)tr;
            fprintf(stderr, "[BLACK_VR] test: level %u Easy record set to %d\n", idx, tr);
            tr = -1000;
        }
    }
    for (i = 0; i < 4u; i++) now[3 + i] = idx < 8u ? GUEST32(0x002D0D84u + i * 0x100u + idx * 0x20u) : 0xEEEEEEEEu;
    for (i = 0; i < 0x40u; i++) n += GUEST8(0x002DB670u + i) != 0;
    now[7] = n;
    if (!init || memcmp(now, last, sizeof now)) {
        fprintf(stderr, "[BLACK_VR] tutorial hints: level selection %u (index %d), difficulty %u, level record by "
                "difficulty %d %d %d %d, %u hint(s) already shown%s; VR setting tutorial_hints %s\n", now[0],
                (int)(idx < 8u ? idx : -1), now[1], (int)now[3], (int)now[4], (int)now[5], (int)now[6], n,
                now[0] != 0u ? " - the game shows them only from level selection 0" :
                ((int)now[3] > 0 || (int)now[4] > 0 || (int)now[5] > 0 || (int)now[6] > 0) ? " - the game skips them once this record is above 0" : "",
                vr_game_config()->tutorial_hints ? "on (the records read as not completed)" : "off");
        memcpy(last, now, sizeof now);
    }
    for (i = 0; i < 0x40u; i++) {
        const uint8_t v = GUEST8(0x002DB670u + i);
        if (v && !shown[i]) fprintf(stderr, "[BLACK_VR] tutorial hints: hint %u marked shown%s\n", i, init ? "" : " (before the first HUD update)");
        shown[i] = v;
    }
    bvr_hint_el = el;
    {   /* el, the hint element (ecx of 1314E0): active hint +2110, pending +22A4, state byte +1B */
        static uint32_t la = 1, lp = 1, ls = 0x100;
        if (black_pc_ram_span(el, 0x2300u)) {
            const uint32_t a = GUEST32(el + 0x2110u), p = GUEST32(el + 0x22A4u), st = GUEST8(el + 0x1Bu);
            if (a != la || p != lp || st != ls)
                fprintf(stderr, "[BLACK_VR] tutorial hints: element %08X active %d pending %d state %u\n", el, (int)a, (int)p, st);
            la = a; lp = p; ls = st;
        }
    }
    init = 1;
}

static void black_vr_msg_watch(void)
{
    static uint32_t seen_el, said;
    static uint8_t seen[8][2][0x80];
    const uint8_t *const m = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    const uint32_t hud = black_pc_ram_span(0x002D12E4u, 4u) ? GUEST32(0x002D12E4u) : 0u;
    uint32_t el = 0, e, layer;
    unsigned s, f, i, n = 0;
    if (!hud || !black_pc_ram_span(hud, 0x240u)) return;
    for (layer = 0; layer < GUEST8(hud + 0x23Cu) && layer < 3u && !el; layer++)
        for (e = GUEST32(hud + 0xA0u + layer * 0xA8u); e && n < 64u && black_pc_ram_span(e, 0x10u); e = GUEST32(e + 8u), n++)
            if (GUEST32(e) == 0x00290F58u && black_pc_ram_span(e, 0x1970u)) { el = e; break; }
    if (!el) {
        if (!said) { said = 1; fprintf(stderr, "[BLACK_VR] HUD messages: no element of vtable 290F58 in the HUD (%u seen)\n", n); }
        return;
    }
    if (el != seen_el) fprintf(stderr, "[BLACK_VR] HUD messages: watching the message list of the element at %08X\n", el);
    for (s = 0; s < 8u; s++) {
        int changed = 0;
        for (f = 0; f < 2u; f++) {
            const uint8_t *t = m + el + 0x10E4u + s * 0x100u + f * 0x80u;
            if (memcmp(t, seen[s][f], 0x7Fu)) { memcpy(seen[s][f], t, 0x7Fu); changed = 1; }
        }
        if (changed && el == seen_el && (seen[s][0][0] || seen[s][1][0])) {
            char line[2][0x80];
            for (f = 0; f < 2u; f++) {
                for (i = 0; i < 0x7Fu && seen[s][f][i]; i++)
                    line[f][i] = seen[s][f][i] >= 0x20u && seen[s][f][i] < 0x7Fu ? (char)seen[s][f][i] : '?';
                line[f][i] = 0;
            }
            fprintf(stderr, "[BLACK_VR] HUD message (slot %u): \"%s\" / \"%s\"\n", s, line[0], line[1]);
        }
    }
    seen_el = el;                     /* the first look at an element only takes what is already there */
}

static void bvr_decide_trace(int prev, int m)
{
    static int on = -1;
    static unsigned same;
    if (on < 0) { const char *e = getenv("BLACKVR_DECIDE_TRACE"); on = e && *e && *e != '0'; }
    if (!on) return;
    /* every change, and every 32nd decision of a flat mode that lasts (with a look object: gameplay shown flat) */
    same = prev == m ? same + 1u : 0u;
    if (prev == m && (m == VR_PRESENT_STEREO || !bvr.look || (same & 31u))) return;
    fprintf(stderr, "[BLACK_VR] DECIDE gf %u: mode %d -> %d | photo %d split %d split_pass %d world_camera %d blocked %d "
            "look %08X age %.0f ms zoom %.2f | body yaw %.1f (set %d) look yaw %.1f\n", bvr.game_frame, prev, m,
            pc_photo_active(), pc_split_enabled(), black_split_pass, black_photo_world_camera(),
            black_pc_input_gameplay_blocked_query(), bvr.look, bvr.look ? (black_vr_now() - bvr.look_time) * 1000.0 : -1.0,
            (double)bvr_zoom(), bvr.body_yaw, bvr.have_body,
            bvr.look && black_pc_ram_span(bvr.look, 0x20u) && bvr_f32_ok(bvr.look + 0x18u) ? (double)bvr_f32(bvr.look + 0x18u) : 0.0);
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
        bvr_decide_trace(bvr.mode, m);
        if ((m == VR_PRESENT_STEREO || m == VR_PRESENT_FLAT_SCOPE) &&
            bvr.mode != VR_PRESENT_STEREO && bvr.mode != VR_PRESENT_FLAT_SCOPE)
            bvr.body_fresh = 0;           /* black_vr_look skipped this frame: anchor in black_vr_camera_after_world */
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

/* The drawn gun's barrel in the gun hand's OpenXR aim space (metres; x right, y up, z back): the muzzle locator's
 * record ([[core+358]+44]+40, RenderWare rows right, up, at, pos, posed on the game's aim matrix at the last update),
 * through the last camera entity, the grip and the first-person arms' lock L, as the weapon is drawn. p: the muzzle,
 * a: the barrel's direction (unit, forward). 0 when unreadable or implausible. */
static int bvr_barrel_in_hand(VrVec3 *p, VrVec3 *a)
{
    uint32_t core, m, t;
    VrMat4 ei, l, back = vrm_m4_identity(), x;
    float lf[16], r[4][4], q[4][4];
    int j, c;
    const float upm = vr_game_config()->units_per_metre;
    if (!bvr.yv_ok || !black_pc_ram_span(0x2D45D4u, 4u)) return 0;
    core = GUEST32(0x2D45D4u);
    m = black_pc_ram_span(core, 0x35Cu) ? GUEST32(core + 0x358u) : 0u;
    t = black_pc_ram_span(m, 0x48u) ? GUEST32(m + 0x44u) : 0u;
    if (!black_pc_ram_span(t + 0x40u, 0x40u) || !vrm_m4_inverse(bvr.w_entity, &ei)) return 0;
    vr_grip_lock(lf);
    for (j = 0; j < 4; j++) for (c = 0; c < 4; c++) VRM_AT(l, j, c) = lf[j * 4 + c];
    VRM_AT(back, 0, 3) = -bvr.grip.x; VRM_AT(back, 1, 3) = -bvr.grip.y; VRM_AT(back, 2, 3) = -bvr.grip.z;
    x = vrm_m4_mul(l, vrm_m4_mul(back, ei));
    for (j = 0; j < 4; j++) {
        for (c = 0; c < 3; c++) {
            r[j][c] = bvr_f32(t + 0x40u + (uint32_t)j * 0x10u + (uint32_t)c * 4u);
            if (!(r[j][c] == r[j][c]) || r[j][c] > 1e6f || r[j][c] < -1e6f) return 0;
        }
        r[j][3] = j == 3 ? 1.0f : 0.0f;
        vrm_m4_apply(x, r[j], q[j]);
    }
    *p = vrm_v3(q[3][0] / upm, q[3][1] / upm, -q[3][2] / upm);       /* LH game units -> OpenXR metres */
    *a = vrm_v3(q[2][0], q[2][1], -q[2][2]);
    if (!vrm_normalize(a)) return 0;
    if (a->z > 0.0f) *a = vrm_scale(*a, -1.0f);                      /* forward is -z */
    /* near the hand and within 10 deg of the controller's ray: the gun is held (in raise and switch animations it
     * points elsewhere, and the controller's ray is the better aim) */
    return vrm_len(*p) < 1.5f && a->z < -0.985f;
}

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
        bvr.look_cams = 0;
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
        if (hand_mode && cfg->aim_barrel) {
            /* shots converge on the drawn gun's barrel line (not the controller's ray), aim_converge metres out */
            VrVec3 bp, ba;
            if (bvr_barrel_in_hand(&bp, &ba)) {
                const VrVec3 mp = vrm_add(hand_rel.position, vrm_q_rotate(hand_rel.orientation, bp));
                const VrVec3 md = vrm_q_rotate(hand_rel.orientation, ba);
                VrVec3 dd = vrm_sub(vrm_add(mp, vrm_scale(md, cfg->aim_converge_m)), head_rel.position);
                if (vrm_normalize(&dd)) d = dd;
                if (!(bvr.barrel_ok++ % (getenv("BLACKVR_AIM_LOG") ? 30u : 600u)))
                    fprintf(stderr, "[BLACK_VR] aim: along the drawn barrel: muzzle %.3f %.3f %.3f m from the controller "
                            "(right, up, back), barrel %.1f deg off the controller's ray\n", (double)bp.x, (double)bp.y,
                            (double)bp.z, acos(ba.z < -1.0f ? 1.0 : -(double)ba.z) / VRM_DEG);
            }
        }
        bvr.aim_d = d;
        vgm_dir_angles(d, &ay, &ap);
        g = bvr_f32(look + 0x18u);
        bvr.body_yaw = vgm_body_yaw_step(bvr.body_yaw, bvr.have_body, g, bvr.aim_yaw_prev, turn, 20.0);
        bvr.have_body = 1;
        bvr.body_fresh = 1;
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
        if (!bvr.body_fresh) {
            /* The first STEREO camera after a flat stretch (level start, checkpoint, menu): black_vr_look ran while
             * the view was still flat and skipped, so the body yaw is unset (0) or from before. Anchor it to the
             * game's facing now, as the look's own step would at the next frame (sweep issue 1: the first frame of
             * every level faced 55-96 deg off). */
            VrPose o;
            VrVec3 d;
            double ay, ap;
            const int hm = cfg->aim_mode == VR_AIM_HAND && f && f->hand[cfg->aim_hand & 1].valid;
            const double g = bvr_f32(bvr.look + 0x18u);
            vr_get_recentre(&o);
            d = f ? vgm_aim_dir(hm, vgm_relative(o, f->head), vgm_relative(o, f->hand[cfg->aim_hand & 1].aim),
                                vrm_v3(0, 0, 0), cfg->aim_converge_m)
                  : vrm_v3(0, 0, -1);
            vgm_dir_angles(d, &ay, &ap);
            (void)ap;
            if (!bvr.have_body) bvr.aim_yaw_prev = ay;
            bvr.body_yaw = vgm_body_yaw_step(bvr.body_yaw, bvr.have_body, g, bvr.aim_yaw_prev, 0.0, 20.0);
            bvr.have_body = 1;
            bvr.body_fresh = 1;
        }
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
                /* the muzzle flash draw (its own MVP, not skinned with the weapon): moved by the arms' wrist lock of
                 * the last render (L, in the hand frame), so the flash sits at the drawn muzzle */
                float lf[16];
                VrMat4 l;
                int r2, c2;
                vr_grip_lock(lf);
                for (r2 = 0; r2 < 4; r2++) for (c2 = 0; c2 < 4; c2++) VRM_AT(l, r2, c2) = lf[r2 * 4 + c2];
                bvr.yv_fx_ok = bvr.yv_ok && vgm_weapon_to_hand(bvr.w_head, bvr.w_entity, vrm_m4_mul(bvr.w_hand, l), grip,
                                                               &bvr.yv_fx);
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
/* A rotation and a position (orthonormal axes to 1e-2): what an aim matrix is, not unset memory */
static int bvr_m4_rigid(VrMat4 a)
{
    const VrVec3 x = vgm_col(a, 0), y = vgm_col(a, 1), z = vgm_col(a, 2);
    return fabsf(vrm_dot(x, x) - 1.0f) < 1e-2f && fabsf(vrm_dot(y, y) - 1.0f) < 1e-2f &&
           fabsf(vrm_dot(z, z) - 1.0f) < 1e-2f && fabsf(vrm_dot(x, y)) < 1e-2f && fabsf(vrm_dot(y, z)) < 1e-2f &&
           fabsf(vrm_dot(x, z)) < 1e-2f;
}

static struct {
    int ok;
    uint32_t frame, aim;                      /* aim: the player's aim matrix, when it was this render's camera entity */
    uint32_t aim_off;                         /* ... when it was not, but readable: the camera entity was off it by */
    VrMat4 a_last;                            /* a_last^-1 * w_entity (a melee lunge, a menu's camera) */
    VrMat4 x, back, w_body, w_hand, w_entity;
    uint32_t at[8];
    VrMat4 l[8];
} bvr_fxl;                                    /* the last render's */
static struct { int ok, fresh; uint32_t frame, at; VrVec3 p, p_old; } bvr_fxd;   /* check: the last update's muzzle */
/* The muzzle check in the log (always; ~every 30 s of STEREO with the gun in the hand): how far the held effects'
 * update put the muzzle record from where the next render puts the hand-held muzzle (0 when the placement is right),
 * and how many updates had to do without the aim matrix (the fallback, which keeps the shot's recoil in the spawn). */
static struct { unsigned n, fallback, offset; double sum, max, body_yaw; } bvr_fxs;
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
        /* the grip this render used (vm_offset + the first-person arms' auto-grip baseline), then the arms' per-draw
         * wrist lock L (last render's, in the hand frame), so the effects start at the drawn gun's muzzle */
        float lf[16];
        VrMat4 l;
        int r2, c2;
        vr_grip_lock(lf);
        for (r2 = 0; r2 < 4; r2++) for (c2 = 0; c2 < 4; c2++) VRM_AT(l, r2, c2) = lf[r2 * 4 + c2];
        VRM_AT(bvr_fxl.back, 0, 3) = -bvr.grip.x;
        VRM_AT(bvr_fxl.back, 1, 3) = -bvr.grip.y;
        VRM_AT(bvr_fxl.back, 2, 3) = -bvr.grip.z;
        bvr_fxl.back = vrm_m4_mul(l, bvr_fxl.back);
        (void)cfg;
    }
    bvr_fxl.w_body = bvr.w_body;
    bvr_fxl.w_hand = bvr.w_hand;
    bvr_fxl.w_entity = bvr.w_entity;
    va = bvr_aim_va();
    bvr_fxl.aim = va && bvr_aim_view(va, &a) && bvr_m4_near(a, bvr.w_entity, 2e-3f, 2e-3f * cfg->units_per_metre) ? va : 0;
    bvr_fxl.aim_off = !bvr_fxl.aim && va && bvr_aim_view(va, &a) && bvr_m4_rigid(a) && bvr_m4_rigid(bvr.w_entity) &&
                      vrm_len(vrm_sub(vgm_col(a, 3), vgm_col(bvr.w_entity, 3))) <= 1.0f * cfg->units_per_metre ? va : 0;
    if (bvr_fxl.aim_off) bvr_fxl.a_last = a;
    {
        static int logged;
        if (!logged && !bvr_fxl.aim) {
            logged = 1;
            fprintf(stderr, "[BLACK_VR] gun fx: the player's aim matrix is not this render's camera (logged once): the held "
                    "effects carry the camera's offset from it%s\n", bvr_fxl.aim_off ? "" : " (unreadable: the last render's "
                    "transform, moved with the hand)");
        }
    }
    for (r = 0; r < 8u; r++)
        if ((bvr_fxl.at[r] = bvr_fp_record(r)) != 0 && !bvr_rw_read(bvr_fxl.at[r], &bvr_fxl.l[r])) bvr_fxl.at[r] = 0;
    bvr_fxl.frame = bvr.game_frame;
    bvr_fxl.ok = 1;
    /* trace: where the last update started the muzzle's effects vs where this render puts the muzzle in the hand (the
     * late latch's own move at display aside), and where e856ad7 started them */
    if (bvr_fxd.ok && bvr_fxl.at[5] && bvr_fxd.at == bvr_fxl.at[5]) {
        float p4[4], b[4];
        VrVec3 t;
        double dm;
        p4[0] = VRM_AT(bvr_fxl.l[5], 0, 3); p4[1] = VRM_AT(bvr_fxl.l[5], 1, 3); p4[2] = VRM_AT(bvr_fxl.l[5], 2, 3); p4[3] = 1.0f;
        vrm_m4_apply(bvr_fxl.x, p4, b);
        t = vrm_v3(b[0], b[1], b[2]);
        dm = (double)vrm_len(vrm_sub(bvr_fxd.p, t));
        if (bvr_aim_trace())
            fprintf(stderr, "[BLACK_VR] FXC gf %u muzzle effects started at update after gf %u: %.4f from the hand-held "
                    "muzzle (e856ad7: %.4f)%s\n", bvr.game_frame, bvr_fxd.frame, dm,
                    (double)vrm_len(vrm_sub(bvr_fxd.p_old, t)),
                    bvr_fxd.fresh == 1 ? "" : bvr_fxd.fresh == 2 ? ", camera off the aim matrix" : ", no aim matrix");
        dm /= (double)cfg->units_per_metre;
        if (fabs(vrm_wrap_deg(bvr.body_yaw - bvr_fxs.body_yaw)) > 0.5) goto turned;   /* a snap/smooth turn: below */
        bvr_fxs.n++;
        bvr_fxs.sum += dm;
        if (dm > bvr_fxs.max) bvr_fxs.max = dm;
        if (!bvr_fxd.fresh) bvr_fxs.fallback++;
        if (bvr_fxd.fresh == 2) bvr_fxs.offset++;
        if (bvr_fxs.n >= 1800u) {
            fprintf(stderr, "[BLACK_VR] gun fx check: %u updates, muzzle effects started %.1f cm (mean) / %.1f cm (max) "
                    "from the hand-held muzzle; %u with the camera off the aim matrix, %u without the aim matrix\n",
                    bvr_fxs.n, 100.0 * bvr_fxs.sum / bvr_fxs.n, 100.0 * bvr_fxs.max, bvr_fxs.offset, bvr_fxs.fallback);
            memset(&bvr_fxs, 0, sizeof(bvr_fxs));
        }
    }
    /* A turn between the update and this render moves the whole view about the body: the effects started in that one
     * update are off by the turn (a 35 deg snap: ~40 cm at the muzzle, one puff), which the check leaves out. */
turned:
    bvr_fxs.body_yaw = bvr.body_yaw;
    bvr_fxd.ok = 0;
}

/* The transform the next render will give the weapon (world X = w_hand * grip^-1 * w_entity^-1), built now, in the
 * game's update: the camera entity it will have is the player's aim matrix now (the records are posed on it; the camera
 * entity itself still has the last frame's), the hand is this game frame's latch, on the last render's body moved as
 * the aim matrix moved. Without the aim matrix: the last render's X, carried by the hand's move. fresh: 1 with the aim
 * matrix, 2 with the camera off it, 0 without; dp: the body's step. Needs bvr_fxl.ok. */
static VrMat4 bvr_fx_x_now(int *fresh_out, VrVec3 *dp_out, VrMat4 *h_out)
{
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
    } else if (bvr_fxl.aim_off && bvr_aim_va() == bvr_fxl.aim_off && bvr_aim_view(bvr_fxl.aim_off, &m_now) &&
               bvr_m4_rigid(m_now) && vrm_m4_inverse(bvr_fxl.a_last, &minv)) {
        /* The last render's camera entity was off the aim matrix (O = A_last^-1 * E_last: a melee lunge, a menu's
         * camera). The records are still posed on the aim matrix, now with this frame's recoil: the next render's
         * camera entity is taken as A_now * O, so X = h_now * grip^-1 * (A_now * O)^-1 keeps the recoil out of the
         * spawn as above. (The last render's X alone kept it in: the shotgun's 8 deg kick put its fireball ~10 cm
         * above the muzzle.) */
        const VrMat4 e_next = vrm_m4_mul(m_now, vrm_m4_mul(minv, bvr_fxl.w_entity));
        VrMat4 einv;
        dp = vrm_sub(vgm_col(e_next, 3), vgm_col(bvr_fxl.w_entity, 3));
        if (vrm_len(dp) <= 1.0f * cfg->units_per_metre && vrm_m4_inverse(e_next, &einv)) {
            VRM_AT(h_now, 0, 3) += dp.x; VRM_AT(h_now, 1, 3) += dp.y; VRM_AT(h_now, 2, 3) += dp.z;
            x = vrm_m4_mul(h_now, vrm_m4_mul(bvr_fxl.back, einv));
            fresh = 2;
        } else {
            dp = vrm_v3(0, 0, 0);
        }
    }
    if (!fresh && vrm_m4_inverse(bvr_fxl.w_hand, &hinv)) x = vrm_m4_mul(h_now, vrm_m4_mul(hinv, bvr_fxl.x));
    *fresh_out = fresh;
    *dp_out = dp;
    *h_out = h_now;
    return x;
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
        VrMat4 h_now, x;
        VrVec3 dp;
        int fresh;
        x = bvr_fx_x_now(&fresh, &dp, &h_now);
        for (r = 0; r < 8u; r++) {
            const uint32_t at = bvr_fp_record(r);
            VrMat4 now, m;
            int dup = 0;
            if (!at || !bvr_rw_read(at, &now)) continue;
            for (j = 0; j < bvr_fxu_n; j++) if (bvr_fxu_at[j] == at) dup = 1;
            if (dup) continue;
            m = vrm_m4_mul(x, now);
            if (r == 5u) {
                /* the muzzle check (and the trace: what e856ad7 did, the last render's X on the game's record, its
                 * motion since as a translation) */
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
                    fresh == 1 ? "" : fresh == 2 ? ", camera off the aim matrix" : ", no aim matrix");
        if (bvr_fxu_n && (++bvr_fxu_updates == 1u || !(bvr_fxu_updates & 4095u)))
            fprintf(stderr, "[BLACK_VR] gun fx: the weapon's held effects follow it to the hand (%u updates%s)\n",
                    bvr_fxu_updates, fresh == 1 ? "" : fresh == 2 ? ", the camera off the aim matrix" : ", without the aim matrix");
        BVR_FP_END;
    }
}

/* The muzzle effects along the barrel (2026-11-13, Kenny: "the shotgun fireball ... it's a bit high", with the gun
 * still). The shotgun's fireball emitter (the fast one, ~42 m/s) is tilted 4 deg up from the muzzle record's axis
 * (harness, BLACKVR_AIM_TRACE=3 FXV lines: emitter 3 "at 4.0/0.0 deg", its particles 3.9 deg up); the flat game aims
 * it a little up from a gun held low beside the camera, toward the crosshair. With the gun in the hand it shows as a
 * fireball leaving above the barrel (4 deg: 3 cm at 40 cm, 7 cm at 1 m). After the held effects' update (records in the
 * hand), each emitter of the player's weapon effects whose axis is within 6 deg of its record's axis is turned onto
 * it, about its own position; emitters set wider (smoke drifting sideways) keep their direction. The update sets the
 * emitters from the records each frame (163FD0), so this is redone each frame. BLACKVR_GUN_FX_ALIGN=0: as the game. */
static void bvr_fx_align(void)
{
    static int on = -1;
    uint32_t fx, player;
    unsigned k, n, e;
    if (on < 0) { const char *v = getenv("BLACKVR_GUN_FX_ALIGN"); on = !v || *v != '0'; }
    if (!on || !bvr_fxu_n || !(fx = bvr_fx()) || !black_pc_ram_span(0x2D45D4u, 4u)) return;
    player = GUEST32(0x2D45D4u) + 0x30u;
    n = *(uint16_t *)((uintptr_t)g_xbox_mem_offset + fx + 0x3448u);
    for (k = 0; k < n; k++) {
        const unsigned i = *(uint16_t *)((uintptr_t)g_xbox_mem_offset + GUEST32(fx + 0x3440u) + k * 2u);
        const uint32_t slot = fx + i * 0x70u;
        VrVec3 z;
        if (i >= BVR_FX_SLOTS || GUEST32(fx + 0x2300u + i * 4u) != player || GUEST32(fx + 0x2440u + i * 4u) > 7u) continue;
        if (!bvr_f32_ok(slot + 0x40u) || !bvr_f32_ok(slot + 0x44u) || !bvr_f32_ok(slot + 0x48u)) continue;
        z = vrm_v3(bvr_f32(slot + 0x40u), bvr_f32(slot + 0x44u), bvr_f32(slot + 0x48u));     /* the record's at row */
        if (!vrm_normalize(&z)) continue;
        for (e = 0; e < 8u; e++) {
            const uint32_t em = GUEST32(slot + e * 4u);
            VrVec3 a, axis, rows[3];
            float c, ang;
            int j;
            if (em <= 1u || !black_pc_ram_span(em, 0x40u)) continue;
            for (j = 0; j < 3; j++) {
                const uint32_t r = em + (uint32_t)j * 0x10u;
                if (!bvr_f32_ok(r) || !bvr_f32_ok(r + 4u) || !bvr_f32_ok(r + 8u)) break;
                rows[j] = vrm_v3(bvr_f32(r), bvr_f32(r + 4u), bvr_f32(r + 8u));
            }
            if (j < 3) continue;
            a = rows[2];
            if (!vrm_normalize(&a)) continue;
            c = vrm_dot(a, z);
            if (c > 0.9999985f || c < 0.9945219f) continue;                  /* 0.1 .. 6 deg */
            axis = vrm_cross(a, z);
            if (!vrm_normalize(&axis)) continue;
            ang = acosf(c);
            {
                const VrQuat q = vrm_q_axis_angle(axis, ang);
                for (j = 0; j < 3; j++) {
                    const VrVec3 v = vrm_q_rotate(q, rows[j]);
                    const uint32_t r = em + (uint32_t)j * 0x10u;
                    bvr_put(r, v.x); bvr_put(r + 4u, v.y); bvr_put(r + 8u, v.z);
                }
            }
        }
    }
}

/* After it: the records back as the game left them. */
static void black_vr_fx_update_end(void)
{
    unsigned k;
    {
        BVR_FP_BEGIN;
        bvr_fx_align();
        BVR_FP_END;
    }
    for (k = 0; k < bvr_fxu_n; k++)
        memcpy((uint8_t *)(uintptr_t)g_xbox_mem_offset + bvr_fxu_at[k], bvr_fxu_kept[k], 0x40u);
    bvr_fxu_n = 0;
}

/* ---- the player's shots leave the drawn barrel ----------------------------------------------------------------------
 * Bullets: the shot (1DBD50, arg 0 the shot: +4 the shooter's controller, whose +F0 is the actor) takes its origin from
 * the actor's eye (its vtable +4C matrix's position, or the actor's +100 with the controller's +107 set) and has 1CEF40
 * work out the direction (toward the weapon's aim point, weapon+10, with spread, or exactly toward weapon+20) and the
 * range: 1CEF40(&origin) with edi = the direction; the hit test (CA170) and a projectile spawn (1DAFF0) both start at
 * that origin. Rockets: the launcher's shot (1E8540, ebx = the shot) launches through 1E35F0(a0 = the launch matrix,
 * RenderWare rows right, up, at, pos: the flat muzzle record's position, at along the aim; a1 = the velocity).
 * Flat, the gun is drawn on the camera: a bullet from the eye or a rocket from the flat muzzle leaves along the gun. In
 * VR the gun is in the hand, so bullets left from between the eyes and rockets from where the flat gun would be, beside
 * the drawn tube (seen in the RPG on the headset).
 * With aim_barrel on (VR STEREO, hand aim, the gun drawn in the hand), the player's own shot is moved with the gun: by
 * the transform the next render gives the weapon (X = w_hand * grip^-1 * aim^-1, bvr_fx_x_now: the flat gun onto the
 * drawn one). A bullet starts at the drawn muzzle (X * the muzzle record's position) along X's turn of the game's
 * direction (spread kept); a rocket's launch matrix becomes X * it and its velocity X's turn of it. So both leave the
 * drawn muzzle along the drawn barrel, wherever the hand is. Only when the muzzle moves under 1.5 m (a recentre or a
 * teleport otherwise: the game's shot is kept). */
static struct { int on; uint32_t actor; } bvr_shot;
static struct { unsigned n, moved; double off_sum, turn_sum; } bvr_shots;

static void black_vr_shot_begin(uint32_t shot)
{
    const VrGameConfig *cfg = vr_game_config();
    uint32_t ctl, actor, core;
    bvr_shot.on = 0;
    if (!vr_active() || !cfg->aim_barrel || cfg->aim_mode != VR_AIM_HAND || bvr.mode != VR_PRESENT_STEREO ||
        !bvr_gun_fx() || !bvr_fxl.ok || !bvr.yv_ok || !black_pc_ram_span(0x2D45D4u, 4u) || !black_pc_ram_span(shot, 8u))
        return;
    core = GUEST32(0x2D45D4u);
    ctl = GUEST32(shot + 4u);
    actor = black_pc_ram_span(ctl + 0xF0u, 4u) ? GUEST32(ctl + 0xF0u) : 0u;
    if (!actor || actor != core + 0x30u) return;              /* the player's own (an AI's shot is left alone) */
    bvr_shot.on = 1;
    bvr_shot.actor = actor;
}
static void black_vr_shot_end(void) { bvr_shot.on = 0; }

/* X's turn of a direction (w = 0) */
static VrVec3 bvr_turn(VrMat4 x, VrVec3 d)
{
    return vrm_v3(VRM_AT(x, 0, 0) * d.x + VRM_AT(x, 0, 1) * d.y + VRM_AT(x, 0, 2) * d.z,
                  VRM_AT(x, 1, 0) * d.x + VRM_AT(x, 1, 1) * d.y + VRM_AT(x, 1, 2) * d.z,
                  VRM_AT(x, 2, 0) * d.x + VRM_AT(x, 2, 1) * d.y + VRM_AT(x, 2, 2) * d.z);
}
/* The drawn barrel's direction for a shot whose game direction is d (from the game's start o toward its target, so
 * converging on the flat crosshair at an angle to the gun): the flat barrel (the muzzle record's at, else the aim
 * matrix's forward) turned by X is the drawn barrel; d's deviation from the unspread direction u (the spread) turned onto it.
 * Its length is d's. 0 when the aim matrix is unreadable. */
static int bvr_barrel_dir(VrMat4 x, VrVec3 d, VrVec3 u, VrVec3 *out)
{
    VrMat4 a;
    VrVec3 f, dn, axis, r;
    const uint32_t va = bvr_aim_va();
    float c, sn, len = vrm_len(d);
    if (!va || !bvr_aim_view(va, &a) || !(len > 1e-6f)) return 0;
    f = vgm_col(a, 2);
    if (!vrm_normalize(&f) || !vrm_normalize(&u)) return 0;
    if (vrm_dot(f, u) < 0.0f) f = vrm_scale(f, -1.0f);
    {   /* the muzzle record's at is the barrel (the UZI's within 2 deg of the aim; the RPG's tube is drawn ~35 deg
         * off it, toward the flat crosshair): taken when within 50 deg of the aim */
        const uint32_t rec = bvr_fp_record(5u);
        VrMat4 m;
        VrVec3 bz;
        if (rec && bvr_rw_read(rec, &m)) {
            bz = vgm_col(m, 2);
            if (vrm_normalize(&bz)) {
                if (vrm_dot(bz, f) < 0.0f) bz = vrm_scale(bz, -1.0f);
                if (vrm_dot(bz, f) > cosf(50.0f * (float)VRM_DEG)) f = bz;
            }
        }
    }
    /* the spread: d turned by the turn taking u onto f (Rodrigues) */
    dn = vrm_scale(d, 1.0f / len);
    axis = vrm_cross(u, f);
    sn = vrm_len(axis);
    c = vrm_dot(u, f);
    if (sn > 1e-6f) {
        axis = vrm_scale(axis, 1.0f / sn);
        r = vrm_add(vrm_add(vrm_scale(dn, c), vrm_scale(vrm_cross(axis, dn), sn)), vrm_scale(axis, vrm_dot(axis, dn) * (1.0f - c)));
    } else {
        r = dn;
    }
    r = bvr_turn(x, r);
    if (!vrm_normalize(&r)) return 0;
    *out = vrm_scale(r, len);
    return 1;
}
static void bvr_shot_log(int ok, const char *what, VrVec3 o, VrVec3 o2, VrVec3 d, VrVec3 d2)
{
    const float upm = vr_game_config()->units_per_metre;
    if (ok) {
        float c = vrm_dot(d, d2) / (vrm_len(d) * vrm_len(d2) + 1e-12f);
        if (c > 1.0f) c = 1.0f;
        if (c < -1.0f) c = -1.0f;
        bvr_shots.moved++;
        bvr_shots.off_sum += (double)(vrm_len(vrm_sub(o2, o)) / upm);
        bvr_shots.turn_sum += acos((double)c) / VRM_DEG;
    }
    bvr_shots.n++;
    if (bvr_shots.n == 1u || !(bvr_shots.n % 100u) || getenv("BLACKVR_AIM_LOG"))
        fprintf(stderr, "[BLACK_VR] shot: %s %s (%u of %u shots moved to the hand; on average %.2f m from the game's start "
                "and turned %.1f deg)\n", what, ok ? "from the drawn muzzle along the drawn barrel" : "kept as the game's",
                bvr_shots.moved, bvr_shots.n, bvr_shots.moved ? bvr_shots.off_sum / bvr_shots.moved : 0.0,
                bvr_shots.moved ? bvr_shots.turn_sum / bvr_shots.moved : 0.0);
}

/* After 1CEF40 in the player's bullet shot: origin and direction to the drawn gun */
static void black_vr_shot_aimed(uint32_t org, uint32_t dir)
{
    if (!bvr_shot.on) return;
    bvr_shot.on = 0;                                            /* once per shot */
    if (!bvr_f32_ok(org) || !bvr_f32_ok(org + 8u) || !bvr_f32_ok(dir) || !bvr_f32_ok(dir + 8u)) return;
    {
        BVR_FP_BEGIN;
        const float upm = vr_game_config()->units_per_metre;
        const uint32_t rec = bvr_fp_record(5u);
        VrMat4 x, h, now;
        VrVec3 o, d, mz = vrm_v3(0, 0, 0), r = vrm_v3(0, 0, 0), dp;
        int fresh, ok = 0;
        o = vrm_v3(bvr_f32(org), bvr_f32(org + 4u), bvr_f32(org + 8u));
        d = vrm_v3(bvr_f32(dir), bvr_f32(dir + 4u), bvr_f32(dir + 8u));
        if (rec && bvr_rw_read(rec, &now) && vrm_len(d) > 1e-6f) {
            float p4[4] = { VRM_AT(now, 0, 3), VRM_AT(now, 1, 3), VRM_AT(now, 2, 3), 1.0f }, q[4];
            x = bvr_fx_x_now(&fresh, &dp, &h);
            vrm_m4_apply(x, p4, q);
            mz = vrm_v3(q[0], q[1], q[2]);
            {   /* the unspread direction: toward the weapon's aim point (weapon+10), else the game's own */
                const uint32_t weapon = black_pc_ram_span(bvr_shot.actor + 0x29Cu, 4u) ? GUEST32(bvr_shot.actor + 0x29Cu) : 0u;
                VrVec3 u = d;
                if (black_pc_ram_span(weapon + 0x10u, 0x10u) && bvr_f32_ok(weapon + 0x10u) && bvr_f32_ok(weapon + 0x18u)) {
                    u = vrm_sub(vrm_v3(bvr_f32(weapon + 0x10u), bvr_f32(weapon + 0x14u), bvr_f32(weapon + 0x18u)), o);
                    if (vrm_len(u) < 0.05f * upm || vrm_dot(u, d) <= 0.0f) u = d;
                }
                ok = vrm_len(vrm_sub(mz, o)) < 1.5f * upm && bvr_barrel_dir(x, d, u, &r);
            }
            if (ok) {
                bvr_put(org, mz.x); bvr_put(org + 4u, mz.y); bvr_put(org + 8u, mz.z);
                bvr_put(dir, r.x); bvr_put(dir + 4u, r.y); bvr_put(dir + 8u, r.z);
            }
        }
        bvr_shot_log(ok, "bullet", o, mz, d, r);
        BVR_FP_END;
    }
}

/* In the player's launcher shot, at the launch (1E35F0): the launch matrix and velocity to the drawn gun */
static void black_vr_shot_launch(uint32_t pos, uint32_t vel)
{
    if (!bvr_shot.on) return;
    bvr_shot.on = 0;
    if (!black_pc_ram_span(pos, 0x40u) || !bvr_f32_ok(vel) || !bvr_f32_ok(vel + 8u)) return;
    {
        BVR_FP_BEGIN;
        const float upm = vr_game_config()->units_per_metre;
        VrMat4 x, h, m, m2;
        VrVec3 v, v2 = vrm_v3(0, 0, 0), dp;
        int fresh, ok = 0;
        v = vrm_v3(bvr_f32(vel), bvr_f32(vel + 4u), bvr_f32(vel + 8u));
        if (bvr_rw_read(pos, &m)) {
            x = bvr_fx_x_now(&fresh, &dp, &h);
            m2 = vrm_m4_mul(x, m);
            ok = vrm_len(vrm_sub(vgm_col(m2, 3), vgm_col(m, 3))) < 1.5f * upm && bvr_barrel_dir(x, v, v, &v2);
            if (ok) {
                /* the launch matrix turned with the velocity (X's turn of the game's onto the drawn barrel), so the
                 * rocket faces its flight */
                VrVec3 f0 = bvr_turn(x, v), f1 = v2, axis;
                if (vrm_normalize(&f0) && vrm_normalize(&f1)) {
                    const float c = vrm_dot(f0, f1);
                    float sn;
                    int col;
                    axis = vrm_cross(f0, f1);
                    sn = vrm_len(axis);
                    if (sn > 1e-6f) {
                        axis = vrm_scale(axis, 1.0f / sn);
                        for (col = 0; col < 3; col++) {
                            const VrVec3 e = vgm_col(m2, col);
                            const VrVec3 t = vrm_add(vrm_add(vrm_scale(e, c), vrm_scale(vrm_cross(axis, e), sn)),
                                                     vrm_scale(axis, vrm_dot(axis, e) * (1.0f - c)));
                            VRM_AT(m2, 0, col) = t.x; VRM_AT(m2, 1, col) = t.y; VRM_AT(m2, 2, col) = t.z;
                        }
                    }
                }
                bvr_rw_write(pos, &m2);
                bvr_put(vel, v2.x); bvr_put(vel + 4u, v2.y); bvr_put(vel + 8u, v2.z);
            }
            bvr_shot_log(ok, "rocket", vgm_col(m, 3), vgm_col(m2, 3), v, v2);
        }
        BVR_FP_END;
    }
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
            /* BLACKVR_AIM_TRACE=3: the emitter's axes and its particles' velocities in the hand-held muzzle's frame
             * (bvr_fxl.x * record 5: x right, y up, z along the barrel), as pitch/yaw degrees off the barrel */
            if (bvr_aim_trace() >= 3 && bvr_fxl.ok && bvr_fxl.at[5] && GUEST32(fx + 0x2440u + i * 4u) == 5u) {
                const VrMat4 mz = vrm_m4_mul(bvr_fxl.x, bvr_fxl.l[5]);
                VrVec3 bx = vgm_col(mz, 0), by = vgm_col(mz, 1), bz = vgm_col(mz, 2);
                VrVec3 ea = vrm_v3(bvr_f32(em + 0x20u), bvr_f32(em + 0x24u), bvr_f32(em + 0x28u));
                VrVec3 eu = vrm_v3(bvr_f32(em + 0x10u), bvr_f32(em + 0x14u), bvr_f32(em + 0x18u));
                VrVec3 ep = vrm_v3(bvr_f32(em + 0x30u), bvr_f32(em + 0x34u), bvr_f32(em + 0x38u));
                const VrVec3 mp = vgm_col(mz, 3);
                VrVec3 vs = vrm_v3(0, 0, 0);
                unsigned np = 0;
                const uint32_t grp = GUEST32(em + 0xA8u);
                vrm_normalize(&bx); vrm_normalize(&by); vrm_normalize(&bz);
                if (black_pc_ram_span(grp, 8u)) {
                    uint32_t blk = GUEST32(grp + 4u);
                    unsigned guard = 0;
                    while (blk && black_pc_ram_span(blk, 0x10u) && guard++ < 16u) {
                        const uint32_t cnt = GUEST32(blk + 8u), dat = GUEST32(blk + 0xCu);
                        unsigned p;
                        if (cnt <= 0x3Cu && black_pc_ram_span(dat, cnt * 0x30u))
                            for (p = 0; p < cnt; p++) {
                                const VrVec3 v = vrm_v3(bvr_f32(dat + p * 0x30u + 0x10u), bvr_f32(dat + p * 0x30u + 0x14u),
                                                        bvr_f32(dat + p * 0x30u + 0x18u));
                                if (isfinite(v.x) && isfinite(v.y) && isfinite(v.z)) { vs = vrm_add(vs, v); np++; }
                            }
                        blk = GUEST32(blk);
                    }
                }
#define BVR_PY(v) (double)(atan2f(vrm_dot(v, by), vrm_dot(v, bz)) * 57.29578f), (double)(atan2f(vrm_dot(v, bx), vrm_dot(v, bz)) * 57.29578f)
                fprintf(stderr, "[BLACK_VR] FXV gf %u slot %u emitter %u: at %.1f/%.1f deg, up %.1f/%.1f, pos off the muzzle "
                        "%.3f %.3f %.3f (x y z), %u particles, mean velocity %.1f/%.1f deg, %.3f per s\n", bvr.game_frame, i, e,
                        BVR_PY(ea), BVR_PY(eu), (double)vrm_dot(vrm_sub(ep, mp), bx), (double)vrm_dot(vrm_sub(ep, mp), by),
                        (double)vrm_dot(vrm_sub(ep, mp), bz), np, BVR_PY(vs), np ? (double)(vrm_len(vs) / (float)np) : 0.0);
#undef BVR_PY
            }
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
static void black_vr_shot_pulse(void) { vr_game_haptic(vr_game_config()->aim_hand & 1, 1.0f, 40u); vr_note_shot(); }
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
