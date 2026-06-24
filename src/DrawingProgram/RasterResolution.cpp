#include "RasterResolution.hpp"

#include "DrawingProgram.hpp"
#include "../World.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include "../CanvasComponents/MyPaintLayerCanvasComponent.hpp"
#include "Layers/DrawingProgramLayerManager.hpp"
#include "Layers/DrawingProgramLayerListItem.hpp"

#include <Helpers/Logger.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace RasterResolution {

#ifdef HVYM_HAS_LIBMYPAINT

namespace {

// One tile = kTilePx^2 px * 4 channels * 2 bytes (16bpc) — uncompressed, the
// in-RAM cost. The save stream zstd's this, so on-disk is smaller, but the
// in-RAM figure is what the artist is fighting (and what hit the >2 GiB ceiling).
constexpr double kBytesPerTile =
    static_cast<double>(HVYM::Brushes::LibMyPaintSkiaSurface::kTilePx) *
    HVYM::Brushes::LibMyPaintSkiaSurface::kTilePx * 4.0 * 2.0;

std::string approx_mb(size_t tiles) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", tiles * kBytesPerTile / (1024.0 * 1024.0));
    return buf;
}

}  // namespace

void halve_layer(DrawingProgram& drawP) {
    auto& world = drawP.world;

    // Same scoping as Flatten: operate on the layer currently being edited.
    auto editLayer = drawP.layerMan.get_editing_layer().lock();
    if (!editLayer || editLayer->is_folder()) {
        Logger::get().log("USERINFO", "Reduce Resolution: no active layer.");
        return;
    }
    auto& components = editLayer->get_layer().components;
    if (!components) return;

    // Build a half-res replacement for each reducible MyPaint raster component.
    // Replace-not-mutate so we reuse Flatten's place-undo + erase-undo machinery
    // (one undo step, NetObj-synced). Unlike Flatten this is camera-independent
    // (pure tile math), so it does NOT bail on parallaxed layers.
    std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> toPlace;
    std::vector<CanvasComponentContainer::ObjInfo*> toErase;
    size_t tilesBefore = 0;
    size_t tilesAfter = 0;

    for (auto it = components->begin(); it != components->end(); ++it) {
        auto& container = *it->obj;
        if (container.get_comp().get_type() != CanvasComponentType::MYPAINTLAYER) continue;
        if (drawP.selection.is_selected(&(*it))) continue;  // mid-manipulation

        auto& srcLayer = static_cast<MyPaintLayerCanvasComponent&>(container.get_comp());
        const size_t srcTiles = srcLayer.surface().allocated_tile_count();
        if (srcTiles < 2) continue;  // already minimal — nothing meaningful to shrink

        auto* reduced = new CanvasComponentContainer(world.netObjMan, CanvasComponentType::MYPAINTLAYER);
        // Same placement; double the world-units-per-pixel so the half-size
        // pixel grid covers the identical on-canvas area.
        reduced->coords = container.coords;
        reduced->coords.inverseScale = container.coords.inverseScale * WorldScalar(2.0);

        auto& dstLayer = static_cast<MyPaintLayerCanvasComponent&>(reduced->get_comp());
        dstLayer.surface().build_halved_from(srcLayer.surface());
        dstLayer.mark_dirty();

        tilesBefore += srcTiles;
        tilesAfter += dstLayer.surface().allocated_tile_count();

        toPlace.emplace_back(it, reduced);   // place at the source's z position
        toErase.push_back(&(*it));
    }

    if (toPlace.empty()) {
        Logger::get().log("USERINFO",
            "Reduce Resolution: no raster layers to reduce (flatten the layer first).");
        return;
    }

    const auto placed = drawP.layerMan.add_many_components_to_specific_layer(*editLayer, toPlace);
    for (auto& pit : placed)
        pit->obj->commit_update(drawP);
    drawP.layerMan.erase_component_container(toErase);

    Logger::get().log("USERINFO",
        "Reduced " + std::to_string(toPlace.size()) + " raster layer(s) to half resolution: " +
        std::to_string(tilesBefore) + " -> " + std::to_string(tilesAfter) + " tiles (~" +
        approx_mb(tilesBefore) + " -> ~" + approx_mb(tilesAfter) + ").");
}

#else  // !HVYM_HAS_LIBMYPAINT

void halve_layer(DrawingProgram&) {
    Logger::get().log("USERINFO", "Reduce Resolution: custom ink isn't available in this build.");
}

#endif

}  // namespace RasterResolution
