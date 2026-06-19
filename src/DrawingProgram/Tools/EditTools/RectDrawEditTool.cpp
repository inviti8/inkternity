#include "RectDrawEditTool.hpp"
#include "../../DrawingProgram.hpp"
#include "../../../World.hpp"
#include "../../../MainProgram.hpp"
#include "../EditTool.hpp"

#include "../../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "../../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../../GUIStuff/ElementHelpers/RadioButtonHelpers.hpp"
#include "../../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../../../GUIStuff/ElementHelpers/CheckBoxHelpers.hpp"
#include <algorithm>
#include <cmath>

namespace {
// PHASE8: keep the per-node bezier arrays sized with the vertex list (empty ->
// all corners; resize preserves existing nodes and appends corners).
void ensure_node_arrays(RectangleCanvasComponent::Data& d) {
    const size_t n = d.points.size();
    d.controlIn.resize(n, Vector2f{0.0f, 0.0f});
    d.controlOut.resize(n, Vector2f{0.0f, 0.0f});
    d.nodeType.resize(n, 0);
}
// Turn node i into a smooth curve: tangents along (next - prev), length ~1/3 of
// the shorter adjacent edge (in/out mirrored).
void make_node_curve(RectangleCanvasComponent::Data& d, size_t i) {
    ensure_node_arrays(d);
    const size_t n = d.points.size();
    if(n < 3 || i >= n) return;
    const size_t prev = (i + n - 1) % n, next = (i + 1) % n;
    auto dist = [&](size_t a, size_t b) {
        const float dx = d.points[a].x() - d.points[b].x(), dy = d.points[a].y() - d.points[b].y();
        return std::sqrt(dx * dx + dy * dy);
    };
    float dx = d.points[next].x() - d.points[prev].x();
    float dy = d.points[next].y() - d.points[prev].y();
    float dl = std::sqrt(dx * dx + dy * dy);
    if(dl < 1e-6f) { dx = 1.0f; dy = 0.0f; dl = 1.0f; }
    dx /= dl; dy /= dl;
    float len = std::min(dist(i, prev), dist(i, next)) * 0.33f;
    if(len < 1e-3f) len = 1.0f;
    d.controlOut[i] = Vector2f{dx * len, dy * len};
    d.controlIn[i]  = Vector2f{-dx * len, -dy * len};
    d.nodeType[i] = 1;   // smooth
}
void make_node_corner(RectangleCanvasComponent::Data& d, size_t i) {
    ensure_node_arrays(d);
    if(i >= d.points.size()) return;
    d.controlIn[i]  = Vector2f{0.0f, 0.0f};
    d.controlOut[i] = Vector2f{0.0f, 0.0f};
    d.nodeType[i] = 0;
}
// Insert a vertex on edge `after`->next. A straight edge gets a midpoint corner;
// a curved edge is split with de Casteljau (t=0.5) so the curve shape is preserved
// and the three involved nodes get correct tangents.
void add_polygon_point(RectangleCanvasComponent::Data& d, size_t after) {
    const size_t m = d.points.size();
    if(m < 2 || after >= m) return;
    const size_t j = (after + 1) % m;
    const Vector2f p0 = d.points[after];
    const Vector2f p3 = d.points[j];
    const bool iCurve = after < d.nodeType.size() && d.nodeType[after] != 0;
    const bool jCurve = j < d.nodeType.size() && d.nodeType[j] != 0;
    const long at = static_cast<long>(after + 1);
    auto mid2 = [](const Vector2f& a, const Vector2f& b) { return Vector2f{(a.x() + b.x()) * 0.5f, (a.y() + b.y()) * 0.5f}; };

    if(!iCurve && !jCurve) {
        // Straight edge -> midpoint corner (keep arrays parallel if present).
        const Vector2f mid = mid2(p0, p3);
        d.points.insert(d.points.begin() + at, mid);
        if(d.controlIn.size()  == m) d.controlIn.insert(d.controlIn.begin() + at, Vector2f{0.0f, 0.0f});
        if(d.controlOut.size() == m) d.controlOut.insert(d.controlOut.begin() + at, Vector2f{0.0f, 0.0f});
        if(d.nodeType.size()   == m) d.nodeType.insert(d.nodeType.begin() + at, static_cast<uint8_t>(0));
        return;
    }

    ensure_node_arrays(d);   // need controlOut[after] / controlIn[j] indexable
    const Vector2f c1 = iCurve ? Vector2f{p0.x() + d.controlOut[after].x(), p0.y() + d.controlOut[after].y()} : p0;
    const Vector2f c2 = jCurve ? Vector2f{p3.x() + d.controlIn[j].x(), p3.y() + d.controlIn[j].y()} : p3;
    const Vector2f m0 = mid2(p0, c1), m1 = mid2(c1, c2), m2 = mid2(c2, p3);
    const Vector2f q0 = mid2(m0, m1), q1 = mid2(m1, m2);
    const Vector2f split = mid2(q0, q1);
    d.controlOut[after] = Vector2f{m0.x() - p0.x(), m0.y() - p0.y()};   // shortened left tangent
    d.points.insert(d.points.begin() + at, split);
    d.controlIn.insert(d.controlIn.begin() + at, Vector2f{q0.x() - split.x(), q0.y() - split.y()});
    d.controlOut.insert(d.controlOut.begin() + at, Vector2f{q1.x() - split.x(), q1.y() - split.y()});
    d.nodeType.insert(d.nodeType.begin() + at, static_cast<uint8_t>(1));   // smooth split node
    const size_t jNew = (after + 1 < m) ? (after + 2) : 0;                 // node j after the insert
    if(jNew < d.controlIn.size())
        d.controlIn[jNew] = Vector2f{m2.x() - d.points[jNew].x(), m2.y() - d.points[jNew].y()};
}
}

RectDrawEditTool::RectDrawEditTool(DrawingProgram& initDrawP, CanvasComponentContainer::ObjInfo* initComp):
    DrawingProgramEditToolBase(initDrawP, initComp)
{}

void RectDrawEditTool::edit_gui(Toolbar& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& a = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
    auto commit_update_and_layout_func = [&] {
        comp->obj->commit_update(drawP);
        drawP.world.main.g.gui.set_to_layout();
    };
    auto commit_update_func = [&] { comp->obj->commit_update(drawP); };

    auto& gui = drawP.world.main.g.gui;
    auto mask_changed = [&] {
        comp->obj->commit_update(drawP);
        drawP.drawCache.invalidate_layer_footprint(comp->obj->parentLayer);   // mask affects whole layer
        drawP.world.main.g.gui.set_to_layout();
    };

    gui.new_id("edit tool rectangle", [&] {
        text_label_centered(gui, a.d.polygonMode ? "Edit Polygon" : "Edit Rectangle");
        checkbox_boolean_field(gui, "use as mask", "Use as mask", &a.d.isMask, mask_changed);
        if(a.d.isMask)
            checkbox_boolean_field(gui, "invert mask", "Invert mask (clip outside)", &a.d.maskInvert, mask_changed);
        if(!a.d.polygonMode)   // corner radius doesn't apply to polygons
            slider_scalar_field(gui, "relradiuswidth", "Corner Radius", &a.d.cornerRadius, 0.0f, 40.0f, { .onEdit = commit_update_func });
        radio_button_selector(gui, "Fill selector", &a.d.fillStrokeMode, {
            {"Fill only", 0},
            {"Outline only", 1},
            {"Fill and outline", 2}
        }, commit_update_and_layout_func);
        if(a.d.fillStrokeMode == 0 || a.d.fillStrokeMode == 2) {
            left_to_right_line_layout(gui, [&] {
                t.color_button_right("Fill color button", &a.d.fillColor, { .onChange = commit_update_func });
                text_label(gui, "Fill Color");
            });
        }
        if(a.d.fillStrokeMode == 1 || a.d.fillStrokeMode == 2) {
            slider_scalar_field(gui, "relstrokewidth", "Outline Size", &a.d.strokeWidth, 3.0f, 40.0f, { .onEdit = commit_update_func });
            left_to_right_line_layout(gui, [&] {
                t.color_button_right("Outline color button", &a.d.strokeColor, { .onChange = commit_update_func });
                text_label(gui, "Outline Color");
            });
        }

        // PHASE6/8: polygon node actions. Only VERTEX handles (index < points
        // count) count for these — tangent handles (indices >= count) are ignored.
        if(a.d.polygonMode && editTool) {
            const size_t n = a.d.points.size();
            std::vector<size_t> selVerts;
            for(size_t s : editTool->selectedHandles)
                if(s < n) selVerts.push_back(s);

            // Add Point — two adjacent vertices selected -> insert a vertex at the
            // edge midpoint (keeping the bezier arrays parallel).
            std::optional<size_t> insertAfter;
            if(selVerts.size() == 2 && n >= 3) {
                const size_t lo = std::min(selVerts[0], selVerts[1]);
                const size_t hi = std::max(selVerts[0], selVerts[1]);
                if(hi == lo + 1) insertAfter = lo;
                else if(lo == 0 && hi == n - 1) insertAfter = hi;
            }
            if(insertAfter.has_value()) {
                const size_t after = insertAfter.value();
                text_button(gui, "polygon add point", "Add Point", { .wide = true, .onClick = [this, after] {
                    auto& rect = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
                    add_polygon_point(rect.d, after);   // de Casteljau split on curved edges
                    comp->obj->commit_update(drawP);
                    if(editTool) editTool->refresh_point_handles();
                    drawP.world.main.g.gui.set_to_layout();
                }});
            }

            // Make Curve / Make Corner — exactly one vertex selected. A curve node
            // also offers a Smooth/Cusp toggle.
            if(selVerts.size() == 1 && n >= 3) {
                const size_t vi = selVerts[0];
                const bool isCurve = vi < a.d.nodeType.size() && a.d.nodeType[vi] != 0;
                text_button(gui, "polygon node curve toggle", isCurve ? "Make Corner" : "Make Curve", { .wide = true, .onClick = [this, vi, isCurve] {
                    auto& rect = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
                    if(isCurve) make_node_corner(rect.d, vi);
                    else        make_node_curve(rect.d, vi);
                    comp->obj->commit_update(drawP);
                    if(editTool) editTool->refresh_point_handles();   // tangent handle count changed
                    drawP.world.main.g.gui.set_to_layout();
                }});
                if(isCurve) {
                    const bool smooth = a.d.nodeType[vi] == 1;
                    text_button(gui, "polygon node smooth toggle", smooth ? "Make Cusp" : "Make Smooth", { .wide = true, .onClick = [this, vi, smooth] {
                        auto& rect = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
                        if(vi < rect.d.nodeType.size()) {
                            if(smooth) {
                                rect.d.nodeType[vi] = 2;   // cusp: tangents become independent
                            }
                            else {
                                rect.d.nodeType[vi] = 1;   // smooth: mirror in <- -out
                                if(vi < rect.d.controlOut.size() && vi < rect.d.controlIn.size())
                                    rect.d.controlIn[vi] = Vector2f{-rect.d.controlOut[vi].x(), -rect.d.controlOut[vi].y()};
                            }
                        }
                        comp->obj->commit_update(drawP);
                        if(editTool) editTool->refresh_point_handles();   // mirror links change
                        drawP.world.main.g.gui.set_to_layout();
                    }});
                }
            }

            if(selVerts.empty())
                text_label_light(gui, "Select 2 adjacent vertices to Add Point, or 1 vertex to curve it");
        }
    });
}

void RectDrawEditTool::edit_start(EditTool& editTool, std::any& prevData) {
    auto& a = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
    prevData = a.d;
    register_handles(editTool);
}

void RectDrawEditTool::register_handles(EditTool& editTool) {
    this->editTool = &editTool;
    auto& a = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
    if(a.d.polygonMode) {
        // PHASE6 M2: one free-moving handle per polygon vertex. Vertex handles are
        // registered FIRST so handle index i (for i < points.size()) maps to vertex
        // i — Add Point and Make Curve/Corner rely on that. PHASE8: bezier tangent
        // handles for curve nodes come AFTER (indices >= points.size()); each holds
        // a control OFFSET with coordMatrix = translate(node) so it renders at
        // node+offset, and armAnchor = the node for the arm line.
        for(auto& pt : a.d.points)
            editTool.add_point_handle({&pt, nullptr, nullptr});
        const size_t n = a.d.points.size();
        for(size_t i = 0; i < n; ++i) {
            if(i >= a.d.nodeType.size() || a.d.nodeType[i] == 0)
                continue;   // corner node — no tangents
            Affine2f m = Affine2f::Identity();
            m.translation() = a.d.points[i];
            // Smooth nodes (type 1) mirror their two tangents; cusp (type 2) don't.
            const bool smooth = a.d.nodeType[i] == 1;
            const bool haveIn  = i < a.d.controlIn.size();
            const bool haveOut = i < a.d.controlOut.size();
            if(haveOut)
                editTool.add_point_handle({&a.d.controlOut[i], nullptr, nullptr, MINIMUM_DISTANCE_BETWEEN_BOUNDS, MINIMUM_DISTANCE_BETWEEN_BOUNDS, m, &a.d.points[i], (smooth && haveIn) ? &a.d.controlIn[i] : nullptr});
            if(haveIn)
                editTool.add_point_handle({&a.d.controlIn[i], nullptr, nullptr, MINIMUM_DISTANCE_BETWEEN_BOUNDS, MINIMUM_DISTANCE_BETWEEN_BOUNDS, m, &a.d.points[i], (smooth && haveOut) ? &a.d.controlOut[i] : nullptr});
        }
    }
    else {
        editTool.add_point_handle({&a.d.p1, nullptr, &a.d.p2});
        editTool.add_point_handle({&a.d.p2, &a.d.p1, nullptr});
    }
}

bool RectDrawEditTool::wants_handle_refresh_on_drag() const {
    auto& a = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
    return a.d.polygonMode;   // keep tangent coordMatrices in sync after a node moves
}

void RectDrawEditTool::commit_edit_updates(std::any& prevData) {
}

bool RectDrawEditTool::edit_update() {
    return true;
}
