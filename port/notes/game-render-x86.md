# game-render-x86: the real game through the production Quest graphics path, headless on x86-64

Stream owner: game-render-x86 agent. Nothing committed. Paths are relative to /root/black; `XR` = `third_party/xboxrecomp`.

## Results at a glance

| What | Status |
|---|---|
| Boot (CRT/XAPI, kernel, APU+DSP, OHCI, Kelvin on the Vulkan backend) | runs, no crash, no undecoded MMIO access |
| Legal / disc check | passes once two host bugs are fixed (AC97 trap, missing movies; below) |
| Title screen ("PRESS START") | renders, on the **flat quad layer** (FLAT_MENU), ~17 frames/s on SwiftShader |
| START via the Quest pad path | reaches the **main menu** (START MISSION / OPTIONS) |
| A, A | **difficulty select**, then NORMAL starts **level 1** loading (`03_N` intro movie absent, treated as played) |
| A | level 1 in **STEREO**: both eye images (multiview), **HUD quad layer** (premultiplied, transparent outside the HUD once bug 3 below is fixed) |
| `[BLACK_VR]` self-checks | screen-right convention confirmed (sign -1), clip y +1, depth row (1.00002, -0.08); pitch sign needs look input (none sent) |
| Shader/pipeline compile failures, fallbacks | none logged; 132 pipelines, 51 vertex / 21 pixel programs, ubershader on ~130 draws until specialised modules arrive |
| Vulkan validation layers | not run (not installed; a source build needs SPIRV-Tools + VVL, not quick on 2 cores) |
| Frame time (SwiftShader, 2 cores, 832x896 per eye) | menus 60-85 ms, level 1 0.9-1.3 s per frame (run 9) |
| Tests after the changes | all pass (section 6) |
| APK | rebuilt with the fixes, zipalign -p 16 OK, apksigner v3 verified |

Frames (run 9, with every fix): `build-render-x86/frames/`: key PNGs, `contact.png`, `index.tsv`, `run.mp4`
(75 s, 1664x896, 6.2 MB), `video/` (per-frame layers + `meta.jsonl`). `frames_run8/` is the run before fix 3.

## 1. What runs

One process, the threads as on the headset:

- **game thread** (16 MB stack): `src/main.c` `black_game_main` built with `BLACK_VR` and the `__ANDROID__` paths:
  crash reporter, memory layout, APU (DSP56300 x86 build), kernel bridge, the recompiled title.
- **Kelvin**: `kelvin_start` -> pusher + GPU thread -> `KELVIN_GFX_BACKEND=vulkan` backend (`kvk_backend.c`) ->
  `vr_renderer_kvk.c` -> `vr_bridge.c` (eye/flat/HUD targets, late latch).
- **XR thread** (the harness): `xr_session.c` `xrs_frame` on tests/vr_glue's fake OpenXR runtime, 30 Hz once the
  renderer runs; `xrEndFrame`'s layers are read back and saved.
- **SwiftShader** as the Vulkan driver, the system loader, `VK_ICD_FILENAMES`.
- **input**: the fake runtime answers the shell's action states by name (`menu`, `button_a`, ... and the right
  stick), so a press goes `xrGetActionState*` -> `input_sync` -> `vr_input_map` -> `VrPadState` -> `vr_get_pad` ->
  `xinput_device.c` (`BLACK_VR`) -> `usb_gamepad` -> OHCI -> the title, exactly the Quest path.
- **head**: fixed (0, 1.6, 0), then after 30 stereo frames a slow sway (yaw +-17 deg / 60 s, pitch +-4.6 deg / 43 s,
  wall clock), so late latch has work to do.

The disc is read in place: `<build>/scratch/root/game/*` are symlinks to `game/*`; saves, partition images, E:\UDATA,
the kernel log and the shader caches land under `<build>/scratch/root`. `game/` was compared (names, sizes, mtimes,
modes) before and after every run: unchanged.

## 2. Build and run

```sh
CC=clang CXX=clang++ cmake -G Ninja -S tests/game_render_x86 -B build-render-x86 -DCMAKE_BUILD_TYPE=Release \
    -DKVK_GLSLANG_DIR=/root/deps/glslang -DKVK_VULKAN_HEADERS=/root/deps/Vulkan-Headers/include \
    -DKVK_TEST_ICD=/root/build/swiftshader/Linux/vk_swiftshader_icd.json
cmake --build build-render-x86                      # ~10 min on 2 cores (generated code 4.5 min, glslang)
GRX_SECONDS=2400 build-render-x86/run.sh > run.log 2>&1   # or ctest --test-dir build-render-x86 -V
python3 tests/game_render_x86/contact_sheet.py build-render-x86/frames
python3 tests/game_render_x86/make_video.py build-render-x86/frames     # -> frames/run.mp4
```

Knobs (env): `GRX_SECONDS`, `GRX_EYE_W/H` (832x896), `GRX_MULTIVIEW`, `GRX_XR_HZ`, `GRX_CAPTURE_EVERY`,
`GRX_ROT_AT`, `GRX_VIDEO`, `GRX_INI` (extra blackvr.ini lines), and `GRX_INPUT`, a press script
`"T:button[:hold]"` with T in seconds after the first renderer frame (default `60:menu,120:a,180:a,240:a,300:a`;
`rt`/`lt` hold a trigger, `rt` fires in level 1: renderer.md section 11; `lc`/`rc` the thumbstick clicks, both held
2.5 s = a both-eye capture to `<scratch>/root/shots`: renderer.md section 12).
A press is held until both its hold time has passed and the backend has drawn 4 more frames (the XR thread and the
game both run slowly here). A file `frames/press` with button names adds presses while running.
The ctest passes on `[GRX] RESULT:` and fails on `[CRASH]`.

### Host adaptation (all in tests/game_render_x86 unless listed)

| Piece | How |
|---|---|
| The Quest source paths on x86 | every game/runtime TU is compiled with `-D__ANDROID__` (no aarch64 flags); `include/` has stand-ins for `<android/log.h>`, `<android/sharedmem.h>`, `<android/api-level.h>`, `<aaudio/AAudio.h>`; `host_stubs.c` implements them (stderr, -1, "no audio device") |
| `<windows.h>`/`<intrin.h>` in the Kelvin core | tests/nv2a_arm64/hostshim; `grx_host.h` (force-included) adds what `win32_compat.h` gives only on aarch64 (64-bit Interlocked, `MemoryBarrier`, `_snprintf_s`, `strncpy_s`) |
| SIGSEGV MMIO traps | `x86_fault_glue.c`: the `a64_fault_*`/`a64_uc_*` glue of `platform/arm64_fault_decode.h` for x86-64 Linux. Same dispatcher semantics; the faulting instruction is decoded by `platform/mmio_decode.h`'s x86-64 decoder (the one Windows uses) over a CONTEXT-shaped copy of the ucontext. Enabled by `-DXR_HOST_FAULT_GLUE` |
| Shell + glue + fake runtime | as tests/vr_glue builds them; `grx_shell.c` plays `xr_main.c`'s part; the shell library is linked whole-archive (its renderer hooks are weak in `xr_vulkan.c`, as the APK links `black_vk_renderer` whole) |
| DSP56300 | `build-dsp/x86_64-unknown-linux-gnu/release/libdsp56300_emu_ffi.a` |

Production files touched only to admit this host (no effect on Quest or Windows builds):
`platform/arm64_fault_decode.h` (glue declarations), `platform/mmio_decode.h`, `nv2a/kelvin/kelvin_mmio.c`,
`usb/ohci.c`, `apu/apu_mmio_hook.c`, `kernel/xbox_memory_layout.c`: `defined(__aarch64__)` became
`(defined(__aarch64__) || defined(XR_HOST_FAULT_GLUE))` on the POSIX fault branches, and `xbox_memory_layout.c` got an
AC97 write-trap branch for such a host (the AArch64 one decodes AArch64 itself). `src/main.c`: the crash reporter
reads pc/sp/lr through `BLACK_UC_PC/SP/LR` (same expressions on AArch64, `REG_RIP`/`REG_RSP` on x86-64).
`tests/vr_glue/fake_openxr.[ch]`: optional `fx_input_bool/vec2` hooks (unset: as before).

## 3. Bugs the run found

1. **A dump without `videos/` never reached the title** (`src/recomp_manual.c`, Quest only). The startup logo
   (`LO_N_US.xmv`) open fails, the movie object's error callback (0x0020B3B0, the disc-error path) runs and the
   movie manager retries the open every frame: "THERE IS A PROBLEM WITH THE DISC YOU ARE USING". The skip
   (`RECOMP_BLACK_SKIP_MOVIES`, default on for the Quest) only ends a movie that is already playing. Fix: that
   callback is intercepted in `recomp_lookup_manual` under `__ANDROID__`; when the manager is opening (state 3) the
   movie whose callback this is, the skip variables would skip it, and its file is absent on the host
   (`xbox_translate_path` + `access`), the manager goes to state 0x1F (the step after a movie's close: restore, then
   idle 0x37) as if the movie had played; any other case calls the native callback. Logged once per movie:
   `[MOVIE] LO_N_US (D:\videos\LO_N_US.xmv) is not on the disc: treated as played`. Seen for LO_N_US, AK_N (menu
   background, so the menu's left panel is black), C_N_US, 02_N, 03_N. This affects the headset with the same dump.
   No regeneration of gen/ is needed (it uses the indirect-call override, not a `_gen` wrapper).
2. **x86-64 host: the AC97 channel reset spun forever** (`kernel/xbox_memory_layout.c`). DirectSound writes the
   reset bit and its next read must see it clear; the trap that clears it existed for Windows and AArch64 only.
   Added for `XR_HOST_FAULT_GLUE` hosts (decode with the glue, store on the briefly writable page, clear RR).
   Host-only; the Quest already had its own.

3. **The HUD layer was opaque black on every other frame** (`kelvin/vk/kvk_backend.c`, `flip_now`; Quest-visible).
   After each flip the backend resets "the" HUD surface to transparent, but it reset only the *last* HUD surface in
   its table. The title double-buffers its back buffer, so there are two HUD colour surfaces (one per back-buffer
   VA); the other one was never cleared and kept the opaque full-screen black of the level's "4 DAYS" intro card
   under every later HUD drawn into it. On the headset that is a black 2.4 m rectangle flashing over the world at
   half the game rate. Fix: reset every HUD colour surface at the flip. Run 8 (before): 567 stereo frames, strict
   alternation transparent / opaque by game frame parity. Run 9 (after): 294 HUD frames, 0 opaque, coverage 4-5%.

Not bugs (checked): the eye images' horizontal offset (~129 px between eyes for distant content) is the FOV
asymmetry of the test FOVs (tan(0.95) vs tan(0.80): 126.3 px at infinity) plus parallax; world_scale 1.0 gives
parallax consistent with the room's depth (near floor 141 px, far door 129 px). The HUD layer is fully opaque only
while the title shows its "4 DAYS" black intro card, transparent (alpha coverage ~4-6%) in play.

## 4. Watched items

- **Present modes**: boot, title, menus, difficulty, loading and the level's intro card are FLAT_MENU (shell submit
  mode 1): one world-locked quad, 2.0 x 1.5 m at 2.2 m, no projection layer, no eye image acquired. Gameplay is
  STEREO: projection layer + view-space HUD quad (2.4 x 1.8 m at 2 m), premultiplied.
- **Late latch**: after the sway starts the backend's correction moves the game camera's forward axis by 40-60 px
  between game latch and render latch, in the sway's direction (game frames take 1.5 s here, so the head moves far
  between the two latches); the submitted poses are the render latch's.
- **Self-checks**: as above. No "1/p00 differs from half_fov_x" warning (the published projection matches the lens
  window: half-FOV 1.538 x 1.386 = the eye union x 1.10).
- **Frame times**: menus 60-130 ms, level 1.3-1.7 s (both eyes 832x896 + HUD, 2 SwiftShader threads).

## 5. Video and contact sheet

- `make_video.py frames` composes every presented frame as the headset would show it: per eye, the projection
  layer's image (black for flat frames), then each quad layer (flat menu / HUD) projected with the submitted eye
  pose and FOV from where the shell placed it (world or view space), premultiplied blend; left | right, a caption
  with the backend frame number, present mode and camera frame. Timing: flat frames at the game clock
  (flips / 60 s); stereo frames at a fixed 12 game frames/s (captioned: SwiftShader takes ~1.2 s per stereo frame and
  the head sway runs on the wall clock, so the game clock would make it race). H.264, `-preset veryfast -crf 23
  -pix_fmt yuv420p`, 30 fps, 2 processes. Run 9: 2656 presented frames -> 75.2 s, 1664x896, 6.2 MB, ~1.5 min.
- `contact_sheet.py frames`: first flat frame, the frame before every scripted press, the first stereo frame (both
  eyes + HUD), the final capture.

## 6. Tests after the changes

| Suite | Result |
|---|---|
| tests/kelvin_vk (`/root/build/kvk-tests`) | 5/5 |
| tests/kelvin_vk arm64 (`/root/build/kvk-android`) | 2/2 |
| tests/vr_glue (`/root/build/vr-glue`) | 2/2 (247 + 249 checks) |
| tests/xr_shell (`build-xr-shell`, `/root/build/xr-shell-check`) | 4/4 each |
| tests/nv2a_arm64 x86 / a64 | 10/10, 6/6 |
| `build-boot-a64` (after rebuilding `build-game-android`) | 1/1 (first GPU touch at 0.2 s) |
| Windows (clang `--target=x86_64-w64-mingw32 -E`, mingw-w64 headers), token streams before vs after, with and without `-DBLACK_VR`: main.c, recomp_manual.c, kelvin_mmio.c, ohci.c, apu_mmio_hook.c, xbox_memory_layout.c, nv2a_mmio_hook.c, win32_compat.c | all identical. `kvk_backend.c` is not built on Windows (d3d11 backend) |

## 7. Still open

- Validation layers never ran here.
- `AK_N` (front-end background movie) and the level movies are absent from this dump: their screen areas stay black.
- The pitch-sign self-check needs look input; the script presses buttons only.
- The level-1 intro card stays up (FLAT_MENU) until A is pressed; at 1.5 s per frame its own timer takes many
  minutes, so the script presses A.
- Audio is silent (no AAudio device on the host; the APU and DSP run).
