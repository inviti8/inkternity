#pragma once
// Shared HTTP core for the hvym-img-tools endpoints (REANGLE_API.md, MESH_REFERENCE.md).
//
// One multipart POST to {base}/tools/{name} → one binary `.glb` out. Every tool
// (reangle, mesh, …) rides the same wire contract — timeouts, TLS, threading,
// glTF validation — so that machinery lives here once and the per-tool code
// (ReangleClient, MeshFlow) is a thin call that names the tool and its fields.
//
// Async by necessity: a cold serverless worker can take minutes to first byte
// (REANGLE_API.md §2), so the request runs on a background thread (curl_multi,
// mirroring Helpers/FileDownloader) and the UI polls `status` each frame. NEVER
// block the UI thread on it.
//
// NATIVE ONLY. The Emscripten/WASM build cannot call these — the service sends no
// CORS headers, so a browser refuses the request before it leaves (REANGLE_API.md
// §9). On that target `request()` fails immediately with a clear message.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AI {

class ToolClient {
public:
    // One in-flight (or finished) tool call. Handed back as a shared_ptr; the
    // worker thread flips `status` to SUCCESS/FAILURE and fills the rest. The UI
    // polls `status` and does the ArmatureModel load on the main thread once SUCCESS.
    struct Request {
        enum class Status { IN_PROGRESS = 0, SUCCESS, FAILURE };
        std::vector<uint8_t> glb;          // the .glb bytes on success (validated: starts with "glTF")
        std::string          error;        // human-readable reason on failure
        std::string          toolVersion;  // X-Tool-Version — which pipeline built the mesh
        std::string          cacheKey;     // X-Cache-Key — sha256(image+params); the library asset id (mesh)
        long                 httpCode = 0; // last HTTP status (0 if the transfer never completed)
        std::atomic<bool>    cacheHit{false};  // X-Cache: HIT (result served from the content-addressed cache)
        std::atomic<float>   progress{0.0f};   // best-effort transfer fraction; treat as INDETERMINATE (see §2)
        std::atomic<Status>  status{Status::IN_PROGRESS};
    };

    // One multipart field: a named file (the image) or a named text value (a param).
    // A file part has non-empty `bytes` (+ filename/mimeType); a text part has
    // `textValue` and empty `bytes`.
    struct Field {
        std::string          name;
        std::string          textValue;     // used when bytes is empty
        std::vector<uint8_t> bytes;          // file part payload (e.g. the PNG)
        std::string          filename;       // for file parts (e.g. "drawing.png")
        std::string          mimeType;       // for file parts (e.g. "image/png")
    };

    // Bracket the subsystem's lifetime. curl's process-global state is owned by
    // Helpers/FileDownloader (both init/cleanup are called in main.cpp around
    // these); ToolClient only manages its own worker thread + shutdown flag.
    static void init();
    static void cleanup();

    // POST `fields` to {baseUrl}/tools/{toolName} with `apiKey` as X-API-Key.
    // Returns immediately; poll the handle. Native only (the WASM stub fails fast
    // — no CORS). `toolName` also names the tool in failure messages and logs.
    // `timeoutSeconds` is the whole-transfer budget: it MUST stay above the
    // service's own cold-start budget so a stuck job surfaces as the service's JSON
    // error, not a client timeout. Since reangle 0.3.0 both tools run on the same
    // TRELLIS worker/endpoint, so there is no per-tool number: ≥900 s for every tool,
    // because the proxy's budget is 840 s and nginx's is 900 s and the client must be
    // the last to give up (MESH_API.md §3). A slow response here is a normal response;
    // the previous 300 s default let the client abort a job the service then cached,
    // which read as "flaky, retry twice" rather than a timeout.
    static std::shared_ptr<Request> request(const std::string& baseUrl,
                                             const std::string& apiKey,
                                             const std::string& toolName,
                                             std::vector<Field> fields,
                                             long timeoutSeconds = 900);
};

}  // namespace AI
