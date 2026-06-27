#include "ArmatureModalScreen.hpp"

#include "ArmatureModel.hpp"
#include "../MainProgram.hpp"

#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"

#include <Helpers/ConvertVec.hpp>
#include <Helpers/Logger.hpp>

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>

#include <algorithm>
#include <cstring>

// GL render path is desktop-GL-3.3 + Ganesh only (same guard family as the rest
// of the armature code). Elsewhere render_3d() is a no-op and the view is blank.
#if defined(USE_BACKEND_OPENGL) && !defined(__EMSCRIPTEN__) && \
    !defined(USE_BACKEND_OPENGLES_3_0) && !defined(USE_BACKEND_OPENGL_2_1) && \
    defined(USE_SKIA_BACKEND_GANESH)
#define ARMATURE_MODAL_GL 1
#include <glad/gl3_3.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#endif

using namespace GUIStuff::ElementHelpers;

ArmatureModalScreen::ArmatureModalScreen(MainProgram& m, std::unique_ptr<Screen> prev)
    : Screen(m), mPrev(std::move(prev)) {}

ArmatureModalScreen::~ArmatureModalScreen() { destroy_fbo(); }

void ArmatureModalScreen::request_redraw() {
    mDirty = true;
    main.g.gui.io.redrawSurface = true;
}

void ArmatureModalScreen::view_rect(float& x, float& y, float& size) const {
    const float w = static_cast<float>(main.window.size.x());
    const float h = static_cast<float>(main.window.size.y());
    const float panelPx = PANEL_W * main.g.final_gui_scale();
    size = std::max(1.0f, std::min(w - panelPx, h));
    x = panelPx + (w - panelPx - size) * 0.5f;
    y = (h - size) * 0.5f;
}

// Project a render-space point to device pixels within the view rect.
bool ArmatureModalScreen::project_point(const Eigen::Vector3f& p, float& sx, float& sy) const {
    const Eigen::Vector4f clip = mCamera.view_proj(1.0f) * Eigen::Vector4f(p.x(), p.y(), p.z(), 1.0f);
    if (clip.w() <= 1e-5f) return false;  // behind the camera
    const Eigen::Vector3f ndc = clip.head<3>() / clip.w();
    float rx, ry, rs;
    view_rect(rx, ry, rs);
    sx = rx + (ndc.x() * 0.5f + 0.5f) * rs;
    sy = ry + (1.0f - (ndc.y() * 0.5f + 0.5f)) * rs;  // GL ndc up → screen down
    return true;
}

bool ArmatureModalScreen::project_joint(int jointIndex, float& sx, float& sy) const {
    if (!mModel) return false;
    return project_point(mModel->joint_world_pos(jointIndex), sx, sy);
}

float ArmatureModalScreen::gizmo_radius() const { return 46.0f * main.g.final_gui_scale(); }

// Fixed screen-space dot positions around the selected joint: X top, Y lower-
// left, Z lower-right. Each is a grab handle to rotate about that local axis.
bool ArmatureModalScreen::axis_dot_screen(int axis, float& sx, float& sy) const {
    if (mSelectedJoint < 0 || axis < 0 || axis > 2) return false;
    float cx, cy;
    if (!project_joint(mSelectedJoint, cx, cy)) return false;
    static const float deg[3] = {-90.0f, 150.0f, 30.0f};
    const float a = deg[axis] * 3.14159265f / 180.0f;
    const float R = gizmo_radius();
    sx = cx + R * std::cos(a);
    sy = cy + R * std::sin(a);
    return true;
}

int ArmatureModalScreen::pick_joint(float mouseX, float mouseY) const {
    if (!mModel) return -1;
    const float thresh = 13.0f * main.g.final_gui_scale();
    const float t2 = thresh * thresh;
    int best = -1;
    float bestD2 = t2;
    for (int j : mModel->pickable_joints()) {
        float sx, sy;
        if (!project_joint(j, sx, sy)) continue;
        const float dx = sx - mouseX, dy = sy - mouseY;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best = j; }
    }
    return best;
}

void ArmatureModalScreen::update() {
    if (mWantExit && !mExitQueued) {
        mExitQueued = true;
        // Deferred screen swap: return the screen we replaced; `self` (this modal)
        // is destroyed after the swap. World is owned by MainProgram → canvas intact.
        main.set_screen([this](std::unique_ptr<Screen> /*self*/) {
            return std::move(mPrev);
        });
    }
}

void ArmatureModalScreen::input_global_back_button_callback() { mWantExit = true; }

void ArmatureModalScreen::input_key_callback(const InputManager::KeyCallbackArgs& k) {
    if (k.down && k.key == InputManager::KEY_GENERIC_ESCAPE) mWantExit = true;
}

void ArmatureModalScreen::input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& b) {
    using MB = InputManager::MouseButton;
    if (!b.down) { mOrbiting = mPanning = mJointRotating = false; mActiveAxis = -1; return; }
    // Don't orbit/pose when the press is on a GUI widget — the GUI sets this on
    // the pointer-over check that runs just before us (scaling-independent).
    if (main.g.gui.cursor_obstructed()) return;
    const bool shift = main.input.key(InputManager::KEY_GENERIC_LSHIFT).held;
    if (b.button == MB::LEFT && !shift) {
        // 1) An axis gizmo dot of the current selection → rotate that axis.
        if (mSelectedJoint >= 0) {
            const float thr = 12.0f * main.g.final_gui_scale();
            int axis = -1;
            float best = thr * thr;
            for (int a = 0; a < 3; ++a) {
                float dx, dy;
                if (!axis_dot_screen(a, dx, dy)) continue;
                const float d2 = (dx - b.pos.x()) * (dx - b.pos.x()) + (dy - b.pos.y()) * (dy - b.pos.y());
                if (d2 <= best) { best = d2; axis = a; }
            }
            if (axis >= 0) {
                mActiveAxis = axis;
                mJointRotating = true;
                float cx, cy;
                project_joint(mSelectedJoint, cx, cy);
                mDragLastVec = Eigen::Vector2f(b.pos.x() - cx, b.pos.y() - cy);
                request_redraw();
                return;
            }
        }
        // 2) A joint dot → select it; 3) empty space → orbit (and deselect).
        const int hit = pick_joint(b.pos.x(), b.pos.y());
        if (hit >= 0) { mSelectedJoint = hit; mActiveAxis = -1; request_redraw(); }
        else { mSelectedJoint = -1; mActiveAxis = -1; mOrbiting = true; request_redraw(); }
    } else if (b.button == MB::LEFT && shift) {
        mPanning = true;
    } else if (b.button == MB::MIDDLE) {
        mPanning = true;
    }
}

void ArmatureModalScreen::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& m) {
    if (mJointRotating && mModel && mSelectedJoint >= 0 && mActiveAxis >= 0) {
        // Rotate about the joint's local axis `mActiveAxis` by the angle the
        // cursor sweeps around the joint's screen centre. Sign follows whether
        // the axis points toward the camera, so the on-screen motion matches.
        float cx, cy;
        if (project_joint(mSelectedJoint, cx, cy)) {
            const Eigen::Vector2f cur(m.pos.x() - cx, m.pos.y() - cy);
            const Eigen::Vector2f last = mDragLastVec;
            const float angle = std::atan2(last.x() * cur.y() - last.y() * cur.x(), last.dot(cur));
            const Eigen::Matrix3f r = mModel->joint_world_matrix(mSelectedJoint).block<3, 3>(0, 0);
            const Eigen::Vector3f axisWorld = r.col(mActiveAxis).normalized();
            const Eigen::Vector3f fwd = (mCamera.target - mCamera.eye()).normalized();
            const float sgn = (axisWorld.dot(fwd) < 0.0f) ? 1.0f : -1.0f;
            Eigen::Vector3f unit = Eigen::Vector3f::Zero();
            unit[mActiveAxis] = 1.0f;  // local axis
            mModel->rotate_joint(mSelectedJoint,
                                 Eigen::Quaternionf(Eigen::AngleAxisf(angle * sgn, unit)));
            mDragLastVec = cur;
            request_redraw();
        }
    } else if (mOrbiting) {
        mCamera.orbit(-m.move.x() * 0.01f, -m.move.y() * 0.01f);
        request_redraw();
    } else if (mPanning) {
        const int side = std::max(1, std::min(main.window.size.x(), main.window.size.y()));
        mCamera.pan(m.move.x(), m.move.y(), mCamera.world_per_pixel(side));
        request_redraw();
    }
}

void ArmatureModalScreen::input_mouse_wheel_callback(const InputManager::MouseWheelCallbackArgs& w) {
    if (main.g.gui.cursor_obstructed()) return;  // let the panel scroll/handle it
    const float ticks = (w.amount.y() != 0.0f) ? w.amount.y() : static_cast<float>(w.tickAmount.y());
    if (ticks == 0.0f) return;
    mCamera.dolly(std::pow(0.88f, ticks));  // wheel up → zoom in
    request_redraw();
}

void ArmatureModalScreen::gui_layout_run() {
    auto& gui = main.g.gui;
    // Match the app's panels: backColor1 background + text_label's frontColor1
    // text is the toolbar's designed-readable combo. Use the same converter the
    // rest of the UI uses (Clay_Color here is 0..1, so don't hand-scale).
    const Clay_Color panelBg = convert_vec4<Clay_Color>(gui.io.theme->backColor1);
    CLAY_AUTO_ID({
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
        },
    }) {
        // Left controls strip.
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = CLAY_SIZING_FIXED(PANEL_W), .height = CLAY_SIZING_GROW(0)},
                .padding = CLAY_PADDING_ALL(gui.io.theme->padding1),
                .childGap = gui.io.theme->childGap1,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
            .backgroundColor = panelBg,
        }) {
            const std::function<void()> markDirty = [this] { request_redraw(); };
            text_label(gui, "Armature Editor (M3)");
            text_label(gui, "Drag: orbit   Shift+drag: pan   Wheel: zoom");
            slider_scalar_field<float>(gui, "light azimuth", "Light azimuth",
                                       &mLight.azimuth, -3.1416f, 3.1416f, {.onEdit = markDirty});
            slider_scalar_field<float>(gui, "light elevation", "Light elevation",
                                       &mLight.elevation, -1.5f, 1.5f, {.onEdit = markDirty});
            slider_scalar_field<float>(gui, "light intensity", "Light intensity",
                                       &mLight.intensity, 0.0f, 2.0f, {.onEdit = markDirty});
            slider_scalar_field<float>(gui, "light ambient", "Ambient",
                                       &mLight.ambient, 0.0f, 1.0f, {.onEdit = markDirty});
            text_label(gui, (mModel && mSelectedJoint >= 0)
                                ? ("Selected: " + mModel->joint_name(mSelectedJoint))
                                : std::string("Selected: (grab a joint dot)"));
            text_button(gui, "armature reset pose", "Reset Pose",
                        {.wide = true, .onClick = [this] {
                            if (mModel) { mModel->reset_pose(); request_redraw(); }
                        }});
            text_button(gui, "armature reset view", "Reset View",
                        {.wide = true, .onClick = [this] { mFramed = false; request_redraw(); }});
            text_button(gui, "armature close", "Close",
                        {.wide = true, .onClick = [this] { mWantExit = true; }});
        }
        // Right area is left empty so the composited 3D view shows through.
        CLAY_AUTO_ID({ .layout = { .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)} } }) {}
    }
}

#ifdef ARMATURE_MODAL_GL

void ArmatureModalScreen::destroy_fbo() {
    if (mDepthRbo) glDeleteRenderbuffers(1, &mDepthRbo);
    if (mColorTex) glDeleteTextures(1, &mColorTex);
    if (mFbo) glDeleteFramebuffers(1, &mFbo);
    mFbo = mColorTex = mDepthRbo = 0;
    mFboDim = 0;
}

void ArmatureModalScreen::render_3d() {
    if (!mModel) { mModel = Armature::default_model(); if (!mModel) return; }
    if (!mFramed) { mCamera.frame_bounds(mModel->bounds_min(), mModel->bounds_max()); mFramed = true; }

    const int side = std::max(64, std::min(main.window.size.x(), main.window.size.y()));
    const int dim = std::min(side, 1024);  // cap readback cost

    if (mFboDim != dim) {
        destroy_fbo();
        glGenFramebuffers(1, &mFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
        glGenTextures(1, &mColorTex);
        glBindTexture(GL_TEXTURE_2D, mColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dim, dim, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mColorTex, 0);
        glGenRenderbuffers(1, &mDepthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, mDepthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, dim, dim);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, mDepthRbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            Logger::get().log("WORLDFATAL", "Armature modal: FBO incomplete.");
            destroy_fbo();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return;
        }
        mFboDim = dim;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
    }

    // Full GL-state spec before clear/draw (M1 finding — Skia leaves it dirty).
    glViewport(0, 0, dim, dim);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    mModel->draw(mCamera.view_proj(1.0f), mLight.travel_dir(),
                 mLight.ambient, mLight.intensity, mLight.sky);

    mPixels.assign(static_cast<size_t>(dim) * dim * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, dim, dim, GL_RGBA, GL_UNSIGNED_BYTE, mPixels.data());
    mPixelsDim = dim;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    main.window.ctx->resetContext();  // THE GATE rule: hand GL state back to Skia
}

#else  // !ARMATURE_MODAL_GL

void ArmatureModalScreen::destroy_fbo() {}
void ArmatureModalScreen::render_3d() {}

#endif

void ArmatureModalScreen::draw(SkCanvas* canvas) {
    // 1) Raw-GL 3D pass (+ resetContext inside) BEFORE any Skia canvas ops.
    if (mDirty) { render_3d(); mDirty = false; }

    // 2) Skia: dark backdrop + the composited figure, centred as a square.
    canvas->clear(SkColorSetARGB(255, 18, 18, 20));
    if (!mPixels.empty() && mPixelsDim > 0) {
        SkBitmap bmp;
        if (bmp.tryAllocPixels(SkImageInfo::Make(
                mPixelsDim, mPixelsDim, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType))) {
            auto* dst = static_cast<uint8_t*>(bmp.getPixels());
            const size_t rowBytes = static_cast<size_t>(mPixelsDim) * 4;
            for (int y = 0; y < mPixelsDim; ++y) {  // GL is bottom-up → flip
                const uint8_t* src = mPixels.data() + static_cast<size_t>(mPixelsDim - 1 - y) * rowBytes;
                std::memcpy(dst + static_cast<size_t>(y) * bmp.rowBytes(), src, rowBytes);
            }
            float rx, ry, rs;
            view_rect(rx, ry, rs);
            canvas->drawImageRect(bmp.asImage(), SkRect::MakeXYWH(rx, ry, rs, rs),
                                  SkSamplingOptions(SkFilterMode::kLinear));
        }
    }

    // 3) Joint handles overlay (M4a): dots for pickable joints, ring for the
    // selected one. Drawn over the figure but under the Clay panel.
    if (mModel) {
        const float scale = main.g.final_gui_scale();
        SkPaint dot;
        dot.setAntiAlias(true);
        dot.setColor(SkColorSetARGB(180, 120, 200, 255));
        for (int j : mModel->pickable_joints()) {
            float sx, sy;
            if (project_joint(j, sx, sy)) canvas->drawCircle(sx, sy, 3.0f * scale, dot);
        }
        if (mSelectedJoint >= 0) {
            float cx, cy;
            if (project_joint(mSelectedJoint, cx, cy)) {
                // Guide circle + 3 axis grab-dots (R=X, G=Y, B=Z).
                SkPaint guide;
                guide.setAntiAlias(true);
                guide.setStyle(SkPaint::kStroke_Style);
                guide.setStrokeWidth(1.5f * scale);
                guide.setColor(SkColorSetARGB(90, 235, 235, 235));
                canvas->drawCircle(cx, cy, gizmo_radius(), guide);

                const SkColor axisCol[3] = {SkColorSetARGB(255, 235, 80, 80),
                                            SkColorSetARGB(255, 90, 205, 90),
                                            SkColorSetARGB(255, 90, 150, 245)};
                for (int a = 0; a < 3; ++a) {
                    float dx, dy;
                    if (!axis_dot_screen(a, dx, dy)) continue;
                    SkPaint line;
                    line.setAntiAlias(true);
                    line.setStyle(SkPaint::kStroke_Style);
                    line.setStrokeWidth(2.0f * scale);
                    line.setColor(axisCol[a]);
                    canvas->drawLine(cx, cy, dx, dy, line);
                    SkPaint fill;
                    fill.setAntiAlias(true);
                    fill.setColor(axisCol[a]);
                    canvas->drawCircle(dx, dy, (a == mActiveAxis ? 8.0f : 6.0f) * scale, fill);
                }
                SkPaint center;
                center.setAntiAlias(true);
                center.setColor(SkColorSetARGB(255, 255, 255, 255));
                canvas->drawCircle(cx, cy, 3.0f * scale, center);
            }
        }
    }
}
