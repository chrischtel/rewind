#include "rewind/sim/text_format.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rw {

// ---- document -------------------------------------------------------------------

const JsonValue* JsonValue::find(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [k, v] : members)
        if (k == key) return &v;
    return nullptr;
}

// ---- parser ---------------------------------------------------------------------

namespace {

struct JsonParser {
    std::string_view src;
    usize pos = 0;
    std::string& error;

    bool fail(const char* what) {
        int line = 1, col = 1;
        for (usize i = 0; i < pos && i < src.size(); ++i) {
            if (src[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        error = std::string(what) + " at line " + std::to_string(line) + ", column " + std::to_string(col);
        return false;
    }

    void skip_ws() {
        while (pos < src.size()) {
            const char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
                while (pos < src.size() && src[pos] != '\n') ++pos; // line comments are allowed
            } else {
                break;
            }
        }
    }

    bool peek(char c) {
        skip_ws();
        return pos < src.size() && src[pos] == c;
    }

    bool expect(char c, const char* what) {
        if (!peek(c)) return fail(what);
        ++pos;
        return true;
    }

    static void append_utf8(std::string& out, u32 cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_string(std::string& out) {
        if (!expect('"', "expected '\"'")) return false;
        out.clear();
        while (pos < src.size()) {
            const char c = src[pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= src.size()) break;
            const char e = src[pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (pos + 4 > src.size()) return fail("bad \\u escape");
                u32 cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = src[pos++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<u32>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<u32>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<u32>(h - 'A' + 10);
                    else return fail("bad \\u escape");
                }
                append_utf8(out, cp);
                break;
            }
            default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > 256) return fail("nesting too deep");
        skip_ws();
        if (pos >= src.size()) return fail("unexpected end of input");
        const char c = src[pos];

        if (c == '{') {
            ++pos;
            out.type = JsonValue::Type::Object;
            if (peek('}')) {
                ++pos;
                return true;
            }
            for (;;) {
                std::string key;
                skip_ws();
                if (!parse_string(key)) return false;
                if (!expect(':', "expected ':'")) return false;
                JsonValue value;
                if (!parse_value(value, depth + 1)) return false;
                out.members.emplace_back(std::move(key), std::move(value));
                if (peek(',')) {
                    ++pos;
                    if (peek('}')) { // trailing comma is tolerated
                        ++pos;
                        return true;
                    }
                    continue;
                }
                return expect('}', "expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++pos;
            out.type = JsonValue::Type::Array;
            if (peek(']')) {
                ++pos;
                return true;
            }
            for (;;) {
                JsonValue value;
                if (!parse_value(value, depth + 1)) return false;
                out.items.push_back(std::move(value));
                if (peek(',')) {
                    ++pos;
                    if (peek(']')) {
                        ++pos;
                        return true;
                    }
                    continue;
                }
                return expect(']', "expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parse_string(out.text);
        }
        if (src.substr(pos, 4) == "true") {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            pos += 4;
            return true;
        }
        if (src.substr(pos, 5) == "false") {
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            pos += 5;
            return true;
        }
        if (src.substr(pos, 4) == "null") {
            out.type = JsonValue::Type::Null;
            pos += 4;
            return true;
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9') || c == '.') {
            const usize start = pos;
            while (pos < src.size()) {
                const char d = src[pos];
                if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') ++pos;
                else break;
            }
            out.type = JsonValue::Type::Number;
            out.text = std::string(src.substr(start, pos - start));
            return true;
        }
        return fail("unexpected character");
    }
};

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

} // namespace

bool parse_json(std::string_view text, JsonValue& out, std::string& error) {
    out = JsonValue{};
    error.clear();
    JsonParser p{text, 0, error};
    if (!p.parse_value(out, 0)) return false;
    p.skip_ws();
    if (p.pos != text.size()) return p.fail("trailing characters");
    return true;
}

// ---- writer ---------------------------------------------------------------------

JsonWriteVisitor::JsonWriteVisitor(bool include_transient) : include_transient_(include_transient) {
    out_ += '{';
    frames_.push_back(Frame{false});
}

std::string JsonWriteVisitor::finish() {
    while (frames_.size() > 1) {
        const bool arr = frames_.back().is_array;
        frames_.pop_back();
        newline();
        out_ += arr ? ']' : '}';
    }
    if (!frames_.empty()) {
        frames_.pop_back();
        out_ += '\n';
        out_ += '}';
        out_ += '\n';
    }
    return std::move(out_);
}

void JsonWriteVisitor::newline() {
    out_ += '\n';
    out_.append(frames_.size() * 2, ' ');
}

void JsonWriteVisitor::key(std::string_view name) {
    Frame& f = frames_.back();
    if (!f.first) out_ += ',';
    f.first = false;
    newline();
    if (!f.is_array) {
        out_ += escape(name);
        out_ += ": ";
    }
}

void JsonWriteVisitor::raw(std::string_view value, std::string_view name, const FieldMeta& m) {
    if (m.transient && !include_transient_) return;
    key(name);
    out_ += value;
}

void JsonWriteVisitor::visit(std::string_view n, bool& v, const FieldMeta& m) { raw(v ? "true" : "false", n, m); }
void JsonWriteVisitor::visit(std::string_view n, i8& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }
void JsonWriteVisitor::visit(std::string_view n, i16& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }
void JsonWriteVisitor::visit(std::string_view n, i32& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }
void JsonWriteVisitor::visit(std::string_view n, i64& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }
void JsonWriteVisitor::visit(std::string_view n, u8& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }
void JsonWriteVisitor::visit(std::string_view n, u16& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }
void JsonWriteVisitor::visit(std::string_view n, u32& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }
void JsonWriteVisitor::visit(std::string_view n, u64& v, const FieldMeta& m) { raw(std::to_string(v), n, m); }

void JsonWriteVisitor::visit(std::string_view n, float& v, const FieldMeta& m) {
    char buf[64];
    if (std::isfinite(v)) std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
    else std::snprintf(buf, sizeof buf, "0");
    raw(buf, n, m);
}

void JsonWriteVisitor::visit(std::string_view n, double& v, const FieldMeta& m) {
    char buf[64];
    if (std::isfinite(v)) std::snprintf(buf, sizeof buf, "%.17g", v);
    else std::snprintf(buf, sizeof buf, "0");
    raw(buf, n, m);
}

void JsonWriteVisitor::visit(std::string_view n, std::string& v, const FieldMeta& m) { raw(escape(v), n, m); }

bool JsonWriteVisitor::begin_struct(std::string_view n, const FieldMeta& m) {
    if (m.transient && !include_transient_) return false;
    key(n);
    out_ += '{';
    frames_.push_back(Frame{false});
    return true;
}

void JsonWriteVisitor::end_struct() {
    const bool empty = frames_.back().first;
    frames_.pop_back();
    if (!empty) newline();
    out_ += '}';
}

bool JsonWriteVisitor::begin_array(std::string_view n, usize&, const FieldMeta& m) {
    if (m.transient && !include_transient_) return false;
    key(n);
    out_ += '[';
    frames_.push_back(Frame{true});
    return true;
}

void JsonWriteVisitor::end_array() {
    const bool empty = frames_.back().first;
    frames_.pop_back();
    if (!empty) newline();
    out_ += ']';
}

// ---- loader ---------------------------------------------------------------------

JsonLoadVisitor::JsonLoadVisitor(const JsonValue& root) { stack_.push_back(&root); }

const JsonValue* JsonLoadVisitor::resolve(std::string_view name) const {
    const JsonValue* top = stack_.back();
    if (!top) return nullptr;
    if (name.empty()) return top;
    return top->find(name);
}

template <class T> void JsonLoadVisitor::integer(std::string_view n, T& v) {
    const JsonValue* j = resolve(n);
    if (!j) return;
    if (j->type == JsonValue::Type::Number) {
        errno = 0;
        if constexpr (std::is_signed_v<T>) {
            const long long parsed = std::strtoll(j->text.c_str(), nullptr, 10);
            v = static_cast<T>(parsed);
        } else {
            const unsigned long long parsed = std::strtoull(j->text.c_str(), nullptr, 10);
            v = static_cast<T>(parsed);
        }
        if (j->text.find_first_of(".eE") != std::string::npos) v = static_cast<T>(std::strtod(j->text.c_str(), nullptr));
    } else if (j->type == JsonValue::Type::Bool) {
        v = static_cast<T>(j->boolean ? 1 : 0);
    } else {
        ++warnings_;
    }
}

template <class T> void JsonLoadVisitor::real(std::string_view n, T& v) {
    const JsonValue* j = resolve(n);
    if (!j) return;
    if (j->type == JsonValue::Type::Number) v = static_cast<T>(std::strtod(j->text.c_str(), nullptr));
    else ++warnings_;
}

void JsonLoadVisitor::visit(std::string_view n, bool& v, const FieldMeta&) {
    const JsonValue* j = resolve(n);
    if (!j) return;
    if (j->type == JsonValue::Type::Bool) v = j->boolean;
    else if (j->type == JsonValue::Type::Number) v = std::strtod(j->text.c_str(), nullptr) != 0;
    else ++warnings_;
}

void JsonLoadVisitor::visit(std::string_view n, i8& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, i16& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, i32& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, i64& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, u8& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, u16& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, u32& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, u64& v, const FieldMeta&) { integer(n, v); }
void JsonLoadVisitor::visit(std::string_view n, float& v, const FieldMeta&) { real(n, v); }
void JsonLoadVisitor::visit(std::string_view n, double& v, const FieldMeta&) { real(n, v); }

void JsonLoadVisitor::visit(std::string_view n, std::string& v, const FieldMeta&) {
    const JsonValue* j = resolve(n);
    if (!j) return;
    if (j->type == JsonValue::Type::String) v = j->text;
    else ++warnings_;
}

bool JsonLoadVisitor::begin_struct(std::string_view n, const FieldMeta&) {
    const JsonValue* j = resolve(n);
    if (!j) return false;
    if (j->type != JsonValue::Type::Object) {
        ++warnings_;
        return false;
    }
    stack_.push_back(j);
    return true;
}

void JsonLoadVisitor::end_struct() { stack_.pop_back(); }

bool JsonLoadVisitor::begin_array(std::string_view n, usize& count, const FieldMeta&) {
    const JsonValue* j = resolve(n);
    if (!j) return false;
    if (j->type != JsonValue::Type::Array) {
        ++warnings_;
        return false;
    }
    count = j->items.size();
    stack_.push_back(j);
    return true;
}

void JsonLoadVisitor::end_array() { stack_.pop_back(); }

bool JsonLoadVisitor::begin_element(usize index) {
    const JsonValue* arr = stack_.back();
    if (!arr || index >= arr->items.size()) return false;
    stack_.push_back(&arr->items[index]);
    return true;
}

void JsonLoadVisitor::end_element() { stack_.pop_back(); }

// ---- files ----------------------------------------------------------------------

bool read_text_file(const std::string& path, std::string& out, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "could not open " + path;
        return false;
    }
    out.clear();
    char buffer[16 * 1024];
    usize n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) out.append(buffer, n);
    std::fclose(f);
    return true;
}

bool write_text_file(const std::string& path, std::string_view text, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = "could not write " + path;
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok && error) *error = "short write to " + path;
    return ok;
}

} // namespace rw
