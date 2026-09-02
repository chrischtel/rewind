// Headless checks for the file watcher and the profiler.

#include <rewind/core/file_watcher.h>
#include <rewind/core/log.h>
#include <rewind/core/profiler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace rw;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

void test_file_watcher() {
    const fs::path dir = fs::temp_directory_path() / "rewind_core_test";
    fs::create_directories(dir);
    const fs::path a = dir / "a.txt";
    const fs::path b = dir / "b.txt";
    write_file(a, "one");
    write_file(b, "one");

    FileWatcher w;
    w.set_settle_ms(0);
    w.track(a.string());
    w.track(b.string());
    w.track(a.string()); // duplicate is harmless
    CHECK(w.count() == 2);
    CHECK(w.tracks(a.string()));

    CHECK(w.poll(true).empty()); // nothing changed yet

    // Touch a: the watcher must report exactly it, exactly once.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write_file(a, "two");
    fs::last_write_time(a, fs::file_time_type::clock::now());
    const auto changed = w.poll(true);
    CHECK(changed.size() == 1);
    CHECK(!changed.empty() && changed[0] == a.string());
    CHECK(w.poll(true).empty());

    // A file that vanishes is not reported and does not break polling.
    fs::remove(b);
    CHECK(w.poll(true).empty());
    write_file(b, "back");
    fs::last_write_time(b, fs::file_time_type::clock::now() + std::chrono::seconds(1));
    const auto back = w.poll(true);
    CHECK(back.size() == 1 && back[0] == b.string());

    // With a settle time, a change is reported only after it stops moving.
    FileWatcher slow;
    slow.set_settle_ms(150);
    slow.track(a.string());
    write_file(a, "three");
    fs::last_write_time(a, fs::file_time_type::clock::now() + std::chrono::seconds(2));
    CHECK(slow.poll(true).empty()); // first sighting starts the settle timer
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto settled = slow.poll(true);
    CHECK(settled.size() == 1);

    w.untrack(a.string());
    CHECK(!w.tracks(a.string()));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void busy(int iterations) {
    volatile int sink = 0;
    for (int i = 0; i < iterations; ++i) sink = sink + i;
}

void test_profiler() {
    Profiler& p = Profiler::instance();
    p.set_enabled(true);

    // Scopes outside a frame are ignored rather than crashing.
    p.push("outside");
    p.pop();

    p.begin_frame();
    {
        RW_PROFILE_SCOPE("outer");
        busy(100000);
        {
            RW_PROFILE_SCOPE("inner");
            busy(50000);
        }
        {
            RW_PROFILE_SCOPE("inner");
            busy(50000);
        }
    }
    p.push("unclosed"); // end_frame closes it
    p.end_frame();

    const auto& samples = p.last_frame();
    CHECK(samples.size() == 4);
    if (samples.size() == 4) {
        CHECK(std::strcmp(samples[0].name, "outer") == 0 && samples[0].depth == 0);
        CHECK(std::strcmp(samples[1].name, "inner") == 0 && samples[1].depth == 1);
        CHECK(std::strcmp(samples[2].name, "inner") == 0 && samples[2].depth == 1);
        CHECK(std::strcmp(samples[3].name, "unclosed") == 0 && samples[3].depth == 0);
        CHECK(samples[0].begin_ns <= samples[1].begin_ns);
        CHECK(samples[1].end_ns <= samples[2].begin_ns);
        CHECK(samples[0].end_ns >= samples[2].end_ns);
        CHECK(samples[0].duration_ns() >= samples[1].duration_ns() + samples[2].duration_ns());
        CHECK(samples[3].end_ns >= samples[3].begin_ns);
    }
    CHECK(p.last_frame_ns() >= samples[0].duration_ns());

    std::vector<float> history;
    p.history(history);
    CHECK(history.size() == 1);
    CHECK(p.average_ms() > 0);
    CHECK(p.worst_ms() >= p.average_ms());

    // Names longer than the buffer are truncated, not overflowed.
    p.begin_frame();
    p.push("a very long region name that certainly exceeds thirty-one characters");
    p.pop();
    p.end_frame();
    CHECK(std::strlen(p.last_frame()[0].name) == 31);

    // Disabled: nothing recorded, frame still accounted.
    p.set_enabled(false);
    p.begin_frame();
    {
        RW_PROFILE_SCOPE("ignored");
    }
    p.end_frame();
    CHECK(p.last_frame().empty());
    p.history(history);
    CHECK(history.size() == 3);
    p.set_enabled(true);
}

void test_log() {
    log_clear();
    log_set_echo(false); // keep the test output readable
    log_set_min_level(LogLevel::Debug);
    log_set_frame(7);
    RW_LOG_INFO("test", "hello %d", 42);
    RW_LOG_WARN("test", "careful");
    RW_LOG_ERROR("other", "broken: %s", "yes");
    RW_LOG_DEBUG("test", "noise");

    std::vector<LogEntry> entries;
    log_snapshot(entries);
    CHECK(entries.size() == 4);
    if (entries.size() == 4) {
        CHECK(entries[0].text == "hello 42" && entries[0].category == "test" && entries[0].level == LogLevel::Info);
        CHECK(entries[0].frame == 7);
        CHECK(entries[2].level == LogLevel::Error && entries[2].category == "other");
        CHECK(entries[3].level == LogLevel::Debug);
    }
    CHECK(log_count(LogLevel::Warn) == 2);
    CHECK(log_count(LogLevel::Error) == 1);
    CHECK(log_count(LogLevel::Debug) == 4);

    // Below the minimum level nothing is stored.
    log_set_min_level(LogLevel::Warn);
    RW_LOG_INFO("test", "dropped");
    log_snapshot(entries);
    CHECK(entries.size() == 4);
    log_set_min_level(LogLevel::Debug);

    // The ring buffer never grows past its capacity.
    for (usize i = 0; i < log_capacity() + 50; ++i) RW_LOG_DEBUG("flood", "%zu", i);
    log_snapshot(entries);
    CHECK(entries.size() == log_capacity());
    CHECK(entries.back().text == std::to_string(log_capacity() + 49));

    log_clear();
    log_snapshot(entries);
    CHECK(entries.empty());
    CHECK(log_count(LogLevel::Debug) == 0);
    log_set_echo(true);
}

} // namespace

int main() {
    test_file_watcher();
    test_profiler();
    test_log();

    if (failures == 0) {
        std::printf("core_test: all checks passed\n");
        return 0;
    }
    std::printf("core_test: %d check(s) failed\n", failures);
    return 1;
}
