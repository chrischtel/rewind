#pragma once

// Decode sound files into 16-bit PCM: WAV (through SDL), Ogg Vorbis
// (stb_vorbis) and the engine's cooked audio, told apart by content. Used
// by the audio player at load time and by the asset cook.

#include "rewind/core/types.h"

#include <string>
#include <vector>

namespace rw {

struct DecodedAudio {
    int sample_rate = 0;
    int channels = 0;
    std::vector<i16> samples; ///< interleaved

    usize frames() const { return channels > 0 ? samples.size() / static_cast<usize>(channels) : 0; }
    float seconds() const { return sample_rate > 0 ? static_cast<float>(frames()) / static_cast<float>(sample_rate) : 0.0f; }
};

/// Decode a whole file held in memory. Returns false with `error` set.
bool decode_audio(const std::vector<u8>& bytes, DecodedAudio& out, std::string& error);

/// Average the channels into one.
void downmix_to_mono(DecodedAudio& audio);

} // namespace rw
