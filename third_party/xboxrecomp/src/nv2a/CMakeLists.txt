# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

# The Kelvin GPU backend (kelvin/kelvin_gfx.h): d3d11 (kelvin_gpu.c and its PC extras: ray tracing, DLSS, texture
# packs) or vulkan (kelvin/vk, for Android/Quest; needs KVK_GLSLANG_DIR). Android defaults to vulkan, every other host
# to d3d11 as before.
if(ANDROID)
    set(_kgfx_default vulkan)
else()
    set(_kgfx_default d3d11)
endif()
set(KELVIN_GFX_BACKEND ${_kgfx_default} CACHE STRING "Kelvin GPU backend: d3d11 or vulkan")
set_property(CACHE KELVIN_GFX_BACKEND PROPERTY STRINGS d3d11 vulkan)
if(KELVIN_GFX_BACKEND STREQUAL "vulkan")
    set(_kgfx_sources)
else()
    set(_kgfx_sources
        kelvin/kelvin_gpu.c
        kelvin/kelvin_rt.c
        kelvin/kelvin_dlss.c
        kelvin/kelvin_texpack.c)
endif()

add_library(xbox_nv2a STATIC
    nv2a_core.c
    nv2a_pgraph_d3d11.c
    nv2a_pb_replay.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
    # Kelvin: the NV2A 3D engine on the CPU (RECOMP_NV2A_KELVIN=1).
    kelvin/kelvin.c
    kelvin/kelvin_mmio.c
    kelvin/kelvin_pgraph.c
    kelvin/kelvin_render.c
    ${_kgfx_sources}
    kelvin/kelvin_vsh.c
    kelvin/kelvin_pixel.c
    kelvin/kelvin_texture.c
    kelvin/kelvin_workers.c
    kelvin/nv2a_vsh_cpu/nv2a_vsh_cpu.c
    kelvin/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
    kelvin/nv2a_vsh_cpu/nv2a_vsh_emulator.c
    kelvin/nv2a_vsh_cpu/nv2a_vsh_emulator4.c
    kelvin/nv2a_vsh_cpu/nv2a_vsh_emulator8.c
    kelvin/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c
)

# The production renderer takes its raster size from the video settings (src/video/pc_video.c); the unit tests that
# include kelvin_render.c directly keep the environment-only behaviour.
set_source_files_properties(kelvin/kelvin_render.c PROPERTIES COMPILE_DEFINITIONS KRES_PC_VIDEO=1)

# Ray tracing shaders (Direct3D 12, shader model 6.5, kelvin/rt): compiled with the Windows SDK's dxc into headers.
# Without dxc the renderer builds without them and ray tracing stays off (kelvin_rt.c).
if(WIN32)
    set(_krt_pf86 "ProgramFiles(x86)")
    file(GLOB _krt_sdk_bins "$ENV{${_krt_pf86}}/Windows Kits/10/bin/10.*/x64")
    list(SORT _krt_sdk_bins COMPARE NATURAL ORDER DESCENDING)
    find_program(KRT_DXC dxc HINTS ${_krt_sdk_bins})
    if(KRT_DXC)
        set(_krt_out ${CMAKE_CURRENT_BINARY_DIR}/krt_shaders)
        set(_krt_headers)
        foreach(_krt_name krt_unpack krt_view krt_reflect krt_calibrate krt_calib_update krt_light krt_reflect_filter krt_light_filter)
            set(_krt_h ${_krt_out}/${_krt_name}.h)
            add_custom_command(
                OUTPUT ${_krt_h}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_krt_out}
                COMMAND ${KRT_DXC} -nologo -T cs_6_5 -E main -O3 -Fh ${_krt_h} -Vn ${_krt_name}_cs
                        ${CMAKE_CURRENT_SOURCE_DIR}/kelvin/rt/${_krt_name}.hlsl
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kelvin/rt/${_krt_name}.hlsl
                        ${CMAKE_CURRENT_SOURCE_DIR}/kelvin/rt/krt_common.hlsli
                VERBATIM)
            list(APPEND _krt_headers ${_krt_h})
        endforeach()
        target_sources(xbox_nv2a PRIVATE ${_krt_headers})
        target_include_directories(xbox_nv2a PRIVATE ${_krt_out})
        target_compile_definitions(xbox_nv2a PRIVATE KRT_HAVE_SHADERS=1)
    else()
        message(STATUS "dxc not found: the renderer is built without ray tracing")
    endif()
endif()

# NVIDIA DLSS (kelvin_dlss.c): the SDK's headers and import library, when they are in third_party/dlss (scripts fetch
# them; they are not part of the repositories). Without them DLAA falls back to FXAA.
set(KDLSS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../../dlss" CACHE PATH "NVIDIA DLSS SDK (include/, lib/Windows_x86_64/x64)")
if(WIN32 AND EXISTS "${KDLSS_SDK}/include/nvsdk_ngx.h" AND EXISTS "${KDLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    target_include_directories(xbox_nv2a PRIVATE "${KDLSS_SDK}/include")
    target_compile_definitions(xbox_nv2a PRIVATE KDLSS_HAVE_SDK=1)
    target_link_libraries(xbox_nv2a PRIVATE "${KDLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    message(STATUS "DLSS SDK: ${KDLSS_SDK}")
else()
    message(STATUS "DLSS SDK not found at ${KDLSS_SDK}: DLAA falls back to FXAA")
endif()

if(KELVIN_GFX_BACKEND STREQUAL "vulkan")
    # kelvin_vk needs kelvin_unsupported and the vertex-program parser from this library, and this library needs
    # kgpu_* from kelvin_vk: the backend's objects go into xbox_nv2a itself.
    add_subdirectory(kelvin/vk)
    target_sources(xbox_nv2a PRIVATE $<TARGET_OBJECTS:kelvin_vk>)
    target_link_libraries(xbox_nv2a PUBLIC kelvin_vk_shadergen glslang glslang-default-resource-limits)
    target_include_directories(xbox_nv2a PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kelvin/vk)
    if(ANDROID)
        target_link_libraries(xbox_nv2a PUBLIC vulkan log)
    elseif(KVK_VULKAN_LIB)
        target_link_libraries(xbox_nv2a PUBLIC ${KVK_VULKAN_LIB})
    endif()
endif()

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xbox_nv2a PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
        # The CPU renderer's hot loops: AVX2 code generation (no FMA
        # contraction under /fp:precise, so results are unchanged) and no
        # stack-cookie checks in its per-pixel functions.
        /arch:AVX2
        /GS-
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_nv2a PRIVATE platform)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
    target_link_libraries(xbox_nv2a PRIVATE windowscodecs ole32)  # photo captures as PNG (WIC)
endif()
