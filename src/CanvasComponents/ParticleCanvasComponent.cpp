#include "ParticleCanvasComponent.hpp"

// Legacy TimelineFX (.eff) backed. The runtime (TLFX particle manager + spawned
// effect) lives behind the PIMPL, gated by HVYM_HAS_TIMELINEFX_LEGACY (desktop;
// mirrors HVYM_HAS_LIBMYPAINT). Web / no-legacy builds get a no-op stub that
// still round-trips the serialized data.
#ifdef HVYM_HAS_TIMELINEFX_LEGACY
#include "Particles/LegacyFxLibrary.hpp"
#include "Particles/LegacyFxRenderer.hpp"
#include "TLFXEffect.h"
#include "TLFXEffectsLibrary.h"
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include "../DrawingProgram/DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../DrawCollision.hpp"
#include "CanvasComponentContainer.hpp"
#include "Helpers/ConvertVec.hpp"
#include <Helpers/Logger.hpp>

#include <include/core/SkCanvas.h>

// ---------------------------------------------------------------------------
#ifdef HVYM_HAS_TIMELINEFX_LEGACY

namespace {
// A large symmetric viewport so TimelineFX's screen-space particle cull never
// trims a centered effect. The manager centers the effect at kVpHalf; draw()
// offsets by -kVpHalf so that center lands at the component's local origin.
constexpr float kVp     = 100000.f;
constexpr float kVpHalf = kVp * 0.5f;
} // namespace

struct ParticleCanvasComponent::Runtime {
    LegacyFxLibrary* lib = nullptr;          // non-owning (FxLibraryStore owns it)
    std::unique_ptr<LegacyFxRenderer> pm;    // owns the spawned effect (ClearAll deletes)
    TLFX::Effect* eff = nullptr;             // the spawned instance (owned by pm); for Anim-FX drive
    float accum = 0.f;                        // real-time -> fixed-step accumulator
    bool started = false;                    // an effect instance has been spawned
    int emptyFrames = 0;                      // consecutive frames with 0 particles
    bool lastVisible = false;                 // for the became-visible edge
    bool drawnSinceUpdate = false;            // set by draw() (true on-screen, real per-layer camera);
                                              // read + reset by update() as the visibility signal
    bool loaded = false;                      // lib resolved + pm built + effect exists
};

void ParticleCanvasComponent::ensure_runtime(DrawingProgram* drawP) const {
    if (rt && rt->loaded) return;
    if (!drawP) return;   // draw() can't resolve a library; update()/init build first
    LegacyFxLibrary* lib = drawP->resolve_fx_library(d.libraryResourceId);
    if (!lib) return;
    if (!lib->GetEffect(d.effectName.c_str())) {
        Logger::get().log("WORLDFATAL", "[Particle] effect not in library: " + d.effectName);
        return;
    }
    rt = std::make_unique<Runtime>();
    rt->lib = lib;
    rt->pm = std::make_unique<LegacyFxRenderer>(5000, 1);
    rt->pm->SetScreenSize(static_cast<int>(kVp), static_cast<int>(kVp));
    rt->pm->SetOrigin(0.f, 0.f, d.localScale);   // camtz = localScale -> positions + sizes scale
    rt->loaded = true;
    // No effect spawned yet — playback is triggered in update() (AUTO: on becoming
    // visible; ON_TOUCH: on trigger_touch).
}

void ParticleCanvasComponent::play_effect() const {
    if (!rt || !rt->lib || !rt->pm) return;
    TLFX::Effect* tmpl = rt->lib->GetEffect(d.effectName.c_str());
    if (!tmpl) return;
    rt->pm->ClearAll();   // remove + delete any prior instance -> fresh play
    TLFX::Effect* eff = new TLFX::Effect(*tmpl, rt->pm.get(), true);
    rt->pm->AddEffect(eff);
    rt->eff = eff;        // keep the handle for Anim-FX drive (pm owns it)
    rt->started = true;
    rt->emptyFrames = 0;
}

void ParticleCanvasComponent::drive_anim_fx(const WorldVec& worldDelta) {
    if (!rt || !rt->eff || !compContainer) return;
    // Shared world delta -> this component's LOCAL delta (dir_to_space = the
    // linear part, no translation) -> sim units (local = sim * localScale).
    const Vector2f localDelta = compContainer->coords.dir_to_space(worldDelta);
    const float inv = (d.localScale != 0.0f) ? (1.0f / d.localScale) : 0.0f;
    rt->eff->SetX(localDelta.x() * inv);
    rt->eff->SetY(localDelta.y() * inv);
    // Grow bounds (monotonic) to cover the travel + the particle spread, so the
    // trail isn't cropped. CRUCIAL: the container caches worldAABB (which drives
    // the clip + cull) and only refreshes it on commit — so after growing the
    // collider we must recompute it, or the effective box stays frozen at the
    // painted origin while the effect travels out of it.
    const float need = localDelta.norm() + spread_margin();
    if (need > d.radius * 1.05f) {
        rebuild_collider(need);
        if (compContainer) compContainer->calculate_world_bounds();
    }
}

void ParticleCanvasComponent::update(DrawingProgram& drawP) {
    ensure_runtime(&drawP);
    if (!rt || !rt->loaded || !rt->pm) return;

    // Visibility for the play trigger is the ground truth from draw(): a component
    // is drawn iff it's on-screen under the camera it's actually rendered with
    // (main, parallax-derived, or reader). Using should_draw(main) here instead
    // mistimes the trigger for parallax layers, whose render camera differs.
    // draw() runs after update(), so this reflects the previous frame (1-frame lag).
    const bool visible = rt->drawnSinceUpdate;
    rt->drawnSinceUpdate = false;
    const bool becameVisible = visible && !rt->lastVisible;
    rt->lastVisible = visible;
    const bool done = rt->started && rt->emptyFrames > 15;

    if (pendingTouch) {
        play_effect();          // a touch always (re)plays, regardless of mode
        pendingTouch = false;
    } else if (d.playMode == PARTICLE_PLAY_AUTO && becameVisible && (!rt->started || done)) {
        // Finite effects play on first view and replay on each re-entry; Continuous
        // effects play once on first view and run (never "done"), so never retrigger.
        play_effect();
    }

    if (rt->started) {
        // Advance the fixed-step sim by real elapsed time (decouple from framerate).
        const float step = 1.0f / TLFX::EffectsLibrary::GetUpdateFrequency();
        rt->accum += static_cast<float>(drawP.world.main.deltaTime);
        int steps = 0;
        while (rt->accum >= step && steps < 4) { rt->pm->Update(); rt->accum -= step; ++steps; }
        if (rt->accum > step) rt->accum = 0.f;
        rt->emptyFrames = (rt->pm->GetParticlesInUse() == 0) ? rt->emptyFrames + 1 : 0;
    }

    // Animated like a GIF: invalidate this component's cache region each frame.
    drawP.invalidate_cache_at_component(&(*compContainer->objInfo));
}

void ParticleCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData,
                                   const std::shared_ptr<void>&) const {
    // Reaching draw() means the container's should_draw() passed for the camera
    // this component is actually rendered with -> the ground-truth visibility the
    // play trigger reads next update(). Set before the started early-return so a
    // not-yet-played effect still registers as visible. Skip the screenshot pass
    // (its own camera must not retrigger playback).
    if (rt && !drawData.takingScreenshot) rt->drawnSinceUpdate = true;
    if (!rt || !rt->loaded || !rt->pm || !rt->started) return;
    rt->pm->set_canvas(canvas);
    rt->pm->drawn = 0;
    canvas->save();
    canvas->translate(-kVpHalf, -kVpHalf);       // manager center -> local origin
    rt->pm->DrawParticles();
    canvas->restore();
}

#else // !HVYM_HAS_TIMELINEFX_LEGACY — web/no-legacy stub

struct ParticleCanvasComponent::Runtime {};
void ParticleCanvasComponent::ensure_runtime(DrawingProgram*) const {}
void ParticleCanvasComponent::play_effect() const {}
void ParticleCanvasComponent::update(DrawingProgram&) {}
void ParticleCanvasComponent::draw(SkCanvas*, const DrawData&, const std::shared_ptr<void>&) const {}
void ParticleCanvasComponent::drive_anim_fx(const WorldVec&) {}

#endif // HVYM_HAS_TIMELINEFX_LEGACY
// ---------------------------------------------------------------------------

ParticleCanvasComponent::ParticleCanvasComponent() = default;
ParticleCanvasComponent::~ParticleCanvasComponent() = default;

void ParticleCanvasComponent::trigger_touch() { pendingTouch = true; }

void ParticleCanvasComponent::get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const {
    resourceSet.emplace(d.libraryResourceId);   // keep the embedded .eff from being pruned
}

void ParticleCanvasComponent::remap_resource_ids(const std::unordered_map<NetworkingObjects::NetObjID, NetworkingObjects::NetObjID>& resourceOldToNewMap) {
    auto it = resourceOldToNewMap.find(d.libraryResourceId);
    if (it != resourceOldToNewMap.end()) d.libraryResourceId = it->second;
}

CanvasComponentType ParticleCanvasComponent::get_type() const {
    return CanvasComponentType::PARTICLE;
}

void ParticleCanvasComponent::initialize_draw_data(DrawingProgram& drawP) {
    create_collider();
    ensure_runtime(&drawP);
}

void ParticleCanvasComponent::create_collider() {
    // Bounds drive the draw-cache clip region; particles that spill past them get
    // cropped to a square. Size the half-extent to the placement scale + the
    // artist's boundsScale (particle brush). Modest by default — Anim-FX grows it
    // dynamically to follow the effect, so the default no longer needs to be huge.
    rebuild_collider(spread_margin());
}

float ParticleCanvasComponent::spread_margin() const {
    return std::max(400.0f, d.localScale * 250.0f) * (d.boundsScale > 0.0f ? d.boundsScale : 1.0f);
}

void ParticleCanvasComponent::rebuild_collider(float radius) {
    using namespace SCollision;
    ColliderCollection<float> objs;
    d.radius = radius;
    const float r = d.radius;
    std::array<Vector2f, 4> t = triangle_from_rect_points(Vector2f{-r, -r}, Vector2f{r, r});
    objs.triangle.emplace_back(t[0], t[1], t[2]);
    objs.triangle.emplace_back(t[2], t[3], t[0]);
    collisionTree.clear();
    collisionTree.calculate_bvh_recursive(objs);
}

bool ParticleCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    return collisionTree.is_collide(checkAgainst);
}

SCollision::AABB<float> ParticleCanvasComponent::get_obj_coord_bounds() const {
    return collisionTree.objects.bounds;
}

std::unique_ptr<CanvasComponent> ParticleCanvasComponent::get_data_copy() const {
    auto c = std::make_unique<ParticleCanvasComponent>();
    c->d = d;
    return c;
}

void ParticleCanvasComponent::set_data_from(const CanvasComponent& other) {
    d = static_cast<const ParticleCanvasComponent&>(other).d;
    rt.reset();  // force rebuild from the new data
}

void ParticleCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    a(d.libraryResourceId, d.effectName, d.seed, d.localScale, d.radius, d.playMode, d.boundsScale);
}
void ParticleCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.libraryResourceId, d.effectName, d.seed, d.localScale, d.radius, d.playMode, d.boundsScale);
    rt.reset();
}
void ParticleCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.libraryResourceId, d.effectName, d.seed, d.localScale, d.radius, d.playMode, d.boundsScale);
}
void ParticleCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    if (version < VersionNumber(0, 14, 0)) {
        // INFPNT000014 (modern .tfx) format: read + discard so the cereal stream
        // stays aligned. Modern packages aren't supported by the legacy path, so
        // such a component loads empty (won't render).
        std::string packageBytes, effectName;
        uint32_t seed = 0;
        float localScale = 0.f, radius = 0.f;
        a(packageBytes, effectName, seed, localScale, radius);
        d = Data{};
    } else {
        a(d.libraryResourceId, d.effectName, d.seed, d.localScale, d.radius);
        if (version >= VersionNumber(0, 15, 0))   // playMode added in INFPNT000016
            a(d.playMode);
        else
            d.playMode = PARTICLE_PLAY_AUTO;
        if (version >= VersionNumber(0, 30, 0))   // boundsScale added in INFPNT000031
            a(d.boundsScale);
        else
            d.boundsScale = 1.0f;
    }
    rt.reset();
}
