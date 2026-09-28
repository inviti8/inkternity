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

void SkeletalCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent);
}
void SkeletalCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent);
}
void SkeletalCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent);
}
void SkeletalCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber) {
    a(d.skeletonResId, d.atlasJsonResId, d.atlasPngResId, d.pos, d.scale, d.clip, d.playing, d.halfExtent);
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
        std::string clip = d.clip;
        if (clip.empty() && !anims.empty()) clip = anims.front();
        if (!clip.empty()) built->play(clip, 0);
    }
    rig = std::move(built);   // even if load failed: non-null invalid rig → placeholder, no rebuild spin
    loadAttempted = true;
}
#endif

void SkeletalCanvasComponent::update(DrawingProgram& drawP) {
#ifdef HVYM_HAS_DRAGONBONES
    ensure_rig(*drawP.world.drawData.rMan);
    if (rig && rig->valid() && d.playing) {
        const auto now = std::chrono::steady_clock::now();
        float dt = 0.0f;
        if (lastTick.time_since_epoch().count() != 0) {
            dt = std::chrono::duration<float>(now - lastTick).count();
            dt = std::clamp(dt, 0.0f, 0.1f);
        }
        lastTick = now;
        rig->update(dt);
        // Animated: force a redraw of this component's cache region each frame.
        drawP.invalidate_cache_at_component(&(*compContainer->objInfo));
    }
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
    ColliderCollection<float> objs;
    const Vector2f tl = d.pos - d.halfExtent * d.scale;
    const Vector2f br = d.pos + d.halfExtent * d.scale;
    std::array<Vector2f, 4> t = triangle_from_rect_points(tl, br);
    objs.triangle.emplace_back(t[0], t[1], t[2]);
    objs.triangle.emplace_back(t[2], t[3], t[0]);
    collisionTree.clear();
    collisionTree.calculate_bvh_recursive(objs);
}

bool SkeletalCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    return collisionTree.is_collide(checkAgainst);
}

SCollision::AABB<float> SkeletalCanvasComponent::get_obj_coord_bounds() const {
    return collisionTree.objects.bounds;
}
