#pragma once
#include <SDL3/SDL_time.h>
#include <nlohmann/json.hpp>
#include <Helpers/VersionNumber.hpp>
#include "InputManager.hpp"

#define DEFAULT_CANVAS_BACKGROUND_COLOR Vector3f{0.07f, 0.07f, 0.07f}

class GlobalConfig {
    public:
        GlobalConfig();

        std::filesystem::path currentSearchPath;

        std::string ownLicenseText;
        std::vector<std::pair<std::string, std::string>> thirdPartyLicenses;

        struct Palette {
            std::string name;
            std::vector<Vector3f> colors;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Palette, name, colors)
        };

        std::vector<Palette> palettes;

        std::filesystem::path configPath;

        float guiScale = 1.0f;

        double dragZoomSpeed = 0.02;
        double scrollZoomSpeed = 0.4;
        float jumpTransitionTime = 0.5f;
        Vector4f jumpTransitionEasing{0.75, 0.25, 0.25, 0.75};
        bool viewWebVersionWelcome = false;

        struct TabletOptions {
            bool pressureAffectsBrushWidth = true;
            float smoothingSamplingTime = 0.04f;
            uint8_t middleClickButton = 1;
            uint8_t rightClickButton = 2;
            bool ignoreMouseMovementWhenPenInProximity = false;
            float brushMinimumSize = 0.0f;
            bool zoomWhilePenDownAndButtonHeld = true;
        } tabletOptions;

        Vector3f defaultCanvasBackgroundColor = DEFAULT_CANVAS_BACKGROUND_COLOR;

        nlohmann::json get_config_json(const InputManager& input) const;
        void set_config_json(InputManager& input, const nlohmann::json& j, VersionNumber version);

        void save_palettes();
        void load_palettes();
        void load_licenses();

        bool showPerformance = false;
        bool checkForUpdates = false;
        bool useNativeFilePicker = true;

        // docs/design/C2PA.md §3.1 + §B — the gateway toggle. Hidden
        // by default per the [[feedback-crypto-averse-users]] UX
        // constraint: until the artist flips this on, no Stellar /
        // wallet / signed-publishing UI is visible. ON reveals the
        // wallet panel (I7), registration walkthrough (I10), and the
        // verifier badges on file import (I16); OFF restores silent
        // behavior with no on-chain side effects.
        bool verifiablePublishingEnabled = false;

        // docs/design/C2PA.md §10/Q3 — which Stellar network the
        // C2PA submitter targets. Defaults to mainnet (where the
        // hvym-cert-registry was deployed 2026-05-21). The
        // STELLAR_NETWORK env var overrides this for dev / smoke
        // testing — set STELLAR_NETWORK=testnet to flip without
        // touching the saved config. The hvym_registry contract
        // itself always lives on mainnet regardless of this knob.
        enum class StellarNetwork {
            Mainnet,
            Testnet,
        } stellarNetwork = StellarNetwork::Mainnet;

        std::string themeCurrentlyLoaded = "Default";

        enum class AntiAliasing {
            NONE,
            SKIA,
            DYNAMIC_MSAA
        } antialiasing = AntiAliasing::SKIA;

        std::string displayName;
        bool flipZoomToolDirection = false;

        bool disableGraphicsDriverWorkarounds = false;
        int vsyncValue = 1;

        SDL_DateFormat dateFormat = SDL_DATE_FORMAT_DDMMYYYY;
        SDL_TimeFormat timeFormat = SDL_TIME_FORMAT_12HR;

        #ifndef __EMSCRIPTEN__
            bool applyDisplayScale = true;
        #endif
    private:
        void load_default_palette();
};

NLOHMANN_JSON_SERIALIZE_ENUM(GlobalConfig::AntiAliasing, {
    {GlobalConfig::AntiAliasing::NONE, "None"},
    {GlobalConfig::AntiAliasing::SKIA, "Skia"},
    {GlobalConfig::AntiAliasing::DYNAMIC_MSAA, "Dynamic MSAA"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(GlobalConfig::StellarNetwork, {
    {GlobalConfig::StellarNetwork::Mainnet, "mainnet"},
    {GlobalConfig::StellarNetwork::Testnet, "testnet"},
})
