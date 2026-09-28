#pragma once
// ANIMATED_IMPORTS.md Phase 1 — the on-canvas representation of an imported 2D
// skeletal rig (DragonBones), drawn LIVE via the embedded runtime + Skia adapter
// (SkeletalRig facade). Unlike ArmatureCanvasComponent (a baked static raster), this
// component holds a live rig and advances it each frame.
//
// The rig's three source blobs (skeleton JSON, atlas JSON, atlas PNG) live in the
// ResourceManager (embedded in the save, net-synced, dedup'd) and are referenced by
// id; the live SkeletalRig is rebuilt lazily from them (never serialized). All rig
// use is gated on HVYM_HAS_DRAGONBONES so web / non-Conan builds still link (the
// component then draws a placeholder).

#include "../SharedTypes.hpp"
#include "CanvasComponent.hpp"
#include "../CoordSpaceHelper.hpp"

#include <Helpers/NetworkingObjects/NetObjID.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef HVYM_HAS_DRAGONBONES
namespace AI { class SkeletalRig; }
#endif

// How a placed rig starts playing (mirrors ParticlePlayMode). AUTO loops its clip
// while on view; ON_TOUCH rests at the setup pose until a reader-mode tap plays the
// clip once (then holds the last frame).
enum SkeletalPlayMode : uint8_t {
    SKELETAL_PLAY_AUTO     = 0,
    SKELETAL_PLAY_ON_TOUCH = 1
};

class SkeletalCanvasComponent : public CanvasComponent {
public:
    SkeletalCanvasComponent();
    ~SkeletalCanvasComponent() override;

    virtual CanvasComponentType get_type() const override;
    virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
    virtual void load(cereal::PortableBinaryInputArchive& a) override;
    virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
    virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
    std::unique_ptr<CanvasComponent> get_data_copy() const override;
    virtual void set_data_from(const CanvasComponent& other) override;
    virtual void get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const override;
    virtual void remap_resource_ids(const std::unordered_map<NetworkingObjects::NetObjID, NetworkingObjects::NetObjID>& resourceOldToNewMap) override;

    virtual void update(DrawingProgram& drawP) override;

    // Instance state (serialized). The three resource ids are the embedded rig.
    struct Data {
        NetworkingObjects::NetObjID skeletonResId{};   // DragonBones *_ske.json
        NetworkingObjects::NetObjID atlasJsonResId{};  // *_tex.json
        NetworkingObjects::NetObjID atlasPngResId{};   // *_tex.png
        Vector2f pos   = {0.0f, 0.0f};                 // world anchor
        float    scale = 1.0f;                          // uniform scale
        std::string clip;                               // current animation ("" = first/none)
        bool     playing = true;                         // AUTO master enable (advance the loop)
        // Half-extents of the selection/collision box (world units, pre-scale).
        Vector2f halfExtent = {256.0f, 256.0f};
        uint8_t  playMode = SKELETAL_PLAY_AUTO;          // SkeletalPlayMode (added INFPNT000034)
    } d;

    // Request a one-shot play (reader-mode tap on an ON_TOUCH rig). Picked up next
    // update(). Mirrors ParticleCanvasComponent::trigger_touch().
    void trigger_touch();
    // Reset live playback to match the current playMode (call after flipping the
    // mode on a selected rig so the change takes effect immediately).
    void apply_play_mode();

    // Clip (animation) selection. Names come from the live rig, so the list is empty
    // until the rig has been built (first draw). active_clip() is the clip currently
    // driving the rig; set_clip() switches it and applies live per the play mode.
    std::vector<std::string> clip_names() const;
    std::string active_clip() const;
    void set_clip(const std::string& name);

    // Re-skin (atlas hot-swap): point the rig at a new texture atlas (its two resource
    // ids), keeping the same skeleton + animations, and force a rebuild. Slot names in
    // the new atlas must match for the art to bind. The current clip keeps playing.
    void reskin(NetworkingObjects::NetObjID newAtlasJsonId, NetworkingObjects::NetObjID newAtlasPngId);

private:
    virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
    virtual void initialize_draw_data(DrawingProgram& drawP) override;
    virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
    virtual SCollision::AABB<float> get_obj_coord_bounds() const override;
    void create_collider();

    SCollision::BVHContainer<float> collisionTree;

#ifdef HVYM_HAS_DRAGONBONES
    // Live rig, rebuilt lazily from the resources. Mutable because draw() is const
    // but must lazily build the rig on first paint. Not serialized, not copied.
    mutable std::unique_ptr<AI::SkeletalRig> rig;
    mutable bool loadAttempted = false;
    // The clip actually driving the rig (d.clip resolved against the rig's animation
    // list, so an empty d.clip still knows what to play on touch).
    mutable std::string activeClip;
    std::chrono::steady_clock::time_point lastTick{};
    bool pendingTouch = false;   // a trigger_touch() awaiting the next update()
    void ensure_rig(class ResourceManager& rMan) const;
    // The rig's drawn bounds in local space, grown monotonically as it animates so
    // the clip/collider box covers the widest pose (a jump won't crop). Empty box +
    // false until the rig has produced geometry.
    bool rigBoundsKnown = false;
    Vector2f rigMinLocal = {0.0f, 0.0f};
    Vector2f rigMaxLocal = {0.0f, 0.0f};
    void refresh_rig_bounds();
#endif
};
