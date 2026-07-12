#pragma once
#include <unordered_set>
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include "../../GUIStuff/Elements/TreeListing.hpp"
#include "DrawingProgramLayerListItem.hpp"
#include "SerializedBlendMode.hpp"

class World;
class DrawingProgramLayerManager;

class DrawingProgramLayerManagerGUI {
    public:
        DrawingProgramLayerManagerGUI(DrawingProgramLayerManager& layerManager);
        void refresh_gui_data();
        void setup_list_gui();
    private:
        std::optional<std::pair<NetworkingObjects::NetObjID, NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem>>> try_to_create_in_proper_position(DrawingProgramLayerListItem* newItem);
        std::pair<NetworkingObjects::NetObjID, NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem>> create_in_proper_position(DrawingProgramLayerListItem* newItem);
        NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem> create_layer(DrawingProgramLayerListItem* newItem);
        void remove_layer(const GUIStuff::TreeListingObjIndexList& objIndex);
        // PHASE4 Part C (§11): lossless merge of a layer's components into
        // the layer below it. By-value index — internals clear the GUI
        // selection (which owns the set the index would reference).
        void merge_layer_down(GUIStuff::TreeListingObjIndexList objIndex);
        void editing_layer_check();
        // Stylus-friendly row actions (no drag / double-click needed).
        // load_edit_state_from populates the property-panel edit fields for a
        // layer; select_layer_by_index makes it the selected + editing target and
        // loads its state; move_layer_step reorders it one slot in its parent (NOT
        // undo-tracked — a lightweight erase+reinsert, per zynx).
        void load_edit_state_from(NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem> layer);
        void select_layer_by_index(const GUIStuff::TreeListingObjIndexList& objIndex);
        void move_layer_step(const GUIStuff::TreeListingObjIndexList& objIndex, bool up);
        // Deep-copy a layer/folder (fresh ids, all fields incl. folder mode +
        // motion path + components) and insert it as a sibling right after the
        // source. Undoable. See DrawingProgramLayerListItem::deep_copy.
        void duplicate_layer(const GUIStuff::TreeListingObjIndexList& objIndex);

        NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem> get_layer_parent_from_obj_index(const GUIStuff::TreeListingObjIndexList& objIndex);
        NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem> get_layer_from_obj_index(const GUIStuff::TreeListingObjIndexList& objIndex);

        std::string nameToEdit;
        std::string nameForNew;
        float alphaValToEdit = 0.0f;
        float depthValToEdit = 0.0f;  // PHASE4 Part A: parallax depth
        size_t blendModeValToEdit = 0;
        // PHASE10 Feature B — folder mode (0 Normal / 1 Parallax / 2 Flip-Book;
        // mutually exclusive) + flip-book group edit state (folders).
        size_t folderModeToEdit = 0;
        float flipbookFpsToEdit = 12.0f;
        size_t flipbookStyleToEdit = 0;    // FlipbookPlayStyle
        size_t flipbookTriggerToEdit = 0;  // FlipbookTriggerMode

        std::set<GUIStuff::TreeListingObjIndexList> selectedLayerIndices;
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> editingLayer;

        std::optional<DrawingProgramLayerListItemMetaInfo> editingLayerOldMetainfo;

        DrawingProgramLayerManager& layerMan;
};
