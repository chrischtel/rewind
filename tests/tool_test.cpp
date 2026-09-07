// Headless checks for the network editor tool driven by fake mouse input.

#include <rewind/debug/editor_tool.h>
#include <rewind/sim/network.h>

#include <cmath>
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

/// One click: press then release at `at`. Returns whether the tool reported an edit.
bool click(NetworkTool& tool, Vec2 at, bool right = false) {
    ToolContext ctx;
    ctx.hovered = true;
    ctx.mouse_world = at;
    if (right) {
        ctx.right_pressed = true;
        tool.update(ctx);
        return ctx.edited;
    }
    ctx.pressed = true;
    ctx.down = true;
    tool.update(ctx);
    bool edited = ctx.edited;
    ToolContext up;
    up.hovered = true;
    up.mouse_world = at;
    up.released = true;
    tool.update(up);
    return edited || up.edited;
}

void drag(NetworkTool& tool, Vec2 from, Vec2 to) {
    ToolContext ctx;
    ctx.hovered = true;
    ctx.mouse_world = from;
    ctx.pressed = true;
    ctx.down = true;
    tool.update(ctx);
    for (int i = 1; i <= 5; ++i) {
        ToolContext move;
        move.hovered = true;
        move.down = true;
        move.mouse_world = lerp(from, to, static_cast<float>(i) / 5);
        tool.update(move);
    }
    ToolContext up;
    up.hovered = true;
    up.mouse_world = to;
    up.released = true;
    tool.update(up);
}

void test_place_and_connect() {
    CurveNetwork net;
    NetworkTool tool(&net);
    tool.forward_tracks = 1;
    tool.backward_tracks = 1;
    tool.track_width = 4;
    CHECK(click(tool, {0, 0}));
    CHECK(net.nodes().size() == 1 && net.edges().empty());
    CHECK(tool.chain_from() == 0);
    CHECK(click(tool, {100, 0}));
    CHECK(net.nodes().size() == 2 && net.edges().size() == 1);
    CHECK(net.edge(0).tracks.size() == 2 && net.track_count() == 2);
    CHECK(click(tool, {200, 50}));
    CHECK(net.edges().size() == 2);
    // Right click ends the chain; the next click starts fresh.
    click(tool, {0, 0}, true);
    CHECK(tool.chain_from() == ~0u);
    CHECK(click(tool, {0, 100}));
    CHECK(net.nodes().size() == 4 && net.edges().size() == 2);
    // Clicking an existing node connects to it (within the pick radius).
    CHECK(click(tool, {2, 1}));
    CHECK(net.edges().size() == 3 && net.edge(2).a == 3 && net.edge(2).b == 0);
    // Hovering a node reports it; empty space reports nothing.
    ToolContext hover;
    hover.hovered = true;
    hover.mouse_world = {100, 1};
    tool.update(hover);
    CHECK(tool.hovered_node() == 1);
    hover.mouse_world = {500, 500};
    tool.update(hover);
    CHECK(tool.hovered_node() == ~0u && tool.hovered_edge() == ~0u);
    CHECK(!hover.status.empty());

    // Drag moves a node and the edge follows after the rebuild.
    drag(tool, {100, 0}, {100, 40});
    CHECK(net.node(1).pos.y > 30);
    CHECK(net.edge(0).path.end() == net.node(1).pos);

    // Split and delete.
    tool.mode = NetworkTool::Mode::Split;
    CHECK(click(tool, {50, 20}));
    CHECK(net.nodes().size() == 5);
    tool.mode = NetworkTool::Mode::Delete;
    const usize edges_before = net.edges().size();
    CHECK(click(tool, {200, 50}));
    CHECK(!net.valid_node(2));
    usize alive = 0;
    for (const NetEdge& e : net.edges()) alive += e.alive ? 1 : 0;
    CHECK(alive < edges_before);
    // Tool without a network does nothing but say so.
    NetworkTool empty(nullptr);
    ToolContext ctx;
    ctx.hovered = true;
    ctx.pressed = true;
    empty.update(ctx);
    CHECK(!ctx.edited && ctx.status == "no network");
    empty.cancel();
}

/// Draw mode: a freehand arc becomes one curved edge between two new nodes,
/// and a second stroke starting on a node continues from it.
void test_draw() {
    CurveNetwork net;
    NetworkTool tool(&net);
    tool.mode = NetworkTool::Mode::Draw;
    ToolContext down;
    down.hovered = true;
    down.pressed = true;
    down.down = true;
    down.mouse_world = {0, 0};
    tool.update(down);
    for (int i = 1; i < 20; ++i) {
        ToolContext move;
        move.hovered = true;
        move.down = true;
        const float k = static_cast<float>(i) / 20;
        move.mouse_world = {k * 200, -std::sin(k * 3.1415926f) * 80};
        tool.update(move);
    }
    CHECK(tool.stroke().size() > 10);
    ToolContext up;
    up.hovered = true;
    up.released = true;
    up.mouse_world = {200, 0};
    tool.update(up);
    CHECK(up.edited && tool.stroke().empty());
    CHECK(net.nodes().size() == 2 && net.edges().size() == 1);
    const NetEdge& e = net.edge(0);
    CHECK(distance(e.path.start(), net.node(e.a).pos) < 1e-3f && distance(e.path.end(), net.node(e.b).pos) < 1e-3f);
    PathSampler s(e.path, 0.1f);
    const Vec2 mid = s.at(s.length() * 0.5f).position;
    CHECK(mid.y < -60 && mid.y > -95 && std::fabs(mid.x - 100) < 15); // follows the arc, not the chord
    CHECK(!e.tracks.empty());

    // Start on the end node: the new edge leaves from it.
    ToolContext again;
    again.hovered = true;
    again.pressed = true;
    again.down = true;
    again.mouse_world = {202, 1};
    tool.update(again);
    for (int i = 1; i <= 10; ++i) {
        ToolContext move;
        move.hovered = true;
        move.down = true;
        move.mouse_world = {200 + static_cast<float>(i) * 10, static_cast<float>(i) * 5};
        tool.update(move);
    }
    ToolContext up2;
    up2.hovered = true;
    up2.released = true;
    up2.mouse_world = {300, 50};
    tool.update(up2);
    CHECK(up2.edited && net.nodes().size() == 3 && net.edges().size() == 2);
    CHECK(net.edge(1).a == e.b);

    // A tiny stroke is a slip, not an edge.
    ToolContext tap;
    tap.hovered = true;
    tap.pressed = true;
    tap.down = true;
    tap.mouse_world = {500, 500};
    tool.update(tap);
    ToolContext tap_up;
    tap_up.hovered = true;
    tap_up.released = true;
    tap_up.mouse_world = {502, 500};
    tool.update(tap_up);
    CHECK(!tap_up.edited && net.nodes().size() == 3);
}

} // namespace

int main() {
    test_draw();
    test_place_and_connect();
    if (failures == 0) std::printf("tool_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
