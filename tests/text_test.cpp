// Headless checks for text: UTF-8 decoding, measurement, and (when a system
// font is available) baking a real TrueType file.

#include <rewind/render/font.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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

void test_utf8() {
    const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
    usize i = 0;
    CHECK(utf8_next(text, i) == 'a');
    CHECK(utf8_next(text, i) == 0xE9);    // é
    CHECK(utf8_next(text, i) == 0x20AC);  // €
    CHECK(utf8_next(text, i) == 0x1F600); // 😀
    CHECK(utf8_next(text, i) == 'z');
    CHECK(i == text.size());

    // Broken sequences decode to U+FFFD and never get stuck.
    const std::string bad = "\xC3";
    i = 0;
    CHECK(utf8_next(bad, i) == 0xFFFD);
    CHECK(i == 1);
    const std::string bad2 = "\xE2\x82x";
    i = 0;
    CHECK(utf8_next(bad2, i) == 0xFFFD);
    CHECK(i == 1);
    const std::string stray = "\x80";
    i = 0;
    CHECK(utf8_next(stray, i) == 0xFFFD);
}

void test_measure_synthetic() {
    FontAtlas a;
    a.line_height = 10;
    a.glyphs['a'] = Glyph{Rect{0, 0, 5, 7}, 0, -7, 6};
    a.glyphs['b'] = Glyph{Rect{5, 0, 5, 7}, 0, -7, 7};
    a.glyphs['?'] = Glyph{Rect{10, 0, 5, 7}, 0, -7, 8};
    a.kerning[(static_cast<u64>('a') << 32) | 'b'] = -1;

    CHECK(a.measure("").x == 0 && a.measure("").y == 10);
    CHECK(a.measure("a").x == 6);
    CHECK(a.measure("ab").x == 12); // 6 + 7 - 1 kerning
    CHECK(a.measure("ab\nb").x == 12 && a.measure("ab\nb").y == 20);
    CHECK(a.measure("z").x == 8); // unknown glyphs fall back to '?'
    CHECK(a.measure("ab", 2).x == 24);
    CHECK(a.find('a') != nullptr && a.find('q') == a.find('?'));
}

void test_bake_system_font() {
    const char* candidates[] = {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    std::string path;
    for (const char* c : candidates)
        if (std::filesystem::exists(c)) {
            path = c;
            break;
        }
    if (path.empty()) {
        std::printf("  (no system font found, bake test skipped)\n");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<u8> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    FontAtlas atlas;
    std::string error;
    const bool ok = bake_font(ttf.data(), ttf.size(), 18.0f, atlas, &error);
    if (!ok) std::printf("bake error: %s\n", error.c_str());
    CHECK(ok);
    CHECK(atlas.glyphs.size() >= 95);
    CHECK(atlas.width >= 128 && atlas.alpha.size() == static_cast<usize>(atlas.width) * atlas.height);
    CHECK(atlas.ascent > 0 && atlas.descent < 0 && atlas.line_height > atlas.ascent);
    const Glyph* h = atlas.find('H');
    CHECK(h && h->advance > 5 && h->src.w > 0);
    CHECK(atlas.measure("Hello").x > atlas.measure("Hi").x);
    CHECK(atlas.measure("Hello\nWorld").y > atlas.measure("Hello").y);
    // Something in the atlas is actually inked.
    bool any = false;
    for (u8 v : atlas.alpha) any = any || v > 0;
    CHECK(any);

    // Garbage is rejected.
    std::vector<u8> junk(100, 7);
    CHECK(!bake_font(junk.data(), junk.size(), 18.0f, atlas, &error));
    CHECK(!error.empty());
}

} // namespace

int main() {
    test_utf8();
    test_measure_synthetic();
    test_bake_system_font();

    if (failures == 0) {
        std::printf("text_test: all checks passed\n");
        return 0;
    }
    std::printf("text_test: %d check(s) failed\n", failures);
    return 1;
}
