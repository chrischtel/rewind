#include "rewind/render/network_draw.h"

namespace rw {

namespace {
StrokeStyle stroke(Color c, float width) {
    StrokeStyle s;
    s.width = width;
    s.r = c.r / 255.0f;
    s.g = c.g / 255.0f;
    s.b = c.b / 255.0f;
    s.a = c.a / 255.0f;
    s.join = LineJoin::Round;
    s.cap = LineCap::Round;
    return s;
}
} // namespace

void draw_network(Renderer2D& r, const CurveNetwork& net, const NetworkDrawStyle& style) {
    if (style.surfaces)
        for (const NetEdge& e : net.edges())
            if (e.alive) r.draw_path(e.path, stroke(style.surface, std::max(e.width, 1.0f)), style.tolerance);

    if (style.tracks) {
        for (u32 i = 0; i < net.track_count(); ++i) {
            const TrackRef t = net.track_ref(i);
            const Path& p = net.track_path(t);
            StrokeStyle s = stroke(style.track, 1);
            s.dash = 6;
            s.gap = 4;
            r.draw_path(p, s, style.tolerance);
            // A small arrow head at the end shows the direction.
            if (!p.empty()) {
                const Vec2 end = p.end();
                const Vec2 d = p.segments.back().direction(1);
                const Vec2 n = perp(d);
                r.draw_line(end.x, end.y, end.x - d.x * 5 + n.x * 3, end.y - d.y * 5 + n.y * 3, style.track);
                r.draw_line(end.x, end.y, end.x - d.x * 5 - n.x * 3, end.y - d.y * 5 - n.y * 3, style.track);
            }
        }
    }

    if (style.connectors)
        for (const Connector& c : net.connectors()) r.draw_path(c.path, stroke(style.connector, 1), style.tolerance);

    if (style.links)
        for (const TrackLink& l : net.links()) {
            const Path& a = net.track_path(l.from);
            const Path& b = net.track_path(l.to);
            if (a.empty() || b.empty()) continue;
            PathSampler sa(a, style.tolerance), sb(b, style.tolerance);
            const Vec2 pa = sa.at(sa.length() * 0.5f).position, pb = sb.at(sb.length() * 0.5f).position;
            r.draw_line(pa.x, pa.y, pb.x, pb.y, style.link);
        }

    if (style.conflicts)
        for (const Conflict& c : net.conflicts()) r.fill_circle(c.where.x, c.where.y, 2.5f, style.conflict, 8);

    if (style.nodes)
        for (const NetNode& n : net.nodes())
            if (n.alive) r.fill_circle(n.pos.x, n.pos.y, style.node_radius, style.node, 12);
}

} // namespace rw
