#pragma once

#include "rewind/core/curve.h"
#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/mesh.h"
#include "rewind/project/asset_ref.h"
#include "rewind/render/texture.h"

#include <memory>
#include <string>
#include <vector>

struct SDL_GPUDevice;
struct SDL_Window;
struct ImDrawData;

namespace rw {

class FileWatcher;
struct Font;
struct Material;

struct Color {
    u8 r = 255, g = 255, b = 255, a = 255;

    void reflect(Visitor& v) {
        v.hint(Widget::Color);
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

    Vec2 to_screen(Vec2 world) const { return {(world.x - x) * zoom, (world.y - y) * zoom}; }
    Vec2 to_world(Vec2 screen) const { return {screen.x / zoom + x, screen.y / zoom + y}; }
    Vec2 center(float view_w, float view_h) const { return {x + view_w / (2 * zoom), y + view_h / (2 * zoom)}; }
    /// A camera looking at `center` with `zoom`, for a view of the given size.
    static Camera centered(Vec2 center, float zoom, float view_w, float view_h) {
        return {center.x - view_w / (2 * zoom), center.y - view_h / (2 * zoom), zoom};
    }

    void reflect(Visitor& v) {
        v.field("x", x);
        v.field("y", y);
        v.field("zoom", zoom, {.min = 0.01f, .max = 64});
    }
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

    // ---- meshes -------------------------------------------------------------
    /// Draw a triangle list with the current material. Vertex colours
    /// multiply the texture (a 1x1 white one when `texture` is null).
    /// `transform` is applied first, then, with `world`, the camera. z is
    /// kept: it only matters with set_depth_test() or a custom matrix.
    void draw_mesh(const Mesh& mesh, const Texture* texture = nullptr, const Mat4* transform = nullptr,
                   bool world = true);

    /// Thick line in world space, see StrokeStyle for joins, caps, dashes and
    /// UVs along the length. Builds a mesh every call; for static geometry
    /// keep a Mesh from build_polyline() and draw_mesh() it instead.
    void draw_polyline(const std::vector<Vec2>& points, const StrokeStyle& style, const Texture* texture = nullptr);

    /// Filled simple polygon in world space (ear clipping, no holes).
    void fill_polygon(const std::vector<Vec2>& points, Color c, const Texture* texture = nullptr,
                      float uv_scale = 0);

    /// Stroke a Path (core/curve.h), flattened to within `tolerance` world units.
    void draw_path(const Path& path, const StrokeStyle& style, float tolerance = 0.25f,
                   const Texture* texture = nullptr);

    /// Fill a closed Path.
    void fill_path(const Path& path, Color c, float tolerance = 0.25f, const Texture* texture = nullptr,
                   float uv_scale = 0);

    // ---- layers, matrices, depth -------------------------------------------
    /// Draw layer for everything drawn from now on. Within a render target,
    /// lower layers are drawn first, and drawing order is kept inside a
    /// layer. Reset to 0 by begin_frame(). Lets a game draw ground, objects
    /// and UI in any code order.
    void set_layer(i32 layer);
    i32 layer() const;

    /// Replace the pixel-space projection for subsequent drawing with your
    /// own view-projection (column-major, see Mat4::perspective/look_at).
    /// nullptr restores the default. Shapes and sprites still go through the
    /// 2D camera; meshes get the matrix directly, which is the 3D path.
    void set_view_projection(const Mat4* matrix);

    /// Depth-tested drawing (less-or-equal, with writes) for subsequent
    /// draws. Each switch to true starts a freshly cleared depth buffer.
    /// Ignored, with a warning at startup, when the GPU offers no depth
    /// format. Reset by begin_frame().
    void set_depth_test(bool enabled);

    // ---- fonts --------------------------------------------------------------
    /// Load a TrueType or OpenType file at a pixel size. Cached by path and
    /// size, re-baked when the file changes. Returns a font with ok == false
    /// (and draws nothing) when the file cannot be read.
    Font* load_font(const std::string& path, float pixel_size);
    Font* load_font(const AssetRef& ref, float pixel_size);

    /// Draw UTF-8 text with (x, y) at the top-left of the first line. Screen
    /// space by default; `world` applies the camera. `scale` multiplies the
    /// baked size (use 1 for crisp text and bake the size you need).
    void draw_text(const Font& font, float x, float y, std::string_view text, Color c, float scale = 1,
                   bool world = false);
    Vec2 measure_text(const Font& font, std::string_view text, float scale = 1) const;

    // ---- textures -----------------------------------------------------------
    /// Load a PNG. Cached by path: calling again returns the same texture,
    /// unless the file changed on disk, in which case it is re-read into the
    /// same Texture object. The file is tracked for asset hot reload.
    Texture* load_texture(const std::string& path);
    /// Same through an asset reference (project/asset_ref.h); nullptr when it resolves to nothing.
    Texture* load_texture(const AssetRef& ref);

    /// Create or update a texture from RGBA8 pixels (row-major, 4 bytes per
    /// pixel). Cached by name. An existing texture is updated in place, even
    /// when the size changes, so pointers held by the game stay valid.
    Texture* create_texture(const std::string& name, int width, int height, const u8* rgba);

    /// Asset hot reload plumbing: the watcher that gets told about files this
    /// renderer reads, and the way to mark one as changed.
    void set_file_watcher(FileWatcher* watcher);
    void track_file(const std::string& path);
    void invalidate_file(const std::string& path);

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

    // ---- materials ----------------------------------------------------------
    /// Load a fragment shader (.spv built by rewind_add_game SHADERS) as a
    /// material. Cached by path; a changed file or source is reloaded into the
    /// same Material object. A failed load keeps the previous pipeline and
    /// reports in `material->error`.
    Material* load_material(const std::string& spv_path);
    Material* load_material(const AssetRef& ref);
    Material* find_material(const std::string& spv_path) const;

    /// Use `material` for the sprites and shapes drawn from now on in this
    /// frame; nullptr restores the default shader. Reset at every begin_frame().
    void set_material(Material* material);

    /// Fragment uniform data (set 3, binding 0) for subsequent draws, at most
    /// 256 bytes. Cleared by begin_frame().
    void set_material_params(const void* data, usize size);
    template <class T> void set_material_params(const T& params) { set_material_params(&params, sizeof params); }

    /// Extra sampled texture for subsequent draws: `slot` 1..3 maps to
    /// binding 1..3 of set 2. nullptr unbinds.
    void set_material_texture(u32 slot, Texture* texture);

    // ---- camera and coordinates ---------------------------------------------
    void set_camera(const Camera& cam) {
        game_camera_ = cam;
        if (!camera_override_) camera_ = cam;
    }
    /// The camera in effect (the override while one is set).
    const Camera& camera() const { return camera_; }

    /// Tools only: replace whatever camera the game sets with this one until
    /// called with nullptr. The editor's free camera uses it.
    void set_camera_override(const Camera* cam) {
        camera_override_ = cam != nullptr;
        camera_ = cam ? *cam : game_camera_;
    }
    bool camera_overridden() const { return camera_override_; }

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
    Camera game_camera_;
    bool camera_override_ = false;
    bool pixel_art_ = false;
    Rect present_{};
    float present_scale_ = 1;
};

} // namespace rw
