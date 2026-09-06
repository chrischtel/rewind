// Headless checks for the game UI (layout and interaction without a
// renderer) and for commands travelling through the input frame.

#include <rewind/render/ui.h>
#include <rewind/sim/input.h>
#include <rewind/sim/text_format.h>

#include <cstdio>

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

UiInput at(Vec2 mouse, bool down = false, bool pressed = false, bool released = false) {
    UiInput in;
    in.mouse = mouse;
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    return in;
}

/// One frame of a small panel. Returns what the button reported.
struct FrameResult {
    bool clicked = false;
    bool toggled = false;
    bool slid = false;
    Rect button_rect;
    Rect slider_rect;
    bool wants_mouse = false;
};

FrameResult frame(Ui& ui, const UiInput& in, bool& flag, float& value) {
    FrameResult r;
    ui.begin(nullptr, in);
    ui.begin_panel({100, 100, 300, 200}, "Panel");
    ui.label("hello");
    r.clicked = ui.button("Press");
    r.button_rect = ui.last_rect();
    ui.same_line();
    r.toggled = ui.toggle("flag", flag);
    r.slid = ui.slider("value", value, 0, 10, 200);
    r.slider_rect = ui.last_rect();
    ui.progress(0.5f);
    ui.end_panel();
    r.wants_mouse = ui.wants_mouse();
    ui.end();
    return r;
}

void test_layout_and_clicks() {
    Ui ui;
    bool flag = false;
    float value = 5;
    FrameResult a = frame(ui, at({0, 0}), flag, value);
    // Layout flows down inside the panel, the toggle sits beside the button.
    CHECK(a.button_rect.x == 108 && a.button_rect.y > 100 + 8);
    CHECK(a.slider_rect.y > a.button_rect.bottom());
    CHECK(a.slider_rect.w == 200);
    CHECK(!a.clicked && !a.wants_mouse);

    // Hover, press, release inside: one click. Hover state is a frame behind, like the gizmos.
    const Vec2 center{a.button_rect.x + a.button_rect.w / 2, a.button_rect.y + a.button_rect.h / 2};
    FrameResult hover = frame(ui, at(center), flag, value);
    CHECK(!hover.clicked);
    FrameResult wants = frame(ui, at(center), flag, value);
    CHECK(wants.wants_mouse); // the mouse is over the panel
    FrameResult press = frame(ui, at(center, true, true), flag, value);
    CHECK(!press.clicked && ui.any_active());
    FrameResult release = frame(ui, at(center, false, false, true), flag, value);
    CHECK(release.clicked);
    CHECK(!ui.any_active());

    // Press inside, drag out, release outside: no click.
    frame(ui, at(center, true, true), flag, value);
    FrameResult away = frame(ui, at({0, 0}, false, false, true), flag, value);
    CHECK(!away.clicked);

    // Toggle flips on click.
    const Rect t = Rect{a.button_rect.right() + 6, a.button_rect.y, 60, a.button_rect.h};
    const Vec2 tc{t.x + 8, t.y + t.h / 2};
    frame(ui, at(tc), flag, value);
    frame(ui, at(tc, true, true), flag, value);
    FrameResult toggled = frame(ui, at(tc, false, false, true), flag, value);
    CHECK(toggled.toggled && flag);

    // Slider: press at 30% of its width sets the value.
    const Vec2 sc{a.slider_rect.x + a.slider_rect.w * 0.3f, a.slider_rect.y + 5};
    frame(ui, at(sc), flag, value);
    FrameResult slid = frame(ui, at(sc, true, true), flag, value);
    CHECK(slid.slid && value > 2.9f && value < 3.1f);
    frame(ui, at({sc.x + 100, sc.y}, true), flag, value); // drag right while held
    CHECK(value > 7.9f && value < 8.1f);
    frame(ui, at({sc.x + 100, sc.y}, false, false, true), flag, value);

    // Disabled input: nothing reacts.
    UiInput off = at(center, true, true);
    off.enabled = false;
    frame(ui, off, flag, value);
    FrameResult none = frame(ui, off, flag, value);
    CHECK(!none.clicked && !none.wants_mouse);

    // Free placement and measuring.
    ui.begin(nullptr, at({0, 0}));
    CHECK(ui.measure("abc").x == 36 && ui.measure("abc").y == 16); // pixel font at scale 2
    CHECK(ui.measure("a\nb").y == 32);
    ui.button_at({10, 10, 50, 20}, "x");
    CHECK(ui.last_rect().w == 50);
    ui.end();
}

void test_commands_in_input() {
    InputFrame in;
    in.command = 7;
    in.command_value = -3;
    const std::string json = to_json(in);
    CHECK(json.find("\"command\": 7") != std::string::npos || json.find("\"command\":7") != std::string::npos);
    InputFrame back;
    std::string error;
    CHECK(from_json(json, back, &error) && back == in);
    CHECK(!(InputFrame{} == in)); // commands are part of the input record and its hash
}

} // namespace

int main() {
    test_layout_and_clicks();
    test_commands_in_input();
    if (failures == 0) std::printf("ui_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
