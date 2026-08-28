#include "WarmLease.hpp"

#include <Helpers/Logger.hpp>

#ifndef __EMSCRIPTEN__

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace AI {
namespace {

using Clock = std::chrono::steady_clock;

// One held lease. Guarded by gMutex (UI reads state/elapsed under the same lock;
// those calls are per-frame and cheap). No atomics — the map would not be movable.
struct Lease {
    std::string        leaseId;
    WarmLease::State   state = WarmLease::State::WARMING;
    float              elapsed = 0.0f;
    Clock::time_point  nextRenew{};        // default (epoch) → due immediately on acquire
    bool               releaseRequested = false;
};

std::atomic<bool> gShutdown{false};

std::mutex gMutex;                          // guards gUrl/gKey/gLeases + CV predicate
std::condition_variable gCv;
std::string gUrl, gKey;                     // shared base + key (same for both tools)
std::map<std::string, Lease> gLeases;       // key = tool name ("reangle" | "mesh")
std::unique_ptr<std::thread> gThread;

const char* kLabel = "inkternity";          // shows in the service's lease metering

size_t collect(void* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<const char*>(data), size * nmemb);
    return size * nmemb;
}

// Shared TLS/timeout policy (matches ReangleClient): verification ON, backed by the
// Windows system cert store (OpenSSL backend has no default CA store). The /warm
// call is a fast JSON round-trip, so a SHORT timeout — a 30 s (cold-start-sized)
// budget it never needs would let two failures exceed the 60 s TTL and drop the
// lease (WARM_LEASE_FIX_PROMPT.md §5).
void apply_common(CURL* c, curl_slist* headers) {
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
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
    bool        ok = false;
    std::string leaseId;
    bool        ready = false;
    double      elapsed = 0.0;
    double      ttl = 60.0;          // lease_ttl_s from the response
    double      renewWithin = 20.0;  // renew_within_s from the response
};

// Build a /warm body. `tool` is ALWAYS included — omitting it warms reangle by
// default (the bug). `lease_id` only on renew/release.
std::string warm_body(const std::string& tool, const std::string& leaseId) {
    std::string b = "{\"tool\":\"" + tool + "\",\"label\":\"" + kLabel + "\"";
    if (!leaseId.empty()) b += ",\"lease_id\":\"" + leaseId + "\"";
    b += "}";
    return b;
}

// POST /warm for `tool` — acquire (empty leaseId) or extend. ok=false on any
// transport/HTTP/parse failure; the caller maps that to State::FAILED + a retry.
WarmResult post_warm(const std::string& baseUrl, const std::string& key,
                     const std::string& tool, const std::string& leaseId) {
    WarmResult r;
    CURL* c = curl_easy_init();
    if (!c) return r;

    const std::string url = warm_url(baseUrl);
    const std::string body = warm_body(tool, leaseId);
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
        Logger::get().cross_platform_println(std::string("[warm:") + tool +
            "] request failed: " + curl_easy_strerror(code));
        return r;
    }
    if (http != 200) {
        Logger::get().cross_platform_println("[warm:" + tool + "] HTTP " +
            std::to_string(http) + ": " + resp.substr(0, 200));
        return r;
    }
    auto j = nlohmann::json::parse(resp, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return r;

    r.leaseId     = j.value("lease_id", leaseId);
    r.ready       = j.value("ready", false);
    r.elapsed     = j.value("elapsed_s", 0.0);
    r.ttl         = j.value("lease_ttl_s", 60.0);
    r.renewWithin = j.value("renew_within_s", 20.0);
    r.ok = true;
    return r;
}

void delete_warm(const std::string& baseUrl, const std::string& key,
                 const std::string& tool, const std::string& leaseId) {
    if (leaseId.empty()) return;
    CURL* c = curl_easy_init();
    if (!c) return;
    const std::string url = warm_url(baseUrl);
    const std::string body = warm_body(tool, leaseId);   // include tool — releasing
    std::string resp;                                    // the wrong tool leaks the lease
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

// The renewal loop. One thread renews every held lease before its TTL. Wakes on a
// 1 s tick (cheap — at most two leases), on enable/disable (CV), and on shutdown.
void worker() {
    for (;;) {
        std::vector<std::pair<std::string, std::string>> toRenew;    // (tool, leaseId)
        std::vector<std::pair<std::string, std::string>> toRelease;  // (tool, leaseId)
        std::string url, key;
        bool shuttingDown = false;
        {
            std::unique_lock<std::mutex> lk(gMutex);
            gCv.wait_for(lk, std::chrono::seconds(1), [] {
                if (gShutdown.load()) return true;
                const auto now = Clock::now();
                for (auto& [t, L] : gLeases)
                    if (L.releaseRequested || L.nextRenew <= now) return true;
                return false;
            });
            url = gUrl; key = gKey;
            shuttingDown = gShutdown.load();
            const auto now = Clock::now();
            for (auto& [t, L] : gLeases) {
                if (shuttingDown || L.releaseRequested)
                    toRelease.emplace_back(t, L.leaseId);
                else if (L.nextRenew <= now)
                    toRenew.emplace_back(t, L.leaseId);
            }
        }

        // Network OUTSIDE the lock.
        for (auto& [t, lid] : toRelease) delete_warm(url, key, t, lid);
        std::vector<std::pair<std::string, WarmResult>> results;
        results.reserve(toRenew.size());
        for (auto& [t, lid] : toRenew) results.emplace_back(t, post_warm(url, key, t, lid));

        {
            std::lock_guard<std::mutex> lk(gMutex);
            for (auto& [t, lid] : toRelease) gLeases.erase(t);
            const auto now = Clock::now();
            for (auto& [t, r] : results) {
                auto it = gLeases.find(t);
                if (it == gLeases.end() || it->second.releaseRequested) continue;
                Lease& L = it->second;
                if (r.ok) {
                    L.leaseId = r.leaseId;
                    L.state = r.ready ? WarmLease::State::WARM : WarmLease::State::WARMING;
                    L.elapsed = static_cast<float>(r.elapsed);
                    // Renew by (ttl - renew_within), not "sleep renew_within" — the
                    // service means "renew when within renew_within of expiry"
                    // (WARM_LEASE_FIX_PROMPT.md §4). Clamp to a sane floor.
                    double interval = r.ttl - r.renewWithin;
                    if (interval < 5.0) interval = 5.0;
                    L.nextRenew = now + std::chrono::duration_cast<Clock::duration>(
                                            std::chrono::duration<double>(interval));
                } else {
                    L.state = WarmLease::State::FAILED;
                    L.nextRenew = now + std::chrono::seconds(3);   // retry soon, stay under TTL
                }
            }
        }

        if (shuttingDown) break;
    }
}

}  // namespace

void WarmLease::init() { gShutdown = false; }

void WarmLease::enable(const std::string& baseUrl, const std::string& apiKey,
                       const std::string& tool) {
    {
        std::lock_guard<std::mutex> lk(gMutex);
        gUrl = baseUrl;
        gKey = apiKey;
        auto it = gLeases.find(tool);
        if (it == gLeases.end()) {
            Lease L;
            L.state = State::WARMING;
            L.nextRenew = Clock::now();   // acquire on the next tick
            gLeases.emplace(tool, std::move(L));
        } else {
            it->second.releaseRequested = false;   // re-enable a lease mid-release
        }
        if (!gThread)
            gThread = std::make_unique<std::thread>(worker);
    }
    gCv.notify_all();
}

void WarmLease::disable(const std::string& tool) {
    {
        std::lock_guard<std::mutex> lk(gMutex);
        auto it = gLeases.find(tool);
        if (it != gLeases.end()) it->second.releaseRequested = true;
    }
    gCv.notify_all();
}

void WarmLease::disable_all() {
    {
        std::lock_guard<std::mutex> lk(gMutex);
        for (auto& [t, L] : gLeases) L.releaseRequested = true;
    }
    gCv.notify_all();
}

void WarmLease::cleanup() {
    { std::lock_guard<std::mutex> lk(gMutex); gShutdown = true; }
    gCv.notify_all();
    if (gThread && gThread->joinable()) gThread->join();
    gThread = nullptr;
    std::lock_guard<std::mutex> lk(gMutex);
    gLeases.clear();
}

bool WarmLease::is_enabled(const std::string& tool) {
    std::lock_guard<std::mutex> lk(gMutex);
    auto it = gLeases.find(tool);
    return it != gLeases.end() && !it->second.releaseRequested;
}

WarmLease::State WarmLease::state(const std::string& tool) {
    std::lock_guard<std::mutex> lk(gMutex);
    auto it = gLeases.find(tool);
    if (it == gLeases.end() || it->second.releaseRequested) return State::OFF;
    return it->second.state;
}

float WarmLease::elapsed_s(const std::string& tool) {
    std::lock_guard<std::mutex> lk(gMutex);
    auto it = gLeases.find(tool);
    return (it == gLeases.end()) ? 0.0f : it->second.elapsed;
}

WarmLease::State WarmLease::combined_state() {
    std::lock_guard<std::mutex> lk(gMutex);
    bool any = false, anyFailed = false, allWarm = true;
    for (auto& [t, L] : gLeases) {
        if (L.releaseRequested) continue;
        any = true;
        if (L.state == State::FAILED) anyFailed = true;
        if (L.state != State::WARM) allWarm = false;
    }
    if (!any) return State::OFF;
    if (anyFailed) return State::FAILED;
    return allWarm ? State::WARM : State::WARMING;
}

bool WarmLease::any_enabled() {
    std::lock_guard<std::mutex> lk(gMutex);
    for (auto& [t, L] : gLeases)
        if (!L.releaseRequested) return true;
    return false;
}

}  // namespace AI

#else  // __EMSCRIPTEN__

namespace AI {
void WarmLease::init() {}
void WarmLease::cleanup() {}
void WarmLease::enable(const std::string&, const std::string&, const std::string&) {}
void WarmLease::disable(const std::string&) {}
void WarmLease::disable_all() {}
bool WarmLease::is_enabled(const std::string&) { return false; }
WarmLease::State WarmLease::state(const std::string&) { return State::OFF; }
float WarmLease::elapsed_s(const std::string&) { return 0.0f; }
WarmLease::State WarmLease::combined_state() { return State::OFF; }
bool WarmLease::any_enabled() { return false; }
}  // namespace AI

#endif
