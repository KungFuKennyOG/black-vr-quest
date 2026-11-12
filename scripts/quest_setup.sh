#!/usr/bin/env bash
# shellcheck disable=SC2034  # desc_<step> variables are read indirectly (${!d})
# quest_setup.sh: rebuild the BLACK VR (Quest 3) dev/test environment in an empty Linux x86_64 workspace.
#
#   scripts/quest_setup.sh [options]
#
# What it sets up (defaults match every path in port/notes/*.md):
#   /root/xtool   Android NDK r27c, SDK build-tools 34.0.0 + platform android-34 (dl.google.com, sha1-checked),
#                 env.sh, android-arm64-static.cmake, arm64run + qemu/bin/qemu-aarch64, bin/extract-xiso,
#                 smoke/ (test APK + the debug signing keystore dbg.jks)
#   /root/deps    pinned source checkouts + their builds (OpenXR loader for Android, QEMU, VVL, mingw-gen, ...)
#   /root/build   SwiftShader + the test-suite build dirs (xrt, nv2a-x86, nv2a-a64, kvk-tests, kvk-android, vr-glue)
#   <repo>        .venv (capstone 5.0.7 from source), build-dsp (DSP56300 Rust core, x86_64 + aarch64-linux-android),
#                 and with game files: game/, src/recomp/gen, build-game-android, build-boot-a64, build-quest-game
# Everything except the NDK/SDK zips, the Rust toolchain (rustup) and Ubuntu/PyPI/crates.io packages is built
# from source. See port/notes/setup.md.
#
# Options:
#   --deps-only            toolchains + dependencies only (no test suites, no game build)
#   --game <dir|iso|url>   the user's own BLACK files: a folder holding default.xbe, an .iso/.xiso disc image
#                          (extracted with extract-xiso built from source), or an http(s) link to either
#                          (a .zip of the folder works too). Never committed; game/ is git-ignored.
#   --copy-game            copy a game folder into <repo>/game instead of symlinking it
#   --regenerate           re-run scripts/generate.py even if src/recomp/gen exists
#   --with-render-x86      with game files, also configure+build tests/game_render_x86 (build-render-x86)
#   --keystore <file>      APK signing keystore to install as <xtool>/smoke/dbg.jks (default: the repo's
#                          port/quest/keys/dbg.jks when present). Password: $BLACKVR_KEYSTORE_PASS (default android)
#   --skip-swiftshader     skip the SwiftShader build (~1.4 GB checkout, ~8 min); Vulkan tests then exit 77 (skipped)
#   --skip-vvl             skip the Vulkan validation layer and its SPIRV/VUL deps
#   --no-apt               do not install Ubuntu packages (only report missing ones)
#   --jobs N               parallel build jobs (default: nproc)
#   --prefix DIR           root for deps/, xtool/, build/ (default /root; other values work for the toolchain
#                          and dependency steps, but several repo CMake files hard-code /root/deps and /root/xtool)
#   --android-cache DIR    use already-downloaded NDK/SDK zips from DIR (same file names as on dl.google.com)
#   --only a,b,c           run only these steps          --skip a,b,c   skip these steps
#   --force a,b,c          re-run these steps even when their check says they are done
#   --dry-run              print what each step would run; change nothing
#   --list                 list the steps and exit
#   -h, --help
#
# Idempotent and resumable: each step first runs a cheap check (tool versions, pinned commits, output files) and
# is skipped when it passes, so re-running after a failure continues where it stopped. Logs: <prefix>/.quest_setup/logs.

set -Eeuo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
HELPERS="$REPO/scripts/quest_setup"

# ------------------------------------------------------------------------------------------------ options
PREFIX=/root
JOBS="$(nproc 2>/dev/null || echo 2)"
DEPS_ONLY=0 SKIP_SS=0 SKIP_VVL=0 NO_APT=0 DRY=0 COPY_GAME=0 REGEN=0 RENDER_X86=0
GAME_SRC="" KEYSTORE_SRC="" ANDROID_CACHE="" ONLY="" SKIPS="" FORCE="" LIST=0

usage() { sed -n '3,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
  case "$1" in
    --deps-only) DEPS_ONLY=1 ;;
    --game) GAME_SRC="${2:?--game needs a path or URL}"; shift ;;
    --copy-game) COPY_GAME=1 ;;
    --regenerate) REGEN=1 ;;
    --with-render-x86) RENDER_X86=1 ;;
    --keystore) KEYSTORE_SRC="${2:?--keystore needs a file}"; shift ;;
    --skip-swiftshader) SKIP_SS=1 ;;
    --skip-vvl) SKIP_VVL=1 ;;
    --no-apt) NO_APT=1 ;;
    --jobs|-j) JOBS="${2:?--jobs needs N}"; shift ;;
    --prefix) PREFIX="${2:?--prefix needs a dir}"; shift ;;
    --android-cache) ANDROID_CACHE="${2:?}"; shift ;;
    --only) ONLY="${2:?}"; shift ;;
    --skip) SKIPS="${2:?}"; shift ;;
    --force) FORCE="${2:?}"; shift ;;
    --dry-run) DRY=1 ;;
    --list) LIST=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be a positive integer" >&2; exit 2; }
mkdir -p "$PREFIX" 2>/dev/null || true
PREFIX="$(cd "$PREFIX" 2>/dev/null && pwd -P || echo "$PREFIX")"
DEPS="$PREFIX/deps" XTOOL="$PREFIX/xtool" BUILD="$PREFIX/build"
STATE="$PREFIX/.quest_setup" LOGS="$PREFIX/.quest_setup/logs" DL="$PREFIX/.quest_setup/downloads"
VENV="$REPO/.venv"
[ -n "$GAME_SRC" ] && [ -e "$GAME_SRC" ] && GAME_SRC="$(cd "$(dirname "$GAME_SRC")" && pwd -P)/$(basename "$GAME_SRC")"

# Same values as the generated <xtool>/env.sh, so steps work before it exists.
export ANDROID_NDK_HOME="$XTOOL/android-ndk-r27c"
export ANDROID_NDK="$ANDROID_NDK_HOME"
export ANDROID_SDK_ROOT="$XTOOL/sdk"
export ANDROID_PLATFORM_JAR="$XTOOL/sdk/android-34/android.jar"
export ANDROID_BUILD_TOOLS="$XTOOL/sdk/android-14"
export NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
export ANDROID_CC="$NDK_BIN/aarch64-linux-android29-clang"
export ANDROID_CXX="$NDK_BIN/aarch64-linux-android29-clang++"
export ANDROID_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
STATIC_TC="$XTOOL/android-arm64-static.cmake"
export PATH="$HOME/.cargo/bin:$PATH"
KS_PASS="${BLACKVR_KEYSTORE_PASS:-android}"
SS_ICD="$BUILD/swiftshader/Linux/vk_swiftshader_icd.json"
VKPROBE="$STATE/bin/vkprobe"

# ------------------------------------------------------------------------------------------------ pins
# name | url | ref (tag/branch, informational) | commit | sparse-checkout dirs (cone mode)
PINS_CORE=(
  "OpenXR-SDK|https://github.com/KhronosGroup/OpenXR-SDK.git|release-1.1.65|24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8|"
  "Vulkan-Headers|https://github.com/KhronosGroup/Vulkan-Headers.git|v1.4.367|9fbc2a782ee3f8a4db64e7d991e23da75e893f44|"
  "glslang|https://github.com/KhronosGroup/glslang.git|main|b93de9f4a8d5963b2f33884ca3d1870f4f282039|"
  "qemu|https://github.com/qemu/qemu.git|v10.1.5|29e39cbdfe8432ac9d2b3bfb944a32c76afe33b8|"
  "capstone-5.0.7|https://github.com/capstone-engine/capstone.git|5.0.7|52c66920fc7bfa15fd9626dfd9f646c698aaa99b|"
  "sse2neon|https://github.com/DLTcollab/sse2neon.git|master|2272251cc3ad9cba8b8c47c4065811cfb396b802|"
  "mingw-w64|https://github.com/mingw-w64/mingw-w64.git|master|ee9bd87644156c41072a11ad957d1e11831a72df|mingw-w64-headers"
  "platform_bionic|https://github.com/aosp-mirror/platform_bionic.git|main|731631f300090436d7f5df80d50b6275c8c60a93|libc/include libc/kernel/android/uapi libc/kernel/uapi"
  "android_frameworks_native|https://github.com/LineageOS/android_frameworks_native.git||46018f54b42489de960c62c3db9de131abce43ea|include/android libs/arect/include libs/binder/liblog_stub/include libs/nativewindow/include"
  "android_libnativehelper|https://github.com/LineageOS/android_libnativehelper.git|cm-10.1-M1|73d3c2e3ed100f1d0ccc08a9675844bf8a0848fa|include/nativehelper"
  "android_system_logging|https://github.com/LineageOS/android_system_logging.git||336e1707c8258adf1f1c5c5511218b533f3a22e8|liblog/include"
)
PINS_VVL=(
  "SPIRV-Headers|https://github.com/KhronosGroup/SPIRV-Headers.git||fef168c54ed2e37c314eb251806aaf9877f38465|"
  "SPIRV-Tools|https://github.com/KhronosGroup/SPIRV-Tools.git||28f99f74a2a922ff85fc8fd966d6b3d6a45be795|"
  "Vulkan-Utility-Libraries|https://github.com/KhronosGroup/Vulkan-Utility-Libraries.git||271510bc50a1ecc65e62ca07dbf6b8dfe530dfd0|"
  "Vulkan-ValidationLayers|https://github.com/KhronosGroup/Vulkan-ValidationLayers.git|v1.4.367|af7950f167455c91eb3c27754907e36f43c9cb2f|"
)
PIN_SWIFTSHADER="swiftshader|https://github.com/google/swiftshader.git||cddb5c91b23ff70a1f607ea31610aa0d7f4b5ad5|"
PIN_XISO="extract-xiso|https://github.com/XboxDev/extract-xiso.git|build-202609111233|3f5b62cfe68f000b0e3c8a30104973f3a297948e|"
# dl.google.com/android/repository: file | sha1 | top-level dir in the zip | install dir
ANDROID_ZIPS=(
  "android-ndk-r27c-linux.zip|090e8083a715fdb1a3e402d0763c388abb03fb4e|android-ndk-r27c|$XTOOL/android-ndk-r27c"
  "build-tools_r34-linux.zip|d6d58e0c6925a9e4d9a541e84cd1f405c2f9d2a9|android-14|$XTOOL/sdk/android-14"
  "platform-34-ext7_r03.zip|1f2e9478d6a7601425ceaa553311dc43191f103d|android-34|$XTOOL/sdk/android-34"
)
APT_PKGS=(build-essential clang clang-18 lld lld-18 llvm llvm-18 cmake ninja-build git curl ca-certificates zip unzip
  xz-utils file patch bison pkgconf libglib2.0-dev libpcre2-dev libffi-dev zlib1g-dev libmount-dev libpixman-1-dev
  libpng-dev libvulkan1 python3 python3-venv python3-pip python3-dev openjdk-21-jdk-headless)
APT_OPTIONAL=(ffmpeg gdb valgrind)

# ------------------------------------------------------------------------------------------------ helpers
if [ -t 1 ]; then B=$'\e[1m' G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' N=$'\e[0m'; else B="" G="" Y="" R="" N=""; fi
say()  { printf '%s\n' "$*"; }
warn() { printf '%s\n' "${Y}WARNING:${N} $*" >&2; }
die()  { printf '%s\n' "${R}ERROR:${N} $*" >&2; exit 1; }
# x: run (or, with --dry-run, print) one command
x() { printf '+ %s\n' "$(printf '%q ' "$@")"; [ "$DRY" = 1 ] && return 0; "$@"; }
in_list() { case ",$2," in *",$1,"*) return 0 ;; esac; return 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# Hosts the session's NO_PROXY sends direct (unreachable here); route them through $HTTPS_PROXY instead.
fix_no_proxy() {
  [ -n "${HTTPS_PROXY:-${https_proxy:-}}" ] || return 0
  local v out h
  for v in NO_PROXY no_proxy; do
    [ -n "${!v:-}" ] || continue
    out=""
    for h in ${!v//,/ }; do
      case "$h" in pypi.org|files.pythonhosted.org|index.crates.io|static.crates.io|crates.io|static.rust-lang.org) ;;
        *) out="${out:+$out,}$h" ;;
      esac
    done
    export "$v=$out"
  done
}

proxy_hint() {
  local p="${HTTPS_PROXY:-${https_proxy:-}}"
  [ -n "$p" ] || return 0
  say "  proxy status (recent failures):"
  curl -sS -m 10 "$p/__agentproxy/status" 2>/dev/null | python3 -c 'import json,sys
try:
  d=json.load(sys.stdin); [print("   ",f) for f in d.get("recentRelayFailures",[])[-5:]] or print("    none recorded")
except Exception as e: print("    (no status:",e,")")' 2>/dev/null || true
}

# net_check URL WHAT: fail clearly when a host is blocked by the network allowlist.
net_check() {
  local url="$1" what="$2" code err
  err="$(mktemp)"
  code="$(curl -sS -o /dev/null -m 30 -I -w '%{http_code}' "$url" 2>"$err" || true)"
  case "$code" in
    2*|3*|400|401|404|405) rm -f "$err"; return 0 ;;
  esac
  say "${R}BLOCKED:${N} $url (needed for $what) -> HTTP '${code:-none}' $(tr '\n' ' ' <"$err")"
  rm -f "$err"
  say "  Only package registries, GitHub and dl.google.com are expected to be reachable. If this host is required,"
  say "  ask for it to be allowed (do not work around the proxy)."
  proxy_hint
  return 1
}

# clone_pin "name|url|ref|commit|sparse dirs" [dest]: shallow checkout of exactly that commit.
clone_pin() {
  local spec="$1" name url ref sha sparse dir have
  IFS='|' read -r name url ref sha sparse <<<"$spec"
  dir="${2:-$DEPS/$name}"
  if [ -d "$dir/.git" ]; then
    have="$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)"
    if [ "$have" = "$sha" ]; then say "  $name: at ${sha:0:12}"; return 0; fi
    if [ -n "$have" ] && [ -n "$(git -C "$dir" status --porcelain 2>/dev/null)" ]; then
      die "$dir is at ${have:0:12} with local changes; expected ${sha:0:12}. Move it away and re-run."
    fi
  else
    x mkdir -p "$dir"
    x git -C "$dir" init -q
    x git -C "$dir" remote add origin "$url"
  fi
  local filter=()
  if [ -n "$sparse" ]; then
    # shellcheck disable=SC2086
    x git -C "$dir" sparse-checkout set --cone $sparse
    filter=(--filter=blob:none)
  fi
  if ! x git -C "$dir" fetch -q --depth 1 "${filter[@]}" origin "$sha"; then
    if [ -n "$ref" ] && x git -C "$dir" fetch -q --depth 1 "${filter[@]}" origin "$ref"; then :; else
      say "git fetch of $url @ $sha failed"; net_check "https://github.com" "git clones" || true; return 1
    fi
  fi
  x git -C "$dir" -c advice.detachedHead=false checkout -q --detach "$sha"
  [ "$DRY" = 1 ] || [ "$(git -C "$dir" rev-parse HEAD)" = "$sha" ] || { say "$dir: HEAD is not $sha"; return 1; }
  say "  $name: checked out ${sha:0:12} ${ref:+($ref)}"
}
pin_ok() { local name url ref sha sparse; IFS='|' read -r name url ref sha sparse <<<"$1"
  [ "$(git -C "${2:-$DEPS/$name}" rev-parse HEAD 2>/dev/null)" = "$sha" ]; }

cm_configure() { x cmake -G Ninja "$@"; }
cm_build() { x cmake --build "$1" -j "$JOBS" "${@:2}"; }
ctest_run() { x ctest --test-dir "$1" --output-on-failure "${@:2}"; }
ctest_detail() { # last "N% tests passed" line of a step log
  { grep -E 'tests passed|No tests were found' "$1" 2>/dev/null || true; } | tail -1 | sed 's/^ *//'; }

file_arch() { file -L "$1" 2>/dev/null | grep -oE 'ARM aarch64|x86-64' | head -1; }

# ------------------------------------------------------------------------------------------------ steps
# Each step has: desc_<s> (one line), check_<s> (read-only; prints a detail line; 0 = done), step_<s> (does it).
# Steps without a check_ (the test suites) always run.

desc_preflight="host, disk, network allowlist (report only; each step re-checks the hosts it fetches from)"
step_preflight() {
  [ "$(uname -s)/$(uname -m)" = "Linux/x86_64" ] || die "needs an x86_64 Linux host (got $(uname -s)/$(uname -m))"
  [ "${BASH_VERSINFO[0]}" -ge 4 ] || die "needs bash >= 4"
  say "  host: $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}"), $(nproc) cores, jobs=$JOBS"
  local need=14 free
  [ "$SKIP_SS" = 1 ] || need=$((need + 2))
  [ "$SKIP_VVL" = 1 ] || need=$((need + 1))
  [ -n "$GAME_SRC" ] && need=$((need + 4))
  [ "$RENDER_X86" = 1 ] && need=$((need + 5))
  free=$(( $(df -Pk "$PREFIX" | awk 'NR==2{print $4}') / 1024 / 1024 ))
  say "  disk: ${free} GB free under $PREFIX, about ${need} GB needed for a fresh run (less when parts exist)"
  [ "$free" -ge "$need" ] || warn "low disk space: ${free} GB free, ~${need} GB needed by a full fresh run"
  [ "$PREFIX" = /root ] || warn "prefix $PREFIX: repo CMake files hard-code /root/deps/sse2neon, /root/xtool/... in places;
  the toolchain/deps steps honour the prefix, test/game builds may still look in /root"
  local bad=""
  net_check https://github.com "git clones" || bad="$bad github.com"
  net_check https://dl.google.com/android/repository/repository2-1.xml "Android NDK/SDK" || bad="$bad dl.google.com"
  net_check https://static.rust-lang.org/dist/channel-rust-stable.toml.sha256 "rustup" || bad="$bad static.rust-lang.org"
  net_check https://pypi.org/simple/pip/ "pip (capstone build deps)" || bad="$bad pypi.org"
  net_check https://index.crates.io/config.json "cargo (DSP56300 crates)" || bad="$bad index.crates.io"
  net_check https://archive.ubuntu.com/ubuntu/ "apt (https mirror through the proxy)" || bad="$bad archive.ubuntu.com"
  if [ -n "$bad" ]; then warn "unreachable:$bad. Steps that need them will fail (finished steps are skipped)."
  else say "  network: github.com, dl.google.com, static.rust-lang.org, pypi.org, index.crates.io, archive.ubuntu.com reachable"; fi
}

desc_apt="Ubuntu packages (compilers, cmake/ninja, glib for QEMU, JDK for apksigner, python3)"
apt_missing() { local p; for p in "$@"; do dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' || printf '%s ' "$p"; done; }
check_apt() {
  local m; m="$(apt_missing "${APT_PKGS[@]}")"
  [ -z "$m" ] || { echo "missing: $m"; return 1; }
  echo "all ${#APT_PKGS[@]} packages present; cmake $(cmake --version | awk 'NR==1{print $3}'), $(gcc --version | head -1 | awk '{print $NF}') gcc, clang $(clang --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
}
step_apt() {
  local missing opt; missing="$(apt_missing "${APT_PKGS[@]}")"; opt="$(apt_missing "${APT_OPTIONAL[@]}")"
  [ -n "$missing$opt" ] || return 0
  if [ "$NO_APT" = 1 ] || [ "$(id -u)" != 0 ]; then
    say "  not installing (--no-apt or not root). Missing: ${missing:-none}; optional missing: ${opt:-none}"
    [ -z "$missing" ] || return 1; return 0
  fi
  export DEBIAN_FRONTEND=noninteractive
  local opts=() out
  # The stock sources are plain http, which the agent proxy cannot carry; https archive.ubuntu.com works through it.
  out="$(apt-get update 2>&1 || true)"; say "$out" | tail -3
  if grep -qE 'Failed to fetch|Could not resolve|Temporary failure' <<<"$out"; then
    say "  apt over http failed; retrying with https sources through \$HTTPS_PROXY"
    mkdir -p "$STATE/apt"
    if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
      sed 's#http://#https://#g' /etc/apt/sources.list.d/ubuntu.sources >"$STATE/apt/ubuntu-https.sources"
    else
      sed 's#http://#https://#g' /etc/apt/sources.list >"$STATE/apt/ubuntu-https.list"
    fi
    opts=(-o Dir::Etc::SourceList=/dev/null -o "Dir::Etc::SourceParts=$STATE/apt")
    [ -n "${HTTPS_PROXY:-}" ] && opts+=(-o "Acquire::https::Proxy=$HTTPS_PROXY")
    x apt-get "${opts[@]}" update
  fi
  # shellcheck disable=SC2086
  [ -z "$missing" ] || x apt-get "${opts[@]}" install -y --no-install-recommends $missing
  # shellcheck disable=SC2086
  [ -z "$opt" ] || x apt-get "${opts[@]}" install -y --no-install-recommends $opt || warn "optional packages failed: $opt"
}

desc_repo="repo submodules needed by the port (xboxrecomp if not vendored, dsp56300)"
check_repo() {
  [ -f "$REPO/third_party/xboxrecomp/CMakeLists.txt" ] && [ -f "$REPO/third_party/dsp56300/crates/emu-ffi/Cargo.toml" ] || return 1
  echo "xboxrecomp $(git -C "$REPO/third_party/xboxrecomp" rev-parse --short HEAD 2>/dev/null || echo vendored), dsp56300 $(git -C "$REPO/third_party/dsp56300" rev-parse --short HEAD 2>/dev/null || echo vendored)"
}
step_repo() {
  [ -e "$REPO/.git" ] || die "$REPO is not a git checkout and the submodules are missing"
  [ -f "$REPO/third_party/xboxrecomp/CMakeLists.txt" ] || x git -C "$REPO" submodule update --init third_party/xboxrecomp
  [ -f "$REPO/third_party/dsp56300/crates/emu-ffi/Cargo.toml" ] || x git -C "$REPO" submodule update --init third_party/dsp56300
  # launcher/vendor/* (Windows/Linux desktop launcher) are not needed for the Quest port.
}

desc_android="Android NDK r27c + build-tools 34.0.0 + platform android-34 (dl.google.com), env.sh, toolchain files"
check_android() {
  grep -q '27.2.12479018' "$ANDROID_NDK/source.properties" 2>/dev/null || return 1
  grep -q 'Pkg.Revision=34.0.0' "$ANDROID_BUILD_TOOLS/source.properties" 2>/dev/null || return 1
  [ -f "$ANDROID_PLATFORM_JAR" ] && [ -f "$XTOOL/env.sh" ] && [ -f "$STATIC_TC" ] && [ -x "$XTOOL/arm64run" ] || return 1
  "$ANDROID_CC" --version >/dev/null 2>&1 && "$ANDROID_BUILD_TOOLS/aapt2" version >/dev/null 2>&1 || return 1
  echo "NDK $(grep -oE 'clang version [0-9.]+' <("$ANDROID_CC" --version)), aapt2 $("$ANDROID_BUILD_TOOLS/aapt2" version 2>&1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?-[0-9]+' | head -1), apksigner $("$ANDROID_BUILD_TOOLS/apksigner" --version 2>/dev/null), glslc $("$ANDROID_NDK/shader-tools/linux-x86_64/glslc" --version 2>/dev/null | head -1 | awk '{print $2}')"
}
step_android() {
  x mkdir -p "$XTOOL/sdk" "$DL"
  local spec file sha1 top dest zip tmp
  for spec in "${ANDROID_ZIPS[@]}"; do
    IFS='|' read -r file sha1 top dest <<<"$spec"
    if [ -e "$dest/source.properties" ]; then say "  $file: already installed at $dest"; continue; fi
    zip="$DL/$file"
    if [ -n "$ANDROID_CACHE" ] && [ -f "$ANDROID_CACHE/$file" ]; then zip="$ANDROID_CACHE/$file"
    elif [ ! -f "$zip" ] || [ "$DRY" = 1 ] || ! echo "$sha1  $zip" | sha1sum -c --status; then
      net_check "https://dl.google.com/android/repository/repository2-1.xml" "Android $file" || return 1
      x curl -fL --retry 3 -C - -o "$zip" "https://dl.google.com/android/repository/$file"
    fi
    if [ "$DRY" = 0 ]; then
      echo "$sha1  $zip" | sha1sum -c - || { rm -f "$zip"; die "$file: sha1 mismatch (deleted; re-run to fetch again)"; }
    fi
    tmp="$XTOOL/.unzip-$top"
    x rm -rf "$tmp"
    x unzip -q "$zip" -d "$tmp"
    x mkdir -p "$(dirname "$dest")"
    x mv "$tmp/$top" "$dest"
    x rm -rf "$tmp"
    [ "$zip" = "$DL/$file" ] && x rm -f "$zip"   # keep the disk small; the sha1 pins make re-downloads safe
  done
  [ "$DRY" = 1 ] && return 0
  write_xtool_files
}
write_xtool_files() {
  cat >"$XTOOL/env.sh" <<EOF
# Source this: . $XTOOL/env.sh      (generated by scripts/quest_setup.sh)
export ANDROID_NDK_HOME=$XTOOL/android-ndk-r27c
export ANDROID_NDK=\$ANDROID_NDK_HOME
export ANDROID_SDK_ROOT=$XTOOL/sdk
export ANDROID_PLATFORM_JAR=$XTOOL/sdk/android-34/android.jar
export ANDROID_BUILD_TOOLS=$XTOOL/sdk/android-14   # build-tools 34.0.0 (aapt2, zipalign, apksigner, d8)
export NDK_BIN=\$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
export ANDROID_CC="\$NDK_BIN/aarch64-linux-android29-clang"
export ANDROID_CXX="\$NDK_BIN/aarch64-linux-android29-clang++"
export ANDROID_TOOLCHAIN_FILE=\$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake
# CMake: -DCMAKE_TOOLCHAIN_FILE=\$ANDROID_TOOLCHAIN_FILE -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
# APK (no Gradle; Maven is unreachable):
#   \$ANDROID_BUILD_TOOLS/aapt2 link -o base.apk --manifest AndroidManifest.xml -I \$ANDROID_PLATFORM_JAR
#   zip -r base.apk lib/arm64-v8a/*.so ; \$ANDROID_BUILD_TOOLS/zipalign -f -p 4 base.apk aligned.apk
#   \$ANDROID_BUILD_TOOLS/apksigner sign --ks $XTOOL/smoke/dbg.jks --ks-pass pass:android --out app.apk aligned.apk
# Working example: $XTOOL/smoke
EOF
  cat >"$STATIC_TC" <<EOF
# Android arm64 (Quest 3) cross toolchain with STATIC executables that run on this x86_64 host
# through $XTOOL/arm64run (QEMU linux-user). Lets \`ctest\` execute arm64 test binaries.
# (generated by scripts/quest_setup.sh; see port/notes/arm64-exec.md)
#
#   cmake -G Ninja -S . -B build-a64 -DCMAKE_TOOLCHAIN_FILE=$STATIC_TC
#   cmake --build build-a64 && ctest --test-dir build-a64 --output-on-failure
#
# Shared libraries (add_library SHARED, e.g. the final libmain.so for the APK) still build normally;
# only executables are linked -static. Override API level with -DANDROID_PLATFORM=android-NN.
set(ANDROID_ABI arm64-v8a CACHE STRING "")
if(NOT ANDROID_PLATFORM)
  set(ANDROID_PLATFORM android-29 CACHE STRING "")
endif()
if(NOT ANDROID_STL)
  set(ANDROID_STL c++_static CACHE STRING "")
endif()
set(_ndk "\$ENV{ANDROID_NDK_HOME}")
if(NOT _ndk)
  set(_ndk $XTOOL/android-ndk-r27c)
endif()
include("\${_ndk}/build/cmake/android.toolchain.cmake")

# Static executables (bionic libc.a; no /system/bin/linker64 needed under the emulator).
# NDK's toolchain adds -pie by default; -static overrides to a static non-PIE executable.
# NDK r27c + CMake 3.28 uses android-legacy.toolchain.cmake, which sets a *normal* variable
# CMAKE_EXE_LINKER_FLAGS (shadowing the cache), so append to that variable here.
# With -DANDROID_USE_LEGACY_TOOLCHAIN_FILE=OFF, CMake's own Android support builds *_INIT instead.
if(_USE_LEGACY_TOOLCHAIN_FILE)
  if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "(^| )-static( |\$)")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static")
  endif()
else()
  string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -static")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "")

set(CMAKE_CROSSCOMPILING_EMULATOR $XTOOL/arm64run CACHE FILEPATH "Runs aarch64 test binaries on the host")
EOF
  cat >"$XTOOL/arm64run" <<EOF
#!/bin/sh
# arm64run: run a static aarch64 ELF (e.g. NDK \`-static\` bionic binary) on this x86_64 host.
# Wraps QEMU linux-user (built from source, qemu v10.1.5) at $XTOOL/qemu/bin/qemu-aarch64.
# Exit code, stdin/stdout/stderr and signals (SIGSEGV + rt_sigframe/ucontext) pass through.
#
# Env:
#   ARM64RUN_CPU   QEMU CPU model (default cortex-a76: ARMv8.2 + fp16/dotprod/rcpc, close to
#                  Quest 3's Cortex-A78C; no SVE). Use "max" for every feature QEMU implements.
#   ARM64RUN_STRACE=1   log guest syscalls to stderr (qemu -strace)
#   ARM64RUN_GDB=port   wait for gdb on that port (qemu -g)
#   ARM64RUN_ARGS  extra raw qemu options
# Usage: arm64run ./prog [args...]
QEMU="\${ARM64RUN_QEMU:-$XTOOL/qemu/bin/qemu-aarch64}"
if [ \$# -lt 1 ]; then echo "usage: arm64run ./static_arm64_binary [args...]" >&2; exit 2; fi
set -- -cpu "\${ARM64RUN_CPU:-cortex-a76}" \\
  \${ARM64RUN_STRACE:+-strace} \${ARM64RUN_GDB:+-g "\$ARM64RUN_GDB"} \${ARM64RUN_ARGS} "\$@"
exec "\$QEMU" "\$@"
EOF
  chmod +x "$XTOOL/arm64run"
  say "  wrote $XTOOL/env.sh, $STATIC_TC, $XTOOL/arm64run"
}

desc_keystore="APK signing keystore at <xtool>/smoke/dbg.jks (the user's key, so installs update in place)"
KS_DEST() { echo "$XTOOL/smoke/dbg.jks"; }
ks_fpr() { keytool -list -v -keystore "$1" -storepass "$KS_PASS" 2>/dev/null | awk '/SHA256:/{print $2; exit}'; }
check_keystore() {
  local d; d="$(KS_DEST)"
  [ -f "$d" ] || return 1
  local f; f="$(ks_fpr "$d")"; [ -n "$f" ] || { echo "cannot open $d with the given password"; return 1; }
  local src="${KEYSTORE_SRC:-$REPO/port/quest/keys/dbg.jks}"
  [ -z "$KEYSTORE_SRC" ] || [ -f "$KEYSTORE_SRC" ] || { echo "--keystore $KEYSTORE_SRC not found"; return 1; }
  if [ -f "$src" ] && ! cmp -s "$src" "$d"; then echo "$d differs from $src"; return 1; fi
  echo "cert SHA-256 $f"
}
step_keystore() {
  local d src; d="$(KS_DEST)"; x mkdir -p "$(dirname "$d")"
  src="$KEYSTORE_SRC"
  [ -z "$src" ] && [ -f "$REPO/port/quest/keys/dbg.jks" ] && src="$REPO/port/quest/keys/dbg.jks"
  if [ -n "$src" ]; then
    [ -f "$src" ] || die "keystore $src not found"
    [ "$DRY" = 1 ] || [ -n "$(ks_fpr "$src")" ] || die "cannot open $src (store password: \$BLACKVR_KEYSTORE_PASS, default android)"
    if [ -f "$d" ] && ! cmp -s "$src" "$d"; then
      x cp -p "$d" "$d.replaced-$(date +%Y%m%d%H%M%S)"
      warn "replacing a different $d (backup kept next to it)"
    fi
    x install -m 600 "$src" "$d"
    say "  installed $src -> $d"
  elif [ ! -f "$d" ]; then
    warn "no keystore given and port/quest/keys/dbg.jks is absent: generating a NEW debug key.
  An APK signed with it cannot update the one on the headset: uninstall com.kenny.blackvr first
  (game files in /sdcard/BlackVR survive). Prefer the user's key: --keystore <file> or BLACKVR_KEYSTORE."
    x keytool -genkeypair -keystore "$d" -storetype PKCS12 -storepass "$KS_PASS" -keypass "$KS_PASS" \
      -alias dbg -dname CN=debug -keyalg RSA -keysize 2048 -validity 10000
  fi
}

desc_smokeapk="smoke APK (NDK .so + aapt2 + zipalign + apksigner): checks the whole APK tool chain"
check_smokeapk() {
  [ -f "$XTOOL/smoke/smoke.apk" ] && "$ANDROID_BUILD_TOOLS/apksigner" verify "$XTOOL/smoke/smoke.apk" >/dev/null 2>&1 || return 1
  echo "smoke.apk verifies ($(du -k "$XTOOL/smoke/smoke.apk" | cut -f1) KB)"
}
step_smokeapk() {
  local S="$XTOOL/smoke"; x mkdir -p "$S/lib/arm64-v8a"
  if [ "$DRY" = 0 ]; then
    cat >"$S/smoke.c" <<'EOF'
#include <android/native_activity.h>
#include <android/log.h>
void ANativeActivity_onCreate(ANativeActivity* a, void* s, size_t n){ __android_log_print(ANDROID_LOG_INFO,"smoke","hello quest"); }
EOF
    cat >"$S/AndroidManifest.xml" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.kenny.smoke" android:versionCode="1" android:versionName="0.1">
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="32"/>
  <application android:label="Smoke" android:hasCode="false">
    <activity android:name="android.app.NativeActivity" android:exported="true">
      <meta-data android:name="android.app.lib_name" android:value="smoke"/>
      <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
    </activity>
  </application>
</manifest>
EOF
  fi
  x "$ANDROID_CC" -shared -fPIC -O2 -o "$S/lib/arm64-v8a/libsmoke.so" "$S/smoke.c" -landroid -llog
  x rm -f "$S/base.apk" "$S/aligned.apk" "$S/smoke.apk"
  x "$ANDROID_BUILD_TOOLS/aapt2" link -o "$S/base.apk" --manifest "$S/AndroidManifest.xml" -I "$ANDROID_PLATFORM_JAR"
  x bash -c "cd '$S' && zip -q -r base.apk lib"
  x "$ANDROID_BUILD_TOOLS/zipalign" -f -p 4 "$S/base.apk" "$S/aligned.apk"
  x "$ANDROID_BUILD_TOOLS/apksigner" sign --ks "$S/dbg.jks" --ks-pass "pass:$KS_PASS" --out "$S/smoke.apk" "$S/aligned.apk"
  x "$ANDROID_BUILD_TOOLS/apksigner" verify --verbose "$S/smoke.apk"
}

desc_deps="pinned source checkouts in <deps> (+ qemu fp-test patch, mingw-gen headers, wincc.sh)"
all_core_pins() { printf '%s\n' "${PINS_CORE[@]}"; [ "$SKIP_VVL" = 1 ] || printf '%s\n' "${PINS_VVL[@]}"; }
check_deps() {
  local p n=0; while IFS= read -r p; do pin_ok "$p" || { echo "not at pin: ${p%%|*}"; return 1; }; n=$((n+1)); done < <(all_core_pins)
  [ -f "$DEPS/mingw-gen/_mingw.h" ] && grep -q '^subdir_done()' "$DEPS/qemu/tests/fp/meson.build" || return 1
  echo "$n checkouts at their pinned commits"
}
step_deps() {
  x mkdir -p "$DEPS"
  local p; while IFS= read -r p; do clone_pin "$p"; done < <(all_core_pins)
  # QEMU: tests/fp needs the berkeley-softfloat-3 subproject from gitlab.com; skip those tests (the only local change)
  local fp="$DEPS/qemu/tests/fp/meson.build"
  if [ "$DRY" = 1 ] || ! grep -q '^subdir_done()' "$fp"; then
    x sed -i '1i # arm64-exec: skip fp tests (gitlab subproject unreachable)\nsubdir_done()' "$fp"
  fi
  # mingw-w64 headers configured the way wincc.sh uses them (compile-only check of the Windows D3D11 path)
  x mkdir -p "$DEPS/mingw-gen/sdks"
  if [ "$DRY" = 0 ]; then
    sed -e 's/@DEFAULT_MSVCRT_VERSION@/0xE00/' -e 's/@DEFAULT_WIN32_WINNT@/0xA00/' \
      "$DEPS/mingw-w64/mingw-w64-headers/crt/_mingw.h.in" >"$DEPS/mingw-gen/_mingw.h"
    : >"$DEPS/mingw-gen/_mingw_ddk.h"; printf '\n' >"$DEPS/mingw-gen/sdks/_mingw_ddk.h"
    mkdir -p "$BUILD"
    cat >"$BUILD/wincc.sh" <<EOF
#!/bin/bash
# clang targeting x86_64 Windows with mingw-w64 headers: compile-only check of the D3D11 path
cd $REPO/third_party/xboxrecomp/src
exec clang --target=x86_64-w64-mingw32 -std=gnu11 -fms-extensions -msse4.2 -O2 -nostdinc -isystem \$(clang -print-resource-dir)/include -isystem $DEPS/mingw-gen -isystem $DEPS/mingw-w64/mingw-w64-headers/include -isystem $DEPS/mingw-w64/mingw-w64-headers/crt -isystem $DEPS/mingw-w64/mingw-w64-headers/defaults/include -I. -Inv2a -Iplatform -Ikernel -DWIN32_LEAN_AND_MEAN -DNOMINMAX "\$@"
EOF
    chmod +x "$BUILD/wincc.sh"
    printf '#include <windows.h>\n#include <d3d11.h>\nint main(void){return 0;}\n' >"$STATE/wincc-check.c"
    "$BUILD/wincc.sh" -fsyntax-only "$STATE/wincc-check.c" && say "  wincc.sh: <windows.h> + <d3d11.h> compile OK"
  fi
}

desc_qemu="QEMU v10.1.5 linux-user aarch64, static (configure + ninja ~3-4 min) -> <xtool>/qemu/bin"
check_qemu() {
  local q="$XTOOL/qemu/bin/qemu-aarch64"
  [ -x "$q" ] && "$q" --version 2>/dev/null | grep -q '10\.1\.5' || return 1
  echo "$("$q" --version | head -1)"
}
step_qemu() {
  local b="$DEPS/qemu-b2"; x mkdir -p "$b"
  if [ "$DRY" = 1 ] || [ ! -f "$b/build.ninja" ]; then
    x bash -c "cd '$b' && '$DEPS/qemu/configure' --target-list=aarch64-linux-user --disable-system --disable-tools \
      --disable-docs --disable-werror --disable-rust --static --prefix='$XTOOL/qemu'"
  fi
  x ninja -C "$b" -j "$JOBS" qemu-aarch64
  x mkdir -p "$XTOOL/qemu/bin"
  x cp "$b/qemu-aarch64" "$XTOOL/qemu/bin/qemu-aarch64"
  x strip "$XTOOL/qemu/bin/qemu-aarch64"
}

desc_arm64selftest="arm64 self-tests under QEMU (NDK -static: argv/exit, NEON, malloc, SIGSEGV ucontext, io, libc++)"
step_arm64selftest() {
  local t="$XTOOL/arm64-tests"; x mkdir -p "$t"
  x cp "$HELPERS"/arm64-tests/{CMakeLists.txt,hello.c,io.c,malloc.c,neon.c,segv.c,cxx.cpp} "$t/"
  cm_configure -S "$t" -B "$t/build" -DCMAKE_TOOLCHAIN_FILE="$STATIC_TC"
  cm_build "$t/build"
  [ "$DRY" = 1 ] || file "$t/build/hello" | grep -q 'statically linked' || { say "hello is not static"; return 1; }
  ctest_run "$t/build"
}

desc_openxr="Khronos OpenXR loader for Android arm64-v8a/android-29 (as port/quest/build_apk.sh builds it)"
check_openxr() {
  local so="$DEPS/build-openxr-android/src/loader/libopenxr_loader.so"
  [ -f "$so" ] && [ "$(file_arch "$so")" = "ARM aarch64" ] || return 1
  echo "libopenxr_loader.so aarch64 ($(du -k "$so" | cut -f1) KB)"
}
step_openxr() {
  cm_configure -S "$DEPS/OpenXR-SDK" -B "$DEPS/build-openxr-android" -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DDYNAMIC_LOADER=ON
  cm_build "$DEPS/build-openxr-android" --target openxr_loader
}

desc_xiso="extract-xiso (XboxDev, from source) -> <xtool>/bin/extract-xiso"
check_xiso() { [ -x "$XTOOL/bin/extract-xiso" ] && pin_ok "$PIN_XISO" || return 1
  echo "$("$XTOOL/bin/extract-xiso" -v 2>&1 | grep -m1 -iE 'extract-xiso|version' || echo built)"; }
step_xiso() {
  clone_pin "$PIN_XISO"
  cm_configure -S "$DEPS/extract-xiso" -B "$DEPS/extract-xiso/build" -DCMAKE_BUILD_TYPE=Release
  cm_build "$DEPS/extract-xiso/build"
  x install -D -m 755 "$DEPS/extract-xiso/build/extract-xiso" "$XTOOL/bin/extract-xiso"
}

desc_python="repo .venv (system site-packages) + capstone 5.0.7 built from source; numpy/Pillow for test tools"
check_python() {
  [ -x "$VENV/bin/python" ] || return 1
  "$VENV/bin/python" -c 'import capstone,numpy,PIL; assert capstone.__version__=="5.0.7"' 2>/dev/null || return 1
  echo "$("$VENV/bin/python" -c 'import sys,capstone,numpy,PIL;print("python",sys.version.split()[0],"capstone",capstone.__version__,"numpy",numpy.__version__,"Pillow",PIL.__version__)')"
}
step_python() {
  [ -x "$VENV/bin/python" ] || x python3 -m venv --system-site-packages "$VENV"
  if [ "$DRY" = 1 ] || ! "$VENV/bin/python" -c 'import capstone; assert capstone.__version__=="5.0.7"' 2>/dev/null; then
    x "$VENV/bin/python" -m pip install "$DEPS/capstone-5.0.7/bindings/python" \
      || { net_check https://pypi.org/simple/setuptools/ "pip build dependencies"; return 1; }
  fi
  if [ "$DRY" = 1 ] || ! "$VENV/bin/python" -c 'import numpy,PIL' 2>/dev/null; then
    x "$VENV/bin/python" -m pip install numpy Pillow || { net_check https://pypi.org/simple/numpy/ pip; return 1; }
  fi
}

desc_rust="Rust toolchain pinned by third_party/dsp56300/rust-toolchain.toml + aarch64-linux-android target (rustup)"
rust_channel() { sed -n 's/^channel *= *"\(.*\)"/\1/p' "$REPO/third_party/dsp56300/rust-toolchain.toml" 2>/dev/null; }
check_rust() {
  local c; c="$(rust_channel)"; [ -n "$c" ] && have rustup || return 1
  rustup run "$c" rustc --version >/dev/null 2>&1 || return 1
  rustup +"$c" target list --installed 2>/dev/null | grep -qx aarch64-linux-android || return 1
  echo "$(rustup run "$c" rustc --version), targets: $(rustup +"$c" target list --installed | tr '\n' ' ')"
}
step_rust() {
  local c; c="$(rust_channel)"; [ -n "$c" ] || die "no channel in third_party/dsp56300/rust-toolchain.toml (run the repo step)"
  if ! have rustup; then
    net_check https://static.rust-lang.org/dist/channel-rust-stable.toml.sha256 rustup || return 1
    x mkdir -p "$DL"
    x curl -fsSL --retry 3 -o "$DL/rustup-init" https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init
    x chmod +x "$DL/rustup-init"
    x "$DL/rustup-init" -y --profile minimal --default-toolchain none --no-modify-path
  fi
  x rustup toolchain install "$c" --profile minimal -c rustfmt -c clippy -t aarch64-linux-android \
    || { net_check https://static.rust-lang.org/dist/channel-rust-stable.toml.sha256 rustup; return 1; }
}

desc_dsp="DSP56300 Rust core (cranelift JIT) for aarch64-linux-android and x86_64 -> <repo>/build-dsp"
DSP_A64="$REPO/build-dsp/aarch64-linux-android/release/libdsp56300_emu_ffi.a"
DSP_X86="$REPO/build-dsp/x86_64-unknown-linux-gnu/release/libdsp56300_emu_ffi.a"
check_dsp() { [ -f "$DSP_A64" ] && [ -f "$DSP_X86" ] || return 1
  echo "aarch64 $(du -m "$DSP_A64" | cut -f1) MB, x86_64 $(du -m "$DSP_X86" | cut -f1) MB"; }
step_dsp() {
  local d="$REPO/third_party/dsp56300"
  export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$ANDROID_CC" CC_aarch64_linux_android="$ANDROID_CC" \
         AR_aarch64_linux_android="$NDK_BIN/llvm-ar"
  local t
  for t in aarch64-linux-android x86_64-unknown-linux-gnu; do
    x bash -c "cd '$d' && cargo build --release --locked --no-default-features -p dsp56300-emu-ffi --target $t --target-dir '$REPO/build-dsp' -j $JOBS" \
      || { net_check https://index.crates.io/config.json "cargo (crates.io)"; return 1; }
  done
}

desc_swiftshader="SwiftShader Vulkan ICD (Subzero; vk_swiftshader only) -> <build>/swiftshader"
build_vkprobe() { [ -x "$VKPROBE" ] && [ "$VKPROBE" -nt "$HELPERS/vkprobe.c" ] && return 0
  [ "$DRY" = 0 ] || return 1
  mkdir -p "$STATE/bin"; cc -O1 -I"$DEPS/Vulkan-Headers/include" "$HELPERS/vkprobe.c" -o "$VKPROBE" -l:libvulkan.so.1; }
check_swiftshader() {
  pin_ok "$PIN_SWIFTSHADER" && [ -f "$SS_ICD" ] && [ -f "$BUILD/swiftshader/Linux/libvk_swiftshader.so" ] || return 1
  if ! build_vkprobe >/dev/null 2>&1; then [ "$DRY" = 1 ] && { echo "ICD built (vkprobe not run in dry-run)"; return 0; }
    echo "vkprobe did not build"; return 1; fi
  local o; o="$(VK_ICD_FILENAMES="$SS_ICD" "$VKPROBE" 2>&1)" || { echo "vkprobe: $o"; return 1; }
  echo "$o" | head -1
}
step_swiftshader() {
  clone_pin "$PIN_SWIFTSHADER"
  cm_configure -S "$DEPS/swiftshader" -B "$BUILD/swiftshader" -DCMAKE_BUILD_TYPE=Release \
    -DSWIFTSHADER_BUILD_TESTS=OFF -DSWIFTSHADER_BUILD_BENCHMARKS=OFF -DSWIFTSHADER_BUILD_PVR=OFF \
    -DREACTOR_BACKEND=Subzero -DSWIFTSHADER_WARNINGS_AS_ERRORS=OFF -DSWIFTSHADER_ENABLE_ASTC=OFF
  cm_build "$BUILD/swiftshader" --target vk_swiftshader
}

desc_vvl="Vulkan validation layer v1.4.367 + SPIRV-Headers/Tools, Vulkan-Headers, VUL -> <deps>/vvl-prefix"
VVL_P="$DEPS/vvl-prefix"
check_vvl() {
  local j="$VVL_P/layer.d/VkLayer_khronos_validation.json"
  [ -f "$VVL_P/lib/libVkLayer_khronos_validation.so" ] && [ -f "$j" ] || return 1
  local msg="layer installed"
  if [ -f "$SS_ICD" ] && build_vkprobe >/dev/null 2>&1; then
    VK_ICD_FILENAMES="$SS_ICD" VK_LAYER_PATH="$VVL_P/layer.d" "$VKPROBE" VK_LAYER_KHRONOS_validation >/dev/null 2>&1 \
      || { echo "instance with VK_LAYER_KHRONOS_validation failed"; return 1; }
    msg="VK_LAYER_KHRONOS_validation loads on SwiftShader"
  fi
  echo "$msg"
}
step_vvl() {
  local B="$DEPS/vvl-build" common=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$VVL_P")
  cm_configure -S "$DEPS/Vulkan-Headers" -B "$B/Vulkan-Headers" "${common[@]}"
  x cmake --install "$B/Vulkan-Headers"
  cm_configure -S "$DEPS/SPIRV-Headers" -B "$B/SPIRV-Headers" "${common[@]}"
  x cmake --install "$B/SPIRV-Headers"
  cm_configure -S "$DEPS/SPIRV-Tools" -B "$B/SPIRV-Tools" "${common[@]}" -DSPIRV-Headers_SOURCE_DIR="$DEPS/SPIRV-Headers" \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON
  cm_build "$B/SPIRV-Tools"; x cmake --install "$B/SPIRV-Tools"
  cm_configure -S "$DEPS/Vulkan-Utility-Libraries" -B "$B/VUL" "${common[@]}" -DCMAKE_PREFIX_PATH="$VVL_P" \
    -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF
  cm_build "$B/VUL"; x cmake --install "$B/VUL"
  cm_configure -S "$DEPS/Vulkan-ValidationLayers" -B "$B/VVL" "${common[@]}" -DCMAKE_PREFIX_PATH="$VVL_P" \
    -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF -DUSE_MIMALLOC=OFF -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF \
    -DBUILD_WSI_XLIB_XRANDR_SUPPORT=OFF -DBUILD_WSI_WAYLAND_SUPPORT=OFF
  cm_build "$B/VVL"; x cmake --install "$B/VVL"
  # The installed manifest names the library relatively; the system loader (1.3.275) wants an absolute path.
  x mkdir -p "$VVL_P/layer.d"
  [ "$DRY" = 1 ] || python3 - "$VVL_P" <<'EOF'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p / "share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
d = json.loads(src.read_text())
d["layer"]["library_path"] = str(p / "lib/libVkLayer_khronos_validation.so")
(p / "layer.d/VkLayer_khronos_validation.json").write_text(json.dumps(d, indent=4) + "\n")
EOF
}

# ---- test suites (always run; failures are reported, not fatal)
ss_args() { [ -f "$SS_ICD" ] && echo "-DKVK_TEST_ICD=$SS_ICD"; }
desc_t_xrshell="tests/xr_shell (shell math/input/hooks/settings/prompts; no GPU) -> <build>/xrt"
step_t_xrshell() {
  cm_configure -S "$REPO/tests/xr_shell" -B "$BUILD/xrt" -DCMAKE_BUILD_TYPE=Release
  cm_build "$BUILD/xrt"; ctest_run "$BUILD/xrt"
}
desc_t_nv2ax86="tests/nv2a_arm64 on x86-64 (AVX2 reference dumps, SSE parity, TSan) -> <build>/nv2a-x86"
step_t_nv2ax86() {
  x env CC=gcc cmake -G Ninja -S "$REPO/tests/nv2a_arm64" -B "$BUILD/nv2a-x86"
  cm_build "$BUILD/nv2a-x86"; ctest_run "$BUILD/nv2a-x86"
}
desc_t_nv2aa64="tests/nv2a_arm64 on arm64 under QEMU (bit parity with the x86 refs) -> <build>/nv2a-a64"
step_t_nv2aa64() {
  cm_configure -S "$REPO/tests/nv2a_arm64" -B "$BUILD/nv2a-a64" -DCMAKE_TOOLCHAIN_FILE="$STATIC_TC" \
    -DNV2A_PARITY_REF_DIR="$BUILD/nv2a-x86/ref"
  cm_build "$BUILD/nv2a-a64"; ctest_run "$BUILD/nv2a-a64"
}
desc_t_kvk="tests/kelvin_vk native (stereo, shadergen, backend on SwiftShader) -> <build>/kvk-tests"
step_t_kvk() {
  # shellcheck disable=SC2046
  cm_configure -S "$REPO/tests/kelvin_vk" -B "$BUILD/kvk-tests" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DKVK_GLSLANG_DIR="$DEPS/glslang" -DKVK_VULKAN_HEADERS="$DEPS/Vulkan-Headers/include" $(ss_args)
  cm_build "$BUILD/kvk-tests"; ctest_run "$BUILD/kvk-tests"
}
desc_t_kvka64="tests/kelvin_vk arm64 (kvk-stereo/shadergen under QEMU, -Wl,--no-undefined link check) -> <build>/kvk-android"
step_t_kvka64() {
  cm_configure -S "$REPO/tests/kelvin_vk" -B "$BUILD/kvk-android" -DCMAKE_TOOLCHAIN_FILE="$STATIC_TC" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DKVK_GLSLANG_DIR="$DEPS/glslang"
  cm_build "$BUILD/kvk-android"; ctest_run "$BUILD/kvk-android"
}
desc_t_vrglue="tests/vr_glue (shell + renderer glue + Kelvin backend on SwiftShader, fake OpenXR) -> <build>/vr-glue"
step_t_vrglue() {
  # shellcheck disable=SC2046
  cm_configure -S "$REPO/tests/vr_glue" -B "$BUILD/vr-glue" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DKVK_GLSLANG_DIR="$DEPS/glslang" -DKVK_VULKAN_HEADERS="$DEPS/Vulkan-Headers/include" \
    -DOPENXR_SDK_DIR="$DEPS/OpenXR-SDK" -DGLSLC="$ANDROID_NDK/shader-tools/linux-x86_64/glslc" $(ss_args)
  cm_build "$BUILD/vr-glue"; ctest_run "$BUILD/vr-glue"
}
desc_t_apkshell="port/quest/build_apk.sh without the game (shell + test scene APK) -> <build>/quest-shell"
apk_env() { env XTOOL_ENV="$XTOOL/env.sh" OPENXR_SDK="$DEPS/OpenXR-SDK" LOADER_BUILD="$DEPS/build-openxr-android" "$@"; }
step_t_apkshell() {
  x apk_env BUILD_DIR="$BUILD/quest-shell" "$REPO/port/quest/build_apk.sh"
  x "$ANDROID_BUILD_TOOLS/apksigner" verify --print-certs "$BUILD/quest-shell/blackvr.apk"
}

# ---- game (only with game files; the code generated from them stays local)
desc_g_stage="the user's game files -> <repo>/game (existing game/ is never modified)"
expected_xbe() { sed -n 's/^EXPECTED_XBE *= *"\([0-9A-Fa-f]*\)".*/\1/p' "$REPO/scripts/generate.py" | head -1; }
check_g_stage() {
  [ -f "$REPO/game/default.xbe" ] || return 1
  local want got; want="$(expected_xbe)"; got="$(sha256sum "$REPO/game/default.xbe" | awk '{print toupper($1)}')"
  if [ -n "$want" ] && [ "$got" != "${want^^}" ]; then echo "default.xbe sha256 $got is not the supported retail XBE"; return 1; fi
  echo "game/ ready ($(find -L "$REPO/game" -type f | wc -l) files, default.xbe sha256 ok)"
}
step_g_stage() {
  local g="$REPO/game" src="$GAME_SRC" stage="$STATE/game-stage" xbe
  [ -f "$g/default.xbe" ] && { say "  using the existing $g (not modified)"; return 0; }
  [ -n "$src" ] || die "no game files: pass --game <dir|iso|url>"
  if [ -e "$g" ] && { [ -L "$g" ] || [ -n "$(ls -A "$g" 2>/dev/null)" ]; }; then
    die "$g exists without default.xbe; move it away first (it is never modified by this script)"
  fi
  x mkdir -p "$stage"
  if [[ "$src" =~ ^https?:// ]]; then
    local host; host="$(sed -E 's#^https?://([^/]+).*#\1#' <<<"$src")"
    net_check "https://$host/" "the game-file link" || die "the game link's host ($host) is not reachable from this workspace; attach the .iso to the session instead"
    x curl -fL --retry 3 -C - -o "$stage/download" "$src"
    src="$stage/download"
  fi
  if [ -f "$src" ] && [ "$DRY" = 0 ] && file -b "$src" | grep -qi 'zip archive'; then
    x rm -rf "$stage/unzip"; x unzip -q "$src" -d "$stage/unzip"; src="$stage/unzip"
  fi
  if [ -d "$src" ]; then
    xbe="$(find -L "$src" -maxdepth 3 -iname default.xbe | head -1)"
    if [ -z "$xbe" ]; then   # a folder holding a disc image
      local iso; iso="$(find "$src" -maxdepth 2 -type f \( -iname '*.iso' -o -iname '*.xiso' \) | head -1)"
      [ -n "$iso" ] || die "no default.xbe or .iso/.xiso found in $src"
      src="$iso"
    else
      local gd; gd="$(dirname "$xbe")"
      if [ "$COPY_GAME" = 1 ] || [[ "$gd" == "$stage"* ]]; then x cp -a "$gd" "$g"
      else x ln -s "$gd" "$g"; fi
    fi
  fi
  if [ -f "$src" ]; then   # disc image
    [ -x "$XTOOL/bin/extract-xiso" ] || step_xiso
    x rm -rf "$REPO/.xiso_tmp"; x mkdir -p "$REPO/.xiso_tmp"
    x "$XTOOL/bin/extract-xiso" -d "$REPO/.xiso_tmp" -x "$src"
    if [ "$DRY" = 0 ]; then
      xbe="$(find "$REPO/.xiso_tmp" -maxdepth 3 -iname default.xbe | head -1)"
      [ -n "$xbe" ] || die "extraction did not produce default.xbe. Is this the BLACK disc image?"
      mv "$(dirname "$xbe")" "$g"
    fi
    x rm -rf "$REPO/.xiso_tmp"
  fi
  x rm -rf "$stage"
  # /game/ in .gitignore matches a directory only; keep a symlinked game out of `git status` locally too.
  if [ -L "$g" ] && [ -d "$REPO/.git" ] && ! grep -qx '/game' "$REPO/.git/info/exclude" 2>/dev/null; then
    x mkdir -p "$REPO/.git/info"; [ "$DRY" = 1 ] || echo '/game' >>"$REPO/.git/info/exclude"
  fi
}
desc_g_generate="scripts/generate.py: recompile the user's default.xbe into src/recomp/gen (~30 s)"
check_g_generate() { [ "$REGEN" = 0 ] && [ -f "$REPO/src/recomp/gen/recomp_dispatch.c" ] || return 1
  echo "src/recomp/gen present ($(ls "$REPO"/src/recomp/gen/recomp_[0-9]*.c 2>/dev/null | wc -l) chunks); --regenerate to redo"; }
step_g_generate() { x bash -c "cd '$REPO' && '$VENV/bin/python' scripts/generate.py"; }
desc_g_android="port/android with BLACK_ANDROID_GAME=ON (game + runtime archives, arm64) -> <repo>/build-game-android"
step_g_android() {
  cm_configure -S "$REPO/port/android" -B "$REPO/build-game-android" -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DBLACK_ANDROID_GAME=ON -DCMAKE_BUILD_TYPE=Release \
    -DSSE2NEON_DIR="$DEPS/sse2neon" -DKVK_GLSLANG_DIR="$DEPS/glslang" -DDSP56300_LIBRARY="$DSP_A64"
  cm_build "$REPO/build-game-android"
}
desc_g_boot="tests/game_boot_arm64: boot the game under QEMU to its first GPU touch -> <repo>/build-boot-a64"
step_g_boot() {
  cm_configure -S "$REPO/tests/game_boot_arm64" -B "$REPO/build-boot-a64" -DCMAKE_TOOLCHAIN_FILE="$STATIC_TC" \
    -DBLACK_GAME_LIB_DIR="$REPO/build-game-android/lib" -DDSP56300_LIBRARY="$DSP_A64"
  cm_build "$REPO/build-boot-a64"; ctest_run "$REPO/build-boot-a64"
}
desc_g_apk="build_apk.sh --with-game (libblackvr.so + loader, signed with <xtool>/smoke/dbg.jks) -> <repo>/build-quest-game"
step_g_apk() {
  x apk_env BUILD_DIR="$REPO/build-quest-game" "$REPO/port/quest/build_apk.sh" --with-game
  x "$ANDROID_BUILD_TOOLS/apksigner" verify --print-certs "$REPO/build-quest-game/blackvr.apk"
}
desc_g_render="tests/game_render_x86 configure+build (headless game on SwiftShader; run it with build-render-x86/run.sh)"
step_g_render() {
  # shellcheck disable=SC2046
  x env CC=clang CXX=clang++ cmake -G Ninja -S "$REPO/tests/game_render_x86" -B "$REPO/build-render-x86" \
    -DCMAKE_BUILD_TYPE=Release -DKVK_GLSLANG_DIR="$DEPS/glslang" -DKVK_VULKAN_HEADERS="$DEPS/Vulkan-Headers/include" \
    -DOPENXR_SDK_DIR="$DEPS/OpenXR-SDK" -DGLSLC="$ANDROID_NDK/shader-tools/linux-x86_64/glslc" $(ss_args) \
    -DDSP56300_LIBRARY="$DSP_X86"
  cm_build "$REPO/build-render-x86"
}

# ------------------------------------------------------------------------------------------------ driver
SETUP_STEPS=(preflight apt repo android keystore smokeapk deps qemu arm64selftest openxr xiso python rust dsp swiftshader vvl)
TEST_STEPS=(t_xrshell t_nv2ax86 t_nv2aa64 t_kvk t_kvka64 t_vrglue t_apkshell)
GAME_STEPS=(g_stage g_generate g_android g_boot g_apk g_render)
ALL_STEPS=("${SETUP_STEPS[@]}" "${TEST_STEPS[@]}" "${GAME_STEPS[@]}")

if [ "$LIST" = 1 ]; then
  for s in "${ALL_STEPS[@]}"; do d="desc_$s"; printf '  %-14s %s\n' "$s" "${!d}"; done; exit 0
fi
for s in ${ONLY//,/ } ${SKIPS//,/ } ${FORCE//,/ }; do
  in_list "$s" "$(IFS=,; echo "${ALL_STEPS[*]}")" || die "unknown step '$s' (see --list)"
done

game_wanted() { [ -n "$GAME_SRC" ] || [ -f "$REPO/game/default.xbe" ]; }
skip_reason() { # prints why a step is not selected
  local s="$1"
  if [ -n "$ONLY" ]; then in_list "$s" "$ONLY" || { echo "not in --only"; return; }; fi
  in_list "$s" "$SKIPS" && { echo "--skip"; return; }
  case "$s" in
    swiftshader) [ "$SKIP_SS" = 1 ] && echo "--skip-swiftshader" ;;
    vvl) [ "$SKIP_VVL" = 1 ] && echo "--skip-vvl" ;;
    t_*) [ "$DEPS_ONLY" = 1 ] && echo "--deps-only" ;;
    g_render) [ "$DEPS_ONLY" = 1 ] && echo "--deps-only"; [ "$RENDER_X86" = 1 ] || echo "needs --with-render-x86"
              game_wanted || echo "no game files (--game)" ;;
    g_*) [ "$DEPS_ONLY" = 1 ] && echo "--deps-only"; game_wanted || echo "no game files (--game)" ;;
  esac
  return 0
}

declare -A STATUS DETAIL TIME
FAILED=0 GAME_BLOCKED="" STEP_PID=""
kill_tree() { local c; for c in $(pgrep -P "$1" 2>/dev/null); do kill_tree "$c"; done; kill -TERM "$1" 2>/dev/null || true; }
on_signal() {
  trap - INT TERM
  [ -n "$STEP_PID" ] && kill_tree "$STEP_PID"
  say ""; say "${R}interrupted${N}: the current step's processes were stopped. Re-run the same command to resume."
  exit 130
}
trap on_signal INT TERM
[ "$DRY" = 1 ] || mkdir -p "$LOGS" "$DL" 2>/dev/null || true
fix_no_proxy
say "${B}BLACK VR Quest setup${N}  repo=$REPO  prefix=$PREFIX (deps=$DEPS xtool=$XTOOL build=$BUILD)$([ "$DRY" = 1 ] && echo '  [DRY RUN]')"
say "logs: $LOGS"
total=${#ALL_STEPS[@]} i=0
for s in "${ALL_STEPS[@]}"; do
  i=$((i + 1)); d="desc_$s"; why="$(skip_reason "$s" | head -1)"
  if [ -n "$why" ]; then STATUS[$s]="skipped"; DETAIL[$s]="$why"; continue; fi
  if [[ "$s" == g_* ]] && [ -n "$GAME_BLOCKED" ]; then STATUS[$s]="blocked"; DETAIL[$s]="after $GAME_BLOCKED failed"; continue; fi
  say ""; say "${B}==> [$i/$total] $s${N}: ${!d}"
  t0=$SECONDS log="$LOGS/$s.log"
  if declare -F "check_$s" >/dev/null && ! in_list "$s" "$FORCE" && det="$(check_$s 2>/dev/null)"; then
    STATUS[$s]="done"; DETAIL[$s]="$det"; TIME[$s]=0; say "    ${G}already done${N}: $det"; continue
  fi
  if [ "$DRY" = 1 ]; then
    ( set -Eeuo pipefail; "step_$s" ) 2>&1 | sed 's/^/    /' || true
    STATUS[$s]="dry-run"; DETAIL[$s]="(nothing changed)"; TIME[$s]=0; continue
  fi
  say "    log: $log"
  echo "=== $(date -Is) $s" >>"$log"
  set +e
  # In the background + wait, so an INT/TERM reaches the trap below at once (and the step's children die with it).
  ( set -Eeuo pipefail; trap 'echo "FAILED (line $LINENO): $BASH_COMMAND" >&2' ERR; "step_$s" ) >>"$log" 2>&1 &
  STEP_PID=$!
  wait "$STEP_PID"; rc=$?
  STEP_PID=""
  set -e
  TIME[$s]=$((SECONDS - t0))
  if [ "$rc" = 0 ] && declare -F "check_$s" >/dev/null; then
    det="$(check_$s 2>&1)" || rc=99
  else
    det="$(ctest_detail "$log")"
    [ -n "$det" ] || det="$(grep -vE '^(\+ |=== |WARNING)' "$log" | grep -v '^ *$' | tail -1 | sed 's/^ *//' | cut -c1-110)"
    [ -n "$det" ] || det="ok"
  fi
  if [ "$rc" = 0 ]; then
    STATUS[$s]="ok"; DETAIL[$s]="$det"; say "    ${G}ok${N} (${TIME[$s]}s): $det"
  else
    FAILED=$((FAILED + 1)); STATUS[$s]="FAILED"
    [ "$rc" = 99 ] && DETAIL[$s]="step ran but its check failed: $det" || DETAIL[$s]="exit $rc, see $log"
    say "    ${R}FAILED${N} (${TIME[$s]}s): ${DETAIL[$s]}"; tail -n 25 "$log" | sed 's/^/    | /'
    case "$s" in
      t_*) ;;                                   # report and continue with the other suites
      g_*) GAME_BLOCKED="$s" ;;
      *) say "    a setup step failed; fix it and re-run (finished steps are skipped)."; break ;;
    esac
  fi
done

say ""; say "${B}Summary${N}"
for s in "${ALL_STEPS[@]}"; do
  st="${STATUS[$s]:-not run}"
  case "$st" in ok|done) c=$G ;; FAILED) c=$R ;; *) c=$Y ;; esac
  t="${TIME[$s]:-}"; [ -n "$t" ] && [ "$t" != 0 ] && t="${t}s" || t=""
  printf '  %-14s %s%-8s%s %6s  %s\n' "$s" "$c" "$st" "$N" "$t" "${DETAIL[$s]:-}"
done
say ""
say "Use the environment: . $XTOOL/env.sh   (notes: port/notes/setup.md)"
[ "$FAILED" = 0 ] || { say "${R}$FAILED step(s) failed.${N}"; exit 1; }
