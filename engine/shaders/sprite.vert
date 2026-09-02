#version 450

// Textured, tinted quads. One pipeline draws everything Rewind renders:
// sprites, tiles, shapes (through a 1x1 white texture) and debug text.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

// SDL GPU convention: vertex uniform buffers live in set 1.
layout(set = 1, binding = 0) uniform Uniforms {
    vec2 scale;  // pixel -> NDC
    vec2 offset;
};

void main() {
    gl_Position = vec4(in_pos * scale + offset, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
