#include "LoadingScreen.hpp"
#include "../MainProgram.hpp"
#include "../GUIHolder.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "DesktopDrawingProgramScreen.hpp"
#include "PhoneDrawingProgramScreen.hpp"
#include <include/core/SkCanvas.h>

using namespace GUIStuff;
using namespace GUIStuff::ElementHelpers;

LoadingScreen::LoadingScreen(MainProgram& m, const CustomEvents::OpenInfiniPaintFileEvent& openFile, std::unique_ptr<Screen> returnScreen):
    Screen(m),
    mOpenFile(openFile),
    mReturnScreen(std::move(returnScreen))
{
    mLabel = "Loading…";
    if(openFile.filePathSource.has_value())
        mLabel = "Loading " + openFile.filePathSource.value().stem().string() + "…";
}

void LoadingScreen::update() {
    // Defer the blocking load by a frame so this screen is presented before the
    // main thread stalls on decompress + deserialize. The screen swap happens in
    // post_callback (after this update), so by our first update() we've already
    // been laid out; waiting one more frame guarantees at least one buffer swap.
    if(mFramesShown < 1) {
        mFramesShown++;
        main.g.gui.set_to_layout();
        return;
    }

    main.create_new_tab(mOpenFile);
    if(mReturnScreen) {
        // Opened from within a canvas — restore the screen we came from so its
        // toolbar/tab state survives (create_new_tab already switched the active
        // tab, so the restored screen renders the newly-opened canvas).
        main.set_screen([this] (std::unique_ptr<Screen>) { return std::move(mReturnScreen); });
    }
    else {
        // Opened from the lobby — mirror FileSelectScreen's platform gating.
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
        main.set_screen([&] (std::unique_ptr<Screen>) { return std::make_unique<PhoneDrawingProgramScreen>(main); });
#else
        main.set_screen([&] (std::unique_ptr<Screen>) { return std::make_unique<DesktopDrawingProgramScreen>(main); });
#endif
    }
}

void LoadingScreen::draw(SkCanvas* canvas) {
    canvas->clear(main.g.gui.io.theme->backColor1);
}

void LoadingScreen::gui_layout_run() {
    auto& gui = main.g.gui;
    gui.new_id("LoadingScreen root", [&] {
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
            },
        }) {
            text_label(gui, mLabel);
        }
    });
}
