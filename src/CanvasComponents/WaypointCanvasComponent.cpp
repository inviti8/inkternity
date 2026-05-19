#include "WaypointCanvasComponent.hpp"
#include "../DrawData.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include "../ReaderMode/ReaderMode.hpp"
#include "../Waypoints/Waypoint.hpp"
#include "../Waypoints/WaypointGraph.hpp"
#include <Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp>
#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>

CanvasComponentType WaypointCanvasComponent::get_type() const {
    return CanvasComponentType::WAYPOINT;
}

WaypointCanvasComponent::WaypointCanvasComponent() {}

void WaypointCanvasComponent::set_data(NetworkingObjects::NetObjID waypointId, const Vector2f& markerPos) {
    d.waypointId = waypointId;
    d.markerPos = markerPos;
}

void WaypointCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    a(d);
}

void WaypointCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d);
}

void WaypointCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d);
}

void WaypointCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber) {
    a(d);
}

std::unique_ptr<CanvasComponent> WaypointCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<WaypointCanvasComponent>();
    toRet->d = d;
    return toRet;
}

void WaypointCanvasComponent::set_data_from(const CanvasComponent& other) {
    d = static_cast<const WaypointCanvasComponent&>(other).d;
}

void WaypointCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>&) const {
    // Reader mode hides editor chrome (PHASE1.md §7); the marker is
    // editor chrome.
    if (drawData.main && drawData.main->world && drawData.main->world->readerMode.is_active())
        return;

    // Marker visual: filled gold disc with a dark outline. Gold (~#E0B040)
    // because it doesn't compete with brush-stroke colors and reads as a
    // "navigation marker" rather than "drawn content". When the waypoint
    // has a skin assigned (PHASE1.md §5a), tint the marker accent-pink
    // so the artist can see at a glance which destinations have artwork.
    const SkScalar cx = static_cast<SkScalar>(d.markerPos.x());
    const SkScalar cy = static_cast<SkScalar>(d.markerPos.y());
    const SkScalar r = static_cast<SkScalar>(MARKER_RADIUS_PX);

    bool hasSkin = false;
    bool isTransition = false;
    bool hasAudio = false;
    bool stopAudio = false;
    if (drawData.main && drawData.main->world) {
        auto wpRef = drawData.main->world->netObjMan.get_obj_temporary_ref_from_id<Waypoint>(d.waypointId);
        if (wpRef) {
            hasSkin = wpRef->has_skin();
            isTransition = wpRef->is_transition();
            hasAudio = wpRef->has_audio();
            stopAudio = wpRef->get_stop_audio();
        }
    }

    SkPaint fill;
    fill.setAntiAlias(drawData.skiaAA);
    fill.setColor4f(hasSkin
        ? SkColor4f{0.92f, 0.40f, 0.62f, 1.0f}   // accent pink for skinned
        : SkColor4f{0.88f, 0.69f, 0.25f, 1.0f}); // gold for plain
    fill.setStyle(SkPaint::kFill_Style);

    SkPaint outline;
    outline.setAntiAlias(drawData.skiaAA);
    outline.setColor4f({0.15f, 0.10f, 0.05f, 1.0f});
    outline.setStyle(SkPaint::kStroke_Style);
    outline.setStrokeWidth(0.0f);

    if (isTransition) {
        // TRANSITIONS.md — transition points draw as a smaller diamond
        // (square rotated 45°) so they read as visually subordinate
        // stepping-stones between user-stop waypoints. ~70% radius keeps
        // them clearly smaller than waypoint discs at any zoom level.
        const SkScalar dr = r * 0.7f;
        SkPathBuilder pb;
        pb.moveTo(cx,      cy - dr);
        pb.lineTo(cx + dr, cy);
        pb.lineTo(cx,      cy + dr);
        pb.lineTo(cx - dr, cy);
        pb.close();
        SkPath diamond = pb.detach();
        canvas->drawPath(diamond, fill);
        canvas->drawPath(diamond, outline);
    } else {
        canvas->drawCircle(cx, cy, r, fill);
        canvas->drawCircle(cx, cy, r, outline);
    }

    // AUDIO.md §4 — tiny indicator overlay so the artist can see at a
    // glance which waypoints carry audio. A small filled dot at the
    // marker's upper-right means "has audio attached"; a tinted dot
    // means "stop-audio anchor." Both glyphs are subordinate to the
    // base shape (≈30% of the marker radius) so they don't compete
    // visually with the skin / transition signal.
    if (hasAudio || stopAudio) {
        const SkScalar dotR = r * 0.32f;
        // Position relative to upper-right of the marker; works for
        // both disc and diamond bases.
        const SkScalar dotCx = cx + r * 0.72f;
        const SkScalar dotCy = cy - r * 0.72f;
        SkPaint dotFill;
        dotFill.setAntiAlias(drawData.skiaAA);
        if (hasAudio)
            dotFill.setColor4f({0.20f, 0.55f, 0.90f, 1.0f});  // bright blue: "audio attached"
        else
            dotFill.setColor4f({0.65f, 0.65f, 0.65f, 1.0f});  // muted grey: "silence anchor"
        canvas->drawCircle(dotCx, dotCy, dotR, dotFill);
        canvas->drawCircle(dotCx, dotCy, dotR, outline);
    }
}

void WaypointCanvasComponent::initialize_draw_data(DrawingProgram&) {
}

bool WaypointCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    // AABB-vs-AABB hit test against the marker's bounding box. M5-a click-
    // detection uses the eraser-style wide-line collider, so an AABB hit
    // is good enough — fine-grained circle hit-test would need a custom
    // collide() overload.
    return SCollision::collide(checkAgainst.bounds, get_obj_coord_bounds());
}

SCollision::AABB<float> WaypointCanvasComponent::get_obj_coord_bounds() const {
    SCollision::AABB<float> bounds;
    bounds.min = Vector2f(d.markerPos.x() - MARKER_RADIUS_PX, d.markerPos.y() - MARKER_RADIUS_PX);
    bounds.max = Vector2f(d.markerPos.x() + MARKER_RADIUS_PX, d.markerPos.y() + MARKER_RADIUS_PX);
    return bounds;
}
