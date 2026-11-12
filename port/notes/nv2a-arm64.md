# nv2a-arm64: the NV2A (Kelvin) core, USB and video runtime on Android arm64

Stream owner: nv2a-arm64 agent. Nothing committed. Paths are relative to /root/black; `XR` = `third_party/xboxrecomp`.

## Results at a glance

| What | Status |
|---|---|
| `port/android/nv2a.cmake` → `xr_nv2a`, `xr_video`, `xr_usb` (+ `xr_hw` INTERFACE), arm64-v8a, NDK r27c, android-29 | **build, 0 errors** (one pre-existing benign warning) |
| Whole-archive `.so` of every runtime archive (xr_usb, xr_nv2a, xr_video, xr_kernel, xr_apu, xr_dsound, xr_platform) | links; **5 strong undefined** non-libc symbols left (input ×3, game ×2; see below) |
| x86 unchanged: 87 runtime TUs × {x86-64 Linux, x86-64 Windows/mingw-w64} × {SSE, AVX2}, preprocessed, current tree vs. a tree with only my files reverted | **348/348 identical token streams** |
| Parity tests, x86-64 (gcc, AVX2 reference + SSE) | **10/10 pass** (incl. 2 ThreadSanitizer runs) |
| Parity tests, arm64 static under `/root/xtool/arm64run` (QEMU cortex-a76) vs the x86 AVX2 dumps | **6/6 pass** |
| Real arm64 parity bugs found and fixed | 3 float→int conversions (one visibly wrong: the LSB combiner mux) |
| Memory-ordering changes | 6 (below), plus a write trap that gives D3D's kickoff the barrier ARM lacks |
| tests/kelvin_vk 5/5, tests/vr_glue 2/2 after the coordinator's kvk_backend.c items | pass |

## 1. Targets and link order (`port/android/nv2a.cmake`)

`include()`d by `port/android/CMakeLists.txt` (it already does, with `OPTIONAL`). It reuses `XR`, `BLACK_ROOT`, `SSE2NEON_DIR`, `XR_ARM64_FLAGS`,
`XR_DEFS` and `XR_LINK_ANDROID_SYSLIBS` from the includer, or sets the same defaults itself; `include_guard(GLOBAL)`.
Enables CXX (glslang).

| Target | Contents |
|---|---|
| `xr_nv2a` | Kelvin: `kelvin.c kelvin_mmio.c kelvin_pgraph.c kelvin_render.c (KRES_PC_VIDEO=1) kelvin_vsh.c kelvin_pixel.c kelvin_texture.c kelvin_workers.c`, `nv2a_vsh_cpu/*` (emulator8 is empty without AVX2), `nv2a_mmio_hook.c`, `nv2a_core.c` (the PRAMDAC palette `nv2a_dac_*` kelvin_mmio needs), **new** `nv2a_pgraph_none.c`, and `$<TARGET_OBJECTS:kelvin_vk>` (the backend and the engine call each other, as upstream folds kelvin_vk into xbox_nv2a). PUBLIC: kelvin_vk_shadergen, glslang, glslang-default-resource-limits, xr_video, xr_platform, m, vulkan/log/android. |
| `xr_video` | all of `src/video` on its non-Windows paths: `fb_present` (xbox_Framebuffer*), `video_player`/`video_pump`, `pc_video`, `pc_video_menu`, `pc_menu(_assets)`, `pc_movie_skip`, `pc_photo`, `pc_crosshair`, `pc_debug`, `pc_split`. |
| `xr_usb` | `ohci.c`, `usb_gamepad.c` (xbox_OhciInit; `RECOMP_USB`). |
| `xr_hw` | INTERFACE: the three. |

Not built: `nv2a_pgraph_d3d11.c` (needs the D3D8→D3D11 device), `nv2a_pb_replay.c` (Burnout 3 menu data), `nv2a_pb_test.c`,
`src/d3d/**` (D3D11/GL). `src/input/**` is the game/input stream's (Agent 3 is porting `pc_input.c`; `xinput_device.c` has the
shell's `BLACK_VR` backend).

**Link order** (static, left needs right): `<game> xr_usb xr_nv2a xr_video xr_kernel xr_apu xr_dsound xr_platform`, input
after xr_usb/xr_video. xr_kernel calls back into xr_nv2a/xr_usb/xr_video (kelvin_start, xbox_OhciInit, xbox_Framebuffer*,
xbox_Video*), so link the archives as a group or whole-archive (port/quest does whole-archive).

Glslang/vulkan: `KVK_GLSLANG_DIR` defaults to `/root/deps/glslang`. kelvin_vk and kelvin_vk_shadergen additionally get
`-march=armv8.2-a -ffp-contract=off` (CPU-side viewport/depth-bias/eye math must not contract to FMA).

**Video "no video" path:** `xbox_VideoPlayFile` returns -1 and `xbox_VideoIsPlaying` 0 on non-Windows. The kernel only calls the
host player under `RECOMP_FMV_HOST` after a successful `.wmv` open; BLACK's movies are its own XMV decoder (game code). The exit
wait loop returns at once.

### Remaining undefined symbols at the final link

| Symbol | Referenced by | Provider |
|---|---|---|
| `xbox_InputGetState` | usb_gamepad.c | `src/input/xinput_device.c` (BLACK_VR backend, shell stream) in the game's input lib |
| `pc_input_apply_report`, `pc_input_set_filter_slot` | usb_gamepad.c | `src/input/pc_input.c` (Agent 3 is porting it) |
| `recomp_lookup`, `recomp_lookup_manual` | kernel | the game (generated code, recomp_manual.c) |
| `dsp56300_*` | apu | resolved when `build-dsp/.../libdsp56300_emu_ffi.a` exists (it does here) |
| `copy_file_range`, `memfd_create`, `getrandom`, ... | NDK libc++ | **weak** API-gated references, harmless at API 29 |

The game also references `pc_input_*`/`pc_controls_*` (input stream). Every `pc_menu/photo/split/crosshair/video/debug/movie_skip`,
`kgpu_*`, `kelvin_*` and `xbox_Framebuffer*` reference from BLACK's src is defined.

## 2. Files changed or added (all arm64 changes behind `__aarch64__ && !_WIN32`, or in new files)

| File | Change |
|---|---|
| `XR/src/platform/win32_compat.h` | aarch64 block: `InterlockedIncrement64/Decrement64/Exchange64/ExchangeAdd64`, `_InterlockedExchange64/ExchangeAdd64` (type-generic SEQ_CST `__atomic`), `MemoryBarrier` (DMB ISH), `YieldProcessor` (ISB, as sse2neon's `_mm_pause`), `SYSTEM_INFO`/`GetSystemInfo` (sysconf page size, online CPUs), `_snprintf_s`/`strncpy_s`/`_TRUNCATE` |
| `XR/src/nv2a/kelvin/kelvin.c` | win32_compat include; PUT read with acquire; GET (USER and CACHE1) published with release via the trap's alias |
| `XR/src/nv2a/kelvin/kelvin_mmio.c` | win32_compat include; **aarch64 fault handler** (`a64_fault_add_handler`, `mmio_emulate`), MMIOSTAT on `a64_uc_pc`; **USER-page write trap** (section 3); `kelvin_user_r32_acquire/w32_release` |
| `XR/src/nv2a/kelvin/kelvin_int.h` | aarch64: the two USER accessors; `kelvin_cvtt_i32/u32` (x86 float→int semantics) |
| `XR/src/nv2a/kelvin/kelvin_workers.c` | win32_compat include; `load()`/`load64()` SEQ_CST on aarch64 (section 3) |
| `XR/src/nv2a/kelvin/kelvin_render.c` | semaphore writes are store-release (fence+memcpy when misaligned) |
| `XR/src/nv2a/kelvin/kelvin_pixel.c` | LSB mux and alpha-test conversions with x86 semantics (section 4) |
| `XR/src/nv2a/kelvin/kelvin_texture.c` | texel address `(int)floorf` with x86 semantics (section 4) |
| `XR/src/video/pc_video.c`, `pc_movie_skip.c`, `video_player.c` | win32_compat include / stub prototypes on aarch64 |
| `XR/src/nv2a/nv2a_pgraph_none.c` | **new**: `pgraph_d3d11_*` with no translator (nv2a_core's methods are "not handled"); built only by nv2a.cmake |
| `XR/src/nv2a/kelvin/vk/kvk_backend.c` | coordinator items: `vkDeviceWaitIdle` in `eye_view()` under `queue_lock()`; aim-dot hiding (section 6) |
| `port/android/nv2a.cmake` | **new** |
| `tests/nv2a_arm64/` | **new**: CMakeLists, `parity_common.h`, `kpix_random.h`, `test_{vsh,pixel,raster,kwork}_parity.c`, `test_put_trap.c`, `test_stubs.c`, `android_stubs.c`, `hostshim/` (test-only x86 Linux shims) |

x86 Linux note: kelvin.c/kelvin_mmio.c/kelvin_workers.c/kelvin_render.c never compiled on x86 Linux (`<windows.h>`, `<intrin.h>`);
they still don't (deliberately unchanged). The tests build them there through `tests/nv2a_arm64/hostshim`.

## 3. Memory ordering: audit and changes

x86 is TSO and every Interlocked op is a full barrier; MSVC's `ReadAcquire` is a plain load on x86 and LDAR on ARM64.
win32_compat's Interlocked* are `__atomic_* SEQ_CST` (LSE `LDADDAL/SWPAL/CASAL` at armv8.2-a), which are fully ordered.

| Handoff | x86 relied on | Change |
|---|---|---|
| **Title → pusher: pushbuffer words then DMA_PUT** (generated D3D code, plain STRs to an anonymous RW page) | TSO store→store | **Nothing on the reading side can fix a writer without a barrier.** kelvin_mmio maps the USER page (aperture 0x800000, one host page) **read-only** over a memfd (ashmem fallback) with a writable alias. Title reads of GET cost nothing; each title store (the kickoff) faults into `kelvin_a64_fault`, which runs `DMB ISH` (orders every earlier store of that thread, the words) and then stores through the alias with release. One SIGSEGV per kickoff (~µs). `RECOMP_KELVIN_PUT_TRAP=0` disables. Misaligned/narrow stores emulated; LDP/STP/atomics through `a64_uc_emulate_mmio`. |
| Pusher reads PUT, then words | loads in order | `kelvin_user_r32_acquire` (LDAR): ARM does not order load→load across the loop's control dependency |
| Pusher publishes GET (USER and PFIFO CACHE1) after reading words / after draw jobs finished reading vertices | store after loads | `kelvin_user_w32_release` (STLR via the alias) |
| Semaphore release after `kelvin_render_sync` (raster threads' pixels) | TSO | store-release (cumulative), fence+memcpy if misaligned; the worker-barrier path already had `MemoryBarrier()` (now DMB) |
| kelvin_workers polls (`load()`, `load64()`) | acquire = plain on x86, full RMW before them | **SEQ_CST on aarch64**: `wait_for`/`notify` (sleepers++→read gen vs gen++→read sleepers) and `run_parallel`/`kwork_parallel` (busy++→read active vs active=0→read busy) are store-buffering handshakes. With LDAR (armv8.2-a) they hold, but with RCpc (`-mcpu=cortex-a78c`, armv8.3+) clang emits LDAPR for acquire, which may pass the preceding SWPAL/LDADDAL. SEQ_CST is always LDAR: same instruction today, robust later. |
| Frame-aspect / view marks | title registers marker before PUT | already acquire/lock; made correct on ARM by the PUT trap (title's count exchange happens-before its trapped PUT store) |
| Trapped PGRAPH/PMC/PCRTC, interrupts, notify ack | VEH + SRW lock | same lock (pthread rwlock) in the SIGSEGV handler; unchanged protocol |
| kwork slots/lanes/tasks, ktask_done, geometry jobs (kelvin_render) | | audited: all already publish with Interlocked (release) and poll with acquire; **no change needed** |

Not addressed (as arm64-runtime.md risk 2): title-side load→load reordering when the title reads GPU-written data (it reads a
semaphore then a surface with plain LDRs). Typical use is "semaphore passed → overwrite resources" (load→store, ordered by the
control dependency), so the engine's release side is sufficient for it.

## 4. Float parity

**No rcp/rsqrt approximations exist** in the Kelvin CPU paths: RCP/RSQ/RCC are `1.0f / x` and `1.0f / sqrtf(x)`, combiner divides
are `_mm_div_ps` (sse2neon: `vdivq_f32` on A64, exact), sqrt is correctly rounded on both. So nothing needs 12-bit-estimate
emulation; if `_mm_rcp_ps/_mm_rsqrt_ps` are ever introduced, note sse2neon gives NEON's estimate plus Newton steps, not Intel's
table: bit-exact parity would need a table port. Flags: sse2neon `SSE2NEON_PRECISE_MINMAX=1`, `-ffp-contract=off`, MXCSR↔FPCR
(rounding, FZ) propagated to workers by `_mm_getcsr/_mm_setcsr`, as on Windows.

**Which code runs:** Windows (`/arch:AVX2`) runs the `__AVX2__`/`__AVX__` blocks (kvsh_run8/emulator8, kpix_shade8's 8-wide
path, ktex_sample_2d8, plane4/interp4/derivs4 in double). arm64 runs the 4-wide/scalar code via sse2neon. The tests compare
those, not just SSE vs NEON.

**Bugs found and fixed (all float→int casts of out-of-range values, UB in C, where the hosts disagree):**
1. **`mux_select` (LSB mux)**: `(unsigned int)(r0.a * 255)` for negative r0.a. x86-64 (MSVC `/fpcvt:BC`, gcc, clang) converts
   through 64-bit CVTTSS2SI and keeps the low bits (−127.5 → 0xFFFFFF81, odd); A64 FCVTZU saturates to 0 (even) → the other mux
   input. Visible in real combiner setups (signed r0.a with LSB mux). 80 surface bytes in the test; fixed with `kelvin_cvtt_u32`.
2. **Texel addressing** `(int)floorf(u)` for huge/inf/NaN coordinates: x86 INT_MIN (also the AVX2 sampler's CVTTPS2DQ) → wraps
   to texel 0; A64 saturates to INT_MAX / 0. 2227 sampler values in the test; fixed with `kelvin_cvtt_i32`.
3. **Alpha test** `(int)floorf(a*255+0.5)` for NaN alpha: same rule.

**Accepted differences (documented, counted by the tests as "NaN bits only"):**
- **Default NaN sign**: x86 generates 0xFFC00000, A64 0x7FC00000 (inf−inf, 0·inf in samplers/blends). No stage reads NaN
  bits (clamps, MAXPS/MINPS and comparisons are sign-independent for NaN).
- **FMAXNM folding**: clang folds `x > 0 ? x : 0` and sse2neon's precise `_mm_max_ps(x, 0)` (vbsl/fcmgt) into **FMAXNM** when one
  operand is a known non-NaN constant (65 sites in kelvin_pixel.c, 12 in kelvin_render.c trace code). Identical to MAXPS for
  ±0 and quiet NaN; for a **signalling** NaN x86 returns 0, FMAXNM a quiet NaN. sNaNs cannot reach those sites in production
  (varyings are interpolated arithmetic, colours pass `clamp_color`, vertex fetches multiply by ±1, texels decode from bytes), so I
  did not pay `-ffp-exception-behavior=maytrap` (stops the fold, but +33% instructions in kelvin_pixel.c). The vertex emulators
  (where raw vertex-buffer sNaNs do arrive) contain no folds and match bit for bit with sNaN inputs. **The same fold can hit the
  generated game code's `XMM_MAX/MIN` against constants** -- see Requests.
- **libm** (`powf`, `log2f`, `exp2f` in EXP/LOG/LIT and exp fog): bionic and glibc agree on every test value (0 ulp); MSVC UCRT
  not testable here, allowance 2 ulp.

**Pre-existing, not changed (x86 behaves the same):**
- `nv2a_vsh_cpu_log` uint32 exponent: **on my path** -- LOG in a CPU-shaded vertex program (before its SPIR-V is ready, or draws the
  GPU path declines) gives LOG.x ≈ 4.29e9 for |x|<1 where the GPU path gives the negative exponent: a CPU-vs-GPU mismatch for the
  first frames of such a program (popping if BLACK uses LOG, e.g. for fog). Same on x86 and arm64.
- `kpix_shade` reads `Regs.ef` uninitialised when the final combiner's E or F input is EF_PROD/V1R0_SUM (invalid on hardware;
  D3D never emits it). Nondeterministic output on any host. One-line fix (`memset(r.ef, 0, ...)`) left to the owner.
- SIMD vertex emulators return the default NaN where the scalar emulator propagates an input payload (NaN bits only).

## 5. Tests (`tests/nv2a_arm64`)

```sh
CC=gcc cmake -S tests/nv2a_arm64 -B /root/build/nv2a-x86 -G Ninja && cmake --build /root/build/nv2a-x86 && ctest --test-dir /root/build/nv2a-x86
. /root/xtool/env.sh; cmake -S tests/nv2a_arm64 -B /root/build/nv2a-a64 -G Ninja \
   -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake -DNV2A_PARITY_REF_DIR=/root/build/nv2a-x86/ref
cmake --build /root/build/nv2a-a64 && ctest --test-dir /root/build/nv2a-a64
```
x86 AVX2 build (`-mavx2 -mfma -ffp-contract=off`) writes reference dumps; the SSE build and the arm64 build compare bit for bit.
TSan needs gcc (clang's compiler-rt TSan runtime is not installed).

| Test | What | Result |
|---|---|---|
| nv2a-vsh | 700 random vertex programs × 8 vertices (all MAC/ILU ops, swizzles, negation, masks, a0-relative, r12=oPos, edge inputs incl. sNaN/denormals/inf): emu4==scalar, emu8==scalar in process; scalar outputs and production KVertex (run8 on AVX2, run4 elsewhere) across hosts, 1,008,000 floats | x86 SSE and arm64: **0 differ** (arm64: 135 NaN-bits only); in-process 78,400/151,200 checks, 0 differ |
| nv2a-pixel | 600 random combiner/texture/depth/blend/fog configs × 8 fragments: `kpix_keep+kpix_shade8` == `kpix_fragment` in process; surfaces across hosts; plus 20,736 direct `ktex_sample_2d` samples (12 formats incl. DXT1/3/5, 2 filters, 9 address modes, edge coordinates), and `ktex_sample_2d8_lod` == scalar on AVX2 | **0 differ** (38,400 bytes; 82,944 floats, 970 NaN-bits only). Mutation: reverting fix 1 → 80 bytes differ, fix 2 → 2227 floats |
| nv2a-raster | kelvin_render.c compiled in (white box): 160 draws / 2,132 random triangles through `raster_triangle`→`flush_tris`→kwork lanes→`tri_scan`→`kpix_shade8`, 3.9 M pixel writes | arm64 with 4 workers and serial, x86 SSE serial, AVX2 serial: **all byte-identical** to AVX2 with 4 workers (196,608 bytes); TSan clean |
| nv2a-kwork | plain-memory handoffs through every pool primitive: lanes, barriers (begin callbacks), exclusive shares, kwork_wait/drain, kwork_parallel, ktask phases | 4,000 jobs, ~31k lane runs, 752 begins, 58 loops: **0 stale reads** (arm64, x86); **TSan: no races** |
| nv2a-put-trap (arm64) | real SIGSEGVs: title thread writes words + plain STR to PUT on the read-only USER page, pusher thread acquires PUT and checks words, publishes GET with release; trapped PMC/PGRAPH (32/8-bit, write-1-to-clear INTR, STP, 64-bit LDR), 16-bit USER store | 6,000 kickoffs, 609,288 words, **0 stale**; all register forms pass |

Limits: QEMU on an x86 host does not reorder memory, so the arm64 runs prove function (and the trap mechanism), not ARM
reordering. TSan checks the handoffs against the C11 model (what ARM may do), but cannot see generated guest code's stores.
No test of the CPU fixed-function vertex path or point/line rasterization; no on-device runs.

During test bring-up, two generator bugs (unsequenced RNG draws; an EF_PROD-reading E/F input) and a test-model bug (barriers
do not hold back later jobs on unlisted lanes) were fixed in the tests, not the runtime.

## 6. Coordinator items (kvk_backend.c)

- `eye_view()`: the cache-overflow `vkDeviceWaitIdle` now runs under `queue_lock()/queue_unlock()` (not called with the lock held
  anywhere). tests/kelvin_vk 5/5 and tests/vr_glue 2/2 pass.
- **Aim dot in hand-aim VR:** `draw_now` drops a HUD draw on the main target whose geometry is the centre reticle (`hud_is_reticle`,
  the same test as D3D11's `kg_hud_is_reticle`) when `vr_hides_aim_dot()`: layers > 1, `kvk_vr_frame_mode()` is STEREO (or unknown),
  and `vr_game_config()->aim_mode == VR_AIM_HAND`. `vr_game_config` has a weak NULL default beside the backend's other weak `vr_*`
  defaults (the shell's `vr_bridge.c` definition wins; it is exported `W`). The glue's lines/files are untouched. **Verified by
  build, both suites and the Android link only**: the existing stereo test draws its HUD as CPU vertices (`gvs` NULL), which the
  reticle test (like D3D11's) never matches, so there is no dedicated test of the hiding.

## 7. Requests

- **Agent 3 / input:** provide `xbox_InputGetState`, `pc_input_apply_report`, `pc_input_set_filter_slot` (and the game's
  `pc_input_*`/`pc_controls_*`) from the input lib. `pc_movie_skip.c` builds subtitle paths with backslashes
  (`subtitles\\fr\\...`), which fopen on Android will not find. Relink: object changes only (kelvin_pixel/texture/mmio/workers/render,
  kvk_backend), no interface or new undefined symbols; `vr_game_config` is a new **weak** definition.
- **Runtime/lifter owner:** the generated code's `XMM_MAX/XMM_MIN` (sse2neon precise) against a constant can be folded to
  FMAXNM/FMINNM by clang and differ from MAXPS/MINPS for **sNaN** operands; guest float data from disk can be sNaN. Options:
  `-ffp-exception-behavior=maytrap` on gen/ (measure), or an asm barrier in the header's arm64 min/max. Also worth adding sNaN
  to tests/arm64_runtime's edge set.
- **Renderer owner:** `kpix_shade`'s uninitialised `Regs.ef` (above); the `nv2a_vsh_cpu_log` fix.

## 8. Risks to test first on device

1. **USER-page write trap**: memfd `MAP_SHARED|MAP_FIXED` read-only over the aperture page under the Quest's SELinux/seccomp
   (ashmem fallback compiled, untested); the kickoff fault rate and cost (~1 SIGSEGV per D3D KickOff); ART's libsigchain passing it on.
   If it fails it logs "NOT trapped" and runs x86-style (unordered).
2. Trapped PGRAPH/PMC/PCRTC accesses through the SIGSEGV path (title ISR, vblank, notify acks): latency per access.
3. Kelvin with `RECOMP_NV2A_KELVIN=1` end to end with the Vulkan backend: never run here beyond unit level.
4. Raster worker count on the XR2 Gen 2 (6 cores → 2 workers + GPU thread; capped at 4 with the GPU backend), placement by
   thread_placement.c, spin budget (`RECOMP_KELVIN_SPIN_US`) vs power.
5. Hand-aim dot hiding: confirm BLACK's dot is GPU-programmed (`gvs`) in the 306–334 × 226–254 box, as on PC.

Housekeeping: the scratchpad's `orig/` folder of an earlier stream was overwritten by my first snapshot copy (theirs had finished);
my own baseline is `nv2a64/pre`.
