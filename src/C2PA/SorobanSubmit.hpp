#pragma once
// docs/design/C2PA.md §8.2 (revised) — thin wrappers around
// `stellar contract invoke` (subprocess-managed by StellarCli) for the
// six hvym-cert-registry entry points. Mirror of
// `pintheon_contracts/mock_c2pa/register.py`'s function signatures.
//
// The desktop never re-encodes canonical_payload — `auth_payload` and
// `auth_signature` are passed as opaque hex blobs through to the
// contract, which reconstructs from the typed params and byte-compares.
// Plan §G3.

#include "StellarCli.hpp"
#include "../GlobalConfig.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace C2PA::Soroban {

struct RpcConfig {
    std::string rpc_url;             // e.g. https://soroban-testnet.stellar.org
    std::string network_passphrase;  // e.g. "Test SDF Network ; September 2015"
};

// Built-in network presets.
RpcConfig testnet_config() noexcept;
RpcConfig mainnet_config() noexcept;

// Resolve from a network kind (preset RPC URLs + passphrases).
RpcConfig config_for(GlobalConfig::StellarNetwork);

// Resolve from GlobalConfig + STELLAR_NETWORK env-var override.
// Env wins for dev / smoke testing; otherwise the persisted user
// preference applies. Matches §10/Q3 resolution.
RpcConfig config_for_env_or_global(const GlobalConfig&);

struct InvokeResult {
    bool success = false;
    std::string tx_hash;    // extracted from stdout if present
    std::string error;      // human-readable failure reason
    std::string raw_out;    // captured CLI stdout/stderr for forensics
};

// register_app_ca — submit the Portal-minted authorization token.
// `source_account` accepts S... strkey, "alice" identity, or G... +
// out-of-band signing (CLI handles the auth wiring).
InvokeResult submit_register(
    const StellarCli& cli,
    const RpcConfig& rpc,
    std::string_view contract_id,
    std::string_view source_account,
    std::string_view app_address,
    const std::array<uint8_t, 32>& member_pubkey,
    std::string_view app_kind,
    const std::array<uint8_t, 32>& fingerprint,
    uint64_t expires_at,
    uint64_t nonce,
    const std::vector<uint8_t>& auth_payload,
    const std::vector<uint8_t>& auth_signature);

// is_trusted view — does the contract consider this (app, fingerprint)
// pair active + non-revoked? Returns nullopt on RPC/CLI failure
// (distinguishes "false" from "couldn't check").
std::optional<bool> is_trusted(
    const StellarCli& cli,
    const RpcConfig& rpc,
    std::string_view contract_id,
    std::string_view source_account,
    std::string_view app_address,
    const std::array<uint8_t, 32>& fingerprint);

// rotate / revoke / get_app_ca wrappers follow the same shape — added
// when I11 (rotate/revoke UI) and I16 (verifier on import) need them.

}  // namespace C2PA::Soroban
