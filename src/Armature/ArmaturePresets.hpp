#pragma once
// PHASE9.5 (docs/design/PHASE9.5.md) — app-level, per-user preset libraries for
// the armature editor: reusable "characters" (body look) and "scenes" (camera +
// light + lens). Stored OUTSIDE any .inkternity file so they carry across files
// and projects — sidecars under <configPath>, exactly like the brush presets.
//
// Layout under <configPath>:
//   armature_characters/
//     <slug>.json        height + material colors + shape-key sliders
//     <slug>.thumb.png   optional 128x128 preview
//   armature_scenes/
//     <slug>.json        camera + lens + light
//     <slug>.thumb.png   optional 128x128 preview
//
// Free-function shape mirrors UserBrushPresets.{hpp,cpp}: no class, no state;
// callers pass <configPath>. JSON via nlohmann; thumbnails are pre-encoded PNG
// bytes supplied by the caller (the editor renders them via ArmatureBake — this
// module is GL-free). LOCKED decisions: see PHASE9.5.md §9.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ArmaturePresets {

// Per-material color override (mirrors ArmatureCanvasComponent::MatColor, kept
// independent so this module doesn't depend on the canvas component).
struct MatColor {
    std::string material;
    float r = 0.8f, g = 0.8f, b = 0.8f, a = 1.0f;
};

// A saved "character": the body look (NOT pose, NOT the baked raster).
struct Character {
    std::string name;
    float height = 1.0f;                   // bone-length scale (1.0 = authored)
    std::vector<MatColor> materialColors;  // per-material RGBA, keyed by name
    std::vector<float> shapeSliders;       // 22 shape-key slider values
    std::string thumbPath;                 // sidecar .thumb.png abs path; "" if none
};

// A saved "scene": the view (camera + lens + light). Model-agnostic.
struct Scene {
    std::string name;
    float camYaw = 0.0f, camPitch = 0.0f, camDist = 3.0f;
    float camTx = 0.0f, camTy = 0.0f, camTz = 0.0f;
    float fovDeg = 40.0f;
    bool ortho = false;
    float lightAz = 0.3f, lightEl = 0.6f, lightInt = 0.95f, lightAmb = 0.5f, lightSky = 0.18f;
    std::string thumbPath;
};

// <configPath>/armature_characters , <configPath>/armature_scenes
std::filesystem::path characters_root(const std::filesystem::path& configPath);
std::filesystem::path scenes_root(const std::filesystem::path& configPath);

// Replace filesystem-forbidden chars with '_' (no silent drop → distinct names
// stay distinct). Trims leading/trailing whitespace + dots; "" → "preset".
std::string filename_slug(std::string_view name);

// True if a preset whose slug matches `name` already exists (save-collision check).
bool character_exists(const std::filesystem::path& configPath, std::string_view name);
bool scene_exists(const std::filesystem::path& configPath, std::string_view name);

// Load every <slug>.json under the library root; thumbPath is the sibling
// .thumb.png when present, "" otherwise. Filesystem natural order.
std::vector<Character> scan_characters(const std::filesystem::path& configPath);
std::vector<Scene> scan_scenes(const std::filesystem::path& configPath);

// Write the preset JSON (path from name's slug) and, if thumbPng is non-empty, a
// sibling .thumb.png; an empty/absent thumb removes any pre-existing one. Creates
// the library dir on demand. Returns false on disk error (logged).
bool save_character(const std::filesystem::path& configPath, const Character& preset,
                    const std::optional<std::vector<uint8_t>>& thumbPng);
bool save_scene(const std::filesystem::path& configPath, const Scene& preset,
                const std::optional<std::vector<uint8_t>>& thumbPng);

// Remove the preset JSON + any sibling .thumb.png. Idempotent.
bool remove_character(const std::filesystem::path& configPath, std::string_view nameOrSlug);
bool remove_scene(const std::filesystem::path& configPath, std::string_view nameOrSlug);

}  // namespace ArmaturePresets
