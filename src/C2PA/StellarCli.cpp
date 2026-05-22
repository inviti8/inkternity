#include "StellarCli.hpp"

#include <Helpers/Logger.hpp>
#include <Helpers/Random.hpp>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

namespace C2PA {

// ---- Compile-time platform/arch detection ---------------------------------
// Maps the build target to the Rust target triple stellar-cli's GitHub
// release assets use. Compile-error on unknown combinations so we never
// silently fall through to a wrong URL.

const char* StellarCli::asset_triple() noexcept {
#if defined(_WIN32)
  #if defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64-pc-windows-msvc";
  #elif defined(_M_X64) || defined(__x86_64__)
    return "x86_64-pc-windows-msvc";
  #else
    return "x86_64-pc-windows-msvc";  // best-effort default for 32-bit dev hosts
  #endif
#elif defined(__APPLE__)
  #if defined(__aarch64__) || defined(__arm64__)
    return "aarch64-apple-darwin";
  #else
    return "x86_64-apple-darwin";
  #endif
#elif defined(__linux__)
  #if defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
  #else
    return "x86_64-unknown-linux-gnu";
  #endif
#else
  #error "Unsupported platform for stellar-cli auto-install — extend C2PA::StellarCli::asset_triple()"
#endif
}

const char* StellarCli::archive_ext() noexcept {
#if defined(_WIN32)
    return ".zip";
#else
    return ".tar.gz";
#endif
}

const char* StellarCli::binary_name() noexcept {
#if defined(_WIN32)
    return "stellar.exe";
#else
    return "stellar";
#endif
}

std::string StellarCli::release_url() {
    std::string url = "https://github.com/stellar/stellar-cli/releases/download/v";
    url += PINNED_VERSION;
    url += "/stellar-cli-";
    url += PINNED_VERSION;
    url += "-";
    url += asset_triple();
    url += archive_ext();
    return url;
}

// ---- ctor + probe ---------------------------------------------------------

StellarCli::StellarCli(std::filesystem::path configPath)
    : configPath_(std::move(configPath)),
      cacheDir_(configPath_ / "c2pa" / "bin") {}

StellarCli::Availability StellarCli::probe() {
    // 1. PATH first — if the user has a system install, prefer it.
    auto fromPath = find_on_path();
    if (!fromPath.empty()) {
        binaryPath_  = fromPath;
        availability_ = Availability::SystemPath;
        return availability_;
    }
    // 2. Cache check.
    auto cached = cacheDir_ / binary_name();
    std::error_code ec;
    if (std::filesystem::exists(cached, ec)) {
        binaryPath_  = cached;
        availability_ = Availability::Cached;
        return availability_;
    }
    binaryPath_.clear();
    availability_ = Availability::NotAvailable;
    return availability_;
}

// ---- Subprocess primitives ------------------------------------------------

namespace {

// Build a NULL-terminated argv pointer array from owned strings.
// Caller's vector keeps the underlying char buffers alive.
std::vector<const char*> make_argv(const std::vector<std::string>& args) {
    std::vector<const char*> out;
    out.reserve(args.size() + 1);
    for (const auto& a : args) out.push_back(a.c_str());
    out.push_back(nullptr);
    return out;
}

#if defined(_WIN32)
// Quote a single argv element using the Windows command-line rules
// (CommandLineToArgvW reverse). Wraps in " if the arg contains
// whitespace or other shell-affecting characters; escapes embedded "
// as \" and runs of \\ before a " as 2N+1 backslashes. Empty arg
// becomes "".
std::string win_quote_arg(std::string_view a) {
    bool needsQuoting = a.empty();
    for (char c : a) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '"') {
            needsQuoting = true;
            break;
        }
    }
    if (!needsQuoting) return std::string(a);

    std::string out;
    out.reserve(a.size() + 4);
    out.push_back('"');
    size_t i = 0;
    while (i < a.size()) {
        size_t bs = 0;
        while (i < a.size() && a[i] == '\\') { ++bs; ++i; }
        if (i == a.size()) {
            // Trailing backslashes are doubled because the closing "
            // would otherwise consume them.
            out.append(bs * 2, '\\');
            break;
        }
        if (a[i] == '"') {
            // Backslashes before a literal " are doubled, then the "
            // is escaped with one extra \.
            out.append(bs * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(bs, '\\');
            out.push_back(a[i]);
        }
        ++i;
    }
    out.push_back('"');
    return out;
}
#endif

// Sensitive arg names whose values are elided from the logged
// argv trace. The value of `--source-account` is also redacted iff
// it looks like an S-strkey (secret) rather than a G-strkey (public)
// — that's caller-context-dependent so we sniff at log time.
const std::initializer_list<std::string_view> kAlwaysRedactedArgs = {
    "--auth_payload",
    "--auth_signature",
    "--sign-with-key",
    "--secret-key",
};

bool is_secret_strkey(std::string_view s) {
    return s.size() == 56 && s.front() == 'S';
}

// Build a one-line redacted argv string for logging. Long hex blobs
// are summarised by length; secret strkeys are replaced with
// "<secret:S...>"; everything else is included verbatim. Newlines
// are stripped so the log line stays single-line.
std::string redact_argv_for_log(const std::vector<std::string>& args) {
    std::string out;
    out.reserve(256);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(' ');
        const std::string& a = args[i];

        // If the previous token names a sensitive arg, redact this
        // token. Also catch `--foo-file-path` companions — the file
        // path itself is non-secret but the content is, so just note
        // the temp file pointer.
        if (i > 0) {
            const std::string& prev = args[i - 1];
            for (auto name : kAlwaysRedactedArgs) {
                if (prev == name) {
                    out.append("<redacted:");
                    out.append(std::to_string(a.size()));
                    out.append(" bytes>");
                    goto next;
                }
            }
        }
        // Auth via secret-key on --source-account.
        if (i > 0 && args[i - 1] == "--source-account" && is_secret_strkey(a)) {
            out.append("<secret-source:S...>");
            goto next;
        }
        // Plain arg — strip newlines defensively.
        for (char c : a) {
            if (c == '\n' || c == '\r') out.push_back(' ');
            else out.push_back(c);
        }
      next:;
    }
    return out;
}

// First N chars of `s` with newlines flattened, for one-line log
// previews of CLI output.
std::string first_chars_flat(const std::string& s, size_t n) {
    std::string out;
    out.reserve(std::min(n, s.size()));
    for (size_t i = 0; i < s.size() && out.size() < n; ++i) {
        char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else out.push_back(c);
    }
    if (s.size() > n) out += " ...";
    return out;
}

// Spawn `argv`, block until exit, return (exit_code, captured
// stdout+stderr).
//
// Windows: bypass SDL_CreateProcess entirely and use _popen from
// MSVCRT. SDL's process layer trips STATUS_STACK_BUFFER_OVERRUN on
// network-bound subprocesses with long argv (root cause unclear after
// trying both the argv property and the cmdline-string property +
// our own Windows-rule quoting). _popen is well-tested for command
// pipes and the C runtime owns the cmd-line shell-quoting. Stderr is
// merged into stdout via the trailing "2>&1".
//
// POSIX: SDL_CreateProcess works fine; keep the original path.
StellarCli::InvocationResult run_subprocess(const std::vector<std::string>& args) {
    StellarCli::InvocationResult r;
    if (args.empty()) { r.spawn_failed = true; return r; }

    // Pre-log: every subprocess goes through here, so any
    // platform-specific weirdness (Linux/macOS smoke test surprises,
    // path-resolution quirks, missing binaries) shows up in the same
    // single-line trace format.
    const auto t_start = std::chrono::steady_clock::now();
    Logger::get().log("INFO",
        "[C2PA::CLI] invoke: " + redact_argv_for_log(args));

#if defined(_WIN32)
    std::string cmdline;
    cmdline.reserve(256);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmdline.push_back(' ');
        cmdline += win_quote_arg(args[i]);
    }
    cmdline += " 2>&1";

    FILE* pipe = _popen(cmdline.c_str(), "rb");
    if (!pipe) {
        r.spawn_failed = true;
        Logger::get().log("INFO",
            "[C2PA::CLI] _popen spawn failed: " + cmdline);
        return r;
    }
    char buf[4096];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), pipe);
        if (n == 0) break;
        r.out.append(buf, n);
    }
    int rc = _pclose(pipe);
    // _pclose returns the subprocess exit code on success, -1 on
    // wait/spawn failure. We mirror the SDL contract: exit_code = -1
    // when something went sideways at the C-runtime layer.
    r.exit_code = rc;
    const auto t_end = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();
    Logger::get().log("INFO",
        "[C2PA::CLI] result: exit=" + std::to_string(r.exit_code)
        + " duration=" + std::to_string(ms) + "ms"
        + " out_len=" + std::to_string(r.out.size())
        + " preview=" + first_chars_flat(r.out, 240));
    return r;
#else
    auto argv = make_argv(args);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                            (void*)argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                          SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN,
                            true);
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) {
        r.spawn_failed = true;
        Logger::get().log("INFO",
            "[C2PA::CLI] SDL_CreateProcessWithProperties spawn failed: "
            + std::string(SDL_GetError() ? SDL_GetError() : ""));
        return r;
    }
    size_t outSize = 0;
    void* buf = SDL_ReadProcess(p, &outSize, &r.exit_code);
    if (buf) {
        r.out.assign(static_cast<const char*>(buf), outSize);
        SDL_free(buf);
    }
    SDL_DestroyProcess(p);
    const auto t_end = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();
    Logger::get().log("INFO",
        "[C2PA::CLI] result: exit=" + std::to_string(r.exit_code)
        + " duration=" + std::to_string(ms) + "ms"
        + " out_len=" + std::to_string(r.out.size())
        + " preview=" + first_chars_flat(r.out, 240));
    return r;
#endif
}

}  // namespace

StellarCli::InvocationResult StellarCli::invoke(
        const std::vector<std::string>& args) const {
    if (binaryPath_.empty()) {
        InvocationResult r;
        r.spawn_failed = true;
        return r;
    }
    std::vector<std::string> full;
    full.reserve(args.size() + 1);
    full.push_back(binaryPath_.string());
    for (const auto& a : args) full.push_back(a);
    return run_subprocess(full);
}

std::filesystem::path StellarCli::find_on_path() const {
    // Try `<probe-cmd> stellar` and read the first path it prints.
    // `where` on Windows, `which` on POSIX.
#if defined(_WIN32)
    auto r = run_subprocess({"where", "stellar"});
#else
    auto r = run_subprocess({"which", "stellar"});
#endif
    if (!r.ok()) return {};

    // Take the first non-empty line of stdout.
    size_t end = r.out.find_first_of("\r\n");
    std::string firstLine = (end == std::string::npos)
        ? r.out : r.out.substr(0, end);
    while (!firstLine.empty() &&
           (firstLine.back() == ' ' || firstLine.back() == '\t')) {
        firstLine.pop_back();
    }
    if (firstLine.empty()) return {};

    std::filesystem::path candidate = firstLine;
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec)) return {};
    return candidate;
}

// ---- Install state machine ------------------------------------------------

bool StellarCli::start_install() {
    if (availability_ == Availability::SystemPath ||
        availability_ == Availability::Cached    ||
        availability_ == Availability::InstallInProgress) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(cacheDir_, ec);
    if (ec) {
        lastError_ = "Could not create install directory: " + ec.message();
        availability_ = Availability::InstallFailed;
        stage_ = InstallStage::Failed;
        return false;
    }

    pendingArchivePath_ = cacheDir_ /
        (std::string("stellar-cli-") + PINNED_VERSION + archive_ext());
    const std::string url = release_url();
    Logger::get().log("INFO",
        "[StellarCli] downloading " + url + " -> "
        + pendingArchivePath_.string());

    pendingDownload_ = FileDownloader::download_data_from_url(url);
    if (!pendingDownload_) {
        lastError_ = "FileDownloader could not start the download.";
        availability_ = Availability::InstallFailed;
        stage_ = InstallStage::Failed;
        return false;
    }

    availability_ = Availability::InstallInProgress;
    stage_        = InstallStage::Downloading;
    progress_     = 0.0f;
    lastError_.clear();
    return true;
}

void StellarCli::poll_install() {
    if (stage_ == InstallStage::Idle ||
        stage_ == InstallStage::Done ||
        stage_ == InstallStage::Failed) {
        return;
    }

    if (stage_ == InstallStage::Downloading) {
        if (!pendingDownload_) {
            lastError_ = "Internal: download handle vanished.";
            availability_ = Availability::InstallFailed;
            stage_ = InstallStage::Failed;
            return;
        }
        progress_ = pendingDownload_->progress.load() * 0.7f;  // 0..0.7 = download

        const auto status = pendingDownload_->status.load();
        if (status == FileDownloader::DownloadData::Status::SUCCESS) {
            // Persist the body to the archive path.
            std::ofstream f(pendingArchivePath_, std::ios::binary | std::ios::trunc);
            if (!f) {
                lastError_ = "Could not write " + pendingArchivePath_.string();
                availability_ = Availability::InstallFailed;
                stage_ = InstallStage::Failed;
                pendingDownload_.reset();
                return;
            }
            f.write(pendingDownload_->str.data(),
                    static_cast<std::streamsize>(pendingDownload_->str.size()));
            f.close();
            pendingDownload_.reset();
            stage_    = InstallStage::Extracting;
            progress_ = 0.7f;
        } else if (status == FileDownloader::DownloadData::Status::FAILURE) {
            lastError_ = "Download failed (network).";
            availability_ = Availability::InstallFailed;
            stage_ = InstallStage::Failed;
            pendingDownload_.reset();
        }
        return;
    }

    if (stage_ == InstallStage::Extracting) {
        // Extraction is synchronous — runs on the UI thread for now.
        // The archive is small (~10-30 MB) and tar/Expand-Archive are
        // fast; future-us can move this to a worker thread if a slow
        // disk makes it visible. The progress bar jumps 0.7 → 0.9.
        if (!extract_archive(pendingArchivePath_, cacheDir_)) {
            lastError_ = "Could not extract " + pendingArchivePath_.string();
            availability_ = Availability::InstallFailed;
            stage_ = InstallStage::Failed;
            return;
        }
        progress_ = 0.9f;
        stage_    = InstallStage::Verifying;
        return;
    }

    if (stage_ == InstallStage::Verifying) {
        const auto bin = cacheDir_ / binary_name();
        if (!verify_binary(bin)) {
            lastError_ = "Installed binary failed --version check.";
            availability_ = Availability::InstallFailed;
            stage_ = InstallStage::Failed;
            return;
        }
        binaryPath_   = bin;
        availability_ = Availability::Cached;
        stage_        = InstallStage::Done;
        progress_     = 1.0f;
        Logger::get().log("USERINFO",
            "Provenance submitter installed at " + bin.string());
        return;
    }
}

// ---- Extraction -----------------------------------------------------------

bool StellarCli::extract_archive(const std::filesystem::path& archive,
                                  const std::filesystem::path& destDir) {
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) return false;

#if defined(_WIN32)
    // Windows: use bsdtar from System32 (ships since Win10 1803).
    auto r = run_subprocess({"tar.exe", "-xf",
                              archive.string(), "-C", destDir.string()});
#else
    auto r = run_subprocess({"tar", "-xzf",
                              archive.string(), "-C", destDir.string()});
#endif
    if (!r.ok()) {
        Logger::get().log("INFO",
            "[StellarCli] extract failed (exit=" + std::to_string(r.exit_code)
            + "): " + r.out);
        return false;
    }

    // GitHub release archives unpack to either:
    //   destDir/stellar[.exe]                       (flat archive)
    //   destDir/<subdir>/stellar[.exe]              (versioned subdir)
    // Find the binary and move it to destDir/<binary_name>.
    auto target = destDir / binary_name();
    if (std::filesystem::exists(target, ec)) {
        // Flat layout — already where we want it.
    } else {
        // Recursively search for the binary, move it up.
        bool found = false;
        for (auto it = std::filesystem::recursive_directory_iterator(destDir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator();
             ++it) {
            if (it->path().filename() == binary_name()) {
                std::filesystem::rename(it->path(), target, ec);
                if (ec) {
                    // Cross-device or in-use — copy+remove fallback.
                    std::filesystem::copy_file(it->path(), target,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) return false;
                    std::filesystem::remove(it->path(), ec);
                }
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

#if !defined(_WIN32)
    // Set executable bit on POSIX.
    std::filesystem::permissions(target,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
        std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, ec);
#endif
    return true;
}

// ---- ScopedJsonFile + make_json_arg_file -----------------------------------

StellarCli::ScopedJsonFile& StellarCli::ScopedJsonFile::operator=(
        StellarCli::ScopedJsonFile&& o) noexcept {
    if (this != &o) {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
        path_ = std::move(o.path_);
        o.path_.clear();
    }
    return *this;
}

StellarCli::ScopedJsonFile::~ScopedJsonFile() noexcept {
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
}

StellarCli::ScopedJsonFile StellarCli::make_json_arg_file(
        std::string_view json_value) const {
    std::filesystem::path tmpDir = cacheDir_ / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    if (ec) return ScopedJsonFile{};

    // Use random alphanumeric for uniqueness across concurrent calls in
    // the same process and across processes sharing the configPath.
    std::filesystem::path tmpPath = tmpDir /
        ("arg_" + Random::get().alphanumeric_str(12) + ".json");
    std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
    if (!f) return ScopedJsonFile{};
    f.write(json_value.data(), static_cast<std::streamsize>(json_value.size()));
    f.close();
    if (!std::filesystem::exists(tmpPath, ec)) return ScopedJsonFile{};
    return ScopedJsonFile{tmpPath};
}

bool StellarCli::verify_binary(const std::filesystem::path& bin) {
    std::error_code ec;
    if (!std::filesystem::exists(bin, ec)) return false;
    auto r = run_subprocess({bin.string(), "--version"});
    if (!r.ok()) {
        Logger::get().log("INFO",
            "[StellarCli] verify failed (exit=" + std::to_string(r.exit_code)
            + "): " + r.out);
        return false;
    }
    // stdout of a valid stellar CLI starts with "stellar " — quick sanity.
    return r.out.find("stellar") != std::string::npos;
}

}  // namespace C2PA
