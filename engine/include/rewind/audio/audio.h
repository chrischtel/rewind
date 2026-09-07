#pragma once

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/project/asset_ref.h"
#include "rewind/sim/reflect.h"

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

/// The player's ears for positional sounds (play_at): full volume within
/// `near_distance`, silent beyond `max_distance`, linear in between, and
/// panned left or right by where the source is relative to the listener.
struct AudioListener {
    Vec2 position;
    float max_distance = 800;
    float near_distance = 40;
};

/// Volume factor (0..1) for a source at `position`.
float positional_gain(Vec2 position, const AudioListener& listener);
/// Pan for a source at `position`: -1 fully left, 0 centre, 1 fully right.
float positional_pan(Vec2 position, const AudioListener& listener);

/// Volumes a player sets: master times one group. play() takes the group.
struct AudioMix {
    float master = 1;
    float sfx = 1;   ///< group 0
    float music = 1; ///< group 1
    float ui = 1;    ///< group 2

    float group(u32 g) const { return g == 1 ? music : g == 2 ? ui : sfx; }
    void reflect(Visitor& v) {
        v.field("master", master, {.min = 0, .max = 1});
        v.field("sfx", sfx, {.min = 0, .max = 1});
        v.field("music", music, {.min = 0, .max = 1});
        v.field("ui", ui, {.min = 0, .max = 1});
    }
};

constexpr u32 kAudioSfx = 0;
constexpr u32 kAudioMusic = 1;
constexpr u32 kAudioUi = 2;

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
    /// Load a WAV, Ogg Vorbis or cooked sound. Cached by path, re-read when
    /// the file changes. load_wav() is the same call under its old name.
    Sound* load_sound(const std::string& path);
    Sound* load_sound(const AssetRef& ref);
    Sound* load_wav(const std::string& path) { return load_sound(path); }
    Sound* load_wav(const AssetRef& ref) { return load_sound(ref); }

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

    /// Start playing. `volume` is linear, 1 = as recorded; the mix's master
    /// and group volumes multiply in. Returns a voice id (0 = not started).
    u32 play(const Sound& sound, float volume = 1.0f, u32 group = kAudioSfx);
    u32 play(const Sound* sound, float volume = 1.0f, u32 group = kAudioSfx) {
        return sound ? play(*sound, volume, group) : 0;
    }

    /// Play at a world position: the distance to the listener sets the
    /// volume (followed as the listener or the voice moves), the direction
    /// the pan (fixed when the voice starts). Out of range = not started.
    u32 play_at(const Sound& sound, Vec2 position, float volume = 1.0f, u32 group = kAudioSfx);
    u32 play_at(const Sound* sound, Vec2 position, float volume = 1.0f, u32 group = kAudioSfx) {
        return sound ? play_at(*sound, position, volume, group) : 0;
    }
    void set_listener(const AudioListener& listener);
    const AudioListener& listener() const { return listener_; }
    /// A moving source: re-evaluates the distance volume.
    void move_voice(u32 voice, Vec2 position);
    void stop(u32 voice);
    bool playing(u32 voice) const;

    /// Stop every voice at once.
    void stop_all();

    void set_master_volume(float volume);
    float master_volume() const { return master_; }
    void set_mix(const AudioMix& mix) {
        mix_ = mix;
        master_ = mix.master < 0 ? 0 : mix.master > 1 ? 1 : mix.master;
    }
    const AudioMix& mix() const { return mix_; }

    /// Call once per frame: releases voices that have finished.
    void update();

    int active_voices() const { return static_cast<int>(voices_.size()); }
    int max_voices() const { return max_voices_; }
    void set_max_voices(int n) { max_voices_ = n < 1 ? 1 : n; }

private:
    u32 device_ = 0;
    float master_ = 1.0f;
    AudioMix mix_;
    int max_voices_ = 32;
    struct Voice {
        SDL_AudioStream* stream;
        u32 id;
        bool positional;
        Vec2 position;
        float volume;
        u32 group;
    };
    u32 start_voice(const Sound& sound, float gain, float pan, bool positional, Vec2 position, float volume, u32 group);
    float base_gain(float volume, u32 group) const;
    std::vector<Voice> voices_;
    AudioListener listener_;
    u32 next_voice_ = 1;
    std::unordered_map<std::string, std::unique_ptr<Sound>> sounds_;
    std::unordered_set<std::string> stale_files_;
    std::unordered_map<std::string, std::string> aliases_; ///< resolved path -> name the game used
    FileWatcher* watcher_ = nullptr;
};

} // namespace rw
