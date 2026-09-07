#pragma once

#include "rewind/sim/reflect.h"
#include "rewind/sim/world.h"

#include <optional>
#include <string>
#include <vector>

namespace rw {

/// Draws reflected state as editable Dear ImGui widgets. Must run between
/// ImGui::NewFrame() and ImGui::Render(). `changed()` tells whether the user
/// edited anything this frame, so the caller can re-snapshot the timeline.
///
/// Structs that announce a Widget hint draw as one control: a color picker,
/// a two- or four-float row, or an entity link with a select button. Integer
/// fields with names draw as dropdowns.
class InspectorVisitor final : public Visitor {
public:
    /// Arrays longer than this show only their first elements.
    explicit InspectorVisitor(usize max_elements = 64) : max_elements_(max_elements) {}

    bool changed() const { return changed_; }

    /// An entity link's select button was clicked somewhere this frame.
    /// Tools poll this once per frame; it is consumed on read.
    static std::optional<Entity> take_link_request();

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
    void hint(Widget widget) override;

private:
    template <class T> void scalar(std::string_view n, T& v, const FieldMeta& m, int imgui_type);
    template <class T> bool named(std::string_view n, T& v, const FieldMeta& m);
    std::string label(std::string_view n);
    void after_edit(bool edited, const FieldMeta& m);
    bool prepare();
    bool collecting() const { return !frames_.empty() && frames_.back().widget != Widget::Default; }
    struct Frame;
    bool path_widget(Frame& f);

    /// A struct being drawn: either a tree node opened lazily on its first
    /// field, or a combined widget collecting its fields' addresses.
    struct Frame {
        Widget widget = Widget::Default;
        std::string label;
        FieldMeta meta;
        bool parent_open = true;
        bool opened = false;
        bool open = false;
        std::vector<u8*> bytes;
        std::vector<float*> floats;
        std::vector<u32*> ints;
        std::vector<bool*> bools;
        std::vector<std::string*> strings;
    };
    bool asset_widget(Frame& f);
    bool def_widget(Frame& f);

    usize max_elements_;
    bool changed_ = false;
    std::vector<usize> index_stack_;
    std::vector<Frame> frames_;
    std::vector<bool> array_collecting_; ///< per open array: entered while collecting (no tree node)
    int id_counter_ = 0;
};

} // namespace rw
