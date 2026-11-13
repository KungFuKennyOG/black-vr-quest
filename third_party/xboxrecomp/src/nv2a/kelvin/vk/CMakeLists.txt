# Kelvin GPU backend on Vulkan (kelvin_gfx.h). Built
#   - into xbox_nv2a by ../../CMakeLists.txt when KELVIN_GFX_BACKEND=vulkan (Android/Quest), and
#   - standalone by tests/kelvin_vk (native unit/integration tests, Android link check).
#
# Targets:
#   kelvin_vk_shadergen  GLSL generators + per-eye math. Pure C, no Vulkan, no glslang.
#   kelvin_vk            the backend (kgpu_* of kelvin_gpu.h, kvk_* of kvk_device.h) + glslang + Vulkan.
#                        Needs from the rest of the runtime: kelvin_unsupported (kelvin.c) and nv2a_vsh_parse_program /
#                        nv2a_vsh_program_destroy (nv2a_vsh_cpu/nv2a_vsh_disassembler.c), and the vertex-program
#                        emulator (nv2a_vsh_emulator.c, _execution_state.c, nv2a_vsh_cpu.c: where a screen-space VR
#                        effect lands); xbox_nv2a provides all of them.
#
# glslang is compiled from source into the app (GLSL -> SPIR-V at run time, see kvk_spirv.h): set KVK_GLSLANG_DIR to
# a checkout of https://github.com/KhronosGroup/glslang (verified with commit b93de9f). Built without SPIRV-Tools /
# optimizer and without the HLSL front end.
cmake_minimum_required(VERSION 3.20)

get_filename_component(KVK_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(KVK_XBOXRECOMP_SRC "${KVK_DIR}/../../.." ABSOLUTE)
get_filename_component(_kvk_default_bridge "${KVK_DIR}/../../../../../../port/quest" ABSOLUTE)
set(KVK_VR_BRIDGE_DIR "${_kvk_default_bridge}" CACHE PATH "Directory of vr_bridge.h (the Quest XR shell's contract)")
set(KVK_GLSLANG_DIR "" CACHE PATH "glslang source checkout, compiled into the Vulkan backend")

add_library(kelvin_vk_shadergen STATIC
    ${KVK_DIR}/kvk_glsl.c
    ${KVK_DIR}/kvk_stereo.c)
target_include_directories(kelvin_vk_shadergen PUBLIC ${KVK_DIR} ${KVK_VR_BRIDGE_DIR})
set_property(TARGET kelvin_vk_shadergen PROPERTY C_STANDARD 11)
set_property(TARGET kelvin_vk_shadergen PROPERTY POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_link_libraries(kelvin_vk_shadergen PUBLIC m)
endif()

if(NOT TARGET glslang)
    if(NOT KVK_GLSLANG_DIR OR NOT EXISTS "${KVK_GLSLANG_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "The Vulkan Kelvin backend needs glslang: set KVK_GLSLANG_DIR to a glslang source checkout")
    endif()
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
    set(_kvk_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${KVK_GLSLANG_DIR}" "${CMAKE_BINARY_DIR}/kvk-glslang" EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${_kvk_shared})
endif()

add_library(kelvin_vk STATIC
    ${KVK_DIR}/kvk_backend.c
    ${KVK_DIR}/kvk_spirv.c
    ${KVK_DIR}/kvk_vkmap.c)
set_property(TARGET kelvin_vk PROPERTY C_STANDARD 11)
set_property(TARGET kelvin_vk PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(kelvin_vk PUBLIC ${KVK_DIR}
    PRIVATE ${KVK_XBOXRECOMP_SRC} ${KVK_XBOXRECOMP_SRC}/nv2a ${KVK_XBOXRECOMP_SRC}/platform)
target_compile_definitions(kelvin_vk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# The call-trace recorder (kvk_trace.inc, RECOMP_KVK_TRACE): host builds only (game_render_x86, tests); the headset's
# library has none of it.
if(NOT ANDROID)
    target_compile_definitions(kelvin_vk PRIVATE KVK_TRACE=1)
endif()
target_link_libraries(kelvin_vk PUBLIC kelvin_vk_shadergen glslang glslang-default-resource-limits)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    target_compile_options(kelvin_vk PRIVATE -msse4.2)        # gfx/kgfx_hash.inc's CRC-32C
endif()
if(NOT MSVC)
    target_compile_options(kelvin_vk PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers)
endif()

# Vulkan: the NDK's libvulkan on Android, the loader elsewhere. Headers: the NDK's, or KVK_VULKAN_HEADERS.
set(KVK_VULKAN_HEADERS "" CACHE PATH "Vulkan-Headers include directory (non-Android hosts without system headers)")
if(KVK_VULKAN_HEADERS)
    target_include_directories(kelvin_vk PUBLIC ${KVK_VULKAN_HEADERS})
    target_include_directories(kelvin_vk_shadergen PUBLIC ${KVK_VULKAN_HEADERS})
endif()
if(ANDROID)
    target_link_libraries(kelvin_vk PUBLIC vulkan log)
else()
    find_library(KVK_VULKAN_LIB NAMES vulkan vulkan-1 libvulkan.so.1 PATHS /usr/lib/x86_64-linux-gnu)
    if(KVK_VULKAN_LIB)
        target_link_libraries(kelvin_vk PUBLIC ${KVK_VULKAN_LIB})
    else()
        message(WARNING "Vulkan loader not found: link kelvin_vk against one yourself")
    endif()
endif()
find_package(Threads REQUIRED)
target_link_libraries(kelvin_vk PUBLIC Threads::Threads)
