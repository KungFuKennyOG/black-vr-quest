# arm64-runtime: XboxRecomp runtime on Android arm64 (Quest 3)

Stream owner: arm64-runtime agent. Nothing committed. Paths are relative to /root/black;
`XR` = `third_party/xboxrecomp`.

## Results at a glance

| What | Status |
|---|---|
| All 16 generated TUs (`src/recomp/gen/recomp_00..13.c`, dispatch, stubs; ~1.45M lines) for arm64-v8a, NDK r27c, `-O2 -march=armv8.2-a` | **compile, 0 errors**, using the edited template header |
| Same TUs on x86-64 with the edited header | **byte-identical object code** (chunks 5 and 12 diffed; see section 1.4) |
| Runtime static libs `xr_platform`, `xr_kernel`, `xr_apu`, `xr_dsound` via `port/android/CMakeLists.txt` | **build, 0 warnings** (shared NDK toolchain and the static test toolchain) |
| `tests/arm64_runtime` under ctest + `/root/xtool/arm64run` (QEMU, cortex-a76) | **5/5 pass**: SIMD (sse2neon), SIMD (portable), path case, memory layout, AAudio ring |
| Same SIMD test natively: SSE and `RECOMP_FORCE_PORTABLE`, gcc and clang | **pass, identical digest** `6230F862060A37AC` on all 4 paths |
| `memory_layout_posix` natively on x86-64 Linux (runtime libs built by hand) | 17/17 PASS (it failed to compile at HEAD, see 2.3) |
| Lifter unit tests (`.venv` python, pytest) | 249 pass, 9 fail; the **same 9 fail on pristine HEAD** (FPU/`rep movsb` tests, not mine) |
| AAudio | compiles and links (`NEEDED libaaudio.so`); logic tested against a fake AAudio; **not run on a device** |

## 1. Generated-code portability (templates/runtime/recomp_types.h)

### 1.1 Inventory: what the emitted C depends on

From lifter.py/translator.py, the post-processing in scripts/generate.py, and grepping the real
generated code.

| Dependency | Where | Status on arm64 |
|---|---|---|
| SSE intrinsics and the `__m128` type (`RecompXmm.m`, `XMM_*` helpers) | header | **fixed**: sse2neon under `__aarch64__`, lane-wise C under `RECOMP_FORCE_PORTABLE` or on a host with neither |
| Raw `_mm_cmplt_ss/_cmpnle_ss/_cmpeq_ss/_cmpneq_ss/_cmpnlt_ps/_div_ps/_sqrt_ps/_set1_ps` on `.m` | spliced in by `generate.py repair_sse` (53 sites) | works through sse2neon; the portable path defines all of them |
| MINPS/MAXPS operand-order semantics (second operand on NaN or ±0) | `XMM_MIN/MAX` | sse2neon's default `vminq` is **wrong** here. The header forces `SSE2NEON_PRECISE_MINMAX=1` and `#error`s if sse2neon was already included without it. A mutation test catches it (568 failures) |
| `(int32_t)float` for cvttss2si/cvtss2si (277 sites in BLACK) | lifter | **fixed in the lifter**. The C cast is UB for NaN/out-of-range: x86 gives 0x80000000, arm64 `fcvtzs` saturates. It now emits `RECOMP_CVTT_F2I32(x)` / `RECOMP_CVTR_F2I32(x)`; on x86 these are the same `cvttss2si` instruction (verified) |
| cvtss2si truncation (pre-existing lifter bug: it should round by MXCSR.RC) | lifter | kept as truncation by default so x86 behaviour does not change. `-DRECOMP_CVT_ROUND_MXCSR` switches to hardware behaviour. BLACK has 4 such sites |
| MMX (`RecompMmx`, `MMX_*`) | header | already scalar C. `MMX_CVT_F2I` now routes through the same conversion helper |
| x87: `double` stack, `g_fp_control_word`, `recomp_fp_precision`, `recomp_fist` (explicit RC), `rint` for frndint, sin/cos/atan2/exp2/log2/fmod | header + lifter | **portable as is**. RC is emulated from the guest control word, never through the host FPU. Only `frndint` (`rint`, 9 sites) uses host rounding, which is round-to-nearest on both hosts unless someone calls fesetround |
| No 80-bit precision anywhere | | the lifter models x87 as binary64 on both hosts, so this is no regression |
| FMA contraction | compiler | **fixed**. clang for aarch64 contracts `a*b+c` into `fmadd` by default, which changes rounding versus x86. The header has `#pragma STDC FP_CONTRACT OFF` and the build passes `-ffp-contract=off`. 0 fmadd/fmla in the generated objects; a test catches its removal |
| `lock xadd` / `lock cmpxchg` → `RECOMP_ATOMIC_*` (10 sites) | header | **fixed**. x86 allows misaligned locked ops; arm64 SIGBUSes (verified under QEMU). The aligned fast path is one LSE `ldaddal`/`casal`; a misaligned dword inside an 8-byte word uses a 64-bit CAS loop; a dword straddling 8 bytes goes to `recomp_atomic_rmw32_split` (lock-serialised, defined in xbox_memory_layout.c) |
| `xchg mem, reg` (8 sites) | lifter | emitted as a plain non-atomic swap on **both** hosts (pre-existing). Same semantics on both, so not a regression; noted as a latent lifter issue |
| Unaligned plain loads/stores (`MEM32` at any address) | header | fine: arm64 normal memory allows unaligned LDR/STR. **Do not use `-fms-volatile`**: it turns MEM* into LDAR/STLR, which SIGBUS when misaligned (verified) |
| `volatile` MEM* accesses | | compile to plain LDR/STR (no barriers), matching x86 TSO only per-thread. Cross-thread guest code relying on x86 store ordering without `lock` could see reordering on arm64. Not addressed (it would need `-fms-volatile` plus an alignment strategy, or DMB on specific paths) |
| `RECOMP_TLS` = `__thread` | header | fine. Bionic ELF TLS needs API 29+, which is our minimum. In a `.so` every register access is a TLSDESC call; see 5 (perf) |
| `__declspec`, `__try/__except`, inline asm, `__readfsdword`, `_rotl`, `_byteswap_*`, `_BitScan*`, calling-convention attributes | | **not emitted** by the lifter. fs: is `g_fs_base` + MEM, rotates and bswap are C helpers, SEH is via `g_seh_ebp`. The only `__debugbreak` maps to `__builtin_trap` off MSVC |
| `char` signedness | | the lifter uses explicit `int8_t`/`uint8_t`, and arm64's unsigned `char` is not relied on |
| 32-bit pointer assumptions | | none in the emitted code (`XBOX_PTR` masks to 32 bits, then adds a 64-bit offset). The runtime truncates host pointers to 32 bits in 3 places in kernel_bridge.c (debug prints, the IoBuildSynchronousFsdRequest return, the ObReferenceObjectByName object): latent on every 64-bit host, not arm64-specific |
| `rdtsc` → `xbox_ReadTimeStampCounter()` | | portable (QueryPerformanceCounter → clock_gettime) |

### 1.2 Header changes (x86 unchanged)

- Host selection block at the top: `RECOMP_HOST_X86` / `RECOMP_HOST_ARM64` / `RECOMP_SIMD_PORTABLE`.
- `RecompXmm.m` is `RECOMP_M128`, which is `__m128` on SSE or sse2neon, else a 16-byte aligned
  `RecompM128` struct. The size and alignment are identical, so the TLS register file has one layout.
- An always-compiled `recomp_px_*` lane-wise reference, plus the portable `XMM_*` set and portable `_mm_*` stand-ins.
- `recomp_cvt_f2i32` / `recomp_cvt_f2i32_ss` and a `_Generic` `RECOMP_CVT(T|R)_F2I32`.
- arm64 `RECOMP_ATOMIC_*` as described above.
- `src/kernel/xbox_memory_layout.h` carries a second copy of the `RecompXmm` union. It got the same host
  selection; it is guarded by `RECOMP_XMM_DEFINED`.

**Regenerate or not?** `src/recomp/gen/recomp_types.h` is a *copy*. The new header works with the
already-generated code as is, because it keeps every old name. Only the cvt fix needs a regenerate
(`/root/black/.venv/bin/python scripts/generate.py`). Until then the 277 bare casts remain, and they
are only wrong for NaN/out-of-range inputs. The arm64 build in `port/android` puts
`templates/runtime` *before* `gen/`, so header fixes apply without regenerating. I did **not**
regenerate (the coordinator owns gen/). The chunk build used a scratch copy where the 277 casts were
rewritten the way the new lifter emits them.

### 1.3 Lifter changes (tools/recomp/lifter.py)

Only one: cvt(t)ss2si/cvt(t)sd2si emit the helper instead of `(int32_t)`. No other lift changed.

### 1.4 x86 equivalence evidence

- `clang -O2` x86-64 objects for chunks 0012 and 0005: with the old header versus the new header and
  the old generated code, **0 differing lines** of disassembly.
- With the new lifter output (casts → helpers): chunk 0012 has 0 differences. Chunk 0005 has the same
  instruction multiset for every `cvt*` (44 cvttss2si each) and differs only in register allocation and
  layout (`cmp a,b; jle` ↔ `cmp b,a; jge`).
- MSVC proper was not available here (no clang-cl or xwin SDK). The x86 paths use the same intrinsics
  as before, so MSVC should see the same thing, but that is unverified.

### 1.5 Regression test: tests/arm64_runtime/test_simd.c

It covers every XMM helper and the spliced raw intrinsics over a 24-value edge set (±0, NaN payloads,
inf, denormals, ties, ±2^31), checked against the lane-wise reference. It also has golden x86 answers
for the conversions under 3 rounding modes and MINPS/MAXPS operand order, unaligned XMM/MMX memory
ops, `lock xadd`/`cmpxchg` at all 16 byte offsets (with neighbour bytes preserved), 4-thread contended
misaligned xadd at offsets 0-7, and FMA contraction. It prints an FNV digest that must match on every
path. Mutation-checked: imprecise minmax, FP_CONTRACT ON and an unguarded misaligned atomic are each
caught (the last as SIGBUS).

## 2. Memory layout on Android arm64

### 2.1 Findings

- Page size: Quest is 4 KB. The existing host-page logic already handles 16 KB.
- `shm_open` does not exist in bionic. `memfd_create` is declared only from API 30 (we target 29) →
  **raw `syscall(__NR_memfd_create)`**, with `ASharedMemory_create` (API 26, ashmem) as a fallback.
  memfd is allowed for untrusted_app under SELinux. I could not verify on-device whether Quest's
  policy allows `MAP_SHARED|PROT_WRITE` mappings of memfd (it does on stock Android 10+).
- `mmap_min_addr`: irrelevant, because the base is OS-chosen (0x7f...) and no low fixed addresses are used.
- **Real bug found and fixed:** only base + 28 mirrors (1.8 GB) were reserved. The apertures sit at
  fixed offsets from the base (0x80000000 contiguous, 0x84000000 low-RAM physical, 0xF0000000 tiled,
  0xFD/FE8/FF000000 devices) and landed on whatever the process had mapped there. Under QEMU (and in
  any real Android process with ART and the linker) **every aperture failed with EEXIST**. The fix:
  on 64-bit POSIX (not Windows, not Darwin) reserve the whole 4 GB guest window as
  `PROT_NONE|MAP_NORESERVE`, and `guest_span_release()` each slice just before its fixed mapping.
  Now all apertures, tiled==contiguous aliasing, low-RAM view and re-init pass on arm64 and x86-64.
- `VirtualAlloc(MEM_RESERVE)` on POSIX mapped read-write and committed. It is now
  `PROT_NONE|MAP_NORESERVE`, so it costs address space, not commit (this matters for Android's LMK).
- AC97 bus-master write trap: the Windows VEH used `EFlags` single-step and did not compile on any
  POSIX host. It is now Windows-only. On arm64 it is reimplemented with the arm64-fault stream's
  decoder (emulate the faulting store on the opened page, clear RR bits, re-close). On x86-64 Linux
  it reports "not available" (no SIGSEGV path existed there before either).

### 2.2 Verified

`memory_layout_posix` (the existing XR test, unchanged) runs as a static arm64 binary through
arm64run: init, host-page alignment, **28/28 memfd mirrors alias the base**, a write through mirror 1
reaches the base, shutdown, re-init, all 5 apertures re-mapped, tiled alias verified,
RECOMP_TRAP_NULL faults and the TIB is intact. All PASS. It also passes natively on x86-64.

### 2.3 Note

At HEAD, `xbox_memory_layout.c` did not compile on *any* POSIX host (the AC97 VEH used Windows
`EXCEPTION_POINTERS`), so `tests/memory_layout_posix` could not have been passing on Linux/macOS
either. Fixed as part of 2.1.

## 3. Win32 compat on bionic (src/platform/win32_compat.*, src/kernel)

Fixed under `__ANDROID__` (or generally where it was a plain bug):
- `pthread_cancel` does not exist in bionic. `TerminateThread` marks the thread exited and logs (no caller in the runtime).
- `explicit_bzero` does not exist in bionic. `SecureZeroMemory` uses a volatile loop.
- `memfd_create` → syscall + ashmem fallback (see 2).
- `view_register`/`view_take` were used before being declared on non-Apple hosts (a hard error with clang).
- Added the POSIX `SleepConditionVariableSRW` (a generation-counted wait, since there is no pthread
  condwait on an rwlock), `CreateWaitableTimerExW` (the high-resolution flag is implicit),
  `InterlockedAdd`, `timeBeginPeriod/timeEndPeriod`, `TIMER_ALL_ACCESS` and
  `CONDITION_VARIABLE_LOCKMODE_SHARED`. kernel_bridge.c used all of these but they were Windows-only.
- `kernel_bridge.c`: `<timeapi.h>` is now `_WIN32` only.
- `thread_placement.c` was Windows-only (`<windows.h>`). It has a POSIX implementation: cpufreq
  `cpuinfo_max_freq` ranks the cores. On XR2 Gen 2, MAIN/PUSHER/GPU go to the 3 fastest cores and
  polling roles to the slow cluster, via `sched_setaffinity` (refused cpusets are tolerated).
  `RECOMP_PIN_<ROLE>` and `RECOMP_THREAD_PLACEMENT=0` are honoured.
- TLS: `RECOMP_TLS`/`XBOX_THREAD_LOCAL` are already `__thread` off MSVC. That is fine on API 29+.
- Case-insensitive paths: `kernel_path.c` resolves each Xbox path under the game/save dir with a
  strcasecmp walk when the exact path is missing (cached; `RECOMP_PATH_CASE=0` disables it). It keeps
  the spelling of missing trailing components, which creates need. BLACK's disc has mixed-case names
  (`FEAudio.awd`, `Andy.aku`, ...: 213 of 439 files). Exported as `xbox_path_resolve_case()`.
- Android save dir default: `<game_dir>/save` (there is no HOME/XDG). **The shell should pass explicit
  dirs** to `xbox_path_init(game_dir, save_dir)`: e.g. game_dir = `getExternalFilesDir()` path holding
  default.xbe, save_dir = `getFilesDir()/save`.

## 4. Audio: AAudio backend

- Path: APU monitor (`apu_core.c` `monitor_submit_packet`) → `xa2_*` contract → backend.
  `apu_host_audio.h` documents that contract (owner thread, ACCEPTED/BACKPRESSURE/ERROR, 2-packet
  prime, reprime after a dry run, 1 s recovery). The `xa2_` names are kept so apu_core.c and the
  Windows build are untouched.
- `src/apu/apu_aaudio.c` (`__ANDROID__`): a 48 kHz stereo i16 LOW_LATENCY shared stream with
  USAGE_GAME, a data callback pulling from a lock-free SPSC ring of 3 × 1024-frame packets (the same
  as the XAudio2 voice queue), and buffer = 2 bursts. The error callback (for example DISCONNECTED on
  a headset switch) only sets a flag; the owner thread reopens. `apu_aaudio_xrun_count()` is extra,
  for diagnostics.
- `apu_xaudio2.c` stubs are now `!_WIN32 && !__ANDROID__`.
- Verified: compiles, links into a .so with `NEEDED libaaudio.so`, and `test_aaudio_ring.c` (a fake
  AAudio driving the real callback) passes 30 checks on arm64: gating, backpressure, chronological PCM
  across odd bursts, underrun reprime, the other-thread refusal, disconnect recovery and the retry delay.
- **Unverified**: real device latency and behaviour. The Quest's native rate is 48 kHz, so no
  resampling is expected.

## 5. Build: port/android/CMakeLists.txt

```
. /root/xtool/env.sh
cmake -G Ninja -S port/android -B build/android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_TOOLCHAIN_FILE \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 [-DBLACK_ANDROID_GAME=ON] [-DDSP56300_LIBRARY=...]
cmake --build build/android
# tests under QEMU:
cmake -G Ninja -S port/android -B build/a64t -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake
cmake --build build/a64t && ctest --test-dir build/a64t --output-on-failure
```

Static libraries (in `<build>/lib/`):
- `libxr_platform.a`: win32_compat + arm64_fault_decode (links log, android)
- `libxr_kernel.a`: the kernel HLE and memory layout (links dl)
- `libxr_apu.a`: APU and AAudio (links aaudio, m; DSP56300 via `-DDSP56300_LIBRARY`)
- `libxr_dsound.a`: DirectSound
- `xr_runtime` (INTERFACE) links all four. `xr_game` (optional) is the generated code.

Flags: `-march=armv8.2-a` (inline LSE atomics, which the fault stream needs), `-ffp-contract=off`,
`-fno-strict-aliasing`, `-fwrapv`, sse2neon on the include path. The subdirectory CMakeLists
(SDL2, OpenSSL, x86_64 Rust triple) are deliberately **not** used.

`port/android/x86shim/` holds `{mm,xmm,emm,pmm,tmm,smm,imm}intrin.h` and `intrin.h` → sse2neon, for
code that includes x86 headers directly (BLACK's src, the renderer). Add it with
`-isystem port/android/x86shim`. The runtime libs above do not need it.

### Link-time unresolved symbols (owned elsewhere)

- `kelvin_active/start/irq_asserted/irq_event/vblank/owns_register`: renderer (src/nv2a/kelvin).
- `xbox_OhciInit`: USB (src/usb). It is not in my build because usb_gamepad.c pulls in
  `input/pc_input.h` (SDL). The XR shell or input owner should build it.
- `xbox_FramebufferWindowSet/Start`, `xbox_VideoPlayFile/IsPlaying`: src/video (renderer or shell).
- `xbox_SetAckTick` (used by apu_core.c): defined in xbox_memory_layout.c, so it resolves when
  xr_kernel is linked.
- `recomp_lookup`, `recomp_lookup_manual`, `black_*` hooks: game code and src/recomp_manual.c.
- `dsp56300_*`: the Rust DSP core. **Blocker:** building it for `aarch64-linux-android` needs that
  rustup target and crates.io (cranelift 0.134), and neither is reachable here (static.rust-lang.org
  and crates.io are blocked). Its cranelift JIT does support aarch64 (`jit.rs` has an aarch64 arm).
  Someone with network access must run
  `cargo build --release -p dsp56300-emu-ffi --target aarch64-linux-android` with the NDK linker, then
  pass `-DDSP56300_LIBRARY=`. JIT on Android also needs executable anonymous memory; that is allowed
  for untrusted_app (ART does it), but verify on the device.

## 6. BLACK's own src: Windows-only inventory (not edited; not my files)

| File | Windows-only constructs |
|---|---|
| `main.c` (641 lines) | `<windows.h>`, `<dbghelp.h>`; `WinMain` and `GetCommandLineA`; VEH crash handler (`AddVectoredExceptionHandler`, `EXCEPTION_POINTERS`, `ContextRecord->Rip`, `SymInitialize/SymFromAddr/SymGetLineFromAddr64`, `GetModuleHandleA`); the FPE-trap VEH with `__declspec(thread)` and TF single-step; `MessageBoxA`; the APU MMIO fault-handler registration. On Android it needs `android_main`/JNI entry, `a64_fault_install()` + `a64_fault_add_handler(apu_fault)` after `xbox_MemoryLayoutInit` (snippet in arm64-fault.md), `xbox_path_init(extfiles, files/save)`, and logcat/tombstone in place of dbghelp |
| `recomp_manual.c` (5210) | `<windows.h>`, `<xmmintrin.h>`; 120 `_mm_getcsr/_mm_setcsr` (MXCSR save/restore around native float code; sse2neon maps RC and FZ to FPCR, so OK with the shim); `__declspec(thread)` ×2 (l.902-903) → `RECOMP_TLS`; `extern __declspec(thread) g_ebp/g_seh_ebp` in the generate.py `IDCT_PREFIX`, which conflicts with the non-TLS decl (l.1094); `__forceinline` before `recomp_types.h` is seen; pc_video/pc_menu/pc_photo/pc_split/pc_crosshair/fb_present (src/video, Windows D3D11 UI) |
| `black_native_menu.inc` | `CreateDirectoryA`, `MoveFileExA(MOVEFILE_REPLACE_EXISTING\|WRITE_THROUGH)`, `DeleteFileA`; `_mm_getcsr/setcsr` |
| `black_hud_input.inc` | `__try/__except` ×4 around guest memory probes, `PAGE_GUARD`/`VirtualQuery`; includes `recomp/gen/recomp_types.h` |
| `black_xmv_yuv.inc`, `black_xmv_idct.inc` (generated by generate.py) | SSE2 integer intrinsics (`_mm_madd_epi16`, `packus`, `unpack*`, ...). These work through sse2neon via the x86shim; `__forceinline` and `__declspec(thread)` need the macros |
| `black_checkpoint.inc`, `black_lod.inc`, `black_gameplay_tuning.inc`, `black_menu_input.inc`, `black_vtx_decode.inc` | `_mm_getcsr/_mm_setcsr` and SSE scalar ops only (fine with the shim) |
| `black_xmv_prof.inc`, `black_xmv_field.inc`, `black_secondary_objectives.inc` | `QueryPerformanceCounter` (compat layer has it) |

## 7. Open issues and risks

1. **DSP56300 for Android is not built** (network). Without it the APU's GP/EP DSP cannot run, and audio stays silent beyond the software mixer.
2. **x86 TSO versus arm64 ordering** for plain guest stores shared between guest threads. Not addressed; watch for lock-free guest code without `lock` prefixes (BLACK's audio completion queue is a candidate).
3. **TLS cost in a .so**: every `g_eax`... access is a TLSDESC call (about 4 instructions plus a branch, cached per function by the compiler). If profiling shows it, build the game and runtime into libmain.so with `-ftls-model=initial-exec` (bionic API 29 supports static TLS in dlopen'd libraries only within a small surplus, so this needs measuring), or move the register file into a per-thread struct passed by pointer (a lifter change).
4. Lifter: `xchg mem,reg` is non-atomic (both hosts). cvtss2si should round (kept truncating behind `RECOMP_CVT_ROUND_MXCSR`).
5. 9 pre-existing lifter unit-test failures at HEAD (FPU operand order, `rep movsb` overlap). Not caused by this stream.
6. memfd under the Quest SELinux policy, AAudio, `sched_setaffinity` cpusets and the DSP JIT's W^X are all unverified on hardware.
7. The renderer's and XR shell's builds must add `-isystem port/android/x86shim` and `-I/root/deps/sse2neon` for any TU including x86 intrinsic headers, and must define `SSE2NEON_PRECISE_MINMAX=1` (or include `recomp_types.h` first).
