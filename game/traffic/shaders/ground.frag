#version 450

// The ground: mottled grass with worn patches, generated rather than
// painted, so the game needs no art for the largest thing on screen.
//
// The mesh feeds world metres in through the uv channel, so the pattern is
// nailed to the world: it does not swim when the camera pans and it gains
// detail when you zoom in. Everything here is presentation.

// The engine always binds a texture in slot 0 (a white 1x1 pixel unless the
// game sets one), and a material must declare at least one sampler, so the
// shader multiplies by it: white leaves the colour alone.
layout(set = 2, binding = 0) uniform sampler2D tex;

layout(set = 3, binding = 0) uniform Params {
    vec4 grass_dark;   // rgb + unused
    vec4 grass_light;
    vec4 soil;         // worn earth showing through
    vec2 detail;       // x: metres per noise cell, y: how much fine detail survives at this zoom
};

layout(location = 0) in vec2 v_uv;   // world position in metres
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

/// Integer hash: no visible axis structure, unlike the usual sin-based ones.
float hash(vec2 cell) {
    uvec2 p = uvec2(ivec2(floor(cell)) + 0x8000);
    uint h = p.x * 0x27d4eb2du ^ p.y * 0x85ebca6bu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return float(h & 0xffffffu) / 16777215.0;
}

/// Value noise with a quintic fade, so neighbouring cells meet without the
/// square creases a cubic fade leaves behind.
float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}

/// Five octaves, each rotated, so nothing lines up with the world axes.
float fbm(vec2 p, int octaves) {
    const mat2 turn = mat2(0.8, 0.6, -0.6, 0.8);
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += value_noise(p) * amplitude;
        total += amplitude;
        p = turn * p * 2.03 + 17.3;
        amplitude *= 0.5;
    }
    return sum / total;
}

void main() {
    vec2 p = v_uv / max(detail.x, 0.01);

    // Warp the field by itself: straight-edged cells become organic patches.
    vec2 warp = vec2(fbm(p * 0.35 + 4.1, 3), fbm(p * 0.35 - 7.7, 3)) - 0.5;
    p += warp * 1.6;

    // Large slow patches of lighter and darker grass, plus a finer weave.
    float patches = fbm(p * 0.3, 4);
    float weave = fbm(p * 1.4, 3);
    float shade = clamp(patches * 0.75 + weave * 0.25, 0.0, 1.0);
    vec3 color = mix(grass_dark.rgb, grass_light.rgb, smoothstep(0.3, 0.72, shade));

    // Bare earth only where the turf is thinnest, and never as a hard edge.
    float wear = smoothstep(0.34, 0.18, patches) * smoothstep(0.5, 0.78, fbm(p * 0.8 + 13.0, 3));
    color = mix(color, soil.rgb, clamp(wear, 0.0, 1.0) * 0.4);

    // Fine speckle, faded out when a pixel covers many metres so it never
    // turns into noise when zoomed out.
    float speckle = fbm(p * 4.0, 2) - 0.5;
    color += speckle * 0.06 * detail.y;

    out_color = vec4(color, 1.0) * v_color * texture(tex, vec2(0.5));
}
