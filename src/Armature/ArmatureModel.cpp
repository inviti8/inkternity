#include "ArmatureModel.hpp"

#include <Helpers/Logger.hpp>

// This TU is the single cgltf implementation (header-only lib, PHASE9 vendoring).
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <string>

// GL methods are desktop-GL-3.3 only (matches ArmatureSpike's guard). The CPU
// loader below compiles on every backend; only upload_gl/draw need GL.
#if defined(USE_BACKEND_OPENGL) && !defined(__EMSCRIPTEN__) && \
    !defined(USE_BACKEND_OPENGLES_3_0) && !defined(USE_BACKEND_OPENGL_2_1)
#define ARMATURE_MODEL_GL 1
#include <glad/gl3_3.h>
#endif

namespace Armature {
namespace {

// cgltf matrices and Eigen default storage are both column-major, so a plain
// map reads them identically. (Unaligned map → no alignment requirement.)
Eigen::Matrix4f mat_from_cgltf(const cgltf_float m[16]) {
    return Eigen::Map<const Eigen::Matrix4f>(m);
}

// Height = natural bone-length scaling: only the vertical STATURE chains scale —
// the spine/torso/neck and the legs (thigh/shin/foot offset) — while the head,
// hands, feet, fingers, and arms keep constant length. This reproduces the
// anthropometric head-canon (extra stature from chest + longer legs; head/hands/
// feet are the constant measuring unit) and, by scaling local TRANSLATIONS (not
// non-uniform Transform scale), never introduces shear in rotated children.
// (Deep-research 2026-06-27.) Name match is whitespace/case-insensitive to
// tolerate the asset's bone-name quirks (trailing spaces, "leftlowerLeg").
bool is_stature_joint(const std::string& raw) {
    std::string n;
    for (char c : raw)
        if (!std::isspace(static_cast<unsigned char>(c)))
            n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // hips: scaling the pelvis-off-root offset by the same factor RAISES the
    // pelvis to match the lengthened legs, so the feet stay grounded (the legs
    // hang ~pelvis-height downward, so this cancels the leg drop) — the figure
    // grows upward from the floor instead of sinking through it.
    if (n == "hips") return true;
    // Torso/neck: spine + chest + neck. upperChest is deliberately EXCLUDED — it
    // sits between the shoulders and neck, and scaling it over-stretches the neck.
    if (n == "spine" || n == "chest" || n == "neck") return true;
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    // Legs (thigh/shin; "foot" bone = ankle offset = shin length) and arms (upper
    // arm/forearm; "hand" = forearm offset, so the arm lengthens proportionally).
    // Hands/feet/fingers/toes themselves stay constant size. NOTE: matches "foot"
    // (the ankle bone, formerly "leftToes"); the per-toe bones use "Toe" singular
    // so they are unaffected.
    return has("upperleg") || has("lowerleg") || has("foot") ||
           has("upperarm") || has("lowerarm") || has("hand");
}

// Segment bones whose MESH should axially stretch with height (vs. just
// telescoping at joints): the limb segments + torso/neck. Each has a single
// limb-continuation child that defines the stretch axis. Excludes hips (a branch
// point / grounding offset) and the foot/hand offsets (foot/hand — those lengthen
// the shin/forearm, whose mesh is on lowerLeg/lowerArm instead).
bool is_stretch_bone(const std::string& raw) {
    std::string n;
    for (char c : raw)
        if (!std::isspace(static_cast<unsigned char>(c)))
            n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "spine" || n == "chest" || n == "neck") return true;
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("base")) return false;
    return has("upperleg") || has("lowerleg") || has("upperarm") || has("lowerarm");
}

// Does this skin look like OUR armature (or a re-export of it)? Keyed off a few
// distinctive canonical bone names (whitespace/case-insensitive). A match loads
// through the poseable armature path; anything else is a static reference mesh.
bool joints_match_canon(const cgltf_skin* skin) {
    auto norm = [](const char* s) {
        std::string n;
        if (s) for (const char* c = s; *c; ++c)
            if (!std::isspace(static_cast<unsigned char>(*c)))
                n += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        return n;
    };
    std::set<std::string> names;
    for (size_t j = 0; j < skin->joints_count; ++j)
        if (skin->joints[j]) names.insert(norm(skin->joints[j]->name));
    auto has = [&](const char* n) { return names.count(n) > 0; };
    const int core = (has("hips") ? 1 : 0) + (has("spine") ? 1 : 0) +
                     (has("head") ? 1 : 0) + (has("neck") ? 1 : 0);
    const bool limbs = (has("leftupperleg") || has("rightupperleg")) &&
                       (has("leftupperarm") || has("rightupperarm"));
    return core >= 4 && limbs;
}

}  // namespace

ArmatureModel::~ArmatureModel() {
#ifdef ARMATURE_MODEL_GL
    for (auto& p : mPrimitives) {
        if (p.ebo) glDeleteBuffers(1, &p.ebo);
        if (p.vbo) glDeleteBuffers(1, &p.vbo);
        if (p.vao) glDeleteVertexArrays(1, &p.vao);
    }
    if (mProgram) glDeleteProgram(mProgram);
#endif
}

bool ArmatureModel::load_from_memory(const void* data, size_t size, std::string& err) {
    cgltf_options options{};
    cgltf_data* gltf = nullptr;
    if (cgltf_parse(&options, data, size, &gltf) != cgltf_result_success) {
        err = "cgltf_parse failed (not a valid glTF/.glb)";
        return false;
    }
    struct Guard { cgltf_data* p; ~Guard() { if (p) cgltf_free(p); } } guard{gltf};

    if (cgltf_load_buffers(&options, gltf, nullptr) != cgltf_result_success) {
        err = "cgltf_load_buffers failed (external/URI buffers are unsupported; "
              "the asset must embed its buffers, as .glb does)";
        return false;
    }

    // Routing (PHASE9 M7): our default rig (and clean re-exports of it) load
    // through the skinned, poseable armature path. Anything else — unskinned, or
    // a skin whose bones don't match our canon — loads as a flattened STATIC
    // reference mesh (M7: external model loading, drawing reference only).
    cgltf_node* meshNode = nullptr;
    for (size_t i = 0; i < gltf->nodes_count; ++i)
        if (gltf->nodes[i].mesh && gltf->nodes[i].skin && gltf->nodes[i].skin->joints_count > 0) {
            meshNode = &gltf->nodes[i]; break;
        }
    if (!meshNode || !joints_match_canon(meshNode->skin))
        return load_static(gltf, err);
    mIsStatic = false;

    cgltf_skin* skin = meshNode->skin;
    cgltf_mesh* mesh = meshNode->mesh;
    mJointCount = static_cast<int>(skin->joints_count);

    // Morph targets (shape keys): names from the mesh, deltas per primitive below.
    mTargetCount = static_cast<int>(mesh->target_names_count
        ? mesh->target_names_count
        : (mesh->primitives_count ? mesh->primitives[0].targets_count : 0));
    mTargetNames.clear();
    for (size_t t = 0; t < mesh->target_names_count; ++t)
        mTargetNames.push_back(mesh->target_names[t] ? mesh->target_names[t] : "");
    while (static_cast<int>(mTargetNames.size()) < mTargetCount)
        mTargetNames.push_back("morph_" + std::to_string(mTargetNames.size()));
    mTargetWeights.assign(mTargetCount, 0.0f);

    // Inverse-bind matrices (bind pose) → flat. Identity if absent.
    mInverseBindFlat.assign(static_cast<size_t>(mJointCount) * 16, 0.0f);
    for (int j = 0; j < mJointCount; ++j) {
        Eigen::Matrix4f ibm = Eigen::Matrix4f::Identity();
        if (skin->inverse_bind_matrices) {
            cgltf_float m[16];
            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, m, 16);
            ibm = mat_from_cgltf(m);
        }
        std::memcpy(&mInverseBindFlat[static_cast<size_t>(j) * 16], ibm.data(), 16 * sizeof(float));
    }

    // Capture the FULL node tree (parent + rest TRS/matrix) so FK posing can
    // recompute joint world matrices from per-joint rotations (M4).
    const size_t nodeCount = gltf->nodes_count;
    mNodes.assign(nodeCount, NodeT{});
    for (size_t i = 0; i < nodeCount; ++i) {
        const cgltf_node& n = gltf->nodes[i];
        NodeT& d = mNodes[i];
        d.parent = n.parent ? static_cast<int>(n.parent - gltf->nodes) : -1;
        d.hasMatrix = n.has_matrix;
        if (n.has_matrix) std::memcpy(d.matrix, n.matrix, 16 * sizeof(float));
        if (n.has_translation) for (int c = 0; c < 3; ++c) d.t[c] = n.translation[c];
        if (n.has_rotation)    for (int c = 0; c < 4; ++c) d.r[c] = n.rotation[c];
        if (n.has_scale)       for (int c = 0; c < 3; ++c) d.s[c] = n.scale[c];
    }
    const int meshNodeIdx = static_cast<int>(meshNode - gltf->nodes);

    mJointToNode.assign(mJointCount, -1);
    mJointNames.assign(mJointCount, std::string());
    for (int j = 0; j < mJointCount; ++j) {
        const int ni = static_cast<int>(skin->joints[j] - gltf->nodes);
        mJointToNode[j] = ni;
        mJointNames[j] = skin->joints[j]->name ? skin->joints[j]->name : "";
        if (ni >= 0 && ni < static_cast<int>(nodeCount)) {
            mNodes[ni].joint = j;
            mNodes[ni].scalesHeight = is_stature_joint(mJointNames[j]);
        }
    }

    // Stretchy-bone axis: for each segment bone, the bind-local direction to its
    // longest child (the limb continuation), normalized. Used to axially stretch
    // the bone's bound mesh so single-bone regions elongate with height.
    mStretchAxis.assign(static_cast<size_t>(mJointCount) * 3, 0.0f);
    for (int j = 0; j < mJointCount; ++j) {
        if (!is_stretch_bone(mJointNames[j])) continue;
        const int node = mJointToNode[j];
        int best = -1;
        float bestLen = 0.0f;
        for (int i = 0; i < static_cast<int>(nodeCount); ++i) {
            if (mNodes[i].parent != node) continue;
            const float* t = mNodes[i].t;
            const float len2 = t[0] * t[0] + t[1] * t[1] + t[2] * t[2];
            if (len2 > bestLen) { bestLen = len2; best = i; }
        }
        if (best >= 0 && bestLen > 1e-12f) {
            Eigen::Vector3f a(mNodes[best].t[0], mNodes[best].t[1], mNodes[best].t[2]);
            a.normalize();
            mStretchAxis[static_cast<size_t>(j) * 3 + 0] = a.x();
            mStretchAxis[static_cast<size_t>(j) * 3 + 1] = a.y();
            mStretchAxis[static_cast<size_t>(j) * 3 + 2] = a.z();
        }
    }

    mJointPose.assign(static_cast<size_t>(mJointCount) * 4, 0.0f);
    for (int j = 0; j < mJointCount; ++j) mJointPose[static_cast<size_t>(j) * 4 + 3] = 1.0f;  // identity

    // meshWorldInv from the rest tree (the mesh node isn't a joint, so it's
    // pose-independent). Computed once here; recompute_skin() reuses it.
    {
        std::vector<Eigen::Matrix4f> world;
        compute_node_worlds(world);
        const Eigen::Matrix4f meshInv = world[meshNodeIdx].inverse();
        std::memcpy(mMeshWorldInv.data(), meshInv.data(), 16 * sizeof(float));
    }

    build_pickable_set();
    recompute_skin();  // fills mSkinFlat + mJointWorldPos for the identity pose

    // Geometry. JOINTS_0 indices are already indices into skin->joints, so they
    // map straight onto skinMats / the uSkin[] uniform.
    mPrimitives.clear();
    mMaterialNames.clear();
    mMatColor.clear();
    mMatColorDefault.clear();
    for (size_t pi = 0; pi < mesh->primitives_count; ++pi) {
        cgltf_primitive& prim = mesh->primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles) continue;

        cgltf_accessor *pos = nullptr, *nrm = nullptr, *jnt = nullptr, *wgt = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a) {
            cgltf_attribute& at = prim.attributes[a];
            switch (at.type) {
                case cgltf_attribute_type_position: pos = at.data; break;
                case cgltf_attribute_type_normal:   nrm = at.data; break;
                case cgltf_attribute_type_joints:   if (at.index == 0) jnt = at.data; break;
                case cgltf_attribute_type_weights:  if (at.index == 0) wgt = at.data; break;
                default: break;
            }
        }
        if (!pos) continue;
        if (!jnt || !wgt) {
            err = "primitive is missing JOINTS_0/WEIGHTS_0 (model is not skinned as required)";
            return false;
        }

        const size_t vcount = pos->count;
        Primitive out;
        out.verts.resize(vcount * FLOATS_PER_VERT);
        for (size_t v = 0; v < vcount; ++v) {
            float* d = &out.verts[v * FLOATS_PER_VERT];
            cgltf_float p3[3] = {0, 0, 0};
            cgltf_accessor_read_float(pos, v, p3, 3);
            d[0] = p3[0]; d[1] = p3[1]; d[2] = p3[2];
            cgltf_float n3[3] = {0, 0, 1};
            if (nrm) cgltf_accessor_read_float(nrm, v, n3, 3);
            d[3] = n3[0]; d[4] = n3[1]; d[5] = n3[2];
            cgltf_uint ju[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(jnt, v, ju, 4);
            d[6] = float(ju[0]); d[7] = float(ju[1]); d[8] = float(ju[2]); d[9] = float(ju[3]);
            cgltf_float w4[4] = {0, 0, 0, 0};
            cgltf_accessor_read_float(wgt, v, w4, 4);
            d[10] = w4[0]; d[11] = w4[1]; d[12] = w4[2]; d[13] = w4[3];
        }

        // Morph target deltas (position + normal) per target for this primitive.
        out.posDelta.assign(prim.targets_count, {});
        out.nrmDelta.assign(prim.targets_count, {});
        for (size_t t = 0; t < prim.targets_count; ++t) {
            cgltf_accessor *tp = nullptr, *tn = nullptr;
            for (size_t a = 0; a < prim.targets[t].attributes_count; ++a) {
                const cgltf_attribute& at = prim.targets[t].attributes[a];
                if (at.type == cgltf_attribute_type_position) tp = at.data;
                else if (at.type == cgltf_attribute_type_normal) tn = at.data;
            }
            // Morph deltas are usually SPARSE accessors (Blender stores each
            // target dense or sparse, whichever is smaller; pure-sparse targets
            // even omit the bufferView). cgltf_accessor_read_float() refuses
            // sparse accessors outright (returns 0, leaving the output zeroed),
            // which silently turned every sparse shape key into a no-op. Unpack
            // the whole accessor instead: cgltf_accessor_unpack_floats() does the
            // dense base + sparse-substitution passes and handles base==0.
            out.posDelta[t].assign(vcount * 3, 0.0f);
            if (tp) cgltf_accessor_unpack_floats(tp, out.posDelta[t].data(), vcount * 3);
            out.nrmDelta[t].assign(vcount * 3, 0.0f);
            if (tn) cgltf_accessor_unpack_floats(tn, out.nrmDelta[t].data(), vcount * 3);
        }

        if (prim.indices) {
            out.indices.resize(prim.indices->count);
            for (size_t k = 0; k < prim.indices->count; ++k)
                out.indices[k] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, k));
        } else {
            out.indices.resize(vcount);
            for (size_t k = 0; k < vcount; ++k) out.indices[k] = static_cast<uint32_t>(k);
        }
        out.indexCount = static_cast<int>(out.indices.size());

        if (prim.material && prim.material->has_pbr_metallic_roughness) {
            const cgltf_float* bc = prim.material->pbr_metallic_roughness.base_color_factor;
            out.baseColor = Eigen::Vector4f(bc[0], bc[1], bc[2], bc[3]);
        }
        // Map this primitive to a distinct material (by name) with a live color.
        std::string matName = (prim.material && prim.material->name && prim.material->name[0])
                                  ? std::string(prim.material->name)
                                  : ("material_" + std::to_string(pi));
        int mi = -1;
        for (int k = 0; k < static_cast<int>(mMaterialNames.size()); ++k)
            if (mMaterialNames[k] == matName) { mi = k; break; }
        if (mi < 0) {
            mi = static_cast<int>(mMaterialNames.size());
            mMaterialNames.push_back(matName);
            mMatColor.push_back({out.baseColor[0], out.baseColor[1], out.baseColor[2], out.baseColor[3]});
        }
        out.materialIndex = mi;
        mPrimitives.push_back(std::move(out));
    }
    if (mPrimitives.empty()) { err = "no triangle primitives found"; return false; }
    mMatColorDefault = mMatColor;  // snapshot glb defaults for reset_material_colors()

    // Bounds of the figure AS RENDERED: CPU-skin each vertex with the rest skin
    // matrices and expand the AABB (correct even when rest pose != bind pose).
    mBoundsMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    mBoundsMax = Eigen::Vector3f::Constant(-std::numeric_limits<float>::max());
    for (const auto& prim : mPrimitives) {
        const size_t vcount = prim.verts.size() / FLOATS_PER_VERT;
        for (size_t v = 0; v < vcount; ++v) {
            const float* d = &prim.verts[v * FLOATS_PER_VERT];
            Eigen::Matrix4f m = Eigen::Matrix4f::Zero();
            for (int k = 0; k < 4; ++k) {
                const float w = d[10 + k];
                if (w != 0.0f) {
                    const int ji = int(d[6 + k]);
                    if (ji >= 0 && ji < mJointCount)
                        m += w * Eigen::Map<const Eigen::Matrix4f>(&mSkinFlat[static_cast<size_t>(ji) * 16]);
                }
            }
            if (m.isZero(0.0f)) m = Eigen::Matrix4f::Identity();
            const Eigen::Vector4f wp = m * Eigen::Vector4f(d[0], d[1], d[2], 1.0f);
            mBoundsMin = mBoundsMin.cwiseMin(wp.head<3>());
            mBoundsMax = mBoundsMax.cwiseMax(wp.head<3>());
        }
    }

    mLoaded = true;
    Logger::get().log("INFO", "ArmatureModel: loaded " + std::to_string(mPrimitives.size()) +
        " primitives, " + std::to_string(mJointCount) + " joints, " +
        std::to_string(mPickableJoints.size()) + " pickable.");
    return true;
}

// Static (non-armature) model load: flatten every mesh node by its WORLD transform
// into one un-posable reference mesh (PHASE9 M7 — scene graph collapsed, no bones,
// no morphs). At bind pose LBS is identity, so baking skinned meshes by their node
// world transform reproduces the rest shape too — no bone evaluation needed. The
// geometry is bound to a single identity "joint" so the existing skinning shader,
// draw, and bake paths render it unchanged. Simple flat shading; no textures (v1).
bool ArmatureModel::load_static(cgltf_data* gltf, std::string& err) {
    mIsStatic = true;
    const Eigen::Matrix4f I = Eigen::Matrix4f::Identity();

    // One identity joint: the shader skins by uSkin[0] = identity.
    mJointCount = 1;
    mInverseBindFlat.assign(16, 0.0f); std::memcpy(mInverseBindFlat.data(), I.data(), 16 * sizeof(float));
    mNodes.assign(1, NodeT{}); mNodes[0].joint = 0;
    mJointToNode.assign(1, 0);
    mJointNames.assign(1, "static");
    mJointPose.assign(4, 0.0f); mJointPose[3] = 1.0f;
    mStretchAxis.assign(3, 0.0f);
    std::memcpy(mMeshWorldInv.data(), I.data(), 16 * sizeof(float));
    mPickableJoints.clear();
    mTargetCount = 0; mTargetNames.clear(); mTargetWeights.clear();

    mPrimitives.clear(); mMaterialNames.clear(); mMatColor.clear(); mMatColorDefault.clear();
    for (size_t ni = 0; ni < gltf->nodes_count; ++ni) {
        cgltf_node& node = gltf->nodes[ni];
        if (!node.mesh) continue;
        cgltf_float wm[16];
        cgltf_node_transform_world(&node, wm);
        const Eigen::Matrix4f W = mat_from_cgltf(wm);
        const Eigen::Matrix3f Nrm = W.block<3, 3>(0, 0).inverse().transpose();
        cgltf_mesh* mesh = node.mesh;
        for (size_t pi = 0; pi < mesh->primitives_count; ++pi) {
            cgltf_primitive& prim = mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            cgltf_accessor *pos = nullptr, *nrm = nullptr;
            for (size_t a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) pos = prim.attributes[a].data;
                else if (prim.attributes[a].type == cgltf_attribute_type_normal) nrm = prim.attributes[a].data;
            }
            if (!pos) continue;
            const size_t vcount = pos->count;
            Primitive out;
            out.verts.resize(vcount * FLOATS_PER_VERT);
            for (size_t v = 0; v < vcount; ++v) {
                float* d = &out.verts[v * FLOATS_PER_VERT];
                cgltf_float p3[3] = {0, 0, 0};
                cgltf_accessor_read_float(pos, v, p3, 3);
                const Eigen::Vector4f wp = W * Eigen::Vector4f(p3[0], p3[1], p3[2], 1.0f);
                d[0] = wp.x(); d[1] = wp.y(); d[2] = wp.z();
                cgltf_float n3[3] = {0, 0, 1};
                if (nrm) cgltf_accessor_read_float(nrm, v, n3, 3);
                Eigen::Vector3f wn = Nrm * Eigen::Vector3f(n3[0], n3[1], n3[2]);
                if (wn.squaredNorm() > 1e-20f) wn.normalize();
                d[3] = wn.x(); d[4] = wn.y(); d[5] = wn.z();
                d[6] = d[7] = d[8] = d[9] = 0.0f;                       // joint 0
                d[10] = 1.0f; d[11] = d[12] = d[13] = 0.0f;            // weight 1 on joint 0
            }
            if (prim.indices) {
                out.indices.resize(prim.indices->count);
                for (size_t k = 0; k < prim.indices->count; ++k)
                    out.indices[k] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, k));
            } else {
                out.indices.resize(vcount);
                for (size_t k = 0; k < vcount; ++k) out.indices[k] = static_cast<uint32_t>(k);
            }
            out.indexCount = static_cast<int>(out.indices.size());
            if (prim.material && prim.material->has_pbr_metallic_roughness) {
                const cgltf_float* bc = prim.material->pbr_metallic_roughness.base_color_factor;
                out.baseColor = Eigen::Vector4f(bc[0], bc[1], bc[2], bc[3]);
            }
            std::string matName = (prim.material && prim.material->name && prim.material->name[0])
                                      ? std::string(prim.material->name)
                                      : ("material_" + std::to_string(mMaterialNames.size()));
            int mi = -1;
            for (int k = 0; k < static_cast<int>(mMaterialNames.size()); ++k)
                if (mMaterialNames[k] == matName) { mi = k; break; }
            if (mi < 0) {
                mi = static_cast<int>(mMaterialNames.size());
                mMaterialNames.push_back(matName);
                mMatColor.push_back({out.baseColor[0], out.baseColor[1], out.baseColor[2], out.baseColor[3]});
            }
            out.materialIndex = mi;
            mPrimitives.push_back(std::move(out));
        }
    }
    if (mPrimitives.empty()) { err = "no triangle geometry found in model"; return false; }
    mMatColorDefault = mMatColor;

    mSkinFlat.assign(16, 0.0f); std::memcpy(mSkinFlat.data(), I.data(), 16 * sizeof(float));
    mJointWorldFlat.assign(16, 0.0f); std::memcpy(mJointWorldFlat.data(), I.data(), 16 * sizeof(float));

    mBoundsMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    mBoundsMax = Eigen::Vector3f::Constant(-std::numeric_limits<float>::max());
    for (const auto& prim : mPrimitives) {
        const size_t vcount = prim.verts.size() / FLOATS_PER_VERT;
        for (size_t v = 0; v < vcount; ++v) {
            const float* d = &prim.verts[v * FLOATS_PER_VERT];
            const Eigen::Vector3f p(d[0], d[1], d[2]);
            mBoundsMin = mBoundsMin.cwiseMin(p);
            mBoundsMax = mBoundsMax.cwiseMax(p);
        }
    }

    mLoaded = true;
    Logger::get().log("INFO", "ArmatureModel: loaded STATIC model, " +
        std::to_string(mPrimitives.size()) + " primitives, " +
        std::to_string(mMaterialNames.size()) + " materials.");
    return true;
}

// --- FK posing (CPU; available on all backends) ---------------------------

Eigen::Matrix4f ArmatureModel::node_local_matrix(int nodeIndex) const {
    const NodeT& n = mNodes[nodeIndex];
    if (n.hasMatrix) return Eigen::Map<const Eigen::Matrix4f>(n.matrix);
    Eigen::Quaternionf q(n.r[3], n.r[0], n.r[1], n.r[2]);  // w,x,y,z from xyzw
    if (n.joint >= 0) {
        const float* p = &mJointPose[static_cast<size_t>(n.joint) * 4];
        q = q * Eigen::Quaternionf(p[3], p[0], p[1], p[2]);  // rest * pose (local frame)
    }
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m.block<3, 3>(0, 0) = q.normalized().toRotationMatrix() *
        Eigen::DiagonalMatrix<float, 3>(n.s[0], n.s[1], n.s[2]);
    // Height: scale local translation ONLY for stature-chain joints (spine/legs),
    // so head/hands/feet keep constant size — natural proportions, no shear.
    const float hs = n.scalesHeight ? mHeightScale : 1.0f;
    m(0, 3) = n.t[0] * hs;
    m(1, 3) = n.t[1] * hs;
    m(2, 3) = n.t[2] * hs;
    return m;
}

void ArmatureModel::compute_node_worlds(std::vector<Eigen::Matrix4f>& world) const {
    const int n = static_cast<int>(mNodes.size());
    world.assign(n, Eigen::Matrix4f::Identity());
    std::vector<char> done(n, 0);
    for (int i = 0; i < n; ++i) {
        if (done[i]) continue;
        std::vector<int> stack;                       // uncomputed ancestors, deepest last
        for (int c = i; c >= 0 && !done[c]; c = mNodes[c].parent) stack.push_back(c);
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            const int node = *it, p = mNodes[node].parent;
            const Eigen::Matrix4f local = node_local_matrix(node);
            world[node] = (p >= 0) ? (world[p] * local).eval() : local;
            done[node] = 1;
        }
    }
}

void ArmatureModel::recompute_skin() {
    std::vector<Eigen::Matrix4f> world;
    compute_node_worlds(world);
    const Eigen::Matrix4f meshInv = Eigen::Map<const Eigen::Matrix4f>(mMeshWorldInv.data());
    mSkinFlat.assign(static_cast<size_t>(mJointCount) * 16, 0.0f);
    mJointWorldFlat.assign(static_cast<size_t>(mJointCount) * 16, 0.0f);
    for (int j = 0; j < mJointCount; ++j) {
        const int node = mJointToNode[j];
        const Eigen::Matrix4f jw = meshInv * world[node];  // joint in render space
        const Eigen::Matrix4f ibm = Eigen::Map<const Eigen::Matrix4f>(&mInverseBindFlat[static_cast<size_t>(j) * 16]);
        // Stretchy bone: axially scale the bone's bound mesh in BIND-local space
        // (between jw and ibm) so it elongates with height. Applied only to this
        // bone's skin matrix — children chain off jw (no scale) → no shear.
        const Eigen::Vector3f a(mStretchAxis[static_cast<size_t>(j) * 3 + 0],
                                mStretchAxis[static_cast<size_t>(j) * 3 + 1],
                                mStretchAxis[static_cast<size_t>(j) * 3 + 2]);
        Eigen::Matrix4f skin;
        if (a.squaredNorm() > 0.0f && mHeightScale != 1.0f) {
            Eigen::Matrix4f s4 = Eigen::Matrix4f::Identity();
            s4.block<3, 3>(0, 0) += (mHeightScale - 1.0f) * (a * a.transpose());
            skin = jw * s4 * ibm;
        } else {
            skin = jw * ibm;
        }
        std::memcpy(&mSkinFlat[static_cast<size_t>(j) * 16], skin.data(), 16 * sizeof(float));
        std::memcpy(&mJointWorldFlat[static_cast<size_t>(j) * 16], jw.data(), 16 * sizeof(float));
    }
}

void ArmatureModel::rotate_joint(int jointIndex, const Eigen::Quaternionf& deltaLocal) {
    if (jointIndex < 0 || jointIndex >= mJointCount) return;
    float* p = &mJointPose[static_cast<size_t>(jointIndex) * 4];
    const Eigen::Quaternionf nq = (Eigen::Quaternionf(p[3], p[0], p[1], p[2]) * deltaLocal).normalized();
    p[0] = nq.x(); p[1] = nq.y(); p[2] = nq.z(); p[3] = nq.w();
    recompute_skin();
}

void ArmatureModel::set_joint_pose(int jointIndex, const Eigen::Quaternionf& localPose) {
    if (jointIndex < 0 || jointIndex >= mJointCount) return;
    const Eigen::Quaternionf q = localPose.normalized();
    float* p = &mJointPose[static_cast<size_t>(jointIndex) * 4];
    p[0] = q.x(); p[1] = q.y(); p[2] = q.z(); p[3] = q.w();
    recompute_skin();
}

void ArmatureModel::reset_pose() {
    for (int j = 0; j < mJointCount; ++j) {
        float* p = &mJointPose[static_cast<size_t>(j) * 4];
        p[0] = p[1] = p[2] = 0.0f; p[3] = 1.0f;
    }
    recompute_skin();
}

void ArmatureModel::set_height(float scale) {
    mHeightScale = std::clamp(scale, 0.2f, 5.0f);
    recompute_skin();
}

void ArmatureModel::set_material_color(int i, float r, float g, float b, float a) {
    if (i >= 0 && i < static_cast<int>(mMatColor.size())) mMatColor[i] = {r, g, b, a};
}

void ArmatureModel::reset_material_colors() { mMatColor = mMatColorDefault; }

int ArmatureModel::find_target(const std::string& name) const {
    for (int t = 0; t < mTargetCount; ++t)
        if (mTargetNames[t] == name) return t;
    return -1;
}

void ArmatureModel::set_morph_weights(const std::vector<float>& weights) {
    mTargetWeights.assign(mTargetCount, 0.0f);
    for (int t = 0; t < mTargetCount && t < static_cast<int>(weights.size()); ++t)
        mTargetWeights[t] = weights[t];
    apply_morphs();
}

void ArmatureModel::reset_morphs() {
    mTargetWeights.assign(mTargetCount, 0.0f);
    apply_morphs();
}

Eigen::Matrix4f ArmatureModel::joint_world_matrix(int jointIndex) const {
    if (jointIndex < 0 || static_cast<size_t>(jointIndex) * 16 + 16 > mJointWorldFlat.size())
        return Eigen::Matrix4f::Identity();
    return Eigen::Map<const Eigen::Matrix4f>(&mJointWorldFlat[static_cast<size_t>(jointIndex) * 16]);
}

Eigen::Vector3f ArmatureModel::joint_world_pos(int jointIndex) const {
    return joint_world_matrix(jointIndex).col(3).head<3>();
}

Eigen::Quaternionf ArmatureModel::joint_pose(int jointIndex) const {
    if (jointIndex < 0 || jointIndex >= mJointCount) return Eigen::Quaternionf::Identity();
    const float* p = &mJointPose[static_cast<size_t>(jointIndex) * 4];
    return Eigen::Quaternionf(p[3], p[0], p[1], p[2]);  // w,x,y,z
}

int ArmatureModel::find_joint(const std::string& name) const {
    for (int j = 0; j < mJointCount; ++j)
        if (mJointNames[j] == name) return j;
    return -1;
}

void ArmatureModel::build_pickable_set() {
    mPickableJoints.clear();
    auto has = [](const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; };
    for (int j = 0; j < mJointCount; ++j) {
        const std::string& n = mJointNames[j];
        const bool excluded =
            has(n, "Base") || n == "root" || n == "neutral_bone" || has(n, "Ear") ||
            has(n, "BigToe") || has(n, "IndexToe") || has(n, "MidToe") ||
            has(n, "RingToe") || has(n, "SmallToe");
        if (!excluded) mPickableJoints.push_back(j);
    }
}

#ifdef ARMATURE_MODEL_GL

namespace {
unsigned compile_shader(GLenum type, const char* src, std::string& err) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        err = log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}
}  // namespace

bool ArmatureModel::upload_gl(std::string& err) {
    if (!mLoaded) { err = "model not loaded"; return false; }
    if (mUploaded) return true;

    // Guard against exceeding the vertex-uniform budget (89 mat4 = 1424
    // components; desktop GL is generous, but verify rather than crash).
    GLint maxComps = 0;
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &maxComps);
    if (mJointCount * 16 + 16 > maxComps) {
        err = "skeleton too large for the vertex-uniform budget (" +
              std::to_string(mJointCount) + " joints)";
        return false;
    }

    const std::string js = std::to_string(mJointCount);
    const std::string vsSrc =
        "#version 330 core\n"
        "layout(location=0) in vec3 aPos;\n"
        "layout(location=1) in vec3 aNormal;\n"
        "layout(location=2) in vec4 aJoints;\n"
        "layout(location=3) in vec4 aWeights;\n"
        "uniform mat4 uViewProj;\n"
        "uniform mat4 uSkin[" + js + "];\n"
        "out vec3 vNormal;\n"
        "void main() {\n"
        "    mat4 m = aWeights.x * uSkin[int(aJoints.x)]\n"
        "           + aWeights.y * uSkin[int(aJoints.y)]\n"
        "           + aWeights.z * uSkin[int(aJoints.z)]\n"
        "           + aWeights.w * uSkin[int(aJoints.w)];\n"
        "    if (m == mat4(0.0)) m = mat4(1.0);\n"
        "    vec4 worldPos = m * vec4(aPos, 1.0);\n"
        "    gl_Position = uViewProj * worldPos;\n"
        "    vNormal = mat3(m) * aNormal;\n"
        "}\n";
    const char* fsSrc =
        "#version 330 core\n"
        "in vec3 vNormal;\n"
        "out vec4 FragColor;\n"
        "uniform vec3 uLightDir;\n"
        "uniform vec3 uColor;\n"
        "uniform float uAmbient;\n"
        "uniform float uDiffuse;\n"
        "uniform float uSky;\n"
        "void main() {\n"
        "    vec3 n = normalize(vNormal);\n"
        "    float diff = max(dot(n, normalize(-uLightDir)), 0.0);\n"
        "    float sky = max(n.y, 0.0) * uSky;            // soft fill from above\n"
        "    vec3 c = uColor * (uAmbient + uDiffuse * diff + sky);\n"
        "    FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
        "}\n";

    std::string serr;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vsSrc.c_str(), serr);
    if (!vs) { err = "vertex shader: " + serr; return false; }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsSrc, serr);
    if (!fs) { glDeleteShader(vs); err = "fragment shader: " + serr; return false; }
    mProgram = glCreateProgram();
    glAttachShader(mProgram, vs);
    glAttachShader(mProgram, fs);
    glLinkProgram(mProgram);
    GLint linked = GL_FALSE;
    glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked) {
        char log[1024] = {0};
        glGetProgramInfoLog(mProgram, sizeof(log) - 1, nullptr, log);
        err = std::string("program link: ") + log;
        glDeleteProgram(mProgram);
        mProgram = 0;
        return false;
    }
    mLocViewProj = glGetUniformLocation(mProgram, "uViewProj");
    mLocLightDir = glGetUniformLocation(mProgram, "uLightDir");
    mLocColor    = glGetUniformLocation(mProgram, "uColor");
    mLocSkin     = glGetUniformLocation(mProgram, "uSkin");
    mLocAmbient  = glGetUniformLocation(mProgram, "uAmbient");
    mLocDiffuse  = glGetUniformLocation(mProgram, "uDiffuse");
    mLocSky      = glGetUniformLocation(mProgram, "uSky");

    for (auto& p : mPrimitives) {
        glGenVertexArrays(1, &p.vao);
        glBindVertexArray(p.vao);
        glGenBuffers(1, &p.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
        glBufferData(GL_ARRAY_BUFFER, p.verts.size() * sizeof(float), p.verts.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &p.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, p.indices.size() * sizeof(uint32_t), p.indices.data(), GL_STATIC_DRAW);
        const GLsizei stride = FLOATS_PER_VERT * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(10 * sizeof(float)));
        glEnableVertexAttribArray(3);
    }
    glBindVertexArray(0);

    mUploaded = true;
    return true;
}

void ArmatureModel::draw(const Eigen::Matrix4f& viewProj, const Eigen::Vector3f& lightDir,
                         float ambient, float diffuse, float sky) const {
    if (!mUploaded) return;
    glUseProgram(mProgram);
    glUniformMatrix4fv(mLocViewProj, 1, GL_FALSE, viewProj.data());
    glUniform3fv(mLocLightDir, 1, lightDir.data());
    glUniform1f(mLocAmbient, ambient);
    glUniform1f(mLocDiffuse, diffuse);
    glUniform1f(mLocSky, sky);
    if (mLocSkin >= 0 && !mSkinFlat.empty())
        glUniformMatrix4fv(mLocSkin, mJointCount, GL_FALSE, mSkinFlat.data());
    for (const auto& p : mPrimitives) {
        // Live material color (rgba; the shader's uColor reads the first 3).
        glUniform3fv(mLocColor, 1, mMatColor[p.materialIndex].data());
        glBindVertexArray(p.vao);
        glDrawElements(GL_TRIANGLES, p.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

void ArmatureModel::apply_morphs() {
    if (!mUploaded) return;
    std::vector<float> v;
    for (auto& p : mPrimitives) {
        v = p.verts;  // base interleaved (pos/normal/joints/weights)
        const size_t vcount = v.size() / FLOATS_PER_VERT;
        for (int t = 0; t < mTargetCount; ++t) {
            const float w = (t < static_cast<int>(mTargetWeights.size())) ? mTargetWeights[t] : 0.0f;
            if (w == 0.0f || t >= static_cast<int>(p.posDelta.size())) continue;
            const std::vector<float>& pd = p.posDelta[t];
            const std::vector<float>& nd = p.nrmDelta[t];
            const bool hasN = nd.size() >= vcount * 3;
            for (size_t vi = 0; vi < vcount; ++vi) {
                float* d = &v[vi * FLOATS_PER_VERT];
                d[0] += w * pd[vi * 3 + 0]; d[1] += w * pd[vi * 3 + 1]; d[2] += w * pd[vi * 3 + 2];
                if (hasN) { d[3] += w * nd[vi * 3 + 0]; d[4] += w * nd[vi * 3 + 1]; d[5] += w * nd[vi * 3 + 2]; }
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

#else  // !ARMATURE_MODEL_GL

bool ArmatureModel::upload_gl(std::string& err) {
    err = "GL not available in this backend";
    return false;
}
void ArmatureModel::draw(const Eigen::Matrix4f&, const Eigen::Vector3f&, float, float, float) const {}
void ArmatureModel::apply_morphs() {}

#endif

// Cached singleton for the bundled default armature. Pure-ish: reads the file,
// parses (CPU), then GL-uploads (no-op/false on non-GL builds → returns null).
ArmatureModel* default_model() {
    static ArmatureModel model;
    static bool tried = false, ready = false;
    if (tried) return ready ? &model : nullptr;
    tried = true;

    std::string bytes;
    {
        std::ifstream f("data/models/inkternity_default_armature.glb",
                        std::ios::in | std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            Logger::get().log("USERINFO",
                "Armature: default model not found "
                "(data/models/inkternity_default_armature.glb).");
            return nullptr;
        }
        const auto sz = f.tellg();
        if (sz <= 0) { Logger::get().log("WORLDFATAL", "Armature: model file empty."); return nullptr; }
        f.seekg(0);
        bytes.resize(static_cast<size_t>(sz));
        f.read(bytes.data(), sz);
    }
    std::string err;
    if (!model.load_from_memory(bytes.data(), bytes.size(), err)) {
        Logger::get().log("WORLDFATAL", "Armature load failed: " + err);
        return nullptr;
    }
    if (!model.upload_gl(err)) {
        Logger::get().log("WORLDFATAL", "Armature GL upload failed: " + err);
        return nullptr;
    }
    ready = true;
    return &model;
}

}  // namespace Armature
