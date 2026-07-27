#include "BrushStrokeCanvasComponent.hpp"
#include <Helpers/Serializers.hpp>
#include <cereal/types/vector.hpp>
#include <limits>
#include <Eigen/Geometry>
#include "Eigen/Core"
#include "Helpers/MathExtras.hpp"
#include "../TimePoint.hpp"
#include <include/core/SkVertices.h>
#include <memory>
#include "../DrawCollision.hpp"
#include "CanvasComponentContainer.hpp"
#include "Helpers/SCollision.hpp"

void BrushStrokeCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    a(*d.points, d.color, d.hasRoundCaps);
}

void BrushStrokeCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(*d.points, d.color, d.hasRoundCaps);
}

void BrushStrokeCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(*d.points, d.color, d.hasRoundCaps);
}

void BrushStrokeCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    a(*d.points, d.color, d.hasRoundCaps);
}

std::unique_ptr<CanvasComponent> BrushStrokeCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<BrushStrokeCanvasComponent>();
    toRet->d = d;
    return toRet;
}

CanvasComponentType BrushStrokeCanvasComponent::get_type() const {
    return CanvasComponentType::BRUSHSTROKE;
}

void BrushStrokeCanvasComponent::change_stroke_color(const Vector4f& newStrokeColor) {
    d.color = newStrokeColor;
}

std::optional<Vector4f> BrushStrokeCanvasComponent::get_stroke_color() const {
    return d.color;
}

void BrushStrokeCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const {
    SkPaint paint;
    paint.setColor4f(SkColor4f{d.color.x(), d.color.y(), d.color.z(), d.color.w()});
    paint.setAntiAlias(drawData.skiaAA);

    unsigned mipmapLevel = compContainer ? compContainer->get_mipmap_level(drawData) : 0;

    if(mipmapLevel == 0 || drawData.takingScreenshot) // check takingScreenshot, to make sure that screenshots dont use LOD
        canvas->drawPath(*brushPath, paint);
    else
        canvas->drawPath(*brushPathLOD[mipmapLevel - 1], paint);
}

std::shared_ptr<void> BrushStrokeCanvasComponent::get_predraw_data_accurate(const DrawData& drawData, const CoordSpaceHelper& coords) const {
    std::vector<BrushStrokeCanvasComponentPoint> points = BrushTess::smooth_points(*d.points, 0, d.points->size() - 1, BrushTess::DEFAULT_SMOOTHNESS);

    SCollision::AABB<float> viewGenerousColliderInObjSpace = coords.world_collider_to_coords<SCollision::AABB<float>>(drawData.cam.viewingAreaGenerousCollider);
    viewGenerousColliderInObjSpace.min -= Vector2f{1.0f, 1.0f};
    viewGenerousColliderInObjSpace.max += Vector2f{1.0f, 1.0f};

    std::vector<std::array<SkPoint, 3>> finalTrianglePoints;

    auto triangleFunc = [&](Vector2f a, Vector2f b, Vector2f c) {
        SCollision::Triangle<float> triCollider = {a, b, c};

        if(SCollision::collide(triCollider.bounds, viewGenerousColliderInObjSpace)) {
            auto clipListFunc = [](const std::vector<std::array<WorldVec, 3>>& clipList, const std::array<WorldVec, 2>& axisLineSegment, const std::function<bool(const WorldVec&)>& isInClippingAreaFunc) {
                std::vector<std::array<WorldVec, 3>> resultList;
                for(auto& t : clipList)
                    clip_triangle_against_axis(resultList, t, axisLineSegment, isInClippingAreaFunc);
                return resultList;
            };

            std::vector<std::array<WorldVec, 3>> clipList;
            clipList.emplace_back(std::array<WorldVec, 3>{coords.from_space(a), coords.from_space(b), coords.from_space(c)});
            clipList = clipListFunc(clipList, {drawData.cam.viewingAreaGenerousCollider.min, drawData.cam.viewingAreaGenerousCollider.top_right()}, [&](const WorldVec& p) {
                return p.y() > drawData.cam.viewingAreaGenerousCollider.min.y();
            });
            clipList = clipListFunc(clipList, {drawData.cam.viewingAreaGenerousCollider.max, drawData.cam.viewingAreaGenerousCollider.bottom_left()}, [&](const WorldVec& p) {
                return p.y() < drawData.cam.viewingAreaGenerousCollider.max.y();
            });
            clipList = clipListFunc(clipList, {drawData.cam.viewingAreaGenerousCollider.min, drawData.cam.viewingAreaGenerousCollider.bottom_left()}, [&](const WorldVec& p) {
                return p.x() > drawData.cam.viewingAreaGenerousCollider.min.x();
            });
            clipList = clipListFunc(clipList, {drawData.cam.viewingAreaGenerousCollider.max, drawData.cam.viewingAreaGenerousCollider.top_right()}, [&](const WorldVec& p) {
                return p.x() < drawData.cam.viewingAreaGenerousCollider.max.x();
            });
            for(auto& tri : clipList) {
                auto& c = drawData.cam.c;
                finalTrianglePoints.emplace_back(std::array<SkPoint, 3>{convert_vec2<SkPoint>(c.to_space(tri[0])), convert_vec2<SkPoint>(c.to_space(tri[1])), convert_vec2<SkPoint>(c.to_space(tri[2]))});
            }
        }
        return false;
    };

    BrushTess::create_triangles(triangleFunc, *d.points, d.hasRoundCaps, points, 0, nullptr);

    if(finalTrianglePoints.empty())
        return nullptr;
    else if(drawData.isSVGRender) {
        auto pathsToDraw = std::make_shared<std::vector<SkPath>>();
        for(auto& finalTriangle : finalTrianglePoints) {
            SkPathBuilder pathBuilder;
            pathBuilder.addPolygon({finalTriangle.data(), finalTriangle.size()}, true);
            pathsToDraw->emplace_back(pathBuilder.detach());
        }
        return pathsToDraw;
    }
    else {
        std::vector<SkPoint> pointVec(finalTrianglePoints.size() * 3);
        for(size_t i = 0; i < finalTrianglePoints.size(); i++) {
            pointVec[i * 3 + 0] = finalTrianglePoints[i][0];
            pointVec[i * 3 + 1] = finalTrianglePoints[i][1];
            pointVec[i * 3 + 2] = finalTrianglePoints[i][2];
        }
        auto verticesToDraw = std::make_shared<sk_sp<SkVertices>>();
        *verticesToDraw = SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode, pointVec.size(), pointVec.data(), nullptr, nullptr);
        return verticesToDraw;
    }
    return nullptr;
}

bool BrushStrokeCanvasComponent::accurate_draw(SkCanvas* canvas, const DrawData& drawData, const CoordSpaceHelper& coords, const std::shared_ptr<void>& predrawData) const {
    if(predrawData) {
        if(drawData.isSVGRender) { // SVG renders can't use saveLayer, so use this despite it being buggy in some scenarios
            auto pathsToDraw = std::static_pointer_cast<std::vector<SkPath>>(predrawData);
            SkPaint paint;
            paint.setColor4f(SkColor4f{d.color.x(), d.color.y(), d.color.z(), d.color.w()});
            for(const SkPath& p : *pathsToDraw)
                canvas->drawPath(p, paint);
        }
        else if(d.color.w() == 1.0f) {
            auto verticesToDraw = std::static_pointer_cast<sk_sp<SkVertices>>(predrawData);
            SkPaint paint;
            paint.setColor4f(SkColor4f{d.color.x(), d.color.y(), d.color.z(), d.color.w()});
            canvas->drawVertices(*verticesToDraw, SkBlendMode::kSrcOver, paint);
        }
        else {
            canvas->saveLayerAlphaf(nullptr, d.color.w());
            auto verticesToDraw = std::static_pointer_cast<sk_sp<SkVertices>>(predrawData);
            SkPaint paint;
            paint.setColor4f(SkColor4f{d.color.x(), d.color.y(), d.color.z(), 1.0f});
            canvas->drawVertices(*verticesToDraw, SkBlendMode::kSrcOver, paint);
            canvas->restore();
        }
    }

    return true;
}

void BrushStrokeCanvasComponent::set_data_from(const CanvasComponent& other) {
    auto& otherStroke = static_cast<const BrushStrokeCanvasComponent&>(other);
    d = otherStroke.d;
}

void BrushStrokeCanvasComponent::initialize_draw_data(DrawingProgram& drawP) {
    if(!d.points->empty()) {
        std::vector<BrushStrokeCanvasComponentPoint> points = BrushTess::smooth_points(*d.points, 0, d.points->size() - 1, BrushTess::DEFAULT_SMOOTHNESS);
        auto triangleFunc = [&](Vector2f a, Vector2f b, Vector2f c) {
            return false;
        };

        auto brushPathBuilder = std::make_shared<SkPathBuilder>();
        auto brushPathLOD0Builder = std::make_shared<SkPathBuilder>();
        auto brushPathLOD1Builder = std::make_shared<SkPathBuilder>();

        BrushTess::create_triangles(triangleFunc, *d.points, d.hasRoundCaps, points, 0, brushPathBuilder);
        BrushTess::create_triangles(triangleFunc, *d.points, d.hasRoundCaps, points, 4, brushPathLOD0Builder);
        BrushTess::create_triangles(triangleFunc, *d.points, d.hasRoundCaps, points, 30, brushPathLOD1Builder);

        brushPath = std::make_shared<SkPath>(brushPathBuilder->detach());
        brushPathLOD[0] = std::make_shared<SkPath>(brushPathLOD0Builder->detach());
        brushPathLOD[1] = std::make_shared<SkPath>(brushPathLOD1Builder->detach());
    }
    create_collider();
}

void BrushStrokeCanvasComponent::create_collider() {
    using namespace SCollision;
    if(d.points->empty())
        return;
    ColliderCollection<float> strokeObjects;
    std::vector<BrushStrokeCanvasComponentPoint> points = BrushTess::smooth_points(*d.points, 0, d.points->size() - 1, BrushTess::DEFAULT_SMOOTHNESS);
    BrushTess::create_triangles([&](Vector2f a, Vector2f b, Vector2f c) {
        strokeObjects.triangle.emplace_back(a, b, c);
        return false;
    }, *d.points, d.hasRoundCaps, points, 0, nullptr);

    BrushTess::finalize_collider(strokeObjects, bounds, precheckAABBLevels);
}

bool BrushStrokeCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
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

    bool toRet = false;
    std::vector<BrushStrokeCanvasComponentPoint> points = BrushTess::smooth_points(*d.points, 0, d.points->size() - 1, BrushTess::DEFAULT_SMOOTHNESS);
    BrushTess::create_triangles([&](Vector2f a, Vector2f b, Vector2f c) {
        toRet = SCollision::collide(checkAgainst, SCollision::Triangle<float>{a, b, c});
        return toRet;
    }, *d.points, d.hasRoundCaps, points, 0, nullptr);
    return toRet;
}

bool BrushStrokeCanvasComponent::should_draw_extra(const DrawData& drawData, const CoordSpaceHelper& coords) const {
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

SCollision::AABB<float> BrushStrokeCanvasComponent::get_obj_coord_bounds() const {
    return bounds;
}
