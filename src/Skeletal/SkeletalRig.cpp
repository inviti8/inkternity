#include "SkeletalRig.hpp"

#include "SkiaDragonBones.hpp"

#include <include/core/SkData.h>
#include <include/core/SkBitmap.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/codec/SkCodec.h>
#include <include/codec/SkPngDecoder.h>

namespace AI {

struct SkeletalRig::Impl {
    dragonBones::SkiaFactory        factory;
    dragonBones::SkiaArmatureProxy* proxy = nullptr;   // owned by the DragonBones object pool
    dragonBones::DragonBonesData*   data  = nullptr;
    sk_sp<SkImage>                  atlas;
};

SkeletalRig::SkeletalRig() : _impl(std::make_unique<Impl>()) {}

SkeletalRig::~SkeletalRig() = default;

bool SkeletalRig::load(const std::string& skeletonJson,
                       const std::string& atlasJson,
                       const void* atlasPngBytes, std::size_t atlasPngLen) {
    if (!atlasPngBytes || atlasPngLen == 0) return false;

    // Decode the atlas PNG (same idiom as AvatarStore/ArmatureModel).
    auto pngData = SkData::MakeWithCopy(atlasPngBytes, atlasPngLen);
    auto codec = SkCodec::MakeFromData(pngData, {SkPngDecoder::Decoder()});
    if (!codec) return false;
    const auto info = codec->getInfo()
                          .makeColorType(kRGBA_8888_SkColorType)
                          .makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bmp;
    if (!bmp.tryAllocPixels(info)) return false;
    if (codec->getPixels(info, bmp.getPixels(), bmp.rowBytes()) != SkCodec::kSuccess) return false;
    bmp.setImmutable();
    _impl->atlas = bmp.asImage();
    if (!_impl->atlas) return false;

    _impl->data = _impl->factory.loadDragonBonesData(skeletonJson);
    if (!_impl->data || _impl->data->getArmatureNames().empty()) return false;

    _impl->factory.loadTextureAtlasData(atlasJson, _impl->atlas);

    _impl->proxy = _impl->factory.buildArmatureDisplay(_impl->data->getArmatureNames()[0]);
    return _impl->proxy != nullptr;
}

bool SkeletalRig::valid() const { return _impl->proxy != nullptr; }

std::vector<std::string> SkeletalRig::animationNames() const {
    if (!_impl->proxy) return {};
    return _impl->proxy->getAnimation()->getAnimationNames();
}

void SkeletalRig::play(const std::string& name, int playTimes) {
    if (_impl->proxy) _impl->proxy->getAnimation()->play(name, playTimes);
}

void SkeletalRig::update(float dtSeconds) {
    // Advance THIS armature only (per-rig), not a shared global clock.
    if (_impl->proxy && _impl->proxy->getArmature())
        _impl->proxy->getArmature()->advanceTime(dtSeconds);
}

void SkeletalRig::draw(SkCanvas* canvas) const {
    if (_impl->proxy) _impl->proxy->draw(canvas);
}

void SkeletalRig::shutdownRuntime() {
    dragonBones::BaseObject::clearPool();
}

}  // namespace AI
