# device-preflight: making the first headset launch work, or fail with a line that names the cause

Stream owner: device-preflight agent. Nothing committed. Paths are relative to /root/black; `XR` = `third_party/xboxrecomp`.
The one feedback channel is the logcat capture `port/quest/mac_install.sh` takes (`adb logcat -b main,system,crash -v
threadtime`). Everything below is designed for that file.

## How to read a device log

```sh
grep -E 'PREFLIGHT|CRASH|STALL|HEARTBEAT|GPU ' blackvr_log_*.txt
```

| Lines | Tag | When |
|---|---|---|
| `PREFLIGHT <check>: OK\|WARN\|FAIL\|SKIP <details>` | BlackVR | once per check at startup, before the game thread exists; `[FATAL]` appended to a FAIL that stops the game from starting |
| `PREFLIGHT <check> \| ...` | BlackVR | a detail line under a check: what it is about to do when that could kill the process, or the list of paths tried |
| `PREFLIGHT summary: N OK, N WARN, N FAIL, N SKIP (failed: ...) -> <verdict>` | BlackVR | after the XR session exists; the verdict says whether the game was started |
| `CRASH <signal> on thread "<name>" (tid N) at T s: fault address ... = guest VA ... (<window>); pc lib!sym+off, lr lib!sym+off; guest esp/eax; last kernel call ...; frames shown` | BlackVR | the first line of any native crash, before the long `[CRASH]` report (tag BlackGame) and debuggerd's tombstone |
| `HEARTBEAT t=N s: frames ... (fps), XR loop ../s; guest icalls ../s, kernel calls ../s; Kelvin flips, words/s, draws, PUT/GET; RSS` | BlackVR | every 10 s while the game runs |
| `STALL no new frame for N s ...` + `STALL game thread: pc ..., frames ...; last kernel call ...; guest esp ...` | BlackVR | every 5 s once no frame has been shown for 5 s; `[STALL] guest threads` (tag BlackGame) every 30 s of a stall |
| `GPU vkQueueSubmit (frame) failed: VkResult -4 (VK_ERROR_DEVICE_LOST ...)` | BlackVR | the Kelvin backend's submit or fence wait failed (first 4) |

The headset also shows it: a **solid red panel** in front of the test scene = a fatal PREFLIGHT check, the game was not
started; an **orange panel** = the game thread returned during startup (`PREFLIGHT game-exit: FAIL`). The existing red
*sky* still means "no game files found". `mac_install.sh` prints the summary line and every FAIL/WARN line, plus the
first CRASH/STALL line, when it stops recording.

Switches (blackvr.ini `env NAME=VALUE` lines): `BLACKVR_PREFLIGHT=0` skips the checks after the game folder search;
`BLACKVR_PREFLIGHT_IGNORE=1` starts the game despite a fatal check; `BLACKVR_HEARTBEAT=0` turns off HEARTBEAT/STALL;
`RECOMP_SIGSTACK_KB` sizes the runtime threads' alternate signal stacks (default 256, 0 = leave bionic's).

## The PREFLIGHT lines, in log order

| Line | What it checks | On FAIL / WARN |
|---|---|---|
| `begin` | Banner only: version, device model, Android release/SDK/build, kernel, page size, CPU max MHz per core, SELinux context, seccomp mode, tagged-address ABI and a sample heap pointer | Nothing to do; it identifies the device. Page size other than 4096: the OHCI 4 KB traps over-trap (arm64-fault.md). |
| `gamedir` (+ `gamedir \| tried ...`) | Each candidate in order, with errno: `<external files dir>/game`, `/sdcard/Android/data/com.kenny.blackvr/files/game` (deduplicated by realpath), `/sdcard/BlackVR/game`, `/sdcard/Download/BlackVR/game`, `/sdcard/BLACK/game`. For the one found: `default.xbe` size, `XBEH` magic, title ID, the first 16 signature bytes, entry count, whether `levels/` exists, and **whether the XBE is the one the APK was generated from** (size and signature, baked in at configure time from `game/default.xbe`) | Not found: copy the disc to the first folder (`mac_install.sh` does). `EACCES` on a `/sdcard/...` folder: grant All files access (`adb shell appops set com.kenny.blackvr MANAGE_EXTERNAL_STORAGE allow`; the app also opens the settings page). `SIZE/SIGNATURE MISMATCH`: the recompiled code is for another XBE; it will crash at random. Rebuild the APK from this disc. WARN `NO levels/`: incomplete extraction. Not fatal: the test pattern runs. |
| `storage` | Free space on the game folder, `save/` and the internal data dir; that `save/` accepts create + write + fsync + unlink | `NOT writable`: saves and the Z: partition image fail. Usually a `/sdcard/...` folder without All files access. `< 300 MB`: free space. |
| `memory` | `/proc/meminfo` total/available/swap, this process's RSS/VmSize, RLIMIT_AS, ActivityManager memory class, `availMem`, LMK threshold and `lowMemory` (JNI) | WARN when < 700 MB available or `lowMemory`: the low-memory killer would end the app with no crash line (look for `lowmemorykiller` / `has died` in the system buffer). Close other apps. |
| `addrspace` [FATAL] | `xbox_MemoryLayoutInit`'s mappings, done and undone: a 4 GB `PROT_NONE\|MAP_NORESERVE` reservation (OS-chosen base, so `mmap_min_addr` is irrelevant), a 64 MB `memfd_create` (falls back to `ASharedMemory`, as the runtime does) mapped `MAP_SHARED\|MAP_FIXED` at the base, all 28 mirrors and the contiguous window at +0x80000000, an aliasing write/read, and a `PROT_NONE` aperture page | The text names the mapping that failed and its errno. Reservation failure also tries 2 GB to say whether a smaller window would fit (none exists: the apertures sit at fixed offsets up to +0xFF000000). memfd `EACCES`/`EPERM` with the ashmem fallback also failing: SELinux; send the log. |
| `sigsegv` [FATAL] | The MMIO trap round trip: the runtime's own dispatcher (`a64_fault_install` / `a64_fault_add_handler` / `a64_uc_emulate_mmio`) on a `PROT_NONE` test page: a trapped load (device value returned), a store (device sees it), 200 loads for the cost per trap, and **a nested trap taken inside the handler** (device models do this; it needs `SA_NODEFER` honoured through the signal chain). Reports whether ART's `libsigchain` is in the process, the previous SIGSEGV action (debuggerd: where unclaimed faults go, so tombstones still happen), and whether the handler ran on the alternate signal stack and how much of it it used | A log that **ends** after `sigsegv \| arming` or `\| nested fault next` means the fault never reached the dispatcher, or the nested one was blocked: the tombstone will show a SIGSEGV at the test page. Wrong values: the decoder/ucontext glue disagrees with this kernel (send the log). The trap cost is ~3-6 us expected (perf.md). |
| `jit` | What the DSP56300's Cranelift JIT does: anonymous `mmap` RW, write code, `__builtin___clear_cache`, `mprotect` RX, run it (returns 42); whether RWX is refused (informational: cranelift never asks for RWX); and **cranelift-jit's aarch64 `membarrier(PRIVATE_EXPEDITED_SYNC_CORE)` sequence** (register on EPERM, GLOBAL on EINVAL), which it calls after every compile and `expect()`s | `mprotect RW->RX refused` (execmem denied) or a membarrier error: the JIT panics when the APU starts ("unable to make memory readable+executable" / "Failed pipeline flush") and the process aborts. Not fatal here because that abort is itself named. Workaround to try: `env RECOMP_AC97_READY=0` (no APU, no DSP; the title may then wait on audio). |
| `syscalls` | Each syscall the runtime uses that an app seccomp filter could refuse, under a temporary SIGSYS handler (a refusal is reported instead of killing the process): `memfd_create`, `sched_getaffinity`, `sched_setaffinity` (same mask) | WARN memfd refused: the runtime uses ashmem instead (see `addrspace`). The runtime makes no `personality`, `mremap`, `userfaultfd`, `ptrace` or `perf_event_open` calls, and `SetThreadPriority` is a no-op on POSIX, so none of those is probed. |
| `altstack` | The main thread's alternate signal stack after it is enlarged to 256 KB | WARN: the mmap failed; handlers keep bionic's 32 KB. |
| `audio` | AAudio as `apu_aaudio.c` opens it: 48 kHz stereo i16, low latency, shared, usage GAME; then start, wait up to 400 ms for 2 data callbacks, stop, close | FAIL: no sound, the game still runs. A stream not at 48000/2/i16 is refused by the runtime's backend (no resampler). WARN: opened but no callback. |
| `vulkan` | A standalone `VkInstance` before OpenXR (separates a GPU problem from a runtime one): GPU name, vendor/device, driver version (Qualcomm 512.x.y decoded), API version, loader version. **Required** (the renderer refuses otherwise: test pattern only): Vulkan 1.1, `multiview` (≥ 2 views), a graphics queue, `robustBufferAccess`, B8G8R8A8/R8G8B8A8 colour attachment + blend + sampled + linear, D24S8 or D32S8, the backend's descriptor/push-constant/UBO/SSBO limits. **Optional** (the backend degrades): `depthClamp`, `samplerAnisotropy`, BC textures (absent on Adreno, decoded on the CPU), `VK_EXT_fragment_density_map` (foveation), `VK_KHR_external_memory` | `MISSING ...`: the game would run without a picture; send the log (not expected on a Quest 3). |
| `openxr` [FATAL on failure] | After `xrs_instance_create`: runtime name and version, system name/vendor, recommended and max eye size, and every extension the shell uses: present vs missing. On failure: the exact call that failed, its `XrResult` by name, file:line, and a hint (`XR_ERROR_RUNTIME_UNAVAILABLE` = the loader found no runtime: Quest OS too old for the Khronos loader's runtime broker, or the manifest `<queries>`; `XR_ERROR_FORM_FACTOR_UNAVAILABLE` = headset not on/ready, Link active; ...) | The app exits cleanly (nothing can be shown without OpenXR). WARN = an optional extension is missing (listed with what it is for). |
| `vulkan-device` [FATAL on failure] | The device made through `XR_KHR_vulkan_enable2`: name, API, queue family, the features actually enabled | Failure names the call and its `VkResult`/`XrResult`. No multiview: FAIL, test pattern only. |
| `openxr-session` [FATAL on failure] | World space (STAGE or LOCAL), swapchain format and format list, eye size and layout, foveation, whether the renderer attached | Failure names the call. `renderer REFUSED`: see the vr-glue line above it. |
| `display` | (after the session goes READY) refresh rates offered and the one requested | WARN: the refresh-rate request failed. |
| `summary` | The counts and the verdict: `starting the game`, `NOT starting the game (... red panel ...)`, `no game files: test pattern`, `smoke build`, `XR startup failed: the app exits` | |
| `sigsegv-game-thread` | The same trap round trip on the game thread itself, after its 256 KB alternate signal stack is installed, before `black_game_main` | As `sigsegv`. |
| `game-exit` | The game thread returned non-zero during startup (orange panel) | The `[BOOT]` / `Recomp:` lines (tag BlackGame) before it say why: `default.xbe` unreadable, memory layout, ... |

## Crash breadcrumbs (src/main.c, `__ANDROID__`)

- **The first line** of every native crash is `CRASH ...` (tag BlackVR, `ANDROID_LOG_FATAL`), written straight to
  logcat, so it survives a process that dies before the stdio pipe that carries the long `[CRASH]` report is drained.
  It has: the signal, thread name and tid, uptime; for SIGSEGV/SIGBUS/SIGILL/SIGFPE the fault address and which guest
  window it is in (guest RAM, mirror, contiguous, tiled, NV2A, APU, MCPX/AC97/USB, thunks, flash, or host memory); the
  pc and lr as `library!symbol+offset` (the generated `sub_XXXXXXXX` functions are exported from `libblackvr.so`, 13,856
  of them, so the guest function is named even in the stripped APK library); guest esp/eax; **the thread's last kernel
  call** (ordinal, its name from `xbox_ordinals.h`, the guest call site, whether it is still inside it and for how
  long); and the frames shown so far and how long ago the last one was.
- **How MMIO traps are told from crashes:** every device handler (Kelvin, OHCI, APU, AC97, memory watch, the preflight
  test page) checks its own address range and returns 0 outside it or when its decoder refuses the instruction (Kelvin
  and the APU log the refused instruction word). The crash reporter is the first handler added, so it runs last, and
  it never claims a fault: after it the dispatcher chains to the previous action (debuggerd through libsigchain), so the
  tombstone is still written and the process dies. A handler that claims a fault always advanced pc
  (`a64_uc_emulate_mmio` steps only on success, AC97's `a64_execute` steps), so there is no claim-without-progress
  loop; a second crash on any thread is not reported again (one report, then debuggerd). Verified by reading
  `XR/src/platform/arm64_fault_decode.c` (`a64_segv`), `kelvin_mmio.c`, `xbox_memory_layout.c` and `src/main.c`.
- **Alternate signal stacks.** In an app ART's libsigchain registers its SIGSEGV handler with `SA_ONSTACK` and calls
  ours from it, so every trapped register access and the crash report run on the thread's alternate stack, not on the
  thread stack as arm64-fault.md assumed for a plain process. bionic's is 32 KB (16 KB on 32-bit), shared with the
  kernel's ~4.6 KB arm64 signal frame per nesting level. Now: every thread `CreateThread` starts
  (`XR/src/platform/win32_compat.c` trampoline; Kelvin, OHCI, vblank, ack, guest threads, APU frame thread) gets a
  256 KB one with a guard page, freed at thread exit; the game thread and the XR thread get one in the shell
  (`bvr_thread_altstack`). The `sigsegv` lines report where the handler actually ran and how much it used. The fatal
  signal handlers (SIGBUS/ILL/TRAP/FPE/ABRT) also run `SA_ONSTACK` now.
- **Hang watchdog** (`black_android_progress_watch`, a `bvr-watch` thread started just before the entry point, on by
  default): progress = frames the shell has shown (`vr_diag_counters`: stereo + flat releases). HEARTBEAT every 10 s;
  after 5 s without a frame, STALL every 5 s with: icall and kernel-call rates (0/0 = blocked or a call-free spin), the
  Kelvin pusher (words moved, flips, PUT/GET, "pusher behind"/"idle", notify-ack/context-switch/flip-stall waits, from
  the new `kelvin_diag_state`), the XR loop rate and session state, a **sample of the game thread** (SIGURG: pc, lr and a
  frame-record walk bounded by its stack, symbolised off the signal path), its last kernel call (read on that thread),
  and the return addresses on its guest stack; every 30 s of a stall the full guest thread table
  (`xbox_DumpGuestThreads`). The existing `RECOMP_WATCHDOG_SECS` watchdog (a one-shot dump and `_exit`) is unchanged.

## Changes

| File | Change |
|---|---|
| `port/quest/src/bvr_diag.{c,h}` (new) | The process-level checks above, one line each; `bvr_thread_altstack`. Pure C on POSIX; weak references to the runtime's dispatcher (the smoke APK has none: SKIP) |
| `port/quest/src/xr_preflight.c` (new) | memory (+ActivityManager via JNI), the process checks in order, AAudio, the standalone Vulkan probe, the openxr / vulkan-device / openxr-session lines |
| `port/quest/src/xr_result_names.h`, `xbox_ordinals.h` (new, generated) | `XrResult`/`VkResult` names (from the SDK headers); the 371 Xbox kernel export names (from `XR/tools/xbe_parser/xbe_parser.py`) |
| `port/quest/src/xr_main.c` | Startup order: banner, game folder (5 candidates, every one logged), storage, blackvr.ini, process checks, audio, Vulkan probe, then OpenXR with a line per stage; a fatal check keeps the game from starting and shows the red panel; the game thread gets its alternate stack and the trap check, and an orange panel if it returns non-zero; "All files access" is requested for any unreadable `/sdcard` candidate |
| `port/quest/src/xr_session.c`, `xr_vulkan.c` | `XRCHECK`/`VKCHECK` log the result by name and record the failing call for the PREFLIGHT line; `xrEnumerateReferenceSpaces`/`xrEnumerateSwapchainFormats` use the two-call idiom (a fixed capacity of 8/32 failed the whole session with `XR_ERROR_SIZE_INSUFFICIENT` on a runtime offering more); the red/orange panel; the `display` line; **foveation fix** (below); a foveated eye swapchain the runtime refuses is retried without foveation |
| `port/quest/src/vr_bridge.c`, `port/quest/vr_bridge.h` | `vr_diag_counters()` (appended to the contract): XR frames, stereo/flat releases and their times, last game frame, mode |
| `port/quest/CMakeLists.txt` | the new sources; `aaudio`, `dl`; `BLACKVR_VERSION`; `BLACKVR_XBE_SIZE`/`_SIG` read from `game/default.xbe` (read only) for a `--with-game` build |
| `port/quest/AndroidManifest.xml` | `android:allowNativeHeapPointerTagging="false"` (below) |
| `port/quest/mac_install.sh` | prints the PREFLIGHT summary, FAIL/WARN lines and the first CRASH/STALL line when it stops |
| `port/quest/README.md` | the folder order and the panels |
| `src/main.c` (`__ANDROID__` only, CRLF kept) | the CRASH line, the heartbeat/stall watchdog, `SA_ONSTACK` on the fatal-signal handlers |
| `XR/src/kernel/kernel_bridge.c` (`!_WIN32`) | `xbox_kernel_last_call()` (the calling thread's last kernel call, signal-safe) and `xbox_kernel_call_total()` |
| `XR/src/nv2a/kelvin/kelvin.c` (`!_WIN32`) | `kelvin_diag_state()`: words, methods, flips, draws, pusher GET/PUT, wait flags |
| `XR/src/nv2a/kelvin/vk/kvk_backend.c` | `vkQueueSubmit`/frame `vkWaitForFences` results logged (first 4; `VK_ERROR_DEVICE_LOST` spelled out, also to tag BlackVR) |
| `XR/src/platform/win32_compat.c` (POSIX-only file) | per-thread 256 KB alternate signal stack; `CreateThread` stacks at least 1 MB (below) |
| `tests/device_preflight/` (new) | the preflight as a program with the real dispatcher: static arm64 under QEMU and native x86-64 |
| `tests/game_render_x86` | the harness runs the preflight in xr_main.c's order and the game-thread trap check |
| `tests/vr_glue` | the fake runtime implements `xrGetInstanceProperties`, records `VK_EXT_fragment_density_map`, and refuses an FDM-flagged swapchain without it (as xr.xml requires); the foveation checks follow the GPU |

## Real bugs found

1. **Foveation could end the launch on the Quest.** With `foveation = medium` (the default) and a runtime offering
   `XR_FB_foveation_vulkan`, the eye swapchain was created with `XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB`,
   but the device was never created with `VK_EXT_fragment_density_map`, which that flag requires ("supported and
   enabled", xr.xml). A runtime that validates refuses the swapchain, `xrs_session_create` fails and the app exits.
   Fixed: the extension is enabled when the GPU has it (Adreno does), foveation is off when it does not, and a refused
   foveated swapchain is retried plain. The fake runtime now enforces the rule; on SwiftShader (no FDM) foveation is
   now off, so the earlier "foveation: fixed, level set" in the headless log (perf.md 5) is "not used" there.
2. **Guest threads had 64 KB host stacks.** POSIX `CreateThread` made the pthread stack exactly `dwStackSize` (min
   64 KB). On Windows that parameter is the initial commit and the thread reserves the image default (1 MB). The lifted
   code runs on the host stack, so every guest thread (`PsCreateSystemThreadEx` passes the title's `KernelStackSize`,
   default 64 KB) had a fraction of its Windows stack. The x86 run never overflowed it; arm64 frames differ. Now at
   least 1 MB (address space only until touched).
3. **SIGSEGV handlers on a 32 KB stack.** See "Alternate signal stacks": libsigchain runs every trap handler on bionic's
   32 KB alternate stack; a deep device model or a nested trap plus the crash report could overflow it, which kills the
   process with no report. Now 256 KB on every runtime thread.
4. **Fixed-capacity OpenXR enumerations** (8 reference spaces, 32 swapchain formats) would fail the session outright
   with `XR_ERROR_SIZE_INSUFFICIENT` on a runtime that offers more. Now two-call.
5. **A GPU device loss was silent** (`vkQueueSubmit`/`vkWaitForFences` results ignored): it would look like a hang.
   Now logged.
6. Pre-existing, not mine, not fixed: `tests/nv2a_arm64` `nv2a-kwork-tsan` reports a data race in
   `kelvin_workers.c` (`start_phase` writes `k->items[ticket % N]` at :411 while `run_task` reads it at :435) in about 1
   run in 10, **with the original `win32_compat.c` too** (3/30 vs 3-5/30 now). The read is address-dependent on the
   claim word, so arm64 hardware orders it; formally a race. It makes that ctest flaky.

## Remaining device-only risks (rough likelihood that each one bites on the first launch)

| # | Risk | Likelihood | What the log shows |
|---|---|---|---|
| 1 | Performance: the game thread over budget (perf.md: 30-55 ms of x86 CPU per level-1 frame); slow motion, not a failure | high, ~60% | HEARTBEAT fps and `game frame` rate; `game_hz = 30` is the default now (perf.md 4.3) |
| 2 | Adreno driver behaviour of the Kelvin backend's generated GLSL/pipelines (validated on SwiftShader only): wrong image, pipeline-creation stalls, or a GPU hang | medium, 25-35% | `GPU ... VK_ERROR_DEVICE_LOST`, STALL with "pusher behind", or a CRASH in the driver (`pc libvulkan`/`vulkan.adreno.so`) |
| 3 | arm64-only guest bugs the x86 harness cannot see: misaligned atomics/exclusives (SIGBUS), x86-TSO assumptions in lock-free guest code (hangs or corruption) | 10-20% | CRASH SIGBUS with `pc sub_...`; STALL with icalls 0/s |
| 4 | Khronos loader 1.1.65 not finding the Meta runtime through the broker on this Quest OS build | 10-15% | `PREFLIGHT openxr: FAIL ... XR_ERROR_RUNTIME_UNAVAILABLE` and a clean exit |
| 5 | Game files unreadable after `adb push` into `Android/data` (ownership under FUSE), or a `/sdcard` folder without All files access | ~10% | `PREFLIGHT gamedir` with the errno per path; `storage` NOT writable |
| 6 | libsigchain not forwarding nested traps, or the trap cost far above 6 us | ~5% | the log ends at `sigsegv \| nested fault next`; `us per trap` |
| 7 | execmem or `membarrier(SYNC_CORE)` refused (DSP JIT panic) | ~5% | `PREFLIGHT jit: FAIL` |
| 8 | memfd `MAP_SHARED` refused by the Quest SELinux policy (ashmem fallback exists) | <5% | `PREFLIGHT addrspace` names the step |
| 9 | Low-memory kill (≈0.5 GB RSS + GPU on 8 GB) | <5% | `memory` WARN, HEARTBEAT RSS; `lowmemorykiller`/`has died` in the system buffer, no CRASH line |
| 10 | AAudio stream not 48 kHz/stereo/i16 (runtime refuses: silence) | <5% | `PREFLIGHT audio: FAIL` |
| 11 | ANR at startup: android_main does the preflight (≈0.5 s, AAudio ≤ 0.4 s) and XR init before polling input | <3% | `ANR in com.kenny.blackvr` in the system buffer |
| 12 | Heap pointer tagging: a host pointer that lost its top byte and is then freed aborts in scudo. Turned off in the manifest (`allowNativeHeapPointerTagging="false"`); the banner shows whether the heap pointer still carries a tag | <2% now | `PREFLIGHT begin ... heap pointer 0xb4...` would mean the opt-out did not apply |
| 13 | Pre-existing 32-bit truncations of host pointers in `kernel_bridge.c` (`ObReferenceObjectByName`'s object, `IoBuildSynchronousFsdRequest`'s return), latent on every 64-bit host and harmless in the x86 run | <2% | a CRASH in the matching bridge, last kernel call ordinal 247 / 62 |
| 14 | `sched_setaffinity` outside the app's cpuset (tolerated), 16 KB pages (Quest 3 is 4 KB), ELF TLS in a dlopen'd library (fine at API 29+), `mmap_min_addr` (base is OS-chosen) | ~0 | `syscalls` / `begin` lines |

Manifest review: `com.oculus.intent.category.VR` is in the launcher intent filter, `android.hardware.vr.headtracking`
(required) and `com.oculus.supportedDevices = quest3|quest3s` are present, the Khronos loader's permissions and
`<queries>` are there, libraries are stored uncompressed and 16 KB aligned (`extractNativeLibs=false`). `targetSdk` 32
means scoped storage applies (`requestLegacyExternalStorage` is ignored on Android 11+), hence the All-files-access
path; `uses-feature` entries are not enforced by `adb install`.

## Tests (2 vCPUs, this workspace)

| Suite | Result |
|---|---|
| `tests/device_preflight`, arm64 static under QEMU (`/root/build/preflight-a64`) | pass: every line emitted; addrspace (28 mirrors + contiguous alias), sigsegv incl. nested trap (6.2 us/trap under QEMU), jit (membarrier SYNC_CORE OK), syscalls; a second thread with the 256 KB altstack; the gamedir search reports every path, the title ID, and catches a size mismatch |
| same, native x86-64 (`/root/build/preflight-x86`) | pass |
| `tests/game_render_x86` (`build-perf-x86`, 480 s run) | all PREFLIGHT lines (11 OK, 1 WARN = the fake runtime lacks the Quest-only extensions, 1 SKIP = AAudio off Android), the game-thread trap check OK, **level 1 reached in stereo** (107 stereo frames, 69,309 stereo draws), `[GRX] RESULT` printed, no CRASH, 48 HEARTBEAT lines (one transient STALL during the menus on SwiftShader, which then cleared) |
| `build-boot-a64` (arm64 boot test) | 1/1; a 24 s run with the stop off shows HEARTBEAT/STALL lines, the SIGURG sample of the game thread and its last kernel call by name |
| `/root/build/kvk-tests` | 5/5 |
| `/root/build/kvk-android` | 2/2 |
| `/root/build/vr-glue` | 2/2 (252 checks each; the FDM rule is now enforced) |
| `/root/build/nv2a-x86` | 10/10 (see bug 6 for the flaky TSan test) |
| `/root/build/nv2a-a64` | 6/6 |
| `/tmp/claude-0/xrt` (xr_shell) | 4/4 |
| `port/android` static arm64 runtime tests (`/root/build/a64rt-check`) | 5/5 (memory layout, AAudio ring, SIMD, path case) |
| Windows | clang `--target=x86_64-w64-mingw32 -E` with mingw-w64 headers of `main.c`, `recomp_manual.c`, `kernel_bridge.c`, `win32_compat.c`, `xbox_memory_layout.c`, `arm64_fault_decode.c`, `kelvin.c`: **identical token streams** before and after. With `-DBLACK_VR` (the Quest-only define) only `recomp_manual.c` differs: the appended `VrDiagCounters` declarations from `vr_bridge.h` |

APK: `build-quest-game/blackvr.apk` (27.5 MB, `--with-game`, shader cache assets), `zipalign -c -p 16` OK, `apksigner
verify` v3 OK, the manifest carries `allowNativeHeapPointerTagging=false`. The smoke APK `build-quest/blackvr.apk`
(992 KB) also rebuilt. Both hold code generated from the user's disc only in the `--with-game` one: keep it local.
