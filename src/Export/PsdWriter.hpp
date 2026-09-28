#pragma once
// Minimal layered Photoshop (.psd / "8BPS") writer — ANIMATED_IMPORTS.md Phase 3.
// Produces an 8-bit RGBA document with one PSD layer per Inkternity layer, so a
// character built as a layer group can be rigged/animated in an external tool
// (SkelForm imports PSD). Uncompressed (raw) channels, Normal blend; per-layer
// name / offset / opacity / visibility. Byte layout validated against psd-tools.
//
// I/O-free: build_psd() returns the file bytes; the caller writes them (SDL_SaveFile,
// matching WorldScreenshot). No Skia or app types leak in — the caller converts its
// rendered layers to straight-alpha RGBA8 first.

#include <cstdint>
#include <string>
#include <vector>

namespace PsdExport {

// One PSD layer. `rgba` is straight (un-premultiplied) 8-bit RGBA, row-major,
// top-down, tightly packed (width*height*4 bytes). (x,y) is the layer's top-left
// in document space; layers may be smaller than the document.
struct Layer {
    std::string          name;
    int32_t              x = 0, y = 0;
    int32_t              width = 0, height = 0;
    std::vector<uint8_t> rgba;
    uint8_t              opacity = 255;
    bool                 visible = true;
};

// Build a PSD file for a `docW`x`docH` document. Layers are ordered BOTTOM to TOP
// (PSD stores them bottom-first). A merged composite is generated automatically by
// alpha-compositing the layers, so preview-based readers show content. Returns the
// complete file bytes (empty on invalid input, e.g. zero-size document).
std::string build_psd(int32_t docW, int32_t docH, const std::vector<Layer>& layersBottomToTop);

}  // namespace PsdExport
