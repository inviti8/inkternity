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

// rotate_app_ca — replace the app's currently-registered fingerprint +
// expiry with a fresh one. Member-signed auth (same shape as register).
// `auth_payload` + `auth_signature` are pass-through bytes from a
// HVYM-CA-ROT-v1 wire token; the contract reconstructs canonical
// payload from typed params and byte-compares (§G3).
InvokeResult submit_rotate(
    const StellarCli& cli,
    const RpcConfig& rpc,
    std::string_view contract_id,
    std::string_view source_account,
    std::string_view app_address,
    const std::array<uint8_t, 32>& new_fingerprint,
    uint64_t new_expires_at,
    uint64_t nonce,
    const std::vector<uint8_t>& auth_payload,
    const std::vector<uint8_t>& auth_signature);

// revoke_by_app — the app self-revokes via require_auth. No Portal
// token required (the app's own Stellar key signs the tx, contract
// checks the source matches the stored app_address). Simplest of the
// three state-changing entry points.
InvokeResult submit_revoke_by_app(
    const StellarCli& cli,
    const RpcConfig& rpc,
    std::string_view contract_id,
    std::string_view source_account,
    std::string_view app_address);

// get_app_ca view — fetch the full on-chain record for an app
// address. Returned as raw CLI output for now; the typed struct
// extractor lands when I16's verifier needs it.
std::optional<std::string> get_app_ca_raw(
    const StellarCli& cli,
    const RpcConfig& rpc,
    std::string_view contract_id,
    std::string_view source_account,
    std::string_view app_address);

// Maximum ExtendFootprintTTLOp `extendTo` accepted by the stellar
// CLI in a single op: ~30 days at the 5s/ledger nominal mainnet
// close, ~34 days at the ~5.76s observed. extendTo is an absolute
// floor (not additive), so re-running early is a cheap no-op for
// rent — only the inclusion fee applies.
constexpr uint32_t kCertTtlExtendLedgers = 535'679;

// Top up the TTL on the on-chain AppCa(app_address) entry so it
// doesn't archive after ~30 days of inactivity. Builds the LedgerKey
// for `DataKey::AppCa(Address)` by piping JSON through `stellar xdr
// encode --type ScVal`, then submits via `stellar contract extend`.
// Costs the inclusion fee plus per-byte rent for the added ledgers
// (sub-cent in XLM for a ~150-byte AppCaRecord).
//
// Cadence (driven by RegistrationFlow):
//   - new register → wait 20 days from register, then extend
//   - re-extend every 20 days while status == Active
// Keeps ~10 days of headroom against the 30-day archival horizon.
InvokeResult extend_app_ca_ttl(
    const StellarCli& cli,
    const RpcConfig& rpc,
    std::string_view contract_id,
    std::string_view source_account,
    std::string_view app_address,
    uint32_t ledgers_to_extend = kCertTtlExtendLedgers);

}  // namespace C2PA::Soroban
