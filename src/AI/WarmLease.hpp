#pragma once
// AI warm-lease client — holds GPU workers awake so the artist pays a tool's cold
// start once per session, not on every idle gap (REANGLE_API.md §11, MESH_API.md §4).
//
// PER-TOOL. reangle and mesh run on SEPARATE RunPod endpoints, and the service
// defaults a missing `tool` to "reangle" — so one lease cannot keep both awake.
// We hold a lease PER TOOL (keyed by tool name), each with its own lease_id, state
// and renewal timer, all renewed by one background thread. Sending the wrong tool
// (or none) warms the wrong endpoint while the UI reads "ready" — the exact bug
// WARM_LEASE_FIX_PROMPT.md describes.
//
// It is a LEASE, not a switch: while enabled, the thread re-POSTs /warm before the
// lease's TTL; silence releases it server-side. So a crash, a sleeping laptop, or a
// closed lid stops the bill on its own — the failure mode is "goes cold" (free),
// never "bills all weekend".
//
// NATIVE ONLY (curl). The WASM build cannot reach the service (no CORS); its stub
// keeps every state OFF and does nothing.

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
        OFF = 0,   // no lease held for this tool
        WARMING,   // lease held, worker not ready to serve yet
        WARM,      // a worker can serve this tool now
        FAILED,    // the lease poll is failing (shown, but keeps retrying)
    };

    static void init();      // reset flags (call at startup, before enable)
    static void cleanup();   // release all leases + join the thread (call at shutdown)

    // Begin holding a lease for `tool` ("reangle" | "mesh") against `baseUrl` with
    // `apiKey`. Idempotent per tool. Starts the renewal thread on first use. The
    // `tool` is sent in every /warm body — omitting it would warm the wrong endpoint.
    static void enable(const std::string& baseUrl, const std::string& apiKey,
                       const std::string& tool);
    // Release one tool's lease (DELETE /warm with its tool). Its state returns to OFF.
    static void disable(const std::string& tool);
    // Release every held lease (the top-bar toggle's "off").
    static void disable_all();

    // Per-tool status — gate each tool's button on ITS OWN tool.
    static bool  is_enabled(const std::string& tool);  // lease held/attempted for this tool?
    static State state(const std::string& tool);       // for gating + per-tool indicators
    static float elapsed_s(const std::string& tool);   // seconds spent warming this tool

    // Aggregate across all held leases, for the single top-bar indicator: OFF if
    // none held; FAILED if any is failing; WARM only when ALL held leases are warm;
    // WARMING otherwise. (The "warm both → ready when both ready" case.)
    static State combined_state();
    static bool  any_enabled();
};

}  // namespace AI
