#version 450

// Textured, tinted triangles. One pipeline draws everything Rewind renders:
// sprites, tiles, shapes (through a 1x1 white texture), text and meshes.
// Positions carry z and go through a full view-projection matrix, so the
// same pipeline serves 2D layers today and 3D later.

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

// SDL GPU convention: vertex uniform buffers live in set 1.
layout(set = 1, binding = 0) uniform Uniforms {
    mat4 view_projection;
};

void main() {
    gl_Position = view_projection * vec4(in_pos, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
