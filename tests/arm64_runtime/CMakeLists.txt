# arm64-runtime regressions. Built from port/android/CMakeLists.txt (targets
# xr_* exist there) or standalone on a native host for the SIMD comparison:
#   cmake -S tests/arm64_runtime -B build/a64rt-native && cmake --build ... && ctest ...
cmake_minimum_required(VERSION 3.20)
project(arm64_runtime_tests C)
enable_testing()

get_filename_component(_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_xr "${_root}/third_party/xboxrecomp")
if(NOT SSE2NEON_DIR)
    set(SSE2NEON_DIR /root/deps/sse2neon)
endif()
find_package(Threads REQUIRED)

# The digest every host path must print (captured from the SSE path).
set(SIMD_DIGEST 6230F862060A37AC)

function(simd_test name)
    add_executable(${name} test_simd.c)
    target_include_directories(${name} PRIVATE ${_xr}/templates/runtime ${SSE2NEON_DIR})
    target_compile_options(${name} PRIVATE -O2 -ffp-contract=off ${ARGN})
    target_link_libraries(${name} PRIVATE m Threads::Threads)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION
        " 0 failures, digest ${SIMD_DIGEST}")
endfunction()

# Whatever the host is (SSE on x86-64, sse2neon on arm64) ...
simd_test(arm64rt_simd_native)
# ... and the lane-wise C path on the same host.
simd_test(arm64rt_simd_portable -DRECOMP_FORCE_PORTABLE)

# Case-insensitive path resolution (kernel_path.c), only where the runtime
# libraries are available (the Android build).
if(TARGET xr_kernel)
    add_library(arm64rt_stubs STATIC runtime_stubs.c)
    add_library(arm64rt_stubs_nolookup STATIC runtime_stubs.c)
    target_compile_definitions(arm64rt_stubs_nolookup PRIVATE STUBS_NO_LOOKUP)
    add_executable(arm64rt_path_case test_path_case.c)
    target_link_libraries(arm64rt_path_case PRIVATE xr_kernel arm64rt_stubs)
    add_test(NAME arm64rt_path_case COMMAND arm64rt_path_case)

    # The memory model on Android arm64: memfd mirrors, 4 KB pages, the high
    # host base, teardown + re-init. Same test as tests/memory_layout_posix.
    add_executable(memory_layout_posix_a64 ${_xr}/tests/memory_layout_posix/test_main.c)
    target_include_directories(memory_layout_posix_a64 PRIVATE ${_xr}/src/kernel)
    target_link_libraries(memory_layout_posix_a64 PRIVATE xr_kernel arm64rt_stubs_nolookup)
    add_test(NAME memory_layout_posix_a64
             COMMAND memory_layout_posix_a64 ${_xr}/tools/conformance/test.xbe)

    # AAudio backend logic against a fake AAudio (device-free).
    add_executable(arm64rt_aaudio_ring test_aaudio_ring.c ${_xr}/src/apu/apu_aaudio.c)
    target_include_directories(arm64rt_aaudio_ring PRIVATE ${_xr}/src/apu)
    target_link_libraries(arm64rt_aaudio_ring PRIVATE arm64rt_stubs Threads::Threads)
    add_test(NAME arm64rt_aaudio_ring COMMAND arm64rt_aaudio_ring)
endif()
