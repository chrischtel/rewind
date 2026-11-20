// Headless checks for the game UI (layout and interaction without a
// renderer) and for commands travelling through the input frame.

#include <rewind/render/ui.h>
#include <rewind/sim/input.h>
#include <rewind/sim/text_format.h>

#include <cstdio>
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

// ---- the editor and analysis widgets -------------------------------------------------

Vec2 middle_of(const Rect& r) { return {r.x + r.w / 2, r.y + r.h / 2}; }

/// Press at `p`, then release there: two frames of `run`.
template <class Frame> void click(Frame&& run, Vec2 p) {
    run(at(p, true, true));
    run(at(p, false, false, true));
}

/// A frame of keyboard only, the mouse parked in the corner.
UiInput typed(const char* s) {
    UiInput in = at({0, 0});
    in.text = s;
    return in;
}

UiInput key(bool UiInput::* which) {
    UiInput in = at({0, 0});
    in.*which = true;
    return in;
}

void test_tabs_and_selectable() {
    Ui ui;
    int tab = 0;
    int picked = -1;
    bool changed = false;
    Rect tabs_rect, rows[3];
    auto run = [&](const UiInput& in) {
        ui.begin(nullptr, in);
        ui.begin_panel({100, 100, 300, 300});
        changed = ui.tabs("view", {"One", "Two", "Three"}, tab);
        tabs_rect = ui.last_rect();
        ui.separator();
        ui.heading("Rows");
        for (int i = 0; i < 3; ++i) {
            if (ui.selectable(i == 0 ? "first" : (i == 1 ? "second" : "third"), picked == i)) picked = i;
            rows[i] = ui.last_rect();
        }
        ui.end_panel();
        ui.end();
    };
    run(at({0, 0}));
    CHECK(tabs_rect.x == 108 && tabs_rect.w == 52 + 52 + 76); // each tab: its text plus padding

    // "Two" is the second tab. The switch happens on release, once.
    const Vec2 two{tabs_rect.x + 52 + 26, tabs_rect.y + tabs_rect.h / 2};
    run(at(two, true, true));
    CHECK(!changed && tab == 0);
    run(at(two, false, false, true));
    CHECK(changed && tab == 1);
    click(run, two);
    CHECK(!changed && tab == 1); // the current tab again: nothing changed

    // List rows span the panel and touch; a click picks one.
    CHECK(rows[0].w == 284 && rows[1].y == rows[0].bottom() && rows[2].y == rows[1].bottom());
    click(run, middle_of(rows[2]));
    CHECK(picked == 2);
    click(run, middle_of(rows[0]));
    CHECK(picked == 0 && tab == 1);
}

void test_scroll_area() {
    Ui ui;
    constexpr int count = 10;
    Rect view, below, rows[count];
    int clicked = -1;
    auto run = [&](const UiInput& in) {
        clicked = -1;
        ui.begin(nullptr, in);
        ui.begin_panel({100, 100, 300, 400});
        ui.begin_scroll("list", 100);
        char name[16];
        for (int i = 0; i < count; ++i) {
            std::snprintf(name, sizeof name, "row %d", i);
            if (ui.selectable(name, false)) clicked = i;
            rows[i] = ui.last_rect();
        }
        ui.end_scroll();
        view = ui.last_rect();
        ui.label("below");
        below = ui.last_rect();
        ui.end_panel();
        ui.end();
    };
    run(at({0, 0}));
    CHECK(view.x == 108 && view.y == 108 && view.w == 284 && view.h == 100);
    CHECK(rows[0].y >= view.y && rows[1].y == rows[0].bottom());
    CHECK(below.y == view.bottom() + 6); // the panel flows on under the area, whatever is inside

    // Row 5 is laid out below the view: on the panel, but culled, so it does not react.
    CHECK(rows[5].y > view.bottom());
    click(run, middle_of(rows[5]));
    CHECK(clicked == -1);
    // A row in view does.
    click(run, middle_of(rows[1]));
    CHECK(clicked == 1);

    // The wheel over the area scrolls it, visible a frame later like hover.
    UiInput wheel = at(middle_of(view));
    wheel.wheel = -2;
    run(wheel);
    run(at(middle_of(view)));
    CHECK(rows[0].bottom() < view.y); // scrolled up and out
    int top = -1;
    for (int i = 0; i < count && top < 0; ++i)
        if (rows[i].y >= view.y) top = i;
    CHECK(top > 0);
    if (top > 0) {
        // The first row wholly in view is one further down the list, and the click reaches that one.
        click(run, {rows[top].x + 20, rows[top].y + 13});
        CHECK(clicked == top);
        // The row above it straddles the edge: culled whole, so its strip inside the view is dead.
        if (rows[top - 1].bottom() > view.y + 1) {
            click(run, {rows[top].x + 20, view.y + 1});
            CHECK(clicked == -1);
        }
    }

    // Scrolling stops at the end of the content: the last row sits wholly in view.
    wheel.wheel = -100;
    run(wheel);
    run(at({0, 0}));
    CHECK(rows[count - 1].y >= view.y && rows[count - 1].bottom() <= view.bottom());
    // The wheel elsewhere is not the area's.
    UiInput outside = at({0, 0});
    outside.wheel = 5;
    run(outside);
    run(at({0, 0}));
    CHECK(rows[count - 1].bottom() <= view.bottom() && rows[0].bottom() < view.y);
}

void test_fields() {
    Ui ui;
    std::string name;
    float seconds = 5;
    bool text_changed = false, number_changed = false;
    Rect text_rect, number_rect;
    auto run = [&](const UiInput& in) {
        ui.begin(nullptr, in);
        ui.begin_panel({100, 100, 300, 200});
        text_changed = ui.text_field("name", name);
        text_rect = ui.last_rect();
        number_changed = ui.number_field("seconds", seconds, 1, 0, 10);
        number_rect = ui.last_rect();
        ui.end_panel();
        ui.end();
    };
    run(at({0, 0}));
    CHECK(text_rect.w == 160 && number_rect.w == 100 && !ui.wants_keyboard());
    // Typing without a focused field goes nowhere.
    run(typed("zz"));
    CHECK(name.empty() && !text_changed);

    // A click gives the field the keyboard; text lands at the caret.
    click(run, middle_of(text_rect));
    CHECK(ui.wants_keyboard());
    run(typed("ab"));
    CHECK(text_changed && name == "ab");
    run(key(&UiInput::key_backspace));
    CHECK(text_changed && name == "a");
    run(key(&UiInput::key_left));
    CHECK(!text_changed);
    run(typed("x"));
    CHECK(name == "xa");
    run(key(&UiInput::key_delete));
    CHECK(name == "x");
    run(key(&UiInput::key_enter));
    CHECK(!ui.wants_keyboard());
    run(typed("zz"));
    CHECK(name == "x" && !text_changed);

    // A click elsewhere takes the keyboard away too.
    const Vec2 elsewhere{105, 290}; // on the panel, on no widget
    click(run, middle_of(text_rect));
    CHECK(ui.wants_keyboard());
    click(run, elsewhere);
    CHECK(!ui.wants_keyboard());

    // Number field: + and - step the value, the range holds.
    const Vec2 plus{number_rect.right() - 5, number_rect.y + 13};
    const Vec2 minus{number_rect.x + 5, number_rect.y + 13};
    click(run, plus);
    CHECK(number_changed && seconds == 6);
    for (int i = 0; i < 6; ++i) click(run, plus);
    CHECK(seconds == 10 && !number_changed); // at the top: the last click changed nothing
    click(run, minus);
    CHECK(number_changed && seconds == 9);

    // Typed text becomes the value on Enter, letters dropped, clamped.
    const Vec2 digits{number_rect.right() - 30, number_rect.y + 13}; // right of the text: caret at its end
    click(run, digits);
    CHECK(ui.wants_keyboard());
    run(key(&UiInput::key_backspace));
    run(typed("4x2"));
    CHECK(seconds == 9); // not before the commit
    run(key(&UiInput::key_enter));
    CHECK(number_changed && seconds == 10 && !ui.wants_keyboard());

    // Losing the keyboard to a click commits as well.
    click(run, digits);
    run(key(&UiInput::key_backspace));
    run(key(&UiInput::key_backspace));
    run(typed("3"));
    click(run, elsewhere);
    CHECK(seconds == 3 && !ui.wants_keyboard());

    // Escape drops what was typed.
    click(run, digits);
    run(typed("7"));
    run(key(&UiInput::key_escape));
    CHECK(seconds == 3 && !ui.wants_keyboard());
}

void test_dropdown() {
    Ui ui;
    int choice = 2;
    bool changed = false, below_clicked = false, wants = false;
    Rect box;
    auto run = [&](const UiInput& in) {
        ui.begin(nullptr, in);
        ui.begin_panel({100, 100, 300, 300});
        changed = ui.dropdown("mode", {"Fixed", "Actuated", "Off"}, choice);
        box = ui.last_rect();
        if (ui.button("Below", 160)) below_clicked = true;
        ui.end_panel();
        wants = ui.wants_mouse();
        ui.end();
    };
    run(at({0, 0}));
    CHECK(box.w == 160 && !wants);

    // A click opens the list, and the open list owns the mouse wherever it is.
    click(run, middle_of(box));
    run(at({0, 0}));
    CHECK(wants && !changed && choice == 2);

    // Its first option lies over the button below: the click picks the option and nothing else.
    const Vec2 first{box.x + 20, box.bottom() + 2 + 13};
    run(at(first, true, true));
    CHECK(changed && choice == 0);
    run(at(first, false, false, true));
    run(at(first));
    CHECK(!below_clicked && !changed);
    run(at({0, 0}));
    run(at({0, 0}));
    CHECK(!wants); // closed

    // Open again; a click elsewhere closes it and changes nothing.
    click(run, middle_of(box));
    CHECK(wants);
    click(run, {300, 350});
    run(at({0, 0}));
    run(at({0, 0}));
    CHECK(choice == 0 && !wants && !below_clicked);

    // With the list gone the button below is a button again.
    click(run, {box.x + 20, box.bottom() + 6 + 13});
    CHECK(below_clicked);
}

void test_table() {
    Ui ui;
    const std::vector<UiColumn> columns = {{"Name", 120, true}, {"Flow", 80, true}, {"Note", 0, false}};
    Rect header, cells[6];
    int sort_column = -1;
    bool descending = false;
    auto run = [&](const UiInput& in) {
        ui.begin(nullptr, in);
        ui.begin_panel({100, 100, 400, 300});
        ui.begin_table("links", columns);
        header = ui.last_rect();
        sort_column = ui.table_sort_column();
        descending = ui.table_sort_descending();
        for (int i = 0; i < 6; ++i) {
            ui.table_cell(i % 3 == 0 ? "Main St" : "12");
            cells[i] = ui.last_rect();
        }
        ui.end_table();
        ui.end_panel();
        ui.end();
    };
    run(at({0, 0}));
    CHECK(header.x == 108 && header.w == 384 && sort_column == -1);
    // Cells go left to right by the column widths (the one without takes what is left) and wrap.
    CHECK(cells[0].x == header.x && cells[0].y == header.bottom() && cells[0].w == 120);
    CHECK(cells[1].x == cells[0].x + 120 && cells[2].x == cells[1].x + 80 && cells[2].w == 184);
    CHECK(cells[3].x == cells[0].x && cells[3].y == cells[0].bottom());

    // A sortable header picks its column, a second click flips the direction.
    const float y = header.y + header.h / 2;
    click(run, {header.x + 160, y});
    CHECK(sort_column == 1 && !descending);
    click(run, {header.x + 160, y});
    CHECK(sort_column == 1 && descending);
    click(run, {header.x + 40, y});
    CHECK(sort_column == 0 && !descending);
    click(run, {header.x + 300, y}); // "Note" is not sortable
    CHECK(sort_column == 0 && !descending);
}

void test_line_chart() {
    Ui ui;
    const float flow[] = {1, 4, 2, 8, 5};
    const float one[] = {3};
    const UiSeries series[] = {
        {flow, 5, rgb(86, 156, 214), "flow"},
        {one, 1, rgb(214, 120, 86), "single"},
        {nullptr, 0, rgb(255, 255, 255), "empty"},
        {flow, 0, rgb(255, 255, 255), {}},
    };
    Rect chart;
    auto run = [&](const UiInput& in) {
        ui.begin(nullptr, in);
        ui.begin_panel({100, 100, 400, 500});
        ui.line_chart(series, 4, 300, 120);
        chart = ui.last_rect();
        ui.line_chart(nullptr, 0, 0, 60);             // no series at all
        ui.line_chart(series + 1, 1, 200, 60, 0, 10); // a single sample, fixed range
        ui.line_chart(series + 2, 2, 200, 60);        // only empty series
        ui.line_chart(series + 1, 1, 200, 60);        // a single sample, range from the data
        ui.end_panel();
        ui.end();
    };
    run(at({0, 0}));
    CHECK(chart.w == 300 && chart.h == 120 + 16 + 6); // the legend row under the plot

    // The mouse across all of them, marker and tooltip included, and a click for good measure.
    for (float y = 110; y < 590; y += 9) {
        for (float x = 104; x < 420; x += 13) run(at({x, y}));
    }
    click(run, middle_of(chart));
    CHECK(!ui.any_active());
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
    test_tabs_and_selectable();
    test_scroll_area();
    test_fields();
    test_dropdown();
    test_table();
    test_line_chart();
    test_commands_in_input();
    if (failures == 0) std::printf("ui_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
