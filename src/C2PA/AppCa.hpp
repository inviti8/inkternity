#pragma once
// docs/design/C2PA.md §2, §5, §8.1 — self-signed Ed25519 X.509 app CA
// per heavymeta_collective/C2PA.md §4.2. Mirror of
// mock_c2pa/andromica/ca_generation.py::AppCa / generate_app_ca.
//
// Holds both the CA's private key and the cert. Move-only — copies are
// deleted because the private key is sensitive and ad-hoc duplication
// is never the right call. The destructor zeroes key material via
// EVP_PKEY_free (which OPENSSL_cleanses the internal scalar).
//
// This module does not touch disk. KeyStore (I3) handles persistence;
// LeafIssuer (I12) consumes the CA to sign per-publish leaves.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare OpenSSL handles so consumers don't pull in <openssl/*>.
typedef struct evp_pkey_st EVP_PKEY;
typedef struct x509_st X509;

namespace C2PA {

class AppCa {
public:
    AppCa() noexcept = default;
    AppCa(const AppCa&) = delete;
    AppCa& operator=(const AppCa&) = delete;
    AppCa(AppCa&& other) noexcept;
    AppCa& operator=(AppCa&& other) noexcept;
    ~AppCa() noexcept;

    // Encoded EKU OID for C2PA claim signing — exposed for cross-module
    // assertions (verifier wrapper in I16 checks the same OID).
    static constexpr const char* C2PA_CLAIM_SIGNING_OID = "1.3.6.1.4.1.42038.1.5.0";

    bool valid() const noexcept { return pkey_ && cert_; }

    // Build a fresh self-signed Ed25519 CA with the four extensions per
    // C2PA.md §3:
    //   - BasicConstraints CA:TRUE pathlen:0  (critical)
    //   - KeyUsage digitalSignature + keyCertSign  (critical)
    //   - ExtendedKeyUsage C2PA_CLAIM_SIGNING_OID  (critical)
    //   - SubjectAlternativeName URIs
    //       stellar:<app_stellar_address>
    //       heavymeta:app/<app_name>
    //     (non-critical)
    //
    // Returns an invalid AppCa (valid() == false) on:
    //   - app_stellar_address is not a Stellar G... strkey (56 chars, leading 'G')
    //   - libcrypto keygen / encoding / sign failure (OOM, RNG miss, etc.)
    //
    // `valid_days` defaults to 10 years (matches the Python reference).
    static AppCa generate(std::string_view app_name,
                          std::string_view app_stellar_address,
                          int valid_days = 3650);

    // Load an existing CA from PKCS#8 PEM private key + PEM cert (the
    // shape KeyStore writes to disk in I3). Returns invalid AppCa on
    // any parse / load failure. Does NOT re-verify the chain — that's
    // the verifier's job at use-time.
    static AppCa load_from_pem(std::string_view private_key_pem,
                               std::string_view cert_pem);

    // DER-encoded X.509 cert. Empty on !valid().
    std::vector<uint8_t> der_bytes() const;

    // PEM-encoded X.509 cert. Empty on !valid().
    std::vector<uint8_t> pem_bytes() const;

    // SHA-256 over der_bytes(). Zeroed on !valid(). This is the
    // `fingerprint` value the contract stores in AppCaRecord.
    std::array<uint8_t, 32> fingerprint_sha256() const;

    // notAfter as Unix timestamp (UTC seconds). 0 on !valid().
    int64_t expires_at_unix() const;

    // Raw 32-byte Ed25519 public key of the CA. Zeroed on !valid().
    std::array<uint8_t, 32> public_key_raw() const;

    // PKCS#8 PEM encoding of the private key, unencrypted. Empty on
    // !valid(). KeyStore (I3) writes this to <configPath>/c2pa/app_ca.key
    // with 0600 / NTFS-user-only permissions.
    std::vector<uint8_t> private_key_pem() const;

    // Internal use by LeafIssuer (I12) — the CA signs the leaf CSR with
    // its private key, and the leaf's issuer Name is the CA's subject.
    // Returned pointers are owned by this AppCa; do not free.
    EVP_PKEY* pkey() const noexcept { return pkey_; }
    X509*     cert() const noexcept { return cert_; }

private:
    AppCa(EVP_PKEY* pkey, X509* cert) noexcept : pkey_(pkey), cert_(cert) {}

    EVP_PKEY* pkey_ = nullptr;
    X509*     cert_ = nullptr;
};

}  // namespace C2PA
