#include "UserBrushPresets.hpp"

#ifdef HVYM_HAS_LIBMYPAINT

#include "MyPaintBrushParams.hpp"
#include <Helpers/Logger.hpp>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>

namespace UserBrushPresets {

namespace {

using HVYM::Brushes::BrushCategory;
using HVYM::Brushes::BrushParams;
using HVYM::Brushes::BrushPreset;
using HVYM::Brushes::LinearPressureMapping;

constexpr const char* JSON_EXT  = ".json";
constexpr const char* ICON_EXT  = ".icon.png";
constexpr int         JSON_FMT_VERSION = 1;
constexpr const char* JSON_FMT_TAG     = "inkternity-brush-preset";

bool write_file_bytes(const std::filesystem::path& path,
                      const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        Logger::get().log("INFO", "[UserBrushPresets] failed to open for write: " + path.string());
        return false;
    }
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(f);
}

// Look up a libmypaint setting by cname. Returns nullopt if the name is
// unknown (typo, future-version setting we don't have a slot for, etc).
std::optional<MyPaintBrushSetting> setting_from_cname_checked(const char* cname) {
    if (!cname || !*cname) return std::nullopt;
    const auto id = mypaint_brush_setting_from_cname(cname);
    // libmypaint returns the sentinel _COUNT (or sometimes a negative
    // value as a signed int cast) for unknown names. Anything outside
    // the valid range is "skip silently."
    const auto raw = static_cast<int>(id);
    if (raw < 0 || raw >= MYPAINT_BRUSH_SETTINGS_COUNT) return std::nullopt;
    return id;
}

}  // namespace

std::filesystem::path presets_root(const std::filesystem::path& configPath) {
    return configPath / "brush_presets";
}

std::string_view category_dir_name(BrushCategory cat) {
    switch (cat) {
        case BrushCategory::SHARP:    return "sharp";
        case BrushCategory::TEXTURED: return "textured";
    }
    return "sharp";  // unreachable; silences warning
}

std::optional<BrushCategory> category_from_dir_name(std::string_view name) {
    if (name == "sharp")    return BrushCategory::SHARP;
    if (name == "textured") return BrushCategory::TEXTURED;
    return std::nullopt;
}

std::filesystem::path preset_json_path(const std::filesystem::path& configPath,
                                       BrushCategory cat,
                                       std::string_view nameOrSlug) {
    return presets_root(configPath) / std::string(category_dir_name(cat))
         / (filename_slug(nameOrSlug) + JSON_EXT);
}

std::filesystem::path preset_icon_path(const std::filesystem::path& configPath,
                                       BrushCategory cat,
                                       std::string_view nameOrSlug) {
    return presets_root(configPath) / std::string(category_dir_name(cat))
         / (filename_slug(nameOrSlug) + ICON_EXT);
}

std::string filename_slug(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        // Forbidden on at least one of Windows / POSIX. Replace with '_'
        // rather than dropping so two distinct names can't silently
        // collapse to the same slug just by removing punctuation.
        switch (c) {
            case '/': case '\\': case ':': case '*': case '?':
            case '"': case '<':  case '>': case '|':
                out += '_'; break;
            // Control chars also forbidden in Windows filenames.
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += '_';
                else                                     out += c;
        }
    }
    // Trim leading/trailing whitespace + dots (Windows trims trailing
    // dots silently which causes name collisions).
    auto isTrim = [](char c) { return c == ' ' || c == '\t' || c == '.'; };
    while (!out.empty() && isTrim(out.front())) out.erase(out.begin());
    while (!out.empty() && isTrim(out.back()))  out.pop_back();
    if (out.empty()) out = "preset";
    return out;
}

std::optional<BrushParams> read_params_json(const std::filesystem::path& jsonPath) {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        Logger::get().log("INFO", "[UserBrushPresets] cannot open: " + jsonPath.string());
        return std::nullopt;
    }
    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e) {
        Logger::get().log("INFO", "[UserBrushPresets] parse failure (" + jsonPath.string() + "): " + e.what());
        return std::nullopt;
    }

    BrushParams params = HVYM::Brushes::inkternity_default_params();

    const auto settingsIt = j.find("settings");
    if (settingsIt == j.end() || !settingsIt->is_object()) {
        // No settings block at all -> preset is "all defaults," which is
        // a valid (if pointless) state. Don't fail; let the caller decide.
        return params;
    }

    for (const auto& [cname, entry] : settingsIt->items()) {
        const auto idOpt = setting_from_cname_checked(cname.c_str());
        if (!idOpt) continue;  // unknown setting; forward-compat skip
        const auto id = *idOpt;
        if (HVYM::Brushes::is_color_param(id)) continue;  // Decision #0

        if (entry.is_object()) {
            // .myb-style: { "base_value": <float>, "inputs": { "pressure": [[x,y]...] } }
            if (auto bvIt = entry.find("base_value"); bvIt != entry.end() && bvIt->is_number()) {
                params.baseValues[id] = bvIt->get<float>();
            }
            if (auto inIt = entry.find("inputs"); inIt != entry.end() && inIt->is_object()) {
                if (auto pIt = inIt->find("pressure"); pIt != inIt->end() && pIt->is_array()) {
                    // First 2 points -> 2-point linear curve. Drop extras
                    // (v1 customization-drawer ships scalar-only; full
                    // curve editing is deferred per PHASE3.md §3 A1).
                    const auto& pts = *pIt;
                    if (pts.size() >= 2 && pts[0].is_array() && pts[1].is_array()
                        && pts[0].size() == 2 && pts[1].size() == 2
                        && pts[0][1].is_number() && pts[1][1].is_number()) {
                        params.pressureMappings.push_back(LinearPressureMapping{
                            id,
                            pts[0][1].get<float>(),
                            pts[1][1].get<float>(),
                        });
                    }
                }
            }
        } else if (entry.is_number()) {
            // Shorthand: "hardness": 0.85 -- just a base value, no input curve.
            params.baseValues[id] = entry.get<float>();
        }
    }
    return params;
}

bool write_params_json(const std::filesystem::path& jsonPath,
                       std::string_view name,
                       BrushCategory cat,
                       const BrushParams& params) {
    nlohmann::json out;
    out["version"]            = JSON_FMT_VERSION;
    out["format"]             = JSON_FMT_TAG;
    out["name"]               = std::string(name);
    out["inkternity_category"]= std::string(category_dir_name(cat));

    // Only write deltas from inkternity_default_params -- keeps the file
    // small and readable. Future schema changes (adding new settings to
    // libmypaint, changing Inkternity defaults) re-derive on load
    // because read_params_json starts from the current defaults too.
    const auto& defaults = HVYM::Brushes::inkternity_default_params();
    nlohmann::json settings = nlohmann::json::object();

    for (int i = 0; i < MYPAINT_BRUSH_SETTINGS_COUNT; ++i) {
        const auto id = static_cast<MyPaintBrushSetting>(i);
        if (HVYM::Brushes::is_color_param(id)) continue;  // Decision #0
        if (params.baseValues[i] != defaults.baseValues[i]) {
            const auto cname = HVYM::Brushes::param_internal_name(id);
            nlohmann::json entry = { {"base_value", params.baseValues[i]} };
            settings[std::string(cname)] = std::move(entry);
        }
    }
    for (const auto& m : params.pressureMappings) {
        if (HVYM::Brushes::is_color_param(m.setting)) continue;
        const auto cname = std::string(HVYM::Brushes::param_internal_name(m.setting));
        // Reuse the base-value entry if it already exists; otherwise
        // create one with the current base value so a reader without
        // the matching default still gets the intended value.
        if (!settings.contains(cname)) {
            settings[cname] = { {"base_value", params.baseValues[m.setting]} };
        }
        settings[cname]["inputs"]["pressure"] = nlohmann::json::array({
            nlohmann::json::array({0.0, m.lowOffset}),
            nlohmann::json::array({1.0, m.highOffset}),
        });
    }

    out["settings"] = std::move(settings);

    std::error_code ec;
    std::filesystem::create_directories(jsonPath.parent_path(), ec);
    if (ec) {
        Logger::get().log("INFO", "[UserBrushPresets] mkdir failed (" + jsonPath.parent_path().string() + "): " + ec.message());
        return false;
    }

    std::ofstream f(jsonPath, std::ios::trunc);
    if (!f.is_open()) {
        Logger::get().log("INFO", "[UserBrushPresets] cannot open for write: " + jsonPath.string());
        return false;
    }
    f << out.dump(2);
    return static_cast<bool>(f);
}

std::vector<BrushPreset> scan(const std::filesystem::path& configPath) {
    std::vector<BrushPreset> out;
    const auto root = presets_root(configPath);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return out;

    for (const auto& catEntry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!catEntry.is_directory()) continue;
        const auto catOpt = category_from_dir_name(catEntry.path().filename().string());
        if (!catOpt) continue;  // unknown category subdir; skip

        for (const auto& fileEntry : std::filesystem::directory_iterator(catEntry.path(), ec)) {
            if (ec) break;
            if (!fileEntry.is_regular_file()) continue;
            const auto& path = fileEntry.path();
            if (path.extension() != JSON_EXT) continue;

            auto paramsOpt = read_params_json(path);
            if (!paramsOpt) continue;

            // Stem is the slug; the human name we prefer to read from
            // inside the JSON, falling back to the slug when missing.
            std::string name = path.stem().string();
            {
                std::ifstream f(path);
                if (f.is_open()) {
                    try {
                        nlohmann::json j;
                        f >> j;
                        if (auto it = j.find("name"); it != j.end() && it->is_string())
                            name = it->get<std::string>();
                    } catch (...) { /* keep slug */ }
                }
            }

            BrushPreset preset;
            preset.name     = std::move(name);
            preset.params   = std::move(*paramsOpt);
            preset.category = *catOpt;

            // Sidecar icon: <slug>.icon.png next to the JSON.
            auto iconPath = path;
            iconPath.replace_extension();  // strip .json
            iconPath += ICON_EXT;
            if (std::filesystem::exists(iconPath, ec) && !ec) {
                preset.iconPath = iconPath.string();
            }

            out.push_back(std::move(preset));
        }
    }
    return out;
}

bool save(const std::filesystem::path& configPath,
          const BrushPreset& preset,
          const std::optional<std::vector<uint8_t>>& iconPngBytes) {
    const auto jsonPath = preset_json_path(configPath, preset.category, preset.name);
    if (!write_params_json(jsonPath, preset.name, preset.category, preset.params)) {
        return false;
    }
    const auto iconPath = preset_icon_path(configPath, preset.category, preset.name);
    if (iconPngBytes.has_value() && !iconPngBytes->empty()) {
        return write_file_bytes(iconPath, *iconPngBytes);
    }
    // Explicit empty / nullopt -> remove any pre-existing icon so the
    // identicon fallback takes over on next scan.
    std::error_code ec;
    std::filesystem::remove(iconPath, ec);
    return true;
}

bool remove(const std::filesystem::path& configPath,
            BrushCategory cat,
            std::string_view nameOrSlug) {
    std::error_code ec;
    const auto jsonPath = preset_json_path(configPath, cat, nameOrSlug);
    const auto iconPath = preset_icon_path(configPath, cat, nameOrSlug);
    std::filesystem::remove(jsonPath, ec);  // idempotent; ignore "not found"
    std::filesystem::remove(iconPath, ec);
    return true;
}

}  // namespace UserBrushPresets

#endif  // HVYM_HAS_LIBMYPAINT
