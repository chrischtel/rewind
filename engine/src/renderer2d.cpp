#include "rewind/render/renderer2d.h"
#include "rewind/core/log.h"

#include "debug_font.h"
#include "rewind/core/file_watcher.h"
#include "rewind_shaders_spv.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>
#include <stb_image.h>

#include <cmath>
#include <cstring>
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

const char kSpriteHlsl[] = R"HLSL(
cbuffer Uniforms : register(b0, space1) { float2 scale; float2 offset; };
struct VSIn { float2 pos : TEXCOORD0; float2 uv : TEXCOORD1; float4 color : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : TEXCOORD1; };
VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos * scale + offset, 0.0, 1.0);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}
Texture2D tex : register(t0, space2);
SamplerState smp : register(s0, space2);
float4 ps_main(VSOut i) : SV_Target { return tex.Sample(smp, i.uv) * i.color; }
)HLSL";

struct Vertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct Uniforms {
    float scale_x, scale_y;
    float offset_x, offset_y;
};

struct Batch {
    SDL_GPUTexture* texture;
    SDL_GPUSampler* sampler;
    u32 first_index;
    u32 index_count;
};

struct Segment {
    Texture* target; ///< nullptr = window
    bool has_clear = false;
    Color clear_color{};
    usize first_batch = 0;
    usize batch_count = 0;
};

#if defined(_WIN32)
bool compile_dxbc(const char* source, const char* entry, const char* target, std::vector<u8>& out) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "sprite.hlsl", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        RW_LOG_ERROR("gpu", "shader %s failed to compile: %s", entry,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
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
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUSampler* sampler_nearest = nullptr;
    SDL_GPUSampler* sampler_linear = nullptr;

    std::unordered_map<std::string, std::unique_ptr<Texture>> textures;
    std::unordered_set<std::string> stale_files; ///< loaded from disk, changed since
    FileWatcher* watcher = nullptr;
    Texture* white = nullptr;
    Texture* font = nullptr;

    // Per-frame recording.
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::vector<Batch> batches;
    std::vector<Segment> segments;
    SDL_GPUCommandBuffer* cmd = nullptr;
    SDL_GPUTexture* swapchain = nullptr;
    u32 swapchain_w = 0, swapchain_h = 0;
    bool frame_active = false;
    Texture* current_target = nullptr;
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

    // ---- setup ------------------------------------------------------------

    SDL_GPUShader* create_shader(SDL_GPUShaderStage stage) {
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
        SDL_GPUShaderCreateInfo info{};
        info.stage = stage;
        info.num_samplers = stage == SDL_GPU_SHADERSTAGE_FRAGMENT ? 1 : 0;
        info.num_uniform_buffers = stage == SDL_GPU_SHADERSTAGE_VERTEX ? 1 : 0;

#if REWIND_HAVE_SPIRV
        if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.entrypoint = "main";
            if (stage == SDL_GPU_SHADERSTAGE_VERTEX) {
                info.code = sprite_vert_spv;
                info.code_size = sprite_vert_spv_size;
            } else {
                info.code = sprite_frag_spv;
                info.code_size = sprite_frag_spv_size;
            }
            return SDL_CreateGPUShader(device, &info);
        }
#endif
#if defined(_WIN32)
        if (formats & SDL_GPU_SHADERFORMAT_DXBC) {
            std::vector<u8> code;
            const bool vs = stage == SDL_GPU_SHADERSTAGE_VERTEX;
            if (!compile_dxbc(kSpriteHlsl, vs ? "vs_main" : "ps_main", vs ? "vs_5_1" : "ps_5_1", code)) return nullptr;
            info.format = SDL_GPU_SHADERFORMAT_DXBC;
            info.entrypoint = vs ? "vs_main" : "ps_main";
            info.code = code.data();
            info.code_size = code.size();
            return SDL_CreateGPUShader(device, &info);
        }
#endif
        RW_LOG_ERROR("gpu", "no shader format available for this GPU driver (formats 0x%x)", formats);
        return nullptr;
    }

    bool create_pipeline() {
        SDL_GPUShader* vs = create_shader(SDL_GPU_SHADERSTAGE_VERTEX);
        SDL_GPUShader* fs = create_shader(SDL_GPU_SHADERSTAGE_FRAGMENT);
        if (!vs || !fs) {
            RW_LOG_ERROR("gpu", "could not create the sprite shaders: %s", SDL_GetError());
            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
            return false;
        }

        SDL_GPUVertexBufferDescription vb{};
        vb.slot = 0;
        vb.pitch = sizeof(Vertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[3]{};
        attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0};
        attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 2};
        attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4};

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

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
        SDL_ReleaseGPUShader(device, vs);
        SDL_ReleaseGPUShader(device, fs);
        if (!pipeline) {
            RW_LOG_ERROR("gpu", "could not create the sprite pipeline: %s", SDL_GetError());
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

    void begin_segment(Texture* target) {
        Segment s;
        s.target = target;
        s.first_batch = batches.size();
        segments.push_back(s);
        current_target = target;
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

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            if (seg.batch_count > 0) {
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                SDL_GPUBufferBinding vbind{vertex_buffer, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
                SDL_GPUBufferBinding ibind{index_buffer, 0};
                SDL_BindGPUIndexBuffer(pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                const Uniforms u{2.0f / static_cast<float>(w), -2.0f / static_cast<float>(h), -1.0f, 1.0f};
                SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);

                for (usize b = seg.first_batch; b < seg.first_batch + seg.batch_count; ++b) {
                    const Batch& batch = batches[b];
                    SDL_GPUTextureSamplerBinding tbind{batch.texture, batch.sampler};
                    SDL_BindGPUFragmentSamplers(pass, 0, &tbind, 1);
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
        cmd = nullptr;
        swapchain = nullptr;
        frame_active = false;
        current_target = nullptr;
    }

    // ---- geometry helpers -------------------------------------------------

    Batch& batch_for(const Texture& t) {
        SDL_GPUSampler* sampler = sampler_for(t);
        Segment& seg = segments.back();
        if (seg.batch_count > 0) {
            Batch& last = batches.back();
            if (last.texture == t.handle && last.sampler == sampler) return last;
        }
        batches.push_back(Batch{t.handle, sampler, static_cast<u32>(indices.size()), 0});
        ++seg.batch_count;
        return batches.back();
    }

    void push_quad(const Texture& t, const float px[4], const float py[4], float u0, float v0, float u1, float v1,
                   Color c) {
        if (!frame_active) return;
        Batch& batch = batch_for(t);
        const u32 base = static_cast<u32>(vertices.size());
        const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f, a = c.a / 255.0f;
        vertices.push_back(Vertex{px[0], py[0], u0, v0, r, g, b, a});
        vertices.push_back(Vertex{px[1], py[1], u1, v0, r, g, b, a});
        vertices.push_back(Vertex{px[2], py[2], u1, v1, r, g, b, a});
        vertices.push_back(Vertex{px[3], py[3], u0, v1, r, g, b, a});
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

    if (!impl_->create_samplers() || !impl_->create_pipeline()) return;

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
    if (im.vertex_buffer) SDL_ReleaseGPUBuffer(im.device, im.vertex_buffer);
    if (im.index_buffer) SDL_ReleaseGPUBuffer(im.device, im.index_buffer);
    if (im.transfer) SDL_ReleaseGPUTransferBuffer(im.device, im.transfer);
    if (im.pipeline) SDL_ReleaseGPUGraphicsPipeline(im.device, im.pipeline);
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
        RW_LOG_INFO("gpu", "SDL_AcquireGPUCommandBuffer: %s", SDL_GetError());
        return false;
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(im.cmd, im.window, &im.swapchain, &im.swapchain_w, &im.swapchain_h)) {
        RW_LOG_INFO("gpu", "SDL_WaitAndAcquireGPUSwapchainTexture: %s", SDL_GetError());
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
    im.begin_segment(nullptr);
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
    im.begin_segment(target);
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
    im.vertices.push_back(Vertex{ox, oy, 0.5f, 0.5f, cr, cg, cb, ca});
    for (int i = 0; i < segments; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(segments) * 6.28318530718f;
        im.vertices.push_back(Vertex{ox + std::cos(angle) * r, oy + std::sin(angle) * r, 0.5f, 0.5f, cr, cg, cb, ca});
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

Texture* Renderer2D::load_texture(const std::string& path) {
    Impl& im = *impl_;
    Texture* existing = find_texture(path);
    const bool stale = im.stale_files.erase(path) > 0;
    if (existing && !stale) return existing;

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        RW_LOG_INFO("gpu", "load_texture(%s): %s", path.c_str(), stbi_failure_reason());
        return existing; // keep showing the last good version
    }
    Texture* texture = create_texture(path, width, height, pixels);
    stbi_image_free(pixels);
    track_file(path);
    return texture;
}

void Renderer2D::set_file_watcher(FileWatcher* watcher) { impl_->watcher = watcher; }

void Renderer2D::track_file(const std::string& path) {
    if (impl_->watcher) impl_->watcher->track(path);
}

void Renderer2D::invalidate_file(const std::string& path) { impl_->stale_files.insert(path); }

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
