#include "BrushPresets.hpp"

#ifdef HVYM_HAS_LIBMYPAINT

#include <initializer_list>
#include <utility>

namespace HVYM::Brushes {

namespace {

// Build a BrushParams starting from inkternity_default_params() and
// layering the listed base-value overrides + pressure mappings on top.
// Reads close to the pre-Phase-3 apply_*() functions with the
// mypaint_brush_set_base_value calls replaced by plain data.
BrushParams build_params(std::initializer_list<std::pair<MyPaintBrushSetting, float>> overrides,
                         std::initializer_list<LinearPressureMapping> mappings) {
    BrushParams p = inkternity_default_params();
    for (auto [setting, value] : overrides) {
        p.baseValues[setting] = value;
    }
    for (const auto& m : mappings) {
        p.pressureMappings.push_back(m);
    }
    return p;
}

// Technical pen — hard edges with very slight pressure variance. Much
// subtler than Fine inker (-0.3..+0.2) or Brush pen (-1.5..+0.8); just
// enough to give the line some life without losing the technical-pen feel.
BrushParams technical_pen_params() {
    return build_params(
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,    1.0f},
            {MYPAINT_BRUSH_SETTING_HARDNESS,              1.0f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,                1.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS, 6.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,6.0f},
        },
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, -2.0f, 1.0f},
        });
}

// Fine inker — crisper than the technical pen (max hardness, no opacity
// falloff) with about half the pressure variance. Same line-weight
// purpose, less hand-drawn looseness. Lighter pressure also drops opacity
// a bit so faint pen-touches leave a faint mark.
BrushParams fine_inker_params() {
    return build_params(
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,    1.5f},
            {MYPAINT_BRUSH_SETTING_HARDNESS,              1.0f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,                1.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS, 5.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,5.0f},
        },
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, -1.0f,  0.5f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,             -0.4f,  0.0f},
        });
}

// Brush pen — heavy pressure-tapered: low pressure = very thin, high =
// thick. Extreme radius pressure response (log-radius range of 5.3 means
// ~40x diameter variance from lightest tap to full pressure). Lightest
// touches are sub-pixel hairlines, heavy presses go fat-brush.
BrushParams brush_pen_params() {
    return build_params(
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,    2.0f},
            {MYPAINT_BRUSH_SETTING_HARDNESS,              0.85f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,                1.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS, 4.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,4.0f},
        },
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, -3.5f, 1.8f},
        });
}

// Fine marker — soft marker tip with light tactile pressure response.
// Pressing harder squeezes out more pigment and flexes the tip slightly,
// so it feels less mechanical without going wet/blendy (PHASE1.md §4
// excludes wet/blendy explicitly). Slight per-dab radius jitter for a
// textured felt-tip edge.
BrushParams fine_marker_params() {
    return build_params(
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,    1.6f},
            {MYPAINT_BRUSH_SETTING_HARDNESS,              0.6f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,                0.7f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS, 4.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,4.0f},
            {MYPAINT_BRUSH_SETTING_RADIUS_BY_RANDOM,      0.15f},
        },
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, -0.4f, 0.3f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,             -0.3f, 0.0f},
        });
}

// Broad marker — large soft brush; lower opacity for marker-style
// buildup. Lighter pressure drops opacity further (real broad markers
// fade out under a light hand). Heavier RADIUS_BY_RANDOM than the fine
// marker because the bigger tip has more edge to express texture across.
// Elongated, tilted dab reads as a blurred chisel-tip rectangle when
// combined with the soft 0.35 hardness.
BrushParams broad_marker_params() {
    return build_params(
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,    3.0f},
            {MYPAINT_BRUSH_SETTING_HARDNESS,              0.35f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,                0.4f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS, 3.5f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,3.5f},
            {MYPAINT_BRUSH_SETTING_RADIUS_BY_RANDOM,      0.25f},
            {MYPAINT_BRUSH_SETTING_ELLIPTICAL_DAB_RATIO,  2.5f},
            {MYPAINT_BRUSH_SETTING_ELLIPTICAL_DAB_ANGLE,  20.0f},
        },
        {
            {MYPAINT_BRUSH_SETTING_OPAQUE, -0.2f, 0.0f},
        });
}

// Wet ink — inky fountain-pen visual: full saturation, soft edges, drops
// of ink scatter slightly so lines aren't crisp. Stays inside PHASE1.md
// §4 (no wet/smudge media): "wet" here is the look (random dab variance),
// not actual blending with surrounding pixels. Opacity stays at full
// (1.0) regardless of pressure — wet ink lines are always solid; only
// drop *size* varies, not saturation.
BrushParams wet_ink_params() {
    return build_params(
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,    1.8f},
            {MYPAINT_BRUSH_SETTING_HARDNESS,              0.55f},
            {MYPAINT_BRUSH_SETTING_OPAQUE,                1.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS, 5.0f},
            {MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,5.0f},
            {MYPAINT_BRUSH_SETTING_RADIUS_BY_RANDOM,      0.50f},
            {MYPAINT_BRUSH_SETTING_OFFSET_BY_RANDOM,      0.50f},
        },
        {
            {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, -0.5f, 0.4f},
        });
}

const std::vector<BrushPreset>& curated_presets_impl() {
    static const std::vector<BrushPreset> kPresets = {
        { "Technical pen", "data/icons/technical-pen.svg", technical_pen_params(), BrushCategory::SHARP    },
        { "Fine inker",    "data/icons/fine-inker.svg",    fine_inker_params(),    BrushCategory::SHARP    },
        { "Brush pen",     "data/icons/brush-pen.svg",     brush_pen_params(),     BrushCategory::SHARP    },
        { "Fine marker",   "data/icons/fine-marker.svg",   fine_marker_params(),   BrushCategory::TEXTURED },
        { "Broad marker",  "data/icons/broad-marker.svg",  broad_marker_params(),  BrushCategory::TEXTURED },
        { "Wet ink",       "data/icons/wet-ink.svg",       wet_ink_params(),       BrushCategory::TEXTURED },
    };
    return kPresets;
}

}  // namespace

const BrushParams& inkternity_default_params() {
    static const BrushParams params = []() {
        BrushParams p;
        // Seed every slot from libmypaint's own defaults so settings the
        // pre-Phase-3 reset_base_values() didn't touch are still in a known
        // state on preset switch.
        for (int i = 0; i < MYPAINT_BRUSH_SETTINGS_COUNT; ++i) {
            const auto id = static_cast<MyPaintBrushSetting>(i);
            p.baseValues[i] = mypaint_brush_setting_info(id)->def;
        }
        // Inkternity overrides — the values the pre-Phase-3
        // reset_base_values() applied that diverge from libmypaint's
        // own defaults (HARDNESS 0.8 -> 0.85, RADIUS_LOGARITHMIC 2.0 ->
        // 1.5, OPAQUE_MULTIPLY 0.0 -> 1.0, DABS_PER_BASIC_RADIUS 0.0 ->
        // 4.0, DABS_PER_ACTUAL_RADIUS 2.0 -> 4.0). Settings already
        // matching libmypaint defaults (OPAQUE, OPAQUE_LINEARIZE,
        // RADIUS_BY_RANDOM, OFFSET_BY_RANDOM, ELLIPTICAL_DAB_*,
        // COLOR_*) don't need an override entry here.
        p.baseValues[MYPAINT_BRUSH_SETTING_HARDNESS]               = 0.85f;
        p.baseValues[MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC]     = 1.5f;
        p.baseValues[MYPAINT_BRUSH_SETTING_OPAQUE_MULTIPLY]        = 1.0f;
        p.baseValues[MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS]  = 4.0f;
        p.baseValues[MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS] = 4.0f;
        return p;
    }();
    return params;
}

const std::vector<BrushPreset>& curated_presets() {
    return curated_presets_impl();
}

BrushPresetDefaults defaults_for(const BrushPreset& preset) {
    return {
        preset.params.baseValues[MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC],
        preset.params.baseValues[MYPAINT_BRUSH_SETTING_HARDNESS],
        preset.params.baseValues[MYPAINT_BRUSH_SETTING_OPAQUE],
    };
}

void apply_preset(MyPaintBrush* brush, int presetIndex) {
    if (!brush) return;
    const auto& list = curated_presets();
    if (list.empty()) return;
    const int safe = (presetIndex >= 0 && presetIndex < static_cast<int>(list.size())) ? presetIndex : 0;
    apply_brush_params(brush, list[safe].params);
}

void apply_tunable_overrides(MyPaintBrush* brush, float diameter, float hardness, float opacity) {
    if (!brush) return;
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, diameter);
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_HARDNESS, hardness);
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_OPAQUE, opacity);
}

void apply_brush_params(MyPaintBrush* brush, const BrushParams& params) {
    if (!brush) return;
    // Base values: write every slot so switching presets fully resets
    // brush state regardless of what the prior preset left in the 50+
    // settings it didn't touch.
    for (int i = 0; i < MYPAINT_BRUSH_SETTINGS_COUNT; ++i) {
        mypaint_brush_set_base_value(brush, static_cast<MyPaintBrushSetting>(i),
                                     params.baseValues[i]);
    }
    // Pressure curves: clear every setting's PRESSURE mapping (cheap; one
    // pointer-tagged free per setting in libmypaint), then install the
    // ones this preset declares. We only operate on the PRESSURE input
    // because the curated set never uses other inputs (random / stroke /
    // tilt / ...). When the customization drawer adds support for other
    // input curves we'll widen this loop.
    for (int i = 0; i < MYPAINT_BRUSH_SETTINGS_COUNT; ++i) {
        mypaint_brush_set_mapping_n(brush, static_cast<MyPaintBrushSetting>(i),
                                    MYPAINT_BRUSH_INPUT_PRESSURE, 0);
    }
    for (const auto& m : params.pressureMappings) {
        mypaint_brush_set_mapping_n(brush, m.setting,
                                    MYPAINT_BRUSH_INPUT_PRESSURE, 2);
        mypaint_brush_set_mapping_point(brush, m.setting,
                                        MYPAINT_BRUSH_INPUT_PRESSURE,
                                        0, 0.0f, m.lowOffset);
        mypaint_brush_set_mapping_point(brush, m.setting,
                                        MYPAINT_BRUSH_INPUT_PRESSURE,
                                        1, 1.0f, m.highOffset);
    }
}

}  // namespace HVYM::Brushes

#endif  // HVYM_HAS_LIBMYPAINT
