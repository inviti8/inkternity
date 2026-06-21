#include "TreeView.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../FontData.hpp"
#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/Elements/Element.hpp"
#include "../GUIStuff/Elements/LayoutElement.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Waypoint.hpp"
#include "WaypointGraph.hpp"
#include <Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp>

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRRect.h>
#include <include/core/SkTextBlob.h>
#include <include/core/SkTypeface.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace {

// Visual constants — kept here so the drawing code and the input hit-
// testing code agree.
constexpr float NODE_W = 220.0f;
constexpr float NODE_H = 40.0f;
constexpr float NODE_PAD_Y = 14.0f;
constexpr float STACK_TOP = 40.0f;
constexpr float PORT_RADIUS = 5.0f;            // visible port dot radius
constexpr float PORT_HIT_RADIUS = 9.0f;        // generous hit radius for port-drag start
constexpr float EDGE_HIT_RADIUS = 6.0f;        // distance threshold for edge hit-test
constexpr float ARROW_LEN = 8.0f;
constexpr float ARROW_HALF = 5.0f;

// Node-editor view zoom limits (graph scale, not canvas scale). 1.0 is the
// default and the *most* zoomed-in we need (right end of the slider); the
// whole range is dedicated to zooming out for navigating large trees. The
// minimum is a small positive floor rather than 0 — scale 0 would collapse
// the graph and divide-by-zero the screen->graph hit-testing.
constexpr float MIN_ZOOM = 0.1f;
constexpr float MAX_ZOOM = 1.0f;
constexpr float WHEEL_ZOOM_STEP = 1.1f;  // per wheel tick

// Squared distance from point p to segment ab.
inline float dist_sq_point_to_segment(const Vector2f& p, const Vector2f& a, const Vector2f& b) {
    const Vector2f ab = b - a;
    const float lenSq = ab.squaredNorm();
    if (lenSq < 1e-6f) return (p - a).squaredNorm();
    const float t = std::clamp((p - a).dot(ab) / lenSq, 0.0f, 1.0f);
    const Vector2f proj = a + ab * t;
    return (p - proj).squaredNorm();
}

}  // namespace

// Custom Element that draws + edits the WaypointGraph in the tree-view
// panel. clay_draw renders nodes/edges and caches their panel-local
// rects; the input callbacks consult those caches to hit-test and
// drive the interaction state machine.
class TreeViewGraphElement : public GUIStuff::Element {
    public:
        explicit TreeViewGraphElement(GUIStuff::GUIManager& g) : Element(g) {}

        void layout(const Clay_ElementId& id, World* w, TreeViewGraphView* v) {
            world = w;
            view = v;
            CLAY(id, {
                .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                .custom = {this}
            }) {}
        }

        void clay_draw(SkCanvas* canvas, GUIStuff::UpdateInputData&, Clay_RenderCommand*, bool skiaAA) override {
            if (!boundingBox.has_value() || !world || !view) return;
            const auto& bb = boundingBox.value();
            const Vector2f panelOrigin = bb.min;
            const Vector2f panelSize = bb.max - bb.min;

            // A scale change that wasn't anchored at the cursor (i.e. from the
            // Zoom slider) is re-centered about the panel centre here, so the
            // graph zooms in place instead of flying toward the origin. Wheel
            // zoom anchors at the cursor itself and syncs lastScale to skip this.
            if (view->scale != lastScale) {
                const float ratio = (lastScale > 0.0f) ? (view->scale / lastScale) : 1.0f;
                const float cx = panelSize.x() * 0.5f;
                const float cy = panelSize.y() * 0.5f;
                view->offsetX = cx - (cx - view->offsetX) * ratio;
                view->offsetY = cy - (cy - view->offsetY) * ratio;
                lastScale = view->scale;
            }

            canvas->save();
            canvas->clipRect(SkRect::MakeLTRB(bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()), skiaAA);

            SkPaint bgPaint;
            bgPaint.setColor4f({0.10f, 0.10f, 0.12f, 1.0f});
            canvas->drawRect(SkRect::MakeLTRB(bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()), bgPaint);

            auto& nodes = world->wpGraph.get_nodes();
            auto& layout = world->wpGraph.mutable_layout();
            if (!nodes) { canvas->restore(); return; }

            // Graph-space -> panel screen-space transform. Drawing everything
            // in graph space under this matrix means node sizes, fonts, stroke
            // widths and skins all scale with the zoom for free; the inverse
            // (to_graph) maps input back for hit-testing.
            canvas->save();
            canvas->translate(panelOrigin.x() + view->offsetX, panelOrigin.y() + view->offsetY);
            canvas->scale(view->scale, view->scale);

            // Auto-place + cache panel-local positions.
            const float stackX = (panelSize.x() - NODE_W) * 0.5f;
            int autoIndex = 0;
            nodeBoxes.clear();
            for (auto& info : *nodes) {
                const auto id = info.obj.get_net_id();
                auto it = layout.find(id);
                Vector2f topLeft;
                if (it == layout.end()) {
                    topLeft = Vector2f(stackX, STACK_TOP + autoIndex * (NODE_H + NODE_PAD_Y));
                    layout.emplace(id, topLeft);
                } else {
                    topLeft = it->second;
                }
                nodeBoxes.push_back({id, topLeft});
                ++autoIndex;
            }

            // Edges first (under nodes). Cache their from/to panel-local
            // positions so right-click hit-test can find them later.
            auto& edges = world->wpGraph.get_edges();
            edgeSegments.clear();
            if (edges) {
                SkPaint edgePaint;
                edgePaint.setAntiAlias(skiaAA);
                edgePaint.setStyle(SkPaint::kStroke_Style);
                edgePaint.setStrokeWidth(2.0f);
                edgePaint.setColor4f({0.88f, 0.69f, 0.25f, 0.80f});
                for (auto& einfo : *edges) {
                    const auto fromId = einfo.obj->get_from();
                    const auto toId = einfo.obj->get_to();
                    auto fromBox = find_node_box(fromId);
                    auto toBox = find_node_box(toId);
                    if (!fromBox || !toBox) continue;
                    const Vector2f fromPanel = fromBox->topLeft + Vector2f(NODE_W * 0.5f, NODE_H);
                    const Vector2f toPanel = toBox->topLeft + Vector2f(NODE_W * 0.5f, 0.0f);
                    edgeSegments.push_back({einfo.obj.get_net_id(), fromPanel, toPanel});

                    const Vector2f from = fromPanel;
                    const Vector2f to = toPanel;
                    canvas->drawLine(from.x(), from.y(), to.x(), to.y(), edgePaint);
                    const Vector2f dir = (to - from).normalized();
                    const Vector2f perp(-dir.y(), dir.x());
                    const Vector2f a1 = to - dir * ARROW_LEN + perp * ARROW_HALF;
                    const Vector2f a2 = to - dir * ARROW_LEN - perp * ARROW_HALF;
                    SkPathBuilder pb;
                    pb.moveTo(a1.x(), a1.y());
                    pb.lineTo(to.x(), to.y());
                    pb.lineTo(a2.x(), a2.y());
                    canvas->drawPath(pb.detach(), edgePaint);
                }
            }

            // Edge-create preview line — drawn while dragging from a port.
            if (dragMode == DragMode::EDGE_CREATE) {
                auto srcBox = find_node_box(dragSourceId);
                if (srcBox) {
                    const Vector2f src = srcBox->topLeft + Vector2f(NODE_W * 0.5f, NODE_H);
                    const Vector2f dst = dragCurrentGraph;
                    SkPaint preview;
                    preview.setAntiAlias(skiaAA);
                    preview.setStyle(SkPaint::kStroke_Style);
                    preview.setStrokeWidth(2.0f);
                    preview.setColor4f({0.95f, 0.85f, 0.45f, 0.85f});
                    canvas->drawLine(src.x(), src.y(), dst.x(), dst.y(), preview);
                }
            }

            // Nodes.
            SkFont font;
            font.setSize(14.0f);
            if (world->main.fonts) {
                auto it = world->main.fonts->map.find("Roboto");
                if (it != world->main.fonts->map.end()) font.setTypeface(it->second);
            }

            const auto selectedOpt = world->wpGraph.has_selection()
                ? std::optional<NetworkingObjects::NetObjID>(world->wpGraph.get_selected())
                : std::nullopt;

            for (const auto& nb : nodeBoxes) {
                const Vector2f topLeft = nb.topLeft;
                const bool selected = selectedOpt.has_value() && selectedOpt.value() == nb.id;
                auto wpRef = world->netObjMan.get_obj_temporary_ref_from_id<Waypoint>(nb.id);
                const bool hasSkin = wpRef && wpRef->has_skin();
                const bool isTransition = wpRef && wpRef->is_transition();

                if (hasSkin) {
                    // Skinned: draw only the skin, preserving aspect ratio
                    // (fit-contain inside the node footprint). No rect, no
                    // label. A selection highlight outline still draws so
                    // the user knows which is selected.
                    sk_sp<SkImage> img = wpRef->get_skin();
                    const float imgW = static_cast<float>(img->width());
                    const float imgH = static_cast<float>(img->height());
                    const float scale = std::min(NODE_W / imgW, NODE_H / imgH);
                    const float drawW = imgW * scale;
                    const float drawH = imgH * scale;
                    const float dx = topLeft.x() + (NODE_W - drawW) * 0.5f;
                    const float dy = topLeft.y() + (NODE_H - drawH) * 0.5f;
                    SkPaint imgPaint;
                    imgPaint.setAntiAlias(skiaAA);
                    canvas->drawImageRect(img.get(),
                                          SkRect::MakeXYWH(dx, dy, drawW, drawH),
                                          SkSamplingOptions{SkFilterMode::kLinear}, &imgPaint);
                    if (selected) {
                        SkPaint sel;
                        sel.setAntiAlias(skiaAA);
                        sel.setStyle(SkPaint::kStroke_Style);
                        sel.setStrokeWidth(2.0f);
                        sel.setColor4f({0.92f, 0.40f, 0.62f, 1.0f});  // accent pink (matches canvas)
                        canvas->drawRect(SkRect::MakeXYWH(dx, dy, drawW, drawH), sel);
                    }
                } else {
                    // Plain: rounded rect + label.
                    SkRect r = SkRect::MakeXYWH(topLeft.x(), topLeft.y(), NODE_W, NODE_H);
                    SkRRect rr = SkRRect::MakeRectXY(r, 6.0f, 6.0f);
                    SkPaint fill;
                    fill.setAntiAlias(skiaAA);
                    fill.setColor4f(selected
                        ? SkColor4f{0.30f, 0.26f, 0.18f, 1.0f}
                        : SkColor4f{0.20f, 0.20f, 0.24f, 1.0f});
                    canvas->drawRRect(rr, fill);
                    SkPaint outline;
                    outline.setAntiAlias(skiaAA);
                    outline.setStyle(SkPaint::kStroke_Style);
                    outline.setStrokeWidth(selected ? 2.0f : 1.5f);
                    outline.setColor4f({0.88f, 0.69f, 0.25f, 1.0f});
                    canvas->drawRRect(rr, outline);

                    std::string display = "(unnamed)";
                    if (wpRef) {
                        const std::string& label = wpRef->get_label();
                        if (!label.empty()) display = label;
                    }
                    SkPaint textPaint;
                    textPaint.setAntiAlias(skiaAA);
                    textPaint.setColor4f({0.95f, 0.95f, 0.95f, 1.0f});
                    canvas->drawSimpleText(display.data(), display.size(), SkTextEncoding::kUTF8,
                                           topLeft.x() + 12.0f, topLeft.y() + NODE_H * 0.5f + 5.0f,
                                           font, textPaint);
                }

                // T4: transition-point badge — small filled diamond in
                // the top-right corner. Same shape as the canvas marker
                // variant so the two views read as the same kind of
                // node at a glance. Drawn on top of both skinned and
                // plain backgrounds so it's visible either way.
                if (isTransition) {
                    constexpr float BADGE_RADIUS = 5.0f;
                    constexpr float BADGE_PAD    = 6.0f;
                    const float bcx = topLeft.x() + NODE_W - BADGE_PAD - BADGE_RADIUS;
                    const float bcy = topLeft.y() + BADGE_PAD + BADGE_RADIUS;
                    SkPathBuilder badgePB;
                    badgePB.moveTo(bcx,                bcy - BADGE_RADIUS);
                    badgePB.lineTo(bcx + BADGE_RADIUS, bcy);
                    badgePB.lineTo(bcx,                bcy + BADGE_RADIUS);
                    badgePB.lineTo(bcx - BADGE_RADIUS, bcy);
                    badgePB.close();
                    SkPath badge = badgePB.detach();
                    SkPaint badgeFill;
                    badgeFill.setAntiAlias(skiaAA);
                    badgeFill.setColor4f({0.88f, 0.69f, 0.25f, 1.0f});  // gold
                    canvas->drawPath(badge, badgeFill);
                    SkPaint badgeOutline;
                    badgeOutline.setAntiAlias(skiaAA);
                    badgeOutline.setStyle(SkPaint::kStroke_Style);
                    badgeOutline.setStrokeWidth(0.0f);
                    badgeOutline.setColor4f({0.15f, 0.10f, 0.05f, 1.0f});
                    canvas->drawPath(badge, badgeOutline);
                }

                // Edge port (small dot at bottom-center). Drag from this
                // dot to another node's body to create an edge.
                const Vector2f port = topLeft + Vector2f(NODE_W * 0.5f, NODE_H);
                SkPaint portPaint;
                portPaint.setAntiAlias(skiaAA);
                portPaint.setColor4f({0.88f, 0.69f, 0.25f, 1.0f});
                canvas->drawCircle(port.x(), port.y(), PORT_RADIUS, portPaint);
            }

            canvas->restore();  // graph-space transform
            canvas->restore();  // panel clip
        }

        void input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button) override {
            if (!boundingBox.has_value() || !world || !view) return;
            const Vector2f panelLocal = button.pos - boundingBox.value().min;  // screen, for panning
            const Vector2f graphLocal = to_graph(button.pos);                  // for hit-testing

            // Middle-button drag pans the view (matches the canvas pan idiom).
            if (button.button == InputManager::MouseButton::MIDDLE) {
                if (button.down && mouseHovering) {
                    dragMode = DragMode::PAN;
                    panStartCursor = panelLocal;
                    panStartOffset = Vector2f(view->offsetX, view->offsetY);
                } else if (!button.down && dragMode == DragMode::PAN) {
                    dragMode = DragMode::NONE;
                }
                return;
            }

            if (button.button == InputManager::MouseButton::LEFT) {
                if (button.down) {
                    if (!mouseHovering) return;
                    // Port hit test first — has priority over node-body
                    // because port overlaps the node's bottom edge.
                    if (auto portHit = hit_test_port(graphLocal)) {
                        dragMode = DragMode::EDGE_CREATE;
                        dragSourceId = portHit.value();
                        dragCurrentGraph = graphLocal;
                        gui.invalidate_draw_element(this);
                        return;
                    }
                    if (auto nodeHit = hit_test_node(graphLocal)) {
                        world->wpGraph.select(nodeHit.value().id);
                        // Selection change ripples to the WaypointTool
                        // settings panel in the right-side toolbar, which
                        // lives OUTSIDE this element's bb. Without a wider
                        // invalidation Clay keeps the panel's cached pixels
                        // until the next hover/cursor event in that area,
                        // so the user sees stale label / transition fields
                        // until they wave the mouse over the toolbar.
                        // Invalidate the full window — the cost is one
                        // extra repaint per node-click, which happens at
                        // human speed.
                        gui.invalidate_draw_in_area(SCollision::AABB<float>{
                            Vector2f{0.0f, 0.0f},
                            world->main.window.size.cast<float>()
                        });
                        // Double-click → focus canvas on the waypoint's framing.
                        if (button.clicks >= 2) {
                            auto wpRef = world->netObjMan.get_obj_temporary_ref_from_id<Waypoint>(nodeHit.value().id);
                            if (wpRef) {
                                world->drawData.cam.smooth_move_to(*world, wpRef->get_coords(), wpRef->get_window_size().cast<float>());
                            }
                            return;
                        }
                        // Otherwise begin REPOSITION drag.
                        dragMode = DragMode::REPOSITION;
                        dragSourceId = nodeHit.value().id;
                        dragOffset = graphLocal - nodeHit.value().topLeft;
                        return;
                    }
                    // Empty space → pan the view (so complex trees can be
                    // navigated by dragging the background, like a canvas).
                    dragMode = DragMode::PAN;
                    panStartCursor = panelLocal;
                    panStartOffset = Vector2f(view->offsetX, view->offsetY);
                } else {
                    // Mouse-up — finalize whatever was in progress.
                    if (dragMode == DragMode::EDGE_CREATE) {
                        if (auto nodeHit = hit_test_node(graphLocal)) {
                            if (nodeHit.value().id != dragSourceId) {
                                world->wpGraph.add_edge_enforcing_invariant(
                                    dragSourceId, nodeHit.value().id, std::optional<std::string>{});
                            }
                        }
                    }
                    dragMode = DragMode::NONE;
                    gui.invalidate_draw_element(this);
                }
                return;
            }

            if (button.button == InputManager::MouseButton::RIGHT && button.down && mouseHovering) {
                // Right-click on an edge deletes it. Label-edit UI for
                // edges is a follow-up.
                if (auto edgeHitId = hit_test_edge(graphLocal)) {
                    auto& edges = world->wpGraph.get_edges();
                    auto it = edges->get(edgeHitId.value());
                    if (it != edges->end())
                        edges->erase(edges, it);
                    gui.invalidate_draw_element(this);
                }
            }
        }

        void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) override {
            if (!boundingBox.has_value() || !world || !view) return;
            const Vector2f panelLocal = motion.pos - boundingBox.value().min;  // screen
            const Vector2f graphLocal = to_graph(motion.pos);
            if (dragMode == DragMode::REPOSITION) {
                world->wpGraph.mutable_layout()[dragSourceId] = graphLocal - dragOffset;
                gui.invalidate_draw_element(this);
            } else if (dragMode == DragMode::EDGE_CREATE) {
                dragCurrentGraph = graphLocal;
                gui.invalidate_draw_element(this);
            } else if (dragMode == DragMode::PAN) {
                const Vector2f delta = panelLocal - panStartCursor;
                view->offsetX = panStartOffset.x() + delta.x();
                view->offsetY = panStartOffset.y() + delta.y();
                gui.invalidate_draw_element(this);
            }
        }

        void input_mouse_wheel_callback(const InputManager::MouseWheelCallbackArgs& wheel) override {
            if (!boundingBox.has_value() || !world || !view) return;
            if (!mouseHovering || wheel.tickAmount.y() == 0.0f) return;
            const float oldScale = view->scale;
            const float newScale = std::clamp(
                oldScale * static_cast<float>(std::pow(WHEEL_ZOOM_STEP, wheel.tickAmount.y())),
                MIN_ZOOM, MAX_ZOOM);
            if (newScale == oldScale) return;
            // Anchor the zoom at the cursor: keep the graph point under the
            // pointer fixed on screen. Sync lastScale so clay_draw doesn't also
            // re-centre this change about the panel middle.
            const Vector2f panelLocal = wheel.mousePos - boundingBox.value().min;
            const float gx = (panelLocal.x() - view->offsetX) / oldScale;
            const float gy = (panelLocal.y() - view->offsetY) / oldScale;
            view->offsetX = panelLocal.x() - gx * newScale;
            view->offsetY = panelLocal.y() - gy * newScale;
            view->scale = newScale;
            lastScale = newScale;
            gui.invalidate_draw_element(this);
        }

    private:
        struct NodeBox {
            NetworkingObjects::NetObjID id;
            Vector2f topLeft;
        };
        struct EdgeSegment {
            NetworkingObjects::NetObjID id;
            Vector2f from;  // panel-local
            Vector2f to;    // panel-local
        };

        // Map a screen position to graph space (inverse of the clay_draw
        // matrix). Caller guarantees boundingBox + view are set.
        Vector2f to_graph(const Vector2f& screenPos) const {
            const Vector2f panelLocal = screenPos - boundingBox.value().min;
            return Vector2f((panelLocal.x() - view->offsetX) / view->scale,
                            (panelLocal.y() - view->offsetY) / view->scale);
        }

        const NodeBox* find_node_box(NetworkingObjects::NetObjID id) const {
            for (const auto& nb : nodeBoxes) if (nb.id == id) return &nb;
            return nullptr;
        }

        std::optional<NetworkingObjects::NetObjID> hit_test_port(const Vector2f& panelLocal) const {
            const float r2 = PORT_HIT_RADIUS * PORT_HIT_RADIUS;
            for (const auto& nb : nodeBoxes) {
                const Vector2f port = nb.topLeft + Vector2f(NODE_W * 0.5f, NODE_H);
                if ((panelLocal - port).squaredNorm() <= r2) return nb.id;
            }
            return std::nullopt;
        }

        std::optional<NodeBox> hit_test_node(const Vector2f& panelLocal) const {
            for (const auto& nb : nodeBoxes) {
                if (panelLocal.x() >= nb.topLeft.x() && panelLocal.x() <= nb.topLeft.x() + NODE_W &&
                    panelLocal.y() >= nb.topLeft.y() && panelLocal.y() <= nb.topLeft.y() + NODE_H) {
                    return nb;
                }
            }
            return std::nullopt;
        }

        std::optional<NetworkingObjects::NetObjID> hit_test_edge(const Vector2f& panelLocal) const {
            const float r2 = EDGE_HIT_RADIUS * EDGE_HIT_RADIUS;
            for (const auto& es : edgeSegments) {
                if (dist_sq_point_to_segment(panelLocal, es.from, es.to) <= r2) return es.id;
            }
            return std::nullopt;
        }

        World* world = nullptr;
        TreeViewGraphView* view = nullptr;    // pan/zoom state owned by TreeView
        float lastScale = 1.0f;               // detects slider zoom for re-centring
        std::vector<NodeBox> nodeBoxes;       // cached during clay_draw (graph space)
        std::vector<EdgeSegment> edgeSegments; // cached during clay_draw (graph space)

        enum class DragMode { NONE, REPOSITION, EDGE_CREATE, PAN };
        DragMode dragMode = DragMode::NONE;
        NetworkingObjects::NetObjID dragSourceId{};
        Vector2f dragOffset{0, 0};        // REPOSITION: cursor offset from node top-left (graph space)
        Vector2f dragCurrentGraph{0, 0};  // EDGE_CREATE: current cursor in graph space
        Vector2f panStartCursor{0, 0};    // PAN: cursor (panel/screen) at drag start
        Vector2f panStartOffset{0, 0};    // PAN: view offset at drag start
};

TreeView::TreeView(World& w)
    : world(w) {}

void TreeView::gui(GUIStuff::GUIManager& gui) {
    if (!visible) return;
    // Reader mode auto-hides the editor chrome (PHASE1.md §7).
    if (world.readerMode.is_active()) return;
    using namespace GUIStuff;
    using namespace GUIStuff::ElementHelpers;
    auto& io = gui.io;

    constexpr float PANEL_WIDTH = 360.0f;

    gui.element<LayoutElement>("tree view panel", [&] (LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_FIXED(PANEL_WIDTH), .height = CLAY_SIZING_GROW(0)},
                .padding = CLAY_PADDING_ALL(static_cast<uint16_t>(io.theme->padding1)),
                .childGap = static_cast<uint16_t>(io.theme->childGap1),
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_TOP},
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = convert_vec4<Clay_Color>(io.theme->backColor1),
            .cornerRadius = CLAY_CORNER_RADIUS(io.theme->windowCorners1)
        }) {
            text_label_centered(gui, "Tree");
            // Zoom control for the node view — navigation is independent of the
            // main canvas below (pinch over this panel no longer drives the
            // canvas; see DrawCamera::input_multi_finger_touch_callback).
            slider_scalar_field<float>(gui, "node zoom", "Zoom", &view.scale, MIN_ZOOM, MAX_ZOOM, {
                .decimalPrecision = 2,
                .onEdit = [&] {
                    // Slider zoom isn't cursor-anchored; the graph element
                    // re-centres about the panel middle on its next clay_draw.
                    // Force that repaint (the element's bb is outside this row).
                    gui.invalidate_draw_in_area(SCollision::AABB<float>{
                        Vector2f{0.0f, 0.0f},
                        world.main.window.size.cast<float>()
                    });
                }
            });
            gui.element<TreeViewGraphElement>("graph", &world, &view);
        }
    });
}
