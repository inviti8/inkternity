#pragma once
// Headless end-to-end check for the Phase-4 pay path (AI_BILLING_PHASE4.md §10
// task 8): drives the REAL compiled RequestSigner + X402Challenge + StellarPay +
// WarmPay::settle_window against a live proxy, so the app<->proxy handshake is
// exercised without the GUI. Invoked via `inkternity --x402-selftest`.

#include "WarmPay.hpp"   // PayContext
#include <string>

namespace AI {

// POST /warm (signed) -> expect 402 -> settle_window (pay on-chain) -> POST /warm
// again. Returns a human-readable report; `ok` is the overall pass/fail (a
// settled payment counts as pass even if the grant is still pending — settle-on-
// grant without a warm GPU).
std::string x402_selftest(const std::string& endpoint, const std::string& apiKey,
                          const PayContext& ctx, bool& ok);

}  // namespace AI
