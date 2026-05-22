#pragma once
// docs/design/C2PA.md §6 — issue an ephemeral per-publish leaf cert
// signed by the app CA. Mirror of
// mock_c2pa/andromica/ca_generation.py::issue_member_leaf.
//
// Called on the hot path: every canvas save / image export gets a
// fresh leaf with a 24h notAfter so a compromised leaf has a tight
// time window. The private seed is held only inside the returned
// MemberLeaf for the duration of one manifest-signing call, then
// zeroed by the destructor.
//
// Extensions on the leaf:
//   - BasicConstraints CA:FALSE                       (critical)
//   - KeyUsage digitalSignature + contentCommitment   (critical)
//   - ExtendedKeyUsage 1.3.6.1.4.1.42038.1.5.0        (critical)
//   - SubjectAlternativeName URIs                     (non-critical)
//       stellar:<member_stellar_address>
//       <member_canon_url> if non-empty
//
// The leaf's SAN stellar URI is informational. The verifier
// (I16) treats the on-chain AppCaRecord.member_pubkey as the
// authoritative member identity, not the SAN string.

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace C2PA {

class AppCa;

class MemberLeaf {
public:
    MemberLeaf() = default;
    ~MemberLeaf() noexcept;
    MemberLeaf(const MemberLeaf&) = delete;
    MemberLeaf& operator=(const MemberLeaf&) = delete;
    MemberLeaf(MemberLeaf&& other) noexcept;
    MemberLeaf& operator=(MemberLeaf&& other) noexcept;

    bool valid() const noexcept { return !cert_der_.empty(); }

    // DER-encoded X.509 leaf cert. Goes into the C2PA manifest's
    // signing chain alongside the CA's DER.
    const std::vector<uint8_t>& cert_der() const noexcept { return cert_der_; }
    std::vector<uint8_t>&       cert_der_mut() noexcept   { return cert_der_; }

    // Raw 32-byte Ed25519 private seed. Hand to c2pa-rs's
    // SignerCallback (I13) which calls back into our tweetnacl
    // crypto_sign_detached so the seed never crosses the FFI in raw
    // form. Zeroed by the destructor; copies are deleted.
    const std::array<uint8_t, 32>& private_seed() const noexcept { return private_seed_; }
    std::array<uint8_t, 32>&       private_seed_mut() noexcept   { return private_seed_; }

    // Matching Ed25519 public key — useful for sanity checks and
    // for the verifier to extract from the leaf cert.
    const std::array<uint8_t, 32>& public_key() const noexcept { return public_key_; }
    std::array<uint8_t, 32>&       public_key_mut() noexcept   { return public_key_; }

private:
    std::vector<uint8_t>     cert_der_;
    std::array<uint8_t, 32>  private_seed_{};
    std::array<uint8_t, 32>  public_key_{};
};

// Issue an ephemeral leaf cert signed by `ca`. Returns an invalid
// MemberLeaf (valid() == false) on:
//   - ca not valid
//   - member_stellar_address not a Stellar G... strkey
//   - tweetnacl keygen / OpenSSL X509 build / sign failure
//
// `member_name` goes into CN; empty defaults to "Inkternity Member".
// `member_canon_url` is the optional second SAN URI; empty omits it.
// `valid_for` defaults to 24 hours (matches the Python reference).
MemberLeaf issue_leaf(const AppCa& ca,
                      std::string_view member_stellar_address,
                      std::string_view member_name = {},
                      std::string_view member_canon_url = {},
                      std::chrono::hours valid_for = std::chrono::hours{24});

}  // namespace C2PA
