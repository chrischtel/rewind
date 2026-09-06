#include "rewind/debug/inspector.h"

#include <imgui.h>

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
    case Widget::Default: break;
    }
    ImGui::EndDisabled();
    after_edit(edited, m);
}

bool InspectorVisitor::begin_array(std::string_view n, usize& count, const FieldMeta& m) {
    if (collecting() || !prepare()) return false;
    std::string id = label(n);
    const usize pos = id.find("##");
    const std::string shown = id.substr(0, pos) + " [" + std::to_string(count) + "]";
    id = shown + id.substr(pos);
    const bool open = ImGui::TreeNodeEx(id.c_str());
    if (m.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tooltip);
    if (open && count > max_elements_) ImGui::TextDisabled("showing first %zu of %zu", max_elements_, count);
    return open;
}

void InspectorVisitor::end_array() { ImGui::TreePop(); }

bool InspectorVisitor::begin_element(usize index) {
    if (index >= max_elements_) return false;
    index_stack_.push_back(index);
    return true;
}

void InspectorVisitor::end_element() { index_stack_.pop_back(); }

} // namespace rw
