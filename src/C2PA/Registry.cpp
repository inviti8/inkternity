#include "Registry.hpp"

#include <Helpers/Logger.hpp>
#include <cctype>
#include <string>

namespace C2PA {

Registry::Registry(const StellarCli& cli) : cli_(cli) {}

const char* Registry::fallback_for(GlobalConfig::StellarNetwork n) {
    return n == GlobalConfig::StellarNetwork::Testnet
        ? CERT_REGISTRY_TESTNET_FALLBACK
        : CERT_REGISTRY_MAINNET_FALLBACK;
}

void Registry::invalidate_cache() {
    cachedMainnet_.reset();
    cachedTestnet_.reset();
    lastWasFromRegistry_ = false;
}

namespace {

// Stellar contract IDs are 56-char base32 starting with 'C' followed
// by 55 of [A-Z2-7] (RFC 4648 base32 alphabet). The registry CLI
// returns the value as a JSON-quoted string ("CABC...XYZ"); extract
// the first such run from the captured stdout.
std::optional<std::string> find_contract_id(const std::string& out) {
    auto is_b32 = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
    };
    for (size_t i = 0; i + 56 <= out.size(); ++i) {
        if (out[i] != 'C') continue;
        bool all = true;
        for (size_t k = 1; k < 56; ++k) {
            if (!is_b32(out[i + k])) { all = false; break; }
        }
        if (!all) continue;
        // Reject runs glued onto a longer base32 token.
        if (i > 0 && is_b32(out[i - 1])) continue;
        if (i + 56 < out.size() && is_b32(out[i + 56])) continue;
        return out.substr(i, 56);
    }
    return std::nullopt;
}

const char* network_label(GlobalConfig::StellarNetwork n) {
    return n == GlobalConfig::StellarNetwork::Testnet ? "Testnet" : "Mainnet";
}

}  // namespace

std::optional<std::string> Registry::lookup_via_cli(
        GlobalConfig::StellarNetwork network) const {
    if (cli_.binary_path().empty()) return std::nullopt;

    // CLI scval arg formatting matrix (confirmed empirically against
    // the deployed contract): String values are JSON-quoted, enum
    // variants are JSON-quoted, Address is bare, Bytes/BytesN are
    // bare hex, integers are bare.
    std::string netArg = std::string("\"") + network_label(network) + "\"";

    auto r = cli_.invoke({
        "contract", "invoke",
        "--id",                 HVYM_REGISTRY_CONTRACT_ID,
        "--source-account",     DEFAULT_SOURCE_PUB,
        "--rpc-url",            REGISTRY_RPC_URL,
        "--network-passphrase", REGISTRY_PASSPHRASE,
        "--send",               "no",
        "--",
        "get_contract_id",
        "--name",    "\"hvym_cert_registry\"",
        "--network", netArg,
    });
    if (!r.ok()) {
        Logger::get().log("INFO",
            std::string("[C2PA::Registry] hvym_registry lookup exit=")
            + std::to_string(r.exit_code) + ":\n" + r.out);
        return std::nullopt;
    }
    auto id = find_contract_id(r.out);
    if (!id) {
        Logger::get().log("INFO",
            "[C2PA::Registry] hvym_registry returned no parseable contract ID. Raw:\n" + r.out);
        return std::nullopt;
    }
    return id;
}

std::string Registry::cert_registry_id(GlobalConfig::StellarNetwork net) {
    auto& slot = (net == GlobalConfig::StellarNetwork::Testnet)
        ? cachedTestnet_
        : cachedMainnet_;
    if (slot) {
        // Cached — we already know whether the cached value came from
        // the registry or the fallback; don't re-flip lastWasFromRegistry_
        // since a future test against an invalidate_cache()-then-lookup
        // wants the next live attempt to be visible.
        return *slot;
    }
    if (auto live = lookup_via_cli(net)) {
        slot = *live;
        lastWasFromRegistry_ = true;
        Logger::get().log("INFO",
            std::string("[C2PA::Registry] resolved hvym_cert_registry@")
            + network_label(net) + " = " + *live);
        return *live;
    }
    // Lookup failed — fall back to §0 hardcoded.
    slot = fallback_for(net);
    lastWasFromRegistry_ = false;
    Logger::get().log("INFO",
        std::string("[C2PA::Registry] registry lookup failed for ")
        + network_label(net) + " — using hardcoded fallback " + *slot);
    return *slot;
}

}  // namespace C2PA
