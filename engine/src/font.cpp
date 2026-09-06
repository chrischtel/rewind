#include "rewind/render/font.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>

namespace rw {

// ---- atlas ---------------------------------------------------------------------------

const Glyph* FontAtlas::find(u32 codepoint) const {
    auto it = glyphs.find(codepoint);
    if (it == glyphs.end() && codepoint != '?') it = glyphs.find('?');
    return it == glyphs.end() ? nullptr : &it->second;
}

float FontAtlas::kern(u32 left, u32 right) const {
    auto it = kerning.find((static_cast<u64>(left) << 32) | right);
    return it == kerning.end() ? 0.0f : it->second;
}

Vec2 FontAtlas::measure(std::string_view text, float scale) const {
    float widest = 0, line = 0;
    int lines = 1;
    u32 prev = 0;
    for (usize i = 0; i < text.size();) {
        const u32 cp = utf8_next(text, i);
        if (cp == '\n') {
            widest = std::max(widest, line);
            line = 0;
            prev = 0;
            ++lines;
            continue;
        }
        const Glyph* g = find(cp);
        if (!g) continue;
        line += (g->advance + kern(prev, cp)) * scale;
        prev = cp;
    }
    widest = std::max(widest, line);
    return Vec2{widest, static_cast<float>(lines) * line_height * scale};
}

// ---- utf-8 ---------------------------------------------------------------------------

u32 utf8_next(std::string_view text, usize& i) {
    const auto byte = [&](usize k) { return static_cast<u8>(text[k]); };
    const u8 c = byte(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int extra = 0;
    u32 cp = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        ++i;
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; ++k) {
        if (i + static_cast<usize>(k) >= text.size() || (byte(i + static_cast<usize>(k)) & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (byte(i + static_cast<usize>(k)) & 0x3F);
    }
    i += static_cast<usize>(extra) + 1;
    return cp;
}

// ---- baking --------------------------------------------------------------------------

bool bake_font(const u8* ttf, usize size, float pixel_size, FontAtlas& out, std::string* error) {
    out = FontAtlas{};
    if (!ttf || size < 12) {
        if (error) *error = "not a font file";
        return false;
    }
    // stb_truetype trusts its input; reject anything that is not a font
    // container before it starts following offsets.
    const int offset = stbtt_GetFontOffsetForIndex(ttf, 0);
    if (offset < 0 || static_cast<usize>(offset) + 12 > size) {
        if (error) *error = "not a TrueType or OpenType font";
        return false;
    }
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf, offset)) {
        if (error) *error = "stb_truetype could not read this font";
        return false;
    }
    if (pixel_size < 4) pixel_size = 4;

    // Pack ASCII and Latin-1 supplement; grow the atlas until everything fits.
    constexpr int kFirstAscii = 32, kCountAscii = 95;
    constexpr int kFirstLatin = 160, kCountLatin = 96;
    std::vector<stbtt_packedchar> ascii(kCountAscii), latin(kCountLatin);
    int atlas = pixel_size <= 20 ? 256 : (pixel_size <= 48 ? 512 : 1024);
    for (;; atlas *= 2) {
        if (atlas > 4096) {
            if (error) *error = "font too large to pack";
            return false;
        }
        out.alpha.assign(static_cast<usize>(atlas) * atlas, 0);
        stbtt_pack_context pack;
        if (!stbtt_PackBegin(&pack, out.alpha.data(), atlas, atlas, 0, 1, nullptr)) continue;
        stbtt_PackSetOversampling(&pack, 1, 1);
        stbtt_pack_range ranges[2] = {};
        ranges[0].font_size = pixel_size;
        ranges[0].first_unicode_codepoint_in_range = kFirstAscii;
        ranges[0].num_chars = kCountAscii;
        ranges[0].chardata_for_range = ascii.data();
        ranges[1].font_size = pixel_size;
        ranges[1].first_unicode_codepoint_in_range = kFirstLatin;
        ranges[1].num_chars = kCountLatin;
        ranges[1].chardata_for_range = latin.data();
        const int ok = stbtt_PackFontRanges(&pack, ttf, 0, ranges, 2);
        stbtt_PackEnd(&pack);
        if (ok) break;
    }
    out.width = out.height = atlas;
    out.pixel_size = pixel_size;

    const float scale = stbtt_ScaleForPixelHeight(&info, pixel_size);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    out.ascent = static_cast<float>(ascent) * scale;
    out.descent = static_cast<float>(descent) * scale;
    out.line_height = static_cast<float>(ascent - descent + gap) * scale;

    auto take = [&](const std::vector<stbtt_packedchar>& chars, int first) {
        for (usize i = 0; i < chars.size(); ++i) {
            const stbtt_packedchar& c = chars[i];
            Glyph g;
            g.src = Rect{static_cast<float>(c.x0), static_cast<float>(c.y0), static_cast<float>(c.x1 - c.x0),
                         static_cast<float>(c.y1 - c.y0)};
            g.x_off = c.xoff;
            g.y_off = c.yoff;
            g.advance = c.xadvance;
            out.glyphs[static_cast<u32>(first) + static_cast<u32>(i)] = g;
        }
    };
    take(ascii, kFirstAscii);
    take(latin, kFirstLatin);

    // Kerning for the ASCII range; only pairs that actually kern are kept.
    for (int a = kFirstAscii; a < kFirstAscii + kCountAscii; ++a) {
        for (int b = kFirstAscii; b < kFirstAscii + kCountAscii; ++b) {
            const int k = stbtt_GetCodepointKernAdvance(&info, a, b);
            if (k != 0) out.kerning[(static_cast<u64>(a) << 32) | static_cast<u64>(b)] = static_cast<float>(k) * scale;
        }
    }
    return true;
}

} // namespace rw
