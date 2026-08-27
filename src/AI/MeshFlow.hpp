#pragma once
// AI mesh-reference flow — one rough sketch → one untextured, orbitable 3D
// reference on the canvas (MESH_REFERENCE.md §4). Mirrors ReangleFlow:
//
//   begin_capture → SquareCanvasCaptureTool (opaque WYSIWYG square) → onCapture
//     PNG-encodes the region → start() POSTs it to /tools/mesh via ToolClient →
//     tick() polls each frame → on success the untextured .glb is placed as a
//     static ARMATURE reference (the flat/lit gray model) and the orbit editor
//     opens so the artist can pick an angle to draw over.
//
// Untextured by design: the artist draws OVER the reference, so there is no
// texture atlas, no tone handling — none of the reangle texture path applies. One
// call per sketch, then all interaction (orbit) is local. The mesh runs on its own
// RunPod endpoint; the proxy routes by tool name (MESH_REFERENCE.md §7).

#include "../DrawingProgram/Tools/SquareCanvasCaptureTool.hpp"   // CaptureRegion

#include <cstdint>
#include <string>
#include <vector>

class DrawingProgram;

namespace AI {

class MeshFlow {
public:
    // Menu action: activate the square-capture tool so the artist frames the
    // sketch; the captured region is sent to /tools/mesh on commit.
    static void begin_capture(DrawingProgram& drawP);

    // Poll the in-flight request; called every frame from DrawingProgram::update.
    static void tick(DrawingProgram& drawP);

    // True while a request is outstanding (guards a second concurrent mesh call
    // and lets the menu reflect a busy state).
    static bool is_busy();

private:
    // Encoded PNG → POST. `region` is the captured world square, remembered so the
    // reference can be placed where the artist sketched it (MESH_REFERENCE.md §5).
    static void start(DrawingProgram& drawP, std::vector<uint8_t> png,
                      const SquareCanvasCaptureTool::CaptureRegion& region);
};

}  // namespace AI
