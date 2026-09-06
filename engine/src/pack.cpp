#include "rewind/project/pack.h"

#include "rewind/sim/serialize.h"

#include <cstring>

namespace rw {

namespace {
constexpr u32 kMagic = 0x4B505752u; // "RWPK"
constexpr u32 kVersion = 1;

u64 fnv1a(const std::vector<u8>& bytes) {
    u64 h = 1469598103934665603ull;
    for (u8 b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}
} // namespace

bool write_pack(const std::string& file, const std::vector<PackInput>& inputs, std::string* error) {
    // Header and table first, sized exactly, then the blobs in order.
    Writer table;
    table.pod(kMagic);
    table.pod(kVersion);
    table.pod<u64>(inputs.size());
    // Two passes: the table needs offsets, offsets need the table size.
    usize table_size = 4 + 4 + 8;
    for (const PackInput& in : inputs) table_size += 8 + in.name.size() + 8 + 8 + 8;
    u64 offset = table_size;
    for (const PackInput& in : inputs) {
        table.str(in.name);
        table.pod<u64>(offset);
        table.pod<u64>(in.bytes.size());
        table.pod<u64>(fnv1a(in.bytes));
        offset += in.bytes.size();
    }
    if (table.bytes().size() != table_size) {
        if (error) *error = "internal: pack table size mismatch";
        return false;
    }
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) {
        if (error) *error = "could not write " + file;
        return false;
    }
    bool ok = std::fwrite(table.bytes().data(), 1, table.bytes().size(), f) == table.bytes().size();
    for (const PackInput& in : inputs)
        if (ok && !in.bytes.empty()) ok = std::fwrite(in.bytes.data(), 1, in.bytes.size(), f) == in.bytes.size();
    std::fclose(f);
    if (!ok && error) *error = "short write to " + file;
    return ok;
}

Pack::~Pack() { close(); }

void Pack::close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    entries_.clear();
    by_name_.clear();
    path_.clear();
}

bool Pack::open(const std::string& file, std::string* error) {
    close();
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) {
        if (error) *error = "could not open " + file;
        return false;
    }
    u8 head[16];
    if (std::fread(head, 1, sizeof head, f) != sizeof head) {
        std::fclose(f);
        if (error) *error = file + " is too short to be a pack";
        return false;
    }
    Reader hr(head, sizeof head);
    const u32 magic = hr.pod<u32>(), version = hr.pod<u32>();
    const u64 count = hr.pod<u64>();
    if (magic != kMagic || version != kVersion) {
        std::fclose(f);
        if (error) *error = file + " is not a Rewind pack (or a newer version)";
        return false;
    }
    // Read the rest of the table entry by entry.
    for (u64 i = 0; i < count; ++i) {
        u64 name_len = 0;
        if (std::fread(&name_len, 1, 8, f) != 8 || name_len > 4096) break;
        std::string name(static_cast<usize>(name_len), '\0');
        u8 rest[24];
        if (std::fread(name.data(), 1, name.size(), f) != name.size() || std::fread(rest, 1, 24, f) != 24) break;
        Reader rr(rest, 24);
        PackEntry e;
        e.name = std::move(name);
        e.offset = rr.pod<u64>();
        e.size = rr.pod<u64>();
        e.hash = rr.pod<u64>();
        by_name_[e.name] = entries_.size();
        entries_.push_back(std::move(e));
    }
    if (entries_.size() != count) {
        std::fclose(f);
        entries_.clear();
        by_name_.clear();
        if (error) *error = file + ": damaged pack table";
        return false;
    }
    file_ = f;
    path_ = file;
    return true;
}

const PackEntry* Pack::find(const std::string& name) const {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &entries_[it->second];
}

bool Pack::read(const std::string& name, std::vector<u8>& out) const {
    const PackEntry* e = find(name);
    if (!e || !file_) return false;
    out.resize(static_cast<usize>(e->size));
    if (e->size == 0) return true;
#if defined(_WIN32)
    if (_fseeki64(file_, static_cast<long long>(e->offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(file_, static_cast<off_t>(e->offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(out.data(), 1, out.size(), file_) == out.size();
}

} // namespace rw
