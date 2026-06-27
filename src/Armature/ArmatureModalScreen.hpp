#pragma once
// PHASE9 (docs/design/PHASE9.md) — M3. The live armature editor as a full-screen
// modal Screen: each frame it renders the loaded figure with a raw-GL pass into
// its own FBO, resetContext()s, and composites the result onto the window canvas;
// the user orbits/pans/zooms with the mouse and tunes the light from a Clay panel.
//
// The World is owned by MainProgram (not the Screen), so entering/leaving this
// modal never touches the canvas — on exit we restore the screen we replaced.
// M4 adds the joint gizmo + FK posing; M5 adds the on-canvas ARMATURE component,
// the widget, and the Bake button. This screen is the shell those build on.

#include "../Screens/Screen.hpp"
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include "../SharedTypes.hpp"
#include "ArmatureView.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Armature { class ArmatureModel; }
class DrawingProgram;

class ArmatureModalScreen : public Screen {
public:
    // `editTarget`/`editDrawP` bind the editor to an on-canvas ARMATURE component:
    // the modal seeds from its saved state and Bake writes it back. Both null =
    // the standalone (dev) editor with no write-back.
    ArmatureModalScreen(MainProgram& m, std::unique_ptr<Screen> prev,
                        DrawingProgram* editDrawP = nullptr,
                        CanvasComponentContainer::ObjInfo* editTarget = nullptr);
    ~ArmatureModalScreen() override;

    // Open the editor on an existing ARMATURE component (double-click re-edit).
    static void open_for(DrawingProgram& drawP, CanvasComponentContainer::ObjInfo* target);
    // Create a new ARMATURE component at the view centre (T-pose, initial bake)
    // and place it on the active layer. The "Add Armature" action.
    static void add_armature_to_canvas(DrawingProgram& drawP);
    // M7: load an external glTF/.glb from disk, embed it in the ResourceManager,
    // and drop it on the canvas (poseable if it matches our rig, else a flattened
    // static reference mesh). The "Load Model" action.
    static void load_model_into_canvas(DrawingProgram& drawP, const std::filesystem::path& path);

    void update() override;
    void draw(SkCanvas* canvas) override;
    void gui_layout_run() override;

    void input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& b) override;
    void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& m) override;
    void input_mouse_wheel_callback(const InputManager::MouseWheelCallbackArgs& w) override;
    void input_key_callback(const InputManager::KeyCallbackArgs& k) override;
    void input_global_back_button_callback() override;

private:
    void request_redraw();          // mark dirty + ask the app to redraw
    void render_3d();               // raw-GL render into the FBO + readback (GL builds only)
    void destroy_fbo();
    void do_bake();                 // render at bake res → write d + raster back → exit

    static constexpr float PANEL_W = 270.0f;     // left controls strip (logical px)
    static constexpr float TOP_BAR_H = 40.0f;    // top tab bar height (logical px)
    int mTab = 2;                                // active tab: 0 Camera 1 Light 2 Pose 3 Body 4 Materials

    std::unique_ptr<Screen> mPrev;            // the screen we replaced (restored on exit)
    DrawingProgram* mEditDrawP = nullptr;                       // bound component's program
    CanvasComponentContainer::ObjInfo* mEditTarget = nullptr;  // bound ARMATURE component
    Armature::ArmatureModel* mModel = nullptr;  // borrowed: owned model or the singleton
    // Set when editing an embedded external model (loaded + GL-uploaded per-edit);
    // mModel points into it. Null when using the shared bundled-default singleton.
    std::unique_ptr<Armature::ArmatureModel> mOwnedModel;
    bool mBundledDefault = true;   // false for any loaded model (gates Body tab)
    Armature::OrbitCamera mCamera;
    Armature::Lighting mLight;
    float mHeight = 1.0f;          // uniform bone-length scale (M5.1a)
    float mFovDeg = 40.0f;         // camera vertical FOV (degrees), drives mCamera.fovY
    std::vector<Vector4f> mMatColors;   // editable per-material colors (M5.1b)
    std::vector<float> mShapeSliders;   // 22 shape-key slider values (M5.1c)
    void apply_shape_sliders();         // slider values → 40 morph weights → model
    // Collapsible shape-key groups (Body/Head/Eyes/Nose/Mouth/Expression).
    std::array<bool, 6> mGroupOpen{true, false, false, false, false, false};
    bool mFramed = false;

    // Offscreen target (square). Persisted across frames; rebuilt on size change.
    unsigned mFbo = 0, mColorTex = 0, mDepthRbo = 0;
    int mFboDim = 0;
    std::vector<uint8_t> mPixels;   // last readback (bottom-up RGBA8, dim*dim*4)
    int mPixelsDim = 0;

    bool mDirty = true;             // 3D needs a re-render
    // Camera drag tool (Camera tab): what an empty-space left-drag does. Gizmo/
    // joint picking still take priority so posing works in any mode. Shift+drag
    // (pan), middle-drag (pan) and the wheel (zoom) remain mode-independent.
    enum class CamTool { ORBIT, PAN, ZOOM };
    CamTool mCamTool = CamTool::ORBIT;
    bool mOrbiting = false, mPanning = false, mZooming = false;
    bool mWantExit = false, mExitQueued = false;

    // FK posing (M4): selection + a 3-axis (R=X,G=Y,B=Z) rotate gizmo.
    int mSelectedJoint = -1;
    bool mJointRotating = false;
    int mActiveAxis = -1;                 // 0/1/2 while dragging an axis dot
    Eigen::Vector2f mDragLastVec = Eigen::Vector2f::Zero();  // cursor−center, screen px

    // The square device-px rect the 3D view composites into (computed from the
    // window size + GUI scale; shared by draw and joint projection/picking).
    void view_rect(float& x, float& y, float& size) const;
    bool project_point(const Eigen::Vector3f& world, float& sx, float& sy) const;
    bool project_joint(int jointIndex, float& sx, float& sy) const;
    int pick_joint(float mouseX, float mouseY) const;       // nearest pickable joint, or -1
    // Screen position of the selected joint's axis-`a` gizmo dot. False if none.
    bool axis_dot_screen(int axis, float& sx, float& sy) const;
    float gizmo_radius() const;           // device px
};
