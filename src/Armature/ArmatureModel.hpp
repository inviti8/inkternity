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
// no-ops elsewhere (same GL guard as the rest of the armature code).

#include <Eigen/Dense>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct cgltf_data;  // fwd-decl (cgltf.h is included only in the .cpp)

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
    // `zUpToYUp` rotates the loaded geometry −90° about X (Z-up → Y-up). The
    // reangle service returns trimesh/TripoSR meshes that are Z-up (figure tall
    // along +Z), which lie down in this Y-up viewer; reangle passes true. Leave
    // false for normal glTF (Y-up by spec) so user-loaded models aren't rotated.
    bool load_from_memory(const void* data, size_t size, std::string& err,
                          bool zUpToYUp = false);

    // Create GL buffers + the skinning program for the parsed data. The GL
    // context must be current. On failure returns false and fills `err`.
    bool upload_gl(std::string& err);

    bool is_loaded() const { return mLoaded; }
    bool is_uploaded() const { return mUploaded; }
    // A flattened static reference model (M7) — no skin/pose/morphs. The editor
    // hides the Pose/Body tabs for these; only camera/light/materials/lens apply.
    bool is_static() const { return mIsStatic; }

    // Draw every primitive with the current skin matrices. The CALLER owns GL
    // render state (FBO bound, depth test on, cull off, viewport set).
    // `viewProj` is column-major; `lightDir` is the world-space direction the
    // light travels; ambient/diffuse/sky are the lighting gains.
    //
    // If this is a TEXTURED static model (a reangle mesh — has UVs + an embedded
    // base-color texture, REANGLE_PIPELINE.md §7.3), it renders through a separate
    // unlit texture-sampling program instead (the lighting args are ignored), so
    // the bake shows the artist's own front-projected pixels, not a shaded blob.
    void draw(const Eigen::Matrix4f& viewProj, const Eigen::Vector3f& lightDir,
              float ambient, float diffuse, float sky) const;

    // True once a static model carrying UVs + an embedded texture has GL-uploaded
    // — i.e. draw() will use the textured (style-preserving) path.
    bool is_textured() const { return mHasTexture; }

    // AABB of the figure as rendered (rest pose, CPU-skinned). Valid after load.
    const Eigen::Vector3f& bounds_min() const { return mBoundsMin; }
    const Eigen::Vector3f& bounds_max() const { return mBoundsMax; }
    int joint_count() const { return mJointCount; }

    // --- FK posing (M4) ---------------------------------------------------
    // Accumulate a local-space rotation on a joint and refresh the skin.
    void rotate_joint(int jointIndex, const Eigen::Quaternionf& deltaLocal);
    // Set a joint's absolute local-space pose rotation (relative to bind).
    void set_joint_pose(int jointIndex, const Eigen::Quaternionf& localPose);
    void reset_pose();

    // Uniform bone-length scale (height). 1.0 = authored size. Scales the skeleton
    // about the root, so limbs lengthen proportionally (PHASE9 height = bone-length).
    void set_height(float scale);
    float height() const { return mHeightScale; }

    // Material colors (M5.1b): per-material live base color (rgba). Default is the
    // glb's baseColorFactor; the editor can override it per material.
    int material_count() const { return static_cast<int>(mMaterialNames.size()); }
    const std::string& material_name(int i) const { return mMaterialNames[i]; }
    std::array<float, 4> material_color(int i) const { return mMatColor[i]; }
    void set_material_color(int i, float r, float g, float b, float a);
    void reset_material_colors();   // restore the glb defaults (shared model hygiene)

    // Shape keys / morph targets (M5.1c). Weights apply BEFORE skinning (CPU sum
    // of base + Σ w·delta, re-uploaded to the VBO). `weights` is indexed by target.
    int target_count() const { return mTargetCount; }
    const std::string& target_name(int i) const { return mTargetNames[i]; }
    int find_target(const std::string& name) const;
    void set_morph_weights(const std::vector<float>& weights);
    void reset_morphs();            // all weights 0 (shared model hygiene)

    // The curated set of joints the poser exposes (Decision Q1): core + fingers,
    // toes collapsed, helper/ear/neutral bones excluded. Indices into joints.
    const std::vector<int>& pickable_joints() const { return mPickableJoints; }
    // World-space position / full transform of a joint at the CURRENT pose
    // (render space — same space the figure is drawn in). Valid after load/pose.
    Eigen::Vector3f joint_world_pos(int jointIndex) const;
    Eigen::Matrix4f joint_world_matrix(int jointIndex) const;

    // Pose round-trip (M5 persistence): read a joint's local pose rotation, and
    // resolve a joint index by name (for seeding a saved pose).
    Eigen::Quaternionf joint_pose(int jointIndex) const;
    int find_joint(const std::string& name) const;
    const std::string& joint_name(int jointIndex) const { return mJointNames[jointIndex]; }

private:
    // Interleaved vertex layout: pos(3) normal(3) joints(4) weights(4).
    static constexpr int FLOATS_PER_VERT = 14;

    struct Primitive {
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        // TEXCOORD_0 (2/vertex), only populated for textured static meshes
        // (reangle). Kept in its own buffer so the skinned rig's 14-float
        // interleaved layout is untouched.
        std::vector<float> uv;
        Eigen::Vector4f baseColor{0.8f, 0.8f, 0.8f, 1.0f};
        int materialIndex = 0;        // into mMaterialNames / mMatColor
        // Morph target deltas (M5.1c): per target, 3 floats/vertex (pos + normal).
        std::vector<std::vector<float>> posDelta;
        std::vector<std::vector<float>> nrmDelta;
        unsigned vao = 0, vbo = 0, ebo = 0, uvVbo = 0;
        int indexCount = 0;
    };

    std::vector<Primitive> mPrimitives;
    // Skin matrices stored FLAT (16 col-major floats per joint) — this is exactly
    // glUniformMatrix4fv's layout and avoids Eigen-in-std::vector alignment issues.
    int mJointCount = 0;
    std::vector<float> mInverseBindFlat;  // per joint (bind pose), 16/joint
    std::vector<float> mSkinFlat;         // per joint: meshInv * jointWorld * inverseBind
    Eigen::Vector3f mBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f mBoundsMax = Eigen::Vector3f::Zero();

    // --- FK node tree (plain floats → no Eigen alignment concerns) ---------
    struct NodeT {
        int parent = -1;
        int joint = -1;          // index into joints, or -1
        bool scalesHeight = false;  // part of the spine/leg stature chain (M5.1a redesign)
        bool hasMatrix = false;
        float t[3] = {0, 0, 0};
        float r[4] = {0, 0, 0, 1};  // quaternion xyzw
        float s[3] = {1, 1, 1};
        float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    };
    std::vector<NodeT> mNodes;
    std::vector<int> mJointToNode;        // jointCount → node index
    std::vector<std::string> mJointNames; // jointCount
    std::vector<float> mJointPose;        // 4/joint, xyzw, identity = no pose
    std::vector<float> mJointWorldFlat;   // 16/joint, current pose (render space)
    // Stretchy-bone axis (M5.1a): bind-local unit direction to the limb child for
    // the segment bones, else zero. Used to axially scale the bone's bound mesh
    // (in its skin matrix only — not propagated to children, so no shear).
    std::vector<float> mStretchAxis;      // 3/joint
    std::vector<int> mPickableJoints;
    std::array<float, 16> mMeshWorldInv = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float mHeightScale = 1.0f;            // uniform bone-length scale (height)
    std::vector<std::string> mMaterialNames;
    std::vector<std::array<float, 4>> mMatColor;         // live rgba per material
    std::vector<std::array<float, 4>> mMatColorDefault;  // glb baseColors (for reset)
    int mTargetCount = 0;
    std::vector<std::string> mTargetNames;
    std::vector<float> mTargetWeights;
    void apply_morphs();   // rebuild VBOs from base + weighted deltas (GL)
    // Decode + upload the embedded base-color texture and build the unlit
    // sampling program (GL builds only; best-effort — see .cpp). Called from
    // upload_gl for textured static (reangle) meshes.
    void upload_texture();

    // Flatten a non-armature glTF into one static reference mesh (M7).
    // `zUpToYUp` applies the −90°-about-X up-axis correction (see load_from_memory).
    bool load_static(cgltf_data* gltf, std::string& err, bool zUpToYUp = false);

    Eigen::Matrix4f node_local_matrix(int nodeIndex) const;
    void compute_node_worlds(std::vector<Eigen::Matrix4f>& world) const;
    void recompute_skin();
    void build_pickable_set();

    unsigned mProgram = 0;
    int mLocViewProj = -1, mLocLightDir = -1, mLocColor = -1, mLocSkin = -1;
    int mLocAmbient = -1, mLocDiffuse = -1, mLocSky = -1;

    // Textured static-mesh path (reangle, §7.3): the base-color image bytes as
    // embedded in the glb (raw PNG/JPEG — decoded at upload_gl time, kept out of
    // the CPU loader), the uploaded GL texture, and the unlit sampling program.
    std::vector<uint8_t> mTextureEncoded;
    unsigned mTexture = 0;
    unsigned mTexProgram = 0;
    int mTexLocViewProj = -1, mTexLocSampler = -1;
    bool mHasTexture = false;   // true once a texture + UVs uploaded successfully

    bool mLoaded = false;
    bool mUploaded = false;
    bool mIsStatic = false;   // loaded via load_static (flattened, un-posable)
};

// Load + GL-upload the bundled default armature once (cached for the app's life).
// Returns nullptr (with an in-app log) if the file is missing or the build lacks
// desktop GL. The GL context must be current on the first call.
ArmatureModel* default_model();

}  // namespace Armature
