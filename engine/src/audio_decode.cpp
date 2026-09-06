#include "rewind/assets/audio_decode.h"

#include "rewind/project/cooked.h"

#include <SDL3/SDL.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include <cstring>

namespace rw {

namespace {

bool looks_like_ogg(const std::vector<u8>& b) { return b.size() >= 4 && std::memcmp(b.data(), "OggS", 4) == 0; }
bool looks_like_wav(const std::vector<u8>& b) {
    return b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WAVE", 4) == 0;
}

bool decode_wav(const std::vector<u8>& bytes, DecodedAudio& out, std::string& error) {
    SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (!io) {
        error = SDL_GetError();
        return false;
    }
    SDL_AudioSpec spec{};
    u8* buffer = nullptr;
    u32 length = 0;
    if (!SDL_LoadWAV_IO(io, true, &spec, &buffer, &length)) {
        error = SDL_GetError();
        return false;
    }
    // Whatever the file held, hand out 16-bit samples at the file's rate.
    SDL_AudioSpec dst{SDL_AUDIO_S16, spec.channels, spec.freq};
    u8* converted = nullptr;
    int converted_len = 0;
    const bool ok = SDL_ConvertAudioSamples(&spec, buffer, static_cast<int>(length), &dst, &converted, &converted_len);
    SDL_free(buffer);
    if (!ok) {
        error = SDL_GetError();
        return false;
    }
    out.sample_rate = spec.freq;
    out.channels = spec.channels;
    out.samples.resize(static_cast<usize>(converted_len) / sizeof(i16));
    std::memcpy(out.samples.data(), converted, static_cast<usize>(converted_len));
    SDL_free(converted);
    return true;
}

bool decode_ogg(const std::vector<u8>& bytes, DecodedAudio& out, std::string& error) {
    int channels = 0, rate = 0;
    short* samples = nullptr;
    const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &channels, &rate, &samples);
    if (frames < 0 || !samples) {
        error = "not a valid Ogg Vorbis stream";
        return false;
    }
    out.sample_rate = rate;
    out.channels = channels;
    out.samples.assign(samples, samples + static_cast<usize>(frames) * static_cast<usize>(channels));
    free(samples);
    return true;
}

} // namespace

bool decode_audio(const std::vector<u8>& bytes, DecodedAudio& out, std::string& error) {
    out = DecodedAudio{};
    if (cooked_magic(bytes) == kCookedAudioMagic) {
        CookedAudio cooked;
        if (!decode_cooked_audio(bytes, cooked)) {
            error = "damaged cooked audio";
            return false;
        }
        out.sample_rate = cooked.sample_rate;
        out.channels = cooked.channels;
        out.samples = std::move(cooked.samples);
        return true;
    }
    if (looks_like_ogg(bytes)) return decode_ogg(bytes, out, error);
    if (looks_like_wav(bytes)) return decode_wav(bytes, out, error);
    error = "unknown sound format (WAV, Ogg Vorbis or cooked audio expected)";
    return false;
}

void downmix_to_mono(DecodedAudio& audio) {
    if (audio.channels <= 1) return;
    const usize n = audio.frames();
    const usize c = static_cast<usize>(audio.channels);
    std::vector<i16> mono(n);
    for (usize i = 0; i < n; ++i) {
        i32 sum = 0;
        for (usize k = 0; k < c; ++k) sum += audio.samples[i * c + k];
        mono[i] = static_cast<i16>(sum / static_cast<i32>(c));
    }
    audio.samples = std::move(mono);
    audio.channels = 1;
}

} // namespace rw
