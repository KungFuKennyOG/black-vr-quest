# The in-headset settings panel's build pieces (port/notes/settings-panel.md), shared by port/quest/CMakeLists.txt
# and tests/vr_glue: Dear ImGui (third_party/imgui, vendored release v1.93.0) as the static library blackvr_imgui,
# and the generated includes (panel shaders -> SPIR-V arrays, the Roboto font -> a byte array) in PANEL_GEN_INCS.
# Needs: BLACK_ROOT, GLSLC, PANEL_GEN_DIR (an output directory, added to the include path of the panel sources).
set(_quest "${BLACK_ROOT}/port/quest")
set(IMGUI_DIR "${BLACK_ROOT}/third_party/imgui")
file(MAKE_DIRECTORY "${PANEL_GEN_DIR}")

add_library(blackvr_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(blackvr_imgui PUBLIC "${IMGUI_DIR}")
# no ini/log files, no demo or debug windows, no default fonts (the panel embeds Roboto Medium), no exceptions/RTTI
target_compile_definitions(blackvr_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS IMGUI_DISABLE_DEFAULT_FONT)
target_compile_options(blackvr_imgui PRIVATE -O2 -fno-exceptions -fno-rtti -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -w)
set_target_properties(blackvr_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(PANEL_GEN_INCS)
foreach(stage vert frag)
    set(src "${_quest}/shaders/panel.${stage}")
    set(raw "${PANEL_GEN_DIR}/panel.${stage}.raw")
    set(inc "${PANEL_GEN_DIR}/panel.${stage}.inc")
    add_custom_command(OUTPUT "${inc}"
        COMMAND "${GLSLC}" --target-env=vulkan1.1 -O -mfmt=c -o "${raw}" "${src}"
        COMMAND "${CMAKE_COMMAND}" -DIN=${raw} -DOUT=${inc} -DNAME=panel_${stage}_spv -P "${_quest}/cmake/wrap_spirv.cmake"
        DEPENDS "${src}" "${_quest}/cmake/wrap_spirv.cmake" VERBATIM)
    list(APPEND PANEL_GEN_INCS "${inc}")
endforeach()
set(_font_inc "${PANEL_GEN_DIR}/roboto_medium.inc")
add_custom_command(OUTPUT "${_font_inc}"
    COMMAND "${CMAKE_COMMAND}" -DIN=${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf -DOUT=${_font_inc} -DNAME=roboto_medium_ttf
            -P "${_quest}/cmake/bin2c.cmake"
    DEPENDS "${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf" "${_quest}/cmake/bin2c.cmake" VERBATIM)
list(APPEND PANEL_GEN_INCS "${_font_inc}")
