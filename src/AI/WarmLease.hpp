#pragma once
// AI warm-lease client — holds a GPU worker awake so the artist pays the reangle
// cold start once per session, not on every idle gap (REANGLE_API.md §11).
//
// It is a LEASE, not a switch: while enabled, a background thread re-POSTs
// /warm every `renew_within_s` (~20 s); silence releases it server-side within
// ~lease_ttl. So a crash, a sleeping laptop, or a closed lid stops the bill on
// its own — the failure mode is "goes cold" (free), never "bills all weekend".
// Every /warm response carries the current state, so the renewal poll is also
// the status feed (no separate GET needed while a lease is held).
//
// NATIVE ONLY (curl). The WASM build cannot reach the service (no CORS); its
// stub keeps state OFF and does nothing.

#include <atomic>
#include <string>

class GlobalConfig;

namespace AI {

class WarmLease {
public:
    // NOTE: no value named ERROR — that is a wingdi.h macro (#define ERROR 0) and
    // any TU that transitively includes windows.h (e.g. Toolbar.cpp via SDL/Skia)
    // would mangle the enum. Use FAILED.
    enum class State {
        OFF = 0,   // no lease held (toggle off)
        WARMING,   // lease held, worker not ready to serve yet
        WARM,      // a worker can serve now
        FAILED,    // the lease poll is failing (shown, but keeps retrying)
    };

    static void init();     // reset flags (call at startup, before enable)
    static void cleanup();   // release the lease + join the thread (call at shutdown)

    // Begin holding a lease against `baseUrl` with `apiKey`. Idempotent: calling
    // again just refreshes the target. Starts the renewal thread on first use.
    static void enable(const std::string& baseUrl, const std::string& apiKey);
    // Release the lease (DELETE /warm) and stop renewing. State returns to OFF.
    static void disable();

    static bool  is_enabled();      // is a lease currently being held/attempted?
    static State state();           // for the header indicator
    static float elapsed_s();       // seconds spent warming (show while WARMING)
};

}  // namespace AI
