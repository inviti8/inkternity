#pragma once
#include "DrawingProgramToolBase.hpp"
#include "../../SharedTypes.hpp"   // WorldVec / WorldScalar for the captured region
#include <include/core/SkRefCnt.h>
#include <functional>

class DrawingProgram;
class SkImage;
struct DrawData;

// PHASE3.md §3 A1.M6 + §4 B.M3 prerequisite (PHASE3.md §8 Shared.M1).
//
// Sibling of ButtonSelectTool, with two changes:
//   - the selection rect is aspect-locked to 1:1 (always a square)
//   - the captured pixels are downscaled to a fixed targetSize x targetSize
//     SkImage, then handed to an on-commit callback the caller provides
//
// Lifetime model: caller constructs the tool with (targetSize, callback),
// installs it via DrawingProgram::switch_to_tool_ptr, then forgets about
// it. On commit (mouse-up after a non-degenerate drag) the tool fires the
// callback with the captured SkImage and restores the prior tool. On
// cancel (Escape, or switch_tool called externally) the tool restores
// the prior tool WITHOUT firing the callback -- the caller's "in-progress
// capture" state should remain unchanged in that case.
//
// Encoding to PNG is intentionally the caller's responsibility (A1.M6
// holds the SkImage in-memory until preset save; B.M3 hands it to
// AvatarStore which encodes 256/64 sidecar PNGs). Keeps the tool's
// contract focused: square pixels in, square pixels out.
class SquareCanvasCaptureTool : public DrawingProgramToolBase {
    public:
        // Source-pixel side cap per PHASE3.md Decision #13. The aspect-locked
        // square is computed in cam-space then clamped so the captured world
        // region never exceeds this on either side. Well above what 64x64 or
        // 256x256 downscale needs; prevents the pathological "capture the
        // whole canvas at extreme zoom-out" case.
        static constexpr int MAX_SOURCE_SIDE_PX = 2048;

        // The captured square in WORLD space (so a caller can place something back
        // over the exact source pixels, at the right scale/position, even after an
        // async round-trip when the camera may have moved).
        struct CaptureRegion {
            WorldVec topLeft;         // world position of the square's top-left corner
            WorldScalar sideWorld{1}; // world length of one side of the captured square
            double rotation = 0.0;    // camera rotation at capture time
        };

        using OnCaptureCallback =
            std::function<void(sk_sp<SkImage> capturedImage, const CaptureRegion& region)>;

        // targetSize: final output side length in px (e.g. 64 for brush
        //             icons, 256 for avatar masters).
        // previousToolType: tool to restore on commit/cancel. Captured at
        //             construction so the tool can roll back the active
        //             tool without the caller having to track it.
        // onCapture: invoked exactly once on commit with the captured
        //             targetSize x targetSize SkImage. Never invoked on
        //             cancel. May be null (capture without callback is a
        //             no-op, used only for tests).
        // transparentBackground: true (default) captures only drawn pixels on a
        //             transparent background (brush icons / avatars want this).
        //             false composites all VISIBLE layers over the canvas
        //             background — a WYSIWYG, opaque "drawing on paper" image
        //             (what AI reangle needs; the service mattes an opaque figure,
        //             not bare transparent strokes).
        // centerOut: false (default) drags the square from a corner (anchor →
        //             cursor). true grows it symmetrically OUTWARD from the
        //             mouse-down point, so that point is the square's CENTER —
        //             lets the artist put the cursor on a feature (e.g. the hips)
        //             and have it be the known center of the capture (AI reangle).
        SquareCanvasCaptureTool(DrawingProgram& initDrawP,
                                int targetSize,
                                DrawingProgramToolType previousToolType,
                                OnCaptureCallback onCapture,
                                bool transparentBackground = true,
                                bool centerOut = false);

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
        virtual void input_key_callback(const InputManager::KeyCallbackArgs& key) override;

    private:
        // Given the anchor (mouse-down) and current cursor position in
        // cam-space, returns (camMin, camMax) for the aspect-locked square
        // rect, clamped so the corresponding world region never exceeds
        // MAX_SOURCE_SIDE_PX on either side.
        std::pair<Vector2f, Vector2f> compute_square_cam_rect(const DrawData& drawData) const;

        // Renders the cam-space square to a targetSize x targetSize raster
        // surface, snapshots, fires onCapture_. Lifted (with the
        // aspect-square specialization) from ButtonSelectTool::capture_skin_to_selected.
        void capture_and_commit(const Vector2f& camMin, const Vector2f& camMax);

        // Hand control back to previousToolType_ without invoking the
        // callback. Used on Escape, right-click, or any externally
        // triggered switch_tool.
        void cancel_and_restore();

        int targetSize_;
        DrawingProgramToolType previousToolType_;
        OnCaptureCallback onCapture_;
        bool transparentBackground_ = true;
        bool centerOut_ = false;
        bool dragging_       = false;
        bool restoreOnSwitch_= true;  // set false during commit so capture's own switch_tool doesn't recurse
        Vector2f dragStart_  {0.0f, 0.0f};
        Vector2f dragCurrent_{0.0f, 0.0f};
};
