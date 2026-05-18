#include "MyPaintBrushParams.hpp"

#ifdef HVYM_HAS_LIBMYPAINT

#include <array>

namespace HVYM::Brushes {

namespace {

// 57 entries = 65 libmypaint settings minus the 8 color settings filtered
// per PHASE3.md Decision #0 (color_h/s/v + change_color_h/l/hsl_s/v/hsv_s).
//
// Order matches the libmypaint enum so a future flat "show every param"
// view keeps the source ordering by default. The drawer groups via the
// `group` field.
//
// log-scale flags follow libmypaint's _LOG / _LOGARITHMIC naming
// convention. RESTORE_COLOR is intentionally NOT excluded: its prefix is
// 'restore_', not 'color_'; libmypaint treats it as a smudge-bucket
// reset control, not a color parameter.
constexpr std::array<BrushParamMeta, 57> kParams = {{
    {MYPAINT_BRUSH_SETTING_OPAQUE,                       BrushParamGroup::Basic,     false, false},
    {MYPAINT_BRUSH_SETTING_OPAQUE_MULTIPLY,              BrushParamGroup::Basic,     false, false},
    {MYPAINT_BRUSH_SETTING_OPAQUE_LINEARIZE,             BrushParamGroup::Basic,     false, false},
    {MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,           BrushParamGroup::Basic,     true,  false},
    {MYPAINT_BRUSH_SETTING_HARDNESS,                     BrushParamGroup::Basic,     false, false},
    {MYPAINT_BRUSH_SETTING_SOFTNESS,                     BrushParamGroup::Basic,     false, false},
    {MYPAINT_BRUSH_SETTING_ANTI_ALIASING,                BrushParamGroup::Basic,     false, false},
    {MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS,        BrushParamGroup::Dabs,      false, false},
    {MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,       BrushParamGroup::Dabs,      false, false},
    {MYPAINT_BRUSH_SETTING_DABS_PER_SECOND,              BrushParamGroup::Dabs,      false, false},
    {MYPAINT_BRUSH_SETTING_GRIDMAP_SCALE,                BrushParamGroup::GridMap,   true,  false},
    {MYPAINT_BRUSH_SETTING_GRIDMAP_SCALE_X,              BrushParamGroup::GridMap,   true,  false},
    {MYPAINT_BRUSH_SETTING_GRIDMAP_SCALE_Y,              BrushParamGroup::GridMap,   true,  false},
    {MYPAINT_BRUSH_SETTING_RADIUS_BY_RANDOM,             BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_SPEED1_SLOWNESS,              BrushParamGroup::Speed,     false, false},
    {MYPAINT_BRUSH_SETTING_SPEED2_SLOWNESS,              BrushParamGroup::Speed,     false, false},
    {MYPAINT_BRUSH_SETTING_SPEED1_GAMMA,                 BrushParamGroup::Speed,     false, false},
    {MYPAINT_BRUSH_SETTING_SPEED2_GAMMA,                 BrushParamGroup::Speed,     false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_BY_RANDOM,             BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_Y,                     BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_X,                     BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_ANGLE,                 BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_ANGLE_ASC,             BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_ANGLE_VIEW,            BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_ANGLE_2,               BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_ANGLE_2_ASC,           BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_ANGLE_2_VIEW,          BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_ANGLE_ADJ,             BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_MULTIPLIER,            BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_BY_SPEED,              BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_OFFSET_BY_SPEED_SLOWNESS,     BrushParamGroup::Jitter,    false, false},
    {MYPAINT_BRUSH_SETTING_SLOW_TRACKING,                BrushParamGroup::Tracking,  false, false},
    {MYPAINT_BRUSH_SETTING_SLOW_TRACKING_PER_DAB,        BrushParamGroup::Tracking,  false, false},
    {MYPAINT_BRUSH_SETTING_TRACKING_NOISE,               BrushParamGroup::Tracking,  false, false},
    // COLOR_H / COLOR_S / COLOR_V — filtered (Decision #0)
    {MYPAINT_BRUSH_SETTING_RESTORE_COLOR,                BrushParamGroup::Smudge,    false, false},
    // CHANGE_COLOR_H / _L / _HSL_S / _V / _HSV_S — filtered (Decision #0)
    {MYPAINT_BRUSH_SETTING_SMUDGE,                       BrushParamGroup::Smudge,    false, false},
    {MYPAINT_BRUSH_SETTING_PAINT_MODE,                   BrushParamGroup::Smudge,    false, false},
    {MYPAINT_BRUSH_SETTING_SMUDGE_TRANSPARENCY,          BrushParamGroup::Smudge,    false, false},
    {MYPAINT_BRUSH_SETTING_SMUDGE_LENGTH,                BrushParamGroup::Smudge,    false, false},
    {MYPAINT_BRUSH_SETTING_SMUDGE_LENGTH_LOG,            BrushParamGroup::Smudge,    true,  false},
    {MYPAINT_BRUSH_SETTING_SMUDGE_BUCKET,                BrushParamGroup::Smudge,    false, false},
    {MYPAINT_BRUSH_SETTING_SMUDGE_RADIUS_LOG,            BrushParamGroup::Smudge,    true,  false},
    {MYPAINT_BRUSH_SETTING_ERASER,                       BrushParamGroup::Rendering, false, true },
    {MYPAINT_BRUSH_SETTING_STROKE_THRESHOLD,             BrushParamGroup::Stroke,    false, false},
    {MYPAINT_BRUSH_SETTING_STROKE_DURATION_LOGARITHMIC,  BrushParamGroup::Stroke,    true,  false},
    {MYPAINT_BRUSH_SETTING_STROKE_HOLDTIME,              BrushParamGroup::Stroke,    false, false},
    {MYPAINT_BRUSH_SETTING_CUSTOM_INPUT,                 BrushParamGroup::Custom,    false, false},
    {MYPAINT_BRUSH_SETTING_CUSTOM_INPUT_SLOWNESS,        BrushParamGroup::Custom,    false, false},
    {MYPAINT_BRUSH_SETTING_ELLIPTICAL_DAB_RATIO,         BrushParamGroup::Shape,     false, false},
    {MYPAINT_BRUSH_SETTING_ELLIPTICAL_DAB_ANGLE,         BrushParamGroup::Shape,     false, false},
    {MYPAINT_BRUSH_SETTING_DIRECTION_FILTER,             BrushParamGroup::Tracking,  false, false},
    {MYPAINT_BRUSH_SETTING_LOCK_ALPHA,                   BrushParamGroup::Rendering, false, true },
    {MYPAINT_BRUSH_SETTING_COLORIZE,                     BrushParamGroup::Rendering, false, true },
    {MYPAINT_BRUSH_SETTING_POSTERIZE,                    BrushParamGroup::Rendering, false, false},
    {MYPAINT_BRUSH_SETTING_POSTERIZE_NUM,                BrushParamGroup::Rendering, false, false},
    {MYPAINT_BRUSH_SETTING_SNAP_TO_PIXEL,                BrushParamGroup::Rendering, false, true },
    {MYPAINT_BRUSH_SETTING_PRESSURE_GAIN_LOG,            BrushParamGroup::Basic,     true,  false},
}};

// Compile-time guard against a libmypaint upgrade adding/removing settings
// silently. If libmypaint ever changes MYPAINT_BRUSH_SETTINGS_COUNT, the
// resync is: (a) update kParams above with new entries (or remove gone
// ones, keeping color filtering), (b) bump the literal here.
static_assert(MYPAINT_BRUSH_SETTINGS_COUNT == 65,
              "libmypaint setting count changed; resync kParams[] in "
              "MyPaintBrushParams.cpp against the new brushsettings.json");

}  // namespace

std::span<const BrushParamMeta> all_params() {
    return {kParams.data(), kParams.size()};
}

std::string_view group_display_name(BrushParamGroup g) {
    switch (g) {
        case BrushParamGroup::Basic:     return "Basic";
        case BrushParamGroup::Dabs:      return "Dabs";
        case BrushParamGroup::Speed:     return "Speed";
        case BrushParamGroup::Smudge:    return "Smudge";
        case BrushParamGroup::Jitter:    return "Jitter";
        case BrushParamGroup::Tracking:  return "Tracking";
        case BrushParamGroup::Shape:     return "Shape";
        case BrushParamGroup::Stroke:    return "Stroke";
        case BrushParamGroup::GridMap:   return "GridMap";
        case BrushParamGroup::Rendering: return "Rendering";
        case BrushParamGroup::Custom:    return "Custom";
    }
    return "Unknown";
}

BrushParamRange param_range(MyPaintBrushSetting id) {
    const MyPaintBrushSettingInfo* info = mypaint_brush_setting_info(id);
    return {info->min, info->def, info->max, static_cast<bool>(info->constant)};
}

std::string_view param_display_name(MyPaintBrushSetting id) {
    const MyPaintBrushSettingInfo* info = mypaint_brush_setting_info(id);
    return info->name ? std::string_view{info->name} : std::string_view{};
}

std::string_view param_internal_name(MyPaintBrushSetting id) {
    const MyPaintBrushSettingInfo* info = mypaint_brush_setting_info(id);
    return info->cname ? std::string_view{info->cname} : std::string_view{};
}

std::string_view param_tooltip(MyPaintBrushSetting id) {
    const MyPaintBrushSettingInfo* info = mypaint_brush_setting_info(id);
    return info->tooltip ? std::string_view{info->tooltip} : std::string_view{};
}

bool is_color_param(MyPaintBrushSetting id) {
    switch (id) {
        case MYPAINT_BRUSH_SETTING_COLOR_H:
        case MYPAINT_BRUSH_SETTING_COLOR_S:
        case MYPAINT_BRUSH_SETTING_COLOR_V:
        case MYPAINT_BRUSH_SETTING_CHANGE_COLOR_H:
        case MYPAINT_BRUSH_SETTING_CHANGE_COLOR_L:
        case MYPAINT_BRUSH_SETTING_CHANGE_COLOR_HSL_S:
        case MYPAINT_BRUSH_SETTING_CHANGE_COLOR_V:
        case MYPAINT_BRUSH_SETTING_CHANGE_COLOR_HSV_S:
            return true;
        default:
            return false;
    }
}

}  // namespace HVYM::Brushes

#endif  // HVYM_HAS_LIBMYPAINT
