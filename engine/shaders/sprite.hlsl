// Same pipeline as sprite.vert / sprite.frag, for Direct3D 12. Compiled at
// runtime with D3DCompile (shader model 5.1, DXBC), so no build step and
// no Windows SDK tools are needed to run on Windows.
//
// SDL GPU register conventions for DXBC:
//   vertex   uniform buffers  b[n], space1
//   fragment textures         t[n], space2   samplers s[n], space2

cbuffer Uniforms : register(b0, space1) {
    float2 scale;
    float2 offset;
};

struct VSIn {
    float2 pos : TEXCOORD0;
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
    o.pos = float4(i.pos * scale + offset, 0.0, 1.0);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

Texture2D tex : register(t0, space2);
SamplerState smp : register(s0, space2);

float4 ps_main(VSOut i) : SV_Target {
    return tex.Sample(smp, i.uv) * i.color;
}
