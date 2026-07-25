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
#include "Layers/SerializedBlendMode.hpp"

#include <Helpers/Logger.hpp>

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
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

namespace {

// The bakeable content of one layer for a Merge Down: visual components to
// draw (z-ascending) + mask shapes to clip with.
struct MergeLayerBake {
    std::vector<CanvasComponentContainer::ObjInfoIterator> sources;   // z-ascending
    std::vector<CanvasComponentContainer::ObjInfoIterator> masks;     // clip shapes
};

}  // namespace

bool merge_down_baked(DrawingProgram& drawP,
                      DrawingProgramLayerListItem& upper,
                      DrawingProgramLayerListItem& lower) {
    auto& world = drawP.world;
    auto& main = world.main;
    const auto& cam = world.drawData.cam;

    // Collect both layers' bakeable content, accumulating the merged bounds and
    // finest raster scale across the pair (the bake must resolve the sharpest
    // source on either layer). Mirrors flatten_layer's skip-list, but where
    // flatten leaves un-bakeable items behind on the surviving layer, a merge
    // CONSUMES the layer — so anything that can't be baked (still downloading,
    // mid-edit) would be lost, and we REFUSE rather than silently skip. A
    // lambda (not a free helper) so it keeps merge_down_baked's friend access
    // to DrawingProgram::droppedDownloadingFiles.
    std::optional<SCollision::AABB<WorldScalar>> mergedAABB;
    std::optional<WorldScalar> finestInverseScale;
    bool hasVectorSource = false;
    auto collect = [&](DrawingProgramLayerListItem& layer, const char* label, MergeLayerBake& out) -> bool {
        auto& components = layer.get_layer().components;
        if (!components) return true;
        for (auto it = components->begin(); it != components->end(); ++it) {
            auto& container = *it->obj;
            const auto type = container.get_comp().get_type();
            // Functional marker, not artwork — left in place by the caller's
            // waypoint guard, never reached here (merge refuses on waypoints).
            if (type == CanvasComponentType::WAYPOINT) continue;
            const auto wb = container.get_world_bounds();
            if (!wb.has_value()) continue;
            if (type == CanvasComponentType::IMAGE) {
                auto& img = static_cast<ImageCanvasComponent&>(container.get_comp());
                const bool stillDownloading = std::any_of(
                    drawP.droppedDownloadingFiles.begin(), drawP.droppedDownloadingFiles.end(),
                    [&](const auto& df) { return df.comp == &(*it); });
                if (img.d.editing || stillDownloading ||
                    !world.drawData.rMan->get_display_data(img.d.imageID)) {
                    Logger::get().log("USERINFO",
                        std::string("Merge Down: the ") + label +
                        " layer has an image still loading or being cropped — finish that first.");
                    return false;
                }
            }
            if (type == CanvasComponentType::TEXTBOX &&
                static_cast<TextBoxCanvasComponent&>(container.get_comp()).d.editing) {
                Logger::get().log("USERINFO",
                    std::string("Merge Down: the ") + label +
                    " layer has a text box being edited — finish typing first.");
                return false;
            }

            if (container.get_comp().is_mask()) {
                out.masks.push_back(it);
                continue;
            }

            out.sources.push_back(it);
            if (!mergedAABB) mergedAABB = wb.value();
            else mergedAABB->include_aabb_in_bounds(wb.value());
            if (type == CanvasComponentType::MYPAINTLAYER || type == CanvasComponentType::IMAGE) {
                const WorldScalar inv = container.coords.inverseScale;
                if (!finestInverseScale || inv < finestInverseScale.value())
                    finestInverseScale = inv;
            } else {
                hasVectorSource = true;
            }
        }
        return true;
    };

    MergeLayerBake lowerBake, upperBake;
    if (!collect(lower, "lower", lowerBake)) return false;
    if (!collect(upper, "upper", upperBake)) return false;

    // The point of this path is to preserve the UPPER layer's blend — if it has
    // no paintable content, there's nothing to bake. Refuse rather than
    // needlessly rasterize the lower layer (the artist can just delete the
    // empty blend layer).
    if (upperBake.sources.empty()) {
        Logger::get().log("USERINFO",
            "Merge Down: the upper layer has no paintable content to merge (delete it instead).");
        return false;
    }
    if (!mergedAABB) {
        Logger::get().log("USERINFO", "Merge Down: nothing to bake.");
        return false;
    }

    // Bake scale — identical rules to flatten_layer: never coarser than the
    // finest raster source, never coarser than the current zoom once vectors
    // are in the mix (WYSIWYG floor), and coarsened only if the bitmap would
    // exceed the per-axis cap.
    const int maxFlattenDim = std::max(MAXIMUM_FLATTEN_SIZE_PX, 256);
    const WorldVec dim = mergedAABB->dim();
    WorldScalar targetInv = finestInverseScale.value_or(cam.c.inverseScale);
    if (hasVectorSource && cam.c.inverseScale < targetInv)
        targetInv = cam.c.inverseScale;
    const WorldScalar minInvX = dim.x().divide_double(static_cast<double>(maxFlattenDim));
    const WorldScalar minInvY = dim.y().divide_double(static_cast<double>(maxFlattenDim));
    bool downsampled = false;
    if (minInvX > targetInv) { targetInv = minInvX; downsampled = true; }
    if (minInvY > targetInv) { targetInv = minInvY; downsampled = true; }

    const CoordSpaceHelper targetCoords(mergedAABB->min, targetInv, 0.0);
    const Vector2f resF = targetCoords.to_space(mergedAABB->max);
    Vector2i resolution{
        std::clamp(static_cast<int>(std::ceil(resF.x())), 1, maxFlattenDim),
        std::clamp(static_cast<int>(std::ceil(resF.y())), 1, maxFlattenDim)};

    sk_sp<SkSurface> surface = main.create_native_surface(resolution, false);
    if (!surface) {
        Logger::get().log("WORLDFATAL", "Merge Down: could not create a render surface.");
        return false;
    }
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SkColor4f{0.0f, 0.0f, 0.0f, 0.0f});

    DrawData dd = world.drawData;
    dd.cam.c = targetCoords;
    dd.cam.set_viewing_area(resolution.cast<float>());
    dd.refresh_draw_optimizing_values();

    auto draw_layer = [&](DrawingProgramLayerListItem& layer, const MergeLayerBake& bake) {
        canvas->save();
        if (!bake.masks.empty())
            drawP.drawCache.apply_layer_mask_clip(layer, canvas, dd);
        for (auto& it : bake.sources) {
            auto& container = *it->obj;
            container.draw_with_predraw_data(canvas, dd, container.calculate_predraw_data(dd));
        }
        canvas->restore();
    };

    // Lower layer = backdrop. The caller guarantees it's default alpha +
    // Source-Over, so drawing it straight onto the cleared surface reproduces
    // its live compositing over an empty backdrop.
    draw_layer(lower, lowerBake);

    // Upper layer composited with ITS alpha + blend mode — byte-for-byte the
    // saveLayer the live renderer uses (DrawingProgramLayerListItem::draw).
    SkPaint upperPaint;
    upperPaint.setAlphaf(upper.get_alpha());
    upperPaint.setBlendMode(serialized_blend_mode_to_sk_blend_mode(upper.get_blend_mode()));
    canvas->saveLayer(nullptr, &upperPaint);
    draw_layer(upper, upperBake);
    canvas->restore();

    // Read back as 8bpc UNPREMULTIPLIED RGBA (what import_from_bitmap expects).
    SkBitmap baked;
    if (!baked.tryAllocPixels(SkImageInfo::Make(
            resolution.x(), resolution.y(),
            kRGBA_8888_SkColorType, kUnpremul_SkAlphaType))) {
        Logger::get().log("WORLDFATAL", "Merge Down: could not allocate readback bitmap.");
        return false;
    }
    if (!surface->readPixels(baked, 0, 0)) {
        Logger::get().log("WORLDFATAL", "Merge Down: readback (readPixels) failed.");
        return false;
    }

    auto* merged = new CanvasComponentContainer(world.netObjMan, CanvasComponentType::MYPAINTLAYER);
    merged->coords = targetCoords;
    auto& mergedLayer = static_cast<MyPaintLayerCanvasComponent&>(merged->get_comp());
    mergedLayer.surface().import_from_bitmap(baked, 0, 0);
    mergedLayer.mark_dirty();

    // Place the single baked raster on top of the lower layer, then erase every
    // component that went into the bake (from BOTH layers). Place-before-erase
    // reuses the same undo machinery as flatten (place-undo + erase-undo); the
    // caller then removes the now-empty upper layer (its own delete-undo).
    std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> toPlace;
    toPlace.emplace_back(lower.get_layer().components->end(), merged);
    const auto placed = drawP.layerMan.add_many_components_to_specific_layer(lower, toPlace);
    for (auto& pit : placed)
        pit->obj->commit_update(drawP);

    std::vector<CanvasComponentContainer::ObjInfo*> toErase;
    toErase.reserve(lowerBake.sources.size() + lowerBake.masks.size() +
                    upperBake.sources.size() + upperBake.masks.size());
    for (auto& it : lowerBake.sources) toErase.push_back(&(*it));
    for (auto& it : lowerBake.masks)   toErase.push_back(&(*it));
    for (auto& it : upperBake.sources) toErase.push_back(&(*it));
    for (auto& it : upperBake.masks)   toErase.push_back(&(*it));
    drawP.layerMan.erase_component_container(toErase);

    std::string note;
    if (downsampled)
        note = " Region large — baked at reduced resolution.";
    Logger::get().log("USERINFO",
        "Merged '" + upper.get_name() + "' down into '" + lower.get_name() +
        "' as a baked raster (blend mode preserved)." + note);
    return true;
}

#else  // !HVYM_HAS_LIBMYPAINT

void flatten_layer(DrawingProgram&) {
    // The merged result is a libmypaint surface (raster-erasable), so
    // flatten as a whole is gated on the ink feature.
    Logger::get().log("USERINFO", "Flatten: custom ink isn't available in this build.");
}

bool merge_down_baked(DrawingProgram&, DrawingProgramLayerListItem&, DrawingProgramLayerListItem&) {
    // The baked result is a libmypaint surface (raster-erasable), so the
    // blend-aware merge is gated on the ink feature like flatten.
    Logger::get().log("USERINFO",
        "Merge Down: merging a layer with a non-default blend mode bakes to a raster, which needs the custom-ink build.");
    return false;
}

#endif

}  // namespace RasterFlatten
