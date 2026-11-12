# perf: first-launch performance and robustness on Quest 3, measured on the headless x86 run

Stream owner: perf agent. Nothing committed. Paths are relative to /root/black; `XR` = `third_party/xboxrecomp`.
Runs: `build-perf-x86` (the same CMake project as `build-render-x86`, a separate build/scratch dir so the validation
agent's runs were not disturbed). Logs: `build-perf-x86/perf1.log` (before any change, 60 Hz, cold caches),
`perf3.log` (all changes, 30 Hz, cold caches), `perf4.log` (all changes, warm caches, async pipelines forced on as on
the Quest). Summaries: `python3 tests/game_render_x86/perf_summary.py <log>`.

## Results at a glance

| | Before (perf1) | After (perf3/perf4) |
|---|---|---|
| Trapped MMIO accesses on the real (ARM) path | 34 per menu frame, 73 per level-1 frame | 36 / 62 per frame; ~1,100/s projected on the Quest |
| Cost of one trap | x86 1.25-1.39 us, QEMU arm64 3.0-3.7 us | Quest estimate 3-6 us (10 us pessimistic) |
| Trapping cost on the Quest | | **~0.3-1.1% of one core. Not a bottleneck; no hot register** |
| Pipelines created at first use, on the GPU thread | 144, 5.6 s on SwiftShader, worst 1.31 s | **0 on the GPU thread**: 90 precreated on workers at start from the manifest, 7 new ones async, 12 draws skipped in a whole run |
| glslang at first boot | 47 compiles | **0** (SPIR-V shipped in the APK) |
| Partition images | 5 GB nominal, `ftruncate`d | **0 bytes** except what the title writes (Z: 116 KB) |
| NV2A ack thread on an idle core | `sched_yield` loop: 100% of a core | sleeps 200 us after 64 quiet turns |
| Game rate | 60 forced | `game_hz` knob, **default 30** (4.3) |
| Title-screen hang under CPU load (30 Hz) | ~1 run in 10 | **0 in 39** (pad model answers HID GET_REPORT, 4.3) |
| XAPI cancel-path crash under CPU load | 1 in 39 (either rate); 3 in 11 with USB jitter | **0 in 74** (OHCI frames wait for the driver, 4.3.1) |
| Level 1 stereo reached | ~400 s, 134 stereo frames in 575 s | 247 s, 366 stereo frames in 585 s |

## 1. Measurements

### 1.1 Instrumentation (test-only except where noted)

- `tests/game_render_x86/x86_fault_glue.c`: every serviced SIGSEGV is counted by guest address, direction and thread,
  with the time spent in the handlers (device model + engine lock + decode), lock-free.
- `grx_perf.c`: `pthread_create` and the runtime's `CreateThread` are wrapped at link time (`-Wl,--wrap`) so every
  thread is known by its start routine; CPU per thread from `/proc/self/task/*/schedstat`; memory from
  `/proc/self/status`, `smaps_rollup`, `maps`. `grx_kstats.c` reads the Kelvin counters (words, methods, flips, draws).
  Every 15 s the harness prints `[PERF]` lines; `perf_summary.py` aggregates them per mode (FLAT = title/menus,
  STEREO = level 1), per game frame (flip).
- `kvk_device.h` `KvkStats` (production, harmless): `pipe_ns`, `pipe_max_ns`, `glslang_ns`, `glslang_compiles`,
  `spv_cache_hits`.
- Production, `XR/src/kernel/thread_placement.c` (POSIX only): `xbox_PinThread` names the role threads (`bx-main`,
  `bx-pusher`, `bx-worker`, `bx-vblank`, `bx-ack`), for `top`/perfetto/simpleperf on the headset too.
- `tests/perf/fault_bench.c`: one trapped access through the game's own dispatcher and emulator.

**Host artifact found and removed:** on the x86 test host the pusher published DMA GET by storing to the read-only
USER page (the `__aarch64__`-only alias path), so every GET publish trapped: 38 per menu frame, **883 per level-1
frame**, none of which exist on the Quest. `kelvin.c`/`kelvin_int.h` now take the alias path under
`XR_HOST_FAULT_GLUE` too (test build only; Windows tokens identical), so the x86 run counts what the headset will.

### 1.2 Per game frame (x86 headless, SwiftShader, 2 vCPUs)

| | Menus (FLAT) | Level 1 (STEREO) |
|---|---|---|
| Pushbuffer words / methods | 6,518 / 6,229 | 206,070 / 193,852 |
| Draws | 20 | 723 (all GPU-vertex; CPU-shaded 0) |
| Trapped accesses | 35.9 (572/s) | 61.8 (67/s, vblank starved, see 1.4) |
| PUT-trap faults (kickoffs) | 2.1 | 5.0 |

Trapped accesses per frame by register (perf3, menus): vblank ISR on `bx-vblank`: PMC 0x140 W 5.3, PMC 0x100 R 4.7,
PMC 0x140 R 2.7, PGRAPH 0x100 R/W 3.2, 0x704/0x708/0x108 R 3.1, 0x720 W 2.1, 0x71C W 1.0, PCRTC 0x100 W 2.0, 0x800 W
1.0; OHCI 0x8-0x14 R/W 7.5; game thread: USER 0x800040 W (PUT kickoff) 2.1, APU 0x5A018 R 1.0, PGRAPH 0x400B10 R
0.05. Nothing reaches thousands; the largest single register is ~5 per frame.

CPU per frame by thread (ms of this host's CPU; the host is a shared 2.3 GHz Xeon vCPU pair, with SwiftShader's two
threads taking 45-65% each, so these are upper bounds):

| Thread | Menus | Level 1 | Note |
|---|---|---|---|
| game (`black_game_main`) | 22.2 | 30-54 | 30.1 (+13.1 other guest threads) at 60 Hz (perf1), 54.4 at 30 Hz (perf3) |
| pusher + Vulkan backend (`bx-pusher`) | 1.4 | 12-21 | 190k methods + 720 draws recorded per frame |
| raster workers (`bx-worker`) | 0.1 | ~0 | only CPU-shaded vertices; none in level 1 |
| APU frame thread + DSP56300 | 25 | — | **41% of a core continuously** (time-driven, not per frame) |
| `bx-ack` | 2.8 | 43 | 12% of a core before the backoff, 4.6% after, on this saturated host |
| `bx-vblank` | 0.5 | 1.4 | |
| shader compile workers | ~0 | ~0 | glslang only at first sight |

### 1.3 Cost of one trap (`tests/perf`, `/root/build/perf-bench{,-a64}`)

| Host | trapped load | trapped store | bare round trip | rt_sigprocmask |
|---|---|---|---|---|
| x86-64 native (Firecracker VM) | 1.25-1.36 us | 1.22-1.37 us | 1.24-1.39 us | 94 ns |
| arm64 under QEMU (`arm64run`, cortex-a76) | 2.96-3.66 us | 2.98 us | 2.47 us | 0.9-1.1 us |

The decode/emulate work is negligible natively (trapped = bare on x86); QEMU emulates the signal frame in user space,
so its numbers only bound the decoder's instruction count. **Quest estimate:** arm64 kernel signal delivery and
`rt_sigreturn` with the FPSIMD frame ~2-4 us on a Cortex-A78, plus ART's libsigchain (its handler runs ART's fault
manager checks and an `rt_sigprocmask` before calling ours) ~0.5-1 us, plus the decoder ~0.2 us: **3-6 us per trap,
10 us pessimistic.**

### 1.4 Projection to the Quest (30 Hz game)

- The vblank ISR does ~14 trapped accesses per vblank and the OHCI ISR ~6 per interrupt. In level 1 on SwiftShader
  the ISR ran only ~7 times a second (it waits for the engine lock, below); on the headset it runs at 60 Hz:
  **~900 ISR traps/s + ~6 per game frame (5 PUT kickoffs, 1 APU read) = ~1,100/s.**
- At 3-6 us: 3-7 ms per second, **0.3-0.7% of one core** (1.1% at 10 us), mostly on the vblank thread. On the game
  thread: ~6 traps per frame, **30-60 us per 33 ms frame.** Trapping is not a bottleneck, and no register is hot
  enough to be worth making plain memory or patching at its call site; nothing was changed for it.
- What the handler times show instead is **engine-lock waiting**: a vblank-ISR read of PGRAPH 0x400100 waited 1.0 ms
  on average in menus and 13.6 ms in level 1, PMC 0x100 ~1 ms: the pusher holds the Kelvin lock across method
  batches, including the backend's draws and flip. The lock wait shrinks with the GPU on the headset but remains the
  source of vblank jitter (see Remaining risk).
- Per 33 ms frame: game thread 30-55 ms of this host's CPU in level 1 (an A78 at 2.36 GHz is roughly comparable per
  clock, before the TLSDESC cost of a .so, arm64-runtime.md 7.3), pusher+backend 12-21 ms on its own core, APU+DSP
  ~40% of a core, traps <0.5 ms. **The game thread is the CPU risk, not the traps**: at 60 Hz (16.7 ms) it is 2-3x
  over budget; at 30 Hz it is at or above budget in heavy scenes.

### 1.5 TLS model of the generated code (tried, not changed)

The guest registers (`g_eax`.., `RECOMP_TLS` = `__thread`, `xbox_memory_layout.c`) live in `libblackvr.so`, which the
NativeActivity `dlopen`s, so they use the general-dynamic model: on arm64 one TLSDESC resolver call per register per
function, ~9 per guest function (`recomp_0013.o`: 5,878 TLSDESC calls in 803 functions).
- **initial-exec: unsafe, not used.** Bionic's linker refuses an IE TLS access to a symbol of a `dlopen`ed library
  ("TLS symbol ... in dlopened ... using IE access model"); bionic has no static-TLS surplus for late-loaded
  libraries (bionic `docs/elf-tls.md`). local-exec is valid only in executables.
- **local-dynamic** (one TLSDESC per function for the module base, then offsets) is what would help. AArch64 LLVM
  only emits it behind `-mllvm -aarch64-elf-ldtls-generation=1`; that cut `recomp_0013.o` to 800 TLSDESC calls, but
  NDK r27c's lld cannot link the result ("unknown relocation (528)/(530)", TLSLD_ADD_DTPREL_HI12/LO12_NC). Reverted.
- Measured on x86 (dlopen'd .so, 9 TLS registers, gcc): TLSDESC 9.6-9.8 ns per guest call, LD 3.6-3.9, IE 3.5-5.0.
  The level-1 frame makes ~57-62k direct + ~11k indirect guest calls (`-DGRX_COUNT_CALLS` build, `build-calls-x86`),
  so the whole lever is ~0.4 ms per frame on x86, ~0.5-0.8 ms on an A78: 1-2% of a 33 ms frame, not the game
  thread's problem. A later lifter change could keep the register file in one TLS struct (one TLSDESC per function)
  or pass a context pointer; not worth it before headset profiles.

## 2. Trap hot spots

None (1.4). The only hot trap in the x86 run (pusher GET, 883 per level-1 frame) does not exist on ARM and was a test
artifact, fixed in the test build. The PUT trap costs ~5 faults per level-1 frame.

## 3. First-use hitches

### 3.1 Asynchronous pipelines (`XR/src/nv2a/kelvin/vk/kvk_backend.c`, pipeline section only)

- Title-draw pipelines are keyed by **stable identities**: the shaders' GLSL hash and length (the SPIR-V cache name;
  `KVK_SID_VS_CPU/UBER/UBER_FLAT` for the built-ins) and the compatible pass's formats and views, with the backend's
  depth format symbolic (D24S8 on Adreno, D32S8 on SwiftShader). Internal passes keep handle keys and stay synchronous.
- `RECOMP_KGPU_PIPE_ASYNC` (default **on under `__ANDROID__`**, off elsewhere so the parity tests, which draw once and
  compare, are unchanged): a new pipeline goes into the table as "being created" and onto the existing compile workers
  (`vkCreateGraphicsPipelines` with the internally synchronised `VkPipelineCache`). Until it is ready the draw uses the
  ubershader pipeline with the same state if that one is ready, else it is skipped (as the shaders already did).
- **Pipeline manifest** `vk_pipe_manifest.bin`: every title-draw pipeline met is appended (64-byte records). At start
  (async mode) every record whose formats the device can render to is created on the workers before the title needs
  it.
- `VkPipelineCache` (driver-specific, never shipped): saved every 1800 frames as before **and 300 frames after the last
  new pipeline**, so a level's set is on disk even though the headset kills the process without a shutdown.
- The headless kvk library is not built with `__ANDROID__`, so perf4 forces `RECOMP_KGPU_PIPE_ASYNC=1`. Result
  (warm): 90 pipelines queued at start, 7 created later on workers, **12 draws skipped and 82 ubershader draws in the
  whole run, no pipeline created on the GPU thread**. `kvk-backend-cpu-parity` also passes with async forced (it waits
  for compiles between passes); `kvk-backend-uber`/`-stereo` draw once and are only valid synchronous.

### 3.2 Shipped SPIR-V

- `port/quest/build_apk.sh --with-game` ships `assets/kgpu-cache/` from `BLACKVR_SHADER_CACHE` (default
  `build-render-x86/scratch/root/kgpu-cache`; `none` to skip): every `vk_[vf]_*.spv`, `vk_vs_manifest.bin`,
  `vk_pipe_manifest.bin`, **never `vk_pipeline_cache.bin`**. The APK built here ships the perf run's cache: 53 files,
  1.1 MB (51 SPIR-V modules, 20 vertex and 31 fragment, and 93 pipeline records), menus + level 1.
  It is made from the user's own game, like the rest of a `--with-game` APK: keep it local.
- `xr_main.c` `seed_shader_cache()`: before the renderer starts, missing SPIR-V files are copied from the assets into
  `<internalDataPath>/kgpu-cache`, and the manifests' records are appended once per shipped cache (`seed.stamp` =
  file count + bytes; the backend ignores duplicates). Warm-run effect: **0 glslang compiles** (perf4) vs 51 cold.
- The cache names are hashes of the GLSL text, so a module is only ever used for exactly its source; if the device's
  generator ever emitted different text it would simply compile it.

## 4. Device risks

### 4.1 Partition images: sparse by construction (`XR/src/kernel/kernel_path.c`, `kernel_file.c`, `kernel_bridge.c`)

- `Partition1-5.img` are created **empty** and grow only by what the title writes. BLACK formats the Z: utility
  drive (Partition5, **116 KB**); 1-4 stay at 0 bytes (they were 0 KB allocated in every earlier run too).
- The partition's size comes from the retail table, not the file: `IOCTL_DISK_GET_PARTITION_INFO` and
  `NtQueryInformationFile` report the nominal length, and `NtReadFile` returns zeros past the file's end up to it,
  exactly what the old `ftruncate`d sparse file answered. No dependence on filesystem holes (FUSE, exFAT).
- Images left at full size by an earlier build keep working. POSIX only; Windows tokens identical.
- Verified: cold and warm runs boot, format and reuse Z:, reach level 1; `build-boot-a64` passes.

### 4.2 Memory

| | RSS | anon | shmem | PSS | VmSize | mappings |
|---|---|---|---|---|---|---|
| memory layout up (boot) | 48 MB | 33 | 5 | 47 MB | 4.8 GB | 193 |
| menus | 331 MB | 246 | 58 | 328 MB | 5.7 GB | 269 |
| level 1 | 510 MB | 409 | 71 | 508 MB | 5.8 GB | 465 |

- shmem is the 64 MB guest RAM memfd (mapped 29 times, counted once). anon includes SwiftShader's device memory; on
  the headset textures and targets are GPU (kgsl) memory.
- **The 4 GB reservation costs no memory**: it is `PROT_NONE | MAP_NORESERVE` (VmSize only, no commit, no RSS, page
  tables only where touched). It exists so the fixed apertures at base + 0x80000000 / 0xF0000000 / 0xFD000000 /
  0xFE800000 / flash land in our own reservation (arm64-runtime.md 2.1); shrinking it would mean moving apertures in
  the lifter. Not reduced. LMK looks at RSS/PSS/swap; 0.5 GB on an 8 GB headset is not a risk.
- `src/main.c` (`__ANDROID__`): `[MEM]` lines in logcat (tag BlackGame) after the memory layout is up, at 20 s, 60 s
  and every 5 minutes: RSS (anon/file/shmem), PSS, swap, address space and mapping count.

### 4.3 Game rate: `game_hz`, default 30; the title-screen hang

BLACK steps its world by a fixed 1/30 s per main-loop iteration; physics, timers and filters are frame-locked. 60 Hz is
the PC build's enhancement (`RECOMP_BLACK_60HZ`), which halves the step and patches the oscillator, the angular filter
and the physics drag/sleep thresholds. On the Quest it doubles the game thread's work per second, and the measured
game thread (1.4) cannot hold 16.7 ms, so a 60 Hz game runs in slow motion. `game_hz = 30` halves the CPU. The shell
exports `RECOMP_BLACK_HZ`/`RECOMP_BLACK_60HZ` (an explicit `env` line still wins). **Default: 30.** It was 60 while
the USB hang (below) and the cancel-path crash (4.3.1) were open, since both showed up at 30 Hz under load; both are
fixed in the device model now. `game_hz = 60` remains available.

**The hang.** Under CPU contention ~1 run in 10 at 30 Hz stopped at the title screen (frame ~33), with "flip stall
not released by a vblank ISR within 100 ms" before it. Root cause, from gdb dumps of hung runs (`GRX_STALL_DUMP`):
- When a pad enumerates (`pad 2 arriving`, game frames 30-37) XAPI sends HID GET_REPORT(input) (`A1 01 0100`,
  length 0x20). The pad model (`XR/src/usb/usb_gamepad.c`) stalled every class request it did not know.
- The STALL sends XAPI's USB driver into its stall-recovery DPC (`sub_002813B9` -> `sub_00281054` -> `sub_00280F60`),
  which walks the TDs from `ED.HeadP` until it finds its own marker. If the controller thread has already retired
  them, so that `HeadP` is the tail TD, the walk runs off the list and **spins forever at DISPATCH_LEVEL on the timer
  (vblank) thread**. No DPC or ISR runs after that, including the GPU notify ISR: the pusher waits for its notify
  ack, the game waits for the pusher. The flip-stall timeouts are the same starvation seen from Kelvin.
- Timing: which wins (driver DPC vs controller thread) depends on scheduling, hence load-dependent; 30 Hz
  enumerates the pad in a window where the race is lost far more often (0 of 11 hangs at 60 Hz under the same load).
- Not caused by this stream's changes: with `RECOMP_ACK_SLEEP_US=0` 4 of 11 runs still hung under load, and async
  pipelines are off in the x86 build that hung.

**Fix** (`usb_gamepad.c`, not on Windows): answer GET_REPORT(input) with the current report, as a real Duke/S pad
does. XAPI never takes the stall path at boot (0 STALL lines), both pads enumerate, input works. Fixes tried and
reverted because they did not stop it: OHCI lock and SOF ordering, keeping the controller thread out while a DPC
runs. `ohci.c` now logs `[OHCIn] pad n: request .. not supported: STALL` (8 times at most) for any request still
stalled, so a device log shows whether the driver can reach that path at all.

**Stress** (`build-perf-x86/stress/stress.sh <tag> <runs> <secs> [burners]`: fresh scratch per run, 7 `yes`
burners on 2 vCPUs, 40-45 s runs, a run with no frame for 22 s is dumped and counted as a hang):

| | Hangs | Crashes |
|---|---|---|
| Before, 30 Hz | ~5-6 in 48 (10-12%); 25-36% with injected OHCI jitter | |
| After, 30 Hz | **0 in 39** | 1 in 39 |
| After, 60 Hz | **0 in 39** | 1 in 39 |
| Flip-stall timeouts, after (78 runs) | 0 | |

Residual at the time: 1 crash in 39 at **both** rates under ~4x CPU oversubscription, in the same XAPI cancel path.
Fixed since; see 4.3.1. Full runs at the 60 Hz rate reached level 1 in stereo (246 s) with the image matching the
reference.

#### 4.3.1 The cancel-path crash (fixed in `XR/src/usb/ohci.c`, POSIX only)

**Symptom.** `[CRASH]` reading guest 0x7FFFFFF8 on the timer thread, `esi = 0xFFFFFFF0`, guest stack `0x00281099`
(`sub_00280F60`) <- `0x002811AD` (`sub_00281100`) <- `0x00281463` (the USB DPC `sub_002813B9`); the endpoint's HeadP
is 0xFFFFFFF0 and the TD being completed has CC 0xF.

**What XAPI does** (read from the disassembly of its XPP section). Cancelling a transfer (`0x0027F7CD`: an
enumeration request that timed out, a pipe being closed) queues the URB on the controller extension's abort list
(+0x42C), sets the ED's sKip bit and records "current frame + 1" (`0x0027F755`); the HC is then supposed to be off
the endpoint. The DPC handles the ISR's latched status in order: WritebackDoneHead first (complete everything in
HccaDoneHead), then StartOfFrame, where `sub_00281100` finishes every abort whose frame has passed. It walks the ED
from HeadP for the URB's TDs (TD +0x18 = URB), stopping at TailP, unlinks them by writing the last one's NextTD into
HeadP, sets their CC to 0xF (NotAccessed) and completes them through `sub_00281054` -> `sub_00280F60`. **If none of
the URB's TDs is on the ED, the walk stops at the tail TD and takes it for the URB's**: HeadP becomes the tail's
NextTD (0xFFFFFFFF, a free TD), the tail is "completed" with CC 0xF, and the completion walks NextTD into
0xFFFFFFF0. That is the crash. The driver relies on a TD the HC has retired being in the done queue it processes
before the abort's frame comes, which on a console holds because the ISR and DPC run within the 1 ms frame.

**What the model did** (`RECOMP_USB_EVLOG` ring of a crashed run): the HC retired the control transfer's five TDs
while WDH was still set from the previous batch, so they waited in the controller (OHCI 6.5.3: write-back only
while WDH is clear). The model's frame clock kept going (20 frames a tick), the driver set sKip and later acked WDH;
its DPC then completed the *previous* HccaDoneHead and ran the abort with the frame long passed: the transfer's TDs
were in the HC's held queue, on neither the ED nor HCCA. The timer thread runs the ISR/DPC tens of ms late under
load, so the race is load-dependent and rate-independent, as seen.

**Fix: frames wait for the driver.** The controller thread starts no frame (no HcFmNumber/HccaFrameNumber advance, no
SF, no list processing) while a done queue is owed: WDH set and enabled, or retired TDs held. The emulated bus thus
runs in the driver's time: whatever is retired in a frame is in the done queue the driver takes before any later
frame starts, which is the guarantee XAPI's abort path relies on. No time-out: a driver whose ISR/DPC cannot run
takes neither completions nor SOFs, so letting frames pass only makes its aborts unsafe (an earlier variant resumed
frames after 10 s: it tripped in 3 of 11 jittered runs, each time while the timer thread waited behind the Kelvin
lock, which reopens exactly the window the crash needs; no crash was seen with it, but it was removed). WDH masked
or a controller reset hold nothing. `[OHCI0] frames held for 250 ticks` /
`frames resume after N ticks held` (4 times at most) say when the driver was slow; it is normal under heavy load
(the timer thread waits behind the pusher's Kelvin lock), not a fault.

Also corrected in the same pass, from the OHCI 1.0a spec and xemu's `hcd-ohci.c`:
- **Registers shared with the driver's fault handler are updated atomically** (`__atomic`, seq-cst RMW, acquire
  loads): HcInterruptStatus (the HC sets SF/WDH/RHSC while the driver clears bits; a plain `|=` racing the driver's
  `&= ~v` could lose either), HcCommandStatus, HcInterruptEnable, the port status registers. HccaDoneHead and the TDs
  are written before WDH is set, HccaFrameNumber before SF (release), so on arm64 whoever sees the bit sees the data.
- **HeadP is written back after every retired TD**, as the HC does (6.4.4.1), not once per ED after the walk.
- **Data toggle**: from the TD when T[1] is set, else the ED's toggleCarry, flipped per packet moved, kept in the TD
  (T = 1x) and in toggleCarry (4.3.1.2/4.3.2). The TD's CC field and NextTD are written as before; EC stays 0.
- **FrameNumberOverflow** is raised when bit 15 of the frame number toggles. XAPI enables it and keeps the high
  half of its 32-bit frame number from it (`0x00280D16`); without it the 32-bit frame number fell back by 65536 at the
  16-bit wrap (~65 s of bus time), and every abort or frame wait then stalled for another ~65 s.
- **HcFmNumber reads return the controller's frame number** (was derived from the register read count, so it
  disagreed with HccaFrameNumber).
- sKip and Halted EDs are left untouched (6.4.2), as before; a TD with CC 0xF is never produced by the HC.
The Windows build keeps the old code paths: `clang --target=x86_64-w64-mingw32 -E` token streams of `ohci.c` with
and without `-DBLACK_VR` are identical before and after.

**Test knobs** (POSIX, env): `RECOMP_USB_JITTER_US=n` sleeps the controller thread 0..n us at half of its scheduling
points (frame start, each retired TD, before write-back); `RECOMP_USB_EVLOG=<file>` keeps a 32k-event ring
(frames, retirements, write-backs, status/enable/command writes, ED states) in a shared file mapping that survives
a crash: `python3 tests/game_render_x86/ohci_evlog.py <file> [N] [--ed VA]`.

**Stress** (`build-usb-x86`, a separate build of `tests/game_render_x86`; `build-usb-x86/stress/stress.sh <tag> <runs>
<secs> [burners]`, 7 `yes` burners on 2 vCPUs started and bounded by the script, 40 s per run; `ab.sh` alternates the
old and new controller under the same burners; a hang = a watchdog stall still running at the time budget after
>= 20 s):

| Controller | Rate | Load | Runs | Crashes | Hangs |
|---|---|---|---|---|---|
| old (`game_render_x86.legacy`) | 30 | burners + 3 ms jitter | 11 | **3** (same signature) | 0 |
| old, A/B interleaved | 30 | burners | 5 | 1 | 0 |
| new | 30 | burners | 43 (38 + 5 A/B) | **0** | **0** |
| new | 30 | burners + 3 ms jitter | 20 | **0** | **0** |
| new | 60 | burners + 3 ms jitter | 10 | **0** | **0** |
| new | 30 | burners, 100 s | 1 | 0 | 0 (276 frames) |

Most runs show one transient stall of 7-25 s at about frame 117, where the title's 3D attract scene starts: the
pusher waits in SwiftShader (`frame_begin` <- `kgpu_flip`) holding the Kelvin lock and the timer thread waits for
that lock. The old controller shows it too (`pre_j30_a` runs 10-11), the 100 s run continues past it, and no USB
event is involved; it is SwiftShader under 7 burners, not a hang. A run with input (no burners, 30 Hz): START
reaches the main menu.

### 4.4 Raster workers

With the Vulkan backend the raster threads only shade vertices of draws whose vertex program is not compiled yet
(0.1 ms per menu frame, none in level 1). Defaults `raster_workers = 1` (`RECOMP_KELVIN_THREADS = 2`) and
`raster_spin_us = 50` (`RECOMP_KELVIN_SPIN_US`; `kelvin_workers.c` now accepts any 0-5000 on Android, Windows keeps
its 50/250/1000). Placement already puts WORKER on the efficiency cluster.

### 4.5 The NV2A acknowledger (`XR/src/kernel/xbox_memory_layout.c`, `__ANDROID__`)

Its loop ended in `Sleep(0)` = `sched_yield()`, which returns at once on a core with nothing else to run: one silver
core at 100% on the headset. Now: a turn that changed a register is followed by yields as before (microsecond
latency in a burst); after 64 quiet turns it sleeps `RECOMP_ACK_SLEEP_US` (default 200 us; 0 = the old loop). A title
waiting on something only this loop answers (the DSP doorbell, a handful of times at boot) waits at most 200 us;
KeTickCount (1 ms units) stays current. Measured on the contended host: 4.6% of a core, and the 60 Hz run under
contention drew 396 frames in 60 s vs 264 without the backoff.

## 5. GPU: foveation and resolution

- **`resolution_scale` default 0.8** (was 1.0): Quest 3's 2064x2208 recommendation becomes 1651x1766 per eye. The eye
  swapchain sets the Kelvin raster size, so the main target, its offscreen chain and the composition all scale
  (~0.64x fragments). The test harnesses pin 1.0 so their eye images stay comparable.
- **`foveation = medium`** (off/low/medium/high): when the runtime offers XR_FB_foveation, _configuration,
  XR_FB_swapchain_update_state and XR_FB_foveation_vulkan, the shell enables them, creates the eye swapchain(s) with
  `XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB`, creates a level profile (no dynamic foveation) and sets
  it with `xrUpdateSwapchainFB`, then destroys the profile; flat and HUD quads are not foveated. Failure only logs.
  Extension negotiation moved into `xrs_instance_extensions/xrs_instance_procs` so the harnesses run it too.
- `tests/vr_glue/fake_openxr` advertises the extensions and implements the three calls with validation (a profile on
  a swapchain without the flag is refused); `vr_glue` checks the flag on the eye swapchains only, level medium, one
  update per eye swapchain, every profile destroyed (251 / 255 checks). The headless run logs
  `foveation: fixed, level set on the eye swapchain`.
- **Limit, important:** on Vulkan the runtime's foveation acts through the swapchain's fragment density map, which
  only takes effect in render passes that bind it. The backend's only pass into the eye swapchain is the composition
  (one textured blit per eye), and the world is rendered into Kelvin's own main target, so **today FFR saves little**;
  the resolution scale is the real GPU lever. Making FFR pay means a fragment density map (`VK_EXT_fragment_density_map`,
  Adreno has it) on the main-target multiview passes; not done (untestable on SwiftShader).
- Already in the shell: refresh rate (XR_FB_display_refresh_rate), performance levels, thread hints.

> Update (device-preflight.md, bug 1): the FDM swapchain flag requires `VK_EXT_fragment_density_map` enabled on the
> device; the shell now enables it where the GPU has it and leaves foveation off otherwise, so the SwiftShader runs
> log "foveation: not used". The fake runtime enforces the rule.

## 6. blackvr.ini knobs (new or changed)

| Key | Default | Range | Effect |
|---|---|---|---|
| `resolution_scale` | **0.8** (was 1.0) | 0.3-2.0 | eye swapchain and Kelvin raster size vs the recommendation |
| `foveation` | medium | off, low, medium, high | XR_FB_foveation level on the eye swapchain |
| `game_hz` | **30** (was 60) | 30, 60 | `RECOMP_BLACK_HZ` / `RECOMP_BLACK_60HZ` (4.3) |
| `raster_workers` | 1 | 0-4 | `RECOMP_KELVIN_THREADS` = workers + 1 |
| `raster_spin_us` | 50 | 0-5000 | `RECOMP_KELVIN_SPIN_US` |

Env only: `RECOMP_KGPU_PIPE_ASYNC` (Android default 1), `RECOMP_ACK_SLEEP_US` (200). `vr_config_export_env` exports
the keys before the game thread (explicit `env` lines first, with overwrite; the keys only fill what is unset; the
game's own defaults last). Unit test: `tests/xr_shell` (53 checks).

## 7. Tests

| Suite | Result |
|---|---|
| tests/kelvin_vk (`/root/build/kvk-tests`) | 5/5 (cpu-parity also with async forced) |
| `/root/build/kvk-android` | 2/2 |
| tests/vr_glue (`/root/build/vr-glue`) | 2/2, 251 + 255 checks (foveation checks added) |
| tests/xr_shell (`build-xr-shell`) | 4/4 (53 input/config checks) |
| tests/nv2a_arm64 x86 / a64 | 10/10, 6/6 |
| `build-boot-a64` | 1/1 (first GPU touch at 0.2 s; `[MEM]` line logged) |
| Headless game (`build-perf-x86`, same project as `build-render-x86`) | cold and warm runs reach level 1 in stereo; eye image matches `build-render-x86/frames/latest_eyeL.png` (same room, weapon, HUD) |
| tests/perf fault_bench | x86 and arm64 run |
| USB cancel-path fix (4.3.1), later pass | kvk-tests 5/5, kvk-android 2/2, vr-glue 2/2, nv2a-x86 10/10, nv2a-a64 6/6, xr_shell 4/4 (53 input checks, `game_hz` default 30), `build-boot-a64` 1/1; `ohci.c` builds for arm64 (`build-game-android`, NDK r27c); Windows tokens of `ohci.c` identical |
| Windows (clang `--target=x86_64-w64-mingw32 -E`, mingw-w64 headers), token streams before vs after: thread_placement.c, kernel_path.c, kernel_file.c, kernel_bridge.c, xbox_memory_layout.c, kelvin.c, kelvin_workers.c, kelvin_mmio.c, usb_gamepad.c, ohci.c, main.c and recomp_manual.c with and without `-DBLACK_VR` | **all identical** (kvk_backend.c is not built on Windows) |

APK: `build-quest-game/blackvr.apk` rebuilt (27 MB, shader cache assets 1.1 MB), zipalign -p 16 OK, apksigner v3
verified.

## 8. Remaining performance risk (in order)

1. **The game thread.** 30-55 ms of x86 CPU per level-1 frame (upper bound: shared vCPUs), so at or over the 33 ms
   budget on an A78 even at 30 Hz, before the .so's TLSDESC overhead. First thing to measure on the headset
   (`simpleperf` on `bx-main`; the threads are named now), then `game_hz = 30`. The TLS model is worth only 1-2%
   (1.5); the lever is profiling the generated code's hot functions.
2. **APU + DSP56300: ~40% of a core continuously.** On its own thread; watch it does not share a core with the game.
   *Fixed in section 10: ~3% of an x86 core now; the 40% was the EP DSP spinning in an idle poll loop.*
3. **Engine-lock waits.** Register accesses wait for the pusher's lock (1-14 ms averages on SwiftShader), so vblank
   ISRs are delivered late behind long method batches and the backend's flip. If frame pacing is uneven on the
   headset, release the lock around the backend's blocking calls.
4. **USB** (4.3): the title hang and the XAPI cancel-path crash are fixed in the device model (0 in 74 stressed
   runs). A device log with many `frames held` lines means the timer thread (USB ISR/DPC) is starved.
5. **FFR without a fragment density map** gains little (5).
6. Async pipelines skip a few draws at a pipeline's first use when the manifest lacks it (12 in a warm run).
7. Unverified on hardware as before: signal cost through libsigchain (estimate 3-6 us), Adreno pipeline-creation
   times, GPU memory.

## 9. Game-thread profile and optimisation pass (2026-11-03/04)

Nothing committed. Supersedes the game-thread lines of 1.2/1.4/8.1 and the conclusion of 1.5 (the TLS model *was*
worth fixing: 1-2% was measured against a frame that was mostly busy-wait).

### 9.1 Tools (test-only, tests/game_render_x86)

- `grx_prof.c`: `GRX_PROF=<file>` makes the game thread sample itself on a software perf event of its own task clock
  (`perf_event_open` PERF_COUNT_SW_TASK_CLOCK, user time, 250 us, `GRX_PROF_US`; paranoid 2 allows it, no `perf` binary
  here) with a libgcc backtrace and the Kelvin flip count. The POSIX CPU-clock timer fallback only fires at this
  kernel's 250 Hz tick. `grx_prof.py <file> [--from-flip --to-flip --exclude FUNC --inclusive --per-flip]`: self,
  inclusive, by category, per flip.
- `GRX_FRAMECPU=<file>` (render_main.c) + `grx_framecpu.py`: the game thread's CPU clock (user + kernel) per Kelvin flip.
- `fwd` in `GRX_INPUT` holds the left stick forward (walks).
- `-DGRX_COUNT_CALLS` builds (`build-calls-x86`) now also write per-VA call counts (`GRX_CALL_HIST=<file>`), and
  `-DGRX_CALL_TRACE` hashes every guest call target plus the registers at the call (`[TRACE]` at powers of two,
  `GRX_TRACE_FROM/TO` lists calls): the equivalence check of 9.5.
- `build-perf-x86/prof/ab.sh <tag> before|after`: one profiled 480 s run (`game_render_x86.before` = the old binary).
  Window: `GRX_INPUT="40:menu,70:a,100:a,130:a,190:a,200:fwd:20,225:right:2,230:fwd:20,255:left:2,260:fwd:30,
  300:right:3,305:fwd:40,350:left:3,355:fwd:40,400:right:2,405:fwd:40"`, eyes 416x448, no video: level 1 in stereo
  from ~137 s, then ~300-390 flips of walking and turning through the first rooms. Same script for every run.

### 9.2 Where the game thread's time goes (x86, level 1, 3 baseline runs)

1. **A busy-wait, not work.** `sub_0026CC10` is D3D's push-buffer space wait: the loop at 0x0026CD80 re-reads the
   GPU's GET until the span it needs is free. It spins for as long as the pusher and the backend take to drain the
   buffer, vblank waits included: **96% of the game thread's CPU in the menus, 93-97% in level 1** (87-212 ms per
   level-1 flip on SwiftShader). That is what the 30-55 ms/frame and the 100-540 ms spikes of 1.2/1.4 were. On the
   headset it would have kept one big core at 100% whatever the scene.
2. **The real work: median 6.0 ms, p95 9.75 ms, mean 6.2 ms per level-1 flip** (spin left out; runs b1-b3: 5.6, 6.5,
   6.6 ms mean). Generated guest code 91%, spread flat (hottest function `sub_0015B380` 7%, then < 2.6% each); libm
   2.5%, libc 2.4%, win32 compat 2.1%, indirect dispatch (`recomp_lookup`) 1.8%, kernel HLE 0.1%, MMIO traps 0.1%.
   66,000 guest calls per level-1 flip (direct + indirect, `GRX_CALL_HIST`).
3. **What the x86 numbers hide (arm64 code inspection, NDK r27c objects of the APK build).** In the dlopen()ed
   `libblackvr.so` bionic gives TLS only dynamically: each of the ~40 register variables a function touches is one
   TLSDESC call into `tlsdesc_resolver_dynamic` (call site + ~17 instructions). 82,500 such call sites in the
   generated code, **~680,000 resolver calls per level-1 frame** (10.3 per guest call, weighted by the measured call
   counts), plus a reload of `g_xbox_mem_offset` before nearly every guest access (with `-fno-strict-aliasing` every
   volatile guest store may alias it): 413,000 of the 3.63 M generated instructions. The x86 harness links the code
   into an executable (local-exec TLS), so none of the TLSDESC cost shows there.

### 9.3 Changes

| # | Change | Where | Scope |
|---|---|---|---|
| 1 | **The push-buffer wait yields.** A title hook calls `black_pb_space_wait(spins)` on each further iteration of the 0x0026CD80 loop: 64 `yield`/`pause`, 64 `sched_yield`, then `nanosleep` 50 us backing off to 100/200 us. Guest state untouched. `RECOMP_BLACK_PB_SPIN=1` restores the spin. | `scripts/generate.py` (title_hooks, declarations), `src/recomp_manual.c` | POSIX; on Windows the hook is `((void)0)` |
| 2 | **The register file is one TLS object** (`RecompRegFile g_recomp_regs`; `g_eax`, `g_xmm0`, `g_fp_stack`, ... are macros for its members, so no source changes beyond the declarations). One TLSDESC per function instead of ~10. | new `templates/runtime/recomp_regfile.h`; `recomp_types.h`, `src/kernel/xbox_memory_layout.[ch]` (definition, initial values kept), the extern sites in kernel_rtl.c, kernel_bridge.c, recomp_trace.c, ohci.c, src/main.c, recomp_manual.c, black_native_menu.inc, black_hud_input.inc, generate.py's IDCT prefix; `tools/recomp/translator.py` copies the new header next to gen's recomp_types.h | `__ANDROID__` (and the x86 harness, which builds the Android paths); `RECOMP_NO_REGFILE` opts out; Windows keeps the separate variables. Mixing the two models cannot link (the old symbols are gone) |
| 3 | **`g_xbox_mem_offset` is invariant in generated code**: declared `const` there (clang treats it as memory no store modifies), so it is loaded once per function. Only the runtime writes it, before any generated code runs. | `recomp_types.h` | generated code, clang, `__ANDROID__`; `RECOMP_NO_MEMBASE_INVARIANT` opts out |
| 4 | **Device-path XMM lanes go through general registers** (`RECOMP_LANE_GPR`): left alone, clang stores lane n of a vector with `ST1 {v.s}[n]` (and loads with `LD1`) in `XMM_MEM`/`XMM_STORE`'s device branch and the half moves, which the arm64 MMIO decoder refuses, on exactly the path kept for it. | `recomp_types.h` | AArch64 only |
| 5 | **`-fno-slp-vectorize` for the generated code**: with one register-file object the SLP vectorizer loaded neighbouring registers as one vector and pushed a lane of it to guest memory with `ST1`. Off: smaller code and no ST1 lane stores. | `port/android/game.cmake` (and the x86 harness) | Android build |
| 6 | **`-Wl,-Bsymbolic-functions`** for `libblackvr.so`: calls between the 14 chunks bind inside the library (direct `BL`, not a PLT stub + indirect branch). PLT entries 9,779 -> 532; `sub_` symbols stay exported (13,856), so the crash reporter still names them. | `port/quest/CMakeLists.txt` | APK |

gen/ was regenerated with `scripts/generate.py` (a scratch regeneration before any change was byte-identical to the
old gen/; after, the only differences are the hook lines in `sub_0026CC10`, the declarations block, recomp_types.h
and the new recomp_regfile.h). `tests/arm64_runtime/test_simd.c` defines `g_recomp_regs` under RECOMP_REGFILE.

### 9.4 Results

**x86 headless, level-1 window, 3 runs each** (`prof/b1-b3` before, `prof/c1-c3` final):

| Game thread per level-1 flip | Before | After |
|---|---|---|
| CPU (user + kernel), median / p95 / mean | 7.1 / 745 / 154 ms | **5.5 / 39 / 9.3 ms** |
| Work (user, spin and sleeps left out), median / p95 / mean | 6.0 / 9.75 / 6.2 ms | **5.25 / 8.25 / 5.55 ms** (-12% / -15% / -10%) |
| In the push-buffer wait (user) | 87-212 ms | 0.08-0.12 ms (+ kernel time of the sleeps) |
| Menus: CPU per flip, median / p95 / mean | 21.1 / 42 / 21.8 ms | **1.6 / 4.2 / 1.9 ms** |

The remaining level-1 p95 of 39 ms is kernel time of the wait's sleeps while SwiftShader takes ~1 s per frame (one
wakeup per 50-200 us); with a fast GPU the waits, and so that cost, shrink to a few hundred us per frame. The x86 work
gain is change 3 (the offset was reloaded before guest accesses on x86 too); change 2 is neutral on x86 (local-exec).

**arm64 generated code (NDK r27c, the APK's flags), static and weighted by the measured level-1 call counts:**

| | Before | After |
|---|---|---|
| Instructions in the generated code | 3,632,544 | **2,609,167 (-28%)** |
| TLSDESC call sites | 82,500 | **13,732 (one per function)** |
| TLSDESC resolver calls per level-1 frame | ~680,000 | **~66,000** |
| Reloads of `g_xbox_mem_offset` (`ldr xN, [xM]`) | 413,666 | 61,847 |
| Size of the called functions, weighted by calls, per frame | 28.9 M instructions | 21.1 M |
| `libblackvr.so` .text / PLT entries | 20.4 MB / 9,779 | **16.3 MB / 532** |
| Guest-access instructions the MMIO decoder refuses (all LD1/ST1/LD1R lanes) | 3,548 | 403 (LD1R/LD1 of the stack, as before) |
| APK | 27 MB | 22 MB |

The x86 build of the same code has 2.76 M instructions: the arm64 code is now as dense as the x86 one, where before it
had ~30% more instructions plus ~600,000 resolver calls a frame.

### 9.5 Equivalence

- Lifted C is unchanged except the hook in `sub_0026CC10`; the register change is storage only (same types, same
  per-thread objects, same initial values) and the offset is never written while generated code runs.
- **Call trace**: old and new trees built with `-DGRX_CALL_TRACE` (x86 harness, fresh scratch each run): the hash of
  every call target and the 7 registers at each call is identical in all 4 runs (2 old, 2 new) for the first 16,384
  calls of the game thread, and a listing of calls 16,384-32,768 (target + registers) is identical between old and new.
  After that two runs of the *same* build diverge (thread timing), and old and new runs produce hashes from the same
  set at 32,768 calls.
- Windows (`clang --target=x86_64-w64-mingw32 -E`, mingw-w64 headers): token streams before vs after are identical for
  xbox_memory_layout.c, kernel_rtl.c, recomp_trace.c, kernel_bridge.c, ohci.c, main.c and recomp_manual.c, each with and
  without `-DBLACK_VR`, and for generated chunks 0-12; chunk 13 differs only by the unused `_pb_spin` local and the
  `((void)0)` hook, and its mingw object code for `sub_0026CC10` is identical.
- Headless runs: all 6 after-runs reach level 1 in stereo, walk and turn.

### 9.6 Tests

kvk-tests 5/5, kvk-android 2/2, vr-glue 2/2 (338 + 342 checks, gun phase included), nv2a-x86 10/10, nv2a-a64 6/6, xr_shell (`/tmp/claude-0/xrt`)
4/4, arm64 runtime (`/root/build/a64rt-check`, incl. test_simd with the register-file object) 5/5, `build-boot-a64`
10/10 (rebuilt on `build-game-android`; the boot binary has `g_recomp_regs` and the hook). APK:
`build-quest-game/blackvr.apk` rebuilt with `--with-game`, 22 MB, `zipalign -c -p 16` OK, `apksigner verify` v3 OK.

**Harness gun-latch test (`ctest -R gunlatch`, full game): FAILS, and not because of this pass.** The off hand and arm
pose differ between the "before" and "after" frames and 3-5% of the world changes. The *old* binary
(`game_render_x86.before`, built from the tree before any change here) fails with the same numbers (49.2% / 33.9% of
the moved pixels on the weapon, median patch error 30 px), and so does the new binary on a copy of
`build-render-x86`'s scratch; the binary in `build-render-x86` (built 19:24, before `kvk_backend.c`,
`kvk_vr_layers.inc` and `gun_latch_check.py` were changed again at 19:38-19:56) passes on that same scratch (median
error 3.2 / 2.3 px, world 0.01% changed). So the regression is in those later Kelvin/VR-layer or checker edits, which
every build made after them contains. The vr-glue gun phase, the check this pass was asked to keep, passes.

### 9.7 Quest 3 estimate (game thread)

The x86 work is 5.5 ms mean, 8.25 ms p95 per level-1 flip on a 2.1 GHz Xeon vCPU (shared host, sampler on). A Cortex-
A78 at 2.36 GHz is roughly that per clock and the arm64 code now has about the x86 instruction count, plus ~66,000
TLSDESC calls (~0.3 ms). **Estimate: 5-8 ms typical, ~10 ms p95 per 33.3 ms frame** in this window: about a quarter of
the 30 Hz budget, so the game thread is no longer the CPU risk (before: ~2.5-3 ms more TLSDESC + reload overhead, and a
core held at 100% by the wait). Heavier scenes (combat, many AI, explosions) are not in this window; even at twice the
cost they fit 30 Hz. 60 Hz (16.7 ms) looks possible for the game thread in light scenes; the pusher/backend and the GPU
decide it. Unmeasured on hardware; `simpleperf` on `bx-main` is still the first thing to run on the headset.

### 9.8 Tried or considered, not done

- **-O3** for the generated code: +2.4% code size (chunk 1, 245k -> 251k instructions), no measurable way to show a
  gain here; kept -O2.
- **LTO/ThinLTO, PGO**: 64 MB of C; cross-chunk inlining of the small guest functions might help, but link time, the
  crash reporter's per-function names and no device profile for PGO. Not tried.
- **`-fvisibility=hidden`**: would drop the exported `sub_` names the crash reporter relies on; `-Bsymbolic-functions`
  gets the direct calls without that.
- **Indirect-call dispatch** (`recomp_lookup` ~1.8%, the manual-override check before it ~0.3%): too small to be worth
  a direct-mapped table change now.
- **A futex wake from the pusher instead of polling sleeps** in the push-buffer wait: wakes on every 1 KiB GET publish
  while the game waits (~750 a level-1 frame), no cheaper than the backed-off poll; not done.
- **APU/DSP thread** (40-65% of a core on this host): on its own thread; untouched here (8.2).

### 9.9 Remaining risk / follow-ups

- The arm64 MMIO decoder still refuses LD1/LD1R single-structure loads: 403 sites in the generated code (they were
  there before: stack data read with a broadcast). Harmless unless such an access hits a device aperture; if a device
  log ever shows a refused `0x0d40..`/`0x4d40..` word, teach `a64_decode` LD1/ST1 single structure.
- The wait still costs one wakeup per 50-200 us of waiting.

## 10. APU frame thread: the EP DSP's idle poll loop (2026-11-11)

Nothing committed. Headset evidence: `mcpx.apu_thread` 70% of a big core in the menus, 72-79% in level 1, constant
(60 Hz). x86 harness baseline: 44% of a core in both. This is the "APU + DSP: ~40%" of 1.2 / 8.2.

### 10.1 Where the time went

Tools (test-only): `grx_prof.c` now also samples the APU thread (`GRX_PROF_APU=<file>`; the XR thread finds
`mcpx.apu_thread` by name at its first flush, opens a user-time task-clock perf event on that tid and directs SIGPROF
at it; its own ring and file, `grx_prof.py` reads it unchanged). `RECOMP_APU_DSP_STATS=<prefix>` (apu_dsp.c, env-gated,
off by default) prints per-core cycles used per tick and halt rate every 15000 ticks, and writes the JIT's block
profile and P memory (`<prefix>.{gp,ep}.{prof,pram}`). The P memory was disassembled with the in-tree
`dsp56300-disasm`, built from source. The dumps are disc-derived and stay in `build-apu-x86/diag`.
Runs: `build-apu-x86` (configured like `build-perf-x86`), `prof/ab.sh <tag> <binary> [secs]` = the input script of 9.1,
`RECOMP_BLACK_HZ=60`, audio recorded to `prof/<tag>.wav`. `prof/apucpu.py` averages the HEARTBEAT lines (user + kernel).

- **~97% of the thread is DSP56300 execution, ~95% of it one loop on the EP.** Samples (menus and level 1 alike): JIT
  code 39%, the JIT's run/dispatch loop 26%, `jit_update_nz` 21%, interrupt polling 4%, `dsp_read_peripheral` 4%.
  Voice processor, mixdown, DMA, output: ~3% together.
- **GP**: 303 of 88,875 cycles per tick, halts every tick. Cheap.
- **EP**: 88,873 of 88,875 cycles per tick, **never halts**, PC always `$000031`. One block (`$31`, 3 words, 5 cycles)
  is 100% of its time: ~17,800 dispatches per 32-sample tick, ~27 M per second. The EP microcode, after its work,
  writes HALT (`movep #1,x:$ffffc4`) and then waits for the start-of-frame bit in a counting loop:

      P:$30  clr  a
      P:$31  add  #$01,a
      P:$32  jclr #1,x:$ffffc5,$000031

  In this model, HALT holds only until the next 32-sample tick (`dsp_run_frame` clears `halt_requested` every tick).
  The EP's start of frame comes on one tick in eight, and it is only latched into `$ffffc5` while the core is halted
  (or by a HALT write with frames pending). So the EP resumes on a non-start tick, enters the loop, and never sees the bit
  again. It spins for every cycle of every tick, from boot on, in menus and level 1 alike. It never writes its output
  FIFO either: `RECOMP_APU_TRACE_OUTPUT` shows `native-ticks=0` in every run, before and after. Everything the game plays
  is the direct-mixdown fallback ("direct-mixdown output fallback active").

Not the cause (checked): the timer pacing (one wait per 8 ticks, 188/s, deadline-driven, no spin), the voice processor
(~1% of the thread; it only walks active voices), the output path.

### 10.2 Change: exact fast-forward of the idle poll loop (`XR/src/apu/apu_dsp.c`, all platforms)

`dsp_run_tick` replaces the plain `dsp56300_run` of a tick. When the core is parked at the head of
`add #xx,a|b ; jclr #n,x:$ffffc5,head` (matched in P memory), it qualifies if all of these hold:
- the bit is clear;
- the core is quiescent: no interrupt pipeline or pending interrupt, no REP, not in a DO loop (SR.LF), not halted,
  normal power state;
- the accumulator is canonical.

Then the tick runs on the JIT in three parts that add up to the same budget:
1. One real iteration. It brings the budget to exactly 1, and it measures the iteration's cost and accumulator delta.
   It also checks that nothing else changed except the CCR flags (all registers, stacks, interrupt block, PC).
2. A skip of all but the last two iterations. The accumulator, cycle counter and budget advance by that many. Skipped
   only if the accumulator cannot overflow (so the sticky L bit cannot be set).
3. The last iterations, run for real. They leave the PC, CCR and the negative cycle overshoot exactly as the full run.

Why it is exact: `$ffffc5` is `dsp->interrupts | dma.pending_interrupts | EOL`. During a tick the only things that
write it are the DSP's own peripheral writes (the loop makes none). The frame thread holds the APU lock, and every guest
register access takes that lock. The read has no side effect. Anything else falls back to the full run.
`RECOMP_APU_DSP_FASTFWD=0` turns it off. `=verify` runs the full loop and compares it with the prediction (no saving).

Not done, and flagged for the lead: *fixing* the HALT model so the EP waits for its start of frame. The EP would then
run its program and its native PCM would replace the fallback mix. That changes what the game hears, and it is the
untested "full in-game audio fidelity" path (apu/README). It is a separate decision.

### 10.3 Results

APU thread CPU, HEARTBEAT, % of one x86 core (2-vCPU host shared with SwiftShader and other agents' runs):

| Run | Menus | Level 1 |
|---|---|---|
| before b1 | 43.5% (36-51) | 44.1% (32-51) |
| before b2 (host also running QEMU boot tests) | 33.9% (25-40) | never left the front end (see 10.4) |
| after a1 | 3.0% | 3.0% |
| after a2 | 2.4% | 2.7% |
| after a3 | 2.7% | 2.5% |

**~15x less.** User-time samples after: VP 16%, DSP DMA word-by-word FFI reads ~25%, GP JIT a few %, the rest small.
That is ~0.5 ms of CPU per 16.7 ms frame here. On the Quest, scaling 70% by the same ratio gives **~5% of a big core,
unmeasured.** With the APU off the two vCPUs, the harness did more in the same 330 s window: 5,718 renderer frames vs
3,064, and 300 level-1 stereo frames vs 159 (a1 vs b1). arm64 QEMU boot (real game microcode, `BOOT_STOP_AT_GPU=0`,
120 s): fast-forward taken on 74,997 of 75,000 EP ticks, APU thread 10-12% vs 28-52% with the full loop (verify mode).
Those are QEMU numbers, not headset numbers.

### 10.4 Equivalence

- **In-game verify (x86, `RECOMP_APU_DSP_FASTFWD=verify`, 300 s, menus + level 1): 164,996 EP ticks, 0 mismatches.**
  This is every EP tick but 4 at boot. Each tick checks PC, accumulator, cycle counter, budget, all registers but the
  CCR, stacks and the interrupt block against the prediction.
- **arm64 (QEMU, Cranelift aarch64 back end, real microcode): 4,096+ ticks verified, 0 mismatches.**
- **Unit test `tests/apu_dsp_fastfwd`** compiles the production apu_dsp.c. A forked reference runs each scenario with the
  fast-forward off, and the parent compares the full DSP state after every tick: park / random exits / accumulator near
  overflow / `b` + `add #3` + EOL bit; 4,000 ticks each. **Identical on x86 and on arm64** (`/root/xtool/arm64run`). A
  negative control (accumulator off by 1 in the skip) fails all four.
- **Audio**: the fast-forward cannot change the output. The EP never produces FIFO output in either build
  (`native-ticks=0`); all output is the fallback mix of the VP's bins, and the VP/GP paths are untouched. The WAVs of
  different runs are not sample-comparable (the game's timing is not deterministic, so 4 s windows do not line up).
  Their statistics match: format OK; spectral roll-off 11.5 kHz, centroid 812-815 Hz (719 in a3) before and after;
  level-1 RMS -27.1..-28.7 dBFS in all; no clipping. What differs is caused by the starved baseline on this host:
  - dropouts: 763 (b1) vs 26-98 (after);
  - rebases (producer more than 64 ms behind): 1,236 (b2) vs 37-135;
  - device underruns: 1,678 vs 61-183;
  - front-end silence: 41.6% vs 19-56%.
  The b2 baseline, run while QEMU boot tests loaded the host, stayed in the front end and was 98% silent. It is the
  before binary, so this is not caused by the change, but it shows what a starved APU does here.
- All after-runs (a1, a2, a3) reach level 1 in stereo with sound, walk and turn, no crash. a3 shows one 1 s flat
  presentation inside level 1 (a mode switch, then back to stereo).
- Windows: same C, exact on every platform. A mingw-target syntax check of apu_dsp.c is clean.

### 10.5 Tests

`tests/apu_dsp_fastfwd` (new; CMake, x86 ctest + NDK arm64 build run under arm64run) PASS on both. nv2a-x86 10/10,
xr_shell (`/tmp/claude-0/xrt`) 4/4. `build-game-android` rebuilt (libxr_apu.a, libblack_game.a), then `build-boot-a64`
rebuilt and ctest 3/3 pass. That ctest stops at the first GPU touch (~0.5 s), before the title starts the APU, so the
two 120 s runs with `BOOT_STOP_AT_GPU=0` above (fast-forward on / verify) are the ones that exercise the change. Both
ran to the time limit, no crash. `tests/apu_spatial`, which links the whole runtime, does not configure on this host
(no SDL2). The other APU suites extract apu_core.c / apu_xaudio2.c functions, which this change does not touch.

## 11. Pusher (`bx-pusher`) profile and optimisation pass (2026-11-11/12)

Nothing committed. Headset evidence: `bx-pusher` 75-98% of a big core, game 38-50 fps in busier level-1 scenes,
30-35k draws/s. The pusher thread is the pushbuffer decoder *and* the whole Vulkan backend (with `kvk` there is no
async GPU thread: `kgpu_async_active()` is false), so its time is decode + draw setup + recording + submit.

### 11.1 Tools (test-only)

- `grx_prof.c`: `GRX_PROF_PUSH=<file>` samples `bx-pusher` like `GRX_PROF_APU` (user-time task clock; the APU/pusher
  threads now share one table of named threads). `grx_prof.py` reads it unchanged.
- `build-push-x86` (configured like `build-perf-x86`, plus `-g` for line attribution; no codegen change).
  `prof/ab.sh <tag> [binary] [secs]`: the input script of 9.1 with the right trigger held in bursts (`215:rt:8, 240:rt:8,
  270:rt:10, 310:rt:10, 360:rt:10, 410:rt:8`), 60 Hz, 416x448 eyes, 500 us samples, `RECOMP_KGPU_PIPE_ASYNC=1`.
  `prof/tools/buckets.py` (exclusive cost buckets), `lines.py` (self samples by source line), `summary.py`
  (ms per level-1 flip, us per draw), `firstown.py` (driver/libc time by our calling function).
- Harness artifact found: `kelvin_vk` is built by its own CMakeLists without `__ANDROID__`, so pipelines were created
  synchronously on the pusher (0.6-1.0 ms per level-1 flip of SwiftShader JIT). On the Quest they are async; runs
  from b2 on set `RECOMP_KGPU_PIPE_ASYNC=1`, and the tables below leave pipeline creation out.
- New `KvkStats` counters (production, harmless; printed in the `[KVK] frames` line): `tset_writes`,
  `tset_flushes` (cached sets dropped because a view/sampler they name was released), `np_vertices` /
  `np_cache_hits` (classifier emulator runs / per-draw reuses), `tex_sig_bytes`.

### 11.2 Where the pusher's time went (x86, level 1, baseline runs b1/b2: 543/525 draws, ~160k words per flip)

User CPU per level-1 flip, b1 (b2 within 10%):

| # | Cost | ms/flip | share |
|---|---|---|---|
| 1 | **Texture descriptor sets** (`tset_get`: `vkAllocateDescriptorSets` + `vkUpdateDescriptorSets`): the cache was emptied at every `frame_begin` and is direct-mapped, so ~264 of ~464 draws per frame wrote a fresh set (0 flushes: all misses from the reset and collisions). SwiftShader charges ~23 us per write | 6.5 | 47% |
| 2 | `draw_now` itself: vertex-range copies into the ring, u32->u16 index conversion, the `tex_get` probe (texture addresses are 4 KiB aligned, so the home slot depended only on format/size), surface scans, combiner constants | 1.75 | 13% |
| 3 | The pusher's word loop (`kelvin_thread` self). ~35% of it is the **idle spin** (kelvin.c 830-838: 3000 `pause` + `sched_yield`) | 1.4 | 10% |
| 4 | Texture content signature (`tex_signature`: every used texture re-hashed once per kick; ~3 MB/frame) | 0.70 | 5% |
| 5 | Method handlers (`kelvin_pgraph_method` and below). Histogram (`RECOMP_KELVIN_METHODSTAT`): `INLINE_ARRAY` 0x1818 and `ARRAY_ELEMENT16` 0x1800 are ~95% of words and already batched; constants (0xB80..) ~2% | 0.66 | 5% |
| 6 | Kelvin draw setup (`kpix_setup`, `ktex_setup`, `kvsh_begin_draw`, `gpu_draw_gvs`) | 0.59 | 4% |
| 7 | Command binds / dynamic state (incl. one `vkCmdBindIndexBuffer` per draw: each draw's indices had their own offset) | 0.50 | 4% |
| 8 | **VR classifier** (`screen_effect` -> `np_extent`, renderer.md section 11): per evaluated vertex a 4.3 KB `memset` + 3 KB constants `memcpy` + 16 input fetches. ~1,400-2,500 vertex evaluations per frame (20-100 sprite candidates) | 0.48 (1.0-1.16 while firing) | 3-7% |
| 9 | Flip/submit/frame begin; constants upload (`upload_dedup`); surfaces/passes | 0.32 / 0.26 / 0.10 | |

Per draw: **24.6 us (b1), 23.7 us (b2)**, of which ~12 us is SwiftShader's descriptor writes. The headset's driver
costs differ (Adreno writes a descriptor set for ~1-3 us), so only the non-driver part transfers directly.

### 11.3 Changes (`XR/src/nv2a/kelvin/vk/kvk_backend.c` unless noted; all platforms that build kvk; Windows builds d3d11)

| # | Change | Exactness |
|---|---|---|
| 1 | **Persistent texture descriptor sets.** 1024 x 4-way cache (LRU per bucket) of sets from one `FREE_DESCRIPTOR_SET` pool (8192 sets), kept across frames. A set is dropped when a view or sampler it names is released (`tset_forget`, called from `img_release`, the sampler eviction and the two eye-view destroy sites incl. `kvk_vr_layers.inc`), or on bucket eviction; dropped sets are freed through the dead list when the frame retires (`DEAD_TSET`). Pool full or fragmented: the old per-frame pools (`tset_alloc_frame`). `frame_begin` / `tex_free` / sampler eviction no longer clear the whole cache | Sets are immutable once written and keyed by the exact 4 views + 4 samplers; a handle is purged before its deferred destruction can let the driver reuse it. `RECOMP_KVK_TSET_VERIFY=1` checks on every hit that each handle is still owned by a live surface/texture/eye view/sampler |
| 2 | **Classifier fast path** (`np_eval`): for a program with no constant writes and no relative or out-of-range constant reads (scanned per draw), the emulator reads the draw's constants in place; per vertex only outputs/temps/a0 are zeroed and only the inputs the program reads are fetched; an element's result is reused within the draw (64-entry memo). Anything else takes the old code (`np_pos_ref`). Same inputs, same operations in the same order, so the same oPos bits | `RECOMP_KVK_NP_VERIFY=1` runs the reference for every fast evaluation and logs mismatches |
| 3 | Index buffer bound once per chunk/type/pass at offset 0; each draw passes `firstIndex = ib_off / index size` (`ib_off` is 4-aligned). `draw_tint` takes the same `firstIndex` | Same index bytes fetched |
| 4 | `surf_get_ex`: the last colour/depth result is returned when it matches all fields (at most one used entry per va/depth/hud, so the scan would return it); `vkGetPhysicalDeviceFormatProperties` only for a new surface. `surf_for_texture`/`surf_find`: a 256-bit filter of used surface addresses (rebuilt when `surf_gen` changes) skips both 64-entry scans for ordinary textures | No false negatives; same entry returned |
| 5 | `tex_get`'s slot hash is mixed before the modulo; restrict-qualified index conversion | Where an entry lives does not change what a lookup returns |

Pusher word loop, PUT/GET publication and their memory ordering: untouched. No shader or pipeline-state change, so
the shipped `kgpu-cache` needs no pruning.

### 11.4 Results (x86, same script; level-1 flips from the first stereo frame; pipeline creation left out)

| Run | Binary | draws/flip | pusher ms/flip (median / p95) | us/draw |
|---|---|---|---|---|
| b1 | before (sync pipelines) | 543 | 13.34 (13.5 / 21.0) | 24.6 |
| b2 | before | 525 | 12.46 (12.5 / 19.0) | 23.7 |
| a1 | changes 1-3 | 657 | 7.46 (7.0 / 11.0) | 11.4 |
| a2 | 1-4 | 531 | 5.91 (6.0 / 10.0) | 11.1 |
| a3 | 1-5 (final but log text) | 621 | 6.94 (6.5 / 10.5) | 11.2 |

- **Per draw -54% on x86** (24 -> 11 us). Descriptor writes: ~264 per frame -> 972 in a whole run (~0 per level-1
  frame); `tset_get` 6.2-6.5 -> 0.13-0.15 ms/flip.
- Classifier: in a3's last 131 flips (102 sprite draws, 7,764 vertex lookups per frame, 33% reused) it cost
  0.57 ms/flip; at the old ~0.46 us per vertex the same work is ~3.6 ms/flip. About 0.11 us per evaluated vertex
  (~4x), ~6x per lookup.
- Non-driver per-draw time: 12.0 us (b2) -> 10.9-11.0 us (a2/a3), from 2, 4 and 5.
- What remains (a3, ms/flip): `draw_now` copies/conversion 1.70, word loop 1.47 (idle spin ~35% of it), method
  handlers 0.76, texture signature 0.72, Kelvin draw setup 0.67, binds 0.54, classifier 0.27, constants 0.27,
  flip/submit 0.27, descriptor sets 0.14.

### 11.5 Equivalence

- **Verify run v1** (`RECOMP_KVK_NP_VERIFY=1 RECOMP_KVK_TSET_VERIFY=1`, 420 s, menus + level 1 with firing):
  **655,360 classifier vertices checked against the reference, 0 mismatches; 311,296 cache hits checked, 0 stale handles.**
- tests/kelvin_vk (`/root/build/kvk-tests`, pixel comparisons) 5/5; tests/vr_glue 2/2: 386 + 390 checks, including every
  sprite / glow / light-shaft / full-screen / glare-target classification check of renderer.md 11, also with both
  verify modes on.
- Harness captures: frame PNGs that do not depend on game timing are byte-identical before/after (first flat frame,
  `0005_before-a3`, the first stereo HUD layer); the others differ between any two runs of the same binary (time-driven
  input and animation), so they are not a test.
- All runs (a1-a3, v1, f1) reach level 1 in stereo (multiview), fire, no `[CRASH]`.

### 11.6 Quest 3 estimate (unmeasured)

- x86 has about the A78's per-clock speed for this code, so the non-driver savings carry over: ~1 us per draw plus the
  classifier, **~0.7 ms/frame at 700 draws, 1-3 ms/frame in effect-heavy scenes** (explosions, smoke, firing).
- Driver savings depend on Adreno: ~260-400 descriptor allocate+update pairs per frame (~1-3 us each: **0.3-1.2 ms**) and
  one `vkCmdBindIndexBuffer` per draw (**~0.2-0.4 ms** at 700 draws).
- Together **~1.5-4.5 ms of a ~16-20 ms pusher frame**, more in combat: roughly 10-25% less pusher time. Whether that
  holds 60 fps depends on how much of the measured 75-98% is work.
- **The HEARTBEAT % overstates the work.** When the pusher has nothing to do (PUT == GET, or a flip stall waiting
  for vblank) it spins: 3000 `pause`/`yield` then `sched_yield`, again and again, so an idle pusher reads ~100% of a core.
  On x86 the spin is ~0.3-0.5 ms/flip of user time even though the pusher mostly blocks in SwiftShader's fence waits. On the headset,
  where the GPU is fast, the spin is likely a large part of the 75-98%.
- **First thing to run on the device:** `env RECOMP_KELVIN_TIMING=1` in blackvr.ini. `[KELVIN] ms per flip` then
  splits "pusher" (inside `pusher_run`, fence waits included) from "no words: for PUT / flip / ack" (idle-spin wall
  time). simpleperf on `bx-pusher` shows the driver share.

### 11.7 Not done / follow-ups

- **The idle spin.** A futex wake from the PUT trap (kelvin_mmio.c) instead of `pause`/`sched_yield` would free the
  core and make the CPU numbers mean work. It changes pusher wake-up latency and touches the PUT/GET path, so it was
  left for a decision.
- Vertex data is copied into the ring for every draw (~1 ms/flip). A cross-frame vertex-buffer cache would need a
  content hash about as costly as the copy, but would save write bandwidth to uncached memory on the Quest.
- Texture signatures are recomputed once per kick (~3 MB/frame). Doing it less often needs proof that guest texture
  memory changes only at flips, which we do not have.
- Constants: `upload_dedup` compares 3 KB per draw (0.27 ms/flip). A version counter set by every constants writer
  would remove that compare, but every writer has to be found.
- Moving recording to a second thread: not needed for these gains. Ordering (GET publication behind the draws) makes
  it a larger change.
