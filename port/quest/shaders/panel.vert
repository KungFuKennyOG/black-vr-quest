#version 450
// Settings panel (vr_panel_ui.cpp): Dear ImGui vertices in panel pixels -> clip space.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_col;          // R8G8B8A8_UNORM, straight alpha, sRGB-encoded
layout(push_constant) uniform PC { vec2 scale; vec2 translate; } pc;
layout(location = 0) out vec4 col;
layout(location = 1) out vec2 uv;
void main()
{
    col = in_col;
    uv = in_uv;
    gl_Position = vec4(in_pos * pc.scale + pc.translate, 0.0, 1.0);
}
