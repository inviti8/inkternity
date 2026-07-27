#include "VectorGroupCanvasComponent.hpp"
#include "BrushStrokeTessellation.hpp"
#include "CanvasComponentContainer.hpp"
#include "../DrawData.hpp"
#include <Helpers/Serializers.hpp>
#include <Helpers/ConvertVec.hpp>
#include <cereal/types/vector.hpp>
#include <include/core/SkPaint.h>
#include <cmath>
#include <optional>

namespace {

// Maps a sub-stroke's object-space point into the group's object space.
// Equivalent to rel.from_space (a uniform scale + rotation + translation), but
// float and inlined. Cm-independent by design: the baked geometry lives in
// group-object space and the engine applies the container's coords at draw, so
// the group transform-follows automatically when moved/scaled.
struct SubXf {
    float tx, ty, sc, cs, sn;
    Vector2f apply(Vector2f p) const {
        const Vector2f sp = sc * p;
        return Vector2f{tx + cs * sp.x() - sn * sp.y(), ty + sn * sp.x() + cs * sp.y()};
    }
};

SubXf make_sub_xf(const CoordSpaceHelper& rel) {
    SubXf x;
    x.tx = static_cast<float>(rel.pos.x());
    x.ty = static_cast<float>(rel.pos.y());
    x.sc = static_cast<float>(rel.inverseScale);
    x.cs = static_cast<float>(std::cos(rel.rotation));
    x.sn = static_cast<float>(std::sin(rel.rotation));
    return x;
}

bool same_color(const Vector4f& a, const Vector4f& b) {
    return a.x() == b.x() && a.y() == b.y() && a.z() == b.z() && a.w() == b.w();
}

}  // namespace

CanvasComponentType VectorGroupCanvasComponent::get_type() const {
    return CanvasComponentType::VECTORGROUP;
}

void VectorGroupCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    uint32_t n = static_cast<uint32_t>(d.subStrokes.size());
    a(n);
    for(const auto& s : d.subStrokes)
        a(s.coords, *s.points, s.color, s.hasRoundCaps);
}

void VectorGroupCanvasComponent::read(cereal::PortableBinaryInputArchive& a) {
    uint32_t n = 0;
    a(n);
    d.subStrokes.clear();
    d.subStrokes.resize(n);   // each SubStroke default-constructs its points vector
    for(auto& s : d.subStrokes)
        a(s.coords, *s.points, s.color, s.hasRoundCaps);
}

void VectorGroupCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    read(a);
}

void VectorGroupCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    save(a);
}

void VectorGroupCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    read(a);
}

std::unique_ptr<CanvasComponent> VectorGroupCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<VectorGroupCanvasComponent>();
    toRet->d = d;   // sub-strokes are immutable; sharing the point vectors is fine
    return toRet;
}

void VectorGroupCanvasComponent::set_data_from(const CanvasComponent& other) {
    d = static_cast<const VectorGroupCanvasComponent&>(other).d;
}

std::optional<Vector4f> VectorGroupCanvasComponent::get_stroke_color() const {
    return std::nullopt;
}

void VectorGroupCanvasComponent::change_stroke_color(const Vector4f& newStrokeColor) {
    // No-op: a consolidated group has many colors.
}

uint64_t VectorGroupCanvasComponent::get_memory_size_bytes() const {
    uint64_t total = 0;
    for(const auto& s : d.subStrokes)
        total += s.points->size() * sizeof(BrushStrokeCanvasComponentPoint);
    return total;
}

void VectorGroupCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const {
    // One drawVertices per batch (a run of same-color opaque strokes, or a single
    // translucent stroke). Positions are already in group-object space; the
    // canvas is at group-object space, so no per-batch transform is needed.
    for(const auto& b : batches) {
        if(!b.verts)
            continue;
        SkPaint paint;
        if(b.alpha < 1.0f) {
            // Isolate so the translucent stroke's self-overlaps don't double-blend
            // (matches a standalone stroke's saveLayerAlphaf).
            canvas->saveLayerAlphaf(nullptr, b.alpha);
            paint.setColor4f(SkColor4f{b.color.x(), b.color.y(), b.color.z(), 1.0f});
            canvas->drawVertices(b.verts, SkBlendMode::kSrcOver, paint);
            canvas->restore();
        } else {
            paint.setColor4f(SkColor4f{b.color.x(), b.color.y(), b.color.z(), b.color.w()});
            canvas->drawVertices(b.verts, SkBlendMode::kSrcOver, paint);
        }
    }
}

void VectorGroupCanvasComponent::initialize_draw_data(DrawingProgram& drawP) {
    batches.clear();
    SCollision::ColliderCollection<float> allTriangles;

    // Accumulate consecutive same-color opaque strokes into one vertex buffer;
    // flush the run whenever the color changes or a translucent stroke appears
    // (translucent strokes get isolated batches to preserve their blending).
    std::vector<SkPoint> runPos;
    std::optional<Vector4f> runColor;
    auto flush_run = [&]() {
        if(runPos.empty())
            return;
        batches.push_back({SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode,
                                                static_cast<int>(runPos.size()),
                                                runPos.data(), nullptr, nullptr),
                           *runColor, 1.0f});
        runPos.clear();
        runColor.reset();
    };

    for(const auto& s : d.subStrokes) {
        if(s.points->empty())
            continue;

        const SubXf xf = make_sub_xf(s.coords);
        std::vector<BrushStrokeCanvasComponentPoint> smoothed =
            BrushTess::smooth_points(*s.points, 0, s.points->size() - 1, BrushTess::DEFAULT_SMOOTHNESS);
        const bool translucent = s.color.w() < 1.0f;

        std::vector<SkPoint> subPos;
        BrushTess::create_triangles([&](Vector2f a, Vector2f b2, Vector2f c) {
            const Vector2f A = xf.apply(a), B = xf.apply(b2), C = xf.apply(c);
            subPos.push_back(convert_vec2<SkPoint>(A));
            subPos.push_back(convert_vec2<SkPoint>(B));
            subPos.push_back(convert_vec2<SkPoint>(C));
            allTriangles.triangle.emplace_back(A, B, C);   // one collider for the whole group
            return false;
        }, *s.points, s.hasRoundCaps, smoothed, 0, nullptr);

        if(subPos.empty())
            continue;

        if(translucent) {
            flush_run();
            batches.push_back({SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode,
                                                    static_cast<int>(subPos.size()),
                                                    subPos.data(), nullptr, nullptr),
                               s.color, s.color.w()});
        } else if(runColor && same_color(*runColor, s.color)) {
            runPos.insert(runPos.end(), subPos.begin(), subPos.end());
        } else {
            flush_run();
            runColor = s.color;
            runPos = std::move(subPos);
        }
    }
    flush_run();

    bounds = SCollision::AABB<float>{};
    precheckAABBLevels.clear();
    if(!allTriangles.triangle.empty())
        BrushTess::finalize_collider(allTriangles, bounds, precheckAABBLevels);
}

bool VectorGroupCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    if(!SCollision::collide(checkAgainst, bounds))
        return false;

    bool deepPrecheck = false;
    for(auto& aabb : precheckAABBLevels) {
        if(SCollision::collide(checkAgainst, aabb)) {
            deepPrecheck = true;
            break;
        }
    }
    if(!deepPrecheck && !precheckAABBLevels.empty())
        return false;

    // Narrow phase: re-tessellate each sub-stroke (transformed into group space)
    // and test its triangles. Runs only on user-driven hit-tests (eraser /
    // select), never per frame.
    for(const auto& s : d.subStrokes) {
        if(s.points->empty())
            continue;
        const SubXf xf = make_sub_xf(s.coords);
        std::vector<BrushStrokeCanvasComponentPoint> smoothed =
            BrushTess::smooth_points(*s.points, 0, s.points->size() - 1, BrushTess::DEFAULT_SMOOTHNESS);
        bool hit = false;
        BrushTess::create_triangles([&](Vector2f a, Vector2f b2, Vector2f c) {
            hit = SCollision::collide(checkAgainst, SCollision::Triangle<float>{
                xf.apply(a), xf.apply(b2), xf.apply(c)});
            return hit;
        }, *s.points, s.hasRoundCaps, smoothed, 0, nullptr);
        if(hit)
            return true;
    }
    return false;
}

bool VectorGroupCanvasComponent::should_draw_extra(const DrawData& drawData, const CoordSpaceHelper& coords) const {
    SCollision::AABB<float> viewGenerousColliderInObjSpace = coords.world_collider_to_coords<SCollision::AABB<float>>(drawData.cam.viewingAreaGenerousCollider);
    viewGenerousColliderInObjSpace.min -= Vector2f{1.0f, 1.0f};
    viewGenerousColliderInObjSpace.max += Vector2f{1.0f, 1.0f};
    bool deepPrecheck = false;
    for(auto& aabb : precheckAABBLevels) {
        if(SCollision::collide(viewGenerousColliderInObjSpace, aabb)) {
            deepPrecheck = true;
            break;
        }
    }
    if(!deepPrecheck && !precheckAABBLevels.empty())
        return false;
    return true;
}

SCollision::AABB<float> VectorGroupCanvasComponent::get_obj_coord_bounds() const {
    return bounds;
}
