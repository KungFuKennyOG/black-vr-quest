# tests/perf: the cost of one trapped MMIO access (SIGSEGV round trip) through the game's own dispatcher and emulator
# (port/notes/perf.md). A measurement, not a pass/fail test: it fails only if a trapped access is not serviced.
#
#   cmake -G Ninja -S tests/perf -B build-perf-bench && cmake --build build-perf-bench && build-perf-bench/fault_bench
#   (AArch64: -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake; runs under /root/xtool/arm64run)
cmake_minimum_required(VERSION 3.20)
project(fault_bench C)
set(CMAKE_C_STANDARD 11)
get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PLATFORM_DIR "${BLACK_ROOT}/third_party/xboxrecomp/src/platform")
enable_testing()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR ANDROID_ABI STREQUAL "arm64-v8a")
    add_executable(fault_bench fault_bench.c "${PLATFORM_DIR}/arm64_fault_decode.c")
    target_compile_options(fault_bench PRIVATE -O2 -march=armv8.2-a)
else()
    # x86-64 Linux: the headless game run's glue (tests/game_render_x86/x86_fault_glue.c, mmio_decode.h's decoder)
    add_executable(fault_bench fault_bench.c "${BLACK_ROOT}/tests/game_render_x86/x86_fault_glue.c"
                   "${PLATFORM_DIR}/arm64_fault_decode.c")
    target_compile_definitions(fault_bench PRIVATE XR_HOST_FAULT_GLUE=1)
    target_include_directories(fault_bench PRIVATE "${BLACK_ROOT}/tests/game_render_x86/glue_inc")
    target_compile_options(fault_bench PRIVATE -O2)
endif()
target_include_directories(fault_bench PRIVATE "${PLATFORM_DIR}")
add_test(NAME fault-bench COMMAND fault_bench 100000)
