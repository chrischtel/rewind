#include "rewind/audio/audio.h"
#include "rewind/assets/audio_decode.h"
#include "rewind/core/log.h"

#include "rewind/core/file_watcher.h"
#include "rewind/core/paths.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rw {

float Sound::seconds() const {
    const int frame_bytes = SDL_AUDIO_BYTESIZE(static_cast<SDL_AudioFormat>(format)) * channels;
    if (frame_bytes <= 0 || sample_rate <= 0) return 0;
    return static_cast<float>(data.size() / static_cast<usize>(frame_bytes)) / static_cast<float>(sample_rate);
}

Audio::~Audio() { shutdown(); }

bool Audio::init() {
    if (device_) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        RW_LOG_ERROR("audio", "audio: SDL_InitSubSystem failed: %s", SDL_GetError());
        return false;
    }
    const SDL_AudioSpec want{SDL_AUDIO_F32, 2, 48000};
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
    if (!device_) {
        RW_LOG_ERROR("audio", "audio: could not open the default playback device: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec got{};
    int frames = 0;
    SDL_GetAudioDeviceFormat(device_, &got, &frames);
    RW_LOG_INFO("audio", "audio: %d Hz, %d channels, %d frames per buffer", got.freq, got.channels, frames);
    return true;
}

void Audio::shutdown() {
    stop_all();
    sounds_.clear();
    if (device_) SDL_CloseAudioDevice(device_);
    device_ = 0;
}

Sound* Audio::find_sound(const std::string& name) const {
    auto it = sounds_.find(name);
    return it == sounds_.end() ? nullptr : it->second.get();
}

Sound* Audio::load_sound(const AssetRef& ref) {
    const std::string file = resolve_asset(ref);
    if (file.empty()) {
        RW_LOG_WARN("audio", "load_sound: asset reference '%s' (%s) resolves to nothing", ref.path.c_str(), ref.id.c_str());
        return nullptr;
    }
    return load_sound(file);
}

Sound* Audio::load_sound(const std::string& path) {
    Sound* existing = find_sound(path);
    const bool stale = stale_files_.erase(path) > 0;
    if (existing && !stale) return existing;

    const std::string file = resolve_asset_path(path);
    std::vector<u8> bytes;
    std::string error;
    DecodedAudio decoded;
    if (!read_asset_bytes(path, bytes)) {
        RW_LOG_INFO("audio", "load_sound(%s): could not open %s", path.c_str(), file.c_str());
        return existing;
    }
    if (!decode_audio(bytes, decoded, error)) {
        RW_LOG_INFO("audio", "load_sound(%s): %s", path.c_str(), error.c_str());
        return existing; // keep the last good version
    }
    Sound* sound = existing;
    if (!sound) {
        auto owned = std::make_unique<Sound>();
        sound = owned.get();
        sounds_[path] = std::move(owned);
    }
    sound->sample_rate = decoded.sample_rate;
    sound->channels = decoded.channels;
    sound->format = SDL_AUDIO_S16;
    sound->data.resize(decoded.samples.size() * sizeof(i16));
    if (!decoded.samples.empty()) std::memcpy(sound->data.data(), decoded.samples.data(), sound->data.size());
    if (watcher_) watcher_->track(file);
    if (file != path) aliases_[file] = path;
    return sound;
}

Sound* Audio::create_sound(const std::string& name, int sample_rate, int channels, const float* samples,
                           usize sample_count) {
    auto it = sounds_.find(name);
    Sound* sound;
    if (it != sounds_.end()) {
        sound = it->second.get();
    } else {
        auto owned = std::make_unique<Sound>();
        sound = owned.get();
        sounds_[name] = std::move(owned);
    }
    sound->sample_rate = sample_rate;
    sound->channels = channels;
    sound->format = SDL_AUDIO_F32;
    sound->data.resize(sample_count * sizeof(float));
    if (sample_count) std::memcpy(sound->data.data(), samples, sample_count * sizeof(float));
    return sound;
}

float positional_gain(Vec2 position, const AudioListener& l) {
    const float d = distance(position, l.position);
    if (d <= l.near_distance) return 1;
    if (d >= l.max_distance || l.max_distance <= l.near_distance) return 0;
    return 1 - (d - l.near_distance) / (l.max_distance - l.near_distance);
}

float positional_pan(Vec2 position, const AudioListener& l) {
    const float half = std::max(l.max_distance * 0.5f, 1.0f);
    return clampf((position.x - l.position.x) / half, -1, 1);
}

float Audio::base_gain(float volume, u32 group) const {
    return std::max(0.0f, volume) * master_ * std::clamp(mix_.group(group), 0.0f, 1.0f);
}

u32 Audio::start_voice(const Sound& sound, float gain, float pan, bool positional, Vec2 position, float volume, u32 group) {
    if (!device_ || sound.data.empty()) return 0;

    if (static_cast<int>(voices_.size()) >= max_voices_) {
        // Steal the oldest voice.
        SDL_DestroyAudioStream(voices_.front().stream);
        voices_.erase(voices_.begin());
    }

    const SDL_AudioSpec src{static_cast<SDL_AudioFormat>(sound.format), sound.channels, sound.sample_rate};
    SDL_AudioSpec dst{};
    SDL_GetAudioDeviceFormat(device_, &dst, nullptr);

    SDL_AudioStream* stream = nullptr;
    if (std::fabs(pan) > 0.001f) {
        // Constant-power pan baked into a stereo float copy of the sound.
        const SDL_AudioSpec stereo{SDL_AUDIO_F32, 2, sound.sample_rate};
        u8* converted = nullptr;
        int len = 0;
        if (SDL_ConvertAudioSamples(&src, sound.data.data(), static_cast<int>(sound.data.size()), &stereo, &converted, &len)) {
            float* f = reinterpret_cast<float*>(converted);
            const usize frames = static_cast<usize>(len) / (2 * sizeof(float));
            const float angle = (pan + 1) * 0.78539816f; // 0..pi/2
            const float left = std::cos(angle), right = std::sin(angle);
            for (usize i = 0; i < frames; ++i) {
                f[2 * i] *= left;
                f[2 * i + 1] *= right;
            }
            stream = SDL_CreateAudioStream(&stereo, &dst);
            if (stream) SDL_PutAudioStreamData(stream, converted, len);
            SDL_free(converted);
        }
    }
    if (!stream) {
        stream = SDL_CreateAudioStream(&src, &dst);
        if (!stream) {
            RW_LOG_ERROR("audio", "audio: SDL_CreateAudioStream failed: %s", SDL_GetError());
            return 0;
        }
        SDL_PutAudioStreamData(stream, sound.data.data(), static_cast<int>(sound.data.size()));
    }
    SDL_SetAudioStreamGain(stream, gain);
    SDL_FlushAudioStream(stream);
    if (!SDL_BindAudioStream(device_, stream)) {
        RW_LOG_ERROR("audio", "audio: SDL_BindAudioStream failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return 0;
    }
    const Voice v{stream, next_voice_++, positional, position, volume, group};
    voices_.push_back(v);
    return v.id;
}

u32 Audio::play(const Sound& sound, float volume, u32 group) {
    return start_voice(sound, base_gain(volume, group), 0, false, {}, volume, group);
}

u32 Audio::play_at(const Sound& sound, Vec2 position, float volume, u32 group) {
    const float g = positional_gain(position, listener_);
    if (g <= 0) return 0;
    return start_voice(sound, base_gain(volume, group) * g, positional_pan(position, listener_), true, position, volume, group);
}

void Audio::set_listener(const AudioListener& listener) {
    listener_ = listener;
    for (Voice& v : voices_)
        if (v.positional) SDL_SetAudioStreamGain(v.stream, base_gain(v.volume, v.group) * positional_gain(v.position, listener_));
}

void Audio::move_voice(u32 voice, Vec2 position) {
    for (Voice& v : voices_) {
        if (v.id != voice) continue;
        v.position = position;
        if (v.positional) SDL_SetAudioStreamGain(v.stream, base_gain(v.volume, v.group) * positional_gain(position, listener_));
    }
}

void Audio::stop(u32 voice) {
    for (usize i = 0; i < voices_.size(); ++i) {
        if (voices_[i].id != voice) continue;
        SDL_DestroyAudioStream(voices_[i].stream);
        voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

bool Audio::playing(u32 voice) const {
    for (const Voice& v : voices_)
        if (v.id == voice) return true;
    return false;
}

void Audio::stop_all() {
    for (const Voice& v : voices_) SDL_DestroyAudioStream(v.stream);
    voices_.clear();
}

void Audio::set_master_volume(float volume) {
    master_ = std::clamp(volume, 0.0f, 1.0f);
    // Existing voices keep their gain; new ones pick up the change. Simple and predictable.
}

void Audio::update() {
    for (usize i = 0; i < voices_.size();) {
        SDL_AudioStream* s = voices_[i].stream;
        if (SDL_GetAudioStreamQueued(s) == 0 && SDL_GetAudioStreamAvailable(s) == 0) {
            SDL_DestroyAudioStream(s);
            voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

} // namespace rw
