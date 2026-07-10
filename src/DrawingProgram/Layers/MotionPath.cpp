#include "MotionPath.hpp"
#include "../../Waypoints/Waypoint.hpp"   // TransitionEasing + transition_easing_to_bezier_curve
#include <Helpers/BezierEasing.hpp>
#include <algorithm>
#include <cmath>

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
    return { pos, scale };
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
