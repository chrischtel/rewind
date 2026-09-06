#pragma once

// A small immediate-mode UI for the game itself (HUD, menus, tool bars):
// panels, labels, buttons, toggles, sliders, progress bars, tooltips, in
// screen space, styled by a UiStyle, drawn through the Renderer2D with the
// engine's fonts or the built-in pixel font. Not Dear ImGui: this is what
// players see, skinned by the game, and it works in pixel mode.
//
// UI is presentation. When a click must change the simulation, turn it
// into a command: App::push_command() puts it into the next InputFrame,
// so replays, rewinds and network peers see the same click.

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/renderer2d.h"

#include <string>
#include <string_view>
#include <vector>

namespace rw {

struct Font;

/// Per-frame mouse state in render-target pixels (App::ui_input()).
struct UiInput {
    Vec2 mouse;
    bool down = false;
    bool pressed = false;
    bool released = false;
    i32 wheel = 0;
    bool enabled = true; ///< false: nothing reacts (the mouse belongs to something else)
};

struct UiStyle {
    Color panel = rgba(26, 28, 36, 235);
    Color panel_border = rgba(74, 78, 96, 255);
    Color title = rgb(235, 235, 240);
    Color text = rgb(225, 225, 232);
    Color text_dim = rgb(150, 152, 168);
    Color button = rgb(52, 56, 70);
    Color button_hover = rgb(74, 80, 100);
    Color button_active = rgb(86, 132, 200);
    Color button_border = rgba(96, 102, 124, 255);
    Color accent = rgb(86, 156, 214);
    Color track = rgb(40, 42, 52);
    Color tooltip = rgba(18, 18, 24, 245);
    float padding = 8;
    float spacing = 6;
    float item_height = 26;
    float rounding = 4;     ///< 0 = square corners
    int pixel_font_scale = 2; ///< when no Font is given
    const Texture* panel_texture = nullptr; ///< optional nine-slice skin for panels
    float panel_texture_border = 8;
    const Texture* button_texture = nullptr;
    float button_texture_border = 6;
};

class Ui {
public:
    /// Start a frame. `renderer` may be null (layout and hit testing only,
    /// for tests). Screen space: the renderer's camera is reset for the
    /// duration and restored by end().
    void begin(Renderer2D* renderer, const UiInput& input, const Font* font = nullptr, const UiStyle& style = {});
    void end();

    // ---- layout ----------------------------------------------------------------
    /// A panel with an optional title; widgets flow top to bottom inside.
    void begin_panel(const Rect& rect, std::string_view title = {});
    void end_panel();
    /// Put the next widget to the right of the previous one instead of below.
    void same_line(float gap = -1);
    void spacing(float height = -1);
    /// Where the next widget goes (top-left), and the content width left.
    Vec2 cursor() const { return cursor_; }
    float avail_width() const;
    Rect last_rect() const { return last_; }

    // ---- widgets (true = changed / clicked this frame) ---------------------------
    void label(std::string_view text, bool dim = false);
    bool button(std::string_view text, float width = 0);
    bool toggle(std::string_view text, bool& value);
    bool slider(std::string_view text, float& value, float min, float max, float width = 160);
    void progress(float fraction, float width = 160, Color color = {0, 0, 0, 0}, std::string_view text = {});
    /// Free placement: a button covering `rect`, wherever it is.
    bool button_at(const Rect& rect, std::string_view text);
    /// Text for the last widget while the mouse is on it (drawn at end()).
    void tooltip(std::string_view text);

    // ---- queries ----------------------------------------------------------------
    bool hovered_last() const { return hot_ == last_id_ && last_id_ != 0; }
    /// The mouse is over a panel or widget: the game should ignore it.
    bool wants_mouse() const { return wants_mouse_; }
    bool any_active() const { return active_ != 0; }
    Vec2 measure(std::string_view text) const;
    float line_height() const;

    const UiStyle& style() const { return style_; }

private:
    struct Panel {
        Rect rect;
        Vec2 cursor;
        float row_height;
    };
    u32 next_id(std::string_view label);
    bool behaviour(u32 id, const Rect& rect, bool* hovered);
    void place(float width, float height, Rect& out);
    void fill_rounded(const Rect& r, Color c, const Texture* skin, float skin_border);
    void stroke_rounded(const Rect& r, Color c);
    void text(Vec2 at, std::string_view s, Color c);

    Renderer2D* renderer_ = nullptr;
    const Font* font_ = nullptr;
    UiStyle style_;
    UiInput in_;
    UiInput prev_in_;
    Camera saved_camera_;
    bool saved_override_ = false;
    std::vector<Panel> panels_;
    Vec2 cursor_;
    Rect last_{};
    float row_height_ = 0;
    bool same_line_ = false;
    float same_line_gap_ = 0;
    u32 last_id_ = 0;
    u32 id_counter_ = 0;
    u32 hot_ = 0;      ///< resolved at end(), applies next frame (like the gizmos)
    u32 hot_next_ = 0;
    u32 active_ = 0;
    bool wants_mouse_ = false;
    bool wants_mouse_next_ = false;
    std::string tooltip_;
    Vec2 tooltip_at_;
};

} // namespace rw
