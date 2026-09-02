#include "rewind/debug/inspector.h"

#include <imgui.h>

#include <cstring>

namespace rw {

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

template <class T> void InspectorVisitor::scalar(std::string_view n, T& v, const FieldMeta& m, int imgui_type) {
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
    const std::string id = label(n);
    ImGui::BeginDisabled(m.read_only);
    bool copy = v;
    const bool edited = ImGui::Checkbox(id.c_str(), &copy);
    ImGui::EndDisabled();
    if (edited && !m.read_only) v = copy;
    after_edit(edited, m);
}

void InspectorVisitor::visit(std::string_view n, i8& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_S8); }
void InspectorVisitor::visit(std::string_view n, i16& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_S16); }
void InspectorVisitor::visit(std::string_view n, i32& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_S32); }
void InspectorVisitor::visit(std::string_view n, i64& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_S64); }
void InspectorVisitor::visit(std::string_view n, u8& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_U8); }
void InspectorVisitor::visit(std::string_view n, u16& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_U16); }
void InspectorVisitor::visit(std::string_view n, u32& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_U32); }
void InspectorVisitor::visit(std::string_view n, u64& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_U64); }
void InspectorVisitor::visit(std::string_view n, float& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_Float); }
void InspectorVisitor::visit(std::string_view n, double& v, const FieldMeta& m) { scalar(n, v, m, ImGuiDataType_Double); }

void InspectorVisitor::visit(std::string_view n, std::string& v, const FieldMeta& m) {
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
    const std::string id = label(n);
    const bool open = ImGui::TreeNodeEx(id.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    if (m.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tooltip);
    return open;
}

void InspectorVisitor::end_struct() { ImGui::TreePop(); }

bool InspectorVisitor::begin_array(std::string_view n, usize& count, const FieldMeta& m) {
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
