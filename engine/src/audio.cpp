#include "rewind/audio/audio.h"
#include "rewind/core/log.h"

#include "rewind/core/file_watcher.h"
#include "rewind/core/paths.h"

#include <SDL3/SDL.h>

#include <algorithm>
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

Sound* Audio::load_wav(const std::string& path) {
    Sound* existing = find_sound(path);
    const bool stale = stale_files_.erase(path) > 0;
    if (existing && !stale) return existing;

    SDL_AudioSpec spec{};
    u8* buffer = nullptr;
    u32 length = 0;
    const std::string file = resolve_asset_path(path);
    if (!SDL_LoadWAV(file.c_str(), &spec, &buffer, &length)) {
        RW_LOG_INFO("audio", "load_wav(%s): %s", path.c_str(), SDL_GetError());
        return existing; // keep the last good version
    }
    Sound* sound = existing;
    if (!sound) {
        auto owned = std::make_unique<Sound>();
        sound = owned.get();
        sounds_[path] = std::move(owned);
    }
    sound->sample_rate = spec.freq;
    sound->channels = spec.channels;
    sound->format = spec.format;
    sound->data.assign(buffer, buffer + length);
    SDL_free(buffer);
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

void Audio::play(const Sound& sound, float volume) {
    if (!device_ || sound.data.empty()) return;

    if (static_cast<int>(voices_.size()) >= max_voices_) {
        // Steal the oldest voice.
        SDL_DestroyAudioStream(voices_.front());
        voices_.erase(voices_.begin());
    }

    const SDL_AudioSpec src{static_cast<SDL_AudioFormat>(sound.format), sound.channels, sound.sample_rate};
    SDL_AudioSpec dst{};
    SDL_GetAudioDeviceFormat(device_, &dst, nullptr);

    SDL_AudioStream* stream = SDL_CreateAudioStream(&src, &dst);
    if (!stream) {
        RW_LOG_ERROR("audio", "audio: SDL_CreateAudioStream failed: %s", SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(stream, std::max(0.0f, volume) * master_);
    SDL_PutAudioStreamData(stream, sound.data.data(), static_cast<int>(sound.data.size()));
    SDL_FlushAudioStream(stream);
    if (!SDL_BindAudioStream(device_, stream)) {
        RW_LOG_ERROR("audio", "audio: SDL_BindAudioStream failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return;
    }
    voices_.push_back(stream);
}

void Audio::stop_all() {
    for (SDL_AudioStream* s : voices_) SDL_DestroyAudioStream(s);
    voices_.clear();
}

void Audio::set_master_volume(float volume) {
    master_ = std::clamp(volume, 0.0f, 1.0f);
    // Existing voices keep their gain; new ones pick up the change. Simple and predictable.
}

void Audio::update() {
    for (usize i = 0; i < voices_.size();) {
        SDL_AudioStream* s = voices_[i];
        if (SDL_GetAudioStreamQueued(s) == 0 && SDL_GetAudioStreamAvailable(s) == 0) {
            SDL_DestroyAudioStream(s);
            voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

} // namespace rw
