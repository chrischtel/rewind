#pragma once

struct SDL_GPUTexture;

namespace rw {

/// A GPU texture owned by the Renderer2D that created it. Games hold
/// pointers to these; they stay valid until the renderer shuts down, which
/// includes across hot reloads.
struct Texture {
    int width = 0;
    int height = 0;
    SDL_GPUTexture* handle = nullptr;
    bool nearest = true;        ///< Sampling: nearest-neighbour or linear.
    bool render_target = false; ///< Can be drawn into with Renderer2D::set_target().
};

} // namespace rw
