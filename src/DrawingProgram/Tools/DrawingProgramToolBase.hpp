#pragma once
#include <include/core/SkCanvas.h>
#include "../../DrawData.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"
#include "../../InputManager.hpp"

class DrawingProgram;
class PhoneDrawingProgramScreen;

#define MINIMUM_DISTANCE_BETWEEN_BOUNDS 10.0f

enum class DrawingProgramToolType : int {
    BRUSH = 0,
    ERASER,
    LASSOSELECT,
    RECTSELECT,
    RECTANGLE,
    ELLIPSE,
    TEXTBOX,
    EYEDROPPER,
    SCREENSHOT,
    GRIDMODIFY,
    EDIT,
    ZOOM,
    PAN,
    LINE,
    MYPAINTBRUSH,
    WAYPOINT,
    BUTTONSELECT,
    STROKEVECTORIZE,
    // PHASE3.md Shared.M1 — short-lived capture tool, constructed
    // directly with target-size + callback args via switch_to_tool_ptr.
    // Never reached through allocate_tool_type (the by-type path has no
    // place to plumb the callback; see DrawingProgramToolBase.cpp).
    SQUARECANVASCAPTURE
};

class DrawingProgramToolBase {
    public:
        DrawingProgramToolBase(DrawingProgram& initDrawP);
        virtual DrawingProgramToolType get_type() = 0;
        virtual void gui_toolbox(Toolbar& t) = 0;
        virtual void gui_phone_toolbox(PhoneDrawingProgramScreen& t) = 0;
        virtual void erase_component(CanvasComponentContainer::ObjInfo* erasedComp) = 0;
        virtual void right_click_popup_gui(Toolbar& t, Vector2f popupPos) = 0;
        virtual void tool_update() = 0;
        virtual void draw(SkCanvas* canvas, const DrawData& drawData) = 0;
        virtual void switch_tool(DrawingProgramToolType newTool) = 0;
        virtual bool prevent_undo_or_redo() = 0;
        virtual void input_paste_callback(const CustomEvents::PasteEvent& paste);
        virtual void input_text_key_callback(const InputManager::KeyCallbackArgs& key);
        virtual void input_text_callback(const InputManager::TextCallbackArgs& text);
        virtual void input_key_callback(const InputManager::KeyCallbackArgs& key);
        virtual void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button);
        virtual void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion);
        virtual void input_pen_button_callback(const InputManager::PenButtonCallbackArgs& button);
        virtual void input_pen_touch_callback(const InputManager::PenTouchCallbackArgs& touch);
        virtual void input_pen_motion_callback(const InputManager::PenMotionCallbackArgs& motion);
        virtual void input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis);
        virtual std::optional<InputManager::TextBoxStartInfo> get_text_box_start_info();
        virtual ~DrawingProgramToolBase(); 
        static std::unique_ptr<DrawingProgramToolBase> allocate_tool_type(DrawingProgram& drawP, DrawingProgramToolType t);
    protected:
        DrawingProgram& drawP;
};
