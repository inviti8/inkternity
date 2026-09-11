#pragma once
// AI_BILLING_PHASE4.md §3.1 — thin wrappers around the `stellar` CLI for the
// two classic operations x402 billing needs: `change-trust` (so the wallet can
// hold USDC) and `payment` (buying a warm window). Mirrors the C2PA
// SorobanSubmit pattern: build argv → StellarCli::invoke → scrape tx hash.
//
// Deliberately does NOT own a StellarCli — the caller passes one so C2PA and
// billing can share a single probe/auto-install (AI_BILLING_PHASE4.md §3.4).
//
// The payment path is a 5-step pipeline because `stellar 23.4.1` has no
// `--memo` flag and the proxy REQUIRES a MEMO_TEXT that binds the payment to
// the quote (§2.2): build-only → decode → inject memo JSON → encode → sign →
// send. Memo JSON shape confirmed against testnet: env["tx"]["tx"]["memo"] =
// {"text": <memo>} (§2.2, task #1 RESOLVED).

#include <string>

namespace C2PA { class StellarCli; }

namespace AI {

struct PayResult {
    bool        ok = false;
    std::string tx_hash;   // 64-hex Stellar tx hash on success (the X-Payment value)
    std::string error;     // human-readable failure reason (empty on success)
    std::string raw;       // captured CLI output, for logs/forensics
};

class StellarPay {
public:
    // Convert a decimal asset amount ("0.75") to integer stroops ("7500000",
    // 1 unit = 10^7 stroops). String math — never float. Returns false on a
    // malformed amount or more than 7 fractional digits.
    static bool to_stroops(const std::string& decimalAmount, std::string& outStroops);

    // Create a `assetCode:issuer` trustline from the account behind
    // `sourceSecret` (S-strkey; the CLI signs locally). One-shot
    // build+sign+send. An already-existing trustline is reported ok=true.
    // Needs ~0.5 XLM of reserve to be available in the wallet.
    static PayResult ensure_trustline(const C2PA::StellarCli& cli,
                                      const std::string& sourceSecret,
                                      const std::string& assetCode,
                                      const std::string& issuer,
                                      const std::string& rpcUrl,
                                      const std::string& networkPassphrase);

    // Pay `amountStroops` of `assetCode:issuer` to `payTo` with MEMO_TEXT=`memo`.
    // `sourcePubkey` (G) is the build-only source; `sourceSecret` (S) signs.
    // The two MUST be the same account — the proxy binds payer==identity.
    // `memo` must be <= 28 UTF-8 bytes (MEMO_TEXT limit); a longer memo is a
    // hard error (never truncate — a wrong memo settles nothing).
    static PayResult pay(const C2PA::StellarCli& cli,
                         const std::string& sourcePubkey,
                         const std::string& sourceSecret,
                         const std::string& payTo,
                         const std::string& assetCode,
                         const std::string& issuer,
                         const std::string& amountStroops,
                         const std::string& memo,
                         const std::string& rpcUrl,
                         const std::string& networkPassphrase);
};

}  // namespace AI
