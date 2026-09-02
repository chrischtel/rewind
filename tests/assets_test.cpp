// Headless checks for the Aseprite parser and the animation player. The test
// writes small .aseprite files into memory byte by byte, following the
// public file format specification, so no editor is needed.

#include <rewind/assets/aseprite.h>
#include <rewind/render/sprite.h>
#include <rewind/sim/animation.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

// ---- a tiny Aseprite file writer --------------------------------------------

struct Bytes {
    std::vector<u8> v;
    void b(u8 x) { v.push_back(x); }
    void w(u16 x) { b(static_cast<u8>(x)); b(static_cast<u8>(x >> 8)); }
    void d(u32 x) { w(static_cast<u16>(x)); w(static_cast<u16>(x >> 16)); }
    void s(const std::string& str) {
        w(static_cast<u16>(str.size()));
        for (char c : str) b(static_cast<u8>(c));
    }
    void raw(const std::vector<u8>& data) { v.insert(v.end(), data.begin(), data.end()); }
    void patch_d(usize at, u32 x) {
        v[at] = static_cast<u8>(x);
        v[at + 1] = static_cast<u8>(x >> 8);
        v[at + 2] = static_cast<u8>(x >> 16);
        v[at + 3] = static_cast<u8>(x >> 24);
    }
};

struct Chunk {
    u16 type;
    Bytes body;
};

struct Frame {
    u16 duration_ms;
    std::vector<Chunk> chunks;
};

std::vector<u8> build_ase(u16 width, u16 height, u16 depth, u8 transparent_index,
                          const std::vector<Frame>& frames) {
    Bytes f;
    f.d(0); // file size, patched below
    f.w(0xA5E0);
    f.w(static_cast<u16>(frames.size()));
    f.w(width);
    f.w(height);
    f.w(depth);
    f.d(1); // flags: layer opacity valid
    f.w(100);
    f.d(0);
    f.d(0);
    f.b(transparent_index);
    f.b(0); f.b(0); f.b(0);
    f.w(256);
    f.b(1);
    f.b(1);
    f.w(0); f.w(0); f.w(16); f.w(16);
    for (int i = 0; i < 84; ++i) f.b(0);

    for (const Frame& frame : frames) {
        const usize start = f.v.size();
        f.d(0); // frame bytes, patched
        f.w(0xF1FA);
        f.w(static_cast<u16>(frame.chunks.size()));
        f.w(frame.duration_ms);
        f.b(0); f.b(0);
        f.d(static_cast<u32>(frame.chunks.size()));
        for (const Chunk& c : frame.chunks) {
            f.d(static_cast<u32>(c.body.v.size() + 6));
            f.w(c.type);
            f.raw(c.body.v);
        }
        f.patch_d(start, static_cast<u32>(f.v.size() - start));
    }
    f.patch_d(0, static_cast<u32>(f.v.size()));
    return f.v;
}

Chunk layer_chunk(const std::string& name, bool visible, u16 type, u16 child_level, u8 opacity) {
    Chunk c{0x2004, {}};
    c.body.w(visible ? 1 : 0);
    c.body.w(type);
    c.body.w(child_level);
    c.body.w(0);
    c.body.w(0);
    c.body.w(0);
    c.body.b(opacity);
    c.body.b(0); c.body.b(0); c.body.b(0);
    c.body.s(name);
    return c;
}

Chunk raw_cel(u16 layer, i16 x, i16 y, u8 opacity, u16 w, u16 h, const std::vector<u8>& pixels) {
    Chunk c{0x2005, {}};
    c.body.w(layer);
    c.body.w(static_cast<u16>(x));
    c.body.w(static_cast<u16>(y));
    c.body.b(opacity);
    c.body.w(0); // raw
    c.body.w(0); // z-index
    for (int i = 0; i < 5; ++i) c.body.b(0);
    c.body.w(w);
    c.body.w(h);
    c.body.raw(pixels);
    return c;
}

Chunk linked_cel(u16 layer, u16 frame) {
    Chunk c{0x2005, {}};
    c.body.w(layer);
    c.body.w(0);
    c.body.w(0);
    c.body.b(255);
    c.body.w(1); // linked
    c.body.w(0);
    for (int i = 0; i < 5; ++i) c.body.b(0);
    c.body.w(frame);
    return c;
}

/// zlib stream with one stored (uncompressed) deflate block.
std::vector<u8> zlib_stored(const std::vector<u8>& data) {
    Bytes z;
    z.b(0x78);
    z.b(0x01);
    z.b(0x01); // BFINAL=1, BTYPE=00
    const u16 len = static_cast<u16>(data.size());
    z.w(len);
    z.w(static_cast<u16>(~len));
    z.raw(data);
    u32 a = 1, b = 0;
    for (u8 byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const u32 adler = (b << 16) | a;
    z.b(static_cast<u8>(adler >> 24));
    z.b(static_cast<u8>(adler >> 16));
    z.b(static_cast<u8>(adler >> 8));
    z.b(static_cast<u8>(adler));
    return z.v;
}

Chunk compressed_cel(u16 layer, i16 x, i16 y, u16 w, u16 h, const std::vector<u8>& pixels) {
    Chunk c{0x2005, {}};
    c.body.w(layer);
    c.body.w(static_cast<u16>(x));
    c.body.w(static_cast<u16>(y));
    c.body.b(255);
    c.body.w(2); // compressed
    c.body.w(0);
    for (int i = 0; i < 5; ++i) c.body.b(0);
    c.body.w(w);
    c.body.w(h);
    c.body.raw(zlib_stored(pixels));
    return c;
}

Chunk tags_chunk(const std::vector<AseTag>& tags) {
    Chunk c{0x2018, {}};
    c.body.w(static_cast<u16>(tags.size()));
    for (int i = 0; i < 8; ++i) c.body.b(0);
    for (const AseTag& t : tags) {
        c.body.w(t.from);
        c.body.w(t.to);
        c.body.b(t.direction);
        c.body.w(t.repeat);
        for (int i = 0; i < 6; ++i) c.body.b(0);
        c.body.b(0); c.body.b(0); c.body.b(0);
        c.body.b(0);
        c.body.s(t.name);
    }
    return c;
}

Chunk slice_chunk(const std::string& name, i32 x, i32 y, u32 w, u32 h, bool pivot, i32 px, i32 py) {
    Chunk c{0x2022, {}};
    c.body.d(1);
    c.body.d(pivot ? 2 : 0);
    c.body.d(0);
    c.body.s(name);
    c.body.d(0);
    c.body.d(static_cast<u32>(x));
    c.body.d(static_cast<u32>(y));
    c.body.d(w);
    c.body.d(h);
    if (pivot) {
        c.body.d(static_cast<u32>(px));
        c.body.d(static_cast<u32>(py));
    }
    return c;
}

Chunk palette_chunk(const std::vector<std::array<u8, 4>>& colors) {
    Chunk c{0x2019, {}};
    c.body.d(static_cast<u32>(colors.size()));
    c.body.d(0);
    c.body.d(static_cast<u32>(colors.size() - 1));
    for (int i = 0; i < 8; ++i) c.body.b(0);
    for (const auto& col : colors) {
        c.body.w(0);
        c.body.b(col[0]); c.body.b(col[1]); c.body.b(col[2]); c.body.b(col[3]);
    }
    return c;
}

const u8* pixel(const AseDocument& doc, usize frame, int x, int y) {
    return &doc.frames[frame].rgba[(static_cast<usize>(y) * doc.width + x) * 4];
}

bool is(const u8* p, u8 r, u8 g, u8 b, u8 a) { return p[0] == r && p[1] == g && p[2] == b && p[3] == a; }

// ---- tests ------------------------------------------------------------------

void test_rgba_document() {
    // 3x3 canvas, two frames. Frame 0: a 2x2 red cel at (0,0) plus a hidden
    // layer that must not show. Frame 1: a zlib cel of blue at (1,1).
    const std::vector<u8> red = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const std::vector<u8> green = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    const std::vector<u8> blue = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};

    Frame f0{100, {}};
    f0.chunks.push_back(layer_chunk("base", true, 0, 0, 255));
    f0.chunks.push_back(layer_chunk("hidden", false, 0, 0, 255));
    f0.chunks.push_back(tags_chunk({AseTag{"walk", 0, 1, 0, 0}}));
    f0.chunks.push_back(slice_chunk("hit", 1, 1, 2, 2, true, 1, 1));
    f0.chunks.push_back(raw_cel(0, 0, 0, 255, 2, 2, red));
    f0.chunks.push_back(raw_cel(1, 1, 1, 255, 2, 2, green));

    Frame f1{200, {}};
    f1.chunks.push_back(compressed_cel(0, 1, 1, 2, 2, blue));

    const std::vector<u8> file = build_ase(3, 3, 32, 0, {f0, f1});
    AseDocument doc;
    std::string error;
    const bool ok = parse_aseprite(file.data(), file.size(), doc, error);
    if (!ok) std::printf("parse error: %s\n", error.c_str());
    CHECK(ok);
    CHECK(doc.width == 3 && doc.height == 3);
    CHECK(doc.layers.size() == 2);
    CHECK(doc.frames.size() == 2);
    CHECK(doc.frames[0].duration_ms == 100);
    CHECK(doc.frames[1].duration_ms == 200);

    CHECK(is(pixel(doc, 0, 0, 0), 255, 0, 0, 255));
    CHECK(is(pixel(doc, 0, 1, 1), 255, 0, 0, 255));
    CHECK(is(pixel(doc, 0, 2, 2), 0, 0, 0, 0)); // hidden layer stays hidden
    CHECK(is(pixel(doc, 0, 2, 0), 0, 0, 0, 0));

    CHECK(is(pixel(doc, 1, 0, 0), 0, 0, 0, 0));
    CHECK(is(pixel(doc, 1, 1, 1), 0, 0, 255, 255));
    CHECK(is(pixel(doc, 1, 2, 2), 0, 0, 255, 255));

    CHECK(doc.tags.size() == 1);
    CHECK(doc.tags[0].name == "walk" && doc.tags[0].from == 0 && doc.tags[0].to == 1);
    CHECK(doc.slices.size() == 1);
    CHECK(doc.slices[0].name == "hit" && doc.slices[0].x == 1 && doc.slices[0].w == 2);
    CHECK(doc.slices[0].has_pivot && doc.slices[0].pivot_x == 1);
}

void test_opacity_and_groups() {
    // A group that is hidden hides its child even though the child is visible.
    // A 50% opacity cel over an opaque white pixel blends to grey.
    const std::vector<u8> white = {255, 255, 255, 255};
    const std::vector<u8> black = {0, 0, 0, 255};

    Frame f0{100, {}};
    f0.chunks.push_back(layer_chunk("bg", true, 0, 0, 255));
    f0.chunks.push_back(layer_chunk("half", true, 0, 0, 255));
    f0.chunks.push_back(layer_chunk("group", false, 1, 0, 255));
    f0.chunks.push_back(layer_chunk("child", true, 0, 1, 255));
    f0.chunks.push_back(raw_cel(0, 0, 0, 255, 1, 1, white));
    f0.chunks.push_back(raw_cel(1, 0, 0, 128, 1, 1, black));
    f0.chunks.push_back(raw_cel(3, 1, 0, 255, 1, 1, black));

    const std::vector<u8> file = build_ase(2, 1, 32, 0, {f0});
    AseDocument doc;
    std::string error;
    CHECK(parse_aseprite(file.data(), file.size(), doc, error));
    const u8* p = pixel(doc, 0, 0, 0);
    CHECK(p[3] == 255);
    CHECK(p[0] > 110 && p[0] < 145); // ~50% grey
    CHECK(is(pixel(doc, 0, 1, 0), 0, 0, 0, 0)); // child of hidden group
}

void test_indexed_and_linked() {
    // Indexed mode: index 0 is transparent, index 1 red. Frame 1 links to frame 0.
    Frame f0{50, {}};
    f0.chunks.push_back(palette_chunk({{0, 0, 0, 255}, {200, 10, 10, 255}}));
    f0.chunks.push_back(layer_chunk("base", true, 0, 0, 255));
    f0.chunks.push_back(raw_cel(0, 0, 0, 255, 2, 1, {0, 1}));
    Frame f1{50, {}};
    f1.chunks.push_back(linked_cel(0, 0));

    const std::vector<u8> file = build_ase(2, 1, 8, 0, {f0, f1});
    AseDocument doc;
    std::string error;
    const bool ok = parse_aseprite(file.data(), file.size(), doc, error);
    if (!ok) std::printf("parse error: %s\n", error.c_str());
    CHECK(ok);
    CHECK(is(pixel(doc, 0, 0, 0), 0, 0, 0, 0));
    CHECK(is(pixel(doc, 0, 1, 0), 200, 10, 10, 255));
    CHECK(is(pixel(doc, 1, 1, 0), 200, 10, 10, 255)); // via the link
}

Chunk tileset_chunk(u32 id, const std::string& name, u16 tw, u16 th, u32 count, const std::vector<u8>& pixels) {
    Chunk c{0x2023, {}};
    c.body.d(id);
    c.body.d(2); // tiles embedded
    c.body.d(count);
    c.body.w(tw);
    c.body.w(th);
    c.body.w(0); // base index
    for (int i = 0; i < 14; ++i) c.body.b(0);
    c.body.s(name);
    const std::vector<u8> z = zlib_stored(pixels);
    c.body.d(static_cast<u32>(z.size()));
    c.body.raw(z);
    return c;
}

Chunk tilemap_layer_chunk(const std::string& name, u32 tileset) {
    Chunk c = layer_chunk(name, true, 2, 0, 255);
    c.body.d(tileset);
    return c;
}

Chunk tilemap_cel(u16 layer, i16 x, i16 y, u16 w, u16 h, const std::vector<u32>& tiles) {
    Chunk c{0x2005, {}};
    c.body.w(layer);
    c.body.w(static_cast<u16>(x));
    c.body.w(static_cast<u16>(y));
    c.body.b(255);
    c.body.w(3); // compressed tilemap
    c.body.w(0);
    for (int i = 0; i < 5; ++i) c.body.b(0);
    c.body.w(w);
    c.body.w(h);
    c.body.w(32);
    c.body.d(0x1fffffff);
    c.body.d(0x80000000);
    c.body.d(0x40000000);
    c.body.d(0x20000000);
    for (int i = 0; i < 10; ++i) c.body.b(0);
    std::vector<u8> raw;
    for (u32 t : tiles) {
        raw.push_back(static_cast<u8>(t));
        raw.push_back(static_cast<u8>(t >> 8));
        raw.push_back(static_cast<u8>(t >> 16));
        raw.push_back(static_cast<u8>(t >> 24));
    }
    c.body.raw(zlib_stored(raw));
    return c;
}

void test_tilemap_layer() {
    // Tileset of two 2x2 tiles: tile 0 empty, tile 1 solid green with one red pixel top-left.
    std::vector<u8> tiles(2 * 2 * 2 * 4, 0);
    for (int i = 4; i < 8; ++i) { // second tile
        u8* p = &tiles[static_cast<usize>(i) * 4];
        p[0] = 0; p[1] = 255; p[2] = 0; p[3] = 255;
    }
    tiles[4 * 4 + 0] = 255; tiles[4 * 4 + 1] = 0; // first pixel of tile 1 is red

    // 3x2 map on a 6x4 canvas: tile 1 at (0,0), flipped-x tile 1 at (2,1).
    Frame f0{100, {}};
    f0.chunks.push_back(tileset_chunk(0, "ground", 2, 2, 2, tiles));
    f0.chunks.push_back(tilemap_layer_chunk("level", 0));
    f0.chunks.push_back(tilemap_cel(0, 0, 0, 3, 2, {1, 0, 0, 0, 0, 1u | 0x80000000u}));

    const std::vector<u8> file = build_ase(6, 4, 32, 0, {f0});
    AseDocument doc;
    std::string error;
    const bool ok = parse_aseprite(file.data(), file.size(), doc, error);
    if (!ok) std::printf("parse error: %s\n", error.c_str());
    CHECK(ok);
    CHECK(doc.tilesets.size() == 1);
    CHECK(doc.tilesets[0].name == "ground" && doc.tilesets[0].tile_count == 2);
    CHECK(doc.tilesets[0].rgba.size() == 2 * 4 * 4);
    CHECK(doc.tilemaps.size() == 1);
    CHECK(doc.tilemaps[0].layer_name == "level");
    CHECK(doc.tilemaps[0].width == 3 && doc.tilemaps[0].height == 2);
    CHECK(doc.tilemaps[0].cells[0] == 1);
    CHECK(doc.tilemaps[0].cells[5] == (1u | 0x80000000u));

    // Composited frame: tile at (0,0) shows red at its top-left; the flipped
    // tile at pixel (4,2) shows red at its top-right.
    CHECK(is(pixel(doc, 0, 0, 0), 255, 0, 0, 255));
    CHECK(is(pixel(doc, 0, 1, 0), 0, 255, 0, 255));
    CHECK(is(pixel(doc, 0, 2, 0), 0, 0, 0, 0));
    CHECK(is(pixel(doc, 0, 5, 2), 255, 0, 0, 255));
    CHECK(is(pixel(doc, 0, 4, 2), 0, 255, 0, 255));
}

void test_rejects_garbage() {
    AseDocument doc;
    std::string error;
    std::vector<u8> junk(300, 0x42);
    CHECK(!parse_aseprite(junk.data(), junk.size(), doc, error));
    CHECK(!error.empty());

    // A valid file cut off halfway must fail cleanly, not crash.
    const std::vector<u8> red = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    Frame f0{100, {}};
    f0.chunks.push_back(layer_chunk("base", true, 0, 0, 255));
    f0.chunks.push_back(raw_cel(0, 0, 0, 255, 2, 2, red));
    std::vector<u8> file = build_ase(2, 2, 32, 0, {f0});
    file.resize(file.size() - 10);
    CHECK(!parse_aseprite(file.data(), file.size(), doc, error));
}

SpriteSheet test_sheet() {
    SpriteSheet s;
    for (int i = 0; i < 6; ++i) s.frames.push_back(SpriteFrame{Rect{i * 8.0f, 0, 8, 8}, 100});
    s.tags.push_back(SpriteTag{"loop", 0, 2, TagDirection::Forward, 0});
    s.tags.push_back(SpriteTag{"once", 3, 5, TagDirection::Forward, 1});
    s.tags.push_back(SpriteTag{"ping", 0, 2, TagDirection::PingPong, 0});
    s.tags.push_back(SpriteTag{"rev", 3, 5, TagDirection::Reverse, 2});
    return s;
}

void test_animation() {
    const SpriteSheet s = test_sheet();
    const float dt = 1.0f / 60.0f;

    AnimState a;
    anim_play(a, s, "loop");
    CHECK(a.frame == 0);
    for (int i = 0; i < 6; ++i) anim_advance(a, s, dt); // 100 ms
    CHECK(a.frame == 1);
    for (int i = 0; i < 12; ++i) anim_advance(a, s, dt); // +200 ms -> wrapped
    CHECK(a.frame == 0);
    CHECK(a.loops == 1);
    CHECK(!a.finished);

    // Non-looping: plays 3 frames then parks on the last one.
    anim_play(a, s, "once");
    CHECK(a.frame == 3);
    for (int i = 0; i < 60; ++i) anim_advance(a, s, dt); // one full second
    CHECK(a.frame == 5);
    CHECK(a.finished);
    anim_play(a, s, "once"); // finished, so playing again restarts
    CHECK(a.frame == 3 && !a.finished);

    // Ping-pong: 0 1 2 1 0 1 2 ...
    anim_play(a, s, "ping");
    std::vector<u16> seen{a.frame};
    for (int step = 0; step < 6; ++step) {
        for (int i = 0; i < 6; ++i) anim_advance(a, s, dt);
        seen.push_back(a.frame);
    }
    const std::vector<u16> expected{0, 1, 2, 1, 0, 1, 2};
    CHECK(seen == expected);

    // Reverse with repeat 2: 5 4 3 | 5 4 3 stop.
    anim_play(a, s, "rev");
    CHECK(a.frame == 5);
    for (int i = 0; i < 6 * 5; ++i) anim_advance(a, s, dt);
    CHECK(a.frame == 3 && !a.finished);
    for (int i = 0; i < 6; ++i) anim_advance(a, s, dt);
    CHECK(a.finished && a.frame == 3);

    // A huge step skips several frames without losing the remainder.
    anim_play(a, s, "loop", true);
    anim_advance(a, s, 0.25f); // 250 ms = 2 frames + 50 ms
    CHECK(a.frame == 2);
    CHECK(a.elapsed_us == 50000);

    // Same inputs, same result: the player is deterministic by construction.
    AnimState b1, b2;
    anim_play(b1, s, "ping");
    anim_play(b2, s, "ping");
    for (int i = 0; i < 500; ++i) {
        anim_advance(b1, s, dt);
        anim_advance(b2, s, dt);
    }
    CHECK(std::memcmp(&b1, &b2, sizeof b1) == 0);
}

void test_grid_sheet() {
    Texture tex;
    tex.width = 32;
    tex.height = 16;
    const SpriteSheet s = sprite_sheet_from_grid(&tex, 8, 8, 8, 120);
    CHECK(s.frames.size() == 8);
    CHECK(s.frames[3].src.x == 24 && s.frames[3].src.y == 0);
    CHECK(s.frames[4].src.x == 0 && s.frames[4].src.y == 8);
    CHECK(s.frames[7].duration_ms == 120);
}

} // namespace

int main() {
    test_rgba_document();
    test_opacity_and_groups();
    test_indexed_and_linked();
    test_tilemap_layer();
    test_rejects_garbage();
    test_animation();
    test_grid_sheet();

    if (failures == 0) {
        std::printf("assets_test: all checks passed\n");
        return 0;
    }
    std::printf("assets_test: %d check(s) failed\n", failures);
    return 1;
}
