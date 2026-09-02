#pragma once

#include "rewind/core/types.h"

#include <string>
#include <vector>

namespace rw {

enum class LogLevel : u8 { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct LogEntry {
    LogLevel level;
    u64 time_ms;   ///< Milliseconds since the log started.
    u32 frame;     ///< Frame number the App was on, 0 before the loop.
    std::string category;
    std::string text;
};

/// Engine-wide log. Every message goes to stderr (and the debugger output on
/// Windows) and into a ring buffer the overlay console shows. Categories are
/// free-form short strings: "gpu", "audio", "hot", "assets", or your game's.
/// Thread-safe. Prefer the RW_LOG_* macros below.
void log_write(LogLevel level, const char* category, const char* fmt, ...);

void log_set_min_level(LogLevel level);
LogLevel log_min_level();

/// Whether entries are also printed to stderr and the debugger. On by default.
void log_set_echo(bool on);

/// Copy of the retained entries, oldest first.
void log_snapshot(std::vector<LogEntry>& out);
usize log_capacity();

/// Messages at or above `level` since start or the last clear.
u32 log_count(LogLevel level);
void log_clear();

/// Set by the App each frame so entries can say when they happened.
void log_set_frame(u32 frame);

/// Route SDL's own messages (category "sdl") through this log as well.
void log_capture_sdl();

const char* log_level_name(LogLevel level);

} // namespace rw

#define RW_LOG_DEBUG(category, ...) ::rw::log_write(::rw::LogLevel::Debug, category, __VA_ARGS__)
#define RW_LOG_INFO(category, ...) ::rw::log_write(::rw::LogLevel::Info, category, __VA_ARGS__)
#define RW_LOG_WARN(category, ...) ::rw::log_write(::rw::LogLevel::Warn, category, __VA_ARGS__)
#define RW_LOG_ERROR(category, ...) ::rw::log_write(::rw::LogLevel::Error, category, __VA_ARGS__)
