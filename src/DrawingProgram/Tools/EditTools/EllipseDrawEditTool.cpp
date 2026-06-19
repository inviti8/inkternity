#include "EllipseDrawEditTool.hpp"
#include "../../DrawingProgram.hpp"
#include "../../../World.hpp"
#include "../../../MainProgram.hpp"
#include "../../../DrawData.hpp"
#include <cereal/types/vector.hpp>
#include "../EditTool.hpp"

#include "../../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../../GUIStuff/ElementHelpers/RadioButtonHelpers.hpp"
#include "../../../GUIStuff/ElementHelpers/LayoutHelpers.hpp"
#include "../../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"

EllipseDrawEditTool::EllipseDrawEditTool(DrawingProgram& initDrawP, CanvasComponentContainer::ObjInfo* initComp):
    DrawingProgramEditToolBase(initDrawP, initComp)
{}

void EllipseDrawEditTool::edit_gui(Toolbar& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& a = static_cast<EllipseCanvasComponent&>(comp->obj->get_comp());
    auto& gui = drawP.world.main.g.gui;
    auto commit_update_func = [&] { comp->obj->commit_update(drawP); };
    auto commit_update_and_layout_func = [&] {
        comp->obj->commit_update(drawP);
        drawP.world.main.g.gui.set_to_layout();
    };

    gui.new_id("edit tool ellipse", [&] {
        text_label_centered(gui, "Edit Ellipse");
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
            slider_scalar_field(gui, "relstrokewidth", "Outline Size", &a.d.strokeWidth, 3.0f, 40.0f, {.onEdit = commit_update_func});
            left_to_right_line_layout(gui, [&] {
                t.color_button_right("Outline color button", &a.d.strokeColor, { .onChange = commit_update_func });
                text_label(gui, "Outline Color");
            });
        }
    });
}

void EllipseDrawEditTool::edit_start(EditTool& editTool, std::any& prevData) {
    auto& a = static_cast<EllipseCanvasComponent&>(comp->obj->get_comp());

    prevData = a.d;
    // PHASE6: editing an ellipse makes it skewable. Convert a legacy bbox ellipse
    // to the affine center+tips form (lossless — same axis-aligned ellipse) the
    // first time it's edited; thereafter the two axis-tip handles rotate/scale/
    // shear it. (Undo restores the pre-edit data via EditTool's oldData snapshot.)
    if(!a.d.affineMode) {
        a.d.center = Vector2f{(a.d.p1.x() + a.d.p2.x()) * 0.5f, (a.d.p1.y() + a.d.p2.y()) * 0.5f};
        a.d.tipA = Vector2f{a.d.p2.x(), a.d.center.y()};   // right end of the horizontal semi-axis
        a.d.tipB = Vector2f{a.d.center.x(), a.d.p2.y()};   // bottom end of the vertical semi-axis
        a.d.affineMode = true;
        comp->obj->commit_update(drawP);
    }
    // Two free-moving handles at the semi-axis tips. The center stays put while
    // editing (move the whole ellipse with the normal selection drag); dragging a
    // tip changes that axis, and non-perpendicular axes produce the shear/skew.
    editTool.add_point_handle({&a.d.tipA, nullptr, nullptr});
    editTool.add_point_handle({&a.d.tipB, nullptr, nullptr});
}

void EllipseDrawEditTool::commit_edit_updates(std::any& prevData) {
}

bool EllipseDrawEditTool::edit_update() {
    return true;
}
