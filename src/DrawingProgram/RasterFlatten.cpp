#include "RasterFlatten.hpp"

#include "DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../DrawData.hpp"
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include "../CanvasComponents/MyPaintLayerCanvasComponent.hpp"
#include "Layers/DrawingProgramLayerManager.hpp"
#include "Layers/DrawingProgramLayerListItem.hpp"

#include <Helpers/Logger.hpp>

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace RasterFlatten {

#ifdef HVYM_HAS_LIBMYPAINT

namespace {
// Per-axis cap on the baked bitmap so an over-zoomed-out flatten can't ask
// for a gigapixel surface. Hit only when the in-view region is physically
// large relative to the finest stroke detail; then we coarsen the bake
// scale to fit and tell the user.
constexpr int kMaxFlattenDim = 8192;
}  // namespace

void flatten_ink_strokes_in_view(DrawingProgram& drawP) {
    auto& world = drawP.world;
    auto& main = world.main;

    // Flatten the layer currently being edited — same scoping as the brush,
    // eraser, and vectorize tool. Bail if the edit target is unset/a folder.
    auto editLayer = drawP.layerMan.get_editing_layer().lock();
    if (!editLayer || editLayer->is_folder()) {
        Logger::get().log("USERINFO", "Flatten: no active layer to flatten.");
        return;
    }
    auto& components = editLayer->get_layer().components;
    if (!components) return;

    // Active-view world AABB: project the four screen corners into world
    // space (matches how the screenshot + vectorize tools build their rects).
    const auto& cam = world.drawData.cam;
    const auto& camCoords = cam.c;
    const Vector2f screen = cam.viewingArea;
    const WorldVec c00 = camCoords.from_space({0.0f, 0.0f});
    const WorldVec c10 = camCoords.from_space({screen.x(), 0.0f});
    const WorldVec c01 = camCoords.from_space({0.0f, screen.y()});
    const WorldVec c11 = camCoords.from_space({screen.x(), screen.y()});
    SCollision::AABB<WorldScalar> viewAABB;
    viewAABB.min = WorldVec{std::min({c00.x(), c10.x(), c01.x(), c11.x()}),
                            std::min({c00.y(), c10.y(), c01.y(), c11.y()})};
    viewAABB.max = WorldVec{std::max({c00.x(), c10.x(), c01.x(), c11.x()}),
                            std::max({c00.y(), c10.y(), c01.y(), c11.y()})};

    // Collect in-view raster strokes; track the merged bounds + the finest
    // (smallest inverseScale = highest-res) source so the bake loses no detail.
    std::vector<CanvasComponentContainer::ObjInfoIterator> sources;
    std::optional<SCollision::AABB<WorldScalar>> mergedAABB;
    std::optional<WorldScalar> finestInverseScale;
    for (auto it = components->begin(); it != components->end(); ++it) {
        auto& container = *it->obj;
        if (container.get_comp().get_type() != CanvasComponentType::MYPAINTLAYER) continue;
        const auto wb = container.get_world_bounds();
        if (!wb.has_value()) continue;
        if (!SCollision::collide(wb.value(), viewAABB)) continue;

        sources.push_back(it);
        if (!mergedAABB) mergedAABB = wb.value();
        else mergedAABB->include_aabb_in_bounds(wb.value());
        const WorldScalar inv = container.coords.inverseScale;
        if (!finestInverseScale || inv < finestInverseScale.value())
            finestInverseScale = inv;
    }

    if (sources.size() < 2) {
        Logger::get().log("USERINFO",
            "Flatten: need at least 2 ink strokes in view (found " +
            std::to_string(sources.size()) + ").");
        return;
    }

    // Bake scale = finest source scale, coarsened only if the resulting
    // bitmap would blow past the per-axis cap.
    const WorldVec dim = mergedAABB->dim();
    WorldScalar targetInv = finestInverseScale.value();
    const WorldScalar minInvX = dim.x().divide_double(static_cast<double>(kMaxFlattenDim));
    const WorldScalar minInvY = dim.y().divide_double(static_cast<double>(kMaxFlattenDim));
    bool downsampled = false;
    if (minInvX > targetInv) { targetInv = minInvX; downsampled = true; }
    if (minInvY > targetInv) { targetInv = minInvY; downsampled = true; }

    const CoordSpaceHelper targetCoords(mergedAABB->min, targetInv, 0.0);
    // pos == AABB.min and rotation 0, so to_space(min) == 0 and
    // to_space(max) == the pixel resolution.
    const Vector2f resF = targetCoords.to_space(mergedAABB->max);
    Vector2i resolution{
        std::clamp(static_cast<int>(std::ceil(resF.x())), 1, kMaxFlattenDim),
        std::clamp(static_cast<int>(std::ceil(resF.y())), 1, kMaxFlattenDim)};

    sk_sp<SkSurface> surface = main.create_native_surface(resolution, false);
    if (!surface) {
        Logger::get().log("WORLDFATAL", "Flatten: could not create a render surface.");
        return;
    }
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SkColor4f{0.0f, 0.0f, 0.0f, 0.0f});

    // Render each source at the bake scale. Reuse the live DrawData but
    // retarget the camera onto our offscreen coords (same trick the BVH
    // node-cache uses in refresh_draw_cache).
    DrawData dd = world.drawData;
    dd.cam.c = targetCoords;
    dd.cam.set_viewing_area(resolution.cast<float>());
    dd.refresh_draw_optimizing_values();
    // `components` is z-ascending, and `sources` preserves that order, so
    // drawing front-to-back reproduces the on-canvas stacking.
    for (auto& it : sources) {
        auto& container = *it->obj;
        container.draw_with_predraw_data(canvas, dd, container.calculate_predraw_data(dd));
    }

    // Read the composited pixels back as 8bpc UNPREMULTIPLIED RGBA — exactly
    // what import_from_bitmap expects (readPixels unpremultiplies for us).
    SkBitmap baked;
    if (!baked.tryAllocPixels(SkImageInfo::Make(
            resolution.x(), resolution.y(),
            kRGBA_8888_SkColorType, kUnpremul_SkAlphaType))) {
        Logger::get().log("WORLDFATAL", "Flatten: could not allocate readback bitmap.");
        return;
    }
    if (!surface->readPixels(baked, 0, 0)) {
        Logger::get().log("WORLDFATAL", "Flatten: readback (readPixels) failed.");
        return;
    }

    // Bake into one merged raster component placed at the target coords.
    auto* merged = new CanvasComponentContainer(world.netObjMan, CanvasComponentType::MYPAINTLAYER);
    merged->coords = targetCoords;
    auto& mergedLayer = static_cast<MyPaintLayerCanvasComponent&>(merged->get_comp());
    mergedLayer.surface().import_from_bitmap(baked, 0, 0);
    mergedLayer.mark_dirty();

    // Anchor the merged component at the top of the flattened block (highest
    // z among the sources) so it lands where the most-recent stroke was.
    CanvasComponentContainer::ObjInfoIterator anchor = sources.front();
    for (auto& it : sources)
        if (it->pos > anchor->pos) anchor = it;

    std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> toPlace;
    toPlace.emplace_back(anchor, merged);
    const auto placed = drawP.layerMan.add_many_components_to_specific_layer(*editLayer, toPlace);
    for (auto& pit : placed)
        pit->obj->commit_update(drawP);

    std::vector<CanvasComponentContainer::ObjInfo*> toErase;
    toErase.reserve(sources.size());
    for (auto& it : sources) toErase.push_back(&(*it));
    drawP.layerMan.erase_component_container(toErase);

    Logger::get().log("USERINFO",
        "Flattened " + std::to_string(sources.size()) + " ink strokes" +
        (downsampled ? " (region large — baked at reduced resolution)." : "."));
}

#else  // !HVYM_HAS_LIBMYPAINT

void flatten_ink_strokes_in_view(DrawingProgram&) {
    Logger::get().log("USERINFO", "Flatten: custom ink isn't available in this build.");
}

#endif

}  // namespace RasterFlatten
