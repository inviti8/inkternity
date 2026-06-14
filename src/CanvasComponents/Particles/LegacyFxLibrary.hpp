#pragma once

// One imported legacy TimelineFX library (.eff). Owns the in-memory data.xml +
// shape PNG bytes (unzipped from the .eff with miniz) and the parsed
// TLFX::EffectsLibrary. Desktop-only, gated by HVYM_HAS_TIMELINEFX_LEGACY
// (mirrors HVYM_HAS_TIMELINEFX / HVYM_HAS_LIBMYPAINT). PHASE5.1 M0.
//
// Usage:
//   LegacyFxLibrary lib;
//   if (lib.loadFromEff(effBytes, effSize)) { for (auto& n : lib.topLevelEffectNames()) ... }
// then spawn effects via the TLFX::EffectsLibrary API (GetEffect, etc.) and
// render with a TLFX::ParticleManager subclass (see the M3 component).

#ifdef HVYM_HAS_TIMELINEFX_LEGACY

#include <map>
#include <string>
#include <vector>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

#include "TLFXAnimImage.h"
#include "TLFXEffectsLibrary.h"

class LegacyFxLibrary;

// AnimImage backed by an SkImage decoded from the library's in-memory PNG bytes.
class LegacyAnimImage : public TLFX::AnimImage {
public:
    explicit LegacyAnimImage(const LegacyFxLibrary* owner) : _owner(owner) {}
    bool Load(const char* filename) override;   // decode from owner's byte map
    sk_sp<SkImage> image;
private:
    const LegacyFxLibrary* _owner;
};

class LegacyFxLibrary : public TLFX::EffectsLibrary {
public:
    // Unzip a .eff (zip of data.xml + shape PNGs) in memory and parse it.
    bool loadFromEff(const void* effZipBytes, size_t size);

    // Top-level effect names (no '/', i.e. not sub-effects) — for the picker.
    std::vector<std::string> topLevelEffectNames() const;

    // Raw PNG bytes for a shape filename (as referenced in data.xml URLs).
    sk_sp<SkData> shapeBytes(const std::string& filename) const;

protected:
    TLFX::XMLLoader* CreateLoader() const override;   // MemPugiXMLLoader(_dataXml)
    TLFX::AnimImage* CreateImage() const override;    // LegacyAnimImage(this)

private:
    std::string _dataXml;
    std::map<std::string, sk_sp<SkData>> _shapeBytes;   // basename -> PNG bytes
};

#endif // HVYM_HAS_TIMELINEFX_LEGACY
