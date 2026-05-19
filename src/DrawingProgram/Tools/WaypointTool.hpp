#pragma once
#include "DrawingProgramToolBase.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"
#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <cstdint>

class DrawingProgram;
struct DrawData;

// FRAME_ANIM.md §3 — axis for the Next Frame snap-pan. Tool state, not
// persisted (per the no-data-model design).
enum class FrameStepAxis : uint8_t {
    POS_X = 0,
    NEG_X = 1,
    POS_Y = 2,
    NEG_Y = 3
};

// PHASE1.md §5 — canvas-side waypoint editor.
//
// M5-a behaviour:
//   - Click on empty canvas: drop a new Waypoint into WaypointGraph and
//     a WaypointCanvasComponent into the active layer at the click point.
//     The Waypoint snapshots the current camera (CoordSpaceHelper +
//     window size), which is what "click to focus" replays later.
//   - Click on an existing waypoint marker: smooth-move the camera to
//     that waypoint's stored framing.
//
// Deferred: framing-rect handles for the selected waypoint (M5-b),
// faint outgoing-edge previews (M5-c), reader-mode chrome toggle (M7).
class WaypointTool : public DrawingProgramToolBase {
    public:
        WaypointTool(DrawingProgram& initDrawP);

        virtual DrawingProgramToolType get_type() override;
        virtual void gui_toolbox(Toolbar& t) override;
        virtual void gui_phone_toolbox(PhoneDrawingProgramScreen& t) override;
        virtual void tool_update() override;
        virtual void right_click_popup_gui(Toolbar& t, Vector2f popupPos) override;
        virtual void erase_component(CanvasComponentContainer::ObjInfo* erasedComp) override;
        virtual void switch_tool(DrawingProgramToolType newTool) override;
        virtual void draw(SkCanvas* canvas, const DrawData& drawData) override;
        virtual bool prevent_undo_or_redo() override;
        virtual void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) override;

    private:
        // Returns true if the click landed on an existing waypoint
        // (and the camera was smooth-moved to it, and selection updated).
        // Otherwise the caller falls through to drop-a-new-waypoint.
        bool try_focus_existing_waypoint(const Vector2f& clickPos);

        // Drop a new Waypoint into WaypointGraph and a WaypointCanvasComponent
        // into the active layer. Selects the new waypoint.
        void drop_waypoint(const Vector2f& clickPos);

        // Shift+click handler: if the click hit an existing waypoint and
        // a different waypoint is selected, append an Edge from selection
        // to the clicked one. Returns true if an edge was created.
        bool try_create_edge_to_clicked(const Vector2f& clickPos);

        // Drops the entire draw-cache surface so the next frame
        // re-renders every component. Used after a waypoint property
        // change that alters the marker's pixels (e.g. the transition
        // flag) — without this, the BVH keeps blitting the previously-
        // rendered marker. Heavy hammer but safe and matches what
        // CanvasTheme uses for the same class of "visuals changed"
        // event; the cost is one full re-render on a user click, which
        // is unnoticeable.
        void invalidate_marker_caches();

        // FRAME_ANIM.md §3 — snap-pans the editor camera by exactly one
        // current-window-size viewport in `axis` (projected through the
        // live camera's rotation via coords.from_space). Selection is
        // deliberately NOT modified so a subsequent Copy Frame keeps
        // sourcing from the previous frame.
        void next_frame_step(FrameStepAxis axis);

        // FRAME_ANIM.md §4 — renders the currently-selected waypoint's
        // active-layer pixels into an ImageCanvasComponent placed at
        // the live camera's framing rect on a layer chosen by
        // `frameCopyLayerOffset` (relative to the currently-edited
        // layer in the flattened layer order; clamps back to the
        // edited layer if the offset is out of bounds). One-shot
        // raster paste; the resulting image is a regular canvas
        // component (transformable, erasable, hideable via layer).
        void copy_frame_to_current();

        // Selection state lives on WaypointGraph as the single source of
        // truth shared between this tool and the TreeView panel — a
        // click in either updates the other's chrome.

        // FRAME_ANIM.md §3 — session-only tool state for the Next Frame
        // axis radio. Not persisted, not synced. Default: +X.
        FrameStepAxis frameStepAxis = FrameStepAxis::POS_X;

        // FRAME_ANIM.md §4 — destination layer offset for Copy Frame,
        // relative to the currently-edited layer in the flattened layer
        // order. 0 = same layer, -1 = layer below, +1 = layer above. If
        // the resulting index is out of bounds, falls back to the
        // current layer. Session-only state; default 0.
        int frameCopyLayerOffset = 0;
        static constexpr int FRAME_COPY_LAYER_OFFSET_MIN = -10;
        static constexpr int FRAME_COPY_LAYER_OFFSET_MAX =  10;

        // FRAME_ANIM.md §4 — Next Frame stashes the previously-selected
        // waypoint here, clears wpGraph selection, then pans the camera.
        // The next call to drop_waypoint sees `hasPendingChainSource`
        // and auto-wires an edge `pendingChainSourceId -> newWaypoint`
        // so the animation chain builds without the artist having to
        // shift+click an offscreen previous waypoint to wire each edge.
        // Copy Frame then resolves "previous frame" via the incoming
        // edge to the currently-selected waypoint (graph-based, not
        // tool-state-based).
        NetworkingObjects::NetObjID pendingChainSourceId{};
        bool hasPendingChainSource = false;
};
