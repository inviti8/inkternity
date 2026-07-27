#pragma once
// Consolidate Vectors (INFPNT000033). A VECTORGROUP bakes many BRUSHSTROKE
// components from one layer into a single pure-vector component: it stores the
// source strokes' original point data (z-ascending) plus each stroke's coord
// space relative to the group, and draws them in order with their own
// color/alpha. Appearance is pixel-identical to the separate strokes, but the
// whole layer becomes ONE component — one BVH entry, one predraw, one collider
// — which removes the O(N)-per-stroke cost that makes panning a heavy vector
// canvas sluggish (see docs/design/PERF-INVESTIGATION.md; the window cache is
// recomposited every pan frame, drawing each un-cached stroke individually).
//
// Unlike Flatten Layer this does NOT rasterize: the result stays a true vector
// (infinite zoom, tiny memory) and is not gated on libmypaint.
//
// v1 does not override the accurate (extreme-zoom re-clip) draw path a single
// stroke uses past ~2^14x zoom; the group falls back to its transformed static
// SkPath there. Normal and pan-zoom rendering is unaffected.

#include "CanvasComponent.hpp"
#include "BrushStrokeTessellation.hpp"   // BrushStrokeCanvasComponentPoint
#include "../CoordSpaceHelper.hpp"
#include "Helpers/SCollision.hpp"
#include <Helpers/Serializers.hpp>
#include <include/core/SkVertices.h>
#include <memory>
#include <vector>

class VectorGroupCanvasComponent : public CanvasComponent {
    public:
        // One consolidated stroke: its coord space RELATIVE to the group
        // container's coords, plus the untouched source stroke data. Storing the
        // relative coords (not requantized points) keeps each stroke's native
        // tessellation exact and makes the whole group transform-follow when the
        // container is moved/scaled.
        struct SubStroke {
            CoordSpaceHelper coords;   // relative to the group container coords
            std::shared_ptr<std::vector<BrushStrokeCanvasComponentPoint>> points =
                std::make_shared<std::vector<BrushStrokeCanvasComponentPoint>>();
            Vector4f color;
            bool hasRoundCaps = false;
        };

        struct Data {
            std::vector<SubStroke> subStrokes;   // z-ascending (front-to-back draw order)
        } d;

        virtual CanvasComponentType get_type() const override;
        virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load(cereal::PortableBinaryInputArchive& a) override;
        virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
        virtual std::unique_ptr<CanvasComponent> get_data_copy() const override;
        virtual void set_data_from(const CanvasComponent& other) override;

        // A group carries many colors; report none so the color picker doesn't
        // misrepresent it, and ignore recolor requests.
        virtual std::optional<Vector4f> get_stroke_color() const override;
        virtual void change_stroke_color(const Vector4f& newStrokeColor) override;
        virtual uint64_t get_memory_size_bytes() const override;

    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
        bool should_draw_extra(const DrawData& drawData, const CoordSpaceHelper& coords) const override;
        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;

        void read(cereal::PortableBinaryInputArchive& a);   // shared by load / load_file

        // Baked geometry (rebuilt by initialize_draw_data). To cut per-frame draw
        // calls from one-per-stroke to a handful, consecutive same-color opaque
        // strokes are tessellated into ONE SkVertices (positions already in
        // group-object space) drawn in a single drawVertices call. z-order is
        // preserved because batches are emitted front-to-back and triangles
        // within a batch keep source order. A stroke with alpha < 1 gets its own
        // batch (drawn inside a saveLayerAlphaf) so overlaps don't double-blend,
        // exactly as a standalone stroke does. Tradeoff vs per-stroke drawPath:
        // drawVertices doesn't per-edge antialias, so edges are slightly harder —
        // accepted for the large draw-call reduction (still true vector, scales
        // cleanly, unlike a raster bake).
        struct Batch {
            sk_sp<SkVertices> verts;
            Vector4f color;    // rgb used; a is the fill alpha (1 for opaque batches)
            float alpha;       // < 1 → wrap in saveLayerAlphaf(alpha)
        };
        std::vector<Batch> batches;
        SCollision::AABB<float> bounds;
        std::vector<SCollision::AABB<float>> precheckAABBLevels;
};
