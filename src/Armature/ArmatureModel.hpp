#pragma once
// PHASE9 (docs/design/PHASE9.md, ARMATURE-SCHEMA.md) — M2.
//
// A loaded, skinned glTF/.glb model: CPU geometry + skin + the matrices needed
// to render it, plus its GL resources. M2 renders the REST (bind) pose with
// hand-rolled linear-blend skinning (PHASE9 Decision §2 — no ozz). FK posing
// (M4) will recompute the per-joint matrices from joint rotations and refresh
// the skin matrices; the geometry/GL upload stays the same.
//
// Loading (`load_from_memory`) is pure CPU (cgltf + Eigen) and builds on any
// backend; the GL methods (`upload_gl`, `draw`) are desktop-GL-3.3 only and are
// no-ops elsewhere (matching ArmatureSpike's guard).

#include <Eigen/Dense>
#include <cstdint>
#include <string>
#include <vector>

namespace Armature {

class ArmatureModel {
public:
    ArmatureModel() = default;
    ~ArmatureModel();
    ArmatureModel(const ArmatureModel&) = delete;
    ArmatureModel& operator=(const ArmatureModel&) = delete;

    // Parse a .glb (or .gltf with embedded buffers) from memory and extract
    // geometry + skin. Validates that a skin with JOINTS_0/WEIGHTS_0 is present
    // (risk #5). On failure returns false and fills `err`. Does not touch GL;
    // `data`/`size` need only stay valid for the duration of the call.
    bool load_from_memory(const void* data, size_t size, std::string& err);

    // Create GL buffers + the skinning program for the parsed data. The GL
    // context must be current. On failure returns false and fills `err`.
    bool upload_gl(std::string& err);

    bool is_loaded() const { return mLoaded; }
    bool is_uploaded() const { return mUploaded; }

    // Draw every primitive with the current skin matrices. The CALLER owns GL
    // render state (FBO bound, depth test on, cull off, viewport set).
    // `viewProj` is column-major; `lightDir` is a world-space direction.
    void draw(const Eigen::Matrix4f& viewProj, const Eigen::Vector3f& lightDir) const;

    // AABB of the figure as rendered (rest pose, CPU-skinned). Valid after load.
    const Eigen::Vector3f& bounds_min() const { return mBoundsMin; }
    const Eigen::Vector3f& bounds_max() const { return mBoundsMax; }
    int joint_count() const { return mJointCount; }

private:
    // Interleaved vertex layout: pos(3) normal(3) joints(4) weights(4).
    static constexpr int FLOATS_PER_VERT = 14;

    struct Primitive {
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        Eigen::Vector4f baseColor{0.8f, 0.8f, 0.8f, 1.0f};
        unsigned vao = 0, vbo = 0, ebo = 0;
        int indexCount = 0;
    };

    std::vector<Primitive> mPrimitives;
    // Skin matrices stored FLAT (16 col-major floats per joint) — this is exactly
    // glUniformMatrix4fv's layout and avoids Eigen-in-std::vector alignment issues.
    int mJointCount = 0;
    std::vector<float> mInverseBindFlat;  // per joint (bind pose) — retained for M4 FK
    std::vector<float> mSkinFlat;         // per joint: meshInv * jointWorld * inverseBind (rest)
    Eigen::Vector3f mBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f mBoundsMax = Eigen::Vector3f::Zero();

    unsigned mProgram = 0;
    int mLocViewProj = -1, mLocLightDir = -1, mLocColor = -1, mLocSkin = -1;

    bool mLoaded = false;
    bool mUploaded = false;
};

}  // namespace Armature
