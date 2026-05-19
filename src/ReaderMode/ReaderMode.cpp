#include "ReaderMode.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../FontData.hpp"
#include <Helpers/Logger.hpp>
#include "../Waypoints/Waypoint.hpp"
#include "../Waypoints/Edge.hpp"
#include "../Waypoints/WaypointGraph.hpp"
#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/Elements/Element.hpp"
#include "../GUIStuff/Elements/LayoutElement.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "Helpers/ConvertVec.hpp"
#include <Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp>

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkTypeface.h>

#include <algorithm>

ReaderMode::ReaderMode(World& w) : world(w) {}

void ReaderMode::toggle() {
    set_active(!active);
}

// TRANSITIONS.md T7 — small helper used by every navigation entry
// point: returns true if `id` resolves to a Waypoint with the
// transition flag set. Dangling refs and missing nodes return false.
namespace {
bool is_waypoint_transition(World& w, NetworkingObjects::NetObjID id) {
    auto ref = w.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(id);
    return ref && ref->is_transition();
}
}  // namespace

bool ReaderMode::current_is_transition() const {
    return currentId.has_value() && is_waypoint_transition(world, currentId.value());
}

void ReaderMode::set_active(bool a) {
    if (active == a) return;
    active = a;
    cancel_auto_advance();  // clean state on every toggle
    if (active) {
        // Pick a starting waypoint: existing selection if any, else the
        // first node in the graph. Reader mode with zero waypoints is a
        // valid (if empty) state — currentId stays nullopt.
        std::optional<NetworkingObjects::NetObjID> startId;
        if (world.wpGraph.has_selection())
            startId = world.wpGraph.get_selected();
        else if (world.wpGraph.get_nodes() && !world.wpGraph.get_nodes()->empty())
            startId = world.wpGraph.get_nodes()->begin()->obj.get_net_id();
        currentId = startId;
        history.clear();
        if (currentId.has_value()) {
            snap_camera_to_current();
            // AUDIO.md §5 — initial enter into reader mode fires the
            // audio rule IMMEDIATELY, overriding the deferred-to-
            // arrival path snap_camera_to_current uses for in-mode
            // navigation. Rationale: there's no previous clip
            // playing on enter, so deferring is pure delay (the
            // artist sees the camera move with no sound for the
            // full jumpTransitionTime). Subsequent navigations DO
            // defer because the current clip should keep playing
            // through the camera pan until the new clip arrives.
            audioApplyPending = false;
            audioApplyDelay   = 0.0f;
            apply_audio_rule_for(currentId.value());
            // T7: entering reader mode directly onto a transition
            // point is allowed — auto-advance kicks in on the first
            // tick after this. The first node the reader actually
            // *stops* at is the first non-transition downstream.
            if (current_is_transition()) enter_transition_phase_for_current();
        }
    } else {
        // AUDIO.md §5 — author mode is unconditionally silent. Clear
        // any pending deferred audio-rule application so toggling
        // back on later starts clean from the new arrival.
        world.main.audioEngine.stop_current();
        audioApplyPending = false;
        audioApplyDelay   = 0.0f;
    }
    world.set_to_layout_gui_if_focus();
}

void ReaderMode::set_current(NetworkingObjects::NetObjID id) {
    currentId = id;
    cancel_auto_advance();
    if (active) {
        snap_camera_to_current();
        if (current_is_transition()) enter_transition_phase_for_current();
    }
}

void ReaderMode::forward() {
    if (!active || !currentId.has_value()) return;
    auto& edges = world.wpGraph.get_edges();
    if (!edges) return;
    // First outgoing edge wins (per-edge ordering inside the list is
    // stable; it's the order edges were created in). M7-c branch UI
    // is the proper destination picker; this stays as the keyboard
    // arrow's "advance" semantics.
    for (auto& info : *edges) {
        if (info.obj->get_from() != currentId.value()) continue;
        // Route through navigate_to so all the T7/T8/T10 bookkeeping
        // (history-skip, chain reset, transition-phase entry) lives
        // in one place.
        navigate_to(info.obj->get_to());
        return;
    }
    // No outgoing edge — dead end. M7-d will surface a "the end"
    // affordance here.
}

void ReaderMode::back() {
    if (!active || history.empty()) return;
    cancel_auto_advance();  // user input cancels any in-flight chain
    currentId = history.back();
    history.pop_back();
    snap_camera_to_current();
    // The previous waypoint we just popped to is by definition NOT a
    // transition (T8: transitions never enter history). So no need
    // to re-enter the transition phase here.
    world.set_to_layout_gui_if_focus();
}

std::vector<std::pair<NetworkingObjects::NetObjID, std::optional<std::string>>>
ReaderMode::outgoing_choices() const {
    std::vector<std::pair<NetworkingObjects::NetObjID, std::optional<std::string>>> out;
    if (!currentId.has_value()) return out;
    auto& edges = world.wpGraph.get_edges();
    if (!edges) return out;
    for (auto& info : *edges) {
        if (info.obj->get_from() != currentId.value()) continue;
        out.emplace_back(info.obj->get_to(), info.obj->get_label());
    }
    return out;
}

bool ReaderMode::is_branch_point() const {
    return outgoing_choices().size() >= 2;
}

void ReaderMode::navigate_to(NetworkingObjects::NetObjID id) {
    if (!active) return;
    cancel_auto_advance();
    chainHopCount = 0;  // user-driven nav resets the cycle counter
    // T8: only push to history if leaving a real (non-transition)
    // waypoint. Transition ids never enter history, so the chain
    // A -> P1 -> P2 -> B leaves only A on the stack when at B.
    if (currentId.has_value() && !current_is_transition())
        history.push_back(currentId.value());
    currentId = id;
    snap_camera_to_current();
    if (current_is_transition()) enter_transition_phase_for_current();
    world.set_to_layout_gui_if_focus();
}

void ReaderMode::auto_advance_to(NetworkingObjects::NetObjID id) {
    // Internal navigation triggered by the auto-advance state machine.
    // Distinct from navigate_to in three ways:
    //  - never pushes onto history (T8 — we're leaving a transition)
    //  - DOESN'T reset chainHopCount (T10 — keep counting through chain)
    //  - DOESN'T cancel the auto-advance (we ARE the auto-advance)
    if (!active) return;
    currentId = id;
    snap_camera_to_current();
    if (current_is_transition()) enter_transition_phase_for_current();
    else                          phase = TransitionPhase::IDLE;
    world.set_to_layout_gui_if_focus();
}

void ReaderMode::snap_camera_to_current() {
    if (!currentId.has_value()) return;
    auto wpRef = world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(currentId.value());
    if (!wpRef) return;
    // PHASE2 M4 + M5: per-waypoint speed multiplier scales the transition
    // duration; per-waypoint easing preset overrides the global curve.
    // Both apply to the camera move INTO this waypoint.
    world.drawData.cam.smooth_move_to(world, wpRef->get_coords(),
                                      wpRef->get_window_size().cast<float>(),
                                      /*instantJump=*/false,
                                      wpRef->get_transition_speed_multiplier(),
                                      transition_easing_to_bezier_curve(wpRef->get_transition_easing()));
    // AUDIO.md §5 — every arrival eventually fires the audio rule,
    // but defer the firing until the camera ACTUALLY arrives at the
    // new waypoint. Otherwise pressing a navigation button would cut
    // the current clip the instant the smooth-move starts, leaving
    // the artist hearing silence for the duration of the pan. We
    // tick our own arrival timer (same pattern as the transition
    // state machine) and apply the rule when it expires. The current
    // clip keeps playing through the interim. snap_camera_to_current
    // is the single hook for set_current / navigate_to /
    // auto_advance_to / set_active(true), so this covers every
    // navigation entry point.
    if (active) {
        audioApplyDelay = compute_camera_duration_for(currentId.value());
        audioApplyPending = true;
    }
}

void ReaderMode::cancel_auto_advance() {
    phase = TransitionPhase::IDLE;
    cameraTimeRemaining = 0.0f;
    pauseTimeRemaining  = 0.0f;
    autoAdvanceTarget.reset();
    chainHopCount = 0;
}

NetworkingObjects::NetObjID ReaderMode::resolve_audio_for(NetworkingObjects::NetObjID waypointId,
                                                          int recursionGuard) const {
    if (recursionGuard <= 0) return NetworkingObjects::NetObjID{};
    auto wpRef = world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(waypointId);
    if (!wpRef) return NetworkingObjects::NetObjID{};
    // Either kind of explicit resolution stops the walk and returns
    // THIS waypoint as the resolver. The caller decides whether that
    // means "play" (has_audio) or "silence" (stopAudio); audio wins
    // when both are set on the same waypoint per the §3 precedence.
    if (wpRef->has_audio())      return waypointId;
    if (wpRef->get_stop_audio()) return waypointId;
    // No explicit resolution here — walk first incoming edge.
    auto& edges = world.wpGraph.get_edges();
    if (!edges) return NetworkingObjects::NetObjID{};
    for (auto& info : *edges) {
        if (info.obj->get_to() == waypointId)
            return resolve_audio_for(info.obj->get_from(), recursionGuard - 1);
    }
    // No incoming edge: chain root with no audio attached.
    return NetworkingObjects::NetObjID{};
}

void ReaderMode::apply_audio_rule_for(NetworkingObjects::NetObjID id) {
    AudioEngine& engine = world.main.audioEngine;
    const NetworkingObjects::NetObjID resolverId =
        resolve_audio_for(id, MAX_TRANSITION_CHAIN);
    if (resolverId == NetworkingObjects::NetObjID{}) {
        engine.stop_current();
        return;
    }
    auto resolverRef = world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(resolverId);
    if (!resolverRef) {
        engine.stop_current();
        return;
    }
    if (resolverRef->has_audio()) {
        // §3 precedence: audioId wins when both fields are set.
        engine.play(resolverRef->get_audio_id(),
                    resolverRef->get_audio_loops(),
                    world);
    } else {
        // Resolver had stopAudio without audioId attached — silence.
        engine.stop_current();
    }
}

float ReaderMode::compute_camera_duration_for(NetworkingObjects::NetObjID id) const {
    auto ref = world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(id);
    if (!ref) return 0.0f;
    // Mirrors DrawCamera::smooth_move_to math (jumpTransitionTime /
    // speedMultiplier), so our local timer tracks the camera's move
    // without polling DrawCamera state.
    const float mult = ref->get_transition_speed_multiplier();
    if (mult <= 0.0f) return 0.0f;
    return world.main.conf.jumpTransitionTime / mult;
}

void ReaderMode::enter_transition_phase_for_current() {
    if (!currentId.has_value()) return;
    cameraTimeRemaining = compute_camera_duration_for(currentId.value());
    pauseTimeRemaining  = 0.0f;
    // First outgoing edge target — wins by graph order (T5 enforces
    // single-out for transitions; if multi-out somehow leaks in, the
    // first edge is the deterministic fallback per the design doc).
    autoAdvanceTarget.reset();
    if (auto& edges = world.wpGraph.get_edges()) {
        for (auto& info : *edges) {
            if (info.obj->get_from() != currentId.value()) continue;
            autoAdvanceTarget = info.obj->get_to();
            break;
        }
    }
    // T11: transition with no outgoing edges = dead-end. We don't
    // enter the auto-advance phase at all; reader stops here just
    // like any dead-end waypoint. Existing dead-end UX (back-only)
    // applies.
    if (!autoAdvanceTarget.has_value()) {
        phase = TransitionPhase::IDLE;
        return;
    }
    phase = TransitionPhase::CAMERA_MOVING;
}

void ReaderMode::update(float deltaTime) {
    if (!active) return;
    if (deltaTime <= 0.0f) return;

    // AUDIO.md §5 — fire the pending audio rule once the camera has
    // arrived. Independent of the transition state machine: applies
    // to every navigation (forward / back / branch / auto-advance /
    // entry). Each new snap_camera_to_current resets audioApplyDelay,
    // so rapid-fire navigations roll the timer forward to the latest
    // target's arrival time; the current clip plays through the
    // interim.
    if (audioApplyPending) {
        audioApplyDelay -= deltaTime;
        if (audioApplyDelay <= 0.0f) {
            audioApplyPending = false;
            audioApplyDelay   = 0.0f;
            if (currentId.has_value())
                apply_audio_rule_for(currentId.value());
        }
    }

    if (phase == TransitionPhase::IDLE) return;

    if (phase == TransitionPhase::CAMERA_MOVING) {
        cameraTimeRemaining -= deltaTime;
        if (cameraTimeRemaining > 0.0f) return;
        // Camera arrived. Read stopTime from the current Waypoint;
        // <= 0 means "no pause, advance immediately."
        float stopTime = 0.0f;
        if (currentId.has_value()) {
            auto ref = world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(currentId.value());
            if (ref) stopTime = ref->get_stop_time();
        }
        if (stopTime <= 0.0f) {
            // Skip the pause phase; fall straight through to the
            // PAUSING-handling logic below by resetting the timer to
            // 0 and changing phase. (Easier than duplicating the
            // advance code.)
            phase = TransitionPhase::PAUSING;
            pauseTimeRemaining = 0.0f;
        } else {
            phase = TransitionPhase::PAUSING;
            pauseTimeRemaining = stopTime;
            return;
        }
    }

    if (phase == TransitionPhase::PAUSING) {
        pauseTimeRemaining -= deltaTime;
        if (pauseTimeRemaining > 0.0f) return;
        // T10: cycle protection — bail before the next hop if we've
        // walked too many transitions in a row. The reader sees the
        // current node as a dead-end (back-button only) which is the
        // safest visible behavior; logged once per occurrence.
        if (chainHopCount >= MAX_TRANSITION_CHAIN) {
            Logger::get().log("WARN", "ReaderMode: transition chain exceeded MAX_TRANSITION_CHAIN, stopping at current node");
            cancel_auto_advance();
            return;
        }
        if (autoAdvanceTarget.has_value()) {
            ++chainHopCount;
            auto target = autoAdvanceTarget.value();
            // Note: auto_advance_to may set phase to CAMERA_MOVING
            // again (chained transition) or IDLE (landed on a real
            // waypoint).
            auto_advance_to(target);
        } else {
            // Dead-end transition — already handled at phase entry,
            // but defensive in case something raced.
            cancel_auto_advance();
        }
    }
}

namespace {

constexpr float BRANCH_BUTTON_SIDE = 140.0f;  // square buttons
constexpr float BRANCH_OVERLAY_PADDING = 12.0f;

// One choice in the reader-mode branch overlay. Renders the target
// waypoint's skin when set, falls back to a labeled rounded-rect.
// Click navigates the reader to the target.
class BranchChoiceElement : public GUIStuff::Element {
    public:
        explicit BranchChoiceElement(GUIStuff::GUIManager& g) : Element(g) {}

        void layout(const Clay_ElementId& id, World* w,
                    NetworkingObjects::NetObjID target,
                    std::optional<std::string> edgeLabel) {
            world = w;
            targetId = target;
            label = std::move(edgeLabel);
            CLAY(id, {
                .layout = {.sizing = {.width = CLAY_SIZING_FIXED(BRANCH_BUTTON_SIDE),
                                     .height = CLAY_SIZING_FIXED(BRANCH_BUTTON_SIDE)}},
                .custom = {this}
            }) {}
        }

        void clay_draw(SkCanvas* canvas, GUIStuff::UpdateInputData&, Clay_RenderCommand*, bool skiaAA) override {
            if (!boundingBox.has_value() || !world) return;
            const auto& bb = boundingBox.value();
            const SkRect rect = SkRect::MakeLTRB(bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y());

            auto wpRef = world->netObjMan.get_obj_temporary_ref_from_id<Waypoint>(targetId);
            const bool hasSkin = wpRef && wpRef->has_skin();

            // Hover/held visual feedback — outline brightens.
            const bool active = mouseHovering;

            if (hasSkin) {
                sk_sp<SkImage> img = wpRef->get_skin();
                const float imgW = static_cast<float>(img->width());
                const float imgH = static_cast<float>(img->height());
                const float scale = std::min((rect.width() - 4.0f) / imgW, (rect.height() - 4.0f) / imgH);
                const float drawW = imgW * scale;
                const float drawH = imgH * scale;
                const float dx = rect.fLeft + (rect.width() - drawW) * 0.5f;
                const float dy = rect.fTop + (rect.height() - drawH) * 0.5f;
                SkPaint imgPaint;
                imgPaint.setAntiAlias(skiaAA);
                canvas->drawImageRect(img.get(),
                                      SkRect::MakeXYWH(dx, dy, drawW, drawH),
                                      SkSamplingOptions{SkFilterMode::kLinear}, &imgPaint);
                if (active) {
                    SkPaint sel;
                    sel.setAntiAlias(skiaAA);
                    sel.setStyle(SkPaint::kStroke_Style);
                    sel.setStrokeWidth(3.0f);
                    sel.setColor4f({0.95f, 0.85f, 0.45f, 1.0f});
                    canvas->drawRect(SkRect::MakeXYWH(dx, dy, drawW, drawH), sel);
                }
            } else {
                // No skin — draw a rounded rect with the choice label
                // (or target waypoint name) centered.
                SkRRect rr = SkRRect::MakeRectXY(rect, 8.0f, 8.0f);
                SkPaint fill;
                fill.setAntiAlias(skiaAA);
                fill.setColor4f(active
                    ? SkColor4f{0.32f, 0.27f, 0.18f, 1.0f}
                    : SkColor4f{0.20f, 0.20f, 0.24f, 1.0f});
                canvas->drawRRect(rr, fill);
                SkPaint outline;
                outline.setAntiAlias(skiaAA);
                outline.setStyle(SkPaint::kStroke_Style);
                outline.setStrokeWidth(active ? 2.5f : 1.5f);
                outline.setColor4f({0.88f, 0.69f, 0.25f, 1.0f});
                canvas->drawRRect(rr, outline);

                std::string text = label.value_or(std::string{});
                if (text.empty() && wpRef) text = wpRef->get_label();
                if (text.empty()) text = "(unnamed)";

                SkFont font;
                font.setSize(14.0f);
                if (world->main.fonts) {
                    auto it = world->main.fonts->map.find("Roboto");
                    if (it != world->main.fonts->map.end()) font.setTypeface(it->second);
                }
                SkPaint textPaint;
                textPaint.setAntiAlias(skiaAA);
                textPaint.setColor4f({0.95f, 0.95f, 0.95f, 1.0f});
                // Crude single-line center; M-skin will not be the only
                // place that wants better text layout, but for now this
                // ships the feature.
                canvas->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                                       rect.fLeft + 12.0f, rect.fTop + rect.height() * 0.5f + 5.0f,
                                       font, textPaint);
            }
        }

        void input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button) override {
            if (!world) return;
            if (button.button != InputManager::MouseButton::LEFT) return;
            if (!button.down) return;
            // mouseHovering doesn't update reliably for floating elements;
            // hit-test the bounding box directly.
            if (!boundingBox.has_value()) return;
            const auto& bb = boundingBox.value();
            if (button.pos.x() < bb.min.x() || button.pos.x() > bb.max.x() ||
                button.pos.y() < bb.min.y() || button.pos.y() > bb.max.y()) return;
            world->readerMode.navigate_to(targetId);
        }

    private:
        World* world = nullptr;
        NetworkingObjects::NetObjID targetId{};
        std::optional<std::string> label;
};

}  // namespace

void render_reader_branch_overlay(World& world, GUIStuff::GUIManager& gui) {
    if (!world.readerMode.is_active()) return;
    // T9 / TRANSITIONS.md — at a transition point the camera is
    // auto-advancing. The reader has no decision to make (no choice
    // buttons) and should not be able to interrupt the flow (no back
    // button either). Nothing renders during a transition stop.
    if (world.readerMode.current_is_transition()) return;
    auto choices = world.readerMode.outgoing_choices();
    // Render whenever there's something to show — at least one
    // outgoing choice OR history to step back through. Dead-ends with
    // history still get a back button so the reader isn't stranded.
    // (M7-d will add a "the end" affordance for true dead-ends with
    // no history either.)
    if (choices.empty() && !world.readerMode.has_history()) return;
    using namespace GUIStuff;

    gui.element<LayoutElement>("reader branch overlay", [&] (LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)},
                .padding = CLAY_PADDING_ALL(static_cast<uint16_t>(BRANCH_OVERLAY_PADDING)),
                .childGap = static_cast<uint16_t>(BRANCH_OVERLAY_PADDING),
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            },
            // Transparent container — only the buttons themselves
            // visually layer over the canvas. Pinned to the screen's
            // bottom-center; small upward offset keeps the buttons
            // from kissing the screen edge.
            .floating = {
                .offset = { .y = -BRANCH_OVERLAY_PADDING },
                .zIndex = static_cast<int16_t>(gui.get_z_index() + 10),
                .attachPoints = {
                    .element = CLAY_ATTACH_POINT_CENTER_BOTTOM,
                    .parent  = CLAY_ATTACH_POINT_CENTER_BOTTOM
                },
                .attachTo = CLAY_ATTACH_TO_ROOT
            },
        }) {
            // Back button as the first item, only when there's history
            // to pop. Sized down to ~60% of the choice-button side so
            // the bare arrow glyph dominates and reads as "just a
            // symbol" rather than a button-in-a-box. TRANSPARENT_ALL
            // already kills the resting background; the smaller hit
            // box collapses the empty padding around the glyph.
            if (world.readerMode.has_history()) {
                GUIStuff::ElementHelpers::svg_icon_button(
                    gui, "reader back",
                    "data/icons/RemixIcon/arrow-left-s-line.svg", {
                    .drawType = GUIStuff::SelectableButton::DrawType::TRANSPARENT_ALL,
                    .size = BRANCH_BUTTON_SIDE * 0.6f,
                    .onClick = [&world] { world.readerMode.back(); }
                });
            }
            for (size_t i = 0; i < choices.size(); ++i) {
                gui.new_id(static_cast<int64_t>(i), [&] {
                    gui.element<BranchChoiceElement>("button", &world, choices[i].first, choices[i].second);
                });
            }
        }
    });
}
