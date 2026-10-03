#pragma once
#include <include/core/SkCanvas.h>
#include <unordered_map>
#include <vector>
#include "../../DrawData.hpp"
#include "DrawingProgramToolBase.hpp"
#include "../../CanvasComponents/VectorGroupCanvasComponent.hpp"   // EraseSeg (partial mode)

class DrawingProgram;

class EraserTool : public DrawingProgramToolBase {
    public:
        EraserTool(DrawingProgram& initDrawP);
        virtual DrawingProgramToolType get_type() override;
        virtual void gui_toolbox(Toolbar& t) override;
        virtual void gui_phone_toolbox(PhoneDrawingProgramScreen& t) override;
        virtual void right_click_popup_gui(Toolbar& t, Vector2f popupPos) override;
        virtual void erase_component(CanvasComponentContainer::ObjInfo* erasedComp) override;
        virtual void tool_update() override;
        virtual void switch_tool(DrawingProgramToolType newTool) override;
        virtual void draw(SkCanvas* canvas, const DrawData& drawData) override;
        virtual bool prevent_undo_or_redo() override;
        virtual void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) override;
        virtual void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) override;

        std::unordered_set<CanvasComponentContainer::ObjInfo*> erasedComponents; // Pointers will be erased from this set if theyre erased in the main list (done by callback)
    private:
        void erase_between_points(const Vector2f& start, const Vector2f& end);
        std::optional<Vector2f> lastPosOpt;
        bool isErasing = false;
        // VECTOR_ERASER.md — Partial mode: eraser segments (in each touched VECTORGROUP's
        // object space) accumulated across the drag, applied once in switch_tool.
        std::unordered_map<CanvasComponentContainer::ObjInfo*,
                           std::vector<VectorGroupCanvasComponent::EraseSeg>> partialErase;
};
