#include "rewind/render/renderer2d.h"

#include "debug_font.h"
#include "rewind/core/file_watcher.h"
#include "rewind/core/log.h"
#include "rewind/core/paths.h"
#include "rewind/project/cooked.h"
#include "rewind/render/font.h"
#include "rewind/render/material.h"
#include "rewind/render/shader_cross.h"
#include "rewind_shaders_spv.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <d3dcompiler.h>
#include <windows.h>
#endif

namespace rw {

namespace {

constexpr u32 kMaxExtraTextures = 3;
constexpr usize kMaxParamsBytes = 256;

// Fallback for Windows builds made without glslc: the sprite shader as HLSL.
const char kSpriteHlsl[] = R"HLSL(
cbuffer Uniforms : register(b0, space1) { float4x4 view_projection; };
struct VSIn { float3 pos : TEXCOORD0; float2 uv : TEXCOORD1; float4 color : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : TEXCOORD1; };
VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = mul(view_projection, float4(i.pos, 1.0));
    o.uv = i.uv;
    o.color = i.color;
    return o;
}
Texture2D tex : register(t0, space2);
SamplerState smp : register(s0, space2);
float4 ps_main(VSOut i) : SV_Target { return tex.Sample(smp, i.uv) * i.color; }
)HLSL";

/// GPU vertex: matches MeshVertex byte for byte, and the attribute layout
/// in make_pipeline().
struct Vertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
};
static_assert(sizeof(Vertex) == sizeof(MeshVertex), "Vertex and MeshVertex must match");

struct Uniforms {
    float view_projection[16]; ///< column-major, see Mat4
};

struct Batch {
    SDL_GPUGraphicsPipeline* pipeline;
    SDL_GPUTexture* texture;
    SDL_GPUSampler* sampler;
    SDL_GPUTexture* extra_tex[kMaxExtraTextures];
    SDL_GPUSampler* extra_smp[kMaxExtraTextures];
    u32 extra_count;
    u32 params_offset;
    u32 params_size;
    u32 first_index;
    u32 index_count;
    i32 layer;
};

/// A stretch of drawing with one target, one view-projection and one depth
/// setting. Each becomes a render pass.
struct Segment {
    Texture* target; ///< nullptr = window
    bool has_clear = false;
    Color clear_color{};
    bool custom_vp = false;
    Mat4 vp;
    bool depth = false;
    usize first_batch = 0;
    usize batch_count = 0;
};

bool read_file(const std::string& path, std::vector<u8>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    u8 buffer[16 * 1024];
    usize n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
    return true;
}

std::string read_text(const std::string& path) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) return {};
    std::string s(bytes.begin(), bytes.end());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

#if defined(_WIN32)
bool compile_dxbc(const char* source, const char* entry, const char* target, std::vector<u8>& out,
                  std::string& error) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "shader.hlsl", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown D3DCompile error";
        if (errors) errors->Release();
        return false;
    }
    out.assign(static_cast<const u8*>(code->GetBufferPointer()),
               static_cast<const u8*>(code->GetBufferPointer()) + code->GetBufferSize());
    code->Release();
    if (errors) errors->Release();
    return true;
}
#endif


} // namespace

struct Renderer2D::Impl {
    SDL_GPUDevice* device = nullptr;
    SDL_Window* window = nullptr;
    bool ok = false;

    SDL_GPUTextureFormat target_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUShader* sprite_vs = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_depth = nullptr;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    struct DepthBuffer {
        u32 w, h;
        SDL_GPUTexture* texture;
    };
    std::vector<DepthBuffer> depth_buffers; ///< one per target size, made on demand
    SDL_GPUSampler* sampler_nearest = nullptr;
    SDL_GPUSampler* sampler_linear = nullptr;

    std::unordered_map<std::string, std::unique_ptr<Texture>> textures;
    std::unordered_map<std::string, std::unique_ptr<Material>> materials;
    std::unordered_map<std::string, std::unique_ptr<Font>> fonts;
    std::unordered_set<std::string> stale_files; ///< loaded from disk, changed since
    std::unordered_map<std::string, std::string> aliases; ///< resolved path -> name the game used
    FileWatcher* watcher = nullptr;
    Texture* white = nullptr;
    Texture* font = nullptr;

    // Per-frame recording.
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::vector<Batch> batches;
    std::vector<Segment> segments;
    std::vector<u8> params_blob;
    SDL_GPUCommandBuffer* cmd = nullptr;
    SDL_GPUTexture* swapchain = nullptr;
    u32 swapchain_w = 0, swapchain_h = 0;
    bool frame_active = false;
    Texture* current_target = nullptr;
    bool current_custom_vp = false;
    Mat4 current_vp;
    bool current_depth = false;
    i32 current_layer = 0;
    Material* current_material = nullptr;
    u8 current_params[kMaxParamsBytes] = {};
    u32 current_params_size = 0;
    Texture* extra[kMaxExtraTextures] = {};
    int stat_draw_calls = 0;
    int stat_vertices = 0;
    int last_draw_calls = 0;
    int last_vertices = 0;

    // GPU buffers, grown on demand.
    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUBuffer* index_buffer = nullptr;
    SDL_GPUTransferBuffer* transfer = nullptr;
    u32 vertex_capacity = 0;
    u32 index_capacity = 0;
    u32 transfer_capacity = 0;

    // ---- shaders and pipelines --------------------------------------------

    /// Create a shader from SPIR-V on any backend: directly on Vulkan, through
    /// HLSL and DXBC on Direct3D 12.
    SDL_GPUShader* shader_from_spirv(const u8* spv, usize size, SDL_GPUShaderStage stage, u32 samplers,
                                     u32 uniforms, std::string& error) {
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
        SDL_GPUShaderCreateInfo info{};
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        info.entrypoint = "main";

        if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.code = spv;
            info.code_size = size;
            SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
            if (!s) error = SDL_GetError();
            return s;
        }
#if defined(_WIN32)
        if (formats & SDL_GPU_SHADERFORMAT_DXBC) {
            std::string hlsl;
            if (!spirv_to_hlsl(spv, size, hlsl, error)) return nullptr;
            std::vector<u8> code;
            const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;
            if (!compile_dxbc(hlsl.c_str(), "main", vertex ? "vs_5_1" : "ps_5_1", code, error)) return nullptr;
            info.format = SDL_GPU_SHADERFORMAT_DXBC;
            info.code = code.data();
            info.code_size = code.size();
            SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
            if (!s) error = SDL_GetError();
            return s;
        }
#endif
        error = "no usable shader format on this GPU driver";
        return nullptr;
    }

    /// The engine's sprite shaders: embedded SPIR-V when available, else the HLSL twin.
    SDL_GPUShader* sprite_shader(SDL_GPUShaderStage stage, std::string& error) {
        const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;
#if REWIND_HAVE_SPIRV
        return shader_from_spirv(vertex ? sprite_vert_spv : sprite_frag_spv,
                                 vertex ? sprite_vert_spv_size : sprite_frag_spv_size, stage, vertex ? 0 : 1,
                                 vertex ? 1 : 0, error);
#else
#if defined(_WIN32)
        if (SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_DXBC) {
            std::vector<u8> code;
            if (!compile_dxbc(kSpriteHlsl, vertex ? "vs_main" : "ps_main", vertex ? "vs_5_1" : "ps_5_1", code, error))
                return nullptr;
            SDL_GPUShaderCreateInfo info{};
            info.stage = stage;
            info.num_samplers = vertex ? 0 : 1;
            info.num_uniform_buffers = vertex ? 1 : 0;
            info.entrypoint = vertex ? "vs_main" : "ps_main";
            info.format = SDL_GPU_SHADERFORMAT_DXBC;
            info.code = code.data();
            info.code_size = code.size();
            SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
            if (!s) error = SDL_GetError();
            return s;
        }
#endif
        error = "engine built without SPIR-V and no Direct3D available";
        return nullptr;
#endif
    }

    /// Pick a depth format the driver supports. Called once.
    void choose_depth_format() {
        const SDL_GPUTextureFormat candidates[] = {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                                   SDL_GPU_TEXTUREFORMAT_D16_UNORM};
        for (SDL_GPUTextureFormat f : candidates)
            if (SDL_GPUTextureSupportsFormat(device, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
                depth_format = f;
                return;
            }
    }

    SDL_GPUTexture* depth_buffer_for(u32 w, u32 h) {
        for (const DepthBuffer& d : depth_buffers)
            if (d.w == w && d.h == h) return d.texture;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = depth_format;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(device, &info);
        if (!t) {
            RW_LOG_ERROR("gpu", "depth buffer %ux%u: %s", w, h, SDL_GetError());
            return nullptr;
        }
        SDL_SetGPUTextureName(device, t, "__depth");
        depth_buffers.push_back({w, h, t});
        return t;
    }

    /// Every pipeline shares one vertex layout and blend state; `depth`
    /// adds a less-or-equal depth test with writes for depth-tested passes.
    SDL_GPUGraphicsPipeline* make_pipeline(SDL_GPUShader* vs, SDL_GPUShader* fs, bool depth, std::string& error) {
        SDL_GPUVertexBufferDescription vb{};
        vb.slot = 0;
        vb.pitch = sizeof(Vertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[3]{};
        attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
        attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 3};
        attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 5};

        SDL_GPUColorTargetDescription color{};
        color.format = target_format;
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vs;
        info.fragment_shader = fs;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.vertex_input_state.vertex_buffer_descriptions = &vb;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attrs;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        if (depth) {
            info.target_info.has_depth_stencil_target = true;
            info.target_info.depth_stencil_format = depth_format;
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = true;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        }

        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!p) error = SDL_GetError();
        return p;
    }

    /// Both variants of a pipeline for one fragment shader. Returns false
    /// (with nothing created) on any failure.
    bool make_pipelines(SDL_GPUShader* fs, SDL_GPUGraphicsPipeline*& plain, SDL_GPUGraphicsPipeline*& with_depth,
                        std::string& error) {
        SDL_GPUGraphicsPipeline* p = make_pipeline(sprite_vs, fs, false, error);
        if (!p) return false;
        SDL_GPUGraphicsPipeline* d = nullptr;
        if (depth_format != SDL_GPU_TEXTUREFORMAT_INVALID) {
            d = make_pipeline(sprite_vs, fs, true, error);
            if (!d) {
                SDL_ReleaseGPUGraphicsPipeline(device, p);
                return false;
            }
        }
        plain = p;
        with_depth = d;
        return true;
    }

    bool create_sprite_pipeline() {
        std::string error;
        sprite_vs = sprite_shader(SDL_GPU_SHADERSTAGE_VERTEX, error);
        if (!sprite_vs) {
            RW_LOG_ERROR("gpu", "sprite vertex shader: %s", error.c_str());
            return false;
        }
        SDL_GPUShader* fs = sprite_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, error);
        if (!fs) {
            RW_LOG_ERROR("gpu", "sprite fragment shader: %s", error.c_str());
            return false;
        }
        const bool made = make_pipelines(fs, pipeline, pipeline_depth, error);
        SDL_ReleaseGPUShader(device, fs);
        if (!made) {
            RW_LOG_ERROR("gpu", "sprite pipeline: %s", error.c_str());
            return false;
        }
        return true;
    }

    bool create_samplers() {
        SDL_GPUSamplerCreateInfo info{};
        info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.min_filter = SDL_GPU_FILTER_NEAREST;
        info.mag_filter = SDL_GPU_FILTER_NEAREST;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampler_nearest = SDL_CreateGPUSampler(device, &info);
        info.min_filter = SDL_GPU_FILTER_LINEAR;
        info.mag_filter = SDL_GPU_FILTER_LINEAR;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        sampler_linear = SDL_CreateGPUSampler(device, &info);
        return sampler_nearest && sampler_linear;
    }

    /// (Re)build a material's pipeline from its .spv file. Keeps the old
    /// pipeline when anything fails so the game keeps rendering.
    bool build_material(Material& m, const std::string& file) {
        std::vector<u8> spv;
        if (!read_file(file, spv)) {
            m.error = "could not read " + file + " (was it built? SHADERS in rewind_add_game needs glslc)";
            return false;
        }
        ShaderInfo info;
        std::string error;
        if (!reflect_spirv(spv.data(), spv.size(), info, error)) {
            m.error = error;
            return false;
        }
        if (info.vertex) {
            m.error = "materials are fragment shaders, this is a vertex shader";
            return false;
        }
        if (info.samplers < 1 || info.samplers > 1 + kMaxExtraTextures) {
            m.error = "a material needs 1 to " + std::to_string(1 + kMaxExtraTextures) + " samplers, this one has " +
                      std::to_string(info.samplers);
            return false;
        }
        if (info.uniform_buffers > 1) {
            m.error = "a material may declare at most one uniform block (set 3, binding 0)";
            return false;
        }
        SDL_GPUShader* fs = shader_from_spirv(spv.data(), spv.size(), SDL_GPU_SHADERSTAGE_FRAGMENT, info.samplers,
                                              info.uniform_buffers, error);
        if (!fs) {
            m.error = error;
            return false;
        }
        SDL_GPUGraphicsPipeline* p = nullptr;
        SDL_GPUGraphicsPipeline* pd = nullptr;
        const bool made = make_pipelines(fs, p, pd, error);
        SDL_ReleaseGPUShader(device, fs);
        if (!made) {
            m.error = error;
            return false;
        }
        if (m.pipeline || m.pipeline_depth) {
            SDL_WaitForGPUIdle(device);
            if (m.pipeline) SDL_ReleaseGPUGraphicsPipeline(device, m.pipeline);
            if (m.pipeline_depth) SDL_ReleaseGPUGraphicsPipeline(device, m.pipeline_depth);
        }
        m.pipeline = p;
        m.pipeline_depth = pd;
        m.sampler_count = info.samplers;
        m.uniform_buffers = info.uniform_buffers;
        m.ok = true;
        m.error.clear();
        ++m.generation;
        return true;
    }

    // ---- textures ---------------------------------------------------------

    bool upload_pixels(SDL_GPUTexture* texture, int width, int height, const u8* rgba) {
        const u32 size = static_cast<u32>(width) * static_cast<u32>(height) * 4u;
        SDL_GPUTransferBufferCreateInfo tinfo{};
        tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tinfo.size = size;
        SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device, &tinfo);
        if (!staging) return false;
        void* mapped = SDL_MapGPUTransferBuffer(device, staging, false);
        if (!mapped) {
            SDL_ReleaseGPUTransferBuffer(device, staging);
            return false;
        }
        std::memcpy(mapped, rgba, size);
        SDL_UnmapGPUTransferBuffer(device, staging);

        SDL_GPUCommandBuffer* upload = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(upload);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = staging;
        SDL_GPUTextureRegion dst{};
        dst.texture = texture;
        dst.w = static_cast<u32>(width);
        dst.h = static_cast<u32>(height);
        dst.d = 1;
        SDL_UploadToGPUTexture(pass, &src, &dst, true);
        SDL_EndGPUCopyPass(pass);
        SDL_SubmitGPUCommandBuffer(upload);
        SDL_ReleaseGPUTransferBuffer(device, staging);
        return true;
    }

    Texture* make_texture(const std::string& name, int width, int height, bool render_target, bool nearest) {
        Texture* existing = nullptr;
        auto it = textures.find(name);
        if (it != textures.end()) {
            existing = it->second.get();
            if (existing->width == width && existing->height == height && existing->render_target == render_target)
                return existing;
            // Size changed (asset edited): swap the GPU texture underneath the
            // same object so every pointer a game holds stays valid.
            SDL_WaitForGPUIdle(device);
            SDL_ReleaseGPUTexture(device, existing->handle);
            existing->handle = nullptr;
        }

        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = render_target ? target_format : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (render_target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        info.width = static_cast<u32>(width);
        info.height = static_cast<u32>(height);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* handle = SDL_CreateGPUTexture(device, &info);
        if (!handle) {
            RW_LOG_ERROR("gpu", "create texture '%s' (%dx%d) failed: %s", name.c_str(), width, height, SDL_GetError());
            return nullptr;
        }
        SDL_SetGPUTextureName(device, handle, name.c_str());

        if (existing) {
            existing->width = width;
            existing->height = height;
            existing->handle = handle;
            existing->render_target = render_target;
            return existing;
        }
        auto owned = std::make_unique<Texture>();
        owned->width = width;
        owned->height = height;
        owned->handle = handle;
        owned->nearest = nearest;
        owned->render_target = render_target;
        Texture* result = owned.get();
        textures[name] = std::move(owned);
        return result;
    }

    SDL_GPUSampler* sampler_for(const Texture& t) const { return t.nearest ? sampler_nearest : sampler_linear; }

    // ---- recording --------------------------------------------------------

    /// Start a segment with the current target, matrix and depth setting.
    /// An empty trailing segment is reused instead of stacking up.
    void begin_segment() {
        if (!segments.empty() && segments.back().batch_count == 0 && !segments.back().has_clear) {
            segments.pop_back();
        }
        Segment s;
        s.target = current_target;
        s.custom_vp = current_custom_vp;
        s.vp = current_vp;
        s.depth = current_depth && depth_format != SDL_GPU_TEXTUREFORMAT_INVALID;
        s.first_batch = batches.size();
        segments.push_back(s);
    }

    void ensure_buffers() {
        const u32 vcount = static_cast<u32>(vertices.size());
        const u32 icount = static_cast<u32>(indices.size());
        if (vcount > vertex_capacity) {
            if (vertex_buffer) SDL_ReleaseGPUBuffer(device, vertex_buffer);
            vertex_capacity = std::max<u32>(vcount * 2, 4096);
            SDL_GPUBufferCreateInfo info{};
            info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            info.size = vertex_capacity * sizeof(Vertex);
            vertex_buffer = SDL_CreateGPUBuffer(device, &info);
        }
        if (icount > index_capacity) {
            if (index_buffer) SDL_ReleaseGPUBuffer(device, index_buffer);
            index_capacity = std::max<u32>(icount * 2, 6144);
            SDL_GPUBufferCreateInfo info{};
            info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
            info.size = index_capacity * sizeof(u32);
            index_buffer = SDL_CreateGPUBuffer(device, &info);
        }
        const u32 needed = vcount * sizeof(Vertex) + icount * sizeof(u32);
        if (needed > transfer_capacity) {
            if (transfer) SDL_ReleaseGPUTransferBuffer(device, transfer);
            transfer_capacity = std::max<u32>(needed * 2, 256 * 1024);
            SDL_GPUTransferBufferCreateInfo info{};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = transfer_capacity;
            transfer = SDL_CreateGPUTransferBuffer(device, &info);
        }
    }

    void upload_geometry() {
        if (vertices.empty() || indices.empty()) return;
        ensure_buffers();
        const u32 vbytes = static_cast<u32>(vertices.size() * sizeof(Vertex));
        const u32 ibytes = static_cast<u32>(indices.size() * sizeof(u32));

        u8* mapped = static_cast<u8*>(SDL_MapGPUTransferBuffer(device, transfer, true));
        if (!mapped) return;
        std::memcpy(mapped, vertices.data(), vbytes);
        std::memcpy(mapped + vbytes, indices.data(), ibytes);
        SDL_UnmapGPUTransferBuffer(device, transfer);

        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{transfer, 0};
        SDL_GPUBufferRegion dst{vertex_buffer, 0, vbytes};
        SDL_UploadToGPUBuffer(pass, &src, &dst, true);
        src.offset = vbytes;
        dst = SDL_GPUBufferRegion{index_buffer, 0, ibytes};
        SDL_UploadToGPUBuffer(pass, &src, &dst, true);
        SDL_EndGPUCopyPass(pass);
    }

    void record_passes(ImDrawData* overlay) {
        std::vector<SDL_GPUTexture*> cleared;
        for (usize si = 0; si < segments.size(); ++si) {
            const Segment& seg = segments[si];
            SDL_GPUTexture* texture = seg.target ? seg.target->handle : swapchain;
            const u32 w = seg.target ? static_cast<u32>(seg.target->width) : swapchain_w;
            const u32 h = seg.target ? static_cast<u32>(seg.target->height) : swapchain_h;
            if (!texture || w == 0 || h == 0) continue;

            const bool first_use = std::find(cleared.begin(), cleared.end(), texture) == cleared.end();
            const bool last = si + 1 == segments.size();
            if (seg.batch_count == 0 && !seg.has_clear && !first_use && !(last && overlay)) continue;

            SDL_GPUColorTargetInfo target{};
            target.texture = texture;
            target.store_op = SDL_GPU_STOREOP_STORE;
            if (seg.has_clear || first_use) {
                const Color c = seg.has_clear ? seg.clear_color : Color{0, 0, 0, 255};
                target.load_op = SDL_GPU_LOADOP_CLEAR;
                target.clear_color = SDL_FColor{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
            } else {
                target.load_op = SDL_GPU_LOADOP_LOAD;
            }
            if (first_use) cleared.push_back(texture);

            // Depth-tested segments get a depth buffer, cleared at the start of the pass.
            SDL_GPUDepthStencilTargetInfo depth{};
            SDL_GPUTexture* depth_tex = seg.depth ? depth_buffer_for(w, h) : nullptr;
            if (depth_tex) {
                depth.texture = depth_tex;
                depth.clear_depth = 1.0f;
                depth.load_op = SDL_GPU_LOADOP_CLEAR;
                depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
                depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
                depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                depth.cycle = true;
            }

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, depth_tex ? &depth : nullptr);
            if (seg.batch_count > 0) {
                SDL_GPUBufferBinding vbind{vertex_buffer, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
                SDL_GPUBufferBinding ibind{index_buffer, 0};
                SDL_BindGPUIndexBuffer(pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                const Mat4 vp = seg.custom_vp ? seg.vp : Mat4::ortho_pixels(static_cast<float>(w), static_cast<float>(h));
                Uniforms u{};
                std::memcpy(u.view_projection, vp.m, sizeof u.view_projection);
                SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);

                // Draw layers: lower first, recording order within a layer.
                std::stable_sort(batches.begin() + static_cast<std::ptrdiff_t>(seg.first_batch),
                                 batches.begin() + static_cast<std::ptrdiff_t>(seg.first_batch + seg.batch_count),
                                 [](const Batch& a, const Batch& b) { return a.layer < b.layer; });

                SDL_GPUGraphicsPipeline* bound = nullptr;
                for (usize b = seg.first_batch; b < seg.first_batch + seg.batch_count; ++b) {
                    const Batch& batch = batches[b];
                    if (batch.pipeline != bound) {
                        SDL_BindGPUGraphicsPipeline(pass, batch.pipeline);
                        bound = batch.pipeline;
                    }
                    if (batch.params_size > 0)
                        SDL_PushGPUFragmentUniformData(cmd, 0, params_blob.data() + batch.params_offset, batch.params_size);
                    SDL_GPUTextureSamplerBinding bindings[1 + kMaxExtraTextures];
                    bindings[0] = {batch.texture, batch.sampler};
                    for (u32 i = 0; i < batch.extra_count; ++i) bindings[1 + i] = {batch.extra_tex[i], batch.extra_smp[i]};
                    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 1 + batch.extra_count);
                    SDL_DrawGPUIndexedPrimitives(pass, batch.index_count, 1, batch.first_index, 0, 0);
                    ++stat_draw_calls;
                }
            }
            if (last && overlay) ImGui_ImplSDLGPU3_RenderDrawData(overlay, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }
    }

    void reset_frame() {
        vertices.clear();
        indices.clear();
        batches.clear();
        segments.clear();
        params_blob.clear();
        cmd = nullptr;
        swapchain = nullptr;
        frame_active = false;
        current_target = nullptr;
        current_custom_vp = false;
        current_depth = false;
        current_layer = 0;
        current_material = nullptr;
        current_params_size = 0;
        for (Texture*& t : extra) t = nullptr;
    }

    // ---- geometry helpers -------------------------------------------------

    Batch& batch_for(const Texture& t) {
        SDL_GPUSampler* sampler = sampler_for(t);
        Material* m = current_material && current_material->ok ? current_material : nullptr;
        const bool depth = segments.back().depth;
        SDL_GPUGraphicsPipeline* pipe = m ? (depth ? m->pipeline_depth : m->pipeline) : (depth ? pipeline_depth : pipeline);
        const u32 extra_count = m ? m->sampler_count - 1 : 0;
        const u32 params_size = m && m->uniform_buffers > 0 ? current_params_size : 0;

        SDL_GPUTexture* ex_tex[kMaxExtraTextures] = {};
        SDL_GPUSampler* ex_smp[kMaxExtraTextures] = {};
        for (u32 i = 0; i < extra_count; ++i) {
            const Texture* e = extra[i] ? extra[i] : white;
            ex_tex[i] = e->handle;
            ex_smp[i] = sampler_for(*e);
        }

        Segment& seg = segments.back();
        if (seg.batch_count > 0) {
            Batch& last = batches.back();
            bool same = last.pipeline == pipe && last.texture == t.handle && last.sampler == sampler &&
                        last.extra_count == extra_count && last.params_size == params_size &&
                        last.layer == current_layer;
            for (u32 i = 0; same && i < extra_count; ++i)
                same = last.extra_tex[i] == ex_tex[i] && last.extra_smp[i] == ex_smp[i];
            if (same && params_size > 0)
                same = std::memcmp(params_blob.data() + last.params_offset, current_params, params_size) == 0;
            if (same) return last;
        }

        Batch b{};
        b.pipeline = pipe;
        b.texture = t.handle;
        b.sampler = sampler;
        b.extra_count = extra_count;
        for (u32 i = 0; i < extra_count; ++i) {
            b.extra_tex[i] = ex_tex[i];
            b.extra_smp[i] = ex_smp[i];
        }
        b.params_size = params_size;
        b.params_offset = static_cast<u32>(params_blob.size());
        if (params_size > 0) params_blob.insert(params_blob.end(), current_params, current_params + params_size);
        b.first_index = static_cast<u32>(indices.size());
        b.index_count = 0;
        b.layer = current_layer;
        batches.push_back(b);
        ++seg.batch_count;
        return batches.back();
    }

    void push_quad(const Texture& t, const float px[4], const float py[4], float u0, float v0, float u1, float v1,
                   Color c) {
        if (!frame_active) return;
        Batch& batch = batch_for(t);
        const u32 base = static_cast<u32>(vertices.size());
        const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f, a = c.a / 255.0f;
        vertices.push_back(Vertex{px[0], py[0], 0, u0, v0, r, g, b, a});
        vertices.push_back(Vertex{px[1], py[1], 0, u1, v0, r, g, b, a});
        vertices.push_back(Vertex{px[2], py[2], 0, u1, v1, r, g, b, a});
        vertices.push_back(Vertex{px[3], py[3], 0, u0, v1, r, g, b, a});
        const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
        indices.insert(indices.end(), quad, quad + 6);
        batch.index_count += 6;
        stat_vertices += 4;
    }

    void push_rect(const Texture& t, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   Color c) {
        const float px[4] = {x, x + w, x + w, x};
        const float py[4] = {y, y, y + h, y + h};
        push_quad(t, px, py, u0, v0, u1, v1, c);
    }
};

// ---- construction -------------------------------------------------------------

Renderer2D::Renderer2D(SDL_GPUDevice* device, SDL_Window* window) : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    impl_->window = window;
    impl_->target_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    impl_->choose_depth_format();
    if (impl_->depth_format == SDL_GPU_TEXTUREFORMAT_INVALID)
        RW_LOG_WARN("gpu", "no depth format supported; set_depth_test() will be ignored");

    if (!impl_->create_samplers() || !impl_->create_sprite_pipeline()) return;

    const u8 white_pixel[4] = {255, 255, 255, 255};
    impl_->white = create_texture("__white", 1, 1, white_pixel);

    const std::vector<u8> atlas = DebugFont::build_atlas();
    impl_->font = create_texture("__font", DebugFont::kAtlasW, DebugFont::kAtlasH, atlas.data());
    if (impl_->font) impl_->font->nearest = true;

    impl_->ok = impl_->white && impl_->font;
}

Renderer2D::~Renderer2D() {
    Impl& im = *impl_;
    if (!im.device) return;
    SDL_WaitForGPUIdle(im.device);
    for (auto& [name, t] : im.textures)
        if (t->handle) SDL_ReleaseGPUTexture(im.device, t->handle);
    for (auto& [name, m] : im.materials) {
        if (m->pipeline) SDL_ReleaseGPUGraphicsPipeline(im.device, m->pipeline);
        if (m->pipeline_depth) SDL_ReleaseGPUGraphicsPipeline(im.device, m->pipeline_depth);
    }
    for (const Impl::DepthBuffer& d : im.depth_buffers) SDL_ReleaseGPUTexture(im.device, d.texture);
    if (im.vertex_buffer) SDL_ReleaseGPUBuffer(im.device, im.vertex_buffer);
    if (im.index_buffer) SDL_ReleaseGPUBuffer(im.device, im.index_buffer);
    if (im.transfer) SDL_ReleaseGPUTransferBuffer(im.device, im.transfer);
    if (im.pipeline) SDL_ReleaseGPUGraphicsPipeline(im.device, im.pipeline);
    if (im.pipeline_depth) SDL_ReleaseGPUGraphicsPipeline(im.device, im.pipeline_depth);
    if (im.sprite_vs) SDL_ReleaseGPUShader(im.device, im.sprite_vs);
    if (im.sampler_nearest) SDL_ReleaseGPUSampler(im.device, im.sampler_nearest);
    if (im.sampler_linear) SDL_ReleaseGPUSampler(im.device, im.sampler_linear);
}

bool Renderer2D::ok() const { return impl_->ok; }
SDL_GPUDevice* Renderer2D::device() const { return impl_->device; }
const char* Renderer2D::driver_name() const { return SDL_GetGPUDeviceDriver(impl_->device); }
int Renderer2D::draw_calls_last_frame() const { return impl_->last_draw_calls; }
int Renderer2D::vertices_last_frame() const { return impl_->last_vertices; }

// ---- frame ----------------------------------------------------------------------

bool Renderer2D::begin_frame() {
    Impl& im = *impl_;
    im.reset_frame();
    im.stat_draw_calls = 0;
    im.stat_vertices = 0;
    if (!im.ok) return false;

    im.cmd = SDL_AcquireGPUCommandBuffer(im.device);
    if (!im.cmd) {
        RW_LOG_ERROR("gpu", "SDL_AcquireGPUCommandBuffer: %s", SDL_GetError());
        return false;
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(im.cmd, im.window, &im.swapchain, &im.swapchain_w, &im.swapchain_h)) {
        RW_LOG_ERROR("gpu", "SDL_WaitAndAcquireGPUSwapchainTexture: %s", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(im.cmd);
        im.cmd = nullptr;
        return false;
    }
    if (!im.swapchain) { // minimised: nothing to draw into this frame
        SDL_SubmitGPUCommandBuffer(im.cmd);
        im.cmd = nullptr;
        return false;
    }
    im.frame_active = true;
    im.begin_segment();
    return true;
}

void Renderer2D::set_target(Texture* target) {
    Impl& im = *impl_;
    if (!im.frame_active) return;
    if (target && !target->render_target) {
        RW_LOG_WARN("gpu", "set_target: texture is not a render target");
        target = nullptr;
    }
    if (im.current_target == target && !im.segments.empty()) return;
    im.current_target = target;
    im.begin_segment();
}

void Renderer2D::set_view_projection(const Mat4* matrix) {
    Impl& im = *impl_;
    if (!im.frame_active) return;
    const bool custom = matrix != nullptr;
    if (custom == im.current_custom_vp && (!custom || std::memcmp(matrix->m, im.current_vp.m, sizeof matrix->m) == 0))
        return;
    im.current_custom_vp = custom;
    if (custom) im.current_vp = *matrix;
    im.begin_segment();
}

void Renderer2D::set_depth_test(bool enabled) {
    Impl& im = *impl_;
    if (!im.frame_active) return;
    if (enabled && im.depth_format == SDL_GPU_TEXTUREFORMAT_INVALID) enabled = false;
    if (enabled == im.current_depth) return;
    im.current_depth = enabled;
    im.begin_segment();
}

void Renderer2D::set_layer(i32 layer) { impl_->current_layer = layer; }
i32 Renderer2D::layer() const { return impl_->current_layer; }

void Renderer2D::draw_mesh(const Mesh& mesh, const Texture* texture, const Mat4* transform, bool world) {
    Impl& im = *impl_;
    if (!im.frame_active || mesh.indices.empty() || mesh.vertices.empty()) return;
    const Texture& t = texture ? *texture : *im.white;
    Batch& batch = im.batch_for(t);
    const u32 base = static_cast<u32>(im.vertices.size());
    im.vertices.reserve(im.vertices.size() + mesh.vertices.size());
    for (const MeshVertex& v : mesh.vertices) {
        Vec3 p = transform ? transform->transform_point(v.pos) : v.pos;
        if (world) {
            p.x = sx(p.x);
            p.y = sy(p.y);
        }
        im.vertices.push_back(Vertex{p.x, p.y, p.z, v.uv.x, v.uv.y, v.r, v.g, v.b, v.a});
    }
    const u32 vcount = static_cast<u32>(mesh.vertices.size());
    im.indices.reserve(im.indices.size() + mesh.indices.size());
    u32 emitted = 0;
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        if (a >= vcount || b >= vcount || c >= vcount) continue; // skip broken triangles instead of reading garbage
        im.indices.push_back(base + a);
        im.indices.push_back(base + b);
        im.indices.push_back(base + c);
        emitted += 3;
    }
    batch.index_count += emitted;
    im.stat_vertices += static_cast<int>(vcount);
}

void Renderer2D::draw_polyline(const std::vector<Vec2>& points, const StrokeStyle& style, const Texture* texture) {
    Mesh mesh;
    build_polyline(points, style, mesh);
    draw_mesh(mesh, texture, nullptr, true);
}

void Renderer2D::fill_polygon(const std::vector<Vec2>& points, Color c, const Texture* texture, float uv_scale) {
    Mesh mesh;
    build_polygon(points, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f, mesh, uv_scale);
    draw_mesh(mesh, texture, nullptr, true);
}

void Renderer2D::draw_path(const Path& path, const StrokeStyle& style, float tolerance, const Texture* texture) {
    std::vector<Vec2> points;
    path.flatten(tolerance / std::max(camera_.zoom, 1e-3f), points);
    StrokeStyle s = style;
    s.closed = path.closed;
    draw_polyline(points, s, texture);
}

void Renderer2D::fill_path(const Path& path, Color c, float tolerance, const Texture* texture, float uv_scale) {
    std::vector<Vec2> points;
    path.flatten(tolerance / std::max(camera_.zoom, 1e-3f), points);
    fill_polygon(points, c, texture, uv_scale);
}

void Renderer2D::end_frame(ImDrawData* overlay) {
    Impl& im = *impl_;
    if (!im.frame_active || !im.cmd) {
        im.reset_frame();
        return;
    }
    im.upload_geometry();
    if (overlay) ImGui_ImplSDLGPU3_PrepareDrawData(overlay, im.cmd);
    im.record_passes(overlay);
    SDL_SubmitGPUCommandBuffer(im.cmd);
    im.last_draw_calls = im.stat_draw_calls;
    im.last_vertices = im.stat_vertices;
    im.reset_frame();
}

// ---- shapes ---------------------------------------------------------------------

void Renderer2D::clear(Color c) {
    Impl& im = *impl_;
    if (!im.frame_active) return;
    Segment& seg = im.segments.back();
    if (seg.batch_count == 0) {
        seg.has_clear = true;
        seg.clear_color = c;
    } else {
        // Drawing already happened in this segment: paint over it.
        Color opaque = c;
        opaque.a = 255;
        im.push_rect(*im.white, 0, 0, static_cast<float>(width()), static_cast<float>(height()), 0, 0, 1, 1, opaque);
    }
}

void Renderer2D::fill_rect(float x, float y, float w, float h, Color c) {
    impl_->push_rect(*impl_->white, sx(x), sy(y), w * camera_.zoom, h * camera_.zoom, 0, 0, 1, 1, c);
}

void Renderer2D::draw_rect(float x, float y, float w, float h, Color c) {
    const float x0 = sx(x), y0 = sy(y);
    const float x1 = x0 + w * camera_.zoom, y1 = y0 + h * camera_.zoom;
    Impl& im = *impl_;
    im.push_rect(*im.white, x0, y0, x1 - x0, 1, 0, 0, 1, 1, c);
    im.push_rect(*im.white, x0, y1 - 1, x1 - x0, 1, 0, 0, 1, 1, c);
    im.push_rect(*im.white, x0, y0 + 1, 1, y1 - y0 - 2, 0, 0, 1, 1, c);
    im.push_rect(*im.white, x1 - 1, y0 + 1, 1, y1 - y0 - 2, 0, 0, 1, 1, c);
}

void Renderer2D::draw_line(float x1, float y1, float x2, float y2, Color c) {
    const float ax = sx(x1), ay = sy(y1), bx = sx(x2), by = sy(y2);
    float dx = bx - ax, dy = by - ay;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    dx /= len;
    dy /= len;
    const float nx = -dy * 0.5f, ny = dx * 0.5f; // one pixel thick
    const float px[4] = {ax + nx, bx + nx, bx - nx, ax - nx};
    const float py[4] = {ay + ny, by + ny, by - ny, ay - ny};
    impl_->push_quad(*impl_->white, px, py, 0, 0, 1, 1, c);
}

void Renderer2D::fill_circle(float cx, float cy, float radius, Color c, int segments) {
    Impl& im = *impl_;
    if (!im.frame_active) return;
    if (segments < 3) segments = 3;
    const float r = radius * camera_.zoom;
    const float ox = sx(cx), oy = sy(cy);
    Batch& batch = im.batch_for(*im.white);
    const u32 base = static_cast<u32>(im.vertices.size());
    const float cr = c.r / 255.0f, cg = c.g / 255.0f, cb = c.b / 255.0f, ca = c.a / 255.0f;
    im.vertices.push_back(Vertex{ox, oy, 0, 0.5f, 0.5f, cr, cg, cb, ca});
    for (int i = 0; i < segments; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(segments) * 6.28318530718f;
        im.vertices.push_back(Vertex{ox + std::cos(angle) * r, oy + std::sin(angle) * r, 0, 0.5f, 0.5f, cr, cg, cb, ca});
    }
    for (int i = 0; i < segments; ++i) {
        im.indices.push_back(base);
        im.indices.push_back(base + 1 + static_cast<u32>(i));
        im.indices.push_back(base + 1 + static_cast<u32>((i + 1) % segments));
    }
    batch.index_count += static_cast<u32>(segments) * 3;
    im.stat_vertices += segments + 1;
}

void Renderer2D::draw_text(float x, float y, const char* text, Color c, int scale) {
    Impl& im = *impl_;
    if (!im.frame_active || !text) return;
    if (scale < 1) scale = 1;
    const float cw = static_cast<float>(DebugFont::kCellW * scale);
    const float ch = static_cast<float>(DebugFont::kCellH * scale);
    const float u_cell = static_cast<float>(DebugFont::kCellW) / DebugFont::kAtlasW;
    const float v_cell = static_cast<float>(DebugFont::kCellH) / DebugFont::kAtlasH;
    float pen_x = x;
    float pen_y = y;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            pen_x = x;
            pen_y += ch;
            continue;
        }
        const int cell = DebugFont::cell_for(*p);
        if (cell != 0) {
            const float u0 = static_cast<float>(cell % DebugFont::kColumns) * u_cell;
            const float v0 = static_cast<float>(cell / DebugFont::kColumns) * v_cell;
            im.push_rect(*im.font, pen_x, pen_y, cw, ch, u0, v0, u0 + u_cell, v0 + v_cell, c);
        }
        pen_x += cw;
    }
}


// ---- fonts ----------------------------------------------------------------------

Font* Renderer2D::load_font(const std::string& path, float pixel_size) {
    Impl& im = *impl_;
    const std::string key = path + "@" + std::to_string(static_cast<int>(pixel_size * 100));
    auto it = im.fonts.find(key);
    Font* font = it == im.fonts.end() ? nullptr : it->second.get();
    const std::string file = resolve_asset_path(path);
    const bool stale = im.stale_files.erase(path) > 0 || im.stale_files.erase(file) > 0;
    if (font && !stale) return font;

    if (!font) {
        auto owned = std::make_unique<Font>();
        owned->path = path;
        font = owned.get();
        im.fonts[key] = std::move(owned);
    }

    std::vector<u8> ttf;
    std::string error;
    if (!read_file(file, ttf)) {
        RW_LOG_ERROR("gpu", "load_font(%s): could not read the file", path.c_str());
        return font;
    }
    FontAtlas atlas;
    if (!bake_font(ttf.data(), ttf.size(), pixel_size, atlas, &error)) {
        RW_LOG_ERROR("gpu", "load_font(%s): %s", path.c_str(), error.c_str());
        return font;
    }

    // Coverage to white RGBA so the sprite pipeline tints it with the vertex colour.
    std::vector<u8> rgba(static_cast<usize>(atlas.width) * atlas.height * 4);
    for (usize i = 0; i < atlas.alpha.size(); ++i) {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = atlas.alpha[i];
    }
    Texture* texture = create_texture("__font:" + key, atlas.width, atlas.height, rgba.data());
    if (!texture) return font;
    // Small sizes read as pixel fonts and want hard edges; larger ones look better smoothed.
    texture->nearest = pixel_art_ && pixel_size <= 24;

    font->atlas = std::move(atlas);
    font->texture = texture;
    font->ok = true;
    track_file(file);
    if (file != path) im.aliases[file] = path;
    RW_LOG_INFO("gpu", "font %s at %.0f px: %zu glyphs, %dx%d atlas", path.c_str(), pixel_size,
                font->atlas.glyphs.size(), font->atlas.width, font->atlas.height);
    return font;
}

Vec2 Renderer2D::measure_text(const Font& font, std::string_view text, float scale) const {
    return font.atlas.measure(text, scale);
}

void Renderer2D::draw_text(const Font& font, float x, float y, std::string_view text, Color c, float scale,
                           bool world) {
    Impl& im = *impl_;
    if (!im.frame_active || !font.ok || !font.texture) return;
    const FontAtlas& a = font.atlas;
    const float zoom = world ? camera_.zoom : 1.0f;
    const float s = scale * zoom;
    const float origin_x = world ? sx(x) : x;
    const float origin_y = world ? sy(y) : y;
    const float tw = static_cast<float>(font.texture->width);
    const float th = static_cast<float>(font.texture->height);

    float pen_x = origin_x;
    float baseline = origin_y + a.ascent * s;
    u32 prev = 0;
    for (usize i = 0; i < text.size();) {
        const u32 cp = utf8_next(text, i);
        if (cp == '\n') {
            pen_x = origin_x;
            baseline += a.line_height * s;
            prev = 0;
            continue;
        }
        const Glyph* g = a.find(cp);
        if (!g) continue;
        pen_x += a.kern(prev, cp) * s;
        if (g->src.w > 0 && g->src.h > 0) {
            const float gx = std::floor(pen_x + g->x_off * s);
            const float gy = std::floor(baseline + g->y_off * s);
            im.push_rect(*font.texture, gx, gy, g->src.w * s, g->src.h * s, g->src.x / tw, g->src.y / th,
                         (g->src.x + g->src.w) / tw, (g->src.y + g->src.h) / th, c);
        }
        pen_x += g->advance * s;
        prev = cp;
    }
}

// ---- textures -------------------------------------------------------------------

Texture* Renderer2D::find_texture(const std::string& name) const {
    auto it = impl_->textures.find(name);
    return it == impl_->textures.end() ? nullptr : it->second.get();
}

Texture* Renderer2D::create_texture(const std::string& name, int width, int height, const u8* rgba) {
    if (width <= 0 || height <= 0) {
        RW_LOG_ERROR("gpu", "create_texture(%s): invalid size %dx%d", name.c_str(), width, height);
        return nullptr;
    }
    Texture* t = impl_->make_texture(name, width, height, false, pixel_art_);
    if (t && rgba && !impl_->upload_pixels(t->handle, width, height, rgba)) {
        RW_LOG_ERROR("gpu", "create_texture(%s): upload failed: %s", name.c_str(), SDL_GetError());
    }
    return t;
}

Texture* Renderer2D::create_render_target(const std::string& name, int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    return impl_->make_texture(name, width, height, true, pixel_art_);
}

Texture* Renderer2D::load_texture(const AssetRef& ref) {
    const std::string file = resolve_asset(ref);
    if (file.empty()) {
        RW_LOG_WARN("gpu", "load_texture: asset reference '%s' (%s) resolves to nothing", ref.path.c_str(), ref.id.c_str());
        return nullptr;
    }
    return load_texture(file);
}

Font* Renderer2D::load_font(const AssetRef& ref, float pixel_size) {
    const std::string file = resolve_asset(ref);
    if (file.empty()) {
        RW_LOG_WARN("gpu", "load_font: asset reference '%s' (%s) resolves to nothing", ref.path.c_str(), ref.id.c_str());
        return nullptr;
    }
    return load_font(file, pixel_size);
}

Material* Renderer2D::load_material(const AssetRef& ref) {
    const std::string file = resolve_asset(ref);
    if (file.empty()) {
        RW_LOG_WARN("gpu", "load_material: asset reference '%s' (%s) resolves to nothing", ref.path.c_str(), ref.id.c_str());
        return nullptr;
    }
    return load_material(file);
}

Texture* Renderer2D::load_texture(const std::string& path) {
    Impl& im = *impl_;
    Texture* existing = find_texture(path);
    const bool stale = im.stale_files.erase(path) > 0;
    if (existing && !stale) return existing;

    const std::string file = resolve_asset_path(path);
    Texture* texture = nullptr;

    // Cooked textures are recognised by content, so games load by source name
    // and get whichever the project provides.
    std::vector<u8> bytes;
    CookedTexture cooked;
    if (read_file(file, bytes) && cooked_magic(bytes) == kCookedTextureMagic) {
        if (!decode_cooked_texture(bytes, cooked)) {
            RW_LOG_ERROR("gpu", "load_texture(%s): damaged cooked texture", path.c_str());
            return existing;
        }
        texture = create_texture(path, cooked.width, cooked.height, cooked.rgba.data());
        if (texture) texture->nearest = cooked.nearest;
    } else {
        int width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
        if (!pixels) {
            RW_LOG_ERROR("gpu", "load_texture(%s): %s", path.c_str(), stbi_failure_reason());
            return existing; // keep showing the last good version
        }
        texture = create_texture(path, width, height, pixels);
        stbi_image_free(pixels);
    }
    track_file(file);
    if (file != path) im.aliases[file] = path;
    return texture;
}

void Renderer2D::set_file_watcher(FileWatcher* watcher) { impl_->watcher = watcher; }

void Renderer2D::track_file(const std::string& path) {
    if (impl_->watcher) impl_->watcher->track(path);
}

void Renderer2D::invalidate_file(const std::string& path) {
    impl_->stale_files.insert(path);
    // The watcher reports resolved paths; games ask by the name they used.
    auto alias = impl_->aliases.find(path);
    if (alias != impl_->aliases.end()) impl_->stale_files.insert(alias->second);
}

void Renderer2D::draw_sprite(const Texture& texture, const Rect& src, float x, float y, const SpriteOptions& o) {
    if (!texture.handle || texture.width <= 0 || texture.height <= 0) return;

    const float zx = camera_.zoom * o.scale_x;
    const float zy = camera_.zoom * o.scale_y;
    const float w = src.w * zx;
    const float h = src.h * zy;
    const float ox = (o.flip_x ? src.w - o.origin.x : o.origin.x) * zx;
    const float oy = (o.flip_y ? src.h - o.origin.y : o.origin.y) * zy;
    const float px0 = sx(x), py0 = sy(y);

    // Corners relative to the pivot, then rotated and translated.
    const float cx[4] = {-ox, w - ox, w - ox, -ox};
    const float cy[4] = {-oy, -oy, h - oy, h - oy};
    float px[4], py[4];
    if (o.rotation_deg != 0) {
        const float rad = o.rotation_deg * 0.01745329252f;
        const float cs = std::cos(rad), sn = std::sin(rad);
        for (int i = 0; i < 4; ++i) {
            px[i] = px0 + cx[i] * cs - cy[i] * sn;
            py[i] = py0 + cx[i] * sn + cy[i] * cs;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            px[i] = px0 + cx[i];
            py[i] = py0 + cy[i];
        }
    }

    float u0 = src.x / static_cast<float>(texture.width);
    float v0 = src.y / static_cast<float>(texture.height);
    float u1 = (src.x + src.w) / static_cast<float>(texture.width);
    float v1 = (src.y + src.h) / static_cast<float>(texture.height);
    if (o.flip_x) std::swap(u0, u1);
    if (o.flip_y) std::swap(v0, v1);

    impl_->push_quad(texture, px, py, u0, v0, u1, v1, o.tint);
}

void Renderer2D::draw_texture(const Texture& texture, float x, float y, const SpriteOptions& o) {
    draw_sprite(texture, Rect{0, 0, static_cast<float>(texture.width), static_cast<float>(texture.height)}, x, y, o);
}

// ---- materials ------------------------------------------------------------------

Material* Renderer2D::find_material(const std::string& path) const {
    auto it = impl_->materials.find(path);
    return it == impl_->materials.end() ? nullptr : it->second.get();
}

Material* Renderer2D::load_material(const std::string& path) {
    Impl& im = *impl_;
    Material* m = find_material(path);
    const bool spv_stale = im.stale_files.erase(path) > 0;
    const bool source_stale = m && !m->source_path.empty() && im.stale_files.erase(m->source_path) > 0;
    if (m && !spv_stale && !source_stale) return m;

    if (!m) {
        auto owned = std::make_unique<Material>();
        owned->path = path;
        m = owned.get();
        im.materials[path] = std::move(owned);
    }
    // The .spv may only exist next to the executable; the sidecar names the source.
    std::string file = resolve_asset_path(path);
    if (m->source_path.empty()) m->source_path = read_text(file + ".src");
    {
        // A loose GLSL file (project shaders/ before a cook): compile it next to itself.
        std::vector<u8> head;
        if (read_file(file, head) && !head.empty() && !is_spirv(head.data(), head.size())) {
            m->source_path = file;
            const std::string spv = file + ".spv";
            std::string log;
            if (!compile_glsl_file(file, spv, log)) {
                m->error = log.empty() ? "glslc failed" : log;
                RW_LOG_ERROR("gpu", "material %s: %s", path.c_str(), m->error.c_str());
                return m;
            }
            file = spv;
        }
    }
    if (file != path) {
        im.aliases[file] = path;
        // The .spv might not exist yet (glslc missing): still watch the path we resolved to.
    }
    track_file(file);
    if (!m->source_path.empty()) track_file(m->source_path);

    if (source_stale) {
        // The GLSL changed: rebuild the SPIR-V with the same compiler the build uses.
        std::string log;
        if (!compile_glsl_file(m->source_path, file, log)) {
            m->error = log.empty() ? "glslc failed" : log;
            RW_LOG_ERROR("gpu", "material %s: %s", std::filesystem::path(m->source_path).filename().string().c_str(),
                         m->error.c_str());
            return m; // keep the previous pipeline
        }
        im.stale_files.erase(path); // the watcher will report the fresh .spv too; already handled
        im.stale_files.erase(file);
    }

    if (im.build_material(*m, file)) {
        RW_LOG_INFO("gpu", "material %s ready (%u samplers%s)", std::filesystem::path(path).filename().string().c_str(),
                    m->sampler_count, m->uniform_buffers ? ", params" : "");
    } else {
        RW_LOG_ERROR("gpu", "material %s: %s", path.c_str(), m->error.c_str());
    }
    return m;
}

void Renderer2D::set_material(Material* material) { impl_->current_material = material; }

void Renderer2D::set_material_params(const void* data, usize size) {
    Impl& im = *impl_;
    if (!data || size == 0) {
        im.current_params_size = 0;
        return;
    }
    if (size > kMaxParamsBytes) size = kMaxParamsBytes;
    std::memcpy(im.current_params, data, size);
    im.current_params_size = static_cast<u32>(size);
}

void Renderer2D::set_material_texture(u32 slot, Texture* texture) {
    if (slot < 1 || slot > kMaxExtraTextures) return;
    impl_->extra[slot - 1] = texture;
}

// ---- camera and coordinates -----------------------------------------------------

int Renderer2D::width() const {
    const Impl& im = *impl_;
    if (im.current_target) return im.current_target->width;
    return static_cast<int>(im.swapchain_w);
}

int Renderer2D::height() const {
    const Impl& im = *impl_;
    if (im.current_target) return im.current_target->height;
    return static_cast<int>(im.swapchain_h);
}

int Renderer2D::window_width() const { return static_cast<int>(impl_->swapchain_w); }
int Renderer2D::window_height() const { return static_cast<int>(impl_->swapchain_h); }

Vec2 Renderer2D::window_to_render(float wx, float wy) const {
    if (present_scale_ <= 0) return {wx, wy};
    return {(wx - present_.x) / present_scale_, (wy - present_.y) / present_scale_};
}

Vec2 Renderer2D::render_to_world(Vec2 p) const {
    const float zoom = camera_.zoom > 0 ? camera_.zoom : 1;
    return {p.x / zoom + camera_.x, p.y / zoom + camera_.y};
}

} // namespace rw
