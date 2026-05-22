#include "WireToken.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <nlohmann/json.hpp>
#include <string_view>

namespace C2PA::WireToken {

const char* parse_status_str(ParseStatus s) noexcept {
    switch (s) {
        case ParseStatus::OK:               return "ok";
        case ParseStatus::Malformed:        return "Token wire format is unreadable.";
        case ParseStatus::WrongIntent:      return "Token intent does not match the expected operation.";
        case ParseStatus::MemberPubkeyBad:  return "Token's member key field is not 32 raw bytes.";
        case ParseStatus::FingerprintBad:   return "Token's fingerprint field is not a 32-byte SHA-256.";
        case ParseStatus::MissingField:     return "Token is missing a required field.";
    }
    return "unknown";
}

namespace {

// URL-safe base64 decode, no padding required. Returns empty on malformed
// input. Same shape as Subscription::b64u_decode at TokenVerifier.cpp:34
// (intentionally duplicated — keeps WireToken self-contained per the
// plan's ~120 LoC budget; a shared crypto/Base64Url module is a
// follow-up if a fourth user appears).
std::vector<uint8_t> b64u_decode(std::string_view in) {
    static int8_t table[256];
    static bool table_inited = false;
    if (!table_inited) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; ++i) table[(uint8_t)alpha[i]] = (int8_t)i;
        table_inited = true;
    }
    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = table[(uint8_t)c];
        if (v < 0) return {};
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xff));
        }
    }
    return out;
}

// 64-char lowercase-hex → 32 raw bytes. Returns true iff `in` is exactly
// 64 chars from [0-9a-f]. Note: cert_registry_canonical.py emits
// lowercase only; we accept lowercase only for byte-strictness against
// what the contract reconstructs.
bool hex_decode_32(std::string_view in, std::array<uint8_t, 32>& out) {
    if (in.size() != 64) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        int hi = nibble(in[2 * i]);
        int lo = nibble(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool looks_like_stellar_pubkey(const std::string& s) {
    return s.size() == 56 && s.front() == 'G';
}

// Split + b64u-decode both wire halves. Populates auth_signature and
// auth_payload. Returns Malformed on shape failure, otherwise OK; the
// caller still has to JSON-parse auth_payload.
ParseStatus split_wire(const std::string& token,
                       std::vector<uint8_t>& sig,
                       std::vector<uint8_t>& payload) {
    const auto dot = token.find('.');
    if (dot == std::string::npos) return ParseStatus::Malformed;
    sig     = b64u_decode(std::string_view(token.data(), dot));
    payload = b64u_decode(std::string_view(token.data() + dot + 1,
                                           token.size() - dot - 1));
    if (sig.size() != 64 || payload.empty()) return ParseStatus::Malformed;
    return ParseStatus::OK;
}

std::string hex_of(const std::array<uint8_t, 32>& a) {
    static constexpr char d[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        out[2 * i]     = d[(a[i] >> 4) & 0xf];
        out[2 * i + 1] = d[a[i] & 0xf];
    }
    return out;
}

}  // namespace

ParseStatus parse_register(const std::string& token, RegisterParams& out) {
    out = {};
    auto rc = split_wire(token, out.auth_signature, out.auth_payload);
    if (rc != ParseStatus::OK) return rc;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(out.auth_payload.begin(), out.auth_payload.end());
    } catch (...) { return ParseStatus::Malformed; }
    if (!j.is_object()) return ParseStatus::Malformed;

    if (!j.contains("i") || !j["i"].is_string()) return ParseStatus::MissingField;
    if (j["i"].get<std::string>() != "register-app-ca") return ParseStatus::WrongIntent;

    auto must_str = [&](const char* k, std::string& dst) -> bool {
        if (!j.contains(k) || !j[k].is_string()) return false;
        dst = j[k].get<std::string>();
        return !dst.empty();
    };
    auto must_int = [&](const char* k, int64_t& dst) -> bool {
        if (!j.contains(k) || !j[k].is_number_integer()) return false;
        dst = j[k].get<int64_t>();
        return true;
    };

    if (!must_str("a", out.app_address))   return ParseStatus::MissingField;
    if (!must_str("k", out.app_kind))      return ParseStatus::MissingField;
    if (!must_int("exp", out.expires_at_unix)) return ParseStatus::MissingField;
    if (!must_int("n",   out.nonce))       return ParseStatus::MissingField;

    std::string fp_hex, m_hex;
    if (!must_str("fp", fp_hex)) return ParseStatus::MissingField;
    if (!must_str("m",  m_hex))  return ParseStatus::MissingField;
    if (!hex_decode_32(fp_hex, out.fingerprint))   return ParseStatus::FingerprintBad;
    if (!hex_decode_32(m_hex,  out.member_pubkey)) return ParseStatus::MemberPubkeyBad;

    return ParseStatus::OK;
}

ParseStatus parse_rotate(const std::string& token, RotateParams& out) {
    out = {};
    auto rc = split_wire(token, out.auth_signature, out.auth_payload);
    if (rc != ParseStatus::OK) return rc;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(out.auth_payload.begin(), out.auth_payload.end());
    } catch (...) { return ParseStatus::Malformed; }
    if (!j.is_object()) return ParseStatus::Malformed;

    if (!j.contains("i") || !j["i"].is_string()) return ParseStatus::MissingField;
    if (j["i"].get<std::string>() != "rotate-app-ca") return ParseStatus::WrongIntent;

    if (!j.contains("a") || !j["a"].is_string()) return ParseStatus::MissingField;
    out.app_address = j["a"].get<std::string>();
    if (out.app_address.empty()) return ParseStatus::MissingField;

    if (!j.contains("exp") || !j["exp"].is_number_integer()) return ParseStatus::MissingField;
    out.new_expires_at_unix = j["exp"].get<int64_t>();

    if (!j.contains("n") || !j["n"].is_number_integer()) return ParseStatus::MissingField;
    out.nonce = j["n"].get<int64_t>();

    if (!j.contains("fp") || !j["fp"].is_string()) return ParseStatus::MissingField;
    const std::string fp_hex = j["fp"].get<std::string>();
    if (!hex_decode_32(fp_hex, out.new_fingerprint)) return ParseStatus::FingerprintBad;

    return ParseStatus::OK;
}

ParseStatus parse_revoke(const std::string& token, RevokeParams& out) {
    out = {};
    auto rc = split_wire(token, out.auth_signature, out.auth_payload);
    if (rc != ParseStatus::OK) return rc;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(out.auth_payload.begin(), out.auth_payload.end());
    } catch (...) { return ParseStatus::Malformed; }
    if (!j.is_object()) return ParseStatus::Malformed;

    if (!j.contains("i") || !j["i"].is_string()) return ParseStatus::MissingField;
    if (j["i"].get<std::string>() != "revoke-app-ca") return ParseStatus::WrongIntent;

    if (!j.contains("a") || !j["a"].is_string()) return ParseStatus::MissingField;
    out.app_address = j["a"].get<std::string>();
    if (out.app_address.empty()) return ParseStatus::MissingField;

    if (!j.contains("n") || !j["n"].is_number_integer()) return ParseStatus::MissingField;
    out.nonce = j["n"].get<int64_t>();

    return ParseStatus::OK;
}

// ---- Cross-check helpers -------------------------------------------------

std::string check_register(const RegisterParams& got,
                           const ExpectedRegisterValues& expected) {
    if (!looks_like_stellar_pubkey(expected.app_address))
        return "Local app address is not a Stellar G-address.";
    if (got.app_address != expected.app_address)
        return std::format("Token app_address {} does not match this install's {}.",
            got.app_address, expected.app_address);
    if (got.app_kind != expected.app_kind)
        return std::format("Token app_kind '{}' does not match expected '{}'.",
            got.app_kind, expected.app_kind);
    if (got.fingerprint != expected.fingerprint)
        return std::format("Token fingerprint {} does not match local CA {}.",
            hex_of(got.fingerprint), hex_of(expected.fingerprint));
    if (got.expires_at_unix != expected.expires_at_unix)
        return std::format("Token expires_at {} does not match local CA {}.",
            got.expires_at_unix, expected.expires_at_unix);
    return {};
}

std::string check_rotate(const RotateParams& got,
                         const ExpectedRotateValues& expected) {
    if (got.app_address != expected.app_address)
        return std::format("Token app_address {} does not match this install's {}.",
            got.app_address, expected.app_address);
    if (got.new_fingerprint != expected.new_fingerprint)
        return std::format("Token new_fingerprint {} does not match local new CA {}.",
            hex_of(got.new_fingerprint), hex_of(expected.new_fingerprint));
    if (got.new_expires_at_unix != expected.new_expires_at_unix)
        return std::format("Token new_expires_at {} does not match local new CA {}.",
            got.new_expires_at_unix, expected.new_expires_at_unix);
    return {};
}

std::string check_revoke(const RevokeParams& got,
                         const ExpectedRevokeValues& expected) {
    if (got.app_address != expected.app_address)
        return std::format("Token app_address {} does not match this install's {}.",
            got.app_address, expected.app_address);
    return {};
}

}  // namespace C2PA::WireToken
