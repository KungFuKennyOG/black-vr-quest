# NV2A (Kelvin) on arm64: x86-vs-arm64 parity and memory-ordering tests.
#
# 1. Native x86-64 (the reference; also builds an SSE-only variant and a ThreadSanitizer run):
#      cmake -S tests/nv2a_arm64 -B build/nv2a-x86 -G Ninja && cmake --build build/nv2a-x86
#      ctest --test-dir build/nv2a-x86            # writes build/nv2a-x86/ref/*.bin from the AVX2 build
# 2. arm64 under QEMU, checked against those dumps:
#      cmake -S tests/nv2a_arm64 -B build/nv2a-a64 -G Ninja \
#            -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake \
#            -DNV2A_PARITY_REF_DIR=$PWD/build/nv2a-x86/ref
#      cmake --build build/nv2a-a64 && ctest --test-dir build/nv2a-a64
#
# The AVX2 build (-mavx2 -mfma, -ffp-contract=off) takes the code paths MSVC /arch:AVX2 takes
# on Windows (__AVX__/__AVX2__ blocks: kvsh_run8, kpix_shade8, ktex_sample_2d8, plane4/interp4).
cmake_minimum_required(VERSION 3.20)
project(nv2a_arm64_tests C)
enable_testing()

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XSRC "${BLACK_ROOT}/third_party/xboxrecomp/src")
set(KEL "${XSRC}/nv2a/kelvin")
set(NV2A_PARITY_REF_DIR "" CACHE PATH "Directory of the x86-64 AVX2 reference dumps (arm64 builds)")
find_package(Threads REQUIRED)

set(_common_defs WIN32_LEAN_AND_MEAN NOMINMAX)
set(_common_inc ${XSRC} ${XSRC}/nv2a ${XSRC}/platform ${KEL} ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_arm64 ON)
    set(_variants a64)
    # exactly the production flags (port/android): sse2neon with precise MINPS/MAXPS, no FMA
    set(_flags_a64 -O2 -march=armv8.2-a -ffp-contract=off -fno-strict-aliasing -fwrapv
        -isystem ${BLACK_ROOT}/port/android/x86shim -I/root/deps/sse2neon)
    list(APPEND _common_defs SSE2NEON_PRECISE_MINMAX=1)
else()
    set(_arm64 OFF)
    set(_variants avx2 sse)
    set(_x86 -O2 -ffp-contract=off -fno-strict-aliasing -fwrapv -w
        -I${CMAKE_CURRENT_SOURCE_DIR}/hostshim "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/hostshim/x86_extra.h"
        "SHELL:-include immintrin.h")
    set(_flags_avx2 ${_x86} -mavx2 -mfma -msse4.2)
    set(_flags_sse ${_x86} -msse2)
endif()

foreach(v ${_variants})
    # the POSIX Win32 layer (and the arm64 fault decoder)
    set(_plat ${XSRC}/platform/win32_compat.c)
    if(_arm64)
        list(APPEND _plat ${XSRC}/platform/arm64_fault_decode.c ${CMAKE_CURRENT_SOURCE_DIR}/android_stubs.c)
    endif()
    add_library(nv2a_plat_${v} STATIC ${_plat})
    target_include_directories(nv2a_plat_${v} PUBLIC ${_common_inc})
    target_compile_definitions(nv2a_plat_${v} PUBLIC ${_common_defs})
    target_compile_options(nv2a_plat_${v} PRIVATE ${_flags_${v}} -w)
    target_link_libraries(nv2a_plat_${v} PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

    # the Kelvin core pieces under test (kelvin_render.c is compiled into the raster test)
    add_library(nv2a_kcore_${v} STATIC
        ${KEL}/kelvin_vsh.c ${KEL}/kelvin_pixel.c ${KEL}/kelvin_texture.c ${KEL}/kelvin_workers.c
        ${KEL}/nv2a_vsh_cpu/nv2a_vsh_cpu.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
        ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator4.c
        ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator8.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c)
    target_compile_options(nv2a_kcore_${v} PRIVATE ${_flags_${v}} -w)
    target_link_libraries(nv2a_kcore_${v} PUBLIC nv2a_plat_${v})

    foreach(t vsh pixel raster kwork)
        set(_extra)
        if(t STREQUAL "raster")
            set(_defs TEST_STUB_GPU)
        elseif(t STREQUAL "kwork")
            set(_defs TEST_STUB_GPU TEST_STUB_GEOMETRY)
        else()
            set(_defs TEST_STUB_GEOMETRY TEST_STUB_GPU)
        endif()
        add_executable(nv2a-${t}-${v} test_${t}_parity.c test_stubs.c)
        target_compile_definitions(nv2a-${t}-${v} PRIVATE ${_defs})
        target_compile_options(nv2a-${t}-${v} PRIVATE ${_flags_${v}})
        target_link_libraries(nv2a-${t}-${v} PRIVATE nv2a_kcore_${v})
    endforeach()
    set_source_files_properties(test_raster_parity.c PROPERTIES COMPILE_OPTIONS "-w")
endforeach()

# ---- tests ----------------------------------------------------------------------------------
set(_ref ${CMAKE_BINARY_DIR}/ref)
file(MAKE_DIRECTORY ${_ref})
if(NOT _arm64)
    foreach(t vsh pixel raster)
        add_test(NAME nv2a-${t}-dump-avx2 COMMAND nv2a-${t}-avx2 --dump ${_ref}/${t}.bin)
        set_tests_properties(nv2a-${t}-dump-avx2 PROPERTIES FIXTURES_SETUP ref_${t})
        add_test(NAME nv2a-${t}-check-sse COMMAND nv2a-${t}-sse --check ${_ref}/${t}.bin)
        set_tests_properties(nv2a-${t}-check-sse PROPERTIES FIXTURES_REQUIRED ref_${t})
    endforeach()
    set_tests_properties(nv2a-raster-dump-avx2 PROPERTIES ENVIRONMENT "RECOMP_KELVIN_THREADS=5")
    set_tests_properties(nv2a-raster-check-sse PROPERTIES ENVIRONMENT "RECOMP_KELVIN_THREADS=1")
    add_test(NAME nv2a-raster-check-avx2-serial COMMAND nv2a-raster-avx2 --check ${_ref}/raster.bin)
    set_tests_properties(nv2a-raster-check-avx2-serial PROPERTIES FIXTURES_REQUIRED ref_raster
                         ENVIRONMENT "RECOMP_KELVIN_THREADS=1")
    add_test(NAME nv2a-kwork-avx2 COMMAND nv2a-kwork-avx2)
    set_tests_properties(nv2a-kwork-avx2 PROPERTIES ENVIRONMENT "RECOMP_KELVIN_THREADS=5")

    # ThreadSanitizer: the worker pool and the multi-threaded raster under the C11 model,
    # which (unlike x86 hardware, or QEMU on x86) flags every handoff that is not an
    # acquire/release or seq_cst atomic -- what ARM would be allowed to reorder.
    # (clang needs its compiler-rt TSan runtime installed; gcc's libtsan is enough: CC=gcc)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS -fsanitize=thread)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
    check_c_source_compiles("int main(void) { return 0; }" NV2A_HAVE_TSAN)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    option(NV2A_TSAN "Build ThreadSanitizer variants" ${NV2A_HAVE_TSAN})
    if(NV2A_TSAN)
        foreach(t kwork raster)
            add_executable(nv2a-${t}-tsan test_${t}_parity.c test_stubs.c
                ${KEL}/kelvin_vsh.c ${KEL}/kelvin_pixel.c ${KEL}/kelvin_texture.c ${KEL}/kelvin_workers.c
                ${KEL}/nv2a_vsh_cpu/nv2a_vsh_cpu.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
                ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator.c ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator4.c
                ${KEL}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c ${XSRC}/platform/win32_compat.c)
            target_include_directories(nv2a-${t}-tsan PRIVATE ${_common_inc})
            target_compile_definitions(nv2a-${t}-tsan PRIVATE ${_common_defs} TEST_STUB_GPU
                $<$<STREQUAL:${t},kwork>:TEST_STUB_GEOMETRY>)
            target_compile_options(nv2a-${t}-tsan PRIVATE ${_flags_sse} -fsanitize=thread -g -O1)
            target_link_options(nv2a-${t}-tsan PRIVATE -fsanitize=thread)
            target_link_libraries(nv2a-${t}-tsan PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
        endforeach()
        add_test(NAME nv2a-kwork-tsan COMMAND nv2a-kwork-tsan)
        add_test(NAME nv2a-raster-tsan COMMAND nv2a-raster-tsan)
        set_tests_properties(nv2a-kwork-tsan nv2a-raster-tsan PROPERTIES
            ENVIRONMENT "RECOMP_KELVIN_THREADS=5;TSAN_OPTIONS=halt_on_error=1 exitcode=66")
    endif()
else()
    foreach(t vsh pixel raster)
        if(NV2A_PARITY_REF_DIR)
            add_test(NAME nv2a-${t}-check-a64 COMMAND nv2a-${t}-a64 --check ${NV2A_PARITY_REF_DIR}/${t}.bin)
        else()
            add_test(NAME nv2a-${t}-a64 COMMAND nv2a-${t}-a64)
        endif()
    endforeach()
    if(NV2A_PARITY_REF_DIR)
        set_tests_properties(nv2a-raster-check-a64 PROPERTIES ENVIRONMENT "RECOMP_KELVIN_THREADS=5")
        add_test(NAME nv2a-raster-check-a64-serial COMMAND nv2a-raster-a64 --check ${NV2A_PARITY_REF_DIR}/raster.bin)
        set_tests_properties(nv2a-raster-check-a64-serial PROPERTIES ENVIRONMENT "RECOMP_KELVIN_THREADS=1")
    endif()
    add_test(NAME nv2a-kwork-a64 COMMAND nv2a-kwork-a64)
    set_tests_properties(nv2a-kwork-a64 PROPERTIES ENVIRONMENT "RECOMP_KELVIN_THREADS=5")
    # The PUT/GET handoff and the trapped registers through the real SIGSEGV path (arm64 only).
    add_executable(nv2a-put-trap test_put_trap.c ${KEL}/kelvin_mmio.c)
    target_compile_definitions(nv2a-put-trap PRIVATE TEST_PUT_TRAP)
    target_compile_options(nv2a-put-trap PRIVATE ${_flags_a64} -w)
    target_link_libraries(nv2a-put-trap PRIVATE nv2a_plat_a64)
    add_test(NAME nv2a-put-trap COMMAND nv2a-put-trap)
endif()
