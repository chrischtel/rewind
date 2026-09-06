#include "rewind/world/tilemap.h"

#include "rewind/assets/aseprite.h"
#include "rewind/core/paths.h"
#include "rewind/render/renderer2d.h"
#include "rewind/render/sprite.h"

#include <algorithm>
#include <cmath>

namespace rw {

namespace {
constexpr float kEpsilon = 0.001f;

int floor_div(float v, int size) { return static_cast<int>(std::floor(v / static_cast<float>(size))); }
} // namespace

bool TileMap::solid_at(float x, float y) const {
    return is_solid(floor_div(x, tile_width), floor_div(y, tile_height));
}

bool TileMap::overlaps_solid(const Rect& box) const {
    const int tx0 = floor_div(box.x, tile_width);
    const int tx1 = floor_div(box.x + box.w - kEpsilon, tile_width);
    const int ty0 = floor_div(box.y, tile_height);
    const int ty1 = floor_div(box.y + box.h - kEpsilon, tile_height);
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
            if (is_solid(tx, ty)) return true;
    return false;
}

Rect TileMap::tile_src(u32 id) const {
    const int columns = tileset_columns > 0 ? tileset_columns : 1;
    const int i = static_cast<int>(id);
    return Rect{static_cast<float>((i % columns) * tile_width), static_cast<float>((i / columns) * tile_height),
                static_cast<float>(tile_width), static_cast<float>(tile_height)};
}

TileHit tilemap_move(const TileMap& map, Rect& box, float dx, float dy) {
    TileHit hit;
    if (map.width <= 0 || map.height <= 0 || map.tile_width <= 0 || map.tile_height <= 0) {
        box.x += dx;
        box.y += dy;
        return hit;
    }

    // Each axis sweeps every tile column (or row) between the old and the new
    // leading edge, so a fast box cannot tunnel through a thin wall. Tiles the
    // box already overlaps at the start are skipped, which lets it escape if
    // it ever ends up inside something.

    // Horizontal.
    if (dx != 0) {
        const int ty0 = floor_div(box.y, map.tile_height);
        const int ty1 = floor_div(box.y + box.h - kEpsilon, map.tile_height);
        auto column_solid = [&](int tx) {
            for (int ty = ty0; ty <= ty1; ++ty)
                if (map.is_solid(tx, ty)) return true;
            return false;
        };
        if (dx > 0) {
            const int from = floor_div(box.x + box.w - kEpsilon, map.tile_width);
            const int to = floor_div(box.x + dx + box.w - kEpsilon, map.tile_width);
            box.x += dx;
            for (int tx = from + 1; tx <= to; ++tx) {
                if (column_solid(tx)) {
                    box.x = static_cast<float>(tx * map.tile_width) - box.w;
                    hit.right = true;
                    break;
                }
            }
        } else {
            const int from = floor_div(box.x, map.tile_width);
            const int to = floor_div(box.x + dx, map.tile_width);
            box.x += dx;
            for (int tx = from - 1; tx >= to; --tx) {
                if (column_solid(tx)) {
                    box.x = static_cast<float>((tx + 1) * map.tile_width);
                    hit.left = true;
                    break;
                }
            }
        }
    }

    // Vertical.
    if (dy != 0) {
        const int tx0 = floor_div(box.x, map.tile_width);
        const int tx1 = floor_div(box.x + box.w - kEpsilon, map.tile_width);
        auto row_solid = [&](int ty) {
            for (int tx = tx0; tx <= tx1; ++tx)
                if (map.is_solid(tx, ty)) return true;
            return false;
        };
        if (dy > 0) {
            const int from = floor_div(box.y + box.h - kEpsilon, map.tile_height);
            const int to = floor_div(box.y + dy + box.h - kEpsilon, map.tile_height);
            box.y += dy;
            for (int ty = from + 1; ty <= to; ++ty) {
                if (row_solid(ty)) {
                    box.y = static_cast<float>(ty * map.tile_height) - box.h;
                    hit.bottom = true;
                    break;
                }
            }
        } else {
            const int from = floor_div(box.y, map.tile_height);
            const int to = floor_div(box.y + dy, map.tile_height);
            box.y += dy;
            for (int ty = from - 1; ty >= to; --ty) {
                if (row_solid(ty)) {
                    box.y = static_cast<float>((ty + 1) * map.tile_height);
                    hit.top = true;
                    break;
                }
            }
        }
    }
    return hit;
}

void draw_tilemap(Renderer2D& r, const TileMap& map, float x, float y) {
    if (!map.tileset || map.width <= 0 || map.height <= 0) return;

    const Camera& cam = r.camera();
    const float zoom = cam.zoom > 0 ? cam.zoom : 1;
    const float view_w = static_cast<float>(r.width()) / zoom;
    const float view_h = static_cast<float>(r.height()) / zoom;

    const int tx0 = std::max(0, floor_div(cam.x - x, map.tile_width));
    const int ty0 = std::max(0, floor_div(cam.y - y, map.tile_height));
    const int tx1 = std::min(map.width - 1, floor_div(cam.x - x + view_w, map.tile_width) + 1);
    const int ty1 = std::min(map.height - 1, floor_div(cam.y - y + view_h, map.tile_height) + 1);

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const u32 cell = map.cell(tx, ty);
            const u32 id = cell & TileMap::kIdMask;
            if (id == 0) continue;

            SpriteOptions o;
            o.flip_x = (cell & TileMap::kFlipX) != 0;
            o.flip_y = (cell & TileMap::kFlipY) != 0;
            float px = x + static_cast<float>(tx * map.tile_width);
            float py = y + static_cast<float>(ty * map.tile_height);
            if (cell & TileMap::kFlipDiagonal) {
                // Transpose = rotate 90 degrees clockwise, then mirror horizontally.
                o.rotation_deg = 90;
                o.flip_x = !o.flip_x;
                o.origin = Vec2{static_cast<float>(map.tile_width) * 0.5f, static_cast<float>(map.tile_height) * 0.5f};
                px += static_cast<float>(map.tile_width) * 0.5f;
                py += static_cast<float>(map.tile_height) * 0.5f;
            }
            r.draw_sprite(*map.tileset, map.tile_src(id), px, py, o);
        }
    }
}

TileMap tilemap_from_text(const std::vector<std::string>& rows, std::string_view legend, int tile_width,
                          int tile_height) {
    TileMap map;
    map.tile_width = tile_width;
    map.tile_height = tile_height;
    map.height = static_cast<int>(rows.size());
    for (const std::string& row : rows) map.width = std::max(map.width, static_cast<int>(row.size()));
    map.cells.assign(static_cast<usize>(map.width) * map.height, 0);
    for (int ty = 0; ty < map.height; ++ty) {
        const std::string& row = rows[static_cast<usize>(ty)];
        for (int tx = 0; tx < static_cast<int>(row.size()); ++tx) {
            const usize pos = legend.find(row[static_cast<usize>(tx)]);
            if (pos != std::string_view::npos) map.set(tx, ty, static_cast<u32>(pos) + 1);
        }
    }
    return map;
}

bool load_aseprite_tilemap(Renderer2D& renderer, const std::string& path, std::string_view layer_name,
                           TileMap& out, std::string* error) {
    std::vector<AseTilemap> tilemaps;
    std::vector<AseTileset> tilesets;
    if (!load_sheet_tilemaps(path, tilemaps, tilesets, error)) return false;

    const AseTilemap* layer = nullptr;
    for (const AseTilemap& t : tilemaps)
        if (t.layer_name == layer_name) layer = &t;
    if (!layer) {
        if (error) *error = "no tilemap layer named '" + std::string(layer_name) + "' in " + path;
        return false;
    }
    if (layer->tileset >= tilesets.size()) {
        if (error) *error = "tilemap layer refers to a missing tileset";
        return false;
    }
    const AseTileset& ts = tilesets[layer->tileset];
    if (ts.rgba.empty()) {
        if (error) *error = "tileset '" + ts.name + "' keeps its tiles in an external file, which is not supported";
        return false;
    }

    // Tiles are stacked vertically in the file; keep that layout.
    Texture* texture = renderer.create_texture(path + "#" + std::string(layer_name), ts.tile_width,
                                               ts.tile_height * static_cast<int>(ts.tile_count), ts.rgba.data());
    if (!texture) {
        if (error) *error = "could not create the tileset texture";
        return false;
    }
    renderer.track_file(resolve_asset_path(path));

    out = TileMap{};
    out.width = layer->width;
    out.height = layer->height;
    out.tile_width = ts.tile_width;
    out.tile_height = ts.tile_height;
    out.cells = layer->cells;
    out.tileset = texture;
    out.tileset_columns = 1;
    return true;
}

} // namespace rw
