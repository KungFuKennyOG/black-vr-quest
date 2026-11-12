# BLACK VR - Quest 3 app shell (libblackvr.so), built with the Android NDK.
#
#   cmake -G Ninja -S port/quest -B build-quest \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
#
# port/quest/build_apk.sh does this, then packages and signs the APK.
#
# Game / runtime / renderer linkage
#   BLACKVR_WITH_GAME=OFF (default): smoke build. The shell, OpenXR, Vulkan,
#     input and the test pattern only. Runs with no game files.
#   BLACKVR_WITH_GAME=ON : links the game and runtime static libraries the other
#     streams produce. Expected CMake targets (or imported libraries, see
#     BLACKVR_PREBUILT_LIBS):
#        black_game      src/main.c (defines black_game_main under __ANDROID__),
#                        src/recomp_manual.c, src/recomp/gen/*.c, built with -DBLACK_VR
#        xboxrecomp      the runtime umbrella (xbox_kernel, xbox_d3d8, xbox_dsound,
#                        xbox_input, xbox_usb, xbox_apu, xbox_nv2a, xbox_video, platform),
#                        xbox_input built with -DBLACK_VR
#        black_vk_renderer  src/vr_renderer_kvk.c (defined here when the runtime build
#                        provides kelvin_vk_shadergen): vr_renderer_get_requirements,
#                        vr_renderer_init, vr_renderer_shutdown, vr_mark_world_pass on the
#                        Kelvin Vulkan backend (port/notes/vr-glue.md)
#     BLACKVR_GAME_CMAKE: a CMake file that defines those targets (included here),
#     default ${BLACK_ROOT}/port/android/game.cmake (owned by the runtime stream).
#     Any target that is missing is replaced by the shell's weak stub and reported.
cmake_minimum_required(VERSION 3.20)
project(blackvr C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(BLACK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(OPENXR_SDK_DIR "/root/deps/OpenXR-SDK" CACHE PATH "OpenXR-SDK (headers)")
set(OPENXR_LOADER_SO "/root/deps/build-openxr-android/src/loader/libopenxr_loader.so" CACHE FILEPATH
    "libopenxr_loader.so built for arm64-v8a (port/quest/build_apk.sh builds it)")
option(BLACKVR_WITH_GAME "Link the recompiled game, runtime and renderer" OFF)
set(BLACKVR_GAME_CMAKE "${BLACK_ROOT}/port/android/game.cmake" CACHE FILEPATH "Defines black_game / xboxrecomp / black_vk_renderer")
if(NOT EXISTS "${BLACKVR_GAME_CMAKE}")
    set(BLACKVR_GAME_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/game_targets.cmake")
endif()
set(BLACKVR_PREBUILT_LIBS "" CACHE STRING "Extra static libraries to link (paths), for out-of-tree builds")

if(NOT ANDROID)
    message(FATAL_ERROR "port/quest builds for Android only; use the NDK toolchain file")
endif()

# ---- shaders -> C arrays -------------------------------------------------------
find_program(GLSLC glslc HINTS "${ANDROID_NDK}/shader-tools/${ANDROID_NDK_HOST_SYSTEM_NAME}" REQUIRED)
set(SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
set(SHADER_INCS)
foreach(stage vert frag)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/testpattern.${stage}")
    set(raw "${SHADER_OUT}/testpattern.${stage}.raw")
    set(inc "${SHADER_OUT}/testpattern.${stage}.inc")
    add_custom_command(OUTPUT "${inc}"
        COMMAND "${GLSLC}" --target-env=vulkan1.1 -O -mfmt=c -o "${raw}" "${src}"
        COMMAND "${CMAKE_COMMAND}" -DIN=${raw} -DOUT=${inc} -DNAME=testpattern_${stage}_spv
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake"
        VERBATIM)
    list(APPEND SHADER_INCS "${inc}")
endforeach()
# the in-headset settings panel: Dear ImGui (third_party/imgui), its shaders and font (port/notes/settings-panel.md)
set(PANEL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/panel")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/settings_panel.cmake")
add_custom_target(blackvr_shaders DEPENDS ${SHADER_INCS} ${PANEL_GEN_INCS})

# ---- native_app_glue ----------------------------------------------------------------
add_library(native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")

# ---- OpenXR loader -----------------------------------------------------------------------
# build_apk.sh: built from source (OPENXR_LOADER_SO). Gradle: Khronos' prefab package.
option(BLACKVR_USE_PREFAB_LOADER "Use the OpenXR loader prefab package (Gradle)" OFF)
if(BLACKVR_USE_PREFAB_LOADER)
    find_package(OpenXR REQUIRED CONFIG)
    add_library(openxr_loader INTERFACE)
    target_link_libraries(openxr_loader INTERFACE OpenXR::openxr_loader)
    target_include_directories(openxr_loader INTERFACE "${OPENXR_SDK_DIR}/include")
else()
    add_library(openxr_loader SHARED IMPORTED)
    set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION "${OPENXR_LOADER_SO}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENXR_SDK_DIR}/include")
endif()

# ---- the shell -----------------------------------------------------------------------
add_library(blackvr SHARED
    src/xr_main.c
    src/xr_session.c
    src/xr_vulkan.c
    src/vr_bridge.c
    src/bvr_diag.c          # device preflight, process checks (port/notes/device-preflight.md)
    src/xr_preflight.c      # device preflight, Vulkan / OpenXR / AAudio / ActivityManager
    src/vr_settings_menu.c  # the VR settings state: the game's VR SETTINGS page (vr_menu_*) and the panel
    src/xr_panel.c          # the floating settings panel (fallback): layers, input
    src/vr_panel_ui.cpp     # the settings panel's Dear ImGui UI and its Vulkan drawing
)
set_source_files_properties(src/vr_panel_ui.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti;-fvisibility=hidden")
add_dependencies(blackvr blackvr_shaders)
target_include_directories(blackvr PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}" "${SHADER_OUT}" "${PANEL_GEN_DIR}")
target_compile_options(blackvr PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -O2 -fno-strict-aliasing)
target_compile_definitions(blackvr PRIVATE XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_VULKAN VK_USE_PLATFORM_ANDROID_KHR)
# ANativeActivity_onCreate must stay exported; 16 KB pages for Android 15+ devices.
# -Bsymbolic-functions: calls to functions defined in this library bind inside it (a direct BL, not a PLT stub and an
# indirect branch: ~9,800 of them were PLT calls, mostly generated code calling across its chunks). The symbols stay
# exported, so the crash reporter still names sub_XXXXXXXX (dladdr). Nothing interposes on them. port/notes/perf.md 9.
target_link_options(blackvr PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384 -Wl,--no-undefined
                    -Wl,-Bsymbolic-functions)
target_link_libraries(blackvr PRIVATE native_app_glue openxr_loader blackvr_imgui vulkan android log aaudio m dl)
# For the preflight banner and its XBE check: the version, and the XBE the recompiled code was generated from (a
# --with-game APK only; the copy on the headset is compared with it: size and the first 16 signature bytes).
if(EXISTS "${BLACK_ROOT}/VERSION")
    file(STRINGS "${BLACK_ROOT}/VERSION" _bvr_version LIMIT_COUNT 1)
    target_compile_definitions(blackvr PRIVATE BLACKVR_VERSION="${_bvr_version}")
endif()
if(BLACKVR_WITH_GAME AND EXISTS "${BLACK_ROOT}/game/default.xbe")
    file(SIZE "${BLACK_ROOT}/game/default.xbe" _bvr_xbe_size)
    file(READ "${BLACK_ROOT}/game/default.xbe" _bvr_xbe_sig OFFSET 4 LIMIT 16 HEX)
    target_compile_definitions(blackvr PRIVATE BLACKVR_XBE_SIZE=${_bvr_xbe_size} BLACKVR_XBE_SIG="${_bvr_xbe_sig}")
    message(STATUS "preflight XBE check: ${_bvr_xbe_size} bytes, signature ${_bvr_xbe_sig}")
endif()

if(BLACKVR_WITH_GAME)
    if(EXISTS "${BLACKVR_GAME_CMAKE}")
        include("${BLACKVR_GAME_CMAKE}")
    else()
        message(WARNING "BLACKVR_GAME_CMAKE (${BLACKVR_GAME_CMAKE}) not found")
    endif()
    # The renderer glue (vr-glue stream): the shell's renderer hooks on the Kelvin Vulkan backend. The backend's
    # objects live in xbox_nv2a (KELVIN_GFX_BACKEND=vulkan); this needs its headers and the per-eye math.
    if(NOT TARGET black_vk_renderer AND TARGET kelvin_vk_shadergen)
        add_library(black_vk_renderer STATIC src/vr_renderer_kvk.c)
        target_include_directories(black_vk_renderer PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}"
            "${BLACK_ROOT}/third_party/xboxrecomp/src/nv2a/kelvin/vk")
        target_compile_options(black_vk_renderer PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
        target_link_libraries(black_vk_renderer PRIVATE kelvin_vk_shadergen vulkan log)
    endif()
    foreach(t black_game xboxrecomp black_vk_renderer)
        if(TARGET ${t})
            target_link_libraries(blackvr PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,${t}>)
            message(STATUS "linking ${t}")
        else()
            message(WARNING "target ${t} not available: stubbed (weak defaults in the shell)")
        endif()
    endforeach()
    if(TARGET black_game)
        target_compile_definitions(blackvr PRIVATE BLACKVR_HAVE_GAME=1)
    endif()
endif()
if(BLACKVR_PREBUILT_LIBS)
    target_link_libraries(blackvr PRIVATE -Wl,--whole-archive ${BLACKVR_PREBUILT_LIBS} -Wl,--no-whole-archive)
endif()
