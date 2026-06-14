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
// Run:   tfx_legacy_spike <file.eff> <out.png|out_dir> [effect|__all__|__first__] [frames] [canvas_px]
//        The .eff is read from disk and unzipped IN MEMORY (miniz) — data.xml +
//        shape PNGs never touch the filesystem. This is the M0 import path.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "miniz.h"

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

// The .eff unzipped into memory: data.xml text + shape filename -> PNG bytes.
static std::string g_dataXml;
static std::map<std::string, sk_sp<SkData>> g_shapeBytes;

namespace {

class SkiaImage : public TLFX::AnimImage {
public:
    sk_sp<SkImage> img;
    bool Load(const char* filename) override {
        auto it = g_shapeBytes.find(filename);
        if (it == g_shapeBytes.end()) {
            std::printf("[legacy] shape not in package: %s\n", filename);
            return false;
        }
        std::unique_ptr<SkCodec> codec = SkPngDecoder::Decode(it->second, nullptr);
        if (!codec) { std::printf("[legacy] decode fail %s\n", filename); return false; }
        auto [decoded, res] = codec->getImage();
        img = decoded;
        return img != nullptr;
    }
};

// PugiXMLLoader reads data.xml from a file (load_file); feed it from the
// in-memory buffer instead (load_buffer), replicating Open()'s cursor setup.
class MemPugiXMLLoader : public TLFX::PugiXMLLoader {
public:
    explicit MemPugiXMLLoader(int shapes) : TLFX::PugiXMLLoader(shapes) {}
    bool Open(const char* /*filename*/) override {
        _error[0] = 0;
        pugi::xml_parse_result result = _doc.load_buffer(g_dataXml.data(), g_dataXml.size());
        if (!result) {
            std::snprintf(_error, sizeof(_error), "parse error: %s", result.description());
            return false;
        }
        if (!_doc.child("EFFECTS")) {
            std::snprintf(_error, sizeof(_error), "root <EFFECTS> missing");
            return false;
        }
        _currentShape = _doc.child("EFFECTS").child("SHAPES").child("IMAGE");
        _currentFolder = _doc.child("EFFECTS").child("FOLDER");
        while (!_currentEffect && _currentFolder) {
            _currentEffect = _currentFolder.child("EFFECT");
            if (!_currentEffect) _currentFolder = _currentFolder.next_sibling("FOLDER");
        }
        if (!_currentEffect) _currentEffect = _doc.child("EFFECTS").child("EFFECT");
        return true;
    }
};

class SkiaLibrary : public TLFX::EffectsLibrary {
public:
    TLFX::XMLLoader* CreateLoader() const override {
        return new MemPugiXMLLoader((int)_shapeList.size());
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

    void DrawSprite(TLFX::AnimImage* sprite, float px, float py, float frame,
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

        // Animated shapes are grid sprite-sheets: GetWidth/Height are the FRAME
        // size (from the <IMAGE> WIDTH/HEIGHT attrs); si->img is the whole sheet.
        // Frames are laid out row-major, columns = sheetWidth / frameWidth.
        float fw = sprite->GetWidth(), fh = sprite->GetHeight();
        const float sheetW = (float)si->img->width(), sheetH = (float)si->img->height();
        if (fw <= 0.f || fh <= 0.f) { fw = sheetW; fh = sheetH; }      // single-frame fallback
        const int cols  = std::max(1, (int)(sheetW / fw + 0.5f));
        const int count = std::max(1, sprite->GetFramesCount());
        int fi = (int)frame;
        fi = ((fi % count) + count) % count;                          // wrap (handles <0)
        const float srcX = (fi % cols) * fw, srcY = (fi / cols) * fh;

        canvas->save();
        canvas->translate(px, py);
        canvas->rotate(rotation);              // DrawSprite rotation is in degrees
        canvas->scale(scaleX, scaleY);
        // x,y = image handle in frame pixels -> pivot the quad there.
        canvas->drawImageRect(si->img, SkRect::MakeXYWH(srcX, srcY, fw, fh),
                              SkRect::MakeXYWH(-x, -y, fw, fh),
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
    if (argc < 3) {
        std::printf("usage: tfx_legacy_spike <file.eff> <out.png|out_dir> "
                    "[effect|__all__|__first__] [frames] [canvas_px]\n");
        return 2;
    }
    const char* effPath = argv[1];
    const std::string outArg = argv[2];
    const std::string want = argc > 3 ? argv[3] : "__first__";
    const int frames = argc > 4 ? std::atoi(argv[4]) : 50;
    const int canvas_px = argc > 5 ? std::atoi(argv[5]) : 768;

    // Read the .eff and unzip it in memory (miniz) into g_dataXml + g_shapeBytes.
    sk_sp<SkData> eff = SkData::MakeFromFileName(effPath);
    if (!eff) { std::printf("[legacy] cannot open %s\n", effPath); return 1; }
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, eff->data(), eff->size(), 0)) {
        std::printf("[legacy] not a valid .eff/zip: %s\n", effPath);
        return 1;
    }
    mz_uint nfiles = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < nfiles; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        size_t outSize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
        if (!p) continue;
        std::string name = st.m_filename;
        std::string base = name.substr(name.find_last_of("/\\") + 1);
        if (base == "data.xml") {
            g_dataXml.assign(static_cast<const char*>(p), outSize);
        } else {
            std::string ext;
            size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) ext = base.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".png") g_shapeBytes[base] = SkData::MakeWithCopy(p, outSize);
        }
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    if (g_dataXml.empty()) { std::printf("[legacy] no data.xml in %s\n", effPath); return 1; }
    std::printf("[legacy] %s -> %zu shapes, data.xml %zu bytes (in memory)\n",
                effPath, g_shapeBytes.size(), g_dataXml.size());

    SkiaLibrary lib;
    if (!lib.Load("<memory>")) { std::printf("[legacy] FAILED to parse data.xml\n"); return 1; }
    auto names = lib.TopLevelEffectNames();
    std::printf("[legacy] loaded; %zu top-level effects:\n", names.size());
    for (auto& n : names) std::printf("  - %s\n", n.c_str());

    const bool sweep = (want == "__all__");
    {
        std::error_code ec;
        std::filesystem::create_directories(
            sweep ? std::filesystem::path(outArg)
                  : std::filesystem::path(outArg).parent_path(), ec);
    }
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
        // _camtx = -originX, so origin (0,0) maps world (0,0) -> screen center
        // (centerX/Y). The effect sits at world 0,0, so it renders centered.
        pm->SetOrigin(0.0f, 0.0f, 1.0f);

        TLFX::Effect* eff = new TLFX::Effect(*tmpl, pm, true);
        pm->AddEffect(eff);

        sk_sp<SkSurface> surface =
            SkSurfaces::Raster(SkImageInfo::MakeN32Premul(canvas_px, canvas_px));
        SkCanvas* canvas = surface->getCanvas();
        pm->canvas = canvas;

        // Capture the peak-population frame: one-shot bursts (explosions, muzzle
        // flashes) die within a few frames, while continuous emitters ramp up and
        // plateau. Snapshotting the frame with the most live particles catches
        // both at their fullest instead of a fixed frame that misses one-shots.
        int bestInUse = -1, bestFrame = 0, bestDrawn = 0;
        sk_sp<SkImage> best;
        for (int f = 0; f < frames; ++f) {
            pm->Update();
            int inUse = pm->GetParticlesInUse();
            if (inUse > bestInUse) {
                canvas->clear(SK_ColorBLACK);
                pm->drawn = 0;
                pm->DrawParticles();
                bestInUse = inUse;
                bestFrame = f;
                bestDrawn = pm->drawn;
                best = surface->makeImageSnapshot();   // raster surface is COW-safe
            }
        }
        if (!best) { canvas->clear(SK_ColorBLACK); best = surface->makeImageSnapshot(); }

        const std::string out = sweep ? (outArg + "/" + sanitize(name) + ".png") : outArg;
        sk_sp<SkImage> raster = best->makeRasterImage();
        SkPixmap px;
        bool ok = raster && raster->peekPixels(&px);
        if (ok) { SkFILEWStream s(out.c_str()); ok = s.isValid() && SkPngEncoder::Encode(&s, px, {}); }
        std::printf("[legacy] %-24s peakFrame=%2d inUse=%4d drawn=%4d -> %s (%s)\n",
                    name.c_str(), bestFrame, bestInUse, bestDrawn, out.c_str(),
                    ok ? "ok" : "FAIL");
    }
    return 0;
}
