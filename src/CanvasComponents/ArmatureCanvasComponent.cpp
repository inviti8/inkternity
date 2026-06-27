#include "ArmatureCanvasComponent.hpp"

#include "../DrawCollision.hpp"

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>

#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <cstring>

CanvasComponentType ArmatureCanvasComponent::get_type() const {
    return CanvasComponentType::ARMATURE;
}

// Wire payload (peers share the build → all fields present).
void ArmatureCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    a(d.rigId, d.pose,
      d.camYaw, d.camPitch, d.camDist, d.camTx, d.camTy, d.camTz,
      d.lightAz, d.lightEl, d.lightInt, d.lightAmb, d.lightSky,
      d.rasterDim, d.rasterRGBA, d.height, d.materialColors, d.shapeSliders);
}
void ArmatureCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.rigId, d.pose,
      d.camYaw, d.camPitch, d.camDist, d.camTx, d.camTy, d.camTz,
      d.lightAz, d.lightEl, d.lightInt, d.lightAmb, d.lightSky,
      d.rasterDim, d.rasterRGBA, d.height, d.materialColors, d.shapeSliders);
    cachedImage_ = nullptr;
}

void ArmatureCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.rigId, d.pose,
      d.camYaw, d.camPitch, d.camDist, d.camTx, d.camTy, d.camTz,
      d.lightAz, d.lightEl, d.lightInt, d.lightAmb, d.lightSky,
      d.rasterDim, d.rasterRGBA);
    a(d.height);          // M5.1a (0.22.0+)
    a(d.materialColors);  // M5.1b (0.23.0+)
    a(d.shapeSliders);    // M5.1c (0.24.0+)
    a(d.fovDeg, d.ortho); // M6 camera lens (0.25.0+)
}
void ArmatureCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    // PHASE9 (INFPNT000022 / 0.21.0): the armature type was introduced whole, so
    // a file containing one is always >= 0.21.0 (older builds can't open it).
    if (version >= VersionNumber(0, 21, 0)) {
        a(d.rigId, d.pose,
          d.camYaw, d.camPitch, d.camDist, d.camTx, d.camTy, d.camTz,
          d.lightAz, d.lightEl, d.lightInt, d.lightAmb, d.lightSky,
          d.rasterDim, d.rasterRGBA);
    }
    if (version >= VersionNumber(0, 22, 0))  // M5.1a: uniform bone-length height
        a(d.height);
    if (version >= VersionNumber(0, 23, 0))  // M5.1b: per-material color overrides
        a(d.materialColors);
    if (version >= VersionNumber(0, 24, 0))  // M5.1c: shape-key slider values
        a(d.shapeSliders);
    if (version >= VersionNumber(0, 25, 0))  // M6: camera lens (FOV + ortho)
        a(d.fovDeg, d.ortho);
    cachedImage_ = nullptr;
}

std::unique_ptr<CanvasComponent> ArmatureCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<ArmatureCanvasComponent>();
    toRet->d = d;
    return toRet;
}
void ArmatureCanvasComponent::set_data_from(const CanvasComponent& other) {
    d = static_cast<const ArmatureCanvasComponent&>(other).d;
    cachedImage_ = nullptr;
}

void ArmatureCanvasComponent::set_raster(std::vector<uint8_t> rgba, int dim) {
    d.rasterRGBA = std::move(rgba);
    d.rasterDim = dim;
    cachedImage_ = nullptr;
}

void ArmatureCanvasComponent::ensure_image() const {
    if (cachedImage_) return;
    if (d.rasterDim <= 0 ||
        d.rasterRGBA.size() < static_cast<size_t>(d.rasterDim) * d.rasterDim * 4)
        return;
    SkBitmap bmp;
    if (!bmp.tryAllocPixels(SkImageInfo::Make(
            d.rasterDim, d.rasterDim, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType)))
        return;
    std::memcpy(bmp.getPixels(), d.rasterRGBA.data(),
                static_cast<size_t>(d.rasterDim) * d.rasterDim * 4);  // own the pixels
    bmp.setImmutable();
    cachedImage_ = bmp.asImage();
}

void ArmatureCanvasComponent::draw(SkCanvas* canvas, const DrawData&, const std::shared_ptr<void>&) const {
    ensure_image();
    if (!cachedImage_) return;
    const float s = static_cast<float>(d.rasterDim);
    // Component-local space is the baked-raster's pixels; the container has already
    // applied this component's coords (placement + uniform scale). Square 1:1.
    canvas->drawImageRect(cachedImage_, SkRect::MakeWH(s, s),
                          SkSamplingOptions(SkFilterMode::kLinear));
}

void ArmatureCanvasComponent::initialize_draw_data(DrawingProgram&) {
    cachedImage_ = nullptr;   // rebuilt lazily in draw()
    create_collider();
}

void ArmatureCanvasComponent::create_collider() {
    using namespace SCollision;
    const float s = (d.rasterDim > 0) ? static_cast<float>(d.rasterDim) : 256.0f;
    ColliderCollection<float> objects;
    // Two triangles covering the square [0,0]→[s,s] (hit-test / selection).
    const Vector2f a{0.0f, 0.0f}, b{s, 0.0f}, c{s, s}, e{0.0f, s};
    objects.triangle.emplace_back(a, b, c);
    objects.triangle.emplace_back(c, e, a);
    collisionTree.clear();
    collisionTree.calculate_bvh_recursive(objects);
}

bool ArmatureCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    return collisionTree.is_collide(checkAgainst);
}

SCollision::AABB<float> ArmatureCanvasComponent::get_obj_coord_bounds() const {
    return collisionTree.objects.bounds;
}
