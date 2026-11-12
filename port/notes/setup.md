# setup: rebuilding the dev/test environment in a new cloud workspace

`scripts/quest_setup.sh` turns an empty Ubuntu 24.04 x86_64 workspace into the environment the rest of
`port/notes/` assumes: the Android NDK/SDK in `/root/xtool`, pinned sources and their builds in `/root/deps`,
SwiftShader and the test-suite build dirs in `/root/build`, and the repo-local pieces (`.venv`, `build-dsp`).
Then it builds and runs the test suites. With game files, it also generates, builds, boots and packages the game.

```sh
git clone https://github.com/KungFuKennyOG/black-vr-quest.git /root/black   # private; the session's GitHub access
cd /root/black
scripts/quest_setup.sh                                  # toolchains, deps, test suites (no game)
scripts/quest_setup.sh --game /path/to/BLACK.iso        # ... plus generate + game build + boot test + APK
scripts/quest_setup.sh --list                           # the steps
scripts/quest_setup.sh --dry-run                        # what would run; changes nothing
. /root/xtool/env.sh                                    # afterwards, as every note assumes
```

Re-running is safe. Each step first runs a cheap check: pinned commit, tool version, output file, or a Vulkan
instance on SwiftShader. If the check passes, the step is skipped. So after a failure (network, disk) you fix
the cause and run the same command again. Logs go to `/root/.quest_setup/logs/<step>.log`. The run ends with a
summary table: one line per step with ok / done / skipped / FAILED and what was verified.

## Options

| Flag | Effect |
|---|---|
| `--deps-only` | toolchains and dependencies only; no test suites and no game |
| `--game <dir\|iso\|url>` | the user's own files: a folder holding `default.xbe`, an `.iso`/`.xiso` (extracted with extract-xiso), or an http(s) link to either (a `.zip` of the folder works too) |
| `--copy-game` | copy a game folder into `game/` (the default is a symlink) |
| `--regenerate` | run `scripts/generate.py` again even if `src/recomp/gen` exists |
| `--with-render-x86` | with game files, also configure and build `tests/game_render_x86` (it is not run: a run takes 40 min) |
| `--keystore <file>` | the APK signing key to install as `/root/xtool/smoke/dbg.jks` (see *Signing key*) |
| `--skip-swiftshader` | skip the slowest piece. The Vulkan tests then exit 77 and count as skipped |
| `--skip-vvl` | skip the validation layer and its SPIRV-Headers/SPIRV-Tools/Vulkan-Utility-Libraries checkouts |
| `--no-apt` | report missing Ubuntu packages and do not install them |
| `--jobs N` | build parallelism (default `nproc`) |
| `--prefix DIR` | root for `deps/ xtool/ build/` (default `/root`). Keep the default: several repo CMake files hard-code `/root/deps/sse2neon`, `/root/xtool/android-arm64-static.cmake`, `/root/xtool/arm64run`. The toolchain and dependency steps honour the prefix; the test and game builds may still reach into `/root` |
| `--android-cache DIR` | take the NDK/SDK zips from DIR instead of downloading them |
| `--only a,b` / `--skip a,b` / `--force a,b` | step selection. `--force` re-runs a step even when its check passes |

## What it does

| Step | Result | Verified by |
|---|---|---|
| `preflight` | host, disk and network report | x86_64 Linux, free GB, each allowlisted host answers |
| `apt` | Ubuntu packages (below). Only missing ones are installed | `dpkg-query`; cmake, gcc and clang versions |
| `repo` | `git submodule update --init` for `third_party/dsp56300` (and `third_party/xboxrecomp` where it is still a submodule; the GitHub mirror vendors it). The `launcher/vendor/*` submodules are not needed and are not fetched | marker files |
| `android` | `android-ndk-r27c-linux.zip`, `build-tools_r34-linux.zip` (unzips to `sdk/android-14`), `platform-34-ext7_r03.zip` (`sdk/android-34`) from `dl.google.com/android/repository`, sha1 pinned from `repository2-1.xml`. Writes `env.sh`, `android-arm64-static.cmake` and `arm64run` | NDK clang, aapt2, apksigner and glslc versions |
| `keystore` | `/root/xtool/smoke/dbg.jks` | `keytool -list`; the cert's SHA-256 is printed |
| `smokeapk` | `/root/xtool/smoke/smoke.apk`: NativeActivity `.so`, then aapt2, zip, zipalign, apksigner | `apksigner verify` |
| `deps` | the pinned checkouts below. Adds the one-line `tests/fp` skip patch to QEMU, writes the `mingw-gen` headers and `/root/build/wincc.sh` | `HEAD` equals the pin; `<windows.h>` and `<d3d11.h>` compile with wincc |
| `qemu` | QEMU linux-user aarch64, static, into `/root/xtool/qemu/bin` (configure line from `qemu-b2/config.log`) | `qemu-aarch64 --version` reports 10.1.5 |
| `arm64selftest` | `/root/xtool/arm64-tests`: hello, neon, malloc, segv, io, cxx and exit-code tests (sources in `scripts/quest_setup/arm64-tests`) | ctest, 7/7, under QEMU |
| `openxr` | Khronos OpenXR loader for arm64-v8a/android-29 in `/root/deps/build-openxr-android`, configured the way `build_apk.sh` configures it | the `.so` is aarch64 |
| `xiso` | extract-xiso from source, into `/root/xtool/bin/extract-xiso` | binary present, at the pin |
| `python` | `<repo>/.venv` with `--system-site-packages`, capstone 5.0.7 built from `/root/deps/capstone-5.0.7/bindings/python`, and numpy/Pillow if the system Python lacks them | imports and versions |
| `rust` | rustup (if missing), the toolchain pinned by `third_party/dsp56300/rust-toolchain.toml` (1.98.1), target `aarch64-linux-android` | `rustc --version`, installed targets |
| `dsp` | `build-dsp/{aarch64-linux-android,x86_64-unknown-linux-gnu}/release/libdsp56300_emu_ffi.a` (`cargo build --release --locked --no-default-features`, NDK clang as linker) | both archives exist |
| `swiftshader` | `/root/build/swiftshader` (Subzero, `vk_swiftshader` target only; flags from `swiftshader_build.sh`) | `scripts/quest_setup/vkprobe.c` creates an instance through the system loader and lists *SwiftShader Device (Subzero)* |
| `vvl` | Vulkan-Headers, SPIRV-Headers, SPIRV-Tools, VUL and VVL installed into `/root/deps/vvl-prefix`, plus `layer.d/` with an absolute library path (validation-audio.md section 1) | vkprobe enables `VK_LAYER_KHRONOS_validation` explicitly on SwiftShader. A missing layer fails with `VK_ERROR_LAYER_NOT_PRESENT` |
| `t_xrshell` | `tests/xr_shell` → `/root/build/xrt` | ctest |
| `t_nv2ax86` | `tests/nv2a_arm64` with gcc (AVX2 refs, SSE, TSan) → `/root/build/nv2a-x86` | ctest |
| `t_nv2aa64` | the same suite on arm64 under QEMU, compared against the x86 refs → `/root/build/nv2a-a64` | ctest |
| `t_kvk` | `tests/kelvin_vk` native → `/root/build/kvk-tests` | ctest (the backend tests skip without SwiftShader) |
| `t_kvka64` | `tests/kelvin_vk` arm64 → `/root/build/kvk-android` | ctest under QEMU, plus the `-Wl,--no-undefined` link check |
| `t_vrglue` | `tests/vr_glue` → `/root/build/vr-glue` | ctest (skips without SwiftShader) |
| `t_apkshell` | `build_apk.sh` without the game → `/root/build/quest-shell/blackvr.apk` | `apksigner verify --print-certs` |
| `g_stage` | the user's files → `game/` (see below). An existing `game/default.xbe` is used as is and never touched | sha256 of `default.xbe` against `EXPECTED_XBE` in `generate.py` |
| `g_generate` | `.venv/bin/python scripts/generate.py` → `src/recomp/gen`, `src/black_xmv_idct.inc`, `.work/generate-*` (~30 s) | `recomp_dispatch.c` present |
| `g_android` | `port/android` with `BLACK_ANDROID_GAME=ON` → `build-game-android` (game-android.md section 1) | build |
| `g_boot` | `tests/game_boot_arm64` → `build-boot-a64`. Boots under QEMU until the first GPU touch | ctest (`STOP: first GPU touch`, no `[CRASH]`) |
| `g_apk` | `BUILD_DIR=build-quest-game port/quest/build_apk.sh --with-game` → `build-quest-game/blackvr.apk` | `apksigner verify --print-certs` (check that the cert is the expected key) |
| `g_render` | only with `--with-render-x86`: `tests/game_render_x86` → `build-render-x86`, configured as in game-render-x86.md section 2 | build. Run it with `GRX_SECONDS=2400 build-render-x86/run.sh`. That run also writes the shader cache that `build_apk.sh --with-game` ships as `assets/kgpu-cache`. Without the cache, the headset compiles shaders on first start |

Configure lines come from each build dir's `CMakeCache.txt`, the `*.cfg.log`/`*_build.sh` files in `/root/build`,
`qemu-b2/config.log` and the notes. The following old build dirs are **not** recreated. Configure them by hand the
same way if you need them:
- the early glslang experiments (`/root/build/glslang-*`): superseded, because the suites build glslang via `KVK_GLSLANG_DIR`
- `a64rt-check`, `nv2a64-*`, `perf-bench*`, `preflight-*`, `xr-shell-check`
- the extra `build-*-x86` copies of `game_render_x86`

### Pinned sources (`/root/deps`)

All are shallow (`--depth 1`) fetches of the exact commit. The sparse ones use cone mode and `--filter=blob:none`.

| Dir | Repo | Commit (ref) | Used for |
|---|---|---|---|
| OpenXR-SDK | KhronosGroup/OpenXR-SDK | 24a1f2b92d21 (release-1.1.65) | headers + Android loader |
| Vulkan-Headers | KhronosGroup/Vulkan-Headers | 9fbc2a782ee3 (v1.4.367) | Vulkan headers for the tests, VVL |
| glslang | KhronosGroup/glslang | b93de9f4a8d5 | runtime GLSL→SPIR-V (`KVK_GLSLANG_DIR`) |
| qemu | qemu/qemu | 29e39cbdfe84 (v10.1.5) | arm64 test runner |
| capstone-5.0.7 | capstone-engine/capstone | 52c66920fc7b (5.0.7) | generate.py disassembler |
| sse2neon | DLTcollab/sse2neon | 2272251cc3ad | x86 intrinsics on arm64 |
| mingw-w64 (sparse: mingw-w64-headers) | mingw-w64/mingw-w64 | ee9bd8764415 | wincc.sh Windows compile check |
| platform_bionic (sparse: libc/include, libc/kernel/{android/,}uapi) | aosp-mirror/platform_bionic | 731631f30009 | port/android/syntax_check.sh |
| android_frameworks_native (sparse) | LineageOS | 46018f54b424 | syntax_check.sh |
| android_libnativehelper (sparse) | LineageOS | 73d3c2e3ed10 | syntax_check.sh |
| android_system_logging (sparse: liblog/include) | LineageOS | 336e1707c825 | syntax_check.sh |
| SPIRV-Headers / SPIRV-Tools / Vulkan-Utility-Libraries / Vulkan-ValidationLayers | KhronosGroup | fef168c54ed2 / 28f99f74a2a9 / 271510bc50a1 / af7950f16745 (v1.4.367) | validation layer |
| swiftshader | google/swiftshader | cddb5c91b23f | Vulkan on the CPU |
| extract-xiso | XboxDev/extract-xiso | 3f5b62cfe68f (build-202609111233) | disc images |

`syntax_check.sh` also wants `/root/deps/aaudio_include`. It did not exist in the original workspace either, so it
is not reproduced.

### What is not built from source

These are the only prebuilt inputs:
- the Android NDK r27c and SDK packages from dl.google.com (sha1-checked)
- the Rust toolchain from static.rust-lang.org (rustup; there is no practical bootstrap from source)
- Ubuntu packages, through apt
- packages from PyPI: setuptools to build the capstone binding, and numpy/Pillow only if the system Python lacks them
- the crates.io crates pinned by the DSP's `Cargo.lock`

Ubuntu packages: `build-essential clang clang-18 lld lld-18 llvm llvm-18 cmake ninja-build git curl ca-certificates zip unzip
xz-utils file patch bison pkgconf libglib2.0-dev libpcre2-dev libffi-dev zlib1g-dev libmount-dev libpixman-1-dev libpng-dev
libvulkan1 python3 python3-venv python3-pip python3-dev openjdk-21-jdk-headless`. Optional: `ffmpeg` (make_video.py),
`gdb`, `valgrind`. `libvulkan1` is the system Vulkan loader (1.3.275) that every Vulkan test links. The JDK runs
apksigner and d8.

## Network

Only package registries, GitHub and dl.google.com are reachable, through the agent proxy (`$HTTPS_PROXY`).
The script handles two quirks:
- **NO_PROXY.** The session's `NO_PROXY` lists `pypi.org`, `files.pythonhosted.org` and `index.crates.io`, so pip
  and cargo try to connect directly and fail. The script removes those hosts from `NO_PROXY`/`no_proxy` for its
  own child processes (dsp-arm64.md has the same note).
- **apt over http.** The stock Ubuntu sources are `http://`, which the proxy does not carry (405, or DNS failure).
  When `apt-get update` fails, the script writes an `https://` copy of the sources to `/root/.quest_setup/apt` and
  runs apt with `Acquire::https::Proxy=$HTTPS_PROXY`. `/etc/apt` is not changed.

`preflight` probes every host and reports which ones answer. Each step probes the hosts it needs again when a fetch
fails, and prints the proxy's recent failures. A blocked host is reported as `BLOCKED: <url>`. Do not retry around
it: ask for the host to be allowed. gitlab.com was blocked when QEMU was first built, which is why the
`tests/fp` patch exists. The patch is harmless now.

## Timings and disk (2 cores)

The numbers come from the original build logs, plus the verification run below. That run shared the 2 cores
with other agents' game runs (load average about 10), so its times are an upper bound: an idle host is 2-4x faster.

| Piece | Time | Disk |
|---|---|---|
| apt (fresh image) | 2-5 min | ~1.5 GB |
| NDK + SDK download and unzip | 2-4 min (SDK zips alone: 15 s) | 2.3 GB (+0.7 GB zip, deleted after the unzip) |
| pinned checkouts without SwiftShader | ~1 min (measured 63 s, VVL pins included) | ~0.6 GB (+0.15 GB with VVL) |
| QEMU | ~3 min idle (measured 10.7 min under load) | 0.15 GB build + 8 MB binary |
| OpenXR loader, extract-xiso, smoke APK, arm64 self-tests | ~2 min (loader 97 s, the rest seconds) | small |
| capstone (pip build) | ~2 min (measured) | 25 MB |
| Rust toolchain + target | 1-2 min | ~1.4 GB (`~/.rustup`) |
| DSP56300, both targets | ~8-12 min | 0.7 GB + 0.25 GB `~/.cargo` |
| SwiftShader checkout + build | 3 + ~8 min | 1.4 GB + 75 MB |
| VVL (SPIRV-Tools, VUL, VVL) | ~12-15 min | 0.35 GB |
| xr_shell + nv2a-x86 + nv2a-a64 | 34 s + 3.9 min + 1.8 min (measured under load) | 40 MB |
| kvk-tests, kvk-android, vr-glue (each compiles glslang) | ~15-20 min | ~0.95 GB |
| game: generate, game-android, boot test, APK | 0.5 + ~15 + ~3 + ~3 min | ~2 GB (`.work` 0.65 GB, `build-game-android` 0.45 GB, `build-quest-game` 0.7 GB) |
| `--with-render-x86` (build only) | ~10 min | ~1 GB, and up to 4.5 GB once runs write frames |

A full fresh run is **about 1.5-2 hours on 2 cores**, and needs **about 15 GB free** (20 GB with the game and
render-x86), not counting the game files themselves (1.2 GB extracted, plus the image).

## Game files in a new session

The game is the user's own disc. It is never committed, and `/game/` is git-ignored. The generated code
(`src/recomp/gen`, `src/black_xmv_idct.inc`, `.work/`) and every `--with-game` APK are made from the disc too:
keep them local, never push or share them.

To provide the files, the user does one of these:
1. **Attaches the disc image (.iso/.xiso) to the session.** Then: `scripts/quest_setup.sh --game <path to the attached file>`.
   extract-xiso unpacks it into `game/`.
2. **Shares a download link.** Then: `scripts/quest_setup.sh --game 'https://...'`. The file is downloaded to
   `/root/.quest_setup/game-stage`, unzipped if it is a zip, extracted if it is an image, and the staging copy is
   deleted. The host must be reachable from the workspace. If it is not, the script says so: attach the file instead.
   A link the user shares is private, so do not copy it into notes, commits or logs that leave the workspace.
3. **Has an extracted folder somewhere.** Then: `--game /path/to/folder`. `game/` becomes a symlink to it, or a copy
   with `--copy-game`, and `/game` is added to `.git/info/exclude`, because the `/game/` ignore rule matches only
   a directory.

`default.xbe` is checked against `EXPECTED_XBE` in `scripts/generate.py` (the supported retail release). If
`game/default.xbe` already exists, the script uses it as is and never writes into `game/`.

The headset needs the game files too (README *Game files*: `/sdcard/BlackVR/game` or the app's files dir).
They are installed separately, with `adb push` or `mac_install.sh`, and are not part of the APK.

## Signing key

`build_apk.sh` signs with `$BLACKVR_KEYSTORE` (default `/root/xtool/smoke/dbg.jks`), `$BLACKVR_KEYSTORE_PASS`
(default `android`) and `$BLACKVR_KEY_ALIAS` (optional; the key is `dbg`). It already honours these, so nothing
needs to change there.

Android only installs an update over an app signed with the **same** key. An APK signed with a new debug key
therefore fails to install over the BLACK VR already on the headset (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`). You
would have to `adb uninstall com.kenny.blackvr` first. That removes the app's own data and its
`Android/data/com.kenny.blackvr/files` folder. Game files in `/sdcard/BlackVR` (or `/sdcard/BLACK`) survive the
uninstall. Keep the original key:
- The user's GitHub repo carries it at **`port/quest/keys/dbg.jks`** (store password `android`, README alongside).
  The `keystore` step copies it to `/root/xtool/smoke/dbg.jks`, so the default path works with no environment
  variables. If a different `dbg.jks` is already there, it is first backed up as `dbg.jks.replaced-<date>`.
- Another key: `--keystore <file>` installs it the same way, or point `BLACKVR_KEYSTORE` (+ `_PASS`, `_ALIAS`)
  at it when calling `build_apk.sh`.
- Only when neither exists does the script generate a new key (PKCS12, alias `dbg`, CN=debug, RSA 2048,
  10000 days: the same parameters as the original). It prints a warning, because that APK will not update the
  installed one.

To check which key an APK carries, run `apksigner verify --print-certs build-quest-game/blackvr.apk`. The original
key's certificate SHA-256 starts with `19:18:5F:C1`; the `keystore` step prints the full value.

## Verification of this script (2026-11-12, in the original workspace)

These steps **ran for real** against a scratch prefix (`--prefix /home/claude/setup-test`). The repo was
`/root/black`, so `repo`, `python`, `rust` and `dsp` found their existing results.
- `preflight`: all six hosts answer.
- `android`: the SDK zips downloaded and their sha1 checked. The extracted tree is md5-identical to `/root/xtool/sdk`.
  The 664 MB NDK was symlinked in to save disk, and its URL, size and sha1 were checked against `repo.xml`.
- `keystore`: the generate-a-new-key path, then the smoke APK signed and verified with it.
- `deps`: 15 checkouts at their pins in 63 s. The sparse checkouts have the same file lists as the originals.
  The `mingw-gen` headers and the QEMU patch are identical to the originals. wincc compiles `<windows.h>` + `<d3d11.h>`.
- `qemu`: a fresh build, `qemu-aarch64 version 10.1.5`. `arm64selftest` then passed 7/7 under it.
- `openxr`: the loader built (aarch64).
- `xiso`: built at the pin, and a synthetic image round-tripped through `extract-xiso -c` / `-x`.
- `t_xrshell` 6/6, `t_nv2ax86` 10/10, `t_nv2aa64` 6/6 (under the scratch QEMU).
- Resume: after a failure and after an interrupt, the next run skipped every finished step. INT/TERM stop the
  current step's process tree.

These pieces were **verified separately**:
- capstone 5.0.7 built from the scratch checkout into a throwaway venv (2 min), and disassembles.
- The DSP aarch64 cargo build, run with an empty `CARGO_HOME`: crates came from crates.io through the proxy, which
  checks the NO_PROXY fix. 4 min 20 s; the 35.4 MB archive matches the original's size.
- `g_stage` against a throwaway repo copy with a synthetic game: `.iso` → extract, folder → symlink (+
  `.git/info/exclude`), `--copy-game`, `.zip`, and an existing `game/` left untouched (with its sha mismatch reported).
- The `apt` https fallback was checked by hand with the same `apt-get -o ...` options (25 MB of indexes fetched).
  All packages were already installed, so nothing was installed.

**Partly run:** `vvl`. Vulkan-Headers and SPIRV-Headers installed; SPIRV-Tools configured and was a third built
when it was stopped, because the host was overloaded and the build would have taken over 30 min. VUL and VVL use
the configure lines recovered from `vvl-build/*/CMakeCache.txt`.

**Dry-run only:**
- SwiftShader (1.4 GB checkout) and the Vulkan suites (`t_kvk`, `t_kvka64`, `t_vrglue`).
- `t_apkshell` and the game steps. In this workspace they would rebuild `port/quest` and the game while other
  work is under way there.
- In `/root` itself, the checks for `apt`, `repo`, `android`, `keystore`, `smokeapk`, `deps`, `qemu`, `openxr`,
  `python`, `rust`, `dsp`, `swiftshader` (vkprobe lists *SwiftShader Device (Subzero)*) and `vvl` (the layer is
  enabled explicitly) all pass against the existing install. So the pins and configure options match what is here.

**Not exercised:** the `--game <url>` download path, rustup installation on a machine without rustup, and apt
actually installing packages.
