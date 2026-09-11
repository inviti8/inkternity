#pragma once
// A single process-lifetime StellarCli shared by the two crypto-rails features
// that shell out to the `stellar` binary: C2PA verifiable publishing
// (RegistrationFlow) and AI warm-time billing (AI_BILLING_PHASE4.md §3.4).
//
// Sharing one instance means the PATH probe / auto-install runs once and both
// features see the same resolved binary, whichever the artist enables first.
// Constructed + probed on the first call; `configPath` is honored only then
// (both callers pass the same main.conf.configPath).

#include <filesystem>

namespace C2PA {

class StellarCli;

StellarCli& shared_stellar_cli(const std::filesystem::path& configPath);

}  // namespace C2PA
