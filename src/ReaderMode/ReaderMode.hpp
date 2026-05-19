#pragma once
#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class World;
namespace GUIStuff { class GUIManager; }

// PHASE1.md §7 — reader mode toggle + traversal state.
//
// M7-a: just the toggle + "current waypoint" anchor. Editor chrome
// (tree-view panel, waypoint markers) hides while active; the camera
// smooth-moves to the selected waypoint on entry. The user-facing
// keyboard navigation, branch UI, and dead-end affordance land in
// M7-b/c/d.
class ReaderMode {
    public:
        explicit ReaderMode(World& w);

        bool is_active() const { return active; }
        void toggle();
        void set_active(bool a);

        // Currently displayed waypoint while in reader mode. Set on entry,
        // updated by forward/back navigation.
        bool has_current() const                       { return currentId.has_value(); }
        NetworkingObjects::NetObjID get_current() const { return currentId.value_or(NetworkingObjects::NetObjID{}); }
        void set_current(NetworkingObjects::NetObjID id);

        // Forward: follows the current waypoint's first outgoing edge.
        // At a branch point (2+ outgoing edges) this still picks the
        // first edge silently — the branch-choice overlay (M7-c) is
        // the proper UI for that case, but keyboard nav stays usable.
        void forward();

        // Back: pops the most recent visit off the history stack and
        // navigates to it. No-op at the start.
        void back();
        bool has_history() const { return !history.empty(); }

        // Outgoing edges from the current waypoint, in graph order.
        // Each entry is (target waypoint id, optional edge label).
        // Empty when current is null or has no outgoing edges.
        std::vector<std::pair<NetworkingObjects::NetObjID, std::optional<std::string>>> outgoing_choices() const;

        // Convenience: 2+ outgoing edges from current — i.e. the
        // branch-choice overlay should render.
        bool is_branch_point() const;

        // Direct navigation: pushes current onto history, sets new
        // current, snaps the camera. Does nothing if reader mode
        // isn't active.
        void navigate_to(NetworkingObjects::NetObjID id);

        // TRANSITIONS.md T7 — per-frame tick. Drives the auto-advance
        // state machine (camera-arrival timer + stopTime pause). No-op
        // when not on a transition point. Called from World::update
        // after the camera tick.
        void update(float deltaTime);

        // TRANSITIONS.md T9 — true when the current node is a
        // transition point. Branch-overlay render path uses this to
        // suppress the outgoing-choice buttons (the reader doesn't
        // pick from a transition; it's auto-advanced).
        bool current_is_transition() const;

    private:
        // Anchors the camera to currentId's stored framing.
        void snap_camera_to_current();

        // TRANSITIONS.md T7 — internal "user didn't trigger this"
        // navigation, used by the auto-advance walk. Same as
        // navigate_to but skips the history push (transition ids
        // never enter history per T8) and doesn't reset the chain
        // hop counter.
        void auto_advance_to(NetworkingObjects::NetObjID id);

        // TRANSITIONS.md T7 — entered after navigate_to / auto_advance_to
        // lands on a transition point. Snapshots the camera-move
        // duration locally so we tick our own timer rather than polling
        // DrawCamera (the polish-attempt failure mode).
        void enter_transition_phase_for_current();

        // TRANSITIONS.md T7 — clears phase + timers. Called on user
        // input (back/forward/branch click) and on set_active(false)
        // so the auto-advance never fires after a user interaction.
        void cancel_auto_advance();

        // AUDIO.md §5 — recursive walk that resolves "what audio rule
        // applies at waypoint W." Returns the id of the resolving
        // waypoint (the first one upstream that either has audio
        // attached or stopAudio set); empty NetObjID if no resolver
        // is found within MAX_TRANSITION_CHAIN hops. Walks the first
        // incoming edge at each step (same tiebreaker as transitions
        // — TRANSITIONS.md §5).
        NetworkingObjects::NetObjID resolve_audio_for(NetworkingObjects::NetObjID waypointId,
                                                     int recursionGuard) const;

        // AUDIO.md §5 — applies the audio rule for `id` to the
        // engine. Stops the in-flight clip first (per the "audio
        // replays from the predecessor" design — every arrival
        // restarts), then either starts the resolved clip with the
        // resolver waypoint's loop flag, or stays silent if no
        // resolver exists or the resolver is a stopAudio anchor.
        void apply_audio_rule_for(NetworkingObjects::NetObjID id);

        // TRANSITIONS.md T7 — looks up the target Waypoint and returns
        // the duration its smooth_move_to will use. Mirrors the math
        // DrawCamera::smooth_move_to applies (jumpTransitionTime /
        // speedMultiplier). Used to seed the local camera-arrival
        // timer.
        float compute_camera_duration_for(NetworkingObjects::NetObjID id) const;

        // TRANSITIONS.md T10 — guard against an artist building a
        // transition cycle (A -> P -> A). The chain hop counter
        // resets on any user-driven nav, increments on each
        // auto-advance step, and bails when it exceeds this cap.
        static constexpr int MAX_TRANSITION_CHAIN = 32;

        enum class TransitionPhase : uint8_t {
            IDLE,            // not on a transition, or chain finished
            CAMERA_MOVING,   // smooth-move in flight to a transition
            PAUSING,         // arrived; counting down stopTime
        };

        World& world;
        bool active = false;
        std::optional<NetworkingObjects::NetObjID> currentId;
        // History of previously-visited waypoint ids, in order — back()
        // pops one. Forward navigation pushes the prior currentId here
        // before swapping current. Transition-point ids are NEVER
        // pushed (T8): a chain A -> P1 -> P2 -> B leaves only A in
        // history when the reader is at B.
        std::vector<NetworkingObjects::NetObjID> history;

        TransitionPhase phase = TransitionPhase::IDLE;
        float cameraTimeRemaining = 0.0f;
        float pauseTimeRemaining  = 0.0f;
        std::optional<NetworkingObjects::NetObjID> autoAdvanceTarget;
        int   chainHopCount = 0;

        // AUDIO.md §5 — audio rule fires when the camera ARRIVES at a
        // new waypoint, not when it starts moving. Mirror of the
        // transition state machine's camera-arrival timer (we tick
        // our own duration rather than poll DrawCamera). snap_camera_
        // to_current seeds these; update(deltaTime) ticks them down
        // and fires apply_audio_rule_for once the timer expires.
        // Pending flag handles the "navigation rapidly re-fires
        // before arrival" case: each new snap resets the timer to
        // the new target's arrival duration, the previous pending
        // rule never lands. The current clip keeps playing through
        // the interim (audible continuity across button-mash).
        bool  audioApplyPending = false;
        float audioApplyDelay   = 0.0f;
};

// Renders the branch-choice overlay when reader mode is active and
// the current waypoint has 2+ outgoing edges. Lays out a horizontal
// row of buttons, each clickable; each button shows the *target*
// waypoint's skin (or a generic labeled rect when no skin is set).
// Lives outside the class so it can sit alongside the layout code
// in the active screen without adding a circular include.
void render_reader_branch_overlay(World& world, GUIStuff::GUIManager& gui);
