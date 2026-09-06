#include "rewind/debug/inspector.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace rw {

namespace {
std::optional<Entity> g_link_request;
}

std::optional<Entity> InspectorVisitor::take_link_request() {
    std::optional<Entity> r = g_link_request;
    g_link_request.reset();
    return r;
}

std::string InspectorVisitor::label(std::string_view n) {
    std::string text;
    if (n.empty() && !index_stack_.empty()) {
        text = "[" + std::to_string(index_stack_.back()) + "]";
    } else {
        text.assign(n.data(), n.size());
    }
    // Unique ImGui id even when names repeat inside arrays.
    text += "##f" + std::to_string(id_counter_++);
    return text;
}

void InspectorVisitor::after_edit(bool edited, const FieldMeta& m) {
    if (edited && !m.read_only) changed_ = true;
    if (m.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tooltip);
}

// A plain struct becomes a tree node the moment its first field arrives; by
// then a hint() would already have turned it into a combined widget instead.
bool InspectorVisitor::prepare() {
    if (frames_.empty()) return true;
    Frame& f = frames_.back();
    if (f.widget != Widget::Default) return false; // collecting, handled by the caller
    if (!f.opened) {
        f.opened = true;
        f.open = f.parent_open ? ImGui::TreeNodeEx(f.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen) : false;
        if (f.open && f.meta.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.meta.tooltip);
    }
    return f.open;
}

template <class T> bool InspectorVisitor::named(std::string_view n, T& v, const FieldMeta& m) {
    if (!m.names || m.name_count == 0) return false;
    const std::string id = label(n);
    int current = static_cast<int>(v);
    if (current < 0 || current >= static_cast<int>(m.name_count)) current = 0;
    ImGui::BeginDisabled(m.read_only);
    const bool edited = ImGui::Combo(id.c_str(), &current, m.names, static_cast<int>(m.name_count));
    ImGui::EndDisabled();
    if (edited && !m.read_only) v = static_cast<T>(current);
    after_edit(edited, m);
    return true;
}

template <class T> void InspectorVisitor::scalar(std::string_view n, T& v, const FieldMeta& m, int imgui_type) {
    if (!prepare()) return;
    if (named(n, v, m)) return;
    const std::string id = label(n);
    ImGui::BeginDisabled(m.read_only);
    T copy = v;
    bool edited;
    if (m.min != 0 || m.max != 0) {
        T lo = static_cast<T>(m.min), hi = static_cast<T>(m.max);
        edited = ImGui::SliderScalar(id.c_str(), imgui_type, &copy, &lo, &hi);
    } else {
        edited = ImGui::DragScalar(id.c_str(), imgui_type, &copy, m.speed);
    }
    ImGui::EndDisabled();
    if (edited && !m.read_only) v = copy;
    if (m.transient) {
        ImGui::SameLine();
        ImGui::TextDisabled("(transient)");
    }
    after_edit(edited, m);
}

void InspectorVisitor::visit(std::string_view n, bool& v, const FieldMeta& m) {
    if (collecting()) {
        frames_.back().bools.push_back(&v);
        return;
    }
    if (!prepare()) return;
    const std::string id = label(n);
    ImGui::BeginDisabled(m.read_only);
    bool copy = v;
    const bool edited = ImGui::Checkbox(id.c_str(), &copy);
    ImGui::EndDisabled();
    if (edited && !m.read_only) v = copy;
    after_edit(edited, m);
}

// Fields of a combined widget are recorded, not drawn.
void InspectorVisitor::visit(std::string_view n, u8& v, const FieldMeta& m) {
    if (collecting()) {
        frames_.back().bytes.push_back(&v);
        return;
    }
    scalar(n, v, m, ImGuiDataType_U8);
}
void InspectorVisitor::visit(std::string_view n, u32& v, const FieldMeta& m) {
    if (collecting()) {
        frames_.back().ints.push_back(&v);
        return;
    }
    scalar(n, v, m, ImGuiDataType_U32);
}
void InspectorVisitor::visit(std::string_view n, float& v, const FieldMeta& m) {
    if (collecting()) {
        frames_.back().floats.push_back(&v);
        return;
    }
    scalar(n, v, m, ImGuiDataType_Float);
}
void InspectorVisitor::visit(std::string_view n, i8& v, const FieldMeta& m) { if (!collecting()) scalar(n, v, m, ImGuiDataType_S8); }
void InspectorVisitor::visit(std::string_view n, i16& v, const FieldMeta& m) { if (!collecting()) scalar(n, v, m, ImGuiDataType_S16); }
void InspectorVisitor::visit(std::string_view n, i32& v, const FieldMeta& m) { if (!collecting()) scalar(n, v, m, ImGuiDataType_S32); }
void InspectorVisitor::visit(std::string_view n, i64& v, const FieldMeta& m) { if (!collecting()) scalar(n, v, m, ImGuiDataType_S64); }
void InspectorVisitor::visit(std::string_view n, u16& v, const FieldMeta& m) { if (!collecting()) scalar(n, v, m, ImGuiDataType_U16); }
void InspectorVisitor::visit(std::string_view n, u64& v, const FieldMeta& m) { if (!collecting()) scalar(n, v, m, ImGuiDataType_U64); }
void InspectorVisitor::visit(std::string_view n, double& v, const FieldMeta& m) { if (!collecting()) scalar(n, v, m, ImGuiDataType_Double); }

void InspectorVisitor::visit(std::string_view n, std::string& v, const FieldMeta& m) {
    if (collecting() || !prepare()) return;
    const std::string id = label(n);
    char buffer[256];
    std::strncpy(buffer, v.c_str(), sizeof buffer - 1);
    buffer[sizeof buffer - 1] = '\0';
    ImGui::BeginDisabled(m.read_only);
    const bool edited = ImGui::InputText(id.c_str(), buffer, sizeof buffer);
    ImGui::EndDisabled();
    if (edited && !m.read_only) v = buffer;
    after_edit(edited, m);
}

bool InspectorVisitor::begin_struct(std::string_view n, const FieldMeta& m) {
    Frame f;
    f.meta = m;
    if (collecting()) {
        f.widget = frames_.back().widget; // nested inside a combined widget: keep collecting
        f.label = std::string(n);
        frames_.push_back(std::move(f));
        return true;
    }
    // The parent must be open for anything below to draw.
    f.parent_open = prepare();
    f.label = label(n);
    frames_.push_back(std::move(f));
    return true;
}

void InspectorVisitor::hint(Widget widget) {
    if (frames_.empty() || widget == Widget::Default) return;
    Frame& f = frames_.back();
    if (f.widget != Widget::Default || f.opened) return;
    f.widget = widget;
}

void InspectorVisitor::end_struct() {
    if (frames_.empty()) return;
    Frame f = std::move(frames_.back());
    frames_.pop_back();

    if (!frames_.empty() && frames_.back().widget != Widget::Default) {
        // Nested collection: hand the gathered fields up.
        Frame& parent = frames_.back();
        parent.bytes.insert(parent.bytes.end(), f.bytes.begin(), f.bytes.end());
        parent.floats.insert(parent.floats.end(), f.floats.begin(), f.floats.end());
        parent.ints.insert(parent.ints.end(), f.ints.begin(), f.ints.end());
        parent.bools.insert(parent.bools.end(), f.bools.begin(), f.bools.end());
        return;
    }
    if (f.widget == Widget::Default) {
        if (f.opened && f.open) ImGui::TreePop();
        return;
    }
    if (!f.parent_open) return;

    const std::string& id = f.label;
    const FieldMeta& m = f.meta;
    ImGui::BeginDisabled(m.read_only);
    bool edited = false;
    switch (f.widget) {
    case Widget::Color:
        if (f.bytes.size() >= 4) {
            float c[4] = {*f.bytes[0] / 255.0f, *f.bytes[1] / 255.0f, *f.bytes[2] / 255.0f, *f.bytes[3] / 255.0f};
            edited = ImGui::ColorEdit4(id.c_str(), c, ImGuiColorEditFlags_AlphaBar);
            if (edited && !m.read_only)
                for (int i = 0; i < 4; ++i) *f.bytes[i] = static_cast<u8>(c[i] * 255.0f + 0.5f);
        }
        break;
    case Widget::Vec2:
        if (f.floats.size() >= 2) {
            float v[2] = {*f.floats[0], *f.floats[1]};
            edited = ImGui::DragFloat2(id.c_str(), v, m.speed);
            if (edited && !m.read_only) {
                *f.floats[0] = v[0];
                *f.floats[1] = v[1];
            }
        }
        break;
    case Widget::Rect:
        if (f.floats.size() >= 4) {
            float v[4] = {*f.floats[0], *f.floats[1], *f.floats[2], *f.floats[3]};
            edited = ImGui::DragFloat4(id.c_str(), v, m.speed);
            if (edited && !m.read_only)
                for (int i = 0; i < 4; ++i) *f.floats[i] = v[i];
        }
        break;
    case Widget::Entity:
        if (f.ints.size() >= 2) {
            const Entity e{*f.ints[0], *f.ints[1]};
            const std::string name = id.substr(0, id.find("##"));
            if (e.is_null()) {
                ImGui::TextDisabled("%s: none", name.c_str());
            } else {
                ImGui::Text("%s: entity #%u (gen %u)", name.c_str(), e.index, e.generation);
                ImGui::SameLine();
                if (ImGui::SmallButton(("select" + id).c_str())) g_link_request = e;
            }
        }
        break;
    case Widget::Path: edited = path_widget(f); break;
    case Widget::Default: break;
    }
    ImGui::EndDisabled();
    after_edit(edited, m);
}

// A path: preview of the curve, closed flag, and the control points as rows.
bool InspectorVisitor::path_widget(Frame& f) {
    const usize segs = f.floats.size() / 8;
    const std::string name = f.label.substr(0, f.label.find("##"));
    bool edited = false;
    ImGui::PushID(f.label.c_str());
    ImGui::Text("%s: %zu segment%s", name.c_str(), segs, segs == 1 ? "" : "s");
    if (!f.bools.empty()) {
        ImGui::SameLine();
        bool closed = *f.bools[0];
        if (ImGui::Checkbox("closed", &closed) && !f.meta.read_only) {
            *f.bools[0] = closed;
            edited = true;
        }
    }
    if (segs > 0) {
        // Preview: fit every control point into a strip.
        auto pt = [&](usize i) { return ImVec2(*f.floats[2 * i], *f.floats[2 * i + 1]); };
        float min_x = pt(0).x, max_x = min_x, min_y = pt(0).y, max_y = min_y;
        for (usize i = 1; i < segs * 4; ++i) {
            min_x = std::min(min_x, pt(i).x); max_x = std::max(max_x, pt(i).x);
            min_y = std::min(min_y, pt(i).y); max_y = std::max(max_y, pt(i).y);
        }
        const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 60.0f), 90.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##preview", size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 60));
        const float pad = 8;
        const float sx = (max_x - min_x) > 1e-6f ? (size.x - 2 * pad) / (max_x - min_x) : 1;
        const float sy = (max_y - min_y) > 1e-6f ? (size.y - 2 * pad) / (max_y - min_y) : 1;
        const float s = std::min(sx, sy);
        const float ox = origin.x + pad + ((size.x - 2 * pad) - (max_x - min_x) * s) * 0.5f;
        const float oy = origin.y + pad + ((size.y - 2 * pad) - (max_y - min_y) * s) * 0.5f;
        auto map = [&](ImVec2 p) { return ImVec2(ox + (p.x - min_x) * s, oy + (p.y - min_y) * s); };
        for (usize sg = 0; sg < segs; ++sg) {
            const ImVec2 p0 = pt(sg * 4), p1 = pt(sg * 4 + 1), p2 = pt(sg * 4 + 2), p3 = pt(sg * 4 + 3);
            dl->AddBezierCubic(map(p0), map(p1), map(p2), map(p3), IM_COL32(120, 200, 255, 255), 1.5f, 12);
            dl->AddCircleFilled(map(p0), 2.5f, IM_COL32(255, 255, 255, 255));
            if (sg + 1 == segs) dl->AddCircleFilled(map(p3), 2.5f, IM_COL32(255, 255, 255, 255));
        }
        if (ImGui::TreeNode("points")) {
            for (usize sg = 0; sg < segs; ++sg) {
                ImGui::PushID(static_cast<int>(sg));
                const char* names[4] = {"anchor", "out", "in", "end"};
                for (usize k = 0; k < 4; ++k) {
                    if (k == 3 && sg + 1 < segs) continue; // shared with the next anchor
                    float v[2] = {*f.floats[(sg * 4 + k) * 2], *f.floats[(sg * 4 + k) * 2 + 1]};
                    if (ImGui::DragFloat2(names[k], v, f.meta.speed) && !f.meta.read_only) {
                        *f.floats[(sg * 4 + k) * 2] = v[0];
                        *f.floats[(sg * 4 + k) * 2 + 1] = v[1];
                        if (k == 0 && sg > 0) { // keep the previous segment's end on the anchor
                            *f.floats[((sg - 1) * 4 + 3) * 2] = v[0];
                            *f.floats[((sg - 1) * 4 + 3) * 2 + 1] = v[1];
                        }
                        edited = true;
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
    ImGui::PopID();
    return edited;
}

bool InspectorVisitor::begin_array(std::string_view n, usize& count, const FieldMeta& m) {
    if (collecting()) {
        array_collecting_.push_back(true);
        return true; // nested structs keep collecting (a Path's segments)
    }
    if (!prepare()) return false;
    array_collecting_.push_back(false);
    std::string id = label(n);
    const usize pos = id.find("##");
    const std::string shown = id.substr(0, pos) + " [" + std::to_string(count) + "]";
    id = shown + id.substr(pos);
    const bool open = ImGui::TreeNodeEx(id.c_str());
    if (m.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tooltip);
    if (open && count > max_elements_) ImGui::TextDisabled("showing first %zu of %zu", max_elements_, count);
    if (!open) array_collecting_.pop_back(); // end_array() is not called for closed arrays
    return open;
}

void InspectorVisitor::end_array() {
    const bool collecting_array = !array_collecting_.empty() && array_collecting_.back();
    if (!array_collecting_.empty()) array_collecting_.pop_back();
    if (!collecting_array) ImGui::TreePop();
}

bool InspectorVisitor::begin_element(usize index) {
    if (index >= max_elements_) return false;
    index_stack_.push_back(index);
    return true;
}

void InspectorVisitor::end_element() { index_stack_.pop_back(); }

} // namespace rw
