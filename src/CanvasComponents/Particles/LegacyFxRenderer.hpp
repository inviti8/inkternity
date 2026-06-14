#pragma once

// A TLFX::ParticleManager that draws particles to an SkCanvas using the legacy
// per-sprite color model (modulate the shape by the particle rgb, additive or
// alpha blend, grid sprite-sheet frame sub-rect). Proven in the PHASE5 spike;
// the M3 ParticleCanvasComponent owns one of these per placement and ticks it
// on the canvas clock. PHASE5.1 M3. Gated by HVYM_HAS_TIMELINEFX_LEGACY.

#ifdef HVYM_HAS_TIMELINEFX_LEGACY

#include "TLFXParticleManager.h"

class SkCanvas;

class LegacyFxRenderer : public TLFX::ParticleManager {
public:
    LegacyFxRenderer(int particles, int layers);

    // The canvas DrawParticles() draws into. Set before each DrawParticles().
    void set_canvas(SkCanvas* c) { _canvas = c; }
    int drawn = 0;   // particles drawn in the last DrawParticles()

protected:
    void DrawSprite(TLFX::AnimImage* sprite, float px, float py, float frame,
                    float x, float y, float rotation, float scaleX, float scaleY,
                    unsigned char r, unsigned char g, unsigned char b, float a,
                    bool additive) override;

private:
    SkCanvas* _canvas = nullptr;
};

#endif // HVYM_HAS_TIMELINEFX_LEGACY
