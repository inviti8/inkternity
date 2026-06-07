// Standalone repro for the C2PA embed "NotSupported: type is unsupported"
// failure seen on screenshot export (error.png, 2026-06-06). Reuses the
// exact production path: AppCa::generate -> issue_leaf ->
// embed_into_image_file, against caller-supplied image files.
//
// Usage: c2pa_repro <image> [<image> ...]
// For each input writes <image>.signed<ext> and prints the EmbedResult.

#include "../src/C2PA/AppCa.hpp"
#include "../src/C2PA/LeafIssuer.hpp"
#include "../src/C2PA/Manifest.hpp"

#include <Helpers/Logger.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {
std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: c2pa_repro [--ca-dir <dir-with-app_ca.key/.crt>] <image> [<image> ...]\n";
        return 2;
    }

    // Manifest.cpp logs through these channels; route them all to stdout.
    for (const char* chan : {"INFO", "WORLDFATAL", "USERINFO", "ERROR"})
        Logger::get().add_log(chan, [chan](const std::string& t) {
            std::cout << "[" << chan << "] " << t << std::endl;
        });

    // Fake-but-shape-valid Stellar address (AppCa only checks 56 chars + 'G').
    const std::string stellar(56, 'A');

    int argi = 1;
    C2PA::AppCa ca;
    if (std::string(argv[1]) == "--ca-dir" && argc >= 4) {
        const std::filesystem::path dir = argv[2];
        argi = 3;
        ca = C2PA::AppCa::load_from_pem(slurp(dir / "app_ca.key"),
                                        slurp(dir / "app_ca.crt"));
        if (!ca.valid()) { std::cerr << "AppCa::load_from_pem failed for " << dir << "\n"; return 1; }
        std::cout << "(using persisted CA from " << dir << ")\n";
    } else {
        ca = C2PA::AppCa::generate("Inkternity", "G" + stellar.substr(1));
        if (!ca.valid()) { std::cerr << "AppCa::generate failed\n"; return 1; }
    }

    C2PA::MemberLeaf leaf = C2PA::issue_leaf(ca, "G" + stellar.substr(1), "Inkternity Member");
    if (!leaf.valid()) { std::cerr << "issue_leaf failed\n"; return 1; }

    // Same manifest shape PublishHook builds.
    const std::string manifest = R"({
  "claim_generator": "inkternity/0.12.0",
  "title": "repro",
  "assertions": [
    { "label": "c2pa.actions",
      "data": { "actions": [{"action":"c2pa.created"}] } }
  ]
})";

    // Mimic the production session: the Verifier panel read_and_verify()s
    // the .inkternity canvas on load, which fails with UnsupportedType.
    // If c2pa_error() is a sticky last-error slot, that stale error will
    // make the NEXT (successful) embed misreport as failed.
    {
        const std::filesystem::path fake = std::filesystem::temp_directory_path() / "poison.inkternity";
        std::ofstream(fake, std::ios::binary) << "not a real container";
        auto rr = C2PA::read_and_verify(fake);
        std::cout << "=== poison read_and_verify: has_manifest=" << rr.has_manifest
                  << " error='" << rr.error << "'\n";
    }

    int failures = 0;
    for (int i = argi; i < argc; ++i) {
        std::filesystem::path src = argv[i];
        std::filesystem::path dst = src;
        dst += ".signed";
        dst += src.extension();
        std::cout << "=== embed " << src << " -> " << dst << "\n";
        auto r = C2PA::embed_into_image_file(src, dst, manifest, ca, leaf);
        if (r.success) std::cout << "OK\n";
        else { std::cout << "FAIL: " << r.error << "\n"; ++failures; }
    }

    // Second pass on a worker thread. c2pa-rs settings are thread-local
    // but apply_c2pa_settings_once's guard is process-global, so this
    // thread embeds under c2pa-rs DEFAULT settings (verify_after_sign=true,
    // verify_trust=true) — the state the app is in whenever the first c2pa
    // call of the session happened on a different thread.
    std::thread([&] {
        for (int i = argi; i < argc; ++i) {
            std::filesystem::path src = argv[i];
            std::filesystem::path dst = src;
            dst += ".thread";
            dst += src.extension();
            std::cout << "=== [worker thread] embed " << src << " -> " << dst << "\n";
            auto r = C2PA::embed_into_image_file(src, dst, manifest, ca, leaf);
            if (r.success) std::cout << "OK\n";
            else { std::cout << "FAIL: " << r.error << "\n"; ++failures; }
        }
    }).join();
    return failures ? 1 : 0;
}
