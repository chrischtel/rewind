#include "rewind/assets/aseprite.h"

#include "rewind/core/paths.h"

#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rw {

namespace {

// Chunk types from the Aseprite file format specification.
constexpr u16 kChunkOldPalette256 = 0x0004;
constexpr u16 kChunkOldPalette64 = 0x0011;
constexpr u16 kChunkLayer = 0x2004;
constexpr u16 kChunkCel = 0x2005;
constexpr u16 kChunkTags = 0x2018;
constexpr u16 kChunkPalette = 0x2019;
constexpr u16 kChunkSlice = 0x2022;
constexpr u16 kChunkTileset = 0x2023;

constexpr u16 kFileMagic = 0xA5E0;
constexpr u16 kFrameMagic = 0xF1FA;

constexpr u16 kLayerVisible = 1;
constexpr u32 kHeaderLayerOpacityValid = 1;

constexpr u32 kTilesetExternal = 1;
constexpr u32 kTilesetEmbedded = 2;

struct Cursor {
    const u8* p;
    const u8* end;
    bool ok = true;

    usize remaining() const { return static_cast<usize>(end - p); }

    bool need(usize n) {
        if (remaining() < n) {
            ok = false;
            p = end;
            return false;
        }
        return true;
    }

    u8 byte() {
        if (!need(1)) return 0;
        return *p++;
    }
    u16 word() {
        if (!need(2)) return 0;
        const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
        p += 2;
        return v;
    }
    i16 short_() { return static_cast<i16>(word()); }
    u32 dword() {
        if (!need(4)) return 0;
        const u32 v = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                      (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
        p += 4;
        return v;
    }
    i32 long_() { return static_cast<i32>(dword()); }
    std::string string() {
        const u16 len = word();
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        return s;
    }
    void skip(usize n) {
        if (need(n)) p += n;
    }
};

struct Rgba {
    u8 r = 0, g = 0, b = 0, a = 0;
};

/// A decoded cel: where it sits on the canvas and its pixels, or a link to
/// the frame whose cel on the same layer should be used instead, or a grid
/// of tile ids for tilemap layers.
struct Cel {
    bool present = false;
    i32 link = -1;
    i32 x = 0, y = 0;
    int w = 0, h = 0;
    u8 opacity = 255;
    bool tilemap = false;
    std::vector<u8> rgba;
    std::vector<u32> tiles;
};

/// Inflate a zlib stream with stb_image's decoder.
bool inflate(const u8* data, usize size, usize expected, std::vector<u8>& out) {
    int out_len = 0;
    char* raw = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(data),
                                                             static_cast<int>(size), static_cast<int>(expected),
                                                             &out_len, 1);
    if (!raw) return false;
    out.assign(reinterpret_cast<u8*>(raw), reinterpret_cast<u8*>(raw) + out_len);
    free(raw);
    return true;
}

struct Parser {
    AseDocument& doc;
    std::string& error;
    u16 depth = 32;
    u8 transparent_index = 0;
    u32 header_flags = 0;
    std::vector<Rgba> palette = std::vector<Rgba>(256);
    std::vector<std::vector<Cel>> cels; ///< [frame][layer]

    bool fail(const char* what) {
        error = what;
        return false;
    }

    bool decode_pixels(const u8* src, usize src_len, int w, int h, std::vector<u8>& out) {
        const usize count = static_cast<usize>(w) * static_cast<usize>(h);
        const usize bpp = depth / 8;
        if (src_len < count * bpp) return fail("pixel data is truncated");
        out.resize(count * 4);
        for (usize i = 0; i < count; ++i) {
            u8* d = &out[i * 4];
            switch (depth) {
            case 32:
                d[0] = src[i * 4 + 0];
                d[1] = src[i * 4 + 1];
                d[2] = src[i * 4 + 2];
                d[3] = src[i * 4 + 3];
                break;
            case 16:
                d[0] = d[1] = d[2] = src[i * 2];
                d[3] = src[i * 2 + 1];
                break;
            case 8: {
                const u8 index = src[i];
                if (index == transparent_index) {
                    d[0] = d[1] = d[2] = d[3] = 0;
                } else {
                    const Rgba& c = palette[index];
                    d[0] = c.r;
                    d[1] = c.g;
                    d[2] = c.b;
                    d[3] = c.a;
                }
                break;
            }
            default: return fail("unsupported color depth");
            }
        }
        return true;
    }

    bool read_layer(Cursor c) {
        AseLayer layer;
        const u16 flags = c.word();
        layer.type = c.word();
        layer.child_level = c.word();
        c.word(); // default width, unused
        c.word(); // default height, unused
        c.word(); // blend mode: only Normal is honoured
        layer.opacity = c.byte();
        c.skip(3);
        layer.name = c.string();
        if (layer.type == 2) layer.tileset = c.dword();
        layer.visible = (flags & kLayerVisible) != 0;
        if ((header_flags & kHeaderLayerOpacityValid) == 0) layer.opacity = 255;
        if (!c.ok) return fail("layer chunk is truncated");
        doc.layers.push_back(std::move(layer));
        return true;
    }

    bool read_cel(Cursor c, usize frame) {
        const u16 layer_index = c.word();
        Cel cel;
        cel.present = true;
        cel.x = c.short_();
        cel.y = c.short_();
        cel.opacity = c.byte();
        const u16 type = c.word();
        c.short_(); // z-index
        c.skip(5);
        if (!c.ok) return fail("cel chunk is truncated");

        if (layer_index >= doc.layers.size()) return fail("cel refers to a layer that does not exist");
        std::vector<Cel>& row = cels[frame];
        if (row.size() < doc.layers.size()) row.resize(doc.layers.size());

        switch (type) {
        case 0: { // raw image
            cel.w = c.word();
            cel.h = c.word();
            if (!c.ok) return fail("raw cel is truncated");
            if (!decode_pixels(c.p, c.remaining(), cel.w, cel.h, cel.rgba)) return false;
            break;
        }
        case 1: { // linked cel
            cel.link = c.word();
            if (!c.ok) return fail("linked cel is truncated");
            break;
        }
        case 2: { // zlib-compressed image
            cel.w = c.word();
            cel.h = c.word();
            if (!c.ok) return fail("compressed cel is truncated");
            std::vector<u8> raw;
            const usize expected = static_cast<usize>(cel.w) * cel.h * (depth / 8);
            if (!inflate(c.p, c.remaining(), expected, raw)) return fail("cel zlib data could not be decompressed");
            if (!decode_pixels(raw.data(), raw.size(), cel.w, cel.h, cel.rgba)) return false;
            break;
        }
        case 3: { // zlib-compressed tilemap
            cel.tilemap = true;
            cel.w = c.word();
            cel.h = c.word();
            const u16 bits_per_tile = c.word();
            const u32 id_mask = c.dword();
            const u32 xflip_mask = c.dword();
            const u32 yflip_mask = c.dword();
            const u32 dflip_mask = c.dword();
            c.skip(10);
            if (!c.ok) return fail("tilemap cel is truncated");
            if (bits_per_tile != 32) return fail("only 32-bit tilemap cells are supported");
            const usize count = static_cast<usize>(cel.w) * cel.h;
            std::vector<u8> raw;
            if (!inflate(c.p, c.remaining(), count * 4, raw)) return fail("tilemap zlib data could not be decompressed");
            if (raw.size() < count * 4) return fail("tilemap data is truncated");
            cel.tiles.resize(count);
            for (usize i = 0; i < count; ++i) {
                const u32 v = static_cast<u32>(raw[i * 4]) | (static_cast<u32>(raw[i * 4 + 1]) << 8) |
                              (static_cast<u32>(raw[i * 4 + 2]) << 16) | (static_cast<u32>(raw[i * 4 + 3]) << 24);
                u32 cellv = v & id_mask;
                if (v & xflip_mask) cellv |= 0x80000000u;
                if (v & yflip_mask) cellv |= 0x40000000u;
                if (v & dflip_mask) cellv |= 0x20000000u;
                cel.tiles[i] = cellv;
            }
            break;
        }
        default: return fail("unknown cel type");
        }

        row[layer_index] = std::move(cel);
        return true;
    }

    bool read_tileset(Cursor c) {
        const u32 id = c.dword();
        const u32 flags = c.dword();
        AseTileset ts;
        ts.tile_count = c.dword();
        ts.tile_width = c.word();
        ts.tile_height = c.word();
        c.short_(); // base index (for display)
        c.skip(14);
        ts.name = c.string();
        if (flags & kTilesetExternal) {
            c.dword();
            c.dword();
        }
        if (flags & kTilesetEmbedded) {
            const u32 compressed = c.dword();
            if (!c.ok || compressed > c.remaining()) return fail("tileset chunk is truncated");
            std::vector<u8> raw;
            const usize expected = static_cast<usize>(ts.tile_width) * ts.tile_height * ts.tile_count * (depth / 8);
            if (!inflate(c.p, compressed, expected, raw)) return fail("tileset zlib data could not be decompressed");
            if (!decode_pixels(raw.data(), raw.size(), ts.tile_width,
                               ts.tile_height * static_cast<int>(ts.tile_count), ts.rgba))
                return false;
        }
        if (!c.ok) return fail("tileset chunk is truncated");
        if (doc.tilesets.size() <= id) doc.tilesets.resize(static_cast<usize>(id) + 1);
        doc.tilesets[id] = std::move(ts);
        return true;
    }

    bool read_tags(Cursor c) {
        const u16 count = c.word();
        c.skip(8);
        for (u16 i = 0; i < count; ++i) {
            AseTag tag;
            tag.from = c.word();
            tag.to = c.word();
            tag.direction = c.byte();
            tag.repeat = c.word();
            c.skip(6);
            c.skip(3); // deprecated tag color
            c.skip(1);
            tag.name = c.string();
            if (!c.ok) return fail("tags chunk is truncated");
            doc.tags.push_back(std::move(tag));
        }
        return true;
    }

    bool read_palette(Cursor c) {
        c.dword(); // new palette size
        const u32 first = c.dword();
        const u32 last = c.dword();
        c.skip(8);
        for (u32 i = first; i <= last && c.ok; ++i) {
            const u16 flags = c.word();
            Rgba color;
            color.r = c.byte();
            color.g = c.byte();
            color.b = c.byte();
            color.a = c.byte();
            if (flags & 1) c.string();
            if (i < palette.size()) palette[i] = color;
        }
        return c.ok || fail("palette chunk is truncated");
    }

    bool read_old_palette(Cursor c, bool six_bit) {
        const u16 packets = c.word();
        u32 index = 0;
        for (u16 p = 0; p < packets && c.ok; ++p) {
            index += c.byte();
            u32 count = c.byte();
            if (count == 0) count = 256;
            for (u32 i = 0; i < count && c.ok; ++i) {
                Rgba color;
                color.r = c.byte();
                color.g = c.byte();
                color.b = c.byte();
                color.a = 255;
                if (six_bit) {
                    color.r = static_cast<u8>(color.r << 2);
                    color.g = static_cast<u8>(color.g << 2);
                    color.b = static_cast<u8>(color.b << 2);
                }
                if (index < palette.size()) palette[index] = color;
                ++index;
            }
        }
        return c.ok || fail("old palette chunk is truncated");
    }

    bool read_slice(Cursor c) {
        const u32 keys = c.dword();
        const u32 flags = c.dword();
        c.dword();
        AseSlice slice;
        slice.name = c.string();
        for (u32 k = 0; k < keys && c.ok; ++k) {
            c.dword(); // frame number of this key
            const i32 x = c.long_();
            const i32 y = c.long_();
            const u32 w = c.dword();
            const u32 h = c.dword();
            if (flags & 1) { // 9-patch center, unused
                c.long_();
                c.long_();
                c.dword();
                c.dword();
            }
            i32 px = 0, py = 0;
            if (flags & 2) {
                px = c.long_();
                py = c.long_();
            }
            if (k == 0) {
                slice.x = x;
                slice.y = y;
                slice.w = w;
                slice.h = h;
                slice.has_pivot = (flags & 2) != 0;
                slice.pivot_x = px;
                slice.pivot_y = py;
            }
        }
        if (!c.ok) return fail("slice chunk is truncated");
        doc.slices.push_back(std::move(slice));
        return true;
    }

    /// A layer is drawn only if it and every group above it are visible.
    bool layer_effectively_visible(usize index) const {
        int level = doc.layers[index].child_level;
        if (!doc.layers[index].visible) return false;
        for (usize i = index; i-- > 0 && level > 0;) {
            if (doc.layers[i].child_level < level) {
                if (!doc.layers[i].visible) return false;
                level = doc.layers[i].child_level;
            }
        }
        return true;
    }

    const Cel* resolve(usize frame, usize layer) const {
        for (int guard = 0; guard < 64; ++guard) {
            if (frame >= cels.size() || layer >= cels[frame].size()) return nullptr;
            const Cel& cel = cels[frame][layer];
            if (!cel.present) return nullptr;
            if (cel.link < 0) return &cel;
            frame = static_cast<usize>(cel.link);
        }
        return nullptr;
    }

    static void blend_pixel(u8* dst, const u8* src, u32 alpha) {
        const u32 sa = src[3] * alpha / 255;
        if (sa == 0) return;
        const u32 da = dst[3];
        const u32 out_a = sa + da * (255 - sa) / 255;
        if (out_a == 0) return;
        for (int i = 0; i < 3; ++i) {
            const u32 s = src[i] * sa;
            const u32 d = dst[i] * da * (255 - sa) / 255;
            dst[i] = static_cast<u8>((s + d) / out_a);
        }
        dst[3] = static_cast<u8>(out_a);
    }

    void blit(AseFrame& out, const u8* src, int src_w, int src_h, int src_stride_px, i32 at_x, i32 at_y,
              u32 alpha, bool flip_x, bool flip_y) {
        for (int cy = 0; cy < src_h; ++cy) {
            const i32 y = at_y + cy;
            if (y < 0 || y >= doc.height) continue;
            const int sy = flip_y ? src_h - 1 - cy : cy;
            for (int cx = 0; cx < src_w; ++cx) {
                const i32 x = at_x + cx;
                if (x < 0 || x >= doc.width) continue;
                const int sx = flip_x ? src_w - 1 - cx : cx;
                const u8* s = &src[(static_cast<usize>(sy) * src_stride_px + sx) * 4];
                u8* d = &out.rgba[(static_cast<usize>(y) * doc.width + x) * 4];
                blend_pixel(d, s, alpha);
            }
        }
    }

    void composite(usize frame) {
        AseFrame& out = doc.frames[frame];
        out.rgba.assign(static_cast<usize>(doc.width) * doc.height * 4, 0);
        for (usize layer = 0; layer < doc.layers.size(); ++layer) {
            const AseLayer& l = doc.layers[layer];
            if (l.type == 1 || !layer_effectively_visible(layer)) continue;
            const Cel* cel = resolve(frame, layer);
            if (!cel) continue;
            const u32 alpha = static_cast<u32>(cel->opacity) * l.opacity / 255;

            if (!cel->tilemap) {
                blit(out, cel->rgba.data(), cel->w, cel->h, cel->w, cel->x, cel->y, alpha, false, false);
                continue;
            }

            if (l.tileset >= doc.tilesets.size()) continue;
            const AseTileset& ts = doc.tilesets[l.tileset];
            if (ts.rgba.empty()) continue;
            for (int ty = 0; ty < cel->h; ++ty) {
                for (int tx = 0; tx < cel->w; ++tx) {
                    const u32 v = cel->tiles[static_cast<usize>(ty) * cel->w + tx];
                    const u32 id = v & 0x1fffffffu;
                    if (id == 0 || id >= ts.tile_count) continue;
                    const u8* tile = &ts.rgba[static_cast<usize>(id) * ts.tile_width * ts.tile_height * 4];
                    blit(out, tile, ts.tile_width, ts.tile_height, ts.tile_width, cel->x + tx * ts.tile_width,
                         cel->y + ty * ts.tile_height, alpha, (v & 0x80000000u) != 0, (v & 0x40000000u) != 0);
                }
            }
        }
    }

    void collect_tilemaps() {
        for (usize layer = 0; layer < doc.layers.size(); ++layer) {
            const AseLayer& l = doc.layers[layer];
            if (l.type != 2) continue;
            const Cel* cel = resolve(0, layer);
            AseTilemap tm;
            tm.layer_name = l.name;
            tm.layer = layer;
            tm.tileset = l.tileset;
            if (cel && cel->tilemap) {
                tm.width = cel->w;
                tm.height = cel->h;
                tm.cells = cel->tiles;
                // Cels may be offset from the canvas origin; keep the grid as authored.
            }
            doc.tilemaps.push_back(std::move(tm));
        }
    }

    bool parse(const u8* data, usize size) {
        Cursor c{data, data + size};
        c.dword(); // file size
        if (c.word() != kFileMagic) return fail("not an Aseprite file (bad magic)");
        const u16 frame_count = c.word();
        doc.width = c.word();
        doc.height = c.word();
        depth = c.word();
        header_flags = c.dword();
        c.word();  // speed (deprecated)
        c.dword(); // 0
        c.dword(); // 0
        transparent_index = c.byte();
        c.skip(3);
        c.word(); // number of colors
        c.byte(); // pixel width
        c.byte(); // pixel height
        c.short_();
        c.short_();
        c.word();
        c.word();
        c.skip(84);
        if (!c.ok) return fail("header is truncated");
        if (depth != 32 && depth != 16 && depth != 8) return fail("unsupported color depth");
        if (doc.width <= 0 || doc.height <= 0) return fail("invalid canvas size");
        if (frame_count == 0) return fail("file has no frames");

        doc.frames.resize(frame_count);
        cels.resize(frame_count);

        for (usize f = 0; f < frame_count; ++f) {
            const u8* frame_start = c.p;
            const u32 frame_bytes = c.dword();
            if (c.word() != kFrameMagic) return fail("bad frame magic");
            const u16 old_chunks = c.word();
            doc.frames[f].duration_ms = c.word();
            c.skip(2);
            const u32 new_chunks = c.dword();
            if (!c.ok) return fail("frame header is truncated");
            const u32 chunk_count = new_chunks ? new_chunks : old_chunks;
            if (frame_bytes < 16 || static_cast<usize>(frame_bytes) > static_cast<usize>(c.end - frame_start))
                return fail("frame size is out of range");
            const u8* frame_end = frame_start + frame_bytes;

            for (u32 k = 0; k < chunk_count; ++k) {
                const u8* chunk_start = c.p;
                const u32 chunk_size = c.dword();
                const u16 type = c.word();
                if (!c.ok) return fail("chunk header is truncated");
                if (chunk_size < 6 || static_cast<usize>(chunk_size) > static_cast<usize>(frame_end - chunk_start))
                    return fail("chunk size is out of range");
                Cursor body{c.p, chunk_start + chunk_size};

                bool ok = true;
                switch (type) {
                case kChunkLayer: ok = f == 0 ? read_layer(body) : true; break;
                case kChunkCel: ok = read_cel(body, f); break;
                case kChunkTags: ok = read_tags(body); break;
                case kChunkPalette: ok = read_palette(body); break;
                case kChunkOldPalette256: ok = read_old_palette(body, false); break;
                case kChunkOldPalette64: ok = read_old_palette(body, true); break;
                case kChunkSlice: ok = read_slice(body); break;
                case kChunkTileset: ok = read_tileset(body); break;
                default: break; // user data, color profile, external files...: not needed
                }
                if (!ok) return false;
                c.p = chunk_start + chunk_size;
            }
            c.p = frame_end;
        }

        for (usize f = 0; f < frame_count; ++f) composite(f);
        collect_tilemaps();
        return true;
    }
};

} // namespace

bool parse_aseprite(const u8* data, usize size, AseDocument& out, std::string& error) {
    out = AseDocument{};
    error.clear();
    Parser parser{out, error};
    if (!parser.parse(data, size)) {
        out = AseDocument{};
        return false;
    }
    return true;
}

bool load_aseprite(const std::string& path, AseDocument& out, std::string& error) {
    std::FILE* file = std::fopen(resolve_asset_path(path).c_str(), "rb");
    if (!file) {
        error = "could not open " + path;
        return false;
    }
    std::vector<u8> bytes;
    u8 buffer[64 * 1024];
    usize n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(file);
    return parse_aseprite(bytes.data(), bytes.size(), out, error);
}

} // namespace rw
