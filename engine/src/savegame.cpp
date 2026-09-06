#include "rewind/sim/savegame.h"

#include "rewind/core/player_settings.h"
#include "rewind/sim/game.h"
#include "rewind/sim/serialize.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace rw {

namespace {
constexpr u32 kMagic = 0x56535752u; // "RWSV"
constexpr u32 kFormat = 1;

bool read_all(const std::string& path, std::vector<u8>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    u8 buffer[64 * 1024];
    usize n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
    return true;
}

/// Header, then the state; `state_offset` says where the state begins.
bool parse_header(const std::vector<u8>& bytes, SaveInfo& info, usize& state_offset, std::string* error) {
    Reader r(bytes);
    if (bytes.size() < 8 || r.pod<u32>() != kMagic) {
        if (error) *error = "not a Rewind save file";
        return false;
    }
    if (r.pod<u32>() != kFormat) {
        if (error) *error = "save file from a newer engine";
        return false;
    }
    info.version = r.pod<u32>();
    info.timestamp = r.pod<u64>();
    info.tick = r.pod<u64>();
    r.str(info.label);
    const u64 size = r.pod<u64>();
    if (!r.ok() || size > r.remaining()) {
        if (error) *error = "damaged save file";
        return false;
    }
    info.bytes = static_cast<usize>(size);
    state_offset = bytes.size() - r.remaining();
    return true;
}
} // namespace

bool write_savegame(const std::string& file, Game& game, u32 version, const std::string& label, u64 tick, std::string* error) {
    Writer state;
    game.save(state);
    Writer w;
    w.pod(kMagic);
    w.pod(kFormat);
    w.pod(version);
    w.pod<u64>(static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    w.pod<u64>(tick);
    w.str(label);
    w.pod<u64>(state.bytes().size());
    w.append(state.bytes().data(), state.bytes().size());
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) {
        if (error) *error = "could not write " + file;
        return false;
    }
    const bool ok = std::fwrite(w.bytes().data(), 1, w.bytes().size(), f) == w.bytes().size();
    std::fclose(f);
    if (!ok && error) *error = "short write to " + file;
    return ok;
}

bool peek_savegame(const std::string& file, SaveInfo& info, std::string* error) {
    std::vector<u8> bytes;
    if (!read_all(file, bytes)) {
        if (error) *error = "could not open " + file;
        return false;
    }
    usize offset = 0;
    if (!parse_header(bytes, info, offset, error)) return false;
    info.file = file;
    return true;
}

bool read_savegame(const std::string& file, Game& game, u32 current_version, const SaveMigration& migrate, SaveInfo* info_out,
                   std::string* error) {
    std::vector<u8> bytes;
    if (!read_all(file, bytes)) {
        if (error) *error = "could not open " + file;
        return false;
    }
    SaveInfo info;
    usize offset = 0;
    if (!parse_header(bytes, info, offset, error)) return false;
    info.file = file;
    std::vector<u8> state(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + info.bytes));
    if (info.version != current_version) {
        if (!migrate) {
            if (error) *error = "save version " + std::to_string(info.version) + " differs from " + std::to_string(current_version) +
                                " and the game has no migration";
            return false;
        }
        if (!migrate(info.version, state)) {
            if (error) *error = "the game refused to migrate save version " + std::to_string(info.version);
            return false;
        }
    }
    Reader r(state);
    game.load(r);
    if (!r.ok() || r.remaining() != 0) {
        if (error) *error = "the state in " + file + " does not fit this game's layout";
        return false;
    }
    if (info_out) *info_out = info;
    return true;
}

std::vector<SaveInfo> list_savegames(const std::string& dir) {
    std::vector<SaveInfo> out;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".rwsave") continue;
        SaveInfo info;
        if (peek_savegame(entry.path().string(), info)) out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.timestamp > b.timestamp; });
    return out;
}

std::string savegame_dir(const std::string& organization, const std::string& title) {
    const std::string base = user_data_dir(organization, title);
    if (base.empty()) return {};
    const std::string dir = base + "saves/";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string format_timestamp(u64 seconds) {
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

} // namespace rw
