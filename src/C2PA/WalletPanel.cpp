#include "WalletPanel.hpp"

#include "QrCodeRender.hpp"
#include "../MainProgram.hpp"
#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/Elements/MemoryImageDisplay.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"

#include <Helpers/Logger.hpp>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <nlohmann/json.hpp>

namespace C2PA {

std::string WalletPanel::horizon_base_url(MainProgram& main) {
    // Env override wins for dev / smoke testing.
    if (const char* env = std::getenv("STELLAR_NETWORK")) {
        if (std::strcmp(env, "testnet") == 0)
            return "https://horizon-testnet.stellar.org";
        if (std::strcmp(env, "mainnet") == 0)
            return "https://horizon.stellar.org";
    }
    return main.conf.stellarNetwork == GlobalConfig::StellarNetwork::Testnet
        ? "https://horizon-testnet.stellar.org"
        : "https://horizon.stellar.org";
}

void WalletPanel::refresh_balance(MainProgram& main, const std::string& app_pubkey) {
    if (state_ == FetchState::InFlight) return;
    if (app_pubkey.empty()) return;

    const std::string url = horizon_base_url(main) + "/accounts/" + app_pubkey;
    pendingFetch_ = FileDownloader::download_data_from_url(url);
    state_ = FetchState::InFlight;
}

std::string WalletPanel::interpret_horizon_response(const std::string& body) {
    isFunded_ = false;
    if (body.empty()) return "Balance check failed (empty response).";

    try {
        auto j = nlohmann::json::parse(body);

        // 404 path: Horizon returns a `status: 404` envelope when the
        // account hasn't been created on-ledger yet. The account is
        // "unfunded" — the artist needs to send ~5 XLM to it.
        if (j.contains("status") && j["status"].is_number_integer()) {
            const int s = j["status"].get<int>();
            if (s == 404) {
                // 404 = unfunded account. Treat as a 0-XLM observation
                // so the popup baseline anchors here; the first
                // funding transfer (404 -> 5 XLM) then registers as a
                // delta and toasts the artist.
                observedXlm_     = 0.0;
                haveObservedXlm_ = true;
                return "Not funded yet — send XLM to this address.";
            }
            return "Balance check failed (HTTP " + std::to_string(s) + ").";
        }

        // Success path: `balances` is an array; native asset is the
        // entry with asset_type == "native".
        if (!j.contains("balances") || !j["balances"].is_array())
            return "Balance check failed (no balances field).";

        for (const auto& b : j["balances"]) {
            if (!b.is_object()) continue;
            if (b.value("asset_type", "") != "native") continue;
            std::string bal = b.value("balance", "");
            if (bal.empty()) continue;

            // Parse the numeric value first (before trimming display
            // zeros) so the Fund popup can detect deltas. std::stod
            // accepts Horizon's fixed-7-decimal format directly.
            try {
                observedXlm_     = std::stod(bal);
                haveObservedXlm_ = true;
            } catch (...) { /* leave observed unchanged */ }

            // Trim trailing zeros after the decimal point for display.
            // Horizon's native balance is fixed at 7 decimals — "10.5000000"
            // becomes "10.5", "0.0000000" stays "0.0".
            const auto dot = bal.find('.');
            if (dot != std::string::npos) {
                size_t end = bal.find_last_not_of('0');
                if (end > dot) bal.erase(end + 1);
                else           bal.erase(dot + 2);  // keep "x.0"
            }
            isFunded_ = true;
            return "Balance: " + bal + " XLM";
        }
        // No native entry — treat as 0 XLM, but mark as observed so
        // the popup baseline still anchors on this read.
        observedXlm_     = 0.0;
        haveObservedXlm_ = true;
        return "Balance: 0 XLM (no native asset entry)";
    } catch (const std::exception& e) {
        return std::string("Balance check failed (parse): ") + e.what();
    }
}

void WalletPanel::render(MainProgram& main) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = main.g.gui;
    auto& io  = gui.io;

    // Drain a resolved fetch on whichever frame arrives after completion.
    if (state_ == FetchState::InFlight && pendingFetch_) {
        const auto status = pendingFetch_->status.load();
        if (status == FileDownloader::DownloadData::Status::SUCCESS) {
            lastBalanceText_ = interpret_horizon_response(pendingFetch_->str);
            pendingFetch_.reset();
            state_ = FetchState::Idle;

            // Fund-popup delta detection. First successful read after
            // open just anchors the baseline (silent — the artist may
            // already be funded). Subsequent reads with a higher
            // balance toast the delta and roll baseline forward, so
            // multi-step funding ("send 5, then send 10") shows up as
            // two distinct toasts.
            if (fundPopupOpen_ && haveObservedXlm_) {
                if (!baselineSet_) {
                    baselineXlm_ = observedXlm_;
                    baselineSet_ = true;
                } else if (observedXlm_ > baselineXlm_ + 1e-7) {
                    const double delta = observedXlm_ - baselineXlm_;
                    std::ostringstream msg;
                    msg.setf(std::ios::fixed);
                    msg.precision(delta < 1.0 ? 4 : 2);
                    msg << "Received " << delta << " XLM";
                    // USERINFO is rendered as a Toolbar-overlay toast,
                    // which only exists in canvas mode. Also stash the
                    // line into fundReceivedMsg_ so the popup itself
                    // (visible on FileSelectScreen / settings) shows
                    // the receipt prominently.
                    Logger::get().log("USERINFO", msg.str());
                    fundReceivedMsg_ = msg.str();
                    baselineXlm_     = observedXlm_;
                }
            }

            // Self-chaining poll: kick the next probe while the popup
            // is open. set_to_layout() + the new in-flight fetch keep
            // render() being called, which keeps the chain alive.
            if (fundPopupOpen_) {
                refresh_balance(main, main.devKeys.app_pubkey());
            }
            gui.set_to_layout();
        } else if (status == FileDownloader::DownloadData::Status::FAILURE) {
            lastBalanceText_ = "Balance check failed (network error).";
            isFunded_ = false;
            pendingFetch_.reset();
            state_ = FetchState::Idle;
            // Retry on transient failure while polling for funds — the
            // next render will see Idle state and re-fire below.
            if (fundPopupOpen_) {
                refresh_balance(main, main.devKeys.app_pubkey());
            }
            gui.set_to_layout();
        }
    }

    if (!main.devKeys.is_loaded()) {
        text_label(gui, "App keypair not loaded — wallet unavailable.");
        return;
    }

    std::string pubkey = main.devKeys.app_pubkey();

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
        text_button(gui, "c2pa wallet toggle",
            open_ ? "[-] Wallet" : "[+] Wallet", {
            .wide = true,
            .onClick = [this] { open_ = !open_; },
        });
        if (open_) {
        text_label(gui, "Funding address (same as your app identity):");
        input_text_field(gui, "c2pa wallet pubkey display", "App pubkey", &pubkey, {
            .immutable = true,
        });
        text_button(gui, "c2pa wallet copy address", "Copy address", {
            .wide = true,
            .onClick = [&main] {
                main.input.set_clipboard_str(main.devKeys.app_pubkey());
            },
        });

        // Fund button. Opens a floating popup with the address + QR +
        // a live balance poll. QR is lazily encoded the first time the
        // popup opens, cached until the pubkey changes (rotation /
        // restore). On open, baselineSet_ is cleared so the next
        // successful Horizon read becomes the new baseline — the
        // delta-detect logic above then toasts every subsequent
        // increase.
        text_button(gui, "c2pa wallet fund", "Fund", {
            .wide = true,
            .onClick = [this, pubkey, &main] {
                fundPopupOpen_ = true;
                baselineSet_   = false;
                if (qrEncodedAddress_ != pubkey || !qrImage_) {
                    qrImage_          = render_qr_image(pubkey, 256);
                    qrEncodedAddress_ = pubkey;
                    if (!qrImage_) {
                        Logger::get().log("INFO",
                            "[C2PA::WalletPanel] QR encode returned null");
                    }
                }
                // Kick the first probe — subsequent probes self-chain
                // from the success/failure drain in render().
                refresh_balance(main, main.devKeys.app_pubkey());
            },
        });

        const std::string balanceLabel = state_ == FetchState::InFlight
            ? "Checking balance..."
            : (lastBalanceText_.empty()
                ? "Balance: (click Refresh to check)"
                : lastBalanceText_);
        text_label(gui, balanceLabel.c_str());

        text_button(gui, "c2pa wallet refresh balance", "Refresh balance", {
            .wide = true,
            .onClick = [this, &main] {
                refresh_balance(main, main.devKeys.app_pubkey());
            },
        });

        text_label_light(gui,
            "Approximately 5 XLM is needed in this account to register "
            "or maintain verifiable publishing. Fund it from any "
            "Stellar wallet (Lobstr, Solar, Freighter) — Inkternity "
            "never holds custody.");
        }  // if (open_)

        // Fund popup. Floating, attached to the GUIManager root so it
        // overlays the whole screen regardless of where this wallet
        // panel sits in the layout. Lives OUTSIDE the if (open_) block
        // so the popup persists even if the artist collapses the
        // wallet header while it's open. No outside-click dismiss to
        // avoid event-ordering races against the Fund button — Close
        // is the only path out.
        if (fundPopupOpen_) {
            gui.set_z_index(gui.get_z_index() + 100, [&] {
            CLAY_AUTO_ID({
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIT(420), .height = CLAY_SIZING_FIT(0)},
                    .padding = CLAY_PADDING_ALL(io.theme->padding1),
                    .childGap = io.theme->childGap1,
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_TOP },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
                .backgroundColor = convert_vec4<Clay_Color>(io.theme->backColor1),
                .cornerRadius = CLAY_CORNER_RADIUS(io.theme->windowCorners1),
                .floating = {
                    .zIndex = static_cast<int16_t>(gui.get_z_index()),
                    .attachPoints = {
                        .element = CLAY_ATTACH_POINT_CENTER_CENTER,
                        .parent  = CLAY_ATTACH_POINT_CENTER_CENTER,
                    },
                    .attachTo = CLAY_ATTACH_TO_ROOT,
                },
            }) {
                text_label_centered(gui, "Fund this wallet");
                text_label(gui,
                    "Send XLM to the address below. This window will "
                    "ping the network in the background and pop a "
                    "toast when the funds arrive.");

                input_text_field(gui, "c2pa fund popup address",
                    "App pubkey", &pubkey, { .immutable = true });
                text_button(gui, "c2pa fund popup copy",
                    "Copy address", {
                    .wide = true,
                    .onClick = [&main] {
                        main.input.set_clipboard_str(
                            main.devKeys.app_pubkey());
                    },
                });

                if (qrImage_) {
                    constexpr float kQrSide = 192.0f;
                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = {
                                .width  = CLAY_SIZING_FIXED(kQrSide),
                                .height = CLAY_SIZING_FIXED(kQrSide),
                            },
                        },
                        .backgroundColor = convert_vec4<Clay_Color>(io.theme->backColor2),
                    }) {
                        gui.element<MemoryImageDisplay>(
                            "c2pa fund popup qr",
                            MemoryImageDisplay::Data{
                                .img = qrImage_, .radius = 0.0f });
                    }
                }

                // Receipt banner. When the delta-detect branch above
                // fires, fundReceivedMsg_ holds e.g. "Received 10.00
                // XLM" — surface it inside the popup since the global
                // USERINFO toast only renders in canvas mode and
                // settings testers wouldn't otherwise see it.
                if (!fundReceivedMsg_.empty()) {
                    text_label_centered(gui, fundReceivedMsg_.c_str());
                }

                const std::string status = state_ == FetchState::InFlight
                    ? "Polling for funds..."
                    : (lastBalanceText_.empty()
                        ? "Polling for funds..."
                        : lastBalanceText_);
                text_label(gui, status.c_str());

                text_button(gui, "c2pa fund popup close",
                    "Close", {
                    .wide = true,
                    .onClick = [this] {
                        fundPopupOpen_ = false;
                        baselineSet_   = false;
                        fundReceivedMsg_.clear();
                    },
                });
            }
            });
        }
    }
}

}  // namespace C2PA
