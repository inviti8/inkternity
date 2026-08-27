#pragma once
#include <unordered_set>
#include <filesystem>
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
        // Export/Import Layer Group (File menu). Export writes the single
        // selected layer/folder subtree + its resources to `path` (.inkgroup);
        // a no-op with a user message if the selection isn't exactly one item.
        // Import loads such a file, inserts the group at the TOP of the stack
        // with fresh net ids, selects it, and pushes an undo. Both surface
        // failures to the user log rather than throwing. See
        // World::export_layer_group / read_layer_group_file.
        void export_selected_group(const std::filesystem::path& path);
        void import_group_from_file(const std::filesystem::path& path);
        // Create a new (non-folder) layer directly above the current edit target —
        // in front of it in its parent list, i.e. higher in the visual stack — with
        // full undo + net-create, make it the new edit target, and return it. Used
        // to drop an AI reference mesh on its own layer above the artist's drawing
        // (MESH_REFERENCE.md §5). Handles a root-level edit target; if the target is
        // nested in a folder (or there is none) the layer lands at the top of the
        // root stack, which is still above the drawing.
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> create_layer_above_editing(const std::string& name);
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
        // Scale-aware "on view": min on-screen size (px) before an AUTO
        // flip-book / anim-fx group fires. 0 = always (any overlap).
        float autoTriggerMinScreenPxToEdit = 0.0f;
        // Collapsible folder-mode settings sections. Flip-Book (and to a lesser
        // extent Anim-FX / Parallax) add enough widgets to crowd out the layer
        // list in the fixed-height panel, so each is foldable. Default collapsed
        // so selecting a group leaves the list reachable; state persists for the
        // session (shared across groups of the same mode).
        bool sectionFlipbookOpen = false;
        bool sectionAnimFxOpen = false;
        bool sectionParallaxOpen = false;

        std::set<GUIStuff::TreeListingObjIndexList> selectedLayerIndices;
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> editingLayer;
        // Last single-selected layer/group — the target for File-menu actions
        // (Export Layer Group). selectedLayerIndices and editingLayer are BOTH
        // wiped by refresh_gui_data(), which fires when the Layers panel closes
        // — and the panel closes as the File menu opens, so neither is readable
        // by the time Export runs. This one is only ever set (on a single
        // selection), never cleared, so the target survives the menu switch.
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> lastSingleSelectedItem;

        std::optional<DrawingProgramLayerListItemMetaInfo> editingLayerOldMetainfo;

        DrawingProgramLayerManager& layerMan;
};
