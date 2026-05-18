#pragma once
#include "DrawingProgramToolBase.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"

#ifdef HVYM_HAS_LIBMYPAINT
extern "C" {
#include <mypaint-brush.h>
}
#endif

#include <chrono>

class DrawingProgram;
struct DrawData;
class MyPaintLayerCanvasComponent;

// PHASE1.md §4 — second brush tool, alongside the existing vector BrushTool.
// Each pen-down creates a new MyPaintLayerCanvasComponent (one layer per
// stroke, mirroring how vector BrushTool produces one BrushStrokeCanvasComponent
// per stroke). Pen-motion events feed the libmypaint brush state machine,
// which dispatches dabs onto the layer's LibMyPaintSkiaSurface.
//
// M3 minimum: hardcoded brush settings (radius, hardness, color from the
// global foreground). The curated preset registry and brush-picker UI from
// PHASE1.md M3 are follow-on work — see TODOs in MyPaintBrushTool.cpp.
class MyPaintBrushTool : public DrawingProgramToolBase {
    public:
        MyPaintBrushTool(DrawingProgram& initDrawP);
        virtual ~MyPaintBrushTool() override;

        virtual DrawingProgramToolType get_type() override;
        virtual void gui_toolbox(Toolbar& t) override;
        virtual void gui_phone_toolbox(PhoneDrawingProgramScreen& t) override;
        virtual void tool_update() override;
        virtual void right_click_popup_gui(Toolbar& t, Vector2f popupPos) override;
        virtual void erase_component(CanvasComponentContainer::ObjInfo* erasedComp) override;
        virtual void switch_tool(DrawingProgramToolType newTool) override;
        virtual void draw(SkCanvas* canvas, const DrawData& drawData) override;
        virtual bool prevent_undo_or_redo() override;
        virtual void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) override;
        virtual void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) override;
        virtual void input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis) override;

#ifdef HVYM_HAS_LIBMYPAINT
        // PHASE3 A1.M4 — exposed for the customization drawer to read +
        // mutate base values live. Returned pointer is valid for the
        // tool's lifetime; the brush instance is recreated only when
        // MyPaintBrushTool is destroyed (tool switch). Drawer must
        // bail when get_type() != MYPAINTBRUSH (callers check this
        // before calling get_brush()).
        MyPaintBrush* get_brush() { return brush_; }
#endif

    private:
        void begin_stroke(const Vector2f& canvasPos, float pressure);
        void continue_stroke(const Vector2f& canvasPos, float pressure);
        void end_stroke();

        CanvasComponentContainer::ObjInfo* objInfoBeingEdited = nullptr;
        std::chrono::steady_clock::time_point lastEventTime{};
        float currentPressure = 1.0f;

#ifdef HVYM_HAS_LIBMYPAINT
        MyPaintBrush* brush_ = nullptr;
#endif
};
