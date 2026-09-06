#pragma once

// String tables per language: "strings/<code>.json" is a flat JSON object
// of key to text. tr("menu.play") looks the key up in the current language,
// then the fallback, then returns the key itself in brackets so missing
// entries are visible in the game instead of silent. {placeholders} are
// filled by trf().

#include "rewind/core/types.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rw {

class StringTable {
public:
    /// Parse a JSON object of strings; nested objects flatten with dots.
    bool load_json(const std::string& text, std::string* error = nullptr);
    /// Through the asset roots and packs: "strings/de.json".
    bool load(const std::string& asset_path, std::string* error = nullptr);
    void set(const std::string& key, const std::string& text) { strings_[key] = text; }
    const std::string* find(const std::string& key) const;
    usize size() const { return strings_.size(); }
    void clear() { strings_.clear(); }

private:
    std::unordered_map<std::string, std::string> strings_;
};

class Localization {
public:
    /// Language tables live at "<dir>/<code>.json"; the fallback is loaded too.
    void configure(const std::string& dir = "strings", const std::string& fallback = "en");
    /// Load `code` (and the fallback when different). Returns false when the
    /// language file is missing; the fallback then serves everything.
    bool set_language(const std::string& code);
    const std::string& language() const { return language_; }
    const std::string& fallback() const { return fallback_; }
    /// Languages with a table in the folder (loose files only, or names given to set_available()).
    std::vector<std::string> available() const;
    void set_available(std::vector<std::string> codes) { available_ = std::move(codes); }

    const std::string& text(const std::string& key) const;
    /// Replace {name} with the given values.
    std::string format(const std::string& key, const std::vector<std::pair<std::string, std::string>>& values) const;
    bool has(const std::string& key) const;

    StringTable& table() { return current_; }
    StringTable& fallback_table() { return fallback_table_; }

    static Localization& global();

private:
    std::string dir_ = "strings";
    std::string fallback_ = "en";
    std::string language_;
    StringTable current_;
    StringTable fallback_table_;
    std::vector<std::string> available_;
    mutable std::string missing_;
};

/// Shortcuts on the global instance.
inline const std::string& tr(const std::string& key) { return Localization::global().text(key); }
inline std::string trf(const std::string& key, const std::vector<std::pair<std::string, std::string>>& values) {
    return Localization::global().format(key, values);
}

} // namespace rw
