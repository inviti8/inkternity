#include "ArmatureSpike.hpp"

#include <Helpers/Logger.hpp>

// Desktop GL 3.3 + Ganesh + libmypaint are all required for the real spike:
// raw GL renders the cube, Ganesh owns the shared context we must not corrupt,
// and the MYPAINTLAYER bake target is a libmypaint surface. Any other build
// (Emscripten/GLES, GL 2.1, Vulkan/Graphite, no-ink) gets the no-op below.
#if defined(USE_BACKEND_OPENGL) && !defined(__EMSCRIPTEN__) && \
    !defined(USE_BACKEND_OPENGLES_3_0) && !defined(USE_BACKEND_OPENGL_2_1) && \
    defined(USE_SKIA_BACKEND_GANESH) && defined(HVYM_HAS_LIBMYPAINT)
#define ARMATURE_SPIKE_ENABLED 1
#endif

#ifdef ARMATURE_SPIKE_ENABLED

// glad first (declarations only — the GLAD_GL_IMPLEMENTATION lives in main.cpp,
// which loads the function pointers once at startup; we just reference them).
#include <glad/gl3_3.h>

#include "../DrawingProgram/DrawingProgram.hpp"
#include "../DrawingProgram/Layers/DrawingProgramLayerManager.hpp"
#include "../DrawingProgram/Layers/DrawingProgramLayerListItem.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../DrawData.hpp"
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include "../CanvasComponents/MyPaintLayerCanvasComponent.hpp"

#include <include/core/SkBitmap.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkColorType.h>
#include <include/core/SkAlphaType.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <Eigen/Dense>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ArmatureSpike {
namespace {

constexpr int SPIKE_DIM = 512;  // square bake; one cube doesn't need more

// --- tiny matrix helpers (Eigen is column-major, which is exactly what GL
// wants, so we hand mat.data() straight to glUniformMatrix4fv with
// transpose == GL_FALSE). -------------------------------------------------

Eigen::Matrix4f perspective(float fovYRad, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovYRad * 0.5f);
    Eigen::Matrix4f m = Eigen::Matrix4f::Zero();
    m(0, 0) = f / aspect;
    m(1, 1) = f;
    m(2, 2) = (zfar + znear) / (znear - zfar);
    m(2, 3) = (2.0f * zfar * znear) / (znear - zfar);
    m(3, 2) = -1.0f;
    return m;
}

// 36 vertices (6 faces * 2 tris * 3), interleaved position(3) + normal(3).
const std::array<float, 36 * 6> CUBE_VERTS = {{
    // back face (-Z)
    -0.5f,-0.5f,-0.5f, 0,0,-1,  -0.5f, 0.5f,-0.5f, 0,0,-1,   0.5f, 0.5f,-0.5f, 0,0,-1,
    -0.5f,-0.5f,-0.5f, 0,0,-1,   0.5f, 0.5f,-0.5f, 0,0,-1,   0.5f,-0.5f,-0.5f, 0,0,-1,
    // front face (+Z)
    -0.5f,-0.5f, 0.5f, 0,0, 1,   0.5f, 0.5f, 0.5f, 0,0, 1,  -0.5f, 0.5f, 0.5f, 0,0, 1,
    -0.5f,-0.5f, 0.5f, 0,0, 1,   0.5f,-0.5f, 0.5f, 0,0, 1,   0.5f, 0.5f, 0.5f, 0,0, 1,
    // left face (-X)
    -0.5f, 0.5f, 0.5f,-1,0,0,   -0.5f, 0.5f,-0.5f,-1,0,0,   -0.5f,-0.5f,-0.5f,-1,0,0,
    -0.5f, 0.5f, 0.5f,-1,0,0,   -0.5f,-0.5f,-0.5f,-1,0,0,   -0.5f,-0.5f, 0.5f,-1,0,0,
    // right face (+X)
     0.5f, 0.5f, 0.5f, 1,0,0,    0.5f,-0.5f,-0.5f, 1,0,0,    0.5f, 0.5f,-0.5f, 1,0,0,
     0.5f, 0.5f, 0.5f, 1,0,0,    0.5f,-0.5f, 0.5f, 1,0,0,    0.5f,-0.5f,-0.5f, 1,0,0,
    // bottom face (-Y)
    -0.5f,-0.5f,-0.5f, 0,-1,0,   0.5f,-0.5f,-0.5f, 0,-1,0,   0.5f,-0.5f, 0.5f, 0,-1,0,
    -0.5f,-0.5f,-0.5f, 0,-1,0,   0.5f,-0.5f, 0.5f, 0,-1,0,  -0.5f,-0.5f, 0.5f, 0,-1,0,
    // top face (+Y)
    -0.5f, 0.5f,-0.5f, 0, 1,0,   0.5f, 0.5f, 0.5f, 0, 1,0,   0.5f, 0.5f,-0.5f, 0, 1,0,
    -0.5f, 0.5f,-0.5f, 0, 1,0,  -0.5f, 0.5f, 0.5f, 0, 1,0,   0.5f, 0.5f, 0.5f, 0, 1,0,
}};

const char* VERT_SRC = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
}
)";

const char* FRAG_SRC = R"(#version 330 core
in vec3 vNormal;
out vec4 FragColor;
uniform vec3 uLightDir;
uniform vec3 uColor;
void main() {
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, normalize(-uLightDir)), 0.0);
    float ambient = 0.28;
    vec3 c = uColor * (ambient + diff * 0.85);
    FragColor = vec4(c, 1.0);
}
)";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        Logger::get().log("WORLDFATAL",
            std::string("Armature spike: shader compile failed: ") + log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// Render the lit cube into a fresh FBO and read the pixels back (bottom-up,
// as GL gives them). Returns false on any GL failure. On success `outPixels`
// holds dim*dim*4 RGBA8 bytes.
bool render_cube_to_pixels(int dim, std::vector<uint8_t>& outPixels) {
    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    GLuint prog = 0, vao = 0, vbo = 0, vs = 0, fs = 0;
    bool ok = false;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dim, dim, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    // Our own depth attachment — the window framebuffer has none.
    glGenRenderbuffers(1, &depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, dim, dim);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Logger::get().log("WORLDFATAL", "Armature spike: FBO incomplete.");
        goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) goto cleanup;
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    {
        GLint linked = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[1024] = {0};
            glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
            Logger::get().log("WORLDFATAL",
                std::string("Armature spike: program link failed: ") + log);
            goto cleanup;
        }
    }

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * CUBE_VERTS.size(), CUBE_VERTS.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Fully specify every piece of state the render+clear depends on — do NOT
    // assume defaults. Skia/Ganesh leaves GL state in whatever its last batch
    // needed (depth/colour/stencil masks off, scissor on, blend on, ...), and
    // glClear/glDraw silently honour those masks. A left-disabled depth or
    // colour mask is the classic "renders nothing into my FBO" trap. (M1
    // finding: this is mandatory before any 3D pass on the shared context.)
    glViewport(0, 0, dim, dim);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);  // opaque cube on transparent clear → no premul concerns
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    {
        const Eigen::Matrix4f proj = perspective(45.0f * 3.14159265f / 180.0f, 1.0f, 0.1f, 100.0f);
        Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
        view(2, 3) = -4.0f;  // camera at +Z looking at origin
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        model.block<3, 3>(0, 0) =
            (Eigen::AngleAxisf(0.6f, Eigen::Vector3f::UnitY()) *
             Eigen::AngleAxisf(0.45f, Eigen::Vector3f::UnitX())).toRotationMatrix();
        const Eigen::Matrix4f mvp = proj * view * model;

        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp.data());
        glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"), 1, GL_FALSE, model.data());
        const float lightDir[3] = {0.4f, -0.7f, -0.6f};
        const float color[3] = {0.85f, 0.55f, 0.30f};
        glUniform3fv(glGetUniformLocation(prog, "uLightDir"), 1, lightDir);
        glUniform3fv(glGetUniformLocation(prog, "uColor"), 1, color);

        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    outPixels.assign(static_cast<size_t>(dim) * dim * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, dim, dim, GL_RGBA, GL_UNSIGNED_BYTE, outPixels.data());
    {
        // Diagnostic: how many opaque pixels actually came back? 0 ⇒ the render
        // produced nothing (state/matrix problem). >0 ⇒ render is fine and any
        // "invisible" result is placement/import. Logged so we get a definitive
        // signal from a single test run.
        const GLenum err = glGetError();
        size_t opaque = 0;
        for (size_t i = 3; i < outPixels.size(); i += 4)
            if (outPixels[i] != 0) ++opaque;
        Logger::get().log("INFO",
            "Armature spike: readback glGetError=" + std::to_string(err) +
            ", opaque px=" + std::to_string(opaque) +
            "/" + std::to_string(static_cast<size_t>(dim) * dim));
    }
    ok = true;

cleanup:
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (prog) glDeleteProgram(prog);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (depthRbo) glDeleteRenderbuffers(1, &depthRbo);
    if (colorTex) glDeleteTextures(1, &colorTex);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    return ok;
}

}  // namespace

void run_spike(DrawingProgram& drawP) {
    auto& world = drawP.world;
    auto& main = world.main;

    auto editLayer = drawP.layerMan.get_editing_layer().lock();
    if (!editLayer || editLayer->is_folder()) {
        Logger::get().log("USERINFO", "Armature spike: no active layer to bake into.");
        return;
    }

    // 1) raw-GL 3D pass into our own FBO.
    std::vector<uint8_t> pixels;
    if (!render_cube_to_pixels(SPIKE_DIM, pixels))
        return;  // already logged

    // 2) THE GATE: hand GL state back to Ganesh. We mutated bound FBO, program,
    //    VAO, depth-test, viewport, clear colour — all of which Ganesh caches
    //    assumptions about. resetContext() makes Skia forget and re-set them.
    main.window.ctx->resetContext();

    // 3) GL gives bottom-up rows; SkBitmap is top-down → flip while copying.
    SkBitmap baked;
    if (!baked.tryAllocPixels(SkImageInfo::Make(
            SPIKE_DIM, SPIKE_DIM, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType))) {
        Logger::get().log("WORLDFATAL", "Armature spike: could not allocate bitmap.");
        return;
    }
    {
        auto* dst = static_cast<uint8_t*>(baked.getPixels());
        const size_t srcRowBytes = static_cast<size_t>(SPIKE_DIM) * 4;
        for (int y = 0; y < SPIKE_DIM; ++y) {
            const uint8_t* srcRow = pixels.data() + static_cast<size_t>(SPIKE_DIM - 1 - y) * srcRowBytes;
            std::memcpy(dst + static_cast<size_t>(y) * baked.rowBytes(), srcRow, srcRowBytes);
        }
    }

    // 4) Bake to a MYPAINTLAYER component centred in the current view — the
    //    exact second half of RasterFlatten::flatten_layer.
    const CoordSpaceHelper& camC = world.drawData.cam.c;
    const Vector2f winSize = main.window.size.cast<float>();
    const WorldVec centerWorld = camC.from_space(winSize * 0.5f);
    const WorldVec halfImgWorld = camC.dir_from_space(Vector2f(SPIKE_DIM * 0.5f, SPIKE_DIM * 0.5f));
    const CoordSpaceHelper coords(centerWorld - halfImgWorld, camC.inverseScale, 0.0);

    auto* comp = new CanvasComponentContainer(world.netObjMan, CanvasComponentType::MYPAINTLAYER);
    comp->coords = coords;
    auto& layer = static_cast<MyPaintLayerCanvasComponent&>(comp->get_comp());
    layer.surface().import_from_bitmap(baked, 0, 0);
    layer.mark_dirty();

    std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> toPlace;
    toPlace.emplace_back(drawP.layerMan.get_edited_layer_end_iterator(), comp);
    const auto placed = drawP.layerMan.add_many_components_to_specific_layer(*editLayer, toPlace);
    for (auto& pit : placed) {
        pit->obj->commit_update(drawP);
        if (pit->obj->get_world_bounds().has_value())  // matches the tools' guard
            drawP.layerMan.add_undo_place_component(&(*pit));
    }

    Logger::get().log("USERINFO",
        "Armature spike: baked a " + std::to_string(SPIKE_DIM) + "px cube to canvas.");
}

}  // namespace ArmatureSpike

#else  // !ARMATURE_SPIKE_ENABLED

namespace ArmatureSpike {
void run_spike(DrawingProgram&) {
    Logger::get().log("USERINFO",
        "Armature spike: needs desktop GL 3.3 + Ganesh + libmypaint in this build.");
}
}  // namespace ArmatureSpike

#endif
