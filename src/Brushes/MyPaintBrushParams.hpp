#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#ifdef HVYM_HAS_LIBMYPAINT

extern "C" {
#include <mypaint-brush-settings.h>
}

namespace HVYM::Brushes {

// PHASE3.md §3 A1 — the customization-drawer parameter catalog.
//
// libmypaint exposes 65 brush settings (MYPAINT_BRUSH_SETTINGS_COUNT). 8 of
// those are color settings (color_h, color_s, color_v, change_color_h,
// change_color_l, change_color_hsl_s, change_color_v, change_color_hsv_s)
// which are deliberately excluded per PHASE3.md Decision #0 — a brush
// preset captures *feel*, not color. This module is the single source of
// truth for that exclusion + the display grouping.
//
// Min / max / default / tooltip / display name come straight from libmypaint
// at runtime (mypaint_brush_setting_info), so this module does not duplicate
// the brushsettings.json metadata. Our value-add is the grouping (an
// Inkternity taxonomy, not a libmypaint one), the color exclusion, and a
// couple of UI hints (log-scale slider, render-as-boolean).
enum class BrushParamGroup : uint8_t {
    Basic = 0,    // opacity, radius, hardness, softness, anti-aliasing, pressure gain
    Dabs,         // dabs-per-radius / dabs-per-second
    Speed,        // speed1/2 slowness + gamma
    Smudge,       // smudge bucket, smudge length, restore-color, paint-mode
    Jitter,       // offset / radius randomness, by-speed offset
    Tracking,     // slow_tracking, tracking_noise, direction_filter
    Shape,        // elliptical dab ratio + angle
    Stroke,       // stroke duration / hold / threshold (the 'stroke' input plumbing)
    GridMap,      // gridmap_scale (and x/y) — texture-grid input scale
    Rendering,    // eraser / lock_alpha / colorize / posterize / snap_to_pixel
    Custom,       // user-defined custom input + its slowness
};

// Per-param UI metadata layered on top of libmypaint's runtime info.
//   logScale  -- slider should be log-scale (radius_logarithmic, smudge_*_log, ...)
//   boolean   -- render as a toggle, not a slider (eraser, lock_alpha, colorize,
//                snap_to_pixel). libmypaint stores these as 0.0 / 1.0 floats.
struct BrushParamMeta {
    MyPaintBrushSetting id;
    BrushParamGroup group;
    bool logScale;
    bool boolean;
};

// All 57 non-color settings, in libmypaint enum order. Stable order is the
// drawer's natural top-to-bottom layout when grouped views are flattened.
std::span<const BrushParamMeta> all_params();

// Human-readable group label. English; gettext-ready later.
std::string_view group_display_name(BrushParamGroup g);

// libmypaint accessors — thin wrappers around mypaint_brush_setting_info()
// so callers don't need to extern-C the C header. All return references to
// libmypaint static data; safe to hold across frames.
struct BrushParamRange {
    float min;
    float def;
    float max;
    bool  constant;
};
BrushParamRange   param_range(MyPaintBrushSetting id);
std::string_view  param_display_name(MyPaintBrushSetting id);
std::string_view  param_internal_name(MyPaintBrushSetting id);
std::string_view  param_tooltip(MyPaintBrushSetting id);

// Programmatic expression of Decision #0. True for color_h/s/v +
// change_color_h/l/hsl_s/v/hsv_s. Callers that load preset JSON from disk
// should skip any key whose setting id satisfies this; the in-memory
// param table here has them filtered out already.
bool is_color_param(MyPaintBrushSetting id);

}  // namespace HVYM::Brushes

#endif  // HVYM_HAS_LIBMYPAINT
