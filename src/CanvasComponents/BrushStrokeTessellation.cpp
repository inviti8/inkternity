#include "BrushStrokeTessellation.hpp"

#include <Eigen/Geometry>
#include "Eigen/Core"
#include "Helpers/MathExtras.hpp"
#include "Helpers/ConvertVec.hpp"
#include "../TimePoint.hpp"   // lerp_vec
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <utility>

namespace BrushTess {

std::vector<size_t> get_wedge_indices(const std::vector<BrushStrokeCanvasComponentPoint>& points) {
    if(points.size() < 2)
        return {};
    std::vector<size_t> toRet;
    toRet.emplace_back(0);
    for(size_t i = 1; i < points.size() - 1; i++) {
        Vector2f v1Norm = (points[i - 1].pos - points[i].pos).normalized();
        Vector2f v2Norm = (points[i + 1].pos - points[i].pos).normalized();
        if(v1Norm.dot(v2Norm) > -0.6f)
            toRet.emplace_back(i);
    }
    toRet.emplace_back(points.size() - 1);
    return toRet;
}

std::vector<BrushStrokeCanvasComponentPoint> smooth_points(const std::vector<BrushStrokeCanvasComponentPoint>& points, size_t beginIndex, size_t endIndex, unsigned numOfDivisions) {
    size_t pointsSize = endIndex - beginIndex + 1;
    if(pointsSize < 2)
        return {points[beginIndex], points[endIndex]};

    std::vector<BrushStrokeCanvasComponentPoint> toRet;

    BrushStrokeCanvasComponentPoint p0 = points[beginIndex];
    p0.pos = p0.pos + p0.width * (p0.pos - points[beginIndex + 1].pos).normalized() * DRAW_MINIMUM_LIMIT;
    BrushStrokeCanvasComponentPoint p1 = points[beginIndex];
    BrushStrokeCanvasComponentPoint p2 = points[beginIndex + 1];
    BrushStrokeCanvasComponentPoint p3;

    for(size_t i = beginIndex; i < endIndex; i++) {

        if(i == endIndex - 1) {
            p3 = points[i + 1];
            p3.pos = p3.pos + p3.width * (p3.pos - points[i].pos).normalized() * DRAW_MINIMUM_LIMIT;
        }
        else
            p3 = points[i + 2];

        toRet.emplace_back(p1);

        float tMove = 1.0 / static_cast<float>(numOfDivisions);
        for(unsigned d = 1; d < numOfDivisions; d++) {
            float t = tMove * static_cast<float>(d);
            BrushStrokeCanvasComponentPoint pToAdd;
            pToAdd.pos = catmull_rom(p0.pos, p1.pos, p2.pos, p3.pos, t);
            pToAdd.width = std::lerp(p1.width, p2.width, t);
            if(!(std::isnan(pToAdd.pos.x()) || std::isnan(pToAdd.pos.y()))) // Don't add NAN values
                toRet.emplace_back(pToAdd);
        }
        p0 = p1;
        p1 = p2;
        p2 = p3;
    }
    toRet.emplace_back(points[endIndex]);

    return toRet;
}

std::vector<BrushStrokeCanvasComponentPoint> every_nth_point_include_front_and_back(const std::vector<BrushStrokeCanvasComponentPoint>& pts, size_t n) {
    if(pts.size() <= 3 || n == 0)
        return pts;
    std::vector<BrushStrokeCanvasComponentPoint> toRet;
    toRet.emplace_back(pts.front());
    for(size_t i = n; i < pts.size() - 1; i += n)
        toRet.emplace_back(pts[i]);
    toRet.emplace_back(pts.back());
    return toRet;
}

void create_triangles(const std::function<bool(Vector2f, Vector2f, Vector2f)>& passTriangleFunc, const std::vector<BrushStrokeCanvasComponentPoint>& rawPoints, bool hasRoundCaps, const std::vector<BrushStrokeCanvasComponentPoint>& smoothedPoints, size_t skipVertexCount, std::shared_ptr<SkPathBuilder> bPathBuilder) {
    const int ARC_SMOOTHNESS = 10;
    const int CIRCLE_SMOOTHNESS = 20;
    const std::vector<BrushStrokeCanvasComponentPoint>& pointsN = rawPoints;
    std::vector<SkPoint> topPoints;
    std::vector<SkPoint> bottomPoints;

    if(pointsN.size() == 1 && hasRoundCaps) {
        std::vector<Vector2f> circlePoints = gen_circle_points(pointsN.front().pos, pointsN.front().width / 2.0f, CIRCLE_SMOOTHNESS);
        for(size_t i = 0; i < circlePoints.size(); i++) {
            if(passTriangleFunc(pointsN.front().pos, circlePoints[i], circlePoints[(i + 1) % circlePoints.size()])) return;
            topPoints.emplace_back(convert_vec2<SkPoint>(circlePoints[i]));
        }
    }

    if(pointsN.size() < 2) {
        if(bPathBuilder && !topPoints.empty())
            bPathBuilder->addPolygon({topPoints.data(), topPoints.size()}, false);
        return;
    }

    auto points = every_nth_point_include_front_and_back(smoothedPoints, skipVertexCount);

    std::vector<size_t> wedgeIndices = get_wedge_indices(points);
    for(size_t i = 0; i < wedgeIndices.size() - 1; i++) {

        size_t pointsBegin = wedgeIndices[i];
        size_t pointsEnd = wedgeIndices[i + 1];

        std::array<Vector2f, 3> vertArray;
        size_t newIndex = 0;

        Vector2f perpPrev = perpendicular_vec2((points[pointsBegin + 1].pos - points[pointsBegin].pos).eval());
        perpPrev.normalize();

        vertArray[0] = points[pointsBegin].pos + perpPrev * points[pointsBegin].width * 0.5f;
        topPoints.emplace_back(convert_vec2<SkPoint>(vertArray[0]));
        vertArray[1] = points[pointsBegin].pos - perpPrev * points[pointsBegin].width * 0.5f;
        bottomPoints.emplace_back(convert_vec2<SkPoint>(vertArray[1]));
        newIndex = 2;

        if(hasRoundCaps && pointsBegin == 0) {
            Vector2f arcDirStart = (convert_vec2<Vector2f>(vertArray[0]) - points[0].pos).normalized();
            Vector2f arcDirEnd = (convert_vec2<Vector2f>(vertArray[1]) - points[0].pos).normalized();
            Vector2f arcDirCenter = (points[0].pos - points[1].pos).normalized();
            std::vector<Vector2f> arcPoints = arc_vec<float>(points[0].pos, arcDirStart, arcDirEnd, arcDirCenter, points[0].width * 0.5f, ARC_SMOOTHNESS);
            arcPoints.emplace(arcPoints.begin(), convert_vec2<Vector2f>(vertArray[0]));
            arcPoints.emplace_back(convert_vec2<Vector2f>(vertArray[1]));
            for(size_t i = 0; i < arcPoints.size() - 1; i++) {
                if(passTriangleFunc(points[0].pos, arcPoints[i], arcPoints[i + 1])) return;
                bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
            }
            bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
        }

        for(size_t j = pointsBegin + 1; j < pointsEnd; j++) {
            if(j >= pointsEnd)
                j = pointsEnd - 1;

            Vector2f v1 = points[j - 1].pos - points[j].pos;
            Vector2f v2 = points[j].pos - points[j + 1].pos;
            Vector2f perp = perpendicular_vec2((v1 + v2).eval()).normalized();
            if(std::isnan(perp.dot(perpPrev))) {
                std::cout << "NAN FOUND" << std::endl;
                std::cout << v1 << std::endl;
                std::cout << v2 << std::endl;
                std::cout << perp << std::endl;
                std::cout << perpPrev << std::endl;
                throw 1;
            }
            if(perp.dot(perpPrev) < 0.0)
                perp = -perp;

            vertArray[newIndex] = points[j].pos + perp * 0.5f * points[j].width;
            topPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
            newIndex = (newIndex + 1) % 3;
            if(passTriangleFunc(vertArray[0], vertArray[1], vertArray[2])) return;

            vertArray[newIndex] = points[j].pos - perp * 0.5f * points[j].width;
            bottomPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
            newIndex = (newIndex + 1) % 3;
            if(passTriangleFunc(vertArray[0], vertArray[1], vertArray[2])) return;

            perpPrev = perp;
        }

        Vector2f rectDir = points[pointsEnd - 1].pos - points[pointsEnd].pos;
        rectDir.normalize();
        Vector2f perp = perpendicular_vec2(rectDir);
        if(perp.dot(perpPrev) < 0.0)
            perp = -perp;
        vertArray[newIndex] = points[pointsEnd].pos + perp * points[pointsEnd].width * 0.5;
        topPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
        newIndex = (newIndex + 1) % 3;
        if(passTriangleFunc(vertArray[0], vertArray[1], vertArray[2])) return;

        vertArray[newIndex] = points[pointsEnd].pos - perp * points[pointsEnd].width * 0.5;
        bottomPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
        newIndex = (newIndex + 1) % 3;
        if(passTriangleFunc(vertArray[0], vertArray[1], vertArray[2])) return;

        // Render wedge
        if(pointsEnd != points.size() - 1) {
            // Find 4 points involved in intersection
            Vector2f currentP1 = points[pointsEnd].pos - perp * points[pointsEnd].width * 0.5;
            Vector2f currentP2 = points[pointsEnd].pos + perp * points[pointsEnd].width * 0.5;
            Vector2f nextRectDir = points[pointsEnd + 1].pos - points[pointsEnd].pos;
            nextRectDir.normalize();
            Vector2f nextPerp = perpendicular_vec2(nextRectDir);
            Vector2f nextP1 = points[pointsEnd].pos - nextPerp * points[pointsEnd].width * 0.5;
            Vector2f nextP2 = points[pointsEnd].pos + nextPerp * points[pointsEnd].width * 0.5;

            // Find center of wedge
            Vector2f wedgeCenter = line_line_intersection(currentP1, currentP2, nextP1, nextP2);

            Vector2f wedgeP1 = ((currentP1 - currentP2).dot(nextRectDir)) <= 0.0 ? currentP1 : currentP2;
            Vector2f wedgeP2 = ((nextP1 - nextP2).dot(rectDir)) <= 0.0 ? nextP1 : nextP2;
            bool isTopWedge = wedgeP1 == convert_vec2<Vector2f>(topPoints.back());

            float arcRadius;

            std::vector<Vector2f> arcPoints;

            if(wedgeCenter.x() == std::numeric_limits<float>::max()) {
                wedgeCenter = (currentP1 + currentP2) * 0.5;
                wedgeP1 = currentP1;
                wedgeP2 = currentP2;
                arcRadius = points[pointsEnd].width * 0.5;
                arcPoints = arc_vec(wedgeCenter, (wedgeP1 - wedgeCenter).normalized(), (wedgeP2 - wedgeCenter).normalized(), (-rectDir).eval(), arcRadius, ARC_SMOOTHNESS);
            }
            else {
                arcRadius = (wedgeCenter - wedgeP1).norm();
                arcPoints = arc_vec(wedgeCenter, (wedgeP1 - wedgeCenter).normalized(), (wedgeP2 - wedgeCenter).normalized(), lerp_vec((-rectDir).eval(), (-nextRectDir).eval(), 0.5).normalized(), arcRadius, ARC_SMOOTHNESS);
            }

            arcPoints.emplace(arcPoints.begin(), wedgeP1);
            arcPoints.emplace_back(wedgeP2);
            for(size_t i = 0; i < arcPoints.size() - 1; i++) {
                if(passTriangleFunc(wedgeCenter, arcPoints[i], arcPoints[i + 1])) return;
                if(isTopWedge)
                    topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
                else
                    bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
            }
            if(isTopWedge)
                topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
            else
                bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
        }
        else if(hasRoundCaps) { // It's the last point, cap it off
            Vector2f arcDirStart = (convert_vec2<Vector2f>(vertArray[(newIndex + 1) % 3]) - points.back().pos).normalized();
            Vector2f arcDirEnd = (convert_vec2<Vector2f>(vertArray[(newIndex + 2) % 3]) - points.back().pos).normalized();
            Vector2f arcDirCenter = -rectDir;
            std::vector<Vector2f> arcPoints = arc_vec<float>(points.back().pos, arcDirStart, arcDirEnd, arcDirCenter, points.back().width * 0.5f, ARC_SMOOTHNESS);
            arcPoints.emplace(arcPoints.begin(), convert_vec2<Vector2f>(vertArray[(newIndex + 1) % 3]));
            arcPoints.emplace_back(convert_vec2<Vector2f>(vertArray[(newIndex + 2) % 3]));
            for(size_t i = 0; i < arcPoints.size() - 1; i++) {
                if(passTriangleFunc(points.back().pos, arcPoints[i], arcPoints[i + 1])) return;
                topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
            }
            topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
        }
    }

    if(bPathBuilder && !topPoints.empty()) {
        topPoints.insert(topPoints.begin(), bottomPoints.rbegin(), bottomPoints.rend());
            bPathBuilder->addPolygon({topPoints.data(), topPoints.size()}, false);
    }
}

std::vector<BrushStrokeCanvasComponentPoint> simplify_points(const std::vector<BrushStrokeCanvasComponentPoint>& pts, float epsilon) {
    const size_t n = pts.size();
    if(n <= 2 || epsilon <= 0.0f)
        return pts;

    std::vector<char> keep(n, 0);
    keep[0] = keep[n - 1] = 1;

    // Iterative (stack-based) RDP — strokes can hold thousands of points, so
    // avoid deep recursion.
    std::vector<std::pair<size_t, size_t>> stack;
    stack.emplace_back(0, n - 1);
    while(!stack.empty()) {
        const auto [i, j] = stack.back();
        stack.pop_back();
        if(j <= i + 1)
            continue;
        const Vector2f a = pts[i].pos;
        const Vector2f b = pts[j].pos;
        const Vector2f ab = b - a;
        const float L = ab.norm();
        float maxD = 0.0f;
        size_t idx = i;
        for(size_t k = i + 1; k < j; k++) {
            float d;
            if(L < 1e-12f)
                d = (pts[k].pos - a).norm();
            else {
                const float cross = ab.x() * (pts[k].pos.y() - a.y()) - ab.y() * (pts[k].pos.x() - a.x());
                d = std::fabs(cross) / L;
            }
            if(d > maxD) {
                maxD = d;
                idx = k;
            }
        }
        if(maxD > epsilon) {
            keep[idx] = 1;
            stack.emplace_back(i, idx);
            stack.emplace_back(idx, j);
        }
    }

    std::vector<BrushStrokeCanvasComponentPoint> out;
    out.reserve(n);
    for(size_t k = 0; k < n; k++)
        if(keep[k])
            out.push_back(pts[k]);
    return out;
}

void build_precheck_levels_from_bvh(std::vector<SCollision::AABB<float>>& out, size_t level, const std::vector<SCollision::BVHContainer<float>>& levelArray) {
    for(const auto& a : levelArray) {
        if(level == 0 || a.children.empty())
            out.emplace_back(a.objects.bounds);
        else
            build_precheck_levels_from_bvh(out, level - 1, a.children);
    }
}

void finalize_collider(SCollision::ColliderCollection<float>& strokeObjects, SCollision::AABB<float>& outBounds, std::vector<SCollision::AABB<float>>& outPrecheckLevels) {
    strokeObjects.recalculate_bounds();

    SCollision::BVHContainer<float> collisionTree;
    collisionTree.calculate_bvh_recursive(strokeObjects, AABB_PRECHECK_BVH_LEVELS + 1);
    outPrecheckLevels.clear();
    build_precheck_levels_from_bvh(outPrecheckLevels, AABB_PRECHECK_BVH_LEVELS, collisionTree.children);

    outBounds = collisionTree.objects.bounds;
}

}  // namespace BrushTess
