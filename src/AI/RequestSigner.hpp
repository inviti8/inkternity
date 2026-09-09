#pragma once
// AI request signing — AI_BILLING_INTEGRATION.md Phase 1 (client).
//
// Attaches a per-install IDENTITY signature to every /warm and /tools/* call
// so the proxy attributes warm-time and inference to a wallet pubkey instead
// of the shared HVYM_TOOLS_KEY (the free-ride vector, §6). The identity is the
// existing DevKeys ed25519 wallet key — no portal, no new credential (§2.10).
//
// ADDITIVE by design: X-API-Key still rides alongside these headers until the
// proxy verifies signatures. That is the coordinated cutover (§7 caveat) — do
// NOT drop X-API-Key client-side until the proxy accepts the signed auth, or
// the client locks itself out. Unknown headers are ignored by the current
// proxy, so shipping this ahead of proxy support is safe.
//
// WIRE CONTRACT (the proxy mirrors this to verify) — headers per request:
//     X-Ink-Pubkey: <G... Stellar strkey identity>
//     X-Ink-Auth:   <base64url(64-byte ed25519 sig)> "." <base64url(payload)>
//   where `payload` is the EXACT signed bytes: compact, sorted-key JSON with
//   no spaces, keys in this order (alphabetical):
//     {"i":"ai-request","lid":<lease_id|"">,"m":<METHOD>,"n":<nonce-hex>,
//      "p":<path>,"t":<unix-seconds>,"tool":<tool>}
//   Envelope shape matches C2PA/WireToken + Subscription/TokenVerifier.
//
// Server MUST, to make this replay-proof: verify the sig against X-Ink-Pubkey;
// reject when |now - t| exceeds a small freshness window; and dedupe `n`
// within that window. The request BODY is intentionally NOT bound (the
// /tools/* body is a curl-generated multipart we don't hold as one buffer) —
// so within the freshness window a captured header could carry a different
// body. On the paid-only model that is bounded (it spends the captured
// identity's own paid window, one nonce, once) and is a deliberate Phase-1
// scope call; bind a body/param hash later if it proves necessary.
//
// Thread-safety: init() is called once at startup before any warm/tool request
// and the identity is immutable afterward, so sign_request()/pubkey() are safe
// to call from the WarmLease renewal thread and ToolClient worker.

#include <string>
#include <vector>
#include <cstdint>

namespace AI {

class RequestSigner {
public:
    struct Header { std::string name; std::string value; };

    // Set the signing identity from DevKeys: `seed32` = app_seed_bytes() (the
    // 32-byte ed25519 seed), `pubkeyStrkey` = app_pubkey() (a G... strkey).
    // Disables signing (available() == false) if seed32 is null or the pubkey
    // is not a 56-char G... strkey. Call once after DevKeys::load().
    static void init(const uint8_t* seed32, const std::string& pubkeyStrkey);

    static bool available();             // identity set and usable
    static const std::string& pubkey();  // G... identity (also the metering label); "" if unavailable

    // Signed auth headers for one request. `method` e.g. "POST"/"DELETE";
    // `path` the URL path the server sees, e.g. "/warm" or "/tools/mesh";
    // `tool` "reangle"/"mesh"; `leaseId` "" when not applicable. Returns an
    // empty vector when unavailable — callers then send only X-API-Key.
    static std::vector<Header> sign_request(const std::string& method,
                                            const std::string& path,
                                            const std::string& tool,
                                            const std::string& leaseId);
};

}  // namespace AI
