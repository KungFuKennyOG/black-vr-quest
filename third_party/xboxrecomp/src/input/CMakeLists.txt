# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    pc_input.c
    pc_controls.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(BLACK_VR)
    # Quest: the pad comes from the XR shell (port/quest/vr_bridge.h), no SDL2.
    get_filename_component(BLACK_VR_QUEST "${CMAKE_CURRENT_SOURCE_DIR}/../../../../port/quest" ABSOLUTE)
    target_compile_definitions(xbox_input PRIVATE BLACK_VR)
    target_include_directories(xbox_input PRIVATE "${BLACK_VR_QUEST}" "${BLACK_VR_QUEST}/src")
    target_link_libraries(xbox_input PUBLIC platform)
elseif(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
