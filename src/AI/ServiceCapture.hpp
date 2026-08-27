#pragma once
// Shared capture helpers for the hvym-img-tools flows (ReangleFlow, MeshFlow).
//
// Both flows do the same two things with a SquareCanvasCaptureTool result before
// sending it: reject a blank frame, and PNG-encode it. The service expects an
// OPAQUE, WYSIWYG image it can matte (a transparent capture of bare strokes broke
// the isnet matte — REANGLE_PIPELINE.md / project memory), so the capture is taken
// with transparentBackground=false and these helpers judge tone, not alpha.

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/encode/SkPngEncoder.h>

namespace AI {

// Does the capture contain a drawing, or just an empty region? The capture
// composites all visible layers over the canvas background (WYSIWYG, opaque), so an
// empty frame is a near-uniform image (the bare background), not transparency —
// detect it by a lack of tonal range. Fail-open on any read failure.
inline bool capture_has_content(const sk_sp<SkImage>& image) {
    if (!image) return false;
    sk_sp<SkImage> raster = image->makeRasterImage(nullptr);
    SkPixmap pix;
    if (!raster || !raster->peekPixels(&pix)) return true;
    if (pix.info().bytesPerPixel() != 4) return true;   // unknown layout → don't block
    int lo = 255, hi = 0;
    const int w = pix.width(), h = pix.height();
    for (int y = 0; y < h; y += 2) {
        const auto* row = static_cast<const uint8_t*>(pix.addr(0, y));
        if (!row) continue;
        for (int x = 0; x < w; x += 2) {
            const int L = (row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
            if (L < lo) lo = L;
            if (L > hi) hi = L;
        }
    }
    return (hi - lo) > 16;   // a meaningful tonal spread means there's a drawing
}

// Encode a captured square (RGBA) to PNG. std::nullopt if it can't be read.
inline std::optional<std::vector<uint8_t>> encode_capture_png(const sk_sp<SkImage>& image) {
    if (!image) return std::nullopt;
    sk_sp<SkImage> raster = image->makeRasterImage(nullptr);
    SkPixmap pix;
    if (!raster || !raster->peekPixels(&pix)) return std::nullopt;
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, pix, {})) return std::nullopt;
    auto data = stream.detachAsData();
    if (!data) return std::nullopt;
    std::vector<uint8_t> bytes(data->size());
    std::memcpy(bytes.data(), data->bytes(), data->size());
    return bytes;
}

}  // namespace AI
