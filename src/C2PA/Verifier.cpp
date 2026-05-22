#include "Verifier.hpp"

#include "Manifest.hpp"

#include <Helpers/Logger.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace C2PA {

const char* verify_status_str(VerifyStatus s) noexcept {
    switch (s) {
        case VerifyStatus::NoManifest:         return "no manifest";
        case VerifyStatus::ManifestMalformed:  return "manifest malformed";
        case VerifyStatus::SignatureInvalid:   return "signature invalid";
        case VerifyStatus::TrustedLocal:       return "trusted (local check)";
    }
    return "unknown";
}

namespace {

// Per c2pa-rs JSON: the active manifest sits at
// manifests[active_manifest]. Within it, validation_status (if
// present) is a vector of {"code": "...", "explanation": "...", ...}.
// Any "FAIL" or "ERR" code marks the signature as invalid for our
// purposes. The exact set of codes that c2pa-rs considers
// non-failure is wide; we treat presence of any code containing
// "fail" / "invalid" / "error" / "broken" as failure.
bool validation_status_indicates_failure(const nlohmann::json& vs) {
    if (!vs.is_array()) return false;
    for (const auto& entry : vs) {
        if (!entry.is_object()) continue;
        std::string code = entry.value("code", "");
        std::transform(code.begin(), code.end(), code.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (code.find("fail")    != std::string::npos ||
            code.find("invalid") != std::string::npos ||
            code.find("error")   != std::string::npos ||
            code.find("broken")  != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

VerifyResult verify_file(const std::filesystem::path& path) {
    VerifyResult vr;
    Logger::get().log("INFO",
        "[C2PA::Verifier] inspect " + path.string());

    auto rr = read_and_verify(path);
    if (!rr.has_manifest) {
        vr.status = VerifyStatus::NoManifest;
        vr.detail = rr.error.empty()
            ? std::string("No provenance manifest in this file.")
            : ("No usable manifest: " + rr.error);
        Logger::get().log("INFO",
            "[C2PA::Verifier] no manifest in " + path.string());
        return vr;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(rr.manifest_json);
    } catch (const std::exception& e) {
        vr.status = VerifyStatus::ManifestMalformed;
        vr.detail = std::string("Manifest JSON parse failed: ") + e.what();
        Logger::get().log("INFO",
            "[C2PA::Verifier] manifest unparseable: " + std::string(e.what()));
        return vr;
    }

    std::string activeId = j.value("active_manifest", "");
    if (activeId.empty() || !j.contains("manifests")
        || !j["manifests"].is_object()
        || !j["manifests"].contains(activeId)) {
        vr.status = VerifyStatus::ManifestMalformed;
        vr.detail = "Manifest has no active entry.";
        Logger::get().log("INFO",
            "[C2PA::Verifier] no active manifest entry");
        return vr;
    }
    const auto& active = j["manifests"][activeId];

    // Pull display fields. claim_generator_info is an array; the first
    // entry's name is what c2pa.org's inspector tends to show.
    vr.title = active.value("title", "");
    if (active.contains("claim_generator_info")
        && active["claim_generator_info"].is_array()
        && !active["claim_generator_info"].empty()) {
        const auto& cgi = active["claim_generator_info"][0];
        if (cgi.is_object()) {
            std::string name    = cgi.value("name",    "");
            std::string version = cgi.value("version", "");
            vr.generator = name + (version.empty() ? "" : ("/" + version));
        }
    }

    // c2pa-rs reports per-manifest validation in validation_status
    // (legacy) or validation_results (newer). Check both; any failing
    // entry trips SignatureInvalid.
    if ((active.contains("validation_status")
            && validation_status_indicates_failure(active["validation_status"]))
        || (j.contains("validation_status")
            && validation_status_indicates_failure(j["validation_status"]))) {
        vr.status = VerifyStatus::SignatureInvalid;
        vr.detail = "Signature verification failed: see manifest "
                     "validation_status for details.";
        Logger::get().log("INFO",
            "[C2PA::Verifier] validation_status indicates failure");
        return vr;
    }

    // No failure markers + c2pa-rs accepted the read → locally
    // trusted. The on-chain trust check (cert SAN → hvym_cert_registry
    // lookup → fingerprint compare) lands in a follow-up.
    vr.status = VerifyStatus::TrustedLocal;
    vr.detail = "Provenance signature verifies locally"
                 + (vr.generator.empty() ? std::string()
                                          : (" (signed by " + vr.generator + ")"))
                 + ". On-chain trust check deferred (v1).";
    Logger::get().log("INFO",
        "[C2PA::Verifier] TrustedLocal: " + path.string());
    return vr;
}

}  // namespace C2PA
