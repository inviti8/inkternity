#pragma once
// AI camera-reangle client — the network half of REANGLE_PIPELINE.md §7, whose
// full client contract is docs/design/REANGLE_API.md.
//
// One character drawing (PNG) in → one textured `.glb` out, over
// https://img.hvym.link/tools/reangle. The returned mesh carries the artist's
// original art as a front-projected UV atlas, so it feeds straight into
// Armature::ArmatureModel::load_from_memory and is orbited + baked in-app.
//
// This is now a THIN WRAPPER over AI::ToolClient (the shared HTTP core all
// hvym-img-tools endpoints ride — see ToolClient.hpp / MESH_REFERENCE.md §3): it
// only names the "reangle" tool and builds its two fields (image + mc_resolution).
// The async/TLS/timeout/glTF machinery all lives in ToolClient.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ToolClient.hpp"

namespace AI {

class ReangleClient {
public:
    // Same handle type the shared core hands back; ReangleFlow polls it unchanged.
    using Request = ToolClient::Request;

    // Bracket the subsystem's lifetime (forwards to ToolClient — kept as the name
    // main.cpp already calls; see MESH_REFERENCE.md §3.2).
    static void init();
    static void cleanup();

    // Kick off one reangle against `baseUrl` (e.g. "https://img.hvym.link") with
    // `apiKey` (sent as X-API-Key). `png` is the rasterized selection, PNG-encoded.
    // `mcResolution` is the marching-cubes grid (64–512; 256 is the validated
    // default — REANGLE_API.md §8). Returns immediately; poll the handle.
    static std::shared_ptr<Request> request(const std::vector<uint8_t>& png,
                                             const std::string& baseUrl,
                                             const std::string& apiKey,
                                             int mcResolution = 256);
};

}  // namespace AI
