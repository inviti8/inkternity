#include "ArmatureBake.hpp"

#include <Helpers/Logger.hpp>

// Desktop GL 3.3 + Ganesh only (same guard as the editor's render path). The
// bake itself needs no libmypaint — it only renders the model and reads it back.
#if defined(USE_BACKEND_OPENGL) && !defined(__EMSCRIPTEN__) && \
    !defined(USE_BACKEND_OPENGLES_3_0) && !defined(USE_BACKEND_OPENGL_2_1) && \
    defined(USE_SKIA_BACKEND_GANESH)
#define ARMATURE_BAKE_GL 1
#endif

#ifdef ARMATURE_BAKE_GL

#include <glad/gl3_3.h>
#include "ArmatureModel.hpp"

#include <cstring>
#include <vector>

namespace ArmatureBake {

bool render_armature_rgba(Armature::ArmatureModel& model, const Eigen::Matrix4f& viewProj,
                          const Eigen::Vector3f& lightDir, float ambient, float diffuse,
                          float sky, int dim, std::vector<uint8_t>& outTopDown) {
    outTopDown.clear();
    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    bool ok = false;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dim, dim, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    glGenRenderbuffers(1, &depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, dim, dim);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Logger::get().log("WORLDFATAL", "Armature bake: FBO incomplete.");
        goto cleanup;
    }

    // Fully specify state before clear/draw (M1 finding — Skia leaves it dirty).
    glViewport(0, 0, dim, dim);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);  // model materials are double-sided
    glDisable(GL_FRAMEBUFFER_SRGB);  // don't sRGB-encode our linear byte writes (see render_3d)
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    model.draw(viewProj, lightDir, ambient, diffuse, sky);

    {
        std::vector<uint8_t> bottomUp(static_cast<size_t>(dim) * dim * 4, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dim, dim, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
        outTopDown.assign(static_cast<size_t>(dim) * dim * 4, 0);
        const size_t row = static_cast<size_t>(dim) * 4;
        for (int y = 0; y < dim; ++y)
            std::memcpy(&outTopDown[static_cast<size_t>(y) * row],
                        &bottomUp[static_cast<size_t>(dim - 1 - y) * row], row);
    }
    ok = true;

cleanup:
    if (depthRbo) glDeleteRenderbuffers(1, &depthRbo);
    if (colorTex) glDeleteTextures(1, &colorTex);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (!ok) outTopDown.clear();
    return ok;  // caller must resetContext() before the next Skia draw
}

}  // namespace ArmatureBake

#else  // !ARMATURE_BAKE_GL

namespace ArmatureBake {
bool render_armature_rgba(Armature::ArmatureModel&, const Eigen::Matrix4f&, const Eigen::Vector3f&,
                          float, float, float, int, std::vector<uint8_t>& out) {
    out.clear();
    return false;
}
}  // namespace ArmatureBake

#endif
