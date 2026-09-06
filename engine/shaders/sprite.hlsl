// Same pipeline as sprite.vert / sprite.frag, for Direct3D 12 builds made
// without glslc. Compiled at runtime with D3DCompile (shader model 5.1).
//
// SDL GPU register conventions for DXBC:
//   vertex   uniform buffers  b[n], space1
//   fragment textures         t[n], space2   samplers s[n], space2

cbuffer Uniforms : register(b0, space1) {
    float4x4 view_projection; // column-major in memory, like the GLSL twin
};

struct VSIn {
    float3 pos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = mul(view_projection, float4(i.pos, 1.0));
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

Texture2D tex : register(t0, space2);
SamplerState smp : register(s0, space2);

float4 ps_main(VSOut i) : SV_Target {
    return tex.Sample(smp, i.uv) * i.color;
}
