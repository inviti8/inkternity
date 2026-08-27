#include "ReangleFlow.hpp"

#include "ReangleClient.hpp"
#include "../DrawingProgram/DrawingProgram.hpp"
#include "../DrawingProgram/Tools/SquareCanvasCaptureTool.hpp"
#include "../Armature/ArmatureModalScreen.hpp"

#include <Helpers/Logger.hpp>

#include <include/core/SkImage.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/encode/SkPngEncoder.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace AI {
namespace {

// The single in-flight reangle (null when idle). Only touched on the UI thread
// (begin_capture/start/tick all run there), so no locking needed here — the
// atomics live inside ReangleClient::Request for the worker-thread handoff.
std::shared_ptr<ReangleClient::Request> gPending;

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// Encode a captured square (premultiplied RGBA, transparent background) to PNG —
// the SquareCanvasCaptureTool / BrushCustomizationDrawer icon path.
std::optional<std::vector<uint8_t>> encode_png(const sk_sp<SkImage>& image) {
    if (!image) return std::nullopt;
    sk_sp<SkImage> raster = image->makeRasterImage(nullptr);
    SkPixmap pix;
    if (!raster || !raster->peekPixels(&pix)) return std::nullopt;
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, pix, {})) return std::nullopt;
    auto data = stream.detachAsData();
    if (!data) return std::nullopt;
    std::vector<uint8_t> bytes(data->size());
    std::memcpy(bytes.data(), data->bytes(), data->size());
    return bytes;
}

}  // namespace

bool ReangleFlow::is_busy() { return gPending != nullptr; }

void ReangleFlow::begin_capture(DrawingProgram& drawP) {
    if (is_busy()) {
        Logger::get().log("USERINFO", "A reangle is already in progress — one at a time.");
        return;
    }
    const auto previousToolType = drawP.drawTool ? drawP.drawTool->get_type()
                                                 : DrawingProgramToolType::MYPAINTBRUSH;
    // The DrawingProgram outlives the tool (owned by World), so capturing it by
    // reference for the capture callback is safe.
    auto onCapture = [&drawP](sk_sp<SkImage> image) {
        auto png = encode_png(image);
        if (!png) {
            Logger::get().log("USERINFO", "Reangle: could not read the captured region.");
            return;
        }
        start(drawP, std::move(*png));
    };
    // 512² is the validated input size (REANGLE_API.md §8); the service mattes +
    // normalizes internally, so a larger capture would only cost upload time.
    auto tool = std::make_unique<SquareCanvasCaptureTool>(
        drawP, /*targetSize=*/512, previousToolType, std::move(onCapture));
    drawP.switch_to_tool_ptr(std::move(tool));
    Logger::get().log("USERINFO", "Reangle: drag a square around the character to send it.");
}

void ReangleFlow::start(DrawingProgram& drawP, std::vector<uint8_t> png) {
    (void)drawP;  // completion is routed through tick(), which owns the drawP ref
    const std::string endpoint = env_or("HVYM_TOOLS_ENDPOINT", "https://img.hvym.link");
    const char* key = std::getenv("HVYM_TOOLS_KEY");
    if (!key || !*key) {
        Logger::get().log("USERINFO",
            "Reangle is not configured — set the HVYM_TOOLS_KEY environment variable.");
        return;
    }
    gPending = ReangleClient::request(png, endpoint, key, 256);
    // request() can fail synchronously (bad config / empty image) — surface that
    // now rather than leaving a dead handle for tick() to report.
    if (gPending && gPending->status == ReangleClient::Request::Status::FAILURE) {
        Logger::get().log("USERINFO", gPending->error);
        gPending = nullptr;
        return;
    }
    Logger::get().log("USERINFO",
        "Building a 3D proxy — the first request after idle can take a few minutes.");
}

void ReangleFlow::tick(DrawingProgram& drawP) {
    if (!gPending) return;
    const auto status = gPending->status.load();
    if (status == ReangleClient::Request::Status::IN_PROGRESS) return;

    auto done = gPending;   // keep the handle alive while we consume it
    gPending = nullptr;
    if (status == ReangleClient::Request::Status::SUCCESS) {
        const bool placed = ArmatureModalScreen::load_reangle_mesh_into_canvas(
            drawP, std::string_view(reinterpret_cast<const char*>(done->glb.data()),
                                    done->glb.size()));
        if (placed)
            Logger::get().log("USERINFO", done->cacheHit
                ? "Reangle ready (cached) — double-click the model to orbit and bake."
                : "Reangle ready — double-click the model to orbit and bake.");
        // On failure the loader already logged the specific reason.
    } else {
        Logger::get().log("USERINFO",
            done->error.empty() ? "Reangle failed." : done->error);
    }
}

}  // namespace AI
