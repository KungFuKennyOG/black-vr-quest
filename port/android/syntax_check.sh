#!/usr/bin/env bash
# Syntax-check C sources for aarch64-linux-android29 without an NDK.
# Uses clang's own builtin headers plus bionic headers cloned into /root/deps.
# Usage: port/android/syntax_check.sh [extra clang args] file.c ...
set -u
BIONIC="${BIONIC:-/root/deps/platform_bionic/libc}"
API="${ANDROID_API:-29}"
RES="$(clang -print-resource-dir)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
XR="$ROOT/third_party/xboxrecomp"
DEPS_INC=()
for d in /root/deps/aaudio_include /root/deps/android_system_logging/liblog/include \
         /root/deps/android_frameworks_native/include; do
  [ -d "$d" ] && DEPS_INC+=(-isystem "$d")
done
exec clang --target=aarch64-linux-android"$API" -fsyntax-only -std=gnu11 \
  -nostdinc -isystem "$RES/include" \
  -isystem "$BIONIC/include" -isystem "$BIONIC/kernel/uapi" \
  -isystem "$BIONIC/kernel/uapi/asm-arm64" -isystem "$BIONIC/kernel/android/uapi" \
  "${DEPS_INC[@]}" \
  -D__ANDROID_API__="$API" -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -I"$XR/src" -I"$XR/src/platform" -I"$XR/src/kernel" -I"$XR/src/apu" \
  -I"$XR/src/audio" -I"$XR/src/nv2a" -I"$XR/templates/runtime" -I"$XR/include" \
  -I"$ROOT/third_party/dsp56300/crates/emu-ffi/include" \
  -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
  -Wno-unused-but-set-variable "$@"
