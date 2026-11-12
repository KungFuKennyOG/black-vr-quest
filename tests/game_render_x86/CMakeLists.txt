# game_render_x86: the real BLACK game driven end to end through the production Quest graphics path, headless, on
# x86-64 Linux: generated code + recomp_manual.c + main.c (BLACK_VR) -> runtime (kernel, APU+DSP, USB, input) ->
# NV2A/Kelvin (KELVIN_GFX_BACKEND=vulkan) -> Kelvin Vulkan backend -> renderer glue (vr_renderer_kvk.c) -> the XR
# shell (xr_session.c, xr_vulkan.c, vr_bridge.c) -> tests/vr_glue's fake OpenXR runtime, on SwiftShader.
# Frames (both eyes, the flat quad layer, the HUD layer) are saved as PNG. See port/notes/game-render-x86.md.
#
#   CC=clang CXX=clang++ cmake -G Ninja -S tests/game_render_x86 -B build-render-x86 \
#       -DKVK_GLSLANG_DIR=/root/deps/glslang -DKVK_VULKAN_HEADERS=/root/deps/Vulkan-Headers/include \
#       -DKVK_TEST_ICD=/root/build/swiftshader/Linux/vk_swiftshader_icd.json
#   cmake --build build-render-x86
#   build-render-x86/run.sh            # or: ctest --test-dir build-render-x86 -V
#
# Host adaptation, confined to this directory: the game and runtime are compiled for x86-64 Linux with
# -D__ANDROID__, so they take exactly the Quest's (Android) source paths; include/ stands in for the NDK headers
# they use (liblog, ASharedMemory, AAudio: host_stubs.c), tests/nv2a_arm64/hostshim maps the Kelvin core's
# <windows.h>/<intrin.h> to the POSIX compat layer, and x86_fault_glue.c implements the SIGSEGV/MMIO glue of
# platform/arm64_fault_decode.h for x86-64 (-DXR_HOST_FAULT_GLUE selects the runtime's POSIX fault paths).
# The user's disc files are read in place (symlinks); everything the run writes goes to the build directory.
cmake_minimum_required(VERSION 3.20)
project(game_render_x86 C CXX)
enable_testing()

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Configure with CC=clang CXX=clang++ (the Quest build's compiler family)")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(FATAL_ERROR "game_render_x86 is the x86-64 Linux host run")
endif()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XR "${BLACK_ROOT}/third_party/xboxrecomp")
set(XSRC "${XR}/src")
set(KEL "${XSRC}/nv2a/kelvin")
set(QUEST "${BLACK_ROOT}/port/quest")
set(GLUE_TEST "${BLACK_ROOT}/tests/vr_glue")
set(HOSTSHIM "${BLACK_ROOT}/tests/nv2a_arm64/hostshim")
set(GRX "${CMAKE_CURRENT_SOURCE_DIR}")
set(OPENXR_SDK_DIR "/root/deps/OpenXR-SDK" CACHE PATH "OpenXR-SDK (headers)")
set(KVK_TEST_ICD "" CACHE FILEPATH "Vulkan ICD json (SwiftShader's vk_swiftshader_icd.json)")
set(BLACK_GAME_ROOT "${BLACK_ROOT}" CACHE PATH "Folder holding game/default.xbe (read in place, never written)")
set(DSP56300_LIBRARY "${BLACK_ROOT}/build-dsp/x86_64-unknown-linux-gnu/release/libdsp56300_emu_ffi.a" CACHE FILEPATH
    "the DSP56300 core built for x86_64-unknown-linux-gnu")
set(GRX_SECONDS 2400 CACHE STRING "wall-clock budget of the ctest run")
find_package(Threads REQUIRED)
find_package(PNG REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{ANDROID_NDK}/shader-tools/linux-x86_64"
             "/root/xtool/android-ndk-r27c/shader-tools/linux-x86_64" REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# ---- the Kelvin Vulkan backend + glslang (as tests/vr_glue builds them) --------------------------------------------
add_subdirectory("${KEL}/vk" kelvin_vk)

# ---- flags ----------------------------------------------------------------------------------------------------------
# The Quest's flags without the AArch64 ones (port/android: -ffp-contract=off -fno-strict-aliasing -fwrapv), and
# the Android source paths (__ANDROID__) with this host's fault glue (XR_HOST_FAULT_GLUE).
set(GRX_DEFS __ANDROID__=1 _GNU_SOURCE WIN32_LEAN_AND_MEAN NOMINMAX XR_HOST_FAULT_GLUE=1)
set(GRX_FLAGS -O2 -g1 -ffp-contract=off -fno-strict-aliasing -fwrapv -msse2
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-comment
    -Wno-macro-redefined -Wno-format -Wno-builtin-macro-redefined -Wno-bitwise-instead-of-logical
    -Wno-sometimes-uninitialized -Wno-array-bounds -Wno-missing-braces -Wno-incompatible-pointer-types-discards-qualifiers
    -Wno-int-conversion -Wno-pointer-sign -Wno-deprecated-non-prototype)
set(GRX_STUB_INC "${GRX}/include")
# Kelvin core and video: <windows.h> / <intrin.h> -> the POSIX layer (nv2a_arm64's test shims), with the
# primitives win32_compat.h provides only on aarch64.
set(GRX_KEL_FLAGS "SHELL:-include ${GRX}/grx_host.h" -I${HOSTSHIM})

# ---- runtime: platform, kernel, APU, DirectSound (port/android/CMakeLists.txt's lists) -----------------------------
add_library(grx_platform STATIC
    ${XSRC}/platform/win32_compat.c
    ${XSRC}/platform/arm64_fault_decode.c          # the pure decoder only off AArch64
    ${GRX}/x86_fault_glue.c)
target_include_directories(grx_platform PUBLIC ${XSRC} ${XSRC}/platform ${XR}/templates/runtime ${XR}/include)
target_include_directories(grx_platform SYSTEM PUBLIC ${GRX_STUB_INC})
target_compile_definitions(grx_platform PUBLIC ${GRX_DEFS})
target_compile_options(grx_platform PRIVATE ${GRX_FLAGS})
set_source_files_properties(${GRX}/x86_fault_glue.c PROPERTIES INCLUDE_DIRECTORIES "${GRX}/glue_inc")
target_link_libraries(grx_platform PUBLIC Threads::Threads dl m)

set(GRX_KERNEL_SOURCES
    kernel_path.c kernel_pool.c kernel_rtl.c kernel_memory.c kernel_file.c
    kernel_thread.c kernel_sync.c kernel_hal.c kernel_xbox.c kernel_ob.c
    kernel_io.c kernel_crypto.c kernel_thunks.c kernel_bridge.c
    xbox_memory_layout.c memory_watch.c icall_feedback.c recomp_trace.c
    nv2a_pb_scan.c nv2a_pb_exec.c thread_placement.c)
list(TRANSFORM GRX_KERNEL_SOURCES PREPEND ${XSRC}/kernel/)
add_library(grx_kernel STATIC ${GRX_KERNEL_SOURCES})
target_include_directories(grx_kernel PUBLIC ${XSRC}/kernel)
target_compile_options(grx_kernel PRIVATE ${GRX_FLAGS})
target_link_libraries(grx_kernel PUBLIC grx_platform)

add_library(grx_apu STATIC
    ${XSRC}/apu/apu_core.c ${XSRC}/apu/apu_vp.c ${XSRC}/apu/apu_dsp.c ${XSRC}/apu/apu_dma.c
    ${XSRC}/apu/apu_dsp_engine.c ${XSRC}/apu/apu_mmio_hook.c
    ${XSRC}/apu/apu_xaudio2.c ${XSRC}/apu/apu_aaudio.c)
target_include_directories(grx_apu PUBLIC ${XSRC}/apu "${BLACK_ROOT}/third_party/dsp56300/crates/emu-ffi/include")
target_include_directories(grx_apu PRIVATE ${XSRC}/nv2a)
target_compile_options(grx_apu PRIVATE ${GRX_FLAGS})
target_link_libraries(grx_apu PUBLIC grx_platform)

add_library(grx_dsound STATIC ${XSRC}/audio/dsound_device.c ${XSRC}/audio/wma_decoder.c)
target_include_directories(grx_dsound PUBLIC ${XSRC}/audio)
target_compile_options(grx_dsound PRIVATE ${GRX_FLAGS})
target_link_libraries(grx_dsound PUBLIC grx_apu grx_platform)

# ---- NV2A / video / USB (port/android/nv2a.cmake's lists) ---------------------------------------------------------
add_library(grx_video STATIC
    ${XSRC}/video/fb_present.c ${XSRC}/video/video_player.c ${XSRC}/video/video_pump.c ${XSRC}/video/pc_video.c
    ${XSRC}/video/pc_video_menu.c ${XSRC}/video/pc_menu.c ${XSRC}/video/pc_menu_assets.c
    ${XSRC}/video/pc_movie_skip.c ${XSRC}/video/pc_photo.c ${XSRC}/video/pc_crosshair.c ${XSRC}/video/pc_debug.c
    ${XSRC}/video/pc_split.c)
target_include_directories(grx_video PUBLIC ${XSRC}/video ${XSRC})
target_include_directories(grx_video PRIVATE ${XSRC}/input ${HOSTSHIM})
target_compile_options(grx_video PRIVATE ${GRX_FLAGS} ${GRX_KEL_FLAGS})
target_link_libraries(grx_video PUBLIC grx_platform m)
# video_player.c includes its own header (the stubs' VideoFramePixels) only under __aarch64__
set_source_files_properties(${XSRC}/video/video_player.c PROPERTIES COMPILE_OPTIONS "-include;${XSRC}/video/video_player.h")

add_library(grx_nv2a STATIC
    ${KEL}/kelvin.c ${KEL}/kelvin_mmio.c ${KEL}/kelvin_pgraph.c ${KEL}/kelvin_render.c ${KEL}/kelvin_vsh.c
    ${KEL}/kelvin_pixel.c ${KEL}/kelvin_texture.c ${KEL}/kelvin_workers.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_cpu.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator4.c
    ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator8.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c
    ${XSRC}/nv2a/nv2a_mmio_hook.c ${XSRC}/nv2a/nv2a_core.c ${XSRC}/nv2a/nv2a_pgraph_none.c)
set_source_files_properties(${KEL}/kelvin_render.c PROPERTIES COMPILE_DEFINITIONS KRES_PC_VIDEO=1)
target_include_directories(grx_nv2a PUBLIC ${XSRC}/nv2a ${KEL} ${KEL}/vk)
target_include_directories(grx_nv2a PRIVATE ${XSRC} ${XSRC}/platform ${XSRC}/kernel ${HOSTSHIM})
target_compile_options(grx_nv2a PRIVATE ${GRX_FLAGS} ${GRX_KEL_FLAGS})
target_link_libraries(grx_nv2a PUBLIC kelvin_vk grx_video grx_platform m)

add_library(grx_usb STATIC ${XSRC}/usb/ohci.c ${XSRC}/usb/usb_gamepad.c)
target_include_directories(grx_usb PUBLIC ${XSRC}/usb)
target_include_directories(grx_usb PRIVATE ${XSRC} ${XSRC}/kernel ${XSRC}/input ${HOSTSHIM})
target_compile_options(grx_usb PRIVATE ${GRX_FLAGS} ${GRX_KEL_FLAGS})
target_link_libraries(grx_usb PUBLIC grx_platform)

# ---- input: the Quest pad backend (BLACK_VR) + pc_input/pc_controls (port/android/game.cmake) ---------------------
add_library(grx_input STATIC ${XSRC}/input/xinput_device.c ${XSRC}/input/pc_input.c ${XSRC}/input/pc_controls.c)
target_include_directories(grx_input PUBLIC ${XSRC}/input)
target_include_directories(grx_input PRIVATE "${BLACK_ROOT}/port/android/include" ${QUEST} ${QUEST}/src)
target_compile_definitions(grx_input PRIVATE BLACK_VR)
target_compile_options(grx_input PRIVATE ${GRX_FLAGS} ${GRX_KEL_FLAGS})
target_include_directories(grx_input PRIVATE ${XSRC})
target_link_libraries(grx_input PUBLIC grx_platform)

# ---- the game (port/android/game.cmake) ------------------------------------------------------------------------------
file(GLOB BLACK_GEN_SOURCES CONFIGURE_DEPENDS "${BLACK_ROOT}/src/recomp/gen/*.c")
if(NOT BLACK_GEN_SOURCES)
    message(FATAL_ERROR "src/recomp/gen is empty: generate the recompiled code from your own default.xbe first")
endif()
file(STRINGS "${BLACK_ROOT}/VERSION" BLACK_VERSION_STRING LIMIT_COUNT 1)
add_library(grx_gen OBJECT ${BLACK_GEN_SOURCES})
target_include_directories(grx_gen BEFORE PRIVATE ${XR}/templates/runtime)
target_include_directories(grx_gen PRIVATE "${BLACK_ROOT}/src/recomp/gen" "${BLACK_ROOT}/src" "${BLACK_ROOT}/port/android/include")
target_include_directories(grx_gen SYSTEM PRIVATE ${GRX_STUB_INC})
target_compile_definitions(grx_gen PRIVATE ${GRX_DEFS})
target_compile_options(grx_gen PRIVATE ${GRX_FLAGS} -fno-slp-vectorize -w)   # as port/android/game.cmake

add_library(grx_game STATIC $<TARGET_OBJECTS:grx_gen> "${BLACK_ROOT}/src/main.c" "${BLACK_ROOT}/src/recomp_manual.c")
target_include_directories(grx_game BEFORE PRIVATE ${XR}/templates/runtime)
target_include_directories(grx_game PRIVATE
    "${BLACK_ROOT}/src" "${BLACK_ROOT}/src/recomp"
    ${XR}/include ${XSRC} ${XSRC}/kernel ${XSRC}/platform ${XSRC}/apu ${XSRC}/audio ${XSRC}/d3d ${XSRC}/input
    ${XSRC}/nv2a ${XSRC}/nv2a/kelvin ${XSRC}/video ${XSRC}/usb ${QUEST} ${QUEST}/src "${BLACK_ROOT}/port/android/include")
target_compile_definitions(grx_game PRIVATE BLACK_VR BLACK_VERSION="${BLACK_VERSION_STRING}-quest-x86test")
target_compile_options(grx_game PRIVATE ${GRX_FLAGS} -Wall -Wno-unused-parameter)
target_link_libraries(grx_game PUBLIC grx_kernel grx_apu grx_dsound grx_input grx_usb grx_nv2a grx_video grx_platform)

# ---- the XR shell + renderer glue + fake OpenXR (as tests/vr_glue builds them) -------------------------------------
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
add_library(grx_shell STATIC
    ${QUEST}/src/xr_session.c ${QUEST}/src/xr_vulkan.c ${QUEST}/src/vr_bridge.c ${QUEST}/src/vr_renderer_kvk.c
    ${QUEST}/src/bvr_diag.c ${QUEST}/src/xr_preflight.c       # the device preflight (port/notes/device-preflight.md)
    ${GLUE_TEST}/fake_openxr.c ${GRX}/grx_shell.c ${SHADER_INCS})
target_include_directories(grx_shell PRIVATE
    ${GLUE_TEST}/stubs ${GLUE_TEST} ${QUEST}/src ${QUEST} ${SHADER_OUT} ${OPENXR_SDK_DIR}/include ${KEL}/vk ${GRX})
target_compile_definitions(grx_shell PRIVATE _GNU_SOURCE XR_USE_GRAPHICS_API_VULKAN)
target_compile_options(grx_shell PRIVATE -O2 -g1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
                       -Wno-unused-function -fno-strict-aliasing)
target_link_libraries(grx_shell PUBLIC kelvin_vk_shadergen)

# ---- the run ----------------------------------------------------------------------------------------------------------
# grx_perf.c: per-interval CPU per thread, trapped-MMIO accesses by register (x86_fault_glue.c) and the Kelvin counters
# (grx_kstats.c, compiled as the Kelvin core); port/notes/perf.md
add_executable(game_render_x86 render_main.c host_stubs.c grx_perf.c grx_prof.c grx_kstats.c)
set_source_files_properties(${GRX}/grx_kstats.c PROPERTIES
    COMPILE_OPTIONS "-include;${GRX}/grx_host.h;${GRX_FLAGS}"
    INCLUDE_DIRECTORIES "${HOSTSHIM};${XSRC};${XSRC}/platform;${XSRC}/kernel;${KEL};${XSRC}/nv2a"
    COMPILE_DEFINITIONS "${GRX_DEFS}")
target_include_directories(game_render_x86 PRIVATE ${GRX} ${GLUE_TEST} ${QUEST} ${QUEST}/src ${KEL} ${KEL}/vk ${XSRC}
    ${XSRC}/nv2a ${XSRC}/platform ${OPENXR_SDK_DIR}/include)
target_include_directories(game_render_x86 SYSTEM PRIVATE ${GRX_STUB_INC})
target_compile_definitions(game_render_x86 PRIVATE _GNU_SOURCE XR_USE_GRAPHICS_API_VULKAN WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(game_render_x86 PRIVATE -O1 -g -Wall -Wno-unused-parameter)
target_link_libraries(game_render_x86 PRIVATE
    -Wl,--start-group
    $<LINK_LIBRARY:WHOLE_ARCHIVE,grx_game> $<LINK_LIBRARY:WHOLE_ARCHIVE,grx_shell> grx_usb grx_nv2a grx_video grx_input grx_kernel grx_apu grx_dsound
    grx_platform kelvin_vk kelvin_vk_shadergen glslang glslang-default-resource-limits
    -Wl,--end-group
    "${DSP56300_LIBRARY}" PNG::PNG Threads::Threads dl m rt
    -Wl,--wrap=pthread_create -Wl,--wrap=CreateThread)

# the shell's renderer hooks are weak in xr_vulkan.c; the glue (vr_renderer_kvk.c) must be linked whole, as the APK
# links black_vk_renderer
configure_file(${GRX}/run.sh.in ${CMAKE_CURRENT_BINARY_DIR}/run.sh @ONLY
               FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
if(EXISTS "${BLACK_GAME_ROOT}/game/default.xbe")
    add_test(NAME game_render_x86 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/run.sh)
    set_tests_properties(game_render_x86 PROPERTIES TIMEOUT 7200
        PASS_REGULAR_EXPRESSION "\\[GRX\\] RESULT: renderer frames"
        FAIL_REGULAR_EXPRESSION "\\[CRASH\\]|\\[GRX\\] FAIL")
    # the gun latch (port/notes/gun-latch.md): a level-1 run with a tracked controller that turns between a frame's
    # game latch and its late latch, then the image check of the saved frames
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME game_render_x86_gunlatch COMMAND sh -c
            "rm -rf '${CMAKE_CURRENT_BINARY_DIR}/frames/gunlatch' && GRX_GUN_TEST=1 GRX_VIDEO=0 GRX_SECONDS=\${GRX_SECONDS:-600} '${CMAKE_CURRENT_BINARY_DIR}/run.sh' && '${Python3_EXECUTABLE}' '${GRX}/gun_latch_check.py' '${CMAKE_CURRENT_BINARY_DIR}/frames'")
        set_tests_properties(game_render_x86_gunlatch PROPERTIES TIMEOUT 7200
            PASS_REGULAR_EXPRESSION "gun_latch_check: PASS"
            FAIL_REGULAR_EXPRESSION "\\[CRASH\\]|\\[GRX\\] FAIL|gun_latch_check: FAIL|INCOMPLETE")
    endif()
endif()
