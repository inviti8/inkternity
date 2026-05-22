#include "Bundle.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <string>

namespace C2PA {

const char* app_kind_to_string(AppKind k) noexcept {
    switch (k) {
        case AppKind::Inkternity: return "Inkternity";
        case AppKind::Andromica:  return "Andromica";
        case AppKind::Pintheon:   return "Pintheon";
        case AppKind::Other:      return "Other";
    }
    return "Other";
}

namespace {

bool looks_like_stellar_pubkey(const std::string& s) {
    return s.size() == 56 && s.front() == 'G';
}

// Lowercase hex encoding — matches Python's bytes.hex() output the
// parser expects (cert_registry_bundle.py:194 lowercases then checks).
std::string to_hex_lower(const std::array<uint8_t, 32>& bytes) {
    static constexpr char d[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        out[2 * i]     = d[(bytes[i] >> 4) & 0xf];
        out[2 * i + 1] = d[bytes[i] & 0xf];
    }
    return out;
}

// Format a unix timestamp as ISO 8601 with seconds precision and the
// explicit UTC offset "+00:00" — byte-identical to Python's
// datetime.fromtimestamp(ts, tz=UTC).isoformat(timespec="seconds").
// Empty string on conversion failure (only possible on extreme out-of-range).
std::string format_iso8601_utc(int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm_utc{};
#if defined(_WIN32)
    if (_gmtime64_s(&tm_utc, reinterpret_cast<__time64_t*>(&t)) != 0) return {};
#else
    if (!gmtime_r(&t, &tm_utc)) return {};
#endif
    char buf[40];
    int n = std::snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d+00:00",
        tm_utc.tm_year + 1900,
        tm_utc.tm_mon + 1,
        tm_utc.tm_mday,
        tm_utc.tm_hour,
        tm_utc.tm_min,
        tm_utc.tm_sec);
    if (n <= 0 || n >= static_cast<int>(sizeof(buf))) return {};
    return std::string(buf, static_cast<size_t>(n));
}

}  // namespace

namespace Bundle {

std::string render_register(const RegisterBundle& b) {
    if (!looks_like_stellar_pubkey(b.app_address)) return {};
    if (b.expires_at_unix <= 0) return {};

    const std::string iso = format_iso8601_utc(b.expires_at_unix);
    if (iso.empty()) return {};

    std::string out;
    out.reserve(220);
    out += "HVYM-CA-REG-v1\n";
    out += "app_address:    "; out += b.app_address;                 out += '\n';
    out += "app_kind:       "; out += app_kind_to_string(b.app_kind); out += '\n';
    out += "fingerprint:    "; out += to_hex_lower(b.fingerprint);   out += '\n';
    out += "pubkey_alg:     ed25519\n";
    out += "expires_at:     "; out += iso;                            out += '\n';
    return out;
}

std::string render_rotate(const RotateBundle& b) {
    if (!looks_like_stellar_pubkey(b.app_address)) return {};
    if (b.new_expires_at_unix <= 0) return {};

    const std::string iso = format_iso8601_utc(b.new_expires_at_unix);
    if (iso.empty()) return {};

    std::string out;
    out.reserve(180);
    out += "HVYM-CA-ROT-v1\n";
    out += "app_address:        "; out += b.app_address;                   out += '\n';
    out += "new_fingerprint:    "; out += to_hex_lower(b.new_fingerprint); out += '\n';
    out += "new_expires_at:     "; out += iso;                              out += '\n';
    return out;
}

std::string render_revoke(const RevokeBundle& b) {
    if (!looks_like_stellar_pubkey(b.app_address)) return {};

    std::string out;
    out.reserve(80);
    out += "HVYM-CA-REV-v1\n";
    out += "app_address:    "; out += b.app_address; out += '\n';
    return out;
}

}  // namespace Bundle

}  // namespace C2PA
