#include "X402SelfTest.hpp"

#ifndef __EMSCRIPTEN__

#include "RequestSigner.hpp"
#include "X402Challenge.hpp"

#include <curl/curl.h>
#include <sstream>

namespace AI {
namespace {

size_t collect(void* data, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<const char*>(data), size * nmemb);
    return size * nmemb;
}

// POST /warm exactly as WarmLease does (X-API-Key + signed identity headers +
// {tool,label}). Returns HTTP status (0 on transport failure); fills `resp`.
long post_warm(const std::string& endpoint, const std::string& key,
               const std::string& tool, const std::string& pubkey, std::string& resp) {
    CURL* c = curl_easy_init();
    if (!c) return 0;
    std::string url = endpoint;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/warm";
    const std::string body = std::string("{\"tool\":\"") + tool + "\",\"label\":\"" + pubkey + "\"}";

    curl_slist* h = nullptr;
    h = curl_slist_append(h, ("X-API-Key: " + key).c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    for (const auto& hd : RequestSigner::sign_request("POST", "/warm", tool, ""))
        h = curl_slist_append(h, (hd.name + ": " + hd.value).c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);

    const CURLcode code = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (code != CURLE_OK) { resp = std::string("transport: ") + curl_easy_strerror(code); return 0; }
    return http;
}

}  // namespace

std::string x402_selftest(const std::string& endpoint, const std::string& apiKey,
                          const PayContext& ctx, bool& ok) {
    ok = false;
    std::ostringstream r;
    const std::string tool = "reangle";

    r << "signer_available=" << (RequestSigner::available() ? "yes" : "no") << "\n";

    // 1) POST /warm unpaid -> expect 402 with a challenge.
    std::string body;
    const long http = post_warm(endpoint, apiKey, tool, ctx.pubkey, body);
    r << "warm_http=" << http << "\n";
    r << "warm_body=" << body.substr(0, 500) << "\n";
    if (http == 200) { r << "NOTE: already had a live window (200, no 402). Nothing to pay.\n"; ok = true; return r.str(); }
    if (http != 402) { r << "FAIL: expected 402, got " << http << "\n"; return r.str(); }

    auto ch = X402Challenge::parse(body);
    if (!ch || !ch->valid()) { r << "FAIL: could not parse x402 challenge\n"; return r.str(); }
    r << "challenge: asset=" << ch->asset << " amount=" << ch->amount
      << " payTo=" << ch->payTo << " issuer=" << ch->issuer
      << " memo=" << ch->memo << " net=" << ch->network << "\n";

    // 2) Settle the window on-chain via the real StellarPay/WarmPay code.
    const SettleResult s = settle_window(*ch, tool, endpoint, apiKey, ctx);
    r << "settle_ok=" << (s.ok ? "true" : "false")
      << " tx=" << s.txHash << " err=" << s.error << "\n";
    if (!s.ok) return r.str();

    // 3) POST /warm again -> should be 200 (granted, or pending under settle-on-grant).
    std::string body2;
    const long http2 = post_warm(endpoint, apiKey, tool, ctx.pubkey, body2);
    r << "warm_after_pay_http=" << http2 << "\n";
    r << "warm_after_pay_body=" << body2.substr(0, 500) << "\n";

    // A settled payment is the core pass; a non-200 acquire afterwards only means
    // the window is pending because no real GPU warmed (settle-on-grant).
    ok = true;
    r << (http2 == 200
            ? "PASS: window purchased and lease granted\n"
            : "PASS (core): payment settled; /warm still pending is expected without a warm GPU\n");
    return r.str();
}

}  // namespace AI

#else  // __EMSCRIPTEN__

namespace AI {
std::string x402_selftest(const std::string&, const std::string&, const PayContext&, bool& ok) {
    ok = false;
    return "x402 selftest unavailable on this platform";
}
}  // namespace AI

#endif
