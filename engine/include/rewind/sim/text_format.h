#pragma once

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rw {

/// Minimal JSON document. Numbers keep their source text so 64-bit integers
/// round-trip exactly.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text; ///< number text or string contents
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    const JsonValue* find(std::string_view key) const;
};

/// Parse JSON text. On failure `error` names the line and column.
bool parse_json(std::string_view text, JsonValue& out, std::string& error);

/// Writes reflected state as pretty JSON, field names included, so files stay
/// readable and diffable and survive fields being added, removed or reordered.
class JsonWriteVisitor final : public Visitor {
public:
    explicit JsonWriteVisitor(bool include_transient = false);

    /// Closes the root object and returns the document.
    std::string finish();

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
    bool begin_element(usize) override { return true; }
    void end_element() override {}

private:
    struct Frame {
        bool is_array;
        bool first = true;
    };
    void key(std::string_view name);
    void raw(std::string_view value, std::string_view name, const FieldMeta& m);
    void newline();

    std::string out_;
    std::vector<Frame> frames_;
    bool include_transient_;
};

/// Reads reflected state from a parsed JSON document by field name. Missing
/// fields keep their current values, unknown fields are ignored, and type
/// mismatches are counted in warnings().
class JsonLoadVisitor final : public Visitor {
public:
    explicit JsonLoadVisitor(const JsonValue& root);

    int warnings() const { return warnings_; }

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
    const JsonValue* resolve(std::string_view name) const;
    template <class T> void integer(std::string_view n, T& v);
    template <class T> void real(std::string_view n, T& v);

    std::vector<const JsonValue*> stack_;
    int warnings_ = 0;
};

template <Reflectable T> std::string to_json(T& object, bool include_transient = false) {
    JsonWriteVisitor w(include_transient);
    object.reflect(w);
    return w.finish();
}

template <Reflectable T> bool from_json(std::string_view text, T& object, std::string* error = nullptr) {
    JsonValue doc;
    std::string err;
    if (!parse_json(text, doc, err)) {
        if (error) *error = err;
        return false;
    }
    JsonLoadVisitor v(doc);
    object.reflect(v);
    return true;
}

bool read_text_file(const std::string& path, std::string& out, std::string* error = nullptr);
bool write_text_file(const std::string& path, std::string_view text, std::string* error = nullptr);

template <Reflectable T> bool save_json_file(const std::string& path, T& object, std::string* error = nullptr) {
    return write_text_file(path, to_json(object), error);
}

template <Reflectable T> bool load_json_file(const std::string& path, T& object, std::string* error = nullptr) {
    std::string text;
    if (!read_text_file(path, text, error)) return false;
    return from_json(text, object, error);
}

} // namespace rw
