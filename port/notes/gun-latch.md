# gun-latch: the first-person weapon follows the controller at display time

Status 2026-11-03. Nothing committed, nothing has run on a headset. Paths are relative to /root/black; `XR` means
`third_party/xboxrecomp`.

## Results at a glance

| What | Result |
|---|---|
| Discriminator | **Provenance plus fingerprint.** The game hook already knows the weapon's MVP uploads (`BLACK_VIEWMODEL_OWNER`). It records the exact re-targeted MVP with the frame's camera mark. The backend classifies a draw as the weapon only if its vertex program builds oPos from those constant registers (static data flow) **and** they hold that MVP bit for bit. |
| Level 1, x86 harness, 3 logged frames, 672 main-target draws each | exactly 2 draws per frame classified: the pistol and both arms, program 463DBDF0. No false positives. The debug tint covers the weapon and arms and nothing else. |
| Weapon shift from a controller turned 5° between the game latch and the late latch | about 33–37 px per eye. Textured patches match the hand delta's prediction with a median error of 3.2 / 2.4 px. |
| World in the same frame | unchanged: 0.004–0.007 % of world pixels differ, against 14–17 % had it moved like the weapon. 99.9 % of the changed pixels belong to the tinted weapon. |
| tests/vr_glue (new gun phase) | multiview 338 checks, two-swapchain 342 checks, 0 failed (was 247 / 249). Two mutations caught. |
| tests/xr_shell hooks | 439 checks, both variants pass |
| Other suites | kvk-tests 5/5 (see the flake note), kvk-android 2/2, nv2a-x86 10/10, nv2a-a64 6/6, xrt 4/4, boot-a64 1/1 |
| Windows | clang `--target=x86_64-w64-mingw32 -E` of `recomp_manual.c` and `main.c` before vs after: identical token streams without `-DBLACK_VR`. With `-DBLACK_VR`, `recomp_manual.c` differs only in the new `black_vr.inc` and `vr_bridge.h` lines. |
| APK | `build-quest-game/blackvr.apk` rebuilt with `--with-game` (27 MB). `zipalign -c -p 16` OK, `apksigner verify` v3 OK. The glue compiles with `-Werror`. |
| `gun_latch` default | **on** |

PNGs (left | right eye) in `build-render-x86/frames/`:
- `gunlatch_before.png` / `gunlatch_after.png`: the requested pair.
- Also `gunlatch_prev.png`, `gunlatch_settled.png`, `gunlatch_tint.png`, `gunlatch_diff.png` (|after − before| × 4) and `gunlatch_report.json`.

## 1. The discriminator study (`RECOMP_KVK_VMLOG=<n>`, x86 harness, level 1)

`RECOMP_KVK_VMLOG=n` logs, for n stereo frames, one line per main-target draw and every clear. Each line holds:
- the classification;
- the program hash and the constant registers its position is made from;
- c10 rows, the c112 w row and the viewport rows c58/c59;
- depth, raster and blend state.

Logging starts after the first frame with a weapon draw, or at the 60th stereo frame.

Per frame there were 672 world draws, then the HUD.

| Candidate | What the log shows | Usable? |
|---|---|---|
| Own projection / near plane | The weapon's MVP (c10..c13) is the world projection: x row (−0.6501, 0, 0, 0), so 1/p00 = 1.538 = the published half-FOV, and w row (0, 0, 1, 0.05). Stereo sets the framing scale to 1. | no |
| Own depth range / viewport | c58 = (320, −240, 16777215, 0) and c59 = (320.53, 240.53, 0, 0), identical to every world draw; zclip 0..16777215 | no |
| Depth clear before the weapon | one clear per frame, at the start (colour + depth + stencil). None before the weapon. | no |
| Distinct model-view | The weapon mesh is in camera space and CPU-animated (the MVP is constant from frame to frame; the animation is in the vertex data). With the hand weapon on, its MVP is the re-targeted one: unique, but only the game knows it. | via the game |
| Draw order | draws 670–671 of 672, then one pre-transformed draw, then the HUD. The order is not structural; effects can follow. | no (fragile) |
| Program hash | 463DBDF0 drew only the weapon here (2 draws), but it is a generic skinned-mesh program, and characters could share it. | no (fragile) |
| Same state otherwise | LEQUAL, depth write on, no blend, back-face culling, textured, exactly like world props | no |

So nothing in the push buffer alone tells the weapon apart from the world. The robust signal is the game's own
provenance:
- 1D25D0 submits the first-person mesh with the actor+2C8 material parameter.
- The 172EB0 bracket (`black_viewmodel_draw_begin/end`) already turns that into `black_vr_vm_draw`.
- Its 16F610 MVP upload (c10..c13) is what `black_vr.inc` re-targets to the hand. The muzzle flash's D4A50 upload (c112..c115) is re-targeted the same way.

**Chosen.** A draw is the weapon only when all of these hold:
1. It is a stereo world draw (main target, before the HUD, perspective position) drawn by a GPU vertex program.
2. Its program's oPos depends, by absolute address, on the four registers a weapon MVP was uploaded to. This is
   `kvk_vsh_pos_consts`, a 192-bit data-flow set computed once per program.
3. Those four registers hold one of the MVPs recorded for **this frame's camera mark**, all 64 bytes equal.
4. That frame's `VrGameGun.valid`: the game really attached the weapon to the tracked hand.

**Why it is robust.** Geometry placed through the exact matrix the game made for the weapon this frame *is* the weapon.
A second pass of the weapon would also match, and moving it with the weapon is right.

**What it rejects:**
- Stale constants: a later draw whose program does not read those registers. The log shows exactly this: the
  pre-transformed draw right after the weapon still has the weapon's c10..c13 and is rejected.
- A near miss differing by one ulp (tested).
- Frames whose mark was refused (no MVPs are recorded for them).
- Unmarked frames.
- Frames where the hand is not tracked or `gun_latch` is off.

Anything uncertain stays world: it gets no correction, which is the fail-safe side.

## 2. Implementation

**Game thread** (`src/black_vr.inc`, `src/recomp_manual.c` `BLACK_VR` blocks):
- `black_vr_camera_after_world` publishes `VrGameGun` (the stage aim pose `f->hand[aim].aim` the weapon was placed at,
  the hand index, and `valid = yv_ok && hand_locked`) just before `bvr_publish`.
- `black_vr_upload_at(upload, reg)` calls `vr_mark_viewmodel_mvp(r, reg)` after a successful re-target, with exactly
  the 16 floats written back.
  - `reg` is 10 for the mesh (`black_vr_upload`).
  - It is 0x70 = 112 for the muzzle flash.
  - `sub_00269930` writes the register number straight into NV097_SET_TRANSFORM_CONSTANT_LOAD, so these are hardware
    indices.

**Bridge** (`port/quest/vr_bridge.h`, append-only section "gun-latch stream"; `src/vr_bridge.c`):
- `VrGameGun`, `vr_publish_game_gun()` and `vr_game_gun()`: the pending pose is stored with the next published camera,
  once.
- `vr_mark_viewmodel_mvp()`: implemented by the renderer glue; `xr_vulkan.c` has a weak no-op.
- `VrRenderGun`, `vr_set_render_gun()` and `vr_render_gun()`:
  - In `vr_acquire_eye_targets`, after the eyes are latched, `latch_gun()` locates the weapon hand's **aim space at the
    same display time t**.
  - It records `{game_hand, hand, valid, serial = VrRenderPose.serial}`.
  - The result is invalid if `gun_latch` is off, the game pose is invalid, or the hand is not located with valid
    position and orientation.
- `VrGunLatchConfig` / `vr_gun_latch_config()`:
  - `enabled` comes from `blackvr.ini gun_latch`.
  - `debug_tint` comes from the env `BLACKVR_GUN_LATCH_DEBUG`: `1` gives half-transparent magenta; a value such as
    `0x80FFFF00` gives that colour (0xAABBGGRR).

**Glue** (`port/quest/src/vr_renderer_kvk.c`):
- **The camera mark's ring slot grows:**
  - `VrGameGun`, copied at the mark;
  - a serial;
  - up to 8 weapon MVPs, recorded after the mark (into the newest *accepted* mark's slot).
- **Order is guaranteed:** the GPU thread reads the slot live under the lock, checked by serial. The game records an MVP
  before it pushes the constants, so a draw is never seen before its record.
- **Hooks:**
  - `kvk_vr_viewmodel_draw()`: the classification above.
  - `kvk_vr_viewmodel_hook(FLIP)`: computes the weapon's eye transforms.
    - It sets D = hand_late · inverse(hand_game), in stage space.
    - It then runs the normal per-eye correction with eye′_i = inverse(D) · eye_i, so E′_i = inverse(eye_i) · D · game_head.
      The weapon lands where the controller is at the eyes' display time; the compositor then treats it like the world.
    - It returns false when there is no delta:
      - the poses are bitwise equal (exactly identity);
      - the latch is invalid or from another latch;
      - |translation| > 0.30 m or the turn is > 40°: a tracking glitch or recentre, so the game's placement is kept.
  - `kvk_vr_viewmodel_tint()`.
- **Counters:** `vr_glue_gun_counters()`, and a line at detach.

**Backend** (`XR/src/nv2a/kelvin/vk/`, Vulkan only):
- `kvk_glsl.c` `kvk_vsh_pos_consts()`. `KvkVs.pos_consts` is set at program creation.
- **Second stereo block:**
  - A second stereo block per chunk (`stereo_vm`) for weapon draws.
  - At the first draw it is a copy of the frame's block, so before the latch the weapon is drawn exactly as before.
  - `vrl_relatch` rewrites both kinds of block at the flip, the weapon's with `kvk_vr_viewmodel_hook`, or the frame's
    transforms if that returns false.
  - The decision is made at record time and the delta at the flip, so the late latch costs nothing extra.
  - The shader is unchanged: the weapon draw only binds another offset.
- `vm_tint()`: the debug tint. It redraws the classified draw with the clear-colour fragment shader, alpha-blended,
  depth LEQUAL, no depth write. Its pipeline is keyed by handle and kept out of the manifest.
- `KvkStats.viewmodel_draws` (appended).
- `RECOMP_KVK_VMLOG` (section 1).

**Config:**
- `blackvr.ini gun_latch = on | off`, default **on**. It is in `vr_config.h` and in the default ini written by
  `xr_main.c`.
- The debug view: `env BLACKVR_GUN_LATCH_DEBUG=1` in `blackvr.ini`, or the environment.

## 3. Tests

**tests/vr_glue** (both swapchain layouts), new phase "gun latch":
- **Setup:**
  - The fake runtime now tracks hands: `fx_hand_pose`, with action spaces mapped to hand and aim/grip.
  - The "game" draws a weapon quad at its hand (+ a grip offset) through its own MVP in c96..c99 and records it.
- **Ini:** `gun_latch` defaults to on, and on/off parse.
- **G1, same hand:**
  - the latch is valid and the delta is identity (< 1e-6);
  - no delta is applied, and exactly 1 draw is classified;
  - the weapon is where the game put it (< 1 px);
  - the world quad sits at its submitted-pose position.
- **G2, the hand moved 6 cm and turned 12° after the game latch:**
  - the weapon lands, in both eyes, where the *late* hand puts it (< 0.4 px), 40 px from the game's placement;
  - the world quad stays (< 1 px);
  - a world quad whose program reads c100..c103 while c96..c99 still hold the weapon MVP stays world;
  - a quad drawn with the weapon MVP changed by 1 ulp stays world;
  - exactly 1 draw is classified and 1 delta is applied.
- **G3:** with `gun_latch` off, the weapon stays at the game's placement.
- **G4:** when the game did not attach the weapon (`valid` 0), it is not classified.
- **G5:** when the hand is untracked at the late latch, there is no delta.
- **Tint (two-swapchain run, env set):** the tint is exactly on the weapon's pixels. The multiview run has no tint.
- **Mutations:**
  - "classify every draw" fails 7 checks (the world quad moves);
  - "never apply the delta" fails 3 checks (the weapon stays at the game hand).

**tests/xr_shell `test_hooks.c`** (the real `black_vr.inc`):
- the re-targeted MVP is recorded bit for bit for c10, and the flash's for c112;
- world uploads are never recorded;
- the camera goes out with the hand pose;
- with an untracked hand: no re-target, no record, `valid` 0.

**tests/game_render_x86**, `GRX_GUN_TEST=1` (registered as ctest `game_render_x86_gunlatch`; the runs here were made by hand with the same commands:
`python3 tests/game_render_x86/gun_latch_check.py build-render-x86/frames`):
- **Setup:**
  - The right controller is tracked (`GRX_HAND`).
  - It is held still in front of the head (0.12, −0.22, −0.35 m), and the head does not sway.
- **Sequence:**
  1. After 20 stereo frames with the weapon in the hand it saves `prev` and `before`.
  2. The controller then turns 5° (`GRX_GUN_YAW`) about the head.
  3. It saves `after`: the first frame whose late latch has the new hand while the game used the old one (identified
     from the render pose and gun snapshot taken at the image's release).
  4. It saves `settled` (the game itself at the new hand) and `tint` (the debug tint switched on at run time).
- **Run 2:** prev, before, after = game frames 38, 39, 40 (consecutive), settled 43, tint 45. 2 weapon draws every frame.
- **The checker, per eye, with weapon pixels taken from the tint:**
  1. Textured 64 px weapon patches that the game did not move from prev to before: the before→after shift equals the
     hand delta's prediction (the submitted eye pose and FOV, depth 0.15–0.8 m).
     - Measured: median 3.2 / 2.4 px; 94 % within 8 px.
     - Pistol patches: about (−32, +2) measured vs (−30.5, 0) predicted.
     - The forearm patches nearest the eye deviate 3–8 px, roughly as 1/depth. That fits the game's own idle weapon motion
       (CPU vertex animation) of a few millimetres between two game frames. The geometry itself is exact to < 1 px in
       vr_glue.
  2. after vs settled: the residual is small (cost 3.6 / 3.0 vs 38.6 / 35.2 unshifted). Shift −7,−8 / −2,−11 px is the
     game's own turn sway over 3 frames.
  3. World away from the weapon: mean |diff| 0.10 / 0.11; 0.007 / 0.004 % changed (> 24/255), against 13.9 / 16.7 % had
     the world moved like the weapon.
  4. 99.89 / 99.94 % of the changed pixels belong to the tinted weapon (at either position).
- **Coverage:**
  - Both runs reached level 1 in STEREO.
  - `game/` metadata md5 was the same before and after.
  - The tint frame shows the pistol and both forearms, and nothing in the world.

**Suites:**
- `/root/build/kvk-tests` 5/5, `/root/build/kvk-android` 2/2, `/root/build/vr-glue` 2/2, `/root/build/nv2a-x86` 10/10,
  `/root/build/nv2a-a64` 6/6, `/tmp/claude-0/xrt` 4/4, `build-boot-a64` 1/1.
- `tests/game_boot_arm64/boot_stubs.c` gained the two new game-side symbols.

**kvk-backend uber / cpu-parity failures (resolved; test bug exposed by this change).**
- Symptom: after this change, these tests failed on some runs: 3 pixels, max 203 (pixels 178, 195, 318).
- **Cause:** the tests' random pixel-shader generator (`rnd_ps`) could set the final combiner's E or F input to EF_PROD (0xF). That setting is circular and invalid.
  - Every GPU shader reads 0 there.
  - The CPU reference (`kpix_shade`) reads an uninitialised `Regs.ef`.
  - This change altered the stack and binary layout, which exposed it. It is not a race, and the renderer was never wrong.
  - Before this change the tests did pass: 15/15 with the pre-change backend.
- **Fix:** `tests/kelvin_vk/test_backend.c` `rnd_ps` now maps EF_PROD to ZERO in E/F. Result: 5/5 on repeated runs.
- **Optional:** add `memset(r.ef, 0, sizeof r.ef)` in `kelvin_pixel.c` `kpix_shade` (shared with Windows; not done).

## 4. Limits

- **Hand weapon only.**
  - The latch needs `aim = hand`, `hand_weapon = 1`, a tracked hand, and the screen convention locked; the game must
    have re-targeted the weapon.
  - In head-aim mode the weapon rides the game camera, which the eye correction already late-latches.
- **Scope / zoom:**
  - High zoom is FLAT_SCOPE: a head-locked flat window with the game's own lens. No eye images, no latch.
  - Low zoom below `scope_zoom` is STEREO and latched normally.
- **Muzzle flash:**
  - Wired: c112 is recorded from D4A50's player-flash path, and the flash program 78E49D77 reads c112..c115 into oPos.
  - Not exercised: the harness never fires.
  - Other D4A50 effects (world) record nothing and stay world.
- **Hands and arms:** in BLACK they are part of the first-person draws and move with the weapon (checked: both draws).
- **Game-rate animation stays game-rate.** The weapon's idle motion, the turn sway, recoil and reload are the game's
  vertex animation and update at `game_hz`. The latch moves the whole weapon rigidly with the controller between game
  frames.
- **Gameplay stays game-rate.** The aim and the shots follow the game latch's hand: bullets go where the weapon pointed
  at the game frame, at most one game frame behind.
- **Not classified, so they lag:**
  - CPU-shaded weapon draws (a program not compiled yet: the first frames only);
  - frames without a camera mark;
  - more than 8 weapon MVPs in a frame.
- **Shell casings, tracers, decals** are world objects and stay world.
- **Deltas larger than 0.30 m or 40°** between the two latches are not applied (glitch guard).
- **Untested on hardware:** on Quest, the hand is located at the same predicted display time as the eyes (OpenXR aim
  space), and the compositor reprojects the weapon like the world. Watch for the weapon "swimming" against the
  controller model: the controller model is not drawn.

## 5. Files

Changed:
- `src/black_vr.inc`, `src/recomp_manual.c` (`BLACK_VR` only)
- `port/quest/vr_bridge.h` (append), `src/vr_bridge.c`, `src/vr_renderer_kvk.c`, `src/vr_config.h`, `src/xr_main.c`,
  `src/xr_vulkan.c`
- `XR/src/nv2a/kelvin/vk/kvk_backend.c`, `kvk_glsl.[ch]`, `kvk_vr_hooks.h`, `kvk_vr_layers.inc`, `kvk_device.h`
- `tests/vr_glue/test_vr_glue.c`, `fake_openxr.[ch]`, `shell_access.[ch]`
- `tests/xr_shell/test_hooks.c`
- `tests/game_render_x86/render_main.c`, `CMakeLists.txt`
- `tests/game_boot_arm64/boot_stubs.c`

New:
- `tests/game_render_x86/gun_latch_check.py`
- this note

Not touched: `XR/src/usb/*`.

### GRX_GUN_AT default 20 -> 90 (2026-11-04)
After the game-thread CPU pass (perf.md §9) the harness reached level 1 at a different moment, and the check fired at
20 stereo frames while the weapon's raise/idle animation was still playing (hands parting between game frames 51 and 52),
so "world changed" and patch-error checks failed. The latch itself still moved the weapon by the hand delta (-40 px vs
-41 expected). Waiting 90 steady weapon frames lands on a still pose: PASS, median patch error 0.4 px, 90/95 % within
3 px (L/R), after == settled. Run: `build-perf-x86/gun_at90.log`.
