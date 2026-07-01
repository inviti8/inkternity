#pragma once
#include "CanvasComponent.hpp"
#include "Helpers/SCollision.hpp"
#include <Eigen/Core>

#ifdef HVYM_HAS_LIBMYPAINT
#include "../Brushes/LibMyPaintSkiaSurface.hpp"
#endif

#include <memory>
#include <vector>

// PHASE1.md §3 — per-stroke raster layer that owns a LibMyPaintSkiaSurface.
// One MyPaintBrushTool stroke produces one of these, mirroring how each
// vector BrushTool stroke produces a BrushStrokeCanvasComponent. Tiles live
// in component-local pixel coordinates; the container's CoordSpaceHelper
// places the layer in world space, exactly like BrushStroke.
//
// Serialization (both disk and wire) goes through the surface's
// save_tiles_to_archive / load_tiles_from_archive helpers plus the
// v0.9+ stroke-recording fields. See RASTER-WIRE-SYNC.md for the wire
// path's design — the wire payload mirrors the disk payload because
// raster strokes can't be cheaply re-derived the way vector strokes
// can. Bandwidth: typical strokes ~50-200 KB on the wire; large
// strokes up to ~1 MB. Acceptable for Phase 1; recording-replay
// optimization deferred.
class MyPaintLayerCanvasComponent : public CanvasComponent {
    public:
        MyPaintLayerCanvasComponent();

        virtual CanvasComponentType get_type() const override;
        virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load(cereal::PortableBinaryInputArchive& a) override;
        virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
        virtual std::unique_ptr<CanvasComponent> get_data_copy() const override;
        virtual void set_data_from(const CanvasComponent& other) override;
        virtual uint64_t get_memory_size_bytes() const override;

#ifdef HVYM_HAS_LIBMYPAINT
        // Public so the tool can hand it to a MyPaintBrush mid-stroke.
        HVYM::Brushes::LibMyPaintSkiaSurface& surface() { return *surface_; }

        // Called by the tool after each end_atomic so cached drawables
        // (currently just the bounds) recompute on the next draw.
        void mark_dirty();

        // Punch destination-out dabs along the segment localStart -> localEnd.
        // localRadius is in component-local pixels (caller is responsible for
        // converting camera-space radius via the same coord transform used
        // for the endpoints). Used by EraserTool — see PHASE1.md §4 risks
        // ("destination-out dab pass on the raster surface for libmypaint
        // layers, unchanged path-erase for vector strokes").
        void erase_along_segment(const Vector2f& localStart, const Vector2f& localEnd, float localRadius);

        // PHASE2 M1: per-stroke recording. Each MyPaintLayer component
        // holds at most one libmypaint stroke (M3 architecture: one
        // pen-down → one component). The brush tool calls
        // begin_recorded_stroke at pen-down, then record_stroke_sample
        // for each pen-motion event. The samples store the artist's pen
        // path (component-local coords + pressure) — that's the input
        // libmypaint converts into dabs, and it's the right input for
        // M2's bezier fitting (smoother + more truthful to artist intent
        // than the post-discretization dab positions).
        //
        // The recording is "valid" until invalidate_recording() is
        // called. The eraser invalidates on touch — once a stroke's
        // raster has been independently modified, replaying its
        // recorded path would no longer match what the user sees.
        struct RecordedStrokeSample {
            float x, y;       // component-local coords (same space as tile data)
            float pressure;   // 0..1, multiplies brush radius for spine width
            template <typename Archive> void serialize(Archive& a) { a(x, y, pressure); }
        };
        void begin_recorded_stroke(const Eigen::Vector3f& color, float baseRadius);
        void record_stroke_sample(float x, float y, float pressure);
        void invalidate_recording();

        bool has_valid_recording() const { return strokeRecordingValid_ && !recordedSamples_.empty(); }
        const std::vector<RecordedStrokeSample>& get_recorded_samples() const { return recordedSamples_; }
        Eigen::Vector3f get_recorded_color() const { return recordedColor_; }
        float get_recorded_base_radius() const { return recordedBaseRadius_; }
#endif

    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;

#ifdef HVYM_HAS_LIBMYPAINT
        // unique_ptr so the component is movable/copyable-controlled even
        // though LibMyPaintSkiaSurface is non-copyable. get_data_copy makes a
        // fresh empty surface — copying a populated raster layer is a
        // follow-up (M4 file-format work needs the same machinery).
        std::unique_ptr<HVYM::Brushes::LibMyPaintSkiaSurface> surface_;

        mutable bool boundsCacheValid_ = false;
        mutable SCollision::AABB<float> boundsCache_{};

        // PERF-INVESTIGATION.md finding #2: composite_to_bitmap + heap
        // alloc + GPU upload was happening every frame per stroke for
        // any component not in the BVH draw cache. Particularly bad
        // after an eraser pass: the eraser mutates many components at
        // once, all get pulled back to unsortedComponents, and the
        // per-frame redraw cost compounds until the BVH rebuilds.
        //
        // Cache the composited SkImage; invalidate only on mark_dirty
        // (which is called after every surface mutation: begin/continue
        // brush stroke and erase_along_segment). Subsequent draws with
        // an unmutated surface just blit the cached image — no
        // allocate, no per-tile composite, no per-pixel premul math.
        //
        // `mutable` because draw() is const; the cache is implementation
        // detail, not part of logical state. Same thread for read/write
        // (input + render thread), so no synchronization needed.
        mutable sk_sp<SkImage> cachedDrawImage_;
        mutable int cachedDrawX_ = 0;
        mutable int cachedDrawY_ = 0;

        // PHASE2 M1: stroke recording state. Set on begin_recorded_stroke,
        // appended on record_stroke_sample, dropped on invalidate_recording.
        std::vector<RecordedStrokeSample> recordedSamples_;
        Eigen::Vector3f recordedColor_{0.0f, 0.0f, 0.0f};
        float recordedBaseRadius_ = 0.0f;
        bool strokeRecordingValid_ = false;
#endif
};
