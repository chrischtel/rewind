#include "rewind/assets/png.h"

#include <cstdio>
#include <cstring>

namespace rw {

namespace {

u32 crc_table[256];
bool crc_ready = false;

void init_crc() {
    if (crc_ready) return;
    for (u32 n = 0; n < 256; ++n) {
        u32 c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = true;
}

u32 crc32(const u8* data, usize size, u32 crc = 0xFFFFFFFFu) {
    init_crc();
    for (usize i = 0; i < size; ++i) crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put_u32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) {
    put_u32(out, static_cast<u32>(data.size()));
    std::vector<u8> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_u32(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

} // namespace

bool encode_png(int width, int height, const u8* rgba, std::vector<u8>& out) {
    if (width <= 0 || height <= 0 || !rgba) return false;
    out.clear();
    const u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), signature, signature + 8);

    std::vector<u8> ihdr;
    put_u32(ihdr, static_cast<u32>(width));
    put_u32(ihdr, static_cast<u32>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);

    // Raw scanlines with filter byte 0, wrapped in zlib stored blocks.
    std::vector<u8> raw;
    const usize row = static_cast<usize>(width) * 4;
    raw.reserve((row + 1) * static_cast<usize>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + static_cast<usize>(y) * row, rgba + static_cast<usize>(y + 1) * row);
    }
    std::vector<u8> z;
    z.push_back(0x78);
    z.push_back(0x01);
    usize pos = 0;
    do {
        const usize len = std::min<usize>(65535, raw.size() - pos);
        const bool last = pos + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<u8>(len));
        z.push_back(static_cast<u8>(len >> 8));
        z.push_back(static_cast<u8>(~len));
        z.push_back(static_cast<u8>((~len) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    } while (pos < raw.size());
    u32 a = 1, b = 0;
    for (u8 v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put_u32(z, (b << 16) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return true;
}

bool write_png(const std::string& path, int width, int height, const u8* rgba, std::string* error) {
    std::vector<u8> bytes;
    if (!encode_png(width, height, rgba, bytes)) {
        if (error) *error = "invalid image";
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = "could not write " + path;
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

} // namespace rw
