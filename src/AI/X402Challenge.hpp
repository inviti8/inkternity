#pragma once
// AI_BILLING_PHASE4.md §3.2 — parse the x402 payment challenge the proxy returns
// with a 402 (and the same shape from GET /warm/price). Pure: no network, no CLI.
//
// 402 body shape (hvym-img-tools X402_CLIENT_HANDOFF.md §1):
//   {"detail":"no live paid window for this identity",
//    "x402":{"asset":"USDC","issuer":"G...","amount":"0.75","pay_to":"G...",
//            "network":"public","window_s":900,"memo":"9f2c…",
//            "price_id":"9f2c…","horizon":"https://horizon.stellar.org"}}
// GET /warm/price returns those fields at the top level (no "x402" wrapper).

#include <optional>
#include <string>

namespace AI {

struct X402Challenge {
    std::string asset;      // e.g. "USDC"
    std::string issuer;     // G... asset issuer
    std::string amount;     // decimal string, e.g. "0.75"
    std::string payTo;      // G... payee
    std::string network;    // "public" | "testnet"
    std::string memo;       // MEMO_TEXT to bind the payment to this quote
    std::string priceId;    // opaque quote id, echoed to /warm/pay
    std::string horizon;    // informational (proxy verifies via Horizon)
    double      windowS = 0.0;

    // All fields the payment path needs are present.
    bool valid() const {
        return !asset.empty() && !issuer.empty() && !amount.empty() &&
               !payTo.empty() && !network.empty() && !memo.empty();
    }

    // Parse a 402 (or /warm/price) body. Accepts the "x402"-wrapped and the
    // top-level shapes. nullopt if the body is not valid JSON.
    static std::optional<X402Challenge> parse(const std::string& body);
};

}  // namespace AI
