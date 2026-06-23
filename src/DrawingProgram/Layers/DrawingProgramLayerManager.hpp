#pragma once
#include "DrawingProgramLayerListItem.hpp"
#include "DrawingProgramLayerManagerGUI.hpp"
#include "LayerKind.hpp"
#include <unordered_set>

class DrawingProgramLayerManager {
    private:
        DrawingProgram& drawP;
        friend class DrawingProgramLayerManagerGUI;
        friend class DrawingProgramLayerListItem;
        friend class DrawingProgramLayer;
        friend class DrawingProgramLayerFolder;
        NetworkingObjects::NetObjOwnerPtr<DrawingProgramLayerListItem> layerTreeRoot;
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> editingLayer;
        void set_initial_editing_layer();
        void add_undo_erase_components(const std::unordered_map<DrawingProgramLayerListItem*, std::vector<CanvasComponentContainer::ObjInfoIterator>>& eraseMap);
        void add_undo_place_components(DrawingProgramLayerListItem* parent, const std::vector<CanvasComponentContainer::ObjInfoIterator>& placeList);
        void erase_component_map(const std::unordered_map<DrawingProgramLayerListItem*, std::vector<CanvasComponentContainer::ObjInfoIterator>>& eraseMap);
        bool commitUpdateOnComponentInsert = true;
        bool addToCacheOnComponentInsert = true;
    public:
        enum class LayerSelector {
            ALL_VISIBLE_LAYERS,
            LAYER_BEING_EDITED
        };

        DrawingProgramLayerManager(DrawingProgram& drawProg);
        void server_init_no_file();
        void draw(SkCanvas* canvas, const DrawData& drawData);
        void write_components_server(cereal::PortableBinaryOutputArchive& a);
        void read_components_client(cereal::PortableBinaryInputArchive& a);
        bool is_a_layer_being_edited();
        void scale_up(const WorldScalar& scaleUpAmount);
        template <typename List> void erase_component_container(const List& compsToErase) {
            if(!compsToErase.empty()) {
                std::unordered_map<DrawingProgramLayerListItem*, std::vector<CanvasComponentContainer::ObjInfoIterator>> idsToEraseInSpecificLayers;
                for(auto& c : compsToErase)
                    idsToEraseInSpecificLayers[c->obj->parentLayer].emplace_back(c->obj->objInfo);
                for(auto& [parent, its] : idsToEraseInSpecificLayers) {
                    std::sort(its.begin(), its.end(), [](auto& a, auto& b) {
                        return a->pos < b->pos;
                    });
                }
                erase_component_map(idsToEraseInSpecificLayers);
            }
        }
        uint32_t edited_layer_component_count();
        CanvasComponentContainer::ObjInfoIterator get_edited_layer_end_iterator();
        void push_components_to(const std::vector<CanvasComponentContainer::ObjInfo*>& objs, bool beginIfTrueEndIfFalse);
        bool component_passes_layer_selector(CanvasComponentContainer::ObjInfo* c, LayerSelector layerSelector);
        void disable_add_to_cache_block(const std::function<void()>& toRun);
        void disable_commit_update_block(const std::function<void()>& toRun);
        void disable_add_to_cache_and_commit_update_block(const std::function<void()>& toRun);

        void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version);
        void save_file(cereal::PortableBinaryOutputArchive& a) const;
        void get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet);

        bool layer_tree_root_exists();
        const DrawingProgramLayerListItem& get_layer_root();
        uint32_t total_component_count();

        // PHASE4 Part A: true if any visible non-folder layer has a
        // non-zero parallax depth. The live draw path bypasses the
        // window/BVH caches while this holds (per-layer derived cameras
        // make cross-layer cached composites wrong).
        bool any_visible_parallax_layer();
        // True if the current edit target actually parallaxes (non-zero
        // depth AND inside an active parallax group) — editing is locked
        // for such layers until edit-at-neutral lands (PARALLAX-SCENES §8).
        bool editing_layer_is_parallaxed();
        // True if the current edit target sits under an active parallax
        // group (nearest-ancestor rule). Drives the depth "no active group"
        // hint in the layer panel.
        bool editing_layer_in_active_parallax_group();

        // PHASE2: ensure exactly one of each named-kind layer exists at
        // root; lazy-create any that are missing. Sets editingLayer to
        // the INK layer afterward. Safe to call multiple times.
        void ensure_named_layers();
        // Returns the root-level layer with the given non-DEFAULT kind,
        // or an expired weak ptr if none exists.
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> get_named_layer(LayerKind k);
        // External access to the current edit target. Setter is used by
        // the top-bar layer dropdown; the layer-manager side panel
        // continues to assign editingLayer directly via friend access.
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> get_editing_layer() const { return editingLayer; }
        void set_editing_layer(NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> l) { editingLayer = l; }

        CanvasComponentContainer::ObjInfo* add_component_to_layer_being_edited(CanvasComponentContainer* newObj);
        std::vector<CanvasComponentContainer::ObjInfoIterator> add_many_components_to_layer_being_edited(const std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>>& newObjs);
        // PHASE2 M3: place into a layer that isn't necessarily the
        // active edit target. Mirrors add_many_components_to_layer_being_edited
        // but takes the destination layer as an explicit argument.
        std::vector<CanvasComponentContainer::ObjInfoIterator> add_many_components_to_specific_layer(
            DrawingProgramLayerListItem& destLayer,
            const std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>>& newObjs);
        std::vector<CanvasComponentContainer::ObjInfo*> get_flattened_component_list() const;
        std::vector<DrawingProgramLayerListItem*> get_flattened_layer_list();
        void add_undo_place_component(CanvasComponentContainer::ObjInfo* objInfo);
        DrawingProgramLayerManagerGUI listGUI;
};
