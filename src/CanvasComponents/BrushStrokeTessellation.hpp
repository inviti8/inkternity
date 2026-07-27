#pragma once
// Shared vector-stroke geometry core, extracted from BrushStrokeCanvasComponent
// so BrushStrokeCanvasComponent (one stroke) and VectorGroupCanvasComponent
// (many strokes consolidated into one pure-vector component) tessellate
// byte-identically. These are the fidelity-critical routines: the smoothing
// (Catmull-Rom), the wedge/round-cap triangle fan, and the collider BVH. Keep
// them here as free functions so the two components can never drift.

#include <Helpers/Serializers.hpp>
#include <Helpers/SCollision.hpp>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPoint.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

// One sample along a vector stroke (object-space position + brush width).
// Lives here (not in BrushStrokeCanvasComponent) because both the stroke and
// the consolidated group serialize/tessellate these.
struct BrushStrokeCanvasComponentPoint {
    Vector2f pos;
    float width;
    template <typename Archive> void serialize(Archive& a) {
        a(pos, width);
    }
};

namespace BrushTess {

// Matches the constants that used to live in BrushStrokeCanvasComponent.
constexpr float DRAW_MINIMUM_LIMIT = 1.0f;
constexpr unsigned DEFAULT_SMOOTHNESS = 5;
constexpr int AABB_PRECHECK_BVH_LEVELS = 2;

// Catmull-Rom smoothing of a raw point list into a denser polyline.
std::vector<BrushStrokeCanvasComponentPoint> smooth_points(
    const std::vector<BrushStrokeCanvasComponentPoint>& points,
    size_t beginIndex, size_t endIndex, unsigned numOfDivisions);

// Indices where the stroke bends sharply enough to need a wedge join.
std::vector<size_t> get_wedge_indices(const std::vector<BrushStrokeCanvasComponentPoint>& points);

// Decimated point list (keeps first + last) for LOD tessellation.
std::vector<BrushStrokeCanvasComponentPoint> every_nth_point_include_front_and_back(
    const std::vector<BrushStrokeCanvasComponentPoint>& pts, size_t n);

// Tessellate the stroke into triangles. For each triangle, passTriangleFunc is
// called (return true to abort early, e.g. a hit-test that found a collision).
// If bPathBuilder is non-null, the filled outline polygon is appended to it.
// rawPoints/hasRoundCaps describe the source stroke (used for the single-dab
// round-cap case and end caps); smoothedPoints is the output of smooth_points.
void create_triangles(
    const std::function<bool(Vector2f, Vector2f, Vector2f)>& passTriangleFunc,
    const std::vector<BrushStrokeCanvasComponentPoint>& rawPoints, bool hasRoundCaps,
    const std::vector<BrushStrokeCanvasComponentPoint>& smoothedPoints,
    size_t skipVertexCount, std::shared_ptr<SkPathBuilder> bPathBuilder);

// Ramer–Douglas–Peucker simplification of a raw stroke: drops points whose
// perpendicular deviation from the simplified polyline is below `epsilon` (in
// the stroke's own object-space units). Endpoints are always kept. Downstream
// smoothing re-curves through the survivors, so with a small epsilon (a fraction
// of the brush width) the visual change is negligible while triangle count drops
// sharply. Used by Consolidate Vectors to lighten the baked geometry.
std::vector<BrushStrokeCanvasComponentPoint> simplify_points(
    const std::vector<BrushStrokeCanvasComponentPoint>& pts, float epsilon);

// Recursively flatten a BVH into a small list of precheck AABBs at a fixed
// depth (the cheap broad-phase used by collides_within_coords / should_draw).
void build_precheck_levels_from_bvh(
    std::vector<SCollision::AABB<float>>& out, size_t level,
    const std::vector<SCollision::BVHContainer<float>>& levelArray);

// Finish a collider: from an already-populated triangle collection, compute the
// object-space bounds and the precheck-AABB levels. Shared by a single stroke
// (its own triangles) and a consolidated group (all sub-strokes' triangles
// accumulated into one collection → one BVH).
void finalize_collider(
    SCollision::ColliderCollection<float>& strokeObjects,
    SCollision::AABB<float>& outBounds,
    std::vector<SCollision::AABB<float>>& outPrecheckLevels);

}  // namespace BrushTess
