#pragma once

// STYLE.md as code. Every number the game draws with lives here and nowhere
// else, so the look is one file to argue with rather than a hundred literals
// spread through the drawing.
//
// The rules these express: nine colours and no tenth, one grid, three line
// weights, and nothing that snaps.

#include <rewind/rewind.h>

namespace freight {

using rw::Color;
using rw::rgb;
using rw::rgba;

// ---- colour -----------------------------------------------------------------
// Paper ground, ink lines, one warm signal colour. `ground`, `surface` and
// `line` are one family and read as one colour, so this list is really five.

inline constexpr Color kGround = rgb(0xEF, 0xEB, 0xE3);  ///< the world sits on this
inline constexpr Color kSurface = rgb(0xF8, 0xF6, 0xF1); ///< panels, cards, hardstanding
inline constexpr Color kLine = rgb(0xC4, 0xBF, 0xB5);    ///< rules, borders, the grid
inline constexpr Color kInk = rgb(0x22, 0x27, 0x2B);     ///< text, outlines, vehicle bodies
inline constexpr Color kMuted = rgb(0x6B, 0x70, 0x75);   ///< secondary text, distant detail
inline constexpr Color kAccent = rgb(0xCE, 0x5A, 0x28);  ///< the one warm colour: ours, selected
inline constexpr Color kGood = rgb(0x4A, 0x7C, 0x59);    ///< on time, in profit, loaded
inline constexpr Color kWarn = rgb(0xC9, 0xA2, 0x27);    ///< late, low, wearing
inline constexpr Color kBad = rgb(0xA6, 0x3D, 0x2F);     ///< failed, empty, overdue

/// Ground cover, all within a step of `ground` so the world stays quiet and
/// only our own things are allowed to be saturated.
inline constexpr Color kGrass = rgb(0xE4, 0xE3, 0xD4);
inline constexpr Color kBallast = rgb(0xD3, 0xCE, 0xC3);
inline constexpr Color kAsphalt = rgb(0xDA, 0xD6, 0xCE);
inline constexpr Color kShadow = rgba(0x22, 0x27, 0x2B, 0x14);

// ---- measure: the UI --------------------------------------------------------
// An 8 unit grid. Nothing sits off it.

inline constexpr float kUnit = 8;
inline constexpr float kPad = 16;      ///< inside a panel
inline constexpr float kGap = 8;       ///< between items
inline constexpr float kGroupGap = 24; ///< between groups of items
inline constexpr float kRow = 32;      ///< a row, and a button
inline constexpr float kRadius = 4;    ///< the only corner radius there is
inline constexpr float kBorder = 1;

// Type sizes, for when the game has a real font. Until then the built-in
// face draws at integer scales and these pick the nearest.
inline constexpr float kTitleSize = 16;
inline constexpr float kBodySize = 14;
inline constexpr float kSmallSize = 12;
inline constexpr float kFigureSize = 28; ///< money, the clock

// ---- measure: the world -----------------------------------------------------
// Real metres, like the traffic game. Getting these right is free and it is
// most of why a top-down scene reads as convincing.

inline constexpr float kGauge = 1.435f;    ///< standard gauge
inline constexpr float kSleeper = 2.6f;    ///< sleeper length
inline constexpr float kTrackSpacing = 3.6f;
inline constexpr float kLaneWidth = 3.25f;
inline constexpr float kBallastWidth = 4.4f;

/// Line weights in metres, so they scale with the zoom.
inline constexpr float kHairline = 0.08f;
inline constexpr float kStandard = 0.14f;
inline constexpr float kEmphasis = 0.30f;

// ---- motion -----------------------------------------------------------------
// Nothing appears, disappears or jumps. One curve: ease-out cubic.

inline constexpr float kHoverTime = 0.12f;  ///< hover, press
inline constexpr float kPanelTime = 0.18f;  ///< a panel, a tab
inline constexpr float kNumberTime = 0.24f; ///< a number changing, counted not cut
inline constexpr float kCameraRate = 12.0f; ///< approach() e-folds per second, ~300 ms to settle

/// StrokeStyle carries its colour as four floats rather than a Color.
inline rw::StrokeStyle stroke(float width, Color c, rw::LineCap cap = rw::LineCap::Butt) {
    rw::StrokeStyle s;
    s.width = width;
    s.cap = cap;
    s.join = rw::LineJoin::Round;
    s.r = c.r / 255.0f;
    s.g = c.g / 255.0f;
    s.b = c.b / 255.0f;
    s.a = c.a / 255.0f;
    return s;
}

} // namespace freight
