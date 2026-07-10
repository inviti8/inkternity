#include "MotionPathTool.hpp"

#include "../DrawingProgram.hpp"
#include "../../World.hpp"
#include "../../MainProgram.hpp"
#include "../Layers/DrawingProgramLayerManager.hpp"
#include "../Layers/DrawingProgramLayerListItem.hpp"
#include "../Layers/MotionPath.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/LayoutHelpers.hpp"
#include "../../GUIStuff/Elements/DropDown.hpp"
#include "../../Waypoints/Waypoint.hpp"
#include "../../WorldUndoManager.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <algorithm>
#include <string>
#include <cstdio>

namespace {
inline bool is_zero(const Vector2f& v) { return v.x() == 0.0f && v.y() == 0.0f; }

// Green(first) / red(last) / yellow(middle) diamond node handle, white outline.
void draw_diamond(SkCanvas* canvas, const Vector2f& c, float r, const SkColor4f& fill) {
    SkPathBuilder pb;
    pb.moveTo(c.x(), c.y() - r);
    pb.lineTo(c.x() + r, c.y());
    pb.lineTo(c.x(), c.y() + r);
    pb.lineTo(c.x() - r, c.y());
    pb.close();
    const SkPath p = pb.detach();
    SkPaint f;
    f.setAntiAlias(true);
    f.setColor4f(fill);
    canvas->drawPath(p, f);
    SkPaint o;
    o.setAntiAlias(true);
    o.setStyle(SkPaint::kStroke_Style);
    o.setStrokeWidth(1.5f);
    o.setColor4f(SkColor4f{1.0f, 1.0f, 1.0f, 1.0f});
    canvas->drawPath(p, o);
}

// --- Open-path node math (adapted from the shape polygon ops, which wrap closed;
// a motion path is an OPEN curve so no modulo, and endpoints have one neighbor). ---
void mp_ensure_arrays(MotionPath& mp) {
    const size_t n = mp.points.size();
    mp.controlIn.resize(n, Vector2f{0.0f, 0.0f});
    mp.controlOut.resize(n, Vector2f{0.0f, 0.0f});
    mp.nodeType.resize(n, static_cast<uint8_t>(0));
    mp.nodeSeconds.resize(n, 1.0f);   // seconds for the segment ending at this node
    mp.nodeScale.resize(n, 1.0f);
    mp.nodeEasing.resize(n, static_cast<uint8_t>(1));   // TransitionEasing::EASE
    mp.nodeRotation.resize(n, 0.0f);
    if(!mp.nodeSeconds.empty()) mp.nodeSeconds[0] = 0.0f;   // start has no incoming segment
}
void mp_make_curve(MotionPath& mp, size_t i) {
    mp_ensure_arrays(mp);
    const size_t n = mp.points.size();
    if(n < 2 || i >= n) return;
    Vector2f dir = (i == 0)       ? (mp.points[1] - mp.points[0])
                 : (i == n - 1)   ? (mp.points[n - 1] - mp.points[n - 2])
                                  : (mp.points[i + 1] - mp.points[i - 1]);
    float dl = dir.norm();
    if(dl < 1e-6f) { dir = Vector2f{1.0f, 0.0f}; dl = 1.0f; }
    dir /= dl;
    float len = 1e9f;
    if(i > 0)     len = std::min(len, (mp.points[i] - mp.points[i - 1]).norm());
    if(i < n - 1) len = std::min(len, (mp.points[i + 1] - mp.points[i]).norm());
    if(len > 1e8f) len = 50.0f;
    len *= 0.33f;
    if(len < 1e-3f) len = 1.0f;
    mp.controlOut[i] = dir * len;
    mp.controlIn[i]  = dir * -len;
    mp.nodeType[i] = 1;
}
void mp_make_corner(MotionPath& mp, size_t i) {
    mp_ensure_arrays(mp);
    if(i >= mp.points.size()) return;
    mp.controlIn[i]  = Vector2f{0.0f, 0.0f};
    mp.controlOut[i] = Vector2f{0.0f, 0.0f};
    mp.nodeType[i] = 0;
}
// Insert a node on the open edge after->after+1 (de Casteljau split on curves).
void mp_insert_after(MotionPath& mp, size_t after) {
    mp_ensure_arrays(mp);
    const size_t n = mp.points.size();
    if(after + 1 >= n) return;
    const size_t j = after + 1;
    const Vector2f p0 = mp.points[after], p3 = mp.points[j];
    const bool iCurve = mp.nodeType[after] != 0;
    const bool jCurve = mp.nodeType[j] != 0;
    const long at = static_cast<long>(after) + 1;
    auto mid = [](const Vector2f& a, const Vector2f& b) { return Vector2f{(a.x() + b.x()) * 0.5f, (a.y() + b.y()) * 0.5f}; };
    // Split the segment's seconds in half so inserting a control point doesn't
    // change the overall timing (after->new and new->oldj each get half).
    const float segHalf = ((j < mp.nodeSeconds.size()) ? mp.nodeSeconds[j] : 1.0f) * 0.5f;
    if(j < mp.nodeSeconds.size()) mp.nodeSeconds[j] = segHalf;   // becomes new->oldj after the shift
    const float newScale = (mp.nodeScale[after] + mp.nodeScale[j]) * 0.5f;
    const float newRot   = (mp.nodeRotation[after] + mp.nodeRotation[j]) * 0.5f;
    const uint8_t newEasing = mp.nodeEasing[after];
    auto insert_channels = [&](uint8_t type) {
        mp.nodeType.insert(mp.nodeType.begin() + at, type);
        mp.nodeSeconds.insert(mp.nodeSeconds.begin() + at, segHalf);   // after->new
        mp.nodeScale.insert(mp.nodeScale.begin() + at, newScale);
        mp.nodeEasing.insert(mp.nodeEasing.begin() + at, newEasing);
        mp.nodeRotation.insert(mp.nodeRotation.begin() + at, newRot);
    };
    if(!iCurve && !jCurve) {
        mp.points.insert(mp.points.begin() + at, mid(p0, p3));
        mp.controlIn.insert(mp.controlIn.begin() + at, Vector2f{0.0f, 0.0f});
        mp.controlOut.insert(mp.controlOut.begin() + at, Vector2f{0.0f, 0.0f});
        insert_channels(0);
        return;
    }
    const Vector2f c1 = iCurve ? Vector2f{p0 + mp.controlOut[after]} : p0;
    const Vector2f c2 = jCurve ? Vector2f{p3 + mp.controlIn[j]} : p3;
    const Vector2f m0 = mid(p0, c1), m1 = mid(c1, c2), m2 = mid(c2, p3);
    const Vector2f q0 = mid(m0, m1), q1 = mid(m1, m2);
    const Vector2f split = mid(q0, q1);
    mp.controlOut[after] = m0 - p0;
    mp.points.insert(mp.points.begin() + at, split);
    mp.controlIn.insert(mp.controlIn.begin() + at, q0 - split);
    mp.controlOut.insert(mp.controlOut.begin() + at, q1 - split);
    insert_channels(1);
    const size_t jNew = after + 2;
    if(jNew < mp.controlIn.size())
        mp.controlIn[jNew] = m2 - mp.points[jNew];
}
void mp_delete_node(MotionPath& mp, size_t i) {
    if(i >= mp.points.size()) return;
    // Preserve total time: fold this node's incoming segment into the next one.
    if(i < mp.nodeSeconds.size() && i + 1 < mp.nodeSeconds.size())
        mp.nodeSeconds[i + 1] += mp.nodeSeconds[i];
    auto era = [&](auto& v) { if(i < v.size()) v.erase(v.begin() + static_cast<long>(i)); };
    era(mp.points); era(mp.controlIn); era(mp.controlOut);
    era(mp.nodeType); era(mp.nodeSeconds); era(mp.nodeScale); era(mp.nodeEasing); era(mp.nodeRotation);
    if(!mp.nodeSeconds.empty()) mp.nodeSeconds[0] = 0.0f;   // the new start has no incoming segment
}

const std::vector<std::string>& play_style_names() {
    static const std::vector<std::string> v = {"Play Once", "Loop", "Ping-Pong"};
    return v;
}

// Persisted-field equality (skips coords — unchanged by node editing — and the
// transient runtime), so end_edit only pushes an undo when something changed.
bool mp_data_equal(const MotionPath& a, const MotionPath& b) {
    return a.points == b.points && a.controlIn == b.controlIn && a.controlOut == b.controlOut
        && a.nodeType == b.nodeType && a.nodeSeconds == b.nodeSeconds && a.nodeScale == b.nodeScale
        && a.nodeEasing == b.nodeEasing && a.nodeRotation == b.nodeRotation && a.playStyle == b.playStyle;
}

// Swap-based undo: stores one MotionPath snapshot and swaps it with the live path
// on undo/redo (mirrors the layer editor's EditLayerWorldUndoAction).
class MotionPathUndoAction : public WorldUndoAction {
    public:
        MotionPathUndoAction(MotionPath snap, WorldUndoManager::UndoObjectID initUndoID):
            data(std::move(snap)), undoID(initUndoID) {}
        std::string get_name() const override { return "Edit Motion Path"; }
        bool undo(WorldUndoManager& u) override { return swap_state(u); }
        bool redo(WorldUndoManager& u) override { return swap_state(u); }
        bool swap_state(WorldUndoManager& u) {
            auto netid = u.get_netid_from_undoid(undoID);
            if(!netid.has_value()) return false;
            auto ref = u.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(netid.value());
            if(!ref) return false;
            MotionPath* mp = ref->get_motion_path();
            if(!mp) return false;
            std::swap(*mp, data);   // live <-> stored (redo swaps back)
            ref->commit_motion_path(u.world.drawProg.layerMan);
            u.world.main.g.gui.set_to_layout();
            return true;
        }
        MotionPath data;
        WorldUndoManager::UndoObjectID undoID;
};
}

MotionPathTool::MotionPathTool(DrawingProgram& initDrawP, NetworkingObjects::NetObjID initFolderId):
    DrawingProgramToolBase(initDrawP),
    folderId(initFolderId)
{}

DrawingProgramToolType MotionPathTool::get_type() {
    return DrawingProgramToolType::MOTIONPATH;
}

DrawingProgramLayerListItem* MotionPathTool::folder() const {
    if(folderId == NetworkingObjects::NetObjID{0, 0}) return nullptr;
    auto ref = drawP.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(folderId);
    return ref ? ref.get() : nullptr;
}

MotionPath* MotionPathTool::path() const {
    DrawingProgramLayerListItem* f = folder();
    return f ? f->get_motion_path() : nullptr;
}

void MotionPathTool::commit() {
    DrawingProgramLayerListItem* f = folder();
    if(f) f->commit_motion_path(drawP.layerMan);
}

void MotionPathTool::switch_tool(DrawingProgramToolType) { end_edit(); }
void MotionPathTool::erase_component(CanvasComponentContainer::ObjInfo*) {}

void MotionPathTool::begin_edit() {
    if(undoBaseline.has_value()) return;
    if(MotionPath* mp = path())
        undoBaseline = *mp;
}

void MotionPathTool::end_edit() {
    if(!undoBaseline.has_value()) return;
    MotionPath* mp = path();
    if(mp && !mp_data_equal(*mp, undoBaseline.value())) {
        const WorldUndoManager::UndoObjectID undoID = drawP.world.undo.get_undoid_from_netid(folderId);
        drawP.world.undo.push(std::make_unique<MotionPathUndoAction>(std::move(undoBaseline.value()), undoID));
    }
    undoBaseline.reset();
}
void MotionPathTool::right_click_popup_gui(Toolbar&, Vector2f) {}
void MotionPathTool::tool_update() {}
bool MotionPathTool::prevent_undo_or_redo() { return false; }

void MotionPathTool::gui_toolbox(Toolbar&) { build_toolbox(); }
void MotionPathTool::gui_phone_toolbox(PhoneDrawingProgramScreen&) { build_toolbox(); }

void MotionPathTool::build_toolbox() {
    using namespace GUIStuff;
    using namespace GUIStuff::ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    MotionPath* mp = path();

    gui.new_id("motion path tool", [&] {
        text_label_centered(gui, "Motion Path");
        if(!mp) { text_button(gui, "mp done", "Done", { .wide = true, .onClick = [this] { drawP.switch_to_tool(DrawingProgramToolType::EDIT); }}); return; }
        const size_t n = mp->points.size();

        // Per-node inspector (one node selected).
        if(selectedNode.has_value() && selectedNode.value() < n) {
            const size_t i = selectedNode.value();
            text_label(gui, i == 0 ? "Start node (green)" : (i == n - 1 ? "End node (red)" : ("Node " + std::to_string(i))));
            // Per-segment duration: seconds to travel here from the previous node.
            // The start node has no incoming segment, so it has no Seconds field.
            if(i >= 1 && i < mp->nodeSeconds.size())
                slider_scalar_field(gui, "mp node seconds", "Seconds (from prev)", &mp->nodeSeconds[i], 0.1f, 10.0f, { .decimalPrecision = 1, .onEdit = [this] { begin_edit(); commit(); } });
            slider_scalar_field(gui, "mp node scale", "Scale", &mp->nodeScale[i], 0.1f, 10.0f, { .decimalPrecision = 2, .onEdit = [this] { begin_edit(); commit(); } });
            if(i < mp->nodeRotation.size())
                slider_scalar_field(gui, "mp node rotation", "Rotate along path (-1..1)", &mp->nodeRotation[i], -1.0f, 1.0f, { .decimalPrecision = 2, .onEdit = [this] { begin_edit(); commit(); } });
            left_to_right_line_layout(gui, [&] {
                text_label(gui, "Easing");
                gui.element<DropDown<uint8_t>>("mp node easing", &mp->nodeEasing[i], transition_easing_display_names(), DropdownOptions{ .onClick = [this] { begin_edit(); commit(); } });
            });
            const bool isCurve = i < mp->nodeType.size() && mp->nodeType[i] != 0;
            text_button(gui, "mp curve toggle", isCurve ? "Make Corner" : "Make Curve", { .wide = true, .onClick = [this, i, isCurve] {
                begin_edit(); MotionPath* p = path(); if(!p) return;
                if(isCurve) mp_make_corner(*p, i); else mp_make_curve(*p, i);
                commit(); end_edit(); drawP.world.main.g.gui.set_to_layout();
            }});
            if(isCurve) {
                const bool smooth = mp->nodeType[i] == 1;
                text_button(gui, "mp smooth toggle", smooth ? "Make Cusp" : "Make Smooth", { .wide = true, .onClick = [this, i, smooth] {
                    begin_edit(); MotionPath* p = path(); if(!p || i >= p->nodeType.size()) return;
                    p->nodeType[i] = smooth ? 2 : 1;
                    if(!smooth && i < p->controlOut.size() && i < p->controlIn.size())
                        p->controlIn[i] = Vector2f{-p->controlOut[i].x(), -p->controlOut[i].y()};
                    commit(); end_edit(); drawP.world.main.g.gui.set_to_layout();
                }});
            }
            // Add a node adjacent to the selection (open path: after it, or before
            // if it's the last node).
            const size_t addAfter = (i == n - 1) ? (n - 2) : i;
            text_button(gui, "mp add node", "Add Node", { .wide = true, .onClick = [this, addAfter] {
                begin_edit(); MotionPath* p = path(); if(!p) return;
                mp_insert_after(*p, addAfter);
                selectedNode = addAfter + 1;
                commit(); end_edit(); drawP.world.main.g.gui.set_to_layout();
            }});
            if(n > 2)
                text_button(gui, "mp delete node", "Delete Node", { .wide = true, .onClick = [this, i] {
                    begin_edit(); MotionPath* p = path(); if(!p) return;
                    mp_delete_node(*p, i);
                    selectedNode.reset();
                    commit(); end_edit(); drawP.world.main.g.gui.set_to_layout();
                }});
        }
        else
            text_label_light(gui, "Click a diamond to edit a node. Green = start, red = end.");

        // Path-level playback (shown when the start node is selected or nothing is).
        if(!selectedNode.has_value() || selectedNode.value() == 0) {
            left_to_right_line_layout(gui, [&] {
                text_label(gui, "Play Style");
                gui.element<DropDown<uint8_t>>("mp play style", reinterpret_cast<uint8_t*>(&mp->playStyle), play_style_names(), DropdownOptions{ .onClick = [this] { begin_edit(); commit(); } });
            });
            char totalBuf[48];
            std::snprintf(totalBuf, sizeof(totalBuf), "Total: %.1f s", mp->total_seconds());
            text_label(gui, totalBuf);
        }

        text_button(gui, "mp done", "Done", { .wide = true, .onClick = [this] { drawP.switch_to_tool(DrawingProgramToolType::EDIT); }});
    });
}

void MotionPathTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    MotionPath* mp = path();
    if(!mp || mp->points.empty()) return;
    const size_t n = mp->points.size();
    auto scr = [&](const Vector2f& local) {
        return drawData.cam.c.to_space(mp->coords.from_space(local));
    };

    // The bezier curve (open; screen space — the cam transform is affine, so
    // transforming control points then cubicTo == transforming the curve).
    if(n >= 2) {
        SkPathBuilder pb;
        const Vector2f p0 = scr(mp->points[0]);
        pb.moveTo(p0.x(), p0.y());
        for(size_t i = 0; i + 1 < n; ++i) {
            const Vector2f cOut = (i < mp->controlOut.size()) ? mp->controlOut[i] : Vector2f{0.0f, 0.0f};
            const Vector2f cIn  = ((i + 1) < mp->controlIn.size()) ? mp->controlIn[i + 1] : Vector2f{0.0f, 0.0f};
            const Vector2f b = scr(mp->points[i + 1]);
            if(!is_zero(cOut) || !is_zero(cIn)) {
                const Vector2f c1 = scr(mp->points[i] + cOut);
                const Vector2f c2 = scr(mp->points[i + 1] + cIn);
                pb.cubicTo(c1.x(), c1.y(), c2.x(), c2.y(), b.x(), b.y());
            }
            else
                pb.lineTo(b.x(), b.y());
        }
        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth(2.0f);
        stroke.setColor4f(SkColor4f{0.9f, 0.9f, 0.95f, 0.9f});
        canvas->drawPath(pb.detach(), stroke);
    }

    // Tangent arms + green dots for curve nodes.
    for(size_t i = 0; i < n; ++i) {
        if(i >= mp->nodeType.size() || mp->nodeType[i] == 0) continue;
        const Vector2f node = scr(mp->points[i]);
        auto arm = [&](const Vector2f& off) {
            const Vector2f t = scr(mp->points[i] + off);
            SkPaint a;
            a.setAntiAlias(true);
            a.setStrokeWidth(1.0f);
            a.setColor4f(SkColor4f{0.2f, 0.8f, 0.4f, 0.8f});
            canvas->drawLine(node.x(), node.y(), t.x(), t.y(), a);
            drawP.draw_drag_circle(canvas, t, SkColor4f{0.2f, 0.8f, 0.4f, 1.0f}, drawData);
        };
        if(i < mp->controlOut.size() && !is_zero(mp->controlOut[i])) arm(mp->controlOut[i]);
        if(i < mp->controlIn.size()  && !is_zero(mp->controlIn[i])) arm(mp->controlIn[i]);
    }

    // Node diamonds: green first, red last, yellow middle (larger when selected).
    const float r = drawP.drag_point_radius();
    for(size_t i = 0; i < n; ++i) {
        const SkColor4f col = (i == 0)     ? SkColor4f{0.2f, 0.85f, 0.3f, 1.0f}
                            : (i == n - 1) ? SkColor4f{0.9f, 0.25f, 0.25f, 1.0f}
                                           : SkColor4f{0.95f, 0.85f, 0.1f, 1.0f};
        const bool sel = selectedNode.has_value() && selectedNode.value() == i;
        draw_diamond(canvas, scr(mp->points[i]), sel ? r * 1.4f : r, col);
    }
}

void MotionPathTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    MotionPath* mp = path();
    if(!mp) return;
    if(button.down) {
        end_edit();    // flush any pending inspector-edit session
        begin_edit();  // snapshot for the interaction about to start (no-op if unchanged on release)
        const float r = drawP.drag_point_radius();
        const size_t n = mp->points.size();
        const auto& cam = drawP.world.drawData.cam.c;
        auto near_screen = [&](const Vector2f& local) {
            return (cam.to_space(mp->coords.from_space(local)) - button.pos).norm() <= r;
        };
        // Tangent handles first (they sit on top of the node they belong to).
        for(size_t i = 0; i < n; ++i) {
            if(i >= mp->nodeType.size() || mp->nodeType[i] == 0) continue;
            if(i < mp->controlOut.size() && !is_zero(mp->controlOut[i]) && near_screen(mp->points[i] + mp->controlOut[i])) {
                dragging = true; dragNode = i; dragTangent = true; dragTangentIn = false;
                dragMoved = false; dragDownScreen = button.pos; selectedNode = i; return;
            }
            if(i < mp->controlIn.size() && !is_zero(mp->controlIn[i]) && near_screen(mp->points[i] + mp->controlIn[i])) {
                dragging = true; dragNode = i; dragTangent = true; dragTangentIn = true;
                dragMoved = false; dragDownScreen = button.pos; selectedNode = i; return;
            }
        }
        // Nodes.
        for(size_t i = 0; i < n; ++i) {
            if(near_screen(mp->points[i])) {
                dragging = true; dragNode = i; dragTangent = false;
                dragMoved = false; dragDownScreen = button.pos; selectedNode = i;
                drawP.world.main.g.gui.set_to_layout();
                return;
            }
        }
        // Missed everything → deselect.
        if(selectedNode.has_value()) {
            selectedNode.reset();
            drawP.world.main.g.gui.set_to_layout();
        }
    }
    else {
        dragging = false;
        end_edit();   // commit the drag (or no-op click) as one undo step
    }
}

void MotionPathTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if(!dragging || !drawP.controls.leftClickHeld) return;
    MotionPath* mp = path();
    if(!mp) { dragging = false; return; }
    if((motion.pos - dragDownScreen).norm() > 4.0f) dragMoved = true;
    const Vector2f local = mp->coords.get_mouse_pos(drawP.world);
    if(dragTangent) {
        if(dragNode >= mp->points.size()) return;
        const Vector2f offset = local - mp->points[dragNode];
        const bool smooth = dragNode < mp->nodeType.size() && mp->nodeType[dragNode] == 1;
        if(dragTangentIn) {
            if(dragNode < mp->controlIn.size())  mp->controlIn[dragNode]  = offset;
            if(smooth && dragNode < mp->controlOut.size()) mp->controlOut[dragNode] = Vector2f{-offset.x(), -offset.y()};
        }
        else {
            if(dragNode < mp->controlOut.size()) mp->controlOut[dragNode] = offset;
            if(smooth && dragNode < mp->controlIn.size())  mp->controlIn[dragNode]  = Vector2f{-offset.x(), -offset.y()};
        }
    }
    else {
        if(dragNode < mp->points.size()) mp->points[dragNode] = local;
    }
    commit();
}
