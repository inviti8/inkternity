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
    // The parallax anchor is a world coordinate; depth is dimensionless.
    metaInfo.parallaxAnchorX = metaInfo.parallaxAnchorX * scaleUpAmount;
    metaInfo.parallaxAnchorY = metaInfo.parallaxAnchorY * scaleUpAmount;
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

void DrawingProgramLayerListItem::scale_up(const WorldScalar& scaleUpAmount) {
    // World rescale moves every world coordinate — the parallax anchor
    // included. Depth is a dimensionless ratio and stays put.
    if(displayData) {
        displayData->parallaxAnchorX = displayData->parallaxAnchorX * scaleUpAmount;
        displayData->parallaxAnchorY = displayData->parallaxAnchorY * scaleUpAmount;
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
    if(version >= VersionNumber(0, 12, 0))
        a(*displayData);
    else {
        // Pre-INFPNT000013 files stored only these three fields (no
        // parallax). Reading them individually replicates the old
        // struct's byte layout exactly; depth/anchor stay at their
        // zero defaults → identical behavior to before.
        a(displayData->alpha, displayData->visible, displayData->blendMode);
    }

    if(version >= VersionNumber(0, 8, 0)) {
        uint8_t kindByte;
        a(kindByte);
        kind = static_cast<LayerKind>(kindByte);
    }
    // Pre-0.8 layers stay LayerKind::DEFAULT (the ctor default).

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
        // PHASE4 Part A: a non-zero depth renders this layer through a
        // DERIVED camera — same zoom/rotation, position pulled toward the
        // anchor by 1/(1+depth), so camera pans produce parallax while
        // zoom (uniform magnification) is untouched. Pure translation in
        // world space → affine → works for the SVG render path too.
        // v1: layers only; folders draw their subtree with the camera
        // they were given (depth on folder rows isn't exposed in the UI).
        const DrawData* dd = &drawData;
        std::optional<DrawData> derived;
        if(!folderData && displayData->parallaxDepth != 0.0f) {
            const double invFactor = 1.0 + static_cast<double>(displayData->parallaxDepth);
            derived = drawData;
            derived->cam.c.pos = WorldVec{
                displayData->parallaxAnchorX + (drawData.cam.c.pos.x() - displayData->parallaxAnchorX).divide_double(invFactor),
                displayData->parallaxAnchorY + (drawData.cam.c.pos.y() - displayData->parallaxAnchorY).divide_double(invFactor)};
            derived->refresh_draw_optimizing_values();
            dd = &derived.value();
        }

        if(!drawData.isSVGRender) {
            SkPaint layerPaint;
            layerPaint.setAlphaf(displayData->alpha);
            layerPaint.setBlendMode(serialized_blend_mode_to_sk_blend_mode(displayData->blendMode));
            canvas->saveLayer(nullptr, &layerPaint);
        }

        if(folderData)
            folderData->draw(canvas, *dd);
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
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

float DrawingProgramLayerListItem::get_alpha() const {
    return displayData->alpha;
}

void DrawingProgramLayerListItem::set_visible(DrawingProgramLayerManager& layerMan, bool newVisible) const {
    if(displayData && displayData->visible != newVisible) {
        displayData->visible = newVisible;
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

bool DrawingProgramLayerListItem::get_visible() const {
    return displayData->visible;
}

void DrawingProgramLayerListItem::set_blend_mode(DrawingProgramLayerManager& layerMan, SerializedBlendMode newBlendMode) const {
    if(displayData && displayData->blendMode != newBlendMode) {
        displayData->blendMode = newBlendMode;
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

SerializedBlendMode DrawingProgramLayerListItem::get_blend_mode() const {
    return displayData->blendMode;
}

namespace {
// Per-component WorldVec / double — parallax factors are bounded
// doubles, so divide_double keeps everything in exact FixedPoint land.
WorldVec worldvec_divide_double(const WorldVec& v, double s) {
    return WorldVec{v.x().divide_double(s), v.y().divide_double(s)};
}
// Derived camera position for a layer at `depth` anchored at `A`:
//   derived = A + (camPos - A) / (1 + depth)        (PHASE4.md §3)
WorldVec parallax_derived_pos(const WorldVec& camPos, const WorldVec& A, float depth) {
    return A + worldvec_divide_double(camPos - A, 1.0 + static_cast<double>(depth));
}
}  // namespace

void DrawingProgramLayerListItem::set_parallax_depth(DrawingProgramLayerManager& layerMan, float newDepth) const {
    if(!displayData) return;
    newDepth = std::clamp(newDepth, -0.95f, 1000.0f);
    const float oldDepth = displayData->parallaxDepth;
    if(oldDepth == newDepth) return;

    // No-jump rule: solve for the anchor that keeps the layer's derived
    // camera position (and therefore its on-screen placement) unchanged
    // at this exact moment. K = derived(A_old, d_old); then
    //   A_new = (K·(1+d_new) − camPos) / d_new
    // For d_new == 0 the anchor is irrelevant (factor 1); park it at the
    // camera so a later depth edit pivots where the artist is looking.
    const WorldVec camPos = layerMan.drawP.world.drawData.cam.c.pos;
    const WorldVec K = parallax_derived_pos(camPos, get_parallax_anchor(), oldDepth);
    WorldVec newAnchor;
    if(newDepth == 0.0f)
        newAnchor = camPos;
    else {
        const double dn = static_cast<double>(newDepth);
        const WorldVec kScaled = worldvec_divide_double(K, 1.0 / (1.0 + dn));
        newAnchor = worldvec_divide_double(kScaled - camPos, dn);
    }
    set_parallax_raw(layerMan, newDepth, newAnchor);
}

void DrawingProgramLayerListItem::set_parallax_raw(DrawingProgramLayerManager& layerMan, float depth, const WorldVec& anchor) const {
    if(displayData && (displayData->parallaxDepth != depth ||
                       displayData->parallaxAnchorX != anchor.x() ||
                       displayData->parallaxAnchorY != anchor.y())) {
        displayData->parallaxDepth = depth;
        displayData->parallaxAnchorX = anchor.x();
        displayData->parallaxAnchorY = anchor.y();
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
        layerMan.drawP.world.delayedUpdateObjectManager.send_update_to_all<DisplayData>(displayData, false);
    }
}

float DrawingProgramLayerListItem::get_parallax_depth() const {
    return displayData->parallaxDepth;
}

WorldVec DrawingProgramLayerListItem::get_parallax_anchor() const {
    return WorldVec{displayData->parallaxAnchorX, displayData->parallaxAnchorY};
}

void DrawingProgramLayerListItem::set_metainfo(DrawingProgramLayerManager& layerMan, const DrawingProgramLayerListItemMetaInfo& metaInfo) {
    set_blend_mode(layerMan, metaInfo.blendMode);
    set_alpha(layerMan, metaInfo.alpha);
    // Raw restore — undo must bring back the exact depth+anchor pair,
    // not re-run the no-jump recompute against wherever the camera is now.
    set_parallax_raw(layerMan, metaInfo.parallaxDepth,
                     WorldVec{metaInfo.parallaxAnchorX, metaInfo.parallaxAnchorY});
    set_name(layerMan.drawP.world.delayedUpdateObjectManager, metaInfo.name);
}

DrawingProgramLayerListItemMetaInfo DrawingProgramLayerListItem::get_metainfo() const {
    return DrawingProgramLayerListItemMetaInfo{
        .name = get_name(),
        .alpha = get_alpha(),
        .blendMode = get_blend_mode(),
        .parallaxDepth = get_parallax_depth(),
        .parallaxAnchorX = displayData->parallaxAnchorX,
        .parallaxAnchorY = displayData->parallaxAnchorY
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
}
