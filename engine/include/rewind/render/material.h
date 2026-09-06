#pragma once

#include "rewind/core/types.h"

#include <string>

struct SDL_GPUGraphicsPipeline;

namespace rw {

/// A custom fragment shader for sprites and shapes, owned by the Renderer2D
/// that created it. Written in GLSL against the SDL GPU conventions:
///
///     layout(set = 2, binding = 0) uniform sampler2D tex;      // the sprite
///     layout(set = 2, binding = 1) uniform sampler2D extra;    // optional, see set_material_texture()
///     layout(set = 3, binding = 0) uniform Params { float t; }; // optional, see set_material_params()
///     layout(location = 0) in vec2 v_uv;
///     layout(location = 1) in vec4 v_color;
///     layout(location = 0) out vec4 out_color;
///
/// Compiled to SPIR-V at build time (rewind_add_game(... SHADERS ...)); on
/// Direct3D 12 the engine cross-compiles that SPIR-V at load time. When the
/// GLSL source changes and glslc is available, the material recompiles live.
struct Material {
    std::string path;        ///< the .spv file
    std::string source_path; ///< the GLSL source, if known (for live recompiles)
    bool ok = false;
    std::string error;       ///< last compile or load error, empty when ok
    u32 sampler_count = 1;   ///< sampled textures the shader declares
    u32 uniform_buffers = 0; ///< 0 or 1: whether Params exists
    u32 generation = 0;      ///< bumps on every successful (re)load
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_depth = nullptr; ///< same shader, depth test on (see Renderer2D::set_depth_test)
};

} // namespace rw
