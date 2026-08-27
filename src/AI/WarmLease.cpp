#include "WarmLease.hpp"

#include <Helpers/Logger.hpp>

#ifndef __EMSCRIPTEN__

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace AI {
namespace {

std::atomic<bool>  gEnabled{false};
std::atomic<bool>  gShutdown{false};
std::atomic<WarmLease::State> gState{WarmLease::State::OFF};
std::atomic<float> gElapsed{0.0f};

std::mutex gMutex;                       // guards gUrl/gKey/gLeaseId + CV
std::condition_variable gCv;
std::string gUrl, gKey, gLeaseId;
std::unique_ptr<std::thread> gThread;

size_t collect(void* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<const char*>(data), size * nmemb);
    return size * nmemb;
}

// Apply the shared TLS/timeout policy (matches ReangleClient): verification ON,
// backed by the Windows system cert store (OpenSSL backend has no default CA
// store); short timeouts — /warm is a quick JSON call, never a cold-start wait.
void apply_common(CURL* c, curl_slist* headers) {
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
}

std::string warm_url(const std::string& baseUrl) {
    std::string u = baseUrl;
    if (!u.empty() && u.back() == '/') u.pop_back();
    return u + "/warm";
}

struct WarmResult {
    bool ok = false;
    std::string leaseId;
    bool ready = false;
    double elapsed = 0.0;
    double renewWithin = 20.0;
};

// POST /warm — acquire (empty leaseId) or extend. Returns ok=false on any
// transport/HTTP/parse failure; the caller maps that to State::FAILED + a retry.
WarmResult post_warm(const std::string& baseUrl, const std::string& key,
                     const std::string& leaseId) {
    WarmResult r;
    CURL* c = curl_easy_init();
    if (!c) return r;

    const std::string url = warm_url(baseUrl);
    const std::string body = leaseId.empty()
        ? std::string("{}")
        : std::string("{\"lease_id\":\"") + leaseId + "\"}";
    std::string resp;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-API-Key: " + key).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    apply_common(c, headers);

    const CURLcode code = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (code != CURLE_OK) {
        Logger::get().cross_platform_println(std::string("[warm] request failed: ") +
                                             curl_easy_strerror(code));
        return r;
    }
    if (http != 200) {
        Logger::get().cross_platform_println("[warm] HTTP " + std::to_string(http) +
                                             ": " + resp.substr(0, 200));
        return r;
    }
    auto j = nlohmann::json::parse(resp, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return r;

    r.leaseId     = j.value("lease_id", leaseId);
    r.ready       = j.value("ready", false);
    r.elapsed     = j.value("elapsed_s", 0.0);
    r.renewWithin = j.value("renew_within_s", 20.0);
    r.ok = true;
    return r;
}

void delete_warm(const std::string& baseUrl, const std::string& key,
                 const std::string& leaseId) {
    if (leaseId.empty()) return;
    CURL* c = curl_easy_init();
    if (!c) return;
    const std::string url = warm_url(baseUrl);
    const std::string body = std::string("{\"lease_id\":\"") + leaseId + "\"}";
    std::string resp;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-API-Key: " + key).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    apply_common(c, headers);
    curl_easy_perform(c);   // best-effort; the lease also lapses on its own
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
}

// The renewal loop. Idle-waits (interruptibly) when disabled; while enabled,
// POSTs, updates the atomics the UI reads, then waits ~renew_within_s.
void worker() {
    for (;;) {
        std::string url, key, leaseId;
        {
            std::unique_lock<std::mutex> lk(gMutex);
            gCv.wait(lk, [] { return gShutdown.load() || gEnabled.load(); });
            if (gShutdown) break;
            url = gUrl; key = gKey; leaseId = gLeaseId;
        }

        const WarmResult r = post_warm(url, key, leaseId);
        double renew = 20.0;
        if (r.ok) {
            { std::lock_guard<std::mutex> lk(gMutex); gLeaseId = r.leaseId; }
            gState = r.ready ? WarmLease::State::WARM : WarmLease::State::WARMING;
            gElapsed = static_cast<float>(r.elapsed);
            renew = (r.renewWithin > 1.0) ? r.renewWithin : 20.0;
        } else {
            gState = WarmLease::State::FAILED;
            renew = 5.0;   // retry sooner after a failure
        }

        // Wait until the next renewal, waking early on disable/shutdown.
        bool releasing;
        {
            std::unique_lock<std::mutex> lk(gMutex);
            gCv.wait_for(lk, std::chrono::duration<double>(renew),
                         [] { return gShutdown.load() || !gEnabled.load(); });
            releasing = gShutdown.load() || !gEnabled.load();
            if (releasing) { url = gUrl; key = gKey; leaseId = gLeaseId; }
        }
        if (releasing) {
            delete_warm(url, key, leaseId);
            { std::lock_guard<std::mutex> lk(gMutex); gLeaseId.clear(); }
            gState = WarmLease::State::OFF;
            gElapsed = 0.0f;
            if (gShutdown) break;
            // else: loop back to the idle wait until re-enabled
        }
    }
}

}  // namespace

void WarmLease::init() { gShutdown = false; }

void WarmLease::enable(const std::string& baseUrl, const std::string& apiKey) {
    {
        std::lock_guard<std::mutex> lk(gMutex);
        gUrl = baseUrl;
        gKey = apiKey;
        gEnabled = true;
        if (gState == State::OFF) gState = State::WARMING;
        if (!gThread)
            gThread = std::make_unique<std::thread>(worker);
    }
    gCv.notify_all();
}

void WarmLease::disable() {
    { std::lock_guard<std::mutex> lk(gMutex); gEnabled = false; }
    gCv.notify_all();
}

void WarmLease::cleanup() {
    { std::lock_guard<std::mutex> lk(gMutex); gShutdown = true; gEnabled = false; }
    gCv.notify_all();
    if (gThread && gThread->joinable()) gThread->join();
    gThread = nullptr;
}

bool WarmLease::is_enabled() { return gEnabled.load(); }
WarmLease::State WarmLease::state() { return gState.load(); }
float WarmLease::elapsed_s() { return gElapsed.load(); }

}  // namespace AI

#else  // __EMSCRIPTEN__

namespace AI {
void WarmLease::init() {}
void WarmLease::cleanup() {}
void WarmLease::enable(const std::string&, const std::string&) {}
void WarmLease::disable() {}
bool WarmLease::is_enabled() { return false; }
WarmLease::State WarmLease::state() { return State::OFF; }
float WarmLease::elapsed_s() { return 0.0f; }
}  // namespace AI

#endif
