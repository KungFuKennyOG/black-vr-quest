# game_boot_arm64: the arm64 BLACK game build booted as a static executable under QEMU.
#
#   . /root/xtool/env.sh
#   # 1. the game + runtime archives (shared NDK toolchain; the same objects go into libblackvr.so)
#   cmake -G Ninja -S port/android -B build-game-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_TOOLCHAIN_FILE \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DBLACK_ANDROID_GAME=ON
#   cmake --build build-game-android
#   # 2. this test (static toolchain: runs through /root/xtool/arm64run)
#   cmake -G Ninja -S tests/game_boot_arm64 -B build-boot-a64 \
#         -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake \
#         -DBLACK_GAME_LIB_DIR=$PWD/build-game-android/lib
#   cmake --build build-boot-a64 && ctest --test-dir build-boot-a64 --output-on-failure
#
# The test reads the user's own disc files in place from BLACK_GAME_ROOT/game (default: the repo's
# game/, never copied) and writes saves only to the build directory. BOOT_SECONDS bounds the run.
cmake_minimum_required(VERSION 3.20)
project(game_boot_arm64 C)
enable_testing()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XR "${BLACK_ROOT}/third_party/xboxrecomp")
set(BLACK_GAME_LIB_DIR "${BLACK_ROOT}/build-game-android/lib" CACHE PATH "lib/ of the port/android build with BLACK_ANDROID_GAME=ON")
set(BLACK_GAME_ROOT "${BLACK_ROOT}" CACHE PATH "Folder holding game/default.xbe (read in place)")
set(DSP56300_LIBRARY "${BLACK_ROOT}/build-dsp/aarch64-linux-android/release/libdsp56300_emu_ffi.a" CACHE FILEPATH "")
set(BOOT_SECONDS 300 CACHE STRING "Wall-clock bound for the guest run")

foreach(lib black_game xr_input xr_kernel xr_apu xr_dsound xr_platform)
    if(NOT EXISTS "${BLACK_GAME_LIB_DIR}/lib${lib}.a")
        message(FATAL_ERROR "${BLACK_GAME_LIB_DIR}/lib${lib}.a missing: build port/android with -DBLACK_ANDROID_GAME=ON first")
    endif()
    add_library(${lib} STATIC IMPORTED)
    set_target_properties(${lib} PROPERTIES IMPORTED_LOCATION "${BLACK_GAME_LIB_DIR}/lib${lib}.a")
endforeach()

add_executable(game_boot_arm64 boot_main.c boot_stubs.c)
target_include_directories(game_boot_arm64 PRIVATE ${XR}/src/video ${XR}/src/input ${XR}/src
                           "${BLACK_ROOT}/port/quest")
target_compile_options(game_boot_arm64 PRIVATE -O1 -Wall -Wno-unused-parameter)
target_link_libraries(game_boot_arm64 PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,black_game>
    xr_input xr_kernel xr_apu xr_dsound xr_platform)
if(EXISTS "${DSP56300_LIBRARY}")
    target_link_libraries(game_boot_arm64 PRIVATE "${DSP56300_LIBRARY}")
endif()
target_link_libraries(game_boot_arm64 PRIVATE m dl)

if(EXISTS "${BLACK_GAME_ROOT}/game/default.xbe")
    add_test(NAME game_boot_arm64
             COMMAND game_boot_arm64 "${BLACK_GAME_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/scratch")
    set_tests_properties(game_boot_arm64 PROPERTIES
        ENVIRONMENT "BOOT_SECONDS=${BOOT_SECONDS}"
        TIMEOUT 3600
        # Pass = the guest reached the GPU (or ran to the time limit) without a crash.
        PASS_REGULAR_EXPRESSION "STOP: first GPU touch|STOP: time limit"
        FAIL_REGULAR_EXPRESSION "\\[CRASH\\]")
else()
    message(WARNING "${BLACK_GAME_ROOT}/game/default.xbe not found: the boot test is built but not registered")
endif()
