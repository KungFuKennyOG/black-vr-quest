# Renderer stream: Kelvin NV2A -> Vulkan (Quest 3, arm64, multiview stereo)

Status 2026-11-02. A Vulkan backend for the Kelvin engine exists, builds as a static library for arm64-v8a with
NDK r27c, links into an Android `.so` with `--no-undefined`, and runs end to end on a real Vulkan implementation
(SwiftShader, built from source) where its output is compared pixel by pixel against Kelvin's own CPU renderer.
The Windows D3D11 build is unchanged (same preprocessed tokens and the same object code for `kelvin_gpu.c`).
Nothing has run on a Quest or any Adreno GPU yet.

## 1. What the Kelvin GPU path depends on (D3D11 inventory)

The engine (`kelvin.c`, `kelvin_render.c`, `kelvin_pgraph.c`, `kelvin_workers.c`, `kelvin_mmio.c`) reaches the
host GPU **only** through the `kgpu_*` functions in `kelvin_gpu.h` (draw / clear / flip / views / vertex-program
readiness / ordering hooks), plus `kgpu_set_ao_camera`, `kgpu_request_capture`, and `kgpu_capture_result` called
from `src/recomp_manual.c`. No D3D type crosses that boundary. Everything D3D lives in `kelvin_gpu.c` and its
`.inc` files, plus `kelvin_rt.c`, `kelvin_dlss.c`, and `kelvin_texpack.c`. There are 713 `ID3D11*` references
(kelvin_gpu.c 325, post 105, draw 96, rt 92, ssr 45, dlss 30+17, rt.c 25, time 14, prof 13, texpack 12,
vsh 11, pss 4).

**Core (ported):**
- **Device:** DXGI adapter pick and `D3D11CreateDevice`.
- **Shader compile:** `D3DCompile`, loaded via `LoadLibrary("d3dcompiler_47.dll")`. Fixed HLSL covers the CPU-vertex
  VS, the combiner ubershader (with a flat variant), clear VS/PS, depth clear, and depth↔colour reinterpretation
  (d2c/c2d). There is one generated VS per NV2A transform program (vs_5_0, quick then optimised build, compiled on
  worker threads, cached on disk with a manifest and precompiled at boot) and one generated PS per combiner
  configuration (ps_5_0).
- **State objects:** blend/depth/raster/sampler caches.
- **Buffers:** dynamic ring buffers: a 64 MiB float4 SRV for CPU-shaded vertex records, a 64 MiB raw R32_UINT
  "arena" for guest vertex data, and a 16 MiB index buffer. Constant buffers: PSConst, VSConst, VSG, C[192], and
  attribute descriptors.
- **Surfaces:** Texture2D with RTV/SRV, and R32G8X24 depth surfaces. Main 640x480 targets are rendered at host
  resolution.
- **Textures:** guest texture decode (swizzle, all texel formats, YUV, DXT; native BC when the size allows).
- **Copies and passes:** feedback copies (a draw that samples its own target), the d2c/c2d passes, and clears
  (full, partial, masked by draw).
- **Readback:** staging textures.
- **Present:** an NT-shared texture ring with D3D11 fences to `fb_present`, or CPU readback.

**PC extras (stay D3D11-only and are compiled out of Vulkan; their entry points are stubs there):**
- SSR (`kelvin_gpu_ssr.inc`)
- AO/DOF/FXAA/colour post (`kelvin_gpu_post.inc`, compute shaders)
- DLSS/DLAA (`kelvin_dlss.c`, `kelvin_gpu_dlss.inc`)
- D3D12 ray tracing (`kelvin_rt.c`, `kelvin_gpu_rt.inc`, stream-out GS)
- texture packs (`kelvin_texpack.c`)
- profilers and samplers (`_prof`, `_time`, `_slow`, `_incl`)
- split-screen views (`pc_split`)
- photo capture (returns -1 on Vulkan)
- shared-texture presentation
- the async submission thread (`kelvin_gpu_async.inc`)
- RECOMP_KGPU_COMPARE/PROBE/PASSCMP/DRAWLOG diagnostics

## 2. The seam

`kelvin_gpu.h` already is a narrow, backend-neutral interface. So the seam is the **link-time choice of
implementation**, and only backend-neutral code was factored out:

- `src/nv2a/kelvin/kelvin_gfx.h` documents the contract and the PC-only extras. It also declares the three
  title-thread entry points and two VR view modes for `kelvin_mark_view`: `KG_VIEW_VR_WORLD=16` and
  `KG_VIEW_VR_SCREEN=17`.
- `src/nv2a/kelvin/gfx/*.inc` holds four blocks **moved verbatim out of kelvin_gpu.c** and `#include`d back at the
  same places:
  - `kgfx_cfg_types.inc`: the PSConst/VSG layouts.
  - `kgfx_cfg_fill.inc`: KPixelState→PSConst.
  - `kgfx_hash.inc`: the texture signature. It uses CRC-32C via SSE4.2 on x86, the ARMv8 CRC extension on arm64,
    or software elsewhere, and gives the same values on every path.
  - `kgfx_texdecode.inc`: texel decode, swizzle, and DXT.
- `src/nv2a/CMakeLists.txt` adds `KELVIN_GFX_BACKEND = d3d11 | vulkan`. Android defaults to vulkan; every other
  host defaults to d3d11 with the original source list. With vulkan, `kelvin_gpu.c/_rt/_dlss/_texpack` drop out
  and the `kelvin_vk` objects go into `xbox_nv2a`. This is a cyclic dependency: kvk needs `kelvin_unsupported` and
  `nv2a_vsh_parse_program`.
- **Windows unchanged, verified:** `kelvin_gpu.c` was preprocessed for x86_64 Windows (clang + mingw-w64 headers).
  The token stream is identical to the original (734,790 tokens), and `llvm-objdump -d` plus `.rdata` of the object
  are byte-identical. The MSVC/clang-cl build itself was not run here.

## 3. Vulkan backend (`src/nv2a/kelvin/vk/`)

| File | What |
|---|---|
| `kvk_device.h` | Host API: `kvk_set_device` (shell-created VkInstance/PhysicalDevice/Device/queue plus enabled features and an optional queue lock), `kvk_set_raster_size`, `kvk_set_stereo`, `kvk_set_cache_dir`, `kvk_shutdown`, plus test/diagnostic calls `kvk_read_surface`, `kvk_get_stats`, `kvk_wait_compiles`. |
| `kvk_backend.c` | Implements all of `kelvin_gpu.h`. It also has weak `vr_*` defaults, so it runs flat without a shell. |
| `kvk_glsl.[ch]` | GLSL generators: NV2A vertex programs, specialised combiners, the combiner ubershader, the CPU-vertex VS, internal passes, and position classification. |
| `kvk_spirv.[ch]` | glslang C-interface wrapper (GLSL 450 → SPIR-V 1.3, Vulkan 1.1). |
| `kvk_stereo.[ch]` | Per-eye math (section 5). |
| `kvk_vkmap.[ch]` | NV2A state → Vk enums (blend, compare, stencil, address, filter, border, surface formats). |
| `kvk_cfg.h` | The shared PSConst layout for the vk modules. |
| `CMakeLists.txt` | Targets `kelvin_vk_shadergen` (pure C) and `kelvin_vk` (backend + glslang + Vulkan). |

**Design (mirrors the D3D11 backend's semantics):**
- **Submission:** synchronous like D3D11 with RECOMP_KGPU_ASYNC=0. `kgpu_async_active()` is false, `defer_call`
  runs immediately, and `get_bound` returns `get`. The Kelvin GPU thread records one command buffer per frame plus
  an upload command buffer submitted ahead of it. Two frames are in flight, each with a fence, a chain of 32 MiB
  host-visible ring chunks (constants, vertex data, indices, staging), descriptor pools, and a deferred-destroy
  list.
- **Descriptors:** set 0 holds six dynamic descriptors per chunk: UBO PSConst / VSG-or-VSConst / C[192] / attribute
  table / stereo block, and an SSBO with the vertex data (4 MiB window). Set 1 holds four combined image samplers,
  cached per frame. A 64-byte push constant carries the texture-layer bits, the stereo flag, and the vertex-data
  size. Out-of-range vertex reads return 0, as D3D11's typed buffer does.
- **Render passes:** opened lazily per (colour, depth) pair. A full clear on a surface with no open pass becomes the
  next pass's `loadOp CLEAR` (tiler-friendly). Partial clears use `vkCmdClearAttachments`; masked clears use a quad.
  Layout transitions are tracked per image, and explicit external subpass dependencies are set.
- **Pipelines:** keyed on (VS module, FS module, compatible render pass, packed blend/depth/stencil/raster state).
  Viewport, scissor, blend constants, stencil masks and reference, and depth bias are dynamic. Pipelines go through
  a `VkPipelineCache` saved to `<cache>/vk_pipeline_cache.bin`.
- **Shaders:**
  - Generated GLSL is compiled on worker threads and cached as `<cache>/vk_{v,f}_<hash>_<len>.spv`.
  - Until a vertex program's module is ready, `kgpu_vs_ready()` returns false and the engine CPU-shades that draw.
  - Until a combiner's module is ready, the ubershader is used.
  - A vertex-program manifest (`vk_vs_manifest.bin`, same record format as D3D11's) re-queues every program seen
    in earlier runs into the SPIR-V cache at init.
- **Textures:** same cache and revalidation as D3D11 (once per kick, content signature). A changed texture gets a
  new image, so earlier draws in the frame keep the old one. DXT uploads natively when `textureCompressionBC` is
  enabled; otherwise it is decoded to RGBA8 (the Quest case).
- **Depth:** D24S8 if supported, else D32S8. Depth bias is converted exactly as D3D11 converts it. depth↔colour
  reinterpretation (BLACK's glare pass) and feedback copies are ported.
- **Difference from D3D11:** blend factors 14/15 use the constant's alpha (`CONSTANT_ALPHA`), as the CPU reference
  does. D3D11 approximates them with the whole constant colour.

## 4. GLSL → SPIR-V: glslang at run time (decision)

**Picked:** glslang, compiled from source into the app (`/root/deps/glslang` @ b93de9f, ENABLE_OPT=OFF, HLSL off).
It builds for arm64 with the NDK: the whole backend plus glslang is 3.7 MB stripped. The NDK's shaderc copy has no
CMake build and is older, so it was not used.

**Why not emit SPIR-V directly:**
- **Cost:** the generators would have to be rewritten.
- **No validator in the loop:** the GLSL path lets the tests prove the generated code mirrors the D3D11 HLSL
  line by line (section 7).
- **Small saving:** glslang is not where the hitch is.

**Measured (x86 desktop core, single thread):**

| Step | Time |
|---|---|
| GLSL generation | ≈15 µs |
| glslang, combiner shader | 5.4 ms |
| glslang, ubershader | 5.2 ms |
| glslang, 40-step vertex program | 6.8 ms |

Expect roughly 2x that on Quest's A78 cores. The driver's `vkCreateGraphicsPipelines` (Adreno, unmeasured; SwiftShader ≈100 ms
each) costs more than glslang and exists whichever way SPIR-V is produced.

**Hitching on Quest:**
- **Shader compiles:** off the GPU thread, with fallbacks (CPU vertices / ubershader), so no stalls.
- **Warm runs:** the SPIR-V cache and manifest make shader availability immediate.
- **Remaining hitch:** **pipelines are still created synchronously on the GPU thread** the first time each
  (shaders, state) combination is drawn. `VkPipelineCache` turns that into a fast cache hit on later runs, but first
  play-through hitches. Fixes, in order of value:
  1. Use `VK_EXT_extended_dynamic_state`/`2`/`3` (Adreno 740 has 1 and 2) so blend, depth, and cull stop
     multiplying pipelines.
  2. Record (shader, state) keys to a manifest and precompile them at boot on workers.
  3. Create pipelines asynchronously and drop or substitute the draw for the frame or two it takes.

## 5. Stereo (multiview) and where the eye transform is applied

**Multiview:**
- `kvk_set_stereo(true)` makes **every** Kelvin render target a 2-layer image array, and every pass a multiview pass
  with view mask 0b11. Each eye gets its own copy of every offscreen chain: shadow, reflection, glare, and depth
  reinterpretations, all per eye.
- Textures are `sampler2DArray`. A push-constant bit per stage says "this texture is a per-eye surface: sample
  layer `gl_ViewIndex`", otherwise layer 0. Camera-independent offscreen work is done twice; BLACK's offscreen work
  is small.
- **Fallback (replaying a frame's draws per eye) was not needed:** multiview is core in Vulkan 1.1 and on all Quest
  drivers.

**Eye transform:**
- **Where:** in the vertex stage, after everything the D3D11 shader does, in `kvk_finish()` in `kvk_glsl.c`. The
  finished D3D-convention clip position is multiplied by `eye_clip[gl_ViewIndex]` when the draw's push constant
  says "world", then y is flipped for Vulkan.
- **No title constants are patched:** every world vertex, from any program, comes out as `P_game·V_game·M·x`. So
  `C = P_eye·V_eye·inv(V_game)·inv(P_game) = P_eye·E·inv(P_game)`, where E is the eye relative to the game camera.
  This covers rigid, skinned, CPU-shaded, and first-person-weapon draws alike. Model and view never need separating
  at the Kelvin level.
- **Two forms are implemented and cross-checked:**
  - **FULL** acts on (x,y,z,w). It is exact only if the published P_game depth row (near/far) is exact. With a
    wrong near/far the eye separation is mis-scaled; the test demonstrates this.
  - **FROM_W** acts on (x,y,w,1). It recovers the view point from x, y and w (w is view depth for any perspective),
    so **parallax is exact regardless of near/far**; the depth row only sets written depth values. This is the
    default.
- **Conventions:** the axis signs of the game's view space (x left/right, ±z forward) are read off P_game, so OpenXR
  poses convert without knowing RenderWare's handedness. This is tested with D3D, mirrored-x, and −z conventions.
- **Inputs used:**
  - `VrFrameState.head`, `.eye[i].pose`, `.eye[i].fov`, `.world_scale`
  - `VrGameCamera.proj` when valid, else `half_fov_x/y` with a default near/far
  - `VrEye.view/proj` are not used.
- **Model assumed:** the game hooks drive the game camera from the HMD head pose, so only the eye-from-head offset
  is applied here.

**Which draws get it:**
- Only draws on the main (640x480 guest) target, before the HUD starts, whose position is **perspective**.
- **GPU programs:** a static data-flow pass (`kvk_vsh_pos_class`) asks whether oPos.w comes out of a dot product of
  vertex input with constants. Pre-transformed screen-space passes, like glare and full-screen effects that
  read the eye's own layer, stay pixel-aligned per eye, which is correct.
- **CPU-shaded draws:** w ≠ 1 marks the draw perspective.
- **Screen-space effects (2026-11-11, section 11):** every other draw of a STEREO frame's world pass is corrected
  too, except full-screen passes (reaching all four edges and covering the target). Its w is rebuilt from z through
  the game projection's depth row first (`KVK_DF_SCREEN_DEPTH`). The same holds for the glare chain's 4:3 offscreen
  view images (160x120, 80x60).
- **Explicit override:** the game hooks can mark regions with `kelvin_mark_view(va, KG_VIEW_VR_WORLD /
  KG_VIEW_VR_SCREEN)`.

**HUD / 2D / menus / movies / scope:**
- **HUD start (auto):** after ≥4 depth-writing world draws, the first main-target draw that tests depth with ALWAYS
  and writes none begins the HUD. This is the rule D3D11's post-processing already uses to find the HUD.
- **HUD start (explicit):** `KG_VIEW_VR_SCREEN`.
- **HUD redirect:** from then on, main-target draws go to a mono 1280x960 HUD surface that keeps premultiplied
  coverage. Alpha blends with ONE / ONE_MINUS_SRC_ALPHA; draws that don't blend force alpha to 1.
- **At the flip:** each eye image is composed in one pass per eye image (one multiview pass when both eyes share an
  array swapchain). The composition draws:
  1. The world, copied from the main target's layer by sampling. This keeps gamma-space colours correct on an SRGB
     swapchain written through its UNORM twin view, and allows scaling.
  2. The HUD on a head-locked virtual screen: 2.4 m wide at 2 m, 4:3, with per-eye parallax.
- **Frames without world draws** (menus, movies, loading screens) show the whole picture on that virtual screen over
  black.
- **Scope masks:** they draw after the world and end up on the HUD layer. A stencil-based mask would not see the
  world's stencil there (open issue).

**Budgets (Quest 3, XR2 Gen 2):**
- **Frame budget:** 72 Hz = 13.9 ms, 90 Hz = 11.1 ms. Allow about 1–1.5 ms for the compositor.
- **Rendering cost:** multiview keeps CPU submission and vertex fetch at about mono cost. Fragment work is 2x at a
  given per-eye resolution.
- **Suggested start:** a 1440x1584 per-eye raster (about 0.7x the recommended 2064x2208) with D24S8 and BGRA8.
  Per eye that is 2x9 MB colour plus 2x9 MB depth for the main target, plus HUD 4.9 MB. Tune with
  `kvk_set_raster_size`.
- **Game rate:** the title will likely tick below 72 Hz. Submit every frame the game produces and let OpenXR
  reproject (ATW). That requires the pose the frame was rendered with (request 1 below).
- **Not measured:** no GPU timings on Adreno.

## 6. Build

- **Targets (CMake):**
  - `kelvin_vk_shadergen`: `kvk_glsl.c`, `kvk_stereo.c`.
  - `kelvin_vk`: `kvk_backend.c`, `kvk_spirv.c`, `kvk_vkmap.c`, plus glslang and Vulkan.
  - Both are defined by `third_party/xboxrecomp/src/nv2a/kelvin/vk/CMakeLists.txt`. They are pulled into
    `xbox_nv2a` when `KELVIN_GFX_BACKEND=vulkan`, which is the Android default.
- **Cache variables:** `KVK_GLSLANG_DIR` (required), `KVK_VR_BRIDGE_DIR` (defaults to `port/quest`), and
  `KVK_VULKAN_HEADERS` (non-Android hosts without system headers).
- **arm64 artifacts built here** (`/root/build/kvk-android`, toolchain `/root/xtool/android-arm64-static.cmake`,
  android-29):
  - `kelvin_vk/libkelvin_vk.a` (0.8 MB)
  - `kelvin_vk/libkelvin_vk_shadergen.a`
  - glslang static libs under `kvk-glslang/`
  - `libkvk_linkcheck.so`: links with `-Wl,--no-undefined`; NEEDED libm, libvulkan, liblog, libdl, libc; 3.7 MB
    stripped. It exports `kgpu_*`/`kvk_*` and has weak `vr_*`.
- **Commands:**
  ```sh
  . /root/xtool/env.sh
  cmake -S tests/kelvin_vk -B build-kvk-a64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DKVK_GLSLANG_DIR=/root/deps/glslang
  cmake --build build-kvk-a64 && ctest --test-dir build-kvk-a64      # kvk-stereo, kvk-shadergen under QEMU
  ```
- **Still blocked:** a full `xbox_nv2a` arm64 build fails in the Kelvin **core**, not the backend. `kelvin.c`,
  `kelvin_mmio.c`, `kelvin_workers.c`, `kelvin_render.c`, `kelvin_pixel.c`, `kelvin_texture.c`, `kelvin_vsh.c`,
  and `nv2a_vsh_emulator4.c` use `<windows.h>`/`<intrin.h>`, SSE/AVX, and MMX. They need the POSIX compat headers
  and NEON (sse2neon) work from the platform/arm64 stream. The backend objects themselves build inside `xbox_nv2a`.

## 7. Tests (`tests/kelvin_vk/`)

- **Native build:**
  ```sh
  cmake -S tests/kelvin_vk -B build-kvk -G Ninja -DKVK_GLSLANG_DIR=/root/deps/glslang \
    -DKVK_VULKAN_HEADERS=/root/deps/Vulkan-Headers/include \
    -DKVK_TEST_ICD=/root/build/swiftshader/Linux/vk_swiftshader_icd.json
  ```
  then `ctest`.
- **SwiftShader (needed to run the backend tests):** built from source at `/root/deps/swiftshader` (Subzero JIT,
  `vk_swiftshader` target only) into `/root/build/swiftshader`. Without it, the `kvk-backend` tests exit 77
  (skipped).

| Test | Checks | Result |
|---|---|---|
| `kvk-stereo` | FULL == FROM_W == general formula == direct eye projection for random cameras (3 conventions), heads, and canted eyes. Parallax sign and magnitude match plain trigonometry, and vanish at infinity. Only FROM_W stays exact with a wrong near/far. Virtual-screen quad placement. | 168,237 checks pass (native + arm64) |
| `kvk-shadergen` | 400 random combiner configs and 300 random vertex programs (all opcodes, swizzles, negation, write masks, relative addressing, R12/oPos aliasing, constant writes refused) compile with glslang. After a fixed HLSL→GLSL spelling map, their bodies are **text-identical** to what the production D3D11 generators (extracted from `kelvin_gpu_pss.inc`/`kelvin_gpu_vsh.inc` by `extract_d3d.py`) emit. The PS key equals `ps_key_of`. All fixed shaders compile. Position classification is checked on hand-written programs. Mutation check: changing one constant in the GLSL generator fails 255 configs. | pass (native + arm64) |
| `kvk-backend-cpu-parity` | 384 random combiner configs (1–4 stages, final combiner, swizzled/linear/DXT1 textures, projective coords, alpha test, alpha kill, colour keys) are drawn through `kgpu_draw` and through `kpix_fragment`/`ktex_*` (Kelvin's CPU renderer), byte-compared. Then 384 random vertex programs on the GPU path vs the `nv2a_vsh_cpu` emulator. Plus every attribute format (CMP, UB_D3D, UB_OGL, S1, F, S32K, unaligned), depth test LESS/GREATER, masked and partial clears, depth-as-colour (Z24S8 packing). | specialised shaders, MSB mux: **0/384 differ, max 1/255**. Random mux: 3/384 (see 8). Vertex programs: 3/384 beyond 2/255 (RSQ precision, one SLT boundary). |
| `kvk-backend-uber` | Same combiner comparison with the ubershader, DXT decoded on the CPU (Quest has no BC). | MSB: 0/384, max 1/255 |
| `kvk-backend-stereo` | Stereo multiview with a fake shell (`vr_*` strong symbols) and an Xbox-style transform program. Each eye's quad centre matches trigonometry within 0.2 px (136.00 vs 136.19 px, 120.00 vs 119.81 px). The HUD is auto-detected, kept out of the world layers, and appears on the virtual screen in both eye images at the expected place and parallax (within 0.2 px). A menu frame goes on the virtual screen. The eye targets are released once per flip. | pass |

## 8. Findings in existing code (not changed)

- **`nv2a_vsh_cpu_log` bug:** the emulator (`kelvin/nv2a_vsh_cpu/nv2a_vsh_cpu.c`) computes the exponent as
  `uint32_t` (`((bits >> 23) & 0xFF) - 127`). For |x| < 1, `LOG.x` is about 4.29e9 instead of the negative exponent.
  The D3D11 HLSL (and so this GLSL) uses the signed value, which means the PC build's CPU-vertex and GPU-vertex
  paths already disagree there. It is a one-line fix (`int32_t`), left alone because it changes PC CPU-path
  behaviour. `kvk-backend` prints both values.
- **LSB mux sensitivity:** in combiner LSB-mux mode (`(uint)(r0.a*255) & 1`), a 1-ulp difference between GPU UNORM
  conversion and the CPU's `k*(1/255)` flips the mux. This is shared by the D3D11 backend. Real NV2A uses 8-bit
  fixed point; rounding instead of truncating, `uint(r0.a*255 + 0.5)`, in all three implementations would make
  them agree.
- **Blend factors 14/15:** D3D11 maps them to the whole blend colour; the CPU renderer and Vulkan use its alpha.

## 9. Open issues / not done

1. Pipeline creation is synchronous (section 4). Use extended dynamic state, a pipeline manifest, and async creation.
   **Done** (manifest + async creation on the workers, Android default): perf.md section 3. Extended dynamic state not used.
2. The game camera must belong to the frame being drawn. Today `vr_game_camera()` is sampled at the first draw of a
   frame on the GPU thread, which may be a newer title frame than the one being drawn. It should travel with the
   pushbuffer, as `kelvin_mark_frame_aspect` does (request 1).
3. Not yet done:
   - the HUD as a real OpenXR quad layer (request 2)
   - a stencil-correct scope mask in VR
   - mirror/water reflections that use screen-projective lookups of a mono texture will be per-eye wrong
4. Device memory is one `VkDeviceMemory` per image, with no sub-allocator. Textures are never evicted (as on D3D11).
   Watch `maxMemoryAllocationCount`; add a sub-allocator (VMA or a simple slab).
5. Raster size, HUD size, and virtual-screen size and distance are fixed defaults with no settings UI.
6. Not yet ported:
   - split-screen (`KG_VIEW_FRAME/FIT`)
   - photo capture
   - async submission thread (probably unnecessary: Vulkan recording is cheap, so measure first)
7. Not exercised by tests:
   - stencil
   - cube textures (unsupported on both GPU backends; the CPU renderer only)
   - the c2d (colour→depth) pass
   - shell-provided SRGB swapchains with mutable format
   - the `queue_lock` path
8. Nothing has run on Adreno. SwiftShader validates semantics, not performance or driver quirks. Run with the
   validation layers on the device first.

## 11. Effects in one eye: screen-space sprites (fixed 2026-11-11)

**Symptom (first Quest 3 run):** muzzle smoke, impact dust, sparks, fire and explosions showed in one eye only.
World, gun and HUD were fine. Reproduced headless (`game_render_x86`, multiview, level 1 with the right trigger held,
`GRX_INPUT=...,330:rt:6`; the harness gained `rt`/`lt` through a new `fx_input_float` hook in the fake runtime):
fire at a window drawn in the right eye only (`build-render-x86/sprite_fix_before_after.png`; before run
`frames_fx2/video/v002841`, after run `frames_fx3/video/v002093`).

**Cause:**
- BLACK projects its particles on the CPU through the camera and draws camera-facing quads in **screen space**:
  - vertex program `oPos = v0 * c98 + c97`, with c98 = (640, 480, zmax*B, 1), c97 = (0.53, 0.53, zmax*A, 0) and
    v0 = (x/640, y/480, 1/w, 1);
  - so **w = 1** and the depth is only in z;
  - drawn depth-tested (LEQUAL), no depth write, blended (SRC_ALPHA/ONE_MINUS_SRC_ALPHA, or additive).
- `kvk_vsh_pos_class` rightly calls that program PASSTHROUGH (no dot product), and `cpu_w_perspective` says the same
  for its CPU-shaded first frames (w = 1). The draw got no eye correction, so both eye layers got the **game
  camera's** picture of the sprite at the same pixel.
- With the asymmetric eye FOVs, that pixel is a different direction in each eye: in the vr_glue scene it is 43 px
  from where the eye should see the sprite; in level 1, distant content is ~125 px apart between the eyes, while the
  sprite stayed at one pixel.
- The sprite is depth-tested against each eye's corrected depth buffer at that wrong pixel. In one eye that pixel
  lands on a nearer wall, window frame or the gun, and the sprite is rejected; in the other it passes. That is
  "visible in one eye". Nothing to do with multiview vs two swapchains, layer selection or FDM: the Kelvin targets
  are always multiview, and both swapchain modes compose the same layers.
- The muzzle flash itself (program 78E49D77, MVP in c112) was already a transformed world draw. Its smoke and sparks
  are sprites.

**Fix, first pass (2026-11-11, 22:08 APK):** depth-tested non-perspective draws only. On the headset that was
"mostly fixed, some effects still wrong". The light shaft over a fire (reference footage: a tall, bright, additive
column next to a building) was still wrong.

**Fix, second pass (`XR/src/nv2a/kelvin/vk`), what is in place now:**
- **Every non-perspective draw of a STEREO frame's world pass** is a screen-space effect and gets the eye correction
  (`kvk_backend.c` `screen_effect`). That holds depth-tested or not, before or after the frame's first world draw.
  - "STEREO frame" means the glue reports `VR_PRESENT_STEREO`; without a glue, a world draw must come first.
  - Menus, movies and FLAT frames are untouched; the HUD (`hud_starts`) is never in the world pass.
  - Such a draw is flagged `KVK_DF_SCREEN_DEPTH` and counted in `KvkStats.sprite_draws`.
- **Exception: full-screen passes** (fades, flashes, colour filters, glare composite and blur steps) stay
  pixel-aligned per eye (`np_fullscreen`, counted in `KvkStats.fullscreen_draws`).
  - Where a draw lands is computed on the CPU (`np_extent`): GPU programs run through the NV2A vertex emulator
    (`nv2a_vsh_emu_execute`; CPU-shaded draws carry their records), up to 32 triangles per draw.
  - A pass counts as full-screen only when it reaches **all four edges** of its target (within a pixel) and covers
    it: one triangle ≥ 45 % of the target (a quad's half, or the single big triangle of BLACK's passes), or a
    mesh at one depth summing to ≥ 98 %.
  - Large effects that miss an edge stay corrected: a full-height light shaft, an explosion filling most of the view.
- **Offscreen view images (the light shafts):**
  - BLACK's glare chain draws fire, smoke and glow sources into 160x120 and 80x60 targets. Programs `A1A4C702`,
    `B76536BC` and `3516849B` (depth-tested, additive) draw into va `83248000`/`83270000`. The image is blurred into
    streaks (`F833FAD3`, `8ADAFB52`, full-target triangles) and added back over the main target (`A82C1860`).
  - Those targets were never corrected: both eye layers held the game camera's glare, so the shaft sat at the same
    pixel in both eyes.
  - Now a 4:3 colour target smaller than 640x480 (`eye_aligned_target`) gets the main target's treatment for its
    world-pass draws before the HUD: perspective draws through eye_clip, screen-space ones as above, full-target
    blur/copy steps left. Square or odd targets are left: shadow maps, and the 64x32 `60ED135F` target.
  - Counted in `KvkStats.offscreen_stereo_draws`.
- **Shader (`kvk_glsl.c kvk_finish`):**
  - w = B / (z/w − A) from the depth row (`KvkStereoFrame.misc[2..3]` = A = m22/m32, B = m23 of P_game, from the
    camera hook's measured row).
  - With no usable z (z ≤ 0: no depth given; or beyond the far plane), the vertex is placed in its own direction at
    half the far distance, B/(1 − A)/2, with its own depth value kept. That is a fallback for glows that carry no
    depth: their direction is exact, their parallax that of a distant object.
  - No depth row: the game's position.
- Every vertex shader's text changed, so the SPIR-V cache refills once. Stale vertex modules and their pipeline
  records were pruned from `build-render-x86/scratch/root/kgpu-cache` (the shipped cache) after the first pass.

**Level-1 data (`RECOMP_KVK_SPRITELOG=1`, `build-render-x86/run_fx4.log`, `run_fx5.log`):**
- Non-perspective main-target world-pass draws:
  - particle program `3516849B` (and its CPU-shaded first frames): corrected;
  - `A82C1860` and `94EDF4FB`: three-vertex triangles over 0..1280 x 0..960, full-screen, left.
- The HUD begins at `60ED135F` (the health bar, x 45–181, y 26–81), never earlier.
- Offscreen: the glare chain above.

**Regression tests (`tests/vr_glue` frame 2, both swapchain modes; 386/390 checks):**
- Corrected effects, each where each eye sees its world point (≤ 1.5 px; measured ≤ 0.5 px), and different between
  the eyes:
  - a depth-tested program sprite;
  - a depth-tested CPU-shaded sprite;
  - a no-depth-test glow drawn before the world;
  - a glow with z = 0 (at half the far distance along its ray);
  - a full-height light shaft that does not reach the side edges.
- A full-screen additive gradient stays pixel-aligned in both eyes (blue = 40·x/W at the left edge, the middle and
  the right edge).
- In the 160x120 view image, the program sprite and a perspective quad (its own viewport rows) are at each eye's
  position in that eye's layer (≤ 0.75 px). A 64x64 target keeps the same pixel in both layers.
- Stats: 6 effects corrected, 1 full-screen pass, 2 glare-target draws.
- Without the offscreen rule, 5 checks fail; without the first-pass rule, 7.

**Limits:**
- Parallax is as exact as the depth row. BLACK's (1.00002, −0.08) matches the sprites' own c97/c98 mapping within
  0.03 %.
- Glows without a z sit at half the far distance: direction right, parallax that of the far scene.
- A depth-tested full-screen quad at constant depth is still left alone only if it reaches all four edges.
- A 4:3 offscreen target drawn from another camera (a mirror, a security monitor) would be corrected as if it were
  the player's view. None in level 1.
- Not yet confirmed on the headset.

## 12. VR debug aids: effect tint, both-eye capture, sprite log

- **`BLACKVR_SPRITE_DEBUG=1`** (environment, or blackvr.ini `env BLACKVR_SPRITE_DEBUG=1`): on the main target, each
  draw corrected as a screen-space effect is drawn again in translucent **green**, and each blended non-perspective
  world-pass draw that was **not** corrected (the full-screen passes) in translucent **red**. The HUD is never
  tinted.
  - Same mechanism as `BLACKVR_GUN_LATCH_DEBUG` (`draw_tint`: the draw again with a flat colour, depth-tested
    against itself).
  - The offscreen glare images are not tinted; they show through the composite.
- **Both-eye capture:** a capture saves the next STEREO frame's two eye images as PNG to `<game root>/shots/`
  (`/sdcard/BlackVR/shots` on the headset) and logs `CAPTURE saved <left> <right>`.
  - Files: `eye_<date>_<time>_f<renderer frame>_L.png` / `_R.png`, RGB as the swapchain holds the bytes,
    uncompressed deflate, about 5.7 MB per eye at 1344x1408.
  - Three triggers:
    - both thumbsticks held **2.5 s** (`VR_CAPTURE_HOLD_S`). The same chord recentres at 1 s, so keep holding past
      the recentre;
    - `BLACKVR_CAPTURE_EVERY=<seconds>`;
    - `vr_capture_request()` from code.
  - How (`kvk_vr_layers.inc` `vrl_capture_record`/`_deliver`):
    - the frame's composition is drawn once more into a 2-layer RGBA8 image of the backend's (the swapchain images
      have no TRANSFER_SRC usage), and copied to a readback buffer;
    - that frame's submit is waited for (one frame's stall, only on a capture frame);
    - the pixels go to `kvk_vr_capture_done`, and the glue (`vr_renderer_kvk.c`) writes the PNGs on a thread of its
      own.
  - The HUD is not in it when the HUD is on its own quad layer.
- **`RECOMP_KVK_SPRITELOG=1`:** one log line for each of these, in stereo frames:
  - each non-perspective world-pass draw and its decision (extent, z range, area, depth and blend state);
  - the draw that begins the HUD;
  - the blended draws into offscreen targets, with their decision;
  - the HUD draws.
  `RECOMP_KVK_SPRITELOG=2` also logs each blended perspective world draw (`PFX`: guest-pixel extents, view depth, the
  constant registers its position reads, gun-latch class) and adds to every `SPRITE` line the eye NDC where its centre
  lands (gun-latch.md section 6).
- **Harness:** `GRX_INPUT` buttons `lc`/`rc` (thumbstick clicks), `rt`/`lt` (triggers). Captures land in
  `<scratch>/root/shots`. vr_glue checks that a requested capture's PNGs equal the released eye images (worst
  difference 0) in both swapchain modes.

## 10. Requests

**XR shell (owner of `port/quest/vr_bridge.h`; I did not edit it):**
1. **Frame and pose identity:** add the head pose and frame identity the camera was built from to `VrGameCamera`,
   e.g. `VrPose head; int64_t predicted_display_time; uint32_t frame_index;`. The renderer would then report which
   frame each composed image belongs to, e.g. a `frame_index` argument to `vr_acquire_eye_targets` or
   `vr_release_eye_targets`. The shell must submit the projection layer with *that* pose for timewarp to be right.
2. **HUD quad layer:** add `int vr_acquire_hud_target(VrEyeTarget *out); void vr_release_hud_target(void);` for an
   `XrCompositionLayerQuad`. The backend would then hand its mono HUD surface to the quad layer instead of drawing
   the virtual screen into the eye images.
3. **Device creation:** create the VkDevice with Vulkan 1.1 and `multiview` (required). Enable `robustBufferAccess`
   and `depthClamp`, and `textureCompressionBC` if present. Then call `kvk_set_device()`,
   `kvk_set_raster_size()`, `kvk_set_stereo(true)`, and `kvk_set_cache_dir(<app files dir>)` **before**
   `kelvin_start()`.
4. **Swapchains:** use a 2-layer array swapchain for both eyes (one multiview composition pass), or one per eye;
   both are supported. Request `COLOR_ATTACHMENT` usage. If the format is `_SRGB`, also request `MUTABLE_FORMAT`
   with the UNORM twin in the format list and set `KvkDeviceInfo.swapchain_mutable_format`. Images are expected,
   and returned, in COLOR_ATTACHMENT_OPTIMAL.
5. **Queue lock:** if `xrEndFrame`/`xrReleaseSwapchainImage` can run on another thread than the Kelvin GPU thread,
   pass `queue_lock/queue_unlock`. The backend calls `vr_release_eye_targets()` right after its `vkQueueSubmit`
   in `kgpu_flip`.
6. **Contract wording:** state in `vr_bridge.h` that `VrGameCamera.proj` is the projection producing exactly what
   the title's vertex programs output for the world camera before the viewport (D3D clip: x right, y up,
   z/w∈[0,1]). Also state that `world_scale` is game units per metre.

**Game hooks (src/black_vr):**
- Publish `VrGameCamera` from the D2F40 hook: half-frustum from +48/+4C, plus near/far, or the real matrix.
- Drive the world camera from the head pose.
- Widen the game FOV to at least the eye FOV so the periphery is not culled.
- Leave `kgpu_set_viewmodel` scale at 1 under VR.
- Optionally mark `KG_VIEW_VR_WORLD` at world-camera begin and `KG_VIEW_VR_SCREEN` at the HUD through
  `kelvin_mark_view`.

**Platform / arm64 stream:** port the Kelvin core files listed in section 6 (x86 SIMD and `windows.h` includes).
The CPU vertex fallback (`kelvin_vsh.c`) is on the critical path whenever a vertex program is not yet compiled.

## Compile waits for world draws (2026-11-15)

Kenny on the headset: "sometimes the bodies only show the upper body and butt/crotch floating and moving, but no head
arms or legs, then they reappear". Leading cause (not yet confirmed from a device log): a draw whose vertex program,
combiner or pipeline is still being made on a compile worker was skipped, and a character's parts use different ones.
At a level start the worker queue holds the whole pipeline manifest, so a draw's own compile waited behind it.
- Compiles a draw asks for go to an **urgent queue** the workers take first; a manifest precreation a draw needs is
  copied there once (`KvkPipe.promoted`; `claim` makes sure only one copy builds it).
- A **world draw on the main target** (not the HUD) whose program or pipeline is not ready waits for it, at most
  10 ms in all per frame (`compile_wait`; `RECOMP_KVK_COMPILE_WAIT_US`, 0 = off). Past that it is skipped as before.
- Log: `world draw skipped: its vertex program|pipeline was not made in time (vs ..., frame ...; N world draws skipped
  so far, M compile waits, T ms waited in all)` (first 20, then every 200th) and, with the stats line,
  `compiles a world draw waited for: ...`. No such lines while the bodies break = it is something else.
- **Not the cause (headset log 2026-11-15 23:27):** `skipped 0` for the whole session and no wait lines, while the
  bodies still broke. The waits stay (harmless); the next lead is the draw lists below.

## Draw lists (2026-11-15)
The game's renderer keeps seven fixed-size draw lists ([0x2D67F4]+0xCA6C, sizes 1200/32/400/150/16/3/48 from the
table at 0x28F410, set up each frame by 15A5F0); a full list silently drops every mesh added after it. VR's view is
much wider than the TV's, so more meshes pass culling. Lists 0/2/3 now get their own room, 4096/2048/512
(`src/recomp_manual.c` BLACK_DRAW_LISTS; `RECOMP_BLACK_DRAWLISTS=0` turns it off). The sizes and the finding are
TGP482's ("fix objects vanishing at high draw distance"; facts only). Log every 600 frames when a list reached the
game's own size: `[BLACK] draw lists (last 600 frames): most entries ...`. No such line = this was not it either.
