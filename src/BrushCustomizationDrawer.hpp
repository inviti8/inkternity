#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <include/core/SkRefCnt.h>

#ifdef HVYM_HAS_LIBMYPAINT
#include "Brushes/BrushPresets.hpp"
extern "C" {
#include <mypaint-brush-settings.h>
}
#endif

class SkImage;
class Toolbar;

// PHASE3.md §3 A1.M4 — Customization drawer popup body.
//
// Owns the per-frame "snapshot brush base values + render sliders" loop.
// The popup container, attach-to-button geometry, and outside-click
// dismiss are Toolbar's responsibility -- we just render the body inside
// the scope it provides.
//
// Lifetime: one instance owned by Toolbar (constructed once). The active
// MyPaintBrushTool may come and go (tool switch); render() reads the
// current tool each frame and is a no-op when the active tool isn't a
// MyPaintBrushTool.
//
// Scope of this milestone (A1.M4):
//   - All 57 non-color params from HVYM::Brushes::all_params(), grouped
//     by BrushParamGroup with a text label per group.
//   - Each row: slider_scalar_field (label + numeric textbox + slider),
//     clamped to libmypaint's documented min/max.
//   - Live mutation: slider edits push to mypaint_brush_set_base_value
//     immediately so the next stroke uses the new value.
//
// Out of scope for this milestone (other A1 sub-milestones cover):
//   - Preset name display + Save / Save-as / Reset-all buttons   -> A1.M5
//   - Icon preview + "Capture from canvas..." button             -> A1.M6
//   - Per-group collapse toggle (everything renders expanded for now)
//   - Boolean-as-checkbox rendering for eraser / lock_alpha / colorize /
//     snap_to_pixel (currently rendered as 0..1 sliders; works but
//     less ergonomic -- polish item)
//   - Pressure-input curve editing (entire libmypaint curve UI is
//     deferred per PHASE3.md §3 A1, "scalar-only in v1")
//   - Phone-UI parity (deferred to A.M-phone milestone)
class BrushCustomizationDrawer {
    public:
        explicit BrushCustomizationDrawer(Toolbar& toolbar);

        // Render the popup body inside the caller's Clay scope. No-op
        // when no MyPaintBrushTool is active (the toolbar icon button
        // should also be hidden in that case; this is the safety net).
        void render_body();

    private:
#ifdef HVYM_HAS_LIBMYPAINT
        // Mirror of the live brush's base values, updated at the start of
        // every render. Sliders bind to slots in this array; their onEdit
        // callback pushes the slot back to the brush via
        // mypaint_brush_set_base_value. Indexed by MyPaintBrushSetting id.
        std::array<float, MYPAINT_BRUSH_SETTINGS_COUNT> liveValues_{};
        // Per-group expansion state. Indexed by BrushParamGroup value;
        // sized to the largest enum value + 1. Persists across opens of
        // the drawer (so a user who only ever tunes Smudge doesn't have
        // to re-expand it every time). Default: only Basic expanded.
        std::array<bool, 16> groupExpanded_{};

        // PHASE3.md §3 A1.M5 -- save-as modal state. When saveModalOpen_
        // is true, render_body swaps the param-sliders view for a name
        // + category input form. On confirm we snapshot the live brush
        // via brush_params_from_live and write through
        // UserBrushPresets::save into <configPath>/brush_presets/.
        bool saveModalOpen_ = false;
        std::string saveModalName_;
        HVYM::Brushes::BrushCategory saveModalCategory_ = HVYM::Brushes::BrushCategory::SHARP;

        // PHASE3.md §3 A1.M6 -- in-progress icon for the next preset
        // save. Populated by the SquareCanvasCaptureTool callback after
        // the artist drags a square on the canvas. Cleared on Save (the
        // PNG bytes are written via UserBrushPresets::save) and on
        // Cancel. Persists across modal close-and-reopen if the artist
        // captures then changes their mind about name / category.
        sk_sp<SkImage> capturedIcon_;

        // Renders the save-as modal body inside the drawer's popup.
        // Separate function so the param-sliders body and the save body
        // are clearly distinct render paths.
        void render_save_modal();
        // Closes the drawer popup, activates SquareCanvasCaptureTool at
        // 64x64 with a callback that stores the captured image into
        // capturedIcon_ and re-opens the drawer + save modal.
        void start_icon_capture();
#endif
        Toolbar& toolbar_;
};
