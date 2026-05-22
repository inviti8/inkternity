#include "LeafIssuer.hpp"

#include "AppCa.hpp"

#include <Helpers/Logger.hpp>

extern "C" {
#include "../../deps/tweetnacl/tweetnacl.h"
}

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace C2PA {

namespace {

// Mirror of the RAII deleters in AppCa.cpp — kept private here to
// avoid a header coupling. Same ownership story; the EVP_PKEY for
// the leaf is short-lived (constructed → used for X509_set_pubkey →
// freed after X509_sign).
struct PkeyDel  { void operator()(EVP_PKEY* p)         const noexcept { if (p) EVP_PKEY_free(p); } };
struct X509Del  { void operator()(X509* p)             const noexcept { if (p) X509_free(p); } };
struct NameDel  { void operator()(X509_NAME* p)        const noexcept { if (p) X509_NAME_free(p); } };
struct ExtDel   { void operator()(X509_EXTENSION* p)   const noexcept { if (p) X509_EXTENSION_free(p); } };
struct AsnIntDel{ void operator()(ASN1_INTEGER* p)     const noexcept { if (p) ASN1_INTEGER_free(p); } };
using PkeyPtr   = std::unique_ptr<EVP_PKEY,       PkeyDel>;
using X509Ptr   = std::unique_ptr<X509,           X509Del>;
using NamePtr   = std::unique_ptr<X509_NAME,      NameDel>;
using ExtPtr    = std::unique_ptr<X509_EXTENSION, ExtDel>;
using AsnIntPtr = std::unique_ptr<ASN1_INTEGER,   AsnIntDel>;

bool looks_like_stellar_pubkey(std::string_view s) {
    return s.size() == 56 && s.front() == 'G';
}

bool set_random_serial(X509* cert) {
    AsnIntPtr serial(ASN1_INTEGER_new());
    if (!serial) return false;
    uint8_t buf[20];
    if (RAND_bytes(buf, sizeof(buf)) != 1) return false;
    buf[0] &= 0x7f;
    if (buf[0] == 0) buf[0] = 1;
    BIGNUM* bn = BN_bin2bn(buf, sizeof(buf), nullptr);
    if (!bn) return false;
    ASN1_INTEGER* asn = BN_to_ASN1_INTEGER(bn, serial.get());
    BN_free(bn);
    if (!asn) return false;
    return X509_set_serialNumber(cert, serial.get()) == 1;
}

NamePtr build_leaf_subject(std::string_view member_name) {
    NamePtr name(X509_NAME_new());
    if (!name) return {};
    const std::string cn = member_name.empty()
        ? std::string("Inkternity Member")
        : std::string(member_name);
    if (X509_NAME_add_entry_by_NID(name.get(), NID_commonName, MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(cn.data()),
            static_cast<int>(cn.size()), -1, 0) != 1) return {};
    static const char kOrg[] = "Heavymeta Cooperative";
    if (X509_NAME_add_entry_by_NID(name.get(), NID_organizationName, MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(kOrg),
            static_cast<int>(sizeof(kOrg) - 1), -1, 0) != 1) return {};
    static const char kOu[] = "Members";
    if (X509_NAME_add_entry_by_NID(name.get(), NID_organizationalUnitName, MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(kOu),
            static_cast<int>(sizeof(kOu) - 1), -1, 0) != 1) return {};
    return name;
}

bool add_text_ext_nid(X509V3_CTX* ctx, X509* cert, int nid, const char* value) {
    ExtPtr ext(X509V3_EXT_conf_nid(nullptr, ctx, nid, value));
    if (!ext) return false;
    return X509_add_ext(cert, ext.get(), -1) == 1;
}

// Same OID-registration shape AppCa::generate uses. Safe to re-invoke;
// OBJ_create returns NID_undef the second time, we look up the
// existing NID via OBJ_txt2obj.
int ensure_c2pa_eku_nid() {
    int nid = OBJ_create(AppCa::C2PA_CLAIM_SIGNING_OID, "c2paClaimSigning",
                         "C2PA Claim Signing");
    if (nid == NID_undef) {
        ASN1_OBJECT* o = OBJ_txt2obj(AppCa::C2PA_CLAIM_SIGNING_OID, 1);
        if (!o) return NID_undef;
        nid = OBJ_obj2nid(o);
        ASN1_OBJECT_free(o);
    }
    return nid;
}

std::vector<uint8_t> serialize_der(X509* cert) {
    unsigned char* buf = nullptr;
    int len = i2d_X509(cert, &buf);
    if (len <= 0 || !buf) {
        if (buf) OPENSSL_free(buf);
        return {};
    }
    std::vector<uint8_t> out(buf, buf + len);
    OPENSSL_free(buf);
    return out;
}

}  // namespace

// ---- MemberLeaf rule-of-five ---------------------------------------------

MemberLeaf::MemberLeaf(MemberLeaf&& other) noexcept
    : cert_der_(std::move(other.cert_der_)),
      private_seed_(other.private_seed_),
      public_key_(other.public_key_) {
    other.private_seed_.fill(0);
    other.public_key_.fill(0);
}

MemberLeaf& MemberLeaf::operator=(MemberLeaf&& other) noexcept {
    if (this != &other) {
        // Wipe whatever this instance currently holds before stealing
        // the donor's bytes.
        OPENSSL_cleanse(private_seed_.data(), private_seed_.size());
        OPENSSL_cleanse(public_key_.data(),   public_key_.size());
        cert_der_     = std::move(other.cert_der_);
        private_seed_ = other.private_seed_;
        public_key_   = other.public_key_;
        other.private_seed_.fill(0);
        other.public_key_.fill(0);
    }
    return *this;
}

MemberLeaf::~MemberLeaf() noexcept {
    OPENSSL_cleanse(private_seed_.data(), private_seed_.size());
    OPENSSL_cleanse(public_key_.data(),   public_key_.size());
}

// ---- issue_leaf ----------------------------------------------------------

MemberLeaf issue_leaf(const AppCa& ca,
                      std::string_view member_stellar_address,
                      std::string_view member_name,
                      std::string_view member_canon_url,
                      std::chrono::hours valid_for) {
    MemberLeaf out;
    if (!ca.valid()) {
        Logger::get().log("INFO",
            "[C2PA::LeafIssuer] issue_leaf: invalid app CA");
        return out;
    }
    if (!looks_like_stellar_pubkey(member_stellar_address)) {
        Logger::get().log("INFO",
            "[C2PA::LeafIssuer] issue_leaf: member_stellar_address not G-strkey");
        return out;
    }
    if (valid_for.count() <= 0 || valid_for.count() > 24 * 365) {
        Logger::get().log("INFO",
            "[C2PA::LeafIssuer] issue_leaf: implausible valid_for");
        return out;
    }

    // ---- Tweetnacl Ed25519 keypair ---------------------------------------
    // crypto_sign_keypair writes 32 bytes of pubkey + 64 bytes of
    // "secret" = seed || pubkey. The first 32 secret bytes are the
    // seed (the canonical Ed25519 private key); the second 32 are
    // the pubkey embedded for fast signing without re-derivation.
    std::array<uint8_t, 32> pub{};
    std::array<uint8_t, 64> secret{};
    if (crypto_sign_keypair(pub.data(), secret.data()) != 0) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::LeafIssuer] crypto_sign_keypair failed");
        return out;
    }
    std::array<uint8_t, 32> seed{};
    std::memcpy(seed.data(), secret.data(), 32);
    // Wipe the larger tweetnacl secret buffer (we only need the seed
    // half going forward).
    OPENSSL_cleanse(secret.data(), secret.size());

    // ---- Wrap the seed in an EVP_PKEY so OpenSSL can use it as the
    //      leaf's pubkey input.
    PkeyPtr leafPkey(EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    if (!leafPkey) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::LeafIssuer] EVP_PKEY_new_raw_private_key failed");
        OPENSSL_cleanse(seed.data(), seed.size());
        return out;
    }

    // ---- Cert skeleton ----------------------------------------------------
    X509Ptr cert(X509_new());
    if (!cert) { OPENSSL_cleanse(seed.data(), seed.size()); return out; }
    if (X509_set_version(cert.get(), 2 /* V3 */) != 1) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    if (!set_random_serial(cert.get())) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    if (X509_set_pubkey(cert.get(), leafPkey.get()) != 1) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }

    NamePtr subject = build_leaf_subject(member_name);
    if (!subject) { OPENSSL_cleanse(seed.data(), seed.size()); return out; }
    if (X509_set_subject_name(cert.get(), subject.get()) != 1) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    // Issuer = CA's subject. X509_get_subject_name returns an
    // internal pointer owned by the CA cert; we don't free it.
    X509_NAME* caSubject = X509_get_subject_name(ca.cert());
    if (!caSubject || X509_set_issuer_name(cert.get(), caSubject) != 1) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }

    if (!X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0)) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    const long secs = static_cast<long>(valid_for.count()) * 60L * 60L;
    if (!X509_gmtime_adj(X509_getm_notAfter(cert.get()), secs)) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }

    // ---- Extensions -------------------------------------------------------
    // The leaf v3 context references the issuer (CA) for AKI lookups
    // and the subject (self) for SKI generation. X509V3_CTX has to
    // be initialized with both.
    X509V3_CTX v3ctx{};
    X509V3_set_ctx(&v3ctx, ca.cert(), cert.get(), nullptr, nullptr, 0);

    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_basic_constraints,
                          "critical,CA:FALSE")) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_key_usage,
                          "critical,digitalSignature,nonRepudiation")) {
        // OpenSSL spells contentCommitment as "nonRepudiation" in the
        // X509V3_EXT_conf text form — same OID, legacy name.
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    int ekuNid = ensure_c2pa_eku_nid();
    if (ekuNid == NID_undef) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    // Include id-kp-documentSigning (1.3.6.1.5.5.7.3.36) alongside the
    // Adobe-prefix c2paClaimSigning OID. c2pa-rs's trust policy
    // delegates EKU validation to a configurable allow-list; document
    // signing is the standardized one most policies recognize.
    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_ext_key_usage,
                          "critical,c2paClaimSigning,1.3.6.1.5.5.7.3.36")) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    // SubjectKeyIdentifier + AuthorityKeyIdentifier — required by
    // c2pa-rs's certificate_profile check (looks for both extensions
    // to build the chain). "hash" computes SHA-1 of the leaf pubkey;
    // "keyid:always" pulls the CA's SKI into the leaf's AKI. The
    // X509V3_CTX was set up with both certs above so OpenSSL has the
    // pubkeys it needs.
    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_subject_key_identifier,
                          "hash")) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    if (!add_text_ext_nid(&v3ctx, cert.get(), NID_authority_key_identifier,
                          "keyid:always")) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }

    // SAN URIs. Sanitize defensively — the same character checks
    // AppCa::generate uses to keep ',' and '\n' out of the comma-
    // delimited X509V3_EXT_conf value list.
    {
        std::string addr(member_stellar_address);
        std::string canon(member_canon_url);
        auto bad = [](char c) {
            return c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\0';
        };
        if (std::any_of(addr.begin(),  addr.end(),  bad)) {
            OPENSSL_cleanse(seed.data(), seed.size()); return out;
        }
        if (std::any_of(canon.begin(), canon.end(), bad)) {
            OPENSSL_cleanse(seed.data(), seed.size()); return out;
        }
        std::string san_value = "URI:stellar:" + addr;
        if (!canon.empty()) { san_value += ",URI:"; san_value += canon; }
        if (!add_text_ext_nid(&v3ctx, cert.get(), NID_subject_alt_name,
                              san_value.c_str())) {
            OPENSSL_cleanse(seed.data(), seed.size()); return out;
        }
    }

    // ---- Sign (Ed25519: digest = NULL) -----------------------------------
    if (X509_sign(cert.get(), ca.pkey(), nullptr) <= 0) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::LeafIssuer] X509_sign failed");
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }

    // ---- Serialize + assemble -------------------------------------------
    out.cert_der_mut() = serialize_der(cert.get());
    if (out.cert_der().empty()) {
        OPENSSL_cleanse(seed.data(), seed.size()); return out;
    }
    out.private_seed_mut() = seed;
    out.public_key_mut()   = pub;
    OPENSSL_cleanse(seed.data(), seed.size());
    return out;
}

}  // namespace C2PA
