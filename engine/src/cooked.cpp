#include "rewind/project/cooked.h"

#include "rewind/sim/serialize.h"

#include <cmath>
#include <cstring>

namespace rw {

namespace {
constexpr u32 kVersion = 1;
}

u32 cooked_magic(const std::vector<u8>& bytes) {
    if (bytes.size() < 8) return 0;
    u32 magic = 0;
    std::memcpy(&magic, bytes.data(), 4);
    return magic == kCookedTextureMagic || magic == kCookedSheetMagic || magic == kCookedAudioMagic ? magic : 0;
}

// ---- audio -------------------------------------------------------------------------

void encode_cooked_audio(const CookedAudio& a, std::vector<u8>& out) {
    Writer w;
    w.pod(kCookedAudioMagic);
    w.pod(kVersion);
    w.pod<i32>(a.sample_rate);
    w.pod<i32>(a.channels);
    w.pod(a.source_hash);
    w.vec(a.samples);
    out = std::move(w.bytes());
}

bool decode_cooked_audio(const std::vector<u8>& bytes, CookedAudio& out) {
    Reader r(bytes);
    if (r.pod<u32>() != kCookedAudioMagic || r.pod<u32>() != kVersion) return false;
    out = CookedAudio{};
    out.sample_rate = r.pod<i32>();
    out.channels = r.pod<i32>();
    out.source_hash = r.pod<u64>();
    r.vec(out.samples);
    return r.ok() && out.sample_rate > 0 && out.channels > 0 && out.samples.size() % static_cast<usize>(out.channels) == 0;
}

// ---- texture -----------------------------------------------------------------------

void encode_cooked_texture(const CookedTexture& t, std::vector<u8>& out) {
    Writer w;
    w.pod(kCookedTextureMagic);
    w.pod(kVersion);
    w.pod<i32>(t.width);
    w.pod<i32>(t.height);
    w.pod<u8>(t.nearest ? 1 : 0);
    w.pod(t.source_hash);
    w.vec(t.rgba);
    out = std::move(w.bytes());
}

bool decode_cooked_texture(const std::vector<u8>& bytes, CookedTexture& out) {
    Reader r(bytes);
    if (r.pod<u32>() != kCookedTextureMagic || r.pod<u32>() != kVersion) return false;
    out = CookedTexture{};
    out.width = r.pod<i32>();
    out.height = r.pod<i32>();
    out.nearest = r.pod<u8>() != 0;
    out.source_hash = r.pod<u64>();
    r.vec(out.rgba);
    return r.ok() && out.width > 0 && out.height > 0 &&
           out.rgba.size() == static_cast<usize>(out.width) * static_cast<usize>(out.height) * 4;
}

// ---- sheet -------------------------------------------------------------------------

void encode_cooked_sheet(const CookedSheet& s, std::vector<u8>& out) {
    Writer w;
    w.pod(kCookedSheetMagic);
    w.pod(kVersion);
    w.pod<i32>(s.atlas_width);
    w.pod<i32>(s.atlas_height);
    w.pod<i32>(s.frame_width);
    w.pod<i32>(s.frame_height);
    w.pod<i32>(s.columns);
    w.pod<u8>(s.nearest ? 1 : 0);
    w.pod(s.source_hash);
    w.vec(s.rgba);
    w.vec(s.durations_ms);
    w.pod<u32>(static_cast<u32>(s.tags.size()));
    for (const AseTag& t : s.tags) {
        w.str(t.name);
        w.pod(t.from);
        w.pod(t.to);
        w.pod(t.direction);
        w.pod(t.repeat);
    }
    w.pod<u32>(static_cast<u32>(s.slices.size()));
    for (const AseSlice& sl : s.slices) {
        w.str(sl.name);
        w.pod(sl.x);
        w.pod(sl.y);
        w.pod(sl.w);
        w.pod(sl.h);
        w.pod<u8>(sl.has_pivot ? 1 : 0);
        w.pod(sl.pivot_x);
        w.pod(sl.pivot_y);
    }
    w.pod<u32>(static_cast<u32>(s.tilesets.size()));
    for (const AseTileset& ts : s.tilesets) {
        w.str(ts.name);
        w.pod<i32>(ts.tile_width);
        w.pod<i32>(ts.tile_height);
        w.pod(ts.tile_count);
        w.vec(ts.rgba);
    }
    w.pod<u32>(static_cast<u32>(s.tilemaps.size()));
    for (const AseTilemap& tm : s.tilemaps) {
        w.str(tm.layer_name);
        w.pod<u64>(tm.layer);
        w.pod<u64>(tm.tileset);
        w.pod<i32>(tm.width);
        w.pod<i32>(tm.height);
        w.vec(tm.cells);
    }
    out = std::move(w.bytes());
}

bool decode_cooked_sheet(const std::vector<u8>& bytes, CookedSheet& out) {
    Reader r(bytes);
    if (r.pod<u32>() != kCookedSheetMagic || r.pod<u32>() != kVersion) return false;
    out = CookedSheet{};
    out.atlas_width = r.pod<i32>();
    out.atlas_height = r.pod<i32>();
    out.frame_width = r.pod<i32>();
    out.frame_height = r.pod<i32>();
    out.columns = r.pod<i32>();
    out.nearest = r.pod<u8>() != 0;
    out.source_hash = r.pod<u64>();
    r.vec(out.rgba);
    r.vec(out.durations_ms);
    const u32 tags = r.pod<u32>();
    for (u32 i = 0; i < tags && r.ok(); ++i) {
        AseTag t;
        r.str(t.name);
        r.pod(t.from);
        r.pod(t.to);
        r.pod(t.direction);
        r.pod(t.repeat);
        out.tags.push_back(std::move(t));
    }
    const u32 slices = r.pod<u32>();
    for (u32 i = 0; i < slices && r.ok(); ++i) {
        AseSlice s;
        r.str(s.name);
        r.pod(s.x);
        r.pod(s.y);
        r.pod(s.w);
        r.pod(s.h);
        s.has_pivot = r.pod<u8>() != 0;
        r.pod(s.pivot_x);
        r.pod(s.pivot_y);
        out.slices.push_back(std::move(s));
    }
    const u32 tilesets = r.pod<u32>();
    for (u32 i = 0; i < tilesets && r.ok(); ++i) {
        AseTileset ts;
        r.str(ts.name);
        ts.tile_width = r.pod<i32>();
        ts.tile_height = r.pod<i32>();
        r.pod(ts.tile_count);
        r.vec(ts.rgba);
        out.tilesets.push_back(std::move(ts));
    }
    const u32 tilemaps = r.pod<u32>();
    for (u32 i = 0; i < tilemaps && r.ok(); ++i) {
        AseTilemap tm;
        r.str(tm.layer_name);
        tm.layer = static_cast<usize>(r.pod<u64>());
        tm.tileset = static_cast<usize>(r.pod<u64>());
        tm.width = r.pod<i32>();
        tm.height = r.pod<i32>();
        r.vec(tm.cells);
        out.tilemaps.push_back(std::move(tm));
    }
    return r.ok() && out.atlas_width > 0 && out.atlas_height > 0 &&
           out.rgba.size() == static_cast<usize>(out.atlas_width) * static_cast<usize>(out.atlas_height) * 4;
}

CookedSheet cook_aseprite(const AseDocument& doc, u64 source_hash, bool nearest) {
    CookedSheet s;
    s.source_hash = source_hash;
    s.nearest = nearest;
    s.frame_width = doc.width;
    s.frame_height = doc.height;
    const int count = static_cast<int>(doc.frames.size());
    s.columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))));
    const int rows = (count + s.columns - 1) / s.columns;
    s.atlas_width = s.columns * doc.width;
    s.atlas_height = rows * doc.height;
    s.rgba.assign(static_cast<usize>(s.atlas_width) * s.atlas_height * 4, 0);
    for (int i = 0; i < count; ++i) {
        const int ox = (i % s.columns) * doc.width;
        const int oy = (i / s.columns) * doc.height;
        const std::vector<u8>& src = doc.frames[static_cast<usize>(i)].rgba;
        for (int y = 0; y < doc.height; ++y) {
            std::memcpy(&s.rgba[(static_cast<usize>(oy + y) * s.atlas_width + ox) * 4],
                        &src[static_cast<usize>(y) * doc.width * 4], static_cast<usize>(doc.width) * 4);
        }
        s.durations_ms.push_back(doc.frames[static_cast<usize>(i)].duration_ms);
    }
    s.tags = doc.tags;
    s.slices = doc.slices;
    s.tilesets = doc.tilesets;
    s.tilemaps = doc.tilemaps;
    return s;
}

} // namespace rw
