#pragma once
// AI camera-reangle client — the network half of REANGLE_PIPELINE.md §7, whose
// full client contract is docs/design/REANGLE_API.md.
//
// One character drawing (PNG) in → one textured `.glb` out, over
// https://img.hvym.link/tools/reangle. The returned mesh carries the artist's
// original art as a front-projected UV atlas, so it feeds straight into
// Armature::ArmatureModel::load_from_memory and is orbited + baked in-app; the
// service is not in the interactive loop (one call per drawing, then all local).
//
// Async by necessity: a cold serverless worker can take minutes to first byte
// (REANGLE_API.md §2), so the request runs on a background thread (curl_multi,
// mirroring Helpers/FileDownloader) and the UI polls `status` each frame. NEVER
// block the UI thread on it.
//
// NATIVE ONLY. The Emscripten/WASM build cannot call this — the service sends no
// CORS headers, so a browser refuses the request before it leaves (REANGLE_API.md
// §9). On that target `request()` fails immediately with a clear message.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AI {

class ReangleClient {
public:
    // One in-flight (or finished) reangle. Handed back as a shared_ptr; the worker
    // thread flips `status` to SUCCESS/FAILURE and fills the rest. The UI polls
    // `status` and does the ArmatureModel load on the main thread once SUCCESS.
    struct Request {
        enum class Status { IN_PROGRESS = 0, SUCCESS, FAILURE };
        std::vector<uint8_t> glb;          // the .glb bytes on success (validated: starts with "glTF")
        std::string          error;        // human-readable reason on failure
        std::string          toolVersion;  // X-Tool-Version — which pipeline built the mesh
        long                 httpCode = 0; // last HTTP status (0 if the transfer never completed)
        std::atomic<bool>    cacheHit{false};  // X-Cache: HIT (result served from the content-addressed cache)
        std::atomic<float>   progress{0.0f};   // best-effort transfer fraction; treat as INDETERMINATE (see §2)
        std::atomic<Status>  status{Status::IN_PROGRESS};
    };

    // Bracket the subsystem's lifetime. curl's process-global state is owned by
    // Helpers/FileDownloader (both init/cleanup are called in main.cpp around
    // these); ReangleClient only manages its own worker thread + shutdown flag.
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
