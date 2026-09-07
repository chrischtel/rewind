#include "rewind/project/data_table.h"

#include <algorithm>

namespace rw {

namespace {
std::vector<DataTableBase*>& registry() {
    static std::vector<DataTableBase*> tables;
    return tables;
}
} // namespace

DataTableBase::DataTableBase(std::string name, std::string path) : name_(std::move(name)), path_(std::move(path)) {
    registry().push_back(this);
}

DataTableBase::~DataTableBase() {
    auto& r = registry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
}

DataTableBase* DataTableBase::find(std::string_view name) {
    for (DataTableBase* t : registry())
        if (t->name_ == name) return t;
    return nullptr;
}

std::vector<DataTableBase*> DataTableBase::all() { return registry(); }

i32 DataTableBase::index_of(std::string_view id) const {
    if (id.empty()) return -1;
    const auto it = index_.find(std::string(id));
    return it == index_.end() ? -1 : static_cast<i32>(it->second);
}

bool DataTableBase::load(std::string* error) {
    std::string text;
    if (!read_asset_text(path_, text)) {
        if (error) *error = "data table " + name_ + ": cannot read " + path_;
        return false;
    }
    std::string parse_error;
    if (!load_text(text, &parse_error)) {
        if (error) *error = "data table " + name_ + " (" + path_ + "): " + parse_error;
        return false;
    }
    return true;
}

bool DataTableBase::save(std::string* error) {
    if (path_.empty()) {
        if (error) *error = "data table " + name_ + " has no path";
        return false;
    }
    return write_text_file(resolved_path(), to_text(), error);
}

void DataTableBase::changed() {
    index_.clear();
    problems_.clear();
    const usize n = size();
    for (usize i = 0; i < n; ++i) {
        const std::string& id = id_at(i);
        if (id.empty()) {
            problems_.push_back("row " + std::to_string(i) + " has no id");
            continue;
        }
        const auto it = index_.find(id);
        if (it != index_.end()) {
            problems_.push_back("duplicate id \"" + id + "\" (rows " + std::to_string(it->second) + " and " + std::to_string(i) + ")");
            continue;
        }
        index_.emplace(id, i);
    }
    ++version_;
}

bool parse_table_json(std::string_view text, JsonValue& out, std::string* error) {
    JsonValue doc;
    std::string err;
    if (!parse_json(text, doc, err)) {
        if (error) *error = err;
        return false;
    }
    if (doc.type == JsonValue::Type::Array) {
        out = JsonValue{};
        out.type = JsonValue::Type::Object;
        out.members.emplace_back("rows", std::move(doc));
        return true;
    }
    if (doc.type != JsonValue::Type::Object || !doc.find("rows") || doc.find("rows")->type != JsonValue::Type::Array) {
        if (error) *error = "expected an array of rows or {\"rows\": [...]}";
        return false;
    }
    out = std::move(doc);
    return true;
}

} // namespace rw
