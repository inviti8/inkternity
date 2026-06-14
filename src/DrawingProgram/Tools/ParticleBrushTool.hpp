#pragma once
#include "DrawingProgramToolBase.hpp"

// PHASE5.1 M2 — the particle brush. Stamps the FX Library's active effect onto
// the canvas as ParticleCanvasComponents. v1: click to place one at the cursor
// (brush size = the placement's localScale). Rate-based drag-stamping + a size
// slider are M2.1. Host-authored; no-op without HVYM_HAS_TIMELINEFX_LEGACY.
class ParticleBrushTool : public DrawingProgramToolBase {
    public:
        ParticleBrushTool(DrawingProgram& initDrawP);
        DrawingProgramToolType get_type() override;
        void gui_toolbox(Toolbar& t) override;
        void gui_phone_toolbox(PhoneDrawingProgramScreen& t) override;
        void erase_component(CanvasComponentContainer::ObjInfo* erasedComp) override;
        void right_click_popup_gui(Toolbar& t, Vector2f popupPos) override;
        void tool_update() override;
        void draw(SkCanvas* canvas, const DrawData& drawData) override;
        void switch_tool(DrawingProgramToolType newTool) override;
        bool prevent_undo_or_redo() override;
        void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) override;
        void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) override;

    private:
        void stamp(Vector2f camPos);   // place the active effect at a cam-space point
        float brushSize = 3.0f;        // -> placement localScale
        float rate = 3.0f;             // placements per second while held/dragging
        bool painting = false;
        float accum = 0.0f;            // time accumulator for rate-based stamping
        Vector2f lastCamPos{0.0f, 0.0f};
};
