#include "KeyStore.hpp"

#include "AppCa.hpp"

#include <Helpers/Logger.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

#include <openssl/crypto.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace C2PA {

namespace {

// Read entire file as a binary blob. Returns empty on any failure.
std::string read_file_binary(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamsize n = f.tellg();
    if (n < 0) return {};
    f.seekg(0, std::ios::beg);
    std::string out(static_cast<size_t>(n), '\0');
    f.read(out.data(), n);
    if (!f && !f.eof()) return {};
    return out;
}

// Write `bytes` to `final` atomically via final.tmp + rename. Returns
// true iff the rename completed and the bytes are at the final path.
// On failure, the .tmp may linger for forensic purposes.
bool write_file_atomic(const std::filesystem::path& finalPath,
                       const void* data, size_t len) {
    std::filesystem::path tmpPath = finalPath;
    tmpPath += ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
        if (!out) return false;
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmpPath, finalPath, ec);
    if (ec) {
        // rename can fail across drives or on Windows when the target
        // exists with a lock. Fall back to copy + remove so the bytes
        // still land. If copy also fails, the caller sees a save
        // failure and the prior good file remains untouched.
        std::filesystem::copy_file(tmpPath, finalPath,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
        std::filesystem::remove(tmpPath, ec);
    }
    return true;
}

// Restrict the file to owner-only read/write on POSIX. On Windows
// <APPDATA> is per-user-private by default; no DACL adjustment.
void lock_down_perms(const std::filesystem::path& p) {
#if defined(_WIN32)
    (void)p;
#else
    std::error_code ec;
    std::filesystem::permissions(p,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
#endif
}

bool ensure_dir(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return !ec;
}

}  // namespace

const char* registration_status_to_string(RegistrationStatus s) noexcept {
    switch (s) {
        case RegistrationStatus::Unregistered:   return "unregistered";
        case RegistrationStatus::PendingFunding: return "pending_funding";
        case RegistrationStatus::PendingToken:   return "pending_token";
        case RegistrationStatus::Active:         return "active";
        case RegistrationStatus::Revoked:        return "revoked";
    }
    return "unregistered";
}

RegistrationStatus registration_status_from_string(const std::string& s) noexcept {
    if (s == "pending_funding") return RegistrationStatus::PendingFunding;
    if (s == "pending_token")   return RegistrationStatus::PendingToken;
    if (s == "active")          return RegistrationStatus::Active;
    if (s == "revoked")         return RegistrationStatus::Revoked;
    return RegistrationStatus::Unregistered;
}

KeyStore::KeyStore(std::filesystem::path configPath)
    : configPath_(std::move(configPath)),
      c2paDir_(configPath_ / "c2pa"),
      keyPath_(c2paDir_ / "app_ca.key"),
      crtPath_(c2paDir_ / "app_ca.crt"),
      derPath_(c2paDir_ / "app_ca.der"),
      statePath_(c2paDir_ / "registration_status.json"),
      oldDir_(c2paDir_ / "old_ca") {}

bool KeyStore::has_saved_ca() const {
    std::error_code ec;
    return std::filesystem::exists(keyPath_, ec)
        && std::filesystem::exists(crtPath_, ec);
}

bool KeyStore::save(const AppCa& ca) {
    if (!ca.valid()) return false;
    if (!ensure_dir(c2paDir_)) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::KeyStore] could not create " + c2paDir_.string());
        return false;
    }

    // Pull the byte representations once. private_key_pem returns a
    // freshly-allocated buffer that we OPENSSL_cleanse before scope exit
    // (matches the DevKeys memzero pattern at src/DevKeys.cpp:80-82).
    auto keyPem = ca.private_key_pem();
    auto crtPem = ca.pem_bytes();
    auto der    = ca.der_bytes();
    if (keyPem.empty() || crtPem.empty() || der.empty()) {
        OPENSSL_cleanse(keyPem.data(), keyPem.size());
        return false;
    }

    bool ok = true;
    ok = ok && write_file_atomic(keyPath_, keyPem.data(), keyPem.size());
    if (ok) lock_down_perms(keyPath_);
    ok = ok && write_file_atomic(crtPath_, crtPem.data(), crtPem.size());
    ok = ok && write_file_atomic(derPath_, der.data(),    der.size());

    OPENSSL_cleanse(keyPem.data(), keyPem.size());

    if (!ok) {
        Logger::get().log("WORLDFATAL",
            "[C2PA::KeyStore] write failed under " + c2paDir_.string());
    }
    return ok;
}

AppCa KeyStore::load() const {
    if (!has_saved_ca()) return {};
    const std::string keyPem = read_file_binary(keyPath_);
    const std::string crtPem = read_file_binary(crtPath_);
    if (keyPem.empty() || crtPem.empty()) return {};
    AppCa ca = AppCa::load_from_pem(keyPem, crtPem);
    // No need to cleanse: AppCa's destructor wipes the parsed key via
    // EVP_PKEY_free; the keyPem buffer goes out of scope here. The
    // buffer's bytes are still in process memory until the std::string
    // dtor runs, but they're identical to what's on disk — leaking
    // them in a memory dump exposes nothing new.
    return ca;
}

bool KeyStore::rotate_current_to_old() {
    if (!has_saved_ca()) return true;  // no-op
    if (!ensure_dir(oldDir_)) return false;

    // Overwrite any prior old_ca contents — only one generation of
    // grace is kept per the plan (§3.4).
    std::error_code ec;
    auto move_or_copy = [&](const std::filesystem::path& src,
                             const std::filesystem::path& dst) -> bool {
        if (!std::filesystem::exists(src, ec)) return true;
        std::filesystem::remove(dst, ec);
        std::filesystem::rename(src, dst, ec);
        if (ec) {
            std::filesystem::copy_file(src, dst,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return false;
            std::filesystem::remove(src, ec);
        }
        return true;
    };

    bool ok = true;
    ok = ok && move_or_copy(keyPath_, oldDir_ / "app_ca.key");
    if (ok) lock_down_perms(oldDir_ / "app_ca.key");
    ok = ok && move_or_copy(crtPath_, oldDir_ / "app_ca.crt");
    ok = ok && move_or_copy(derPath_, oldDir_ / "app_ca.der");
    return ok;
}

RegistrationState KeyStore::load_state() const {
    RegistrationState state{};
    std::error_code ec;
    if (!std::filesystem::exists(statePath_, ec)) return state;
    const std::string contents = read_file_binary(statePath_);
    if (contents.empty()) return state;
    try {
        auto j = nlohmann::json::parse(contents);
        if (j.contains("status") && j["status"].is_string()) {
            state.status = registration_status_from_string(
                j["status"].get<std::string>());
        }
        if (j.contains("serial") && j["serial"].is_string()) {
            state.serial_hex = j["serial"].get<std::string>();
        }
        if (j.contains("expires_at") && j["expires_at"].is_number_integer()) {
            state.expires_at_unix = j["expires_at"].get<int64_t>();
        }
        if (j.contains("last_known_member_pubkey")
                && j["last_known_member_pubkey"].is_string()) {
            state.last_known_member_pubkey =
                j["last_known_member_pubkey"].get<std::string>();
        }
        if (j.contains("last_ttl_extend_at")
                && j["last_ttl_extend_at"].is_number_integer()) {
            state.last_ttl_extend_at_unix = j["last_ttl_extend_at"].get<int64_t>();
        }
    } catch (...) {
        return RegistrationState{};  // fall through to defaults on any parse error
    }
    return state;
}

bool KeyStore::save_state(const RegistrationState& state) {
    if (!ensure_dir(c2paDir_)) return false;
    nlohmann::json j = {
        {"status",                   registration_status_to_string(state.status)},
        {"serial",                   state.serial_hex},
        {"expires_at",               state.expires_at_unix},
        {"last_known_member_pubkey", state.last_known_member_pubkey},
        {"last_ttl_extend_at",       state.last_ttl_extend_at_unix},
    };
    const std::string dump = j.dump(2);
    return write_file_atomic(statePath_, dump.data(), dump.size());
}

}  // namespace C2PA
