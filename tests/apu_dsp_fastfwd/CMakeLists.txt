# apu_dsp_fastfwd: the DSP idle-poll fast-forward (apu_dsp.c, port/notes/perf.md 10) against full execution.
# Linux/Android only (fork + pipe). Host:
#   cmake -S tests/apu_dsp_fastfwd -B build-apu-x86/fftest-cmake && cmake --build build-apu-x86/fftest-cmake
#   ctest --test-dir build-apu-x86/fftest-cmake
# arm64 (QEMU user mode): configure with the NDK toolchain (-DCMAKE_TOOLCHAIN_FILE=$ANDROID_TOOLCHAIN_FILE
#   -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DDSP56300_LIBRARY=<aarch64-linux-android lib>), then
#   /root/xtool/arm64run <build>/apu-dsp-fastfwd-test
cmake_minimum_required(VERSION 3.20)
project(apu_dsp_fastfwd_test C)
set(CMAKE_C_STANDARD 11)
get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XBOXRECOMP_DIR "${BLACK_ROOT}/third_party/xboxrecomp" CACHE PATH "XboxRecomp runtime")
if(ANDROID)
    set(_dsp_triple aarch64-linux-android)
else()
    set(_dsp_triple x86_64-unknown-linux-gnu)
endif()
set(DSP56300_LIBRARY "${BLACK_ROOT}/build-dsp/${_dsp_triple}/release/libdsp56300_emu_ffi.a" CACHE FILEPATH
    "the DSP56300 core built for this target (third_party/dsp56300, see port/notes/dsp-arm64.md)")
set(X "${XBOXRECOMP_DIR}/src")
add_executable(apu-dsp-fastfwd-test test_main.c ${X}/apu/apu_dma.c ${X}/apu/apu_dsp_engine.c)
target_include_directories(apu-dsp-fastfwd-test PRIVATE ${X}/apu ${X}/nv2a ${X} ${X}/platform
    "${BLACK_ROOT}/third_party/dsp56300/crates/emu-ffi/include")
target_compile_definitions(apu-dsp-fastfwd-test PRIVATE _GNU_SOURCE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(apu-dsp-fastfwd-test PRIVATE -w)
target_link_libraries(apu-dsp-fastfwd-test PRIVATE "${DSP56300_LIBRARY}" m dl)
if(ANDROID)
    target_link_options(apu-dsp-fastfwd-test PRIVATE -static)
else()
    target_link_libraries(apu-dsp-fastfwd-test PRIVATE pthread)
endif()
enable_testing()
add_test(NAME apu-dsp-fastfwd COMMAND apu-dsp-fastfwd-test)
set_tests_properties(apu-dsp-fastfwd PROPERTIES TIMEOUT 120)
