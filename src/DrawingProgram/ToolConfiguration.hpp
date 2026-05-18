#pragma once
#include "../SharedTypes.hpp"
#include "nlohmann/json.hpp"
#include "../GUIStuff/GUIManager.hpp"
#include "../WorldScreenshot.hpp"
#include "Tools/DrawingProgramToolBase.hpp"

class ToolConfiguration {
    public:
        struct BrushToolConfig {
            bool hasRoundCaps = true;
            float relativeWidth = 15.0f;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BrushToolConfig, hasRoundCaps, relativeWidth)
        } brush;

        struct EraserToolConfig {
            float relativeWidth = 15.0f;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EraserToolConfig, relativeWidth)
        } eraser;

        struct EllipseDrawToolConfig {
            float relativeWidth = 15.0f;
            int fillStrokeMode = 1;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EllipseDrawToolConfig, relativeWidth, fillStrokeMode)
        } ellipseDraw;

        struct RectDrawToolConfig {
            float relativeWidth = 15.0f;
            float relativeRadiusWidth = 10.0f;
            int fillStrokeMode = 1;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RectDrawToolConfig, relativeWidth, relativeRadiusWidth, fillStrokeMode)
        } rectDraw;

        struct EyeDropperToolConfig {
            bool selectingStrokeColor = true;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EyeDropperToolConfig, selectingStrokeColor)
        } eyeDropper;

        struct LineDrawToolConfig {
            bool hasRoundCaps = true;
            float relativeWidth = 15.0f;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(LineDrawToolConfig, hasRoundCaps, relativeWidth)
        } lineDraw;

        // Per-preset, user-tunable overrides surfaced as sliders in the
        // brush picker (diameter/hardness/opacity). Initial values come
        // from BrushPresetDefaults; once the user moves a slider the
        // edited value sticks across app restarts. A separate "reset to
        // defaults" action would refill from the preset defaults — not
        // wired yet (see follow-up).
        struct MyPaintBrushPresetOverrides {
            float diameter = 1.5f;
            float hardness = 0.85f;
            float opacity = 1.0f;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MyPaintBrushPresetOverrides, diameter, hardness, opacity)
        };

        struct MyPaintBrushToolConfig {
            int activePresetIndex = 0;  // index into HVYM::Brushes::curated_presets()
            std::vector<MyPaintBrushPresetOverrides> overrides;  // sized to match preset count on first use
            // PHASE3 A2.M2 -- when non-empty, takes precedence over
            // activePresetIndex: apply_active_preset_with_overrides
            // reads BrushParams from this file via UserBrushPresets and
            // applies them in place of the curated preset + overrides
            // path. Cleared when the user picks a curated preset from
            // the brush picker. Empty path = legacy curated-only mode.
            std::string activeUserPresetPath;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MyPaintBrushToolConfig, activePresetIndex, overrides, activeUserPresetPath)
        } myPaintBrush;

        struct ScreenshotToolConfig {
            int setDimensionSize = 1000;
            bool setDimensionIsX = true;
            WorldScreenshotInfo::ScreenshotType selectedType = WorldScreenshotInfo::ScreenshotType::JPG;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ScreenshotToolConfig, setDimensionSize, setDimensionIsX, selectedType)
        } screenshot;

        struct GlobalConfig {
            Vector4f foregroundColor{1.0f, 1.0f, 1.0f, 1.0f};
            Vector4f backgroundColor{0.0f, 0.0f, 0.0f, 1.0f};
            float relativeWidth = 15.0f;
            bool useGlobalRelativeWidth = false;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GlobalConfig, useGlobalRelativeWidth, foregroundColor, backgroundColor, relativeWidth)
        } globalConf;

        enum class RelativeWidthFailCode {
            SUCCESS,
            TOO_ZOOMED_IN,
            TOO_ZOOMED_OUT
        };

        float& get_stroke_size_relative_width_ref(DrawingProgramToolType toolType);
        const float& get_stroke_size_relative_width_ref(DrawingProgramToolType toolType) const;
        std::pair<std::optional<float>, RelativeWidthFailCode> get_relative_width_from_value(DrawingProgram& drawP, const WorldScalar& camInverseScale, float relativeWidth) const;
        std::pair<std::optional<float>, RelativeWidthFailCode> get_relative_width_stroke_size(DrawingProgram& drawP, const WorldScalar& camInverseScale) const;
        void print_relative_width_fail_message(RelativeWidthFailCode failCode);
        void relative_width_gui(DrawingProgram& drawP, const char* label);

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ToolConfiguration, brush, eraser, ellipseDraw, rectDraw, eyeDropper, lineDraw, myPaintBrush, screenshot, globalConf)
};
