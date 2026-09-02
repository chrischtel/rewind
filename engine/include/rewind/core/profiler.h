#pragma once

#include "rewind/core/types.h"

#include <vector>

namespace rw {

/// One timed region of the last completed frame.
struct ProfileSample {
    char name[32];
    int depth;
    u64 begin_ns; ///< Relative to the frame start.
    u64 end_ns;
    u64 duration_ns() const { return end_ns - begin_ns; }
};

/// Always-on frame profiler. Main thread only. The App opens and closes the
/// frame; the engine and games mark regions with RW_PROFILE_SCOPE("name").
/// Costs a clock read per scope, so it stays on in shipping builds too.
class Profiler {
public:
    static Profiler& instance();

    void begin_frame();
    void end_frame();
    void push(const char* name);
    void pop();

    bool enabled() const { return enabled_; }
    void set_enabled(bool on) { enabled_ = on; }

    /// Regions of the last completed frame, in start order, nested by depth.
    const std::vector<ProfileSample>& last_frame() const { return last_; }
    u64 last_frame_ns() const { return last_frame_ns_; }

    /// Frame durations in milliseconds, oldest first, up to history_size().
    void history(std::vector<float>& out) const;
    usize history_size() const { return kHistory; }
    float average_ms() const;
    float worst_ms() const;

private:
    static constexpr usize kHistory = 240;

    u64 now_ns() const;

    std::vector<ProfileSample> current_;
    std::vector<ProfileSample> last_;
    std::vector<usize> stack_;
    u64 frame_begin_ns_ = 0;
    u64 last_frame_ns_ = 0;
    float history_[kHistory] = {};
    usize history_pos_ = 0;
    usize history_count_ = 0;
    bool enabled_ = true;
    bool in_frame_ = false;
};

struct ProfileScope {
    explicit ProfileScope(const char* name) { Profiler::instance().push(name); }
    ~ProfileScope() { Profiler::instance().pop(); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
};

#define RW_PROFILE_CONCAT_INNER(a, b) a##b
#define RW_PROFILE_CONCAT(a, b) RW_PROFILE_CONCAT_INNER(a, b)

/// Time the rest of the enclosing block under `name` (a string literal or
/// any string that outlives the scope; it is copied, so temporaries are fine).
#define RW_PROFILE_SCOPE(name) ::rw::ProfileScope RW_PROFILE_CONCAT(rw_profile_scope_, __LINE__)(name)
#define RW_PROFILE_FUNCTION() RW_PROFILE_SCOPE(__func__)

} // namespace rw
