#include "MotionPathTool.hpp"

#include "../DrawingProgram.hpp"
#include "../../World.hpp"
#include "../../MainProgram.hpp"
#include "../Layers/DrawingProgramLayerManager.hpp"
#include "../Layers/DrawingProgramLayerListItem.hpp"
#include "../Layers/MotionPath.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <algorithm>
#include <string>

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

void MotionPathTool::switch_tool(DrawingProgramToolType) {}
void MotionPathTool::erase_component(CanvasComponentContainer::ObjInfo*) {}
void MotionPathTool::right_click_popup_gui(Toolbar&, Vector2f) {}
void MotionPathTool::tool_update() {}
bool MotionPathTool::prevent_undo_or_redo() { return false; }

void MotionPathTool::gui_toolbox(Toolbar&) {
    using namespace GUIStuff::ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    MotionPath* mp = path();
    gui.new_id("motion path tool", [&] {
        text_label_centered(gui, "Motion Path");
        if(mp)
            text_label(gui, std::to_string(mp->points.size()) + " nodes");
        text_button(gui, "motion path done", "Done", { .wide = true, .onClick = [this] {
            drawP.switch_to_tool(DrawingProgramToolType::EDIT);
        }});
    });
}

void MotionPathTool::gui_phone_toolbox(PhoneDrawingProgramScreen&) {
    // Same minimal panel; both read the shared GUIManager.
    using namespace GUIStuff::ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    MotionPath* mp = path();
    gui.new_id("motion path tool phone", [&] {
        text_label_centered(gui, "Motion Path");
        if(mp)
            text_label(gui, std::to_string(mp->points.size()) + " nodes");
        text_button(gui, "motion path done phone", "Done", { .wide = true, .onClick = [this] {
            drawP.switch_to_tool(DrawingProgramToolType::EDIT);
        }});
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
