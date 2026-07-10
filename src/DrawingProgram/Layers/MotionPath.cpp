#include "MotionPath.hpp"
#include "../../Waypoints/Waypoint.hpp"   // TransitionEasing + transition_easing_to_bezier_curve
#include <Helpers/BezierEasing.hpp>
#include <algorithm>
#include <cmath>

namespace {
// Geometric tangent (direction of travel) of the segment seg->seg+1 at local t.
Vector2f segment_tangent(const MotionPath& mp, size_t seg, float t) {
    const Vector2f p0 = mp.points[seg];
    const Vector2f p3 = mp.points[seg + 1];
    const Vector2f cOut = (seg < mp.controlOut.size()) ? mp.controlOut[seg] : Vector2f{0.0f, 0.0f};
    const Vector2f cIn  = ((seg + 1) < mp.controlIn.size()) ? mp.controlIn[seg + 1] : Vector2f{0.0f, 0.0f};
    const bool curved = (cOut.x() != 0.0f || cOut.y() != 0.0f || cIn.x() != 0.0f || cIn.y() != 0.0f);
    if(!curved) return p3 - p0;
    const Vector2f c1 = p0 + cOut;
    const Vector2f c2 = p3 + cIn;
    const float mt = 1.0f - t;
    // Cubic Bezier derivative B'(t).
    return 3.0f * mt * mt * (c1 - p0) + 6.0f * mt * t * (c2 - c1) + 3.0f * t * t * (p3 - c2);
}
float tangent_angle(const Vector2f& v) {
    return (v.squaredNorm() > 1e-12f) ? std::atan2(v.y(), v.x()) : 0.0f;
}
}

float MotionPath::total_seconds() const {
    float t = 0.0f;
    for(float s : nodeSeconds)
        t += (s > 0.0f) ? s : 0.0f;
    return t;
}

MotionPath::Sample MotionPath::sample(double elapsed) const {
    const size_t n = points.size();
    if(n == 0) return { Vector2f{0.0f, 0.0f}, 1.0f };
    if(n == 1) return { points[0], nodeScale.empty() ? 1.0f : nodeScale[0] };

    // Per-segment duration (seconds) of the edge ENDING at node i (i >= 1).
    auto seg_secs = [&](size_t i) -> double {
        const float s = (i < nodeSeconds.size()) ? nodeSeconds[i] : 1.0f;
        return (s > 0.0f) ? static_cast<double>(s) : 0.0f;
    };
    const double total = static_cast<double>(total_seconds());
    double e = std::clamp(elapsed, 0.0, total);

    // Walk segments, accumulating seconds, to find the active one + local t.
    size_t seg = n - 2;
    double segStart = 0.0, segLen = seg_secs(n - 1);
    double cum = 0.0;
    for(size_t i = 0; i + 1 < n; ++i) {
        const double len = seg_secs(i + 1);
        if(e <= cum + len || i + 1 == n - 1) { seg = i; segStart = cum; segLen = len; break; }
        cum += len;
    }
    double u = (segLen > 0.0) ? (e - segStart) / segLen : 0.0;
    u = std::clamp(u, 0.0, 1.0);

    // Ease within the segment (reuse the waypoint easing vocabulary).
    const uint8_t easeByte = (seg < nodeEasing.size()) ? nodeEasing[seg] : static_cast<uint8_t>(1);
    const BezierEasing be(transition_easing_to_bezier_curve(static_cast<TransitionEasing>(easeByte)));
    const float eu = be(static_cast<float>(u));

    // Position: cubic when a tangent is present on the edge, else straight lerp.
    const Vector2f p0 = points[seg];
    const Vector2f p3 = points[seg + 1];
    const Vector2f cOut = (seg < controlOut.size()) ? controlOut[seg] : Vector2f{0.0f, 0.0f};
    const Vector2f cIn  = ((seg + 1) < controlIn.size()) ? controlIn[seg + 1] : Vector2f{0.0f, 0.0f};
    const bool curved = (cOut.x() != 0.0f || cOut.y() != 0.0f || cIn.x() != 0.0f || cIn.y() != 0.0f);
    Vector2f pos;
    if(curved) {
        const Vector2f c1 = p0 + cOut;
        const Vector2f c2 = p3 + cIn;
        const float mt = 1.0f - eu;
        pos = (mt * mt * mt) * p0 + (3.0f * mt * mt * eu) * c1 + (3.0f * mt * eu * eu) * c2 + (eu * eu * eu) * p3;
    }
    else {
        pos = p0 + (p3 - p0) * eu;
    }

    const float s0 = (seg < nodeScale.size()) ? nodeScale[seg] : 1.0f;
    const float s1 = ((seg + 1) < nodeScale.size()) ? nodeScale[seg + 1] : 1.0f;
    const float scale = s0 + (s1 - s0) * eu;

    // Rotation-along-tangent: the tangent's turn from the START heading, scaled by
    // the per-node factor (−1..1, tweened). Deviation-from-start so progress 0 is
    // unrotated (the element keeps its authored orientation). Computed in path-local
    // space, so it's camera-independent; the factor also lets the artist flip the
    // sign if the visual sense is reversed.
    const float r0 = (seg < nodeRotation.size()) ? nodeRotation[seg] : 0.0f;
    const float r1 = ((seg + 1) < nodeRotation.size()) ? nodeRotation[seg + 1] : 0.0f;
    const float rFactor = r0 + (r1 - r0) * eu;
    float rotationDeg = 0.0f;
    if(rFactor != 0.0f) {
        const float angNow  = tangent_angle(segment_tangent(*this, seg, static_cast<float>(u)));
        const float angStart = tangent_angle(segment_tangent(*this, 0, 0.0f));
        rotationDeg = rFactor * (angNow - angStart) * (180.0f / 3.14159265358979323846f);
    }
    return { pos, scale, rotationDeg };
}

void MotionPath::advance(float deltaTime) {
    const double total = static_cast<double>(total_seconds());
    if(total <= 0.0 || points.size() < 2) return;
    const double dt = static_cast<double>(deltaTime);
    switch(playStyle) {
        case FlipbookPlayStyle::ONCE:
            pathProgress = std::min(total, pathProgress + dt);
            break;
        case FlipbookPlayStyle::LOOP:
            pathProgress += dt;
            pathProgress = std::fmod(pathProgress, total);
            if(pathProgress < 0.0) pathProgress += total;
            break;
        case FlipbookPlayStyle::PING_PONG:
            pathProgress += pathReversing ? -dt : dt;
            if(pathProgress > total)    { pathProgress = 2.0 * total - pathProgress; pathReversing = true; }
            else if(pathProgress < 0.0) { pathProgress = -pathProgress;              pathReversing = false; }
            pathProgress = std::clamp(pathProgress, 0.0, total);
            break;
    }
}

void MotionPath::reset() {
    pathProgress = 0.0;
    pathReversing = false;
}
