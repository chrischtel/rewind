#include "rewind/render/ui.h"

#include "rewind/render/font.h"
#include "rewind/render/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rw {

namespace {
constexpr int kPixelCharW = 6;
constexpr int kPixelCharH = 8;
constexpr float kScrollBar = 10;  // width of a scroll area's bar
constexpr float kScrollInset = 2; // between a scroll area's frame and its content
constexpr float kWheelRows = 2;   // item heights per wheel click
constexpr u32 kCaretBlink = 30;   // frames the caret is shown, then hidden

// What a field did this frame.
enum : u32 { kFieldChanged = 1, kFieldCommit = 2 };

u32 hash_label(std::string_view s, u32 salt) {
    u32 h = 2166136261u ^ salt;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= 16777619u;
    }
    return h ? h : 1;
}

// State that outlives the frame (scroll offsets, the focused field, the
// open dropdown, sort columns) is keyed by the id string alone: next_id()
// counts widgets, and that count shifts when rows come and go.
u32 state_key(std::string_view s) { return hash_label(s, 0x9e3779b9u); }

// Byte offsets of the UTF-8 character boundaries around `i`.
usize prev_char(std::string_view s, usize i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<u8>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

usize next_char(std::string_view s, usize i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<u8>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

std::string format_number(float v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 6), static_cast<double>(v));
    std::string out = buf;
    // "-0" is what a small negative value rounds to, and reads as a bug.
    if (!out.empty() && out[0] == '-' && out.find_first_not_of("-0.") == std::string::npos) out.erase(0, 1);
    return out;
}

float round_to(float v, int decimals) {
    float scale = 1;
    for (int i = 0; i < std::clamp(decimals, 0, 6); ++i) scale *= 10;
    return std::round(v * scale) / scale;
}

std::vector<Vec2> rounded_points(const Rect& r, float radius) {
    std::vector<Vec2> pts;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    if (radius <= 0.5f) {
        pts = {{r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()}, {r.x, r.bottom()}};
        return pts;
    }
    const int steps = 4;
    auto corner = [&](Vec2 c, float a0) {
        for (int i = 0; i <= steps; ++i) {
            const float a = a0 + static_cast<float>(i) / steps * 1.5707963f;
            pts.push_back({c.x + std::cos(a) * radius, c.y + std::sin(a) * radius});
        }
    };
    corner({r.x + radius, r.y + radius}, 3.1415926f);
    corner({r.right() - radius, r.y + radius}, -1.5707963f);
    corner({r.right() - radius, r.bottom() - radius}, 0);
    corner({r.x + radius, r.bottom() - radius}, 1.5707963f);
    return pts;
}
} // namespace

// ---- frame ---------------------------------------------------------------------------

void Ui::begin(Renderer2D* renderer, const UiInput& input, const Font* font, const UiStyle& style) {
    renderer_ = renderer;
    font_ = font && font->ok ? font : nullptr;
    style_ = style;
    prev_in_ = in_;
    in_ = input;
    in_.mouse = input.mouse * (1.0f / scale_);
    if (renderer_) {
        // Screen space even under the editor's camera override.
        saved_camera_ = renderer_->camera();
        saved_override_ = renderer_->camera_overridden();
        scaled_camera_ = Camera{0, 0, scale_};
        renderer_->set_camera_override(&scaled_camera_);
    }
    panels_.clear();
    cursor_ = {style_.padding, style_.padding};
    row_height_ = 0;
    same_line_ = false;
    last_ = {};
    last_id_ = 0;
    id_counter_ = 0;
    hot_ = hot_next_;
    hot_next_ = 0;
    wants_mouse_ = wants_mouse_next_;
    wants_mouse_next_ = false;
    tooltip_.clear();
    if (!in_.down && !in_.released) active_ = 0; // a release frame still belongs to the active widget
    ++frame_;
    scroll_stack_.clear();
    clipping_ = false;
    culled_ = false;
    wheel_used_ = false;
    chain_ = {-1e9f, -1e9f};
    table_open_ = false;
    table_column_ = 0;
    if (focus_pending_ != 0) {
        // Taken a frame after the click, so the field that had the keyboard
        // has committed its text first, whichever of the two comes first.
        focus_ = focus_pending_;
        focus_pending_ = 0;
        focus_fresh_ = true;
        caret_frame_ = frame_;
    }
    focus_seen_ = false;
    popup_seen_ = false;
    if (swallow_ && !in_.down && !in_.released) swallow_ = false;
}

void Ui::end() {
    while (!scroll_stack_.empty()) end_scroll();
    while (!panels_.empty()) end_panel();
    table_open_ = false;
    clipping_ = false;
    culled_ = false;
    // A field or a list whose widget was not submitted this frame goes with it.
    if (!focus_seen_) focus_ = 0;
    focus_fresh_ = false;
    if (!popup_seen_) open_dropdown_ = 0;
    if (renderer_ && open_dropdown_ != 0 && !popup_options_.empty()) {
        fill_rounded(popup_rect_, style_.tooltip, nullptr, 0);
        const float row = popup_rect_.h / static_cast<float>(popup_options_.size());
        for (usize i = 0; i < popup_options_.size(); ++i) {
            const Rect item{popup_rect_.x, popup_rect_.y + row * static_cast<float>(i), popup_rect_.w, row};
            const int index = static_cast<int>(i);
            if (index == popup_hover_) renderer_->fill_rect(item, style_.button_hover);
            else if (index == popup_current_) renderer_->fill_rect(item, style_.selection);
            const Vec2 size = measure(popup_options_[i]);
            text({item.x + style_.padding, item.y + (row - size.y) * 0.5f}, popup_options_[i], style_.text);
        }
        stroke_rounded(popup_rect_, style_.panel_border);
    }
    if (renderer_ && !tooltip_.empty()) {
        const Vec2 size = measure(tooltip_);
        Rect r{tooltip_at_.x + 12, tooltip_at_.y + 16, size.x + style_.padding * 2, size.y + style_.padding};
        const float w = static_cast<float>(renderer_->width()), h = static_cast<float>(renderer_->height());
        if (r.right() > w) r.x = std::max(0.0f, w - r.w);
        if (r.bottom() > h) r.y = std::max(0.0f, tooltip_at_.y - r.h - 4);
        fill_rounded(r, style_.tooltip, nullptr, 0);
        stroke_rounded(r, style_.panel_border);
        text({r.x + style_.padding, r.y + style_.padding * 0.5f}, tooltip_, style_.text);
    }
    if (renderer_) renderer_->set_camera_override(saved_override_ ? &saved_camera_ : nullptr);
    renderer_ = nullptr;
}

// ---- text ------------------------------------------------------------------------------

Vec2 Ui::measure(std::string_view s) const {
    if (font_) return font_->atlas.measure(s, 1.0f);
    float w = 0, longest = 0;
    int lines = 1;
    for (char c : s) {
        if (c == '\n') {
            longest = std::max(longest, w);
            w = 0;
            ++lines;
        } else {
            w += static_cast<float>(kPixelCharW * style_.pixel_font_scale);
        }
    }
    longest = std::max(longest, w);
    return {longest, static_cast<float>(kPixelCharH * style_.pixel_font_scale * lines)};
}

float Ui::line_height() const {
    return font_ ? font_->atlas.line_height : static_cast<float>(kPixelCharH * style_.pixel_font_scale);
}

void Ui::text(Vec2 at, std::string_view s, Color c) {
    if (!drawing()) return;
    // Text is drawn in screen space whatever the camera, so it is scaled here.
    const float px = std::floor(at.x * scale_), py = std::floor(at.y * scale_);
    if (font_) renderer_->draw_text(*font_, px, py, s, c, scale_, false);
    else renderer_->draw_text(px, py, std::string(s).c_str(), c, static_cast<int>(static_cast<float>(style_.pixel_font_scale) * scale_ + 0.5f));
}

/// The longest prefix of `s` that is no wider than `width`.
std::string_view Ui::fit(std::string_view s, float width) const {
    usize n = s.size();
    while (n > 0 && measure(s.substr(0, n)).x > width) n = prev_char(s, n);
    return s.substr(0, n);
}

/// Text cut off at `width`: nothing clips it, so a long cell would run into the next.
void Ui::text_fit(Vec2 at, std::string_view s, Color c, float width) {
    if (!drawing()) return;
    text(at, fit(s, width), c);
}

// ---- drawing -----------------------------------------------------------------------------

void Ui::fill_rounded(const Rect& r, Color c, const Texture* skin, float skin_border) {
    if (!drawing()) return;
    if (skin && skin->handle) {
        renderer_->draw_nine_slice(*skin, Rect{0, 0, static_cast<float>(skin->width), static_cast<float>(skin->height)}, r,
                                   skin_border, c);
        return;
    }
    if (style_.rounding <= 0.5f) {
        renderer_->fill_rect(r, c);
        return;
    }
    renderer_->fill_polygon(rounded_points(r, style_.rounding), c);
}

void Ui::stroke_rounded(const Rect& r, Color c) {
    if (!drawing()) return;
    if (style_.rounding <= 0.5f) {
        renderer_->draw_rect(r, c);
        return;
    }
    StrokeStyle s;
    s.width = 1;
    s.closed = true;
    s.r = c.r / 255.0f;
    s.g = c.g / 255.0f;
    s.b = c.b / 255.0f;
    s.a = c.a / 255.0f;
    renderer_->draw_polyline(rounded_points(Rect{r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, style_.rounding), s);
}

/// Arrows are drawn, not typed: the pixel font has no glyphs for them.
void Ui::triangle(Vec2 a, Vec2 b, Vec2 c, Color color) {
    if (!drawing()) return;
    const std::vector<Vec2> points{a, b, c};
    renderer_->fill_polygon(points, color);
}

// ---- layout ------------------------------------------------------------------------------

void Ui::begin_panel(const Rect& rect, std::string_view title) {
    Panel p;
    p.rect = rect;
    p.cursor = {rect.x + style_.padding, rect.y + style_.padding};
    p.row_height = 0;
    fill_rounded(rect, style_.panel, style_.panel_texture, style_.panel_texture_border);
    stroke_rounded(rect, style_.panel_border);
    if (in_.enabled && rect.contains(in_.mouse.x, in_.mouse.y)) wants_mouse_next_ = true;
    if (!title.empty()) {
        text(p.cursor, title, style_.title);
        p.cursor.y += line_height() + style_.spacing;
    }
    panels_.push_back(p);
    cursor_ = p.cursor;
    row_height_ = 0;
    same_line_ = false;
}

void Ui::end_panel() {
    if (panels_.empty()) return;
    panels_.pop_back();
    if (!panels_.empty()) {
        cursor_ = panels_.back().cursor;
        row_height_ = panels_.back().row_height;
    } else {
        cursor_ = {style_.padding, style_.padding};
        row_height_ = 0;
    }
    same_line_ = false;
}

void Ui::same_line(float gap) {
    same_line_ = true;
    same_line_gap_ = gap < 0 ? style_.spacing : gap;
}

void Ui::spacing(float height) {
    cursor_.y += height < 0 ? style_.spacing : height;
    if (!panels_.empty()) panels_.back().cursor = cursor_;
}

float Ui::avail_width() const {
    if (panels_.empty()) return renderer_ ? static_cast<float>(renderer_->width()) - cursor_.x : 1e6f;
    return panels_.back().rect.right() - style_.padding - cursor_.x;
}

void Ui::place(float width, float height, Rect& out) {
    if (same_line_) {
        cursor_.x = last_.right() + same_line_gap_;
        cursor_.y = last_.y;
        same_line_ = false;
    }
    out = Rect{cursor_.x, cursor_.y, width, height};
    last_ = out;
    culled_ = clipped(out);
    row_height_ = std::max(row_height_, height);
    // Next widget below this row (same_line() moves it back beside).
    cursor_.x = panels_.empty() ? style_.padding : panels_.back().rect.x + style_.padding;
    cursor_.y = out.y + row_height_ + style_.spacing;
    row_height_ = 0;
    if (!panels_.empty()) {
        panels_.back().cursor = cursor_;
        panels_.back().row_height = row_height_;
    }
}

/// place() for the rows of a list or a table, which touch: when the cursor
/// is still where the previous row left it, the spacing is taken back.
void Ui::place_row(float width, float height, Rect& out) {
    if (!same_line_ && cursor_ == chain_) cursor_.y -= style_.spacing;
    place(width, height, out);
    chain_ = cursor_;
}

/// Inside a scroll area: true when `r` is not wholly inside the view from
/// top to bottom. The renderer has no scissor, so such a widget is neither
/// drawn nor hit; rows come and go whole at the edges.
bool Ui::clipped(const Rect& r) const {
    return clipping_ && (r.y < clip_.y - 0.5f || r.bottom() > clip_.bottom() + 0.5f);
}

u32 Ui::next_id(std::string_view label) { return hash_label(label, ++id_counter_); }

bool Ui::behaviour(u32 id, const Rect& rect, bool* hovered) {
    last_id_ = id;
    bool over = in_.enabled && !culled_ && rect.contains(in_.mouse.x, in_.mouse.y);
    // An open dropdown list owns the mouse, and so does the click that closed it.
    if (swallow_ || (open_dropdown_ != 0 && id != popup_owner_)) over = false;
    if (over) {
        hot_next_ = id;
        wants_mouse_next_ = true;
    }
    if (hovered) *hovered = hot_ == id;
    if (in_.pressed && over && active_ == 0) active_ = id;
    if (in_.released && active_ == id) {
        active_ = 0;
        return over;
    }
    return false;
}

void Ui::separator() {
    Rect r;
    place(avail_width(), 1, r);
    last_id_ = 0;
    if (drawing()) renderer_->fill_rect(r, style_.grid);
}

// ---- scroll areas ------------------------------------------------------------------------

void Ui::begin_scroll(std::string_view id, float height) {
    const u32 key = state_key(id);
    usize index = 0;
    while (index < scrolls_.size() && scrolls_[index].key != key) ++index;
    if (index == scrolls_.size()) scrolls_.push_back({key, 0, 0, 0});

    ScrollFrame f;
    f.index = index;
    f.offset = scrolls_[index].offset;
    place(avail_width(), height, f.view);
    f.id = next_id(id);
    last_id_ = 0;
    f.resume = cursor_;
    f.clip = clip_;
    f.clipping = clipping_;
    f.culled = culled_;
    fill_rounded(f.view, style_.field, nullptr, 0);
    stroke_rounded(f.view, style_.button_border);
    if (in_.enabled && !culled_ && f.view.contains(in_.mouse.x, in_.mouse.y)) wants_mouse_next_ = true;

    // The view is the clip rectangle for what follows; inside another
    // scroll area it is what both let through, nothing when this area is
    // itself out of view.
    if (culled_) {
        clip_ = Rect{f.view.x, f.view.y, f.view.w, -1e6f};
    } else if (clipping_) {
        const float top = std::max(clip_.y, f.view.y);
        const float bottom = std::min(clip_.bottom(), f.view.bottom());
        clip_ = Rect{f.view.x, top, f.view.w, bottom - top};
    } else {
        clip_ = f.view;
    }
    clipping_ = true;

    // Content flows like in a panel, shifted up by the offset. The bar's
    // strip is kept free once last frame's content was taller than the view.
    const float bar = scrolls_[index].content > f.view.h ? kScrollBar : 0.0f;
    Panel p;
    p.rect = Rect{f.view.x - style_.padding + kScrollInset, f.view.y,
                  f.view.w - bar + (style_.padding - kScrollInset) * 2, f.view.h};
    p.cursor = {f.view.x + kScrollInset, f.view.y + kScrollInset - f.offset};
    p.row_height = 0;
    panels_.push_back(p);
    scroll_stack_.push_back(f);
    cursor_ = p.cursor;
    row_height_ = 0;
    same_line_ = false;
}

void Ui::end_scroll() {
    if (scroll_stack_.empty()) return;
    const ScrollFrame f = scroll_stack_.back();
    scroll_stack_.pop_back();
    Scroll& state = scrolls_[f.index];
    // What was laid out, from the inset above the first row to the one below the last.
    state.content = std::max(cursor_.y - style_.spacing - (f.view.y - f.offset) + kScrollInset, 0.0f);

    if (!panels_.empty()) panels_.pop_back();
    clip_ = f.clip;
    clipping_ = f.clipping;
    culled_ = f.culled;
    cursor_ = f.resume;
    row_height_ = 0;
    same_line_ = false;
    if (!panels_.empty()) {
        panels_.back().cursor = cursor_;
        panels_.back().row_height = 0;
    }
    last_ = f.view;
    last_id_ = 0;

    // The wheel goes to the innermost area under the mouse (it ends first),
    // and like hover it shows a frame later.
    const bool over = in_.enabled && !culled_ && !swallow_ && open_dropdown_ == 0 &&
                      f.view.contains(in_.mouse.x, in_.mouse.y);
    if (over && in_.wheel != 0 && !wheel_used_) {
        state.offset -= static_cast<float>(in_.wheel) * style_.item_height * kWheelRows;
        wheel_used_ = true;
    }

    const float max_offset = std::max(state.content - f.view.h, 0.0f);
    if (max_offset <= 0) {
        state.offset = 0;
        return;
    }
    const Rect track{f.view.right() - kScrollBar, f.view.y, kScrollBar, f.view.h};
    const float thumb_h = clampf(f.view.h * f.view.h / state.content, std::min(16.0f, f.view.h), f.view.h);
    const float travel = std::max(f.view.h - thumb_h, 1.0f);
    bool hovered = false;
    behaviour(f.id, track, &hovered);
    last_id_ = 0;
    if (active_ == f.id && in_.enabled) {
        const float thumb_y = track.y + travel * clampf(state.offset / max_offset, 0, 1);
        // Grabbed on the thumb it keeps its place under the mouse; a click on the track centres it there.
        if (in_.pressed) {
            const bool on_thumb = in_.mouse.y >= thumb_y && in_.mouse.y < thumb_y + thumb_h;
            state.grab = on_thumb ? in_.mouse.y - thumb_y : thumb_h * 0.5f;
        }
        state.offset = clampf((in_.mouse.y - state.grab - track.y) / travel, 0, 1) * max_offset;
    }
    state.offset = clampf(state.offset, 0, max_offset);
    if (drawing()) {
        const Rect thumb{track.x + 2, track.y + travel * (state.offset / max_offset), track.w - 4, thumb_h};
        Color c = style_.button_border;
        if (active_ == f.id) c = style_.button_active;
        else if (hovered) c = style_.button_hover;
        renderer_->fill_rect(track, style_.track);
        renderer_->fill_rect(thumb, c);
    }
}

// ---- widgets -----------------------------------------------------------------------------

void Ui::label(std::string_view s, bool dim) {
    const Vec2 size = measure(s);
    Rect r;
    place(size.x, std::max(size.y, style_.item_height), r);
    last_id_ = 0;
    text({r.x, r.y + (r.h - size.y) * 0.5f}, s, dim ? style_.text_dim : style_.text);
}

void Ui::heading(std::string_view s) {
    spacing(style_.spacing);
    const Vec2 size = measure(s);
    Rect r;
    place(size.x, size.y, r);
    last_id_ = 0;
    text({r.x, r.y}, s, style_.title);
}

bool Ui::button_at(const Rect& rect, std::string_view s) {
    const u32 id = next_id(s);
    culled_ = clipped(rect);
    bool hovered = false;
    const bool clicked = behaviour(id, rect, &hovered);
    last_ = rect;
    Color c = style_.button;
    if (active_ == id) c = style_.button_active;
    else if (hovered) c = style_.button_hover;
    fill_rounded(rect, c, style_.button_texture, style_.button_texture_border);
    stroke_rounded(rect, style_.button_border);
    const Vec2 size = measure(s);
    text({rect.x + (rect.w - size.x) * 0.5f, rect.y + (rect.h - size.y) * 0.5f}, s, style_.text);
    return clicked;
}

bool Ui::button(std::string_view s, float width) {
    const Vec2 size = measure(s);
    Rect r;
    place(width > 0 ? width : size.x + style_.padding * 2, style_.item_height, r);
    return button_at(r, s);
}

bool Ui::toggle(std::string_view s, bool& value) {
    const Vec2 size = measure(s);
    const float box = style_.item_height * 0.6f;
    Rect r;
    place(box + style_.spacing + size.x, style_.item_height, r);
    const u32 id = next_id(s);
    bool hovered = false;
    const bool clicked = behaviour(id, r, &hovered);
    if (clicked) value = !value;
    const Rect b{r.x, r.y + (r.h - box) * 0.5f, box, box};
    fill_rounded(b, value ? style_.accent : (hovered ? style_.button_hover : style_.button), nullptr, 0);
    stroke_rounded(b, style_.button_border);
    text({b.right() + style_.spacing, r.y + (r.h - size.y) * 0.5f}, s, style_.text);
    return clicked;
}

bool Ui::slider(std::string_view s, float& value, float min, float max, float width) {
    const Vec2 size = measure(s);
    Rect r;
    place(width, style_.item_height, r);
    const u32 id = next_id(s);
    bool hovered = false;
    behaviour(id, r, &hovered);
    bool changed = false;
    if (active_ == id && in_.enabled && max > min) {
        const float t = clampf((in_.mouse.x - r.x) / std::max(r.w, 1.0f), 0, 1);
        const float next = min + (max - min) * t;
        changed = next != value;
        value = next;
    }
    const float frac = max > min ? clampf((value - min) / (max - min), 0, 1) : 0;
    fill_rounded(r, style_.track, nullptr, 0);
    fill_rounded(Rect{r.x, r.y, std::max(r.w * frac, 0.0f), r.h}, active_ == id ? style_.button_active : style_.accent, nullptr, 0);
    stroke_rounded(r, hovered ? style_.button_hover : style_.button_border);
    text({r.x + style_.padding, r.y + (r.h - size.y) * 0.5f}, s, style_.text);
    return changed;
}

void Ui::progress(float fraction, float width, Color color, std::string_view s) {
    Rect r;
    place(width, style_.item_height * 0.6f, r);
    last_id_ = 0;
    fill_rounded(r, style_.track, nullptr, 0);
    fill_rounded(Rect{r.x, r.y, r.w * clampf(fraction, 0, 1), r.h}, color.a == 0 ? style_.accent : color, nullptr, 0);
    stroke_rounded(r, style_.button_border);
    if (!s.empty()) {
        const Vec2 size = measure(s);
        text({r.x + (r.w - size.x) * 0.5f, r.y + (r.h - size.y) * 0.5f}, s, style_.text);
    }
}

bool Ui::selectable(std::string_view s, bool selected, float width) {
    Rect r;
    place_row(width > 0 ? width : avail_width(), style_.item_height, r);
    const u32 id = next_id(s);
    bool hovered = false;
    const bool clicked = behaviour(id, r, &hovered);
    if (drawing()) {
        if (selected) renderer_->fill_rect(r, style_.selection);
        else if (hovered || active_ == id) renderer_->fill_rect(r, style_.button_hover);
    }
    text_fit({r.x + style_.padding, r.y + (r.h - line_height()) * 0.5f}, s, selected ? style_.title : style_.text,
             r.w - style_.padding * 2);
    return clicked;
}

bool Ui::tabs(std::string_view id, const std::vector<std::string_view>& names, int& current) {
    const u32 base = next_id(id);
    float total = 0;
    for (std::string_view name : names) total += measure(name).x + style_.padding * 2;
    Rect r;
    place(total, style_.item_height, r);
    bool changed = false;
    float x = r.x;
    for (usize i = 0; i < names.size(); ++i) {
        const Vec2 size = measure(names[i]);
        const Rect tab{x, r.y, size.x + style_.padding * 2, r.h};
        x += tab.w;
        const u32 tab_id = hash_label(names[i], base + static_cast<u32>(i));
        const int index = static_cast<int>(i);
        bool hovered = false;
        if (behaviour(tab_id, tab, &hovered) && current != index) {
            current = index;
            changed = true;
        }
        Color c = style_.button;
        if (current == index) c = style_.accent;
        else if (active_ == tab_id) c = style_.button_active;
        else if (hovered) c = style_.button_hover;
        fill_rounded(tab, c, nullptr, 0);
        stroke_rounded(tab, style_.button_border);
        text({tab.x + (tab.w - size.x) * 0.5f, tab.y + (tab.h - size.y) * 0.5f}, names[i],
             current == index ? style_.title : style_.text);
    }
    last_id_ = 0;
    return changed;
}

// ---- fields ------------------------------------------------------------------------------

/// The part of a field's text that fits `width` with the caret inside it:
/// a long text slides along one character at a time.
void Ui::visible_span(std::string_view s, usize caret, float width, usize& first, usize& last) const {
    first = 0;
    while (first < caret && measure(s.substr(first, caret - first)).x > width) first = next_char(s, first);
    last = caret;
    while (last < s.size()) {
        const usize next = next_char(s, last);
        if (measure(s.substr(first, next - first)).x > width) break;
        last = next;
    }
}

/// The character boundary in s[first, last] nearest to `x` pixels right of `first`.
usize Ui::caret_at(std::string_view s, usize first, usize last, float x) const {
    usize best = first;
    float best_distance = absf(x);
    for (usize i = first; i < last;) {
        i = next_char(s, i);
        const float distance = absf(measure(s.substr(first, i - first)).x - x);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

/// What text and number fields share: focus, the caret, editing `s` with
/// this frame's keys, drawing. Returns kField bits.
u32 Ui::field(u32 id, u32 key, const Rect& r, std::string& s, bool numeric) {
    bool hovered = false;
    behaviour(id, r, &hovered);
    u32 result = 0;
    const float inner = std::max(r.w - style_.padding * 2, 1.0f);
    const bool pressed_here = in_.pressed && active_ == id;
    if (pressed_here && focus_ != key) {
        focus_pending_ = key;
    } else if (in_.pressed && !pressed_here && focus_ == key) {
        // A click elsewhere: done with this field.
        focus_ = 0;
        result |= kFieldCommit;
    }

    if (focus_ == key) {
        focus_seen_ = true;
        caret_ = focus_fresh_ ? s.size() : std::min(caret_, s.size());
        const bool mouse_on = in_.enabled && r.contains(in_.mouse.x, in_.mouse.y);
        if (pressed_here || (focus_fresh_ && mouse_on)) {
            // The caret goes where the click was.
            usize first = 0, last = 0;
            visible_span(s, caret_, inner, first, last);
            caret_ = caret_at(s, first, last, in_.mouse.x - (r.x + style_.padding));
            caret_frame_ = frame_;
        }
        focus_fresh_ = false;

        std::string typed;
        for (char c : in_.text) {
            const u8 byte = static_cast<u8>(c);
            if (byte < 32 || byte == 127) continue;
            if (numeric) {
                if (c == ',') c = '.'; // the decimal key of half the world's keypads
                if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) continue;
            }
            typed.push_back(c);
        }
        if (!typed.empty()) {
            s.insert(caret_, typed);
            caret_ += typed.size();
            result |= kFieldChanged;
        }
        if (in_.key_backspace && caret_ > 0) {
            const usize from = prev_char(s, caret_);
            s.erase(from, caret_ - from);
            caret_ = from;
            result |= kFieldChanged;
        }
        if (in_.key_delete && caret_ < s.size()) {
            s.erase(caret_, next_char(s, caret_) - caret_);
            result |= kFieldChanged;
        }
        if (in_.key_left) caret_ = prev_char(s, caret_);
        if (in_.key_right) caret_ = next_char(s, caret_);
        if (!typed.empty() || in_.key_backspace || in_.key_delete || in_.key_left || in_.key_right)
            caret_frame_ = frame_; // visible while typing
        if (in_.key_enter) {
            focus_ = 0;
            result |= kFieldCommit;
        } else if (in_.key_escape) {
            focus_ = 0;
        }
    }

    const bool focused = focus_ == key;
    fill_rounded(r, style_.field, nullptr, 0);
    stroke_rounded(r, focused ? style_.accent : (hovered ? style_.button_hover : style_.button_border));
    if (drawing()) {
        const std::string_view all = s;
        usize first = 0, last = 0;
        visible_span(all, focused ? caret_ : 0, inner, first, last);
        const float height = line_height();
        const Vec2 at{r.x + style_.padding, r.y + (r.h - height) * 0.5f};
        text(at, all.substr(first, last - first), style_.text);
        if (focused && ((frame_ - caret_frame_) / kCaretBlink) % 2 == 0) {
            const float x = at.x + measure(all.substr(first, caret_ - first)).x;
            renderer_->fill_rect(std::floor(x), at.y, 1, height, style_.text);
        }
    }
    return result;
}

bool Ui::text_field(std::string_view id, std::string& value, float width) {
    Rect r;
    place(width, style_.item_height, r);
    const u32 widget = next_id(id);
    return (field(widget, state_key(id), r, value, false) & kFieldChanged) != 0;
}

bool Ui::number_field(std::string_view id, float& value, float step, float min, float max, float width, int decimals) {
    Rect r;
    place(width, style_.item_height, r);
    const u32 widget = next_id(id);
    const u32 key = state_key(id);
    const float side = std::min(style_.item_height, r.w * 0.25f);
    const Rect minus{r.x, r.y, side, r.h};
    const Rect plus{r.right() - side, r.y, side, r.h};
    const Rect middle{r.x + side + 2, r.y, std::max(r.w - (side + 2) * 2, 1.0f), r.h};
    const float before = value;

    // While it has the keyboard the field shows what is being typed, which
    // need not be a number yet ("-", "1."); otherwise the value.
    const bool editing = focus_ == key;
    if (editing && focus_fresh_) edit_ = format_number(value, decimals);
    std::string shown;
    if (!editing) shown = format_number(value, decimals);
    const u32 flags = field(widget, key, middle, editing ? edit_ : shown, true);
    if (editing && (flags & kFieldCommit) != 0) {
        char* parsed_to = nullptr;
        const float parsed = std::strtof(edit_.c_str(), &parsed_to);
        if (parsed_to != edit_.c_str() && std::isfinite(parsed)) value = clampf(round_to(parsed, decimals), min, max);
    }

    // The click that lands on a button first takes the keyboard from the
    // field (commit, on press), then steps the committed value (on release).
    if (button_at(minus, "-")) value = clampf(value - step, min, max);
    if (button_at(plus, "+")) value = clampf(value + step, min, max);
    last_ = r;
    last_id_ = widget;
    return value != before;
}

bool Ui::dropdown(std::string_view id, const std::vector<std::string_view>& options, int& current, float width) {
    Rect r;
    place(width, style_.item_height, r);
    const u32 widget = next_id(id);
    const u32 key = state_key(id);
    if (open_dropdown_ == key) popup_owner_ = widget; // its own button still reacts
    bool hovered = false;
    const bool clicked = behaviour(widget, r, &hovered);
    if (clicked) {
        open_dropdown_ = open_dropdown_ == key ? 0 : key;
        popup_owner_ = widget;
    }

    bool changed = false;
    if (open_dropdown_ == key && (culled_ || options.empty())) open_dropdown_ = 0; // scrolled out of view
    if (open_dropdown_ == key) {
        popup_seen_ = true;
        const float row = style_.item_height;
        const int count = static_cast<int>(options.size());
        float w = r.w;
        for (std::string_view option : options) w = std::max(w, measure(option).x + style_.padding * 2);
        Rect list{r.x, r.bottom() + 2, w, row * static_cast<float>(count)};
        if (renderer_) {
            // Above the button when there is no room below, and not off the right edge.
            const float screen_w = static_cast<float>(renderer_->width());
            const float screen_h = static_cast<float>(renderer_->height());
            if (list.bottom() > screen_h && r.y - 2 - list.h >= 0) list.y = r.y - 2 - list.h;
            if (list.right() > screen_w) list.x = std::max(0.0f, screen_w - list.w);
        }
        int over_item = -1;
        if (in_.enabled && list.contains(in_.mouse.x, in_.mouse.y))
            over_item = std::clamp(static_cast<int>((in_.mouse.y - list.y) / row), 0, count - 1);
        if (in_.pressed) {
            if (over_item >= 0) {
                changed = over_item != current;
                current = over_item;
                open_dropdown_ = 0;
                swallow_ = true;
            } else if (active_ != widget) {
                // A click elsewhere closes the list and does nothing else.
                open_dropdown_ = 0;
                swallow_ = true;
            }
        }
        if (open_dropdown_ == key) {
            // Drawn at end(), over whatever comes after this widget.
            wants_mouse_next_ = true;
            popup_rect_ = list;
            popup_hover_ = over_item;
            popup_current_ = current;
            popup_options_.resize(options.size());
            for (usize i = 0; i < options.size(); ++i) popup_options_[i] = std::string(options[i]);
        }
    }

    Color c = style_.button;
    if (active_ == widget || open_dropdown_ == key) c = style_.button_active;
    else if (hovered) c = style_.button_hover;
    fill_rounded(r, c, style_.button_texture, style_.button_texture_border);
    stroke_rounded(r, style_.button_border);
    const float arrow = std::floor(r.h * 0.3f);
    if (current >= 0 && current < static_cast<int>(options.size())) {
        const std::string_view shown = options[static_cast<usize>(current)];
        text_fit({r.x + style_.padding, r.y + (r.h - line_height()) * 0.5f}, shown, style_.text,
                 r.w - style_.padding * 2 - arrow - style_.spacing);
    }
    const Vec2 tip{r.right() - style_.padding - arrow * 0.5f, r.y + r.h * 0.5f + arrow * 0.3f};
    triangle({tip.x - arrow * 0.5f, tip.y - arrow * 0.6f}, {tip.x + arrow * 0.5f, tip.y - arrow * 0.6f}, tip, style_.text);
    return changed;
}

void Ui::tooltip(std::string_view s) {
    if (hovered_last()) {
        tooltip_ = std::string(s);
        tooltip_at_ = in_.mouse;
    }
}

// ---- tables ------------------------------------------------------------------------------

void Ui::begin_table(std::string_view id, const std::vector<UiColumn>& columns) {
    const u32 key = state_key(id);
    table_ = 0;
    while (table_ < tables_.size() && tables_[table_].key != key) ++table_;
    if (table_ == tables_.size()) tables_.push_back({key, -1, false});

    // Columns without a width share what the fixed ones leave.
    float fixed = 0;
    usize flexible = 0;
    for (const UiColumn& column : columns) {
        if (column.width > 0) fixed += column.width;
        else ++flexible;
    }
    const float share = flexible > 0 ? std::max((avail_width() - fixed) / static_cast<float>(flexible), 0.0f) : 0.0f;
    table_widths_.clear();
    table_total_ = 0;
    for (const UiColumn& column : columns) {
        table_widths_.push_back(column.width > 0 ? column.width : share);
        table_total_ += table_widths_.back();
    }
    table_column_ = 0;
    table_row_ = 0;
    table_open_ = true;

    Rect header;
    place(table_total_, line_height() + style_.spacing, header);
    chain_ = cursor_; // the first row sits right under the header
    if (drawing()) renderer_->fill_rect(header, style_.button);
    const float pad = style_.spacing;
    const float arrow = std::floor(header.h * 0.3f);
    float x = header.x;
    for (usize i = 0; i < columns.size(); ++i) {
        const Rect cell{x, header.y, table_widths_[i], header.h};
        x += cell.w;
        const int index = static_cast<int>(i);
        if (columns[i].sortable) {
            bool hovered = false;
            if (behaviour(next_id(columns[i].title), cell, &hovered)) {
                Table& t = tables_[table_];
                if (t.sort_column == index) {
                    t.descending = !t.descending;
                } else {
                    t.sort_column = index;
                    t.descending = false;
                }
            }
            if (hovered && drawing()) renderer_->fill_rect(cell, style_.button_hover);
        }
        const bool sorted = tables_[table_].sort_column == index;
        text_fit({cell.x + pad, cell.y + (cell.h - line_height()) * 0.5f}, columns[i].title,
                 sorted ? style_.title : style_.text_dim, cell.w - pad * 2 - (sorted ? arrow + pad : 0.0f));
        if (sorted) {
            // Pointing up for ascending, down for descending.
            const float ax = cell.right() - pad - arrow, mid = cell.y + cell.h * 0.5f;
            const float dy = tables_[table_].descending ? arrow * 0.3f : -arrow * 0.3f;
            triangle({ax, mid - dy}, {ax + arrow, mid - dy}, {ax + arrow * 0.5f, mid + dy}, style_.title);
        }
    }
    last_ = header;
    last_id_ = 0;
}

void Ui::table_cell(std::string_view s) {
    if (!table_open_ || table_widths_.empty()) return;
    if (table_column_ == 0) {
        place_row(table_total_, line_height() + style_.spacing, table_row_rect_);
        if ((table_row_ & 1u) != 0 && drawing()) renderer_->fill_rect(table_row_rect_, style_.row_alt);
    }
    culled_ = clipped(table_row_rect_);
    float x = table_row_rect_.x;
    for (usize i = 0; i < table_column_; ++i) x += table_widths_[i];
    const Rect cell{x, table_row_rect_.y, table_widths_[table_column_], table_row_rect_.h};
    const float pad = style_.spacing;
    text_fit({cell.x + pad, cell.y + (cell.h - line_height()) * 0.5f}, s, style_.text, cell.w - pad * 2);
    last_ = cell;
    last_id_ = 0;
    if (++table_column_ == table_widths_.size()) {
        table_column_ = 0;
        ++table_row_;
    }
}

void Ui::end_table() {
    table_open_ = false;
    table_column_ = 0;
    chain_ = {-1e9f, -1e9f}; // what follows keeps its distance
}

int Ui::table_sort_column() const { return table_ < tables_.size() ? tables_[table_].sort_column : -1; }

bool Ui::table_sort_descending() const { return table_ < tables_.size() && tables_[table_].descending; }

// ---- charts ------------------------------------------------------------------------------

void Ui::line_chart(const UiSeries* series, usize series_count, float width, float height, float y_min, float y_max) {
    if (!series) series_count = 0;
    bool named = false;
    usize longest = 0;
    for (usize k = 0; k < series_count; ++k) {
        named = named || !series[k].name.empty();
        if (series[k].values) longest = std::max(longest, series[k].count);
    }

    // The value range: the caller's, or what the data spans.
    float lo = y_min, hi = y_max;
    if (!(y_max > y_min)) {
        bool any = false;
        for (usize k = 0; k < series_count; ++k) {
            if (!series[k].values) continue;
            for (usize i = 0; i < series[k].count; ++i) {
                const float v = series[k].values[i];
                if (!std::isfinite(v)) continue;
                lo = any ? std::min(lo, v) : v;
                hi = any ? std::max(hi, v) : v;
                any = true;
            }
        }
        if (!any) {
            lo = 0;
            hi = 1;
        } else if (!(hi > lo)) {
            // A flat line: give it the room between itself and zero, twice.
            if (hi > 0) {
                lo = 0;
                hi *= 2;
            } else if (hi < 0) {
                lo *= 2;
                hi = 0;
            } else {
                hi = 1;
            }
        }
    }
    const float range = hi - lo;
    const int decimals = range >= 20 ? 0 : (range >= 2 ? 1 : 2);
    const int grid_lines = 4;

    const float text_h = line_height();
    const float legend = named ? text_h + style_.spacing : 0.0f;
    Rect r;
    place(width > 0 ? width : avail_width(), height + legend, r);
    bool hovered = false;
    behaviour(next_id("line_chart"), r, &hovered);

    // Value labels down the left, the plot beside them, half a text line
    // short at the top and bottom so the outer labels stay inside.
    float label_w = 0;
    for (int g = 0; g < grid_lines; ++g) {
        const float t = static_cast<float>(g) / static_cast<float>(grid_lines - 1);
        label_w = std::max(label_w, measure(format_number(lerpf(lo, hi, t), decimals)).x);
    }
    const Rect plot{r.x + label_w + style_.spacing, r.y + text_h * 0.5f, std::max(r.w - label_w - style_.spacing, 1.0f),
                    std::max(height - text_h, 1.0f)};
    auto x_of = [&](usize i, usize count) {
        return plot.x + (count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f) * plot.w;
    };
    auto y_of = [&](float v) {
        if (!std::isfinite(v)) v = lo;
        return plot.bottom() - clampf((v - lo) / range, 0, 1) * plot.h;
    };

    if (drawing()) {
        renderer_->fill_rect(plot, style_.field);
        for (int g = 0; g < grid_lines; ++g) {
            const float t = static_cast<float>(g) / static_cast<float>(grid_lines - 1);
            const float y = std::floor(plot.bottom() - t * plot.h);
            if (g > 0 && g < grid_lines - 1) renderer_->draw_line(plot.x, y, plot.right(), y, style_.grid);
            const std::string value = format_number(lerpf(lo, hi, t), decimals);
            text({plot.x - style_.spacing - measure(value).x, y - text_h * 0.5f}, value, style_.text_dim);
        }
        renderer_->draw_rect(plot, style_.button_border);

        std::vector<Vec2> points;
        for (usize k = 0; k < series_count; ++k) {
            const UiSeries& line = series[k];
            if (!line.values || line.count == 0) continue;
            if (line.count == 1) {
                renderer_->fill_circle(plot.x, y_of(line.values[0]), 2.5f, line.color, 10);
                continue;
            }
            // More samples than pixels: every n-th is enough, and always the last.
            const usize stride = std::max<usize>(1, line.count / static_cast<usize>(std::max(plot.w * 2, 1.0f)));
            points.clear();
            for (usize i = 0; i < line.count; i += stride) points.push_back({x_of(i, line.count), y_of(line.values[i])});
            if ((line.count - 1) % stride != 0)
                points.push_back({x_of(line.count - 1, line.count), y_of(line.values[line.count - 1])});
            StrokeStyle stroke;
            stroke.width = 1.5f;
            stroke.join = LineJoin::Bevel;
            stroke.r = line.color.r / 255.0f;
            stroke.g = line.color.g / 255.0f;
            stroke.b = line.color.b / 255.0f;
            stroke.a = line.color.a / 255.0f;
            renderer_->draw_polyline(points, stroke);
        }

        if (named) {
            const float box = std::floor(text_h * 0.6f);
            const float y = r.bottom() - text_h;
            float x = plot.x;
            for (usize k = 0; k < series_count; ++k) {
                if (series[k].name.empty()) continue;
                renderer_->fill_rect(x, y + (text_h - box) * 0.5f, box, box, series[k].color);
                text({x + box + 4, y}, series[k].name, style_.text_dim);
                x += box + 4 + measure(series[k].name).x + style_.spacing * 2;
            }
        }
    }

    // Under the mouse: a marker on the nearest sample and every series' value there.
    if (hovered && longest > 0 && in_.enabled && plot.contains(in_.mouse.x, in_.mouse.y)) {
        const float t = clampf((in_.mouse.x - plot.x) / plot.w, 0, 1);
        const usize sample = longest > 1 ? static_cast<usize>(t * static_cast<float>(longest - 1) + 0.5f) : 0;
        const float snapped = longest > 1 ? static_cast<float>(sample) / static_cast<float>(longest - 1) : 0.0f;
        if (drawing()) {
            const float x = std::floor(x_of(sample, longest));
            renderer_->draw_line(x, plot.y, x, plot.bottom(), style_.text_dim);
        }
        tooltip_.clear();
        for (usize k = 0; k < series_count; ++k) {
            const UiSeries& line = series[k];
            if (!line.values || line.count == 0) continue;
            const usize i = std::min(line.count - 1, static_cast<usize>(snapped * static_cast<float>(line.count - 1) + 0.5f));
            if (drawing()) renderer_->fill_circle(x_of(i, line.count), y_of(line.values[i]), 3, line.color, 10);
            if (!tooltip_.empty()) tooltip_ += '\n';
            if (!line.name.empty()) {
                tooltip_ += std::string(line.name);
                tooltip_ += ": ";
            }
            tooltip_ += format_number(line.values[i], decimals + 1);
        }
        tooltip_at_ = in_.mouse;
    }
}

} // namespace rw
