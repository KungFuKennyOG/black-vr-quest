# Fallback definition of the game targets for BLACKVR_WITH_GAME=ON, used when the
# runtime stream has not provided port/android/game.cmake yet. It is a starting point:
# it defines the targets only when their inputs exist, and leaves the Android build of
# the runtime (platform/kernel/d3d/nv2a ports) to the runtime and renderer streams.
#
#   black_game        src/main.c + src/recomp_manual.c + src/recomp/gen/*.c, -DBLACK_VR
#   xboxrecomp        add_subdirectory(third_party/xboxrecomp) with BLACK_VR=ON
#   black_vk_renderer whatever the renderer stream adds under third_party/xboxrecomp/src/nv2a
option(BLACKVR_FALLBACK_GAME_TARGETS "Try to build the runtime/game from this sketch (needs an Android-ready runtime)" OFF)
if(NOT BLACKVR_FALLBACK_GAME_TARGETS)
    message(WARNING "port/android/game.cmake (runtime stream) not found; game/runtime/renderer stubbed. "
                    "-DBLACKVR_FALLBACK_GAME_TARGETS=ON tries this sketch instead.")
    return()
endif()
set(BLACK_VR ON CACHE BOOL "xboxrecomp input: Quest backend" FORCE)
file(GLOB BLACKVR_GEN CONFIGURE_DEPENDS "${BLACK_ROOT}/src/recomp/gen/*.c")
if(NOT BLACKVR_GEN)
    message(WARNING "src/recomp/gen is empty: generate the recompiled code from your own XBE first "
                    "(scripts/setup-linux.sh); building without the game")
    return()
endif()
if(NOT TARGET xboxrecomp)
    add_subdirectory("${BLACK_ROOT}/third_party/xboxrecomp" "${CMAKE_BINARY_DIR}/xboxrecomp")
endif()
add_library(black_game STATIC "${BLACK_ROOT}/src/main.c" "${BLACK_ROOT}/src/recomp_manual.c" ${BLACKVR_GEN})
target_compile_definitions(black_game PRIVATE BLACK_VR WIN32_LEAN_AND_MEAN NOMINMAX BLACK_VERSION="quest")
target_include_directories(black_game PRIVATE "${BLACK_ROOT}/src" "${BLACK_ROOT}/src/recomp"
    "${BLACK_ROOT}/third_party/xboxrecomp/include" "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../src")
target_link_libraries(black_game PRIVATE xboxrecomp)
