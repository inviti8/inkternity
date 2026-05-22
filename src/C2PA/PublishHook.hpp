#pragma once
// docs/design/C2PA.md §2 / I14-I15 — the publish-time hook that
// embeds a signed C2PA manifest into exported images and saved
// `.inkternity` files. Two slots:
//
//   - world_take_screenshot post-encode-pre-SDL_SaveFile (I14)
//   - World::save_to_file post-rename of the .tmp (I15, sidecar)
//
// Hook is conditional on the gateway toggle being ON and the
// registration status being Active. Otherwise the caller falls
// through to its existing plain-save path.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

class MainProgram;

namespace C2PA::PublishHook {

// Returns true if the hook handled the save (took ownership of the
// destination path — wrote a signed image there OR logged a failure
// AND did NOT fall through). Returns false in the "skip" case
// (gateway off, registration not Active, or unsupported format) —
// the caller then runs its existing plain-save path.
//
// `format_mime` is one of "image/png" / "image/jpeg" / "image/webp"
// in v1. Other formats (e.g. image/svg+xml) return false so the
// caller saves them plain; SVG-as-sidecar is the I15 path.
//
// On signing failure (registration Active but embed_into_image_file
// reports an error), the hook logs WORLDFATAL and still returns
// true — we don't silently substitute an unsigned export for a
// signed one. The artist sees the error in the in-app log toast.
bool try_save_signed_image(MainProgram& main,
                           const std::filesystem::path& dest,
                           const uint8_t* bytes,
                           std::size_t byte_count,
                           std::string_view format_mime);

}  // namespace C2PA::PublishHook
