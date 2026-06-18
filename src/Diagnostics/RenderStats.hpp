#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

// Lightweight per-frame render counters for the perf overlay (PHASE5.5 M0).
//
// Purpose: prove/disprove the PHASE5.5 hypotheses with live numbers before (and
// while) changing render code — F1 (window cache recomposites every motion
// frame), F7 (per-visible-layer saveLayer not viewport-culled), and localizing
// any cost that turns out to live outside DrawingProgram.
//
// Double-buffered so reset placement can't race the overlay read:
//   - `live`  — the frame currently being built; every counter site writes here.
//   - `shown` — the last completed frame; the overlay reads ONLY this.
// begin_frame() publishes live -> shown and clears live. Because the overlay
// reads `shown` (stable for the whole frame), it does not matter where in the
// frame begin_frame() runs or where the GUI reads — no more read-before-write.
//
// Global singleton; with multiple worlds the last one drawn wins, fine for a
// single-canvas diagnostic. Cost is a handful of int adds on the hot paths; the
// overlay is off by default (Settings -> Debug -> show performance metrics).
struct RenderStats {
    struct Frame {
        bool   windowCacheRebuilt   = false;  // F1: full-window recomposite happened this frame
        int    drawCalls            = 0;      // DrawingProgram::draw invocations this frame
        int    treeWalks            = 0;      // full layer-tree walks (draw_components_to_canvas) this frame
        int    cachedNodeBlits      = 0;      // BVH node-cache surfaces blitted (cheap, cached)
        int    directComponentDraws = 0;      // components rasterized directly (uncached/unsorted)
        int    saveLayersIssued     = 0;      // F7.1: layer isolation buffers opened this frame
        int    visibleLayers        = 0;      // leaf-layer VISITS (a layer counts once per walk)
        int    visibleLayersInView  = 0;      // ...of those visits, ones that had content on screen
        int    nodeRebuilds         = 0;      // BVH node-cache surfaces (re)rendered this frame (F2 / cache miss)
        double drawMs               = 0.0;    // total wall time in DrawingProgram::draw this frame
        double updateMs             = 0.0;    // total wall time in DrawingProgram::update (incl. BVH rebuild)
        bool   bvhRebuiltThisFrame  = false;  // full BVH rebuild_cache() fired this frame (expensive)

        // Top-level frame phases (set in main.cpp regular_draw): mainDraw = CPU
        // command recording for the whole window; flush = GPU flushAndSubmit;
        // swap = present (blocks on vsync / GPU backlog).
        double mainDrawMs           = 0.0;
        double flushMs              = 0.0;
        double swapMs               = 0.0;

        // MainProgram::update / World::focus_update phase split — localizes cost
        // that lands outside DrawingProgram (GUI layout, image loading, etc.).
        double guiUpdateMs          = 0.0;    // MainProgram g.update() (Clay layout)
        double focusUpdateMs        = 0.0;    // World::focus_update() total (the focused world's per-frame work)
        double rManUpdateMs         = 0.0;    // ResourceManager::update() (image mipmap decode/upload)
        double camUpdateMs          = 0.0;    // DrawCamera::update_main (smooth move)
        double readerUpdateMs       = 0.0;    // ReaderMode::update (transition state machine)
    };

    Frame live;    // accumulating this frame
    Frame shown;   // last completed frame (overlay reads this)

    // ---- frame-time ring buffer for 1% / 0.1% lows ----
    static constexpr size_t kRingSize = 240;  // ~2-4s of history
    std::array<float, kRingSize> frameMs{};
    size_t ringHead  = 0;
    size_t ringCount = 0;

    // Publish the just-finished frame and start a fresh one. Call once per real
    // frame at a stable boundary (top of SDL_AppIterate).
    void begin_frame() {
        shown = live;
        live = Frame{};
    }

    void push_frame_ms(float ms) {
        frameMs[ringHead] = ms;
        ringHead = (ringHead + 1) % kRingSize;
        if(ringCount < kRingSize)
            ++ringCount;
    }

    // Frame time at the given upper percentile (p=0.99 -> "1% low" framerate:
    // the time worse than 99% of recent frames). Higher ms == worse.
    float percentile_high(float p) const {
        if(ringCount == 0)
            return 0.0f;
        std::array<float, kRingSize> tmp{};
        std::copy(frameMs.begin(), frameMs.begin() + ringCount, tmp.begin());
        std::sort(tmp.begin(), tmp.begin() + ringCount);
        size_t idx = static_cast<size_t>(p * (ringCount - 1));
        return tmp[idx];
    }

    static RenderStats& get() {
        static RenderStats instance;
        return instance;
    }
};
