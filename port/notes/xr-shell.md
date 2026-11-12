# xr-shell: Quest app shell, OpenXR, APK, game-side VR hooks

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
is offered) and `khr/simple_controller`. The mapping is pure (`vr_input_map.h`), see README for the
table. Notes:
- Narrowest runtime hook: `xbox_InputGetState`/`SetState` in `xinput_device.c` under `BLACK_VR`.
  Everything downstream is unchanged: `usb_gamepad_report` -> OHCI -> XAPI -> the game's action map,
  Controls pages and prompts, pc_input merge.
- Right stick X never reaches the game in gameplay: it is the host turn (snap 30° with 0.7/0.3
  hysteresis, or smooth 120°/s), handed to the game hook through `vr_consume_turn()`. Right stick Y
  is also zero in gameplay (pitch comes from the hand/head). In flat modes both pass through and
  also drive the d-pad.
- `xinput_device.c` maps XInput LEFT_SHOULDER -> Xbox **Black** and RIGHT_SHOULDER -> **White**.
  Its README table says the opposite, so I followed the code. Right grip -> Black, left grip -> White.
- Rumble: the game's two motors -> left/right controller haptics (50 ms pulses, refreshed per frame).
- Recentre: both stick clicks held 1 s, or the system recentre
  (`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`). The origin is head yaw + position at that moment.

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
