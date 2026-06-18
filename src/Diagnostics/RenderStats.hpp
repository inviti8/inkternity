#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

// Lightweight per-frame render counters for the perf overlay (PHASE5.5 M0).
//
// Purpose: prove/disprove the two PHASE5.5 hypotheses with live numbers before
// (and while) changing render code:
//   F1 — the window cache fully recomposites every frame the camera moves
//        (watch `windowCacheRebuilt` flicker true throughout a transition).
//   F7 — per-visible-layer saveLayer + scans aren't viewport-culled
//        (watch `visibleLayers` vs `visibleLayersInView`: the gap is wasted
//        isolation buffers, exactly what hiding off-screen layers removed).
//
// Counters are reset at the start of each DrawingProgram::draw via begin_frame()
// and read by Toolbar::performance_metrics(). Global singleton — with multiple
// worlds the last one drawn wins, which is fine for a single-canvas diagnostic.
// Zero cost when the overlay is off (the increments are a handful of int adds on
// the draw path; no allocation, no syscalls).
struct RenderStats {
    // ---- per-frame counters (reset each frame in begin_frame) ----
    bool   windowCacheRebuilt   = false;  // F1: full-window recomposite happened this frame
    int    cachedNodeBlits      = 0;      // BVH node-cache surfaces blitted (cheap, cached)
    int    directComponentDraws = 0;      // components rasterized directly (uncached/unsorted)
    int    saveLayersIssued     = 0;      // F7.1: layer isolation buffers opened this frame
    int    visibleLayers        = 0;      // leaf layers with get_visible()==true that were walked
    int    visibleLayersInView  = 0;      // ...of those, ones that actually had content on screen
    double drawMs               = 0.0;    // wall time spent in DrawingProgram::draw

    // ---- frame-time ring buffer for 1% / 0.1% lows ----
    static constexpr size_t kRingSize = 240;  // ~2-4s of history
    std::array<float, kRingSize> frameMs{};
    size_t ringHead  = 0;
    size_t ringCount = 0;

    void begin_frame() {
        windowCacheRebuilt   = false;
        cachedNodeBlits      = 0;
        directComponentDraws = 0;
        saveLayersIssued     = 0;
        visibleLayers        = 0;
        visibleLayersInView  = 0;
        // drawMs is written at the end of the draw
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
