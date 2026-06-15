#pragma once
#include "CanvasComponent.hpp"
#include "../CoordSpaceHelper.hpp"
#include <Helpers/SCollision.hpp>
#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <cstdint>
#include <memory>
#include <string>

// Playback trigger for a placed effect (PHASE5.1 polish).
enum ParticlePlayMode : uint8_t {
    PARTICLE_PLAY_AUTO     = 0,   // Continuous loops; Finite (re)plays on becoming visible
    PARTICLE_PLAY_ON_TOUCH = 1    // does not play until touched (then plays once)
};

// PHASE5.1 (docs/design/PHASE5.1.md): a placed legacy TimelineFX (.eff) effect
// as a canvas component. References an imported library by ResourceManager id +
// an effect name; the parsed library lives in the DrawingProgram's FxLibraryStore
// (or is re-parsed from the embedded resource on load). Runtime state — the
// TLFX particle manager + spawned effect — lives behind a PIMPL so the legacy
// headers stay out of the rest of the app. Renders live, outside the cache,
// centered at the component's local origin (the GIF pattern: invalidate the
// cache region each frame). 2D-only; honors each effect's Continuous/Finite flag.
class ParticleCanvasComponent : public CanvasComponent {
    public:
        ParticleCanvasComponent();
        ~ParticleCanvasComponent() override;

        virtual CanvasComponentType get_type() const override;
        virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load(cereal::PortableBinaryInputArchive& a) override;
        virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
        virtual std::unique_ptr<CanvasComponent> get_data_copy() const override;
        virtual void set_data_from(const CanvasComponent& other) override;
        virtual void update(DrawingProgram& drawP) override;
        // Resource bookkeeping so the embedded .eff survives save/load + copy/paste
        // (mirrors ImageCanvasComponent): declare the library resource as used, and
        // remap its id when resources are reassigned on load.
        virtual void get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const override;
        virtual void remap_resource_ids(const std::unordered_map<NetworkingObjects::NetObjID, NetworkingObjects::NetObjID>& resourceOldToNewMap) override;

        // Request a play (ON_TOUCH effects, or replay any). Picked up next update.
        void trigger_touch();

        // Serialized authoring data.
        struct Data {
            NetworkingObjects::NetObjID libraryResourceId;  // embedded .eff asset
            std::string effectName;                         // which effect to play
            uint32_t seed = 12345;                          // deterministic replay seed
            float localScale = 3.0f;                        // effect world-unit -> canvas local-unit
            float radius = 600.0f;                          // local half-extent (derived from scale in create_collider)
            uint8_t playMode = PARTICLE_PLAY_AUTO;          // ParticlePlayMode
        } d;

    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;

        void create_collider();
        // Builds the PIMPL runtime: resolves the library + particle manager
        // (needs drawP). draw() passes nullptr and relies on update()/init having
        // built it first (focus_update runs update before draw each frame).
        void ensure_runtime(DrawingProgram* drawP) const;
        void play_effect() const;   // (re)spawn the effect instance — fresh play

        struct Runtime;
        mutable std::unique_ptr<Runtime> rt;
        bool pendingTouch = false;  // a trigger_touch() awaiting the next update
        SCollision::BVHContainer<float> collisionTree;
};
