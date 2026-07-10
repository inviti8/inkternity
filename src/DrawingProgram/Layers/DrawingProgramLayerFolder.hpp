#pragma once
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include <include/core/SkCanvas.h>
#include "../../DrawData.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"
#include "FlipbookMode.hpp"

class DrawingProgramLayerListItem;
class DrawingProgramLayerManager;

class DrawingProgramLayerFolder {
    public:
        void draw(SkCanvas* canvas, const DrawData& drawData) const;
        // PHASE10 Feature B — draw only the single active frame of a flip-book
        // group (flipbookRuntime.frameIndex, which IS the panel/child index)
        // instead of compositing every child. Callers gate on
        // DrawingProgramLayerListItem::is_flipbook_group(). See PHASE10.md.
        void draw_flipbook_frame(SkCanvas* canvas, const DrawData& drawData) const;
        // Number of frames (direct children) in the group.
        size_t frame_count() const;
        // PHASE10 Feature B — playback runtime helpers (operate on
        // flipbookRuntime). begin (re)starts a playthrough at the first frame;
        // advance accumulates deltaTime and steps frames per play style. invert
        // flips the advance direction + start frame (bottom -> top). Both are
        // no-ops unless there are >1 frames. See docs/design/PHASE10.md §B.4.
        void flipbook_begin(bool invert);
        void flipbook_advance(FlipbookPlayStyle style, bool invert, float fps, float deltaTime);
        void set_component_list_callbacks(DrawingProgramLayerManager& layerMan);
        void get_flattened_component_list(std::vector<CanvasComponentContainer::ObjInfo*>& objList) const;
        void set_to_erase();
        NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> get_initial_editing_layer() const;
        void scale_up(const WorldScalar& scaleUpAmount);

        NetworkingObjects::NetObjOwnerPtr<NetworkingObjects::NetObjOrderedList<DrawingProgramLayerListItem>> folderList;
        bool isFolderOpen = false;

        // PHASE10 Feature B — runtime playback state for a flip-book group.
        // NEVER serialized or net-synced: it's per-viewer session state (like
        // the reader-mode toggle), advanced by the playback clock in
        // World::focus_update (B-M3) and read by draw_flipbook_frame. See
        // docs/design/PHASE10.md Feature B §B.3-B.4.
        struct FlipbookRuntime {
            size_t frameIndex = 0;        // panel-order frame currently shown (0 = top row = frame 1)
            double frameTimer = 0.0;      // seconds accumulated toward the next frame
            bool playing = false;         // is the state machine currently advancing?
            bool pingPongReversing = false;
            bool onScreenLast = false;    // AUTO trigger rising-edge tracking
            bool previewPlaying = false;  // drawing-mode debug override (transient)
            bool onionSkin = true;        // ghost adjacent frames while editing this group
        } flipbookRuntime;

        void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version, DrawingProgramLayerManager& layerMan);
        void save_file(cereal::PortableBinaryOutputArchive& a) const;

        void get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const;
        void erase_invalid_components();
};
