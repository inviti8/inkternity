#include "StellarPay.hpp"

#include <Helpers/Logger.hpp>

#include <cctype>

namespace AI {

// ---- to_stroops: pure, available on every platform -----------------------

bool StellarPay::to_stroops(const std::string& decimalAmount, std::string& outStroops) {
    // Trim surrounding whitespace.
    size_t b = 0, e = decimalAmount.size();
    while (b < e && std::isspace(static_cast<unsigned char>(decimalAmount[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(decimalAmount[e - 1]))) --e;
    if (b == e) return false;
    const std::string s = decimalAmount.substr(b, e - b);

    const size_t dot = s.find('.');
    std::string intPart = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string fracPart = (dot == std::string::npos) ? "" : s.substr(dot + 1);

    if (intPart.empty()) intPart = "0";           // ".5" → "0.5"
    if (fracPart.size() > 7) return false;          // finer than a stroop
    auto allDigits = [](const std::string& x) {
        if (x.empty()) return false;
        for (char c : x) if (c < '0' || c > '9') return false;
        return true;
    };
    if (!allDigits(intPart)) return false;
    if (!fracPart.empty() && !allDigits(fracPart)) return false;

    fracPart.append(7 - fracPart.size(), '0');      // pad to 7 fractional digits
    std::string combined = intPart + fracPart;
    // Strip leading zeros, keep at least one digit.
    size_t nz = combined.find_first_not_of('0');
    outStroops = (nz == std::string::npos) ? "0" : combined.substr(nz);
    return true;
}

}  // namespace AI

#ifndef __EMSCRIPTEN__

#include "../C2PA/StellarCli.hpp"

#include <nlohmann/json.hpp>

namespace AI {
namespace {

using C2PA::StellarCli;

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// The single bounded 64-hex token in `out`, or empty. Same scrape C2PA's
// SorobanSubmit uses for the contract-invoke tx hash.
std::string find_tx_hash(const std::string& out) {
    size_t i = 0;
    while (i + 64 <= out.size()) {
        size_t run = 0;
        while (i + run < out.size() && run < 64) {
            const char c = out[i + run];
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
            if (!ok) break;
            ++run;
        }
        if (run == 64) {
            const bool lb = (i == 0) || !std::isxdigit(static_cast<unsigned char>(out[i - 1]));
            const bool rb = (i + 64 == out.size()) ||
                            !std::isxdigit(static_cast<unsigned char>(out[i + 64]));
            if (lb && rb) return out.substr(i, 64);
            i += run;
        } else {
            i += run + 1;
        }
    }
    return {};
}

// Longest base64-charset line in `out`. StellarCli merges stderr into stdout,
// so we can't assume a lone line even with --quiet; pick the XDR by shape.
std::string extract_xdr(const std::string& out) {
    std::string best;
    size_t start = 0;
    while (start <= out.size()) {
        size_t nl = out.find('\n', start);
        std::string line = trim(out.substr(start, (nl == std::string::npos ? out.size() : nl) - start));
        if (line.size() >= 40) {
            bool b64 = true;
            for (char c : line) {
                const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
                if (!ok) { b64 = false; break; }
            }
            if (b64 && line.size() > best.size()) best = line;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return best;
}

// Run one `stellar` subcommand, always quiet so INFO logs don't pollute stdout.
StellarCli::InvocationResult run(const StellarCli& cli, std::vector<std::string> args) {
    args.emplace_back("--quiet");
    return cli.invoke(args);
}

std::string asset_arg(const std::string& code, const std::string& issuer) {
    return code + ":" + issuer;
}

}  // namespace

PayResult StellarPay::ensure_trustline(const StellarCli& cli,
                                       const std::string& sourceSecret,
                                       const std::string& assetCode,
                                       const std::string& issuer,
                                       const std::string& rpcUrl,
                                       const std::string& networkPassphrase) {
    PayResult r;
    if (cli.binary_path().empty()) { r.error = "stellar CLI not available"; return r; }

    // One-shot build+sign+send: --source-account takes the S-secret and signs.
    auto res = run(cli, {
        "tx", "new", "change-trust",
        "--line", asset_arg(assetCode, issuer),
        "--source-account", sourceSecret,
        "--rpc-url", rpcUrl,
        "--network-passphrase", networkPassphrase,
    });
    r.raw = res.out;
    if (res.ok()) {
        r.ok = true;
        r.tx_hash = find_tx_hash(res.out);
        return r;
    }
    // A trustline that already exists is success for our purposes.
    if (res.out.find("op_low_reserve") != std::string::npos) {
        r.error = "not enough XLM to open a USDC trustline (need ~0.5 XLM reserve)";
    } else if (res.out.find("already") != std::string::npos ||
               res.out.find("op_success") != std::string::npos) {
        r.ok = true;   // idempotent
    } else {
        r.error = "change-trust failed (exit " + std::to_string(res.exit_code) + ")";
        if (res.spawn_failed) r.error = "stellar CLI spawn failed";
    }
    return r;
}

PayResult StellarPay::pay(const StellarCli& cli,
                          const std::string& sourcePubkey,
                          const std::string& sourceSecret,
                          const std::string& payTo,
                          const std::string& assetCode,
                          const std::string& issuer,
                          const std::string& amountStroops,
                          const std::string& memo,
                          const std::string& rpcUrl,
                          const std::string& networkPassphrase) {
    PayResult r;
    if (cli.binary_path().empty()) { r.error = "stellar CLI not available"; return r; }
    if (memo.size() > 28) {         // MEMO_TEXT hard limit; never truncate
        r.error = "payment memo exceeds MEMO_TEXT 28-byte limit";
        return r;
    }

    // 1. Build unsigned (CLI fetches sequence from RPC). Source = G pubkey so it
    //    does not try to sign here.
    auto built = run(cli, {
        "tx", "new", "payment", "--build-only",
        "--source-account", sourcePubkey,
        "--destination", payTo,
        "--asset", asset_arg(assetCode, issuer),
        "--amount", amountStroops,
        "--rpc-url", rpcUrl,
        "--network-passphrase", networkPassphrase,
    });
    r.raw = built.out;
    if (!built.ok()) {
        r.error = "payment build failed (exit " + std::to_string(built.exit_code) + ")";
        if (built.spawn_failed) r.error = "stellar CLI spawn failed";
        return r;
    }
    const std::string xdr = extract_xdr(built.out);
    if (xdr.empty()) { r.error = "could not read built transaction XDR"; return r; }

    // 2. Decode to JSON.
    auto decoded = run(cli, {"tx", "decode", xdr});
    if (!decoded.ok()) { r.error = "tx decode failed"; r.raw = decoded.out; return r; }
    const size_t brace = decoded.out.find('{');
    if (brace == std::string::npos) { r.error = "tx decode produced no JSON"; r.raw = decoded.out; return r; }
    auto j = nlohmann::json::parse(decoded.out.substr(brace), nullptr, /*exceptions=*/false);
    if (j.is_discarded() || !j.is_object() ||
        !j.contains("tx") || !j["tx"].is_object() ||
        !j["tx"].contains("tx") || !j["tx"]["tx"].is_object()) {
        r.error = "unexpected transaction envelope shape";
        r.raw = decoded.out;
        return r;
    }

    // 3. Inject MEMO_TEXT (shape confirmed §2.2: {"text": <memo>}).
    j["tx"]["tx"]["memo"] = nlohmann::json{{"text", memo}};

    // 4. Stage the JSON to a temp file (long, quoted — must not go on argv) and
    //    re-encode to XDR.
    auto jf = cli.make_json_arg_file(j.dump());
    if (!jf.valid()) { r.error = "could not stage envelope JSON"; return r; }
    auto encoded = run(cli, {"tx", "encode", jf.path().string()});
    if (!encoded.ok()) { r.error = "tx encode failed"; r.raw = encoded.out; return r; }
    const std::string xdr2 = extract_xdr(encoded.out);
    if (xdr2.empty()) { r.error = "could not read memo'd transaction XDR"; r.raw = encoded.out; return r; }

    // 5. Sign (needs the passphrase to compute the tx hash — and 23.4.1 also
    //    requires --rpc-url alongside it), then send.
    auto signed_ = run(cli, {
        "tx", "sign", xdr2,
        "--sign-with-key", sourceSecret,
        "--rpc-url", rpcUrl,
        "--network-passphrase", networkPassphrase,
    });
    if (!signed_.ok()) { r.error = "tx sign failed"; r.raw = signed_.out; return r; }
    const std::string sxdr = extract_xdr(signed_.out);
    if (sxdr.empty()) { r.error = "could not read signed transaction XDR"; r.raw = signed_.out; return r; }

    auto sent = run(cli, {
        "tx", "send", sxdr,
        "--rpc-url", rpcUrl,
        "--network-passphrase", networkPassphrase,
    });
    r.raw = sent.out;
    if (!sent.ok()) {
        if (sent.out.find("op_no_trust") != std::string::npos)
            r.error = "destination/asset trustline missing (op_no_trust)";
        else if (sent.out.find("op_underfunded") != std::string::npos ||
                 sent.out.find("tx_insufficient_balance") != std::string::npos)
            r.error = "insufficient USDC/XLM balance to pay";
        else
            r.error = "tx send failed (exit " + std::to_string(sent.exit_code) + ")";
        return r;
    }
    r.tx_hash = find_tx_hash(sent.out);
    if (r.tx_hash.empty()) { r.error = "payment sent but no tx hash in output"; return r; }
    r.ok = true;
    return r;
}

}  // namespace AI

#else  // __EMSCRIPTEN__

namespace AI {
PayResult StellarPay::ensure_trustline(const C2PA::StellarCli&, const std::string&,
        const std::string&, const std::string&, const std::string&, const std::string&) {
    PayResult r; r.error = "payments unavailable on this platform"; return r;
}
PayResult StellarPay::pay(const C2PA::StellarCli&, const std::string&, const std::string&,
        const std::string&, const std::string&, const std::string&, const std::string&,
        const std::string&, const std::string&, const std::string&) {
    PayResult r; r.error = "payments unavailable on this platform"; return r;
}
}  // namespace AI

#endif
