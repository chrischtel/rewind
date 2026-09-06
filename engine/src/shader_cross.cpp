#include "rewind/render/shader_cross.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4245 4267 4324 4389 4456 4457 4458 4702 4996)
#endif
#include <spirv_hlsl.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdio>
#include <cstring>
#include <vector>

namespace rw {

namespace {

bool to_words(const u8* data, usize size, std::vector<uint32_t>& words, std::string& error) {
    if (!data || size < 20 || size % 4 != 0) {
        error = "not a SPIR-V module (size " + std::to_string(size) + ")";
        return false;
    }
    words.resize(size / 4);
    std::memcpy(words.data(), data, size);
    if (words[0] != 0x07230203u) {
        error = "not a SPIR-V module (bad magic)";
        return false;
    }
    return true;
}

} // namespace

bool reflect_spirv(const u8* data, usize size, ShaderInfo& out, std::string& error) {
    std::vector<uint32_t> words;
    if (!to_words(data, size, words, error)) return false;
    try {
        spirv_cross::Compiler compiler(std::move(words));
        const spirv_cross::ShaderResources res = compiler.get_shader_resources();
        out = ShaderInfo{};
        out.samplers = static_cast<u32>(res.sampled_images.size());
        out.uniform_buffers = static_cast<u32>(res.uniform_buffers.size());
        out.storage_buffers = static_cast<u32>(res.storage_buffers.size());
        out.storage_textures = static_cast<u32>(res.storage_images.size());
        out.vertex = compiler.get_execution_model() == spv::ExecutionModelVertex;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool spirv_to_hlsl(const u8* data, usize size, std::string& hlsl, std::string& error) {
    std::vector<uint32_t> words;
    if (!to_words(data, size, words, error)) return false;
    try {
        spirv_cross::CompilerHLSL compiler(std::move(words));
        const bool vertex = compiler.get_execution_model() == spv::ExecutionModelVertex;

        spirv_cross::CompilerHLSL::Options options;
        options.shader_model = 51; // register spaces need 5.1
        compiler.set_hlsl_options(options);

        // SDL GPU conventions. SPIR-V sets -> HLSL register spaces:
        //   vertex:   set 0 textures/samplers -> space0, set 1 uniforms -> space1
        //   fragment: set 2 textures/samplers -> space2, set 3 uniforms -> space3
        const spirv_cross::ShaderResources res = compiler.get_shader_resources();
        const u32 sampler_set = vertex ? 0 : 2;
        const u32 uniform_set = vertex ? 1 : 3;
        const spv::ExecutionModel stage = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;

        for (const auto& image : res.sampled_images) {
            const u32 set = compiler.get_decoration(image.id, spv::DecorationDescriptorSet);
            const u32 binding = compiler.get_decoration(image.id, spv::DecorationBinding);
            spirv_cross::HLSLResourceBinding b{};
            b.stage = stage;
            b.desc_set = set;
            b.binding = binding;
            b.srv.register_space = sampler_set;
            b.srv.register_binding = binding;
            b.sampler.register_space = sampler_set;
            b.sampler.register_binding = binding;
            compiler.add_hlsl_resource_binding(b);
        }
        for (const auto& ubo : res.uniform_buffers) {
            const u32 set = compiler.get_decoration(ubo.id, spv::DecorationDescriptorSet);
            const u32 binding = compiler.get_decoration(ubo.id, spv::DecorationBinding);
            spirv_cross::HLSLResourceBinding b{};
            b.stage = stage;
            b.desc_set = set;
            b.binding = binding;
            b.cbv.register_space = uniform_set;
            b.cbv.register_binding = binding;
            compiler.add_hlsl_resource_binding(b);
        }

        hlsl = compiler.compile();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool compile_glsl_file(const std::string& source, const std::string& output, std::string& log) {
#if defined(REWIND_GLSLC_PATH)
    std::string command = std::string("\"") + REWIND_GLSLC_PATH + "\" -O \"" + source + "\" -o \"" + output + "\" 2>&1";
#if defined(_WIN32)
    command = "\"" + command + "\""; // cmd.exe quoting: wrap the whole line once more
    std::FILE* pipe = _popen(command.c_str(), "r");
#else
    std::FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        log = "could not start glslc";
        return false;
    }
    log.clear();
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe)) log += buffer;
#if defined(_WIN32)
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    return status == 0;
#else
    (void)source;
    (void)output;
    log = "glslc was not available when the engine was built";
    return false;
#endif
}

bool glslc_available() {
#if defined(REWIND_GLSLC_PATH)
    return true;
#else
    return false;
#endif
}

bool is_spirv(const u8* data, usize size) {
    if (!data || size < 4) return false;
    u32 magic = 0;
    std::memcpy(&magic, data, 4);
    return magic == 0x07230203u;
}

} // namespace rw
