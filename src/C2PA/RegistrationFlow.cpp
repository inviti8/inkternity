#include "RegistrationFlow.hpp"

#include "Bundle.hpp"
#include "SorobanSubmit.hpp"
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

RegistrationFlow::~RegistrationFlow() {
    // The submit worker writes results via the SubmitResult shared_ptr,
    // not back into RegistrationFlow itself, so detach() is safe: the
    // shared_ptr keeps the result alive until the thread exits even
    // if RegistrationFlow is destroyed mid-submit. Blocking on join
    // here would freeze app shutdown for up to 10 s on a slow testnet
    // round-trip.
    if (submitThread_.joinable()) submitThread_.detach();
}

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
            "Step 2: Paste the wire token the portal returned. It looks "
            "like \"<base64>.<base64>\" on a single line.");

        input_text_field(gui, "c2pa flow paste token field",
            "Portal-minted token", &pastedToken_);

        text_button(gui, "c2pa flow validate token", "Validate token", {
            .wide = true,
            .onClick = [this, &main] {
                tokenValid_ = false;
                tokenStatusMsg_.clear();
                if (pastedToken_.empty()) {
                    tokenStatusMsg_ = "Paste a token first.";
                    return;
                }
                // Parse the wire format + extract fields.
                auto rc = WireToken::parse_register(pastedToken_, tokenParams_);
                if (rc != WireToken::ParseStatus::OK) {
                    tokenStatusMsg_ = WireToken::parse_status_str(rc);
                    Logger::get().log("INFO",
                        std::string("[C2PA::Flow] paste-token parse failed: ")
                        + tokenStatusMsg_);
                    return;
                }
                // Cross-check against the locally-generated CA + the
                // identity DevKeys knows. Mismatch returns a one-line
                // human-readable string naming the first failing field.
                WireToken::ExpectedRegisterValues expected{};
                expected.app_address     = main.devKeys.app_pubkey();
                expected.app_kind        = "Inkternity";
                expected.fingerprint     = ca_.fingerprint_sha256();
                expected.expires_at_unix = ca_.expires_at_unix();
                std::string mismatch =
                    WireToken::check_register(tokenParams_, expected);
                if (!mismatch.empty()) {
                    tokenStatusMsg_ = mismatch;
                    Logger::get().log("INFO",
                        "[C2PA::Flow] paste-token cross-check failed: " + mismatch);
                    return;
                }
                tokenValid_ = true;
                tokenStatusMsg_ = "Token validated.";
                Logger::get().log("INFO",
                    "[C2PA::Flow] paste-token validated; nonce="
                    + std::to_string(tokenParams_.nonce));
            },
        });

        if (!tokenStatusMsg_.empty()) {
            text_label(gui, tokenStatusMsg_.c_str());
        }
    }
}

void RegistrationFlow::render_funding_card(MainProgram& main, WalletPanel& wallet) {
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
            "Step 3: Make sure the wallet above has about 5 XLM. The "
            "exact ledger fee for register_app_ca lands in this budget.");
        text_label(gui, wallet.is_funded()
            ? "Wallet is funded."
            : "Wallet is not funded yet — send XLM to the address above.");
        text_button(gui, "c2pa flow refresh balance",
            "Refresh balance", {
            .wide = true,
            .onClick = [&main, &wallet] {
                wallet.refresh_balance(main.devKeys.app_pubkey());
            },
        });
    }
}

namespace {

// Worker thread entry: runs the Soroban submit and writes results
// back via the shared SubmitResult pointer. Captured arguments are
// all by-value so the worker survives RegistrationFlow destruction.
void run_submit(
        std::shared_ptr<RegistrationFlow::SubmitResult> result,
        StellarCli* cli,
        std::string  contract_id,
        std::string  source_account,
        std::string  app_address,
        std::array<uint8_t, 32> member_pubkey,
        std::string  app_kind,
        std::array<uint8_t, 32> fingerprint,
        uint64_t     expires_at,
        uint64_t     nonce,
        std::vector<uint8_t> auth_payload,
        std::vector<uint8_t> auth_signature,
        Soroban::RpcConfig rpc,
        std::filesystem::path c2paDir) {
    Logger::get().log("INFO",
        "[C2PA::Flow] submit worker started; rpc=" + rpc.rpc_url);
    auto r = Soroban::submit_register(*cli, rpc, contract_id, source_account,
        app_address, member_pubkey, app_kind, fingerprint,
        expires_at, nonce, auth_payload, auth_signature);
    if (r.success) {
        result->tx_hash = r.tx_hash;
        result->phase.store(RegistrationFlow::SubmitPhase::Success);
        // Persist Active status to disk so the walkthrough self-hides
        // on next render and survives a restart.
        std::error_code ec;
        std::filesystem::create_directories(c2paDir, ec);
        RegistrationState state{};
        state.status                  = RegistrationStatus::Active;
        state.expires_at_unix         = static_cast<int64_t>(expires_at);
        state.last_known_member_pubkey =
            // best-effort — caller already has member_pubkey raw bytes;
            // we don't re-encode to strkey here. The full record can
            // be backfilled by a get_app_ca read in I16's verifier.
            std::string{};
        KeyStore tmpStore(c2paDir.parent_path());
        tmpStore.save_state(state);
    } else {
        result->error = r.error.empty()
            ? std::string("submit failed; see log.txt for raw CLI output")
            : r.error;
        result->phase.store(RegistrationFlow::SubmitPhase::Failed);
    }
}

}  // namespace

void RegistrationFlow::render_submit_card(MainProgram& main, WalletPanel& wallet) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = main.g.gui;
    auto& io  = gui.io;

    const bool funded  = wallet.is_funded();
    const bool ready   = tokenValid_ && funded;
    const auto phase   = submit_ ? submit_->phase.load()
                                  : SubmitPhase::Idle;

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
            "Step 4: Submit the registration on chain. Takes a few "
            "seconds and uses about 5 XLM. The button is disabled "
            "until Steps 2 + 3 are green.");

        if (!ready && phase == SubmitPhase::Idle) {
            std::string reason;
            if (!tokenValid_) reason = "Validate a token in Step 2 first.";
            else if (!funded) reason = "Fund the wallet in Step 3 first.";
            text_label_light(gui, reason.c_str());
        }

        switch (phase) {
        case SubmitPhase::Idle: {
            text_button(gui, "c2pa flow submit",
                ready ? "Register on chain"
                      : "Register on chain (disabled)", {
                .wide = true,
                .onClick = [this, &main] {
                    if (!tokenValid_ || !ca_.valid()) return;
                    if (submit_ &&
                        submit_->phase.load() == SubmitPhase::InFlight) return;

                    // Resolve the cert-registry contract ID for the
                    // currently-selected network. The Registry caches
                    // per session; falls back to the §0 hardcoded ID
                    // if the live hvym_registry lookup misfires.
                    const auto net = main.conf.stellarNetwork;
                    const std::string contract_id =
                        reg_.cert_registry_id(net);
                    const auto rpc = Soroban::config_for_env_or_global(main.conf);

                    submit_ = std::make_shared<SubmitResult>();
                    submit_->phase.store(SubmitPhase::InFlight);

                    if (submitThread_.joinable()) submitThread_.detach();
                    submitThread_ = std::thread(run_submit,
                        submit_,
                        &cli_,
                        contract_id,
                        main.devKeys.app_pubkey(),
                        main.devKeys.app_pubkey(),  // app_address == source
                        tokenParams_.member_pubkey,
                        std::string("Inkternity"),
                        tokenParams_.fingerprint,
                        static_cast<uint64_t>(tokenParams_.expires_at_unix),
                        static_cast<uint64_t>(tokenParams_.nonce),
                        tokenParams_.auth_payload,
                        tokenParams_.auth_signature,
                        rpc,
                        store_.c2pa_dir());
                },
            });
            break;
        }
        case SubmitPhase::InFlight:
            text_label(gui,
                "Submitting on chain... (network round-trip, may take "
                "a few seconds)");
            break;
        case SubmitPhase::Success: {
            const std::string okMsg =
                "Registration submitted. tx_hash="
                + (submit_->tx_hash.empty()
                    ? std::string("<none-parsed>") : submit_->tx_hash);
            text_label(gui, okMsg.c_str());
            // The walkthrough's top-level render() will self-hide on
            // the next frame because KeyStore::load_state() now
            // returns Active.
            break;
        }
        case SubmitPhase::Failed:
            text_label(gui,
                ("Submission failed: " + submit_->error).c_str());
            text_button(gui, "c2pa flow submit retry", "Try again", {
                .wide = true,
                .onClick = [this] { submit_.reset(); },
            });
            break;
        }
    }
}

// ---- I11a: Active branch (rotate + revoke) -------------------------------

namespace {

// Worker for revoke_by_app. No Portal token needed — the artist's
// own app keypair authorizes via require_auth on the contract side.
void run_revoke(
        std::shared_ptr<RegistrationFlow::SubmitResult> result,
        StellarCli* cli,
        std::string contract_id,
        std::string source_account,
        std::string app_address,
        Soroban::RpcConfig rpc,
        std::filesystem::path c2paDir,
        int64_t expires_at_unix) {
    Logger::get().log("INFO",
        "[C2PA::Flow] revoke worker started; rpc=" + rpc.rpc_url);
    auto r = Soroban::submit_revoke_by_app(*cli, rpc, contract_id,
        source_account, app_address);
    if (r.success) {
        result->tx_hash = r.tx_hash;
        result->phase.store(RegistrationFlow::SubmitPhase::Success);
        // Persist Revoked status so the panel self-locks on next render
        // and survives a restart.
        KeyStore tmpStore(c2paDir.parent_path());
        auto state = tmpStore.load_state();
        state.status = RegistrationStatus::Revoked;
        state.expires_at_unix = expires_at_unix;
        tmpStore.save_state(state);
    } else {
        result->error = r.error.empty()
            ? std::string("revoke failed; see log.txt for raw CLI output")
            : r.error;
        result->phase.store(RegistrationFlow::SubmitPhase::Failed);
    }
}

}  // namespace

void RegistrationFlow::render_active_card(MainProgram& main) {
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
            "This install is registered for verifiable publishing.");

        // Rotate placeholder — full rotation walkthrough lands in
        // I11b. For now, the button just opens a "coming soon" note
        // so the artist sees the affordance.
        text_button(gui, "c2pa flow rotate stub", "Rotate keys", {
            .wide = true,
            .onClick = [] {
                Logger::get().log("USERINFO",
                    "Rotate keys: full rotation flow lands in I11b. "
                    "Reinstall + restore from the existing app secret "
                    "is the current workaround.");
            },
        });

        text_button(gui, "c2pa flow revoke open",
            "Revoke this install's signing keypair", {
            .wide = true,
            .onClick = [this] { revokeConfirmOpen_ = true; },
        });
    }
}

void RegistrationFlow::render_revoke_confirm_card(MainProgram& main) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = main.g.gui;
    auto& io  = gui.io;

    const auto phase = submit_ ? submit_->phase.load() : SubmitPhase::Idle;

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
            "Revoking marks this install's signing keypair as untrusted "
            "on chain. Future canvases + exports won't carry verifiable "
            "provenance until you re-register. Files you've already "
            "exported continue to verify, but viewers will see them as "
            "'Revoked'.");
        text_label(gui,
            "This action cannot be undone — revoking and re-registering "
            "requires a fresh on-chain submission.");

        switch (phase) {
        case SubmitPhase::Idle: {
            text_button(gui, "c2pa flow revoke confirm",
                "Yes, revoke on chain", {
                .wide = true,
                .onClick = [this, &main] {
                    if (submit_ &&
                        submit_->phase.load() == SubmitPhase::InFlight) return;
                    const auto net = main.conf.stellarNetwork;
                    const std::string contract_id =
                        reg_.cert_registry_id(net);
                    const auto rpc =
                        Soroban::config_for_env_or_global(main.conf);

                    submit_ = std::make_shared<SubmitResult>();
                    submit_->phase.store(SubmitPhase::InFlight);

                    if (submitThread_.joinable()) submitThread_.detach();
                    submitThread_ = std::thread(run_revoke,
                        submit_,
                        &cli_,
                        contract_id,
                        main.devKeys.app_pubkey(),
                        main.devKeys.app_pubkey(),
                        rpc,
                        store_.c2pa_dir(),
                        ca_.expires_at_unix());
                },
            });
            text_button(gui, "c2pa flow revoke cancel", "Cancel", {
                .wide = true,
                .onClick = [this] { revokeConfirmOpen_ = false; },
            });
            break;
        }
        case SubmitPhase::InFlight:
            text_label(gui,
                "Revoking on chain... (network round-trip)");
            break;
        case SubmitPhase::Success:
            // Next render() pass will see status=Revoked and switch
            // to the revoked-acknowledgement message. Surface the
            // tx_hash here for forensic clarity.
            text_label(gui,
                ("Revoked. tx_hash=" + (submit_->tx_hash.empty()
                    ? std::string("<none-parsed>") : submit_->tx_hash)).c_str());
            break;
        case SubmitPhase::Failed:
            text_label(gui,
                ("Revocation failed: " + submit_->error).c_str());
            text_button(gui, "c2pa flow revoke retry", "Try again", {
                .wide = true,
                .onClick = [this] { submit_.reset(); },
            });
            break;
        }
    }
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

    // Once Active, swap the walkthrough for the rotate/revoke surface.
    auto state = store_.load_state();
    if (state.status == RegistrationStatus::Active) {
        render_active_card(main);
        if (revokeConfirmOpen_) {
            render_revoke_confirm_card(main);
        }
        return;
    }

    // Status flipped to Revoked (by a successful submit_revoke_by_app
    // in this session, or persisted from a prior one). Walkthrough
    // hidden; one-line acknowledgement.
    if (state.status == RegistrationStatus::Revoked) {
        using namespace GUIStuff;
        using namespace ElementHelpers;
        text_label(main.g.gui,
            "This install's signing keypair has been revoked. Prior "
            "exports remain verifiable as 'Revoked' until they expire.");
        return;
    }

    render_bundle_card(main);
    render_paste_card(main);
    render_funding_card(main, wallet);
    render_submit_card(main, wallet);
}

}  // namespace C2PA
