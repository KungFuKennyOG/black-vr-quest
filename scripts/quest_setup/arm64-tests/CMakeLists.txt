# QEMU linux-user self-tests for the static NDK toolchain (see port/notes/arm64-exec.md).
#   cmake -G Ninja -S scripts/quest_setup/arm64-tests -B <build> -DCMAKE_TOOLCHAIN_FILE=<xtool>/android-arm64-static.cmake
#   cmake --build <build> && ctest --test-dir <build> --output-on-failure      # 7 tests
cmake_minimum_required(VERSION 3.20)
project(arm64exec_selftest C CXX)
enable_testing()
foreach(t hello neon malloc segv io)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} m)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
add_executable(cxx cxx.cpp)
add_test(NAME cxx COMMAND cxx)
add_test(NAME hello_exit7 COMMAND hello 7)
set_tests_properties(hello_exit7 PROPERTIES WILL_FAIL TRUE)
