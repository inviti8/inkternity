#pragma once
// docs/design/C2PA.md §3.3 — the four-card registration walkthrough
// that runs inside the Verifiable publishing section when the gateway
// is ON and the artist hasn't yet registered their app CA on chain.
//
// Cards (rendered top to bottom inside the panel):
//   1. Bundle — display the HVYM-CA-REG-v1 text, Copy, Open Portal.
//   2. Paste token — paste field + parse + per-field cross-check.
//   3. Funding — re-poll balance, gate Submit on ≥ 5 XLM.
//   4. Submit — invoke register_app_ca on a worker thread, surface result.
//
// State (CA + bundle text + paste/submit) persists across settings
// opens for the lifetime of FileSelectScreen, so the artist can move
// away and come back without losing pasted data.
//
// Owns the long-lived C2PA backend modules (StellarCli, KeyStore,
// Registry) so the PATH probe + RPC contract-id discovery only run
// once per session.

#include "AppCa.hpp"
#include "KeyStore.hpp"
#include "Registry.hpp"
#include "StellarCli.hpp"
#include "WireToken.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

class MainProgram;

namespace C2PA {

class WalletPanel;

class RegistrationFlow {
public:
    explicit RegistrationFlow(std::filesystem::path configPath);
    ~RegistrationFlow();
    RegistrationFlow(const RegistrationFlow&) = delete;
    RegistrationFlow& operator=(const RegistrationFlow&) = delete;

    // Render the walkthrough cards. Call from
    // FileSelectScreen::verifiable_publishing_section once per frame
    // while the gateway toggle is ON.
    void render(MainProgram& main, WalletPanel& wallet);

    // Idempotent CA prep: load from KeyStore if present; otherwise
    // generate fresh, save to disk, build the bundle text. Logs to
    // INFO + USERINFO. No-op once `ca_.valid()`.
    void ensure_ca_loaded(MainProgram& main);

    const AppCa& ca() const noexcept { return ca_; }
    KeyStore&    store()             { return store_; }
    StellarCli&  cli()               { return cli_; }
    Registry&    registry()          { return reg_; }

    // Public so the anonymous-namespace worker entry function in
    // RegistrationFlow.cpp can reference them. Treat as opaque from
    // outside the .cpp.
    enum class SubmitPhase { Idle, InFlight, Success, Failed };
    struct SubmitResult {
        std::atomic<SubmitPhase> phase{SubmitPhase::Idle};
        std::string tx_hash;
        std::string error;
    };

private:
    void render_bundle_card(MainProgram& main);
    void render_paste_card(MainProgram& main);
    void render_funding_card(MainProgram& main, WalletPanel& wallet);
    void render_submit_card(MainProgram& main, WalletPanel& wallet);
    // I11a: rotate + revoke surface, shown only when state.status
    // is Active. Reuses the submit_ shared_ptr / worker-thread
    // pattern from the registration submit path.
    void render_active_card(MainProgram& main);
    void render_revoke_confirm_card(MainProgram& main);
    // I11b: begin a rotation. Generates a staging CA + a
    // HVYM-CA-ROT-v1 bundle text, flips flowMode_ to Rotate so the
    // four walkthrough cards re-render against ROT semantics.
    void begin_rotation(MainProgram& main);

    // Backend modules (long-lived per session).
    StellarCli cli_;
    KeyStore   store_;
    Registry   reg_;

    // CA + derived bundle. Lazy-initialized in ensure_ca_loaded.
    AppCa       ca_;
    std::string bundleText_;

    // Step-1 fold state. Defaults open (the artist usually arrives at
    // this card to grab the bundle text); collapses to free vertical
    // space while they work on Step 2+ down the page.
    bool bundleOpen_ = true;

    // I11b rotation mode. begin_rotation populates stagingCa_ + the
    // HVYM-CA-ROT-v1 bundle text; the walkthrough cards branch on
    // flowMode_ to render against the staging CA / RotateParams /
    // submit_rotate. On successful rotate, KeyStore::rotate_current_to_old
    // moves the old CA out of the way, stagingCa_ moves into ca_, and
    // flowMode_ resets to Register (which is a no-op once status is
    // back to Active and the user closes the panel).
    enum class FlowMode { Register, Rotate };
    FlowMode    flowMode_ = FlowMode::Register;
    AppCa       stagingCa_;
    std::string stagingBundleText_;
    WireToken::RotateParams tokenRotateParams_;

    // Paste-token state (Step 2). pastedToken_ is the artist's
    // input buffer; tokenStatusMsg_ is the line shown below the
    // paste field — empty before any Validate attempt, "Token
    // validated." on success, or a per-field mismatch message
    // from WireToken::check_register on failure. tokenParams_ is
    // populated only when tokenValid_ is true; carried forward to
    // Step 4's Submit call so we hand the contract the exact
    // pass-through auth_payload / auth_signature bytes the Portal
    // signed (gate G3).
    std::string                  pastedToken_;
    bool                         tokenValid_ = false;
    std::string                  tokenStatusMsg_;
    WireToken::RegisterParams    tokenParams_;

    // Submit state (Step 4). The worker thread captures the inputs
    // by value, runs SorobanSubmit::submit_register (5-10 s on testnet),
    // and writes back into the shared SubmitResult via shared_ptr —
    // that way the worker survives RegistrationFlow destruction
    // without dangling-pointer writes. SubmitPhase + SubmitResult are
    // declared above (publicly).
    std::shared_ptr<SubmitResult> submit_;
    std::thread                   submitThread_;

    // I11a revoke-confirm latch. Two-click guard: first click on the
    // "Revoke registration" button flips this on and shows a hard
    // confirmation; second click on the confirm button fires
    // submit_revoke_by_app via the worker. Cancel button resets.
    bool                          revokeConfirmOpen_ = false;
};

}  // namespace C2PA
