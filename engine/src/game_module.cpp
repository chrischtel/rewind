#include "rewind/core/game_module.h"

#include "rewind/core/log.h"
#include "rewind/sim/game.h"
#include "rewind/sim/serialize.h"

#include <string>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rw {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr u32 kGameApiVersion = 1;
constexpr auto kCheckInterval = 200ms;
constexpr auto kSettleTime = 400ms; ///< how long the file must stay unchanged before we act
constexpr int kMaxCopyAttempts = 12; ///< linker may still hold the file; ~5 s of retries

#if defined(_WIN32)
std::string last_error_text() {
    const DWORD code = GetLastError();
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string result = text ? text : "";
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    return result + " (code " + std::to_string(code) + ")";
}

void* open_library(const fs::path& path) { return LoadLibraryW(path.c_str()); }
void close_library(void* handle) { FreeLibrary(static_cast<HMODULE>(handle)); }
void* find_symbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}
std::string library_error() { return last_error_text(); }
#else
void* open_library(const fs::path& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void close_library(void* handle) { dlclose(handle); }
void* find_symbol(void* handle, const char* name) { return dlsym(handle, name); }
std::string library_error() {
    const char* e = dlerror();
    return e ? e : "unknown error";
}
#endif

fs::path copy_name(const fs::path& original, u32 generation) {
    return original.parent_path() /
           (original.stem().string() + ".hot" + std::to_string(generation) + original.extension().string());
}

} // namespace

GameModule::GameModule(fs::path module_path) : path_(std::move(module_path)) {}

GameModule::~GameModule() { unload(current_); }

bool GameModule::load() {
    std::error_code ec;
    if (!fs::exists(path_, ec)) {
        error_ = "game module not found: " + path_.string();
        return false;
    }
    remove_stale_copies();

    Loaded loaded;
    if (!load_copy(1, loaded)) return false;

    current_ = loaded;
    generation_ = 1;
    loaded_mtime_ = fs::last_write_time(path_, ec);
    last_check_ = Clock::now();
    error_.clear();
    return true;
}

GameModule::Reload GameModule::poll() {
    Reload result;
    if (!current_.game) return result;

    const auto now = Clock::now();
    if (now - last_check_ < kCheckInterval) return result;
    last_check_ = now;

    std::error_code ec;
    const auto mtime = fs::last_write_time(path_, ec);
    if (ec) return result; // being rewritten or momentarily missing

    if (mtime == loaded_mtime_) {
        pending_ = false;
        return result;
    }
    if (has_failed_mtime_ && mtime == failed_mtime_) return result;

    if (!pending_ || mtime != pending_mtime_) {
        pending_ = true;
        pending_mtime_ = mtime;
        pending_since_ = now;
        copy_attempts_ = 0;
        return result;
    }
    if (now - pending_since_ < kSettleTime) return result;

    // The file has been stable for a while: try to bring the new build up.
    Loaded next;
    if (!load_copy(generation_ + 1, next)) {
        result.status = Reload::Status::Failed;
        result.error = error_;
        if (copy_failed_ && ++copy_attempts_ < kMaxCopyAttempts) {
            // Copy failed, most likely because the linker still owns the file.
            pending_since_ = now;
            return result;
        }
        pending_ = false;
        has_failed_mtime_ = true;
        failed_mtime_ = mtime;
        return result;
    }

    // Carry the running state across. The new game's constructor has run;
    // its init() has not, and must not: load() replaces the whole state.
    Writer state;
    current_.game->save(state);
    Reader reader(state.bytes());
    next.game->load(reader);
    result.state_compatible = reader.ok() && reader.remaining() == 0;
    if (!result.state_compatible) {
        // The new build reads a different layout. Give it a clean object; the
        // caller will init() it and start a fresh timeline.
        next.destroy(next.game);
        next.game = next.create();
    }

    unload(current_);
    current_ = next;
    ++generation_;
    loaded_mtime_ = mtime;
    pending_ = false;
    has_failed_mtime_ = false;
    error_.clear();
    result.status = Reload::Status::Reloaded;
    return result;
}

bool GameModule::load_copy(u32 generation, Loaded& out) {
    const fs::path copy = copy_name(path_, generation);

    std::error_code ec;
    fs::copy_file(path_, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error_ = "could not copy " + path_.filename().string() + ": " + ec.message();
        copy_failed_ = true;
        return false;
    }
    copy_failed_ = false;
    out.copy = copy;

    void* handle = open_library(copy);
    if (!handle) {
        error_ = "could not load " + copy.filename().string() + ": " + library_error();
        fs::remove(copy, ec);
        out.copy.clear();
        return false;
    }

    auto version_fn = reinterpret_cast<u32 (*)()>(find_symbol(handle, "rewind_game_api_version"));
    auto create_fn = reinterpret_cast<Game* (*)()>(find_symbol(handle, "rewind_create_game"));
    auto destroy_fn = reinterpret_cast<void (*)(Game*)>(find_symbol(handle, "rewind_destroy_game"));

    if (!version_fn || !create_fn || !destroy_fn) {
        error_ = path_.filename().string() +
                 " does not export the Rewind game entry points (did you use REWIND_GAME(...)?)";
        close_library(handle);
        fs::remove(copy, ec);
        out.copy.clear();
        return false;
    }
    if (version_fn() != kGameApiVersion) {
        error_ = "game API version mismatch: module has " + std::to_string(version_fn()) +
                 ", engine expects " + std::to_string(kGameApiVersion);
        close_library(handle);
        fs::remove(copy, ec);
        out.copy.clear();
        return false;
    }

    out.handle = handle;
    out.create = create_fn;
    out.destroy = destroy_fn;
    out.game = create_fn();
    if (!out.game) {
        error_ = "rewind_create_game() returned null";
        close_library(handle);
        fs::remove(copy, ec);
        out = Loaded{};
        return false;
    }
    return true;
}

void GameModule::unload(Loaded& loaded) {
    if (loaded.game && loaded.destroy) loaded.destroy(loaded.game);
    loaded.game = nullptr;
    if (loaded.handle) close_library(loaded.handle);
    loaded.handle = nullptr;
    if (!loaded.copy.empty()) {
        std::error_code ec;
        fs::remove(loaded.copy, ec);
        loaded.copy.clear();
    }
}

void GameModule::remove_stale_copies() {
    // Copies left behind by a previous session that crashed or was killed.
    std::error_code ec;
    const std::string prefix = path_.stem().string() + ".hot";
    const std::string ext = path_.extension().string();
    for (const auto& entry : fs::directory_iterator(path_.parent_path(), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && entry.path().extension().string() == ext) {
            fs::remove(entry.path(), ec); // ignored: a copy still in use stays
        }
    }
}

} // namespace rw
