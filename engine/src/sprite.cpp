#include "rewind/render/sprite.h"

#include "rewind/assets/aseprite.h"
#include "rewind/core/paths.h"
#include "rewind/render/renderer2d.h"

#include <cmath>
#include <cstring>

namespace rw {

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

bool load_aseprite_sheet(Renderer2D& renderer, const std::string& path, SpriteSheet& out,
                         std::string* error) {
    AseDocument doc;
    std::string err;
    if (!load_aseprite(path, doc, err)) {
        if (error) *error = err;
        return false;
    }

    // Pack frames into a near-square grid.
    const int count = static_cast<int>(doc.frames.size());
    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + columns - 1) / columns;
    const int atlas_w = columns * doc.width;
    const int atlas_h = rows * doc.height;

    std::vector<u8> atlas(static_cast<usize>(atlas_w) * atlas_h * 4, 0);
    for (int i = 0; i < count; ++i) {
        const int ox = (i % columns) * doc.width;
        const int oy = (i / columns) * doc.height;
        const std::vector<u8>& src = doc.frames[static_cast<usize>(i)].rgba;
        for (int y = 0; y < doc.height; ++y) {
            std::memcpy(&atlas[(static_cast<usize>(oy + y) * atlas_w + ox) * 4],
                        &src[static_cast<usize>(y) * doc.width * 4], static_cast<usize>(doc.width) * 4);
        }
    }

    Texture* texture = renderer.create_texture(path, atlas_w, atlas_h, atlas.data());
    if (!texture) {
        if (error) *error = "could not create texture for " + path;
        return false;
    }
    renderer.track_file(resolve_asset_path(path)); // edits in Aseprite show up in the running game

    out = SpriteSheet{};
    out.texture = texture;
    out.frame_width = doc.width;
    out.frame_height = doc.height;
    for (int i = 0; i < count; ++i) {
        SpriteFrame frame;
        frame.src = Rect{static_cast<float>((i % columns) * doc.width),
                         static_cast<float>((i / columns) * doc.height),
                         static_cast<float>(doc.width), static_cast<float>(doc.height)};
        frame.duration_ms = doc.frames[static_cast<usize>(i)].duration_ms;
        out.frames.push_back(frame);
    }
    for (const AseTag& t : doc.tags) {
        SpriteTag tag;
        tag.name = t.name;
        tag.from = t.from;
        tag.to = t.to;
        tag.direction = static_cast<TagDirection>(t.direction > 3 ? 0 : t.direction);
        tag.repeat = t.repeat;
        out.tags.push_back(std::move(tag));
    }
    for (const AseSlice& s : doc.slices) {
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

} // namespace rw
