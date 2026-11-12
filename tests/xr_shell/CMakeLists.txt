# Native tests for the Quest XR shell math, input mapping, the Quest button prompts and the BLACK_VR game hooks.
#   cmake -S tests/xr_shell -B build-xr-shell && cmake --build build-xr-shell && ctest --test-dir build-xr-shell
# No game files, OpenXR runtime or GPU needed.
cmake_minimum_required(VERSION 3.20)
project(black_xr_shell_test C)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(QUEST "${CMAKE_CURRENT_SOURCE_DIR}/../../port/quest")
set(extracted "${CMAKE_CURRENT_BINARY_DIR}/black_vr.extracted.inc")
add_custom_command(OUTPUT "${extracted}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract.py" "${extracted}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/recomp_manual.c" "${CMAKE_CURRENT_SOURCE_DIR}/../../src/black_vr.inc"
            "${CMAKE_CURRENT_SOURCE_DIR}/extract.py")
foreach(t math input hooks settings prompts arms)
    add_executable(xr-shell-${t} test_${t}.c)
    target_include_directories(xr-shell-${t} PRIVATE "${QUEST}" "${QUEST}/src" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(xr-shell-${t} PRIVATE m)
    if(MSVC)
        target_compile_options(xr-shell-${t} PRIVATE /W4)
    else()
        target_compile_options(xr-shell-${t} PRIVATE -Wall -Wextra -Wno-unused-function -frounding-math -fno-fast-math)
    endif()
endforeach()
target_sources(xr-shell-hooks PRIVATE "${extracted}")
enable_testing()
foreach(t math input hooks settings prompts arms)
    add_test(NAME xr-shell-${t} COMMAND xr-shell-${t})
endforeach()
# The same hooks against a game whose RenderWare right row is the screen's right:
# the upload self-check must notice and flip the convention.
add_executable(xr-shell-hooks-flipped test_hooks.c "${extracted}")
target_include_directories(xr-shell-hooks-flipped PRIVATE "${QUEST}" "${QUEST}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(xr-shell-hooks-flipped PRIVATE TEST_RIGHT_ROW_SIGN=1.0f)
target_link_libraries(xr-shell-hooks-flipped PRIVATE m)
add_test(NAME xr-shell-hooks-flipped COMMAND xr-shell-hooks-flipped)
