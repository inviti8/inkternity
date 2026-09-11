#pragma once
// AI_BILLING_PHASE4.md §3.2 — orchestrate satisfying one x402 challenge:
// (ensure USDC trustline) → pay on-chain with the memo → POST /warm/pay with the
// tx hash. Headless: the "confirm first, then auto" consent lives at the UI
// toggle; by the time this runs, the artist has already opted in, so it settles
// automatically. Native only (curl + the stellar CLI).

#include "X402Challenge.hpp"
#include <string>

namespace C2PA { class StellarCli; }

namespace AI {

struct SettleResult {
    bool        ok = false;         // /warm/pay returned 200 (window credited/pending)
    std::string error;              // human-readable failure reason
    std::string txHash;             // the payment tx hash, for logs
    bool        needsFunds = false; // insufficient USDC/XLM — retrying won't help
};

// Everything the settle path needs beyond the challenge itself.
struct PayContext {
    const C2PA::StellarCli* cli = nullptr;  // resolved `stellar` binary owner
    std::string pubkey;                      // G... DevKeys app_pubkey (payer == identity)
    std::string secret;                      // S... DevKeys app_secret (signs)
    std::string expectedNetwork;             // app's configured net ("public"/"testnet")
};

// One attempt to buy the window `ch` describes for `tool`. Ensures the trustline
// only if the payment reports it's missing (avoids a redundant on-chain tx when
// one already exists). Returns ok iff /warm/pay 200'd.
SettleResult settle_window(const X402Challenge& ch, const std::string& tool,
                           const std::string& baseUrl, const std::string& apiKey,
                           const PayContext& ctx);

}  // namespace AI
