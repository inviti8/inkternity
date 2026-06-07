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
#include <fstream>
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

    // c2pa-rs refuses to overwrite an existing dest ("bad parameter:
    // Destination file already exists"). The caller's dest comes from a
    // save dialog where the artist already confirmed the overwrite (and
    // the unsigned SDL_SaveFile fallback overwrites unconditionally), so
    // match that semantic here.
    {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
    }

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

    // c2pa-rs C-FFI contract (pinned empirically with tools/c2pa_repro.cpp,
    // v0.84.1): on failure c2pa_sign_file sets the thread-local last-error
    // and returns NULL; on success it returns a (possibly empty) string.
    // Do NOT consult c2pa_error() to detect success — the last-error slot
    // is sticky, so a stale message from any earlier failed c2pa call on
    // this thread (e.g. the Verifier probing an .inkternity file, which
    // always fails with "NotSupported: type is unsupported") would
    // misreport a successful embed as failed. That exact misreport shipped
    // as the rc15 "save screen cap" bug.
    if (signResult) {
        if (*signResult) r.raw_out = signResult;
        c2pa_free(signResult);
    }
    std::error_code ec;
    const auto dstSize = std::filesystem::file_size(dest, ec);
    const bool dstEmpty = ec || dstSize == 0;
    if (!signResult || dstEmpty) {
        const std::string err = drain_c2pa_error();
        if (!err.empty()) r.error = err;
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

// ---- write_sidecar -------------------------------------------------------
//
// c2pa-rs's Builder API has c2pa_builder_set_no_embed which tells the
// signer to compute the hash binding + produce the manifest bytes but
// skip writing them into the asset. We pair that with memory-backed
// source / discard-dest streams to get raw manifest bytes out via
// c2pa_builder_sign's manifest_bytes_ptr out-param, then write to
// asset_path + ".c2pa".

namespace {

struct ROStreamCtx { const std::vector<uint8_t>* data; std::size_t pos; };
struct WOStreamCtx { std::vector<uint8_t>* buf; std::size_t pos = 0; };

extern "C" intptr_t ro_read(StreamContext* sc, uint8_t* out, intptr_t len) {
    auto* s = reinterpret_cast<ROStreamCtx*>(sc);
    if (!s || !s->data) return -1;
    const std::size_t avail =
        s->pos < s->data->size() ? s->data->size() - s->pos : 0;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(len), avail);
    if (n) std::memcpy(out, s->data->data() + s->pos, n);
    s->pos += n;
    return static_cast<intptr_t>(n);
}
extern "C" intptr_t ro_seek(StreamContext* sc, intptr_t offset, C2paSeekMode mode) {
    auto* s = reinterpret_cast<ROStreamCtx*>(sc);
    if (!s || !s->data) return -1;
    int64_t pos = static_cast<int64_t>(s->pos);
    switch (mode) {
        case C2paSeekMode::Start:   pos = offset; break;
        case C2paSeekMode::Current: pos += offset; break;
        case C2paSeekMode::End:
            pos = static_cast<int64_t>(s->data->size()) + offset;
            break;
    }
    if (pos < 0) return -1;
    if (pos > static_cast<int64_t>(s->data->size())) {
        pos = static_cast<int64_t>(s->data->size());
    }
    s->pos = static_cast<std::size_t>(pos);
    return pos;
}
extern "C" intptr_t ro_write(StreamContext*, const uint8_t*, intptr_t) { return -1; }
extern "C" intptr_t ro_flush(StreamContext*) { return 0; }

extern "C" intptr_t wo_read(StreamContext* sc, uint8_t* out, intptr_t len) {
    auto* s = reinterpret_cast<WOStreamCtx*>(sc);
    if (!s || !s->buf) return -1;
    const std::size_t avail =
        s->pos < s->buf->size() ? s->buf->size() - s->pos : 0;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(len), avail);
    if (n) std::memcpy(out, s->buf->data() + s->pos, n);
    s->pos += n;
    return static_cast<intptr_t>(n);
}
extern "C" intptr_t wo_seek(StreamContext* sc, intptr_t offset, C2paSeekMode mode) {
    auto* s = reinterpret_cast<WOStreamCtx*>(sc);
    if (!s || !s->buf) return -1;
    int64_t pos = static_cast<int64_t>(s->pos);
    switch (mode) {
        case C2paSeekMode::Start:   pos = offset; break;
        case C2paSeekMode::Current: pos += offset; break;
        case C2paSeekMode::End:
            pos = static_cast<int64_t>(s->buf->size()) + offset;
            break;
    }
    if (pos < 0) return -1;
    s->pos = static_cast<std::size_t>(pos);
    if (s->pos > s->buf->size())
        s->buf->resize(s->pos, 0);
    return static_cast<intptr_t>(s->pos);
}
extern "C" intptr_t wo_write(StreamContext* sc, const uint8_t* data, intptr_t len) {
    auto* s = reinterpret_cast<WOStreamCtx*>(sc);
    if (!s || !s->buf) return -1;
    const std::size_t end = s->pos + static_cast<std::size_t>(len);
    if (end > s->buf->size())
        s->buf->resize(end);
    std::memcpy(s->buf->data() + s->pos, data, static_cast<std::size_t>(len));
    s->pos = end;
    return len;
}
extern "C" intptr_t wo_flush(StreamContext*) { return 0; }

}  // namespace

EmbedResult write_sidecar(const std::filesystem::path& asset_path,
                          std::string_view             manifest_json,
                          std::string_view             format_mime,
                          const AppCa&                 ca,
                          const MemberLeaf&            leaf) {
    EmbedResult r;
    if (!ca.valid() || !leaf.valid()) {
        r.error = "ca or leaf invalid";
        return r;
    }
    apply_c2pa_settings_once();

    Logger::get().log("INFO",
        "[C2PA::Manifest] sidecar " + asset_path.string()
        + " (format=" + std::string(format_mime) + ")");

    // Load the asset bytes into memory.
    std::vector<uint8_t> assetBytes;
    {
        std::ifstream f(asset_path, std::ios::binary | std::ios::ate);
        if (!f) {
            r.error = "could not open asset: " + asset_path.string();
            return r;
        }
        const std::streamsize sz = f.tellg();
        if (sz <= 0) {
            r.error = "asset is empty";
            return r;
        }
        f.seekg(0, std::ios::beg);
        assetBytes.resize(static_cast<std::size_t>(sz));
        f.read(reinterpret_cast<char*>(assetBytes.data()), sz);
    }

    // Build cert chain + private key PEM (same shape as embed_into_image_file).
    std::string leafPem = der_to_pem(leaf.cert_der());
    auto caPemVec = ca.pem_bytes();
    if (leafPem.empty() || caPemVec.empty()) {
        r.error = "PEM conversion failed (leaf or CA)";
        return r;
    }
    std::string chainPem = leafPem +
        std::string(reinterpret_cast<const char*>(caPemVec.data()),
                    caPemVec.size());
    std::string keyPem = seed_to_pkcs8_pem(leaf.private_seed());
    if (keyPem.empty()) {
        r.error = "private key PEM conversion failed";
        return r;
    }

    C2paSignerInfo info{};
    info.alg         = "Ed25519";
    info.sign_cert   = chainPem.c_str();
    info.private_key = keyPem.c_str();
    info.ta_url      = nullptr;
    C2paSigner* signer = c2pa_signer_from_info(&info);
    OPENSSL_cleanse(keyPem.data(), keyPem.size());
    if (!signer) {
        r.error = "c2pa_signer_from_info failed: " + drain_c2pa_error();
        return r;
    }

    const std::string mj(manifest_json);
    C2paBuilder* builder = c2pa_builder_from_json(mj.c_str());
    if (!builder) {
        c2pa_free(signer);
        r.error = "c2pa_builder_from_json failed: " + drain_c2pa_error();
        return r;
    }
    // Skipping c2pa_builder_set_no_embed in v1 — leaving it on tripped
    // an Io error in c2pa-rs's stream path. With embed enabled, dest
    // ends up with the asset + embedded manifest; we keep
    // manifest_bytes (which is also populated) for the sidecar write
    // and discard dest. Cost is one extra in-memory copy of the
    // signed asset, which is fine for `.inkternity` + SVG sizes.

    ROStreamCtx srcCtx{ &assetBytes, 0 };
    std::vector<uint8_t> discardBuf;
    WOStreamCtx dstCtx{ &discardBuf };

    C2paStream* srcStream = c2pa_create_stream(
        reinterpret_cast<StreamContext*>(&srcCtx),
        ro_read, ro_seek, ro_write, ro_flush);
    C2paStream* dstStream = c2pa_create_stream(
        reinterpret_cast<StreamContext*>(&dstCtx),
        wo_read, wo_seek, wo_write, wo_flush);
    if (!srcStream || !dstStream) {
        if (srcStream) c2pa_release_stream(srcStream);
        if (dstStream) c2pa_release_stream(dstStream);
        c2pa_free(builder);
        c2pa_free(signer);
        r.error = "c2pa_create_stream failed";
        return r;
    }

    const std::string fmt(format_mime);
    const unsigned char* manifestBytes = nullptr;
    int64_t sz = c2pa_builder_sign(builder, fmt.c_str(),
                                    srcStream, dstStream,
                                    signer, &manifestBytes);

    c2pa_release_stream(srcStream);
    c2pa_release_stream(dstStream);
    c2pa_free(builder);
    c2pa_free(signer);

    if (sz < 0 || !manifestBytes) {
        r.error = "c2pa_builder_sign failed: " + drain_c2pa_error();
        return r;
    }

    // Write the manifest bytes to <asset>.c2pa.
    std::filesystem::path sidecarPath = asset_path;
    sidecarPath += ".c2pa";
    {
        std::ofstream f(sidecarPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            c2pa_free(const_cast<unsigned char*>(manifestBytes));
            r.error = "could not open sidecar for write: " + sidecarPath.string();
            return r;
        }
        f.write(reinterpret_cast<const char*>(manifestBytes),
                static_cast<std::streamsize>(sz));
    }
    c2pa_free(const_cast<unsigned char*>(manifestBytes));

    Logger::get().log("INFO",
        "[C2PA::Manifest] sidecar OK: " + sidecarPath.string()
        + " (" + std::to_string(sz) + " bytes)");
    r.success = true;
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
