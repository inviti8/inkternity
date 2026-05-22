#pragma once
// docs/design/C2PA.md §2 + §9 (gate G1/G2) — emit the three pasteable
// text bundles consumed by the Portal's /launch → Provenance card:
//
//   HVYM-CA-REG-v1   first registration
//   HVYM-CA-ROT-v1   rotation
//   HVYM-CA-REV-v1   revocation
//
// Parsing lives at heavymeta_collective/payments/cert_registry_bundle.py.
// Our output must round-trip cleanly through parse_bundle() — that's the
// cross-repo agreement gate G1. Whitespace + label case are tolerant on
// the parser side; the prefix line is strict.

#include <array>
#include <cstdint>
#include <string>

namespace C2PA {

enum class AppKind {
    Inkternity,
    Andromica,
    Pintheon,
    Other,
};

// Lowercase label suitable for the app_kind: line in a REG bundle.
// Order matches the contract's enum AppKind values per §5.2.
const char* app_kind_to_string(AppKind k) noexcept;

struct RegisterBundle {
    std::string app_address;          // G... strkey, 56 chars
    AppKind     app_kind = AppKind::Inkternity;
    std::array<uint8_t, 32> fingerprint{};
    int64_t     expires_at_unix = 0;
};

struct RotateBundle {
    std::string app_address;
    std::array<uint8_t, 32> new_fingerprint{};
    int64_t     new_expires_at_unix = 0;
};

struct RevokeBundle {
    std::string app_address;
};

namespace Bundle {

// Format the three bundle types as text. Empty string on invalid input
// (e.g. app_address not a Stellar G... strkey shape; expires_at_unix
// <= 0). Output ends with a trailing newline.
//
// Spacing matches mock_c2pa/andromica/bundle.py for human readability;
// the Portal parser tolerates any whitespace between label and value
// (per cert_registry_bundle.py:76), so the agreement gate is field
// names + ISO-8601 timestamp shape + lowercase hex fingerprint.
std::string render_register(const RegisterBundle& b);
std::string render_rotate(const RotateBundle& b);
std::string render_revoke(const RevokeBundle& b);

}  // namespace Bundle

}  // namespace C2PA
