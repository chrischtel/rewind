// Headless checks for shader reflection and SPIR-V to HLSL translation. Uses
// the engine's own sprite shaders, embedded at build time, so it runs only
// when glslc was available.

#include <rewind/render/shader_cross.h>

#include <cstdio>
#include <string>

#include "rewind_shaders_spv.h"

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

} // namespace

int main() {
#if REWIND_HAVE_SPIRV
    ShaderInfo info;
    std::string error;

    CHECK(reflect_spirv(sprite_frag_spv, sprite_frag_spv_size, info, error));
    CHECK(!info.vertex);
    CHECK(info.samplers == 1);
    CHECK(info.uniform_buffers == 0);

    CHECK(reflect_spirv(sprite_vert_spv, sprite_vert_spv_size, info, error));
    CHECK(info.vertex);
    CHECK(info.samplers == 0);
    CHECK(info.uniform_buffers == 1);

    std::string hlsl;
    CHECK(spirv_to_hlsl(sprite_frag_spv, sprite_frag_spv_size, hlsl, error));
    if (!error.empty()) std::printf("hlsl error: %s\n", error.c_str());
    // Fragment textures and samplers must land in space2 with matching registers.
    CHECK(hlsl.find("register(t0, space2)") != std::string::npos);
    CHECK(hlsl.find("register(s0, space2)") != std::string::npos);
    CHECK(hlsl.find("main(") != std::string::npos);

    CHECK(spirv_to_hlsl(sprite_vert_spv, sprite_vert_spv_size, hlsl, error));
    CHECK(hlsl.find("register(b0, space1)") != std::string::npos);
    CHECK(hlsl.find("TEXCOORD0") != std::string::npos);
    CHECK(hlsl.find("SV_Position") != std::string::npos);

    // Garbage is rejected with a message, never a crash.
    const unsigned char junk[32] = {1, 2, 3, 4};
    CHECK(!reflect_spirv(junk, sizeof junk, info, error));
    CHECK(!error.empty());
    CHECK(!spirv_to_hlsl(junk, 7, hlsl, error));
#else
    std::printf("shader_test: skipped (no SPIR-V embedded)\n");
#endif

    if (failures == 0) {
        std::printf("shader_test: all checks passed\n");
        return 0;
    }
    std::printf("shader_test: %d check(s) failed\n", failures);
    return 1;
}
