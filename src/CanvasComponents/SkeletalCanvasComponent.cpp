#include "SkeletalCanvasComponent.hpp"

#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include "Helpers/SCollision.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../ResourceManager.hpp"
#include "../DrawCollision.hpp"
#include "CanvasComponentContainer.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>

#ifdef HVYM_HAS_DRAGONBONES
#include "../Skeletal/SkeletalRig.hpp"
#endif

SkeletalCanvasComponent::SkeletalCanvasComponent() = default;
SkeletalCanvasComponent::~SkeletalCanvasComponent() = default;

CanvasComponentType SkeletalCanvasComponent::get_type() const {
    return CanvasComponentType::SKELETAL;
}

// save/load are the in-memory (net-sync + undo) codec: always in lock-step with the
// running binary, so the new playMode field is written unconditionally.
void SkeletalCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent, d.playMode);
}
void SkeletalCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent, d.playMode);
}
void SkeletalCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent, d.playMode);
}
void SkeletalCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent);
    // playMode added in INFPNT000034 (0.33.0); older rigs default to AUTO.
    if (version >= VersionNumber(0, 33, 0))
        a(d.playMode);
    else
        d.playMode = SKELETAL_PLAY_AUTO;
}

std::unique_ptr<CanvasComponent> SkeletalCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<SkeletalCanvasComponent>();
    toRet->d = d;   // live rig is rebuilt lazily from the resource ids; never copied
    return toRet;
}

void SkeletalCanvasComponent::set_data_from(const CanvasComponent& other) {
    d = static_cast<const SkeletalCanvasComponent&>(other).d;
#ifdef HVYM_HAS_DRAGONBONES
    rig.reset();            // force a rebuild against the new resource ids
    loadAttempted = false;
#endif
}

void SkeletalCanvasComponent::get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const {
    resourceSet.emplace(d.skeletonResId);
    resourceSet.emplace(d.atlasJsonResId);
    resourceSet.emplace(d.atlasPngResId);
}

void SkeletalCanvasComponent::remap_resource_ids(const std::unordered_map<NetworkingObjects::NetObjID, NetworkingObjects::NetObjID>& map) {
    auto remap = [&](NetworkingObjects::NetObjID& id) {
        auto it = map.find(id);
        if (it != map.end()) id = it->second;
    };
    remap(d.skeletonResId);
    remap(d.atlasJsonResId);
    remap(d.atlasPngResId);
}

#ifdef HVYM_HAS_DRAGONBONES
void SkeletalCanvasComponent::ensure_rig(ResourceManager& rMan) const {
    if (rig) return;   // built once (valid or not); resource retrieval retries below until then
    std::unordered_set<NetworkingObjects::NetObjID> ids{ d.skeletonResId, d.atlasJsonResId, d.atlasPngResId };
    auto m = rMan.copy_resource_set_to_map(ids);
    auto itSke = m.find(d.skeletonResId);
    auto itAj  = m.find(d.atlasJsonResId);
    auto itPng = m.find(d.atlasPngResId);
    if (itSke == m.end() || itAj == m.end() || itPng == m.end()) return;      // not all retrieved yet
    if (!itSke->second.data || !itAj->second.data || !itPng->second.data) return;

    auto built = std::make_unique<AI::SkeletalRig>();
    const auto& png = *itPng->second.data;
    if (built->load(*itSke->second.data, *itAj->second.data, png.data(), png.size())) {
        const auto anims = built->animationNames();
        activeClip = d.clip;
        if (activeClip.empty() && !anims.empty()) activeClip = anims.front();
        // AUTO: loop the clip immediately. ON_TOUCH: leave at the setup pose until a
        // reader-mode tap calls trigger_touch() (which plays it once).
        if (!activeClip.empty() && d.playMode == SKELETAL_PLAY_AUTO)
            built->play(activeClip, 0);
    }
    rig = std::move(built);   // even if load failed: non-null invalid rig → placeholder, no rebuild spin
    loadAttempted = true;
}
#endif

void SkeletalCanvasComponent::update(DrawingProgram& drawP) {
#ifdef HVYM_HAS_DRAGONBONES
    ensure_rig(*drawP.world.drawData.rMan);
    if (!rig || !rig->valid()) return;

    // A reader-mode tap always (re)plays the clip once, regardless of mode.
    if (pendingTouch) {
        if (!activeClip.empty()) rig->play(activeClip, 1);
        pendingTouch = false;
    }

    // Should the rig advance (and redraw) this frame?
    //  AUTO:     while the master enable is on (the clip loops).
    //  ON_TOUCH: only while a triggered one-shot is still running — at rest (setup
    //            pose or holding the last frame) it neither ticks nor invalidates,
    //            so a resting rig costs nothing.
    const bool advancing = (d.playMode == SKELETAL_PLAY_AUTO) ? d.playing
                                                              : rig->isPlaying();

    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (lastTick.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(now - lastTick).count();
        dt = std::clamp(dt, 0.0f, 0.1f);
    }
    lastTick = now;   // keep dt bounded across resting frames so playback resumes smoothly

    if (advancing) {
        rig->update(dt);
        // Animated: force a redraw of this component's cache region each frame.
        drawP.invalidate_cache_at_component(&(*compContainer->objInfo));
    }
    // Size the clip/collider box to the real drawn pose. Recheck while advancing (a
    // jump extends the box) and until the first bounds are known (initial box for a
    // resting ON_TOUCH rig).
    if (advancing || !rigBoundsKnown)
        refresh_rig_bounds();
#endif
}

void SkeletalCanvasComponent::trigger_touch() {
#ifdef HVYM_HAS_DRAGONBONES
    pendingTouch = true;
#endif
}

void SkeletalCanvasComponent::apply_play_mode() {
#ifdef HVYM_HAS_DRAGONBONES
    if (!rig || !rig->valid()) return;
    if (d.playMode == SKELETAL_PLAY_AUTO) {
        if (!activeClip.empty()) rig->play(activeClip, 0);   // resume the loop now
    } else {
        rig->stop();                                         // rest at the current pose until touched
    }
#endif
}

std::vector<std::string> SkeletalCanvasComponent::clip_names() const {
#ifdef HVYM_HAS_DRAGONBONES
    if (rig && rig->valid()) return rig->animationNames();
#endif
    return {};
}

std::string SkeletalCanvasComponent::active_clip() const {
#ifdef HVYM_HAS_DRAGONBONES
    if (!activeClip.empty()) return activeClip;
#endif
    return d.clip;
}

void SkeletalCanvasComponent::set_clip(const std::string& name) {
    d.clip = name;
#ifdef HVYM_HAS_DRAGONBONES
    activeClip = name;
    apply_play_mode();   // AUTO → loop the new clip now; ON_TOUCH → rest until touched
#endif
}

void SkeletalCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>&) const {
#ifdef HVYM_HAS_DRAGONBONES
    ensure_rig(*drawData.rMan);
    if (rig && rig->valid()) {
        canvas->save();
        canvas->translate(d.pos.x(), d.pos.y());
        canvas->scale(d.scale, d.scale);
        rig->draw(canvas);
        canvas->restore();
        return;
    }
#endif
    // Placeholder: loading, missing resources, failed load, or a non-DragonBones build.
    const Vector2f tl = d.pos - d.halfExtent * d.scale;
    const Vector2f br = d.pos + d.halfExtent * d.scale;
    SkPaint p(SkColor4f{0.40f, 0.42f, 0.52f, 0.40f});
    p.setAntiAlias(drawData.skiaAA);
    canvas->drawRect(SkRect::MakeLTRB(tl.x(), tl.y(), br.x(), br.y()), p);
}

void SkeletalCanvasComponent::initialize_draw_data(DrawingProgram&) {
    create_collider();
}

void SkeletalCanvasComponent::create_collider() {
    using namespace SCollision;
    // Box in canvas/collider space. The rig draws as translate(d.pos)·scale(d.scale)
    // over rig-local coords, so a local point L maps to d.pos + d.scale*L. Prefer the
    // rig's actual (grown) drawn bounds; fall back to the symmetric halfExtent box
    // while loading / on non-DragonBones builds.
    Vector2f tl, br;
#ifdef HVYM_HAS_DRAGONBONES
    if (rigBoundsKnown) {
        tl = d.pos + rigMinLocal * d.scale;
        br = d.pos + rigMaxLocal * d.scale;
    } else
#endif
    {
        tl = d.pos - d.halfExtent * d.scale;
        br = d.pos + d.halfExtent * d.scale;
    }
    ColliderCollection<float> objs;
    std::array<Vector2f, 4> t = triangle_from_rect_points(tl, br);
    objs.triangle.emplace_back(t[0], t[1], t[2]);
    objs.triangle.emplace_back(t[2], t[3], t[0]);
    collisionTree.clear();
    collisionTree.calculate_bvh_recursive(objs);
}

#ifdef HVYM_HAS_DRAGONBONES
void SkeletalCanvasComponent::refresh_rig_bounds() {
    if (!rig || !rig->valid()) return;
    float minX, minY, maxX, maxY;
    if (!rig->localBounds(minX, minY, maxX, maxY)) return;   // no geometry yet — retry next frame
    // A little local-space padding so anti-aliased edges at the extremes aren't
    // shaved by the clip region.
    const float pad = 8.0f;
    Vector2f nmin{minX - pad, minY - pad};
    Vector2f nmax{maxX + pad, maxY + pad};

    bool changed = false;
    if (!rigBoundsKnown) {
        rigMinLocal = nmin; rigMaxLocal = nmax; rigBoundsKnown = true; changed = true;
    } else {
        // Grow only (monotonic): once the box covers the tallest jump / widest reach
        // it stays covering it, so playback never re-clips.
        if (nmin.x() < rigMinLocal.x()) { rigMinLocal.x() = nmin.x(); changed = true; }
        if (nmin.y() < rigMinLocal.y()) { rigMinLocal.y() = nmin.y(); changed = true; }
        if (nmax.x() > rigMaxLocal.x()) { rigMaxLocal.x() = nmax.x(); changed = true; }
        if (nmax.y() > rigMaxLocal.y()) { rigMaxLocal.y() = nmax.y(); changed = true; }
    }
    if (changed) {
        create_collider();
        // The container caches its world AABB (drives the draw-cache clip + culling)
        // and only refreshes on commit — recompute it now or the box stays frozen.
        if (compContainer) compContainer->calculate_world_bounds();
    }
}
#endif

bool SkeletalCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    return collisionTree.is_collide(checkAgainst);
}

SCollision::AABB<float> SkeletalCanvasComponent::get_obj_coord_bounds() const {
    return collisionTree.objects.bounds;
}
