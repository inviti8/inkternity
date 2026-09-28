#include "PsdGroupExport.hpp"
#include "PsdWriter.hpp"

#include "../DrawingProgram/DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../DrawData.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../DrawingProgram/Layers/DrawingProgramLayerListItem.hpp"
#include "../DrawingProgram/Layers/DrawingProgramLayerFolder.hpp"
#include "../DrawingProgram/Layers/SerializedBlendMode.hpp"
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
#include <functional>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace PsdExport {

namespace {
constexpr int MAXDIM = 8192;   // per-axis pixel cap (matches RasterFlatten)

struct LeafData {
    std::vector<CanvasComponentContainer::ObjInfo*> objs;   // drawable components
    std::optional<SCollision::AABB<WorldScalar>> aabb;      // union of their world bounds
};

// Map an Inkternity blend mode to a 4-char PSD blend key. Porter-Duff/compositing
// ops (Src, Dst, In, Out, Xor, …) have no PSD equivalent → Normal.
std::string psd_blend_key(SerializedBlendMode m) {
    switch (m) {
        case SerializedBlendMode::BLEND_MULTIPLY:    return "mul ";
        case SerializedBlendMode::BLEND_MODULATE:    return "mul ";  // closest
        case SerializedBlendMode::BLEND_SCREEN:      return "scrn";
        case SerializedBlendMode::BLEND_OVERLAY:     return "over";
        case SerializedBlendMode::BLEND_DARKEN:      return "dark";
        case SerializedBlendMode::BLEND_LIGHTEN:     return "lite";
        case SerializedBlendMode::BLEND_COLOR_DODGE: return "div ";
        case SerializedBlendMode::BLEND_COLOR_BURN:  return "idiv";
        case SerializedBlendMode::BLEND_HARD_LIGHT:  return "hLit";
        case SerializedBlendMode::BLEND_SOFT_LIGHT:  return "sLit";
        case SerializedBlendMode::BLEND_DIFFERENCE:  return "diff";
        case SerializedBlendMode::BLEND_EXCLUSION:   return "smud";
        case SerializedBlendMode::BLEND_PLUS:        return "lddg";  // linear dodge (add)
        case SerializedBlendMode::BLEND_HUE:         return "hue ";
        case SerializedBlendMode::BLEND_SATURATION:  return "sat ";
        case SerializedBlendMode::BLEND_COLOR:       return "colr";
        case SerializedBlendMode::BLEND_LUMINOSITY:  return "lum ";
        default:                                     return "norm";
    }
}

// Collect a leaf layer's drawable components (skip functional/mask/bounds-less) and
// their union AABB.
LeafData gather_leaf(DrawingProgramLayerListItem& leaf) {
    LeafData ld;
    std::vector<CanvasComponentContainer::ObjInfo*> all;
    leaf.get_flattened_component_list(all);
    for (auto* info : all) {
        CanvasComponentContainer& c = *info->obj;
        const auto type = c.get_comp().get_type();
        if (type == CanvasComponentType::WAYPOINT) continue;
        if (c.get_comp().is_mask()) continue;
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
    sk_sp<SkSurface> surface = drawP.world.main.create_native_surface(resolution, false);
    if (!surface) return false;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SkColor4f{0.0f, 0.0f, 0.0f, 0.0f});

    DrawData dd = drawP.world.drawData;
    dd.cam.c = coords;
    dd.cam.set_viewing_area(resolution.cast<float>());
    dd.refresh_draw_optimizing_values();
    // Draw components directly (no layer alpha/blend — those become PSD layer attrs).
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

    // Pass 1: gather every leaf (for the doc size/scale) and its content.
    std::vector<DrawingProgramLayerListItem*> leaves;
    group.get_flattened_layer_list(leaves);
    if (leaves.empty()) {
        Logger::get().log("USERINFO", "Export PSD: this selection has no layers.");
        return false;
    }
    std::unordered_map<DrawingProgramLayerListItem*, LeafData> leafMap;
    leafMap.reserve(leaves.size());
    std::optional<SCollision::AABB<WorldScalar>> groupAABB;
    std::optional<WorldScalar> finestInv;
    for (auto* leaf : leaves) {
        LeafData ld = gather_leaf(*leaf);
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
        leafMap.emplace(leaf, std::move(ld));
    }
    if (!groupAABB) {
        Logger::get().log("USERINFO", "Export PSD: nothing drawable to export in this selection.");
        return false;
    }

    // Document scale + size.
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

    // Render one leaf into a doc-grid-aligned Image record.
    auto make_image_record = [&](DrawingProgramLayerListItem& leaf) -> Layer {
        Layer PL;
        PL.kind    = LayerKind::Image;
        PL.name    = leaf.get_name();
        PL.opacity = static_cast<uint8_t>(std::clamp(
                         static_cast<int>(std::lround(leaf.get_alpha() * 255.0f)), 0, 255));
        PL.visible = leaf.get_visible();
        PL.blend   = psd_blend_key(leaf.get_blend_mode());

        const LeafData& ld = leafMap[&leaf];
        if (!ld.aabb || ld.objs.empty()) {
            PL.x = PL.y = 0; PL.width = PL.height = 1;   // keep empty layers in the PSD
            PL.rgba.assign(4, 0);
            return PL;
        }
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
            // Fall back to an empty layer rather than aborting the whole export.
            PL.x = PL.y = 0; PL.width = PL.height = 1; PL.rgba.assign(4, 0);
            return PL;
        }
        PL.x = offX; PL.y = offY; PL.width = lw; PL.height = lh;
        PL.rgba = std::move(rgba);
        return PL;
    };

    // Pass 2: emit records BOTTOM-to-TOP. A folder becomes: GroupClose divider, its
    // children (bottom-to-top), then a GroupOpen header (validated ordering). The
    // selected top-level group is unwrapped (its children become the PSD's top level).
    std::vector<Layer> records;
    std::function<void(DrawingProgramLayerListItem&)> emit = [&](DrawingProgramLayerListItem& item) {
        if (item.is_folder()) {
            Layer close; close.kind = LayerKind::GroupClose; close.name = "</Layer group>";
            records.push_back(std::move(close));
            auto& fl = item.get_folder().folderList;
            if (fl)
                for (auto& p : (*fl) | std::views::reverse)   // reverse = bottom-to-top
                    if (p.obj) emit(*p.obj);
            Layer open; open.kind = LayerKind::GroupOpen; open.name = item.get_name();
            open.visible = item.get_visible();
            open.opacity = static_cast<uint8_t>(std::clamp(
                               static_cast<int>(std::lround(item.get_alpha() * 255.0f)), 0, 255));
            open.blend = psd_blend_key(item.get_blend_mode());
            records.push_back(std::move(open));
        } else {
            records.push_back(make_image_record(item));
        }
    };

    if (group.is_folder()) {
        auto& fl = group.get_folder().folderList;
        if (fl)
            for (auto& p : (*fl) | std::views::reverse)
                if (p.obj) emit(*p.obj);
    } else {
        emit(group);   // a single selected layer
    }

    const std::string bytes = build_psd(docW, docH, records);
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
        "Exported PSD: " + std::to_string(leaves.size()) + " layer(s)" +
        (downsampled ? " (large region — reduced resolution)." : "."));
    return true;
}

}  // namespace PsdExport
