#include "SquareCanvasCaptureTool.hpp"

#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../World.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkDashPathEffect.h>

#include <algorithm>
#include <cmath>
#include <utility>

SquareCanvasCaptureTool::SquareCanvasCaptureTool(DrawingProgram& initDrawP,
                                                 int targetSize,
                                                 DrawingProgramToolType previousToolType,
                                                 OnCaptureCallback onCapture)
    : DrawingProgramToolBase(initDrawP),
      targetSize_(std::max(1, targetSize)),
      previousToolType_(previousToolType),
      onCapture_(std::move(onCapture))
{}

DrawingProgramToolType SquareCanvasCaptureTool::get_type() {
    return DrawingProgramToolType::SQUARECANVASCAPTURE;
}

void SquareCanvasCaptureTool::tool_update() {}
void SquareCanvasCaptureTool::erase_component(CanvasComponentContainer::ObjInfo*) {}
void SquareCanvasCaptureTool::right_click_popup_gui(Toolbar&, Vector2f) {}
bool SquareCanvasCaptureTool::prevent_undo_or_redo() { return dragging_; }

void SquareCanvasCaptureTool::switch_tool(DrawingProgramToolType /*newTool*/) {
    // Fires for both our own capture_and_commit roll-back AND any
    // externally triggered switch. restoreOnSwitch_ is false only during
    // capture_and_commit's own switch_to_tool call, so we don't try to
    // re-restore the prior tool a second time.
    dragging_ = false;
    // No callback on cancel paths; capture_and_commit invokes onCapture_
    // itself before triggering its restoreOnSwitch_=false transition.
}

void SquareCanvasCaptureTool::gui_toolbox(Toolbar&) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    gui.new_id("square canvas capture tool", [&] {
        text_label_centered(gui, "Capture");
        text_label(gui, "Drag a square on the canvas to capture an icon.");
        text_label(gui, "Esc to cancel.");
    });
}

void SquareCanvasCaptureTool::gui_phone_toolbox(PhoneDrawingProgramScreen&) {
    // The capture tool is short-lived (active only between the [Capture]
    // button click and the mouse-up that commits). No phone toolbox
    // surface needed -- the prompt lives in the drawer that invoked
    // capture, not in the tool itself.
}

void SquareCanvasCaptureTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    if (!dragging_) return;
    const auto [camMin, camMax] = compute_square_cam_rect(drawData);
    if ((camMax - camMin).x() < 1.0f && (camMax - camMin).y() < 1.0f) return;

    // Same dashed-outline style as ButtonSelectTool (double-color so the
    // rect stays visible over light AND dark canvas areas).
    SkPaint p;
    p.setAntiAlias(drawData.skiaAA);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(0.0f);
    const SkScalar intervals[] = {6.0f, 4.0f};
    p.setPathEffect(SkDashPathEffect::Make(intervals, 0.0f));

    p.setColor4f({0.0f, 0.0f, 0.0f, 0.8f});
    canvas->drawRect(SkRect::MakeLTRB(camMin.x(), camMin.y(), camMax.x(), camMax.y()), p);
    p.setColor4f({1.0f, 1.0f, 1.0f, 0.8f});
    p.setPathEffect(SkDashPathEffect::Make(intervals, 5.0f));
    canvas->drawRect(SkRect::MakeLTRB(camMin.x(), camMin.y(), camMax.x(), camMax.y()), p);
}

void SquareCanvasCaptureTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if (button.button != InputManager::MouseButton::LEFT) {
        // Right-click during a drag cancels (matches the ButtonSelectTool
        // muscle-memory for "abandon current capture").
        if (button.down && button.button == InputManager::MouseButton::RIGHT && !dragging_) {
            cancel_and_restore();
        }
        return;
    }
    if (button.down) {
        if (drawP.world.main.g.gui.cursor_obstructed()) return;
        dragging_ = true;
        dragStart_   = button.pos;
        dragCurrent_ = button.pos;
    } else if (dragging_) {
        dragging_ = false;
        const auto [camMin, camMax] = compute_square_cam_rect(drawP.world.drawData);
        const Vector2f extent = camMax - camMin;
        if (extent.x() < 4.0f || extent.y() < 4.0f) {
            // Degenerate single-click; treat as cancel rather than
            // capturing a 1x1 swatch.
            cancel_and_restore();
            return;
        }
        capture_and_commit(camMin, camMax);
    }
}

void SquareCanvasCaptureTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if (dragging_) dragCurrent_ = motion.pos;
}

void SquareCanvasCaptureTool::input_key_callback(const InputManager::KeyCallbackArgs& key) {
    if (key.down && !key.repeat && key.key == InputManager::KEY_GENERIC_ESCAPE) {
        cancel_and_restore();
    }
}

std::pair<Vector2f, Vector2f> SquareCanvasCaptureTool::compute_square_cam_rect(const DrawData& drawData) const {
    const Vector2f anchor  = dragStart_;
    const Vector2f current = dragCurrent_;

    float dx = current.x() - anchor.x();
    float dy = current.y() - anchor.y();

    // Aspect-lock: side = max of absolute deltas, sign follows the
    // dominant axis. Pure-horizontal or pure-vertical drags default
    // both signs to positive so the rect grows down-right.
    float side = std::max(std::abs(dx), std::abs(dy));
    if (side <= 0.0f) return {anchor, anchor};

    const float sgnX = (dx == 0.0f) ? 1.0f : (dx > 0.0f ? 1.0f : -1.0f);
    const float sgnY = (dy == 0.0f) ? 1.0f : (dy > 0.0f ? 1.0f : -1.0f);

    // Source-pixel cap (Decision #13). Convert the cam-space side to
    // world-pixels via two from_space() probes, clamp, scale back.
    const auto& cameraCoords = drawData.cam.c;
    auto camToWorld = [&](Vector2f camPt) {
        return cameraCoords.from_space({camPt.x(), camPt.y()});
    };
    const auto a   = camToWorld({anchor.x(), anchor.y()});
    const auto bX  = camToWorld({anchor.x() + sgnX * side, anchor.y()});
    const auto bY  = camToWorld({anchor.x(), anchor.y() + sgnY * side});
    const float worldSideX = static_cast<float>((bX - a).norm());
    const float worldSideY = static_cast<float>((bY - a).norm());
    const float worldSide  = std::max(worldSideX, worldSideY);
    if (worldSide > static_cast<float>(MAX_SOURCE_SIDE_PX)) {
        side *= (static_cast<float>(MAX_SOURCE_SIDE_PX) / worldSide);
    }

    const Vector2f corner(anchor.x() + sgnX * side, anchor.y() + sgnY * side);
    return {
        Vector2f(std::min(anchor.x(), corner.x()), std::min(anchor.y(), corner.y())),
        Vector2f(std::max(anchor.x(), corner.x()), std::max(anchor.y(), corner.y()))
    };
}

void SquareCanvasCaptureTool::capture_and_commit(const Vector2f& camMin, const Vector2f& camMax) {
    // Render world into an offscreen targetSize x targetSize raster surface.
    // Lifted from ButtonSelectTool::capture_skin_to_selected with the
    // simplification that output is always square (so newInverseScale
    // collapses to a single value).
    const int outW = targetSize_;
    const int outH = targetSize_;
    SkImageInfo info = SkImageInfo::Make(outW, outH, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) { cancel_and_restore(); return; }
    SkCanvas* offCanvas = surface->getCanvas();

    const auto& cam = drawP.world.drawData.cam;
    const CoordSpaceHelper& cameraCoords = cam.c;

    const WorldVec topLeft     = cameraCoords.from_space({camMin.x(), camMin.y()});
    const WorldVec topRight    = cameraCoords.from_space({camMax.x(), camMin.y()});
    const WorldVec bottomLeft  = cameraCoords.from_space({camMin.x(), camMax.y()});
    const WorldVec bottomRight = cameraCoords.from_space({camMax.x(), camMax.y()});
    const WorldVec camCenter   = (topLeft + bottomRight) / WorldScalar(2);

    const WorldScalar distX = (camCenter - (topLeft + bottomLeft)  * WorldScalar(0.5)).norm();
    const WorldScalar distY = (camCenter - (topLeft + topRight)    * WorldScalar(0.5)).norm();
    const WorldVec vectorZoom{distX / WorldScalar(outW * 0.5), distY / WorldScalar(outH * 0.5)};
    const WorldScalar newInverseScale = (vectorZoom.x() + vectorZoom.y()) * WorldScalar(0.5);

    DrawData captureDD = drawP.world.drawData;
    captureDD.cam.set_based_on_properties(drawP.world, topLeft, newInverseScale, cameraCoords.rotation);
    captureDD.cam.set_viewing_area(Vector2f(static_cast<float>(outW), static_cast<float>(outH)));
    captureDD.takingScreenshot = true;
    captureDD.transparentBackground = true;
    captureDD.refresh_draw_optimizing_values();
    drawP.world.main.draw_world(offCanvas, drawP.world.main.world, captureDD);

    sk_sp<SkImage> snapshot = surface->makeImageSnapshot();
    sk_sp<SkImage> cpuImage = snapshot ? snapshot->makeRasterImage(nullptr) : nullptr;
    if (!cpuImage) cpuImage = snapshot;

    // Fire callback before restoring tool, so the caller's onCapture can
    // store the image in its own state (preset draft / AvatarStore).
    if (onCapture_ && cpuImage) onCapture_(cpuImage);

    // Restore prior tool. restoreOnSwitch_ guard prevents switch_tool()
    // from re-entering cancel_and_restore on the way out.
    restoreOnSwitch_ = false;
    drawP.switch_to_tool(previousToolType_, /*force=*/true);
}

void SquareCanvasCaptureTool::cancel_and_restore() {
    if (!restoreOnSwitch_) return;  // already in the middle of a restore
    restoreOnSwitch_ = false;
    dragging_ = false;
    drawP.switch_to_tool(previousToolType_, /*force=*/true);
}
