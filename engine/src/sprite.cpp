#include "rewind/render/sprite.h"

#include "rewind/assets/aseprite.h"
#include "rewind/core/log.h"
#include "rewind/core/paths.h"
#include "rewind/project/cooked.h"
#include "rewind/render/renderer2d.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace rw {

namespace {

bool read_all(const std::string& path, std::vector<u8>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    u8 buffer[64 * 1024];
    usize n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
    return true;
}

} // namespace

int SpriteSheet::find_tag(std::string_view name) const {
    for (usize i = 0; i < tags.size(); ++i)
        if (tags[i].name == name) return static_cast<int>(i);
    return -1;
}

const SpriteSlice* SpriteSheet::find_slice(std::string_view name) const {
    for (const SpriteSlice& s : slices)
        if (s.name == name) return &s;
    return nullptr;
}

SpriteSheet sprite_sheet_from_grid(Texture* texture, int frame_width, int frame_height,
                                   int frame_count, u32 duration_ms) {
    SpriteSheet sheet;
    sheet.texture = texture;
    sheet.frame_width = frame_width;
    sheet.frame_height = frame_height;
    if (!texture || frame_width <= 0 || frame_height <= 0) return sheet;

    const int columns = texture->width / frame_width;
    if (columns <= 0) return sheet;
    for (int i = 0; i < frame_count; ++i) {
        SpriteFrame frame;
        frame.src = Rect{static_cast<float>((i % columns) * frame_width),
                         static_cast<float>((i / columns) * frame_height),
                         static_cast<float>(frame_width), static_cast<float>(frame_height)};
        frame.duration_ms = duration_ms;
        sheet.frames.push_back(frame);
    }
    return sheet;
}

/// Load either a cooked sheet or a source .aseprite into the intermediate form.
static bool load_sheet_data(const std::string& path, CookedSheet& out, std::string& error) {
    const std::string file = resolve_asset_path(path);
    std::vector<u8> bytes;
    if (!read_all(file, bytes)) {
        error = "could not open " + path;
        return false;
    }
    if (cooked_magic(bytes) == kCookedSheetMagic) {
        if (!decode_cooked_sheet(bytes, out)) {
            error = "damaged cooked sheet " + path;
            return false;
        }
        return true;
    }
    AseDocument doc;
    if (!parse_aseprite(bytes.data(), bytes.size(), doc, error)) return false;
    out = cook_aseprite(doc, 0, true);
    return true;
}

bool load_aseprite_sheet(Renderer2D& renderer, const std::string& path, SpriteSheet& out, std::string* error) {
    CookedSheet data;
    std::string err;
    if (!load_sheet_data(path, data, err)) {
        if (error) *error = err;
        return false;
    }

    Texture* texture = renderer.create_texture(path, data.atlas_width, data.atlas_height, data.rgba.data());
    if (!texture) {
        if (error) *error = "could not create texture for " + path;
        return false;
    }
    renderer.track_file(resolve_asset_path(path)); // edits in Aseprite (or a re-cook) show up in the running game

    out = SpriteSheet{};
    out.texture = texture;
    out.frame_width = data.frame_width;
    out.frame_height = data.frame_height;
    for (usize i = 0; i < data.durations_ms.size(); ++i) {
        SpriteFrame frame;
        const int col = static_cast<int>(i) % data.columns;
        const int row = static_cast<int>(i) / data.columns;
        frame.src = Rect{static_cast<float>(col * data.frame_width), static_cast<float>(row * data.frame_height),
                         static_cast<float>(data.frame_width), static_cast<float>(data.frame_height)};
        frame.duration_ms = data.durations_ms[i];
        out.frames.push_back(frame);
    }
    for (const AseTag& t : data.tags) {
        SpriteTag tag;
        tag.name = t.name;
        tag.from = t.from;
        tag.to = t.to;
        tag.direction = static_cast<TagDirection>(t.direction > 3 ? 0 : t.direction);
        tag.repeat = t.repeat;
        out.tags.push_back(std::move(tag));
    }
    for (const AseSlice& s : data.slices) {
        SpriteSlice slice;
        slice.name = s.name;
        slice.bounds = Rect{static_cast<float>(s.x), static_cast<float>(s.y), static_cast<float>(s.w),
                            static_cast<float>(s.h)};
        slice.has_pivot = s.has_pivot;
        slice.pivot = Vec2{static_cast<float>(s.pivot_x), static_cast<float>(s.pivot_y)};
        out.slices.push_back(std::move(slice));
    }
    return true;
}

bool load_sheet_tilemaps(const std::string& path, std::vector<AseTilemap>& tilemaps, std::vector<AseTileset>& tilesets,
                         std::string* error) {
    CookedSheet data;
    std::string err;
    if (!load_sheet_data(path, data, err)) {
        if (error) *error = err;
        return false;
    }
    tilemaps = std::move(data.tilemaps);
    tilesets = std::move(data.tilesets);
    return true;
}

} // namespace rw
