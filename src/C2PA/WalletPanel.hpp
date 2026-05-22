#pragma once
// docs/design/C2PA.md §3.2 + §4 — wallet panel visible inside the
// "Verifiable publishing" section when the gateway is ON. Surfaces the
// app's Stellar G-address (already shown in the App Key block — same
// keypair per BYLAWS §3.10, no second credential) for copy-to-clipboard,
// plus an async Horizon balance probe so the artist can confirm the
// account has enough XLM to submit register_app_ca.
//
// Network selection: GlobalConfig::stellarNetwork (Mainnet|Testnet)
// is the source of truth, set via the radio in the FileSelectScreen
// "Verifiable publishing" section. STELLAR_NETWORK=testnet|mainnet
// remains as a dev override (matches the Portal's convention) so
// smoke tests can hit the right RPC without flipping the persisted
// knob.
//
// Balance polling: on settings open + on user-clicked Refresh + on
// every Submit attempt (the latter wired by I10). No background poll —
// matches DISTRIBUTION-PHASE1.md §8.6 "on-demand only".
//
// QR code is deferred: no QR primitive exists in the codebase and
// vendoring one is out of scope for I7. Copy button + plain-text
// address is the load-bearing primitive; QR is a nice-to-have follow-up.

#include <Helpers/FileDownloader.hpp>
#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include <memory>
#include <string>

class MainProgram;

namespace C2PA {

class WalletPanel {
public:
    // Render the wallet block. Call only when the gateway toggle is ON
    // (FileSelectScreen::verifiable_publishing_section gates this).
    void render(MainProgram& main);

    // Kick off an async Horizon GET for the given G-address. No-op if a
    // request is already in flight. Internal state cleared on success
    // or failure; render() reflects the result on the next frame.
    // Network comes from GlobalConfig::stellarNetwork, with the
    // STELLAR_NETWORK env var as a dev override (testnet|mainnet).
    void refresh_balance(MainProgram& main, const std::string& app_pubkey);

    // Network selector. Honors STELLAR_NETWORK env var ("testnet" →
    // horizon-testnet.stellar.org), else the persisted
    // GlobalConfig::stellarNetwork knob, else mainnet.
    static std::string horizon_base_url(MainProgram& main);

    // True iff the last successful Horizon read found a native XLM
    // balance on the account. Consumed by RegistrationFlow's Step 3
    // funding gate. nullopt-ish semantics: false also covers
    // "haven't fetched yet" — caller should ensure a fresh probe ran
    // before reading this.
    bool is_funded() const noexcept { return isFunded_; }

private:
    enum class FetchState {
        Idle,        // never fetched, or last fetch fully resolved
        InFlight,    // GET pending
        ResultReady, // pendingFetch_ holds a SUCCESS/FAILURE — render swallows it
    };
    FetchState state_ = FetchState::Idle;
    std::shared_ptr<FileDownloader::DownloadData> pendingFetch_;
    std::string lastBalanceText_;   // display copy (set in render after a fetch resolves)
    bool isFunded_ = false;          // last successful read had a native balance

    // QR display state. Lazily generated on first Show; cleared on Hide
    // or when the underlying pubkey changes (rotation / restore).
    bool qrVisible_ = false;
    sk_sp<SkImage> qrImage_;
    std::string    qrEncodedAddress_;

    // Pull JSON from `data->str` and produce a display string. Sets
    // isFunded_ as a side effect.
    std::string interpret_horizon_response(const std::string& body);
};

}  // namespace C2PA
