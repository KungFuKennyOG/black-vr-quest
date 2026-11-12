# tests/device_preflight: the Quest app's preflight self-checks (port/quest/src/bvr_diag.c) with the runtime's real
# SIGSEGV dispatcher. See port/notes/device-preflight.md.
#
#   arm64 (static bionic, runs under QEMU through /root/xtool/arm64run):
#     cmake -G Ninja -S tests/device_preflight -B build-preflight-a64 -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake
#   x86-64 Linux (tests/game_render_x86's fault glue):
#     cmake -G Ninja -S tests/device_preflight -B build-preflight-x86
#   cmake --build <dir> && ctest --test-dir <dir> --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(device_preflight C)
enable_testing()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XSRC "${BLACK_ROOT}/third_party/xboxrecomp/src")
set(QUEST "${BLACK_ROOT}/port/quest")
set(GRX "${BLACK_ROOT}/tests/game_render_x86")
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)

set(PF_SOURCES test_preflight.c ${QUEST}/src/bvr_diag.c)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND PF_SOURCES ${XSRC}/platform/arm64_fault_decode.c)
else()
    list(APPEND PF_SOURCES ${XSRC}/platform/arm64_fault_decode.c ${GRX}/x86_fault_glue.c)
    set_source_files_properties(${GRX}/x86_fault_glue.c PROPERTIES INCLUDE_DIRECTORIES "${GRX}/glue_inc;${GRX}")
endif()
add_executable(test_preflight ${PF_SOURCES})
target_include_directories(test_preflight PRIVATE ${QUEST}/src ${XSRC}/platform)
if(NOT ANDROID)
    # the NDK's <android/log.h> stand-in (tests/vr_glue)
    target_include_directories(test_preflight PRIVATE "${BLACK_ROOT}/tests/vr_glue/stubs")
    target_compile_definitions(test_preflight PRIVATE XR_HOST_FAULT_GLUE=1)
endif()
# a static bionic binary has no libandroid (ASharedMemory): memfd only
target_compile_definitions(test_preflight PRIVATE BVR_NO_LIBANDROID=1 _GNU_SOURCE)
target_compile_options(test_preflight PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_preflight PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

add_test(NAME device-preflight COMMAND test_preflight ${CMAKE_CURRENT_BINARY_DIR}/scratch)
set_tests_properties(device-preflight PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "PASS: 0 check")
