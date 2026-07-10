#include "MotionPath.hpp"
#include "../../Waypoints/Waypoint.hpp"   // TransitionEasing + transition_easing_to_bezier_curve
#include <Helpers/BezierEasing.hpp>
#include <algorithm>
#include <cmath>

MotionPath::Sample MotionPath::sample(double progress) const {
    const size_t n = points.size();
    if(n == 0) return { Vector2f{0.0f, 0.0f}, 1.0f };
    if(n == 1) return { points[0], nodeScale.empty() ? 1.0f : nodeScale[0] };

    const double p = std::clamp(progress, 0.0, 1.0);
    // Normalized arrival time of node i (falls back to uniform if unset).
    auto t_at = [&](size_t i) -> double {
        return (i < nodeTime.size()) ? static_cast<double>(nodeTime[i])
                                     : static_cast<double>(i) / static_cast<double>(n - 1);
    };
    // First segment whose end-time exceeds p (else the last segment).
    size_t seg = n - 2;
    for(size_t i = 0; i + 1 < n; ++i) {
        if(p < t_at(i + 1)) { seg = i; break; }
    }
    const double t0 = t_at(seg), t1 = t_at(seg + 1);
    double u = (t1 > t0) ? (p - t0) / (t1 - t0) : 0.0;
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
    return { pos, scale };
}

void MotionPath::advance(float deltaTime) {
    if(duration <= 0.0f || points.size() < 2) return;
    const double step = static_cast<double>(deltaTime) / static_cast<double>(duration);
    switch(playStyle) {
        case FlipbookPlayStyle::ONCE:
            pathProgress = std::min(1.0, pathProgress + step);
            break;
        case FlipbookPlayStyle::LOOP:
            pathProgress += step;
            pathProgress -= std::floor(pathProgress);   // wrap into [0,1)
            break;
        case FlipbookPlayStyle::PING_PONG:
            pathProgress += pathReversing ? -step : step;
            if(pathProgress > 1.0)      { pathProgress = 2.0 - pathProgress; pathReversing = true; }
            else if(pathProgress < 0.0) { pathProgress = -pathProgress;      pathReversing = false; }
            pathProgress = std::clamp(pathProgress, 0.0, 1.0);
            break;
    }
}

void MotionPath::reset() {
    pathProgress = 0.0;
    pathReversing = false;
}
