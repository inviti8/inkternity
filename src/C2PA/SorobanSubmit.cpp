#include "SorobanSubmit.hpp"

#include <Helpers/Logger.hpp>
#include <cstdlib>
#include <cstring>
#include <string>

namespace C2PA::Soroban {

namespace {

constexpr const char* kTestnetRpc        = "https://soroban-testnet.stellar.org";
constexpr const char* kTestnetPassphrase = "Test SDF Network ; September 2015";
constexpr const char* kMainnetRpc        = "https://mainnet.sorobanrpc.com";
constexpr const char* kMainnetPassphrase = "Public Global Stellar Network ; September 2015";

std::string hex_lower_of_bytes(const uint8_t* data, size_t len) {
    static constexpr char d[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i]     = d[(data[i] >> 4) & 0xf];
        out[2 * i + 1] = d[data[i] & 0xf];
    }
    return out;
}

std::string hex_lower_of_array32(const std::array<uint8_t, 32>& a) {
    return hex_lower_of_bytes(a.data(), a.size());
}

std::string hex_lower_of_vec(const std::vector<uint8_t>& v) {
    return hex_lower_of_bytes(v.data(), v.size());
}

// Extract a tx hash from the CLI's stdout. Recent stellar-cli prints
// either "ℹ️ Signed transaction: <hash>" or includes the hash on its
// own line. Heuristic: look for a 64-hex-char run.
std::string find_tx_hash(const std::string& out) {
    size_t i = 0;
    while (i + 64 <= out.size()) {
        size_t run = 0;
        while (i + run < out.size() && run < 64) {
            char c = out[i + run];
            const bool ok = (c >= '0' && c <= '9') ||
                            (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
            if (!ok) break;
            ++run;
        }
        if (run == 64) {
            // Make sure it's not inside a longer hex token.
            const bool leftBoundary  = (i == 0) || !std::isxdigit(static_cast<unsigned char>(out[i - 1]));
            const bool rightBoundary = (i + 64 == out.size()) ||
                                       !std::isxdigit(static_cast<unsigned char>(out[i + 64]));
            if (leftBoundary && rightBoundary) {
                return out.substr(i, 64);
            }
            i += run;
        } else {
            i += run + 1;
        }
    }
    return {};
}

// Common option set for any stellar contract invoke call. Builds the
// argv slice up to (but not including) the "--" separator.
std::vector<std::string> base_invoke_args(
        const RpcConfig& rpc,
        std::string_view contract_id,
        std::string_view source_account,
        bool send_no /* read-only */) {
    std::vector<std::string> v;
    v.reserve(16);
    v.emplace_back("contract");
    v.emplace_back("invoke");
    v.emplace_back("--id");          v.emplace_back(std::string(contract_id));
    v.emplace_back("--source-account"); v.emplace_back(std::string(source_account));
    v.emplace_back("--rpc-url");     v.emplace_back(rpc.rpc_url);
    v.emplace_back("--network-passphrase"); v.emplace_back(rpc.network_passphrase);
    if (send_no) { v.emplace_back("--send"); v.emplace_back("no"); }
    v.emplace_back("--");
    return v;
}

}  // namespace

RpcConfig testnet_config() noexcept {
    return RpcConfig{ kTestnetRpc, kTestnetPassphrase };
}

RpcConfig mainnet_config() noexcept {
    return RpcConfig{ kMainnetRpc, kMainnetPassphrase };
}

RpcConfig config_for(GlobalConfig::StellarNetwork n) {
    return n == GlobalConfig::StellarNetwork::Testnet
        ? testnet_config()
        : mainnet_config();
}

RpcConfig config_for_env_or_global(const GlobalConfig& conf) {
    if (const char* env = std::getenv("STELLAR_NETWORK")) {
        if (std::strcmp(env, "testnet") == 0) return testnet_config();
        if (std::strcmp(env, "mainnet") == 0) return mainnet_config();
    }
    return config_for(conf.stellarNetwork);
}

InvokeResult submit_register(
        const StellarCli& cli,
        const RpcConfig& rpc,
        std::string_view contract_id,
        std::string_view source_account,
        std::string_view app_address,
        const std::array<uint8_t, 32>& member_pubkey,
        std::string_view app_kind,
        const std::array<uint8_t, 32>& fingerprint,
        uint64_t expires_at,
        uint64_t nonce,
        const std::vector<uint8_t>& auth_payload,
        const std::vector<uint8_t>& auth_signature) {
    InvokeResult r;

    if (auth_signature.size() != 64) {
        r.error = "auth_signature must be 64 bytes";
        return r;
    }
    if (cli.binary_path().empty()) {
        r.error = "stellar CLI not available";
        return r;
    }

    auto argv = base_invoke_args(rpc, contract_id, source_account, /*send_no=*/false);
    // CLI scval arg format (confirmed against deployed contract):
    //   Address      → bare strkey ("GABC..."), no quotes
    //   String       → JSON-quoted ('"ed25519"')      \  routed through
    //   enum variant → JSON-quoted ('"Inkternity"')   /  --<name>-file-path
    //   Bytes/BytesN → bare hex
    //   integers     → bare numbers
    //
    // JSON-quoted args go via temp file (StellarCli::make_json_arg_file)
    // to dodge the SDL_CreateProcess Windows quoting bug on long
    // network-bound argv. See docs/design/C2PA.md §10 notes.
    auto jstr = [](std::string_view s) {
        return std::string("\"") + std::string(s) + "\"";
    };
    auto kindFile = cli.make_json_arg_file(jstr(app_kind));
    auto algFile  = cli.make_json_arg_file(jstr("ed25519"));
    if (!kindFile.valid() || !algFile.valid()) {
        r.error = "Could not stage scval JSON arg files";
        return r;
    }

    argv.emplace_back("register_app_ca");
    argv.emplace_back("--app_address");          argv.emplace_back(std::string(app_address));
    argv.emplace_back("--member_pubkey");        argv.emplace_back(hex_lower_of_array32(member_pubkey));
    argv.emplace_back("--app_kind-file-path");   argv.emplace_back(kindFile.path().string());
    argv.emplace_back("--fingerprint");          argv.emplace_back(hex_lower_of_array32(fingerprint));
    argv.emplace_back("--pubkey_alg-file-path"); argv.emplace_back(algFile.path().string());
    argv.emplace_back("--expires_at");           argv.emplace_back(std::to_string(expires_at));
    argv.emplace_back("--nonce");                argv.emplace_back(std::to_string(nonce));
    argv.emplace_back("--auth_payload");         argv.emplace_back(hex_lower_of_vec(auth_payload));
    argv.emplace_back("--auth_signature");       argv.emplace_back(hex_lower_of_vec(auth_signature));

    auto out = cli.invoke(argv);
    r.raw_out = std::move(out.out);
    r.tx_hash = find_tx_hash(r.raw_out);
    if (out.ok()) {
        r.success = true;
    } else {
        r.error = "stellar contract invoke exited " + std::to_string(out.exit_code);
        if (out.spawn_failed) r.error = "subprocess spawn failed";
        Logger::get().log("INFO",
            "[Soroban] register_app_ca failed: " + r.error + "\n--- output ---\n" + r.raw_out);
    }
    return r;
}

std::optional<bool> is_trusted(
        const StellarCli& cli,
        const RpcConfig& rpc,
        std::string_view contract_id,
        std::string_view source_account,
        std::string_view app_address,
        const std::array<uint8_t, 32>& fingerprint) {
    if (cli.binary_path().empty()) return std::nullopt;

    auto argv = base_invoke_args(rpc, contract_id, source_account, /*send_no=*/true);
    argv.emplace_back("is_trusted");
    argv.emplace_back("--app_address"); argv.emplace_back(std::string(app_address));
    argv.emplace_back("--fingerprint"); argv.emplace_back(hex_lower_of_array32(fingerprint));

    auto out = cli.invoke(argv);
    if (!out.ok()) {
        Logger::get().log("INFO",
            "[Soroban] is_trusted invocation failed (exit " +
            std::to_string(out.exit_code) + "):\n" + out.out);
        return std::nullopt;
    }

    // The CLI prints the simulated return value at the end of stdout.
    // For a bool result it's "true" or "false" on its own line, often
    // surrounded by quotes or trailing whitespace. Heuristic: find the
    // last occurrence of either token.
    auto find_last = [&out](const std::string& needle) -> size_t {
        return out.out.rfind(needle);
    };
    const size_t t = find_last("true");
    const size_t f = find_last("false");
    if (t == std::string::npos && f == std::string::npos) return std::nullopt;
    if (t == std::string::npos) return false;
    if (f == std::string::npos) return true;
    return t > f;
}

}  // namespace C2PA::Soroban
