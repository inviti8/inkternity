#include "DrawingProgramLayer.hpp"
#include "DrawingProgramLayerManager.hpp"
#include "../DrawingProgram.hpp"
#include "../../World.hpp"
#include "../../MainProgram.hpp"
#include "../../CanvasComponents/WaypointCanvasComponent.hpp"
#include "../../Waypoints/WaypointGraph.hpp"
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include <Helpers/Parallel.hpp>
#include <Helpers/Logger.hpp>

void DrawingProgramLayer::draw(SkCanvas* canvas, const DrawData& drawData) const {
    // skipSelectedComponents: see DrawData.hpp — only set by the live
    // parallax-bypass path, where the selection preview draws separately.
    // (DrawingProgramLayer is on DrawingProgram's friend list.)
    const bool skipSelected = drawData.skipSelectedComponents &&
                              drawData.main && drawData.main->world;
    for(auto& p : *components) {
        if(skipSelected && drawData.main->world->drawProg.selection.is_selected(
               const_cast<CanvasComponentContainer::ObjInfo*>(&p)))
            continue;
        if(p.obj->get_comp().is_mask())   // PHASE7: masks define the clip, not content
            continue;
        p.obj->draw(canvas, drawData);
    }
}

void DrawingProgramLayer::set_component_list_callbacks(DrawingProgramLayerListItem& layerListItem, DrawingProgramLayerManager& layerMan) {
    auto insertCallback = [&](const CanvasComponentContainer::ObjInfoIterator& c) {
        c->obj->objInfo = c;
        c->obj->parentLayer = &layerListItem;
        if(layerMan.commitUpdateOnComponentInsert)
            c->obj->commit_update(layerMan.drawP); // Run commit update on insert so that world bounds are calculated
        if(layerMan.addToCacheOnComponentInsert)
            layerMan.drawP.drawCache.add_component(&(*c));
        // IMAGE (animated GIFs) and PARTICLE (TimelineFX effects) tick every
        // frame via update(); register them as updateable.
        if(c->obj->get_comp().get_type() == CanvasComponentType::IMAGE ||
           c->obj->get_comp().get_type() == CanvasComponentType::PARTICLE)
            layerMan.drawP.updateableComponents.emplace(&(*c));
    };
    eraseCallback = [&](const CanvasComponentContainer::ObjInfoIterator& c) {
        layerMan.drawP.selection.erase_component(&(*c));
        layerMan.drawP.drawCache.erase_component(&(*c));
        layerMan.drawP.drawTool->erase_component(&(*c));
        std::erase_if(layerMan.drawP.droppedDownloadingFiles, [&c](auto& downloadingFile) {
            return downloadingFile.comp == &(*c);
        });
        layerMan.drawP.updateableComponents.erase(&(*c));
        // PHASE2 polish: when a WaypointCanvasComponent goes away (undo
        // of a waypoint drop, eraser tool over the marker, layer delete,
        // etc.) sweep the linked Waypoint out of wpGraph too — without
        // this the tree-view node persists as an orphan and the file
        // still saves it.
        if (c->obj->get_comp().get_type() == CanvasComponentType::WAYPOINT) {
            const auto& wpc = static_cast<const WaypointCanvasComponent&>(c->obj->get_comp());
            layerMan.drawP.world.wpGraph.erase_waypoint_by_id(wpc.get_waypoint_id());
        }
    };
    components->set_insert_callback(insertCallback);
    components->set_erase_callback(eraseCallback);
    components->set_move_callback([&](const CanvasComponentContainer::ObjInfoIterator& c, uint32_t oldPos) {
        layerMan.drawP.drawCache.invalidate_cache_at_optional_aabb(c->obj->get_world_bounds());
        if(layerMan.drawP.selection.is_selected(&(*c)))
            layerMan.drawP.selection.sort_selection(); // If item is selected, selected items will be unordered, so sort everything again
    });
    // Make sure the insert callback is called on existing objects
    for(auto it = components->begin(); it != components->end(); ++it)
        insertCallback(it);
}

void DrawingProgramLayer::set_to_erase() {
    for(auto it = components->begin(); it != components->end(); ++it)
        eraseCallback(it);
}

void DrawingProgramLayer::get_flattened_component_list(std::vector<CanvasComponentContainer::ObjInfo*>& objList) const {
    for(auto& p : *components)
        objList.emplace_back(&p);
}

void DrawingProgramLayer::scale_up(const WorldScalar& scaleUpAmount) {
    for(auto& p : *components)
        p.obj->scale_up(scaleUpAmount);
}

void DrawingProgramLayer::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version, DrawingProgramLayerManager& layerMan) {
    components = layerMan.drawP.world.netObjMan.make_obj<CanvasComponentContainer::NetList>();
    uint32_t layerListSize;
    a(layerListSize);
    for(uint32_t i = 0; i < layerListSize; i++) {
        CanvasComponentContainer* item = new CanvasComponentContainer();
        item->load_file(a, version, layerMan.drawP.world.netObjMan);
        components->push_back_and_send_create(components, item);
    }
}

void DrawingProgramLayer::erase_invalid_components() {
    std::vector<NetworkingObjects::NetObjOrderedListIterator<CanvasComponentContainer>> compsToErase;
    for(auto it = components->begin(); it != components->end(); ++it) {
        if(!it->obj->get_world_bounds().has_value())
            compsToErase.emplace_back(it);
    }
    if(!compsToErase.empty()) {
        Logger::get().log("INFO", "[DrawingProgramLayer::erase_invalid_components] Erased " + std::to_string(compsToErase.size()) + " invalid component(s) from layer");
        components->erase_list(components, compsToErase);
    }
}

void DrawingProgramLayer::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(components->size());
    for(auto& comp : *components)
        comp.obj->save_file(a);
}

void DrawingProgramLayer::get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const {
    for(auto& comp : *components)
        comp.obj->get_comp().get_used_resources(resourceSet);
}
