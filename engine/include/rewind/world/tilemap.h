#pragma once

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/texture.h"

#include <string>
#include <string_view>
#include <vector>

namespace rw {

class Renderer2D;

/// A grid of tile ids over one tileset texture. Plain data: keep it in the
/// game state if tiles change during play (breakable blocks, doors), or
/// treat it as a level asset if they never do.
struct TileMap {
    static constexpr u32 kIdMask = 0x1fffffff;
    static constexpr u32 kFlipX = 0x80000000u;
    static constexpr u32 kFlipY = 0x40000000u;
    static constexpr u32 kFlipDiagonal = 0x20000000u; ///< Transpose, as in Aseprite.

    int width = 0;  ///< In tiles.
    int height = 0;
    int tile_width = 16;
    int tile_height = 16;
    std::vector<u32> cells; ///< id | flip flags per tile, row-major. Id 0 = empty.

    Texture* tileset = nullptr;
    int tileset_columns = 1; ///< How many tiles sit side by side in the tileset texture.

    /// Solidity per tile id (index = id). Empty means every non-zero id is solid.
    std::vector<u8> solid;
    bool solid_outside = true; ///< Treat everything beyond the map as a wall.

    /// Cells and dimensions are state; the tileset texture is an asset and is not saved.
    void reflect(Visitor& v) {
        v.field("width", width, {.read_only = true});
        v.field("height", height, {.read_only = true});
        v.field("tile_width", tile_width, {.read_only = true});
        v.field("tile_height", tile_height, {.read_only = true});
        v.field("cells", cells);
        v.field("solid", solid);
        v.field("solid_outside", solid_outside);
    }

    bool in_bounds(int tx, int ty) const { return tx >= 0 && ty >= 0 && tx < width && ty < height; }
    u32 cell(int tx, int ty) const { return in_bounds(tx, ty) ? cells[static_cast<usize>(ty) * width + tx] : 0; }
    u32 id_at(int tx, int ty) const { return cell(tx, ty) & kIdMask; }
    void set(int tx, int ty, u32 value) {
        if (in_bounds(tx, ty)) cells[static_cast<usize>(ty) * width + tx] = value;
    }

    bool is_solid_id(u32 id) const {
        if (id == 0) return false;
        if (solid.empty()) return true;
        return id < solid.size() && solid[id] != 0;
    }
    bool is_solid(int tx, int ty) const {
        if (!in_bounds(tx, ty)) return solid_outside;
        return is_solid_id(id_at(tx, ty));
    }
    bool solid_at(float x, float y) const;
    bool overlaps_solid(const Rect& box) const;

    float pixel_width() const { return static_cast<float>(width * tile_width); }
    float pixel_height() const { return static_cast<float>(height * tile_height); }

    /// Source rectangle of a tile id inside the tileset texture.
    Rect tile_src(u32 id) const;
};

/// Which sides of a box touched something during tilemap_move().
struct TileHit {
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;
    bool any() const { return left || right || top || bottom; }
};

/// Move `box` by (dx, dy) through the map, stopping at solid tiles. Axes are
/// resolved one after the other (x first), which is what platformers want.
/// Deterministic: plain float arithmetic, no allocation.
TileHit tilemap_move(const TileMap& map, Rect& box, float dx, float dy);

/// Draw every tile the camera can see, with the map's top-left at (x, y).
void draw_tilemap(Renderer2D& renderer, const TileMap& map, float x = 0, float y = 0);

/// Build a map from text rows. `legend` lists the characters for ids 1, 2,
/// 3... in order; any other character (usually '.') is empty.
TileMap tilemap_from_text(const std::vector<std::string>& rows, std::string_view legend,
                          int tile_width, int tile_height);

/// Load a tilemap layer from an .aseprite file. The layer's tileset becomes a
/// texture (cached under "<path>#<layer>"), tile ids and flips are kept.
bool load_aseprite_tilemap(Renderer2D& renderer, const std::string& path, std::string_view layer_name,
                           TileMap& out, std::string* error = nullptr);

} // namespace rw
