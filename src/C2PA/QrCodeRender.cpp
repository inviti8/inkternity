#include "QrCodeRender.hpp"

#include "qrcodegen.hpp"

#include <Helpers/Logger.hpp>

#include <include/core/SkAlphaType.h>
#include <include/core/SkBitmap.h>
#include <include/core/SkColorType.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include <algorithm>
#include <cstring>

namespace C2PA {

sk_sp<SkImage> render_qr_image(std::string_view payload, int out_px) {
    if (payload.empty() || out_px < 16) return nullptr;

    // Encode with Medium ECC — a comfortable balance between density
    // and damage tolerance for screen-displayed codes. Low would also
    // work for a 56-char Stellar address; Medium gives more headroom
    // if the artist's camera is mid-quality.
    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
        std::string(payload).c_str(),
        qrcodegen::QrCode::Ecc::MEDIUM);
    const int matrix = qr.getSize();
    if (matrix <= 0) return nullptr;

    // 4-module quiet-zone on each side, per QR spec.
    constexpr int kQuiet = 4;
    const int totalModules = matrix + 2 * kQuiet;
    const int px_per_module = std::max(1, out_px / totalModules);
    const int bitmap_px     = px_per_module * totalModules;

    SkImageInfo info = SkImageInfo::Make(
        bitmap_px, bitmap_px,
        kRGBA_8888_SkColorType,
        kUnpremul_SkAlphaType);
    SkBitmap bmp;
    if (!bmp.tryAllocPixels(info)) {
        Logger::get().log("INFO",
            "[C2PA::QR] tryAllocPixels failed for "
            + std::to_string(bitmap_px) + "x" + std::to_string(bitmap_px));
        return nullptr;
    }

    // Start with white quiet-zone.
    bmp.eraseColor(SK_ColorWHITE);

    // Paint each "dark" module as a black px_per_module x px_per_module
    // block at (qx, qy) where (qx,qy) is the module's top-left in
    // bitmap coords.
    uint32_t* pixels = static_cast<uint32_t*>(bmp.getPixels());
    if (!pixels) return nullptr;
    const size_t rowPx = bmp.rowBytesAsPixels();

    // RGBA8888 unpremul black with full alpha.
    constexpr uint32_t kBlack = 0xFF000000;  // little-endian: R=0 G=0 B=0 A=0xFF

    for (int my = 0; my < matrix; ++my) {
        for (int mx = 0; mx < matrix; ++mx) {
            if (!qr.getModule(mx, my)) continue;
            const int x0 = (kQuiet + mx) * px_per_module;
            const int y0 = (kQuiet + my) * px_per_module;
            for (int dy = 0; dy < px_per_module; ++dy) {
                uint32_t* row = pixels + (y0 + dy) * rowPx + x0;
                std::fill(row, row + px_per_module, kBlack);
            }
        }
    }

    bmp.setImmutable();
    return bmp.asImage();
}

}  // namespace C2PA
