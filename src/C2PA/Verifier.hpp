#pragma once
// docs/design/C2PA.md §7 — provenance verifier surfaced at file-open.
// Reads any embedded C2PA manifest via Manifest::read_and_verify, then
// classifies the result for the artist's read of the file.
//
// v1 scope (this commit): NoManifest / ManifestMalformed /
// SignatureInvalid / TrustedLocal. The full on-chain trust chain walk
// (cert SAN → Soroban::get_app_ca → is_trusted → member_pubkey
// comparison) is a follow-up; it requires X.509 SAN parsing that
// isn't yet wired. TrustedLocal means c2pa-rs's internal signature
// verification passed but we haven't confirmed the signer's app
// address against the on-chain hvym_cert_registry — informative but
// not authoritative.

#include <filesystem>
#include <string>

namespace C2PA {

enum class VerifyStatus {
    NoManifest,         // file opens fine, no C2PA manifest embedded
    ManifestMalformed,  // manifest present but unparseable JSON / no active_manifest
    SignatureInvalid,   // c2pa-rs's validation_status indicates a verification failure
    TrustedLocal,       // c2pa-rs verified the signature; on-chain trust check deferred
};

const char* verify_status_str(VerifyStatus s) noexcept;

struct VerifyResult {
    VerifyStatus status   = VerifyStatus::NoManifest;
    std::string  title;          // manifest's title, if present
    std::string  generator;      // claim_generator_info name, if present
    std::string  detail;         // human-readable single line for USERINFO toast
};

// Inspect `path` and produce a one-shot verification result. Does
// NOT throw; missing-manifest is the common case and returns
// NoManifest cleanly. Logs INFO at each decision point so an artist
// or maintainer can trace what the verifier saw via log.txt.
VerifyResult verify_file(const std::filesystem::path& path);

}  // namespace C2PA
