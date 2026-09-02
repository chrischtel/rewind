#include "rewind/core/log.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rw {

namespace {

constexpr usize kCapacity = 2000;

struct LogState {
    std::mutex mutex;
    std::deque<LogEntry> entries;
    LogLevel min_level = LogLevel::Debug;
    bool echo = true;
    u32 frame = 0;
    u32 counts[4] = {0, 0, 0, 0};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

LogState& state() {
    static LogState s;
    return s;
}

void store(LogLevel level, const char* category, std::string text) {
    LogState& s = state();
    const char* cat = category ? category : "";

    // Console and debugger first, so nothing is lost if the buffer is not read.
    if (s.echo) {
        std::fprintf(stderr, "[%s] %s: %s\n", log_level_name(level), cat, text.c_str());
        std::fflush(stderr);
#if defined(_WIN32)
        if (IsDebuggerPresent()) {
            std::string line = std::string("[") + log_level_name(level) + "] " + cat + ": " + text + "\n";
            OutputDebugStringA(line.c_str());
        }
#endif
    }

    std::lock_guard<std::mutex> lock(s.mutex);
    LogEntry e;
    e.level = level;
    e.time_ms = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s.start).count());
    e.frame = s.frame;
    e.category = cat;
    e.text = std::move(text);
    s.entries.push_back(std::move(e));
    if (s.entries.size() > kCapacity) s.entries.pop_front();
    ++s.counts[static_cast<int>(level)];
}

void sdl_output(void*, int, SDL_LogPriority priority, const char* message) {
    LogLevel level = LogLevel::Info;
    switch (priority) {
    case SDL_LOG_PRIORITY_TRACE:
    case SDL_LOG_PRIORITY_VERBOSE:
    case SDL_LOG_PRIORITY_DEBUG: level = LogLevel::Debug; break;
    case SDL_LOG_PRIORITY_INFO: level = LogLevel::Info; break;
    case SDL_LOG_PRIORITY_WARN: level = LogLevel::Warn; break;
    case SDL_LOG_PRIORITY_ERROR:
    case SDL_LOG_PRIORITY_CRITICAL: level = LogLevel::Error; break;
    default: break;
    }
    if (level < state().min_level) return;
    store(level, "sdl", message ? message : "");
}

} // namespace

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "?";
}

void log_write(LogLevel level, const char* category, const char* fmt, ...) {
    if (level < state().min_level) return;
    char buffer[2048];
    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt ? fmt : "", args);
    va_end(args);
    store(level, category, buffer);
}

void log_set_min_level(LogLevel level) { state().min_level = level; }
LogLevel log_min_level() { return state().min_level; }
void log_set_echo(bool on) { state().echo = on; }

void log_snapshot(std::vector<LogEntry>& out) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    out.assign(s.entries.begin(), s.entries.end());
}

usize log_capacity() { return kCapacity; }

u32 log_count(LogLevel level) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    u32 total = 0;
    for (int i = static_cast<int>(level); i < 4; ++i) total += s.counts[i];
    return total;
}

void log_clear() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.clear();
    for (u32& c : s.counts) c = 0;
}

void log_set_frame(u32 frame) { state().frame = frame; }

void log_capture_sdl() { SDL_SetLogOutputFunction(sdl_output, nullptr); }

} // namespace rw
