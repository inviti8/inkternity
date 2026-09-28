#include "PsdGroupExport.hpp"
#include "PsdWriter.hpp"

#include "../DrawingProgram/DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../DrawData.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../DrawingProgram/Layers/DrawingProgramLayerListItem.hpp"
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include "../CanvasComponents/CanvasComponentType.hpp"

#include <Helpers/Logger.hpp>
#include <Helpers/SCollision.hpp>

#include <include/core/SkSurface.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkBitmap.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkColor.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace PsdExport {

namespace {
constexpr int MAXDIM = 8192;   // per-axis pixel cap (matches RasterFlatten)

struct LeafData {
    DrawingProgramLayerListItem* layer = nullptr;
    std::vector<CanvasComponentContainer::ObjInfo*> objs;   // drawable components
    std::optional<SCollision::AABB<WorldScalar>> aabb;      // union of their world bounds
};

// Collect a leaf layer's drawable components (skip functional/mask/bounds-less) and
// their union AABB.
LeafData gather_leaf(DrawingProgramLayerListItem* leaf) {
    LeafData ld;
    ld.layer = leaf;
    std::vector<CanvasComponentContainer::ObjInfo*> all;
    leaf->get_flattened_component_list(all);
    for (auto* info : all) {
        CanvasComponentContainer& c = *info->obj;
        const auto type = c.get_comp().get_type();
        if (type == CanvasComponentType::WAYPOINT) continue;   // functional marker, not artwork
        if (c.get_comp().is_mask()) continue;                  // clip shape, not artwork
        const auto wb = c.get_world_bounds();
        if (!wb.has_value()) continue;
        ld.objs.push_back(info);
        if (!ld.aabb) ld.aabb = wb.value();
        else ld.aabb->include_aabb_in_bounds(wb.value());
    }
    return ld;
}

// Render a leaf's components into `resolution` px at `coords` (shared doc scale) and
// read them back as straight-alpha RGBA8.
bool render_leaf(DrawingProgram& drawP, const LeafData& ld, const CoordSpaceHelper& coords,
                 Vector2i resolution, std::vector<uint8_t>& outRgba) {
    auto& main = drawP.world.main;
    sk_sp<SkSurface> surface = main.create_native_surface(resolution, false);
    if (!surface) return false;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SkColor4f{0.0f, 0.0f, 0.0f, 0.0f});

    DrawData dd = drawP.world.drawData;
    dd.cam.c = coords;
    dd.cam.set_viewing_area(resolution.cast<float>());
    dd.refresh_draw_optimizing_values();
    // Draw the components directly (no layer alpha/blend — those go into the PSD layer
    // record). get_flattened_component_list is z-ascending, so this is bottom-to-top.
    for (auto* info : ld.objs) {
        CanvasComponentContainer& c = *info->obj;
        c.draw_with_predraw_data(canvas, dd, c.calculate_predraw_data(dd));
    }

    SkBitmap bmp;
    if (!bmp.tryAllocPixels(SkImageInfo::Make(resolution.x(), resolution.y(),
            kRGBA_8888_SkColorType, kUnpremul_SkAlphaType)))
        return false;
    if (!surface->readPixels(bmp, 0, 0)) return false;

    const int w = resolution.x(), h = resolution.y();
    outRgba.resize(static_cast<size_t>(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(bmp.getPixels());
    const size_t rb = bmp.rowBytes();
    for (int y = 0; y < h; ++y)
        std::memcpy(&outRgba[static_cast<size_t>(y) * w * 4], src + static_cast<size_t>(y) * rb,
                    static_cast<size_t>(w) * 4);
    return true;
}
}  // namespace

bool export_group(DrawingProgram& drawP, DrawingProgramLayerListItem& group,
                  const std::filesystem::path& path) {
    auto& world = drawP.world;

    // Leaf layers, top-to-bottom (folderList[0] is the top layer).
    std::vector<DrawingProgramLayerListItem*> leaves;
    group.get_flattened_layer_list(leaves);
    if (leaves.empty()) {
        Logger::get().log("USERINFO", "Export PSD: this selection has no layers.");
        return false;
    }

    // Gather content + the group AABB and the finest raster scale (sets doc resolution).
    std::vector<LeafData> leafData;
    leafData.reserve(leaves.size());
    std::optional<SCollision::AABB<WorldScalar>> groupAABB;
    std::optional<WorldScalar> finestInv;
    for (auto* leaf : leaves) {
        LeafData ld = gather_leaf(leaf);
        if (ld.aabb) {
            if (!groupAABB) groupAABB = ld.aabb.value();
            else groupAABB->include_aabb_in_bounds(ld.aabb.value());
            for (auto* info : ld.objs) {
                const auto type = info->obj->get_comp().get_type();
                if (type == CanvasComponentType::MYPAINTLAYER || type == CanvasComponentType::IMAGE) {
                    const WorldScalar inv = info->obj->coords.inverseScale;
                    if (!finestInv || inv < finestInv.value()) finestInv = inv;
                }
            }
        }
        leafData.push_back(std::move(ld));
    }
    if (!groupAABB) {
        Logger::get().log("USERINFO", "Export PSD: nothing drawable to export in this selection.");
        return false;
    }

    // Document scale: finest raster source, coarsened only if the doc would exceed the
    // per-axis cap.
    WorldScalar targetInv = finestInv.value_or(world.drawData.cam.c.inverseScale);
    const WorldVec dim = groupAABB->dim();
    bool downsampled = false;
    const WorldScalar minInvX = dim.x().divide_double(static_cast<double>(MAXDIM));
    const WorldScalar minInvY = dim.y().divide_double(static_cast<double>(MAXDIM));
    if (minInvX > targetInv) { targetInv = minInvX; downsampled = true; }
    if (minInvY > targetInv) { targetInv = minInvY; downsampled = true; }

    const CoordSpaceHelper docCoords(groupAABB->min, targetInv, 0.0);
    const Vector2f docResF = docCoords.to_space(groupAABB->max);
    const int docW = std::clamp(static_cast<int>(std::ceil(docResF.x())), 1, MAXDIM);
    const int docH = std::clamp(static_cast<int>(std::ceil(docResF.y())), 1, MAXDIM);

    // Render each leaf into its own doc-grid-aligned rect (keeps per-layer bitmaps
    // small while all layers align to a shared canvas).
    std::vector<Layer> topToBottom;
    topToBottom.reserve(leafData.size());
    for (auto& ld : leafData) {
        Layer PL;
        PL.name    = ld.layer->get_name();
        PL.opacity = static_cast<uint8_t>(std::clamp(
                         static_cast<int>(std::lround(ld.layer->get_alpha() * 255.0f)), 0, 255));
        PL.visible = ld.layer->get_visible();

        if (!ld.aabb || ld.objs.empty()) {
            PL.x = PL.y = 0; PL.width = PL.height = 1;   // keep empty layers in the PSD
            PL.rgba.assign(4, 0);
            topToBottom.push_back(std::move(PL));
            continue;
        }
        // Integer offset on the doc pixel grid; snap the layer's render origin to it so
        // the layer's pixels line up exactly with the document.
        const Vector2f offF = docCoords.to_space(ld.aabb->min);
        const int offX = std::clamp(static_cast<int>(std::floor(offF.x())), 0, std::max(0, docW - 1));
        const int offY = std::clamp(static_cast<int>(std::floor(offF.y())), 0, std::max(0, docH - 1));
        const WorldVec originWorld = docCoords.from_space(Vector2f(static_cast<float>(offX),
                                                                   static_cast<float>(offY)));
        const CoordSpaceHelper layerCoords(originWorld, targetInv, 0.0);
        const Vector2f extF = layerCoords.to_space(ld.aabb->max);
        const int lw = std::clamp(static_cast<int>(std::ceil(extF.x())), 1, docW - offX);
        const int lh = std::clamp(static_cast<int>(std::ceil(extF.y())), 1, docH - offY);

        std::vector<uint8_t> rgba;
        if (!render_leaf(drawP, ld, layerCoords, Vector2i(lw, lh), rgba)) {
            Logger::get().log("WORLDFATAL", "Export PSD: could not render a layer.");
            return false;
        }
        PL.x = offX; PL.y = offY; PL.width = lw; PL.height = lh;
        PL.rgba = std::move(rgba);
        topToBottom.push_back(std::move(PL));
    }

    // PSD stores layers bottom-to-top.
    std::vector<Layer> psdLayers(std::make_move_iterator(topToBottom.rbegin()),
                                 std::make_move_iterator(topToBottom.rend()));
    const std::string bytes = build_psd(docW, docH, psdLayers);
    if (bytes.empty()) {
        Logger::get().log("WORLDFATAL", "Export PSD: encoding failed.");
        return false;
    }

    // Normalize extension (SDL save dialogs don't always append it) and write.
    std::filesystem::path outPath = path;
    std::string ext = outPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".psd") outPath += ".psd";

    if (!SDL_SaveFile(outPath.string().c_str(), bytes.data(), bytes.size())) {
        Logger::get().log("WORLDFATAL", std::string("Export PSD: could not write file: ") + SDL_GetError());
        return false;
    }
    Logger::get().log("USERINFO",
        "Exported PSD: " + std::to_string(psdLayers.size()) + " layer(s)" +
        (downsampled ? " (large region — reduced resolution)." : "."));
    return true;
}

}  // namespace PsdExport
