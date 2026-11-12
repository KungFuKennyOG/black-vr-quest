# BLACK Quest port -- the rest of the XboxRecomp hardware runtime for Android arm64:
# the NV2A GPU (Kelvin engine + Vulkan backend), USB (OHCI) and the video layer.
#
#   include(${CMAKE_CURRENT_LIST_DIR}/nv2a.cmake)      # from port/android/CMakeLists.txt,
#                                                      # after xr_platform/xr_kernel exist
#
# Reuses XR, BLACK_ROOT, SSE2NEON_DIR, XR_ARM64_FLAGS, XR_DEFS and XR_LINK_ANDROID_SYSLIBS
# when the including file set them, and falls back to the same values otherwise, so it
# also works included from any other project (see tests/nv2a_arm64).
#
# Targets (static archives):
#   xr_nv2a   NV2A: the Kelvin engine (PFIFO pusher, PGRAPH, CPU vertex/pixel pipelines,
#             raster workers, trapped registers), nv2a_vsh_cpu, nv2a_mmio_hook, the
#             PRAMDAC palette (nv2a_core.c; its D3D11 PGRAPH translator is replaced by
#             nv2a_pgraph_none.c), and the Vulkan backend's objects
#             (kelvin/vk, KELVIN_GFX_BACKEND=vulkan) -- the backend and the engine call
#             each other, as upstream folds kelvin_vk into xbox_nv2a.
#             Defines kelvin_start/active/vblank/irq_asserted/irq_event/owns_register,
#             kelvin_mark_frame_aspect/view, kgpu_*, kvk_*.
#   xr_video  src/video on its non-Windows paths: fb_present (xbox_Framebuffer*), the
#             FMV player stubs (xbox_VideoPlayFile -> -1, xbox_VideoIsPlaying -> 0: no
#             host player; BLACK's movies are the title's own XMV decoder), pc_video
#             settings, and the pc_menu/photo/split/crosshair/debug/movie_skip entry points.
#   xr_usb    the MCPX OHCI controllers + the USB gamepad (xbox_OhciInit; RECOMP_USB).
#   xr_hw     INTERFACE: all three, plus glslang/Vulkan/log.
#
# Link order (static, left needs right):
#   <game> xr_usb xr_nv2a xr_video xr_kernel xr_apu xr_dsound xr_platform
#   with <input> (xinput_device/pc_input/pc_controls, BLACK_VR) after xr_usb and xr_video.
#   xr_kernel calls back into xr_nv2a/xr_usb/xr_video (kelvin_start, xbox_OhciInit,
#   xbox_Framebuffer*, xbox_Video*), so link the archives as one group
#   (-Wl,--start-group ... --end-group) or whole-archive, as port/quest does.
#
# Symbols left for others at the final link: see port/notes/nv2a-arm64.md.

include_guard(GLOBAL)
enable_language(CXX)                       # glslang (kelvin/vk compiles GLSL at run time)

get_filename_component(_nv_port "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
if(NOT DEFINED BLACK_ROOT)
    get_filename_component(BLACK_ROOT "${_nv_port}/../.." ABSOLUTE)
endif()
if(NOT DEFINED XR)
    set(XR "${BLACK_ROOT}/third_party/xboxrecomp")
endif()
if(NOT DEFINED SSE2NEON_DIR)
    set(SSE2NEON_DIR "/root/deps/sse2neon" CACHE PATH "Directory holding sse2neon.h")
endif()
if(NOT DEFINED XR_ARM64_FLAGS)
    # As port/android/CMakeLists.txt: v8.2 (inline LSE atomics, which the MMIO fault
    # emulator handles), no FMA contraction, no -fms-volatile.
    set(XR_ARM64_FLAGS -march=armv8.2-a -ffp-contract=off -fno-strict-aliasing -fwrapv
        -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-format -Wno-comment)
endif()
if(NOT DEFINED XR_DEFS)
    set(XR_DEFS WIN32_LEAN_AND_MEAN NOMINMAX SSE2NEON_PRECISE_MINMAX=1)
endif()
if(NOT DEFINED XR_LINK_ANDROID_SYSLIBS)
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        set(XR_LINK_ANDROID_SYSLIBS OFF)    # static test executables: no liblog/libvulkan
    else()
        set(XR_LINK_ANDROID_SYSLIBS ON)
    endif()
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    message(FATAL_ERROR "nv2a.cmake builds the aarch64 runtime (got ${CMAKE_SYSTEM_PROCESSOR})")
endif()

set(XR_SRC "${XR}/src")
set(XR_KEL "${XR_SRC}/nv2a/kelvin")
# x86 intrinsic headers (<intrin.h>, <immintrin.h>, ...) -> sse2neon, for the Kelvin TUs.
set(XR_X86SHIM "${_nv_port}/x86shim")
# The Kelvin engine's own warnings are the Windows build's (/W3); keep the
# platform flags but not -Wall's noise from code this port does not change.
set(XR_NV2A_FLAGS ${XR_ARM64_FLAGS} -O2 -Wno-sometimes-uninitialized -Wno-macro-redefined
    -Wno-array-bounds -Wno-missing-braces)

# ---- the Vulkan backend (renderer stream: src/nv2a/kelvin/vk/CMakeLists.txt) ----------
if(NOT TARGET kelvin_vk)
    if(NOT KVK_GLSLANG_DIR AND EXISTS "/root/deps/glslang/CMakeLists.txt")
        set(KVK_GLSLANG_DIR "/root/deps/glslang" CACHE PATH "glslang source checkout")
    endif()
    add_subdirectory("${XR_KEL}/vk" "${CMAKE_BINARY_DIR}/kelvin_vk")
endif()
# Same float rules as the engine: the backend computes viewport, depth-bias and eye math
# on the CPU; clang's default would contract it into FMAs the x86 build never had.
target_compile_options(kelvin_vk PRIVATE -march=armv8.2-a -ffp-contract=off)
target_compile_options(kelvin_vk_shadergen PRIVATE -march=armv8.2-a -ffp-contract=off)

# ---- xr_video ---------------------------------------------------------------------------
add_library(xr_video STATIC
    ${XR_SRC}/video/fb_present.c
    ${XR_SRC}/video/video_player.c
    ${XR_SRC}/video/video_pump.c
    ${XR_SRC}/video/pc_video.c
    ${XR_SRC}/video/pc_video_menu.c
    ${XR_SRC}/video/pc_menu.c
    ${XR_SRC}/video/pc_menu_assets.c
    ${XR_SRC}/video/pc_movie_skip.c
    ${XR_SRC}/video/pc_photo.c
    ${XR_SRC}/video/pc_crosshair.c
    ${XR_SRC}/video/pc_debug.c
    ${XR_SRC}/video/pc_split.c
)
target_include_directories(xr_video PUBLIC ${XR_SRC}/video ${XR_SRC})
target_include_directories(xr_video PRIVATE ${XR_SRC}/platform ${XR_SRC}/input)
target_include_directories(xr_video SYSTEM PRIVATE ${XR_X86SHIM})
target_compile_definitions(xr_video PRIVATE ${XR_DEFS})
target_compile_options(xr_video PRIVATE ${XR_NV2A_FLAGS})
target_link_libraries(xr_video PUBLIC m)
if(TARGET xr_platform)
    target_link_libraries(xr_video PUBLIC xr_platform)
endif()

# ---- xr_nv2a ----------------------------------------------------------------------------
add_library(xr_nv2a STATIC
    ${XR_KEL}/kelvin.c
    ${XR_KEL}/kelvin_mmio.c
    ${XR_KEL}/kelvin_pgraph.c
    ${XR_KEL}/kelvin_render.c
    ${XR_KEL}/kelvin_vsh.c
    ${XR_KEL}/kelvin_pixel.c
    ${XR_KEL}/kelvin_texture.c
    ${XR_KEL}/kelvin_workers.c
    ${XR_KEL}/nv2a_vsh_cpu/nv2a_vsh_cpu.c
    ${XR_KEL}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
    ${XR_KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator.c
    ${XR_KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator4.c
    ${XR_KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator8.c      # AVX2 only: empty on arm64
    ${XR_KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c
    ${XR_SRC}/nv2a/nv2a_mmio_hook.c                  # aarch64 path: arm64-fault stream
    ${XR_SRC}/nv2a/nv2a_core.c                       # PRAMDAC palette (nv2a_dac_*)
    ${XR_SRC}/nv2a/nv2a_pgraph_none.c                # its pgraph_d3d11_* calls: no D3D11 here
    $<TARGET_OBJECTS:kelvin_vk>
)
# Upstream (src/nv2a/CMakeLists.txt): the production renderer takes its raster size
# from the video settings.
set_source_files_properties(${XR_KEL}/kelvin_render.c PROPERTIES COMPILE_DEFINITIONS KRES_PC_VIDEO=1)
target_include_directories(xr_nv2a PUBLIC ${XR_SRC}/nv2a ${XR_KEL} ${XR_KEL}/vk)
target_include_directories(xr_nv2a PRIVATE ${XR_SRC} ${XR_SRC}/platform ${XR_SRC}/kernel ${SSE2NEON_DIR})
target_include_directories(xr_nv2a SYSTEM PRIVATE ${XR_X86SHIM})
target_compile_definitions(xr_nv2a PRIVATE ${XR_DEFS})
target_compile_options(xr_nv2a PRIVATE ${XR_NV2A_FLAGS})
target_link_libraries(xr_nv2a PUBLIC kelvin_vk_shadergen glslang glslang-default-resource-limits
                                     xr_video m)
if(TARGET xr_platform)
    target_link_libraries(xr_nv2a PUBLIC xr_platform)
endif()
if(XR_LINK_ANDROID_SYSLIBS)
    target_link_libraries(xr_nv2a PUBLIC vulkan log android)
endif()

# ---- xr_usb -----------------------------------------------------------------------------
add_library(xr_usb STATIC
    ${XR_SRC}/usb/ohci.c
    ${XR_SRC}/usb/usb_gamepad.c
)
target_include_directories(xr_usb PUBLIC ${XR_SRC}/usb)
target_include_directories(xr_usb PRIVATE ${XR_SRC} ${XR_SRC}/platform ${XR_SRC}/kernel ${XR_SRC}/input)
target_include_directories(xr_usb SYSTEM PRIVATE ${XR_X86SHIM} ${SSE2NEON_DIR})
target_compile_definitions(xr_usb PRIVATE ${XR_DEFS})
target_compile_options(xr_usb PRIVATE ${XR_NV2A_FLAGS})
if(TARGET xr_platform)
    target_link_libraries(xr_usb PUBLIC xr_platform)
endif()

# ---- umbrella -----------------------------------------------------------------------------
add_library(xr_hw INTERFACE)
target_link_libraries(xr_hw INTERFACE xr_usb xr_nv2a xr_video)
