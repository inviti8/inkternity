#include "StellarCli.hpp"

#include <Helpers/Logger.hpp>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>

#include <algorithm>
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

// Spawn `argv`, block until exit, return (exit_code, captured stdout+stderr).
// On spawn failure populates result.spawn_failed.
StellarCli::InvocationResult run_subprocess(const std::vector<std::string>& args) {
    StellarCli::InvocationResult r;
    if (args.empty()) { r.spawn_failed = true; return r; }
    auto argv = make_argv(args);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                            (void*)argv.data());
    // STDIO_APP for stdout so we can SDL_ReadProcess it; merge stderr
    // into stdout so failure diagnostics aren't lost.
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                          SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          SDL_PROCESS_STDIO_APP);
    // Merge stderr into stdout so failure diagnostics aren't lost.
    // Using STDIO_REDIRECT here would require a stderr_source pointer
    // we don't have; the boolean shortcut is the right knob.
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN,
                            true);

    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) {
        r.spawn_failed = true;
        Logger::get().log("INFO",
            "[StellarCli] SDL_CreateProcessWithProperties failed: "
            + std::string(SDL_GetError() ? SDL_GetError() : ""));
        return r;
    }

    // Read everything from stdout. SDL_ReadProcess waits for exit and
    // returns the full buffer; exit_code is filled in.
    size_t outSize = 0;
    void* buf = SDL_ReadProcess(p, &outSize, &r.exit_code);
    if (buf) {
        r.out.assign(static_cast<const char*>(buf), outSize);
        SDL_free(buf);
    }
    SDL_DestroyProcess(p);
    return r;
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
