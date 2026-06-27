#pragma once
// PHASE9 (docs/design/PHASE9.md) — M3. Reusable 3D view state for the armature
// editor: an orbit camera and a simple directional+ambient light, plus the two
// matrix helpers Eigen doesn't ship. Header-only (small, inline). Used by the
// live modal (M3) and the bake path; FK posing (M4) and the full modal (M5)
// build on the same structs.

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace Armature {

// Column-major (Eigen default == what GL wants), right-handed, GL clip space.
inline Eigen::Matrix4f perspective(float fovYRad, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovYRad * 0.5f);
    Eigen::Matrix4f m = Eigen::Matrix4f::Zero();
    m(0, 0) = f / aspect;
    m(1, 1) = f;
    m(2, 2) = (zfar + znear) / (znear - zfar);
    m(2, 3) = (2.0f * zfar * znear) / (znear - zfar);
    m(3, 2) = -1.0f;
    return m;
}

// Symmetric orthographic projection (GL clip space). halfH is the half-height of
// the view box in world units at the target; halfW = halfH*aspect.
inline Eigen::Matrix4f orthographic(float halfH, float aspect, float znear, float zfar) {
    const float halfW = halfH * aspect;
    Eigen::Matrix4f m = Eigen::Matrix4f::Zero();
    m(0, 0) = 1.0f / halfW;
    m(1, 1) = 1.0f / halfH;
    m(2, 2) = -2.0f / (zfar - znear);
    m(2, 3) = -(zfar + znear) / (zfar - znear);
    m(3, 3) = 1.0f;
    return m;
}

inline Eigen::Matrix4f look_at(const Eigen::Vector3f& eye, const Eigen::Vector3f& center,
                               const Eigen::Vector3f& up) {
    const Eigen::Vector3f f = (center - eye).normalized();
    const Eigen::Vector3f s = f.cross(up).normalized();
    const Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m(0, 0) = s.x(); m(0, 1) = s.y(); m(0, 2) = s.z();
    m(1, 0) = u.x(); m(1, 1) = u.y(); m(1, 2) = u.z();
    m(2, 0) = -f.x(); m(2, 1) = -f.y(); m(2, 2) = -f.z();
    m(0, 3) = -s.dot(eye); m(1, 3) = -u.dot(eye); m(2, 3) = f.dot(eye);
    return m;
}

// Orbit camera: looks at `target` from a yaw/pitch/distance. yaw=pitch=0 looks
// from +X toward −X (the figure's front — ARMATURE-SCHEMA §6 D1).
struct OrbitCamera {
    Eigen::Vector3f target = Eigen::Vector3f::Zero();
    float yaw = 0.0f;       // radians, around +Y
    float pitch = 0.0f;     // radians, up/down
    float distance = 3.0f;
    float fovY = 40.0f * 3.14159265f / 180.0f;
    bool ortho = false;     // orthographic projection (no perspective distortion)

    // Frame an AABB: centre the target and back off to fit the largest extent.
    void frame_bounds(const Eigen::Vector3f& mn, const Eigen::Vector3f& mx) {
        target = 0.5f * (mn + mx);
        const Eigen::Vector3f ext = mx - mn;
        const float maxExt = std::max(std::max(ext.x(), ext.y()), ext.z());
        distance = (maxExt * 0.5f) / std::tan(fovY * 0.5f) * 1.3f + 1e-3f;
        yaw = 0.0f;
        pitch = 0.0f;
        mFitExtent = maxExt;
    }

    Eigen::Vector3f eye() const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const Eigen::Vector3f dir(cp * cy, sp, cp * sy);  // yaw=pitch=0 → +X
        return target + dir * distance;
    }

    Eigen::Matrix4f view() const { return look_at(eye(), target, Eigen::Vector3f(0, 1, 0)); }

    Eigen::Matrix4f view_proj(float aspect) const {
        const float r = std::max(mFitExtent, distance);
        const float znear = std::max(0.01f, distance - r);
        const float zfar = distance + r * 2.0f;
        // Ortho half-height tracks distance*tan(fov/2) so toggling persp<->ortho and
        // dollying keep the figure roughly the same on-screen size; pan/world_per_pixel
        // (which use the same expression) stay consistent in both modes.
        if (ortho)
            return orthographic(std::max(1e-3f, distance * std::tan(fovY * 0.5f)),
                                aspect, znear, zfar) * view();
        return perspective(fovY, aspect, znear, zfar) * view();
    }

    void orbit(float dYaw, float dPitch) {
        yaw += dYaw;
        pitch = std::clamp(pitch + dPitch, -1.50f, 1.50f);  // avoid gimbal at poles
    }
    void dolly(float factor) { distance = std::clamp(distance * factor, 0.02f, 1000.0f); }

    // Pan the target in the camera's screen plane (worldPerPixel scales drag px).
    void pan(float dxPixels, float dyPixels, float worldPerPixel) {
        const Eigen::Matrix4f v = view();
        const Eigen::Vector3f right(v(0, 0), v(0, 1), v(0, 2));
        const Eigen::Vector3f up(v(1, 0), v(1, 1), v(1, 2));
        target += (-dxPixels * worldPerPixel) * right + (dyPixels * worldPerPixel) * up;
    }

    // World units spanned by one screen pixel at the target depth (square view).
    float world_per_pixel(int viewportPx) const {
        return (2.0f * distance * std::tan(fovY * 0.5f)) / std::max(1, viewportPx);
    }

private:
    float mFitExtent = 1.0f;
};

// Directional key light + ambient + a soft upward sky fill. The light is given
// by where it comes FROM (azimuth around +Y, elevation above horizon).
struct Lighting {
    float azimuth = 0.3f;     // radians
    float elevation = 0.6f;   // radians above horizon
    float intensity = 0.95f;  // diffuse gain
    float ambient = 0.50f;
    float sky = 0.18f;

    // Direction the light TRAVELS (i.e. uLightDir in the shader).
    Eigen::Vector3f travel_dir() const {
        const float ce = std::cos(elevation), se = std::sin(elevation);
        const float ca = std::cos(azimuth), sa = std::sin(azimuth);
        const Eigen::Vector3f from(ce * ca, se, ce * sa);  // toward the source
        return (-from).normalized();
    }
};

}  // namespace Armature
