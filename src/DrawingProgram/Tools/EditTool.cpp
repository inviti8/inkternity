#include "EditTool.hpp"
#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../DrawData.hpp"
#include "Helpers/MathExtras.hpp"
#include "Helpers/SCollision.hpp"
#include "../../SharedTypes.hpp"
#include <cereal/types/vector.hpp>
#include <algorithm>
#include <memory>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>

#include "EditTools/TextBoxEditTool.hpp"
#include "EditTools/RectDrawEditTool.hpp"
#include "EditTools/ImageEditTool.hpp"
#include "EditTools/EllipseDrawEditTool.hpp"
#include "EditTools/BrushEditTool.hpp"

#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"

EditTool::EditTool(DrawingProgram& initDrawP):
    DrawingProgramToolBase(initDrawP)
{}

DrawingProgramToolType EditTool::get_type() {
    return DrawingProgramToolType::EDIT;
}

void EditTool::gui_toolbox(Toolbar& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;

    if(objInfoBeingEdited)
        compEditTool->edit_gui(t);
    else {
        gui.new_id("edit tool", [&] {
            text_label_centered(gui, "Edit");
        });
    }
}

void EditTool::gui_phone_toolbox(PhoneDrawingProgramScreen& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;

    gui.new_id("edit tool", [&] {
        text_label_centered(gui, "Edit");
    });
}

void EditTool::erase_component(CanvasComponentContainer::ObjInfo* erasedComp) {
    if(erasedComp == objInfoBeingEdited)
        switch_tool(get_type());
}

void EditTool::input_paste_callback(const CustomEvents::PasteEvent& paste) {
    if(objInfoBeingEdited)
        compEditTool->input_paste_callback(paste);
}

void EditTool::input_text_key_callback(const InputManager::KeyCallbackArgs& key) {
    if(objInfoBeingEdited)
        compEditTool->input_text_key_callback(key);
}

void EditTool::input_text_callback(const InputManager::TextCallbackArgs& text) {
    if(objInfoBeingEdited)
        compEditTool->input_text_callback(text);
}

void EditTool::input_key_callback(const InputManager::KeyCallbackArgs& key) {
    drawP.selection.input_key_callback_modify_selection(key);
}

void EditTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    drawP.selection.input_mouse_button_on_canvas_callback_modify_selection(button);
    if(button.button == InputManager::MouseButton::LEFT) {
        if(button.down && !drawP.world.main.g.gui.cursor_obstructed()) {
            if(!objInfoBeingEdited) {
                WorldVec mouseWorldPos = drawP.world.drawData.cam.c.from_space(button.pos);

                SCollision::AABB<WorldScalar> mouseAABB{mouseWorldPos - WorldVec{0.5f, 0.5f}, mouseWorldPos + WorldVec{0.5f, 0.5f}};
                SCollision::ColliderCollection<WorldScalar> cMouseAABB;
                cMouseAABB.aabb.emplace_back(mouseAABB);
                cMouseAABB.recalculate_bounds();

                SCollision::AABB<float> camMouseAABB{drawP.world.main.input.mouse.pos - Vector2f{0.5f, 0.5f}, button.pos + Vector2f{0.5f, 0.5f}};
                SCollision::ColliderCollection<float> camCMouseAABB;
                camCMouseAABB.aabb.emplace_back(camMouseAABB);
                camCMouseAABB.recalculate_bounds();

                bool modifySelection = !drawP.selection.is_being_transformed();
                if(button.clicks >= 2 && !drawP.world.main.input.key(InputManager::KEY_GENERIC_LSHIFT).held && !drawP.world.main.input.key(InputManager::KEY_GENERIC_LALT).held) {
                    CanvasComponentContainer::ObjInfo* selectedObjectToEdit = drawP.selection.get_front_object_colliding_with_in_editing_layer(camCMouseAABB);

                    if(selectedObjectToEdit && is_editable(selectedObjectToEdit)) {
                        drawP.selection.deselect_all();
                        edit_start(selectedObjectToEdit);
                        modifySelection = false;
                    }
                }
                if(modifySelection) {
                    if(drawP.world.main.input.key(InputManager::KEY_GENERIC_LSHIFT).held)
                        drawP.selection.add_from_cam_coord_collider_to_selection(camCMouseAABB, DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED, true);
                    else if(drawP.world.main.input.key(InputManager::KEY_GENERIC_LALT).held)
                        drawP.selection.remove_from_cam_coord_collider_to_selection(camCMouseAABB, DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED, true);
                    else {
                        drawP.selection.deselect_all();
                        drawP.selection.add_from_cam_coord_collider_to_selection(camCMouseAABB, DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED, true);
                    }
                }
            }
            else {
                SCollision::Circle<float> mouseCircle{button.pos, 1.0f};
                SCollision::ColliderCollection<float> cMouseCircle;
                cMouseCircle.circle.emplace_back(mouseCircle);
                cMouseCircle.recalculate_bounds();

                bool isMovingPoint = false;
                bool clickedAway = false;

                if(!pointDragging) {
                    for(HandleData& h : pointHandles) {
                        if(SCollision::collide(mouseCircle, SCollision::Circle<float>(drawP.world.drawData.cam.c.to_space(objInfoBeingEdited->obj->coords.from_space(h.coordMatrix * (*h.p))), drawP.drag_point_radius()))) {
                            pointDragging = &h;
                            isMovingPoint = true;
                        }
                    }
                    if(!isMovingPoint && !objInfoBeingEdited->obj->collides_with_cam_coords(drawP.world.drawData.cam.c, cMouseCircle))
                        clickedAway = true;
                }

                for(size_t hi = 0; hi < pointHandles.size(); ++hi) {
                    HandleData& h = pointHandles[hi];
                    if(SCollision::collide(mouseCircle, SCollision::Circle<float>(drawP.world.drawData.cam.c.to_space(objInfoBeingEdited->obj->coords.from_space(h.coordMatrix * (*h.p))), drawP.drag_point_radius()))) {
                        pointDragging = &h;
                        isMovingPoint = true;
                        // PHASE6: remember which handle was pressed + where, to tell
                        // a click (toggle selection) from a drag (move vertex) on release.
                        pressedHandleIndex = hi;
                        pointDownScreenPos = button.pos;
                        pointDragMoved = false;
                        break;
                    }
                }
                if(!isMovingPoint && !objInfoBeingEdited->obj->collides_with_cam_coords(drawP.world.drawData.cam.c, cMouseCircle))
                    clickedAway = true;

                if(clickedAway) {
                    // PHASE6: don't exit immediately — start a vertex marquee.
                    // On release: a drag box-selects vertices; a click (no drag)
                    // falls through to the old "click empty space exits edit".
                    marqueeActive = true;
                    marqueeStartScreen = button.pos;
                    marqueeCurScreen = button.pos;
                }
                else if(objInfoBeingEdited)
                    compEditTool->input_mouse_button_on_canvas_callback(button, pointDragging);
            }
        }
        else {
            // PHASE6: resolve a vertex marquee. A real drag box-selects vertices;
            // a click (no drag) on empty space exits edit (the prior behavior).
            if(marqueeActive) {
                if((marqueeCurScreen - marqueeStartScreen).norm() > 4.0f)
                    select_handles_in_screen_rect(marqueeStartScreen, marqueeCurScreen);
                else if(objInfoBeingEdited)
                    switch_tool(get_type());
                marqueeActive = false;
            }
            // PHASE6: a press+release on a handle without dragging toggles its
            // selection (used by polygon Add Point); a drag just moved the vertex.
            if(pointDragging && !pointDragMoved && pressedHandleIndex < pointHandles.size())
                toggle_handle_selection(pressedHandleIndex);
            // PHASE8: after dragging a node, bezier tangent handles' coordMatrix
            // (which translates by the node) is stale — re-register to snap them.
            const bool didDragHandle = pointDragging && pointDragMoved;
            if(objInfoBeingEdited)
                compEditTool->input_mouse_button_on_canvas_callback(button, pointDragging);
            if(pointDragging)
                pointDragging = nullptr;
            if(didDragHandle && objInfoBeingEdited && compEditTool->wants_handle_refresh_on_drag())
                refresh_point_handles(true);
        }
    }
}

void EditTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if(marqueeActive)
        marqueeCurScreen = motion.pos;
    if(objInfoBeingEdited) {
        if(drawP.controls.leftClickHeld && pointDragging) {
            // PHASE6: past a few screen px of movement this is a drag, not a click.
            constexpr float CLICK_VS_DRAG_PX = 4.0f;
            if((motion.pos - pointDownScreenPos).norm() > CLICK_VS_DRAG_PX)
                pointDragMoved = true;
            Vector2f newPos = pointDragging->coordMatrix.inverse() * objInfoBeingEdited->obj->coords.get_mouse_pos(drawP.world);
            if(newPos != *pointDragging->p) {
                if(pointDragging->min)
                    newPos = cwise_vec_max((*pointDragging->min + Vector2f{pointDragging->minimumDistanceBetweenMinAndPoint, pointDragging->minimumDistanceBetweenMinAndPoint}).eval(), newPos);
                if(pointDragging->max)
                    newPos = cwise_vec_min((*pointDragging->max - Vector2f{pointDragging->minimumDistanceBetweenMaxAndPoint, pointDragging->minimumDistanceBetweenMaxAndPoint}).eval(), newPos);
                *pointDragging->p = newPos;
                // PHASE8: smooth node — mirror the opposite tangent (offsets, so
                // negate). newPos is the dragged tangent's offset from its node.
                if(pointDragging->mirror)
                    *pointDragging->mirror = Vector2f{-newPos.x(), -newPos.y()};
                objInfoBeingEdited->obj->commit_update(drawP);
            }
        }
        compEditTool->input_mouse_motion_callback(motion, pointDragging);
    }
    drawP.selection.input_mouse_motion_callback_modify_selection(motion);
}

std::optional<InputManager::TextBoxStartInfo> EditTool::get_text_box_start_info() {
    if(objInfoBeingEdited)
        return compEditTool->get_text_box_start_info();
    return std::nullopt;
}

void EditTool::right_click_popup_gui(Toolbar& t, Vector2f popupPos) {
    if(objInfoBeingEdited)
        return compEditTool->right_click_popup_gui(t, popupPos);
    else
        return drawP.selection_action_menu(popupPos);
}

void EditTool::add_point_handle(const HandleData& handle) {
    pointHandles.emplace_back(handle);
}

void EditTool::switch_tool(DrawingProgramToolType newTool) {
    if(objInfoBeingEdited) {
        compEditTool->commit_edit_updates(prevData);
        objInfoBeingEdited->obj->commit_update(drawP);
        objInfoBeingEdited->obj->send_comp_update(drawP, true);

        if(undoAfterEditDone) {
            class EditCanvasComponentWorldUndoAction : public WorldUndoAction {
                public:
                    EditCanvasComponentWorldUndoAction(std::unique_ptr<CanvasComponent> initData, WorldUndoManager::UndoObjectID initUndoID):
                        data(std::move(initData)),
                        undoID(initUndoID)
                    {}
                    std::string get_name() const override {
                        return "Edit Canvas Component";
                    }
                    bool undo(WorldUndoManager& undoMan) override {
                        return undo_redo(undoMan);
                    }
                    bool redo(WorldUndoManager& undoMan) override {
                        return undo_redo(undoMan);
                    }
                    bool undo_redo(WorldUndoManager& undoMan) {
                        std::optional<NetworkingObjects::NetObjID> toEditID = undoMan.get_netid_from_undoid(undoID);
                        if(!toEditID.has_value())
                            return false;
                        auto objPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<CanvasComponentContainer>(toEditID.value());
                        std::unique_ptr<CanvasComponent> newData = objPtr->get_comp().get_data_copy();
                        objPtr->get_comp().set_data_from(*data);
                        data = std::move(newData);
                        objPtr->commit_update(undoMan.world.drawProg);
                        objPtr->send_comp_update(undoMan.world.drawProg, true);
                        return true;
                    }
                    ~EditCanvasComponentWorldUndoAction() {}
    
                    std::unique_ptr<CanvasComponent> data;
                    WorldUndoManager::UndoObjectID undoID;
            };
    
            drawP.world.undo.push(std::make_unique<EditCanvasComponentWorldUndoAction>(std::move(oldData), drawP.world.undo.get_undoid_from_netid(objInfoBeingEdited->obj.get_net_id())));
        }

        oldData = nullptr;
        objInfoBeingEdited = nullptr;
        drawP.world.main.g.gui.set_to_layout();
    }
    pointHandles.clear();
    pointDragging = nullptr;
    selectedHandles.clear();
    marqueeActive = false;

    if(!drawP.is_selection_allowing_tool(newTool))
        drawP.selection.deselect_all();
}

void EditTool::select_handles_in_screen_rect(const Vector2f& a, const Vector2f& b) {
    if(!objInfoBeingEdited) return;
    const Vector2f mn{std::min(a.x(), b.x()), std::min(a.y(), b.y())};
    const Vector2f mx{std::max(a.x(), b.x()), std::max(a.y(), b.y())};
    selectedHandles.clear();
    for(size_t hi = 0; hi < pointHandles.size(); ++hi) {
        HandleData& h = pointHandles[hi];
        const Vector2f sp = drawP.world.drawData.cam.c.to_space(objInfoBeingEdited->obj->coords.from_space(h.coordMatrix * *h.p));
        if(sp.x() >= mn.x() && sp.x() <= mx.x() && sp.y() >= mn.y() && sp.y() <= mx.y())
            selectedHandles.push_back(hi);
    }
}

void EditTool::refresh_point_handles(bool keepSelection) {
    std::vector<size_t> savedSelection;
    if(keepSelection)
        savedSelection = selectedHandles;
    pointHandles.clear();
    selectedHandles.clear();
    pointDragging = nullptr;
    if(objInfoBeingEdited && compEditTool)
        compEditTool->register_handles(*this);
    if(keepSelection)   // valid only if the handle set is the same size/order
        for(size_t i : savedSelection)
            if(i < pointHandles.size())
                selectedHandles.push_back(i);
}

void EditTool::toggle_handle_selection(size_t handleIndex) {
    auto it = std::find(selectedHandles.begin(), selectedHandles.end(), handleIndex);
    if(it != selectedHandles.end())
        selectedHandles.erase(it);
    else
        selectedHandles.push_back(handleIndex);
}

void EditTool::edit_start(CanvasComponentContainer::ObjInfo* comp, bool initUndoAfterEditDone) {
    bool isEditing = true;
    switch(comp->obj->get_comp().get_type()) {
        case CanvasComponentType::TEXTBOX: {
            compEditTool = std::make_unique<TextBoxEditTool>(drawP, comp);
            break;
        }
        case CanvasComponentType::ELLIPSE: {
            compEditTool = std::make_unique<EllipseDrawEditTool>(drawP, comp);
            break;
        }
        case CanvasComponentType::RECTANGLE: {
            compEditTool = std::make_unique<RectDrawEditTool>(drawP, comp);
            break;
        }
        case CanvasComponentType::IMAGE: {
            compEditTool = std::make_unique<ImageEditTool>(drawP, comp);
            break;
        }
        case CanvasComponentType::BRUSHSTROKE: {
            compEditTool = std::make_unique<BrushEditTool>(drawP, comp);
            break;
        }
        default: {
            isEditing = false;
            break;
        }
    }
    if(isEditing) {
        objInfoBeingEdited = comp;
        oldData = comp->obj->get_comp().get_data_copy();
        undoAfterEditDone = initUndoAfterEditDone;
        compEditTool->edit_start(*this, prevData);
        drawP.world.main.g.gui.set_to_layout();
    }
}

bool EditTool::is_editable(CanvasComponentContainer::ObjInfo* comp) {
    return true; // Brush strokes used to not be editable, but now they are so this always returns true
}

void EditTool::tool_update() {
    if(objInfoBeingEdited) {
        objInfoBeingEdited->obj->send_comp_update(drawP, false);
        bool shouldNotReset = compEditTool->edit_update();
        if(!shouldNotReset)
            switch_tool(get_type());
    }
}

bool EditTool::prevent_undo_or_redo() {
    return objInfoBeingEdited || drawP.selection.is_something_selected();
}

void EditTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    if(objInfoBeingEdited) {
        for(size_t hi = 0; hi < pointHandles.size(); ++hi) {
            HandleData& h = pointHandles[hi];
            const bool selected = std::find(selectedHandles.begin(), selectedHandles.end(), hi) != selectedHandles.end();
            const Vector2f tip = drawData.cam.c.to_space(objInfoBeingEdited->obj->coords.from_space(h.coordMatrix * *h.p));
            // PHASE8: a tangent handle draws an "arm" from its node + a green dot.
            if(h.armAnchor) {
                const Vector2f anchor = drawData.cam.c.to_space(objInfoBeingEdited->obj->coords.from_space(*h.armAnchor));
                SkPaint arm;
                arm.setAntiAlias(true);
                arm.setColor4f(SkColor4f{0.2f, 0.8f, 0.4f, 0.8f});
                arm.setStrokeWidth(1.0f);
                canvas->drawLine(anchor.x(), anchor.y(), tip.x(), tip.y(), arm);
                const SkColor4f color = selected ? SkColor4f{0.95f, 0.85f, 0.1f, 1.0f} : SkColor4f{0.2f, 0.8f, 0.4f, 1.0f};
                drawP.draw_drag_circle(canvas, tip, color, drawData);
            }
            else {
                // PHASE6: selected vertices render yellow, the rest cyan.
                const SkColor4f color = selected ? SkColor4f{0.95f, 0.85f, 0.1f, 1.0f} : SkColor4f{0.1f, 0.9f, 0.9f, 1.0f};
                drawP.draw_drag_circle(canvas, tip, color, drawData);
            }
        }
        // PHASE6: vertex marquee (screen space, like the edit handles above).
        if(marqueeActive) {
            const SkRect r = SkRect::MakeLTRB(std::min(marqueeStartScreen.x(), marqueeCurScreen.x()),
                                              std::min(marqueeStartScreen.y(), marqueeCurScreen.y()),
                                              std::max(marqueeStartScreen.x(), marqueeCurScreen.x()),
                                              std::max(marqueeStartScreen.y(), marqueeCurScreen.y()));
            SkPaint fill;
            fill.setColor4f(SkColor4f{0.3f, 0.6f, 0.95f, 0.18f});
            canvas->drawRect(r, fill);
            SkPaint outline;
            outline.setStyle(SkPaint::kStroke_Style);
            outline.setStrokeWidth(1.0f);
            outline.setColor4f(SkColor4f{0.4f, 0.7f, 1.0f, 0.9f});
            canvas->drawRect(r, outline);
        }
    }
}

EditTool::~EditTool() { }
