#include "ArmaturePresets.hpp"

#include <Helpers/Logger.hpp>

#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace ArmaturePresets {
namespace {

constexpr const char* JSON_EXT  = ".json";
constexpr const char* THUMB_EXT = ".thumb.png";
constexpr int JSON_FMT_VERSION  = 1;
constexpr const char* CHAR_TAG  = "inkternity-armature-character";
constexpr const char* SCENE_TAG = "inkternity-armature-scene";
constexpr const char* POSE_TAG  = "inkternity-armature-pose";

bool write_file_bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        Logger::get().log("INFO", "[ArmaturePresets] failed to open for write: " + path.string());
        return false;
    }
    if (!bytes.empty())
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

bool write_json(const std::filesystem::path& jsonPath, const nlohmann::json& j) {
    std::error_code ec;
    std::filesystem::create_directories(jsonPath.parent_path(), ec);  // lazy dir
    std::ofstream f(jsonPath, std::ios::trunc);
    if (!f.is_open()) {
        Logger::get().log("INFO", "[ArmaturePresets] failed to open for write: " + jsonPath.string());
        return false;
    }
    f << j.dump(2);
    return static_cast<bool>(f);
}

// Write `j` to <root>/<slug>.json + optional <root>/<slug>.thumb.png (empty/absent
// thumb removes any pre-existing one). Shared by both preset kinds.
bool save_common(const std::filesystem::path& root, std::string_view name,
                 const nlohmann::json& j, const std::optional<std::vector<uint8_t>>& thumbPng) {
    const std::string slug = filename_slug(name);
    if (!write_json(root / (slug + JSON_EXT), j))
        return false;
    const auto thumbPath = root / (slug + THUMB_EXT);
    if (thumbPng.has_value() && !thumbPng->empty())
        return write_file_bytes(thumbPath, *thumbPng);
    std::error_code ec;
    std::filesystem::remove(thumbPath, ec);  // clear stale thumb
    return true;
}

bool remove_common(const std::filesystem::path& root, std::string_view nameOrSlug) {
    std::error_code ec;
    const std::string slug = filename_slug(nameOrSlug);
    std::filesystem::remove(root / (slug + JSON_EXT), ec);
    std::filesystem::remove(root / (slug + THUMB_EXT), ec);
    return true;
}

// Read+parse one JSON file; nullopt on failure (logged at INFO).
std::optional<nlohmann::json> read_json(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    try {
        nlohmann::json j;
        f >> j;
        return j;
    } catch (const std::exception& e) {
        Logger::get().log("INFO", "[ArmaturePresets] bad JSON " + path.string() + ": " + e.what());
        return std::nullopt;
    }
}

// Sibling <stem>.thumb.png absolute path, or "" when absent.
std::string sibling_thumb(const std::filesystem::path& jsonPath) {
    auto thumb = jsonPath;
    thumb.replace_extension();  // strip .json
    thumb += THUMB_EXT;
    std::error_code ec;
    if (std::filesystem::exists(thumb, ec) && !ec) return thumb.string();
    return "";
}

bool exists_common(const std::filesystem::path& root, std::string_view name) {
    std::error_code ec;
    return std::filesystem::exists(root / (filename_slug(name) + JSON_EXT), ec) && !ec;
}

}  // namespace

std::filesystem::path characters_root(const std::filesystem::path& configPath) {
    return configPath / "armature_characters";
}
std::filesystem::path scenes_root(const std::filesystem::path& configPath) {
    return configPath / "armature_scenes";
}
std::filesystem::path poses_root(const std::filesystem::path& configPath) {
    return configPath / "armature_poses";
}

std::string filename_slug(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        switch (c) {
            case '/': case '\\': case ':': case '*': case '?':
            case '"': case '<':  case '>': case '|':
                out += '_'; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += '_';
                else                                      out += c;
        }
    }
    auto isTrim = [](char c) { return c == ' ' || c == '\t' || c == '.'; };
    while (!out.empty() && isTrim(out.front())) out.erase(out.begin());
    while (!out.empty() && isTrim(out.back()))  out.pop_back();
    if (out.empty()) out = "preset";
    return out;
}

bool character_exists(const std::filesystem::path& configPath, std::string_view name) {
    return exists_common(characters_root(configPath), name);
}
bool scene_exists(const std::filesystem::path& configPath, std::string_view name) {
    return exists_common(scenes_root(configPath), name);
}
bool pose_exists(const std::filesystem::path& configPath, std::string_view name) {
    return exists_common(poses_root(configPath), name);
}

std::vector<Character> scan_characters(const std::filesystem::path& configPath) {
    std::vector<Character> out;
    const auto root = characters_root(configPath);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto& path = entry.path();
        if (path.extension() != JSON_EXT) continue;
        auto jOpt = read_json(path);
        if (!jOpt) continue;
        const nlohmann::json& j = *jOpt;
        Character c;
        c.name = j.value("name", path.stem().string());
        c.height = j.value("height", 1.0f);
        if (j.contains("materialColors") && j["materialColors"].is_array()) {
            for (const auto& mj : j["materialColors"]) {
                MatColor m;
                m.material = mj.value("material", std::string());
                m.r = mj.value("r", 0.8f); m.g = mj.value("g", 0.8f);
                m.b = mj.value("b", 0.8f); m.a = mj.value("a", 1.0f);
                c.materialColors.push_back(std::move(m));
            }
        }
        if (j.contains("shapeSliders") && j["shapeSliders"].is_array()) {
            try { c.shapeSliders = j["shapeSliders"].get<std::vector<float>>(); } catch (...) {}
        }
        c.thumbPath = sibling_thumb(path);
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Scene> scan_scenes(const std::filesystem::path& configPath) {
    std::vector<Scene> out;
    const auto root = scenes_root(configPath);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto& path = entry.path();
        if (path.extension() != JSON_EXT) continue;
        auto jOpt = read_json(path);
        if (!jOpt) continue;
        const nlohmann::json& j = *jOpt;
        Scene s;
        s.name = j.value("name", path.stem().string());
        const auto cam = j.value("cam", nlohmann::json::object());
        s.camYaw = cam.value("yaw", 0.0f);   s.camPitch = cam.value("pitch", 0.0f);
        s.camDist = cam.value("dist", 3.0f);
        s.camTx = cam.value("tx", 0.0f); s.camTy = cam.value("ty", 0.0f); s.camTz = cam.value("tz", 0.0f);
        const auto lens = j.value("lens", nlohmann::json::object());
        s.fovDeg = lens.value("fovDeg", 40.0f); s.ortho = lens.value("ortho", false);
        const auto light = j.value("light", nlohmann::json::object());
        s.lightAz = light.value("az", 0.3f);  s.lightEl = light.value("el", 0.6f);
        s.lightInt = light.value("int", 0.95f); s.lightAmb = light.value("amb", 0.5f);
        s.lightSky = light.value("sky", 0.18f);
        s.thumbPath = sibling_thumb(path);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Pose> scan_poses(const std::filesystem::path& configPath) {
    std::vector<Pose> out;
    const auto root = poses_root(configPath);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto& path = entry.path();
        if (path.extension() != JSON_EXT) continue;
        auto jOpt = read_json(path);
        if (!jOpt) continue;
        const nlohmann::json& j = *jOpt;
        Pose p;
        p.name = j.value("name", path.stem().string());
        if (j.contains("joints") && j["joints"].is_array()) {
            for (const auto& jj : j["joints"]) {
                JointPose jp;
                jp.bone = jj.value("bone", std::string());
                jp.qx = jj.value("qx", 0.0f); jp.qy = jj.value("qy", 0.0f);
                jp.qz = jj.value("qz", 0.0f); jp.qw = jj.value("qw", 1.0f);
                if (!jp.bone.empty()) p.joints.push_back(std::move(jp));
            }
        }
        p.thumbPath = sibling_thumb(path);
        out.push_back(std::move(p));
    }
    return out;
}

bool save_character(const std::filesystem::path& configPath, const Character& preset,
                    const std::optional<std::vector<uint8_t>>& thumbPng) {
    nlohmann::json j;
    j["version"] = JSON_FMT_VERSION;
    j["format"]  = CHAR_TAG;
    j["name"]    = preset.name;
    j["height"]  = preset.height;
    j["materialColors"] = nlohmann::json::array();
    for (const auto& m : preset.materialColors)
        j["materialColors"].push_back({{"material", m.material}, {"r", m.r}, {"g", m.g}, {"b", m.b}, {"a", m.a}});
    j["shapeSliders"] = preset.shapeSliders;
    return save_common(characters_root(configPath), preset.name, j, thumbPng);
}

bool save_scene(const std::filesystem::path& configPath, const Scene& preset,
                const std::optional<std::vector<uint8_t>>& thumbPng) {
    nlohmann::json j;
    j["version"] = JSON_FMT_VERSION;
    j["format"]  = SCENE_TAG;
    j["name"]    = preset.name;
    j["cam"]  = {{"yaw", preset.camYaw}, {"pitch", preset.camPitch}, {"dist", preset.camDist},
                 {"tx", preset.camTx}, {"ty", preset.camTy}, {"tz", preset.camTz}};
    j["lens"] = {{"fovDeg", preset.fovDeg}, {"ortho", preset.ortho}};
    j["light"] = {{"az", preset.lightAz}, {"el", preset.lightEl}, {"int", preset.lightInt},
                  {"amb", preset.lightAmb}, {"sky", preset.lightSky}};
    return save_common(scenes_root(configPath), preset.name, j, thumbPng);
}

bool save_pose(const std::filesystem::path& configPath, const Pose& preset,
               const std::optional<std::vector<uint8_t>>& thumbPng) {
    nlohmann::json j;
    j["version"] = JSON_FMT_VERSION;
    j["format"]  = POSE_TAG;
    j["name"]    = preset.name;
    j["joints"]  = nlohmann::json::array();
    for (const auto& p : preset.joints)
        j["joints"].push_back({{"bone", p.bone}, {"qx", p.qx}, {"qy", p.qy}, {"qz", p.qz}, {"qw", p.qw}});
    return save_common(poses_root(configPath), preset.name, j, thumbPng);
}

bool remove_character(const std::filesystem::path& configPath, std::string_view nameOrSlug) {
    return remove_common(characters_root(configPath), nameOrSlug);
}
bool remove_scene(const std::filesystem::path& configPath, std::string_view nameOrSlug) {
    return remove_common(scenes_root(configPath), nameOrSlug);
}
bool remove_pose(const std::filesystem::path& configPath, std::string_view nameOrSlug) {
    return remove_common(poses_root(configPath), nameOrSlug);
}

}  // namespace ArmaturePresets
