#include "rewind/sim/reflect.h"

#include "rewind/sim/serialize.h"

#include <cstdio>
#include <cstring>

namespace rw {

// ---- SaveVisitor ----------------------------------------------------------------

#define RW_SAVE_POD(T)                                                                             \
    void SaveVisitor::visit(std::string_view, T& v, const FieldMeta& m) {                          \
        if (!m.transient) out_.pod(v);                                                             \
    }
RW_SAVE_POD(bool)
RW_SAVE_POD(i8)
RW_SAVE_POD(i16)
RW_SAVE_POD(i32)
RW_SAVE_POD(i64)
RW_SAVE_POD(u8)
RW_SAVE_POD(u16)
RW_SAVE_POD(u32)
RW_SAVE_POD(u64)
RW_SAVE_POD(float)
RW_SAVE_POD(double)
#undef RW_SAVE_POD

void SaveVisitor::visit(std::string_view, std::string& v, const FieldMeta& m) {
    if (!m.transient) out_.str(v);
}

bool SaveVisitor::begin_struct(std::string_view, const FieldMeta& m) { return !m.transient; }

bool SaveVisitor::begin_array(std::string_view, usize& count, const FieldMeta& m) {
    if (m.transient) return false;
    out_.pod<u64>(count);
    return true;
}

bool SaveVisitor::visit_bulk(void* data, usize count, usize element_size) {
    out_.append(data, count * element_size);
    return true;
}

// ---- LoadVisitor ----------------------------------------------------------------

#define RW_LOAD_POD(T)                                                                             \
    void LoadVisitor::visit(std::string_view, T& v, const FieldMeta& m) {                          \
        if (!m.transient) in_.pod(v);                                                              \
    }
RW_LOAD_POD(bool)
RW_LOAD_POD(i8)
RW_LOAD_POD(i16)
RW_LOAD_POD(i32)
RW_LOAD_POD(i64)
RW_LOAD_POD(u8)
RW_LOAD_POD(u16)
RW_LOAD_POD(u32)
RW_LOAD_POD(u64)
RW_LOAD_POD(float)
RW_LOAD_POD(double)
#undef RW_LOAD_POD

void LoadVisitor::visit(std::string_view, std::string& v, const FieldMeta& m) {
    if (!m.transient) in_.str(v);
}

bool LoadVisitor::begin_struct(std::string_view, const FieldMeta& m) { return !m.transient; }

bool LoadVisitor::begin_array(std::string_view, usize& count, const FieldMeta& m) {
    if (m.transient) return false;
    const u64 n = in_.pod<u64>();
    // Every element takes at least one byte, so a count beyond what is left
    // can only come from a corrupt stream. Refuse it instead of allocating.
    if (!in_.ok() || n > in_.remaining()) {
        count = 0;
        return true;
    }
    count = static_cast<usize>(n);
    return true;
}

bool LoadVisitor::visit_bulk(void* data, usize count, usize element_size) {
    in_.bytes(data, count * element_size);
    return true;
}

// ---- DumpVisitor ----------------------------------------------------------------

std::string DumpVisitor::label(std::string_view name) const {
    if (in_element_ && name.empty()) return "[" + std::to_string(current_index_) + "]";
    return std::string(name);
}

void DumpVisitor::line(std::string_view name, const std::string& value, const FieldMeta& m) {
    text_.append(static_cast<usize>(depth_) * 2, ' ');
    text_ += label(name);
    text_ += " = ";
    text_ += value;
    if (m.transient) text_ += "  (transient)";
    text_ += '\n';
}

void DumpVisitor::visit(std::string_view n, bool& v, const FieldMeta& m) { line(n, v ? "true" : "false", m); }
void DumpVisitor::visit(std::string_view n, i8& v, const FieldMeta& m) { line(n, std::to_string(v), m); }
void DumpVisitor::visit(std::string_view n, i16& v, const FieldMeta& m) { line(n, std::to_string(v), m); }
void DumpVisitor::visit(std::string_view n, i32& v, const FieldMeta& m) { line(n, std::to_string(v), m); }
void DumpVisitor::visit(std::string_view n, i64& v, const FieldMeta& m) { line(n, std::to_string(v), m); }
void DumpVisitor::visit(std::string_view n, u8& v, const FieldMeta& m) { line(n, std::to_string(v), m); }
void DumpVisitor::visit(std::string_view n, u16& v, const FieldMeta& m) { line(n, std::to_string(v), m); }
void DumpVisitor::visit(std::string_view n, u32& v, const FieldMeta& m) { line(n, std::to_string(v), m); }
void DumpVisitor::visit(std::string_view n, u64& v, const FieldMeta& m) { line(n, std::to_string(v), m); }

void DumpVisitor::visit(std::string_view n, float& v, const FieldMeta& m) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", static_cast<double>(v));
    line(n, buf, m);
}

void DumpVisitor::visit(std::string_view n, double& v, const FieldMeta& m) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    line(n, buf, m);
}

void DumpVisitor::visit(std::string_view n, std::string& v, const FieldMeta& m) { line(n, "\"" + v + "\"", m); }

bool DumpVisitor::begin_struct(std::string_view n, const FieldMeta& m) {
    text_.append(static_cast<usize>(depth_) * 2, ' ');
    text_ += label(n);
    text_ += m.transient ? " {  (transient)\n" : " {\n";
    ++depth_;
    return true;
}

void DumpVisitor::end_struct() {
    --depth_;
    text_.append(static_cast<usize>(depth_) * 2, ' ');
    text_ += "}\n";
}

bool DumpVisitor::begin_array(std::string_view n, usize& count, const FieldMeta& m) {
    text_.append(static_cast<usize>(depth_) * 2, ' ');
    text_ += label(n);
    text_ += " [" + std::to_string(count) + "]";
    text_ += m.transient ? "  (transient)\n" : "\n";
    ++depth_;
    if (count > max_elements_) {
        text_.append(static_cast<usize>(depth_) * 2, ' ');
        text_ += "... showing first " + std::to_string(max_elements_) + "\n";
    }
    return true;
}

void DumpVisitor::end_array() { --depth_; }

bool DumpVisitor::begin_element(usize index) {
    if (index >= max_elements_) return false;
    current_index_ = index;
    in_element_ = true;
    return true;
}

void DumpVisitor::end_element() { in_element_ = false; }

} // namespace rw
