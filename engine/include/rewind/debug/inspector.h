#pragma once

#include "rewind/sim/reflect.h"

#include <string>
#include <vector>

namespace rw {

/// Draws reflected state as editable Dear ImGui widgets. Must run between
/// ImGui::NewFrame() and ImGui::Render(). `changed()` tells whether the user
/// edited anything this frame, so the caller can re-snapshot the timeline.
class InspectorVisitor final : public Visitor {
public:
    /// Arrays longer than this show only their first elements.
    explicit InspectorVisitor(usize max_elements = 64) : max_elements_(max_elements) {}

    bool changed() const { return changed_; }

    void visit(std::string_view n, bool& v, const FieldMeta& m) override;
    void visit(std::string_view n, i8& v, const FieldMeta& m) override;
    void visit(std::string_view n, i16& v, const FieldMeta& m) override;
    void visit(std::string_view n, i32& v, const FieldMeta& m) override;
    void visit(std::string_view n, i64& v, const FieldMeta& m) override;
    void visit(std::string_view n, u8& v, const FieldMeta& m) override;
    void visit(std::string_view n, u16& v, const FieldMeta& m) override;
    void visit(std::string_view n, u32& v, const FieldMeta& m) override;
    void visit(std::string_view n, u64& v, const FieldMeta& m) override;
    void visit(std::string_view n, float& v, const FieldMeta& m) override;
    void visit(std::string_view n, double& v, const FieldMeta& m) override;
    void visit(std::string_view n, std::string& v, const FieldMeta& m) override;
    bool begin_struct(std::string_view n, const FieldMeta& m) override;
    void end_struct() override;
    bool begin_array(std::string_view n, usize& count, const FieldMeta& m) override;
    void end_array() override;
    bool begin_element(usize index) override;
    void end_element() override;

private:
    template <class T> void scalar(std::string_view n, T& v, const FieldMeta& m, int imgui_type);
    std::string label(std::string_view n);
    void after_edit(bool edited, const FieldMeta& m);

    usize max_elements_;
    bool changed_ = false;
    std::vector<usize> index_stack_;
    int id_counter_ = 0;
};

} // namespace rw
