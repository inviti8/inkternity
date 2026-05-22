#include "AppCa.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace C2PA {

namespace {

// ---- RAII wrappers for OpenSSL handles ------------------------------------

struct OsslFree { void operator()(void* p) const noexcept { if (p) OPENSSL_free(p); } };
struct BioDel   { void operator()(BIO* p)             const noexcept { if (p) BIO_free_all(p); } };
struct PkeyCtxDel { void operator()(EVP_PKEY_CTX* p)  const noexcept { if (p) EVP_PKEY_CTX_free(p); } };
struct PkeyDel  { void operator()(EVP_PKEY* p)        const noexcept { if (p) EVP_PKEY_free(p); } };
struct X509Del  { void operator()(X509* p)            const noexcept { if (p) X509_free(p); } };
struct NameDel  { void operator()(X509_NAME* p)       const noexcept { if (p) X509_NAME_free(p); } };
struct ExtDel   { void operator()(X509_EXTENSION* p)  const noexcept { if (p) X509_EXTENSION_free(p); } };
struct AsnIntDel{ void operator()(ASN1_INTEGER* p)    const noexcept { if (p) ASN1_INTEGER_free(p); } };

using BioPtr     = std::unique_ptr<BIO,           BioDel>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX,  PkeyCtxDel>;
using PkeyPtr    = std::unique_ptr<EVP_PKEY,      PkeyDel>;
using X509Ptr    = std::unique_ptr<X509,          X509Del>;
using NamePtr    = std::unique_ptr<X509_NAME,     NameDel>;
using ExtPtr     = std::unique_ptr<X509_EXTENSION,ExtDel>;
using AsnIntPtr  = std::unique_ptr<ASN1_INTEGER,  AsnIntDel>;

// ---- Helpers --------------------------------------------------------------

// Stellar G... strkey check (cheap; doesn't validate base32 / CRC since
// that's a separate concern — Stellar::decode_pubkey would, but the cert
// content doesn't require the bytes, only the string form).
bool looks_like_stellar_pubkey(std::string_view s) {
    return s.size() == 56 && s.front() == 'G';
}

// Set a random 159-bit serial (RFC 5280 says <= 20 bytes; positive). The
// CertificateBuilder.random_serial_number() in cryptography uses 160 bits;
// we cap the top bit to 0 to ensure DER-positive without an extra zero byte.
bool set_random_serial(X509* cert) {
    AsnIntPtr serial(ASN1_INTEGER_new());
    if (!serial) return false;
    uint8_t buf[20];
    if (RAND_bytes(buf, sizeof(buf)) != 1) return false;
    buf[0] &= 0x7f;  // top bit clear → positive after BN_to_ASN1_INTEGER
    if (buf[0] == 0) buf[0] = 1;  // avoid an all-zero leading byte
    BIGNUM* bn = BN_bin2bn(buf, sizeof(buf), nullptr);
    if (!bn) return false;
    ASN1_INTEGER* asn = BN_to_ASN1_INTEGER(bn, serial.get());
    BN_free(bn);
    if (!asn) return false;
    if (X509_set_serialNumber(cert, serial.get()) != 1) return false;
    return true;
}

// Build "CN=<app_name> Instance, O=Heavymeta Cooperative, OU=App Instances".
NamePtr build_subject_issuer(std::string_view app_name) {
    NamePtr name(X509_NAME_new());
    if (!name) return {};
    const std::string cn = std::string(app_name) + " Instance";
    if (X509_NAME_add_entry_by_NID(name.get(), NID_commonName, MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(cn.data()),
            static_cast<int>(cn.size()), -1, 0) != 1) return {};
    static const char kOrg[] = "Heavymeta Cooperative";
    if (X509_NAME_add_entry_by_NID(name.get(), NID_organizationName, MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(kOrg),
            static_cast<int>(sizeof(kOrg) - 1), -1, 0) != 1) return {};
    static const char kOu[] = "App Instances";
    if (X509_NAME_add_entry_by_NID(name.get(), NID_organizationalUnitName, MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(kOu),
            static_cast<int>(sizeof(kOu) - 1), -1, 0) != 1) return {};
    return name;
}

// Add a single text-config extension via X509V3_EXT_conf_nid (handles
// "critical," prefix and known shorthand like "CA:TRUE,pathlen:0").
bool add_text_ext_nid(X509V3_CTX* ctx, X509* cert, int nid, const char* value) {
    ExtPtr ext(X509V3_EXT_conf_nid(nullptr, ctx, nid, value));
    if (!ext) return false;
    if (X509_add_ext(cert, ext.get(), -1) != 1) return false;
    return true;
}

// Add the C2PA critical EKU. Built via the dotted OID string since
// libcrypto doesn't ship 1.3.6.1.4.1.42038.1.5.0 in its NID table.
bool add_c2pa_eku(X509V3_CTX* ctx, X509* cert) {
    // Register a temporary short name → OID mapping so X509V3_EXT_conf
    // can resolve "c2paClaimSigning" inside the EKU value. OBJ_create
    // returns the NID; on a second call with the same OID it returns
    // the existing NID. Safe to call repeatedly across multiple
    // generate() invocations within one process.
    int nid = OBJ_create(AppCa::C2PA_CLAIM_SIGNING_OID, "c2paClaimSigning",
                         "C2PA Claim Signing");
    if (nid == NID_undef) {
        // Already registered earlier in this process — look it up.
        ASN1_OBJECT* o = OBJ_txt2obj(AppCa::C2PA_CLAIM_SIGNING_OID, 1);
        if (!o) return false;
        nid = OBJ_obj2nid(o);
        ASN1_OBJECT_free(o);
        if (nid == NID_undef) return false;
    }
    return add_text_ext_nid(ctx, cert, NID_ext_key_usage,
                            "critical,c2paClaimSigning");
}

// Read X.509 notAfter as a UTC Unix timestamp. Returns 0 on parse failure
// (unlikely for a cert we just constructed).
int64_t x509_not_after_unix(const X509* cert) {
    const ASN1_TIME* t = X509_get0_notAfter(cert);
    if (!t) return 0;
    struct tm tm_utc{};
    if (ASN1_TIME_to_tm(t, &tm_utc) != 1) return 0;
#ifdef _WIN32
    return static_cast<int64_t>(_mkgmtime(&tm_utc));
#else
    return static_cast<int64_t>(timegm(&tm_utc));
#endif
}

// Serialize a cert to a DER byte vector. Empty on failure.
std::vector<uint8_t> serialize_der(const X509* cert) {
    unsigned char* buf = nullptr;
    int len = i2d_X509(const_cast<X509*>(cert), &buf);
    if (len <= 0 || !buf) {
        if (buf) OPENSSL_free(buf);
        return {};
    }
    std::vector<uint8_t> out(buf, buf + len);
    OPENSSL_free(buf);
    return out;
}

// Read everything from a BIO into a std::vector<uint8_t>.
std::vector<uint8_t> bio_drain(BIO* bio) {
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    if (!mem || mem->length == 0) return {};
    return std::vector<uint8_t>(mem->data, mem->data + mem->length);
}

}  // namespace

// ---- AppCa rule-of-five ---------------------------------------------------

AppCa::AppCa(AppCa&& other) noexcept : pkey_(other.pkey_), cert_(other.cert_) {
    other.pkey_ = nullptr;
    other.cert_ = nullptr;
}

AppCa& AppCa::operator=(AppCa&& other) noexcept {
    if (this != &other) {
        if (pkey_) EVP_PKEY_free(pkey_);
        if (cert_) X509_free(cert_);
        pkey_ = other.pkey_;
        cert_ = other.cert_;
        other.pkey_ = nullptr;
        other.cert_ = nullptr;
    }
    return *this;
}

AppCa::~AppCa() noexcept {
    if (pkey_) EVP_PKEY_free(pkey_);
    if (cert_) X509_free(cert_);
}

// ---- generate -------------------------------------------------------------

AppCa AppCa::generate(std::string_view app_name,
                      std::string_view app_stellar_address,
                      int valid_days) {
    if (!looks_like_stellar_pubkey(app_stellar_address)) return {};
    if (valid_days <= 0 || valid_days > 100 * 365) return {};

    // ---- Ed25519 keypair --------------------------------------------------
    PkeyCtxPtr kctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr));
    if (!kctx || EVP_PKEY_keygen_init(kctx.get()) <= 0) return {};
    EVP_PKEY* raw_pkey = nullptr;
    if (EVP_PKEY_keygen(kctx.get(), &raw_pkey) <= 0) return {};
    PkeyPtr pkey(raw_pkey);

    // ---- Cert skeleton ----------------------------------------------------
    X509Ptr cert(X509_new());
    if (!cert) return {};
    if (X509_set_version(cert.get(), 2 /* V3 */) != 1) return {};
    if (!set_random_serial(cert.get())) return {};
    if (X509_set_pubkey(cert.get(), pkey.get()) != 1) return {};

    NamePtr name = build_subject_issuer(app_name);
    if (!name) return {};
    if (X509_set_subject_name(cert.get(), name.get()) != 1) return {};
    if (X509_set_issuer_name(cert.get(), name.get())  != 1) return {};

    if (!X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0)) return {};
    if (!X509_gmtime_adj(X509_getm_notAfter(cert.get()),
            static_cast<long>(valid_days) * 24L * 60L * 60L)) return {};

    // ---- Extensions -------------------------------------------------------
    X509V3_CTX v3ctx{};
    X509V3_set_ctx(&v3ctx, cert.get(), cert.get(), nullptr, nullptr, 0);

    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_basic_constraints,
                          "critical,CA:TRUE,pathlen:0")) return {};
    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_key_usage,
                          "critical,digitalSignature,keyCertSign")) return {};
    if (!add_c2pa_eku(&v3ctx, cert.get())) return {};
    // SubjectKeyIdentifier: 160-bit SHA-1 of the pubkey by default.
    // c2pa-rs requires it on the CA so leaves can declare their AKI
    // via "keyid:always" and the chain validator can match them.
    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_subject_key_identifier,
                          "hash")) return {};

    // SAN URIs: stellar:G... + heavymeta:app/<name>. X509V3_EXT_conf
    // takes a comma-separated value list. The strings can't contain '\n'
    // or null bytes; the Stellar address is 56 base32 chars and the
    // app_name is caller-supplied — sanitize defensively below.
    {
        std::string sa(app_stellar_address);
        std::string an(app_name);
        auto bad_char = [](char c) {
            return c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\0';
        };
        if (std::any_of(sa.begin(), sa.end(), bad_char)) return {};
        if (std::any_of(an.begin(), an.end(), bad_char)) return {};
        const std::string san_value =
            "URI:stellar:" + sa + ",URI:heavymeta:app/" + an;
        if (!add_text_ext_nid(&v3ctx, cert.get(), NID_subject_alt_name,
                              san_value.c_str())) return {};
    }

    // ---- Sign (Ed25519: digest=NULL, sign-without-prehash) ----------------
    if (X509_sign(cert.get(), pkey.get(), nullptr) <= 0) return {};

    return AppCa{pkey.release(), cert.release()};
}

// ---- load_from_pem --------------------------------------------------------

AppCa AppCa::load_from_pem(std::string_view private_key_pem,
                           std::string_view cert_pem) {
    BioPtr keyBio(BIO_new_mem_buf(private_key_pem.data(),
                                  static_cast<int>(private_key_pem.size())));
    BioPtr certBio(BIO_new_mem_buf(cert_pem.data(),
                                   static_cast<int>(cert_pem.size())));
    if (!keyBio || !certBio) return {};

    PkeyPtr pkey(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
    X509Ptr cert(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
    if (!pkey || !cert) return {};

    // Sanity: the cert's pubkey must match the loaded private key. Defends
    // against half-overwritten KeyStore state on disk.
    if (X509_check_private_key(cert.get(), pkey.get()) != 1) return {};

    return AppCa{pkey.release(), cert.release()};
}

// ---- accessors ------------------------------------------------------------

std::vector<uint8_t> AppCa::der_bytes() const {
    if (!valid()) return {};
    return serialize_der(cert_);
}

std::vector<uint8_t> AppCa::pem_bytes() const {
    if (!valid()) return {};
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) return {};
    if (PEM_write_bio_X509(bio.get(), cert_) != 1) return {};
    return bio_drain(bio.get());
}

std::array<uint8_t, 32> AppCa::fingerprint_sha256() const {
    std::array<uint8_t, 32> out{};
    if (!valid()) return out;
    const auto der = serialize_der(cert_);
    if (der.empty()) return out;
    SHA256(der.data(), der.size(), out.data());
    return out;
}

int64_t AppCa::expires_at_unix() const {
    if (!valid()) return 0;
    return x509_not_after_unix(cert_);
}

std::array<uint8_t, 32> AppCa::public_key_raw() const {
    std::array<uint8_t, 32> out{};
    if (!valid()) return out;
    size_t len = out.size();
    if (EVP_PKEY_get_raw_public_key(pkey_, out.data(), &len) != 1 || len != 32) {
        out.fill(0);
    }
    return out;
}

std::vector<uint8_t> AppCa::private_key_pem() const {
    if (!valid()) return {};
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) return {};
    if (PEM_write_bio_PKCS8PrivateKey(bio.get(), pkey_, nullptr,
                                      nullptr, 0, nullptr, nullptr) != 1) {
        return {};
    }
    return bio_drain(bio.get());
}

}  // namespace C2PA
