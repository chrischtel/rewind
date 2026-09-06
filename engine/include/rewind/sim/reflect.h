#pragma once

#include "rewind/core/types.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rw {

class Visitor;

/// Hints for tools. Everything is optional.
struct FieldMeta {
    float min = 0;      ///< Inspector range; both 0 = unbounded.
    float max = 0;
    float speed = 1.0f; ///< Inspector drag speed.
    const char* tooltip = nullptr;
    bool transient = false; ///< Shown in the inspector but not saved or hashed.
    bool read_only = false;
    /// Names for an integer or enum field: value 0 = names[0], drawn as a
    /// dropdown. `{.names = kKinds, .name_count = 3}`.
    const char* const* names = nullptr;
    u32 name_count = 0;
};

/// How a struct wants to be shown. Engine types announce this themselves
/// at the start of their reflect(), so a Color is a color picker wherever
/// it appears and nobody has to annotate the field.
enum class Widget : u8 { Default, Color, Vec2, Rect, Entity, Path };

/// A type that can describe its fields: `void reflect(rw::Visitor&)`.
template <class T>
concept Reflectable = requires(T& t, Visitor& v) { t.reflect(v); };

template <class T>
concept Primitive = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

/// Walks a game's state. Implement the primitives and aggregates once and
/// every reflected type works with it. The engine ships visitors that save,
/// load, dump, and inspect; a game can add its own (network sync, undo).
///
/// A struct opts in with one member function:
///
///     void reflect(rw::Visitor& v) {
///         v.field("x", x);
///         v.field("balls", balls, {.tooltip = "one per launch"});
///     }
class Visitor {
public:
    virtual ~Visitor() = default;

    // ---- primitives ---------------------------------------------------------
    virtual void visit(std::string_view name, bool& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, i8& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, i16& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, i32& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, i64& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, u8& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, u16& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, u32& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, u64& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, float& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, double& v, const FieldMeta& m) = 0;
    virtual void visit(std::string_view name, std::string& v, const FieldMeta& m) = 0;

    // ---- aggregates ---------------------------------------------------------
    /// Return false to skip the contents. Only inspection-style visitors may
    /// do that; anything that reads or writes data must always descend.
    virtual bool begin_struct(std::string_view name, const FieldMeta& m) = 0;
    virtual void end_struct() = 0;

    /// `count` is in/out: a loading visitor sets it and the array is resized.
    virtual bool begin_array(std::string_view name, usize& count, const FieldMeta& m) = 0;
    virtual void end_array() = 0;
    virtual bool begin_element(usize index) = 0;
    virtual void end_element() = 0;

    /// Fast path for arrays of numbers: called once the array has its final
    /// size, with the raw bytes. Return true to skip the per-element visits.
    virtual bool visit_bulk(void* data, usize count, usize element_size) {
        (void)data;
        (void)count;
        (void)element_size;
        return false;
    }

    /// Called by a struct right after its begin_struct, to say how it wants
    /// to be drawn. Data visitors ignore it.
    virtual void hint(Widget) {}

    // ---- dispatch -----------------------------------------------------------
    template <class T> void field(std::string_view name, T& value, const FieldMeta& meta = {}) {
        if constexpr (std::is_same_v<T, bool> || Primitive<T> || std::is_same_v<T, std::string>) {
            visit(name, value, meta);
        } else if constexpr (std::is_enum_v<T>) {
            using U = std::underlying_type_t<T>;
            U raw = static_cast<U>(value);
            visit(name, raw, meta);
            value = static_cast<T>(raw);
        } else if constexpr (Reflectable<T>) {
            if (begin_struct(name, meta)) {
                value.reflect(*this);
                end_struct();
            }
        } else if constexpr (is_vector<T>::value) {
            visit_vector(name, value, meta);
        } else {
            static_assert(sizeof(T) == 0, "field(): type needs reflect(Visitor&), or be a number, bool, string, enum or vector");
        }
    }

private:
    template <class T> struct is_vector : std::false_type {};
    template <class T, class A> struct is_vector<std::vector<T, A>> : std::true_type {};

    template <class T> void visit_vector(std::string_view name, std::vector<T>& vec, const FieldMeta& meta) {
        usize count = vec.size();
        if (!begin_array(name, count, meta)) return;
        if (count != vec.size()) vec.resize(count);
        bool done = false;
        if constexpr (Primitive<T> || std::is_same_v<T, bool>) {
            if constexpr (!std::is_same_v<T, bool>) done = visit_bulk(vec.data(), count, sizeof(T));
        }
        if (!done) {
            for (usize i = 0; i < count; ++i) {
                if (!begin_element(i)) continue;
                if constexpr (std::is_same_v<T, bool>) {
                    bool b = vec[i]; // vector<bool> hands out proxies, not references
                    field("", b);
                    vec[i] = b;
                } else {
                    field("", vec[i]);
                }
                end_element();
            }
        }
        end_array();
    }
};

class Writer;
class Reader;

/// Writes every non-transient field into a Writer, in declaration order.
class SaveVisitor final : public Visitor {
public:
    explicit SaveVisitor(Writer& out) : out_(out) {}

    void visit(std::string_view, bool& v, const FieldMeta& m) override;
    void visit(std::string_view, i8& v, const FieldMeta& m) override;
    void visit(std::string_view, i16& v, const FieldMeta& m) override;
    void visit(std::string_view, i32& v, const FieldMeta& m) override;
    void visit(std::string_view, i64& v, const FieldMeta& m) override;
    void visit(std::string_view, u8& v, const FieldMeta& m) override;
    void visit(std::string_view, u16& v, const FieldMeta& m) override;
    void visit(std::string_view, u32& v, const FieldMeta& m) override;
    void visit(std::string_view, u64& v, const FieldMeta& m) override;
    void visit(std::string_view, float& v, const FieldMeta& m) override;
    void visit(std::string_view, double& v, const FieldMeta& m) override;
    void visit(std::string_view, std::string& v, const FieldMeta& m) override;
    bool begin_struct(std::string_view, const FieldMeta& m) override;
    void end_struct() override {}
    bool begin_array(std::string_view, usize& count, const FieldMeta& m) override;
    void end_array() override {}
    bool begin_element(usize) override { return true; }
    void end_element() override {}
    bool visit_bulk(void* data, usize count, usize element_size) override;

private:
    Writer& out_;
};

/// Reads fields back from a Reader, in the same order SaveVisitor wrote them.
class LoadVisitor final : public Visitor {
public:
    explicit LoadVisitor(Reader& in) : in_(in) {}

    void visit(std::string_view, bool& v, const FieldMeta& m) override;
    void visit(std::string_view, i8& v, const FieldMeta& m) override;
    void visit(std::string_view, i16& v, const FieldMeta& m) override;
    void visit(std::string_view, i32& v, const FieldMeta& m) override;
    void visit(std::string_view, i64& v, const FieldMeta& m) override;
    void visit(std::string_view, u8& v, const FieldMeta& m) override;
    void visit(std::string_view, u16& v, const FieldMeta& m) override;
    void visit(std::string_view, u32& v, const FieldMeta& m) override;
    void visit(std::string_view, u64& v, const FieldMeta& m) override;
    void visit(std::string_view, float& v, const FieldMeta& m) override;
    void visit(std::string_view, double& v, const FieldMeta& m) override;
    void visit(std::string_view, std::string& v, const FieldMeta& m) override;
    bool begin_struct(std::string_view, const FieldMeta& m) override;
    void end_struct() override {}
    bool begin_array(std::string_view, usize& count, const FieldMeta& m) override;
    void end_array() override {}
    bool begin_element(usize) override { return true; }
    void end_element() override {}
    bool visit_bulk(void* data, usize count, usize element_size) override;

private:
    Reader& in_;
};

/// Renders the state as indented text, one field per line. For logs, tests,
/// and the "copy state" button in the overlay.
class DumpVisitor final : public Visitor {
public:
    /// Arrays longer than this show only their first elements.
    explicit DumpVisitor(usize max_elements = 16) : max_elements_(max_elements) {}

    const std::string& text() const { return text_; }

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
    void line(std::string_view name, const std::string& value, const FieldMeta& m);
    std::string label(std::string_view name) const;

    std::string text_;
    int depth_ = 0;
    usize max_elements_;
    usize current_index_ = 0;
    bool in_element_ = false;
};

/// Convenience: save any reflectable object to bytes and back.
template <Reflectable T> void save_reflected(T& object, Writer& out) {
    SaveVisitor v(out);
    object.reflect(v);
}

template <Reflectable T> void load_reflected(T& object, Reader& in) {
    LoadVisitor v(in);
    object.reflect(v);
}

} // namespace rw
