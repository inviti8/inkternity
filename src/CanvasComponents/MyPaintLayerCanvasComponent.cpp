#include "MyPaintLayerCanvasComponent.hpp"

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>

#ifdef HVYM_HAS_LIBMYPAINT
extern "C" {
#include <mypaint-surface.h>
}
#include <algorithm>
#include <cmath>
#endif

CanvasComponentType MyPaintLayerCanvasComponent::get_type() const {
    return CanvasComponentType::MYPAINTLAYER;
}

#ifdef HVYM_HAS_LIBMYPAINT

MyPaintLayerCanvasComponent::MyPaintLayerCanvasComponent()
    : surface_(std::make_unique<HVYM::Brushes::LibMyPaintSkiaSurface>()) {}

void MyPaintLayerCanvasComponent::mark_dirty() {
    boundsCacheValid_ = false;
    // Drop the per-component draw cache so the next draw() recomposites
    // from the (now-modified) tile data. See header for rationale.
    cachedDrawImage_.reset();
}

void MyPaintLayerCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    // DISTRIBUTION-PHASE1 / RASTER-WIRE-SYNC.md — wire payload mirrors
    // the disk payload (save_file below). Both halves of the on-the-wire
    // bytes for a libmypaint stroke are exactly what save_file writes:
    // tile data via save_tiles_to_archive + the v0.9+ stroke-recording
    // fields. Wire is between peers running the same build, so no
    // version gating — both sides will read what we wrote in the same
    // order. The recording fields cost ~16 bytes + ~12 bytes per sample
    // on top of the tile data; cheap.
    surface_->save_tiles_to_archive(a);
    a(strokeRecordingValid_);
    a(recordedColor_.x(), recordedColor_.y(), recordedColor_.z());
    a(recordedBaseRadius_);
    a(recordedSamples_);
}

void MyPaintLayerCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    surface_->load_tiles_from_archive(a);
    boundsCacheValid_ = false;
    a(strokeRecordingValid_);
    float r, g, b;
    a(r, g, b);
    recordedColor_ = Eigen::Vector3f{r, g, b};
    a(recordedBaseRadius_);
    a(recordedSamples_);
}

void MyPaintLayerCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    surface_->save_tiles_to_archive(a);
    // PHASE2 M1: per-stroke recording. Always written from v0.9 onward;
    // older versions had no recording field.
    a(strokeRecordingValid_);
    a(recordedColor_.x(), recordedColor_.y(), recordedColor_.z());
    a(recordedBaseRadius_);
    a(recordedSamples_);
}

void MyPaintLayerCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    if (version >= VersionNumber(0, 7, 0)) {
        surface_->load_tiles_from_archive(a);
        boundsCacheValid_ = false;
    }
    // Pre-0.7 files had MyPaintLayer save_file as a stub (no bytes
    // written), so there's nothing to read. The layer just stays empty.

    if (version >= VersionNumber(0, 9, 0)) {
        a(strokeRecordingValid_);
        float r, g, b;
        a(r, g, b);
        recordedColor_ = Eigen::Vector3f{r, g, b};
        a(recordedBaseRadius_);
        a(recordedSamples_);
    }
    // Pre-0.9 components stay with strokeRecordingValid_ = false
    // (the ctor default), which makes them no-ops for the stroke
    // vectorize tool — exactly the design-doc behavior.
}

std::unique_ptr<CanvasComponent> MyPaintLayerCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<MyPaintLayerCanvasComponent>();
    toRet->surface_->copy_tiles_from(*surface_);
    toRet->strokeRecordingValid_ = strokeRecordingValid_;
    toRet->recordedColor_       = recordedColor_;
    toRet->recordedBaseRadius_  = recordedBaseRadius_;
    toRet->recordedSamples_     = recordedSamples_;
    toRet->boundsCacheValid_    = false;
    return toRet;
}

void MyPaintLayerCanvasComponent::set_data_from(const CanvasComponent& other) {
    const auto& src = static_cast<const MyPaintLayerCanvasComponent&>(other);
    surface_->copy_tiles_from(*src.surface_);
    strokeRecordingValid_ = src.strokeRecordingValid_;
    recordedColor_        = src.recordedColor_;
    recordedBaseRadius_   = src.recordedBaseRadius_;
    recordedSamples_      = src.recordedSamples_;
    boundsCacheValid_     = false;
}

void MyPaintLayerCanvasComponent::draw(SkCanvas* canvas, const DrawData&, const std::shared_ptr<void>&) const {
    if (surface_->allocated_tile_count() == 0) return;

    // Per-component draw cache (PERF-INVESTIGATION.md finding #2). Build
    // the bitmap + SkImage once per surface mutation; reuse the SkImage
    // for every subsequent draw until mark_dirty() invalidates. Skia's
    // own GPU texture cache then handles the upload-once, blit-many
    // pattern across frames.
    if (!cachedDrawImage_) {
        const auto bounds = surface_->allocated_tile_bounds_px();
        if (bounds.empty()) return;

        SkBitmap bmp;
        if (!bmp.tryAllocPixels(SkImageInfo::Make(
                bounds.w, bounds.h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType))) {
            return;
        }
        bmp.eraseARGB(0, 0, 0, 0);
        surface_->composite_to_bitmap(bmp, bounds.x, bounds.y);

        // setImmutable() before asImage() lets Skia share pixel memory
        // with the resulting SkImage instead of making a defensive
        // copy. The bitmap goes out of scope after this block; the
        // SkImage owns the pixel data via the ref-counted backing.
        bmp.setImmutable();
        cachedDrawImage_ = bmp.asImage();
        cachedDrawX_ = bounds.x;
        cachedDrawY_ = bounds.y;
    }

    SkPaint paint;
    canvas->drawImage(cachedDrawImage_,
                      static_cast<SkScalar>(cachedDrawX_),
                      static_cast<SkScalar>(cachedDrawY_),
                      SkSamplingOptions{},
                      &paint);
}

void MyPaintLayerCanvasComponent::initialize_draw_data(DrawingProgram&) {
}

bool MyPaintLayerCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    // Coarse AABB-vs-AABB. EraserTool needs this to return true so the layer
    // gets through to its dispatch (which then punches destination-out dabs
    // instead of marking the whole component for delete). Per-pixel
    // precision isn't useful here: a dab outside the actually-painted
    // region is a no-op on the raster surface.
    const auto bounds = get_obj_coord_bounds();
    if (bounds.dim().x() <= 0.0f || bounds.dim().y() <= 0.0f) return false;
    return SCollision::collide(checkAgainst.bounds, bounds);
}

void MyPaintLayerCanvasComponent::erase_along_segment(const Vector2f& localStart, const Vector2f& localEnd, float localRadius) {
    if (localRadius <= 0.0f) return;
    // PHASE2 M1: any eraser touch on this stroke's tiles invalidates
    // the recorded path — replaying it would no longer match what the
    // user sees. Per design doc, the simplest correct behavior is
    // "touched stroke is fully consumed from the recording log."
    invalidate_recording();
    MyPaintSurface* surf = surface_->surface();
    const Vector2f delta = localEnd - localStart;
    const float segLen = delta.norm();
    // ~one dab every half-radius along the segment, plus an anchor so a
    // zero-length segment (single click) still punches one dab.
    const float spacing = std::max(localRadius * 0.5f, 1.0f);
    const int steps = std::max(1, static_cast<int>(std::ceil(segLen / spacing)) + 1);
    const int r = static_cast<int>(std::ceil(localRadius));
    bool anyDab = false;
    mypaint_surface_begin_atomic(surf);
    for (int i = 0; i < steps; ++i) {
        const float t = (steps == 1) ? 0.0f : static_cast<float>(i) / static_cast<float>(steps - 1);
        const Vector2f p = localStart + delta * t;
        // Skip dabs whose footprint covers only unallocated tiles. Without
        // this, sweeping the eraser past a stroke's edge into blank canvas
        // makes libmypaint allocate fresh transparent tiles for every dab
        // — growing component bounds, serialized payload, and BVH AABBs
        // for zero visual effect. Reported as eraser sluggishness after
        // repeated use; see docs/design/PERF-INVESTIGATION.md.
        const int pxX0 = static_cast<int>(std::floor(p.x())) - r;
        const int pxY0 = static_cast<int>(std::floor(p.y())) - r;
        if (!surface_->has_any_tile_in_pixel_rect(pxX0, pxY0, 2 * r, 2 * r))
            continue;
        anyDab = true;
        mypaint_surface_draw_dab(
            surf,
            p.x(), p.y(),
            localRadius,
            0.0f, 0.0f, 0.0f,  // color is irrelevant for an eraser dab (alpha_eraser=0)
            1.0f,              // opaque
            0.6f,              // hardness — somewhat soft eraser edge
            0.0f,              // softness  (NOT 1.0; that's libmypaint's draw-skip kill switch)
            0.0f,              // alpha_eraser=0 → destination-out
            1.0f, 0.0f,        // aspect_ratio, angle
            0.0f, 0.0f,        // lock_alpha, colorize
            0.0f, 0.0f,        // posterize, posterize_num
            1.0f               // paint
        );
    }
    mypaint_surface_end_atomic(surf, nullptr);
    if (anyDab) {
        // Reclaim any tile this segment fully cleared. libmypaint never
        // frees tiles, so without this an erased-to-nothing region keeps
        // its 32 KiB buffer (and its contribution to component bounds).
        // Scoped to the segment's pixel footprint so the scan stays cheap
        // at 60 Hz. See PERF-INVESTIGATION.md (monotonic tile growth).
        const float minX = std::min(localStart.x(), localEnd.x()) - localRadius;
        const float minY = std::min(localStart.y(), localEnd.y()) - localRadius;
        const float maxX = std::max(localStart.x(), localEnd.x()) + localRadius;
        const float maxY = std::max(localStart.y(), localEnd.y()) + localRadius;
        surface_->drop_fully_transparent_tiles_in_pixel_rect(
            static_cast<int>(std::floor(minX)), static_cast<int>(std::floor(minY)),
            static_cast<int>(std::ceil(maxX - minX)) + 1,
            static_cast<int>(std::ceil(maxY - minY)) + 1);
        mark_dirty();
    }
}

void MyPaintLayerCanvasComponent::begin_recorded_stroke(const Eigen::Vector3f& color, float baseRadius) {
    recordedSamples_.clear();
    recordedColor_ = color;
    recordedBaseRadius_ = baseRadius;
    strokeRecordingValid_ = true;
}

void MyPaintLayerCanvasComponent::record_stroke_sample(float x, float y, float pressure) {
    if (!strokeRecordingValid_) return;
    recordedSamples_.push_back({x, y, pressure});
}

void MyPaintLayerCanvasComponent::invalidate_recording() {
    strokeRecordingValid_ = false;
    recordedSamples_.clear();   // also free the storage
    recordedSamples_.shrink_to_fit();
}

SCollision::AABB<float> MyPaintLayerCanvasComponent::get_obj_coord_bounds() const {
    if (!boundsCacheValid_) {
        const auto px = surface_->allocated_tile_bounds_px();
        if (px.empty()) {
            boundsCache_ = SCollision::AABB<float>{};
        } else {
            boundsCache_.min = Vector2f(static_cast<float>(px.x), static_cast<float>(px.y));
            boundsCache_.max = Vector2f(static_cast<float>(px.x + px.w),
                                        static_cast<float>(px.y + px.h));
        }
        boundsCacheValid_ = true;
    }
    return boundsCache_;
}

#else  // !HVYM_HAS_LIBMYPAINT

MyPaintLayerCanvasComponent::MyPaintLayerCanvasComponent() {}
void MyPaintLayerCanvasComponent::save(cereal::PortableBinaryOutputArchive&) const {}
void MyPaintLayerCanvasComponent::load(cereal::PortableBinaryInputArchive&) {}
void MyPaintLayerCanvasComponent::save_file(cereal::PortableBinaryOutputArchive&) const {}
void MyPaintLayerCanvasComponent::load_file(cereal::PortableBinaryInputArchive&, VersionNumber) {}
std::unique_ptr<CanvasComponent> MyPaintLayerCanvasComponent::get_data_copy() const {
    return std::make_unique<MyPaintLayerCanvasComponent>();
}
void MyPaintLayerCanvasComponent::set_data_from(const CanvasComponent&) {}
void MyPaintLayerCanvasComponent::draw(SkCanvas*, const DrawData&, const std::shared_ptr<void>&) const {}
void MyPaintLayerCanvasComponent::initialize_draw_data(DrawingProgram&) {}
bool MyPaintLayerCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>&) const { return false; }
SCollision::AABB<float> MyPaintLayerCanvasComponent::get_obj_coord_bounds() const { return {}; }
// No libmypaint in this build — erase has nothing to do (the layer can't
// have any pixels in the first place).
// (mark_dirty / surface() are gated by HVYM_HAS_LIBMYPAINT in the header.)

#endif
