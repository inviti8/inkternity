#include "WalletPanel.hpp"

#include "../MainProgram.hpp"
#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"

#include <Helpers/Logger.hpp>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>

namespace C2PA {

std::string WalletPanel::horizon_base_url() {
    if (const char* env = std::getenv("STELLAR_NETWORK")) {
        if (std::strcmp(env, "testnet") == 0)
            return "https://horizon-testnet.stellar.org";
    }
    return "https://horizon.stellar.org";
}

void WalletPanel::refresh_balance(const std::string& app_pubkey) {
    if (state_ == FetchState::InFlight) return;
    if (app_pubkey.empty()) return;

    const std::string url = horizon_base_url() + "/accounts/" + app_pubkey;
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
            if (s == 404) return "Not funded yet — send XLM to this address.";
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
            gui.set_to_layout();
        } else if (status == FileDownloader::DownloadData::Status::FAILURE) {
            lastBalanceText_ = "Balance check failed (network error).";
            isFunded_ = false;
            pendingFetch_.reset();
            state_ = FetchState::Idle;
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

        const std::string balanceLabel = state_ == FetchState::InFlight
            ? "Checking balance..."
            : (lastBalanceText_.empty()
                ? "Balance: (click Refresh to check)"
                : lastBalanceText_);
        text_label(gui, balanceLabel.c_str());

        text_button(gui, "c2pa wallet refresh balance", "Refresh balance", {
            .wide = true,
            .onClick = [this, &main] {
                refresh_balance(main.devKeys.app_pubkey());
            },
        });

        text_label_light(gui,
            "Approximately 5 XLM is needed in this account to register "
            "or maintain verifiable publishing. Fund it from any "
            "Stellar wallet (Lobstr, Solar, Freighter) — Inkternity "
            "never holds custody.");
    }
}

}  // namespace C2PA
