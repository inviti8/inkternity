#include "StrokeVectorizeTool.hpp"

#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../World.hpp"
#include "../../CanvasComponents/BrushStrokeCanvasComponent.hpp"
#include "../../CanvasComponents/MyPaintLayerCanvasComponent.hpp"
#include "../../DrawingProgram/Layers/DrawingProgramLayerListItem.hpp"
#include "../../DrawingProgram/Layers/LayerKind.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../StrokeVectorize/SchneiderFit.hpp"
#include "Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathEffect.h>
#include <include/effects/SkDashPathEffect.h>

#include <algorithm>
#include <cmath>
#include <vector>

StrokeVectorizeTool::StrokeVectorizeTool(DrawingProgram& initDrawP)
    : DrawingProgramToolBase(initDrawP) {}

DrawingProgramToolType StrokeVectorizeTool::get_type() {
    return DrawingProgramToolType::STROKEVECTORIZE;
}

void StrokeVectorizeTool::switch_tool(DrawingProgramToolType) {
    dragging = false;
}

void StrokeVectorizeTool::erase_component(CanvasComponentContainer::ObjInfo*) {}
void StrokeVectorizeTool::tool_update() {}
void StrokeVectorizeTool::right_click_popup_gui(Toolbar&, Vector2f) {}
bool StrokeVectorizeTool::prevent_undo_or_redo() { return false; }

void StrokeVectorizeTool::gui_toolbox(Toolbar&) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    gui.new_id("stroke vectorize tool", [&] {
        text_label_centered(gui, "Stroke Vectorize");
        text_label(gui, "Drag a rect over the active layer to convert recorded libmypaint strokes inside it into vector strokes.");
    });
}

void StrokeVectorizeTool::gui_phone_toolbox(PhoneDrawingProgramScreen&) {
    gui_toolbox(*static_cast<Toolbar*>(nullptr));  // shared content; safe (gui_toolbox doesn't deref)
}

void StrokeVectorizeTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    if (!dragging) return;
    const float x1 = std::min(dragStart.x(), dragCurrent.x());
    const float y1 = std::min(dragStart.y(), dragCurrent.y());
    const float x2 = std::max(dragStart.x(), dragCurrent.x());
    const float y2 = std::max(dragStart.y(), dragCurrent.y());

    // Same dashed two-color rect ButtonSelectTool uses; visually
    // distinguishes the rect-drag-tool family from regular strokes.
    SkPaint p;
    p.setAntiAlias(drawData.skiaAA);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(0.0f);
    const SkScalar intervals[] = {6.0f, 4.0f};
    p.setPathEffect(SkDashPathEffect::Make(intervals, 0.0f));
    p.setColor4f({0.0f, 0.0f, 0.0f, 0.8f});
    canvas->drawRect(SkRect::MakeLTRB(x1, y1, x2, y2), p);
    p.setColor4f({1.0f, 1.0f, 1.0f, 0.8f});
    p.setPathEffect(SkDashPathEffect::Make(intervals, 5.0f));
    canvas->drawRect(SkRect::MakeLTRB(x1, y1, x2, y2), p);
}

void StrokeVectorizeTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if (button.button != InputManager::MouseButton::LEFT) return;
    if (button.down) {
        if (drawP.world.main.g.gui.cursor_obstructed()) return;
        dragging = true;
        dragStart = button.pos;
        dragCurrent = button.pos;
    } else if (dragging) {
        dragging = false;
        const Vector2f camMin(std::min(dragStart.x(), button.pos.x()),
                              std::min(dragStart.y(), button.pos.y()));
        const Vector2f camMax(std::max(dragStart.x(), button.pos.x()),
                              std::max(dragStart.y(), button.pos.y()));
        if ((camMax - camMin).x() < 4.0f || (camMax - camMin).y() < 4.0f) return;
        convert_selection(camMin, camMax);
    }
}

void StrokeVectorizeTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if (dragging) dragCurrent = motion.pos;
}

namespace {

#ifdef HVYM_HAS_LIBMYPAINT

// Map a parameter t ∈ [0..1] across the entire stroke back to "source
// sample index space" so we can pull per-sample pressure from the
// recorded samples at any point along the fitted output.
float pressure_at_chord_t(const std::vector<MyPaintLayerCanvasComponent::RecordedStrokeSample>& src,
                          float chordT)
{
    if (src.empty()) return 1.0f;
    if (src.size() == 1) return src[0].pressure;
    const float idxF = std::clamp(chordT, 0.0f, 1.0f) * static_cast<float>(src.size() - 1);
    const int i0 = std::clamp(static_cast<int>(std::floor(idxF)), 0, static_cast<int>(src.size()) - 2);
    const float frac = idxF - static_cast<float>(i0);
    return src[i0].pressure * (1.0f - frac) + src[i0 + 1].pressure * frac;
}

// Build a BrushStrokeCanvasComponent from a fitted-bezier list + the
// source recording (for color, base radius, and per-sample pressure).
// Returns nullptr if the result would be visually empty.
//
// Output strategy: emit the bezier endpoints (P0 of segment 0, plus P3
// of every segment) as the polyline points. BrushStrokeCanvasComponent
// internally Catmull-Rom subdivides each segment 5 times, so a sparse
// input with intelligent breakpoints (which is exactly what Schneider
// gives us — splits land at the high-curvature points) yields a far
// smoother rendered stroke than a dense per-segment resampling would.
// Dense resampling fights the Catmull-Rom smoother and produces
// chevron-shaped artifacts at every input control point.
CanvasComponentContainer* build_vector_stroke_from_recording(
    NetworkingObjects::NetObjManager& netObjMan,
    const std::vector<StrokeVectorize::CubicBezier2D>& beziers,
    const MyPaintLayerCanvasComponent& source,
    const CoordSpaceHelper& sourceCoords)
{
    if (beziers.empty()) return nullptr;

    auto* container = new CanvasComponentContainer(netObjMan, CanvasComponentType::BRUSHSTROKE);
    auto& bs = static_cast<BrushStrokeCanvasComponent&>(container->get_comp());
    container->coords = sourceCoords;

    // Color: source brush RGB + opaque alpha. (libmypaint blends opacity
    // per-dab via the brush settings; the vector stroke uses a single
    // alpha and lets per-point width carry the pressure variation.)
    const Eigen::Vector3f rgb = source.get_recorded_color();
    bs.d.color = Vector4f{rgb.x(), rgb.y(), rgb.z(), 1.0f};
    bs.d.hasRoundCaps = true;

    const float baseRadius = source.get_recorded_base_radius();
    const auto& srcSamples = source.get_recorded_samples();
    const auto width_at = [&](float globalT) {
        const float p = pressure_at_chord_t(srcSamples, globalT);
        // Width = diameter ≈ baseRadius * pressure * 2.
        return std::max(0.5f, baseRadius * p * 2.0f);
    };

    auto& outPts = *bs.d.points;
    outPts.reserve(beziers.size() + 1);

    // First point: P0 of the first bezier (== first recorded sample).
    {
        BrushStrokeCanvasComponentPoint p;
        p.pos = Vector2f{beziers.front().p0.x(), beziers.front().p0.y()};
        p.width = width_at(0.0f);
        outPts.push_back(p);
    }
    // Then P3 of every bezier in order (each is also the next bezier's P0).
    for (size_t b = 0; b < beziers.size(); ++b) {
        const auto& bz = beziers[b];
        const float globalT = static_cast<float>(b + 1) / static_cast<float>(beziers.size());
        BrushStrokeCanvasComponentPoint p;
        p.pos = Vector2f{bz.p3.x(), bz.p3.y()};
        p.width = width_at(globalT);
        outPts.push_back(p);
    }

    if (outPts.size() < 2) {
        delete container;
        return nullptr;
    }
    return container;
}

#endif  // HVYM_HAS_LIBMYPAINT

}  // namespace

void StrokeVectorizeTool::convert_selection(const Vector2f& camMin, const Vector2f& camMax) {
#ifdef HVYM_HAS_LIBMYPAINT
    auto& world = drawP.world;

    // Operate on the layer currently being edited — same scoping as the
    // brush (add-to-layer-being-edited) and eraser (LAYER_BEING_EDITED).
    // libmypaint strokes can live on any layer, not just the named Ink
    // layer, so vectorize follows the active edit target. Bail if that
    // target is unset or a folder.
    auto editLayerLock = drawP.layerMan.get_editing_layer().lock();
    if (!editLayerLock || editLayerLock->is_folder()) return;

    auto& components = editLayerLock->get_layer().components;
    if (!components) return;

    // Build the world-space AABB of the selection rect by projecting
    // the four camera-space corners into world coordinates. This is the
    // intersection-test target for each component's existing world bounds.
    const auto& cameraCoords = world.drawData.cam.c;
    const WorldVec corner00 = cameraCoords.from_space({camMin.x(), camMin.y()});
    const WorldVec corner10 = cameraCoords.from_space({camMax.x(), camMin.y()});
    const WorldVec corner01 = cameraCoords.from_space({camMin.x(), camMax.y()});
    const WorldVec corner11 = cameraCoords.from_space({camMax.x(), camMax.y()});
    SCollision::AABB<WorldScalar> selectionWorldAABB;
    selectionWorldAABB.min = WorldVec{
        std::min({corner00.x(), corner10.x(), corner01.x(), corner11.x()}),
        std::min({corner00.y(), corner10.y(), corner01.y(), corner11.y()})
    };
    selectionWorldAABB.max = WorldVec{
        std::max({corner00.x(), corner10.x(), corner01.x(), corner11.x()}),
        std::max({corner00.y(), corner10.y(), corner01.y(), corner11.y()})
    };

    // First pass: collect (sourceObjInfo, fittedBeziers) for everything
    // we'll convert. Done before any mutation so iterator stability
    // isn't a concern.
    struct Conversion {
        CanvasComponentContainer::ObjInfoIterator sourceIt;
        std::vector<StrokeVectorize::CubicBezier2D> beziers;
    };
    std::vector<Conversion> conversions;

    constexpr float kFitToleranceLocalPx = 0.75f;

    for (auto it = components->begin(); it != components->end(); ++it) {
        auto& container = *it->obj;
        if (container.get_comp().get_type() != CanvasComponentType::MYPAINTLAYER) continue;

        auto& mpLayer = static_cast<MyPaintLayerCanvasComponent&>(container.get_comp());
        if (!mpLayer.has_valid_recording()) continue;

        // Coarse world-bounds intersection check. Cheap reject.
        const auto worldBoundsOpt = container.get_world_bounds();
        if (!worldBoundsOpt.has_value()) continue;
        if (!SCollision::collide(worldBoundsOpt.value(), selectionWorldAABB)) continue;

        // Fit the recorded polyline.
        const auto& src = mpLayer.get_recorded_samples();
        std::vector<Eigen::Vector2f> polyline;
        polyline.reserve(src.size());
        for (const auto& s : src) polyline.emplace_back(s.x, s.y);

        auto beziers = StrokeVectorize::fit_cubic_beziers(polyline, kFitToleranceLocalPx);
        if (beziers.empty()) continue;

        conversions.push_back({it, std::move(beziers)});
    }

    if (conversions.empty()) return;

    // Second pass: build new vector components and push them onto the
    // INK layer. Track them for the place-undo entry.
    std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> placedNewObjs;
    placedNewObjs.reserve(conversions.size());

    for (const auto& conv : conversions) {
        auto& sourceContainer = *conv.sourceIt->obj;
        auto& mpLayer = static_cast<MyPaintLayerCanvasComponent&>(sourceContainer.get_comp());
        CanvasComponentContainer* newContainer =
            build_vector_stroke_from_recording(world.netObjMan, conv.beziers, mpLayer, sourceContainer.coords);
        if (!newContainer) continue;
        // Insert the vector stroke just before the source so it lands
        // at the same z-order (will appear at the same stack position
        // once the source is removed below).
        placedNewObjs.emplace_back(conv.sourceIt, newContainer);
    }

    if (!placedNewObjs.empty())
        drawP.layerMan.add_many_components_to_specific_layer(*editLayerLock, placedNewObjs);

    // Third pass: erase the source MyPaint components. erase_component_container
    // produces its own undo entry; together with the place-undo above, the user
    // hits Ctrl+Z twice to fully revert a drag (TODO: bundle into one).
    std::vector<CanvasComponentContainer::ObjInfo*> sourcesToErase;
    sourcesToErase.reserve(conversions.size());
    for (const auto& conv : conversions)
        sourcesToErase.push_back(&(*conv.sourceIt));
    drawP.layerMan.erase_component_container(sourcesToErase);

#else
    (void)camMin; (void)camMax;
#endif
}
