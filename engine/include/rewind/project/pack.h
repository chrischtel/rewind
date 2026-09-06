#pragma once

// A pack is one file holding a project's cooked and loose assets, scenes
// and project settings, for shipping: exe + game DLL + pack. Entries are
// named exactly as the loaders ask for them ("sprites/hero.png.cooked",
// "fonts/ui.ttf", "scenes/level1.json"). Mounted packs come first in
// asset resolution, so a shipped game reads nothing from the filesystem.
//
// Format: "RWPK", version, entry count, then per entry the name, offset,
// size and content hash, then the data. Small, sequential, no compression
// (cooked textures are already compact; add compression later if needed).

#include "rewind/core/types.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace rw {

struct PackEntry {
    std::string name;
    u64 offset = 0;
    u64 size = 0;
    u64 hash = 0;
};

struct PackInput {
    std::string name;
    std::vector<u8> bytes;
};

/// Write a pack. Names must be unique.
bool write_pack(const std::string& file, const std::vector<PackInput>& inputs, std::string* error = nullptr);

class Pack {
public:
    Pack() = default;
    ~Pack();
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;

    bool open(const std::string& file, std::string* error = nullptr);
    void close();
    bool is_open() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }

    const PackEntry* find(const std::string& name) const;
    bool read(const std::string& name, std::vector<u8>& out) const;
    const std::vector<PackEntry>& entries() const { return entries_; }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    std::vector<PackEntry> entries_;
    std::unordered_map<std::string, usize> by_name_;
};

} // namespace rw
