# Kelvin Vulkan backend tests (usage: port/notes/renderer.md, "Tests").
#   kvk-stereo     per-eye transform math (kvk_stereo.c)
#   kvk-shadergen  GLSL generators: every shader compiles to SPIR-V with glslang; GLSL mirrors the D3D11 HLSL
#                  generators (extracted from the production .inc files); position classification
#   kvk-backend    (host only) the whole backend on a Vulkan device (SwiftShader works): draws compared pixel by
#                  pixel with Kelvin's own CPU renderer (kelvin_pixel.c, kelvin_texture.c) and the NV2A vertex
#                  program emulator (nv2a_vsh_cpu); stereo/multiview, the VR composition, HUD redirection, clears,
#                  depth reinterpretation. Skipped (exit 77) without a Vulkan device.
#   kvk_linkcheck  (Android) libkvk_linkcheck.so: the static kelvin_vk library linked with --no-undefined against
#                  the NDK's libvulkan, every public entry point referenced.
# With /root/xtool/android-arm64-static.cmake the arm64 kvk-stereo and kvk-shadergen run under QEMU (ctest).
cmake_minimum_required(VERSION 3.20)
project(kelvin_vk_tests C CXX)
enable_testing()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XBOXRECOMP_DIR "${BLACK_ROOT}/third_party/xboxrecomp" CACHE PATH "XboxRecomp submodule")
set(KVK_TEST_ICD "" CACHE FILEPATH "Vulkan ICD json for kvk-backend (e.g. SwiftShader's vk_swiftshader_icd.json)")
set(XSRC "${XBOXRECOMP_DIR}/src")
set(KEL "${XSRC}/nv2a/kelvin")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_subdirectory("${KEL}/vk" kelvin_vk)

# the NV2A vertex program parser and CPU emulator (reference for the GPU vertex path)
add_library(kvk_test_vsh STATIC
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_cpu.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c)
target_include_directories(kvk_test_vsh PUBLIC ${KEL})

set(_d3d_gen "${CMAKE_CURRENT_BINARY_DIR}/d3d_generators.inc")
add_custom_command(OUTPUT ${_d3d_gen}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/extract_d3d.py --kelvin ${KEL} --output ${_d3d_gen}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/extract_d3d.py ${KEL}/kelvin_gpu_vsh.inc ${KEL}/kelvin_gpu_pss.inc
    VERBATIM)

add_executable(kvk-stereo test_stereo.c)
target_link_libraries(kvk-stereo PRIVATE kelvin_vk_shadergen)
add_test(NAME kvk-stereo COMMAND kvk-stereo)

add_executable(kvk-shadergen test_shadergen.c ${_d3d_gen} ${KEL}/vk/kvk_spirv.c)
target_include_directories(kvk-shadergen PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${KEL})
target_link_libraries(kvk-shadergen PRIVATE kelvin_vk_shadergen kvk_test_vsh glslang glslang-default-resource-limits)
add_test(NAME kvk-shadergen COMMAND kvk-shadergen)

if(ANDROID)
    add_library(kvk_linkcheck SHARED android_linkcheck.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
        ${KEL}/nv2a_vsh_cpu/nv2a_vsh_cpu.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator.c
        ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c)
    target_include_directories(kvk_linkcheck PRIVATE ${XSRC} ${XSRC}/nv2a ${XSRC}/platform ${KEL})
    target_compile_definitions(kvk_linkcheck PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(kvk_linkcheck PRIVATE kelvin_vk)
    target_link_options(kvk_linkcheck PRIVATE -Wl,--no-undefined)
    return()
endif()

# Kelvin's CPU renderer pieces the backend test compares with (built as the PC build builds them: AVX2/FMA)
add_library(kvk_test_cpuref STATIC ${KEL}/kelvin_pixel.c ${KEL}/kelvin_texture.c)
target_include_directories(kvk_test_cpuref PUBLIC ${XSRC} ${XSRC}/nv2a ${XSRC}/platform ${KEL})
target_compile_definitions(kvk_test_cpuref PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(kvk_test_cpuref PRIVATE -O2 -mavx2 -mfma -msse4.2 -include immintrin.h -w)

add_executable(kvk-backend test_backend.c)
target_include_directories(kvk-backend PRIVATE ${XSRC} ${XSRC}/nv2a ${XSRC}/platform ${KEL} ${KEL}/vk)
target_compile_definitions(kvk-backend PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(kvk-backend PRIVATE kelvin_vk kvk_test_cpuref kvk_test_vsh m)
foreach(mode cpu-parity uber stereo)
    add_test(NAME kvk-backend-${mode} COMMAND kvk-backend ${mode})
    set_tests_properties(kvk-backend-${mode} PROPERTIES SKIP_RETURN_CODE 77)
    if(KVK_TEST_ICD)
        set_tests_properties(kvk-backend-${mode} PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${KVK_TEST_ICD}")
    endif()
endforeach()
