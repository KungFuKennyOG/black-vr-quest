# validation-audio: Vulkan validation of the Quest graphics path, and the game's audio captured headless

Stream owner: validation-audio agent. Nothing committed. Paths relative to /root/black; `XR` = `third_party/xboxrecomp`.

## Results at a glance

| What | Result |
|---|---|
| Validation layer | Khronos VVL **v1.4.367** built from source (Release, ninja, 2 cores), matching `/root/deps/Vulkan-Headers` v1.4.367 |
| Unique VUIDs found | 5 (below), all in `kvk_backend.c`, all fixed |
| After the fixes | **0 errors, 0 warnings**: the game (boot, title, menus, difficulty, level 1 in stereo, 256 stereo frames) with core + sync val; `tests/vr_glue` (both modes); `tests/kelvin_vk` with core + sync val + GPU-AV. One warning left in a kvk unit test only (explained) |
| GPU-assisted validation | works for `tests/kelvin_vk` (`KVK_TEST_BDA=1`); not run on the game (needs bufferDeviceAddress on the shell's device, a production change, for a test-only gain) |
| Audio | captured through the production `apu_aaudio.c` (ring, prime gate, callback) with only the device swapped: 603 s, 48 kHz stereo s16, music/SFX present, no clipping, no DC, press sounds present. Dropouts = APU producer starvation on this 2-core host (counted by the APU's own reprime counter), not a codec/rate bug |
| Outputs | `build-render-x86/frames/audio.wav`, `audio_spectrogram.png`, `audio_analysis.json`, `run_audio.mp4`, `run.mp4`, `contact.png` |
| Tests | all green (section 6) |
| APK | rebuilt, zipalign -p OK, apksigner v3 verified |

## 1. Building and running the validation layer

```sh
# deps (pins from VVL's scripts/known_good.json): Vulkan-Utility-Libraries v1.4.367, SPIRV-Headers fef168c,
# SPIRV-Tools 28f99f7, cloned into /root/deps; Vulkan-Headers v1.4.367 was already there
cmake -G Ninja -S /root/deps/<dep> -B /root/deps/vvl-build/<dep> -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/root/deps/vvl-prefix ...
cmake -G Ninja -S /root/deps/Vulkan-ValidationLayers -B /root/deps/vvl-build/VVL -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/root/deps/vvl-prefix -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF -DUSE_MIMALLOC=OFF \
  -DBUILD_WSI_{XCB,XLIB,XLIB_XRANDR,WAYLAND}_SUPPORT=OFF
```

Layer: `/root/deps/vvl-prefix/lib/libVkLayer_khronos_validation.so`; the installed manifest names the library
relatively, so `/root/deps/vvl-prefix/layer.d/` holds a copy with the absolute path (the system loader is 1.3.275).
`build-render-x86/vvl/vvl_env.sh` (source it with `VVL_LOG=<file>`; `VVL_SYNC=1` default, `VVL_GPUAV=1`, `VVL_BP=1`
for best practices incl. Arm) sets `VK_INSTANCE_LAYERS` and the `VK_KHRONOS_VALIDATION_*` settings. The layer's
"Current Validation Enabled" info confirmed Core, Synchronization, Stateless, Object lifetime, Thread safety, GPU-AV.

Logs: `build-render-x86/vvl/` (`game-run1.*` before the fixes, `game-run2.*` after, `kvk*-*.log`, `vrglue*-*.log`).

## 2. VUIDs and fixes (all in `XR/src/nv2a/kelvin/vk/kvk_backend.c`, which Windows does not build)

| VUID | Where | Fix |
|---|---|---|
| `VUID-VkRenderPassCreateInfo-pDependencies-00837` / `-00838` | `rp_get`: both subpass dependencies put `TRANSFER` stage (and transfer accesses) on the subpass side, which may only name graphics stages | `TRANSFER` stage/access only on the `VK_SUBPASS_EXTERNAL` side (src of the incoming, dst of the outgoing dependency); subpass side = colour output, early/late tests, fragment shader. Every render pass of the backend went through this |
| `VUID-vkCmdDrawIndexed-None-07834`, `VUID-vkCmdDraw-None-07834` | depth-bias dynamic state used after `vkCmdBeginRenderPass` of a **multiview** pass without being set again | With multiview *all* non-render-pass state is undefined at the start of each subpass (spec, multiview chapter). The backend cached bound state across passes (`P.pipe/set0/set1/dyn/ib`) and skipped re-binding. New `state_forget()` after every `vkCmdBeginRenderPass` (`pass_begin`, `depth_as_color`, `depth_from_color`; `compose_pass`/`vrl_pass` already reset most of it) |
| `VUID-vkCmdDrawIndexed-None-08606` | no valid pipeline bound: same cause, the pipeline binding cached from the previous pass | same fix |

The multiview one is the important one for the headset: SwiftShader keeps the state, a tiler's multiview
implementation is allowed to (and Adreno can) drop it, which would draw with stale or no pipeline / dynamic state.

Other fixes from the review (no VUID fired on SwiftShader, but each is a real issue on a stricter driver):

- **Readback memory and host visibility** (`kvk_read_surface`): the copy's buffer now prefers `HOST_CACHED`
  (coherent if there is such a type, else invalidated after the wait); uploads (the ring) avoid `HOST_CACHED`
  (write-combined). A `TRANSFER_WRITE -> HOST_READ` buffer barrier was missing: a fence wait alone only covers device
  accesses. (`kvk_read_surface` is used by tests/diagnostics, not the frame loop.)
- **Sampler LOD bias** clamped to `maxSamplerLodBias` (`VUID-VkSamplerCreateInfo-mipLodBias-01069`; guest biases are
  not bounded by the device's limit).
- **Surface formats** now require `COLOR_ATTACHMENT_BLEND` and `SAMPLED_IMAGE_FILTER_LINEAR` as well (the backend
  blends into and linearly samples its surfaces), else BGRA8.
- **`caps_check()` at `kgpu_init`**: every format, limit and memory type the backend relies on is queried and logged
  once (`[KVK] format ...`, `[KVK] limits: ...`, `[KVK] memory type ...`), and init fails cleanly if a required one
  is missing. The first headset run records what the Quest's driver really reports.

Overlap with the performance agent: none in pipeline creation. `state_forget()` adds a few binds per render pass.

### Remaining warnings

- `Undefined-Value-OutputNotWritten` (9x, `tests/kelvin_vk` cpu-parity only): the random combiner test generates
  alpha-test `NEVER`, whose shader is an unconditional `discard`, so `o_color` is dead code. No pixel is ever
  written; the game never produced it. Left.
- `WARNING-Setting-Limit-Adjusted`: the layer itself enabling features for GPU-AV. Informational.
- Best practices (`VVL_BP=1`, not part of the goal, recorded for the perf stream): Arm/IMG "redundant store /
  untouched stencil on tile" (load/store of D32S8 stencil never used in a pass; store before a clear), "depth bias
  enabled with zero factors" (pipelines keep `depthBiasEnable` on and set the bias dynamically), "LOD clamping in
  sampler" (LOD0 filter modes), "small dedicated allocations" (one `vkAllocateMemory` per image), "clear values
  without CLEAR load op", `robustBufferAccess` on, no `ONE_TIME_SUBMIT` in the vr_glue test's own command buffers,
  `MEMORY_WRITE` in the vr_glue test's readback barrier (test code).

## 3. Adreno 740 (Quest 3): what the backend uses and what is known

Public data: vulkan.gpuinfo.org refuses automated fetches (robots), and no public vulkaninfo dump of the Quest 3's
*proprietary* driver turned up (the GitHub dumps are Turnip/Mesa). What is known: the proprietary Adreno drivers
generally do not expose `textureCompressionBC` (the device test already runs without BC, decoding DXT to RGBA8); the
Quest 3 driver reports 137 device extensions (nevr-runtime issue #869); multiview, depthClamp, samplerAnisotropy,
robustBufferAccess are standard on Adreno 6xx/7xx. Everything below is therefore *queried at run time* now
(`caps_check`), not assumed; the table says what the spec guarantees independently of the driver.

| Used | Guaranteed by the spec? | Handling |
|---|---|---|
| B8G8R8A8 / R8G8B8A8 UNORM colour+blend+sample+linear | yes (mandatory formats) | required by `caps_check` |
| A1R5G5B5, R5G6B5, R8, R8G8 as render targets | yes (mandatory incl. blend) | queried per surface, BGRA8 fallback |
| BC1/2/3 | no; Adreno proprietary: normally absent | native only when the feature is on, else decoded |
| D24_UNORM_S8_UINT / D32_SFLOAT_S8_UINT, depth attachment + sampled | one of them is guaranteed as attachment | `pick_depth_format` queries both |
| push constants 64 B | min 128 | checked |
| per stage 5 dynamic UBO + 1 dynamic SSBO + 4 combined samplers | mins 12/4/16, set dyn 8/4 | checked |
| UBO range 3 KiB, SSBO range 4 MiB | mins 16 KiB / 128 MiB | checked |
| 16-bit index restart | strips only, restart = 0xFFFF exactly (real indices < 0xFFFF by `index_limit`) | correct; list topologies never restart |
| multiview, 2 views | `maxMultiviewViewCount` >= 6 when supported | framebuffers layers = 1, views via mask |
| memory types | typical Adreno: DEVICE_LOCAL; +HOST_VISIBLE+COHERENT; +CACHED; DEVICE_LOCAL+LAZILY_ALLOCATED (to be confirmed by the log) | upload: coherent uncached; readback: cached |
| LAZILY_ALLOCATED transient depth/MSAA | n/a | no MSAA anywhere; Kelvin depth surfaces are stored and later sampled/converted by the title, so not transient |
| `maxMemoryAllocationCount` | min 4096 | one allocation per image: fine for level 1 (~530 textures), but the texture cache allows 4096 slots: a sub-allocator is the long-term fix (memory stream) |

## 4. Audio capture (`tests/game_render_x86`)

- `host_stubs.c`: with `GRX_AUDIO_WAV=<path>` (run.sh sets `frames/audio.wav`; empty disables) AAudio is a fake
  device: `openStream` succeeds at the requested 48 kHz x2 s16, `requestStart` starts a thread that calls the
  production data callback (`apu_aaudio.c aa_data`) one 192-frame burst at a time on the wall clock (catching up if
  late) and appends what it returns to the WAV. Header rewritten every second and at exit; sidecar `.json` with the
  device start time on the harness clock. Nothing in `XR/src/apu` changed.
- `analyse_audio.py <wav> [run.log]`: format, levels, clipping, DC, L/R, roll-off, silence, dropouts, per-press
  response, sections from the run log; writes the spectrogram PNG.
- `make_video.py --flat-clock wall --audio <wav>`: flat frames on the wall clock (in sync with the audio), stereo
  frames at 12 game frames/s with the audio continuing from the first stereo frame's wall time.

### Run 10 (`build-render-x86/run10.log`, 600 s, reached level 1 stereo: 215 stereo frames)

| Section (WAV time) | RMS L/R dBFS | Peak dBFS | Silent 50 ms windows |
|---|---|---|---|
| title 0-61 s | -44 / -43 | -14.6 | 97% (one logo sound at 15 s; the title has no music without the absent `AK_N` movie) |
| front end 61-249 s | -24 / -26 | -5.0 | 30% (menu music from 79 s; gap 181-222 s at difficulty select) |
| level 1 249-603 s | -28 / -28 | -8.8 | 3% |

- Format: 48000 Hz, 2 ch, int16, as opened. No clipped samples. DC -73 / -79 dBFS. L/R correlation 0.73 (real
  stereo, not dual mono). 99% of energy below 11.5 kHz, centroid 0.8 kHz; music spectrum falls off at ~16 kHz,
  consistent with the 32 kHz front-end stream (`FEMusic.ssh` names 32000 Hz) resampled correctly to 48 kHz (a wrong
  rate would move that edge).
- Press sounds: every scripted press is followed within 1.5 s by a peak 4-20 dB over the preceding level
  (menu -20 dBFS from silence; A presses -12/-20/-18/-25 dBFS over music).
- Spectrogram (`frames/audio_spectrogram.png`): logo burst at 15 s; menu sounds at 61-80 s; the menu music 79-181 s
  as a bass-heavy pulse (peak ~60 Hz) with a ~2.5 s envelope; a quiet broadband bed with tonal lines at 10.5/11.7/
  14.5 kHz from 222 s to ~350 s (difficulty select, loading, the "4 DAYS" card, the level start; 40-60 dB below the
  main level, < 1% of the energy above 12 kHz); then level ambience below 3 kHz.
- **Dropouts**: 1412 exact-zero gaps (median 37 ms, p90 60 ms), denser where the host is busiest (level 1:
  110-255 per 30 s). They are the production ring running dry and re-priming: the validation run with
  `RECOMP_APU_TRACE_OUTPUT=1` counted 3189 reprimes for 2744 gaps, `rebases` 2840, `producer-dropped` 148480 samples.
  Cause: the APU frame thread (x86 DSP56300 build) needs 50-55% of a core and shares 2 cores with SwiftShader's two
  threads, the game thread and the Kelvin threads (~195% demand), against a 64 ms ring. Not a codec, rate or ring
  bug. On the headset: watch `apu_aaudio_xrun_count`/reprimes in the first run; if they appear there too, raise
  the APU thread's priority or the ring depth.
- No production audio fix was needed.

### `frames/run_audio.mp4`

265.5 s, 1664x896 H.264 veryfast + AAC 160k. 0-247.7 s: title and menus on the wall clock, audio in sync
(cross-correlation of the muxed track against the WAV: offsets exact at 150/240/250/260 s). From 247.7 s: stereo
frames at 12 game frames/s; each took ~1.3 s wall here, so the picture covers ~15x more game time than the audio:
the audio is the first 18 s of level 1 in real time, increasingly ahead of the picture. `frames/run.mp4` is the same
run on the game clock (61.7 s, no audio), as before. Run 9's frames and video are in `frames_run9/`.

## 5. Production changes and the Windows build

Only `XR/src/nv2a/kelvin/vk/kvk_backend.c` (the Vulkan backend; Windows builds the d3d11 backend:
`XR/src/nv2a/CMakeLists.txt` selects vulkan only for Android). No shell file changed: the shell's own passes and
barriers produced no messages. Test-only: `tests/game_render_x86/{host_stubs.c,run.sh.in,make_video.py,
analyse_audio.py}`, `tests/kelvin_vk/test_backend.c` (`KVK_TEST_BDA=1`). No Windows-built file changed, so no token
comparison was needed beyond that.

## 6. Tests

| Suite | Result |
|---|---|
| tests/kelvin_vk (`/root/build/kvk-tests`) | 5/5 (also clean under core + sync + GPU-AV) |
| `/root/build/kvk-android` | 2/2 |
| tests/vr_glue (`/root/build/vr-glue`) | 2/2 (251 + 255 checks), clean under core + sync |
| tests/xr_shell (`build-xr-shell`, `/root/build/xr-shell-check`) | 4/4, 4/4 |
| tests/nv2a_arm64 (`nv2a-x86`, `nv2a-a64`) | 10/10, 6/6 |
| `build-boot-a64` (after rebuilding `build-game-android`) | 1/1 |
| headless game (`run.sh`, run 10; validation run 2) | level 1 in stereo both times (215 / 256 stereo frames), no `[CRASH]` |
| APK `BUILD_DIR=build-quest-game port/quest/build_apk.sh --with-game` | rebuilt (kvk_backend.c recompiled), zipalign OK, apksigner v3 verified |
