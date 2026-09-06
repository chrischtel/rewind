#pragma once

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/texture.h"

#include <string>
#include <string_view>
#include <vector>

namespace rw {

class Renderer2D;

struct SpriteFrame {
    Rect src;                ///< Where this frame lives in the sheet's texture.
    u32 duration_ms = 100;
};

enum class TagDirection : u8 { Forward = 0, Reverse = 1, PingPong = 2, PingPongReverse = 3 };

struct SpriteTag {
    std::string name;
    u16 from = 0;
    u16 to = 0;
    TagDirection direction = TagDirection::Forward;
    u16 repeat = 0; ///< 0 = loop forever, otherwise play this many times and stop.
};

/// A named rectangle inside a frame, relative to the frame's top-left corner.
struct SpriteSlice {
    std::string name;
    Rect bounds;
    bool has_pivot = false;
    Vec2 pivot;
};

/// Frames, tags and slices over one texture. Not simulation state: build it in
/// Game::load_assets() and read it from tick() through the animation helpers.
struct SpriteSheet {
    Texture* texture = nullptr;
    int frame_width = 0;
    int frame_height = 0;
    std::vector<SpriteFrame> frames;
    std::vector<SpriteTag> tags;
    std::vector<SpriteSlice> slices;

    bool valid() const { return texture != nullptr && !frames.empty(); }

    /// Index of the tag with this name, or -1.
    int find_tag(std::string_view name) const;
    const SpriteSlice* find_slice(std::string_view name) const;
};

/// A sheet over a texture laid out as a grid of equally sized frames, read
/// left to right, top to bottom.
SpriteSheet sprite_sheet_from_grid(Texture* texture, int frame_width, int frame_height,
                                   int frame_count, u32 duration_ms);

/// Load an .aseprite / .ase file straight from disk, or its cooked form when
/// the project was cooked: layers are composited, frames packed into one
/// texture (cached under the file path), tags and slices carried over.
/// Returns false and fills `error` on failure.
bool load_aseprite_sheet(Renderer2D& renderer, const std::string& path, SpriteSheet& out,
                         std::string* error = nullptr);

struct AseTilemap;
struct AseTileset;
/// The tilemap layers and tilesets of an .aseprite file or its cooked form.
bool load_sheet_tilemaps(const std::string& path, std::vector<AseTilemap>& tilemaps, std::vector<AseTileset>& tilesets,
                         std::string* error = nullptr);

} // namespace rw
