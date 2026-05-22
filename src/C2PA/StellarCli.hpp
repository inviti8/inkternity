#pragma once
// docs/design/C2PA.md §8.2 (revised) — Soroban contract invocation via
// the official `stellar` CLI as a subprocess. Plan §10/Q11 was originally
// resolved to a hand-rolled libcurl + scval encoder, but the real porting
// surface for stellar-sdk is ~10K LOC of XDR + transaction-building work
// rather than the ~250 LOC the plan estimated. Founder decision
// (2026-05-21): pivot to shell-out, with auto-install of the CLI on
// first need so the artist never has to install Rust + Cargo + stellar
// manually.
//
// Resolution order on each launch:
//   1. `stellar` on PATH (system install — used as-is).
//   2. Cached binary under <configPath>/c2pa/bin/.
//   3. Auto-install: GitHub Releases download + extract + verify.
//
// The auto-install is async (FileDownloader pattern) and surfaces stage
// + progress so the wallet panel (I7) and registration walkthrough (I10)
// can show "Setting up provenance system..." while the binary lands.
// No crypto-talk in user-facing copy per [[feedback-crypto-averse-users]].

#include <Helpers/FileDownloader.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace C2PA {

class StellarCli {
public:
    // Bump this constant to ship a newer CLI. Auto-install pulls
    // exactly this version from GitHub Releases. A `stellar` already
    // on PATH is used regardless of its version — version mismatch is
    // not enforced (the contract surface is stable enough that any
    // recent CLI works).
    static constexpr const char* PINNED_VERSION = "23.4.1";

    enum class Availability {
        Unknown,             // not yet probed
        SystemPath,          // found on $PATH
        Cached,              // found at <configPath>/c2pa/bin/
        NotAvailable,        // neither found yet — needs auto-install
        InstallInProgress,
        InstallFailed,
    };

    enum class InstallStage {
        Idle,
        Downloading,
        Extracting,
        Verifying,
        Done,
        Failed,
    };

    explicit StellarCli(std::filesystem::path configPath);

    // Synchronous probe — PATH check + cache check. Cheap. Updates
    // internal state; subsequent binary_path() returns the resolved
    // path on success or empty on NotAvailable / InstallFailed.
    Availability probe();

    // Kick off async install if probe() returned NotAvailable. No-op
    // if currently installing or already available. Returns true iff
    // a new install was started this call.
    bool start_install();

    // Pump the install state machine — call once per frame from the
    // UI thread (drains FileDownloader status, runs extraction on the
    // SUCCESS edge, verifies the resulting binary). Cheap when idle.
    void poll_install();

    Availability availability() const noexcept { return availability_; }
    InstallStage stage()        const noexcept { return stage_; }
    float        progress()     const noexcept { return progress_; }
    const std::string& last_error() const noexcept { return lastError_; }

    // Resolved binary path. Empty if neither system nor cached is
    // available yet. Stable across an install completion (set by
    // poll_install on the Done edge).
    const std::filesystem::path& binary_path() const noexcept { return binaryPath_; }

    // Blocking subprocess invocation. Captures stdout. stderr is merged
    // into stdout via the SDL_PROCESS_STDIO_REDIRECT pattern so callers
    // see the full diagnostic if a `stellar` command fails. Call from
    // a worker thread for Soroban operations (the round-trip can take
    // 5-10s on testnet for state-changing calls).
    struct InvocationResult {
        int exit_code = -1;
        std::string out;
        bool spawn_failed = false;
        // True iff exit_code == 0. False on spawn failure or non-zero exit.
        bool ok() const noexcept { return !spawn_failed && exit_code == 0; }
    };
    InvocationResult invoke(const std::vector<std::string>& args) const;

    // RAII wrapper around a temporary JSON-arg file used with
    // `stellar contract invoke ... -- <fn> --<name>-file-path <path>`.
    // SDL_CreateProcess on Windows mishandles long argv lists with
    // embedded JSON-quoted strings (STATUS_STACK_BUFFER_OVERRUN
    // observed empirically against the live registry); routing JSON
    // values through a file sidesteps the argv encoder entirely. The
    // CLI accepts this for any scval type — String, enum variants,
    // even Bytes (raw bytes for the latter).
    //
    // Path is constructed under <configPath>/c2pa/tmp/ with a random
    // suffix so concurrent calls don't collide. Destructor removes
    // the file. Move-only; copies are deleted.
    class ScopedJsonFile {
    public:
        ScopedJsonFile() = default;
        explicit ScopedJsonFile(std::filesystem::path p) noexcept
            : path_(std::move(p)) {}
        ScopedJsonFile(const ScopedJsonFile&) = delete;
        ScopedJsonFile& operator=(const ScopedJsonFile&) = delete;
        ScopedJsonFile(ScopedJsonFile&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
        ScopedJsonFile& operator=(ScopedJsonFile&& o) noexcept;
        ~ScopedJsonFile() noexcept;
        const std::filesystem::path& path() const noexcept { return path_; }
        bool valid() const noexcept { return !path_.empty(); }
    private:
        std::filesystem::path path_;
    };

    // Write `json_value` to a unique temp file and return a
    // ScopedJsonFile pointing at it. Returns an invalid wrapper on
    // failure (caller's --<arg>-file-path then resolves to "" → CLI
    // error, which is what we want). Thread-safe per-instance.
    ScopedJsonFile make_json_arg_file(std::string_view json_value) const;

    // Platform/arch helpers — exposed for diagnostics and the
    // download URL builder. Compile-time constants.
    static const char* asset_triple()   noexcept;
    static const char* archive_ext()    noexcept;  // ".zip" / ".tar.gz"
    static const char* binary_name()    noexcept;  // "stellar.exe" / "stellar"
    static std::string release_url();             // full GitHub URL

private:
    std::filesystem::path find_on_path() const;
    bool extract_archive(const std::filesystem::path& archive,
                         const std::filesystem::path& destDir);
    bool verify_binary(const std::filesystem::path& bin);

    std::filesystem::path configPath_;
    std::filesystem::path cacheDir_;        // <configPath>/c2pa/bin
    std::filesystem::path binaryPath_;      // resolved binary (PATH/cache/installed)
    Availability availability_ = Availability::Unknown;
    InstallStage stage_        = InstallStage::Idle;
    float progress_            = 0.0f;
    std::string lastError_;

    // Active install state.
    std::shared_ptr<FileDownloader::DownloadData> pendingDownload_;
    std::filesystem::path pendingArchivePath_;
};

}  // namespace C2PA
