#pragma once
// docs/design/C2PA.md §8.3 (revised) — thin wrapper over the c2pa-rs
// C ABI's `c2pa_sign_file` / `c2pa_read_file` entry points. Lets the
// rest of Inkternity speak C++ values (paths, strings, AppCa,
// MemberLeaf) without #include "c2pa.h" leaking everywhere.
//
// Implementation uses the deprecated-but-stable convenience symbols
// rather than the streaming Builder/Reader API — they're materially
// the same surface for our needs and ~10x less code. Migrate to the
// builder/context API if the manifest assembly grows past simple
// claim-generator + assertions.
//
// Signing path lifetime: callers construct a fresh LeafIssuer::issue_leaf
// for every embed call (per plan §6, per-publish ephemeral). The
// private key is PEM-serialized inside embed_into_image_file, handed
// to c2pa-rs, and the source PEM buffer is OPENSSL_cleansed before
// the function returns. The seed inside MemberLeaf is wiped by its
// destructor.

#include <filesystem>
#include <string>
#include <string_view>

namespace C2PA {

class AppCa;
class MemberLeaf;

struct EmbedResult {
    bool        success = false;
    std::string error;       // human-readable on failure; empty on success
    std::string raw_out;     // captured c2pa_error() output on failure
};

// Embed a signed C2PA manifest into `source` and write the result to
// `dest` (different paths — c2pa-rs doesn't safely overwrite).
// `manifest_json` is the manifest definition (claim_generator, title,
// assertions, etc). The cert chain is [leaf.cert, ca.cert]; the leaf's
// private seed signs.
//
// Format is sniffed from `dest`'s extension by c2pa-rs (PNG / JPG /
// WEBP supported as of c2pa-rs v0.84.x). Returns success on a NULL
// from c2pa_sign_file; otherwise extracts c2pa_error() into result.
EmbedResult embed_into_image_file(
    const std::filesystem::path& source,
    const std::filesystem::path& dest,
    std::string_view             manifest_json,
    const AppCa&                 ca,
    const MemberLeaf&            leaf);

struct ReadResult {
    bool        has_manifest = false;
    std::string manifest_json;  // populated iff has_manifest
    std::string error;          // populated iff has_manifest == false
                                 // AND it wasn't a "no manifest present" miss
};

// Read + verify any embedded C2PA manifest in `source`. Returns
// `has_manifest=true` + populated JSON on success;
// `has_manifest=false` + empty `error` if the file just has no
// manifest (the common case for un-signed images); both false +
// populated error if the file is unreadable or its manifest is
// malformed. Verification (signing-chain + signature integrity)
// is performed by c2pa-rs internally; the JSON's
// validation_results field carries the outcome.
ReadResult read_and_verify(const std::filesystem::path& source);

// Write a .c2pa sidecar containing a signed manifest for `asset_path`,
// without modifying the asset itself. Used for file formats c2pa-rs
// can't embed manifests directly into (e.g. .inkternity, .svg).
// The sidecar lands at `asset_path + ".c2pa"`.
//
// `format_mime` is the asset's MIME type so c2pa-rs can compute the
// right hash binding. Use "image/svg+xml" for SVG, the closest known
// type for custom containers (or "application/octet-stream" as a
// fallback for `.inkternity`).
//
// Reads the entire asset into memory — fine for canvas files
// (typically < 50 MB) and SVG exports (< 5 MB).
EmbedResult write_sidecar(const std::filesystem::path& asset_path,
                          std::string_view             manifest_json,
                          std::string_view             format_mime,
                          const AppCa&                 ca,
                          const MemberLeaf&            leaf);

}  // namespace C2PA
