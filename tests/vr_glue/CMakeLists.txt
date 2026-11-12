# tests/vr_glue: the Quest shell (port/quest/src: xr_session.c, xr_vulkan.c, vr_bridge.c) + the renderer glue
# (port/quest/src/vr_renderer_kvk.c) + the real Kelvin Vulkan backend, on a real Vulkan device (SwiftShader),
# with OpenXR faked in-process (fake_openxr.c hands out VkImages and captures xrEndFrame's layers).
#
#   . /root/xtool/env.sh   # (only for the NDK's glslc, which compiles the shell's test-pattern shaders)
#   cmake -S tests/vr_glue -B build-vr-glue -G Ninja -DKVK_GLSLANG_DIR=/root/deps/glslang \
#     -DKVK_VULKAN_HEADERS=/root/deps/Vulkan-Headers/include \
#     -DKVK_TEST_ICD=/root/build/swiftshader/Linux/vk_swiftshader_icd.json
#   cmake --build build-vr-glue && ctest --test-dir build-vr-glue --output-on-failure
# Tests vr-glue-multiview (one 2-layer eye swapchain) and vr-glue-two (two swapchains); exit 77 = no Vulkan device.
cmake_minimum_required(VERSION 3.20)
project(vr_glue_tests C CXX)
enable_testing()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XSRC "${BLACK_ROOT}/third_party/xboxrecomp/src")
set(KEL "${XSRC}/nv2a/kelvin")
set(QUEST "${BLACK_ROOT}/port/quest")
set(OPENXR_SDK_DIR "/root/deps/OpenXR-SDK" CACHE PATH "OpenXR-SDK (headers)")
set(KVK_TEST_ICD "" CACHE FILEPATH "Vulkan ICD json (SwiftShader's vk_swiftshader_icd.json)")
find_program(GLSLC glslc HINTS "$ENV{ANDROID_NDK}/shader-tools/linux-x86_64"
             "/root/xtool/android-ndk-r27c/shader-tools/linux-x86_64" REQUIRED)

find_package(Threads REQUIRED)
add_subdirectory("${KEL}/vk" kelvin_vk)

# the shell's test-pattern shaders, as port/quest/CMakeLists.txt builds them
set(SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
set(SHADER_INCS)
foreach(stage vert frag)
    set(src "${QUEST}/shaders/testpattern.${stage}")
    set(raw "${SHADER_OUT}/testpattern.${stage}.raw")
    set(inc "${SHADER_OUT}/testpattern.${stage}.inc")
    add_custom_command(OUTPUT "${inc}"
        COMMAND "${GLSLC}" --target-env=vulkan1.1 -O -mfmt=c -o "${raw}" "${src}"
        COMMAND "${CMAKE_COMMAND}" -DIN=${raw} -DOUT=${inc} -DNAME=testpattern_${stage}_spv -P "${QUEST}/cmake/wrap_spirv.cmake"
        DEPENDS "${src}" "${QUEST}/cmake/wrap_spirv.cmake" VERBATIM)
    list(APPEND SHADER_INCS "${inc}")
endforeach()

# the shell + glue, compiled for the host against stand-ins for the Android-only headers
add_library(vrglue_shell STATIC
    ${QUEST}/src/xr_session.c
    ${QUEST}/src/xr_vulkan.c
    ${QUEST}/src/vr_bridge.c
    ${QUEST}/src/vr_renderer_kvk.c
    shell_access.c
    fake_openxr.c
    ${SHADER_INCS})
target_include_directories(vrglue_shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${CMAKE_CURRENT_SOURCE_DIR} ${QUEST}/src ${QUEST} ${SHADER_OUT}
    ${OPENXR_SDK_DIR}/include ${KEL}/vk)
target_compile_definitions(vrglue_shell PRIVATE _GNU_SOURCE XR_USE_GRAPHICS_API_VULKAN)
target_compile_options(vrglue_shell PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
                       -Wno-unused-function -fno-strict-aliasing)
target_link_libraries(vrglue_shell PUBLIC kelvin_vk_shadergen)

# the NV2A vertex-program parser the backend needs (in the app: xbox_nv2a)
add_library(vrglue_vsh STATIC
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_cpu.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c)
target_include_directories(vrglue_vsh PUBLIC ${KEL})

add_executable(vr-glue test_vr_glue.c)
target_include_directories(vr-glue PRIVATE ${XSRC} ${XSRC}/nv2a ${XSRC}/platform ${KEL} ${KEL}/vk ${QUEST} ${QUEST}/src
    ${CMAKE_CURRENT_SOURCE_DIR} ${OPENXR_SDK_DIR}/include)
target_compile_definitions(vr-glue PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _GNU_SOURCE)
target_link_libraries(vr-glue PRIVATE vrglue_shell kelvin_vk vrglue_vsh m Threads::Threads)

foreach(mode multiview two)
    add_test(NAME vr-glue-${mode} COMMAND vr-glue ${mode})
    set_tests_properties(vr-glue-${mode} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    if(KVK_TEST_ICD)
        set_tests_properties(vr-glue-${mode} PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${KVK_TEST_ICD}")
    endif()
endforeach()
