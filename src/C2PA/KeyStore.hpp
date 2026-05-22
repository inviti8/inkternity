#pragma once
// docs/design/C2PA.md §5 — disk persistence for the app CA + registration
// status. Sits next to DevKeys' inkternity_dev_keys.json under
// <configPath>/c2pa/. No backup, no recovery — device loss = reinstall +
// new CA + new on-chain registration (matches C2PA.md §8 custody model).
//
// Atomic writes via .tmp + rename, same shape as World::save_to_file.
// PKCS#8 PEM is written unencrypted; device-level security is the
// perimeter (matches the existing inkternity_dev_keys.json model per
// src/DevKeys.cpp:148-149). Encryption-at-rest is deliberately scoped
// OUT (§11 of the plan); upgrading both at once is a separate review.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace C2PA {

class AppCa;

enum class RegistrationStatus {
    Unregistered,     // No bundle emitted yet
    PendingFunding,   // Bundle emitted, balance below threshold
    PendingToken,     // Bundle emitted + funded, waiting for Portal wire token
    Active,           // register_app_ca submitted successfully + is_trusted == true
    Revoked,          // revoke_by_app submitted (verifier still works for prior signatures)
};

const char* registration_status_to_string(RegistrationStatus s) noexcept;
RegistrationStatus registration_status_from_string(const std::string& s) noexcept;

struct RegistrationState {
    RegistrationStatus status = RegistrationStatus::Unregistered;
    // Hex (lowercase, 40 chars) of the on-chain record's serial when
    // active; empty otherwise. Read back from get_app_ca to detect
    // drift between local state and registry.
    std::string serial_hex;
    int64_t expires_at_unix = 0;
    // G... strkey of the member who authorized this app instance, as
    // returned by get_app_ca.member_pubkey. Empty if not active.
    std::string last_known_member_pubkey;
    // Unix seconds of the last successful ExtendFootprintTTLOp against
    // the on-chain AppCa(app_address) entry. Drives the 20-day re-extend
    // cadence in RegistrationFlow's Active branch — stellar mainnet caps
    // a single extend at ~30 days (535,679 ledgers), so we re-extend
    // every 20 days to keep ~10 days of margin against archival. Zero
    // on a fresh install OR on upgrade from a pre-rc10 install (which
    // hadn't tracked this field); the trigger logic treats zero as
    // "due now" so existing testers get a topped-up entry on first
    // settings open after upgrade.
    int64_t last_ttl_extend_at_unix = 0;
};

class KeyStore {
public:
    // `configPath` is the directory main.cpp:357-361 resolves at startup
    // (next to inkternity_dev_keys.json, p2p.json, etc.). The c2pa/
    // subdirectory is lazily created on first save.
    explicit KeyStore(std::filesystem::path configPath);

    const std::filesystem::path& c2pa_dir() const noexcept { return c2paDir_; }

    bool has_saved_ca() const;

    // Persist app_ca.key (PKCS#8 PEM), app_ca.crt (PEM), app_ca.der
    // (binary cache for fingerprint comparison). Atomic per file; if
    // any one fails, prior good state on disk is left intact. On
    // POSIX, key file mode is set to 0600 after rename; on Windows
    // <APPDATA> is already per-user-private so no DACL adjustment.
    // Returns true iff all three files landed.
    bool save(const AppCa& ca);

    // Build an AppCa from the on-disk PEM pair. Returns an invalid
    // AppCa (valid() == false) if either file is missing, unreadable,
    // mismatched, or doesn't parse.
    AppCa load() const;

    // Move the current app_ca.{key,crt,der} into c2pa/old_ca/, keeping
    // them around for the 30-day verifier-side grace period
    // (C2PA.md §3.4 rotation). Overwrites any prior old_ca contents.
    // No-op if no current CA. Returns true on success or no-op.
    bool rotate_current_to_old();

    // Atomic JSON read/write for c2pa/registration_status.json.
    // Defaults to {Unregistered, "", 0, ""} if absent or unparseable.
    RegistrationState load_state() const;
    bool save_state(const RegistrationState& state);

private:
    std::filesystem::path configPath_;
    std::filesystem::path c2paDir_;
    std::filesystem::path keyPath_;
    std::filesystem::path crtPath_;
    std::filesystem::path derPath_;
    std::filesystem::path statePath_;
    std::filesystem::path oldDir_;
};

}  // namespace C2PA
