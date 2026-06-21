#pragma once

class World;
namespace GUIStuff { class GUIManager; }

// PHASE1.md §6 (adapted) — graph view of WaypointGraph rendered as a
// collapsible side panel inside the main window, instead of the
// originally-spec'd second OS window. The doc rejected side-panel for
// multi-monitor reasons; this fork is single-user / single-monitor so
// the simplification is acceptable. If multi-monitor matters later
// the rendering code is reusable in a real second window.
//
// M6-a: skeleton — visibility toggle, empty Clay panel reservation.
// M6-b: render WaypointGraph nodes + edges (read-only) using Skia,
//       inside the panel's screen rect, with positions from
//       WaypointGraph::layout (auto-placed if no entry yet).
// M6-c: drag-reposition, drag-from-port to create edge,
//       double-click → focus canvas on the corresponding waypoint.
// View transform for the node-graph editor. WaypointGraph layout positions
// are stored in graph space; the editor maps graph->panel screen space as
//   panel = panelOrigin + offset + graphPt * scale
// so the whole graph can be zoomed (Zoom slider / mouse wheel) and panned
// (drag empty background, or middle-drag) independently of the main canvas
// underneath. Plain floats (not a Vector2f) keep this header dependency-free.
struct TreeViewGraphView {
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

class TreeView {
    public:
        explicit TreeView(World& w);

        bool is_visible() const { return visible; }
        void toggle()           { visible = !visible; }
        void set_visible(bool v){ visible = v; }

        // Lays out the panel in the current Clay layout (called from
        // Toolbar::drawing_program_gui). When hidden the panel
        // contributes zero width to the layout.
        void gui(GUIStuff::GUIManager& gui);

    private:
        World& world;
        bool visible = false;
        TreeViewGraphView view;
};
