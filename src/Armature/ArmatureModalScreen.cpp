#include "ArmatureModalScreen.hpp"

#include "ArmatureModel.hpp"
#include "ArmatureSpike.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include "../WorldUndoManager.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../CanvasComponents/ArmatureCanvasComponent.hpp"
#include "../DrawingProgram/DrawingProgram.hpp"
#include "../DrawingProgram/Layers/DrawingProgramLayerManager.hpp"
#include "../DrawingProgram/Layers/DrawingProgramLayerListItem.hpp"

#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/Elements/SelectableButton.hpp"
#include "../GUIStuff/ElementHelpers/ColorPickerHelpers.hpp"
#include "../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"

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
using GUIStuff::ScrollArea;

namespace {
// The 22-slider shape-key system (ARMATURE-SCHEMA.md §2). Binary sliders blend
// between two opposing morphs (range [-1,1], centre 0); singular sliders drive
// one morph (range [0,1]). Morph names are the actual glb target names.
struct ShapeSlider { const char* group; const char* label; bool binary; const char* left; const char* right; };
const ShapeSlider kShapeSliders[] = {
    {"Body", "Gender", true, "mesh_body_female", "mesh_body_male"},
    {"Body", "Build", true, "mesh_body_thin", "mesh_body_fat"},
    {"Head", "Round Head", false, nullptr, "mesh_head_round"},
    {"Head", "Egg Head", false, nullptr, "mesh_head_egg"},
    {"Head", "Box Head", false, nullptr, "mesh_head_box"},
    {"Head", "Smooth Head", false, nullptr, "mesh_head_smooth"},
    {"Eyes", "Eye Shape", true, "mesh_eyes_almond", "mesh_eyes_monolid"},
    {"Eyes", "Eye Size", true, "mesh_eyes_small", "mesh_eyes_large"},
    {"Eyes", "Eye Tilt", true, "mesh_eyes_tilt_in", "mesh_eyes_tilt_out"},
    {"Eyes", "Eye Depth", true, "mesh_eyes_sunken", "mesh_eyes_bulge"},
    {"Eyes", "Eye Distance", true, "mesh_eyes_together", "mesh_eyes_apart"},
    {"Eyes", "Eye Asymmetry", true, "mesh_eyes_left_down", "mesh_eyes_right_down"},
    {"Nose", "Nose Size", true, "mesh_nose_small", "mesh_nose_wide"},
    {"Nose", "Nose Length", true, "mesh_nose_flat", "mesh_nose_long"},
    {"Nose", "Nose Height", true, "mesh_nose_short", "mesh_nose_tall"},
    {"Mouth", "Upper Lip", true, "mesh_mouth_upper_lip_thin", "mesh_mouth_upper_lip_thick"},
    {"Mouth", "Lower Lip", true, "mesh_mouth_lower_lip_thin", "mesh_mouth_lower_lip_thick"},
    {"Mouth", "Mouth Size", true, "mesh_mouth_narrow", "mesh_mouth_wide"},
    {"Mouth", "Mouth Depth", true, "mesh_mouth_sunken", "mesh_mouth_protrude"},
    {"Expression", "Frown / Smile", true, "mesh_expression_frown", "mesh_expression_smile"},
    {"Expression", "Brows", true, "mesh_expression_brows_down", "mesh_expression_brows_up"},
    {"Expression", "Eyes Open", true, "mesh_expression_eyes_closed", "mesh_expression_eyes_wide"},
};
constexpr int kShapeSliderCount = static_cast<int>(sizeof(kShapeSliders) / sizeof(kShapeSliders[0]));
}  // namespace

void ArmatureModalScreen::apply_shape_sliders() {
    if (!mModel) return;
    std::vector<float> w(mModel->target_count(), 0.0f);
    for (int i = 0; i < kShapeSliderCount && i < static_cast<int>(mShapeSliders.size()); ++i) {
        const ShapeSlider& cfg = kShapeSliders[i];
        const float v = mShapeSliders[i];
        const int ri = cfg.right ? mModel->find_target(cfg.right) : -1;
        if (cfg.binary) {
            const int li = cfg.left ? mModel->find_target(cfg.left) : -1;
            if (li >= 0) w[li] = std::max(0.0f, -v);
            if (ri >= 0) w[ri] = std::max(0.0f, v);
        } else if (ri >= 0) {
            w[ri] = v;
        }
    }
    mModel->set_morph_weights(w);
}

ArmatureModalScreen::ArmatureModalScreen(MainProgram& m, std::unique_ptr<Screen> prev,
                                         DrawingProgram* editDrawP,
                                         CanvasComponentContainer::ObjInfo* editTarget)
    : Screen(m), mPrev(std::move(prev)), mEditDrawP(editDrawP), mEditTarget(editTarget) {
    if (!mEditTarget) return;
    mModel = Armature::default_model();
    if (!mModel) return;
    const auto& d = static_cast<ArmatureCanvasComponent&>(mEditTarget->obj->get_comp()).d;
    // Seed the camera (frame_bounds first for a sane near/far fit, then override).
    mCamera.frame_bounds(mModel->bounds_min(), mModel->bounds_max());
    mCamera.yaw = d.camYaw;
    mCamera.pitch = d.camPitch;
    mCamera.distance = d.camDist;
    mCamera.target = Eigen::Vector3f(d.camTx, d.camTy, d.camTz);
    mFovDeg = d.fovDeg;
    mCamera.fovY = mFovDeg * 0.01745329252f;
    mCamera.ortho = d.ortho;
    mFramed = true;
    // Seed the light.
    mLight.azimuth = d.lightAz;
    mLight.elevation = d.lightEl;
    mLight.intensity = d.lightInt;
    mLight.ambient = d.lightAmb;
    mLight.sky = d.lightSky;
    // Seed the pose onto the shared model.
    mModel->reset_pose();
    for (const auto& pe : d.pose) {
        const int j = mModel->find_joint(pe.bone);
        if (j >= 0) mModel->set_joint_pose(j, Eigen::Quaternionf(pe.qw, pe.qx, pe.qy, pe.qz));
    }
    // Seed customization.
    mHeight = d.height;
    mModel->set_height(mHeight);
    // Material colors: reset to glb defaults, apply saved overrides, then mirror
    // the model's live colors into the editable swatches.
    mModel->reset_material_colors();
    for (const auto& mc : d.materialColors) {
        for (int i = 0; i < mModel->material_count(); ++i)
            if (mModel->material_name(i) == mc.material) {
                mModel->set_material_color(i, mc.r, mc.g, mc.b, mc.a);
                break;
            }
    }
    mMatColors.clear();
    for (int i = 0; i < mModel->material_count(); ++i) {
        const auto c = mModel->material_color(i);
        mMatColors.emplace_back(c[0], c[1], c[2], c[3]);
    }
    // Shape keys: seed slider values from d, then push the morph weights.
    mShapeSliders.assign(kShapeSliderCount, 0.0f);
    for (int i = 0; i < kShapeSliderCount && i < static_cast<int>(d.shapeSliders.size()); ++i)
        mShapeSliders[i] = d.shapeSliders[i];
    apply_shape_sliders();
}

ArmatureModalScreen::~ArmatureModalScreen() { destroy_fbo(); }

void ArmatureModalScreen::request_redraw() {
    mDirty = true;
    main.g.gui.io.redrawSurface = true;
}

void ArmatureModalScreen::view_rect(float& x, float& y, float& size) const {
    const float w = static_cast<float>(main.window.size.x());
    const float h = static_cast<float>(main.window.size.y());
    const float scale = main.g.final_gui_scale();
    const float panelPx = PANEL_W * scale;   // left controls strip
    const float topPx = TOP_BAR_H * scale;   // top tab bar
    const float availW = w - panelPx, availH = h - topPx;
    size = std::max(1.0f, std::min(availW, availH));
    x = panelPx + (availW - size) * 0.5f;
    y = topPx + (availH - size) * 0.5f;
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
    if (!b.down) { mOrbiting = mPanning = mZooming = mJointRotating = false; mActiveAxis = -1; return; }
    // Only orbit/pose when the press is inside the 3D viewport square. This blocks
    // the top bar, the controls panel, AND scroll-area content (which cursor_-
    // obstructed misses, since clipped children aren't in the hover-test list).
    if (main.g.gui.cursor_obstructed()) return;
    {
        float rx, ry, rs;
        view_rect(rx, ry, rs);
        if (b.pos.x() < rx || b.pos.x() > rx + rs || b.pos.y() < ry || b.pos.y() > ry + rs)
            return;
    }
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
        // 2) A joint dot → select it; 3) empty space → the active camera tool
        //    (orbit deselects; pan/zoom leave the selection alone).
        const int hit = pick_joint(b.pos.x(), b.pos.y());
        if (hit >= 0) { mSelectedJoint = hit; mActiveAxis = -1; request_redraw(); }
        else if (mCamTool == CamTool::PAN) { mPanning = true; }
        else if (mCamTool == CamTool::ZOOM) { mZooming = true; }
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
    } else if (mZooming) {
        // Drag up → zoom in, down → out (matches the wheel; ~per-pixel dolly).
        mCamera.dolly(std::pow(0.99f, -m.move.y()));
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
    // backColor1 panel + text_label's frontColor1 text is the toolbar's readable
    // combo. convert_vec4 handles the SkColor4f→Clay_Color mapping (0..1 here).
    const Clay_Color panelBg = convert_vec4<Clay_Color>(gui.io.theme->backColor1);
    const std::function<void()> markDirty = [this] { request_redraw(); };

    CLAY_AUTO_ID({
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
    }) {
        // ---- top tab bar: category buttons + Bake/Close ----
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(TOP_BAR_H)},
                .padding = CLAY_PADDING_ALL(gui.io.theme->padding1),
                .childGap = gui.io.theme->childGap1,
                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
            },
            .backgroundColor = panelBg,
        }) {
            auto tab = [&](const char* id, std::string_view label, int t) {
                text_button(gui, id, label,
                            {.isSelected = (mTab == t),
                             .onClick = [this, t] { mTab = t; request_redraw(); }});
            };
            tab("arm tab camera", "Camera", 0);
            tab("arm tab light", "Light", 1);
            tab("arm tab pose", "Pose", 2);
            tab("arm tab body", "Body", 3);
            tab("arm tab materials", "Materials", 4);
            CLAY_AUTO_ID({ .layout = { .sizing = {.width = CLAY_SIZING_GROW(0)} } }) {}  // spacer
            if (mEditTarget)
                text_button(gui, "armature bake", "Bake", {.onClick = [this] { do_bake(); }});
            text_button(gui, "armature close", "Close", {.onClick = [this] { mWantExit = true; }});
        }

        // ---- body: left controls panel (active tab) + 3D view shows through ----
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
            },
        }) {
            CLAY_AUTO_ID({
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIXED(PANEL_W), .height = CLAY_SIZING_GROW(0)},
                    .padding = CLAY_PADDING_ALL(gui.io.theme->padding1),
                    .childGap = gui.io.theme->childGap1,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
                .backgroundColor = panelBg,
            }) {
                if (mTab == 0) {  // Camera
                    // Drag-tool selector (mirrors the canvas toolbar's pan/zoom
                    // icons). Orbit is the default; pan/zoom match hand.svg/zoom.svg.
                    auto camToolBtn = [&](const char* id, const char* svg, CamTool t) {
                        svg_icon_button(gui, id, svg, {
                            .drawType = GUIStuff::SelectableButton::DrawType::TRANSPARENT_ALL,
                            .isSelected = (mCamTool == t),
                            .onClick = [this, t] { mCamTool = t; request_redraw(); }});
                    };
                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = {.width = CLAY_SIZING_GROW(0)},
                            .childGap = gui.io.theme->childGap1,
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        },
                    }) {
                        camToolBtn("arm cam orbit", "data/icons/refresh-line.svg", CamTool::ORBIT);
                        camToolBtn("arm cam pan", "data/icons/hand.svg", CamTool::PAN);
                        camToolBtn("arm cam zoom", "data/icons/zoom.svg", CamTool::ZOOM);
                    }
                    text_label(gui, mCamTool == CamTool::PAN ? "Drag: pan"
                                  : mCamTool == CamTool::ZOOM ? "Drag: zoom" : "Drag: orbit");
                    text_label(gui, "Shift+drag pan • wheel zoom");
                    // Lens: FOV (perspective distortion) + an orthographic toggle.
                    slider_scalar_field<float>(gui, "armature fov", "Field of View",
                        &mFovDeg, 10.0f, 100.0f, {.onEdit = [this] {
                            mCamera.fovY = mFovDeg * 0.01745329252f;  // deg→rad
                            request_redraw();
                        }});
                    text_button(gui, "armature ortho",
                                mCamera.ortho ? "Lens: Orthographic" : "Lens: Perspective",
                                {.isSelected = mCamera.ortho, .wide = true, .onClick = [this] {
                                    mCamera.ortho = !mCamera.ortho; request_redraw();
                                }});
                    text_button(gui, "armature reset view", "Reset View",
                                {.wide = true, .onClick = [this] { mFramed = false; request_redraw(); }});
                } else if (mTab == 1) {  // Light
                    slider_scalar_field<float>(gui, "light azimuth", "Azimuth",
                                               &mLight.azimuth, -3.1416f, 3.1416f, {.onEdit = markDirty});
                    slider_scalar_field<float>(gui, "light elevation", "Elevation",
                                               &mLight.elevation, -1.5f, 1.5f, {.onEdit = markDirty});
                    slider_scalar_field<float>(gui, "light intensity", "Intensity",
                                               &mLight.intensity, 0.0f, 2.0f, {.onEdit = markDirty});
                    slider_scalar_field<float>(gui, "light ambient", "Ambient",
                                               &mLight.ambient, 0.0f, 1.0f, {.onEdit = markDirty});
                } else if (mTab == 2) {  // Pose
                    text_label(gui, (mModel && mSelectedJoint >= 0)
                                        ? ("Selected: " + mModel->joint_name(mSelectedJoint))
                                        : std::string("Grab a joint dot in the view,"));
                    text_label(gui, "then a colored axis ring to rotate.");
                    text_button(gui, "armature reset pose", "Reset Pose",
                                {.wide = true, .onClick = [this] {
                                    if (mModel) { mModel->reset_pose(); request_redraw(); }
                                }});
                } else if (mTab == 3) {  // Body — height + shape keys (scrollable)
                    gui.clipping_element<ScrollArea>("armature body scroll", ScrollArea::Options{
                        .scrollVertical = true,
                        .clipVertical = true,
                        .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
                        .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                            CLAY_AUTO_ID({
                                .layout = {
                                    .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                                    .childGap = gui.io.theme->childGap1,
                                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                },
                            }) {
                                slider_scalar_field<float>(gui, "armature height", "Height",
                                    &mHeight, 0.7f, 1.4f, {.onEdit = [this] {
                                        if (mModel) mModel->set_height(mHeight);
                                        request_redraw();
                                    }});
                                // Collapsible shape-key groups.
                                static const char* kGroups[6] =
                                    {"Body", "Head", "Eyes", "Nose", "Mouth", "Expression"};
                                static const char* kGroupIds[6] =
                                    {"armgrp0", "armgrp1", "armgrp2", "armgrp3", "armgrp4", "armgrp5"};
                                for (int g = 0; g < 6; ++g) {
                                    const bool open = mGroupOpen[g];
                                    text_button(gui, kGroupIds[g],
                                        std::string(open ? "- " : "+ ") + kGroups[g],
                                        {.isSelected = open, .wide = true, .onClick = [this, g] {
                                            mGroupOpen[g] = !mGroupOpen[g];
                                            main.g.gui.set_to_layout();
                                            request_redraw();
                                        }});
                                    if (!open) continue;
                                    for (int i = 0; i < kShapeSliderCount &&
                                                    i < static_cast<int>(mShapeSliders.size()); ++i) {
                                        const ShapeSlider& cfg = kShapeSliders[i];
                                        if (std::string(cfg.group) != kGroups[g]) continue;
                                        const float lo = cfg.binary ? -1.0f : 0.0f;
                                        slider_scalar_field<float>(gui, cfg.label, cfg.label,
                                            &mShapeSliders[i], lo, 1.0f, {.onEdit = [this] {
                                                apply_shape_sliders();
                                                request_redraw();
                                            }});
                                    }
                                }
                            }
                        }
                    });
                } else if (mTab == 4 && mModel) {  // Materials
                    static const char* kMatIds[8] = {"armmat0", "armmat1", "armmat2", "armmat3",
                                                     "armmat4", "armmat5", "armmat6", "armmat7"};
                    const int n = std::min(mModel->material_count(), 8);
                    for (int i = 0; i < n && i < static_cast<int>(mMatColors.size()); ++i) {
                        color_picker_button_field(gui, kMatIds[i], mModel->material_name(i), &mMatColors[i],
                            {.hasAlpha = false, .onEdit = [this, i] {
                                const Vector4f& c = mMatColors[i];
                                mModel->set_material_color(i, c[0], c[1], c[2], c[3]);
                                request_redraw();
                            }});
                    }
                }
            }
            CLAY_AUTO_ID({ .layout = { .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)} } }) {}
        }
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

namespace {
// Swap-based edit undo (mirrors EditTool.cpp's local class): stores the pre-edit
// data; undo/redo swaps the component's current data with the stored copy.
class EditArmatureUndoAction : public WorldUndoAction {
public:
    EditArmatureUndoAction(std::unique_ptr<CanvasComponent> initData, WorldUndoManager::UndoObjectID id)
        : data(std::move(initData)), undoID(id) {}
    std::string get_name() const override { return "Edit Armature"; }
    bool undo(WorldUndoManager& u) override { return swap(u); }
    bool redo(WorldUndoManager& u) override { return swap(u); }
    bool swap(WorldUndoManager& u) {
        const auto netid = u.get_netid_from_undoid(undoID);
        if (!netid.has_value()) return false;
        auto objPtr = u.world.netObjMan.get_obj_temporary_ref_from_id<CanvasComponentContainer>(netid.value());
        std::unique_ptr<CanvasComponent> cur = objPtr->get_comp().get_data_copy();
        objPtr->get_comp().set_data_from(*data);
        data = std::move(cur);
        objPtr->commit_update(u.world.drawProg);
        objPtr->send_comp_update(u.world.drawProg, true);
        return true;
    }
    std::unique_ptr<CanvasComponent> data;
    WorldUndoManager::UndoObjectID undoID;
};
}  // namespace

void ArmatureModalScreen::do_bake() {
    if (!mEditTarget || !mEditDrawP || !mModel) { mWantExit = true; return; }
    constexpr int DIM = 1024;
    std::vector<uint8_t> rgba;
    if (!ArmatureSpike::render_armature_rgba(*mModel, mCamera.view_proj(1.0f), mLight.travel_dir(),
                                             mLight.ambient, mLight.intensity, mLight.sky, DIM, rgba)) {
        Logger::get().log("USERINFO", "Armature bake failed.");
        mWantExit = true;
        return;
    }
#ifdef ARMATURE_MODAL_GL
    main.window.ctx->resetContext();  // hand GL state back to Skia
#endif
    auto& comp = static_cast<ArmatureCanvasComponent&>(mEditTarget->obj->get_comp());
    std::unique_ptr<CanvasComponent> oldData = comp.get_data_copy();  // pre-edit snapshot

    comp.d.camYaw = mCamera.yaw; comp.d.camPitch = mCamera.pitch; comp.d.camDist = mCamera.distance;
    comp.d.camTx = mCamera.target.x(); comp.d.camTy = mCamera.target.y(); comp.d.camTz = mCamera.target.z();
    comp.d.fovDeg = mFovDeg; comp.d.ortho = mCamera.ortho;
    comp.d.lightAz = mLight.azimuth; comp.d.lightEl = mLight.elevation; comp.d.lightInt = mLight.intensity;
    comp.d.lightAmb = mLight.ambient; comp.d.lightSky = mLight.sky;
    comp.d.height = mHeight;
    comp.d.materialColors.clear();
    for (int i = 0; i < mModel->material_count(); ++i) {
        const auto c = mModel->material_color(i);
        comp.d.materialColors.push_back({mModel->material_name(i), c[0], c[1], c[2], c[3]});
    }
    comp.d.shapeSliders = mShapeSliders;
    comp.d.pose.clear();
    for (int j = 0; j < mModel->joint_count(); ++j) {
        const Eigen::Quaternionf q = mModel->joint_pose(j);
        if (q.vec().squaredNorm() > 1e-8f) {  // non-identity → store
            ArmatureCanvasComponent::PoseEntry pe;
            pe.bone = mModel->joint_name(j);
            pe.qx = q.x(); pe.qy = q.y(); pe.qz = q.z(); pe.qw = q.w();
            comp.d.pose.push_back(std::move(pe));
        }
    }
    comp.set_raster(std::move(rgba), DIM);

    mEditTarget->obj->commit_update(*mEditDrawP);
    mEditTarget->obj->send_comp_update(*mEditDrawP, true);
    mEditDrawP->world.undo.push(std::make_unique<EditArmatureUndoAction>(
        std::move(oldData),
        mEditDrawP->world.undo.get_undoid_from_netid(mEditTarget->obj.get_net_id())));

    mWantExit = true;
}

void ArmatureModalScreen::open_for(DrawingProgram& drawP, CanvasComponentContainer::ObjInfo* target) {
    MainProgram* mp = &drawP.world.main;
    DrawingProgram* dp = &drawP;
    mp->set_screen([mp, dp, target](std::unique_ptr<Screen> prev) -> std::unique_ptr<Screen> {
        return std::make_unique<ArmatureModalScreen>(*mp, std::move(prev), dp, target);
    });
}

void ArmatureModalScreen::add_armature_to_canvas(DrawingProgram& drawP) {
    auto& world = drawP.world;
    auto& main = world.main;
    auto editLayer = drawP.layerMan.get_editing_layer().lock();
    if (!editLayer || editLayer->is_folder()) {
        Logger::get().log("USERINFO", "Armature: no active layer to add to.");
        return;
    }
    Armature::ArmatureModel* model = Armature::default_model();
    if (!model) return;  // already logged
    model->reset_pose();
    model->set_height(1.0f);          // default; clear any leftover scale from a prior edit
    model->reset_material_colors();   // default; clear any leftover color overrides
    model->reset_morphs();            // default; clear any leftover shape-key weights
    Armature::OrbitCamera cam;
    cam.frame_bounds(model->bounds_min(), model->bounds_max());
    Armature::Lighting light;

    constexpr int DIM = 1024;
    std::vector<uint8_t> rgba;
    if (!ArmatureSpike::render_armature_rgba(*model, cam.view_proj(1.0f), light.travel_dir(),
                                             light.ambient, light.intensity, light.sky, DIM, rgba))
        return;
#ifdef ARMATURE_MODAL_GL
    main.window.ctx->resetContext();
#endif
    auto* container = new CanvasComponentContainer(world.netObjMan, CanvasComponentType::ARMATURE);
    auto& arm = static_cast<ArmatureCanvasComponent&>(container->get_comp());
    arm.d.camYaw = cam.yaw; arm.d.camPitch = cam.pitch; arm.d.camDist = cam.distance;
    arm.d.camTx = cam.target.x(); arm.d.camTy = cam.target.y(); arm.d.camTz = cam.target.z();
    arm.d.lightAz = light.azimuth; arm.d.lightEl = light.elevation; arm.d.lightInt = light.intensity;
    arm.d.lightAmb = light.ambient; arm.d.lightSky = light.sky;
    arm.set_raster(std::move(rgba), DIM);

    // Place as a square at the view centre, ~half the min window dimension.
    const CoordSpaceHelper& camC = world.drawData.cam.c;
    const float screenPx = 0.5f * std::min(static_cast<float>(main.window.size.x()),
                                           static_cast<float>(main.window.size.y()));
    const WorldVec centerWorld = camC.from_space(main.window.size.cast<float>() * 0.5f);
    const WorldVec half = camC.dir_from_space(Vector2f(screenPx * 0.5f, screenPx * 0.5f));
    const WorldScalar invScale = camC.inverseScale.divide_double(static_cast<double>(DIM) / screenPx);
    container->coords = CoordSpaceHelper(centerWorld - half, invScale, 0.0);

    std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> toPlace;
    toPlace.emplace_back(drawP.layerMan.get_edited_layer_end_iterator(), container);
    const auto placed = drawP.layerMan.add_many_components_to_specific_layer(*editLayer, toPlace);
    for (auto& pit : placed) {
        pit->obj->commit_update(drawP);
        if (pit->obj->get_world_bounds().has_value())
            drawP.layerMan.add_undo_place_component(&(*pit));
    }
    Logger::get().log("USERINFO", "Added an armature — double-click it to pose.");
}
