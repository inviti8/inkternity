#include "SharedStellarCli.hpp"

#include "StellarCli.hpp"

#include <memory>

namespace C2PA {

StellarCli& shared_stellar_cli(const std::filesystem::path& configPath) {
    // Magic-static init is thread-safe; probe() runs exactly once even if the
    // GUI thread (RegistrationFlow) and the warm-lease thread (billing) race to
    // first use. Subsequent calls ignore configPath and return the same object.
    static std::unique_ptr<StellarCli> instance = [&] {
        auto cli = std::make_unique<StellarCli>(configPath);
        cli->probe();
        return cli;
    }();
    return *instance;
}

}  // namespace C2PA
