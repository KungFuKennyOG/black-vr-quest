#!/usr/bin/env bash
# Builds BLACK VR (com.kenny.blackvr) for Quest 3 without Gradle:
#   1. the Khronos OpenXR loader for arm64-v8a / android-29 (from source, once)
#   2. libblackvr.so with the NDK (port/quest/CMakeLists.txt)
#   3. aapt2 link -> add libs -> zipalign -p 4 -> apksigner (v2+v3)
#
# Usage: port/quest/build_apk.sh [--with-game] [--release] [--clean]
#   --with-game   link the recompiled game/runtime/renderer (BLACKVR_WITH_GAME=ON)
#   --clean       rebuild everything (not the OpenXR loader)
# Env: BLACKVR_KEYSTORE / BLACKVR_KEYSTORE_PASS / BLACKVR_KEY_ALIAS to sign with your
#      own key (default: the debug key /root/xtool/smoke/dbg.jks, pass "android").
#      OPENXR_SDK (default /root/deps/OpenXR-SDK), BUILD_DIR (default build-quest).
#      BLACKVR_SHADER_CACHE (--with-game): a Kelvin shader cache directory to ship as assets/kgpu-cache (SPIR-V and
#      the vertex-program and pipeline manifests, never the driver's vk_pipeline_cache.bin); default the headless
#      run's, build-render-x86/scratch/root/kgpu-cache (port/notes/perf.md); "none" ships none. It is made from the
#      user's own game, like the rest of a --with-game APK: keep it local.
# Output: <repo>/build-quest/blackvr.apk
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ENV_SH="${XTOOL_ENV:-/root/xtool/env.sh}"
[ -f "$ENV_SH" ] && . "$ENV_SH"
: "${ANDROID_NDK:?set ANDROID_NDK (source /root/xtool/env.sh)}"
: "${ANDROID_BUILD_TOOLS:?set ANDROID_BUILD_TOOLS}"
: "${ANDROID_PLATFORM_JAR:?set ANDROID_PLATFORM_JAR}"
TOOLCHAIN="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
OPENXR_SDK="${OPENXR_SDK:-/root/deps/OpenXR-SDK}"
LOADER_BUILD="${LOADER_BUILD:-/root/deps/build-openxr-android}"
BUILD="${BUILD_DIR:-$ROOT/build-quest}"
KS="${BLACKVR_KEYSTORE:-/root/xtool/smoke/dbg.jks}"
KS_PASS="${BLACKVR_KEYSTORE_PASS:-android}"
WITH_GAME=OFF
CLEAN=0
for a in "$@"; do
  case "$a" in
    --with-game) WITH_GAME=ON ;;
    --clean) CLEAN=1 ;;
    --release) ;;   # always Release; kept for symmetry with Gradle
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
STRIP="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
[ "$CLEAN" = 1 ] && rm -rf "$BUILD"
mkdir -p "$BUILD"

# 1. OpenXR loader ------------------------------------------------------------------
LOADER_SO="$LOADER_BUILD/src/loader/libopenxr_loader.so"
if [ ! -f "$LOADER_SO" ]; then
  echo "== building the OpenXR loader (arm64-v8a, android-29)"
  cmake -G Ninja -S "$OPENXR_SDK" -B "$LOADER_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF \
    -DDYNAMIC_LOADER=ON >/dev/null
  cmake --build "$LOADER_BUILD" --target openxr_loader
fi

# 2. native shell ---------------------------------------------------------------------
echo "== building libblackvr.so (game: $WITH_GAME)"
cmake -G Ninja -S "$HERE" -B "$BUILD/native" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DOPENXR_SDK_DIR="$OPENXR_SDK" -DOPENXR_LOADER_SO="$LOADER_SO" -DBLACKVR_WITH_GAME="$WITH_GAME" >/dev/null
cmake --build "$BUILD/native"

# 3. APK ------------------------------------------------------------------------------
echo "== packaging"
STAGE="$BUILD/apk"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib/arm64-v8a"
"$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/libblackvr.so" "$BUILD/native/libblackvr.so"
"$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/libopenxr_loader.so" "$LOADER_SO"
"$ANDROID_BUILD_TOOLS/aapt2" compile --dir "$HERE/res" -o "$BUILD/res.zip"
"$ANDROID_BUILD_TOOLS/aapt2" link -o "$BUILD/base.apk" --manifest "$HERE/AndroidManifest.xml" \
  -I "$ANDROID_PLATFORM_JAR" "$BUILD/res.zip" --min-sdk-version 29 --target-sdk-version 32
# Native libraries stored uncompressed and page-aligned (extractNativeLibs=false).
( cd "$STAGE" && zip -q -0 -r "$BUILD/base.apk" lib )
# The shader cache, compressed (xr_main.c seeds the app's cache dir from it at the first start).
CACHE_SRC="${BLACKVR_SHADER_CACHE:-$ROOT/build-render-x86/scratch/root/kgpu-cache}"
if [ "$WITH_GAME" = ON ] && [ "$CACHE_SRC" != none ] && [ -d "$CACHE_SRC" ]; then
  mkdir -p "$STAGE/assets/kgpu-cache"
  find "$CACHE_SRC" -maxdepth 1 -type f \( -name 'vk_[vf]_*.spv' -o -name 'vk_vs_manifest.bin' -o -name 'vk_pipe_manifest.bin' \) \
    -exec cp {} "$STAGE/assets/kgpu-cache/" \;
  n_assets=$(find "$STAGE/assets/kgpu-cache" -type f | wc -l)
  if [ "$n_assets" -gt 0 ]; then
    ( cd "$STAGE" && zip -q -9 -r "$BUILD/base.apk" assets )
    echo "== shader cache: $n_assets files from $CACHE_SRC ($(du -sh "$STAGE/assets/kgpu-cache" | cut -f1))"
  fi
fi
"$ANDROID_BUILD_TOOLS/zipalign" -f -p 16 "$BUILD/base.apk" "$BUILD/aligned.apk"
SIGN_ARGS=(--ks "$KS" --ks-pass "pass:$KS_PASS")
[ -n "${BLACKVR_KEY_ALIAS:-}" ] && SIGN_ARGS+=(--ks-key-alias "$BLACKVR_KEY_ALIAS")
"$ANDROID_BUILD_TOOLS/apksigner" sign "${SIGN_ARGS[@]}" --v4-signing-enabled false \
  --out "$BUILD/blackvr.apk" "$BUILD/aligned.apk"
"$ANDROID_BUILD_TOOLS/apksigner" verify --verbose "$BUILD/blackvr.apk" | grep -E "Verified using v(2|3)" || true
"$ANDROID_BUILD_TOOLS/zipalign" -c -p 16 "$BUILD/blackvr.apk" && echo "zipalign: OK"
echo "== $BUILD/blackvr.apk ($(du -h "$BUILD/blackvr.apk" | cut -f1))"
