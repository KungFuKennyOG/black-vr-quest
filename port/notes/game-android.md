# game-android: BLACK itself on Quest 3 (aarch64 Android) and the final link

Stream owner: game stream. Nothing committed. Paths are relative to /root/black; `XR` = `third_party/xboxrecomp`.

## Results at a glance

| What | Status |
|---|---|
| `scripts/generate.py` with the new lifter | regenerated in 3m21s. **277 `RECOMP_CVT[TR]_F2I32` sites**; old vs new gen differs **only** on those 277 lines (chunks 2 and 3 unchanged). `gen/recomp_types.h` is now identical to `XR/templates/runtime/recomp_types.h`. `src/black_xmv_idct.inc` unchanged |
| Generated code, arm64 (NDK r27c, `-O2 -march=armv8.2-a -ffp-contract=off`, sse2neon) | 16/16 TUs compile, 0 warnings, 0 errors (~3.5 min on 2 cores) |
| Generated code, x86-64 (clang) | 16/16 TUs pass `-fsyntax-only`; chunk 0012 at `-O2` gives `cvttss2si` for the helpers |
| `main.c`, `recomp_manual.c` (+ all `.inc`, `black_vr.inc`) for arm64 | compile, 0 warnings with the game's flag set |
| Windows equivalence | clang `--target=x86_64-w64-mingw32 -E` (mingw-w64 headers) of `main.c` and `recomp_manual.c` (which pulls in every `.inc`), with and without `-DBLACK_VR`, and of `pc_input.c`, `pc_controls.c`, `kernel_path.c`, `kernel_file.c`, `kernel_bridge.c`: **identical token streams** before and after my edits (blank lines differ; no `__LINE__` user is affected). The only `-DBLACK_VR` difference is the requested `black_vr.inc` change (projection publish, world-mark rule) |
| Link check: game + runtime + nv2a/usb/video + Vulkan backend, `--no-undefined` | only the shell's `vr_*` remain (9 symbols), which `port/quest/src/vr_bridge.c` defines |
| **Final link** `libblackvr.so` (shell + game + runtime + NV2A/Kelvin-Vulkan + USB + video + input + glue + DSP56300) | **links with `-Wl,--no-undefined`**: 139 MB unstripped, 26.2 MB stripped |
| **APK** | `build-quest-game/blackvr.apk`, **27.1 MB (26 MB)**, v2/v3 signed (debug key), `zipalign -p 16` OK |
| arm64 boot test (`tests/game_boot_arm64`, QEMU) | boots through CRT/XAPI init, D3D device creation, loads the frontend assets, submits its first pushbuffer (**first GPU touch at ~0.3 s**). With the stop disabled it runs **5 minutes with no crash** (88k indirect calls), parked in a vblank DPC/event loop waiting on the stubbed GPU |

## 1. Build

```sh
. /root/xtool/env.sh
# The game + every runtime library as archives (port/android)
cmake -G Ninja -S port/android -B build-game-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_TOOLCHAIN_FILE \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DBLACK_ANDROID_GAME=ON
cmake --build build-game-android
# The APK (the shell includes port/android/game.cmake, which adds port/android as a subdirectory)
BUILD_DIR=$PWD/build-quest-game port/quest/build_apk.sh --with-game
```
`BUILD_DIR` keeps the game build apart from the shell's smoke build in `build-quest/`. The APK holds code generated
from the user's own disc: **keep it local, do not distribute it.**

### port/android/game.cmake

There are two ways in:
- From `port/android/CMakeLists.txt` with `-DBLACK_ANDROID_GAME=ON`.
- From `port/quest/CMakeLists.txt` as `BLACKVR_GAME_CMAKE`. Here `xr_kernel` does not exist yet, so the file
  `add_subdirectory`s `port/android` with `BLACK_ANDROID_GAME=ON` and returns. The targets are global, so the
  shell sees them.

It defines:
- **`black_gen`** (OBJECT): `src/recomp/gen/*.c`. `templates/runtime` comes before `gen/`; flags are `-O2 -w`.
- **`black_game`** (STATIC): `black_gen`, plus `src/main.c` and `src/recomp_manual.c`. Built with `-DBLACK_VR`,
  `BLACK_VERSION="<VERSION>-quest"`, and the include dirs `port/quest`, `port/quest/src` and
  `port/android/include`.
- **`xr_input`** (STATIC): `xinput_device.c` (the Quest pad backend, `-DBLACK_VR`), plus `pc_input.c` and
  `pc_controls.c` (the real ones; the keyboard is simply never fed). There is no SDL2.
- **`xboxrecomp`** (INTERFACE): `xr_kernel`, `xr_apu`, `xr_dsound`, `xr_input` and `xr_platform`, plus
  `xr_nv2a`, `xr_usb` and `xr_video` from `nv2a.cmake`. `BLACK_NV2A_TARGETS` overrides that list.

Every TU gets these flags:
`-isystem port/android/x86shim -I/root/deps/sse2neon -DSSE2NEON_PRECISE_MINMAX=1 -march=armv8.2-a -ffp-contract=off
-fno-strict-aliasing -fwrapv`. There is no `-fms-volatile`.

`port/android/CMakeLists.txt` includes `nv2a.cmake` (OPTIONAL) and then `game.cmake`. The old `xr_game` target is
gone. Nothing uses the kernel's `find_package(OpenSSL)` or `third_party/xboxrecomp`'s own CMake. This supersedes
`port/quest/cmake/game_targets.cmake`.

`black_vk_renderer` is **not** defined here: `port/quest/CMakeLists.txt` builds it from `vr_renderer_kvk.c` once
`kelvin_vk_shadergen` exists, and links it whole-archive.

CMake prints one dev warning: WHOLE_ARCHIVE is not applied to the INTERFACE target `xboxrecomp`. This is harmless.
Those libraries link as normal archives (lld needs no link groups), and only `black_game` (for the weak
`black_game_main`) and the glue need whole-archive.

## 2. Source changes (all `__ANDROID__`, so Windows is token-identical)

| File | Change |
|---|---|
| `src/black_android.h` (new) | Android-only stand-in for `<windows.h>`: `win32_android.h`, plus `__declspec(thread)` → `_Thread_local` (this covers the generated IDCT `extern __declspec(thread) g_ebp/g_seh_ebp`, which agrees with `RECOMP_TLS` = `__thread`) and `__forceinline` |
| `port/android/include/win32_android.h` (new) | Win32 calls missing from `win32_compat`: `CreateDirectoryA`, `DeleteFileA`, `MoveFileExA` (rename + directory fsync for `WRITE_THROUGH`; Win32 error codes), `InterlockedOr/And`, and a `MapVirtualKeyW` stub. Each is a macro over a `black_w32_*` function, so it cannot clash if `win32_compat.h` gains the same names later. The video layer could use it too |
| `src/main.c` (CRLF kept) | See 2.1 |
| `src/recomp_manual.c` | `<windows.h>` → `black_android.h`. Android drops the `xbox_translate_path(…, WCHAR*)` redeclaration (`kernel.h` has the char form). `g_black_main_tid` is a weak definition (it lived in the D3D11 `kelvin_gpu.c`, which the Vulkan build drops) |
| `src/black_hud_input.inc` (CRLF) | 3 `__try/__finally` sites (there is no `__except` in any of them) → a `cleanup`-attribute scope (`BLACK_HINT_TRY`/`_END`) that runs the same finally code on every exit, `return` included. A fault inside still ends the process, as on Windows, where no frame handler exists. The `VirtualQuery`/`PAGE_GUARD` TIB probe becomes an explicit range check against the mapped primary RAM |
| `src/black_native_menu.inc` | `bnm_read_game_file`: `xbox_host_char` + `CreateFileA` |
| `src/black_vr.inc` | The glue agent's requests: the world-pass mark is now in every VR mode (`bvr.mode < 0` stands down). The exact learned projection (p00, p02, ±p11, p12, depth row) is published once the conventions lock, with a one-time log if 1/p00 differs from `half_fov_x` by more than 1% |
| `tests/xr_shell/test_hooks.c` | The world-mark expectation now covers flat modes and VR off. 4/4 suites pass; `tests/vr_glue` passes 2/2 |
| `XR/src/input/pc_input.c`, `pc_controls.c` | One `#elif defined(__ANDROID__)` include each |
| `XR/src/kernel/kernel_path.c` (POSIX half) | Ported from the Windows half: the save layout (`TitleData`/`UserData`/`Cache`/`SystemData`), `Partition0.img` with the retail partition table, sparse `Partition1-5.img`, and bare `\Device\Harddisk0\PartitionN` → image. **Without it BLACK boots to the dashboard** (`XLD_ERROR_INVALID_HARD_DISK`) on any POSIX host |
| `XR/src/kernel/kernel_file.c` (POSIX half) | Ported from Windows: a directory open of a device image uses its containing directory. BLACK opens `Partition1` with `FILE_DIRECTORY_FILE` at startup; on POSIX that was `ENOTDIR` → boot to dashboard |
| `XR/src/kernel/kernel_bridge.c` | `bridge_resolve_handle`, `!_WIN32` only: `NtCurrentProcess/Thread` tokens (0xFFFFFFFF/0xFFFFFFFE) are sign-extended. They were zero-extended, so `DuplicateHandle` treated 0xFFFFFFFE as an object and did an atomic increment at 0x100000002: **SIGBUS on arm64** (SIGSEGV on x86-64 Linux) a few seconds into boot |

### 2.1 main.c on Android

- **Entry point.** `black_game_main(argc, argv)` does three things before calling `WinMain(NULL, NULL, "", 0)`:
  - redirects stdout/stderr to logcat (tag `BlackGame`)
  - resolves the paths
  - applies the env defaults
- **Paths.**
  - cwd is the shell's game root (`argv[1]` overrides it).
  - `game_dir = <cwd>/game`, `save_dir = <cwd>/save`, then `xbox_path_init(game_dir, save_dir)`.
  - `default.xbe` is read from `<game_dir>/default.xbe`.
  - `BLACKVR_GAME_DIR` and `BLACKVR_SAVE_DIR` override the dirs.
  - I chose `<cwd>/save` and not `<filesdir>/save`, because `black_checkpoint.inc` writes the literal relative
    `save/checkpoints`; this way checkpoints and kernel saves land in the same place. When the game root is the app's
    external files dir, the two choices are the same.
- **Env defaults** (the launcher's `gameEnvironment()`, `setenv` without overwrite, so `blackvr.ini` env lines win):
  - `RECOMP_AC97_READY=1`, `RECOMP_APU_DSP_ACK=0x83218810`
  - `RECOMP_NV2A_KELVIN=1`, `RECOMP_KELVIN_GPU=1`, `RECOMP_KELVIN_THREADS=3`
  - `RECOMP_VBLANK=1`, `RECOMP_VBLANK_KEEP_GRID=1`, `RECOMP_USB=1`, `RECOMP_FB_WINDOW=1`
  - `RECOMP_LANGUAGE=en-US`, `RECOMP_BLACK_60HZ=1`
  - `RECOMP_BLACK_SKIP_STARTUP_VIDEOS=1` and `RECOMP_BLACK_SKIP_MOVIES=1`: there is no host FMV player, and the
    disc dump has no `videos/`
  - `RECOMP_VIDEO_INI` and `RECOMP_CONTROLS_INI` in `save/`; `RECOMP_KGPU_CACHE=<root>/kgpu-cache`
- **Faults** (`port/notes/arm64-fault.md` order):
  - At the top of WinMain: `a64_fault_install()` and `a64_fault_add_handler(crash reporter)`. It is added first,
    so it runs **after** every device handler: AC97, APU, OHCI, Kelvin, the memory watch.
  - After `xbox_MemoryLayoutInit`: `a64_fault_add_handler(apu_fault)`, the same range check as `veh_handler`.
  - NV2A: as on Windows, `main.c` handles nothing; Kelvin and OHCI register their own handlers.
  - The crash reporter prints `veh_handler`'s report: guest registers, `dladdr` names (the `sub_` functions are
    exported from the .so), the guest stack and objects, the native pc/lr/sp, and return addresses into the .so. It
    then drains the log pipe and **declines**, so debuggerd still writes the tombstone.
  - SIGBUS, SIGILL, SIGTRAP, SIGFPE and SIGABRT get the same report, then the previous handler is restored.
  - `RECOMP_FPE_TRAP` is Windows-only and is logged and ignored.
- `MessageBoxA` → `__android_log_print(ERROR)`. There is no dbghelp.

## 3. arm64 boot test: tests/game_boot_arm64

```sh
cmake -G Ninja -S tests/game_boot_arm64 -B build-boot-a64 -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake \
      -DBLACK_GAME_LIB_DIR=$PWD/build-game-android/lib
cmake --build build-boot-a64 && ctest --test-dir build-boot-a64 --output-on-failure
# longer: BOOT_STOP_AT_GPU=0 BOOT_SECONDS=300 /root/xtool/arm64run build-boot-a64/game_boot_arm64 /root/black /tmp/x
```

- The test links the **same archives that go into `libblackvr.so`** (`black_game`, `xr_*`, DSP56300) as a static
  exe, and runs `black_game_main` on a 16 MB thread.
- `boot_stubs.c` stubs:
  - Kelvin: it reports itself inactive, so the runtime's NV2A ack thread stands in
  - USB, `pc_*` video, `vr_*` (VR off)
  - liblog and AAudio (no device)
- The disc is read **in place**: `<scratch>/root/game/*` are symlinks to the user's `game/*` entries.
  - The title writes `E:\UDATA` and `E:\TDATA` (E: maps to the game dir, as on Windows), the kernel log and
    `save/`; all of it lands in the scratch tree.
  - My first run, before this, created `game/UDATA`, `game/TDATA` and `/root/black/xbox_kernel.log`. I removed them.
- It stops at the first GPU touch (pushbuffer PUT moves, or a stubbed GPU entry is called) or at `BOOT_SECONDS`,
  and prints the indirect-call count, the last 16 icall targets and the NV2A PUT/GET.
- The ctest passes on "STOP: first GPU touch|time limit" and fails on `[CRASH]`.

**What the run showed:**
- The memory layout comes up. The arm64 apertures, 28 memfd mirrors, the tiled alias and the APU/AC97 traps are
  all fine.
- The DSP56300 initialises, and AAudio is refused (no device).
- The kernel bridge resolves 115/115.
- CRT/XAPI start, the partition table is read, and the 60 Hz step is set.
- Assets load: `Core.bin`, the fonts, `FEAudio.awd`, `BkXboxFx.bin`, `Andy.aku` and others.
- `KeConnectInterrupt` hooks the GPU vector, vblank ISRs run, and pushbuffers are submitted.
- **First GPU touch at about 0.3 s.** With the stop off, the run lasts 5 min with no fault, at ~170 kernel
  calls/s (KeInsertQueueDpc / KeSetEvent: the vblank DPC); the title waits on the GPU, which is stubbed here.
- Three host bugs blocked the boot, all POSIX-runtime and not arm64-specific; they are fixed (table above).

## 4. Device risks (first things to watch in `adb logcat -s BlackGame BlackVR`)

1. **Nothing has run on a Quest.** The game, Kelvin/Vulkan and the shell have only met at link time. The first
   real GPU frame, the pushbuffer and the vblank interplay are new territory past the boot test's stopping point.
2. **Partition images on /sdcard.** `save/Partition1-5.img` are sparse files of 2.3 GB plus 0.5 GB plus 3×0.75 GB,
   created with `ftruncate`. FUSE on Android 11+ (/sdcard/Android/data) should keep them sparse; if it does not,
   the first boot writes ~5 GB or fails with ENOSPC. Check with `du` on the device. A fix would be to create only
   `Partition0.img` and map 1-5 lazily. **Fixed**: the images are sparse by construction now (perf.md 4.1).
3. **libsigchain/ART and the SIGSEGV dispatcher.** The APU, AC97 and OHCI traps and Kelvin's register traps must
   reach `a64_segv`, and unclaimed faults must still give a tombstone. This is unverified in an app process.
4. **memfd + `MAP_SHARED` under Quest SELinux, the 4 GB `PROT_NONE` reservation, and RSS.** The game reserves 4 GB
   of address space and maps 64 MB of RAM plus 28 mirrors, plus 64 MB contiguous. An LMK kill would look like a
   silent exit.
5. **Performance.** TLS register access is TLSDESC in a .so (see arm64-runtime.md 7.3), and the 64 MB of
   generated C runs at `-O2`. The 60 Hz step is forced (`RECOMP_BLACK_60HZ`) and may need to drop to 30 if the
   game thread cannot keep up.
6. **Movies are skipped by default.** If the title waits on an XMV that is missing (the dump has no `videos/`), set
   `RECOMP_BLACK_SKIP_MOVIES=0` in `blackvr.ini` to compare.
   **Skipping in VR (2026-11-14).** Two ways, both in `sub_000C4570` (`src/recomp_manual.c`, `BLACK_VR`):
   - VR SETTINGS > Comfort "Skip cutscenes" (`skip_movies`): every movie ends at its first frame.
   - Any controller button (A/B/X/Y/Menu/either trigger) pressed while a movie plays ends it, whether the game lets
     you skip that one or not. One press, no confirmation. `vr_button_presses()` counts fresh presses on the input
     thread; the count is taken when the movie's session starts, so a press before the movie doesn't count. Log:
     `[MOVIE] skipped (player)`. The in-engine scenes (FLAT_CINEMA) are not movies and still play.
7. **The `level_06` files in the dump are truncated.** That level will fail to load; this is not a port bug.
8. **The game's own aiming dot.** The D3D11 backend suppresses it in `kelvin_gpu_draw.inc:492`
   (`pc_crosshair_replaces_dot`). The Vulkan backend does not, so in hand-aim stereo the dot sits at the centre of
   the head-locked HUD. Fixing that is a renderer-side change, mirroring that check for stereo; it is not cheap
   game-side, so it is not done here.
9. **`xbox_kernel.log`** is written to the working directory (the game root), as on Windows.
