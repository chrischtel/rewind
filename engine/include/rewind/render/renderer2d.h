#pragma once

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/texture.h"

#include <memory>
#include <string>

struct SDL_GPUDevice;
struct SDL_Window;
struct ImDrawData;

namespace rw {

struct Color {
    u8 r = 255, g = 255, b = 255, a = 255;

    void reflect(Visitor& v) {
        v.field("r", r, {.min = 0, .max = 255});
        v.field("g", g, {.min = 0, .max = 255});
        v.field("b", b, {.min = 0, .max = 255});
        v.field("a", a, {.min = 0, .max = 255});
    }
};

constexpr Color rgb(u8 r, u8 g, u8 b) { return Color{r, g, b, 255}; }
constexpr Color rgba(u8 r, u8 g, u8 b, u8 a) { return Color{r, g, b, a}; }

/// World-to-screen transform: screen = (world - position) * zoom.
/// With the default camera, world (0,0) is the top-left pixel.
struct Camera {
    float x = 0;
    float y = 0;
    float zoom = 1;
};

struct SpriteOptions {
    float scale_x = 1;
    float scale_y = 1;
    bool flip_x = false;
    bool flip_y = false;
    float rotation_deg = 0;
    Vec2 origin{}; ///< Point inside the source rect that lands on (x, y) and is the rotation pivot.
    Color tint{};
};

/// Immediate-mode 2D drawing on the GPU: shapes, text, textures and sprites
/// through one batched pipeline, one camera. Owns every texture it created.
///
/// Runs on SDL's GPU API, which means Vulkan or Direct3D 12 underneath with
/// our own shaders (see engine/shaders). Everything drawn between
/// begin_frame() and end_frame() is collected into batches and submitted in
/// as few draw calls as texture changes allow.
class Renderer2D {
public:
    Renderer2D(SDL_GPUDevice* device, SDL_Window* window);
    ~Renderer2D();

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    /// False if shaders or pipelines could not be created (see the log).
    bool ok() const;

    // ---- frame --------------------------------------------------------------
    /// Acquire the swapchain image. Returns false when there is nothing to
    /// draw into (minimised window); drawing calls then become no-ops.
    bool begin_frame();

    /// Direct subsequent drawing into `target` (a render target texture) or,
    /// with nullptr, into the window.
    void set_target(Texture* target);

    /// Upload, record every render pass, draw the overlay last, submit.
    void end_frame(ImDrawData* overlay);

    // ---- shapes -------------------------------------------------------------
    void clear(Color c);
    void fill_rect(float x, float y, float w, float h, Color c);
    void fill_rect(const Rect& r, Color c) { fill_rect(r.x, r.y, r.w, r.h, c); }
    void draw_rect(float x, float y, float w, float h, Color c);
    void draw_rect(const Rect& r, Color c) { draw_rect(r.x, r.y, r.w, r.h, c); }
    void draw_line(float x1, float y1, float x2, float y2, Color c);
    void fill_circle(float cx, float cy, float radius, Color c, int segments = 24);

    /// Built-in 5x7 pixel font, 6 px advance, 8 px line height, times `scale`.
    /// Screen space, not affected by the camera.
    void draw_text(float x, float y, const char* text, Color c, int scale = 1);

    // ---- textures -----------------------------------------------------------
    /// Load a PNG. Cached by path: calling again returns the same texture.
    Texture* load_texture(const std::string& path);

    /// Create or update a texture from RGBA8 pixels (row-major, 4 bytes per
    /// pixel). Cached by name; an existing texture of the same size is
    /// updated in place, so pointers held by the game stay valid.
    Texture* create_texture(const std::string& name, int width, int height, const u8* rgba);

    /// Create a texture that can be drawn into with set_target() and then
    /// drawn like any other. Cached by name.
    Texture* create_render_target(const std::string& name, int width, int height);

    Texture* find_texture(const std::string& name) const;

    /// Nearest-neighbour sampling for textures created from now on.
    void set_pixel_art(bool nearest) { pixel_art_ = nearest; }
    bool pixel_art() const { return pixel_art_; }

    /// Draw `src` of the texture with its origin at world (x, y).
    void draw_sprite(const Texture& texture, const Rect& src, float x, float y,
                     const SpriteOptions& options = {});

    /// Draw the whole texture with its origin at world (x, y).
    void draw_texture(const Texture& texture, float x, float y, const SpriteOptions& options = {});

    // ---- camera and coordinates ---------------------------------------------
    void set_camera(const Camera& cam) { camera_ = cam; }
    const Camera& camera() const { return camera_; }

    /// Size of what is being drawn into right now: the render target after
    /// set_target(), otherwise the window.
    int width() const;
    int height() const;

    /// Window (swapchain) size in pixels.
    int window_width() const;
    int window_height() const;

    /// Window pixel -> render-target pixel (accounts for pixel-mode scaling and letterboxing).
    Vec2 window_to_render(float wx, float wy) const;
    /// Render-target pixel -> world (accounts for the camera).
    Vec2 render_to_world(Vec2 p) const;
    Vec2 window_to_world(float wx, float wy) const { return render_to_world(window_to_render(wx, wy)); }

    /// Called by App each frame in pixel mode: where the target lands in the window.
    void set_presentation(const Rect& dst, float scale) {
        present_ = dst;
        present_scale_ = scale;
    }

    // ---- stats and raw access -----------------------------------------------
    int draw_calls_last_frame() const;
    int vertices_last_frame() const;
    const char* driver_name() const;
    SDL_GPUDevice* device() const;

private:
    struct Impl;

    float sx(float world_x) const { return (world_x - camera_.x) * camera_.zoom; }
    float sy(float world_y) const { return (world_y - camera_.y) * camera_.zoom; }

    std::unique_ptr<Impl> impl_;
    Camera camera_;
    bool pixel_art_ = false;
    Rect present_{};
    float present_scale_ = 1;
};

} // namespace rw
