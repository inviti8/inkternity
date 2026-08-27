#include "ReangleClient.hpp"

#include <Helpers/Logger.hpp>

#ifndef __EMSCRIPTEN__

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <curl/multi.h>

namespace AI {
namespace {

// One in-flight easy handle plus everything it borrows for the duration of the
// transfer. curl keeps pointers into `mime`/`headers`/the request buffers, so
// they must outlive the handle — this struct owns them and is freed only after
// curl_multi_remove_handle + curl_easy_cleanup.
struct Handler {
    CURL* easy = nullptr;
    curl_mime* mime = nullptr;
    curl_slist* headers = nullptr;
    std::shared_ptr<ReangleClient::Request> req;
    // Stable backing store for the multipart fields: curl_mime_data with
    // CURL_ZERO_TERMINATED reads lazily, so these must stay put until cleanup.
    std::vector<uint8_t> png;      // the image part (copied so the caller's buffer can go)
    std::string mcResolution;      // decimal string of the mc_resolution field
};

std::mutex gMutex;
std::vector<Handler*> gNew;           // handed off from request(), not yet in the multi
std::vector<Handler*> gCurrent;       // owned by the worker thread
std::atomic<bool> gShutdown{false};
std::unique_ptr<std::thread> gThread;
CURLM* gMulti = nullptr;

// libcurl write callback — binary safe. A .glb contains embedded NULs, so we
// must append by length, never treat the buffer as a C string (REANGLE_API.md §6).
size_t write_to_vector(void* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::vector<uint8_t>*>(userp);
    const size_t n = size * nmemb;
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + n);
    return n;
}

// Capture the headers we branch on: X-Cache (was it a cache hit?) and
// X-Tool-Version (which pipeline built the mesh). Case-insensitive name match.
size_t header_callback(char* buffer, size_t size, size_t nitems, void* userp) {
    auto* req = static_cast<ReangleClient::Request*>(userp);
    const size_t n = size * nitems;
    std::string line(buffer, n);

    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // trim surrounding whitespace + trailing CRLF from the value
        auto notspace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notspace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notspace).base(), value.end());

        if (name == "x-cache") {
            std::string v = value;
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            req->cacheHit = (v == "hit");
        } else if (name == "x-tool-version") {
            req->toolVersion = value;
        }
    }
    return n;
}

// best-effort progress. The server sends nothing until it sends everything
// (REANGLE_API.md §2), so this is only meaningful during the final download and
// the UI must treat it as indeterminate — but we still surface it.
int xfer_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* req = static_cast<ReangleClient::Request*>(clientp);
    req->progress = (dltotal > 0) ? static_cast<float>(dlnow) / static_cast<float>(dltotal) : 0.0f;
    return 0;
}

// Map a human-readable failure for a completed-but-non-200 transfer. The service
// returns JSON errors ({"detail": "..."}) with the real code (REANGLE_API.md §3);
// we translate to something an artist understands and keep the body for the log.
std::string message_for_http(long code, const std::vector<uint8_t>& body) {
    switch (code) {
        case 401: return "Reangle service rejected the API key (check configuration).";
        case 413: return "Drawing is too large to send — reduce the selection size.";
        case 422: return "Reangle request was malformed (internal bug).";
        case 500: return "The reangle pipeline failed on this drawing — try a clearer, "
                         "front-facing pose.";
        case 502:
        case 504: return "The reangle service is busy or warming up — try again in a moment.";
        case 503: return "The reangle service is misconfigured — try again later.";
        default:  break;
    }
    std::string detail(body.begin(), body.end());
    if (detail.size() > 300) detail.resize(300);
    return "Reangle service error (HTTP " + std::to_string(code) + "): " + detail;
}

// The .glb ASCII magic. Validating before load keeps a JSON error body (on an
// unexpected 200-with-wrong-body path) out of the mesh loader (REANGLE_API.md §6).
bool looks_like_glb(const std::vector<uint8_t>& b) {
    return b.size() >= 12 && std::memcmp(b.data(), "glTF", 4) == 0;
}

void finish(Handler* h, CURLcode result) {
    ReangleClient::Request& req = *h->req;
    if (result == CURLE_OK) {
        curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &req.httpCode);
        if (req.httpCode == 200 && looks_like_glb(req.glb)) {
            req.status = ReangleClient::Request::Status::SUCCESS;
        } else if (req.httpCode == 200) {
            req.error = "Reangle service returned a non-model response.";
            req.glb.clear();
            req.status = ReangleClient::Request::Status::FAILURE;
        } else {
            req.error = message_for_http(req.httpCode, req.glb);
            // Worker thread: use cross_platform_println (plain stdout), NOT
            // Logger::log — the latter runs a UI callback under its lock on THIS
            // thread and throws on an unknown category. The user-facing message
            // is surfaced on the main thread by ReangleFlow::tick via req.error.
            Logger::get().cross_platform_println(
                "[reangle] HTTP " + std::to_string(req.httpCode) + ": " +
                std::string(req.glb.begin(), req.glb.end()).substr(0, 300));
            req.glb.clear();
            req.status = ReangleClient::Request::Status::FAILURE;
        }
    } else {
        // Transport failure (timeout, DNS, TLS, connection reset). A retry after a
        // timeout usually collects a finished, cached result (REANGLE_API.md §4).
        req.error = std::string("Reangle request failed: ") + curl_easy_strerror(result);
        req.glb.clear();
        req.status = ReangleClient::Request::Status::FAILURE;
    }
}

void free_handler(Handler* h) {
    if (h->easy) curl_easy_cleanup(h->easy);
    if (h->mime) curl_mime_free(h->mime);
    if (h->headers) curl_slist_free_all(h->headers);
    delete h;
}

// Move handles queued by request() into the multi (worker thread only).
void add_new_handlers_to_multi() {
    std::scoped_lock lock(gMutex);
    for (Handler* h : gNew) {
        curl_multi_add_handle(gMulti, h->easy);
        gCurrent.push_back(h);
    }
    gNew.clear();
}

// The background transfer loop. Started lazily on the first request(), lives
// until cleanup() sets gShutdown and joins.
void worker_update() {
    gMulti = curl_multi_init();
    int stillRunning = 0;

    for (;;) {
        if (gShutdown) break;

        add_new_handlers_to_multi();
        CURLMcode mc = curl_multi_perform(gMulti, &stillRunning);

        for (;;) {
            int msgCount = 0;
            CURLMsg* msg = curl_multi_info_read(gMulti, &msgCount);
            if (!msg) break;
            if (msg->msg != CURLMSG_DONE) continue;
            auto it = std::find_if(gCurrent.begin(), gCurrent.end(),
                                   [&](Handler* h) { return h->easy == msg->easy_handle; });
            if (it != gCurrent.end()) {
                Handler* h = *it;
                finish(h, msg->data.result);
                curl_multi_remove_handle(gMulti, h->easy);
                gCurrent.erase(it);
                free_handler(h);
            }
        }

        if (mc != CURLM_OK) {
            Logger::get().cross_platform_println("[reangle] curl_multi failed; worker stopping.");
            break;
        }
        int numfds = 0;
        curl_multi_poll(gMulti, nullptr, 0, 1000, &numfds);
    }

    // Fail anything still in flight so no handle waits forever on shutdown.
    for (Handler* h : gCurrent) {
        h->req->error = "Reangle cancelled (application closing).";
        h->req->status = ReangleClient::Request::Status::FAILURE;
        curl_multi_remove_handle(gMulti, h->easy);
        free_handler(h);
    }
    gCurrent.clear();
    curl_multi_cleanup(gMulti);
    gMulti = nullptr;
}

}  // namespace

void ReangleClient::init() {
    // curl_global_init is owned by FileDownloader::init (called alongside this in
    // main.cpp); we only reset our own shutdown flag.
    gShutdown = false;
}

std::shared_ptr<ReangleClient::Request> ReangleClient::request(
        const std::vector<uint8_t>& png, const std::string& baseUrl,
        const std::string& apiKey, int mcResolution) {
    auto req = std::make_shared<Request>();

    if (baseUrl.empty() || apiKey.empty()) {
        req->error = "Reangle is not configured (set the endpoint URL and API key).";
        req->status = Request::Status::FAILURE;
        return req;
    }
    if (png.empty()) {
        req->error = "Nothing to reangle (empty image).";
        req->status = Request::Status::FAILURE;
        return req;
    }

    auto* h = new Handler();
    h->req = req;
    h->png = png;                                             // own the bytes
    h->mcResolution = std::to_string(std::clamp(mcResolution, 64, 512));

    h->easy = curl_easy_init();
    if (!h->easy) {
        free_handler(h);
        req->error = "Reangle request could not be created (curl init failed).";
        req->status = Request::Status::FAILURE;
        return req;
    }

    std::string url = baseUrl;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/tools/reangle";
    curl_easy_setopt(h->easy, CURLOPT_URL, url.c_str());

    // --- auth --------------------------------------------------------------
    h->headers = curl_slist_append(h->headers, ("X-API-Key: " + apiKey).c_str());
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);

    // --- multipart body ----------------------------------------------------
    h->mime = curl_mime_init(h->easy);
    curl_mimepart* part = curl_mime_addpart(h->mime);
    curl_mime_name(part, "image");
    curl_mime_filename(part, "drawing.png");
    curl_mime_type(part, "image/png");
    curl_mime_data(part, reinterpret_cast<const char*>(h->png.data()), h->png.size());

    part = curl_mime_addpart(h->mime);
    curl_mime_name(part, "mc_resolution");
    curl_mime_data(part, h->mcResolution.c_str(), CURL_ZERO_TERMINATED);
    curl_easy_setopt(h->easy, CURLOPT_MIMEPOST, h->mime);

    // --- timeouts (REANGLE_API.md §2) --------------------------------------
    // A cold serverless worker can take minutes to first byte; a "reasonable"
    // default aborts mid-cold-start while the job keeps running server-side.
    curl_easy_setopt(h->easy, CURLOPT_TIMEOUT, 300L);          // whole transfer
    curl_easy_setopt(h->easy, CURLOPT_CONNECTTIMEOUT, 15L);    // TCP+TLS only
    // During a cold start literally nothing transfers for minutes — the exact
    // condition the low-speed abort exists to kill. Disable it here.
    curl_easy_setopt(h->easy, CURLOPT_LOW_SPEED_LIMIT, 0L);
    curl_easy_setopt(h->easy, CURLOPT_LOW_SPEED_TIME, 0L);
    curl_easy_setopt(h->easy, CURLOPT_NOSIGNAL, 1L);
    // No CURLOPT_FOLLOWLOCATION: this is a stable HTTPS POST endpoint. An
    // unexpected 3xx would otherwise silently downgrade the multipart POST to a
    // GET; we'd rather it surface as a non-200 the caller reports.
    curl_easy_setopt(h->easy, CURLOPT_USERAGENT, "Inkternity-Reangle/1.0");

    // --- TLS: verification stays ON ---------------------------------------
    // The API key rides in a request header; without certificate verification it
    // is exposed to anyone able to intercept the connection (REANGLE_API.md §5).
    // (FileDownloader disables verification for public asset downloads — we must
    // NOT copy that here.)
    curl_easy_setopt(h->easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h->easy, CURLOPT_SSL_VERIFYHOST, 2L);
    // This build's libcurl uses the OpenSSL backend, which on Windows ships with
    // NO default CA store — so verifying a perfectly valid cert fails with
    // CURLE_PEER_FAILED_VERIFICATION ("SSL peer certificate ... was not OK").
    // NATIVE_CA makes it read the OS trust store (Windows cert store), keeping
    // verification ON. No-op where the backend already uses the system store.
    curl_easy_setopt(h->easy, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);

    // --- binary-safe collection + header capture ---------------------------
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, write_to_vector);
    curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, &req->glb);
    curl_easy_setopt(h->easy, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(h->easy, CURLOPT_HEADERDATA, req.get());
    curl_easy_setopt(h->easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h->easy, CURLOPT_XFERINFOFUNCTION, xfer_callback);
    curl_easy_setopt(h->easy, CURLOPT_XFERINFODATA, req.get());

    {
        std::scoped_lock lock(gMutex);
        gNew.push_back(h);
    }
    if (!gThread)
        gThread = std::make_unique<std::thread>(worker_update);

    return req;
}

void ReangleClient::cleanup() {
    if (gThread && gThread->joinable()) {
        gShutdown = true;
        gThread->join();
    }
    gThread = nullptr;
    // Drain any handlers queued but never picked up by the (now-stopped) worker.
    std::scoped_lock lock(gMutex);
    for (Handler* h : gNew) {
        h->req->error = "Reangle cancelled (application closing).";
        h->req->status = Request::Status::FAILURE;
        free_handler(h);
    }
    gNew.clear();
}

}  // namespace AI

#else  // __EMSCRIPTEN__

// The WASM build cannot reach the service (no CORS — REANGLE_API.md §9). Provide
// the symbols so shared UI code links, but fail every request immediately.
namespace AI {

void ReangleClient::init() {}
void ReangleClient::cleanup() {}

std::shared_ptr<ReangleClient::Request> ReangleClient::request(
        const std::vector<uint8_t>&, const std::string&, const std::string&, int) {
    auto req = std::make_shared<Request>();
    req->error = "Reangle is not available in the web version.";
    req->status = Request::Status::FAILURE;
    return req;
}

}  // namespace AI

#endif
