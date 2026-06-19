#include "RectangleCanvasComponent.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include "Helpers/SCollision.hpp"
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <cereal/types/vector.hpp>
#include <algorithm>
#include <array>
#include "../SharedTypes.hpp"
#include "../DrawCollision.hpp"

namespace {
// PHASE6 M2: ear-clipping triangulation for the polygon fill collider. Handles
// concave polygons (the M1 fan was only correct for the convex quad). O(n^2),
// fine for the small vertex counts shapes have. Assumes a simple (non-self-
// intersecting) polygon; on a degenerate/self-intersecting input it bails early
// with a partial fan, which only softens hit-testing — it never crashes.
float poly_signed_area(const std::vector<Vector2f>& p) {
    float area = 0.0f;
    const size_t n = p.size();
    for(size_t i = 0; i < n; ++i) {
        const Vector2f& c = p[i];
        const Vector2f& d = p[(i + 1) % n];
        area += c.x() * d.y() - d.x() * c.y();
    }
    return area * 0.5f;
}
float tri_cross(const Vector2f& a, const Vector2f& b, const Vector2f& c) {
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}
bool point_in_tri(const Vector2f& p, const Vector2f& a, const Vector2f& b, const Vector2f& c) {
    const float d1 = tri_cross(a, b, p);
    const float d2 = tri_cross(b, c, p);
    const float d3 = tri_cross(c, a, p);
    const bool hasNeg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    const bool hasPos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    return !(hasNeg && hasPos);
}
std::vector<std::array<Vector2f, 3>> triangulate_polygon(std::vector<Vector2f> poly) {
    std::vector<std::array<Vector2f, 3>> tris;
    if(poly.size() < 3) return tris;
    if(poly_signed_area(poly) < 0.0f) std::reverse(poly.begin(), poly.end());   // make CCW
    std::vector<size_t> idx(poly.size());
    for(size_t i = 0; i < poly.size(); ++i) idx[i] = i;
    size_t guard = 0;
    const size_t maxGuard = poly.size() * poly.size() + 10;
    while(idx.size() > 3 && guard++ < maxGuard) {
        bool clipped = false;
        const size_t n = idx.size();
        for(size_t i = 0; i < n; ++i) {
            const size_t pa = idx[(i + n - 1) % n], pb = idx[i], pc = idx[(i + 1) % n];
            const Vector2f& a = poly[pa];
            const Vector2f& b = poly[pb];
            const Vector2f& c = poly[pc];
            if(tri_cross(a, b, c) <= 0.0f) continue;   // reflex vertex (CCW) — not an ear
            bool anyInside = false;
            for(size_t j = 0; j < n && !anyInside; ++j) {
                const size_t vj = idx[j];
                if(vj == pa || vj == pb || vj == pc) continue;
                if(point_in_tri(poly[vj], a, b, c)) anyInside = true;
            }
            if(anyInside) continue;
            tris.push_back({a, b, c});
            idx.erase(idx.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if(!clipped) break;   // degenerate — stop with what we have
    }
    if(idx.size() == 3)
        tris.push_back({poly[idx[0]], poly[idx[1]], poly[idx[2]]});
    return tris;
}
}

CanvasComponentType RectangleCanvasComponent::get_type() const {
    return CanvasComponentType::RECTANGLE;
}

void RectangleCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    // Wire payload — peers run the same build, so all appended fields are present.
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.polygonMode, d.points, d.isMask, d.maskInvert, d.controlIn, d.controlOut, d.nodeType);
}

void RectangleCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.polygonMode, d.points, d.isMask, d.maskInvert, d.controlIn, d.controlOut, d.nodeType);
}

void RectangleCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.polygonMode, d.points, d.isMask, d.maskInvert, d.controlIn, d.controlOut, d.nodeType);
}

void RectangleCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode);
    // PHASE6 (INFPNT000017 / 0.16.0): polygon fields appended. Pre-0.16 files
    // have none — they keep polygonMode=false (default) and load unchanged.
    if(version >= VersionNumber(0, 16, 0))
        a(d.polygonMode, d.points);
    // PHASE7 (INFPNT000019 / 0.18.0): mask flags appended.
    if(version >= VersionNumber(0, 18, 0))
        a(d.isMask, d.maskInvert);
    // PHASE8 (INFPNT000020 / 0.19.0): per-node bezier tangents appended. Pre-0.19
    // polygons have none -> empty arrays -> all-corner (straight) rendering.
    if(version >= VersionNumber(0, 19, 0))
        a(d.controlIn, d.controlOut, d.nodeType);
}

std::optional<SkPath> RectangleCanvasComponent::get_mask_path() const {
    if(!d.isMask) return std::nullopt;
    return rectPath;   // built in create_draw_data (component-local space)
}

void RectangleCanvasComponent::change_stroke_color(const Vector4f& newStrokeColor) {
    d.strokeColor = newStrokeColor;
}

std::optional<Vector4f> RectangleCanvasComponent::get_stroke_color() const {
    return d.strokeColor;
}

std::unique_ptr<CanvasComponent> RectangleCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<RectangleCanvasComponent>();
    toRet->d = d;
    return toRet;
}

void RectangleCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const {
    SkPaint p;
    p.setAntiAlias(drawData.skiaAA);
    if(d.fillStrokeMode == 0 || d.fillStrokeMode == 2) {
        p.setStyle(SkPaint::kFill_Style);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setColor4f(convert_vec4<SkColor4f>(d.fillColor));
        canvas->drawPath(rectPath, p);
    }
    if(d.fillStrokeMode == 1 || d.fillStrokeMode == 2) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setColor4f(convert_vec4<SkColor4f>(d.strokeColor));
        p.setStrokeWidth(d.strokeWidth);
        canvas->drawPath(rectPath, p);
    }
}

void RectangleCanvasComponent::set_data_from(const CanvasComponent& other) {
    auto& otherRect = static_cast<const RectangleCanvasComponent&>(other);
    d = otherRect.d;
}

void RectangleCanvasComponent::initialize_draw_data(DrawingProgram& drawP) {
    create_draw_data();
    create_collider();
}

bool RectangleCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    return collisionTree.is_collide(checkAgainst);
}

void RectangleCanvasComponent::create_draw_data() {
    SkPathBuilder rectPathBuilder;
    if(d.polygonMode && d.points.size() >= 2) {
        // PHASE6/8: closed polygon through the vertex list. PHASE8: an edge is a
        // cubic when either endpoint has a tangent toward it, else a straight line.
        const size_t n = d.points.size();
        auto ctrlOut = [&](size_t i) { return i < d.controlOut.size() ? d.controlOut[i] : Vector2f{0.0f, 0.0f}; };
        auto ctrlIn  = [&](size_t i) { return i < d.controlIn.size()  ? d.controlIn[i]  : Vector2f{0.0f, 0.0f}; };
        auto isZero  = [](const Vector2f& v) { return v.x() == 0.0f && v.y() == 0.0f; };
        rectPathBuilder.moveTo(convert_vec2<SkPoint>(d.points[0]));
        for(size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const Vector2f outC = ctrlOut(i);
            const Vector2f inC  = ctrlIn(j);
            if(!isZero(outC) || !isZero(inC)) {
                const Vector2f c1{d.points[i].x() + outC.x(), d.points[i].y() + outC.y()};
                const Vector2f c2{d.points[j].x() + inC.x(),  d.points[j].y() + inC.y()};
                rectPathBuilder.cubicTo(convert_vec2<SkPoint>(c1), convert_vec2<SkPoint>(c2), convert_vec2<SkPoint>(d.points[j]));
            }
            else {
                rectPathBuilder.lineTo(convert_vec2<SkPoint>(d.points[j]));
            }
        }
        rectPathBuilder.close();
        rectPath = rectPathBuilder.detach();
        return;
    }
    if(d.p1.x() == d.p2.x() || d.p1.y() == d.p2.y()) {
        rectPathBuilder.moveTo(convert_vec2<SkPoint>(d.p1));
        rectPathBuilder.lineTo(convert_vec2<SkPoint>(d.p2));
        rectPath = rectPathBuilder.detach();
    }
    else {
        SkRRect newRect = SkRRect::MakeRectXY(SkRect::MakeLTRB(d.p1.x(), d.p1.y(), d.p2.x(), d.p2.y()), d.cornerRadius, d.cornerRadius);
        rectPathBuilder.addRRect(newRect);
        rectPath = rectPathBuilder.detach();
    }
}

std::vector<Vector2f> RectangleCanvasComponent::flatten_polygon_outline() const {
    std::vector<Vector2f> outline;
    const size_t n = d.points.size();
    if(n < 2) return outline;
    auto ctrlOut = [&](size_t i) { return i < d.controlOut.size() ? d.controlOut[i] : Vector2f{0.0f, 0.0f}; };
    auto ctrlIn  = [&](size_t i) { return i < d.controlIn.size()  ? d.controlIn[i]  : Vector2f{0.0f, 0.0f}; };
    auto isZero  = [](const Vector2f& v) { return v.x() == 0.0f && v.y() == 0.0f; };
    constexpr int SEGS = 12;
    for(size_t i = 0; i < n; ++i) {
        outline.push_back(d.points[i]);   // the vertex
        const size_t j = (i + 1) % n;
        const Vector2f outC = ctrlOut(i);
        const Vector2f inC  = ctrlIn(j);
        if(isZero(outC) && isZero(inC))
            continue;                     // straight edge — next vertex closes it
        const Vector2f& p0 = d.points[i];
        const Vector2f& p3 = d.points[j];
        const Vector2f c1{p0.x() + outC.x(), p0.y() + outC.y()};
        const Vector2f c2{p3.x() + inC.x(),  p3.y() + inC.y()};
        for(int s = 1; s < SEGS; ++s) {   // interior samples only (endpoints are the vertices)
            const float t = static_cast<float>(s) / SEGS;
            const float u = 1.0f - t;
            const float b0 = u * u * u, b1 = 3.0f * u * u * t, b2 = 3.0f * u * t * t, b3 = t * t * t;
            outline.push_back(Vector2f{b0 * p0.x() + b1 * c1.x() + b2 * c2.x() + b3 * p3.x(),
                                       b0 * p0.y() + b1 * c1.y() + b2 * c2.y() + b3 * p3.y()});
        }
    }
    return outline;
}

void RectangleCanvasComponent::create_collider() {
    using namespace SCollision;
    ColliderCollection<float> strokeObjects;
    if(d.polygonMode && d.points.size() >= 3) {
        // PHASE6: outline -> closed polyline; fill (and fill+outline) ->
        // ear-clipping triangulation (concave-correct). PHASE8: curved edges are
        // flattened to a polyline first so the collider follows the curves.
        const std::vector<Vector2f> outline = flatten_polygon_outline();
        if(outline.size() >= 3) {
            if(d.fillStrokeMode == 1) {
                generate_polyline(strokeObjects, outline, d.strokeWidth, true);
            }
            else {
                for(const auto& t : triangulate_polygon(outline))
                    strokeObjects.triangle.emplace_back(t[0], t[1], t[2]);
            }
            collisionTree.clear();
            collisionTree.calculate_bvh_recursive(strokeObjects);
            return;
        }
    }
    if(d.fillStrokeMode == 0) {
        std::array<Vector2f, 4> newT = triangle_from_rect_points(d.p1, d.p2);
        strokeObjects.triangle.emplace_back(newT[0], newT[1], newT[2]);
        strokeObjects.triangle.emplace_back(newT[2], newT[3], newT[0]);
    }
    else if(d.fillStrokeMode == 1) {
        if(d.p1.x() == d.p2.x() || d.p1.y() == d.p2.y()) {
            std::vector<Vector2f> points = {d.p1, d.p2};
            generate_polyline(strokeObjects, points, d.strokeWidth, true);
        }
        else {
            std::array<Vector2f, 4> pointArr = triangle_from_rect_points(d.p1, d.p2);
            std::vector<Vector2f> points(pointArr.begin(), pointArr.end());
            generate_polyline(strokeObjects, points, d.strokeWidth, true);
        }
    }
    else if(d.fillStrokeMode == 2) {
        float strokeRadius = d.strokeWidth * 0.5f;
        std::array<Vector2f, 4> newT = triangle_from_rect_points((d.p1 - Vector2f{strokeRadius, strokeRadius}).eval(), (d.p2 + Vector2f{strokeRadius, strokeRadius}).eval());
        strokeObjects.triangle.emplace_back(newT[0], newT[1], newT[2]);
        strokeObjects.triangle.emplace_back(newT[2], newT[3], newT[0]);
    }

    collisionTree.clear();
    collisionTree.calculate_bvh_recursive(strokeObjects);
}

SCollision::AABB<float> RectangleCanvasComponent::get_obj_coord_bounds() const {
    return collisionTree.objects.bounds;
}
