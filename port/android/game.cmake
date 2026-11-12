# BLACK Quest port -- the game itself for aarch64 Android (game stream).
#
# Defines, for the XR shell's final link (port/quest/CMakeLists.txt, BLACKVR_WITH_GAME=ON):
#
#   black_game         STATIC  src/recomp/gen/*.c (the user's locally generated code), src/main.c
#                              (black_game_main), src/recomp_manual.c (+ .inc files, black_vr.inc).
#                              Built with -DBLACK_VR.
#   xr_input           STATIC  xboxrecomp's input layer with the Quest pad backend (-DBLACK_VR, no SDL2)
#   xboxrecomp         INTERFACE  every runtime library the game needs: xr_kernel, xr_apu, xr_dsound,
#                              xr_platform (port/android/CMakeLists.txt), xr_input, and the NV2A / USB /
#                              video targets of port/android/nv2a.cmake when that file defines them
#
# black_vk_renderer (the shell's renderer glue, port/quest/src/vr_renderer_kvk.c) is defined by
# port/quest/CMakeLists.txt itself, after this file, from the kelvin_vk targets of nv2a.cmake.
#
# Two ways in:
#   * port/android/CMakeLists.txt includes this file (-DBLACK_ANDROID_GAME=ON); the runtime
#     targets already exist.
#   * port/quest/CMakeLists.txt includes it as BLACKVR_GAME_CMAKE. The runtime does not exist
#     yet, so this adds port/android as a subdirectory with BLACK_ANDROID_GAME=ON, which comes
#     back here (first case) and defines everything. Targets are global, so the shell sees them.
#
# Supersedes port/quest/cmake/game_targets.cmake: no add_subdirectory(third_party/xboxrecomp)
# (its kernel wants OpenSSL, its input SDL2, its nv2a D3D11), the runtime is xr_kernel & co.
if(TARGET black_game)
    return()
endif()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(NOT TARGET xr_kernel)
    set(BLACK_ANDROID_GAME ON CACHE BOOL "Build the game (port/android/game.cmake)" FORCE)
    # Static test executables only make sense with the static toolchain, not inside the shell.
    set(BLACK_ANDROID_TESTS OFF CACHE BOOL "Build arm64 runtime tests" FORCE)
    add_subdirectory("${BLACK_ROOT}/port/android" "${CMAKE_BINARY_DIR}/black-android")
    return()
endif()

set(XR "${BLACK_ROOT}/third_party/xboxrecomp")
if(NOT SSE2NEON_DIR)
    set(SSE2NEON_DIR /root/deps/sse2neon)
endif()

# Flags shared by every TU that includes x86 intrinsic headers or recomp_types.h.
#   x86shim: <xmmintrin.h> & co. -> sse2neon; SSE2NEON_PRECISE_MINMAX: MINPS/MAXPS operand order.
#   armv8.2-a: inline LSE atomics (the MMIO fault emulator refuses LDXR/STXR).
#   -ffp-contract=off: no fused multiply-add the x86 build never had.
#   No -fms-volatile: it turns MEM32 into LDAR/STLR, which SIGBUS on misaligned guest accesses.
set(BLACK_A64_INCLUDES
    "${BLACK_ROOT}/port/android/include"
    "${SSE2NEON_DIR}")
set(BLACK_A64_FLAGS
    -isystem "${BLACK_ROOT}/port/android/x86shim"
    -march=armv8.2-a -ffp-contract=off -fno-strict-aliasing -fwrapv)
set(BLACK_A64_DEFS SSE2NEON_PRECISE_MINMAX=1 WIN32_LEAN_AND_MEAN NOMINMAX)

# ---- input: xinput_device.c (Quest pad backend), pc_input.c, pc_controls.c ----------------
add_library(xr_input STATIC
    ${XR}/src/input/xinput_device.c
    ${XR}/src/input/pc_input.c
    ${XR}/src/input/pc_controls.c
)
target_include_directories(xr_input PUBLIC ${XR}/src/input)
target_include_directories(xr_input PRIVATE ${BLACK_A64_INCLUDES}
    "${BLACK_ROOT}/port/quest" "${BLACK_ROOT}/port/quest/src")
target_compile_definitions(xr_input PRIVATE BLACK_VR ${BLACK_A64_DEFS})
target_compile_options(xr_input PRIVATE ${BLACK_A64_FLAGS} -O2 -Wall -Wno-unused-parameter
    -Wno-unused-function -Wno-comment)
target_link_libraries(xr_input PUBLIC xr_platform)

# ---- NV2A / USB / video (agent "nv2a" stream) ----------------------------------------------
# nv2a.cmake is included by port/android/CMakeLists.txt before this file. It names its targets
# in BLACK_NV2A_TARGETS (preferred) or we look for the usual names.
set(_black_hw_targets)
if(DEFINED BLACK_NV2A_TARGETS)
    list(APPEND _black_hw_targets ${BLACK_NV2A_TARGETS})
else()
    foreach(t xr_nv2a xr_usb xr_video xbox_nv2a xbox_usb xbox_video)
        if(TARGET ${t})
            list(APPEND _black_hw_targets ${t})
        endif()
    endforeach()
endif()
message(STATUS "black_game: NV2A/USB/video targets: ${_black_hw_targets}")

# ---- the runtime umbrella the shell links ----------------------------------------------------
add_library(xboxrecomp INTERFACE)
target_link_libraries(xboxrecomp INTERFACE xr_kernel xr_apu xr_dsound xr_input xr_platform
                      ${_black_hw_targets} m dl)

# ---- the game ---------------------------------------------------------------------------------
file(GLOB BLACK_GEN_SOURCES CONFIGURE_DEPENDS "${BLACK_ROOT}/src/recomp/gen/*.c")
if(NOT BLACK_GEN_SOURCES)
    message(FATAL_ERROR "src/recomp/gen is empty: generate the recompiled code from your own "
                        "default.xbe first (scripts/generate.py)")
endif()
file(STRINGS "${BLACK_ROOT}/VERSION" BLACK_VERSION_STRING LIMIT_COUNT 1)

# The generated chunks are ~64 MB of C. Kept as their own object library so they do not
# rebuild when main.c / recomp_manual.c change, and so the warnings stay off for them only
# (the Windows build compiles them with /w too).
add_library(black_gen OBJECT ${BLACK_GEN_SOURCES})
# gen/recomp_types.h is a copy the generator refreshes; the template comes first so a header
# fix applies without regenerating (the two are identical right after scripts/generate.py).
target_include_directories(black_gen BEFORE PRIVATE ${XR}/templates/runtime)
target_include_directories(black_gen PRIVATE "${BLACK_ROOT}/src/recomp/gen" "${BLACK_ROOT}/src"
                           ${BLACK_A64_INCLUDES})
target_compile_definitions(black_gen PRIVATE ${BLACK_A64_DEFS})
# -fno-slp-vectorize: with the register file in one TLS object (recomp_regfile.h) the SLP vectorizer loads adjacent
# registers as one vector and stores a lane of it with ST1 {v.s}[n] -- also to guest memory, and the MMIO fault decoder
# (platform/arm64_fault_decode.c) refuses SIMD single-structure accesses. Off, the generated code has no load or store
# the decoder refuses (it had 3,548 before), and it is slightly smaller. port/notes/perf.md 9.
target_compile_options(black_gen PRIVATE ${BLACK_A64_FLAGS} -O2 -fno-slp-vectorize -w)
set_target_properties(black_gen PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(black_game STATIC
    $<TARGET_OBJECTS:black_gen>
    "${BLACK_ROOT}/src/main.c"
    "${BLACK_ROOT}/src/recomp_manual.c"
)
target_include_directories(black_game BEFORE PRIVATE ${XR}/templates/runtime)
target_include_directories(black_game PRIVATE
    "${BLACK_ROOT}/src" "${BLACK_ROOT}/src/recomp"
    ${XR}/include ${XR}/src ${XR}/src/kernel ${XR}/src/platform ${XR}/src/apu ${XR}/src/audio
    ${XR}/src/d3d ${XR}/src/input ${XR}/src/nv2a ${XR}/src/nv2a/kelvin ${XR}/src/video ${XR}/src/usb
    "${BLACK_ROOT}/port/quest" "${BLACK_ROOT}/port/quest/src"
    ${BLACK_A64_INCLUDES})
target_compile_definitions(black_game PRIVATE BLACK_VR ${BLACK_A64_DEFS}
                           BLACK_VERSION="${BLACK_VERSION_STRING}-quest")
target_compile_options(black_game PRIVATE ${BLACK_A64_FLAGS} -O2 -Wall
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-comment -Wno-macro-redefined -Wno-bitwise-instead-of-logical -Wno-format)
target_link_libraries(black_game PUBLIC xboxrecomp)
if(XR_LINK_ANDROID_SYSLIBS)
    target_link_libraries(black_game PUBLIC log)
endif()
