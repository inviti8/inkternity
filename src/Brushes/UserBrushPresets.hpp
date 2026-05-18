#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef HVYM_HAS_LIBMYPAINT

#include "BrushPresets.hpp"

// PHASE3.md §3 A1.M3 — disk persistence for user-saved brush presets.
//
// Layout under <configPath>/brush_presets/:
//
//   sharp/
//     My Inky Pen 3.json        params (libmypaint cnames, .myb-shaped)
//     My Inky Pen 3.icon.png    optional artist-drawn 64x64 icon
//     Another preset.json
//   textured/
//     Wet ink (mine).json
//     Wet ink (mine).icon.png
//
// Categories are subdirectories (sharp / textured), matching BrushCategory.
// Param JSON mirrors libmypaint's `.myb` JSON-ish format so future "import
// from external .myb" can reuse read_params_json() directly.
//
// Color settings (color_h/s/v + change_color_h/l/hsl_s/v/hsv_s) are
// filtered on both read and write per PHASE3.md Decision #0 — preset
// captures *feel*, never color. If a .myb file authored elsewhere
// includes those keys, read silently drops them; the preset still
// loads with the rest of its settings intact.
//
// Free-function shape mirrors PublishedCanvases.{hpp,cpp} — no class,
// callers pass the relevant <configPath>. Module owns no state.

namespace UserBrushPresets {

// ---- on-disk layout ----------------------------------------------------

// <configPath>/brush_presets — root for everything this module manages.
// Subdirectories are created lazily on first save into them.
std::filesystem::path presets_root(const std::filesystem::path& configPath);

// "sharp" / "textured" — slug used as the category subdirectory name.
std::string_view category_dir_name(HVYM::Brushes::BrushCategory cat);

// Inverse — returns nullopt for any name we don't recognize. Lets scan()
// skip unknown subdirectories without erroring out (forward-compat for
// future category additions).
std::optional<HVYM::Brushes::BrushCategory> category_from_dir_name(std::string_view name);

// <configPath>/brush_presets/<category>/<slug>.json
// <configPath>/brush_presets/<category>/<slug>.icon.png
std::filesystem::path preset_json_path(const std::filesystem::path& configPath,
                                       HVYM::Brushes::BrushCategory cat,
                                       std::string_view nameOrSlug);
std::filesystem::path preset_icon_path(const std::filesystem::path& configPath,
                                       HVYM::Brushes::BrushCategory cat,
                                       std::string_view nameOrSlug);

// Replace filesystem-forbidden chars in `name` with '_'. Two distinct
// names that collapse to the same slug are a save-time error (no silent
// overwrite). Whitespace + Unicode are preserved.
std::string filename_slug(std::string_view name);

// ---- single-file format I/O -------------------------------------------

// Read a single preset JSON and reconstruct the BrushParams. Unknown
// setting cnames + color params are silently dropped (forward-compat
// for libmypaint upgrades + .myb imports). Returns nullopt on read /
// parse failure (with a Logger error logged).
std::optional<HVYM::Brushes::BrushParams> read_params_json(
    const std::filesystem::path& jsonPath);

// Serialize BrushParams to a preset JSON. `name` and `category` are
// recorded in the JSON's top-level metadata (the filename slug may
// differ from `name`). Returns false on write failure.
bool write_params_json(const std::filesystem::path& jsonPath,
                       std::string_view name,
                       HVYM::Brushes::BrushCategory cat,
                       const HVYM::Brushes::BrushParams& params);

// ---- composite preset operations --------------------------------------

// Walk every <category>/*.json under presets_root(configPath), load each,
// and return the populated BrushPreset list. Each entry's iconPath is
// the sibling .icon.png absolute path when that file exists, empty
// otherwise (A2 drawer falls back to an identicon when iconPath is
// empty, per PHASE3.md Decision #14). Filesystem natural order; the
// drawer can re-sort.
std::vector<HVYM::Brushes::BrushPreset> scan(
    const std::filesystem::path& configPath);

// Write preset.params to the categorized JSON, and -- if iconPngBytes is
// provided -- write a 64x64 PNG to the sibling .icon.png path. If
// iconPngBytes is empty, removes any pre-existing sibling icon (the
// artist intentionally cleared it; identicon takes over).
//
// `preset.name` and `preset.category` drive the destination path via
// filename_slug(); preset.iconPath is ignored on write (computed from
// the slug + category).
//
// Creates category subdirectories on demand. Returns false on disk
// error (logged via Logger).
bool save(const std::filesystem::path& configPath,
          const HVYM::Brushes::BrushPreset& preset,
          const std::optional<std::vector<uint8_t>>& iconPngBytes);

// Remove the preset JSON + any sibling .icon.png. Idempotent (returns
// true if files were already absent). Does not remove the category
// subdirectory even when it becomes empty -- leaving it as a no-op
// keeps `save` cheap for the next preset into that category.
bool remove(const std::filesystem::path& configPath,
            HVYM::Brushes::BrushCategory cat,
            std::string_view nameOrSlug);

}  // namespace UserBrushPresets

#endif  // HVYM_HAS_LIBMYPAINT
