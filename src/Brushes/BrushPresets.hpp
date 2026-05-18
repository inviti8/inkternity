#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#ifdef HVYM_HAS_LIBMYPAINT

extern "C" {
#include <mypaint-brush.h>
#include <mypaint-brush-settings.h>
}

namespace HVYM::Brushes {

// PHASE1.md §4 — curated ink/marker preset list. Wet/oily/watercolor/smudge/
// charcoal/bristle presets are explicitly out of scope for Phase 1; only the
// six entries below ever appear in the brush picker.
//
// PHASE3.md §3 A1.M2 refactor: each preset is now expressed declaratively
// as a `BrushParams` struct (every base value + a list of pressure-input
// mappings), driven by a single generic `apply_brush_params()` function.
// The pre-Phase-3 shape was a function pointer per preset that called
// mypaint_brush_set_base_value 6-8 times; that scaled fine for the 6
// curated presets but doesn't generalize to user-saved presets where the
// param set is arbitrary. Same brush state, declarative shape — see
// `inkternity_default_params()` for the shared starting point every
// preset layers on top of.

// User-tunable subset of MyPaintBrush settings, surfaced as sliders in the
// brush picker. Each preset derives its canonical defaults from its
// declarative `params.baseValues`; the tool layer stores per-preset
// overrides separately and writes them on top of apply() at stroke start
// (see MyPaintBrushTool::begin_stroke).
struct BrushPresetDefaults {
    float diameter;  // MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC
    float hardness;  // MYPAINT_BRUSH_SETTING_HARDNESS
    float opacity;   // MYPAINT_BRUSH_SETTING_OPAQUE
};

// PHASE2 M3 follow-up: brush family. SHARP brushes (technical pen, fine
// inker, brush pen) render as crisp, uniform-color lines and convert
// cleanly to vector via StrokeVectorizeTool. TEXTURED brushes (fine
// marker, broad marker, wet ink) carry the wet/blotchy/spatter look as
// emergent properties of per-dab randomness — the vector representation
// can't reproduce that, so recording is intentionally suppressed for
// TEXTURED strokes (has_valid_recording stays false; vectorize tool
// skips them).
enum class BrushCategory : uint8_t {
    SHARP    = 0,
    TEXTURED = 1
};

// A two-point linear pressure → output curve. libmypaint adds the
// interpolated output to the setting's base value at draw time.
//   lowOffset  -- value at pressure 0
//   highOffset -- value at pressure 1
// (Two-point linear is all the curated presets use today; richer curve
// shapes are deferred to a future "curve editor" UI per PHASE3.md §3 A1.)
struct LinearPressureMapping {
    MyPaintBrushSetting setting;
    float lowOffset;
    float highOffset;
};

// Declarative brush state: one float per libmypaint setting + an optional
// list of pressure-input curves. `apply_brush_params()` writes every base
// value (so switching presets fully resets brush state) and reinstalls
// every pressure mapping in the list (clearing any leftover mapping on
// settings not in the list).
struct BrushParams {
    std::array<float, MYPAINT_BRUSH_SETTINGS_COUNT> baseValues{};
    std::vector<LinearPressureMapping> pressureMappings;
};

struct BrushPreset {
    std::string name;
    std::string iconPath;  // svg under data/icons/, e.g. "data/icons/technical-pen.svg"
    BrushParams params;
    BrushCategory category;
};

// Inkternity's preset starting point — libmypaint per-setting defaults
// with the small overrides the pre-Phase-3 reset_base_values() applied
// (HARDNESS 0.85, RADIUS_LOGARITHMIC 1.5, OPAQUE_MULTIPLY 1.0,
// DABS_PER_BASIC_RADIUS 4.0, DABS_PER_ACTUAL_RADIUS 4.0). Every curated
// preset starts from this; future user-saved presets will too.
const BrushParams& inkternity_default_params();

const std::vector<BrushPreset>& curated_presets();

// Slider-seed accessor — derived from params.baseValues so the displayed
// defaults and what apply_brush_params actually writes can never drift.
BrushPresetDefaults defaults_for(const BrushPreset& preset);

// Apply preset at presetIndex. Out-of-range index falls back to preset 0.
void apply_preset(MyPaintBrush* brush, int presetIndex);

// Write the three tunable values onto an already-apply()'d brush. Used by
// the tool to layer per-preset slider overrides on top of canonical
// defaults at stroke start.
void apply_tunable_overrides(MyPaintBrush* brush, float diameter, float hardness, float opacity);

// Generic driver — writes every entry in params.baseValues, clears every
// PRESSURE mapping, then sets the curves declared in params.pressureMappings.
// Safe to call mid-session to switch presets; no leftover state from a
// previously-applied preset survives.
void apply_brush_params(MyPaintBrush* brush, const BrushParams& params);

}  // namespace HVYM::Brushes

#endif  // HVYM_HAS_LIBMYPAINT
