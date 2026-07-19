#pragma once
#include <Helpers/NetworkingObjects/NetObjManager.hpp>
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include <Helpers/NetworkingObjects/DelayUpdateSerializedClassManager.hpp>
#include <unordered_set>
#include "DrawingProgramLayerFolder.hpp"
#include "DrawingProgramLayer.hpp"
#include "LayerKind.hpp"
#include "FlipbookMode.hpp"
#include "MotionPath.hpp"
#include "SerializedBlendMode.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"

class DrawingProgramLayerFolder;
class DrawingProgramLayer;
class World;
class DrawingProgramLayerManager;

struct DrawingProgramLayerListItemMetaInfo {
    std::string name;
    float alpha = 1.0f;
    SerializedBlendMode blendMode = SerializedBlendMode::BLEND_SRC_OVER;
    // PARALLAX-SCENES (was PHASE4 Part A): multiplane parallax, now split
    // across two levels — see docs/design/PARALLAX-SCENES.md §3-§4.
    //   parallaxDepth  — LAYER property: the plane within the scene. 0 =
    //                    canvas plane (default); > 0 = farther (pans less);
    //                    (-1, 0) = nearer (pans more). Inert unless the
    //                    layer is inside an active parallax group.
    //   parallaxAnchorX/Y + parallaxRefScale — FOLDER (group) property:
    //                    the scene's neutral viewpoint position and the zoom
    //                    it was framed at. refScale == 0 means "not a
    //                    parallax group" (the enable flag + value in one).
    // Both folders and layers share this struct (every list item carries a
    // DisplayData), so each level uses only its own fields. Anchor + refScale
    // are stored as WorldScalars rather than a WorldVec: an Eigen member
    // breaks MSVC's SMF-trait evaluation for the recursive
    // optional<vector<DrawingProgramComponentUndoData>> in the undo data.
    float parallaxDepth = 0.0f;
    WorldScalar parallaxAnchorX{0};
    WorldScalar parallaxAnchorY{0};
    WorldScalar parallaxRefScale{0};
    // PHASE10 Feature B — flip-book group (FOLDER property; see FlipbookMode.hpp
    // and docs/design/PHASE10.md). flipbookFps > 0 marks the folder a flip-book;
    // the rest are the play-style / trigger / frame-order axes. Plain
    // enum/float/bool only — no Eigen (undo-struct SMF-trait gotcha, above).
    float flipbookFps = 0.0f;
    FlipbookPlayStyle flipbookPlayStyle = FlipbookPlayStyle::ONCE;
    FlipbookTriggerMode flipbookTriggerMode = FlipbookTriggerMode::AUTO;
    bool flipbookInvert = false;
    // PHASE10.1 — Anim-FX group (FOLDER property): the particle FX inside it
    // travels the folder's MotionPath (with real trailing). Mutually exclusive
    // with parallax/flip-book (enforced by the Folder Mode dropdown).
    bool animFxGroup = false;
    // Scale-aware "on view" (FOLDER property): min on-screen size (longest
    // content edge, screen px) before an AUTO flip-book/anim-fx group fires.
    // 0 = disabled (any overlap triggers). See DisplayData::autoTriggerMinScreenPx.
    float autoTriggerMinScreenPx = 0.0f;
    bool operator==(const DrawingProgramLayerListItemMetaInfo&) const = default;
};

struct DrawingProgramLayerListItemUndoData {
    WorldUndoManager::UndoObjectID undoID;
    WorldUndoManager::UndoObjectID containerUndoID; // Contains either the undoID of folderData->folderList, or layerData->components. Allows for undo to directly point to those two containers
    DrawingProgramLayerListItemMetaInfo metaInfo;
    std::optional<std::vector<DrawingProgramLayerListItemUndoData>> folderData;
    std::optional<std::vector<DrawingProgramComponentUndoData>> layerData;
    void scale_up(const WorldScalar& scaleUpAmount);
};

class DrawingProgramLayerListItem {
    public:
        DrawingProgramLayerListItem();
        DrawingProgramLayerListItem(NetworkingObjects::NetObjManager& netObjMan, const std::string& initName, bool isFolder, LayerKind initKind = LayerKind::DEFAULT);
        DrawingProgramLayerListItem(World& w, const DrawingProgramLayerListItemUndoData& undoData);

        LayerKind get_kind() const { return kind; }
        DrawingProgramLayerListItemUndoData get_undo_data(WorldUndoManager& u) const;
        // Recursively deep-copy this item into a brand-new tree with FRESH net
        // ids — a true duplicate, unlike the undo-data roundtrip (which reuses
        // ids and drops the folder-level parallax / flip-book / anim-fx / motion-
        // path state). Copies every display + folder-mode field and the motion
        // path; clones components via get_data_copy. Transient runtime (flip-book
        // playback, draw cache, list callbacks) is NOT copied — the insert path
        // wires those up. The returned raw item is unowned; hand it to an insert.
        DrawingProgramLayerListItem* deep_copy(NetworkingObjects::NetObjManager& netObjMan) const;
        bool is_folder() const;
        DrawingProgramLayerFolder& get_folder() const;
        DrawingProgramLayer& get_layer() const;
        static void register_class(World& w);
        void reassign_netobj_ids_call();
        void set_to_erase();
        void set_component_list_callbacks(DrawingProgramLayerManager &layerMan);
        void draw(SkCanvas* canvas, const DrawData& drawData) const;
        void set_name(NetworkingObjects::DelayUpdateSerializedClassManager& delayedNetObjMan, const std::string& newName) const;
        const std::string& get_name() const;
        void get_flattened_component_list(std::vector<CanvasComponentContainer::ObjInfo*>& objList) const;
        void get_flattened_layer_list(std::vector<DrawingProgramLayerListItem*>& objList);
        void scale_up(const WorldScalar& scaleUpAmount);
        void get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const;

        void set_alpha(DrawingProgramLayerManager& layerMan, float newAlpha) const;
        float get_alpha() const;

        void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version, DrawingProgramLayerManager& layerMan);
        void save_file(cereal::PortableBinaryOutputArchive& a) const;

        void set_visible(DrawingProgramLayerManager& layerMan, bool newVisible) const;
        bool get_visible() const;

        void set_blend_mode(DrawingProgramLayerManager& layerMan, SerializedBlendMode newBlendMode) const;
        SerializedBlendMode get_blend_mode() const;

        // PARALLAX-SCENES: depth is the layer's plane within its group;
        // anchor/scale live on the group folder, so setting depth no longer
        // recomputes an anchor (at the group's neutral viewpoint a depth
        // change is jump-free anyway). Clamps to (-0.95, 1000).
        void set_parallax_depth(DrawingProgramLayerManager& layerMan, float newDepth) const;
        // Group (folder) parallax: anchor = scene neutral-viewpoint position,
        // refScale = the zoom it was framed at (refScale == 0 disables).
        void set_parallax_group(DrawingProgramLayerManager& layerMan, const WorldScalar& refScale, const WorldVec& anchor) const;
        // Raw restore of the full parallax tuple (undo/metainfo) — no recompute.
        void set_parallax_raw(DrawingProgramLayerManager& layerMan, float depth, const WorldScalar& refScale, const WorldVec& anchor) const;
        float get_parallax_depth() const;
        WorldScalar get_parallax_ref_scale() const;
        WorldVec get_parallax_anchor() const;
        bool is_parallax_group() const;
        // Recursive cache-bypass test: is there a visible depth!=0 layer
        // somewhere under an active parallax group? (ancestorGroupActive
        // carries whether an ancestor folder is already a parallax group.)
        bool has_active_parallax_descendant(bool ancestorGroupActive) const;
        // Recursive lookup: is the item with `targetId` inside an active
        // parallax group (nearest-ancestor rule)? Used by the GUI for the
        // depth "no active group" hint and the edit-lock test.
        bool target_in_active_parallax_group(NetworkingObjects::NetObjID targetId, bool ancestorGroupActive) const;

        // PHASE10 Feature B — flip-book group (folder). fps == 0 disables
        // (folder composites its children normally); fps > 0 makes it a
        // flip-book that draws one frame at a time. See FlipbookMode.hpp and
        // docs/design/PHASE10.md Feature B.
        void set_flipbook_fps(DrawingProgramLayerManager& layerMan, float fps) const;
        void set_flipbook_play_style(DrawingProgramLayerManager& layerMan, FlipbookPlayStyle style) const;
        void set_flipbook_trigger_mode(DrawingProgramLayerManager& layerMan, FlipbookTriggerMode trigger) const;
        void set_flipbook_invert(DrawingProgramLayerManager& layerMan, bool invert) const;
        float get_flipbook_fps() const;
        FlipbookPlayStyle get_flipbook_play_style() const;
        FlipbookTriggerMode get_flipbook_trigger_mode() const;
        bool get_flipbook_invert() const;
        bool is_flipbook_group() const;

        // PHASE10.1 — Anim-FX group (folder): its particle FX travels the folder's
        // MotionPath. See PHASE10.1.md.
        void set_anim_fx_group(DrawingProgramLayerManager& layerMan, bool on) const;
        bool is_anim_fx_group() const;
        // Scale-aware "on view": minimum on-screen size (longest content edge,
        // screen px) before an AUTO flip-book / anim-fx group fires. 0 disables
        // (any viewport overlap triggers). See DisplayData::autoTriggerMinScreenPx.
        void set_auto_trigger_min_screen_px(DrawingProgramLayerManager& layerMan, float px) const;
        float get_auto_trigger_min_screen_px() const;
        // Recursive cache-bypass test: is there a visible flip-book group (with
        // >1 frame) anywhere in this subtree? Mirrors has_active_parallax_descendant
        // — a live flip-book can't share the static composite cache.
        bool has_active_flipbook_descendant() const;

        // MOTION-PATH.md P1 — optional bezier animation path bound to this
        // (flip-book) folder. Null until the artist adds one; owned as a NetObj
        // like nameData/displayData, so it dies with the folder. See MotionPath.hpp.
        bool has_motion_path() const;
        MotionPath* get_motion_path() const;                       // null if none
        MotionPath& ensure_motion_path(DrawingProgramLayerManager& layerMan);   // create if absent
        void remove_motion_path(DrawingProgramLayerManager& layerMan);
        // Push the (locally-edited) path to peers + relayout. Whole-struct sync.
        void commit_motion_path(DrawingProgramLayerManager& layerMan) const;
        // PHASE10 Feature B — per-frame flip-book playback tick (recurses the
        // tree). Advances each live flip-book group's frame per its play style
        // + trigger, but ONLY in a playback context (viewerActive OR the group's
        // transient preview override); otherwise holds the "edit frame" (the
        // selected direct child). See docs/design/PHASE10.md §B.4.
        void update_flipbook_playback(float deltaTime, bool viewerActive, const DrawData& drawData, const DrawingProgramLayerListItem* editingLayer);
        // ON_TOUCH trigger dispatch (recurses): start any visible ON_TOUCH
        // flip-book group whose content is under the tap collider. Mirrors the
        // particle trigger_touch path.
        void trigger_touch_flipbook(const CoordSpaceHelper& camCoords, const SCollision::ColliderCollection<WorldScalar>& tapCollider);

        void set_metainfo(DrawingProgramLayerManager& layerMan, const DrawingProgramLayerListItemMetaInfo& metaInfo);
        DrawingProgramLayerListItemMetaInfo get_metainfo() const;

        uint32_t get_component_count() const;
        // Approximate uncompressed in-RAM footprint (bytes) of every component
        // under this item — sums each component's get_memory_size_bytes.
        uint64_t get_total_memory_bytes() const;

        void erase_invalid_components();
    private:
        static void write_constructor_func(const NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem>& o, cereal::PortableBinaryOutputArchive& a);
        static void read_constructor_func(const NetworkingObjects::NetObjTemporaryPtr<DrawingProgramLayerListItem>& o, cereal::PortableBinaryInputArchive& a, const std::shared_ptr<NetServer::ClientData>& c);
        std::unique_ptr<DrawingProgramLayerFolder> folderData;
        std::unique_ptr<DrawingProgramLayer> layerData;
        struct NameData {
            std::string name;
            template <typename Archive> void serialize(Archive& a) {
                a(name);
            }
        };
        struct DisplayData {
            float alpha = 1.0f;
            bool visible = true;
            SerializedBlendMode blendMode = SerializedBlendMode::BLEND_SRC_OVER;
            // PARALLAX-SCENES — see DrawingProgramLayerListItemMetaInfo for
            // semantics (depth = layer plane; anchor + refScale = group).
            // File-load is version-gated in load_file (pre-INFPNT000013 read
            // only the first three fields; INFPNT000013-20 read depth+anchor
            // but no refScale); the wire always carries all of them
            // (mixed-version collab already requires matching builds).
            float parallaxDepth = 0.0f;
            WorldScalar parallaxAnchorX{0};
            WorldScalar parallaxAnchorY{0};
            WorldScalar parallaxRefScale{0};
            // PHASE10 Feature B — flip-book group (folder). flipbookFps > 0
            // enables; file-load is version-gated in load_file (< 0.27.0 read
            // only the parallax layout, no flip-book fields). See FlipbookMode.hpp.
            float flipbookFps = 0.0f;
            FlipbookPlayStyle flipbookPlayStyle = FlipbookPlayStyle::ONCE;
            FlipbookTriggerMode flipbookTriggerMode = FlipbookTriggerMode::AUTO;
            bool flipbookInvert = false;
            // PHASE10.1 — Anim-FX group (folder). File-load gated in load_file
            // (< 0.29.0 has no such field). See PHASE10.1.md.
            bool animFxGroup = false;
            // Scale-aware "on view" (FOLDER property): minimum on-screen size —
            // the longest edge of the group's content, in screen pixels — before
            // an AUTO flip-book / anim-fx group actually fires. 0 = disabled (any
            // viewport overlap triggers, the pre-0.31 behavior). Lets a group
            // nested deep in a zoom (a butterfly inside a star inside a painting)
            // stay dormant until it's zoomed in far enough to be meaningfully
            // visible. File-load gated in load_file (< 0.31.0 has no such field).
            float autoTriggerMinScreenPx = 0.0f;
            template <typename Archive> void serialize(Archive& a) {
                a(alpha, visible, blendMode, parallaxDepth, parallaxAnchorX, parallaxAnchorY, parallaxRefScale,
                  flipbookFps, flipbookPlayStyle, flipbookTriggerMode, flipbookInvert, animFxGroup,
                  autoTriggerMinScreenPx);
            }
        };
        NetworkingObjects::NetObjOwnerPtr<NameData> nameData;
        NetworkingObjects::NetObjOwnerPtr<DisplayData> displayData;
        // MOTION-PATH.md P1 — optional; null unless this folder has an animation
        // path. Synced/serialized like displayData but gated by a presence bool.
        NetworkingObjects::NetObjOwnerPtr<MotionPath> motionPath;
        // Set at construction; immutable for the lifetime of the layer
        // (named-kind invariants are enforced by the layer manager, not
        // by mutating this field). NetObj-synced via constructor data;
        // file-saved gated at format version >= 0.8.
        LayerKind kind = LayerKind::DEFAULT;
};
