#include "DrawingProgramScreen.hpp"
#include "../MainProgram.hpp"
#include "LoadingScreen.hpp"

DrawingProgramScreen::DrawingProgramScreen(MainProgram& m):
    Screen(m)
{}

void DrawingProgramScreen::update() {
    main.world->focus_update();
}

void DrawingProgramScreen::draw(SkCanvas* canvas) {
    main.draw_world(canvas, main.world, main.world->drawData);
}

void DrawingProgramScreen::input_add_file_to_canvas_callback(const CustomEvents::AddFileToCanvasEvent& addFile) {
    main.world->input_add_file_to_canvas_callback(addFile);
}

void DrawingProgramScreen::input_open_infinipaint_file_callback(const CustomEvents::OpenInfiniPaintFileEvent& openFile) {
    // Large canvases load synchronously on the main thread (seconds of
    // decompress + deserialize) — show a one-frame "Loading…" interstitial
    // first, then restore this screen so the new tab opens with toolbar state
    // intact. A buffer-backed (networked/web) open carries a string_view that
    // can't be deferred, so fall through to the immediate path.
    if(openFile.filePathSource.has_value() && openFile.fileDataBuffer.empty()) {
        main.set_screen([this, openFile] (std::unique_ptr<Screen> old) {
            return std::make_unique<LoadingScreen>(main, openFile, std::move(old));
        });
        return;
    }
    main.create_new_tab(openFile);
}

void DrawingProgramScreen::input_paste_callback(const CustomEvents::PasteEvent& paste) {
    main.world->input_paste_callback(paste);
}

void DrawingProgramScreen::input_drop_file_callback(const InputManager::DropCallbackArgs& drop) {
    if(std::filesystem::is_regular_file(drop.data)) {
        std::filesystem::path droppedFilePath(drop.data);
        if(droppedFilePath.has_extension() && droppedFilePath.extension().string() == std::string("." + World::FILE_EXTENSION)) {
            CustomEvents::emit_event<CustomEvents::OpenInfiniPaintFileEvent>({
                .isClient = false,
                .filePathSource = droppedFilePath
            });
            return;
        }
    }
    main.world->input_drop_file_callback(drop);
}

void DrawingProgramScreen::input_drop_text_callback(const InputManager::DropCallbackArgs& drop) {
    main.world->input_drop_text_callback(drop);
}

void DrawingProgramScreen::input_key_callback(const InputManager::KeyCallbackArgs& key) {
    main.world->input_key_callback(key);
}

void DrawingProgramScreen::input_text_key_callback(const InputManager::KeyCallbackArgs& key) {
    main.world->input_text_key_callback(key);
}

void DrawingProgramScreen::input_text_callback(const InputManager::TextCallbackArgs& text) {
    main.world->input_text_callback(text);
}

void DrawingProgramScreen::input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button) {
    main.world->input_mouse_button_callback(button);
}

void DrawingProgramScreen::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    main.world->input_mouse_motion_callback(motion);
}

void DrawingProgramScreen::input_mouse_wheel_callback(const InputManager::MouseWheelCallbackArgs& wheel) {
    main.world->input_mouse_wheel_callback(wheel);
}

void DrawingProgramScreen::input_pen_button_callback(const InputManager::PenButtonCallbackArgs& button) {
    main.world->input_pen_button_callback(button);
}

void DrawingProgramScreen::input_pen_touch_callback(const InputManager::PenTouchCallbackArgs& touch) {
    main.world->input_pen_touch_callback(touch);
}

void DrawingProgramScreen::input_pen_motion_callback(const InputManager::PenMotionCallbackArgs& motion) {
    main.world->input_pen_motion_callback(motion);
}

void DrawingProgramScreen::input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis) {
    main.world->input_pen_axis_callback(axis);
}

void DrawingProgramScreen::input_multi_finger_touch_callback(const InputManager::MultiFingerTouchCallbackArgs& touch) {
    main.world->input_multi_finger_touch_callback(touch);
}

void DrawingProgramScreen::input_multi_finger_motion_callback(const InputManager::MultiFingerMotionCallbackArgs& motion) {
    main.world->input_multi_finger_motion_callback(motion);
}

void DrawingProgramScreen::input_finger_touch_callback(const InputManager::FingerTouchCallbackArgs& touch) {
}

void DrawingProgramScreen::input_finger_motion_callback(const InputManager::FingerMotionCallbackArgs& motion) {
}

void DrawingProgramScreen::input_window_resize_callback(const InputManager::WindowResizeCallbackArgs& w) {
}

void DrawingProgramScreen::input_window_scale_callback(const InputManager::WindowScaleCallbackArgs& w) {
}

std::optional<InputManager::TextBoxStartInfo> DrawingProgramScreen::get_text_box_start_info() {
    return main.world->get_text_box_start_info();
}
