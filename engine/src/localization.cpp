#include "rewind/core/localization.h"

#include "rewind/core/log.h"
#include "rewind/core/paths.h"
#include "rewind/sim/text_format.h"

#include <algorithm>
#include <filesystem>

namespace rw {

namespace {
void flatten(const JsonValue& value, const std::string& prefix, StringTable& table) {
    for (const auto& [key, member] : value.members) {
        const std::string full = prefix.empty() ? key : prefix + "." + key;
        if (member.type == JsonValue::Type::Object) flatten(member, full, table);
        else if (member.type == JsonValue::Type::String) table.set(full, member.text);
        else if (member.type == JsonValue::Type::Number || member.type == JsonValue::Type::Bool)
            table.set(full, member.type == JsonValue::Type::Bool ? (member.boolean ? "true" : "false") : member.text);
    }
}
} // namespace

bool StringTable::load_json(const std::string& text, std::string* error) {
    JsonValue doc;
    std::string err;
    if (!parse_json(text, doc, err)) {
        if (error) *error = err;
        return false;
    }
    if (doc.type != JsonValue::Type::Object) {
        if (error) *error = "a string table must be a JSON object";
        return false;
    }
    strings_.clear();
    flatten(doc, "", *this);
    return true;
}

bool StringTable::load(const std::string& asset_path, std::string* error) {
    std::string text;
    if (!read_asset_text(asset_path, text)) {
        if (error) *error = "could not open " + asset_path;
        return false;
    }
    return load_json(text, error);
}

const std::string* StringTable::find(const std::string& key) const {
    auto it = strings_.find(key);
    return it == strings_.end() ? nullptr : &it->second;
}

void Localization::configure(const std::string& dir, const std::string& fallback) {
    dir_ = dir;
    fallback_ = fallback;
    std::string error;
    if (!fallback_table_.load(dir_ + "/" + fallback_ + ".json", &error))
        RW_LOG_WARN("loc", "fallback language '%s': %s", fallback_.c_str(), error.c_str());
    if (language_.empty()) language_ = fallback_;
}

bool Localization::set_language(const std::string& code) {
    language_ = code;
    if (code == fallback_) {
        current_ = fallback_table_;
        return true;
    }
    std::string error;
    if (!current_.load(dir_ + "/" + code + ".json", &error)) {
        RW_LOG_WARN("loc", "language '%s': %s (using %s)", code.c_str(), error.c_str(), fallback_.c_str());
        current_.clear();
        return false;
    }
    RW_LOG_INFO("loc", "language %s: %zu strings", code.c_str(), current_.size());
    return true;
}

std::vector<std::string> Localization::available() const {
    if (!available_.empty()) return available_;
    std::vector<std::string> codes;
    std::error_code ec;
    auto scan = [&](const std::string& dir) {
        for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".json") {
                const std::string code = e.path().stem().string();
                if (std::find(codes.begin(), codes.end(), code) == codes.end()) codes.push_back(code);
            }
    };
    scan(dir_);
    for (const std::string& root : asset_roots()) scan(root + dir_);
    for (const std::string& n : packed_asset_names(dir_ + "/")) {
        const std::filesystem::path p(n);
        if (p.extension() == ".json") codes.push_back(p.stem().string());
    }
    return codes;
}

const std::string& Localization::text(const std::string& key) const {
    if (const std::string* s = current_.find(key)) return *s;
    if (const std::string* s = fallback_table_.find(key)) return *s;
    missing_ = "[" + key + "]";
    return missing_;
}

bool Localization::has(const std::string& key) const { return current_.find(key) || fallback_table_.find(key); }

std::string Localization::format(const std::string& key, const std::vector<std::pair<std::string, std::string>>& values) const {
    std::string out = text(key);
    for (const auto& [name, value] : values) {
        const std::string token = "{" + name + "}";
        for (usize pos = out.find(token); pos != std::string::npos; pos = out.find(token, pos + value.size()))
            out.replace(pos, token.size(), value);
    }
    return out;
}

Localization& Localization::global() {
    static Localization loc;
    return loc;
}

} // namespace rw
