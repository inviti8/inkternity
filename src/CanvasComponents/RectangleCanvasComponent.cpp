#include "RectangleCanvasComponent.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include "Helpers/SCollision.hpp"
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <cereal/types/vector.hpp>
#include "../SharedTypes.hpp"
#include "../DrawCollision.hpp"

CanvasComponentType RectangleCanvasComponent::get_type() const {
    return CanvasComponentType::RECTANGLE;
}

void RectangleCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    // Wire payload — peers run the same build, so the PHASE6 fields are always present.
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.polygonMode, d.points);
}

void RectangleCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.polygonMode, d.points);
}

void RectangleCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.polygonMode, d.points);
}

void RectangleCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    a(d.strokeColor, d.fillColor, d.cornerRadius, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode);
    // PHASE6 (INFPNT000017 / 0.16.0): polygon fields appended. Pre-0.16 files
    // have none — they keep polygonMode=false (default) and load unchanged.
    if(version >= VersionNumber(0, 16, 0))
        a(d.polygonMode, d.points);
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
        // PHASE6: closed polygon through the vertex list.
        rectPathBuilder.moveTo(convert_vec2<SkPoint>(d.points[0]));
        for(size_t i = 1; i < d.points.size(); ++i)
            rectPathBuilder.lineTo(convert_vec2<SkPoint>(d.points[i]));
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

void RectangleCanvasComponent::create_collider() {
    using namespace SCollision;
    ColliderCollection<float> strokeObjects;
    if(d.polygonMode && d.points.size() >= 3) {
        // PHASE6: outline -> closed polyline; fill (and fill+outline) -> fan
        // triangulation. Fan is correct for the convex quad M1 produces;
        // concave polygons (from M2/M3 vertex editing) need ear-clipping —
        // tracked for M2.
        if(d.fillStrokeMode == 1) {
            generate_polyline(strokeObjects, d.points, d.strokeWidth, true);
        }
        else {
            for(size_t i = 1; i + 1 < d.points.size(); ++i)
                strokeObjects.triangle.emplace_back(d.points[0], d.points[i], d.points[i + 1]);
        }
        collisionTree.clear();
        collisionTree.calculate_bvh_recursive(strokeObjects);
        return;
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
