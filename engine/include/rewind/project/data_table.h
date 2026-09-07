#pragma once

// Data tables: a game's definitions (unit types, items, tiles, recipes,
// waves...) as rows in a JSON file, one row per id. A row is any reflected
// struct with a `std::string id`, so it can hold numbers, names, colours,
// nested structs, arrays and AssetRefs, and the inspector, the JSON reader
// and the editor's Data panel all come for free.
//
//   struct UnitDef {
//       std::string id;
//       float speed = 1;
//       AssetRef sprite;
//       void reflect(Visitor& v) { v.field("id", id); v.field("speed", speed); v.field("sprite", sprite); }
//   };
//   DataTable<UnitDef> units{"units", "data/units.json"};
//   units.load();                       // {"rows": [{"id": "tank", ...}, ...]} or a bare array
//   const UnitDef* tank = units.find("tank");
//
// A DefRef<T> is a reference to a row by id: it reflects, saves and hashes as
// the id string, resolves against a table (cached until the table changes)
// and the inspector offers a dropdown of ids when the field names its table.
// Tables register by name while they exist (DataTableBase::find), and a game
// that lists them in Game::data_tables() gets them reloaded when the file
// changes on disk, and edited and saved in the editor.
//
// Definitions are data, not simulation state: they are not part of the
// timeline. Changing them mid-replay changes the replay, like changing code.

#include "rewind/core/paths.h"
#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"
#include "rewind/sim/text_format.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rw {

/// Everything about a table that does not depend on the row type.
class DataTableBase {
public:
    DataTableBase(std::string name, std::string path);
    virtual ~DataTableBase();
    DataTableBase(const DataTableBase&) = delete;
    DataTableBase& operator=(const DataTableBase&) = delete;

    const std::string& name() const { return name_; }
    /// Asset path of the JSON file (read through the asset roots and packs).
    const std::string& path() const { return path_; }
    void set_path(std::string path) { path_ = std::move(path); }
    std::string resolved_path() const { return resolve_asset_path(path_); }

    virtual usize size() const = 0;
    virtual const std::string& id_at(usize index) const = 0;
    /// -1 when no row has that id.
    i32 index_of(std::string_view id) const;
    bool has(std::string_view id) const { return index_of(id) >= 0; }

    /// Read the file at path(). False with a message when it cannot be
    /// read or parsed; validation findings go to problems() instead.
    bool load(std::string* error = nullptr);
    virtual bool load_text(std::string_view json, std::string* error = nullptr) = 0;
    virtual std::string to_text() = 0;
    /// Write to the file the table came from (the editor's Save).
    bool save(std::string* error = nullptr);

    // ---- generic row access, for tools ----------------------------------------
    virtual bool reflect_row(usize index, Visitor& v) = 0;
    virtual bool add_row(std::string id) = 0;
    virtual bool duplicate_row(usize index, std::string new_id) = 0;
    virtual bool remove_row(usize index) = 0;
    /// Call after editing rows through at() or reflect_row(): rebuilds the
    /// id index, validates, and bumps version() so DefRef caches refresh.
    void changed();

    u32 version() const { return version_; }
    /// Empty and duplicate ids, one line each.
    const std::vector<std::string>& problems() const { return problems_; }

    /// Tables register by name for as long as they exist.
    static DataTableBase* find(std::string_view name);
    static std::vector<DataTableBase*> all();

protected:
    std::string name_;
    std::string path_;
    u32 version_ = 1;
    std::vector<std::string> problems_;
    std::unordered_map<std::string, usize> index_;
};

/// Normalise a table document: a bare array becomes {"rows": [...]}.
bool parse_table_json(std::string_view text, JsonValue& out, std::string* error);

template <Reflectable T> class DataTable : public DataTableBase {
public:
    DataTable(std::string name, std::string path = {}) : DataTableBase(std::move(name), std::move(path)) {}

    const std::vector<T>& rows() const { return rows_; }
    usize size() const override { return rows_.size(); }
    const std::string& id_at(usize index) const override { return rows_[index].id; }
    const T& at(usize index) const { return rows_[index]; }
    T& at(usize index) { return rows_[index]; } ///< then changed() if the id moved
    const T* find(std::string_view id) const {
        const i32 i = index_of(id);
        return i < 0 ? nullptr : &rows_[static_cast<usize>(i)];
    }
    T* find(std::string_view id) {
        const i32 i = index_of(id);
        return i < 0 ? nullptr : &rows_[static_cast<usize>(i)];
    }

    bool load_text(std::string_view json, std::string* error = nullptr) override {
        JsonValue doc;
        if (!parse_table_json(json, doc, error)) return false;
        std::vector<T> rows;
        Rows wrap{&rows};
        apply_json(doc, wrap);
        rows_ = std::move(rows);
        changed();
        return true;
    }
    std::string to_text() override {
        Rows wrap{&rows_};
        return to_json(wrap);
    }

    bool reflect_row(usize index, Visitor& v) override {
        if (index >= rows_.size()) return false;
        rows_[index].reflect(v);
        return true;
    }
    bool add_row(std::string id) override {
        if (id.empty() || has(id)) return false;
        T row{};
        row.id = std::move(id);
        rows_.push_back(std::move(row));
        changed();
        return true;
    }
    bool duplicate_row(usize index, std::string new_id) override {
        if (index >= rows_.size() || new_id.empty() || has(new_id)) return false;
        T copy = rows_[index];
        copy.id = std::move(new_id);
        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(copy));
        changed();
        return true;
    }
    bool remove_row(usize index) override {
        if (index >= rows_.size()) return false;
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
        changed();
        return true;
    }

    /// The registered table of this row type with that name, if any.
    static DataTable* find_table(std::string_view name) { return dynamic_cast<DataTable*>(DataTableBase::find(name)); }

private:
    struct Rows {
        std::vector<T>* rows;
        void reflect(Visitor& v) { v.field("rows", *rows); }
    };
    std::vector<T> rows_;
};

/// A row reference by id. Saves and hashes as the string; resolve with
/// get(), which caches until the table's version changes.
template <Reflectable T> struct DefRef {
    std::string id;

    DefRef() = default;
    DefRef(std::string_view row_id) : id(row_id) {}

    bool empty() const { return id.empty(); }
    bool operator==(const DefRef& other) const { return id == other.id; }

    const T* get(const DataTable<T>& table) const {
        if (cache_table_ != &table || cache_version_ != table.version()) {
            cached_ = table.find(id);
            cache_table_ = &table;
            cache_version_ = table.version();
        }
        return cached_;
    }
    /// Through the registry (DataTable<T>::find_table).
    const T* get(std::string_view table_name) const {
        const DataTable<T>* table = DataTable<T>::find_table(table_name);
        return table ? get(*table) : nullptr;
    }

    void reflect(Visitor& v) {
        v.hint(Widget::Def);
        v.field("id", id);
    }

private:
    mutable const T* cached_ = nullptr;
    mutable const DataTable<T>* cache_table_ = nullptr;
    mutable u32 cache_version_ = 0;
};

} // namespace rw
