#pragma once
#include <Helpers/NetworkingObjects/NetObjOwnerPtr.hpp>
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include <Helpers/VersionNumber.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include <Eigen/Core>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "../CoordSpaceHelper.hpp"

// PHASE2 M5 — fixed set of CSS-style easing presets for the per-waypoint
// reader-mode transition curve. Stored as a uint8 in the file; resolved
// to (x1, y1, x2, y2) cubic-bezier control points at render time so the
// curve definitions can be tweaked without breaking saved files.
//
// Custom-curve editing (a CSS cubic-bezier visualizer that lets the
// artist drag handles) is intentionally Phase 3 territory. Phase 2
// ships these five and stops there.
enum class TransitionEasing : uint8_t {
    LINEAR      = 0,
    EASE        = 1,  // CSS default — the "ease" timing function
    EASE_IN     = 2,
    EASE_OUT    = 3,
    EASE_IN_OUT = 4
};
Eigen::Vector4f transition_easing_to_bezier_curve(TransitionEasing e);
const std::vector<std::string>& transition_easing_display_names();

class World;
class WaypointGraph;

// PHASE1.md §5 — a single waypoint node. Stores a camera state
// (CoordSpaceHelper + windowSize, mirroring BookmarkData) plus a label.
// One Waypoint == one droppable canvas marker; edges between them live
// in WaypointGraph alongside.
//
// NetObj-class-registered so it participates in multi-user sync (the
// design doc commits to this in §12 to avoid silently breaking the
// existing bookmark sync we're replacing).
//
// Thumbnail is intentionally omitted at M4: it's a lazily regenerated
// SkImage of the framing rect and lives in M5 with the canvas component
// that shows it.
class Waypoint {
    public:
        Waypoint();
        Waypoint(const std::string& initLabel,
                 const CoordSpaceHelper& initCoords,
                 const Vector<int32_t, 2>& initWindowSize);

        const std::string& get_label() const { return label; }
        void set_label(const std::string& newLabel) { label = newLabel; }
        // Direct mutable handle for the label-edit text input. After
        // mutating, callers MUST call publish_label_update() (below) on
        // the corresponding NetObjTemporaryPtr so subscribers receive
        // the update. Phase 0 wires this through WaypointTool's text
        // input via onEdit; future fields follow the same pattern.
        std::string& mutable_label() { return label; }

        // P0.5-LIVE-SYNC: each publish_*_update broadcasts the current
        // value of the named field as a NetObj update message so
        // already-connected peers (subscribers in Phase 0's terminology)
        // see the new value. Idempotent — mutate the field first via
        // the mutable_*() / set_*() handle, then call the matching
        // publish method. Safe to call repeatedly.
        static void publish_label_update(const NetworkingObjects::NetObjTemporaryPtr<Waypoint>& o);
        static void publish_transition_speed_update(const NetworkingObjects::NetObjTemporaryPtr<Waypoint>& o);
        static void publish_transition_easing_update(const NetworkingObjects::NetObjTemporaryPtr<Waypoint>& o);
        static void publish_is_transition_update(const NetworkingObjects::NetObjTemporaryPtr<Waypoint>& o);
        static void publish_stop_time_update(const NetworkingObjects::NetObjTemporaryPtr<Waypoint>& o);
        static void publish_skin_update(const NetworkingObjects::NetObjTemporaryPtr<Waypoint>& o);

        const CoordSpaceHelper& get_coords() const { return coords; }
        const Vector<int32_t, 2>& get_window_size() const { return windowSize; }

        // PHASE1.md §5a — artist-drawn navigation-button image. Captured
        // by ButtonSelectTool. Persisted as PNG bytes in the file format
        // (no NetObj sync for the moment — large payloads through NetObj
        // messages are their own scoping problem and this fork is
        // single-user anyway).
        bool has_skin() const             { return skin != nullptr; }
        sk_sp<SkImage> get_skin() const   { return skin; }
        void set_skin(sk_sp<SkImage> img) { skin = std::move(img); }
        void clear_skin()                 { skin.reset(); }

        // PHASE2 M4: per-waypoint speed multiplier for the reader-mode
        // camera transition INTO this waypoint. Range 0.1 .. 100.0. The
        // applied transition duration is globalDuration / multiplier,
        // so 100.0 plays the transition a hundred times as fast and
        // 0.5 plays it half-speed. Default 1.0 leaves the global
        // behavior unchanged. FRAME_ANIM.md §8: the upper bound was
        // widened from 2.0 to 100.0 so frame-animation chains can
        // reliably approximate hard cuts across the range of
        // jumpTransitionTime defaults (at 0.5s default + multiplier
        // 100, the smooth-pan compresses to ~5ms — well under one
        // displayed frame at 60Hz, indistinguishable from a cut).
        static constexpr float TRANSITION_SPEED_MIN = 0.1f;
        static constexpr float TRANSITION_SPEED_MAX = 100.0f;
        static constexpr float TRANSITION_SPEED_DEFAULT = 1.0f;
        float get_transition_speed_multiplier() const { return transitionSpeedMultiplier; }
        void set_transition_speed_multiplier(float v) { transitionSpeedMultiplier = std::clamp(v, TRANSITION_SPEED_MIN, TRANSITION_SPEED_MAX); }
        // Direct mutable reference for the WaypointTool slider; the slider
        // widget enforces the min/max range so external clamping isn't
        // strictly required, but set_transition_speed_multiplier still
        // clamps when callers come through that path.
        float& mutable_transition_speed_multiplier() { return transitionSpeedMultiplier; }

        // PHASE2 M5: easing preset for the reader-mode transition INTO
        // this waypoint. Stored as enum, resolved to a bezier curve via
        // transition_easing_to_bezier_curve() at transition time.
        TransitionEasing get_transition_easing() const { return transitionEasing; }
        void set_transition_easing(TransitionEasing e) { transitionEasing = e; }
        // Mutable raw access for the WaypointTool dropdown widget.
        TransitionEasing& mutable_transition_easing() { return transitionEasing; }

        // TRANSITIONS.md — transition-point variant. When `isTransition`
        // is true, this waypoint is auto-advanced through by reader mode
        // rather than stopped at: the camera arrives, pauses for
        // `stopTime` seconds, then walks the (single) outgoing edge.
        // Both fields default to "behaves as a normal waypoint" so old
        // saves and toggling the flag off restore Phase-2 behavior
        // exactly. The "single outgoing edge" invariant is enforced at
        // edge-creation/toggle time; reader mode falls back to "first
        // outgoing edge wins" when the data temporarily violates it.
        static constexpr float TRANSITION_STOP_TIME_MIN     = 0.0f;
        static constexpr float TRANSITION_STOP_TIME_MAX     = 5.0f;
        static constexpr float TRANSITION_STOP_TIME_DEFAULT = 0.0f;
        bool  is_transition()                 const { return isTransition; }
        void  set_is_transition(bool v)             { isTransition = v; }
        bool& mutable_is_transition()               { return isTransition; }
        float get_stop_time()                 const { return stopTime; }
        void  set_stop_time(float t)                { stopTime = std::clamp(t, TRANSITION_STOP_TIME_MIN, TRANSITION_STOP_TIME_MAX); }
        float& mutable_stop_time()                  { return stopTime; }

        void scale_up(const WorldScalar& scaleUpAmount);

        void save_file(cereal::PortableBinaryOutputArchive& a) const;
        // Reads the skin payload (PNG bytes) from `a` and assigns it to
        // this waypoint. Called by WaypointGraph::load_file after the
        // waypoint has been constructed via emplace_back_direct (which
        // takes label/coords/windowSize). Only meaningful for files at
        // version >= 0.6.0; pre-0.6 files don't have the payload, so
        // the caller skips this call.
        void load_skin_from_archive(cereal::PortableBinaryInputArchive& a, VersionNumber version);
        // PHASE2 M4: reads the per-waypoint transition controls
        // (currently just the speed multiplier; M5 will append easing).
        // Caller gates on file version >= 0.9.0.
        void load_transition_data_from_archive(cereal::PortableBinaryInputArchive& a, VersionNumber version);
        // TRANSITIONS.md — reads the transition-point fields
        // (isTransition + stopTime). Caller gates on file version >=
        // 0.10.0. Block is independent of load_transition_data so a
        // future migration can rearrange one without touching the
        // other.
        void load_transition_point_data_from_archive(cereal::PortableBinaryInputArchive& a, VersionNumber version);

        static void register_class(World& w);

    private:
        static void write_constructor_data(const NetworkingObjects::NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a);

        std::string label;
        CoordSpaceHelper coords;
        Vector<int32_t, 2> windowSize{0, 0};
        sk_sp<SkImage> skin;
        float transitionSpeedMultiplier = TRANSITION_SPEED_DEFAULT;
        TransitionEasing transitionEasing = TransitionEasing::EASE;
        bool  isTransition = false;
        float stopTime     = TRANSITION_STOP_TIME_DEFAULT;
};
