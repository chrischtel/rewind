#include "rewind/render/ui.h"

#include "rewind/render/font.h"
#include "rewind/render/mesh.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {
constexpr int kPixelCharW = 6;
constexpr int kPixelCharH = 8;

u32 hash_label(std::string_view s, u32 salt) {
    u32 h = 2166136261u ^ salt;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= 16777619u;
    }
    return h ? h : 1;
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
    if (renderer_) {
        // Screen space even under the editor's camera override.
        saved_camera_ = renderer_->camera();
        saved_override_ = renderer_->camera_overridden();
        static const Camera identity{};
        renderer_->set_camera_override(&identity);
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
}

void Ui::end() {
    while (!panels_.empty()) end_panel();
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
    if (!renderer_) return;
    if (font_) renderer_->draw_text(*font_, std::floor(at.x), std::floor(at.y), s, c, 1.0f, false);
    else renderer_->draw_text(std::floor(at.x), std::floor(at.y), std::string(s).c_str(), c, style_.pixel_font_scale);
}

// ---- drawing -----------------------------------------------------------------------------

void Ui::fill_rounded(const Rect& r, Color c, const Texture* skin, float skin_border) {
    if (!renderer_) return;
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
    if (!renderer_) return;
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

u32 Ui::next_id(std::string_view label) { return hash_label(label, ++id_counter_); }

bool Ui::behaviour(u32 id, const Rect& rect, bool* hovered) {
    last_id_ = id;
    const bool over = in_.enabled && rect.contains(in_.mouse.x, in_.mouse.y);
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

// ---- widgets -----------------------------------------------------------------------------

void Ui::label(std::string_view s, bool dim) {
    const Vec2 size = measure(s);
    Rect r;
    place(size.x, std::max(size.y, style_.item_height), r);
    last_id_ = 0;
    text({r.x, r.y + (r.h - size.y) * 0.5f}, s, dim ? style_.text_dim : style_.text);
}

bool Ui::button_at(const Rect& rect, std::string_view s) {
    const u32 id = next_id(s);
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

void Ui::tooltip(std::string_view s) {
    if (hovered_last()) {
        tooltip_ = std::string(s);
        tooltip_at_ = in_.mouse;
    }
}

} // namespace rw
