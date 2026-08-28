#include "MeshFlow.hpp"

#include "ToolClient.hpp"
#include "ReangleFlow.hpp"          // resolve_config — the shared HVYM key/endpoint resolver
#include "ServiceCapture.hpp"       // capture_has_content / encode_capture_png (shared)
#include "../DrawingProgram/DrawingProgram.hpp"
#include "../DrawingProgram/Tools/SquareCanvasCaptureTool.hpp"
#include "../Armature/ArmatureModalScreen.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"        // world.main.conf — the HVYM Tools key/endpoint fields
#include "../GlobalConfig.hpp"

#include <Helpers/Logger.hpp>

#include <include/core/SkImage.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace AI {
namespace {

// The single in-flight mesh request (null when idle). Only touched on the UI
// thread (begin_capture/start/tick all run there), so no locking here — the
// atomics live inside ToolClient::Request for the worker-thread handoff. Separate
// from reangle's gPending: the two tools have their own RunPod endpoints and can
// be in flight independently.
std::shared_ptr<ToolClient::Request> gPending;
// The world region the pending mesh was captured from, so the reference is placed
// where the artist sketched it (MESH_REFERENCE.md §5).
SquareCanvasCaptureTool::CaptureRegion gPendingRegion;

// The decimation target (absolute face count) and seed. 20k keeps ~99.5% of the
// silhouette at ~0.3 MB (mesh.md §3); seed 0 is fixed so a repeat sketch hits the
// content-addressed cache. Not exposed in the UI yet — sensible defaults for the
// MVP; a reroll control comes with the library (MESH_REFERENCE.md §8.5).
constexpr int kTargetFaces = 20000;
constexpr int kSeed = 0;

// Verbose trace to stdout (thread-safe, never throws — unlike Logger::log which
// runs a UI callback and requires a registered category). Prefixed so it's easy to
// grep the console. Temporary diagnostics for the mesh MVP bring-up.
void trace(const std::string& msg) {
    Logger::get().cross_platform_println("[mesh] " + msg);
}

// How many tick() polls the in-flight request has seen — drives a ~1 s heartbeat so
// the console shows the request is still waiting (a cold mesh worker is ~57 s+).
int gWaitTicks = 0;

}  // namespace

bool MeshFlow::is_busy() { return gPending != nullptr; }

void MeshFlow::begin_capture(DrawingProgram& drawP) {
    trace("begin_capture: entered");
    if (is_busy()) {
        trace("begin_capture: already busy, ignoring");
        Logger::get().log("USERINFO", "A 3D reference is already being built — one at a time.");
        return;
    }
    const auto previousToolType = drawP.drawTool ? drawP.drawTool->get_type()
                                                 : DrawingProgramToolType::MYPAINTBRUSH;
    // The DrawingProgram outlives the tool (owned by World), so capturing it by
    // reference for the capture callback is safe.
    auto onCapture = [&drawP](sk_sp<SkImage> image,
                              const SquareCanvasCaptureTool::CaptureRegion& region) {
        trace("onCapture: fired (image " + std::string(image ? "present" : "null") + ")");
        if (!capture_has_content(image)) {
            trace("onCapture: capture has no content (empty region) — aborting");
            Logger::get().log("USERINFO",
                "3D reference: that area is empty — frame a sketch and try again.");
            return;
        }
        auto png = encode_capture_png(image);
        if (!png) {
            trace("onCapture: PNG encode failed — aborting");
            Logger::get().log("USERINFO", "3D reference: could not read the captured region.");
            return;
        }
        trace("onCapture: PNG encoded, " + std::to_string(png->size()) + " bytes → start()");
        start(drawP, std::move(*png), region);
    };
    // Opaque WYSIWYG capture (transparentBackground=false): the service mattes an
    // opaque sketch-on-paper image; a transparent capture of bare strokes broke the
    // matte for reangle (MESH_REFERENCE.md §4). center-out drag for nicer framing.
    auto tool = std::make_unique<SquareCanvasCaptureTool>(
        drawP, /*targetSize=*/512, previousToolType, std::move(onCapture),
        /*transparentBackground=*/false, /*centerOut=*/true);
    drawP.switch_to_tool_ptr(std::move(tool));
    trace("begin_capture: capture tool activated, waiting for the drag");
    Logger::get().log("USERINFO",
        "3D reference: drag out a square around your sketch to build an orbitable mesh.");
}

void MeshFlow::start(DrawingProgram& drawP, std::vector<uint8_t> png,
                     const SquareCanvasCaptureTool::CaptureRegion& region) {
    // Settings → Debug field wins; a blank field falls back to the env var, then
    // (for the endpoint) the built-in default. Shared with reangle + the warm toggle.
    trace("start: entered, png " + std::to_string(png.size()) + " bytes");
    std::string key, endpoint;
    if (!ReangleFlow::resolve_config(drawP.world.main.conf, key, endpoint)) {
        trace("start: NOT CONFIGURED — no API key (endpoint='" + endpoint + "')");
        Logger::get().log("USERINFO",
            "AI tools are not configured — set the HVYM Tools API key in Settings \xe2\x86\x92 "
            "Debug (or the HVYM_TOOLS_KEY environment variable).");
        return;
    }
    // NEVER log the key value — only that it is present and its length (security).
    trace("start: config OK — endpoint='" + endpoint + "', key present (len " +
          std::to_string(key.size()) + ")");

    ToolClient::Field image;
    image.name = "image";
    image.filename = "sketch.png";
    image.mimeType = "image/png";
    image.bytes = std::move(png);

    ToolClient::Field faces;
    faces.name = "target_faces";
    faces.textValue = std::to_string(kTargetFaces);

    ToolClient::Field seed;
    seed.name = "seed";
    seed.textValue = std::to_string(kSeed);

    gPendingRegion = region;
    trace("start: POST " + endpoint + "/tools/mesh (target_faces=" +
          std::to_string(kTargetFaces) + ", seed=" + std::to_string(kSeed) + ")");
    // Mesh gets a long transfer budget: a cold start measured ~547 s and the
    // service's own budget is 840 s, so the client must stay above both (>=900 s)
    // for a stuck job to surface as the service's JSON error, not a client timeout
    // (MESH_API.md §3). The warm-both toggle normally keeps this fast.
    gPending = ToolClient::request(endpoint, key, "mesh",
                                   {std::move(image), std::move(faces), std::move(seed)},
                                   /*timeoutSeconds=*/900);
    // request() can fail synchronously (bad config) — surface that now rather than
    // leaving a dead handle for tick() to report.
    if (gPending && gPending->status == ToolClient::Request::Status::FAILURE) {
        trace("start: request FAILED synchronously — " + gPending->error);
        Logger::get().log("USERINFO", gPending->error);
        gPending = nullptr;
        return;
    }
    gWaitTicks = 0;
    trace("start: request dispatched, polling for completion");
    // A warm mesh worker returns in ~5 s (the worker runs a real reconstruction at
    // startup — MeshTool::warmup — so warmth is genuine). A truly cold start is
    // ~547 s, ~98% of it waiting for GPU capacity (MESH_API.md §3). With AI
    // inference toggled on, the mesh endpoint is kept warm, so this is normally fast.
    Logger::get().log("USERINFO",
        "Building a 3D reference — a warm worker takes ~5 s; a cold start can take several minutes.");
}

void MeshFlow::tick(DrawingProgram& drawP) {
    if (!gPending) return;
    const auto status = gPending->status.load();
    if (status == ToolClient::Request::Status::IN_PROGRESS) {
        // ~1 s heartbeat (assumes ~60 fps) so the console shows it is still waiting.
        if (++gWaitTicks % 60 == 0)
            trace("tick: still waiting (" + std::to_string(gWaitTicks / 60) + "s, dl " +
                  std::to_string(static_cast<int>(gPending->progress.load() * 100)) + "%)");
        return;
    }

    auto done = gPending;   // keep the handle alive while we consume it
    gPending = nullptr;
    trace("tick: request completed — status=" +
          std::string(status == ToolClient::Request::Status::SUCCESS ? "SUCCESS" : "FAILURE") +
          ", http=" + std::to_string(done->httpCode) +
          ", glb=" + std::to_string(done->glb.size()) + " bytes" +
          ", cacheHit=" + std::string(done->cacheHit.load() ? "yes" : "no") +
          ", cacheKey='" + done->cacheKey + "'" +
          ", toolVersion='" + done->toolVersion + "'");
    if (status == ToolClient::Request::Status::SUCCESS) {
        trace("tick: SUCCESS → load_reference_mesh_into_canvas");
        const bool placed = ArmatureModalScreen::load_reference_mesh_into_canvas(
            drawP, std::string_view(reinterpret_cast<const char*>(done->glb.data()),
                                    done->glb.size()),
            gPendingRegion.topLeft, gPendingRegion.sideWorld, gPendingRegion.rotation);
        trace(std::string("tick: load_reference_mesh_into_canvas returned ") +
              (placed ? "true (placed)" : "false (NOT placed — see the load error above)"));
        if (placed)
            Logger::get().log("USERINFO", done->cacheHit
                ? "3D reference ready (cached) — orbit it and draw over it on a new layer."
                : "3D reference ready — orbit it and draw over it on a new layer.");
        // On failure the loader already logged the specific reason.
    } else {
        trace("tick: FAILURE — " + (done->error.empty() ? std::string("(no error text)") : done->error));
        Logger::get().log("USERINFO",
            done->error.empty() ? "Building the 3D reference failed." : done->error);
    }
}

}  // namespace AI
