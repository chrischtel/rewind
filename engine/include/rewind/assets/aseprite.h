#pragma once

#include "rewind/core/types.h"

#include <string>
#include <vector>

namespace rw {

/// A frame tag ("walk", "idle"...) as set in Aseprite's timeline.
struct AseTag {
    std::string name;
    u16 from = 0;
    u16 to = 0;
    u8 direction = 0; ///< 0 forward, 1 reverse, 2 ping-pong, 3 ping-pong reverse
    u16 repeat = 0;   ///< 0 = loop forever
};

/// A slice: a named rectangle drawn in Aseprite, typically a hitbox or an
/// attachment point. Only the first key is read.
struct AseSlice {
    std::string name;
    i32 x = 0;
    i32 y = 0;
    u32 w = 0;
    u32 h = 0;
    bool has_pivot = false;
    i32 pivot_x = 0;
    i32 pivot_y = 0;
};

struct AseLayer {
    std::string name;
    bool visible = true;
    u16 type = 0; ///< 0 image, 1 group, 2 tilemap
    u16 child_level = 0;
    u8 opacity = 255;
    u32 tileset = 0; ///< tilemap layers only
};

/// One frame with every visible layer composited, canvas sized, RGBA8.
struct AseFrame {
    u32 duration_ms = 100;
    std::vector<u8> rgba;
};

/// Tiles of a tileset, stacked vertically: the texture is tile_width wide and
/// tile_height * tile_count high. Tile 0 is always the empty tile.
struct AseTileset {
    std::string name;
    int tile_width = 0;
    int tile_height = 0;
    u32 tile_count = 0;
    std::vector<u8> rgba; ///< empty if the tiles live in an external file
};

/// A tilemap layer's cells in frame 0: id | flip flags, row-major. The flip
/// bits are 0x80000000 (x), 0x40000000 (y), 0x20000000 (diagonal).
struct AseTilemap {
    std::string layer_name;
    usize layer = 0;
    usize tileset = 0;
    int width = 0;
    int height = 0;
    std::vector<u32> cells;
};

struct AseDocument {
    int width = 0;
    int height = 0;
    std::vector<AseLayer> layers;
    std::vector<AseFrame> frames;
    std::vector<AseTag> tags;
    std::vector<AseSlice> slices;
    std::vector<AseTileset> tilesets;
    std::vector<AseTilemap> tilemaps;
};

/// Parse an .aseprite / .ase file from memory. Supports RGBA, grayscale and
/// indexed sprites, raw, zlib-compressed and linked cels, layer and group
/// visibility, layer and cel opacity, tags, slices, tilesets and tilemap
/// layers (which are also drawn into the composited frames). Blend modes
/// other than Normal are ignored.
bool parse_aseprite(const u8* data, usize size, AseDocument& out, std::string& error);

bool load_aseprite(const std::string& path, AseDocument& out, std::string& error);

} // namespace rw
