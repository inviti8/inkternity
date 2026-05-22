#pragma once
// docs/design/C2PA.md §0 + §10/Q11 — discover the live
// hvym_cert_registry contract ID via the on-chain hvym_registry
// (registered there under name "hvym_cert_registry"). Matches the
// Portal's resolution flow in heavymeta_collective/config.py:168-218.
//
// Resolution: shell out via StellarCli to invoke
// `hvym_registry::get_contract_id`. Cache the result per-session so
// every subsequent C2PA op skips the lookup. On any failure (RPC down,
// CLI not available, transient simulate error) fall back to the
// hardcoded §0 IDs so the C2PA pipeline keeps working.
//
// The hvym_registry contract itself ALWAYS lives on mainnet,
// regardless of which network we're querying about. This mirrors the
// Portal's REGISTRY_RPC_URL pinning and keeps a single registry as
// the cross-network source of truth.

#include "StellarCli.hpp"
#include "../GlobalConfig.hpp"

#include <optional>
#include <string>

namespace C2PA {

class Registry {
public:
    // hvym_registry contract ID (§0). Always on mainnet.
    static constexpr const char* HVYM_REGISTRY_CONTRACT_ID =
        "CA6KQ5GYGI33VZB5IGWW7XXLLHR2MPEBWVDREU4P5ZGCSKRGHXBCRKXV";
    // Any G-strkey works for sim queries (registry view doesn't need
    // a real funded account). Matches the Portal's pinned source.
    static constexpr const char* DEFAULT_SOURCE_PUB =
        "GCKF63SHPP3I2HVJY3E5ZXCBBNBF4H7D3OKCG6547SO7VRJFKQDLPS64";
    // RPC + passphrase for the registry contract (mainnet always).
    static constexpr const char* REGISTRY_RPC_URL =
        "https://mainnet.sorobanrpc.com";
    static constexpr const char* REGISTRY_PASSPHRASE =
        "Public Global Stellar Network ; September 2015";

    // Hardcoded §0 fallbacks for hvym_cert_registry on each network.
    // Used when the registry lookup itself fails (no internet, CLI
    // missing, contract rate-limit, etc.). Never returns empty.
    static constexpr const char* CERT_REGISTRY_MAINNET_FALLBACK =
        "CAKBTT765YCBZDPU7RNPGC4C4TSXIRFHQCEBNPEQZNMJCLXAB3K6VE2G";
    static constexpr const char* CERT_REGISTRY_TESTNET_FALLBACK =
        "CC252R637U7QXG5SSHTVHBSKB3PGKRKRP66EEI2IEVTXIQWP6EQRLH2T";

    explicit Registry(const StellarCli& cli);

    // Return the cert-registry contract ID for the requested network.
    // Lazy + cached: first call shells out to hvym_registry, every
    // subsequent call returns the cached value. Falls back to the
    // hardcoded §0 IDs on any error so the gateway flow keeps moving.
    std::string cert_registry_id(GlobalConfig::StellarNetwork);

    // True iff the last cert_registry_id() call returned the on-chain
    // value rather than the §0 fallback. Useful for surfacing "you're
    // running against a stale registry" diagnostics in §10/Q1 rotation
    // monitoring (deferred).
    bool last_was_from_registry() const noexcept { return lastWasFromRegistry_; }

    // Force the next cert_registry_id() call to re-query the on-chain
    // registry instead of using the cached value.
    void invalidate_cache();

private:
    std::optional<std::string> lookup_via_cli(
        GlobalConfig::StellarNetwork network) const;
    static const char* fallback_for(GlobalConfig::StellarNetwork);

    const StellarCli& cli_;
    std::optional<std::string> cachedMainnet_;
    std::optional<std::string> cachedTestnet_;
    bool lastWasFromRegistry_ = false;
};

}  // namespace C2PA
