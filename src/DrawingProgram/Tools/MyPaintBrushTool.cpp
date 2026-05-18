#include "MyPaintBrushTool.hpp"

#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../Brushes/BrushPresets.hpp"
#include "../../Brushes/UserBrushPresets.hpp"
#include "../../CanvasComponents/MyPaintLayerCanvasComponent.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"
#include "Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp"

#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/RadioButtonHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>

#ifdef HVYM_HAS_LIBMYPAINT
#include "../../Brushes/BrushPresets.hpp"
extern "C" {
#include <mypaint-brush-settings.h>
#include <mypaint-surface.h>
}

namespace {
// libmypaint takes color as HSV base values, but the InfiniPaint palette
// hands us RGBA. Standard piecewise RGB->HSV; alpha is ignored (the brush
// preset's OPAQUE/OPAQUE_MULTIPLY already controls per-dab transparency).
void apply_foreground_color_to_brush(MyPaintBrush* brush, const Vector4f& rgba) {
    const float r = rgba.x();
    const float g = rgba.y();
    const float b = rgba.z();
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float delta = maxC - minC;
    float h = 0.0f;
    if (delta > 1e-6f) {
        if (maxC == r)      h = std::fmod(((g - b) / delta) + 6.0f, 6.0f);
        else if (maxC == g) h = ((b - r) / delta) + 2.0f;
        else                h = ((r - g) / delta) + 4.0f;
        h /= 6.0f;  // 0..1
    }
    const float s = (maxC > 1e-6f) ? (delta / maxC) : 0.0f;
    const float v = maxC;
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_COLOR_H, h);
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_COLOR_S, s);
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_COLOR_V, v);
}

// Make sure cfg.overrides has one entry per curated preset, seeded with the
// preset's default tunables. Called everywhere we read overrides so that
// older configs (or fresh-install configs without a saved overrides array)
// gracefully fill in.
void ensure_overrides_initialized(ToolConfiguration::MyPaintBrushToolConfig& cfg) {
    const auto& presets = HVYM::Brushes::curated_presets();
    const size_t need = presets.size();
    if (cfg.overrides.size() < need) {
        const size_t oldSize = cfg.overrides.size();
        cfg.overrides.resize(need);
        for (size_t i = oldSize; i < need; ++i) {
            const auto presetDefaults = HVYM::Brushes::defaults_for(presets[i]);
            cfg.overrides[i].diameter = presetDefaults.diameter;
            cfg.overrides[i].hardness = presetDefaults.hardness;
            cfg.overrides[i].opacity  = presetDefaults.opacity;
        }
    } else if (cfg.overrides.size() > need) {
        cfg.overrides.resize(need);
    }
}

void apply_active_preset_with_overrides(MyPaintBrush* brush, ToolConfiguration::MyPaintBrushToolConfig& cfg) {
    // PHASE3 A2.M2: user preset takes precedence when its path is set.
    // We re-read the JSON every apply (every begin_stroke) so on-disk
    // edits made externally show up next stroke. Files are tiny and
    // the OS caches the buffer; cost is negligible.
    if (!cfg.activeUserPresetPath.empty()) {
        if (auto params = UserBrushPresets::read_params_json(cfg.activeUserPresetPath)) {
            HVYM::Brushes::apply_brush_params(brush, *params);
            return;
        }
        // File missing or unparseable -- fall back to the curated path
        // and forget the user-preset selection so we don't keep hitting
        // the same dead path stroke after stroke.
        cfg.activeUserPresetPath.clear();
    }
    ensure_overrides_initialized(cfg);
    const int idx = (cfg.activePresetIndex >= 0 && cfg.activePresetIndex < static_cast<int>(cfg.overrides.size()))
                  ? cfg.activePresetIndex : 0;
    HVYM::Brushes::apply_preset(brush, idx);
    const auto& ov = cfg.overrides[idx];
    HVYM::Brushes::apply_tunable_overrides(brush, ov.diameter, ov.hardness, ov.opacity);
}
}  // namespace
#endif

MyPaintBrushTool::MyPaintBrushTool(DrawingProgram& initDrawP)
    : DrawingProgramToolBase(initDrawP) {
#ifdef HVYM_HAS_LIBMYPAINT
    brush_ = mypaint_brush_new();
    // Apply the persisted preset (with the user's per-preset slider
    // overrides on top) immediately so the tool is usable before the
    // first stroke. begin_stroke re-applies in case the user picks a
    // different preset or moves a slider between strokes.
    apply_active_preset_with_overrides(brush_, drawP.world.main.toolConfig.myPaintBrush);
#endif
}

MyPaintBrushTool::~MyPaintBrushTool() {
#ifdef HVYM_HAS_LIBMYPAINT
    if (brush_) mypaint_brush_unref(brush_);
#endif
}

DrawingProgramToolType MyPaintBrushTool::get_type() {
    return DrawingProgramToolType::MYPAINTBRUSH;
}

void MyPaintBrushTool::switch_tool(DrawingProgramToolType) {
    end_stroke();
}

void MyPaintBrushTool::erase_component(CanvasComponentContainer::ObjInfo* erasedComp) {
    if (objInfoBeingEdited == erasedComp) objInfoBeingEdited = nullptr;
}

void MyPaintBrushTool::tool_update() {
    if (!drawP.world.main.g.gui.cursor_obstructed())
        drawP.world.main.input.hideCursor = true;
}

void MyPaintBrushTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    // Mirror BrushTool's cursor circle so the user gets the same brush-tool
    // affordance. Hardcoded radius for the M3 minimum — wires to the
    // libmypaint RADIUS_LOGARITHMIC setting in the next M3 commit.
    if (drawP.world.main.input.isTouchDevice || drawData.main->g.gui.cursor_obstructed())
        return;
    const float screenRadius = 8.0f;
    Vector2f pos = drawData.main->input.mouse.pos;
    SkPaint linePaint;
    linePaint.setAntiAlias(drawData.skiaAA);
    linePaint.setStyle(SkPaint::kStroke_Style);
    linePaint.setStrokeCap(SkPaint::kRound_Cap);
    linePaint.setStrokeWidth(0.0f);
    linePaint.setColor4f({1.0f, 1.0f, 1.0f, 1.0f});
    canvas->drawPath(SkPath::Circle(pos.x(), pos.y(), screenRadius), linePaint);
    linePaint.setColor4f({0.0f, 0.0f, 0.0f, 1.0f});
    canvas->drawPath(SkPath::Circle(pos.x(), pos.y(), screenRadius - 1.0f), linePaint);
}

#ifdef HVYM_HAS_LIBMYPAINT
namespace {
// Render the curated presets as a horizontal row of svg_icon_buttons. The
// active preset is shown selected; clicking an icon updates the persisted
// index and re-applies the preset to the live MyPaintBrush so the next
// stroke uses it. Shared by both desktop and phone tool-options paths.
void render_brush_picker_row(GUIStuff::GUIManager& gui,
                             ToolConfiguration::MyPaintBrushToolConfig& cfg,
                             MyPaintBrush* brush) {
    using namespace GUIStuff;
    using namespace GUIStuff::ElementHelpers;
    ensure_overrides_initialized(cfg);
    CLAY_AUTO_ID({
        .layout = {
            .sizing = { .width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0) },
            .childGap = 4,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        const auto& presets = HVYM::Brushes::curated_presets();
        // PHASE2 M3 follow-up: filter the picker to presets in the
        // currently-active brush category (driven by which top-toolbar
        // brush button the user picked: SHARP for the ink button,
        // TEXTURED for the marker/wet-ink button).
        HVYM::Brushes::BrushCategory activeCat =
            (cfg.activePresetIndex >= 0 && cfg.activePresetIndex < static_cast<int>(presets.size()))
                ? presets[cfg.activePresetIndex].category
                : HVYM::Brushes::BrushCategory::SHARP;
        for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
            const auto& preset = presets[i];
            if (preset.category != activeCat) continue;
            svg_icon_button(gui, preset.name.c_str(), preset.iconPath, {
                .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                .isSelected = (cfg.activePresetIndex == i),
                .size = SMALL_BUTTON_SIZE + 4,
                .onClick = [brush, &cfg, i] {
                    cfg.activePresetIndex = i;
                    cfg.activeUserPresetPath.clear();  // curated wins -- drop any user-preset override
                    apply_active_preset_with_overrides(brush, cfg);
                }
            });
        }
    }
}

void render_brush_tunable_sliders(GUIStuff::GUIManager& gui,
                                  ToolConfiguration::MyPaintBrushToolConfig& cfg,
                                  MyPaintBrush* brush) {
    using namespace GUIStuff;
    using namespace GUIStuff::ElementHelpers;
    ensure_overrides_initialized(cfg);
    const int idx = (cfg.activePresetIndex >= 0 && cfg.activePresetIndex < static_cast<int>(cfg.overrides.size()))
                  ? cfg.activePresetIndex : 0;
    auto& ov = cfg.overrides[idx];
    auto onEdit = [brush, &cfg] {
        apply_active_preset_with_overrides(brush, cfg);
    };
    // Diameter is libmypaint's RADIUS_LOGARITHMIC (log2 of pixel radius);
    // -2..5 covers ~0.25 to ~32 pixel radius which spans every preset.
    slider_scalar_field<float>(gui, "diameter", "Diameter", &ov.diameter, -2.0f, 5.0f, { .onEdit = onEdit });
    slider_scalar_field<float>(gui, "hardness", "Hardness", &ov.hardness, 0.0f, 1.0f, { .onEdit = onEdit });
    slider_scalar_field<float>(gui, "opacity",  "Opacity",  &ov.opacity,  0.0f, 1.0f, { .onEdit = onEdit });
}
}  // namespace
#endif

void MyPaintBrushTool::gui_toolbox(Toolbar&) {
#ifdef HVYM_HAS_LIBMYPAINT
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    auto& cfg = drawP.world.main.toolConfig.myPaintBrush;

    gui.new_id("mypaint brush tool", [&] {
        text_label_centered(gui, "Pixel Brush");
        render_brush_picker_row(gui, cfg, brush_);
        render_brush_tunable_sliders(gui, cfg, brush_);
    });
#endif
}

void MyPaintBrushTool::gui_phone_toolbox(PhoneDrawingProgramScreen&) {
#ifdef HVYM_HAS_LIBMYPAINT
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    auto& cfg = drawP.world.main.toolConfig.myPaintBrush;

    gui.new_id("mypaint brush tool phone", [&] {
        render_brush_picker_row(gui, cfg, brush_);
        render_brush_tunable_sliders(gui, cfg, brush_);
    });
#endif
}
void MyPaintBrushTool::right_click_popup_gui(Toolbar&, Vector2f) {}
bool MyPaintBrushTool::prevent_undo_or_redo() { return objInfoBeingEdited != nullptr; }

void MyPaintBrushTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if (button.button != InputManager::MouseButton::LEFT) return;
    if (button.down && drawP.layerMan.is_a_layer_being_edited() && !objInfoBeingEdited
        && !drawP.world.main.g.gui.cursor_obstructed()) {
        if (drawP.world.main.input.pen.isDown && drawP.world.main.input.pen.pressure != 0.0f)
            currentPressure = drawP.world.main.input.pen.pressure;
        else
            currentPressure = 1.0f;
        begin_stroke(button.pos, currentPressure);
    } else if (!button.down && objInfoBeingEdited) {
        end_stroke();
    }
}

void MyPaintBrushTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if (!objInfoBeingEdited) return;
    continue_stroke(motion.pos, currentPressure);
}

void MyPaintBrushTool::input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis) {
    if (axis.axis == SDL_PEN_AXIS_PRESSURE) currentPressure = axis.value;
}

void MyPaintBrushTool::begin_stroke(const Vector2f& canvasPos, float pressure) {
#ifdef HVYM_HAS_LIBMYPAINT
    auto* container = new CanvasComponentContainer(drawP.world.netObjMan, CanvasComponentType::MYPAINTLAYER);
    container->coords = drawP.world.drawData.cam.c;
    objInfoBeingEdited = drawP.layerMan.add_component_to_layer_being_edited(container);

    // Two-step reset: mypaint_brush_reset queues a full state clear (incl.
    // accumulated position) that takes effect on the next stroke_to;
    // mypaint_brush_new_stroke clears stroke-local timers. Without _reset,
    // a new stroke linearly interpolates from the previous stroke's last
    // dab position, painting a connecting line across canvas gaps.
    // Re-apply the current preset (canonical defaults + per-preset slider
    // overrides) before each stroke so any picker/slider change between
    // strokes takes effect even if its onEdit callback didn't run (e.g.
    // config loaded from disk). Palette color overrides preset COLOR_H/S/V
    // so the user's chosen foreground wins regardless of which preset is
    // active.
    apply_active_preset_with_overrides(brush_, drawP.world.main.toolConfig.myPaintBrush);
    apply_foreground_color_to_brush(brush_, drawP.world.main.toolConfig.globalConf.foregroundColor);

    mypaint_brush_reset(brush_);
    mypaint_brush_new_stroke(brush_);

    auto& layer = static_cast<MyPaintLayerCanvasComponent&>(container->get_comp());

    // PHASE2 M1: start recording the stroke. Color comes from the
    // global foreground (same source apply_foreground_color_to_brush
    // just used). Base radius from the brush's current
    // RADIUS_LOGARITHMIC setting (libmypaint stores radius as
    // ln(radius_in_pixels)) so the M2 Schneider-fit pass can recover
    // per-sample width as baseRadius * pressure-derived factor.
    //
    // M3 follow-up: TEXTURED-category brushes (wet ink, markers) are
    // intentionally NOT recorded. Their visual character is emergent
    // from per-dab randomness; the vector representation can't reproduce
    // it, so we'd rather have StrokeVectorize naturally skip them than
    // produce a misleading "vectorized" output that loses the texture.
    {
        const auto& presets = HVYM::Brushes::curated_presets();
        auto& cfg = drawP.world.main.toolConfig.myPaintBrush;
        const HVYM::Brushes::BrushCategory cat =
            (cfg.activePresetIndex >= 0 && cfg.activePresetIndex < static_cast<int>(presets.size()))
                ? presets[cfg.activePresetIndex].category
                : HVYM::Brushes::BrushCategory::SHARP;
        if (cat == HVYM::Brushes::BrushCategory::SHARP) {
            const Vector4f& fg = drawP.world.main.toolConfig.globalConf.foregroundColor;
            const float radiusLog = mypaint_brush_get_base_value(brush_, MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC);
            layer.begin_recorded_stroke(Eigen::Vector3f{fg.x(), fg.y(), fg.z()}, std::exp(radiusLog));
            // Container's coords were just set to the camera coords above,
            // so canvasPos is already in component-local space at this instant.
            layer.record_stroke_sample(canvasPos.x(), canvasPos.y(), pressure);
        }
        // For TEXTURED, no begin_recorded_stroke → has_valid_recording()
        // returns false → vectorize tool skips this stroke.
    }

    MyPaintSurface* surf = layer.surface().surface();

    mypaint_surface_begin_atomic(surf);
    mypaint_brush_stroke_to(brush_, surf,
                            canvasPos.x(), canvasPos.y(),
                            pressure, 0.0f, 0.0f,
                            0.0,  // dtime — first event of stroke (just seeds position post-reset)
                            1.0f, 0.0f, 0.0f, FALSE);
    mypaint_surface_end_atomic(surf, nullptr);
    layer.mark_dirty();
    container->commit_update(drawP);

    lastEventTime = std::chrono::steady_clock::now();
#else
    (void)canvasPos; (void)pressure;
#endif
}

void MyPaintBrushTool::continue_stroke(const Vector2f& canvasPos, float pressure) {
#ifdef HVYM_HAS_LIBMYPAINT
    auto& container = *objInfoBeingEdited->obj;
    auto& layer = static_cast<MyPaintLayerCanvasComponent&>(container.get_comp());
    const Vector2f localPos = container.coords.from_cam_space_to_this(drawP.world, canvasPos);

    // PHASE2 M1: append the pen-motion sample to the recorded stroke
    // (same coords we feed into mypaint_brush_stroke_to below).
    layer.record_stroke_sample(localPos.x(), localPos.y(), pressure);

    const auto now = std::chrono::steady_clock::now();
    const double dtime = std::chrono::duration<double>(now - lastEventTime).count();
    lastEventTime = now;

    MyPaintSurface* surf = layer.surface().surface();
    mypaint_surface_begin_atomic(surf);
    mypaint_brush_stroke_to(brush_, surf,
                            localPos.x(), localPos.y(),
                            pressure, 0.0f, 0.0f,
                            dtime,
                            1.0f, 0.0f, 0.0f, FALSE);
    mypaint_surface_end_atomic(surf, nullptr);
    layer.mark_dirty();
    container.commit_update(drawP);
#else
    (void)canvasPos; (void)pressure;
#endif
}

void MyPaintBrushTool::end_stroke() {
    if (!objInfoBeingEdited) return;
#ifdef HVYM_HAS_LIBMYPAINT
    auto& containerPtr = objInfoBeingEdited->obj;
    containerPtr->commit_update(drawP);
    containerPtr->send_comp_update(drawP, true);
    if (containerPtr->get_world_bounds().has_value()) {
        drawP.layerMan.add_undo_place_component(objInfoBeingEdited);
    } else {
        // Stroke produced no pixels (e.g. clicked-without-dragging and the
        // first seed event drew nothing). Drop it so we don't leave a
        // zero-bounds layer in the undo stack.
        auto& components = containerPtr->parentLayer->get_layer().components;
        components->erase(components, containerPtr->objInfo);
    }
#endif
    objInfoBeingEdited = nullptr;
}
