#include "FxLibraryStore.hpp"

#ifdef HVYM_HAS_TIMELINEFX_LEGACY

#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>

#include "LegacyFxLibrary.hpp"
#include "LegacyFxRenderer.hpp"
#include "TLFXEffect.h"
#include "TLFXEffectsLibrary.h"

namespace {
// Headless-render an effect to a square thumbnail. Mirrors the proven spike
// (tools/tfx_legacy_spike.cpp): centre the effect at world origin, run the sim,
// and snapshot the peak-population frame so one-shot bursts (explosions) and
// continuous emitters (snow) both show at their fullest. The spike fits an
// effect in a 768px frame at camtz=1.0; we render smaller with a proportional
// camtz so the framing matches, then let the panel scale the tile down.
//
// The manager + spawned effect are INTENTIONALLY LEAKED (as the spike does):
// the legacy lib's effect/particle teardown is unsound for some complex nested
// effects (e.g. Smokey Explosion — a parent/child particle ownership double-free
// in Effect::Destroy that the upstream author dodged by leaking). Thumbnails are
// generated once per effect and cached, and the manager is created with 0 pooled
// particles + createParticlesAsNeeded, so the leak is just the effect's peak
// particle count (~a few thousand per whole library, ~1 MB) — a bounded, one-time
// cost in exchange for not crashing on arbitrary effects.
sk_sp<SkImage> render_effect_thumbnail(LegacyFxLibrary& lib, const std::string& name) {
    TLFX::Effect* tmpl = lib.GetEffect(name.c_str());
    if (!tmpl) return nullptr;

    constexpr int   PX        = 256;
    constexpr int   FRAMES    = 50;
    constexpr float SPIKE_FIT = 768.0f;   // px the spike fits an effect into at camtz=1

    LegacyFxRenderer* pm = new LegacyFxRenderer(0, 1);   // 0 pooled -> lazy alloc; leaked (see above)
    pm->SetScreenSize(PX, PX);
    pm->SetOrigin(0.f, 0.f, PX / SPIKE_FIT);   // camtz scales positions + sizes to fit
    pm->AddEffect(new TLFX::Effect(*tmpl, pm, true));

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(PX, PX));
    SkCanvas* canvas = surface->getCanvas();
    pm->set_canvas(canvas);

    int bestInUse = -1;
    sk_sp<SkImage> best;
    for (int f = 0; f < FRAMES; ++f) {
        pm->Update();
        const int inUse = pm->GetParticlesInUse();
        if (inUse > bestInUse) {
            canvas->clear(SK_ColorBLACK);   // additive sprites read true on black (editor look)
            pm->drawn = 0;
            pm->DrawParticles();
            bestInUse = inUse;
            best = surface->makeImageSnapshot();   // raster surface is COW-safe
        }
    }
    if (!best) { canvas->clear(SK_ColorBLACK); best = surface->makeImageSnapshot(); }
    return best;   // pm intentionally not deleted (see note above)
}
} // namespace

FxLibraryStore::FxLibraryStore() = default;
FxLibraryStore::~FxLibraryStore() = default;

sk_sp<SkImage> FxLibraryStore::thumbnail(int libraryIndex, const std::string& effectName) {
    if (libraryIndex < 0 || libraryIndex >= static_cast<int>(_entries.size())) return nullptr;
    Entry& e = _entries[libraryIndex];
    auto it = e.thumbs.find(effectName);
    if (it != e.thumbs.end()) return it->second;   // cached (possibly null)
    sk_sp<SkImage> img = e.lib ? render_effect_thumbnail(*e.lib, effectName) : nullptr;
    e.thumbs.emplace(effectName, img);
    return img;
}

int FxLibraryStore::add(const NetworkingObjects::NetObjID& resourceId, const std::string& name,
                        std::unique_ptr<LegacyFxLibrary> lib) {
    for (size_t i = 0; i < _entries.size(); ++i)
        if (_entries[i].resourceId == resourceId) return static_cast<int>(i);
    _entries.push_back(Entry{resourceId, name, std::move(lib)});
    return static_cast<int>(_entries.size()) - 1;
}

void FxLibraryStore::setActive(int libraryIndex, const std::string& effectName) {
    _activeLibrary = libraryIndex;
    _activeEffect = effectName;
}

bool FxLibraryStore::isActive(int libraryIndex, const std::string& effectName) const {
    return _activeLibrary == libraryIndex && _activeEffect == effectName;
}

bool FxLibraryStore::hasActive() const {
    return _activeLibrary >= 0 && _activeLibrary < static_cast<int>(_entries.size())
        && !_activeEffect.empty();
}

#endif // HVYM_HAS_TIMELINEFX_LEGACY
