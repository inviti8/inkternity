#include "ReangleClient.hpp"

#include <algorithm>

// Thin wrapper: reangle is one specific call into the shared ToolClient core.
// Everything hard (curl_multi worker, TLS NATIVE_CA, 300s timeout, glTF check,
// header capture) lives in ToolClient — see MESH_REFERENCE.md §3.

namespace AI {

void ReangleClient::init() { ToolClient::init(); }
void ReangleClient::cleanup() { ToolClient::cleanup(); }

std::shared_ptr<ReangleClient::Request> ReangleClient::request(
        const std::vector<uint8_t>& png, const std::string& baseUrl,
        const std::string& apiKey, int mcResolution) {
    // Keep the reangle-specific empty-image guard here for a precise message;
    // ToolClient guards the config (URL/key) and empty-fields cases.
    if (png.empty()) {
        auto req = std::make_shared<Request>();
        req->error = "Nothing to reangle (empty image).";
        req->status = Request::Status::FAILURE;
        return req;
    }

    ToolClient::Field image;
    image.name = "image";
    image.filename = "drawing.png";
    image.mimeType = "image/png";
    image.bytes = png;   // ToolClient owns the copy for the transfer's lifetime

    ToolClient::Field mc;
    mc.name = "mc_resolution";
    mc.textValue = std::to_string(std::clamp(mcResolution, 64, 512));

    return ToolClient::request(baseUrl, apiKey, "reangle",
                               {std::move(image), std::move(mc)});
}

}  // namespace AI
