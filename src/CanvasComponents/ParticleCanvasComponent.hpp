#pragma once
#include "CanvasComponent.hpp"
#include "../CoordSpaceHelper.hpp"
#include <Helpers/SCollision.hpp>
#include <memory>
#include <string>

// PHASE5 (docs/design/PHASE5.md): a TimelineFX particle effect as a canvas
// component. The authored effect *package* is embedded verbatim in `d` (saved,
// synced, host-authored); all runtime state — the TimelineFX library + effect
// manager, the decoded shape/ramp SkImages, and the SkRuntimeEffect port of
// timelinefx.frag — lives behind a PIMPL so timelinefx.h stays out of the rest
// of the C++23 app. Renders live, outside the cache, ticked off deltaTime
// (the animated-GIF pattern: invalidate the cache region each frame).
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

        // Serialized authoring data.
        struct Data {
            std::string packageBytes;   // the .tfx effect package, embedded verbatim
            std::string effectName;     // which effect within the package to play
            uint32_t seed = 12345;      // deterministic replay seed
            float localScale = 1.0f;    // tfx world-unit -> canvas local-unit
            float radius = 600.0f;      // local half-extent for bounds / collision
        } d;

    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;

        void create_collider();
        void ensure_runtime() const;  // lazily builds the PIMPL runtime from d

        struct Runtime;
        mutable std::unique_ptr<Runtime> rt;
        SCollision::BVHContainer<float> collisionTree;
};
