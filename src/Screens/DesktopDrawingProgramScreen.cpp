#include "DesktopDrawingProgramScreen.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include "../ReaderMode/ReaderMode.hpp"
#include "DrawingProgramScreen.hpp"

DesktopDrawingProgramScreen::DesktopDrawingProgramScreen(MainProgram& m):
    DrawingProgramScreen(m),
    toolbar(m)
{}

void DesktopDrawingProgramScreen::update() {
    toolbar.update();
    DrawingProgramScreen::update();
}

void DesktopDrawingProgramScreen::gui_layout_run() {
    toolbar.layout_run();
    // Branch-choice + back-button overlay for reader mode. Floating
    // element (pinned to screen bottom-center), self-gates on
    // readerMode.is_active(), so it's safe to call unconditionally.
    if (main.world)
        render_reader_branch_overlay(*main.world, main.g.gui);
}

bool DesktopDrawingProgramScreen::app_close_requested() {
    return toolbar.app_close_requested();
}

void DesktopDrawingProgramScreen::input_key_callback(const InputManager::KeyCallbackArgs& key) {
    switch(key.key) {
        case InputManager::KEY_NOGUI: {
            if(key.down && !key.repeat) {
                toolbar.drawGui = !toolbar.drawGui;
                main.g.gui.set_to_layout();
            }
            break;
        }
        case InputManager::KEY_SAVE: {
            if(key.down && !key.repeat)
                toolbar.save_func();
            break;
        }
        case InputManager::KEY_SAVE_AS: {
            if(key.down && !key.repeat)
                toolbar.save_as_func();
            break;
        }
        case InputManager::KEY_OPEN_CHAT: {
            if(key.down && !key.repeat)
                toolbar.open_chatbox();
            break;
        }
    }
    DrawingProgramScreen::input_key_callback(key);
}

void DesktopDrawingProgramScreen::input_text_key_callback(const InputManager::KeyCallbackArgs& key) {
    switch(key.key) {
        case InputManager::KEY_GENERIC_ESCAPE: {
            if(key.down && !key.repeat)
                toolbar.close_chatbox();
            break;
        }
    }
    DrawingProgramScreen::input_text_key_callback(key);
}
