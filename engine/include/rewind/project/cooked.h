#pragma once

#include "rewind/assets/aseprite.h"
#include "rewind/core/types.h"

#include <string>
#include <vector>

namespace rw {

/// Cooked file formats. Every cooked file starts with a four-byte magic and a
/// version, so loaders can tell a cooked file from a source file by content
/// alone and the same load call works for both.

constexpr u32 kCookedTextureMagic = 0x58455452u; // "RTEX"
constexpr u32 kCookedSheetMagic = 0x54485352u;   // "RSHT"

struct CookedTexture {
    int width = 0;
    int height = 0;
    bool nearest = true;
    u64 source_hash = 0;
    std::vector<u8> rgba;
};

/// A composited Aseprite document packed into one atlas, with the
/// timeline's tags, the slices, and any tilemap layers and tilesets.
struct CookedSheet {
    int atlas_width = 0;
    int atlas_height = 0;
    int frame_width = 0;
    int frame_height = 0;
    int columns = 0;
    bool nearest = true;
    u64 source_hash = 0;
    std::vector<u8> rgba;
    std::vector<u32> durations_ms;
    std::vector<AseTag> tags;
    std::vector<AseSlice> slices;
    std::vector<AseTileset> tilesets;
    std::vector<AseTilemap> tilemaps;
};

void encode_cooked_texture(const CookedTexture& texture, std::vector<u8>& out);
bool decode_cooked_texture(const std::vector<u8>& bytes, CookedTexture& out);

void encode_cooked_sheet(const CookedSheet& sheet, std::vector<u8>& out);
bool decode_cooked_sheet(const std::vector<u8>& bytes, CookedSheet& out);

/// Which cooked format a buffer holds, or 0.
u32 cooked_magic(const std::vector<u8>& bytes);

/// Build a cooked sheet from a parsed Aseprite document.
CookedSheet cook_aseprite(const AseDocument& doc, u64 source_hash, bool nearest);

} // namespace rw
