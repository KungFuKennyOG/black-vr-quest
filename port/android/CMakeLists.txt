# BLACK Quest port -- Android arm64 runtime libraries (arm64-runtime stream).
#
# Builds the XboxRecomp runtime that the recompiled game links against, as
# static libraries for arm64-v8a, with the NDK:
#
#   . /root/xtool/env.sh
#   cmake -G Ninja -S port/android -B build/android \
#         -DCMAKE_TOOLCHAIN_FILE=$ANDROID_TOOLCHAIN_FILE \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
#   cmake --build build/android
#
# Or with /root/xtool/android-arm64-static.cmake to also run the arm64 tests
# under /root/xtool/arm64run via ctest (-DBLACK_ANDROID_TESTS=ON, default on
# when CMAKE_CROSSCOMPILING_EMULATOR is set).
#
# The runtime's own src/*/CMakeLists.txt are written for Windows and desktop
# Linux (SDL2, OpenSSL, the Rust DSP core built for x86_64); none of those
# exist for Android here, so this file lists the sources itself and leaves
# those files untouched.
#
# Targets (static archives under <build>/lib):
#   xr_platform   win32_compat (POSIX Win32 primitives) + arm64_fault_decode
#   xr_kernel     Xbox kernel HLE + memory layout (needs xr_platform)
#   xr_apu        MCPX APU emulation + AAudio host output
#   xr_dsound     DirectSound compatibility
#   xr_runtime    INTERFACE target linking all of the above + aaudio/log/m
#   black_game    OPTIONAL (-DBLACK_ANDROID_GAME=ON): the game, see game.cmake,
#                 which also defines xr_input and the xboxrecomp umbrella
#
# Deliberately NOT here (other streams own them, or they are Windows-only):
#   src/nv2a/**, src/d3d/**, src/video/** (renderer stream)
#   src/usb/** and src/input/** (need the input/XR shell; usb_gamepad pulls in
#     pc_input.h/SDL); kernel references to them are satisfied by the renderer
#     and shell at final link (see port/notes/arm64-runtime.md)
#   apu_mmio_hook.c is compiled (arm64 path by the fault stream) but its
#     registration lives in src/main.c.
cmake_minimum_required(VERSION 3.20)
project(black_android_runtime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # archives end up inside libmain.so

if(NOT ANDROID)
    message(FATAL_ERROR "Configure with the NDK toolchain (CMAKE_TOOLCHAIN_FILE=.../android.toolchain.cmake)")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported (got ${ANDROID_ABI})")
endif()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XR "${BLACK_ROOT}/third_party/xboxrecomp")
set(SSE2NEON_DIR "/root/deps/sse2neon" CACHE PATH "Directory holding sse2neon.h")
if(NOT EXISTS "${SSE2NEON_DIR}/sse2neon.h")
    message(FATAL_ERROR "sse2neon.h not found in SSE2NEON_DIR=${SSE2NEON_DIR}")
endif()

# ---- common flags -----------------------------------------------------------
# armv8.2-a: Quest 3 (XR2 Gen 2, Cortex-A78C/A55) is v8.2. It makes atomics
#   inline LSE (LDADD/CAS/SWP), which the arm64 MMIO fault emulator handles;
#   LDXR/STXR exclusives on device memory are refused there.
# -ffp-contract=off: no fused multiply-add the x86 build never had (also a
#   pragma in recomp_types.h; this covers GCC-style flags and other TUs).
# -fno-strict-aliasing: as the Windows build; the runtime type-puns freely.
# NOT -fms-volatile: it makes every MEM32 an LDAR/STLR, which fault (SIGBUS)
#   on the misaligned guest accesses x86 allows -- verified under QEMU.
set(XR_ARM64_FLAGS
    -march=armv8.2-a
    -ffp-contract=off
    -fno-strict-aliasing
    -fwrapv
    -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-format -Wno-comment
)
set(XR_DEFS WIN32_LEAN_AND_MEAN NOMINMAX SSE2NEON_PRECISE_MINMAX=1)

# ---- platform ---------------------------------------------------------------
add_library(xr_platform STATIC
    ${XR}/src/platform/win32_compat.c
    ${XR}/src/platform/arm64_fault_decode.c
)
target_include_directories(xr_platform PUBLIC
    ${XR}/src ${XR}/src/platform ${XR}/templates/runtime ${XR}/include ${SSE2NEON_DIR})
target_compile_definitions(xr_platform PUBLIC ${XR_DEFS})
target_compile_options(xr_platform PRIVATE ${XR_ARM64_FLAGS} -O2)
# liblog / libandroid exist only as shared libraries, which a static test
# executable cannot use; the arm64 tests provide link stubs for them instead.
option(XR_LINK_ANDROID_SYSLIBS "Link liblog/libandroid/libaaudio (off for static test exes)" ON)
if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(XR_LINK_ANDROID_SYSLIBS OFF)
endif()
if(XR_LINK_ANDROID_SYSLIBS)
    target_link_libraries(xr_platform PUBLIC log android)
endif()

# ---- kernel -----------------------------------------------------------------
set(XR_KERNEL_SOURCES
    kernel_path.c kernel_pool.c kernel_rtl.c kernel_memory.c kernel_file.c
    kernel_thread.c kernel_sync.c kernel_hal.c kernel_xbox.c kernel_ob.c
    kernel_io.c kernel_crypto.c kernel_thunks.c kernel_bridge.c
    xbox_memory_layout.c memory_watch.c icall_feedback.c recomp_trace.c
    nv2a_pb_scan.c nv2a_pb_exec.c thread_placement.c
)
list(TRANSFORM XR_KERNEL_SOURCES PREPEND ${XR}/src/kernel/)
add_library(xr_kernel STATIC ${XR_KERNEL_SOURCES})
target_include_directories(xr_kernel PUBLIC ${XR}/src/kernel)
target_link_libraries(xr_kernel PUBLIC xr_platform dl)   # dladdr (memory_watch)
target_compile_options(xr_kernel PRIVATE ${XR_ARM64_FLAGS} -O2)

# ---- APU + host audio -------------------------------------------------------
set(DSP56300_INCLUDE "${BLACK_ROOT}/third_party/dsp56300/crates/emu-ffi/include")
add_library(xr_apu STATIC
    ${XR}/src/apu/apu_core.c
    ${XR}/src/apu/apu_vp.c
    ${XR}/src/apu/apu_dsp.c
    ${XR}/src/apu/apu_dma.c
    ${XR}/src/apu/apu_dsp_engine.c
    ${XR}/src/apu/apu_mmio_hook.c
    ${XR}/src/apu/apu_xaudio2.c      # compiles to nothing on Android
    ${XR}/src/apu/apu_aaudio.c       # the Android backend
)
target_include_directories(xr_apu PUBLIC ${XR}/src/apu ${DSP56300_INCLUDE})
target_include_directories(xr_apu PRIVATE ${XR}/src/nv2a)   # qemu_shim.h only
target_link_libraries(xr_apu PUBLIC xr_platform m)
if(XR_LINK_ANDROID_SYSLIBS)
    target_link_libraries(xr_apu PUBLIC aaudio)
endif()
target_compile_options(xr_apu PRIVATE ${XR_ARM64_FLAGS} -O2)
# The DSP56300 core is Rust (cranelift JIT); see port/notes/dsp-arm64.md for the
# cargo cross-build into build-dsp/. Its arm64 JIT output matches x86-64 bit for
# bit (tests/dsp_arm64).
set(_dsp_default "${BLACK_ROOT}/build-dsp/aarch64-linux-android/release/libdsp56300_emu_ffi.a")
if(NOT EXISTS "${_dsp_default}")
    set(_dsp_default "")
endif()
set(DSP56300_LIBRARY "${_dsp_default}" CACHE FILEPATH "libdsp56300_emu_ffi.a built for aarch64-linux-android")
if(DSP56300_LIBRARY)
    target_link_libraries(xr_apu PUBLIC "${DSP56300_LIBRARY}" dl)
else()
    message(WARNING "DSP56300_LIBRARY not set: dsp56300_* will be undefined at the final link")
endif()

# ---- DirectSound ------------------------------------------------------------
add_library(xr_dsound STATIC
    ${XR}/src/audio/dsound_device.c
    ${XR}/src/audio/wma_decoder.c
)
target_include_directories(xr_dsound PUBLIC ${XR}/src/audio)
target_link_libraries(xr_dsound PUBLIC xr_apu xr_platform)
target_compile_options(xr_dsound PRIVATE ${XR_ARM64_FLAGS} -O2)

# ---- umbrella ---------------------------------------------------------------
add_library(xr_runtime INTERFACE)
target_link_libraries(xr_runtime INTERFACE xr_kernel xr_apu xr_dsound xr_platform)

# ---- NV2A / USB / video (port/android/nv2a.cmake, nv2a stream) ---------------
# Optional until that file exists. It sees XR, SSE2NEON_DIR, XR_ARM64_FLAGS and
# the xr_* targets above.
include("${CMAKE_CURRENT_SOURCE_DIR}/nv2a.cmake" OPTIONAL RESULT_VARIABLE BLACK_NV2A_CMAKE)
if(NOT BLACK_NV2A_CMAKE)
    message(STATUS "port/android/nv2a.cmake not present: no NV2A/USB/video targets")
endif()

# ---- optional: the game (port/android/game.cmake) ------------------------------
# src/recomp/gen is never committed (.gitignored, generated from the user's own
# disc by scripts/generate.py). game.cmake defines black_game, xr_input,
# and xboxrecomp; port/quest's BLACKVR_WITH_GAME build gets
# here through it.
option(BLACK_ANDROID_GAME "Also build the game (src/recomp/gen, main.c, recomp_manual.c)" OFF)
if(BLACK_ANDROID_GAME)
    include("${CMAKE_CURRENT_SOURCE_DIR}/game.cmake")
endif()

# ---- tests (run under /root/xtool/arm64run when the static toolchain is used)
if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(_tests_default ON)
else()
    set(_tests_default OFF)
endif()
option(BLACK_ANDROID_TESTS "Build arm64 runtime tests" ${_tests_default})
if(BLACK_ANDROID_TESTS)
    enable_testing()
    add_subdirectory(${BLACK_ROOT}/tests/arm64_runtime ${CMAKE_BINARY_DIR}/tests/arm64_runtime)
endif()
