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

private:
    void render_bundle_card(MainProgram& main);
    // Stubs added by I10b / I10c — declared here so the unique IDs
    // for the Clay layout stay stable across the upcoming commits.
    void render_paste_card(MainProgram& main);
    void render_funding_card(MainProgram& main, WalletPanel& wallet);
    void render_submit_card(MainProgram& main, WalletPanel& wallet);

    // Backend modules (long-lived per session).
    StellarCli cli_;
    KeyStore   store_;
    Registry   reg_;

    // CA + derived bundle. Lazy-initialized in ensure_ca_loaded.
    AppCa       ca_;
    std::string bundleText_;
};

}  // namespace C2PA
