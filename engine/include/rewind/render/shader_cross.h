#pragma once

#include "rewind/core/types.h"

#include <string>

namespace rw {

/// What a SPIR-V module binds, read back from the module itself so shader
/// authors never have to declare counts by hand.
struct ShaderInfo {
    u32 samplers = 0;        ///< combined image samplers (set 0 vertex / set 2 fragment)
    u32 uniform_buffers = 0; ///< set 1 vertex / set 3 fragment
    u32 storage_buffers = 0;
    u32 storage_textures = 0;
    bool vertex = false;     ///< execution model
};

/// Reflect a SPIR-V module. Returns false with `error` set on malformed input.
bool reflect_spirv(const u8* data, usize size, ShaderInfo& out, std::string& error);

/// Translate SPIR-V (written against the SDL GPU set conventions) into HLSL
/// shader model 5.1 with the register spaces Direct3D 12 expects. The entry
/// point in the output is always `main`.
bool spirv_to_hlsl(const u8* data, usize size, std::string& hlsl, std::string& error);

/// Compile a GLSL file to SPIR-V with glslc (the Vulkan SDK's compiler the
/// build found). False with the compiler's output in `log`, or when the
/// engine was built without glslc. `glslc_available()` says which.
bool compile_glsl_file(const std::string& source_file, const std::string& output_file, std::string& log);
bool glslc_available();

/// Whether a buffer starts with the SPIR-V magic number.
bool is_spirv(const u8* data, usize size);

} // namespace rw
