#pragma once

// A small immediate-mode UI for the game itself (HUD, menus, tool bars):
// panels, labels, buttons, toggles, sliders, progress bars, tooltips, and
// the pieces an editor or analysis panel is made of (tabs, scrolling lists,
// text and number fields, dropdowns, tables, line charts), in screen space,
// styled by a UiStyle, drawn through the Renderer2D with the engine's fonts
// or the built-in pixel font. Not Dear ImGui: this is what players see,
// skinned by the game, and it works in pixel mode.
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

/// Per-frame mouse and keyboard state, the mouse in render-target pixels
/// (App::ui_input()).
struct UiInput {
    Vec2 mouse;
    bool down = false;
    bool pressed = false;
    bool released = false;
    i32 wheel = 0;
    bool enabled = true; ///< false: nothing reacts (the mouse belongs to something else)
    // Keyboard, for the field that has the focus (Ui::wants_keyboard()).
    std::string text; ///< UTF-8 typed this frame
    bool key_backspace = false;
    bool key_enter = false;
    bool key_escape = false;
    bool key_left = false;
    bool key_right = false;
    bool key_delete = false;
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
    Color field = rgb(18, 20, 27);             ///< text fields, scroll areas, chart background
    Color selection = rgba(86, 156, 214, 120); ///< the selected row of a list or dropdown
    Color row_alt = rgba(255, 255, 255, 14);   ///< laid over every other table row
    Color grid = rgba(96, 102, 124, 120);      ///< chart grid lines and separators
};

/// One column of a table. A width of 0 shares what the fixed columns leave.
struct UiColumn {
    std::string_view title;
    float width = 0;
    bool sortable = false;
};

/// One line of a line chart: `count` samples, oldest first.
struct UiSeries {
    const float* values = nullptr;
    usize count = 0;
    Color color;
    std::string_view name; ///< shown in the legend and the hover tooltip; may be empty
};

class Ui {
public:
    /// Start a frame. `renderer` may be null (layout and hit testing only,
    /// for tests). Screen space: the renderer's camera is reset for the
    /// duration and restored by end().
    void begin(Renderer2D* renderer, const UiInput& input, const Font* font = nullptr, const UiStyle& style = {});
    /// Draw everything this many times larger, from the next begin() on: a
    /// UI laid out for 720 lines stays the same size on a screen with 1440.
    /// Rectangles and the mouse are then in UI units, screen pixels / scale.
    void set_scale(float scale) { scale_ = scale > 0.1f ? scale : 1.0f; }
    float scale() const { return scale_; }
    void end();

    // ---- layout ----------------------------------------------------------------
    /// A panel with an optional title; widgets flow top to bottom inside.
    void begin_panel(const Rect& rect, std::string_view title = {});
    void end_panel();
    /// Put the next widget to the right of the previous one instead of below.
    void same_line(float gap = -1);
    void spacing(float height = -1);
    /// A thin line across the panel.
    void separator();
    /// Where the next widget goes (top-left), and the content width left.
    Vec2 cursor() const { return cursor_; }
    float avail_width() const;
    Rect last_rect() const { return last_; }

    /// A region `height` tall across the panel whose content scrolls: the
    /// wheel over it, or the bar that appears when the content is taller.
    /// The offset is remembered per `id`. The renderer has no scissor, so
    /// a widget shows and reacts only while the whole of it is inside the
    /// region: rows appear and disappear whole at the edges.
    void begin_scroll(std::string_view id, float height);
    void end_scroll();

    // ---- widgets (true = changed / clicked this frame) ---------------------------
    void label(std::string_view text, bool dim = false);
    /// A label in the title colour with a little room above it.
    void heading(std::string_view text);
    bool button(std::string_view text, float width = 0);
    bool toggle(std::string_view text, bool& value);
    bool slider(std::string_view text, float& value, float min, float max, float width = 160);
    void progress(float fraction, float width = 160, Color color = {0, 0, 0, 0}, std::string_view text = {});
    /// Free placement: a button covering `rect`, wherever it is.
    bool button_at(const Rect& rect, std::string_view text);
    /// A list row across the panel (or `width`), lit when selected or under
    /// the mouse; true when clicked. Consecutive rows touch.
    bool selectable(std::string_view text, bool selected, float width = 0);
    /// A row of tab buttons, the current one in the accent colour.
    bool tabs(std::string_view id, const std::vector<std::string_view>& names, int& current);
    /// One line of text. A click gives it the keyboard; a click elsewhere,
    /// Enter or Escape takes it away. True when the text changed.
    bool text_field(std::string_view id, std::string& value, float width = 160);
    /// A number with - and + beside it. Typed text becomes the value on
    /// Enter or when the field loses the keyboard (Escape drops it), rounded
    /// to `decimals` and clamped. True when the value changed.
    bool number_field(std::string_view id, float& value, float step = 1, float min = -1e30f, float max = 1e30f,
                      float width = 100, int decimals = 0);
    /// A button showing the current option; a click opens the list below
    /// it, drawn at end() over everything else. While the list is open it
    /// owns the mouse. True when another option was picked.
    bool dropdown(std::string_view id, const std::vector<std::string_view>& options, int& current, float width = 160);
    /// Text for the last widget while the mouse is on it (drawn at end()).
    void tooltip(std::string_view text);

    // ---- tables and charts -------------------------------------------------------
    /// A header row, then cells left to right, wrapping into rows with
    /// every other one shaded. A click on a sortable header picks that
    /// column, a second click flips the direction; the choice is kept per
    /// `id` and the caller sorts its own rows by it.
    void begin_table(std::string_view id, const std::vector<UiColumn>& columns);
    void table_cell(std::string_view text);
    void end_table();
    /// For the table begun last: the column to sort by, -1 while none was clicked.
    int table_sort_column() const;
    bool table_sort_descending() const;

    /// Lines over a shared value axis, each series stretched over the full
    /// width, with grid lines, value labels and a legend row below (which
    /// adds a text line to `height` when a series has a name). The range
    /// follows the data when y_min == y_max. A width of 0 fills the panel.
    /// Under the mouse a marker shows the nearest sample of every series.
    void line_chart(const UiSeries* series, usize series_count, float width, float height, float y_min = 0,
                    float y_max = 0);

    // ---- queries ----------------------------------------------------------------
    bool hovered_last() const { return hot_ == last_id_ && last_id_ != 0; }
    /// The mouse is over a panel or widget, or a dropdown list is open: the
    /// game should ignore it.
    bool wants_mouse() const { return wants_mouse_ || open_dropdown_ != 0 || swallow_; }
    /// A field has the keyboard: typing is text, not game keys.
    bool wants_keyboard() const { return focus_ != 0 || focus_pending_ != 0; }
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
    /// What a scroll area remembers between frames.
    struct Scroll {
        u32 key;
        float offset;
        float content; ///< height of what was laid out inside, last frame
        float grab;    ///< where on the thumb the drag took hold
    };
    /// A scroll area between begin_scroll() and end_scroll().
    struct ScrollFrame {
        usize index = 0; ///< into scrolls_
        u32 id = 0;      ///< of the bar
        Rect view;
        float offset = 0;
        Vec2 resume;     ///< the cursor below the area
        Rect clip;       ///< what was in effect outside
        bool clipping = false;
        bool culled = false;
    };
    struct Table {
        u32 key;
        int sort_column;
        bool descending;
    };
    u32 next_id(std::string_view label);
    bool behaviour(u32 id, const Rect& rect, bool* hovered);
    void place(float width, float height, Rect& out);
    void place_row(float width, float height, Rect& out);
    bool clipped(const Rect& r) const;
    bool drawing() const { return renderer_ != nullptr && !culled_; }
    void fill_rounded(const Rect& r, Color c, const Texture* skin, float skin_border);
    void stroke_rounded(const Rect& r, Color c);
    void triangle(Vec2 a, Vec2 b, Vec2 c, Color color);
    void text(Vec2 at, std::string_view s, Color c);
    void text_fit(Vec2 at, std::string_view s, Color c, float width);
    std::string_view fit(std::string_view s, float width) const;
    u32 field(u32 id, u32 key, const Rect& r, std::string& s, bool numeric);
    void visible_span(std::string_view s, usize caret, float width, usize& first, usize& last) const;
    usize caret_at(std::string_view s, usize first, usize last, float x) const;

    Renderer2D* renderer_ = nullptr;
    const Font* font_ = nullptr;
    UiStyle style_;
    UiInput in_;
    UiInput prev_in_;
    Camera saved_camera_;
    Camera scaled_camera_;
    float scale_ = 1;
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
    u32 frame_ = 0; ///< counts begin() calls, for the caret's blink

    // Scroll areas: the view stands in for a scissor rectangle.
    std::vector<Scroll> scrolls_;
    std::vector<ScrollFrame> scroll_stack_;
    Rect clip_{};
    bool clipping_ = false;
    bool culled_ = false;     ///< the widget placed last is not wholly inside clip_
    bool wheel_used_ = false; ///< the innermost scroll area under the mouse took it
    Vec2 chain_;              ///< the cursor a list or table row left, see place_row()

    // Fields: one has the keyboard at a time.
    u32 focus_ = 0;
    u32 focus_pending_ = 0;    ///< clicked this frame, focused from the next begin()
    bool focus_fresh_ = false; ///< focus_ was taken at this begin()
    bool focus_seen_ = false;
    usize caret_ = 0;          ///< byte offset into the focused text
    u32 caret_frame_ = 0;      ///< frame_ of the last edit: the blink restarts there
    std::string edit_;         ///< what is being typed into the focused number field

    // The one open dropdown and its list, drawn at end().
    u32 open_dropdown_ = 0;
    u32 popup_owner_ = 0; ///< widget id of its button, the only one that still reacts
    bool popup_seen_ = false;
    bool swallow_ = false; ///< the click that closed the list reaches nothing else
    Rect popup_rect_{};
    int popup_hover_ = -1;
    int popup_current_ = -1;
    std::vector<std::string> popup_options_;

    // Tables: sort choices per id, and the one being laid out.
    std::vector<Table> tables_;
    usize table_ = 0; ///< into tables_: the table begun last
    bool table_open_ = false;
    std::vector<float> table_widths_;
    float table_total_ = 0;
    usize table_column_ = 0;
    u32 table_row_ = 0;
    Rect table_row_rect_{};
};

} // namespace rw
