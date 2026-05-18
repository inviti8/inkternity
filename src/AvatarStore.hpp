#pragma once

#include <cstdint>
#include <filesystem>
#include <include/core/SkRefCnt.h>
#include <vector>

class SkImage;

// PHASE3.md §4 B.M1 -- on-disk persistence for the artist's avatar.
//
// Two files alongside inkternity_dev_keys.json in the user-config dir:
//
//   <configPath>/avatar.png        256x256 local master (~10-30 KB)
//   <configPath>/avatar_wire.png   64x64 wire payload (~3-8 KB)
//
// The wire form is a precomputed downscale so ClientData broadcasts
// don't re-resize on every send. Both files are regenerated on every
// save() -- the artist re-captures, both files get rewritten.
//
// One avatar per app install (per app keypair). Reinstalling Inkternity
// from the same Stellar seed won't restore the avatar -- it's local
// preference, not crypto-recoverable identity (PHASE3.md §4 Storage
// and identity binding).
//
// Free-function shape matches PublishedCanvases / UserBrushPresets.

namespace AvatarStore {

constexpr int MASTER_SIDE_PX = 256;
constexpr int WIRE_SIDE_PX   = 64;

// File paths (do not create directories; caller passes a real configPath).
std::filesystem::path master_path(const std::filesystem::path& configPath);
std::filesystem::path wire_path  (const std::filesystem::path& configPath);

// True iff a master avatar.png exists at the expected path. Used to
// gate the placeholder rendering in the top-toolbar tile.
bool has_avatar(const std::filesystem::path& configPath);

// Encode `master` as the 256x256 avatar.png AND downscale to a 64x64
// avatar_wire.png, writing both atomically (best-effort -- direct
// write, no temp+rename dance, matching how DevKeys / PublishedCanvases
// handle their config files). The input is assumed to already be
// MASTER_SIDE_PX on each side; callers (B.M3 capture path) produce
// it via SquareCanvasCaptureTool at MASTER_SIDE_PX.
//
// Returns false on encode or write failure; logs via Logger.
bool save(const std::filesystem::path& configPath, const sk_sp<SkImage>& master);

// Load and decode the master avatar.png. nullptr if the file is
// missing, unreadable, or fails PNG decode.
sk_sp<SkImage> load_master(const std::filesystem::path& configPath);

// Load the wire form as raw PNG bytes (for ClientData::avatarPng
// serialization in B.M4). Empty vector when no wire file exists.
std::vector<uint8_t> load_wire_bytes(const std::filesystem::path& configPath);

// Remove both files. Idempotent (returns true even if files were
// already absent). Used by the top-toolbar tile's "Clear" popover
// option.
bool clear(const std::filesystem::path& configPath);

}  // namespace AvatarStore
