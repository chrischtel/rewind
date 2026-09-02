#version 450

// Palette swap: buckets the sprite's brightness into four steps and looks
// each step up in a 4x1 palette texture. `mix_amount` blends between the
// original colours and the swapped ones.

layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 2, binding = 1) uniform sampler2D palette;

layout(set = 3, binding = 0) uniform Params {
    float mix_amount;
    float steps;
};

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 c = texture(tex, v_uv);
    if (c.a < 0.01) discard;
    float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float n = max(steps, 1.0);
    float index = floor(lum * (n - 0.001));
    vec4 swapped = texture(palette, vec2((index + 0.5) / n, 0.5));
    out_color = vec4(mix(c.rgb, swapped.rgb, mix_amount), c.a) * v_color;
}
