#pragma once

#include "Element.hpp"
#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

namespace GUIStuff {

// Sibling of ImageDisplay that takes an in-memory SkImage instead of a
// filesystem path. ImageDisplay caches its decode keyed on path, so a
// file rewritten in place (e.g. the avatar tile after a fresh capture)
// would render stale. MemoryImageDisplay re-reads its image pointer
// every layout() call, so callers refresh by simply passing a new
// sk_sp<SkImage>.
//
// PHASE3.md §4 B.M2 -- top-toolbar avatar tile is the primary consumer.
class MemoryImageDisplay : public Element {
    public:
        MemoryImageDisplay(GUIManager& gui);
        struct Data {
            sk_sp<SkImage> img;
            float radius = 0.0f;
            bool operator==(const Data& d) const = default;
        };
        void layout(const Clay_ElementId& id, const Data& data);
        virtual void clay_draw(SkCanvas* canvas, UpdateInputData& io, Clay_RenderCommand* command, bool skiaAA) override;

    private:
        Data d;
};

}
