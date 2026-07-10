#include "DrawingProgramLayerListItem.hpp"
#include "DrawingProgramLayerFolder.hpp"
#include <cereal/types/string.hpp>
#include "DrawingProgramLayer.hpp"
#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../ReaderMode/ReaderMode.hpp"
#include "../../World.hpp"
#include "SerializedBlendMode.hpp"

using namespace NetworkingObjects;

void DrawingProgramLayerListItemUndoData::scale_up(const WorldScalar& scaleUpAmount) {
    // Group anchor is a world coordinate and refScale is a world-per-screen
    // scale — both rescale with the canvas; depth is dimensionless.
    metaInfo.parallaxAnchorX = metaInfo.parallaxAnchorX * scaleUpAmount;
    metaInfo.parallaxAnchorY = metaInfo.parallaxAnchorY * scaleUpAmount;
    metaInfo.parallaxRefScale = metaInfo.parallaxRefScale * scaleUpAmount;
    if(folderData) {
        for(auto& l : folderData.value())
            l.scale_up(scaleUpAmount);
    }
    else {
        for(auto& c : layerData.value())
            c.copyData->scale_up(scaleUpAmount);
    }
}

DrawingProgramLayerListItem::DrawingProgramLayerListItem() {}

DrawingProgramLayerListItem::DrawingProgramLayerListItem(NetworkingObjects::NetObjManager& netObjMan, const std::string& initName, bool isFolder, LayerKind initKind) {
    if(isFolder) {
        folderData = std::make_unique<DrawingProgramLayerFolder>();
        folderData->folderList = netObjMan.make_obj<NetworkingObjects::NetObjOrderedList<DrawingProgramLayerListItem>>();
    }
    else {
        layerData = std::make_unique<DrawingProgramLayer>();
        layerData->components = netObjMan.make_obj<CanvasComponentContainer::NetList>();
    }
    nameData = netObjMan.make_obj<NameData>();
    nameData->name = initName;
    displayData = netObjMan.make_obj<DisplayData>();
    kind = initKind;
}

DrawingProgramLayerListItem::DrawingProgramLayerListItem(World& w, const DrawingProgramLayerListItemUndoData& initData) {
    if(initData.folderData) {
        auto& initFolderList = initData.folderData.value();
        folderData = std::make_unique<DrawingProgramLayerFolder>();
        folderData->folderList = w.netObjMan.make_obj<NetworkingObjects::NetObjOrderedList<DrawingProgramLayerListItem>>();
        w.undo.register_new_netid_to_existing_undoid(initData.containerUndoID, folderData->folderList.get_net_id());
        for(auto& initFolder : initFolderList) {
            auto it = folderData->folderList->emplace_back_direct(folderData->folderList, w, initFolder);
            w.undo.register_new_netid_to_existing_undoid(initFolder.undoID, it->obj.get_net_id());
        }
    }
    else {
        auto& initComponentList = initData.layerData.value();
        layerData = std::make_unique<DrawingProgramLayer>();
        layerData->components = w.netObjMan.make_obj<CanvasComponentContainer::NetList>();
        w.undo.register_new_netid_to_existing_undoid(initData.containerUndoID, layerData->components.get_net_id());
        for(auto& initComponent : initComponentList) {
            auto it = layerData->components->emplace_back_direct(layerData->components, w.netObjMan, *initComponent.copyData);
            w.undo.register_new_netid_to_existing_undoid(initComponent.undoID, it->obj.get_net_id());
        }
    }
    nameData = w.netObjMan.make_obj<NameData>();
    nameData->name = initData.metaInfo.name;
    displayData = w.netObjMan.make_obj<DisplayData>();
    displayData->alpha = initData.metaInfo.alpha;
    displayData->blendMode = initData.metaInfo.blendMode;
}

DrawingProgramLayerListItemUndoData DrawingProgramLayerListItem::get_undo_data(WorldUndoManager& u) const {
    DrawingProgramLayerListItemUndoData toRet;
    toRet.metaInfo.name = nameData->name;
    toRet.metaInfo.blendMode = displayData->blendMode;
    toRet.metaInfo.alpha = displayData->alpha;
    if(folderData) {
        toRet.containerUndoID = u.get_undoid_from_netid(folderData->folderList.get_net_id());
        toRet.folderData = std::vector<DrawingProgramLayerListItemUndoData>();
        auto& folderListToRet = toRet.folderData.value();
        for(auto& f : *folderData->folderList) {
            folderListToRet.emplace_back(f.obj->get_undo_data(u));
            folderListToRet.back().undoID = u.get_undoid_from_netid(f.obj.get_net_id());
        }
    }
    else {
        toRet.containerUndoID = u.get_undoid_from_netid(layerData->components.get_net_id());
        toRet.layerData = std::vector<DrawingProgramComponentUndoData>();
        auto& layerListToRet = toRet.layerData.value();
        for(auto& c : *layerData->components)
            layerListToRet.emplace_back(u.get_undoid_from_netid(c.obj.get_net_id()), c.obj->get_data_copy());
    }
    return toRet;
}

void DrawingProgramLayerListItem::reassign_netobj_ids_call() {
    if(folderData)
        folderData->folderList.reassign_ids();
    else
        layerData->components.reassign_ids();
    nameData.reassign_ids();
    displayData.reassign_ids();
    if(motionPath)
        motionPath.reassign_ids();
}

void DrawingProgramLayerListItem::set_to_erase() {
    if(folderData)
        folderData->set_to_erase();
    else
        layerData->set_to_erase();
}

void DrawingProgramLayerListItem::set_component_list_callbacks(DrawingProgramLayerManager &layerMan) {
    if(folderData)
        folderData->set_component_list_callbacks(layerMan);
    else
        layerData->set_component_list_callbacks(*this, layerMan);
}

void DrawingProgramLayerListItem::get_flattened_component_list(std::vector<CanvasComponentContainer::ObjInfo*>& objList) const {
    if(folderData)
        folderData->get_flattened_component_list(objList);
    else
        layerData->get_flattened_component_list(objList);
}

void DrawingProgramLayerListItem::get_flattened_layer_list(std::vector<DrawingProgramLayerListItem*>& objList) {
    if(folderData) {
        for(auto& c : *folderData->folderList)
            c.obj->get_flattened_layer_list(objList);
    }
    else
        objList.emplace_back(this);
}

uint32_t DrawingProgramLayerListItem::get_component_count() const {
    if(folderData) {
        uint32_t toRet = 0;
        for(auto& c : *folderData->folderList)
            toRet += c.obj->get_component_count();
        return toRet;
    }
    else
        return layerData->components->size();
}

uint64_t DrawingProgramLayerListItem::get_total_memory_bytes() const {
    uint64_t toRet = 0;
    if(folderData) {
        for(auto& c : *folderData->folderList)
            toRet += c.obj->get_total_memory_bytes();
    }
    else if(layerData->components) {
        for(auto& p : *layerData->components)
            toRet += p.obj->get_comp().get_memory_size_bytes();
    }
    return toRet;
}

void DrawingProgramLayerListItem::scale_up(const WorldScalar& scaleUpAmount) {
    // World rescale moves every world coordinate — the group anchor and its
    // reference scale included. Depth is a dimensionless ratio and stays put.
    if(displayData) {
        displayData->parallaxAnchorX = displayData->parallaxAnchorX * scaleUpAmount;
        displayData->parallaxAnchorY = displayData->parallaxAnchorY * scaleUpAmount;
        displayData->parallaxRefScale = displayData->parallaxRefScale * scaleUpAmount;
    }
    if(folderData)
        folderData->scale_up(scaleUpAmount);
    else
        layerData->scale_up(scaleUpAmount);
}

bool DrawingProgramLayerListItem::is_folder() const {
    return folderData != nullptr;
}

DrawingProgramLayerFolder& DrawingProgramLayerListItem::get_folder() const {
    return *folderData;
}

DrawingProgramLayer& DrawingProgramLayerListItem::get_layer() const {
    return *layerData;
}

void DrawingProgramLayerListItem::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version, DrawingProgramLayerManager& layerMan) {
    nameData = layerMan.drawP.world.netObjMan.make_obj<NameData>();
    a(*nameData);

    displayData = layerMan.drawP.world.netObjMan.make_obj<DisplayData>();
    if(version >= VersionNumber(0, 27, 0))
        a(*displayData);  // Current layout: parallax + flip-book fields
    else if(version >= VersionNumber(0, 20, 0)) {
        // PARALLAX-SCENES layout (INFPNT000021-27, 0.20-0.26): parallax fields
        // present, but no flip-book fields. Read the seven explicitly to keep
        // the stream aligned; flip-book fields keep their defaults (fps 0 =
        // not a flip-book group).
        a(displayData->alpha, displayData->visible, displayData->blendMode,
          displayData->parallaxDepth, displayData->parallaxAnchorX, displayData->parallaxAnchorY,
          displayData->parallaxRefScale);
    }
    else if(version >= VersionNumber(0, 12, 0)) {
        // INFPNT000013-20: parallax depth + per-layer anchor present, but no
        // refScale. Consume those six fields to keep the stream aligned;
        // refScale stays 0 → parallax inactive. Per the no-back-compat
        // decision (zynx, 2026-06-22) the old per-layer anchor is NOT
        // migrated to a group — depth survives but renders flat until the
        // layer is put in a parallax group.
        a(displayData->alpha, displayData->visible, displayData->blendMode,
          displayData->parallaxDepth, displayData->parallaxAnchorX, displayData->parallaxAnchorY);
    }
    else {
        // Pre-INFPNT000013 files stored only these three fields (no
        // parallax). Reading them individually replicates the old
        // struct's byte layout exactly; parallax stays at zero defaults.
        a(displayData->alpha, displayData->visible, displayData->blendMode);
    }

    if(version >= VersionNumber(0, 8, 0)) {
        uint8_t kindByte;
        a(kindByte);
        kind = static_cast<LayerKind>(kindByte);
    }
    // Pre-0.8 layers stay LayerKind::DEFAULT (the ctor default).

    // MOTION-PATH.md (INFPNT000029 / 0.28.0): optional animation path. Older
    // files have none → motionPath stays null.
    if(version >= VersionNumber(0, 28, 0)) {
        bool hasMotionPath;
        a(hasMotionPath);
        if(hasMotionPath) {
            motionPath = layerMan.drawP.world.netObjMan.make_obj<MotionPath>();
            a(*motionPath);
        }
    }

    bool isFolder;
    a(isFolder);
    if(isFolder) {
        folderData = std::make_unique<DrawingProgramLayerFolder>();
        folderData->load_file(a, version, layerMan);
    }
    else {
        layerData = std::make_unique<DrawingProgramLayer>();
        layerData->load_file(a, version, layerMan);
    }
}

void DrawingProgramLayerListItem::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(*nameData);
    a(*displayData);
    a(static_cast<uint8_t>(kind));
    // MOTION-PATH.md (INFPNT000029 / 0.28.0): optional per-folder animation path.
    bool hasMotionPath = static_cast<bool>(motionPath);
    a(hasMotionPath);
    if(hasMotionPath)
        a(*motionPath);
    a(static_cast<bool>(folderData));
    if(folderData)
        folderData->save_file(a);
    else
        layerData->save_file(a);
}

void DrawingProgramLayerListItem::get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const {
    if(folderData)
        folderData->get_used_resources(resourceSet);
    else
        layerData->get_used_resources(resourceSet);
}

void DrawingProgramLayerListItem::draw(SkCanvas* canvas, const DrawData& drawData) const {
    // Reader mode hides SKETCH-kind layers (raster-only rough scratch
    // surface; not part of the published comic). COLOR + INK + DEFAULT
    // render normally.
    if(kind == LayerKind::SKETCH && drawData.main && drawData.main->world && drawData.main->world->readerMode.is_active())
        return;
    if(displayData->visible) {
        // PARALLAX-SCENES (docs/design/PARALLAX-SCENES.md §3/§5): parallax is
        // group-anchored. A parallax-group FOLDER injects its anchor/scale
        // into the subtree; each depth!=0 LAYER inside renders through a
        // derived camera pulled toward that anchor. Pure world-space
        // translation → affine → works for the SVG path too.
        const DrawData* dd = &drawData;
        std::optional<DrawData> derived;
        if(folderData) {
            if(is_parallax_group()) {
                // Nearest ancestor wins: a nested group fully replaces the
                // context for its own subtree.
                derived = drawData;
                derived->parallaxGroup.active = true;
                derived->parallaxGroup.anchorX = displayData->parallaxAnchorX;
                derived->parallaxGroup.anchorY = displayData->parallaxAnchorY;
                derived->parallaxGroup.refScale = displayData->parallaxRefScale;
                dd = &derived.value();
            }
        }
        else if(displayData->parallaxDepth != 0.0f && drawData.parallaxGroup.active) {
            // derived.pos = anchor + (cam.pos − anchor)·(1 − pull), where
            //   pull = r·(1 − f(d)),  f(d) = 1/(1+d),  r = min(1, invScale/refScale)
            // The min(1,·) clamp freezes the on-screen magnitude once zoomed
            // in past the scene scale; the zoomed-out branch (r==1) reduces to
            // the original parallax law (offset diminishes with distance).
            // Zoom and rotation are deliberately untouched (uniform → no
            // parallax). NB: scale the WorldVec delta with a direct WorldScalar
            // multiply, NOT divide_double/multiply_double — those invert their
            // argument for |a|<1 (wrong scaling), which is exactly our range.
            const double depth = static_cast<double>(displayData->parallaxDepth);
            const double f = 1.0 / (1.0 + depth);
            double r = static_cast<double>(drawData.cam.c.inverseScale / drawData.parallaxGroup.refScale);
            if(!(r < 1.0)) r = 1.0;   // r = min(1, ratio); also pins inf/NaN to 1
            if(r < 0.0) r = 0.0;
            const WorldScalar keep{1.0 - r * (1.0 - f)};
            const WorldScalar& ax = drawData.parallaxGroup.anchorX;
            const WorldScalar& ay = drawData.parallaxGroup.anchorY;
            derived = drawData;
            derived->cam.c.pos = WorldVec{
                ax + (drawData.cam.c.pos.x() - ax) * keep,
                ay + (drawData.cam.c.pos.y() - ay) * keep};
            derived->refresh_draw_optimizing_values();
            dd = &derived.value();
        }

        if(!drawData.isSVGRender) {
            SkPaint layerPaint;
            layerPaint.setAlphaf(displayData->alpha);
            layerPaint.setBlendMode(serialized_blend_mode_to_sk_blend_mode(displayData->blendMode));
            canvas->saveLayer(nullptr, &layerPaint);
        }

        if(folderData) {
            // PHASE10 Feature B — a flip-book group draws one frame at a time
            // (chosen by its runtime frameIndex), not the full composite. Only
            // reachable on the direct-draw path (the cache is bypassed while a
            // flip-book is live — see DrawingProgramLayerManager::any_visible_flipbook_layer).
            if(is_flipbook_group() && folderData->frame_count() > 1) {
                // MOTION-PATH.md P3 — if a motion path exists, the whole group
                // travels it (translate + scale) as it plays. At progress 0 the
                // sample is the start node + scale from node 0, so the transform
                // is identity (no offset) — static/editing looks unchanged.
                MotionPath* mp = get_motion_path();
                if(mp && mp->points.size() >= 2) {
                    const MotionPath::Sample smp = mp->sample(mp->pathProgress);
                    const Vector2f startScr = dd->cam.c.to_space(mp->coords.from_space(mp->points[0]));
                    const Vector2f curScr   = dd->cam.c.to_space(mp->coords.from_space(smp.pos));
                    const Vector2f delta = curScr - startScr;
                    canvas->save();
                    // Scale about the current sample position, then shift the frame
                    // so its origin (node 0) follows the sampled point.
                    canvas->translate(curScr.x(), curScr.y());
                    canvas->scale(smp.scale, smp.scale);
                    canvas->translate(-curScr.x(), -curScr.y());
                    canvas->translate(delta.x(), delta.y());
                    folderData->draw_flipbook_frame(canvas, *dd);
                    canvas->restore();
                }
                else
                    folderData->draw_flipbook_frame(canvas, *dd);
            }
            else
                folderData->draw(canvas, *dd);
        }
        else {
            // PHASE7: this is the direct draw path (screenshots, SVG export, and
            // the parallax bypass) — apply the layer's mask clip here too, not
            // just in the cached compositor. Own save/restore so it's scoped even
            // on the SVG path (which skips the saveLayer above).
            canvas->save();
            if(drawData.main && drawData.main->world)
                drawData.main->world->drawProg.drawCache.apply_layer_mask_clip(*this, canvas, *dd);
            layerData->draw(canvas, *dd);
            canvas->restore();
        }

        if(!drawData.isSVGRender)
            canvas->restore();
    }
}

void DrawingProgramLayerListItem::set_name(NetworkingObjects::DelayUpdateSerializedClassManager& delayedNetObjMan, const std::string& newName) const {
    if(nameData && nameData->name != newName) {
        nameData->name = newName;
        delayedNetObjMan.send_update_to_all<NameData>(nameData, false);
    }
}

const std::string& DrawingProgramLayerListItem::get_name() const {
    return nameData->name;
}

void DrawingProgramLayerListItem::set_alpha(DrawingProgramLayerManager& layerMan, float newAlpha) const {
    if(displayData && displayData->alpha != newAlpha) {
        displayData->alpha = newAlpha;
        // PERF: an alpha change only affects this layer's own footprint —
        // invalidate just that, not the whole cache (see set_visible).
        layerMan.drawP.drawCache.invalidate_layer_footprint(this);
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

float DrawingProgramLayerListItem::get_alpha() const {
    return displayData->alpha;
}

void DrawingProgramLayerListItem::set_visible(DrawingProgramLayerManager& layerMan, bool newVisible) const {
    if(displayData && displayData->visible != newVisible) {
        displayData->visible = newVisible;
        // PERF: a visibility / alpha / blend-mode change only affects the screen
        // region this layer's own content covers, so invalidate just that
        // footprint instead of clear_own_cached_surfaces() (which threw away
        // EVERY cached node surface, forcing a full-canvas re-rasterize on each
        // toggle — and once per frame while dragging the alpha slider).
        layerMan.drawP.drawCache.invalidate_layer_footprint(this);
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

bool DrawingProgramLayerListItem::get_visible() const {
    return displayData->visible;
}

void DrawingProgramLayerListItem::set_blend_mode(DrawingProgramLayerManager& layerMan, SerializedBlendMode newBlendMode) const {
    if(displayData && displayData->blendMode != newBlendMode) {
        displayData->blendMode = newBlendMode;
        // PERF: a blend-mode change only affects this layer's own footprint —
        // invalidate just that, not the whole cache (see set_visible).
        layerMan.drawP.drawCache.invalidate_layer_footprint(this);
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

SerializedBlendMode DrawingProgramLayerListItem::get_blend_mode() const {
    return displayData->blendMode;
}

void DrawingProgramLayerListItem::set_parallax_depth(DrawingProgramLayerManager& layerMan, float newDepth) const {
    if(!displayData) return;
    newDepth = std::clamp(newDepth, -0.95f, 1000.0f);
    // PARALLAX-SCENES: depth is purely this layer's plane within its group;
    // the anchor/scale live on the group folder, so there's no per-layer
    // no-jump recompute. At the group's neutral viewpoint (cam.pos ==
    // anchor) a depth change is jump-free regardless.
    set_parallax_raw(layerMan, newDepth, displayData->parallaxRefScale, get_parallax_anchor());
}

void DrawingProgramLayerListItem::set_parallax_group(DrawingProgramLayerManager& layerMan, const WorldScalar& refScale, const WorldVec& anchor) const {
    if(!displayData) return;
    // Capturing at the current camera (refScale = inverseScale, anchor =
    // cam.pos) makes cam.pos == anchor at that instant → every child renders
    // at offset 0 → no jump. refScale == 0 disables the group.
    set_parallax_raw(layerMan, displayData->parallaxDepth, refScale, anchor);
}

void DrawingProgramLayerListItem::set_parallax_raw(DrawingProgramLayerManager& layerMan, float depth, const WorldScalar& refScale, const WorldVec& anchor) const {
    if(displayData && (displayData->parallaxDepth != depth ||
                       displayData->parallaxRefScale != refScale ||
                       displayData->parallaxAnchorX != anchor.x() ||
                       displayData->parallaxAnchorY != anchor.y())) {
        displayData->parallaxDepth = depth;
        displayData->parallaxRefScale = refScale;
        displayData->parallaxAnchorX = anchor.x();
        displayData->parallaxAnchorY = anchor.y();
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

float DrawingProgramLayerListItem::get_parallax_depth() const {
    return displayData->parallaxDepth;
}

WorldScalar DrawingProgramLayerListItem::get_parallax_ref_scale() const {
    return displayData->parallaxRefScale;
}

WorldVec DrawingProgramLayerListItem::get_parallax_anchor() const {
    return WorldVec{displayData->parallaxAnchorX, displayData->parallaxAnchorY};
}

bool DrawingProgramLayerListItem::is_parallax_group() const {
    return displayData && displayData->parallaxRefScale != WorldScalar{0};
}

bool DrawingProgramLayerListItem::has_active_parallax_descendant(bool ancestorGroupActive) const {
    // A hidden subtree doesn't draw, so it never needs the cache bypass.
    if(!displayData || !displayData->visible) return false;
    if(folderData) {
        const bool groupActive = ancestorGroupActive || is_parallax_group();
        for(auto& c : *folderData->folderList)
            if(c.obj->has_active_parallax_descendant(groupActive))
                return true;
        return false;
    }
    return ancestorGroupActive && displayData->parallaxDepth != 0.0f;
}

bool DrawingProgramLayerListItem::target_in_active_parallax_group(NetworkingObjects::NetObjID targetId, bool ancestorGroupActive) const {
    if(!folderData) return false;
    const bool groupActive = ancestorGroupActive || is_parallax_group();
    for(auto& c : *folderData->folderList) {
        if(c.obj.get_net_id() == targetId)
            return groupActive;
        if(c.obj->target_in_active_parallax_group(targetId, groupActive))
            return true;
    }
    return false;
}

void DrawingProgramLayerListItem::set_flipbook_fps(DrawingProgramLayerManager& layerMan, float fps) const {
    if(!displayData) return;
    if(fps < 0.0f) fps = 0.0f;
    if(displayData->flipbookFps == fps) return;
    // Crossing the fps 0 <-> >0 boundary flips the draw path (composite ALL
    // children vs. show ONE frame), so the cached composite must be rebuilt. A
    // pure rate change draws identically in the static/edit view, so it must
    // NOT clear — clearing on every slider tick is exactly the stall the
    // incremental-BVH work removed.
    const bool enabledChanged = (displayData->flipbookFps > 0.0f) != (fps > 0.0f);
    displayData->flipbookFps = fps;
    if(enabledChanged)
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
    layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
}

void DrawingProgramLayerListItem::set_flipbook_play_style(DrawingProgramLayerManager& layerMan, FlipbookPlayStyle style) const {
    // Play style / trigger / invert only affect PLAYBACK (viewer mode or the
    // preview override), never the static edit-mode pixels, so no cache clear.
    if(!displayData || displayData->flipbookPlayStyle == style) return;
    displayData->flipbookPlayStyle = style;
    layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
}

void DrawingProgramLayerListItem::set_flipbook_trigger_mode(DrawingProgramLayerManager& layerMan, FlipbookTriggerMode trigger) const {
    if(!displayData || displayData->flipbookTriggerMode == trigger) return;
    displayData->flipbookTriggerMode = trigger;
    layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
}

void DrawingProgramLayerListItem::set_flipbook_invert(DrawingProgramLayerManager& layerMan, bool invert) const {
    if(!displayData || displayData->flipbookInvert == invert) return;
    displayData->flipbookInvert = invert;
    layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
}

float DrawingProgramLayerListItem::get_flipbook_fps() const {
    return displayData ? displayData->flipbookFps : 0.0f;
}

FlipbookPlayStyle DrawingProgramLayerListItem::get_flipbook_play_style() const {
    return displayData ? displayData->flipbookPlayStyle : FlipbookPlayStyle::ONCE;
}

FlipbookTriggerMode DrawingProgramLayerListItem::get_flipbook_trigger_mode() const {
    return displayData ? displayData->flipbookTriggerMode : FlipbookTriggerMode::AUTO;
}

bool DrawingProgramLayerListItem::get_flipbook_invert() const {
    return displayData && displayData->flipbookInvert;
}

bool DrawingProgramLayerListItem::is_flipbook_group() const {
    // Only meaningful on a folder; callers gate on is_folder()/folderData. The
    // fps > 0 sentinel doubles as the enable flag (like parallaxRefScale != 0).
    return displayData && displayData->flipbookFps > 0.0f;
}

bool DrawingProgramLayerListItem::has_active_flipbook_descendant() const {
    // A hidden subtree doesn't draw, so it never needs the cache bypass. A
    // flip-book with 0 or 1 frames can't animate either.
    if(!displayData || !displayData->visible) return false;
    if(!folderData) return false;
    if(is_flipbook_group() && folderData->folderList->size() > 1)
        return true;
    for(auto& c : *folderData->folderList)
        if(c.obj->has_active_flipbook_descendant())
            return true;
    return false;
}

// True if any component under this item has cached world bounds intersecting
// the generous view collider — i.e. the group's content is on screen. Uses the
// cached worldAABB (no recompute), so it's O(components) with no allocation
// beyond the flattened list.
static bool flipbook_content_on_screen(const DrawingProgramLayerListItem& item, const DrawData& drawData) {
    std::vector<CanvasComponentContainer::ObjInfo*> comps;
    item.get_flattened_component_list(comps);
    for(auto* c : comps) {
        const auto& wb = c->obj->get_world_bounds();
        if(wb.has_value() && SCollision::collide(wb.value(), drawData.cam.viewingAreaGenerousCollider))
            return true;
    }
    return false;
}

void DrawingProgramLayerListItem::update_flipbook_playback(float deltaTime, bool viewerActive, const DrawData& drawData, const DrawingProgramLayerListItem* editingLayer) {
    if(!folderData) return;
    if(is_flipbook_group() && folderData->frame_count() > 1) {
        auto& rt = folderData->flipbookRuntime;
        const bool invert = get_flipbook_invert();
        const bool playContext = viewerActive || rt.previewPlaying;
        // MOTION-PATH.md P3 — the path clock rides alongside the frame clock:
        // advance whenever frames advance, reset when the group (re)starts or
        // sits static. Its own playStyle governs how pathProgress wraps.
        MotionPath* mp = get_motion_path();
        if(!playContext) {
            // Drawing mode, no preview: static. Hold the "edit frame" — the
            // selected direct child, so the artist edits the frame they picked
            // in the layer panel. Reset the clock + AUTO rising-edge memory so
            // entering viewer mode / re-entering view re-triggers cleanly.
            rt.playing = false;
            rt.frameTimer = 0.0;
            rt.pingPongReversing = false;
            rt.onScreenLast = false;
            if(mp) mp->reset();   // sit at the path start (identity transform)
            if(editingLayer) {
                size_t p = 0;
                bool found = false;
                for(auto& c : *folderData->folderList) {
                    if(&(*c.obj) == editingLayer) { found = true; break; }
                    ++p;
                }
                if(found)
                    rt.frameIndex = p;
            }
        }
        else if(rt.previewPlaying) {
            // Drawing-mode debug preview ignores the trigger axis — just play.
            folderData->flipbook_advance(get_flipbook_play_style(), invert, get_flipbook_fps(), deltaTime);
            if(mp) mp->advance(deltaTime);
        }
        else if(get_flipbook_trigger_mode() == FlipbookTriggerMode::AUTO) {
            const bool onScreen = flipbook_content_on_screen(*this, drawData);
            if(onScreen && !rt.onScreenLast) {
                folderData->flipbook_begin(invert);   // rising edge (re)starts
                if(mp) mp->reset();
            }
            rt.onScreenLast = onScreen;
            if(onScreen) {
                folderData->flipbook_advance(get_flipbook_play_style(), invert, get_flipbook_fps(), deltaTime);
                if(mp) mp->advance(deltaTime);
            }
        }
        else {
            // ON_TOUCH: playing is set by trigger_touch_flipbook; runs per style.
            folderData->flipbook_advance(get_flipbook_play_style(), invert, get_flipbook_fps(), deltaTime);
            if(mp) mp->advance(deltaTime);
        }
    }
    for(auto& c : *folderData->folderList)
        c.obj->update_flipbook_playback(deltaTime, viewerActive, drawData, editingLayer);
}

void DrawingProgramLayerListItem::trigger_touch_flipbook(const CoordSpaceHelper& camCoords, const SCollision::ColliderCollection<WorldScalar>& tapCollider) {
    if(!folderData) return;
    if(get_visible() && is_flipbook_group() && folderData->frame_count() > 1 &&
       get_flipbook_trigger_mode() == FlipbookTriggerMode::ON_TOUCH) {
        std::vector<CanvasComponentContainer::ObjInfo*> comps;
        get_flattened_component_list(comps);
        for(auto* c : comps) {
            if(c->obj->collides_with_world_coords(camCoords, tapCollider)) {
                folderData->flipbook_begin(get_flipbook_invert());
                if(MotionPath* mp = get_motion_path()) mp->reset();   // MOTION-PATH.md P3
                break;
            }
        }
    }
    for(auto& c : *folderData->folderList)
        c.obj->trigger_touch_flipbook(camCoords, tapCollider);
}

bool DrawingProgramLayerListItem::has_motion_path() const {
    // A *usable* path needs at least a start + end node; an emptied path (after
    // remove_motion_path) keeps its NetObj but reads as "no path".
    return motionPath && motionPath->points.size() >= 2;
}

MotionPath* DrawingProgramLayerListItem::get_motion_path() const {
    return motionPath ? motionPath.get() : nullptr;
}

MotionPath& DrawingProgramLayerListItem::ensure_motion_path(DrawingProgramLayerManager& layerMan) {
    if(!motionPath)
        motionPath = layerMan.drawP.world.netObjMan.make_obj<MotionPath>();
    return *motionPath;
}

void DrawingProgramLayerListItem::remove_motion_path(DrawingProgramLayerManager& layerMan) {
    // NetObjOwnerPtr is create-once / destroy-with-holder (no runtime destroy),
    // so "remove" clears the path's content — the NetObj lingers, empty, and
    // has_motion_path() reads false. Re-adding reuses it.
    if(!motionPath) return;
    motionPath->points.clear();
    motionPath->controlIn.clear();
    motionPath->controlOut.clear();
    motionPath->nodeType.clear();
    motionPath->nodeTime.clear();
    motionPath->nodeScale.clear();
    motionPath->nodeEasing.clear();
    layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<MotionPath>(motionPath, false);
}

void DrawingProgramLayerListItem::commit_motion_path(DrawingProgramLayerManager& layerMan) const {
    if(motionPath)
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<MotionPath>(motionPath, false);
}

void DrawingProgramLayerListItem::set_metainfo(DrawingProgramLayerManager& layerMan, const DrawingProgramLayerListItemMetaInfo& metaInfo) {
    set_blend_mode(layerMan, metaInfo.blendMode);
    set_alpha(layerMan, metaInfo.alpha);
    // Raw restore — undo must bring back the exact depth + refScale + anchor
    // tuple, not re-derive anything against wherever the camera is now.
    set_parallax_raw(layerMan, metaInfo.parallaxDepth, metaInfo.parallaxRefScale,
                     WorldVec{metaInfo.parallaxAnchorX, metaInfo.parallaxAnchorY});
    set_flipbook_fps(layerMan, metaInfo.flipbookFps);
    set_flipbook_play_style(layerMan, metaInfo.flipbookPlayStyle);
    set_flipbook_trigger_mode(layerMan, metaInfo.flipbookTriggerMode);
    set_flipbook_invert(layerMan, metaInfo.flipbookInvert);
    set_name(layerMan.drawP.world.delayedUpdateObjectManager, metaInfo.name);
}

DrawingProgramLayerListItemMetaInfo DrawingProgramLayerListItem::get_metainfo() const {
    return DrawingProgramLayerListItemMetaInfo{
        .name = get_name(),
        .alpha = get_alpha(),
        .blendMode = get_blend_mode(),
        .parallaxDepth = get_parallax_depth(),
        .parallaxAnchorX = displayData->parallaxAnchorX,
        .parallaxAnchorY = displayData->parallaxAnchorY,
        .parallaxRefScale = displayData->parallaxRefScale,
        .flipbookFps = displayData->flipbookFps,
        .flipbookPlayStyle = displayData->flipbookPlayStyle,
        .flipbookTriggerMode = displayData->flipbookTriggerMode,
        .flipbookInvert = displayData->flipbookInvert
    };
}

void DrawingProgramLayerListItem::erase_invalid_components() {
    if(folderData)
        folderData->erase_invalid_components();
    else
        layerData->erase_invalid_components();
}

void DrawingProgramLayerListItem::register_class(World& w) {
    w.netObjMan.register_class<DrawingProgramLayerListItem, DrawingProgramLayerListItem, DrawingProgramLayerListItem, DrawingProgramLayerListItem>({
        .writeConstructorFuncClient = write_constructor_func,
        .readConstructorFuncClient = read_constructor_func,
        .readUpdateFuncClient = nullptr,
        .writeConstructorFuncServer = write_constructor_func,
        .readConstructorFuncServer = read_constructor_func,
        .readUpdateFuncServer = nullptr,
    });
    w.delayedUpdateObjectManager.register_class<NameData>(w.netObjMan, NetworkingObjects::DelayUpdateSerializedClassManager::CustomConstructors<NameData>{
        .postUpdateFunc = [&](NameData& o) {
            w.set_to_layout_gui_if_focus();
        }
    });
    w.delayedUpdateObjectManager.register_class<DisplayData>(w.netObjMan, NetworkingObjects::DelayUpdateSerializedClassManager::CustomConstructors<DisplayData>{
        .postUpdateFunc = [&](DisplayData& o) {
            w.drawProg.drawCache.clear_own_cached_surfaces();
            w.set_to_layout_gui_if_focus();
        }
    });
    // MOTION-PATH.md P1 — the optional per-folder animation path syncs whole-
    // struct like DisplayData. A path edit changes playback/chrome, not the
    // static composite, so no cache clear — just relayout.
    w.delayedUpdateObjectManager.register_class<MotionPath>(w.netObjMan, NetworkingObjects::DelayUpdateSerializedClassManager::CustomConstructors<MotionPath>{
        .postUpdateFunc = [&](MotionPath& o) {
            w.set_to_layout_gui_if_focus();
        }
    });
}

void DrawingProgramLayerListItem::write_constructor_func(const NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem>& o, cereal::PortableBinaryOutputArchive& a) {
    a(o->is_folder());
    if(o->is_folder())
        o->folderData->folderList.write_create_message(a);
    else
        o->layerData->components.write_create_message(a);
    o->nameData.write_create_message(a);
    o->displayData.write_create_message(a);
    a(static_cast<uint8_t>(o->kind));
    // MOTION-PATH.md P1 — optional; presence bool then the create-message.
    bool hasMotionPath = static_cast<bool>(o->motionPath);
    a(hasMotionPath);
    if(hasMotionPath)
        o->motionPath.write_create_message(a);
}

void DrawingProgramLayerListItem::read_constructor_func(const NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem>& o, cereal::PortableBinaryInputArchive& a, const std::shared_ptr<NetServer::ClientData>& c) {
    bool isFolder;
    a(isFolder);
    if(isFolder) {
        o->folderData = std::make_unique<DrawingProgramLayerFolder>();
        o->folderData->folderList = o.get_obj_man()->read_create_message<NetworkingObjects::NetObjOrderedList<DrawingProgramLayerListItem>>(a, c);
    }
    else {
        o->layerData = std::make_unique<DrawingProgramLayer>();
        o->layerData->components = o.get_obj_man()->read_create_message<CanvasComponentContainer::NetList>(a, c);
    }
    o->nameData = o.get_obj_man()->read_create_message<NameData>(a, c);
    o->displayData = o.get_obj_man()->read_create_message<DisplayData>(a, c);
    uint8_t kindByte;
    a(kindByte);
    o->kind = static_cast<LayerKind>(kindByte);
    bool hasMotionPath;
    a(hasMotionPath);
    if(hasMotionPath)
        o->motionPath = o.get_obj_man()->read_create_message<MotionPath>(a, c);
}
