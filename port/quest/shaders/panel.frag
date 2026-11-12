#version 450
// Settings panel: texture (the font atlas) x vertex colour. Blended "over" onto a cleared (0,0,0,0) target with
// alpha ONE / ONE_MINUS_SRC_ALPHA, which leaves premultiplied colour for the compositor's quad layer.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 col;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
    o = col * texture(tex, uv);
}
