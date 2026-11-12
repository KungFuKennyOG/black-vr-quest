# vr-glue: the Quest XR shell joined to the Kelvin Vulkan backend

Status 2026-11-02. The shell's renderer hooks are implemented on the Kelvin Vulkan backend
(`port/quest/src/vr_renderer_kvk.c`). The shell and the backend now agree on device, swapchains,
queue lock, frame identity, the per-eye correction, the HUD layer and the flat modes. A headless
integration test drives the real shell code, the glue and the real backend on SwiftShader with a
fake OpenXR runtime: **2 configurations, 247 + 249 checks, all pass**. Four deliberate mutations
(no flip re-latch, camera mark ignored, raw pose submitted, correction misapplied) each fail it.
The smoke APK still builds, signs (v3) and aligns, and shows the test pattern without a game.
Nothing has run on a headset.

## Decisions

### 1. One per-eye correction, owned by the backend; the shell owns the poses
- **Where it is applied:** only in the backend's vertex stage, via `kvk_stereo.c` in the
  FROM_W form: `C_i = P_eye_i · E_i · R(P_game)`.
- **What E_i is:** `E_i = inverse(eye_i) · game_head`, using stage poses, the game's view axes
  (read off `P_game`) and translation in game units.
  - `game_head` is `VrGameCameraExt.head_pose`, the pose the game built the camera from.
  - `eye_i` is `VrRenderPose.eye_pose[i]`, which the shell latches once, in
    `vr_acquire_eye_targets`.
- **What is submitted:** the projection layer goes out with exactly `eye_i` and `eye_fov[i]`.
- **So head motion is corrected exactly once in each interval:**
  - From the game's latch to the render latch, by `E_i`.
  - From the render latch to display, by the compositor, which is given `eye_i`.
- **Why the backend and not the shell's `clip_from_game_clip`:**
  - FROM_W keeps parallax exact whatever the game's near/far. The shell's FULL form needs the
    exact depth row; without it, `clip_valid` is 0.
  - The backend already reads the view-space axis signs off `P_game`.
  - Applying the shell's matrix as well would correct twice.
- **What changed to get there:**
  - The backend no longer builds the eye poses from `vr_frame_state()`, which is the game
    latch and may be newer than the frame.
  - `VrEyeTransforms` is still computed, from the same poses and the same camera. It is the
    same correction in the FULL form, for diagnostics.
  - The test proves the shell's FULL form and the backend's FULL and FROM_W forms agree
    (worst NDC difference 3.6e-7).
- **Clamp:** a head beyond `max_head_offset` is clamped at render time as in the game. The
  clamped eye is drawn *and* submitted. Before this change the shell drew the clamped eye but
  submitted the raw one.

### 2. Late latch at the flip, applied to an already-recorded frame
- **At the first draw:** the glue gives the backend preliminary transforms: the game latch's
  eye offsets around `game_head`.
- **At the flip:** `vr_acquire_eye_targets()` late-latches the poses. The glue then recomputes
  the transforms for them, and the backend rewrites this frame's stereo blocks in place
  (`vrl_relatch`).
  - The blocks are host-coherent and already referenced by the recorded draws.
  - The frame is not submitted yet.
- **Result:** the GPU draws the frame for the pose that is submitted with it, as late as the
  submit, at no extra cost. With the test's head motion the correction is about 14 px.
- **Exception:** draws submitted mid-frame (only `kvk_read_surface`, a diagnostic) keep the
  preliminary transforms.

### 3. Frame identity travels with the push buffer (fixes renderer open issue 2)
- **Game thread:** `vr_mark_world_pass(va, 1)` copies the newest published camera and its Ext
  into a 16-slot ring. It then calls `kelvin_mark_view(va, KVK_VIEW_VR_CAMERA = 18, slot, 0, 0,
  640, 480)`.
- **GPU thread:** when the pusher reaches that word, `kgpu_set_view` hands the slot to
  `kvk_vr_view_mark`. That frame then uses that camera, for both the transforms and
  `vr_set_render_frame` → `VrRenderPose.game_frame`.
- **Effect on HUD detection:** the mark does not change draw classification. KG_VIEW_VR_WORLD
  would turn HUD auto-detection off for the rest of the frame.
- **Frames without a mark** (menus, FMV, or while the game hook marks STEREO frames only) use
  the newest camera and the game's current `vr_present_mode()`.
- **Tested:** camera 2 is published before frame 1 is drawn, and frame 1 still uses camera 1.

### 4. One flat mechanism: the shell's quad layer
- **Routing:** the glue says per frame whether it is STEREO or FLAT_* (`kvk_vr_frame_mode`):
  - from the camera mark's `present_mode`,
  - otherwise from `vr_present_mode()`.
- **FLAT_* frames:**
  - The whole picture goes into `vr_acquire_flat_target()`: the main target's layer 0, opaque,
    with the HUD surface blended over it, premultiplied.
  - No eye image is acquired.
- **Placement:** the shell shows the flat image as before. MENU and CINEMA are world-locked at
  2.2 m, 2 m wide. SCOPE is head-locked at 0.9 m.
- **Fallback:** the backend's "virtual screen inside the eye images" is now used only when
  there is no flat target, for example the old `kvk-backend` test with no shell.
- **What the XR thread presents:** what the renderer *released last* (`submit.mode`), not the
  game's current wish. Layers therefore always show one rendered frame. A FLAT→STEREO switch
  can no longer pair the projection layer with a stale flat image, or show an empty quad.

### 5. HUD as a real OpenXR quad layer (renderer request 2)
- **When:** in a STEREO frame with HUD draws.
- **Image:** the backend copies its mono 1280×960 HUD surface into `vr_acquire_hud_target()`.
  It keeps premultiplied alpha: transparent where nothing was drawn.
- **Layer:** the shell adds an `XrCompositionLayerQuad` in VIEW space, after the projection
  layer, with `BLEND_TEXTURE_SOURCE_ALPHA`.
  - It is placed at `hud_distance` (3.0 m) and `hud_width` (2.4 m), 4:3. These are new
    `blackvr.ini` keys and match the backend's virtual screen.
- **Pairing with the eye images:** the HUD image is published *with* `vr_release_eye_targets()`
  under the queue lock. A HUD is never shown over another frame's world. A STEREO frame
  without HUD draws shows no HUD layer.
- **Why the compositor and not the eye images:** a compositor quad is sharper, with one
  resample instead of two. It is also locked to the display-time head pose.

### 6. Device, swapchains, queue (renderer requests 3–5)
- **Device features:**
  - The glue asks for `robustBufferAccess`, `depthClamp`, `samplerAnisotropy` and
    `textureCompressionBC`.
  - The shell enables each one the device has. Asking for a missing feature fails
    vkCreateDevice, so missing ones are logged and left off.
  - It always enables multiview and uses Vulkan ≥ 1.1.
  - What is on is published as `vr_vulkan_features()`. The glue fills `KvkDeviceInfo` from it.
- **Swapchains:**
  - Usage: COLOR_ATTACHMENT | SAMPLED | TRANSFER_DST | MUTABLE_FORMAT, sRGB.
  - With `XR_KHR_vulkan_swapchain_format_list` (enabled when offered), the sRGB and UNORM
    formats are listed so Adreno can keep UBWC compression on a mutable image.
  - `swapchain_mutable_format = 1` makes the backend write through UNORM views. The test
    checks the gamma-space bytes arrive unchanged: 0.1 → 26, not 89.
  - Eyes use one 2-layer array (multiview, the default) or two swapchains; both are tested.
    Flat and HUD are each 1280×960.
- **Queue lock:**
  - The glue passes `vr_queue_lock/unlock` as `KvkDeviceInfo.queue_lock`.
  - **Fix:** `xrAcquireSwapchainImage` now also takes the lock. XR_KHR_vulkan_enable2 lists it
    with xrBeginFrame, xrEndFrame and xrReleaseSwapchainImage as calls that may use the queue.
    `xrWaitSwapchainImage` stays unlocked.
  - The fake runtime checks that every one of these calls holds the lock.

### 7. Startup order, fallback, shutdown
- **Startup:**
  - The shell calls `vr_renderer_init` only when a game will start (`want_renderer` = game
    files found and `black_game_main` linked). It does so before the game thread exists, so
    `kvk_set_device/raster_size/stereo/cache_dir` are always in place before `kelvin_start` →
    `kgpu_init`.
  - The glue also sets `RECOMP_NV2A_KELVIN=1` unless the user set it.
- **Fallback:** the test pattern keeps running until the renderer first acquires a target.
  - This covers game boot, or a game that never produces a frame.
  - `tp_lock` hands the swapchains over, so the two never touch them at the same time.
  - With no game, it is the test pattern only, as before.
- **Shutdown:** `vr_renderer_shutdown` only stops the hooks. `kvk_shutdown` is not called,
  because the Kelvin GPU thread may still be inside the backend. With a running game, the
  shell leaves Vulkan and OpenXR objects to the `_exit` that follows, and does not destroy
  them under that thread.

## Call order and threads

```
XR thread    vr_renderer_get_requirements          (before the VkInstance)
             xrs_vulkan_create: instance/device (features ∩ device), queue
             xrs_session_create: swapchains (eyes, flat, HUD), actions, test pattern,
             vr_renderer_init(ctx) -> kvk_set_device / set_raster_size(eye w,h) / set_stereo(true) / set_cache_dir
             [android_main starts the game thread]
game thread  ... kelvin_start() -> GPU thread -> kgpu_init()  (uses the kvk_set_* above)
             per frame: black_vr_look -> vr_latch_frame_state; D2F40 -> vr_publish_game_camera_ex(cam, ext{head_pose, game_frame, mode})
                        1EE820 -> vr_mark_world_pass(va,1) -> kelvin_mark_view(va, 18, slot)
GPU thread   kgpu_set_view(18, slot) -> kvk_vr_view_mark: this frame's camera
             first draw -> kvk_vr_stereo_hook(FIRST_DRAW): preliminary transforms; vr_set_render_frame(cam, ext, mode)
             kgpu_flip -> compose: FLAT_*  -> vr_acquire_flat_target, picture + HUD
                                   STEREO -> vr_acquire_eye_targets (late latch -> VrRenderPose)
                                             kvk_vr_stereo_hook(FLIP) -> stereo blocks rewritten
                                             vr_acquire_hud_target (if HUD drawn), compose eyes, copy HUD
                       -> vkQueueSubmit (queue lock)
                       -> vr_release_flat_target | vr_release_hud_target + vr_release_eye_targets (commit poses + HUD)
                       -> kvk_vr_frame_done
XR thread    xrWaitFrame, input, xrBeginFrame; under the queue lock: layers from what was released last
             (projection with the committed poses [+ HUD quad] | flat quad), xrEndFrame
```

## Interface additions

**`port/quest/vr_bridge.h`** (append-only; nothing above changed):
- **Contract wording:**
  - `VrGameCamera.proj` is the D3D clip the title's world vertex programs produce. Its x, y
    and w rows must be exact.
  - `world_scale` is game units per metre.
  - The per-eye correction is applied once, by the backend.
- `VrRenderPose`, plus `vr_set_render_frame(cam, ext, present_mode)` and `vr_render_pose()`:
  frame identity, and the poses drawn for = the poses submitted.
- `vr_acquire_hud_target` / `vr_release_hud_target` and `VrHudLayout` / `vr_hud_layout()`.
- `VrVulkanFeatures` / `vr_vulkan_features()` and `vr_cache_dir()`.

**`third_party/xboxrecomp/src/nv2a/kelvin/vk/kvk_vr_hooks.h`** (new): the hooks the glue
implements. Each has a weak default that keeps the backend's earlier behaviour.
- `kvk_vr_stereo_hook(out, cfg, phase)`
- `kvk_vr_frame_mode()`
- `kvk_vr_view_mark(mode, slot)`
- `kvk_vr_frame_done()`
- `KVK_VIEW_VR_CAMERA` (18)

## Files

New:
- `port/quest/src/vr_renderer_kvk.c`: the glue. In the game build it is
  `black_vk_renderer`, defined in `port/quest/CMakeLists.txt` and linked whole-archive.
- `third_party/xboxrecomp/src/nv2a/kelvin/vk/kvk_vr_hooks.h`
- `third_party/xboxrecomp/src/nv2a/kelvin/vk/kvk_vr_layers.inc`: the flat and HUD passes,
  the re-latch, weak defaults.
- `tests/vr_glue/`: `CMakeLists.txt`, `test_vr_glue.c`, `fake_openxr.[ch]`,
  `shell_access.[ch]`, and `stubs/` (host stand-ins for `<android/log.h>`, `<jni.h>` and
  native_app_glue).
- `port/notes/vr-glue.md`

Changed:
- `port/quest/vr_bridge.h` (append)
- `src/vr_bridge.c`:
  - render frame and pose, HUD target, commit of the drawn poses + HUD + mode
  - camera ring 3→8, copied under the lock
  - `vr_vulkan_features`, `vr_cache_dir`, `vr_hud_layout`
- `src/xr_session.c`:
  - format-list extension, HUD swapchain, renderer gating
  - test pattern until the first renderer frame
  - queue-locked acquire
  - layers from what was released, HUD quad
  - shutdown with a live game
- `src/xr_vulkan.c`: renderer features ∩ device features, recorded; swapchain format list;
  HUD swapchain destroy.
- `src/xr_shell.h`, `src/xr_main.c`: cache dir, `want_renderer`, HUD keys in the default ini.
- `src/vr_config.h`: `hud_distance`, `hud_width`.
- `src/vr_game_math.h`: `vgm_eye_transforms` reports the clamped eye pose, which is what is
  drawn and submitted.
- `CMakeLists.txt`: the `black_vk_renderer` target.
- `README.md`

**`kvk_backend.c`** (16 lines, all marked):
- `#include "kvk_vr_hooks.h"`.
- `stereo_prepare`: call the hook, falling back to the old two lines.
- `#include "kvk_vr_layers.inc"` before `compose_eyes`.
- `compose_eyes`: `vrl_compose_flat` first, `vrl_relatch` after `stereo_prepare`, and
  `vrl_hud_layer` before composing.
- `flip_now`: `vr_release_eye_targets()` → `vrl_after_submit(composed)`.
- `kgpu_set_view`: forward VR marks; camera mark → `V.prepared = false`.
- The D3D11/x86 build does not compile any vk file, so it is unchanged.

## Tests

Build and run on SwiftShader:

```sh
. /root/xtool/env.sh   # the NDK's glslc builds the shell's test-pattern SPIR-V
cmake -S tests/vr_glue -B build-vr-glue -G Ninja -DKVK_GLSLANG_DIR=/root/deps/glslang \
  -DKVK_VULKAN_HEADERS=/root/deps/Vulkan-Headers/include \
  -DKVK_TEST_ICD=/root/build/swiftshader/Linux/vk_swiftshader_icd.json
cmake --build build-vr-glue && ctest --test-dir build-vr-glue --output-on-failure
```

| Test | Result |
|---|---|
| `vr-glue-multiview` (one 2-layer eye swapchain) | 247 checks, 0 failed |
| `vr-glue-two` (two eye swapchains) | 249 checks, 0 failed |
| `tests/kelvin_vk` regression: stereo, shadergen, backend cpu-parity, uber, stereo (no glue, weak defaults) | 5/5 pass |
| `tests/xr_shell`: math 139, input 40, hooks 234, hooks-flipped 234 | 4/4 pass |
| `vr_renderer_kvk.c` compiled for arm64 with `-Werror` | clean |
| `black_vk_renderer` built in the `--with-game` configuration | builds |
| `kvk_backend.c` arm64 syntax check | clean |

What the integration test covers:
- **Device:**
  - Vulkan 1.1 instance, multiview on.
  - robustBufferAccess, depthClamp and BC on exactly where present.
- **Swapchains:** usage, MUTABLE_FORMAT, sRGB+UNORM format list, array size.
- **Fallback:** the test pattern until the first renderer frame, submitted with its own poses.
- **Frame identity:** camera 2 is published before frame 1 is drawn, and frame 1 is drawn
  with camera 1 (`game_frame`, `game_head`).
- **Pose identity:**
  - The submitted pose and FOV are bitwise equal to `VrRenderPose`, with matching latch serial.
  - In both eyes, the world quad lands within 1 px of where the *submitted* pose and FOV put
    it, and about 14 px from where the game-time pose would put it.
  - This holds under head motion of up to 2 rad/s between latch and submit.
- **One correction:** the shell's FULL form equals the backend's FULL and FROM_W forms.
- **HUD:**
  - The layer is in view space, 2.4×1.8 m at 2 m, premultiplied.
  - The HUD square is opaque and the rest transparent.
  - No HUD pixels in the eye images.
  - No HUD layer when no HUD was drawn.
- **Flat MENU** (marked) **and flat SCOPE** (unmarked, from `vr_present_mode`):
  - The image goes to the flat quad only, with its overlay.
  - No eye image is acquired.
  - Correct space and placement.
- **Clamp:** the clamped eye is drawn and submitted.
- **Locking:** every queue-using OpenXR call held the queue lock.
- **Concurrency:** an XR thread runs `xrs_frame` concurrently with 16 game+GPU frames. Over
  about 100 submitted views, every one paired each released image with its render pose, and
  every frame's pixels matched.

## Requests for the game hooks (`src/black_vr.inc`)

Already done there, and reconciled:
- the camera is published from D2F40 and driven from the head pose (body × head);
- the FOV is widened to the eye union × `fov_margin`;
- the viewmodel scale is 1 in stereo (`black_vm_scale_bits`).

Still requested:
1. **Mark every VR frame, not only STEREO.** In `black_vr_world_mark`, replace
   `if (bvr.mode != VR_PRESENT_STEREO || !black_pc_ram_span(0x0027BFF8u, 4u)) return;` with
   `if (bvr.mode < 0 || !black_pc_ram_span(0x0027BFF8u, 4u)) return;`.
   - Then FLAT_CINEMA and FLAT_SCOPE frames carry their mode in the push buffer as well.
   - Otherwise a frame drawn just after a mode switch is presented with the new mode.
   - Update the `tests/xr_shell` expectation "world-pass marks happen in stereo only".
2. **Publish the projection the title really uses.** The backend needs the x, y and w rows of
   `cam.proj` to be exact, and the clip-y sign right.
   - In `BlackVr`, add `double p00, p02, p11, p12; int proj_ok;`.
   - In `bvr_learn`, after `bvr.depth_ok = 1;`, add
     `bvr.p00 = c.p00; bvr.p02 = c.p02; bvr.p11 = c.p11; bvr.p12 = c.p12; bvr.proj_ok = 1;`.
   - In `bvr_publish`, after `proj = vgm_game_proj(...)`, add:
     ```c
     if (bvr.proj_ok && bvr.hand_locked) {
         memset(proj.m, 0, sizeof(proj.m));
         VRM_AT(proj, 0, 0) = (float)bvr.p00;                    VRM_AT(proj, 0, 2) = (float)bvr.p02;
         VRM_AT(proj, 1, 1) = (float)(bvr.clip_y_sign * bvr.p11); VRM_AT(proj, 1, 2) = (float)bvr.p12;
         VRM_AT(proj, 2, 2) = bvr.depth_a; VRM_AT(proj, 2, 3) = bvr.depth_b; VRM_AT(proj, 3, 2) = 1.0f;
     }
     ```
   - Log once if `1/p00` differs from `half_fov_x` by more than 1%: that would mean the lens
     window is not what the title projects with.
3. **Optional:** call `kelvin_mark_view(va, KG_VIEW_VR_SCREEN, 0, 0, 0, 640, 480)` where the
   HUD starts, if auto-detection misfires on device ("frame N: HUD begins" in the log).
4. **Note:** in hand-aim mode the game's crosshair sits at the centre of the head-locked HUD,
   not on the hand ray. Hide it in `VR_AIM_HAND`, or replace it with a reticle (follow-up).

## Notes for the Kelvin core / backend owner
- `eye_view()` calls `vkDeviceWaitIdle` without the queue lock when its 16-entry cache
  overflows. A device wait needs every queue externally synchronised.
  - This cannot happen with 3-image swapchains: at most 12 views.
  - Wrap it in `queue_lock()` anyway.
- The flat and HUD passes sample the main target, which is at eye resolution (for example
  1584 px high), down to 960 px with linear filtering and no mips. They could look aliased on
  fine text; a box filter or a mip would fix it.

## First things to check on a Quest
1. Logs: `adb logcat -s BlackVR KVK`.
   - "device features: multiview 1 robustBufferAccess 1 depthClamp 1".
   - "sRGB+UNORM format list". If it is missing, check GPU time for lost UBWC.
   - "Kelvin Vulkan backend attached".
   - "renderer took over the display".
   - At exit, "N camera marks made (0 refused), N reached".
2. The test pattern shows during game boot and disappears at the first menu frame. The menu
   is a world-locked 2 m screen.
3. In gameplay:
   - The world stays still under fast head turns. Wrong pose identity shows as the world
     swimming with head motion.
   - The world is at the right scale (`world_scale`).
   - The HUD is crisp at 2 m and moves with the head.
   - There is no HUD ghost in the world.
4. Fast head turns show missing geometry at the eye edges if `fov_margin` (1.10, about +2.6° at
   a 50° half-FOV) is smaller than the late-latch rotation. 200°/s × 30 ms is 6°. Try 1.2–1.25
   and watch vertex cost.
5. Scope (FLAT_SCOPE head-locked window) and cutscenes (FLAT_CINEMA) switch cleanly: no
   one-frame flash of the wrong layer.
6. Run once with the Vulkan validation layers. Watch for:
   - layout errors on the swapchain images, which are expected in COLOR_ATTACHMENT_OPTIMAL;
   - UNORM views of sRGB swapchain images without MUTABLE_FORMAT, if the runtime ignores the
     flag;
   - queue-lock races.
