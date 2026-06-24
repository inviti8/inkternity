#include "HostOnly.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_filesystem.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "../MainProgram.hpp"
#include "../World.hpp"
#include "../PublishedCanvases.hpp"
#include "../CustomEvents.hpp"
#include "../HostMode.hpp"
#include "../SwitchCWD.hpp"
#include <Helpers/Logger.hpp>
#include <Helpers/Networking/NetLibrary.hpp>
#include <Helpers/StringHelpers.hpp>
#include <unicode/udata.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <process.h>
#else
    #include <cerrno>
    #include <signal.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace HostOnly {

namespace {

// Stderr-only logging for the very early path (before MainProgram +
// Logger handlers exist). Once the file logger is wired up, prefer
// Logger::get().log("...") so output goes both to the per-canvas log
// file and to anything else the Logger fans out to.
void early_log(const std::string& s) {
    std::cerr << "[HostOnly] " << s << std::endl;
}

bool process_alive(int pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r != WAIT_OBJECT_0;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
#endif
}

struct Args {
    std::filesystem::path canvasPath;
    std::optional<int> parentPid;
};

// Argv parser for `--host-only <canvas-path> [--parent-pid <pid>]`.
// Caller verified argv[1] == "--host-only" already, so we start at
// argv[2]. Returns nullopt on malformed input.
std::optional<Args> parse_args(int argc, char** argv) {
    if (argc < 3) {
        early_log("usage: --host-only <canvas-path> [--parent-pid <pid>]");
        return std::nullopt;
    }
    Args a;
    a.canvasPath = std::filesystem::path(argv[2]);
    for (int i = 3; i < argc; i++) {
        const std::string_view flag(argv[i]);
        if (flag == "--parent-pid" && i + 1 < argc) {
            a.parentPid = std::atoi(argv[++i]);
        }
    }
    return a;
}

// Per-canvas log file: <configPath>/logs/host-<canvas-stem>.log.
// Truncates each invocation so we don't accumulate noise across
// restarts. Returns the path so we can mention it in stderr (which
// also gets piped back to the parent process for visibility).
std::filesystem::path setup_log_file(const std::filesystem::path& configPath,
                                     const std::filesystem::path& canvasPath,
                                     std::ofstream& outLogFile) {
    std::error_code ec;
    std::filesystem::create_directories(configPath / "logs", ec);
    std::string stem = canvasPath.stem().string();
    if (stem.empty()) stem = "anonymous";
    auto logPath = configPath / "logs" / ("host-" + stem + ".log");
    outLogFile.open(logPath, std::ios::out | std::ios::trunc);
    return logPath;
}

// Resolve configPath the same way the main process does, so both find
// the same DevKeys / p2p.json. See init_logs() in main.cpp.
std::filesystem::path resolve_config_path() {
#ifdef CONFIG_NEXT_TO_EXECUTABLE
    char* cwd = SDL_GetCurrentDirectory();
    std::filesystem::path p = std::filesystem::path(cwd) / "config";
    SDL_free(cwd);
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
#else
    char* configPathSDL = SDL_GetPrefPath("HEAVYMETA", "Inkternity");
    std::filesystem::path p(configPathSDL ? configPathSDL : "");
    SDL_free(configPathSDL);
    return p;
#endif
}

// Drain stdin on a background thread, set `stopFlag` when STOP is
// received or stdin closes (EOF — implies pipe closed by parent).
// Returns the joinable thread; caller is responsible for joining
// after stopFlag becomes true or after the main loop is otherwise
// shutting down.
//
// Why a thread: std::getline(std::cin, line) blocks until a line is
// available, but the host loop needs to keep ticking NetLibrary +
// the orphan-detect check. Non-blocking stdin reads are platform-
// specific (PeekNamedPipe on Windows, poll() on POSIX); a dedicated
// reader thread sidesteps that.
std::thread spawn_stdin_reader(std::atomic<bool>& stopFlag) {
    return std::thread([&stopFlag]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "STOP") {
                Logger::get().log("INFO", "[HostOnly] stdin: received STOP");
                stopFlag.store(true);
                return;
            }
            Logger::get().log("INFO",
                "[HostOnly] stdin: ignored line: " + line);
        }
        // EOF or error — parent closed its end of the pipe. Treat as
        // STOP so the main loop exits gracefully instead of hanging.
        Logger::get().log("INFO",
            "[HostOnly] stdin: EOF (parent closed pipe)");
        stopFlag.store(true);
    });
}

// Set up just enough of MainProgram to satisfy World's construction
// and NetLibrary's init. Skips: SDL window/GPU init, Skia
// GrDirectContext, the file-select / drawing screens, ICU, fonts UI.
// Keeps: input manager (cheap), FontData (used by some construction
// paths even when not rendering), DevKeys, GlobalConfig.
std::unique_ptr<MainProgram> build_headless_main(
    const std::filesystem::path& configPath,
    std::ofstream* logFile)
{
    auto m = std::make_unique<MainProgram>();
    m->logFile = logFile;
    m->conf.configPath = configPath;
    m->homePath = configPath;  // unused in this path, but field is set
    m->documentsPath = configPath;
    // World ctor reads `main.window.size` to seed the camera viewing
    // area. The actual values don't matter for hosting (no rendering),
    // but the field has to be non-degenerate (positive ints) or the
    // camera's viewing-area init can divide-by-zero downstream. 1x1
    // is the smallest valid value.
    m->window.size = Vector2i(1, 1);
    m->window.sdlWindow = nullptr;
    m->window.canCreateSurfaces = false;
    // Load config so displayName / palettes / etc. are populated.
    // Failures here are non-fatal — defaults are fine for headless
    // hosting.
    try { m->load_config(); }
    catch (const std::exception& e) {
        Logger::get().log("INFO",
            std::string("[HostOnly] load_config failed (using defaults): ") +
            e.what());
    }
    // DevKeys MUST load successfully — without app_secret, SUBSCRIPTION
    // hosting can't derive its stable globalID + localID.
    m->devKeys.ensure_app_keypair(m->conf.configPath);
    m->devKeys.load(m->conf.configPath);
    return m;
}

// Sleep cadence between ticks. Mirrors the ~60 Hz update rate the
// main UI loop drives NetLibrary at. Higher cadences burn CPU on a
// process that has no visible output; lower cadences risk latency on
// signaling traffic.
constexpr int TICK_SLEEP_MS = 16;

// Orphan-detect poll cadence — coarser than tick because we don't
// need millisecond precision on parent-death detection. 5 * 16ms ≈
// 80ms worst-case latency, well under the harness's target of ≤1s.
constexpr int PARENT_POLL_EVERY_N_TICKS = 5;

int run_host(const Args& args) {
    early_log("starting host-only for: " + args.canvasPath.string() +
              (args.parentPid
                ? " (parent-pid=" + std::to_string(*args.parentPid) + ")"
                : " (no parent-pid; orphan-detect disabled)"));

    if (!std::filesystem::exists(args.canvasPath)) {
        early_log("canvas file does not exist: " + args.canvasPath.string());
        return 1;
    }

    // Match the same early-init steps main.cpp does — CWD adjustment
    // so `data/...` resolves to the bundled assets dir, then SDL +
    // ICU + CustomEvents. The headless path doesn't render, but
    // FontData and parts of the canvas-load path still touch these.
    switch_cwd();

    // SDL with events subsystem (needed by SDL_RegisterEvents in
    // CustomEvents::init). We deliberately do NOT request VIDEO/AUDIO
    // — the side-instance has no window, no audio.
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        early_log(std::string("SDL_Init(SDL_INIT_EVENTS) failed: ") +
                  SDL_GetError());
        return 1;
    }

    // ICU common data — same load main.cpp does. The data buffer must
    // outlive every UCD lookup, so keep it in a function-static.
    static std::string icudt;
    {
        UErrorCode uerr = U_ZERO_ERROR;
        try {
            icudt = read_file_to_string("data/icudt77l-small.dat");
            udata_setCommonData(static_cast<void*>(icudt.data()), &uerr);
            if (U_FAILURE(uerr)) {
                early_log(std::string("udata_setCommonData failed: ") +
                          u_errorName(uerr));
            }
        } catch (const std::exception& e) {
            early_log(std::string("could not load icudt77l-small.dat: ") +
                      e.what() + " (continuing without ICU)");
        }
    }

    CustomEvents::init();

    auto configPath = resolve_config_path();
    if (configPath.empty()) {
        early_log("could not resolve config path");
        SDL_Quit();
        return 1;
    }

    std::ofstream logFileStream;
    auto logPath = setup_log_file(configPath, args.canvasPath, logFileStream);
    if (!logFileStream.is_open()) {
        early_log("could not open log file: " + logPath.string());
        // Continue — the file logger will be a no-op but stderr still
        // works.
    }
    early_log("log file: " + logPath.string());

    // Wire a minimal Logger that writes to our per-canvas log file.
    // The MainProgram ctor will register WORLDFATAL/USERINFO/CHAT
    // handlers that target *logFile too — we set logFile to a
    // local ofstream so those handlers find a live stream.
    Logger::get().add_log("INFO", [&logFileStream](const std::string& text) {
        if (logFileStream.is_open())
            logFileStream << "[INFO] " << text << std::endl;
        std::cerr << "[HostOnly][INFO] " << text << std::endl;
    });
    Logger::get().add_log("FATAL", [&logFileStream](const std::string& text) {
        if (logFileStream.is_open())
            logFileStream << "[FATAL] " << text << std::endl;
        std::cerr << "[HostOnly][FATAL] " << text << std::endl;
    });

    std::unique_ptr<MainProgram> m;
    try {
        m = build_headless_main(configPath, &logFileStream);
    } catch (const std::exception& e) {
        Logger::get().log("FATAL",
            std::string("[HostOnly] MainProgram setup failed: ") + e.what());
        SDL_Quit();
        return 1;
    }

    // Lock acquisition is the cheap fast-fail gate: another process
    // (typically a stale lock we'll silently reclaim, or a sibling
    // side-instance that won the race) → exit 2 before we spend
    // cycles loading the canvas.
    if (!PublishedCanvases::try_acquire_lock(args.canvasPath)) {
        Logger::get().log("FATAL",
            "[HostOnly] could not acquire lock for " +
            args.canvasPath.string() +
            " — another process holds it");
        SDL_Quit();
        return 2;
    }
    Logger::get().log("INFO",
        "[HostOnly] acquired lock for " + args.canvasPath.string());

    std::shared_ptr<World> world;
    try {
        CustomEvents::OpenInfiniPaintFileEvent openFile{};
        openFile.isClient = false;
        openFile.filePathSource = args.canvasPath;
        world = std::make_shared<World>(*m, openFile);
        m->worlds.emplace_back(world);
        m->world = world;
    } catch (const std::exception& e) {
        Logger::get().log("FATAL",
            std::string("[HostOnly] World construction failed: ") + e.what());
        PublishedCanvases::release_lock(args.canvasPath);
        SDL_Quit();
        return 1;
    }

    try {
        world->start_hosting(HostMode::SUBSCRIPTION, "", "");
    } catch (const std::exception& e) {
        Logger::get().log("FATAL",
            std::string("[HostOnly] start_hosting failed: ") + e.what());
        m->worlds.clear();
        m->world.reset();
        world.reset();
        PublishedCanvases::release_lock(args.canvasPath);
        SDL_Quit();
        return 1;
    }
    Logger::get().log("INFO",
        "[HostOnly] hosting started, netSource=" + world->netSource);

    // Print READY to stdout so the parent process can synchronize
    // ("started hosting, can now route subscribers here"). Mirrors
    // the protocol the test harness uses for stdin-loop children.
    std::cout << "READY " << world->netSource << std::endl;
    std::cout.flush();

    std::atomic<bool> stopRequested{false};
    std::thread stdinReader = spawn_stdin_reader(stopRequested);

    int tick = 0;
    int exit_code = 0;
    while (!stopRequested.load()) {
        NetLibrary::update();
        world->update();
        tick++;
        if (args.parentPid &&
            (tick % PARENT_POLL_EVERY_N_TICKS == 0) &&
            !process_alive(*args.parentPid))
        {
            Logger::get().log("INFO",
                "[HostOnly] parent process " +
                std::to_string(*args.parentPid) +
                " is no longer alive — initiating orphan shutdown");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(TICK_SLEEP_MS));
    }

    Logger::get().log("INFO", "[HostOnly] shutting down");
    std::cout << "STOPPING" << std::endl;
    std::cout.flush();

    // World holds a NetServer registered with NetLibrary. Dropping
    // the shared_ptr triggers the dtor chain → NetServer is unregistered
    // → libdatachannel closes peer connections and the signaling WSS.
    m->worlds.clear();
    m->world.reset();
    world.reset();

    PublishedCanvases::release_lock(args.canvasPath);
    Logger::get().log("INFO", "[HostOnly] released lock");

    // The stdin thread may still be blocked in getline. If stdin is a
    // pipe and the parent has closed it, getline returns false and
    // the thread exits cleanly. If the parent has NOT closed it
    // (orphan-detect path: parent killed -9), getline blocks
    // indefinitely. Detach so we don't deadlock on join — the OS
    // reaps the thread when the process exits.
    if (stdinReader.joinable())
        stdinReader.detach();

    // Tear down MainProgram explicitly so its destructors run with
    // Logger still wired. NetLibrary::destroy() is invoked transitively
    // by MainProgram's dtor chain via the existing shutdown path.
    m.reset();

    SDL_Quit();
    return exit_code;
}

// RECOVERY: headlessly load a canvas with World::recoveryLoad enabled
// (zero-pads the decompressed buffer so a tail over-read returns zeros
// instead of throwing, and logs per-subsystem stream positions), then
// write a clean copy to outPath. Mirrors run_host's minimal headless
// init but skips the publish lock and the hosting loop. The per-subsystem
// position log tells us whether the parse stayed aligned to the very end
// (recovery trustworthy) or desynced early (a real serialization bug).
int run_recover(const std::filesystem::path& inPath,
                const std::filesystem::path& outPath) {
    early_log("recover: in=" + inPath.string() + " out=" + outPath.string());
    if (!std::filesystem::exists(inPath)) {
        early_log("input canvas does not exist: " + inPath.string());
        return 1;
    }

    switch_cwd();
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        early_log(std::string("SDL_Init(SDL_INIT_EVENTS) failed: ") + SDL_GetError());
        return 1;
    }

    static std::string icudt;
    {
        UErrorCode uerr = U_ZERO_ERROR;
        try {
            icudt = read_file_to_string("data/icudt77l-small.dat");
            udata_setCommonData(static_cast<void*>(icudt.data()), &uerr);
            if (U_FAILURE(uerr))
                early_log(std::string("udata_setCommonData failed: ") + u_errorName(uerr));
        } catch (const std::exception& e) {
            early_log(std::string("could not load icudt77l-small.dat: ") +
                      e.what() + " (continuing without ICU)");
        }
    }

    CustomEvents::init();

    auto configPath = resolve_config_path();
    if (configPath.empty()) {
        early_log("could not resolve config path");
        SDL_Quit();
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(configPath / "logs", ec);
    std::ofstream logFileStream(configPath / "logs" / "recover.log",
                                std::ios::out | std::ios::trunc);
    auto fanout = [&logFileStream](const char* tag, const std::string& text) {
        if (logFileStream.is_open())
            logFileStream << tag << " " << text << std::endl;
        std::cerr << "[recover]" << tag << " " << text << std::endl;
    };
    Logger::get().add_log("INFO",       [&](const std::string& t){ fanout("[INFO]", t); });
    Logger::get().add_log("USERINFO",   [&](const std::string& t){ fanout("[USERINFO]", t); });
    Logger::get().add_log("WORLDFATAL", [&](const std::string& t){ fanout("[FATAL]", t); });
    Logger::get().add_log("FATAL",      [&](const std::string& t){ fanout("[FATAL]", t); });

    std::unique_ptr<MainProgram> m;
    try {
        m = build_headless_main(configPath, &logFileStream);
    } catch (const std::exception& e) {
        Logger::get().log("FATAL", std::string("MainProgram setup failed: ") + e.what());
        SDL_Quit();
        return 1;
    }

    World::recoveryLoad = true;
    std::shared_ptr<World> world;
    try {
        CustomEvents::OpenInfiniPaintFileEvent openFile{};
        openFile.isClient = false;
        openFile.filePathSource = inPath;
        world = std::make_shared<World>(*m, openFile);
        m->worlds.emplace_back(world);
        m->world = world;
    } catch (const std::exception& e) {
        Logger::get().log("FATAL", std::string("recover load FAILED (over-read exceeds pad): ") + e.what());
        World::recoveryLoad = false;
        SDL_Quit();
        return 1;
    }
    World::recoveryLoad = false;

    try {
        world->save_recovery_copy(outPath);
        Logger::get().log("USERINFO", "recovered canvas written to " + outPath.string());
    } catch (const std::exception& e) {
        Logger::get().log("FATAL", std::string("recover save failed: ") + e.what());
        m->worlds.clear();
        m->world.reset();
        world.reset();
        SDL_Quit();
        return 1;
    }

    m->worlds.clear();
    m->world.reset();
    world.reset();
    m.reset();
    SDL_Quit();
    return 0;
}

}  // anonymous namespace

std::optional<int> dispatch(int argc, char** argv) {
    if (argc < 2) return std::nullopt;
    const std::string_view cmd(argv[1]);

    if (cmd == "--host-only") {
        auto args = parse_args(argc, argv);
        if (!args) return 1;
        return run_host(*args);
    }

    if (cmd == "--recover-canvas") {
        if (argc < 4) {
            early_log("usage: --recover-canvas <in-canvas> <out-canvas>");
            return 1;
        }
        return run_recover(std::filesystem::path(argv[2]),
                           std::filesystem::path(argv[3]));
    }

    return std::nullopt;
}

}  // namespace HostOnly
