#include "MemoryImageDisplay.hpp"
#include "../GUIManager.hpp"
#include <include/core/SkRRect.h>
#include <include/core/SkPaint.h>

namespace GUIStuff {

MemoryImageDisplay::MemoryImageDisplay(GUIManager& gui): Element(gui) {}

void MemoryImageDisplay::layout(const Clay_ElementId& id, const Data& data) {
    if (data != d) {
        d = data;
        gui.invalidate_draw_element(this);
    }
    CLAY(id, {
        .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
        .custom = {.customData = this}
    }) {}
}

void MemoryImageDisplay::clay_draw(SkCanvas* canvas, UpdateInputData& io, Clay_RenderCommand* /*command*/, bool skiaAA) {
    if (d.img) {
        SkPaint p;
        p.setAntiAlias(skiaAA);
        if (d.radius != 0.0f) {
            canvas->save();
            SkRRect r = SkRRect::MakeRectXY(boundingBox.value().get_sk_rect(), d.radius, d.radius);
            canvas->clipRRect(r, skiaAA);
            canvas->drawImageRect(d.img, boundingBox.value().get_sk_rect(),
                                  {SkFilterMode::kLinear, SkMipmapMode::kNone}, &p);
            canvas->restore();
        }
        else {
            canvas->drawImageRect(d.img, boundingBox.value().get_sk_rect(),
                                  {SkFilterMode::kLinear, SkMipmapMode::kNone}, &p);
        }
    }
    else {
        // No image: render the theme's foreground swatch as a placeholder
        // (same fallback shape ImageDisplay uses when its file fails to
        // decode). Callers that want a richer placeholder (e.g., hash-
        // derived color from a pubkey) should pass an image they
        // pre-rendered to that effect instead.
        SkPaint p{io.theme->frontColor1};
        p.setAntiAlias(skiaAA);
        SkRRect r = SkRRect::MakeRectXY(boundingBox.value().get_sk_rect(), d.radius, d.radius);
        canvas->drawRRect(r, p);
    }
}

}
