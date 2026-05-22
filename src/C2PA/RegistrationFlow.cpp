#include "RegistrationFlow.hpp"

#include "Bundle.hpp"
#include "WalletPanel.hpp"
#include "../MainProgram.hpp"
#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"

#include <Helpers/Logger.hpp>

#include <SDL3/SDL_misc.h>

namespace C2PA {

RegistrationFlow::RegistrationFlow(std::filesystem::path configPath)
    : cli_(configPath),
      store_(std::move(configPath)),
      reg_(cli_) {
    // Probe the stellar CLI eagerly so subsequent registry / submit
    // calls don't pay the PATH-resolution cost on the GUI thread.
    cli_.probe();
}

RegistrationFlow::~RegistrationFlow() = default;

void RegistrationFlow::ensure_ca_loaded(MainProgram& main) {
    if (ca_.valid()) return;

    // Try disk first — survives across runs.
    if (store_.has_saved_ca()) {
        AppCa loaded = store_.load();
        if (loaded.valid()) {
            ca_ = std::move(loaded);
            Logger::get().log("INFO",
                "[C2PA::Flow] loaded persisted app CA from "
                + store_.c2pa_dir().string());
        } else {
            Logger::get().log("WORLDFATAL",
                "[C2PA::Flow] persisted CA failed to parse — generating fresh");
        }
    }

    // No usable CA on disk: generate + persist. The app's G-address
    // (DevKeys::app_pubkey) goes into the cert's SAN as the Stellar
    // binding the on-chain registry keys off.
    if (!ca_.valid()) {
        if (!main.devKeys.is_loaded()) {
            Logger::get().log("WORLDFATAL",
                "[C2PA::Flow] cannot generate CA: DevKeys not loaded");
            return;
        }
        ca_ = AppCa::generate("Inkternity", main.devKeys.app_pubkey());
        if (!ca_.valid()) {
            Logger::get().log("WORLDFATAL",
                "[C2PA::Flow] AppCa::generate returned invalid CA");
            return;
        }
        if (!store_.save(ca_)) {
            Logger::get().log("WORLDFATAL",
                "[C2PA::Flow] could not persist freshly-generated CA");
            // Carry on with the in-memory CA; persistence retry happens
            // on next ensure_ca_loaded call.
        } else {
            Logger::get().log("USERINFO", "Verifiable publishing identity created.");
        }
    }

    // Derive bundle text once. The expiry comes from the CA's notAfter,
    // the fingerprint is SHA-256 over the DER cert; app_kind is
    // hardcoded "Inkternity" for this build. The bundle is what the
    // artist pastes into the Portal.
    RegisterBundle b{};
    b.app_address     = main.devKeys.app_pubkey();
    b.app_kind        = AppKind::Inkternity;
    b.fingerprint     = ca_.fingerprint_sha256();
    b.expires_at_unix = ca_.expires_at_unix();
    bundleText_ = Bundle::render_register(b);
    if (bundleText_.empty()) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::Flow] Bundle::render_register returned empty");
    }
}

// ---- Card 1: Bundle ------------------------------------------------------

void RegistrationFlow::render_bundle_card(MainProgram& main) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = main.g.gui;
    auto& io  = gui.io;

    CLAY_AUTO_ID({
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
            .padding = CLAY_PADDING_ALL(io.theme->padding1),
            .childGap = io.theme->childGap1,
            .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_TOP },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
        .backgroundColor = convert_vec4<Clay_Color>(io.theme->backColor1),
    }) {
        text_label(gui,
            "Step 1: Copy this provenance bundle, then open the "
            "Heavymeta portal to authorize it. The portal will return "
            "a one-line token you paste back in Step 2.");

        if (bundleText_.empty()) {
            text_label_light(gui, "(bundle not yet generated)");
            return;
        }

        // Multi-line read-only display. text_label handles \n breaks.
        text_label(gui, bundleText_.c_str());

        text_button(gui, "c2pa flow copy bundle", "Copy bundle", {
            .wide = true,
            .onClick = [this, &main] {
                main.input.set_clipboard_str(bundleText_);
            },
        });
        text_button(gui, "c2pa flow open portal",
            "Open Heavymeta portal", {
            .wide = true,
            .onClick = [] {
                // Plain URL via SDL_OpenURL — plan §10/Q4 resolution.
                // No custom protocol handler; the artist's default
                // browser handles the redirect to the provenance card.
                if (!SDL_OpenURL("https://heavymeta.art/launch")) {
                    Logger::get().log("WORLDFATAL",
                        std::string("[C2PA::Flow] SDL_OpenURL failed: ")
                        + (SDL_GetError() ? SDL_GetError() : ""));
                }
            },
        });
    }
}

// ---- Card stubs (I10b / I10c) --------------------------------------------

void RegistrationFlow::render_paste_card(MainProgram& main) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = main.g.gui;
    text_label_light(gui,
        "Step 2: paste-token + cross-check — lands in next commit (I10b).");
}

void RegistrationFlow::render_funding_card(MainProgram& main, WalletPanel&) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = main.g.gui;
    text_label_light(gui,
        "Step 3: funding gate — lands in next commit (I10c).");
}

void RegistrationFlow::render_submit_card(MainProgram& main, WalletPanel&) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = main.g.gui;
    text_label_light(gui,
        "Step 4: submit register_app_ca — lands in next commit (I10c).");
}

// ---- Top-level render ----------------------------------------------------

void RegistrationFlow::render(MainProgram& main, WalletPanel& wallet) {
    ensure_ca_loaded(main);
    if (!ca_.valid()) {
        using namespace GUIStuff;
        using namespace ElementHelpers;
        text_label(main.g.gui,
            "Could not initialize verifiable-publishing identity. "
            "Check log.txt for details.");
        return;
    }

    // Skip the walkthrough entirely once registration is Active.
    auto state = store_.load_state();
    if (state.status == RegistrationStatus::Active) {
        using namespace GUIStuff;
        using namespace ElementHelpers;
        text_label(main.g.gui,
            "Your install is registered for verifiable publishing. "
            "Manage rotation or revocation below.");
        // I11 attaches the rotate/revoke UI here.
        return;
    }

    render_bundle_card(main);
    render_paste_card(main);
    render_funding_card(main, wallet);
    render_submit_card(main, wallet);
}

}  // namespace C2PA
