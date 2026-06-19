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

        // PHASE6 M3: Add Point — click two adjacent vertices on the canvas (they
        // turn yellow), then insert a new vertex at the midpoint of that edge.
        if(a.d.polygonMode && editTool) {
            const auto& sel = editTool->selectedHandles;
            const size_t n = a.d.points.size();
            std::optional<size_t> insertAfter;   // edge start index, or none if selection isn't a single edge
            if(sel.size() == 2 && n >= 3) {
                const size_t lo = std::min(sel[0], sel[1]);
                const size_t hi = std::max(sel[0], sel[1]);
                if(hi == lo + 1) insertAfter = lo;                  // adjacent in sequence
                else if(lo == 0 && hi == n - 1) insertAfter = hi;   // wrap-around edge (last -> first)
            }
            if(insertAfter.has_value()) {
                const size_t after = insertAfter.value();
                text_button(gui, "polygon add point", "Add Point", { .wide = true, .onClick = [this, after] {
                    auto& rect = static_cast<RectangleCanvasComponent&>(comp->obj->get_comp());
                    const size_t m = rect.d.points.size();
                    const Vector2f& va = rect.d.points[after];
                    const Vector2f& vb = rect.d.points[(after + 1) % m];
                    const Vector2f mid{(va.x() + vb.x()) * 0.5f, (va.y() + vb.y()) * 0.5f};
                    rect.d.points.insert(rect.d.points.begin() + static_cast<long>(after + 1), mid);
                    comp->obj->commit_update(drawP);
                    if(editTool) editTool->refresh_point_handles();   // vector resized -> re-point handles
                    drawP.world.main.g.gui.set_to_layout();
                }});
            }
            else {
                text_label_light(gui, "Select 2 adjacent vertices to add a point");
            }
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
        // PHASE6 M2: one free-moving handle per polygon vertex. Handles hold
        // pointers into a.d.points; the vector is not resized during a drag
        // session. After Add Point grows the vector, EditTool::refresh_point_handles
        // calls this again to re-point the handles (M3).
        for(auto& pt : a.d.points)
            editTool.add_point_handle({&pt, nullptr, nullptr});
    }
    else {
        editTool.add_point_handle({&a.d.p1, nullptr, &a.d.p2});
        editTool.add_point_handle({&a.d.p2, &a.d.p1, nullptr});
    }
}

void RectDrawEditTool::commit_edit_updates(std::any& prevData) {
}

bool RectDrawEditTool::edit_update() {
    return true;
}
