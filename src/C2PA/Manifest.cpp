#include "Manifest.hpp"

#include "AppCa.hpp"
#include "LeafIssuer.hpp"

#include <Helpers/Logger.hpp>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

extern "C" {
#include "c2pa.h"
}

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace C2PA {

namespace {

// RAII deleters local to this TU (mirrors Manifest's policy of not
// leaking openssl/c2pa types through the public header).
struct BioDel  { void operator()(BIO* p)      const noexcept { if (p) BIO_free_all(p); } };
struct X509Del { void operator()(X509* p)     const noexcept { if (p) X509_free(p); } };
struct PkeyDel { void operator()(EVP_PKEY* p) const noexcept { if (p) EVP_PKEY_free(p); } };
using BioPtr  = std::unique_ptr<BIO,      BioDel>;
using X509Ptr = std::unique_ptr<X509,     X509Del>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDel>;

// Convert a leaf DER blob to a PEM-encoded cert string.
std::string der_to_pem(const std::vector<uint8_t>& der) {
    const unsigned char* p = der.data();
    X509Ptr cert(d2i_X509(nullptr, &p, static_cast<long>(der.size())));
    if (!cert) return {};
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) return {};
    if (PEM_write_bio_X509(bio.get(), cert.get()) != 1) return {};
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio.get(), &mem);
    if (!mem || mem->length == 0) return {};
    return std::string(mem->data, mem->length);
}

// PEM-encode the leaf's raw Ed25519 seed as PKCS#8 (unencrypted).
// The buffer is intended for one synchronous use by c2pa-rs; caller
// OPENSSL_cleanses it on return.
std::string seed_to_pkcs8_pem(const std::array<uint8_t, 32>& seed) {
    PkeyPtr pkey(EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    if (!pkey) return {};
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) return {};
    if (PEM_write_bio_PKCS8PrivateKey(bio.get(), pkey.get(),
            nullptr, nullptr, 0, nullptr, nullptr) != 1) return {};
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio.get(), &mem);
    if (!mem || mem->length == 0) return {};
    return std::string(mem->data, mem->length);
}

// Read c2pa_error() into a std::string. The error string is owned by
// the c2pa lib; caller must c2pa_free it.
std::string drain_c2pa_error() {
    char* err = c2pa_error();
    if (!err) return {};
    std::string out(err);
    c2pa_free(err);
    return out;
}

}  // namespace

// One-time-per-process settings load. c2pa-rs ships with a built-in
// trust list (CAI roots); our cert chain isn't on it, so sign-time
// trust verification would reject every Inkternity-issued manifest.
// Disable c2pa-rs's verify_after_sign + verify_trust — Inkternity's
// own verifier (I16) checks trust via the on-chain hvym_cert_registry
// instead, which is the authoritative trust path for this app's
// provenance model. c2pa_load_settings is thread-local; we apply it
// inside each embed/read call rather than once at process start, so
// background threads inherit the right config.
void apply_c2pa_settings_once() {
    static bool loaded = false;
    if (loaded) return;
    static constexpr const char* kJson = R"({
        "verify": {
            "verify_after_sign": false,
            "verify_trust": false
        }
    })";
    if (c2pa_load_settings(kJson, "json") != 0) {
        const std::string err = drain_c2pa_error();
        Logger::get().log("WORLDFATAL",
            "[C2PA::Manifest] c2pa_load_settings failed: " + err);
    }
    loaded = true;
}

EmbedResult embed_into_image_file(
        const std::filesystem::path& source,
        const std::filesystem::path& dest,
        std::string_view             manifest_json,
        const AppCa&                 ca,
        const MemberLeaf&            leaf) {
    EmbedResult r;
    if (!ca.valid() || !leaf.valid()) {
        r.error = "ca or leaf invalid";
        return r;
    }
    apply_c2pa_settings_once();

    Logger::get().log("INFO",
        "[C2PA::Manifest] embed " + source.string() + " -> " + dest.string());

    // Build the cert chain PEM (leaf || CA). c2pa-rs expects the
    // signing cert first, intermediates after, root last.
    std::string leafPem = der_to_pem(leaf.cert_der());
    auto caPemVec = ca.pem_bytes();
    if (leafPem.empty() || caPemVec.empty()) {
        r.error = "PEM conversion failed (leaf or CA)";
        return r;
    }
    std::string chainPem = leafPem +
        std::string(reinterpret_cast<const char*>(caPemVec.data()),
                    caPemVec.size());

    // PEM-encode the leaf private key. Lives only for the duration
    // of c2pa_sign_file; cleansed before this function returns.
    std::string keyPem = seed_to_pkcs8_pem(leaf.private_seed());
    if (keyPem.empty()) {
        r.error = "private key PEM conversion failed";
        return r;
    }

    C2paSignerInfo info{};
    info.alg         = "Ed25519";
    info.sign_cert   = chainPem.c_str();
    info.private_key = keyPem.c_str();
    info.ta_url      = nullptr;  // no timestamp authority in v1

    const std::string srcStr = source.string();
    const std::string dstStr = dest.string();
    const std::string mj(manifest_json);

    char* signResult = c2pa_sign_file(
        srcStr.c_str(),
        dstStr.c_str(),
        mj.c_str(),
        &info,
        /*data_dir=*/nullptr);

    // Best-effort wipe of the PEM buffer (c2pa-rs has presumably
    // copied the bytes by now; we can't be 100% sure but this is
    // the same memzero pattern as DevKeys / KeyStore).
    OPENSSL_cleanse(keyPem.data(), keyPem.size());

    // The c2pa-rs contract for the deprecated c2pa_sign_file is
    // genuinely ambiguous in the cbindgen header — "returns an error
    // field if there were errors" doesn't pin down NULL-vs-empty-vs-
    // error-bytes. Empirically (v0.84.x): on failure the returned
    // string is the failure message; on success c2pa_error() is
    // empty and the dest file is non-empty. We treat presence of
    // any c2pa_error message OR a zero-byte dest as failure.
    if (signResult) {
        if (*signResult) r.raw_out = signResult;
        c2pa_free(signResult);
    }
    const std::string err = drain_c2pa_error();
    std::error_code ec;
    const auto dstSize = std::filesystem::file_size(dest, ec);
    const bool dstEmpty = ec || dstSize == 0;
    if (!err.empty() || !r.raw_out.empty() || dstEmpty) {
        if (!err.empty()) r.error = err;
        else if (!r.raw_out.empty()) r.error = r.raw_out;
        else r.error = "destination file is empty after c2pa_sign_file";
        Logger::get().log("WORLDFATAL",
            "[C2PA::Manifest] embed FAILED: " + r.error);
        return r;
    }
    r.success = true;
    Logger::get().log("INFO",
        "[C2PA::Manifest] embed OK: " + dest.string()
        + " (" + std::to_string(dstSize) + " bytes)");
    return r;
}

ReadResult read_and_verify(const std::filesystem::path& source) {
    ReadResult r;
    apply_c2pa_settings_once();
    Logger::get().log("INFO",
        "[C2PA::Manifest] read " + source.string());
    const std::string srcStr = source.string();
    char* json = c2pa_read_file(srcStr.c_str(), /*data_dir=*/nullptr);
    if (!json) {
        const std::string err = drain_c2pa_error();
        // c2pa-rs returns NULL with an error message both when there's
        // no manifest AND when the file is unreadable. Treat
        // "No claim found" / similar as the no-manifest case (common,
        // every unsigned image) and everything else as an actual
        // error. The exact phrase varies by c2pa-rs version; sniff
        // for keywords.
        const bool likely_no_manifest =
            err.find("No claim") != std::string::npos ||
            err.find("no manifest") != std::string::npos ||
            err.find("Provenance") != std::string::npos;
        if (likely_no_manifest) {
            Logger::get().log("INFO",
                "[C2PA::Manifest] no manifest in " + source.string());
        } else {
            r.error = err.empty() ? std::string("unknown read error") : err;
            Logger::get().log("INFO",
                "[C2PA::Manifest] read error: " + r.error);
        }
        return r;
    }
    r.has_manifest  = true;
    r.manifest_json = json;
    c2pa_free(json);
    Logger::get().log("INFO",
        "[C2PA::Manifest] read OK; manifest_len="
        + std::to_string(r.manifest_json.size()));
    return r;
}

}  // namespace C2PA
