#include "LegacyFxRenderer.hpp"

#ifdef HVYM_HAS_TIMELINEFX_LEGACY

#include <algorithm>

#include "LegacyFxLibrary.hpp"   // LegacyAnimImage (holds the decoded SkImage)

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>

LegacyFxRenderer::LegacyFxRenderer(int particles, int layers)
    : TLFX::ParticleManager(particles, layers) {}

void LegacyFxRenderer::DrawSprite(TLFX::AnimImage* sprite, float px, float py, float frame,
                                  float x, float y, float rotation, float scaleX, float scaleY,
                                  unsigned char r, unsigned char g, unsigned char b, float a,
                                  bool additive) {
    auto* si = static_cast<LegacyAnimImage*>(sprite);
    if (!si || !si->image || !_canvas) return;

    SkPaint paint;
    // Per-particle tint: modulate the (white/grey) shape by the particle rgb.
    paint.setColorFilter(SkColorFilters::Blend(SkColorSetARGB(255, r, g, b),
                                               SkBlendMode::kModulate));
    paint.setAlphaf(a < 0.f ? 0.f : (a > 1.f ? 1.f : a));
    paint.setBlendMode(additive ? SkBlendMode::kPlus : SkBlendMode::kSrcOver);

    // Animated shapes are grid sprite-sheets: GetWidth/Height = frame size,
    // si->image = whole sheet, columns = sheetWidth / frameWidth, row-major.
    float fw = sprite->GetWidth(), fh = sprite->GetHeight();
    const float sheetW = static_cast<float>(si->image->width());
    const float sheetH = static_cast<float>(si->image->height());
    if (fw <= 0.f || fh <= 0.f) { fw = sheetW; fh = sheetH; }
    const int cols  = std::max(1, static_cast<int>(sheetW / fw + 0.5f));
    const int count = std::max(1, sprite->GetFramesCount());
    int fi = static_cast<int>(frame);
    fi = ((fi % count) + count) % count;
    const float srcX = (fi % cols) * fw, srcY = (fi / cols) * fh;

    _canvas->save();
    _canvas->translate(px, py);
    _canvas->rotate(rotation);              // DrawSprite rotation is in degrees
    _canvas->scale(scaleX, scaleY);
    // x,y = image handle in frame pixels -> pivot the quad there.
    _canvas->drawImageRect(si->image, SkRect::MakeXYWH(srcX, srcY, fw, fh),
                           SkRect::MakeXYWH(-x, -y, fw, fh),
                           SkSamplingOptions(SkFilterMode::kLinear), &paint,
                           SkCanvas::kStrict_SrcRectConstraint);
    _canvas->restore();
    ++drawn;
}

#endif // HVYM_HAS_TIMELINEFX_LEGACY
