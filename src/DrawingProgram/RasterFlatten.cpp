#include "RasterFlatten.hpp"

#include "DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../DrawData.hpp"
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include "../CanvasComponents/ImageCanvasComponent.hpp"
#include "../CanvasComponents/MyPaintLayerCanvasComponent.hpp"
#include "../CanvasComponents/TextBoxCanvasComponent.hpp"
#include "../ResourceManager.hpp"
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

// Per-axis cap on the baked bitmap so an over-zoomed-out flatten can't ask
// for a gigapixel surface. Hit when the in-view region is physically large
// relative to the bake scale; then we coarsen the scale to fit and tell
// the user. Live-tunable (Settings → Debug), persisted in config.json —
// defined outside the HVYM_HAS_LIBMYPAINT guard because GlobalConfig and
// the settings GUI reference it in every build.
int MAXIMUM_FLATTEN_SIZE_PX = 8192;

#ifdef HVYM_HAS_LIBMYPAINT

void flatten_layer(DrawingProgram& drawP) {
    auto& world = drawP.world;
    auto& main = world.main;

    // Flatten the layer currently being edited — same scoping as the brush,
    // eraser, and vectorize tool. Bail if the edit target is unset/a folder.
    auto editLayer = drawP.layerMan.get_editing_layer().lock();
    if (!editLayer || editLayer->is_folder()) {
        Logger::get().log("USERINFO", "Flatten: no active layer to flatten.");
        return;
    }
    // PHASE4 Part A (§6): editing — flatten included — is locked to
    // depth-0 layers. The view AABB below is built from the MAIN camera,
    // but a parallaxed layer renders through its derived camera, so
    // "what's in view" wouldn't match what the artist sees.
    if (drawP.layerMan.editing_layer_is_parallaxed()) {
        Logger::get().log("USERINFO",
            "This layer has parallax depth — set its depth to 0 to flatten it.");
        return;
    }
    auto& components = editLayer->get_layer().components;
    if (!components) return;

    const auto& cam = world.drawData.cam;

    // Collect every visual component on the layer (the whole layer, not just
    // what's on screen — flattening a layer bakes ALL of its artwork into one
    // image). Skip-list rather than allow-list:
    //  - WAYPOINT: functional marker, not artwork — rasterizing the pin
    //    would orphan it from wpGraph.
    //  - IMAGE without its resource on hand (still downloading / display
    //    missing): it currently draws as a gray placeholder; baking that
    //    and erasing the component would silently lose the real image.
    //  - currently-selected components: mid-manipulation, and some types
    //    draw editing overlays (crop shade, handles) that must not bake.
    // Track merged bounds + the finest (smallest inverseScale =
    // highest-res) RASTER source; vector content has no native scale and
    // is covered by the current-view WYSIWYG floor below.
    std::vector<CanvasComponentContainer::ObjInfoIterator> sources;
    std::vector<CanvasComponentContainer::ObjInfoIterator> maskSources;   // PHASE7: clip shapes
    std::optional<SCollision::AABB<WorldScalar>> mergedAABB;
    std::optional<WorldScalar> finestInverseScale;
    bool hasVectorSource = false;
    size_t skippedPendingImages = 0;
    for (auto it = components->begin(); it != components->end(); ++it) {
        auto& container = *it->obj;
        const auto type = container.get_comp().get_type();
        if (type == CanvasComponentType::WAYPOINT) continue;
        const auto wb = container.get_world_bounds();
        if (!wb.has_value()) continue;
        if (drawP.selection.is_selected(&(*it))) continue;
        if (type == CanvasComponentType::IMAGE) {
            auto& img = static_cast<ImageCanvasComponent&>(container.get_comp());
            // Mid-crop: draws the crop-shade overlay, and the edit tool
            // holds a reference — leave it alone (same reason as selected).
            if (img.d.editing) continue;
            const bool stillDownloading = std::any_of(
                drawP.droppedDownloadingFiles.begin(), drawP.droppedDownloadingFiles.end(),
                [&](const auto& df) { return df.comp == &(*it); });
            if (stillDownloading || !world.drawData.rMan->get_display_data(img.d.imageID)) {
                ++skippedPendingImages;
                continue;
            }
        }
        if (type == CanvasComponentType::TEXTBOX &&
            static_cast<TextBoxCanvasComponent&>(container.get_comp()).d.editing)
            continue;  // text being typed right now — don't bake the cursor

        // PHASE7: mask shapes aren't artwork — they define the clip and are
        // consumed by the bake (collected here for the clip + erased after).
        if (container.get_comp().is_mask()) {
            maskSources.push_back(it);
            continue;
        }

        sources.push_back(it);
        if (!mergedAABB) mergedAABB = wb.value();
        else mergedAABB->include_aabb_in_bounds(wb.value());
        // Raster-backed sources pin the bake scale (their pixels are the
        // detail that exists). For IMAGE, coords.inverseScale is the
        // placement scale — a conservative proxy for its pixel density.
        if (type == CanvasComponentType::MYPAINTLAYER || type == CanvasComponentType::IMAGE) {
            const WorldScalar inv = container.coords.inverseScale;
            if (!finestInverseScale || inv < finestInverseScale.value())
                finestInverseScale = inv;
        } else {
            hasVectorSource = true;
        }
    }

    // Normally need >= 2 components to merge; but baking even a single shape is
    // meaningful when there's a mask to collapse into it (PHASE7).
    const bool enoughToBake = sources.size() >= 2 || (!sources.empty() && !maskSources.empty());
    if (!enoughToBake) {
        Logger::get().log("USERINFO",
            "Flatten: need at least 2 components on the layer (found " +
            std::to_string(sources.size()) + ").");
        return;
    }

    // Bake scale: never coarser than the finest raster source AND never
    // coarser than the current zoom (WYSIWYG floor — vectors are
    // resolution-free, so "at least as sharp as what's on screen" is the
    // intuitive guarantee; zoom in before flattening a vector-bearing layer
    // to keep more detail). Coarsened only if the resulting bitmap would blow
    // past the per-axis cap — likely for a layer whose artwork spans a large
    // world area (the "Region large — reduced resolution" note below fires).
    const int maxFlattenDim = std::max(MAXIMUM_FLATTEN_SIZE_PX, 256);
    const WorldVec dim = mergedAABB->dim();
    // Pure-raster flatten keeps the finest-source rule (baking finer than
    // the sources' pixels gains nothing and costs tile memory); the
    // current-zoom floor applies once vector content is in the mix.
    WorldScalar targetInv = finestInverseScale.value_or(cam.c.inverseScale);
    if (hasVectorSource && cam.c.inverseScale < targetInv)
        targetInv = cam.c.inverseScale;
    const WorldScalar minInvX = dim.x().divide_double(static_cast<double>(maxFlattenDim));
    const WorldScalar minInvY = dim.y().divide_double(static_cast<double>(maxFlattenDim));
    bool downsampled = false;
    if (minInvX > targetInv) { targetInv = minInvX; downsampled = true; }
    if (minInvY > targetInv) { targetInv = minInvY; downsampled = true; }

    const CoordSpaceHelper targetCoords(mergedAABB->min, targetInv, 0.0);
    // pos == AABB.min and rotation 0, so to_space(min) == 0 and
    // to_space(max) == the pixel resolution.
    const Vector2f resF = targetCoords.to_space(mergedAABB->max);
    Vector2i resolution{
        std::clamp(static_cast<int>(std::ceil(resF.x())), 1, maxFlattenDim),
        std::clamp(static_cast<int>(std::ceil(resF.y())), 1, maxFlattenDim)};

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
    // PHASE7: clip to the layer's mask shapes so the bake collapses the masking
    // into the raster (the same clip the live compositor applies).
    canvas->save();
    if (!maskSources.empty())
        drawP.drawCache.apply_layer_mask_clip(*editLayer, canvas, dd);
    for (auto& it : sources) {
        auto& container = *it->obj;
        container.draw_with_predraw_data(canvas, dd, container.calculate_predraw_data(dd));
    }
    canvas->restore();

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
    toErase.reserve(sources.size() + maskSources.size());
    for (auto& it : sources) toErase.push_back(&(*it));
    for (auto& it : maskSources) toErase.push_back(&(*it));   // PHASE7: masks consumed by the bake
    drawP.layerMan.erase_component_container(toErase);

    std::string note;
    if (downsampled)
        note += " Region large — baked at reduced resolution.";
    if (skippedPendingImages)
        note += " Skipped " + std::to_string(skippedPendingImages) +
                " still-loading image(s).";
    Logger::get().log("USERINFO",
        "Flattened " + std::to_string(sources.size()) + " components." + note);
}

#else  // !HVYM_HAS_LIBMYPAINT

void flatten_layer(DrawingProgram&) {
    // The merged result is a libmypaint surface (raster-erasable), so
    // flatten as a whole is gated on the ink feature.
    Logger::get().log("USERINFO", "Flatten: custom ink isn't available in this build.");
}

#endif

}  // namespace RasterFlatten
