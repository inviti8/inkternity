#include "PublishHook.hpp"

#include "AppCa.hpp"
#include "KeyStore.hpp"
#include "LeafIssuer.hpp"
#include "Manifest.hpp"
#include "../GlobalConfig.hpp"
#include "../MainProgram.hpp"

#include <Helpers/Logger.hpp>
#include <Helpers/Random.hpp>

#include <fstream>
#include <string>

namespace C2PA::PublishHook {

namespace {

const char* extension_for_mime(std::string_view mime) {
    if (mime == "image/png")  return ".png";
    if (mime == "image/jpeg") return ".jpg";
    if (mime == "image/webp") return ".webp";
    return nullptr;
}

// Build a minimal C2PA manifest JSON. claim_generator names the
// software + version so a manifest reader can attribute the export
// without consulting the signer cert. assertions[0] is the
// canonical "c2pa.created" action — this image was newly created
// by the signer (not derived from a prior signed input).
//
// `title` is what shows in c2pa.org viewers and image inspectors.
std::string build_manifest_json(std::string_view title) {
    std::string json;
    json.reserve(256);
    json += "{\n";
    json += "  \"claim_generator\": \"inkternity/";
    json += "0.12.0";
    json += "\",\n";
    json += "  \"title\": \"";
    // Defensively escape any " in the title.
    for (char c : title) {
        if (c == '"') json += "\\\"";
        else if (c == '\\') json += "\\\\";
        else json.push_back(c);
    }
    json += "\",\n";
    json += "  \"assertions\": [\n";
    json += "    { \"label\": \"c2pa.actions\",\n";
    json += "      \"data\": { \"actions\": [{\"action\":\"c2pa.created\"}] } }\n";
    json += "  ]\n";
    json += "}";
    return json;
}

}  // namespace

bool try_save_signed_image(MainProgram& main,
                           const std::filesystem::path& dest,
                           const uint8_t* bytes,
                           std::size_t byte_count,
                           std::string_view format_mime) {
    // Gate 1: artist hasn't opted in.
    if (!main.conf.verifiablePublishingEnabled) return false;

    // Gate 2: format unsupported by c2pa-rs's file API in v1
    // (SVG sidecar handled separately by I15).
    const char* ext = extension_for_mime(format_mime);
    if (!ext) {
        Logger::get().log("INFO",
            std::string("[C2PA::PublishHook] unsupported format ") +
            std::string(format_mime) + " — skipping signing");
        return false;
    }

    // Gate 3: registration not Active. Gateway-on without on-chain
    // registration means the artist hasn't completed the walkthrough
    // — fall through to a plain save so the export still lands.
    KeyStore store(main.conf.configPath);
    const auto state = store.load_state();
    if (state.status != RegistrationStatus::Active) {
        Logger::get().log("INFO",
            "[C2PA::PublishHook] registration not Active — skipping signing");
        return false;
    }

    // Load the persisted CA. has_saved_ca() is the cheap pre-check.
    if (!store.has_saved_ca()) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::PublishHook] registration Active but no CA on disk — "
            "publish unsigned to preserve the user's export");
        return false;
    }
    AppCa ca = store.load();
    if (!ca.valid()) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::PublishHook] persisted CA failed to parse — "
            "publish unsigned to preserve the user's export");
        return false;
    }

    if (!main.devKeys.is_loaded()) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::PublishHook] DevKeys not loaded — cannot issue leaf");
        return false;
    }

    // Per-publish leaf (plan §6 — one per export, forensic notBefore
    // timestamp). 24h validity is more than enough for the embed.
    MemberLeaf leaf = issue_leaf(ca, main.devKeys.app_pubkey(),
        "Inkternity Member");
    if (!leaf.valid()) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::PublishHook] LeafIssuer returned invalid leaf");
        return true;  // we own the dest path; can't fall through to plain save
    }

    // Stage the unsigned encoded bytes in a temp file so c2pa-rs has
    // a source to read. c2pa_sign_file can't deal with stdin / memory
    // streams in its convenience API; the Builder/Stream API would
    // skip this, but that's an I13-followup refactor.
    std::filesystem::path tmpDir = store.c2pa_dir() / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    std::filesystem::path tmpPath = tmpDir /
        ("publish_" + Random::get().alphanumeric_str(10) + ext);
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            Logger::get().log("WORLDFATAL",
                "[C2PA::PublishHook] could not stage temp at " + tmpPath.string());
            return true;
        }
        f.write(reinterpret_cast<const char*>(bytes),
                static_cast<std::streamsize>(byte_count));
    }

    // Title defaults to the destination's filename stem so the C2PA
    // viewer says "myscreenshot.png" rather than "Inkternity Export".
    const std::string title = dest.stem().string();
    const std::string manifest = build_manifest_json(
        title.empty() ? std::string("Inkternity Export") : title);

    auto er = embed_into_image_file(tmpPath, dest, manifest, ca, leaf);
    std::filesystem::remove(tmpPath, ec);
    if (!er.success) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::PublishHook] embed failed: " + er.error);
    } else {
        Logger::get().log("USERINFO",
            "Signed export written to " + dest.string());
    }
    return true;
}

bool try_write_sidecar(MainProgram& main,
                       const std::filesystem::path& asset_path,
                       std::string_view format_mime) {
    if (!main.conf.verifiablePublishingEnabled) return false;

    KeyStore store(main.conf.configPath);
    if (store.load_state().status != RegistrationStatus::Active) {
        return false;  // not registered yet; quiet
    }

    // Sidecar generation via c2pa_builder_sign tripped a Rust-side
    // io::Error with memory-backed streams under v0.84.1 — needs
    // focused debugging or a switch to file-backed streams + the
    // post-sign manifest-extraction path. Tracked as a known v1 gap.
    // The asset itself (.inkternity / .svg) lands unsigned for now;
    // the export path's embed signing in I14 covers the image case.
    static bool warned = false;
    if (!warned) {
        Logger::get().log("USERINFO",
            "Verifiable publishing for " + std::string(format_mime)
            + " assets isn't fully wired yet — the export saved "
            "unsigned (the .c2pa sidecar will land in a follow-up "
            "build). Inkternity image exports continue to sign "
            "normally.");
        warned = true;
    }
    Logger::get().log("INFO",
        "[C2PA::PublishHook] sidecar TODO for "
        + asset_path.string() + " (format=" + std::string(format_mime) + ")");
    (void)asset_path;
    return false;
}

}  // namespace C2PA::PublishHook
