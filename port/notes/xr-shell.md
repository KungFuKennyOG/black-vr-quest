# xr-shell: Quest app shell, OpenXR, APK, game-side VR hooks

Status 2026-11-12: in-headset settings panel (left stick click in a menu), see port/notes/settings-panel.md.
Status 2026-11-12: VR controls (actions through the live action map, gestures) added, see Input.
Status 2026-11-02: **the smoke APK builds, packages and signs**: `/root/black/build-quest/blackvr.apk`
(924 KB, v3-verified, zipalign -p 16 OK, `aapt2 dump badging` clean). **Native tests: 4 suites
pass** (math 139 checks, input 40, hooks 228, hooks with the opposite convention 228). Nothing
has run on a headset or with the real game yet.

## Files

| Path | What |
|---|---|
| `port/quest/vr_bridge.h` | shared contract, **original fields and signatures unchanged**; additions are appended (see below) |
| `port/quest/src/xr_main.c` | `android_main`: game folder, `blackvr.ini`, XR startup, game thread, loop |
| `port/quest/src/xr_session.c` | instance (`XR_KHR_loader_init_android`, `XR_KHR_android_create_instance`), system, session, STAGE/LOCAL, actions, frame loop, layers, refresh rate, perf levels |
| `port/quest/src/xr_vulkan.c` | Vulkan through `XR_KHR_vulkan_enable2`, swapchains + views, test scene, weak renderer stubs |
| `port/quest/src/vr_bridge.c` | the contract's implementation (frame latch, late latch, game camera, pad, rumble, Vulkan context) |
| `port/quest/src/vr_math.h`, `vr_game_math.h`, `vr_input_map.h`, `vr_config.h` | header-only pure C: the testable core |
| `port/quest/shaders/testpattern.*` | ray-cast test scene (floor grid, pillar ring, stereo cube, controller beams) |
| `port/quest/AndroidManifest.xml`, `res/` | NativeActivity, `hasCode=false`, Quest metadata |
| `port/quest/CMakeLists.txt`, `cmake/` | NDK build of `libblackvr.so`; game linkage option |
| `port/quest/build_apk.sh` | loader (once) -> shell -> aapt2 -> zip -0 libs -> zipalign -p 16 -> apksigner |
| `port/quest/gradle/` | optional Gradle/AGP project for a PC (not run here: no Maven) |
| `port/quest/README.md` | sideloading, where the disc files go, controls, status |
| `src/black_vr.inc` | the game-side hooks (new) |
| `src/recomp_manual.c` | +48 lines, all in `#ifdef BLACK_VR` (listed below) |
| `src/main.c` | +6 lines: under `BLACK_VR`, `main` becomes `black_game_main` (CRLF kept) |
| `third_party/xboxrecomp/src/input/xinput_device.c`, `CMakeLists.txt` | `#if defined(BLACK_VR)` pad backend before the `_WIN32` one; CMake skips SDL2 under `BLACK_VR` |
| `tests/xr_shell/` | native tests + `extract.py` (pulls `BLACK_VR` block + `black_pc_ram_span` like tests/viewmodel) |
| `/root/deps/build-openxr-android/src/loader/libopenxr_loader.so` | Khronos loader 1.1.65 built from `/root/deps/OpenXR-SDK`, arm64, android-29, c++_static |

Run the tests: `cmake -S tests/xr_shell -B build-xr-shell && cmake --build build-xr-shell && ctest --test-dir build-xr-shell`.

## Verified vs unverified

Verified here:
- The loader builds from source. It exports `xrGetInstanceProcAddr` and the core entry points
  and needs only libdl/liblog/libandroid/libm/libc.
- `libblackvr.so` builds with `-Wall -Wextra` and has no warnings. It is linked with `--no-undefined`,
  exports `ANativeActivity_onCreate`, and uses 16 KB max page size.
- The APK has the right package, permissions, `<queries>`, features and `native-code: arm64-v8a`.
  Libs are stored uncompressed, 16 KB aligned (`extractNativeLibs=false`). The signature verifies.
- `-DBLACKVR_WITH_GAME=ON` configures and links with every game target stubbed (warnings name them).
- `black_vr.inc` and the `BLACK_VR` xinput backend compile for aarch64-android29 with the NDK
  (sse2neon in place of xmmintrin). The arm64 syntax-check errors in `recomp_manual.c` are the same
  set with and without `-DBLACK_VR` (pre-existing: x86 intrinsics headers and `windows.h`).
- `scripts/generate.py` `flatten_manual()` inlines `black_vr.inc`. The `--exclude-manual` path is fine,
  and the include is `#ifdef`'d anyway.
- tests/viewmodel still passes (9912 checks, both modes) with the new viewmodel hook lines.
- Native tests cover these:
  - **math:** pose algebra, Vulkan and D3D-LH projections, RW frame rows <-> LH view.
  - **per-eye transforms:** a world point maps through `eye_from_game_view` / `clip_from_game_clip`
    exactly as through the eye's own view. Disparity = IPD. A late-latched head move shifts the
    world. Head clamp.
  - **yaw:** yaw-sign from the game's own vectors in both handednesses, yaw calibration (noise,
    axis lock, re-seed), the body-yaw absorb/drift rule.
  - **aim and lens:** hand-aim convergence, FOV union incl. canted displays, bob filter attenuation.
  - **weapon:** first-person MVP factorisation and re-targeting (incl. down-pointing clip y, an
    ortho matrix refused), weapon-to-hand placement.
  - **input:** axis/deadzone, buttons, Black/White mapping, snap hysteresis, smooth turn, menu
    passthrough, chords, left-handed, head-relative stick rotation, `blackvr.ini` parsing.
  - **hooks over synthetic guest RAM**, the exact extracted `BLACK_VR` code: present-mode choice
    (stereo/scope/menu/cinema/photo), lens widen + restore, aim follows the hand while the view
    follows only the head, head yaw/pitch, snap turn in view and aim, injected shake/roll
    removed with a level horizon, world uploads untouched. Convention learning (depth row,
    screen-right sign, clip y) works, including a flipped-convention game. Weapon uploads
    are re-targeted only inside the owner bracket, and world-pass marks happen in stereo only.

Not verified (no headset, no runtime, no game here):
- Anything at run time on the Quest: OpenXR calls, `XR_KHR_vulkan_enable2` device creation, the
  multiview feature enable, the swapchain formats offered, the test-scene shader on Adreno,
  Touch Plus binding acceptance, refresh-rate switching, the All-files-access intent (JNI).
- That the hook addresses behave as the code assumes on the real game. Each assumption has a
  runtime self-check that logs (`[BLACK_VR] ...` on stderr), but they are unexercised.
- The Gradle project (no Maven here). `ndkVersion` and the loader AAR version may need bumping.

## Threading and timing (decision)

Three loops, **fully decoupled**. None waits on another except for VkQueue access.

1. **XR thread** (`android_main`): `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` at the display rate
   (72 Hz default, 90 Hz optional). Each frame it syncs actions (pad, hands, haptics) and locates
   the head. It submits a projection layer with the **most recently released** eye images and
   **exactly the poses they were rendered with**, plus a quad layer for flat modes.
2. **Game thread** (`black_game_main`, 16 MB stack): the recompiled game at its own ~60 Hz.
   At the look integrator (once per game frame) it latches a `VrFrameState`
   (`vr_latch_frame_state`). The head and eye poses are predicted for *now + 2 display periods*
   (via `XR_KHR_convert_timespec_time`), because a game frame produced now is seen roughly one
   game frame plus one render frame later. The world camera and aim are built from that latch.
3. **Render thread** (renderer stream): `vr_acquire_eye_targets()` **late-latches** again. It locates
   views for the next display time after now + 1 period, then computes `VrEyeTransforms`: the
   delta from the head pose the game camera was built from to the newest eye poses
   (`eye_from_game_view`, `clip_from_game_clip`). Applying `clip_from_game_clip[i]` to world-pass
   clip positions renders each eye from its newest pose, not the 60 Hz game pose. The
   positional clamp (`max_head_offset`) is applied again here, so a lean cannot pass it at
   render time either.

Comfort effects of this design:
- Head rotation latency is that of the compositor (ATW at 72/90 Hz), independent of the game rate.
  Late latching at render time reduces the reprojection the compositor must do, which reduces
  edge smearing.
- Translation (head lean) is late-latched at render time. The compositor does not do positional
  reprojection for us (no depth submitted), so a game below the display rate shows positional
  judder only on head translation, not rotation.
- World animation updates at the game rate (~60 Hz), shown at 72 Hz (or 90 Hz) with frame
  repeats. 72 Hz is the default because 60 -> 72 repeats 1 frame in 6. At 90 Hz, 1 in 3 repeats
  and animation judder is more visible. A 60 Hz display mode does not exist on Quest 3.
- The game never blocks on `xrWaitFrame`, so a heavy game frame cannot stall head tracking. The
  XR thread keeps resubmitting the last images with their own poses (timewarp).
- The VkQueue is shared with the runtime. `vr_queue_lock()` (recursive) guards every submit. The
  XR thread holds it across layer building + `xrEndFrame`, and `vr_release_eye_targets()` holds it
  across release + pose commit, so a projection layer never pairs an image with another frame's pose.

## Input

The OpenXR action set "gameplay" has per-hand subaction paths. Bindings are suggested for
`oculus/touch_controller`, `meta/touch_controller_plus` (only when `XR_META_touch_controller_plus`
is offered) and `khr/simple_controller`. Pure, unit-tested cores: `vr_input_map.h` (buttons -> game
actions -> pad) and `vr_gestures.h` (swing, duck, crouch sync, scope raise, two-handed aim); `vr_bridge.c`
(`vrb_set_input`, XR thread) runs both every XR frame.

### Actions, not buttons (2026-11-12)
In gameplay (present mode STEREO or FLAT_SCOPE) the controllers produce BLACK's **actions**, and each
action is pressed on whatever Xbox control the game's **live action map** (`[0x2F3BD4]`, 37 slots, read
through `black_pc_input_resolver`, installed with `vr_set_action_resolver` by the game hook) gives its slot.
All four presets (Default/Scout/Assault/Southpaw) and the player's own layout keep working; the move vector
goes to the stick that "move forward" (slot 1) is on (the right one in Southpaw), the other stick stays
centred. Menus and flat modes keep the button pad (`vr_map_menu_pad`): right stick passes through and also
drives the d-pad, face buttons / triggers / Menu (START) pass through, right grip = Black, left grip = White.
Buttons held across a gameplay <-> menu change act only after a release (the A that resumed does not reload;
the Menu held for Back does not press START in the objectives screen).

| Hand (right-handed; `left_handed = 1` mirrors) | Action | Slot (default preset) |
|---|---|---|
| right trigger | Fire (analog on a trigger) | 9 (RT) |
| right A | Reload | 11 (A) |
| a fast swing of the gun hand (right B: nothing, 2026-11-14) | Melee | 32 (B) |
| right stick left/right | turn (snap 30° with 0.7/0.3 hysteresis, or smooth 120°/s), host-side | - |
| right stick up / down | next / previous weapon, one step per push (on 0.7, re-arm under 0.3) | 14 / 15 (d-pad right / left) |
| right stick up/down while the scope is up | the scope's zoom level | 35 (d-pad up) |
| right stick click | Use / pick up / doors | 13 (X) |
| right grip | nothing (`scope_raise = off`: held = scope, scoped weapons only) | - |
| left stick | move (`locomotion = head | aim | offhand`) | 1..4 |
| left stick click, or ducking | Crouch (the game's Toggle Crouch option is respected) | 12 (L3) |
| left trigger | Grenade | 27 (LT) |
| left grip near the gun | two-handed aim | - |
| left X | Health pack | 33 (d-pad down) |
| left Y tap / held 0.5 s | Fire mode / Suppressor (the tap acts on release only if not held) | 30 (d-pad up) / 31 (Y) |
| left Menu tap / held 1 s | Pause / Back (objectives) | 25 (START) / 36 (BACK) |
| both stick clicks 1 s | recentre (neither Crouch nor Use: a click only acts once the other stick has stayed up 60 ms) | - |
| raising a scoped weapon to the eye | the scope (the aim intent, see below) | (16) |

Taps (weapon steps, Y, Menu, swing melee) are pulses of 0.15 s with a 0.1 s gap, queued, and each lasts
until the game has also rendered 2 frames (world cameras published), so a hitching game never misses one
(pause and the menus read the pad once a frame). "Gameplay" for the controls is the mode the game hook last
chose while a camera came in the last 3 s (the display's 250 ms freshness made them flap between the
gameplay and menu mappings on slow frames); the game's state counts as known for 3 s likewise.

**The unassigned slots** (the Controls page's list is `FE_ACTION_*` at 0x2927B4, built by 214820/214960 with
the slot numbers): 34 (White) is **FE_NEXTMUSICTRACK**, the custom-soundtrack "next track" (1EC450 calls
14FE40 on the first-person sound pool `[[2D0D60]+D3C8]`, 10-update cooldown); 35 (d-pad up) is the
**scope zoom level** (1EC450, only for a scoped weapon, descriptor +C0: toggles weapon +102 between the two
zoom levels, "sniper_SetMaxZoom/MinZoom"); 10 (Y) is read only in 1EC450's "any action pressed" check
next to Fire/Melee/Grenade/weapons/Use (no action of its own); 26 (Black), 28 and 29 (d-pad up/down) have
**no reader** in the generated code (no `[0x2F3BD4]` load followed by +68/+70/+74, no slot accessor call).
The VR scheme leaves Black and White unmapped (White would skip a custom-soundtrack track); a harness run
with `BLACKVR_DEBUG_GRIPS=1` pressed both in level 1: no observable effect (no intent, latch, weapon,
crouch or mode change; White has no custom soundtrack to skip here).

### The game's own state (`black_vr_input`, called from the `sub_001EC450` wrapper before the original)
Component (1EC450 ECX) +8C actor, **+40 crouch** (toggled on a press edge when **+102 = Toggle Crouch**,
else held), **+45 aim intent** with toggle latch +104; actor +29C weapon, +2A4 zoom; weapon +EC descriptor
(**+C0 scoped**: aim then always toggles; +B4 / +B8 zoom levels, +BC zoom speed), **+105 aim engaged**,
+102 zoom level, +D8 state; shot record +F4 (+18 magazine, +C the PlayerParams block). Published to the
shell each update (`vr_publish_game_input`, player one only: the actor of the VR look object).

### Gestures (`vr_gestures.h`; every trigger is logged under the BlackVR tag with its measured values)
- **Melee swing**: the gun hand's linear speed relative to the head (XrSpaceVelocity chained to the aim
  space's xrLocateSpace when valid, else pose deltas; the head from pose deltas) above `melee_swing_speed`
  (2.5 m/s) for >= 35 ms and >= 2 samples -> one Melee; 0.6 s cooldown, one per run; > 15 m/s is a tracking
  jump. Tested negatives: walking 1.5 m/s, a 180°/s turn in place (1.6 m/s at 0.5 m), a reload-like 1.8 m/s
  move, a one-frame 5 m/s spike. Log: `melee swing: X m/s`.
- **Duck crouch**: standing head height taken at the first gameplay frame and on every recentre, raised
  slowly (time constant 2 s) when you stand taller, never lowered. Crouch below `duck_ratio` (0.80) x
  standing, stand above +0.08 (0.88). Without a floor (standing < 1 m: LOCAL space) the same fractions of an
  assumed 1.65 m body. Log: `duck: crouch|stand (head, standing, thresholds, game crouch)`.
- **Crouch sync**: Toggle Crouch on (the profile, component +102): one press per body edge when the
  game's crouch (+40) differs, re-pressed (max 3) when the game did not follow within 0.6 s / 6 updates;
  a crouch toggled by the stick click is left alone until the body itself ducks or stands. Toggle Crouch
  off (hold): the crouch control is held while ducked. Game state unknown: one press per edge.
- **Scope raise** (scoped weapons only): the rear sight (aim pose + (0, 5 cm up, 6 cm back)) within
  `scope_raise_distance` (0.15 m) of an eye and the gun within 25° of the view -> up; down beyond 1.5x
  the distance or 40°. The shell turns it into the **aim intent**; the hook writes component +45 with the
  latch +104 held, so the native toggle can never flip it out of step, and writes 0 for every non-scoped
  weapon (they never zoom, never get ADS slowdown or ADS animation). Up + zoom >= `scope_zoom` -> FLAT_SCOPE
  as before. Log: `scope raise: up|down (sight distance, angle, scoped, aiming)`.
- **Two-handed aim**: off-hand grip held while that hand is within 20 cm of the gun hand or of the point
  30 cm ahead of it along the gun, and ahead of it -> the aim pose points from the gun hand to the off hand,
  with the gun hand's roll, blended in over 0.1 s; released, behind the gun hand or > 0.9 m apart -> one-handed.
  It replaces `VrFrameState.hand[aim].aim`, so the game's aim, the weapon placement and the gun latch (which
  locates both hands at display time and applies the same math) all use it. Log: `two-hand on|off`.

### Accuracy (`vr_accuracy`)
BLACK's player spread has **no aim term**. 1CEF40 (called by the shot 1DBD50 from the weapon update 1EEFD0)
spreads each shot by params +8 (Max Spread Angle) plus params +28 (Accuracy Fall Off Over Time) x
min(weapon +E0 / params +2C, 1), where +E0 is the time the trigger has been held (1F0210: grows while
weapon +D8 == 1), scaled by weapon **+E4**. The params are the weapon's PlayerParams block for the player
(1C2760: owner type 2), and 1C2760 sets +E4 = 1.0 for every shot record; the only other writers of +E4 are
the AI's behaviour states (194440: 0.5 / 0 for AI states 1D..1F and 2; 1A7F20: 1.0). Aiming (weapon +105,
1BB110) changes only the zoom (actor +2A4, the lens +750) and the native ADS movement and animation.
So `vr_accuracy = aim` (default) and `hip` both give the game's own spread, and the VR layer never needs
the game's ADS to be accurate: nothing is hooked. The spread's inputs are in `VrGameInputState`
(`BLACKVR_INPUT_TRACE=1` logs them).

### Other notes
- Narrowest runtime hook: `xbox_InputGetState`/`SetState` in `xinput_device.c` under `BLACK_VR`.
  Everything downstream is unchanged: `usb_gamepad_report` -> OHCI -> XAPI -> the game's action map,
  Controls pages and prompts, pc_input merge.
- `xinput_device.c` maps XInput LEFT_SHOULDER -> Xbox **Black** and RIGHT_SHOULDER -> **White**.
  Its README table says the opposite, so I followed the code.
- Rumble: the game's two motors -> left/right controller haptics (50 ms pulses, refreshed per frame).
- Recentre: both stick clicks held 1 s, or the system recentre
  (`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`). The origin is head yaw + position at that moment;
  the duck crouch's standing height is taken again.
- `BLACKVR_DEBUG_GRIPS=1` (experiments only): in gameplay the right grip presses the game's own Aim control
  (the VR scope logic stands down), the left grip Black, both grips White.
- Untuned on a headset: every threshold above (swing speed / time, duck ratios, scope distance / angles,
  two-hand distances, the rear-sight offset). The logs carry the measured values.

### blackvr.ini (input)
`turn`, `snap_degrees`, `smooth_degrees_per_second`, `deadzone`, `left_handed`, `aim`, plus:
`locomotion = head | aim | offhand`, `vr_accuracy = aim | hip`, `melee_swing = on | off`,
`melee_swing_speed` (1..10 m/s, 2.5), `duck_crouch = on | off`, `duck_ratio` (0.5..0.9, 0.80),
`scope_raise = on | off`, `scope_raise_distance` (0.03..0.5 m, 0.15), `two_hand = on | off`.

### Verification (2026-11-12)
- `tests/xr_shell` (`/tmp/claude-0/xrt`): math 139, **input 736** (actions per button through two layouts,
  analog fire / grenade, resolver missing, Use / Crouch clicks, Y and Menu tap vs hold, swing taps queued,
  taps held through a slow game, weapon steps with hysteresis and double pushes, scope zoom step, snap and
  smooth turn, recentre chord never crouching / using, left-handed mirror, menus unchanged, buttons held
  across mode changes, swing true/false positives, duck hysteresis / recalibration / no-floor, crouch sync
  toggle / hold / unknown / retries / slow game, scope raise hysteresis, two-hand math / roll / jitter /
  engage-release, new ini keys), **hooks 447** in both conventions (incl. `black_vr_input` over synthetic RAM:
  published state, the aim intent + latch, player two left alone).
- x86 full game (`build-ctl-x86`, GRX_CONTROLS=1, scripted poses/velocities, `BLACKVR_INPUT_TRACE=1`), level 1
  (logs `build-ctl-x86/run1..3.log`): resolver installed with the default preset (fire 23 RT, reload 16 A,
  crouch 14 L3, use 18 X, weapon 3/2, grenade 22 LT, melee 17 B, health 1, suppressor 19, fire mode 0,
  pause 12, back 13). **Fire**: magazine 15 -> 14 -> 13, fire intent +41. **Reload** (A): reload intent
  +44 seen; the magazine stayed 13/15 (no refill seen: no reserve ammo yet, or the state never got there;
  unverified). **Crouch**: stick click toggles the game's crouch 0 -> 1 -> 0 (Toggle Crouch on in this
  profile); **duck**: head 1.12 m -> "duck: crouch", one press, game crouch 0 -> 1; standing -> one press,
  1 -> 0; ducking while already crouched by the click: no press. **Melee**: B and the scripted 4 m/s swing
  ("melee swing: 4.00 m/s") both set the melee intent +4B, weapon state 28 -> 29 (the melee). **Grenade**:
  intent +43. **Health pack**: X sets latch +108 (1EA730 ran; no pack to use). **Suppressor**: Y tap ->
  latch +107. **Fire mode**: Y hold -> d-pad up tap (the pistol has one mode). **Weapon next/prev**: the
  d-pad taps go out (edge-read by 1F17F0) but the weapon in hand never changed (actor +2BA = 2, slot 0;
  unverified: nothing to switch to found). **Use**: X goes out, no latch change (nothing usable in reach;
  unverified). **Pause**: Menu tap -> FLAT (pause), B resumes. **Back**: Menu hold -> BACK tap goes out, no
  mode change (objectives overlay not checked). **Scope raise** with the non-scoped starting weapon: "up" at
  8 cm / 0 deg, aim stays 0 (never zooms). **Two-hand**: engages at 33 cm. **Spread**: hip and native aim
  (debug grips: aiming 1, zoom 1.08 -> 1.59) fire with the same inputs: scale +E4 1.000, PlayerParams Max
  Spread 0.0000 + 0.3000 over 10 s of firing.
- Not verified: anything on a headset; a scoped weapon (none in level 1; the intent path is unit-tested);
  reload refill, weapon switch, Use and Back effects as noted; the gesture thresholds' feel.

## Game-side hooks (`src/black_vr.inc`)

Addresses and offsets relied on, all taken from recomp_manual.c's own documentation:

| What | Where | Used for |
|---|---|---|
| world camera | `renderer[0x2D67F4] + 0xD450` | the only camera VR changes |
| camera owner / binding | `[0x2D12B8]`; `camera+0x68 == owner+0x740` | player view vs other views (via `black_photo_world_camera`) |
| scripted camera flag | `owner+0x16A4` (clear = scripted) | FLAT_CINEMA |
| lens angle | `owner+0x750` (horizontal degrees) | computing the VR window |
| view window / aspect | `camera+0x70` / `+0x74` (D2F40 input) | widened for both eyes, restored right after `sub_000D2F40_gen` |
| half-frustum | `camera+0x48` / `+0x4C` (D2F40 output) | published `half_fov_x/y` |
| RwCamera frame | `[[camera+0x58]+4]`, rows right/up/at/pos at `+0x10` (modelling) and `+0x50` (LTM) | replaced by body×head (as photo mode does) |
| RwCamera near/far | `rwcam+0x80/+0x84`, trusted only when `rwcam+0x68/+0x6C` == the half-frustum | depth row fallback (**unverified offsets**: RenderWare 3.x layout; the MVP-learned row is preferred) |
| look integrator | `sub_001BD3F0` (ESI = look object): yaw `+0x18`, pitch `+0x1C`, yaw cache `+0x10`, pad smoothing `+0xA0/+0xA4`, clamps `[0x2888C8]`/`[0x295E38]` | VR aim -> game aim (after the pad and mouse look) |
| zoom | `[[look+0x8C]+0x2A4]` (1 = none) | scope switch |
| pause / modal | `black_pc_input_gameplay_blocked_query()` | FLAT_MENU |
| viewmodel ownership | `1D25D0` -> `black_viewmodel_owner(actor)`, draw `172EB0` -> `draw_begin(actor+0x2C8)` / `draw_end` | `black_vr_vm_push/pop` |
| MVP uploads | `16F610` -> `black_viewmodel_constants(c10..c13)`; flash `D4A50` from `0x16B49E` -> `black_viewmodel_muzzle_constants(c112..)` | weapon re-target; world draws teach depth row and conventions |
| world pass | `sub_001EE820` (non-split path); push pointer `[[0x27BFF8]]` | `vr_mark_world_pass(begin/end)` for the renderer |
| camera motion blur | `sub_00168A30`, `effect+0x20` | forced off in stereo |
| reload/aim blur | `sub_001772E0`, `effect+0x34` | forced off in stereo |

recomp_manual.c hook calls (all `#ifdef BLACK_VR`):
- forward declarations before `BLACK_VIEWMODEL_OWNER`
- `black_vr_vm_push` in `black_viewmodel_draw_begin`, `black_vr_vm_pop` in `black_viewmodel_draw_end`
- `black_vr_upload` in `black_viewmodel_constants` and `black_viewmodel_muzzle_constants`
- `#include "black_vr.inc"` after the player-two camera helper (`END BLACK_SPLITSCREEN`)
- `sub_000D2F40`: `black_vr_camera_begin` (and the FOV policy stands down in stereo),
  `black_vr_camera_restore` right after `_gen`, `black_vr_camera_after` at the end (after photo/split)
- `sub_001BD3F0`: `black_vr_look` after `black_mouse_look`
- `sub_00168A30` / `sub_001772E0`: blur off in stereo
- `sub_001EE820`: `black_vr_world_mark(1/0)` around the non-split world draw

### How the camera works
- **View = body frame × tracked head.** Body = the game camera's position (vertical bob
  low-passed, tau 0.2 s, snaps for jumps > 0.6 m), level, facing the world yaw of the host body
  yaw B. B is in game degrees: game-side turns ≥ 20° (respawn, scripted facing) are absorbed,
  smaller drift is dropped, and snap/smooth turns are added. Its mapping to world yaw
  (sign from the game's own screen-right vector, offset learned and low-passed, locked to an
  axis when it settles on one) means **shake, recoil and roll in the game's camera rotation never
  reach the eyes**. Position shake still does, minus the bob filter. This is the "disable camera
  shake / head bob" item: done structurally, not by finding the shake code.
- **Aim**: game yaw = B + hand yaw, pitch = hand pitch (relative to the recentre origin), written
  after the pad/mouse integrator with the game's clamps. The game's shots, AI awareness,
  crosshair and audio listener follow the hand. `aim = head` uses the head direction.
  Head-relative locomotion rotates the move stick by (head yaw − aim yaw).
- **Lens**: for D2F40 the window is set to `hx / tan(angle/2)` and the aspect to `hx/hy`, with
  hx, hy the both-eye tangent union × 1.10. So the game's culling frustum covers both eyes plus
  late-latch room, and native aim zoom (which only changes the angle) does not change the VR FOV.
  The custom-FOV policy stands down in stereo. Zoom ≥ `scope_zoom` (2.5) -> FLAT_SCOPE: the game
  renders its own zoomed lens flat, shown as a head-locked window. "Scripted FOV zoom outside
  scopes" is thus neutralised for gameplay. Scripted cameras are flat anyway.
  For a scoped weapon (descriptor +C0) the threshold is at most 2.0 (2026-11-13): the sniper's X2 is
  2.33, and in STEREO its full-screen scope mask went to the HUD quad as an opaque black panel
  (sweep issue 2). The 1.10 of the raise animation stays STEREO.
- **Look freshness** (2026-11-13): STEREO needs the look integrator to have run within the last 6
  world cameras (`BVR_LOOK_STALE_CAMS`; two frames of level_07, which builds three per frame). It was
  250 ms of wall time, which turned one gameplay frame flat after any hitch of 0.25 s or more (sweep
  issue 3). `BLACKVR_TEST_LOOK_STALE_MS` adds a wall-clock limit for tests; unset, none.
- **Body yaw at a STEREO start** (2026-11-13): the look integrator runs before the world camera's
  decision and skips while the view is flat, so the first STEREO camera after a flat stretch (level
  start, checkpoint, menu) built its view from an unset or old body yaw: the first frame of every level
  faced 55-96 deg off (sweep issue 1). `black_vr_camera_after_world` now steps the body yaw from the
  game's facing first (the look's own step: unset -> anchored; a change of 20 deg or more absorbed).
- **Tutorial hints that stop the game** (2026-11-13, Kenny: they had stopped showing, and when they
  showed they dropped to the flat screen). Two separate things:
  - *Why they stopped.* `sub_0012F650` queues hint `esi` (through `12B3C0`) only if its shown byte
    `[2DB670+id]` is clear (64 bytes, cleared at mission load), the frontend's level selection
    `[2D45F0]+2020C` is 0 (mission 1), and the mission's completion record
    `[2D0D84 + difficulty*0x100 + level*0x20]` (2 = completed, -1 = never; the menus read it) is not
    above 0 on **any** difficulty. Kenny beat mission 1 on Easy (ResultsEasy in his logs), so the game
    stopped showing them everywhere: the game's own behaviour, not the prompts work. VR setting
    `tutorial_hints` (Comfort tab, on by default) wraps `sub_0012F650`: the records read as not
    completed for the call and are restored after it (`recomp_manual.c`; generate.py renames the
    original to `sub_0012F650_gen`).
  - *Which ones stop the game.* `12B3C0` adds the "PRESS A TO CONTINUE" line for the ids at `28AA80`:
    12, 13 (health packs), 23, 87, 24. The game then stops its world clock (so
    `black_pc_input_gameplay_blocked_query` is true and the look integrator stops) but keeps drawing
    the frozen world each frame, blurred by the action blur. `bvr_hint_stop` (element of `1314E0`:
    pending `+22A4` / active `+2110` in that list, state `+1B` 5..8, the pause menu `main+210C8` not open)
    keeps `black_vr_decide` STEREO with the head tracked, the hint on the HUD quad, and lets the action
    blur through (`sub_001772E0`, otherwise off in STEREO). The camera after the hint gets a fresh
    look count, so no flat frame. `BLACKVR_TEST_HINT=id[,hud_updates]` queues a hint in the harness;
    `[BLACK_VR] tutorial hints:` lines log the gate inputs, each hint shown, and the element's state.
- **Self-checks** (logged once):
  - **Screen-right convention:** the RW right row is the screen's left (docs/photo-mode.md). This
    is confirmed by 32 static world-draw MVPs whose view axes match the head frame, and flipped after 8 contrary ones.
  - **Clip y direction:** learned the same way.
  - **Pitch sign:** "positive pitch looks down" (pc_input.h) is checked against the camera's
    at-row and flipped after sustained disagreement.
  - **Depth row** (a, b) is factored out of world MVPs and published as `depth_a/depth_b`.

### First-person weapon in the hand (plan + first implementation)
1. **Done:** each first-person MVP upload (mesh + suppressor through 172EB0's owner bracket, muzzle
   flash through D4A50 from 16B49E) is factored as `P·V·M` → `P·Yv·V·M`. `Yv` moves the weapon from
   where the game drew it (the camera entity = the aim) to the aim hand, with the grip offset
   `weapon_offset` (camera-space metres) landing on the hand origin. P is recovered per draw from
   the MVP (perspective, w = z_view, orthogonal-uniform model-view), so no game projection needs
   reading. Re-targeting waits for the convention lock. The wide-FOV viewmodel scale is reset to 1
   in stereo, so the weapon has true perspective.
2. Next: tune `weapon_offset` per weapon class on device (pistol/rifle/shotgun). Optionally read
   the active weapon from the viewmodel set `[0x2D12C8]` slot `+0x470 + actor+0x328*0x240`
   to pick a preset.
3. Next: the weapon's own animation (recoil, reload) still plays relative to the hand. Consider
   damping the recoil translation from the viewmodel pose matrices (`slot+0x30..0x4C`) if it feels
   wrong.
4. Later: two-handed rifles (left grip pose as the second anchor), hand models, a laser sight from
   the aim pose, and muzzle-origin shots (needs the projectile spawn code; currently the game fires
   from the camera and the shot converges on the hand ray at `aim_converge` m).

## Contract additions (`vr_bridge.h`, appended; nothing existing changed)
- `VrGameCameraExt` + `vr_publish_game_camera_ex` / `vr_game_camera_ext`: the head pose the game
  camera was built from, near/far, depth row, game frame counter, present mode.
- `VR_PRESENT_*`, `vr_set_present_mode` / `vr_present_mode`.
- `VrEyeTransforms` / `vr_eye_transforms()`: per-eye `eye_from_game_view`, `proj` (D3D-LH, game
  depth), `clip_from_game_clip`, pose/fov. Valid between acquire and release.
- `vr_acquire_flat_target` / `vr_release_flat_target`: quad-layer image, 1280×960.
- `VrVulkanContext` / `vr_vulkan_context()`, `vr_queue_lock` / `vr_queue_unlock`.
- Renderer-implemented: `vr_renderer_get_requirements`, `vr_renderer_init`, `vr_renderer_shutdown`,
  `vr_mark_world_pass` (weak no-op defaults in the shell).
- `vr_latch_frame_state`, `vr_sample_head`, `vr_get_recentre` / `vr_request_recentre`,
  `vr_consume_turn`, `VrGameConfig` / `vr_game_config`, `vr_set_rumble`, `vr_get_pad`,
  `VR_PAD_*` bits, `vr_eye_array_view`, `vr_register_render_thread`, `vr_game_camera_fresh`.
- Semantics fixed for existing fields: `VrFrameState.eye[i].proj` is Vulkan RH (near 0.05,
  far 1000 m, y down), `view` = inverse of pose, both in stage space. `world_scale` = game units
  per metre (`blackvr.ini world_scale`, default 1.0, **needs tuning on device**).
  `VrEyeTarget.view` is a 2D UNORM view of one layer of an sRGB image.

## Requests

> Update 2026-11-02 (vr-glue): the renderer requests below and the renderer's requests to the
> shell are settled in `port/notes/vr-glue.md`: the glue `port/quest/src/vr_renderer_kvk.c`, frame
> identity (`VrRenderPose`), HUD quad layer, flat modes on the quad layer only, device features,
> format list, and the queue-locked `xrAcquireSwapchainImage`. The per-eye correction is applied
> once, by the backend, from the poses the shell submits; `clip_from_game_clip` is not applied.

**Renderer (src/nv2a/**):**
1. Define `vr_renderer_get_requirements` (extensions/features) and
   `vr_renderer_init(const VrVulkanContext*)`. Use the shell's VkInstance/VkDevice/VkQueue and
   wrap every queue submit in `vr_queue_lock/unlock`. Call `vr_register_render_thread()` once
   from your submit thread.
2. Per frame in STEREO (`vr_present_mode()`): `vr_acquire_eye_targets()`, then draw the world
   pass (between the `vr_mark_world_pass` begin/end push-buffer positions) once per eye. Prefer
   multiview: `vr_eye_array_view()` and `gl_ViewIndex`. Multiply the game's clip-space
   position by `vr_eye_transforms()->clip_from_game_clip[view]` (when `clip_valid`; otherwise
   identity with a horizontal IPD shift). Viewports and scissors scale from 640×480 to the eye
   size. HUD/2D draws outside the bracket: draw them on a head-locked plane or skip them in
   stereo (the shell has no HUD layer yet). Then `vr_release_eye_targets()`. Leave images in
   `COLOR_ATTACHMENT_OPTIMAL`. Write gamma-encoded colour (UNORM views of sRGB images).
3. In FLAT_* modes: render the normal 640×480 frame and blit it to `vr_acquire_flat_target()`,
   then `vr_release_flat_target()`.
4. Tell me if your vertex path cannot apply a per-view clip matrix (e.g. pre-transformed
   vertices or fixed-function `XYZRHW`), and whether depth is reversed. `depth_a/b` assume
   D3D [0,1].

**Runtime / platform (ARM64):**
1. Provide `port/android/game.cmake` defining `black_game` (src/main.c + recomp_manual.c + gen,
   `-DBLACK_VR`, include dirs `port/quest` and `port/quest/src`) and `xboxrecomp` for Android
   (with `-DBLACK_VR` on `xbox_input`, no SDL2). `port/quest/cmake/game_targets.cmake` is a sketch,
   off by default. It currently stops at the kernel's `find_package(OpenSSL)`.
2. `main.c` under `BLACK_VR` must load `game/default.xbe` relative to the CWD (the shell `chdir`s
   to the folder holding `game/` and `save/`), and must not use `MessageBoxA`/dbghelp/VEH there.
   `#ifdef` those for Android. `black_game_main` runs on a 16 MB pthread, not the process main thread.
3. `black_vr.inc` uses `fenv` + `_mm_getcsr/_mm_setcsr` like the rest of recomp_manual.c, so
   it needs the same sse2neon (or equivalent) shim you use there, `struct timespec`/
   `clock_gettime`, and `xbox_PhysicalAddressOf`.

## World fade-in (Nov 15)
Kenny: "i could see the world loading in at the start of a mission". The game cuts from its loading screen and intro
card (FLAT_MENU) straight to a level still streaming in. The glue (vr_renderer_kvk.c world_fade, at the flip) sets
KvkStereoFrame.fade when the stereo view comes back after more than 0.75 s without it: after a load (> 5 s) black is
held 0.6 s and lifts over 0.8 s (smoothstep), after the pause menu or a scripted scene it lifts over 0.25 s. The
backend (kvk_vr_layers.inc vrl_fade, after the laser) blends black over the main target (the LASER shaders' kind 2,
no depth test); the HUD layer stays on top. `BLACKVR_FADE_S=<seconds>` sets the whole length (45% held); 0 turns it
off, which the x86 harness and the glue test do by default (a frame there takes about a second).

## Open issues / risks
- **No headset run yet.** First run: `adb logcat -s BlackVR` and check the items at the end of the README.
- `world_scale` (units per metre) is a guess (1.0). Photo mode treats game units as metres, so it
  is plausibly right, but tune it on device by matching a door height.
- Hand aim fires from the head position. Bullets converge on the hand ray at 10 m but start at
  the head, so very close targets can be missed by up to the hand offset.
- The near/far offsets `rwcam+0x80/0x84` are not proven for BLACK's RenderWare build. The depth
  row from MVPs takes precedence when world draws arrive.
- If the game's pitch is not in degrees growing downward, the self-check flips the sign but
  logs it. Look for `[BLACK_VR] game pitch grows upward`.
- In stereo the game's HUD (crosshair, ammo) is 2D and would be wrong per eye. The renderer must
  place or drop it, and a head-locked HUD layer is a follow-up.
- Split-screen and photo mode fall back to flat by design.
- Test-pattern mode does not rotate the scene on snap turn. It only shows input arrives.
- The OpenXR loader is 1.1.65 (Oct 2026). Quest runtimes accept `XR_API_VERSION_1_0` instances,
  and the shell requests 1.0.
