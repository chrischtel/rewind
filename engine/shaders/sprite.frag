#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

// SDL GPU convention: fragment sampled textures live in set 2.
layout(set = 2, binding = 0) uniform sampler2D tex;

void main() {
    out_color = texture(tex, v_uv) * v_color;
}
