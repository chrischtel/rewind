// Headless checks for audio decoding and cooking: WAV in memory, cooked
// round trip, downmix, bad input.

#include <rewind/assets/audio_decode.h>
#include <rewind/project/cooked.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

void put32(std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void put16(std::vector<u8>& b, u16 v) {
    b.push_back(static_cast<u8>(v));
    b.push_back(static_cast<u8>(v >> 8));
}

/// A canonical 16-bit PCM WAV.
std::vector<u8> make_wav(int rate, int channels, const std::vector<i16>& samples) {
    std::vector<u8> b;
    const u32 data_bytes = static_cast<u32>(samples.size() * 2);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, 36 + data_bytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, 1); // PCM
    put16(b, static_cast<u16>(channels));
    put32(b, static_cast<u32>(rate));
    put32(b, static_cast<u32>(rate * channels * 2));
    put16(b, static_cast<u16>(channels * 2));
    put16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, data_bytes);
    for (i16 s : samples) put16(b, static_cast<u16>(s));
    return b;
}

void test_wav_and_cook() {
    const std::vector<i16> stereo = {1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000};
    const std::vector<u8> wav = make_wav(22050, 2, stereo);
    DecodedAudio d;
    std::string error;
    CHECK(decode_audio(wav, d, error));
    CHECK(d.sample_rate == 22050 && d.channels == 2 && d.frames() == 4);
    CHECK(d.samples == stereo);
    CHECK(d.seconds() > 0.00018f && d.seconds() < 0.00019f);

    // Cook: encode, recognise by magic, decode back identically.
    CookedAudio cooked;
    cooked.sample_rate = d.sample_rate;
    cooked.channels = d.channels;
    cooked.samples = d.samples;
    cooked.source_hash = 42;
    std::vector<u8> bytes;
    encode_cooked_audio(cooked, bytes);
    CHECK(cooked_magic(bytes) == kCookedAudioMagic);
    CookedAudio back;
    CHECK(decode_cooked_audio(bytes, back) && back.samples == stereo && back.source_hash == 42);
    DecodedAudio from_cooked;
    CHECK(decode_audio(bytes, from_cooked, error) && from_cooked.samples == stereo && from_cooked.channels == 2);

    // Downmix averages the channels.
    downmix_to_mono(d);
    CHECK(d.channels == 1 && d.frames() == 4);
    CHECK(d.samples == std::vector<i16>({0, 0, 0, 0}));
    DecodedAudio m;
    m.sample_rate = 8000;
    m.channels = 2;
    m.samples = {100, 300, -200, -400};
    downmix_to_mono(m);
    CHECK(m.samples == std::vector<i16>({200, -300}));
    downmix_to_mono(m); // already mono: unchanged
    CHECK(m.channels == 1 && m.samples.size() == 2);

    // Garbage is refused, damaged cooked data too.
    std::vector<u8> junk = {'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!decode_audio(junk, d, error) && !error.empty());
    bytes.resize(bytes.size() - 3);
    CHECK(!decode_cooked_audio(bytes, back));
}

} // namespace

int main() {
    test_wav_and_cook();
    if (failures == 0) std::printf("audio_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
