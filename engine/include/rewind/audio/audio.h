#pragma once

#include "rewind/core/types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_AudioStream;

namespace rw {

class FileWatcher;

/// Decoded PCM audio owned by the Audio system. Games hold pointers; they
/// stay valid until shutdown, which includes across hot reloads.
struct Sound {
    int sample_rate = 48000;
    int channels = 1;
    int format = 0;       ///< SDL_AudioFormat
    std::vector<u8> data; ///< interleaved samples in `format`

    float seconds() const;
};

/// Small polyphonic sound player over SDL's audio streams. Each play() gets
/// its own stream; SDL mixes and resamples them. No positional audio yet.
///
/// Audio is deliberately not reachable from Game::tick(): emit an event there
/// and call play() from Game::on_event(), so rewinds and replays stay silent.
class Audio {
public:
    Audio() = default;
    ~Audio();

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    /// Open the default playback device. Returns false (and logs) on failure;
    /// every other call is then a harmless no-op.
    bool init();
    void shutdown();
    bool ready() const { return device_ != 0; }

    /// Load a WAV file. Cached by path; re-read into the same Sound object
    /// when the file changed on disk. Tracked for asset hot reload.
    Sound* load_wav(const std::string& path);

    void set_file_watcher(FileWatcher* watcher) { watcher_ = watcher; }
    void invalidate_file(const std::string& path) {
        stale_files_.insert(path);
        auto alias = aliases_.find(path);
        if (alias != aliases_.end()) stale_files_.insert(alias->second);
    }

    /// Create (or replace) a sound from 32-bit float samples, interleaved if
    /// `channels` > 1. Cached by name.
    Sound* create_sound(const std::string& name, int sample_rate, int channels, const float* samples,
                        usize sample_count);

    Sound* find_sound(const std::string& name) const;

    /// Start playing. `volume` is linear, 1 = as recorded.
    void play(const Sound& sound, float volume = 1.0f);
    void play(const Sound* sound, float volume = 1.0f) {
        if (sound) play(*sound, volume);
    }

    /// Stop every voice at once.
    void stop_all();

    void set_master_volume(float volume);
    float master_volume() const { return master_; }

    /// Call once per frame: releases voices that have finished.
    void update();

    int active_voices() const { return static_cast<int>(voices_.size()); }
    int max_voices() const { return max_voices_; }
    void set_max_voices(int n) { max_voices_ = n < 1 ? 1 : n; }

private:
    u32 device_ = 0;
    float master_ = 1.0f;
    int max_voices_ = 32;
    std::vector<SDL_AudioStream*> voices_;
    std::unordered_map<std::string, std::unique_ptr<Sound>> sounds_;
    std::unordered_set<std::string> stale_files_;
    std::unordered_map<std::string, std::string> aliases_; ///< resolved path -> name the game used
    FileWatcher* watcher_ = nullptr;
};

} // namespace rw
