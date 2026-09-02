#pragma once

#include "rewind/core/types.h"

#include <vector>

namespace rw {

/// Built-in 5x7 pixel font covering printable ASCII. Glyphs sit in 6x8 cells
/// in a 16-column atlas; lowercase letters share the uppercase glyphs.
struct DebugFont {
    static constexpr int kCellW = 6;
    static constexpr int kCellH = 8;
    static constexpr int kColumns = 16;
    static constexpr int kAtlasW = kColumns * kCellW; // 96
    static constexpr int kAtlasH = 6 * kCellH;        // 48, 96 cells for codes 32..127

    /// White glyph pixels with alpha, RGBA8, kAtlasW x kAtlasH.
    static std::vector<u8> build_atlas();

    /// Atlas cell index for a character (0 = space).
    static int cell_for(char c);
};

} // namespace rw
