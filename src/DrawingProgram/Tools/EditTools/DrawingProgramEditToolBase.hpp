#pragma once
#include <include/core/SkCanvas.h>
#include "../../../DrawData.hpp"
#include "../../../CanvasComponents/CanvasComponentContainer.hpp"

class DrawingProgram;
class EditTool;

class DrawingProgramEditToolBase {
    public:
        DrawingProgramEditToolBase(DrawingProgram& initDrawP, CanvasComponentContainer::ObjInfo* initComp);
        virtual void edit_start(EditTool& editTool, std::any& prevData) = 0;
        // PHASE6: (re)register the draggable point handles. Called by edit_start,
        // and again by EditTool::refresh_point_handles() after the editable point
        // list changes size (e.g. polygon Add/Delete vertex), since handles hold
        // pointers into that list. Default no-op for tools with fixed handles.
        virtual void register_handles(EditTool& editTool) {}
        // PHASE8: opt in to re-registering handles after each handle drag — needed
        // for bezier tangents whose coordMatrix tracks a (possibly moved) node.
        virtual bool wants_handle_refresh_on_drag() const { return false; }
        virtual void commit_edit_updates(std::any& prevData) = 0;
        virtual bool edit_update() = 0;
        virtual void edit_gui(Toolbar& t) = 0;
        virtual void right_click_popup_gui(Toolbar& t, Vector2f popupPos);
        virtual void input_paste_callback(const CustomEvents::PasteEvent& paste);
        virtual void input_text_key_callback(const InputManager::KeyCallbackArgs& key);
        virtual void input_text_callback(const InputManager::TextCallbackArgs& text);
        virtual void input_key_callback(const InputManager::KeyCallbackArgs& key);
        virtual void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button, bool isDraggingPoint);
        virtual void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion, bool isDraggingPoint);
        virtual std::optional<InputManager::TextBoxStartInfo> get_text_box_start_info();
        virtual ~DrawingProgramEditToolBase(); 
    protected:
        DrawingProgram& drawP;
        CanvasComponentContainer::ObjInfo* comp;
};
