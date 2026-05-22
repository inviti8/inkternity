#pragma once
// docs/design/C2PA.md §2 + §9 gate G3 — parse the wire token the Portal
// mints in response to a pasted bundle.
//
// Wire format (identical to the existing Subscription token in
// src/Subscription/TokenVerifier.cpp:142-176):
//
//   <base64url(64-byte ed25519 signature)> "." <base64url(json payload)>
//
// Payload (sorted-keys compact JSON, member-signed; mirrors
// heavymeta_collective/payments/cert_registry_canonical.py):
//
//   register: {"a":G,"alg":"ed25519","exp":int,"fp":hex64,
//              "i":"register-app-ca","k":AppKind,"m":hex64,"n":int}
//   rotate:   {"a":G,"exp":int,"fp":hex64,
//              "i":"rotate-app-ca","n":int}
//   revoke:   {"a":G,"i":"revoke-app-ca","n":int}
//
// Critical agreement gate G3: this module captures the *exact bytes*
// the Portal signed and exposes them as opaque blobs. The desktop
// MUST NOT reconstruct the canonical payload — pass auth_payload and
// auth_signature through unchanged to register_app_ca / rotate_app_ca
// / revoke_by_member. cert_registry_canonical.py is informational only.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace C2PA::WireToken {

enum class ParseStatus {
    OK,
    Malformed,        // wire format unparseable: no dot, bad b64u, bad JSON
    WrongIntent,      // payload.i didn't match the expected intent
    MemberPubkeyBad,  // payload.m not 64 lowercase-hex chars (32 raw bytes)
    FingerprintBad,   // payload.fp not 64 lowercase-hex chars
    MissingField,     // a required field was absent
};

const char* parse_status_str(ParseStatus s) noexcept;

struct RegisterParams {
    std::string app_address;   // payload.a — Stellar G... strkey
    std::string app_kind;      // payload.k — raw string; caller maps to AppKind
    std::array<uint8_t, 32> fingerprint{};    // payload.fp decoded
    int64_t expires_at_unix = 0;              // payload.exp
    std::array<uint8_t, 32> member_pubkey{};  // payload.m decoded (raw 32 bytes)
    int64_t nonce = 0;                        // payload.n
    // §G3: opaque pass-through bytes. Hand these to register_app_ca's
    // auth_payload + auth_signature params unmodified.
    std::vector<uint8_t> auth_payload;
    std::vector<uint8_t> auth_signature;
};

struct RotateParams {
    std::string app_address;
    std::array<uint8_t, 32> new_fingerprint{};
    int64_t new_expires_at_unix = 0;
    int64_t nonce = 0;
    std::vector<uint8_t> auth_payload;
    std::vector<uint8_t> auth_signature;
};

struct RevokeParams {
    std::string app_address;
    int64_t nonce = 0;
    std::vector<uint8_t> auth_payload;
    std::vector<uint8_t> auth_signature;
};

// Parse a wire token. Each function fills `out` and returns OK iff the
// token is well-formed AND the intent matches the parser's expected
// intent. Cryptographic signature verification is NOT done here — the
// contract verifies byte-for-byte against the reconstructed canonical
// payload, and the desktop pass-through (G3) is the load-bearing
// invariant. A defensive local verify can be layered on top later.
ParseStatus parse_register(const std::string& token, RegisterParams& out);
ParseStatus parse_rotate  (const std::string& token, RotateParams& out);
ParseStatus parse_revoke  (const std::string& token, RevokeParams& out);

// Cross-check helpers — return empty string on match; otherwise a
// human-readable one-line message naming the first mismatched field
// (matches the per-field failure shape in
// heavymeta_collective/scripts/c2pa_smoke.py:146-153).
struct ExpectedRegisterValues {
    std::string app_address;                // local app G-address
    std::string app_kind;                   // raw string ("Inkternity" etc.)
    std::array<uint8_t, 32> fingerprint{};  // local CA fingerprint
    int64_t expires_at_unix = 0;            // local CA notAfter
};
std::string check_register(const RegisterParams& got,
                           const ExpectedRegisterValues& expected);

struct ExpectedRotateValues {
    std::string app_address;
    std::array<uint8_t, 32> new_fingerprint{};
    int64_t new_expires_at_unix = 0;
};
std::string check_rotate(const RotateParams& got,
                         const ExpectedRotateValues& expected);

struct ExpectedRevokeValues {
    std::string app_address;
};
std::string check_revoke(const RevokeParams& got,
                         const ExpectedRevokeValues& expected);

}  // namespace C2PA::WireToken
