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

## 6. The muzzle glow follows the hand (2026-11-12)

**Symptom (headset):** firing the shotgun or the UZI, the flash at the muzzle was right, but a second, dimmer orange
glow floated some distance from the gun. Reproduced in the x86 harness with the controller held low
(`GRX_HAND=1 GRX_HAND_POS=0.25,-0.50,-0.30`, the pistol): the flash at the gun (left eye ~(402,454) of 480x512) and an
orange ball in the middle of the room (~(331,276)), rising and fading over 5 frames.

**What it is:** not a draw of the flash. A shot (1DBD50 -> 169960) starts a *held effect* ([2D1990]+65690, recomp_manual.c
`BLACK_FX_AT`) on the player's first-person record 5 (the muzzle). Its update (176BD0) places its two emitters where
the record is -- `[[actor+328]+30+where*4]+40`, the first-person model where the flat game holds it -- and spawns
particles in the world there. One of them is the glow: a screen-space sprite (`3516849B`, 4 quads, additive) drawn into
the glare chain's 160x120 source image (va 83248000), blurred and added over the view, hence soft. The gun latch moves
the weapon's MVPs, not those records, so the glow stayed at the flat weapon's place. Evidence (`RECOMP_KVK_SPRITELOG=2`,
`BLACKVR_AIM_TRACE=1`): the glare sprite's eye position computed as `kvk_finish` places it matches the ghost in the
images to ~5 px; emitter 1 sits 2 cm from the flash's own pre-re-target origin; nothing else new is drawn near it.
Moving the emitters around the world draw does nothing (the particles are already in the world), so the fix is at spawn.

**Fix (`src/black_vr.inc`, `src/recomp_manual.c`):**
- `camera_after_world` captures, with the weapon's re-target, the world transform the weapon got
  (X = w_head * yv * w_head^-1 = w_hand * grip^-1 * w_entity^-1) and the player's records 0..7 (`bvr_fx_capture`).
- `sub_00176BD0` (already manual) wraps the game's update with `black_vr_fx_update_begin/_end` in VR: the records are
  set to X * record (plus the record's own translation since that render: the player walking, the recoil), the effects
  follow them, and the records are put back. The flash (16B1E0) still reads the game's record 5 and is re-targeted
  through its MVP as before. Only STEREO frames whose weapon was re-targeted; other owners and `where 8` untouched.
- **`BLACKVR_GUN_FX=0`** (env or blackvr.ini `env BLACKVR_GUN_FX=0`): off, for A/B.
- Result: the glow starts at the hand-held muzzle and drifts forward along the barrel
  (`build-vrmenu-x86/frames-ghost7-on/ghost_before_after_{shot,next,next2}.png`, top row `BLACKVR_GUN_FX=0`, same
  binary and input). The emitter moves by exactly the flash's re-target delta (default pose: (-0.4, -3.3, +4.8) cm vs
  the flash's (-0.4, -4.2, +5.1) cm).
- Limits: X is the last render's, so the spawn point lags the hand's own motion by one game frame (the late latch
  does not apply to effects). Superseded by 6b: the recoil also got in, and the transform is now built for the update. Shell casings (thrown from Bullet_Loc by an animation event, 162FE0) are not moved.
- Tests: `tests/xr_shell` hooks 447 -> 510 checks (both variants) (records moved to X and carried by the record's motion, absent records and
  other owners untouched, restored bit for bit, nothing outside STEREO or without a re-target); two mutations caught.

**Diagnostics added:** `RECOMP_KVK_SPRITELOG=2` (also blended perspective world draws as `PFX` lines with guest-pixel
extents and view depth; every `SPRITE` line gets the eye NDC its centre lands at), `BLACKVR_AIM_TRACE=1` (`AIM`, `FX`,
`MUZZLE` lines), harness `GRX_HAND_POS`, `GRX_HAND_YAW/_PITCH` and the gestures `aiml/aimr/aimu/aimd`.

### 6b. UZI and shotgun: the recoil was in the spawn (2026-11-12, second pass)

**Report (headset, e856ad7):** with the UZI and the shotgun the glow / fireball still floats off the gun.

**Getting the guns in the harness:** `RECOMP_BLACK_TEST_LOADOUT=smg` (UZI) or `=shg` (shotgun), harness-only, in
`black_vr_test_loadout` (called from `black_test_weapon_loadout`, 1B7800): the primary weapon token at core+3F0 becomes
`BG1_<gun>` (12-char names packed base 40: BG1_SMG `5446152380DB8000`, BG1_SHG `5446151EBC278000`, BG1_PST
`544614B7182C0000`), only if the token is in the loaded gun table exactly once with a sane descriptor. Logged as
`test loadout: BG1_shg token ... accepted`.

**Mechanism per weapon** (`BLACKVR_AIM_TRACE=2`, `RECOMP_KVK_SPRITELOG=2`):
- UZI: the flash (16B1E0 via D4A50, c112, re-targeted) plus the held effect on record 5 (glow `3516849B` in the glare
  chain, smoke) -- the same as the pistol.
- Shotgun: no flash draw (its flash entry's texture is 0, 16B1E0 skips it). The fireball *is* the record-5 held effect:
  4 emitters, sprite `3516849B`, drawn into the main target, flying out ~0.6 m per frame.
- No other record, no `where 8` effect, no one-shot spawn elsewhere: the only held effect a shot starts is that slot,
  and nothing else new is drawn near it (`SPRITE`/`PFX` lines).

**Why e856ad7 missed:** the gating was met (`FXU ... moved 3 records`, `gun fx: ... follow it to the hand` logged,
STEREO, yv_ok, hand_locked). But the game poses the first-person records for the update on the player's *aim matrix*
(actor+D0 = [2D45D4]+100), which already carries this frame's recoil, while the camera entity (D2F40's source, the
`w_entity` the latch removes) only copies the aim matrix later in the frame. The recoil is a pitch about the eye,
core+4F4 degrees: 8.1 at a shotgun shot, then x0.9 per frame. So at the effects' update the records stood
`Rot(A_n - A_n-1)` above where the render puts them (measured to 0.001 deg over 8 frames: +8.10, -0.81, -0.73, ...),
and e856ad7's `X_last_render * record` put the shot's fireball 10.2 cm above the hand-held muzzle, flying 8 deg
upward. On top of that e856ad7 used the last render's hand: the spawn trailed a moving hand by its whole per-frame
move (harness sway: 4-6 cm every frame, 11.5 cm at the swaying shot).

**Fix (`black_vr_fx_update_begin`):** the transform is built for the update the way the next render will build it:
`X = w_hand_now * grip^-1 * M_aim^-1`, with M_aim read from actor+D0 at the update (self-checked at each render: it must
equal that render's camera frame to 2e-3, else the fallback below), w_hand_now = this game frame's latched hand on
the last render's body, moved by the aim matrix's step since that render (walking). Records are `X * record` (the
animation is in the game's record and matches the next render's). Fallback (no aim matrix): the last render's X,
moved with the hand. `BLACKVR_GUN_FX=0` still turns it all off. Measured spawn vs the next render's hand-held muzzle
(`FXC` lines, `BLACKVR_AIM_TRACE=1`, world units = m; default hand pose; the e856ad7 column is computed on the same
frames, and matches separate runs with e856ad7's placement):

| | shotgun new | shotgun e856ad7 | UZI new | UZI e856ad7 |
|---|---|---|---|---|
| recoil at the shot (core+4F4) | 8.1 deg | | 1.57-1.67 deg | |
| settled shot | 0.0000 | 0.102 | 0.0000 | 0.017 |
| shot, hand swaying 15 cm | 0.0000 | 0.115 | 0.0000 | 0.054 |
| shot, walking + swaying | 0.0000 | 0.092 | 0.0000 | 0.056 |
| worst frame of the run | 0.0001 | 0.115 (0.132 in the e856ad7 run) | 0.0000 | 0.113 (0.105) |

(The harness hand moves ~6 cm per game frame in the sway: its game frames take ~0.75 s. On the headset e856ad7's
hand lag is the hand's move in one game frame, ~1 cm at 0.5 m/s; the recoil part is as above.)

**What is left: the late latch.** The drawn gun is moved at the flip to the hand located for the display time
(`D = hand_late * hand_game^-1`, vr_renderer_kvk.c); particles already in the world are not. The game's hand is the
XR thread's last sample (its frame's predicted display time, input_sync), the late latch's is the render's display
time: on the headset ~one game frame plus the sample's age, ~17-30 ms, i.e. ~1-1.5 cm at 0.5 m/s, 2-3 cm on a fast
flick, and nothing with the hand still. In the harness that gap is ~0.75 s, so the swaying UZI's glow shows ~5 cm
beside the muzzle with the latch on and sits on it with `GRX_INI="gun_latch = off"` (`uzi_sway_late_latch.png`).
Options, not done: locate the hands at `game_predict_time` in `vr_latch_frame_state` like the eyes (D becomes the
prediction's refinement; also changes the aim's timing), or predict the spawn by the last flip's D.

**Captures** (`build-vrmenu-x86/evidence/gunfx2/`, rows: `BLACKVR_GUN_FX=0` / e856ad7's placement / the new default,
same input script, left | right eye; the sway is wall-clock driven, so the hand is not at the same place in each run):
`shotgun_settled_shot.png`, `shotgun_settled_next.png`, `shotgun_sway_shot.png`, `shotgun_walk_shot.png`,
`uzi_settled_shot.png`, `uzi_sway_shot.png`, `uzi_walk_shot.png`, `uzi_sway_late_latch.png`. Logs:
`build-vrmenu-x86/{shgF,smgF}-{off,e856,on}.log`, `smgF-on-nolatch.log`; harness driver: 60 stereo frames settled, a
shot, then the gesture `sway` held, a shot, then `fwd` held, a shot.

**Tests:** `tests/xr_shell` hooks 510 -> 608 checks (both variants): the aim matrix found and self-checked; a shot
frame with the aim kicked 8 deg, the player stepping 5 cm, record 5 animating 3 deg, a record appearing and the hand
moving 4 cm / 5 deg: every record relative to the next render's hand is its pose on the aim matrix less the grip, and
e856ad7's placement is over 5 cm off; the fallback without the aim matrix; restore bit for bit. Six mutations caught
(the last render's camera entity, no body step, the last render's hand, the last render's records, no hand move in the
fallback, no self-check).

### 6c. The muzzle check in every log (2026-11-13)

**Report (headset, a2b9ad0):** UZI right; the shotgun's fireball still a bit too high or low, with the gun held still.
In the harness the spawn is exact (6b) and the fireball sits on the barrel line just past the muzzle
(`evidence/gunfx2/shotgun_settled_shot.png`). One headset-only cause would fit: the update without the aim matrix
(the fallback keeps the shot's recoil in the spawn, 10 cm above the muzzle for the shotgun, about 1.7 cm for the
UZI). To tell, every log now carries, about every 30 s of STEREO with the gun in the hand:

`gun fx check: N updates, muzzle effects started M cm (mean) / X cm (max) from the hand-held muzzle, F without the aim matrix`

0 / 0 / 0 is the harness result. F > 0 or X of several cm on the headset points at the placement; all 0 points at
the drawing of the fireball itself (the screen-space sprite's per-eye correction, renderer.md 11). The per-frame
`FXC` lines stay behind `BLACKVR_AIM_TRACE=1`. Turn frames are left out (a snap turn between the update and the
render puts that one update's spawn off by the turn: ~40 cm for 35 deg, one puff; smooth turning a few cm).

**The camera off the aim matrix** (same day). The harness showed the self-check failing for ~20 frames after the
objectives screen and after a melee: the camera entity is then off the aim matrix (a lunge, a menu's camera). The
fallback there was the last render's X moved with the hand, which keeps a shot's recoil in the spawn (the e856ad7
error, ~10 cm for the shotgun). Now, while the aim matrix is readable (a rigid matrix within 1 m of the camera), the
update takes the next render's camera entity as `A_now * O` with `O = A_last^-1 * E_last` (the offset seen at the
last render): `X = h_now * grip^-1 * (A_now * O)^-1`, the body moved by that camera's step. With `O = I` it is the
fresh path. Only an unreadable aim matrix still uses the old fallback. The check line counts both:
`...; N with the camera off the aim matrix, F without the aim matrix`. Hook test: an offset camera (2 deg, 2 cm),
an 8 deg kick, a 5 cm step, record 5 animating 3 deg and the hand moving: every component exact, and the old
fallback over 3 cm off (690 checks; undoing the path, the offset or the body step fails it).

### 6d. The shotgun fireball leaves 4 deg above the barrel (2026-11-13)

**Report (headset):** "the shotgun fireball ... it's a bit high", gun held still; the check line read 0.1-0.7 cm (the
spawn is right).

**Measured** (`BLACKVR_AIM_TRACE=3`: `FXV` lines, each emitter of the player's muzzle effect and its particles'
mean velocity as pitch/yaw degrees off the hand-held muzzle's axis, `bvr_fxl.x * record 5`; particles are read from
the emitter's group, `[em+A8]+4` blocks of 0x30-byte entries, velocity at +10). The shotgun's effect has 4 emitters;
the fireball is emitter 3 (~34-42 m/s, 7 particles), whose axis is **4.0 deg up** from the record's (its particles
leave 3.9 deg up, then curve down at ~3 g). The others (smoke, 1-7 m/s) sit on the axis or drift sideways on purpose.
The particle spawn (160E70) takes the direction from the emitter's rows, the emitter's rows are `local[i] * slot
matrix` (163FD0 each update, 16FE10 at creation): the 4 deg is in the effect's data, aimed a little up from a gun held
low beside the flat camera. In the hand it shows as a fireball leaving above the barrel: 3 cm at 40 cm, 7 cm at 1 m.

**Change (`bvr_fx_align`, after the held effects' update, before the records go back):** every emitter of the
player's weapon effects (where 0..7) whose axis is 0.1-6 deg off its record's axis is turned onto it about its own
position. Wider ones keep their direction. Trace after: emitter 3 at 0.0 deg, its particles -0.2 deg at spawn.
`BLACKVR_GUN_FX_ALIGN=0` turns it off.

## 7. Where the shots go vs where the gun points (2026-11-12, measured, not changed)

- Bullets: 1DBD50 fires from the actor's eye toward `weapon+10` (spread from the magazine), which the game puts on
  the first surface along the look direction. That direction is exactly what `black_vr_look` asks (`vgm_aim_dir`:
  eye -> the point 10 m along the controller's aim ray): aim points matched it to 5 digits in all 5 logged shots.
  (The D2F40 camera entity reads 1.5 deg above that pitch; the bullets do not use it, and the latch strips it.)
- Barrel: the muzzle record's idle axis is +1.0 deg up, -0.4 deg left of the camera entity, so the latched barrel
  points 1 deg above the controller ray (2-3 deg in the shot frame with recoil); the muzzle sits ~3 cm left, 9 cm up,
  11 cm ahead of the controller's aim origin (grip offset 0.12,-0.18,0.30).
- So the hit is off the barrel line by: 9-14 cm at 2.7-3.1 m, 21-31 cm at 6.6 m (default hand pose, 0.12/-0.22/-0.35 m),
  and 1.6-2.4 m at 26.6 m with the controller held low (0.25/-0.50/-0.30); 0.9 m off the controller ray there. Most of it
  is the designed parallax (shots from the eye, converging on the controller ray at `aim_converge` = 10 m), plus the
  barrel's 1 deg tilt.
- Options (not done): converge at the distance of the last aim point (`weapon+10`) instead of a fixed 10 m, so the
  shot crosses the controller ray at the surface it hits; and/or turn the latch by the muzzle record's idle tilt so the
  barrel lies on the controller ray.
