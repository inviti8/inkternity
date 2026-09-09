#include "RequestSigner.hpp"

#ifndef __EMSCRIPTEN__

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <string>

extern "C" {
#include "../../deps/tweetnacl/tweetnacl.h"   // C sources; no extern "C" guard in the header
}

namespace AI {
namespace {

// Identity, set once by init() and immutable afterward (see header note on
// thread-safety). gReady gates everything.
bool                     gReady = false;
std::string              gPub;                 // G... strkey
std::array<uint8_t, 64>  gSk{};                // ed25519 secret key (seed → sk)

// URL-safe base64 encode, no padding — inverse of C2PA/WireToken's
// b64u_decode, matching the same alphabet so the two round-trip.
std::string b64u_encode(const uint8_t* data, size_t n) {
    static const char* alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(alpha[(v >> 18) & 0x3f]);
        out.push_back(alpha[(v >> 12) & 0x3f]);
        out.push_back(alpha[(v >> 6) & 0x3f]);
        out.push_back(alpha[v & 0x3f]);
    }
    if (n - i == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(alpha[(v >> 18) & 0x3f]);
        out.push_back(alpha[(v >> 12) & 0x3f]);
    } else if (n - i == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(alpha[(v >> 18) & 0x3f]);
        out.push_back(alpha[(v >> 12) & 0x3f]);
        out.push_back(alpha[(v >> 6) & 0x3f]);
    }
    return out;
}

// Minimal JSON string escaper — the canonical payload is built by hand (not
// via nlohmann) so its bytes are byte-for-byte reproducible on the verifier.
// Only leaseId is externally sourced; the rest are our own ASCII constants.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hx = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hx[(c >> 4) & 0xf]);
                    out.push_back(hx[c & 0xf]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string random_nonce_hex() {
    std::random_device rd;
    std::mt19937_64 gen(((uint64_t)rd() << 32) ^ rd() ^
                        (uint64_t)std::chrono::high_resolution_clock::now()
                            .time_since_epoch().count());
    uint64_t a = gen(), b = gen();
    static const char* hx = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 8; ++i) {
        out[i * 2]     = hx[(a >> (i * 8 + 4)) & 0xf];
        out[i * 2 + 1] = hx[(a >> (i * 8)) & 0xf];
    }
    for (int i = 0; i < 8; ++i) {
        out[16 + i * 2]     = hx[(b >> (i * 8 + 4)) & 0xf];
        out[16 + i * 2 + 1] = hx[(b >> (i * 8)) & 0xf];
    }
    return out;
}

bool looks_like_pubkey(const std::string& s) {
    return s.size() == 56 && s.front() == 'G';
}

}  // namespace

void RequestSigner::init(const uint8_t* seed32, const std::string& pubkeyStrkey) {
    gReady = false;
    gPub.clear();
    if (!seed32 || !looks_like_pubkey(pubkeyStrkey)) return;

    // Expand the 32-byte seed into the 64-byte ed25519 secret key tweetnacl's
    // crypto_sign expects. pk is discarded — the identity pubkey we send on the
    // wire is the Stellar strkey (same 32 bytes, encoded), not this raw pk.
    std::array<uint8_t, 32> pk{};
    if (crypto_sign_ed25519_tweet_seed_keypair(pk.data(), gSk.data(), seed32) != 0)
        return;

    gPub = pubkeyStrkey;
    gReady = true;
}

bool RequestSigner::available() { return gReady; }

const std::string& RequestSigner::pubkey() {
    static const std::string kEmpty;
    return gReady ? gPub : kEmpty;
}

std::vector<RequestSigner::Header> RequestSigner::sign_request(
        const std::string& method, const std::string& path,
        const std::string& tool, const std::string& leaseId) {
    if (!gReady) return {};

    const int64_t t = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    const std::string nonce = random_nonce_hex();

    // Canonical payload — compact, keys alphabetical, no spaces. MUST match
    // the verifier byte-for-byte (see RequestSigner.hpp wire contract).
    std::string payload =
        std::string("{\"i\":\"ai-request\"") +
        ",\"lid\":\"" + json_escape(leaseId) + "\"" +
        ",\"m\":\"" + json_escape(method) + "\"" +
        ",\"n\":\"" + nonce + "\"" +
        ",\"p\":\"" + json_escape(path) + "\"" +
        ",\"t\":" + std::to_string(t) +
        ",\"tool\":\"" + json_escape(tool) + "\"}";

    // ed25519 sign → sm = sig(64) || message; the detached sig is the first 64.
    std::vector<uint8_t> sm(payload.size() + crypto_sign_BYTES);
    unsigned long long smlen = 0;
    if (crypto_sign(sm.data(), &smlen,
                    reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
                    gSk.data()) != 0)
        return {};

    const std::string envelope =
        b64u_encode(sm.data(), crypto_sign_BYTES) + "." +
        b64u_encode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

    return {
        { "X-Ink-Pubkey", gPub },
        { "X-Ink-Auth",   envelope },
    };
}

}  // namespace AI

#else  // __EMSCRIPTEN__ — no AI networking on WASM (no CORS); signing is inert.

namespace AI {
void RequestSigner::init(const uint8_t*, const std::string&) {}
bool RequestSigner::available() { return false; }
const std::string& RequestSigner::pubkey() { static const std::string e; return e; }
std::vector<RequestSigner::Header> RequestSigner::sign_request(
    const std::string&, const std::string&, const std::string&, const std::string&) { return {}; }
}  // namespace AI

#endif
