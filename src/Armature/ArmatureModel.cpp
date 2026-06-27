#include "ArmatureModel.hpp"

#include <Helpers/Logger.hpp>

// This TU is the single cgltf implementation (header-only lib, PHASE9 vendoring).
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
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

    // The skinned figure = the node carrying both a mesh and a skin (risk #5).
    cgltf_node* meshNode = nullptr;
    for (size_t i = 0; i < gltf->nodes_count; ++i)
        if (gltf->nodes[i].mesh && gltf->nodes[i].skin) { meshNode = &gltf->nodes[i]; break; }
    if (!meshNode) { err = "no skinned mesh (a node with both mesh and skin) found"; return false; }

    cgltf_skin* skin = meshNode->skin;
    cgltf_mesh* mesh = meshNode->mesh;
    if (skin->joints_count == 0) { err = "skin has no joints"; return false; }
    mJointCount = static_cast<int>(skin->joints_count);

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
        if (ni >= 0 && ni < static_cast<int>(nodeCount)) mNodes[ni].joint = j;
        mJointNames[j] = skin->joints[j]->name ? skin->joints[j]->name : "";
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
    // Height = uniform bone-length scale: scale every node's local translation,
    // so the whole skeleton scales about the root (limbs lengthen proportionally).
    m(0, 3) = n.t[0] * mHeightScale;
    m(1, 3) = n.t[1] * mHeightScale;
    m(2, 3) = n.t[2] * mHeightScale;
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
        const Eigen::Matrix4f skin = jw * ibm;
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

#else  // !ARMATURE_MODEL_GL

bool ArmatureModel::upload_gl(std::string& err) {
    err = "GL not available in this backend";
    return false;
}
void ArmatureModel::draw(const Eigen::Matrix4f&, const Eigen::Vector3f&, float, float, float) const {}

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
