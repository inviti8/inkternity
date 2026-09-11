#include "WarmPay.hpp"

#include <Helpers/Logger.hpp>

#ifndef __EMSCRIPTEN__

#include "StellarPay.hpp"
#include "RequestSigner.hpp"
#include "../C2PA/SorobanSubmit.hpp"
#include "../GlobalConfig.hpp"

#include <cctype>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace AI {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Canonical net name: "mainnet" | "testnet" | "" (unknown).
std::string canon_net(const std::string& n) {
    const std::string s = lower(n);
    if (s == "public" || s == "mainnet" || s == "pubnet") return "mainnet";
    if (s == "testnet" || s == "test") return "testnet";
    return "";
}

size_t collect(void* data, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<const char*>(data), size * nmemb);
    return size * nmemb;
}

std::string warm_pay_url(const std::string& baseUrl) {
    std::string u = baseUrl;
    if (!u.empty() && u.back() == '/') u.pop_back();
    return u + "/warm/pay";
}

// POST /warm/pay. Returns HTTP status (0 on transport failure); fills `resp`.
long post_warm_pay(const std::string& baseUrl, const std::string& apiKey,
                   const std::string& tool, const std::string& priceId,
                   const std::string& txHash, std::string& resp) {
    CURL* c = curl_easy_init();
    if (!c) return 0;
    const std::string url = warm_pay_url(baseUrl);
    const std::string body = std::string("{\"price_id\":\"") + priceId +
                             "\",\"tool\":\"" + tool + "\"}";

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-API-Key: " + apiKey).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("X-Payment: " + txHash).c_str());
    for (const auto& h : RequestSigner::sign_request("POST", "/warm/pay", tool, ""))
        headers = curl_slist_append(headers, (h.name + ": " + h.value).c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);

    const CURLcode code = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (code != CURLE_OK) {
        Logger::get().cross_platform_println(std::string("[warmpay] /warm/pay transport: ") +
                                             curl_easy_strerror(code));
        return 0;
    }
    return http;
}

bool looks_like_no_trust(const std::string& err) {
    return err.find("op_no_trust") != std::string::npos ||
           err.find("trustline missing") != std::string::npos;
}

}  // namespace

SettleResult settle_window(const X402Challenge& ch, const std::string& tool,
                           const std::string& baseUrl, const std::string& apiKey,
                           const PayContext& ctx) {
    SettleResult r;
    if (!ch.valid())            { r.error = "incomplete payment challenge"; return r; }
    if (!ctx.cli)               { r.error = "payment tooling unavailable"; return r; }
    if (ctx.secret.empty() || ctx.pubkey.empty()) { r.error = "no wallet identity"; return r; }

    // Never pay on the wrong network.
    const std::string net = canon_net(ch.network);
    if (net.empty()) { r.error = "unknown payment network '" + ch.network + "'"; return r; }
    if (!ctx.expectedNetwork.empty() && canon_net(ctx.expectedNetwork) != net) {
        r.error = "challenge network (" + net + ") != app network (" +
                  canon_net(ctx.expectedNetwork) + ")";
        return r;
    }
    const auto rpc = C2PA::Soroban::config_for(
        net == "mainnet" ? GlobalConfig::StellarNetwork::Mainnet
                         : GlobalConfig::StellarNetwork::Testnet);

    std::string stroops;
    if (!StellarPay::to_stroops(ch.amount, stroops)) {
        r.error = "unparseable amount '" + ch.amount + "'";
        return r;
    }

    // Pay. Self-heal a missing trustline once, then retry.
    auto pay = StellarPay::pay(*ctx.cli, ctx.pubkey, ctx.secret, ch.payTo, ch.asset,
                               ch.issuer, stroops, ch.memo, rpc.rpc_url, rpc.network_passphrase);
    if (!pay.ok && looks_like_no_trust(pay.error)) {
        Logger::get().cross_platform_println("[warmpay] no trustline; creating one for " + ch.asset);
        auto tl = StellarPay::ensure_trustline(*ctx.cli, ctx.secret, ch.asset, ch.issuer,
                                               rpc.rpc_url, rpc.network_passphrase);
        if (!tl.ok) {
            r.error = tl.error.empty() ? "could not create USDC trustline" : tl.error;
            r.needsFunds = tl.error.find("XLM") != std::string::npos;
            return r;
        }
        pay = StellarPay::pay(*ctx.cli, ctx.pubkey, ctx.secret, ch.payTo, ch.asset,
                              ch.issuer, stroops, ch.memo, rpc.rpc_url, rpc.network_passphrase);
    }
    if (!pay.ok) {
        r.error = pay.error.empty() ? "payment failed" : pay.error;
        r.needsFunds = pay.error.find("insufficient") != std::string::npos ||
                       pay.error.find("XLM") != std::string::npos;
        return r;
    }
    r.txHash = pay.tx_hash;

    // Present the proof.
    std::string resp;
    const long http = post_warm_pay(baseUrl, apiKey, tool, ch.priceId, pay.tx_hash, resp);
    if (http == 200) {
        r.ok = true;
        Logger::get().cross_platform_println("[warmpay] window settled, tx=" + pay.tx_hash);
        return r;
    }
    // The payment landed on-chain but /warm/pay didn't accept it. The tx hash is
    // idempotent server-side, so a later resubmit is safe (see plan §4/§7).
    std::string detail;
    auto j = nlohmann::json::parse(resp, nullptr, false);
    if (!j.is_discarded() && j.is_object()) detail = j.value("detail", "");
    r.error = "payment sent (tx=" + pay.tx_hash + ") but /warm/pay HTTP " +
              std::to_string(http) + (detail.empty() ? "" : (": " + detail));
    return r;
}

}  // namespace AI

#else  // __EMSCRIPTEN__

namespace AI {
SettleResult settle_window(const X402Challenge&, const std::string&, const std::string&,
                           const std::string&, const PayContext&) {
    SettleResult r; r.error = "payments unavailable on this platform"; return r;
}
}  // namespace AI

#endif
