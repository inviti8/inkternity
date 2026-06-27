#pragma once
// PHASE9 (docs/design/PHASE9.md) — armature/model → raster bake.
//
// Renders a loaded ArmatureModel at a given view-projection + lighting into a
// square top-down RGBA8 buffer (ready for an SkBitmap / component raster) via a
// raw-GL pass in its own FBO (own colour + depth — the window surface has no
// depth buffer), then reads it back. The CALLER must GrDirectContext::
// resetContext() before the next Skia draw (THE GATE rule from M1).
//
// Used by the armature editor's Bake and by the create/load-model placement
// path. No-op (returns false) on builds without desktop GL 3.3 + Ganesh.

#include <Eigen/Dense>
#include <cstdint>
#include <vector>

namespace Armature { class ArmatureModel; }

namespace ArmatureBake {

// Render `model` at `viewProj` + lighting into a `dim`×`dim` RGBA8 buffer,
// top-down. Returns false (and clears `outTopDown`) on GL failure / non-GL build.
bool render_armature_rgba(Armature::ArmatureModel& model, const Eigen::Matrix4f& viewProj,
                          const Eigen::Vector3f& lightDir, float ambient, float diffuse,
                          float sky, int dim, std::vector<uint8_t>& outTopDown);

}  // namespace ArmatureBake
