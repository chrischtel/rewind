#pragma once

// Debug drawing for a CurveNetwork: edge surfaces, tracks with direction,
// connectors, links, conflicts and nodes. For tools and prototypes; a game
// draws its own surfaces with draw_path() and textures.

#include "rewind/render/renderer2d.h"
#include "rewind/sim/network.h"

namespace rw {

struct NetworkDrawStyle {
    bool surfaces = true;
    bool tracks = true;
    bool connectors = true;
    bool links = false;
    bool conflicts = true;
    bool nodes = true;
    float tolerance = 0.5f;
    Color surface = rgba(60, 60, 70, 255);
    Color track = rgba(200, 200, 210, 200);
    Color connector = rgba(120, 200, 255, 160);
    Color link = rgba(255, 220, 120, 120);
    Color conflict = rgba(255, 90, 90, 220);
    Color node = rgba(255, 255, 255, 220);
    float node_radius = 4;
};

void draw_network(Renderer2D& r, const CurveNetwork& net, const NetworkDrawStyle& style = {});

} // namespace rw
