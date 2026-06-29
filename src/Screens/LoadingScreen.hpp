#pragma once
#include "Screen.hpp"
#include "../CustomEvents.hpp"
#include <string>
#include <memory>

// A brief "Loading…" interstitial shown while a large .inkternity file opens.
//
// File loading is fully SYNCHRONOUS on the main/GL thread (World ctor ->
// load_from_file: read -> zstd decompress -> cereal deserialize), so the window
// is frozen for the duration. This screen is presented for one rendered frame
// FIRST, then runs the blocking load on a subsequent frame's update() — so the
// artist sees "Loading…" instead of a frozen previous screen. Only used for
// disk-path opens (a buffer-backed/networked open carries a string_view that
// must be consumed the same frame and cannot be deferred). See
// docs/design/PERF-INVESTIGATION.md.
class LoadingScreen : public Screen {
    public:
        // returnScreen: if provided, this screen is restored after the load
        // (used when opening from within a canvas, to keep toolbar/tab state).
        // If null, a fresh canvas screen is created (used from the file lobby).
        LoadingScreen(MainProgram& m, const CustomEvents::OpenInfiniPaintFileEvent& openFile, std::unique_ptr<Screen> returnScreen = nullptr);
        void update() override;
        void draw(SkCanvas* canvas) override;
        void gui_layout_run() override;
    private:
        CustomEvents::OpenInfiniPaintFileEvent mOpenFile;
        std::unique_ptr<Screen> mReturnScreen;
        std::string mLabel;
        int mFramesShown = 0; // load only after we've been painted at least once
};
