#pragma once
#include <cstdint>
#include <vector>
#include <cereal/types/vector.hpp>
#include <Helpers/Serializers.hpp>
#include "../../SharedTypes.hpp"
#include "../../CoordSpaceHelper.hpp"
#include "FlipbookMode.hpp"

// MOTION-PATH.md — a bezier curve bound to one flip-book group (PHASE10 Feature B,
// §B.12). The flip-book travels this path (translate + scale) as it plays; the
// path is editor-only chrome (drawn by MotionPathTool, never in viewer mode) and
// owned by its flip-book folder (a NetObjOwnerPtr on the list item, like nameData/
// displayData), so it dies with the folder — no owner-id, no cascade.
//
// Geometry mirrors the polygon system exactly (RectangleCanvasComponent::Data):
// node positions + controlIn/controlOut tangent OFFSETS + nodeType, all in the
// path's own LOCAL space (Vector2f), placed in the world by `coords`. Using local
// floats (not WorldVec) inherits the shape system's Eigen/undo-gotcha workaround.
// Per-node animation channels (nodeTime/nodeScale/nodeEasing) run parallel to the
// nodes. Synced whole-struct via DelayUpdateSerializedClassManager (paths are
// small), registered in DrawingProgramLayerListItem::register_class.
struct MotionPath {
    CoordSpaceHelper coords;               // world placement of the path

    std::vector<Vector2f> points;          // node positions (path-local)
    std::vector<Vector2f> controlIn;       // tangent offset from points[i]
    std::vector<Vector2f> controlOut;      // tangent offset from points[i]
    std::vector<uint8_t>  nodeType;        // 0 corner / 1 smooth / 2 cusp

    std::vector<float>    nodeSeconds;     // SECONDS to travel here from the previous
                                           //   node (per-segment duration; [0] unused,
                                           //   the start has no incoming segment).
    std::vector<float>    nodeScale;       // scale at this node (default 1.0), tweens
    std::vector<uint8_t>  nodeEasing;      // TransitionEasing for the segment after i
    std::vector<float>    nodeRotation;    // rotation-along-tangent amount, -1..1 (0 = none;
                                           //   the tangent's turn from the start heading is
                                           //   multiplied by this), tweens between nodes.

    // The path has its OWN play-style, independent of the group's frame cycle
    // (zynx: e.g. a walk cycle that LOOPS its frames while the body travels the
    // path ONCE and stops). Set in the path editor when the curve / first node is
    // selected. Reuses the flip-book enum. Trigger (what STARTS it) is still
    // shared with the group for v1.
    FlipbookPlayStyle playStyle = FlipbookPlayStyle::ONCE;

    // Transient runtime — NEVER serialized/synced (per-viewer, like FlipbookRuntime).
    // Advanced by the flip-book playback tick, read by draw_flipbook_frame.
    double pathProgress = 0.0;             // ELAPSED seconds along the traversal
    bool   pathReversing = false;          // ping-pong direction

    template <typename Archive> void serialize(Archive& a) {
        a(coords, points, controlIn, controlOut, nodeType, nodeSeconds, nodeScale, nodeEasing, nodeRotation, playStyle);
    }

    // Total traversal time = sum of the per-segment seconds.
    float total_seconds() const;

    // MOTION-PATH.md P3 — sample the path at progress ∈ [0,1]: the position
    // (path-local) + interpolated scale. Parametric-per-segment (nodeTime warps
    // the distribution; nodeEasing eases within a segment). See MotionPath.cpp.
    struct Sample { Vector2f pos; float scale; float rotationDeg; };
    Sample sample(double progress) const;
    // Advance pathProgress by deltaTime over `duration`, per playStyle
    // (once/loop/ping-pong). reset() returns to the start.
    void advance(float deltaTime);
    void reset();
};
