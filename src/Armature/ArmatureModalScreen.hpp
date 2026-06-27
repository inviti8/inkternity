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
#include "ArmatureView.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Armature { class ArmatureModel; }

class ArmatureModalScreen : public Screen {
public:
    ArmatureModalScreen(MainProgram& m, std::unique_ptr<Screen> prev);
    ~ArmatureModalScreen() override;

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

    static constexpr float PANEL_W = 270.0f;  // left controls strip (px)

    std::unique_ptr<Screen> mPrev;            // the screen we replaced (restored on exit)
    Armature::ArmatureModel* mModel = nullptr;  // borrowed (cached singleton)
    Armature::OrbitCamera mCamera;
    Armature::Lighting mLight;
    bool mFramed = false;

    // Offscreen target (square). Persisted across frames; rebuilt on size change.
    unsigned mFbo = 0, mColorTex = 0, mDepthRbo = 0;
    int mFboDim = 0;
    std::vector<uint8_t> mPixels;   // last readback (bottom-up RGBA8, dim*dim*4)
    int mPixelsDim = 0;

    bool mDirty = true;             // 3D needs a re-render
    bool mOrbiting = false, mPanning = false;
    bool mWantExit = false, mExitQueued = false;
};
