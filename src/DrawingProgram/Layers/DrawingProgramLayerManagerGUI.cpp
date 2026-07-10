#include "DrawingProgramLayerManagerGUI.hpp"
#include "DrawingProgramLayerListItem.hpp"
#include "DrawingProgramLayerManager.hpp"
#include "../DrawingProgram.hpp"
#include "../Tools/MotionPathTool.hpp"
#include "MotionPath.hpp"
#include <memory>
#include "../../World.hpp"
#include "../../MainProgram.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/NetworkingObjects/NetObjOrderedList.hpp"
#include "SerializedBlendMode.hpp"
#include <Helpers/Logger.hpp>
#include <cstdio>

#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/LayoutHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/CheckBoxHelpers.hpp"
#include "../../GUIStuff/Elements/SVGIcon.hpp"
#include "../../GUIStuff/Elements/DropDown.hpp"

// PHASE10 Feature B — folder-mode dropdown (0 Normal / 1 Parallax / 2 Flip-Book;
// mutually exclusive, since parallax draws all children as depth planes at once
// while a flip-book shows one child frame at a time).
static const std::vector<std::string>& folder_mode_names() {
    static const std::vector<std::string> v = {"Normal Folder", "Parallax Scene", "Flip-Book", "Anim-FX"};
    return v;
}
static size_t folder_mode_index(const DrawingProgramLayerListItem& f) {
    if(f.is_anim_fx_group()) return 3;
    if(f.is_flipbook_group()) return 2;
    if(f.is_parallax_group()) return 1;
    return 0;
}

// PHASE10 Feature B — dropdown label lists; order matches the enum values in
// FlipbookMode.hpp (ONCE/LOOP/PING_PONG, AUTO/ON_TOUCH).
static const std::vector<std::string>& flipbook_play_style_names() {
    static const std::vector<std::string> v = {"Play Once", "Loop", "Ping-Pong"};
    return v;
}
static const std::vector<std::string>& flipbook_trigger_names() {
    static const std::vector<std::string> v = {"Auto (on view)", "On Touch"};
    return v;
}

DrawingProgramLayerManagerGUI::DrawingProgramLayerManagerGUI(DrawingProgramLayerManager& drawLayerMan):
    layerMan(drawLayerMan)
{}

void DrawingProgramLayerManagerGUI::refresh_gui_data() {
    selectedLayerIndices.clear();
    editing_layer_check();
    nameToEdit.clear();
    nameForNew.clear();
    editingLayerOldMetainfo = std::nullopt;
    alphaValToEdit = 0.0f;
    depthValToEdit = 0.0f;
    blendModeValToEdit = 0;
    folderModeToEdit = 0;
    flipbookFpsToEdit = 12.0f;
    flipbookStyleToEdit = 0;
    flipbookTriggerToEdit = 0;
}

NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem> DrawingProgramLayerManagerGUI::get_layer_parent_from_obj_index(const GUIStuff::TreeListingObjIndexList& objIndex) {
    if(objIndex.empty())
        throw std::runtime_error("[DrawingProgramLayerManagerGUI::get_layer_parent_from_obj_index] Called on empty objIndex");
    NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem> toRet = layerMan.layerTreeRoot;
    for(size_t i = 0; i < objIndex.size() - 1; i++)
        toRet = toRet->get_folder().folderList->at(objIndex[i])->obj;
    return toRet;
}

NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem> DrawingProgramLayerManagerGUI::get_layer_from_obj_index(const GUIStuff::TreeListingObjIndexList& objIndex) {
    NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem> toRet = layerMan.layerTreeRoot;
    for(size_t i : objIndex)
        toRet = toRet->get_folder().folderList->at(i)->obj;
    return toRet;
}

void DrawingProgramLayerManagerGUI::setup_list_gui() {
    using namespace NetworkingObjects;
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& world = layerMan.drawP.world;
    if(layerMan.layerTreeRoot) {
        auto& gui = world.main.g.gui;
        if(layerMan.layerTreeRoot->get_folder().folderList->empty()) {
            text_label_centered(gui, "No layers exist.");
            selectedLayerIndices.clear();
            nameToEdit.clear();
        }
        else {
            gui.element<TreeListing>("list", TreeListing::Data {
                .selectedIndices = &selectedLayerIndices,
                .dirInfo = [&](const TreeListingObjIndexList& objIndex) -> std::optional<TreeListing::DirectoryInfo> {
                    auto layer = get_layer_from_obj_index(objIndex);
                    if(!layer->is_folder())
                        return std::nullopt;
                    else
                        return TreeListing::DirectoryInfo{
                            .dirSize = layer->get_folder().folderList->size(),
                            .isOpen = layer->get_folder().isFolderOpen
                        };
                },
                .setDirectoryOpen = [&](const TreeListingObjIndexList& objIndex, bool newDirectoryOpen) {
                    get_layer_from_obj_index(objIndex)->get_folder().isFolderOpen = newDirectoryOpen;
                },
                .drawNonDirectoryObjIconGUI = [&](const TreeListingObjIndexList& objIndex) {
                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                            .padding = CLAY_PADDING_ALL(2),
                        },
                    }) {
                        gui.element<SVGIcon>("layer ico", "data/icons/layer.svg");
                    }
                },
                .drawObjGUI = [&](const TreeListingObjIndexList& objIndex) {
                    auto layer = get_layer_from_obj_index(objIndex);
                    if(!layer->is_folder()) {
                        CLAY_AUTO_ID({
                            .layout = {
                                .sizing = {.width = CLAY_SIZING_FIXED(GUIStuff::TreeListing::ENTRY_HEIGHT), .height = CLAY_SIZING_FIXED(GUIStuff::TreeListing::ENTRY_HEIGHT)},
                                .padding = CLAY_PADDING_ALL(2),
                            },
                        }) {
                            if(layer.get_net_id() == layerMan.editingLayer.get_net_id())
                                gui.element<SVGIcon>("edit ico", "data/icons/pencil.svg");
                        }
                    }
                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                            .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}
                        },
                    }) {
                        // PARALLAX-SCENES: row badge — parallax-scene folders
                        // and depth-carrying layers, so it's obvious why
                        // content pans "differently".
                        std::string rowName = layer->get_name();
                        if(layer->is_parallax_group())
                            rowName += "  (parallax scene)";
                        else if(!layer->is_folder() && layer->get_parallax_depth() != 0.0f) {
                            char depthBuf[24];
                            snprintf(depthBuf, sizeof(depthBuf), "  (depth %.2f)", layer->get_parallax_depth());
                            rowName += depthBuf;
                        }
                        // PHASE10 Feature B — flip-book badge so it's obvious the
                        // folder shows one frame at a time.
                        if(layer->is_flipbook_group())
                            rowName += "  (flip-book)";
                        text_label(gui, rowName);
                    }
                    if(!layer->is_folder()) {
                        gui.set_z_index_keep_clipping_region(gui.get_z_index() + 1, [&] {
                            svg_icon_button(gui, "edit button", "data/icons/pencil.svg", {
                                .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                                .size = TreeListing::ENTRY_HEIGHT,
                                .onClick = [&, layer] {
                                    layerMan.editingLayer = layer;
                                }
                            });
                        });
                    }
                    gui.set_z_index_keep_clipping_region(gui.get_z_index() + 1, [&] {
                        svg_icon_button(gui, "visible button", layer->get_visible() ? "data/icons/eyeopen.svg" : "data/icons/eyeclose.svg", {
                            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                            .size = TreeListing::ENTRY_HEIGHT,
                            .onClick = [&, layer] {
                                layer->set_visible(layerMan, !layer->get_visible());
                            }
                        });
                    });
                    // PHASE2 C5: hide the delete button on named layers
                    // (Sketch / Color / Ink). They're document-level
                    // anchors managed by the layer manager itself.
                    if(layer->get_kind() == LayerKind::DEFAULT) {
                        gui.set_z_index_keep_clipping_region(gui.get_z_index() + 1, [&] {
                            svg_icon_button(gui, "delete button", "data/icons/trash.svg", {
                                .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                                .size = TreeListing::ENTRY_HEIGHT,
                                .onClick = [&, objIndex] {
                                    remove_layer(objIndex);
                                }
                            });
                        });
                    }
                },
                .onDoubleClick = [&](const TreeListingObjIndexList& objIndex) {
                    layerMan.editingLayer = get_layer_from_obj_index(objIndex);
                    layerMan.drawP.world.main.g.gui.set_to_layout();
                },
                .onSelectChange = [&] {
                    editing_layer_check();
                    if(selectedLayerIndices.size() == 1) {
                        const TreeListingObjIndexList& objIndex = *selectedLayerIndices.begin();
                        NetObjTemporaryPtr<DrawingProgramLayerListItem> tempPtr = get_layer_from_obj_index(objIndex);
                        if(tempPtr) {
                            nameToEdit = tempPtr->get_name();
                            alphaValToEdit = tempPtr->get_alpha();
                            depthValToEdit = tempPtr->get_parallax_depth();
                            auto it = std::find(get_blend_mode_useful_list().begin(), get_blend_mode_useful_list().end(), tempPtr->get_blend_mode());
                            blendModeValToEdit = (it == get_blend_mode_useful_list().end()) ? 0 : (it - get_blend_mode_useful_list().begin());
                            // PHASE10 Feature B — folder mode + flip-book edit state.
                            // Show 12 fps as the pre-enable default so switching to
                            // Flip-Book starts sane.
                            folderModeToEdit = tempPtr->is_folder() ? folder_mode_index(*tempPtr) : 0;
                            flipbookFpsToEdit = tempPtr->get_flipbook_fps() > 0.0f ? tempPtr->get_flipbook_fps() : 12.0f;
                            flipbookStyleToEdit = static_cast<size_t>(tempPtr->get_flipbook_play_style());
                            flipbookTriggerToEdit = static_cast<size_t>(tempPtr->get_flipbook_trigger_mode());
                        }
                        else {
                            nameToEdit.clear();
                            alphaValToEdit = 0.0f;
                            depthValToEdit = 0.0f;
                            blendModeValToEdit = 0;
                            folderModeToEdit = 0;
                            flipbookFpsToEdit = 12.0f;
                            flipbookStyleToEdit = 0;
                            flipbookTriggerToEdit = 0;
                        }
                    }
                    else {
                        nameToEdit.clear();
                        alphaValToEdit = 0.0f;
                        depthValToEdit = 0.0f;
                        blendModeValToEdit = 0;
                        folderModeToEdit = 0;
                        flipbookFpsToEdit = 12.0f;
                        flipbookStyleToEdit = 0;
                        flipbookTriggerToEdit = 0;
                    }
                },
                .moveObj = [&](const std::vector<TreeListingObjIndexList>& objectIndicesTreeListing, const TreeListingObjIndexList& newObjIndexTreeListing) {
                    NetObjID listObj;
                    size_t index;
                    struct ParentObjectIDPair {
                        NetObjID parent;
                        NetObjID object;
                    };
                    std::vector<ParentObjectIDPair> objsToInsert;

                    {
                        listObj = get_layer_parent_from_obj_index(newObjIndexTreeListing).get_net_id();
                        index = newObjIndexTreeListing.back();
                        for(auto& objIndex : objectIndicesTreeListing)
                            objsToInsert.emplace_back(get_layer_parent_from_obj_index(objIndex).get_net_id(), get_layer_from_obj_index(objIndex).get_net_id());
                    }

                    std::unordered_map<WorldUndoManager::UndoObjectID, std::pair<std::vector<uint32_t>, std::vector<WorldUndoManager::UndoObjectID>>> toEraseMapUndo;
                    WorldUndoManager::UndoObjectID insertParentUndoID;
                    uint32_t insertUndoPosition;
                    std::vector<WorldUndoManager::UndoObjectID> insertedUndoIDs;

                    world.netObjMan.send_multi_update_messsage([&]() {
                        std::unordered_map<NetObjID, std::vector<NetObjOrderedListIterator<DrawingProgramLayerListItem>>> toEraseMap;
                        uint32_t newIndex = index;
                        std::vector<NetObjOwnerPtr<DrawingProgramLayerListItem>> toInsertObjPtrs;
                        auto& listPtr = world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(listObj)->get_folder().folderList;

                        // Map parent folders to list of objects that should be erased from them
                        for(size_t i = 0; i < objsToInsert.size(); i++) {
                            auto& parentListPtr = world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(objsToInsert[i].parent)->get_folder().folderList;
                            toEraseMap[objsToInsert[i].parent].emplace_back(parentListPtr->get(objsToInsert[i].object));
                            if(newIndex != 0 && objsToInsert[i].parent == listObj && listPtr->get(objsToInsert[i].object)->pos <= index)
                                newIndex--;
                        }

                        // Prepare erase map for undo
                        for(auto& [parentNetID, objsToEraseList] : toEraseMap) {
                            auto& undoEraseListPairForParent = toEraseMapUndo[world.undo.get_undoid_from_netid(parentNetID)];
                            uint32_t i = 0;
                            for(auto& objToErase : objsToEraseList) {
                                undoEraseListPairForParent.first.emplace_back(objToErase->pos - i);
                                undoEraseListPairForParent.second.emplace_back(world.undo.get_undoid_from_netid(objToErase->obj.get_net_id()));
                                i++;
                            }
                        }

                        // Erase the list of objects we previously mapped to each parent, and move the erased objects to vector toInsertObjPtrs
                        for(auto& [listToEraseFrom, setToErase] : toEraseMap) {
                            auto& listToEraseFromPtr = world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(listToEraseFrom)->get_folder().folderList;
                            listToEraseFromPtr->erase_list(listToEraseFromPtr, setToErase, &toInsertObjPtrs);
                        }

                        // Insert objects in toInsertObjPtrs into the folder we have selected
                        std::vector<std::pair<NetObjOrderedListIterator<DrawingProgramLayerListItem>, NetObjOwnerPtr<DrawingProgramLayerListItem>>> toInsert;
                        auto insertIt = listPtr->at(newIndex);
                        for(uint32_t i = 0; i < objsToInsert.size(); i++) {
                            auto it = std::find_if(toInsertObjPtrs.begin(), toInsertObjPtrs.end(), [id = objsToInsert[i].object](NetObjOwnerPtr<DrawingProgramLayerListItem>& objPtr) {
                                return objPtr.get_net_id() == id;
                            });
                            toInsert.emplace_back(insertIt, std::move(*it));
                            toInsert.back().second.reassign_ids();
                        }
                        std::vector<NetObjOrderedListIterator<DrawingProgramLayerListItem>> insertedIterators = listPtr->insert_sorted_list_and_send_create(listPtr, toInsert);

                        // Prepare insert list for undo
                        insertParentUndoID = world.undo.get_undoid_from_netid(listObj);
                        insertUndoPosition = newIndex;
                        for(auto& it : insertedIterators)
                            insertedUndoIDs.emplace_back(world.undo.get_undoid_from_netid(it->obj.get_net_id()));
                    }, NetObjManager::SendUpdateType::SEND_TO_ALL, nullptr);

                    class MoveLayersWorldUndoAction : public WorldUndoAction {
                        public:
                            // If the undo ID maps to the net id, we can also assume that the object is definitely inside the list we want to move it from, and not inside some other list
                            // If it was moved to another list, there would be an undo move that would move it back to this list, and if it was moved by another player the undo id wouldnt map back to the net id, and the undo would fail
                            // We can also assume all objects in all lists remain in the same order for the same reason, so no need to sort anything if it was sorted when the undo action was generated
                            MoveLayersWorldUndoAction(const std::unordered_map<WorldUndoManager::UndoObjectID, std::pair<std::vector<uint32_t>, std::vector<WorldUndoManager::UndoObjectID>>>& initUndoEraseMap,
                                                         WorldUndoManager::UndoObjectID initInsertParentUndoID,
                                                         uint32_t initInsertPosition,
                                                         std::vector<WorldUndoManager::UndoObjectID> initInsertedUndoIDs):
                            undoEraseMap(initUndoEraseMap),
                            insertParentUndoID(initInsertParentUndoID),
                            insertPosition(initInsertPosition),
                            insertedUndoIDs(initInsertedUndoIDs)
                            {}
                            std::string get_name() const override {
                                return "Move Layers";
                            }
                            bool undo(WorldUndoManager& undoMan) override {
                                std::vector<NetObjOwnerPtr<DrawingProgramLayerListItem>> toMoveObjs;
                                std::vector<NetObjOrderedListIterator<DrawingProgramLayerListItem>> toEraseList;
                                NetObjTemporaryPtr<NetObjOrderedList<DrawingProgramLayerListItem>> eraseListPtr;
                                std::unordered_map<NetObjID, std::pair<std::vector<uint32_t>*, std::vector<NetObjID>>> toInsertToMap;

                                {
                                    std::optional<NetObjID> eraseListNetID = undoMan.get_netid_from_undoid(insertParentUndoID);
                                    if(!eraseListNetID.has_value())
                                        return false;
                                    eraseListPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(eraseListNetID.value())->get_folder().folderList;
                                }

                                {
                                    std::vector<NetObjID> toEraseNetIDList;
                                    if(!undoMan.fill_netid_list_from_undoid_list(toEraseNetIDList, insertedUndoIDs))
                                        return false;
                                    toEraseList = eraseListPtr->get_list(toEraseNetIDList);
                                }

                                {
                                    for(auto& [undoID, indexUndoIdListPair] : undoEraseMap) {
                                        std::optional<NetObjID> netID = undoMan.get_netid_from_undoid(undoID);
                                        if(!netID.has_value())
                                            return false;
                                        auto& insertMapEntry = toInsertToMap[netID.value()];
                                        insertMapEntry.first = &indexUndoIdListPair.first;
                                        if(!undoMan.fill_netid_list_from_undoid_list(insertMapEntry.second, indexUndoIdListPair.second))
                                            return false;
                                    }
                                }

                                undoMan.world.netObjMan.send_multi_update_messsage([&]() {
                                    eraseListPtr->erase_list(eraseListPtr, toEraseList, &toMoveObjs);

                                    for(auto& [parentNetID, indexNetIDListPair] : toInsertToMap) {
                                        auto& parentListPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(parentNetID)->get_folder().folderList;
                                        std::vector<NetObjOrderedListIterator<DrawingProgramLayerListItem>> insertIteratorList = parentListPtr->at_ordered_indices(*indexNetIDListPair.first);
                                        auto& insertNetIDList = indexNetIDListPair.second;
                                        std::vector<std::pair<NetObjOrderedListIterator<DrawingProgramLayerListItem>, NetObjOwnerPtr<DrawingProgramLayerListItem>>> toInsert;
                                        for(auto [insertIt, netID] : std::views::zip(insertIteratorList, insertNetIDList)) {
                                            auto it = std::find_if(toMoveObjs.begin(), toMoveObjs.end(), [id = netID](NetObjOwnerPtr<DrawingProgramLayerListItem>& objPtr) {
                                                return objPtr.get_net_id() == id;
                                            });
                                            toInsert.emplace_back(insertIt, std::move(*it));
                                            toInsert.back().second.reassign_ids();
                                        }
                                        parentListPtr->insert_sorted_list_and_send_create(parentListPtr, toInsert);
                                    }
                                }, NetObjManager::SendUpdateType::SEND_TO_ALL, nullptr);
                                return true;
                            }
                            bool redo(WorldUndoManager& undoMan) override {
                                std::unordered_map<NetObjID, std::vector<NetObjID>> toEraseMap;
                                NetObjTemporaryPtr<NetObjOrderedList<DrawingProgramLayerListItem>> listPtr;
                                std::vector<NetObjID> objsToInsert;

                                {
                                    for(auto& [undoID, indexUndoIDListPair] : undoEraseMap) {
                                        std::optional<NetObjID> netID = undoMan.get_netid_from_undoid(undoID);
                                        if(!netID.has_value())
                                            return false;
                                        if(!undoMan.fill_netid_list_from_undoid_list(toEraseMap[netID.value()], indexUndoIDListPair.second))
                                            return false;
                                    }
                                }

                                {
                                    std::optional<NetObjID> netID = undoMan.get_netid_from_undoid(insertParentUndoID);
                                    if(!netID.has_value())
                                        return false;
                                    listPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(netID.value())->get_folder().folderList;
                                }

                                {
                                    if(!undoMan.fill_netid_list_from_undoid_list(objsToInsert, insertedUndoIDs))
                                        return false;
                                }

                                undoMan.world.netObjMan.send_multi_update_messsage([&]() {
                                    std::vector<NetObjOwnerPtr<DrawingProgramLayerListItem>> toInsertObjPtrs;

                                    for(auto& [listToEraseFrom, setToErase] : toEraseMap) {
                                        auto& listToEraseFromPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(listToEraseFrom)->get_folder().folderList;
                                        listToEraseFromPtr->erase_list(listToEraseFromPtr, listToEraseFromPtr->get_list(setToErase), &toInsertObjPtrs);
                                    }

                                    std::vector<std::pair<NetObjOrderedListIterator<DrawingProgramLayerListItem>, NetObjOwnerPtr<DrawingProgramLayerListItem>>> toInsert;
                                    auto insertIt = listPtr->at(insertPosition);
                                    for(uint32_t i = 0; i < objsToInsert.size(); i++) {
                                        auto it = std::find_if(toInsertObjPtrs.begin(), toInsertObjPtrs.end(), [id = objsToInsert[i]](NetObjOwnerPtr<DrawingProgramLayerListItem>& objPtr) {
                                            return objPtr.get_net_id() == id;
                                        });
                                        toInsert.emplace_back(insertIt, std::move(*it));
                                        toInsert.back().second.reassign_ids();
                                    }
                                    listPtr->insert_sorted_list_and_send_create(listPtr, toInsert);
                                }, NetObjManager::SendUpdateType::SEND_TO_ALL, nullptr);

                                return true;
                            }
                            ~MoveLayersWorldUndoAction() {}

                            std::unordered_map<WorldUndoManager::UndoObjectID, std::pair<std::vector<uint32_t>, std::vector<WorldUndoManager::UndoObjectID>>> undoEraseMap;
                            WorldUndoManager::UndoObjectID insertParentUndoID;
                            uint32_t insertPosition;
                            std::vector<WorldUndoManager::UndoObjectID> insertedUndoIDs;
                    };
                    world.undo.push(std::make_unique<MoveLayersWorldUndoAction>(toEraseMapUndo, insertParentUndoID, insertUndoPosition, insertedUndoIDs));
                }
            });
        }

        left_to_right_line_layout(gui, [&]() {
            input_text(gui, "input new name", &nameForNew, {
                .onEnter = [&] {
                    auto newLayerObjInfo = create_layer(new DrawingProgramLayerListItem(world.netObjMan, nameForNew, false));
                    layerMan.editingLayer = newLayerObjInfo->obj;
                    layerMan.drawP.world.main.g.gui.set_to_layout();
                }
            });

            svg_icon_button(gui, "create new layer", "data/icons/plusbold.svg", {
                .size = SMALL_BUTTON_SIZE,
                .onClick = [&] {
                    auto newLayerObjInfo = create_layer(new DrawingProgramLayerListItem(world.netObjMan, nameForNew, false));
                    layerMan.editingLayer = newLayerObjInfo->obj;
                    layerMan.drawP.world.main.g.gui.set_to_layout();
                }
            });

            svg_icon_button(gui, "create new folder", "data/icons/folderbold.svg", {
                .size = SMALL_BUTTON_SIZE,
                .onClick = [&] {
                    auto newFolderObjInfo = create_layer(new DrawingProgramLayerListItem(world.netObjMan, nameForNew, true));
                    newFolderObjInfo->obj->get_folder().isFolderOpen = true;
                }
            });
        });

        auto editingLayerLock = editingLayer.lock();
        if(editingLayerLock) {
            text_label_centered(gui, editingLayerLock->is_folder() ? "Edit Layer Folder" : "Edit Layer");
            // PHASE2 C5: rename is locked for named layers (Sketch /
            // Color / Ink) — their names carry semantic meaning and
            // shouldn't drift. Render a static label instead of an
            // editable field. Alpha + blend mode below stay editable.
            if(editingLayerLock->get_kind() != LayerKind::DEFAULT) {
                left_to_right_line_layout(gui, [&]() {
                    text_label(gui, "Name");
                    text_label(gui, editingLayerLock->get_name() + "  (system layer)");
                });
            }
            else {
                input_text_field(gui, "input edit name", "Name", &nameToEdit, {
                    .onEdit = [&] {
                        auto editingLayerLock = editingLayer.lock();
                        if(editingLayerLock) { // This can be set to null on the way here (close layer popup when textbox is selected)
                            editingLayerLock->set_name(world.delayedUpdateObjectManager, nameToEdit);
                            world.main.g.gui.set_to_layout();
                        }
                    }
                });
            }
            slider_scalar_field(gui, "input alpha slider", "Alpha", &alphaValToEdit, 0.0f, 1.0f, {
                .decimalPrecision = 2,
                .onEdit = [&] {
                    auto editingLayerLock = editingLayer.lock();
                    if(editingLayerLock) {
                        editingLayerLock->set_name(world.delayedUpdateObjectManager, nameToEdit);
                        editingLayerLock->set_alpha(layerMan, alphaValToEdit);
                    }
                }
            });
            left_to_right_line_layout(gui, [&]() {
                text_label(gui, "Blend Mode");
                gui.element<DropDown<size_t>>("input blend mode", &blendModeValToEdit, get_blend_mode_useful_name_list(), DropdownOptions{
                    .onClick = [&] {
                        auto editingLayerLock = editingLayer.lock();
                        if(editingLayerLock)
                            editingLayerLock->set_blend_mode(layerMan, get_blend_mode_useful_list()[blendModeValToEdit]);
                    }
                });
            });
            // A FOLDER is Normal, a Parallax Scene, OR a Flip-Book. Parallax
            // (all children as depth planes at once) and flip-book (one child
            // frame at a time) are opposite readings of the same children, so a
            // folder can be at most one — a single Folder Mode dropdown enforces
            // it. See docs/design/PHASE10.md Feature B §B.2 + PARALLAX-SCENES.md §6.
            if(editingLayerLock->is_folder()) {
                left_to_right_line_layout(gui, [&]() {
                    text_label(gui, "Folder Mode");
                    gui.element<DropDown<size_t>>("folder mode", &folderModeToEdit, folder_mode_names(), DropdownOptions{
                        .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(!lk) return;
                            const CoordSpaceHelper& cam = world.drawData.cam.c;
                            // The four modes are mutually exclusive — clear the
                            // other two, then enable the chosen one.
                            switch(folderModeToEdit) {
                                case 1: // Parallax Scene — capture the current view as
                                        // the neutral viewpoint (cam.pos == anchor → no jump).
                                    lk->set_flipbook_fps(layerMan, 0.0f);
                                    lk->set_anim_fx_group(layerMan, false);
                                    lk->set_parallax_group(layerMan, cam.inverseScale, cam.pos);
                                    break;
                                case 2: // Flip-Book — default 12 fps.
                                    lk->set_parallax_group(layerMan, WorldScalar{0}, lk->get_parallax_anchor());
                                    lk->set_anim_fx_group(layerMan, false);
                                    flipbookFpsToEdit = 12.0f;
                                    lk->set_flipbook_fps(layerMan, flipbookFpsToEdit);
                                    break;
                                case 3: // Anim-FX — particle FX travels a motion path.
                                    lk->set_parallax_group(layerMan, WorldScalar{0}, lk->get_parallax_anchor());
                                    lk->set_flipbook_fps(layerMan, 0.0f);
                                    lk->set_anim_fx_group(layerMan, true);
                                    break;
                                default: // Normal Folder — all off.
                                    lk->set_parallax_group(layerMan, WorldScalar{0}, lk->get_parallax_anchor());
                                    lk->set_flipbook_fps(layerMan, 0.0f);
                                    lk->set_anim_fx_group(layerMan, false);
                                    break;
                            }
                        }
                    });
                });
                if(editingLayerLock->is_parallax_group()) {
                    text_button(gui, "parallax set anchor", "Set Anchor and Scale to Current View", {
                        .wide = true,
                        .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(lk) {
                                const CoordSpaceHelper& cam = world.drawData.cam.c;
                                lk->set_parallax_group(layerMan, cam.inverseScale, cam.pos);
                            }
                        }
                    });
                    // Live readout: how the current view's zoom compares to the
                    // scene's neutral scale (1.00 = at neutral). >1 = zoomed
                    // out from the scene; <1 = zoomed in past it.
                    double rel = static_cast<double>(world.drawData.cam.c.inverseScale / editingLayerLock->get_parallax_ref_scale());
                    char relBuf[80];
                    if(rel >= 1000.0 || !(rel == rel))
                        snprintf(relBuf, sizeof(relBuf), "View vs scene scale: >1000x (1.00 = neutral)");
                    else
                        snprintf(relBuf, sizeof(relBuf), "View vs scene scale: %.2fx (1.00 = neutral)", rel);
                    text_label(gui, relBuf);
                    text_button(gui, "parallax go neutral", "Go to Neutral View", {
                        .wide = true,
                        .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(lk && lk->is_parallax_group()) {
                                CoordSpaceHelper neutral{lk->get_parallax_anchor(), lk->get_parallax_ref_scale(), world.drawData.cam.c.rotation};
                                world.drawData.cam.smooth_move_to(world, neutral, world.main.window.size.cast<float>());
                            }
                        }
                    });
                }
                // PHASE10 Feature B — flip-book controls (Folder Mode == Flip-Book).
                // Child layers are animation frames (top row = frame 1); playback
                // runs only in reader/viewer mode or via Preview. See PHASE10.md.
                else if(editingLayerLock->is_flipbook_group()) {
                    slider_scalar_field(gui, "flipbook fps", "Frames / sec", &flipbookFpsToEdit, 1.0f, 60.0f, {
                        .decimalPrecision = 0,
                        .onEdit = [&] {
                            auto lk = editingLayer.lock();
                            if(lk) lk->set_flipbook_fps(layerMan, flipbookFpsToEdit);
                        }
                    });
                    left_to_right_line_layout(gui, [&]() {
                        text_label(gui, "Play Style");
                        gui.element<DropDown<size_t>>("flipbook style", &flipbookStyleToEdit, flipbook_play_style_names(), DropdownOptions{
                            .onClick = [&] {
                                auto lk = editingLayer.lock();
                                if(lk) lk->set_flipbook_play_style(layerMan, static_cast<FlipbookPlayStyle>(flipbookStyleToEdit));
                            }
                        });
                    });
                    left_to_right_line_layout(gui, [&]() {
                        text_label(gui, "Trigger");
                        gui.element<DropDown<size_t>>("flipbook trigger", &flipbookTriggerToEdit, flipbook_trigger_names(), DropdownOptions{
                            .onClick = [&] {
                                auto lk = editingLayer.lock();
                                if(lk) lk->set_flipbook_trigger_mode(layerMan, static_cast<FlipbookTriggerMode>(flipbookTriggerToEdit));
                            }
                        });
                    });
                    checkbox_field(gui, "flipbook invert", "Invert direction (bottom to top)",
                        [&]() -> bool {
                            auto lk = editingLayer.lock();
                            return lk && lk->get_flipbook_invert();
                        },
                        [&] {
                            auto lk = editingLayer.lock();
                            if(lk) lk->set_flipbook_invert(layerMan, !lk->get_flipbook_invert());
                        });
                    // Onion skin: ghost the frame below + above the one being edited
                    // (a flip-book otherwise shows only one frame). Session-only aid.
                    checkbox_field(gui, "flipbook onion", "Onion skin (ghost adjacent frames)",
                        [&]() -> bool {
                            auto lk = editingLayer.lock();
                            return lk && lk->is_folder() && lk->get_folder().flipbookRuntime.onionSkin;
                        },
                        [&] {
                            auto lk = editingLayer.lock();
                            if(lk && lk->is_folder()) {
                                auto& rt = lk->get_folder().flipbookRuntime;
                                rt.onionSkin = !rt.onionSkin;
                            }
                        });
                    // MOTION-PATH.md — add/edit/remove the animation path (the
                    // group travels this bezier curve as it plays). The path
                    // itself is edited by MotionPathTool (editor-only chrome).
                    if(editingLayerLock->has_motion_path()) {
                        text_button(gui, "flipbook edit path", "Edit Motion Path", { .wide = true, .onClick = [&] {
                            if(editingLayer.lock())
                                world.drawProg.switch_to_tool_ptr(std::make_unique<MotionPathTool>(world.drawProg, editingLayer.get_net_id()));
                        }});
                        text_button(gui, "flipbook remove path", "Remove Motion Path", { .wide = true, .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(lk) lk->remove_motion_path(layerMan);
                        }});
                    }
                    else {
                        text_button(gui, "flipbook add path", "Add Motion Path", { .wide = true, .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(!lk) return;
                            MotionPath& mp = lk->ensure_motion_path(layerMan);
                            // Default 2-node path across the current view. Anchoring
                            // the path coords to the live camera makes path-local ==
                            // screen space at creation, so the nodes land where set.
                            mp.coords = world.drawData.cam.c;
                            const Vector2f ws = world.main.window.size.cast<float>();
                            mp.points = { Vector2f{ws.x() * 0.3f, ws.y() * 0.5f}, Vector2f{ws.x() * 0.7f, ws.y() * 0.5f} };
                            mp.controlIn = { Vector2f{0.0f, 0.0f}, Vector2f{0.0f, 0.0f} };
                            mp.controlOut = { Vector2f{0.0f, 0.0f}, Vector2f{0.0f, 0.0f} };
                            mp.nodeType = { 0, 0 };
                            mp.nodeSeconds = { 0.0f, 2.0f };   // start has no segment; end takes 2s
                            mp.nodeScale = { 1.0f, 1.0f };
                            mp.nodeEasing = { 1, 1 };   // TransitionEasing::EASE
                            mp.nodeRotation = { 0.0f, 0.0f };   // no rotation-along-tangent by default
                            lk->commit_motion_path(layerMan);
                            world.drawProg.switch_to_tool_ptr(std::make_unique<MotionPathTool>(world.drawProg, editingLayer.get_net_id()));
                        }});
                    }
                    // Preview: play in drawing mode to debug timing. Transient
                    // per-folder override (not saved, not synced, not undoable).
                    bool previewing = editingLayerLock->get_folder().flipbookRuntime.previewPlaying;
                    text_button(gui, "flipbook preview", previewing ? "Stop Preview" : "Preview Animation", {
                        .wide = true,
                        .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(lk && lk->is_folder()) {
                                auto& rt = lk->get_folder().flipbookRuntime;
                                if(rt.previewPlaying) {
                                    rt.previewPlaying = false;
                                    rt.playing = false;
                                }
                                else {
                                    rt.previewPlaying = true;
                                    lk->get_folder().flipbook_begin(lk->get_flipbook_invert());
                                    if(MotionPath* mp = lk->get_motion_path()) mp->reset();   // MOTION-PATH.md P3
                                }
                            }
                        }
                    });
                }
                else if(editingLayerLock->is_anim_fx_group()) {
                    // PHASE10.1 — Anim-FX group: the particle FX inside travels the
                    // folder's motion path (with real trailing). Place a particle
                    // effect on a layer in this folder, then draw the path here.
                    text_label_centered(gui, "Anim-FX Group");
                    text_label_light(gui, "Put a particle effect on a layer in this\nfolder; it travels the motion path below.");
                    if(editingLayerLock->has_motion_path()) {
                        text_button(gui, "animfx edit path", "Edit Motion Path", { .wide = true, .onClick = [&] {
                            if(editingLayer.lock())
                                world.drawProg.switch_to_tool_ptr(std::make_unique<MotionPathTool>(world.drawProg, editingLayer.get_net_id()));
                        }});
                        text_button(gui, "animfx remove path", "Remove Motion Path", { .wide = true, .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(lk) lk->remove_motion_path(layerMan);
                        }});
                    }
                    else {
                        text_button(gui, "animfx add path", "Add Motion Path", { .wide = true, .onClick = [&] {
                            auto lk = editingLayer.lock();
                            if(!lk) return;
                            MotionPath& mp = lk->ensure_motion_path(layerMan);
                            mp.coords = world.drawData.cam.c;
                            const Vector2f ws = world.main.window.size.cast<float>();
                            mp.points = { Vector2f{ws.x() * 0.3f, ws.y() * 0.5f}, Vector2f{ws.x() * 0.7f, ws.y() * 0.5f} };
                            mp.controlIn = { Vector2f{0.0f, 0.0f}, Vector2f{0.0f, 0.0f} };
                            mp.controlOut = { Vector2f{0.0f, 0.0f}, Vector2f{0.0f, 0.0f} };
                            mp.nodeType = { 0, 0 };
                            mp.nodeSeconds = { 0.0f, 2.0f };
                            mp.nodeScale = { 1.0f, 1.0f };
                            mp.nodeEasing = { 1, 1 };
                            mp.nodeRotation = { 0.0f, 0.0f };
                            lk->commit_motion_path(layerMan);
                            world.drawProg.switch_to_tool_ptr(std::make_unique<MotionPathTool>(world.drawProg, editingLayer.get_net_id()));
                        }});
                    }
                    // Preview: run the FX along the path in drawing mode to check it
                    // (playback is otherwise reader-mode-only). Transient per-group.
                    if(editingLayerLock->has_motion_path()) {
                        bool previewing = editingLayerLock->get_folder().flipbookRuntime.previewPlaying;
                        text_button(gui, "animfx preview", previewing ? "Stop Preview" : "Preview Animation", {
                            .wide = true,
                            .onClick = [&] {
                                auto lk = editingLayer.lock();
                                if(lk && lk->is_folder()) {
                                    auto& rt = lk->get_folder().flipbookRuntime;
                                    rt.previewPlaying = !rt.previewPlaying;
                                    if(MotionPath* mp = lk->get_motion_path()) mp->reset();
                                }
                            }
                        });
                    }
                }
            }
            else {
                // Layer depth: the plane within its group. 0 = canvas plane;
                // >0 = farther (pans less); <0 = nearer (pans more). Inert
                // unless the layer is inside an active parallax group.
                slider_scalar_field(gui, "input parallax depth slider", "Parallax Depth", &depthValToEdit, -0.9f, 10.0f, {
                    .decimalPrecision = 2,
                    .onEdit = [&] {
                        auto editingLayerLock = editingLayer.lock();
                        if(editingLayerLock)
                            editingLayerLock->set_parallax_depth(layerMan, depthValToEdit);
                    }
                });
                if(depthValToEdit != 0.0f) {
                    if(layerMan.editing_layer_in_active_parallax_group())
                        text_label(gui, "Parallax active: editing this layer is\nlocked until depth returns to 0.");
                    else
                        text_label(gui, "Depth has no effect until this layer's\nfolder is a Parallax Scene.");
                }
            }
            // PHASE4 Part C (§11): lossless merge into the layer below.
            // Shown for plain DEFAULT layers only (named layers respawn
            // on load; folders are out of scope for v1) — the function
            // itself re-checks everything and toasts the reason if a
            // guard trips.
            if(!editingLayerLock->is_folder() && editingLayerLock->get_kind() == LayerKind::DEFAULT) {
                text_button(gui, "merge down button", "Merge Down", {
                    .wide = true,
                    .onClick = [&] {
                        if(selectedLayerIndices.size() == 1)
                            merge_layer_down(*selectedLayerIndices.begin());
                    }
                });
            }
        }
    }
}

std::optional<std::pair<NetworkingObjects::NetObjID, NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem>>> DrawingProgramLayerManagerGUI::try_to_create_in_proper_position(DrawingProgramLayerListItem* newItem) {
    using namespace NetworkingObjects;
    if(selectedLayerIndices.empty())
        return std::nullopt;
    auto& lastObjSelected = *selectedLayerIndices.rbegin();
    NetObjTemporaryPtr<DrawingProgramLayerListItem> objectPtr = get_layer_from_obj_index(lastObjSelected);
    if(objectPtr->is_folder()) {
        objectPtr->get_folder().isFolderOpen = true;
        return std::pair<NetworkingObjects::NetObjID, NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem>>{objectPtr.get_net_id(), objectPtr->get_folder().folderList->insert_and_send_create(objectPtr->get_folder().folderList, objectPtr->get_folder().folderList->begin(), newItem)};
    }
    NetObjTemporaryPtr<DrawingProgramLayerListItem> parentPtr = get_layer_parent_from_obj_index(lastObjSelected);
    return std::pair<NetworkingObjects::NetObjID, NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem>>{parentPtr.get_net_id(), parentPtr->get_folder().folderList->insert_and_send_create(parentPtr->get_folder().folderList, parentPtr->get_folder().folderList->get(objectPtr.get_net_id()), newItem)};
}

std::pair<NetworkingObjects::NetObjID, NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem>> DrawingProgramLayerManagerGUI::create_in_proper_position(DrawingProgramLayerListItem* newItem) {
    auto toRet = try_to_create_in_proper_position(newItem);
    if(!toRet)
        return {layerMan.layerTreeRoot.get_net_id(), layerMan.layerTreeRoot->get_folder().folderList->insert_and_send_create(layerMan.layerTreeRoot->get_folder().folderList, layerMan.layerTreeRoot->get_folder().folderList->begin(), newItem)};
    return toRet.value();
}

NetworkingObjects::NetObjOrderedListIterator<DrawingProgramLayerListItem> DrawingProgramLayerManagerGUI::create_layer(DrawingProgramLayerListItem* newItem) {
    auto& world = layerMan.drawP.world;
    class AddLayerWorldUndoAction : public WorldUndoAction {
        public:
            AddLayerWorldUndoAction(std::unique_ptr<DrawingProgramLayerListItemUndoData> initLayerData, uint32_t newPos, WorldUndoManager::UndoObjectID initParentUndoID, WorldUndoManager::UndoObjectID initUndoID):
                layerData(std::move(initLayerData)),
                pos(newPos),
                parentUndoID(initParentUndoID),
                undoID(initUndoID)
            {}
            std::string get_name() const override {
                return "Add Layer Item";
            }
            bool undo(WorldUndoManager& undoMan) override {
                std::optional<NetworkingObjects::NetObjID> toEraseParentID = undoMan.get_netid_from_undoid(parentUndoID);
                if(!toEraseParentID.has_value())
                    return false;
                std::optional<NetworkingObjects::NetObjID> toEraseID = undoMan.get_netid_from_undoid(undoID);
                if(!toEraseID.has_value())
                    return false;

                auto& layerList = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(toEraseParentID.value())->get_folder().folderList;
                auto it = layerList->get(toEraseID.value());
                layerData = std::make_unique<DrawingProgramLayerListItemUndoData>(it->obj->get_undo_data(undoMan));
                layerList->erase(layerList, it);
                return true;
            }
            bool redo(WorldUndoManager& undoMan) override {
                std::optional<NetworkingObjects::NetObjID> toInsertParentID = undoMan.get_netid_from_undoid(parentUndoID);
                if(!toInsertParentID.has_value())
                    return false;
                std::optional<NetworkingObjects::NetObjID> toInsertID = undoMan.get_netid_from_undoid(undoID);
                if(toInsertID.has_value())
                    return false;

                auto& layerList = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(toInsertParentID.value())->get_folder().folderList;
                auto insertedIt = layerList->emplace_direct(layerList, layerList->at(pos), undoMan.world, *layerData);
                undoMan.register_new_netid_to_existing_undoid(undoID, insertedIt->obj.get_net_id());
                layerData = nullptr;
                return true;
            }
            void scale_up(const WorldScalar& scaleAmount) override {
                if(layerData)
                    layerData->scale_up(scaleAmount);
            }
            ~AddLayerWorldUndoAction() {}

            std::unique_ptr<DrawingProgramLayerListItemUndoData> layerData;
            uint32_t pos;
            WorldUndoManager::UndoObjectID parentUndoID;
            WorldUndoManager::UndoObjectID undoID;
    };

    auto insertedLayerPair = create_in_proper_position(newItem);
    auto& it = insertedLayerPair.second;
    world.undo.push(std::make_unique<AddLayerWorldUndoAction>(std::make_unique<DrawingProgramLayerListItemUndoData>(it->obj->get_undo_data(world.undo)), it->pos, world.undo.get_undoid_from_netid(insertedLayerPair.first), world.undo.get_undoid_from_netid(it->obj.get_net_id())));
    world.main.g.gui.set_to_layout();
    refresh_gui_data();
    return it;
}

void DrawingProgramLayerManagerGUI::remove_layer(const GUIStuff::TreeListingObjIndexList& objIndex) {
    refresh_gui_data();

    auto& world = layerMan.drawP.world;
    // PHASE2 C5: defensive guard. The delete button is hidden for
    // named layers, but if any other code path ever reaches here
    // for a named layer (bulk-delete shortcut, undo replay, etc.)
    // we refuse — the manager's ensure_named_layers invariant
    // depends on the three named layers persisting.
    {
        auto layerToCheck = get_layer_from_obj_index(objIndex);
        if(layerToCheck && layerToCheck->get_kind() != LayerKind::DEFAULT)
            return;
    }
    class DeleteLayerWorldUndoAction : public WorldUndoAction {
        public:
            DeleteLayerWorldUndoAction(std::unique_ptr<DrawingProgramLayerListItemUndoData> initLayerData, uint32_t newPos, WorldUndoManager::UndoObjectID initParentUndoID, WorldUndoManager::UndoObjectID initUndoID):
                layerData(std::move(initLayerData)),
                pos(newPos),
                parentUndoID(initParentUndoID),
                undoID(initUndoID)
            {}
            std::string get_name() const override {
                return "Delete Layer Item";
            }
            bool undo(WorldUndoManager& undoMan) override {
                std::optional<NetworkingObjects::NetObjID> toInsertParentID = undoMan.get_netid_from_undoid(parentUndoID);
                if(!toInsertParentID.has_value())
                    return false;
                std::optional<NetworkingObjects::NetObjID> toInsertID = undoMan.get_netid_from_undoid(undoID);
                if(toInsertID.has_value())
                    return false;

                auto& layerList = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(toInsertParentID.value())->get_folder().folderList;
                auto insertedIt = layerList->emplace_direct(layerList, layerList->at(pos), undoMan.world, *layerData);
                undoMan.register_new_netid_to_existing_undoid(undoID, insertedIt->obj.get_net_id());
                layerData = nullptr;
                return true;
            }
            bool redo(WorldUndoManager& undoMan) override {
                std::optional<NetworkingObjects::NetObjID> toEraseParentID = undoMan.get_netid_from_undoid(parentUndoID);
                if(!toEraseParentID.has_value())
                    return false;
                std::optional<NetworkingObjects::NetObjID> toEraseID = undoMan.get_netid_from_undoid(undoID);
                if(!toEraseID.has_value())
                    return false;

                auto& layerList = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(toEraseParentID.value())->get_folder().folderList;
                auto it = layerList->get(toEraseID.value());
                layerData = std::make_unique<DrawingProgramLayerListItemUndoData>(it->obj->get_undo_data(undoMan));
                layerList->erase(layerList, it);
                return true;
            }
            void scale_up(const WorldScalar& scaleAmount) override {
                if(layerData)
                    layerData->scale_up(scaleAmount);
            }
            ~DeleteLayerWorldUndoAction() {}

            std::unique_ptr<DrawingProgramLayerListItemUndoData> layerData;
            uint32_t pos;
            WorldUndoManager::UndoObjectID parentUndoID;
            WorldUndoManager::UndoObjectID undoID;
    };


    auto parentPtr = get_layer_parent_from_obj_index(objIndex);
    auto layerPtr = get_layer_from_obj_index(objIndex);
    auto it = parentPtr->get_folder().folderList->get(layerPtr.get_net_id());
    world.undo.push(std::make_unique<DeleteLayerWorldUndoAction>(std::make_unique<DrawingProgramLayerListItemUndoData>(layerPtr->get_undo_data(world.undo)), it->pos, world.undo.get_undoid_from_netid(parentPtr.get_net_id()), world.undo.get_undoid_from_netid(layerPtr.get_net_id())));
    parentPtr->get_folder().folderList->erase(parentPtr->get_folder().folderList, it);
    world.main.g.gui.set_to_layout();
}

void DrawingProgramLayerManagerGUI::merge_layer_down(GUIStuff::TreeListingObjIndexList objIndex) {
    using namespace NetworkingObjects;
    auto& world = layerMan.drawP.world;

    NetObjTemporaryPtr<DrawingProgramLayerListItem> srcPtr = get_layer_from_obj_index(objIndex);
    NetObjTemporaryPtr<DrawingProgramLayerListItem> parentPtr = get_layer_parent_from_obj_index(objIndex);
    if(!srcPtr || srcPtr->is_folder())
        return;

    // PHASE4.md §11 refusal guards — no silent appearance changes.
    // Named-kind source: ensure_named_layers() recreates missing named
    // layers on load, so deleting one is pointless (it reappears empty).
    if(srcPtr->get_kind() != LayerKind::DEFAULT) {
        Logger::get().log("USERINFO", "Merge Down: Sketch/Color/Ink layers can't be merged away (they're recreated on load). Merge another layer into them instead.");
        return;
    }

    // Dest = the item directly below in render order. folderList draws
    // reversed (index 0 = topmost), so "below" is the NEXT list index.
    auto& folderList = parentPtr->get_folder().folderList;
    auto srcIt = folderList->get(srcPtr.get_net_id());
    const uint32_t destPos = srcIt->pos + 1;
    if(destPos >= folderList->size()) {
        Logger::get().log("USERINFO", "Merge Down: this is the bottom-most layer — nothing below to merge into.");
        return;
    }
    auto destIt = folderList->at(destPos);
    DrawingProgramLayerListItem& dest = *destIt->obj;
    if(dest.is_folder()) {
        Logger::get().log("USERINFO", "Merge Down: the item below is a folder — move the layer or merge inside the folder instead.");
        return;
    }

    // Layer alpha/blend composite per-LAYER; moving components from
    // under (or into) a non-default one would change how the art looks.
    auto has_default_compositing = [](const DrawingProgramLayerListItem& l) {
        return l.get_alpha() == 1.0f && l.get_blend_mode() == SerializedBlendMode::BLEND_SRC_OVER;
    };
    if(!has_default_compositing(*srcPtr) || !has_default_compositing(dest)) {
        Logger::get().log("USERINFO", "Merge Down: both layers need default alpha + blend mode (a merge under non-default compositing would change the art's appearance). Reset them, or use Flatten Layer.");
        return;
    }
    // Different parallax depths = different derived cameras = content
    // would visually jump. v1 requires both at 0 (PHASE4.md §11).
    if(srcPtr->get_parallax_depth() != 0.0f || dest.get_parallax_depth() != 0.0f) {
        Logger::get().log("USERINFO", "Merge Down: both layers need parallax depth 0.");
        return;
    }

    // Waypoints can't be cloned-then-erased: the erase callback sweeps
    // the waypoint out of wpGraph by id, orphaning the clone.
    auto& srcComponents = srcPtr->get_layer().components;
    for(auto it = srcComponents->begin(); it != srcComponents->end(); ++it) {
        if(it->obj->get_comp().get_type() == CanvasComponentType::WAYPOINT) {
            Logger::get().log("USERINFO", "Merge Down: this layer holds waypoint markers — move them to another layer first.");
            return;
        }
    }

    // Clone source components into dest ABOVE its existing content
    // (components draw begin→end, so end() = top-z; same-anchor pairs
    // insert in order, preserving the source's relative stacking).
    // Clone + erase (rather than moving the live containers) reuses the
    // proven flatten/vectorize undo machinery: place-undo + erase-undo
    // + the layer-delete undo from remove_layer = fully reversible.
    const size_t mergedCount = srcComponents->size();
    if(mergedCount > 0) {
        auto& destComponents = dest.get_layer().components;
        std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> toPlace;
        toPlace.reserve(mergedCount);
        for(auto it = srcComponents->begin(); it != srcComponents->end(); ++it)
            toPlace.emplace_back(destComponents->end(), new CanvasComponentContainer(world.netObjMan, *it->obj->get_data_copy()));
        const auto placed = layerMan.add_many_components_to_specific_layer(*destIt->obj, toPlace);
        for(auto& pit : placed)
            pit->obj->commit_update(layerMan.drawP);

        std::vector<CanvasComponentContainer::ObjInfo*> toErase;
        toErase.reserve(mergedCount);
        for(auto it = srcComponents->begin(); it != srcComponents->end(); ++it)
            toErase.push_back(&(*it));
        layerMan.erase_component_container(toErase);
    }

    // If the merged-away layer was the edit target, hand editing to dest
    // so the artist's next stroke lands where the content went.
    const std::string destName = dest.get_name();
    if(layerMan.editingLayer.lock().get() == srcPtr.get())
        layerMan.editingLayer = world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(destIt->obj.get_net_id());

    remove_layer(objIndex);

    Logger::get().log("USERINFO",
        "Merged " + std::to_string(mergedCount) + " component(s) down into '" + destName + "'.");
}

void DrawingProgramLayerManagerGUI::editing_layer_check() {
    auto& world = layerMan.drawP.world;
    class EditLayerWorldUndoAction : public WorldUndoAction {
        public:
            EditLayerWorldUndoAction(std::unique_ptr<DrawingProgramLayerListItemMetaInfo> initData, WorldUndoManager::UndoObjectID initUndoID):
                data(std::move(initData)),
                undoID(initUndoID)
            {}
            std::string get_name() const override {
                return "Edit Layer";
            }
            bool undo(WorldUndoManager& undoMan) override {
                return undo_redo(undoMan);
            }
            bool redo(WorldUndoManager& undoMan) override {
                return undo_redo(undoMan);
            }
            bool undo_redo(WorldUndoManager& undoMan) {
                std::optional<NetworkingObjects::NetObjID> toModifyID = undoMan.get_netid_from_undoid(undoID);
                if(!toModifyID.has_value())
                    return false;
                auto objPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<DrawingProgramLayerListItem>(toModifyID.value());
                auto newData = std::make_unique<DrawingProgramLayerListItemMetaInfo>(objPtr->get_metainfo());
                objPtr->set_metainfo(undoMan.world.drawProg.layerMan, *data);
                data = std::move(newData);
                return true;
            }
            ~EditLayerWorldUndoAction() {}

            std::unique_ptr<DrawingProgramLayerListItemMetaInfo> data;
            WorldUndoManager::UndoObjectID undoID;
    };

    auto editingLayerLock = editingLayer.lock();
    if(editingLayerLock) {
        const DrawingProgramLayerListItemMetaInfo& currentMetaData = editingLayerLock->get_metainfo();
        if(currentMetaData != editingLayerOldMetainfo.value())
            world.undo.push(std::make_unique<EditLayerWorldUndoAction>(std::make_unique<DrawingProgramLayerListItemMetaInfo>(editingLayerOldMetainfo.value()), world.undo.get_undoid_from_netid(editingLayer.get_net_id())));
    }

    if(selectedLayerIndices.size() == 1) {
        editingLayer = get_layer_from_obj_index(*selectedLayerIndices.begin());
        editingLayerOldMetainfo = editingLayer.lock()->get_metainfo();
    }
    else {
        editingLayer.reset();
        editingLayerOldMetainfo = std::nullopt;
    }
}
