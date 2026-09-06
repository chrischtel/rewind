// Headless checks for the network editor tool driven by fake mouse input.

#include <rewind/debug/editor_tool.h>
#include <rewind/sim/network.h>

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

} // namespace

int main() {
    test_place_and_connect();
    if (failures == 0) std::printf("tool_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
