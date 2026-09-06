#pragma once

#include "rewind/core/math.h"
#include "rewind/core/types.h"
#include "rewind/render/texture.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rw {

/// One character in a baked font atlas.
struct Glyph {
    Rect src;      ///< Atlas rectangle in pixels.
    float x_off;   ///< Pen-relative placement of the bitmap's top-left.
    float y_off;   ///< Relative to the baseline (negative = above it).
    float advance; ///< Pen advance in pixels.
};

/// A font rasterised at one pixel size: coverage bitmap plus glyph metrics
/// and kerning. Pure data, no GPU: bake_font() fills it from a TrueType file.
struct FontAtlas {
    float pixel_size = 0;
    float ascent = 0;      ///< Baseline to top of tallest glyph, pixels.
    float descent = 0;     ///< Baseline to bottom, negative.
    float line_height = 0; ///< Baseline to baseline for stacked lines.
    int width = 0;
    int height = 0;
    std::vector<u8> alpha; ///< width * height coverage values.
    std::unordered_map<u32, Glyph> glyphs;
    std::unordered_map<u64, float> kerning; ///< (left << 32 | right) -> pixels

    const Glyph* find(u32 codepoint) const;
    float kern(u32 left, u32 right) const;

    /// Width and height of `text` drawn at `scale`, newlines included.
    Vec2 measure(std::string_view text, float scale = 1) const;
};

/// Rasterise ASCII and Latin-1 into an atlas at `pixel_size`. `ttf` is the
/// contents of a .ttf or .otf file. Returns false with `error` set on
/// failure.
bool bake_font(const u8* ttf, usize size, float pixel_size, FontAtlas& out, std::string* error = nullptr);

/// Decode the next UTF-8 sequence starting at `i`, advancing it. Invalid
/// bytes decode as U+FFFD and advance by one.
u32 utf8_next(std::string_view text, usize& i);

/// A baked font uploaded to the GPU, owned by the Renderer2D that loaded it.
struct Font {
    std::string path;
    FontAtlas atlas;
    Texture* texture = nullptr;
    bool ok = false;

    float size() const { return atlas.pixel_size; }
    float line_height() const { return atlas.line_height; }
    Vec2 measure(std::string_view text, float scale = 1) const { return atlas.measure(text, scale); }
};

} // namespace rw
