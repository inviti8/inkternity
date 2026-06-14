// PHASE5 legacy feasibility spike (docs/design/PHASE5.md).
//
// Renders LEGACY TimelineFX effects (the stable .eff / data.xml format from the
// classic editor) through Skia, using the render-agnostic classic C++ runtime
// (deps/timelinefx_legacy, = peterigz/timelinefx). We implement three hooks:
//   * SkiaImage      : AnimImage      -> decode a shape PNG to an SkImage
//   * SkiaLibrary    : EffectsLibrary -> hand back our loader + image factory
//   * SkiaPM         : ParticleManager-> DrawSprite() draws one particle in Skia
//
// The legacy color model is simple per-sprite: each particle carries an rgb tint
// (0-255), an alpha (0-1) and an additive flag. We modulate the (usually white)
// shape by that color — no per-texel ramp needed, unlike the modern runtime.
//
// Build: cmake --build build --config Release --target tfx_legacy_spike
// Run:   tfx_legacy_spike <assetDir> <data.xml> <out.png|out_dir> [effect|__all__|__first__] [frames] [canvas_px]
//        (assetDir = folder holding data.xml + the shape PNGs, i.e. the unzipped .eff)
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "TLFXAnimImage.h"
#include "TLFXEffect.h"
#include "TLFXEffectsLibrary.h"
#include "TLFXParticleManager.h"
#include "TLFXPugiXMLLoader.h"

#include <include/codec/SkCodec.h>
#include <include/codec/SkPngDecoder.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/core/SkSurface.h>
#include <include/encode/SkPngEncoder.h>

static std::string g_assetDir;

namespace {

class SkiaImage : public TLFX::AnimImage {
public:
    sk_sp<SkImage> img;
    bool Load(const char* filename) override {
        std::string path = g_assetDir.empty() ? filename : (g_assetDir + "/" + filename);
        sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
        if (!data) { std::printf("[legacy] cannot open %s\n", path.c_str()); return false; }
        std::unique_ptr<SkCodec> codec = SkPngDecoder::Decode(data, nullptr);
        if (!codec) { std::printf("[legacy] decode fail %s\n", path.c_str()); return false; }
        auto [decoded, res] = codec->getImage();
        img = decoded;
        return img != nullptr;
    }
};

class SkiaLibrary : public TLFX::EffectsLibrary {
public:
    TLFX::XMLLoader* CreateLoader() const override {
        return new TLFX::PugiXMLLoader((int)_shapeList.size());
    }
    TLFX::AnimImage* CreateImage() const override { return new SkiaImage; }
    std::vector<std::string> TopLevelEffectNames() const {
        std::vector<std::string> v;
        for (auto& kv : _effects)
            if (kv.first.find('/') == std::string::npos) v.push_back(kv.first);
        return v;
    }
};

class SkiaPM : public TLFX::ParticleManager {
public:
    SkCanvas* canvas = nullptr;
    int drawn = 0;
    SkiaPM(int particles, int layers) : TLFX::ParticleManager(particles, layers) {}

    void DrawSprite(TLFX::AnimImage* sprite, float px, float py, float /*frame*/,
                    float x, float y, float rotation, float scaleX, float scaleY,
                    unsigned char r, unsigned char g, unsigned char b, float a,
                    bool additive) override {
        auto* si = static_cast<SkiaImage*>(sprite);
        if (!si || !si->img || !canvas) return;

        SkPaint paint;
        // Per-particle tint: modulate the (white/grey) shape by the particle rgb.
        paint.setColorFilter(SkColorFilters::Blend(SkColorSetARGB(255, r, g, b),
                                                   SkBlendMode::kModulate));
        paint.setAlphaf(a < 0.f ? 0.f : (a > 1.f ? 1.f : a));
        paint.setBlendMode(additive ? SkBlendMode::kPlus : SkBlendMode::kSrcOver);

        const float w = (float)si->img->width(), h = (float)si->img->height();
        canvas->save();
        canvas->translate(px, py);
        canvas->rotate(rotation);              // DrawSprite rotation is in degrees
        canvas->scale(scaleX, scaleY);
        // x,y = image handle in pixels -> pivot the quad there.
        canvas->drawImageRect(si->img, SkRect::MakeWH(w, h), SkRect::MakeXYWH(-x, -y, w, h),
                              SkSamplingOptions(SkFilterMode::kLinear), &paint,
                              SkCanvas::kStrict_SrcRectConstraint);
        canvas->restore();
        ++drawn;
    }
};

std::string sanitize(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (!std::isalnum((unsigned char)c)) c = '_';
    return o;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: tfx_legacy_spike <assetDir> <data.xml> <out.png|out_dir> "
                    "[effect|__all__|__first__] [frames] [canvas_px]\n");
        return 2;
    }
    g_assetDir = argv[1];
    const char* xml = argv[2];
    const std::string outArg = argv[3];
    const std::string want = argc > 4 ? argv[4] : "__first__";
    const int frames = argc > 5 ? std::atoi(argv[5]) : 50;
    const int canvas_px = argc > 6 ? std::atoi(argv[6]) : 768;

    SkiaLibrary lib;
    if (!lib.Load(xml)) { std::printf("[legacy] FAILED to load %s\n", xml); return 1; }
    auto names = lib.TopLevelEffectNames();
    std::printf("[legacy] loaded; %zu top-level effects:\n", names.size());
    for (auto& n : names) std::printf("  - %s\n", n.c_str());

    const bool sweep = (want == "__all__");
    std::vector<std::string> targets;
    if (sweep) targets = names;
    else if (want == "__first__") { if (!names.empty()) targets.push_back(names.front()); }
    else targets.push_back(want);

    for (const std::string& name : targets) {
        TLFX::Effect* tmpl = lib.GetEffect(name.c_str());
        if (!tmpl) { std::printf("[legacy] no effect '%s'\n", name.c_str()); continue; }

        // Heap-allocate and intentionally leak (throwaway tool) to dodge the
        // manager/effect ownership question on teardown.
        SkiaPM* pm = new SkiaPM(10000, 1);
        pm->SetScreenSize(canvas_px, canvas_px);
        pm->SetOrigin(canvas_px / 2.0f, canvas_px / 2.0f);

        TLFX::Effect* eff = new TLFX::Effect(*tmpl, pm, true);
        pm->AddEffect(eff);
        for (int f = 0; f < frames; ++f) pm->Update();

        sk_sp<SkSurface> surface =
            SkSurfaces::Raster(SkImageInfo::MakeN32Premul(canvas_px, canvas_px));
        SkCanvas* canvas = surface->getCanvas();
        canvas->clear(SK_ColorBLACK);
        pm->canvas = canvas;
        pm->drawn = 0;
        pm->DrawParticles();

        const std::string out = sweep ? (outArg + "/" + sanitize(name) + ".png") : outArg;
        sk_sp<SkImage> raster = surface->makeImageSnapshot()->makeRasterImage();
        SkPixmap px;
        bool ok = raster && raster->peekPixels(&px);
        if (ok) { SkFILEWStream s(out.c_str()); ok = s.isValid() && SkPngEncoder::Encode(&s, px, {}); }
        std::printf("[legacy] %-24s inUse=%4d drawn=%4d -> %s (%s)\n",
                    name.c_str(), pm->GetParticlesInUse(), pm->drawn, out.c_str(),
                    ok ? "ok" : "FAIL");
    }
    return 0;
}
