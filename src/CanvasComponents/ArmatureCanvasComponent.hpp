#pragma once
// PHASE9 (docs/design/PHASE9.md Decision §7, ARMATURE-SCHEMA.md §5) — M5.
//
// A re-editable "smart object": the on-canvas representation of a posed armature.
// Its `d` struct IS the instance-state (rig ref, pose keyed to bone names, camera,
// light) and it owns the baked RGBA raster it draws. Double-clicking it reopens
// the 3D editor (ArmatureModalScreen) seeded from `d`; Bake writes `d` + the raster
// back. Serialized append-and-gate like the shape components (gated >= 0.21.0).
//
// Customization (shape-key sliders, material colors, height) is deferred to a
// follow-on and will append under a later version gate. `d` holds plain types
// only (no Eigen/WorldVec) per [[project_layer_metainfo_eigen_gotcha]].

#include "../SharedTypes.hpp"
#include "CanvasComponent.hpp"
#include "../CoordSpaceHelper.hpp"

#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <include/core/SkImage.h>

#include <cstdint>
#include <string>
#include <vector>

class ArmatureCanvasComponent : public CanvasComponent {
public:
    virtual CanvasComponentType get_type() const override;
    virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
    virtual void load(cereal::PortableBinaryInputArchive& a) override;
    virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
    virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
    std::unique_ptr<CanvasComponent> get_data_copy() const override;
    virtual void set_data_from(const CanvasComponent& other) override;
    // Keep the embedded model resource alive across save/load + copy/paste (M7).
    virtual void get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const override;
    virtual void remap_resource_ids(const std::unordered_map<NetworkingObjects::NetObjID, NetworkingObjects::NetObjID>& resourceOldToNewMap) override;

    struct PoseEntry {
        std::string bone;
        // local-space pose rotation relative to bind, quaternion xyzw
        float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
        bool operator==(const PoseEntry&) const = default;
        template <class Archive> void serialize(Archive& a) { a(bone, qx, qy, qz, qw); }
    };

    struct MatColor {
        std::string material;
        float r = 0.8f, g = 0.8f, b = 0.8f, a = 1.0f;
        bool operator==(const MatColor&) const = default;
        template <class Archive> void serialize(Archive& ar) { ar(material, r, g, b, a); }
    };

    struct Data {
        int rigId = 0;                       // 0 = bundled default (custom rigs deferred)
        // M7: an external model loaded from file, embedded once in the
        // ResourceManager and referenced by id. {} (zero) = the bundled default
        // rig. May be a poseable armature OR a flattened static reference mesh;
        // which one is decided by the loader at edit time (ArmatureModel::is_static).
        NetworkingObjects::NetObjID modelResourceId{};
        std::vector<PoseEntry> pose;         // only non-identity joints

        // Orbit camera (matches Armature::OrbitCamera).
        float camYaw = 0.0f, camPitch = 0.0f, camDist = 3.0f;
        float camTx = 0.0f, camTy = 0.0f, camTz = 0.0f;
        // Camera lens (0.25.0+): vertical FOV in degrees + orthographic toggle.
        float fovDeg = 40.0f;
        bool ortho = false;

        // Lighting (matches Armature::Lighting).
        float lightAz = 0.3f, lightEl = 0.6f, lightInt = 0.95f, lightAmb = 0.5f, lightSky = 0.18f;

        // Customization (M5.1). height = uniform bone-length scale (0.22.0+).
        float height = 1.0f;
        // Per-material color overrides (0.23.0+); empty = use the glb defaults.
        std::vector<MatColor> materialColors;
        // Shape-key slider values (0.24.0+), one per slider in the fixed config
        // order; empty = all default (0). See ArmatureModalScreen's slider table.
        std::vector<float> shapeSliders;

        // Baked raster the component draws (square, unpremultiplied RGBA8).
        int rasterDim = 0;
        std::vector<uint8_t> rasterRGBA;

        bool operator==(const Data&) const = default;
    } d;

    // Runtime-only hint (NOT in Data → not serialized): the embedded mesh is Z-up
    // and needs the −90°-about-X correction on (re)load, so the editor reload
    // matches the placed raster. Set for reangle meshes at placement. Not
    // persisted — after save/reload, re-editing shows the raw orientation until
    // the service emits Y-up (the preferred permanent fix).
    bool zUpToYUpHint = false;

    // Replace the baked raster (called by the modal on Bake). Invalidates cache.
    void set_raster(std::vector<uint8_t> rgba, int dim);

private:
    virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
    virtual void initialize_draw_data(DrawingProgram& drawP) override;
    virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
    virtual SCollision::AABB<float> get_obj_coord_bounds() const override;

    void create_collider();
    void ensure_image() const;          // lazily (re)build cachedImage_ from d.rasterRGBA

    mutable sk_sp<SkImage> cachedImage_;
    SCollision::BVHContainer<float> collisionTree;
};
