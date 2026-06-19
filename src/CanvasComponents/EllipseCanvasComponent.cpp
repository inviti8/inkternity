#include "EllipseCanvasComponent.hpp"
#include "CanvasComponent.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/SCollision.hpp"
#include <include/core/SkPathBuilder.h>
#include <include/core/SkMatrix.h>
#include "../DrawCollision.hpp"

CanvasComponentType EllipseCanvasComponent::get_type() const {
    return CanvasComponentType::ELLIPSE;
}

void EllipseCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    // Wire payload — same-build peers, so all appended fields are always present.
    a(d.strokeColor, d.fillColor, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.affineMode, d.center, d.tipA, d.tipB, d.isMask, d.maskInvert);
}

void EllipseCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.strokeColor, d.fillColor, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.affineMode, d.center, d.tipA, d.tipB, d.isMask, d.maskInvert);
}

void EllipseCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.strokeColor, d.fillColor, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode, d.affineMode, d.center, d.tipA, d.tipB, d.isMask, d.maskInvert);
}

void EllipseCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    a(d.strokeColor, d.fillColor, d.strokeWidth, d.p1, d.p2, d.fillStrokeMode);
    // PHASE6 (INFPNT000018 / 0.17.0): affine fields appended. Pre-0.17 files have
    // none — they keep affineMode=false (default) and load as the legacy bbox
    // ellipse unchanged.
    if(version >= VersionNumber(0, 17, 0))
        a(d.affineMode, d.center, d.tipA, d.tipB);
    // PHASE7 (INFPNT000019 / 0.18.0): mask flags appended.
    if(version >= VersionNumber(0, 18, 0))
        a(d.isMask, d.maskInvert);
}

std::optional<SkPath> EllipseCanvasComponent::get_mask_path() const {
    if(!d.isMask) return std::nullopt;
    return ellipsePath;   // built in create_draw_data (component-local space)
}

void EllipseCanvasComponent::change_stroke_color(const Vector4f& newStrokeColor) {
    d.strokeColor = newStrokeColor;
}

std::optional<Vector4f> EllipseCanvasComponent::get_stroke_color() const {
    return d.strokeColor;
}

void EllipseCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const {
    SkPaint p;
    p.setAntiAlias(drawData.skiaAA);
    if(d.fillStrokeMode == 0 || d.fillStrokeMode == 2) {
        p.setStyle(SkPaint::kFill_Style);
        p.setColor4f(convert_vec4<SkColor4f>(d.fillColor));
        canvas->drawPath(ellipsePath, p);
    }
    if(d.fillStrokeMode == 1 || d.fillStrokeMode == 2) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setColor4f(convert_vec4<SkColor4f>(d.strokeColor));
        p.setStrokeWidth(d.strokeWidth);
        canvas->drawPath(ellipsePath, p);
    }
}

void EllipseCanvasComponent::create_draw_data() {
    if(d.affineMode) {
        // PHASE6: oval inscribed in the parallelogram spanned by the two semi-axes.
        // Build a unit oval and affine-transform the PATH (not the canvas) so the
        // stroke keeps a uniform width while the ellipse shears.
        const Vector2f axisA{d.tipA.x() - d.center.x(), d.tipA.y() - d.center.y()};
        const Vector2f axisB{d.tipB.x() - d.center.x(), d.tipB.y() - d.center.y()};
        SkPathBuilder unit;
        unit.addOval(SkRect::MakeLTRB(-1.0f, -1.0f, 1.0f, 1.0f));
        SkMatrix m;
        m.setAll(axisA.x(), axisB.x(), d.center.x(),
                 axisA.y(), axisB.y(), d.center.y(),
                 0.0f, 0.0f, 1.0f);
        ellipsePath = unit.detach().makeTransform(m);
        return;
    }
    SkPathBuilder ellipsePathBuilder;
    SkRect newRect = SkRect::MakeLTRB(d.p1.x(), d.p1.y(), d.p2.x(), d.p2.y());
    ellipsePathBuilder.addOval(newRect);
    ellipsePath = ellipsePathBuilder.detach();
}

void EllipseCanvasComponent::initialize_draw_data(DrawingProgram& drawP) {
    create_draw_data();
    create_collider();
}

std::unique_ptr<CanvasComponent> EllipseCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<EllipseCanvasComponent>();
    toRet->d = d;
    return toRet;
}

void EllipseCanvasComponent::set_data_from(const CanvasComponent& other) {
    auto& otherEllipse = static_cast<const EllipseCanvasComponent&>(other);
    d = otherEllipse.d;
}

bool EllipseCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    return collisionTree.is_collide(checkAgainst);
}

void EllipseCanvasComponent::create_collider() {
    using namespace SCollision;
    ColliderCollection<float> strokeObjects;

    if(d.affineMode) {
        // PHASE6: sample the sheared ellipse (center + cos t * axisA + sin t * axisB).
        const Vector2f axisA{d.tipA.x() - d.center.x(), d.tipA.y() - d.center.y()};
        const Vector2f axisB{d.tipB.x() - d.center.x(), d.tipB.y() - d.center.y()};
        auto sample = [&](float t) {
            return Vector2f{d.center.x() + std::cos(t) * axisA.x() + std::sin(t) * axisB.x(),
                            d.center.y() + std::cos(t) * axisA.y() + std::sin(t) * axisB.y()};
        };
        if(d.fillStrokeMode == 1) {
            std::vector<Vector2f> points;
            const unsigned numOfSegments = 40;
            for(unsigned i = 0; i < numOfSegments; i++)
                points.emplace_back(sample(static_cast<float>(i) / numOfSegments * std::numbers::pi * 2.0));
            generate_polyline(strokeObjects, points, d.strokeWidth, true);
        }
        else {
            const unsigned numOfSegments = 24;
            Vector2f prevPoint = sample(0.0f);
            for(unsigned i = 1; i <= numOfSegments; i++) {
                Vector2f nextPoint = sample(static_cast<float>(i) / numOfSegments * std::numbers::pi * 2.0);
                strokeObjects.triangle.emplace_back(d.center, prevPoint, nextPoint);
                prevPoint = nextPoint;
            }
        }
        collisionTree.clear();
        collisionTree.calculate_bvh_recursive(strokeObjects);
        return;
    }

    if(d.fillStrokeMode == 0) {
        Vector2f ellipseCenter = (d.p1 + d.p2) * 0.5;
        float a = ellipseCenter.x() - d.p1.x();
        float b = ellipseCenter.y() - d.p1.y();
        Vector2f prevPoint = ellipseCenter + Vector2f{a * std::cos(0), b * std::sin(0)};
        unsigned numOfSegments = 20;
        float segmentStep = 1.0 / numOfSegments;
        float t = 0.0;
        for(unsigned i = 0; i <= numOfSegments; i++) {
            Vector2f nextPoint = ellipseCenter + Vector2f{a * std::cos(t), b * std::sin(t)};
            strokeObjects.triangle.emplace_back(ellipseCenter, prevPoint, nextPoint);
            prevPoint = nextPoint;
            t += segmentStep * std::numbers::pi * 2.0;
        }
    }
    else if(d.fillStrokeMode == 1) {
        std::vector<Vector2f> points;

        Vector2f ellipseCenter = (d.p1 + d.p2) * 0.5;
        float a = ellipseCenter.x() - d.p1.x();
        float b = ellipseCenter.y() - d.p1.y();
        unsigned numOfSegments = 40;
        float segmentStep = 1.0 / numOfSegments;
        float t = 0.0;
        for(unsigned i = 0; i < numOfSegments; i++) {
            points.emplace_back(ellipseCenter + Vector2f{a * std::cos(t), b * std::sin(t)});
            t += segmentStep * std::numbers::pi * 2.0;
        }
        generate_polyline(strokeObjects, points, d.strokeWidth, true);

    }
    else if(d.fillStrokeMode == 2) {
        float strokeRadius = d.strokeWidth * 0.5f;
        Vector2f wNewP1 = d.p1 - Vector2f{strokeRadius, strokeRadius};
        Vector2f wNewP2 = d.p2 + Vector2f{strokeRadius, strokeRadius};
        Vector2f ellipseCenter = (wNewP1 + wNewP2) * 0.5;
        float a = ellipseCenter.x() - wNewP1.x();
        float b = ellipseCenter.y() - wNewP1.y();
        Vector2f prevPoint = ellipseCenter + Vector2f{a * std::cos(0), b * std::sin(0)};
        unsigned numOfSegments = 20;
        float segmentStep = 1.0 / numOfSegments;
        float t = 0.0;
        for(unsigned i = 0; i <= numOfSegments; i++) {
            Vector2f nextPoint = ellipseCenter + Vector2f{a * std::cos(t), b * std::sin(t)};
            strokeObjects.triangle.emplace_back(ellipseCenter, prevPoint, nextPoint);
            prevPoint = nextPoint;
            t += segmentStep * std::numbers::pi * 2.0;
        }
    }

    collisionTree.clear();
    collisionTree.calculate_bvh_recursive(strokeObjects);
}

SCollision::AABB<float> EllipseCanvasComponent::get_obj_coord_bounds() const {
    return collisionTree.objects.bounds;
}
