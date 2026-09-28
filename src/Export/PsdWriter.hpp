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

// A record in the PSD layer stack. Most are Image layers; groups (folders) are
// delimited by a pair of marker records around their children (see build_psd).
enum class LayerKind {
    Image,        // a pixel layer (rgba)
    GroupOpen,    // folder header (lsct type 1): the group's name, sits ABOVE its children
    GroupClose    // bounding divider "</Layer group>" (lsct type 3): sits BELOW its children
};

// One PSD record. For Image, `rgba` is straight (un-premultiplied) 8-bit RGBA,
// row-major, top-down, tightly packed (width*height*4 bytes), and (x,y) is the
// top-left in document space. `blend` is a 4-char PSD blend key ("norm", "mul ", …).
struct Layer {
    LayerKind            kind = LayerKind::Image;
    std::string          name;
    int32_t              x = 0, y = 0;
    int32_t              width = 0, height = 0;
    std::vector<uint8_t> rgba;
    uint8_t              opacity = 255;
    bool                 visible = true;
    std::string          blend = "norm";
};

// Build a PSD for a `docW`x`docH` document. Records are ordered BOTTOM to TOP (PSD
// stores them bottom-first). A group is: a GroupClose divider, then the group's
// children (bottom-to-top), then a GroupOpen header — nesting allowed. A merged
// composite is generated from the Image records so preview-based readers show
// content. Returns the file bytes (empty on invalid input).
std::string build_psd(int32_t docW, int32_t docH, const std::vector<Layer>& recordsBottomToTop);

}  // namespace PsdExport
