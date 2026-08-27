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

}  // namespace

bool MeshFlow::is_busy() { return gPending != nullptr; }

void MeshFlow::begin_capture(DrawingProgram& drawP) {
    if (is_busy()) {
        Logger::get().log("USERINFO", "A 3D reference is already being built — one at a time.");
        return;
    }
    const auto previousToolType = drawP.drawTool ? drawP.drawTool->get_type()
                                                 : DrawingProgramToolType::MYPAINTBRUSH;
    // The DrawingProgram outlives the tool (owned by World), so capturing it by
    // reference for the capture callback is safe.
    auto onCapture = [&drawP](sk_sp<SkImage> image,
                              const SquareCanvasCaptureTool::CaptureRegion& region) {
        if (!capture_has_content(image)) {
            Logger::get().log("USERINFO",
                "3D reference: that area is empty — frame a sketch and try again.");
            return;
        }
        auto png = encode_capture_png(image);
        if (!png) {
            Logger::get().log("USERINFO", "3D reference: could not read the captured region.");
            return;
        }
        start(drawP, std::move(*png), region);
    };
    // Opaque WYSIWYG capture (transparentBackground=false): the service mattes an
    // opaque sketch-on-paper image; a transparent capture of bare strokes broke the
    // matte for reangle (MESH_REFERENCE.md §4). center-out drag for nicer framing.
    auto tool = std::make_unique<SquareCanvasCaptureTool>(
        drawP, /*targetSize=*/512, previousToolType, std::move(onCapture),
        /*transparentBackground=*/false, /*centerOut=*/true);
    drawP.switch_to_tool_ptr(std::move(tool));
    Logger::get().log("USERINFO",
        "3D reference: drag out a square around your sketch to build an orbitable mesh.");
}

void MeshFlow::start(DrawingProgram& drawP, std::vector<uint8_t> png,
                     const SquareCanvasCaptureTool::CaptureRegion& region) {
    // Settings → Debug field wins; a blank field falls back to the env var, then
    // (for the endpoint) the built-in default. Shared with reangle + the warm toggle.
    std::string key, endpoint;
    if (!ReangleFlow::resolve_config(drawP.world.main.conf, key, endpoint)) {
        Logger::get().log("USERINFO",
            "AI tools are not configured — set the HVYM Tools API key in Settings \xe2\x86\x92 "
            "Debug (or the HVYM_TOOLS_KEY environment variable).");
        return;
    }

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
    gPending = ToolClient::request(endpoint, key, "mesh",
                                   {std::move(image), std::move(faces), std::move(seed)});
    // request() can fail synchronously (bad config) — surface that now rather than
    // leaving a dead handle for tick() to report.
    if (gPending && gPending->status == ToolClient::Request::Status::FAILURE) {
        Logger::get().log("USERINFO", gPending->error);
        gPending = nullptr;
        return;
    }
    // The mesh worker's keepalive doesn't run a kernel, so the first request after
    // idle still pays CUDA init — ~57 s even when the lease reads "warm" (mesh.md
    // §6b). Set expectations accordingly.
    Logger::get().log("USERINFO",
        "Building a 3D reference — the first request after idle can take up to a minute.");
}

void MeshFlow::tick(DrawingProgram& drawP) {
    if (!gPending) return;
    const auto status = gPending->status.load();
    if (status == ToolClient::Request::Status::IN_PROGRESS) return;

    auto done = gPending;   // keep the handle alive while we consume it
    gPending = nullptr;
    if (status == ToolClient::Request::Status::SUCCESS) {
        const bool placed = ArmatureModalScreen::load_reference_mesh_into_canvas(
            drawP, std::string_view(reinterpret_cast<const char*>(done->glb.data()),
                                    done->glb.size()),
            gPendingRegion.topLeft, gPendingRegion.sideWorld, gPendingRegion.rotation);
        if (placed)
            Logger::get().log("USERINFO", done->cacheHit
                ? "3D reference ready (cached) — orbit it and draw over it on a new layer."
                : "3D reference ready — orbit it and draw over it on a new layer.");
        // On failure the loader already logged the specific reason.
    } else {
        Logger::get().log("USERINFO",
            done->error.empty() ? "Building the 3D reference failed." : done->error);
    }
}

}  // namespace AI
