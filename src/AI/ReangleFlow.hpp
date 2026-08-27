#pragma once
// AI reangle flow — the glue between a framed canvas selection and an orbitable
// 3D proxy on the canvas. Owns the single in-flight ReangleClient request and
// drives it to completion across frames (REANGLE_PIPELINE.md §7.6):
//
//   begin_capture → SquareCanvasCaptureTool (artist drags a square around the
//     character) → onCapture PNG-encodes the region → start() POSTs it via
//     ReangleClient → tick() polls each frame → on success the textured .glb is
//     placed as a static ARMATURE model (double-click to orbit + bake).
//
// One reangle at a time — the service's access pattern is one call per drawing,
// then all interaction (orbit, bake) is local (REANGLE_API.md §4).

#include <cstdint>
#include <string>
#include <vector>

class DrawingProgram;
class GlobalConfig;

namespace AI {

class ReangleFlow {
public:
    // Resolve the HVYM Tools API key + endpoint: the Settings → Debug field wins,
    // else the env var (HVYM_TOOLS_KEY / HVYM_TOOLS_ENDPOINT), else the built-in
    // default endpoint. Returns true if a key is available. Shared by the reangle
    // call and the warm-lease toggle so both read the same source.
    static bool resolve_config(const GlobalConfig& conf, std::string& key, std::string& endpoint);

    // Menu action: activate the square-capture tool so the artist frames the
    // character; the captured region is sent to the reangle service on commit.
    static void begin_capture(DrawingProgram& drawP);

    // Poll the in-flight request; called every frame from DrawingProgram::update.
    static void tick(DrawingProgram& drawP);

    // True while a request is outstanding (guards a second concurrent reangle and
    // lets the menu reflect a busy state).
    static bool is_busy();

private:
    // Encoded PNG → POST. Reads the endpoint + key from the environment.
    static void start(DrawingProgram& drawP, std::vector<uint8_t> png);
};

}  // namespace AI
