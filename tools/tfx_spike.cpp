// M0 runtime spike for PHASE5 particle systems (docs/design/PHASE5.md).
//
// Sole purpose: prove the vendored TimelineFX runtime (deps/timelinefx,
// pinned to commit a96a36fb6f5e9d9a51ba4c28321cb86360b4c317) compiles and
// links in our MSVC / C++23 toolchain, completely decoupled from the main
// render path. It initialises and shuts the library down — no effect file,
// no rendering. The drawAtlas integration lands later in M0/M1 inside the
// real ParticleCanvasComponent.
//
// Build explicitly (it is EXCLUDE_FROM_ALL):
//   cmake --build build --config Release --target tfx_spike
#include <cstdio>
#include "timelinefx.h"

int main() {
    std::printf("[tfx_spike] TimelineFX vendored runtime, version=%s\n", TFX_VERSION);

    // A modest host memory pool; single worker thread keeps the spike
    // deterministic and avoids depending on the host's core count.
    tfx_InitialiseTimelineFX(1, 64ull * 1024 * 1024);
    std::printf("[tfx_spike] tfx_InitialiseTimelineFX OK\n");

    tfx_EndTimelineFX();
    std::printf("[tfx_spike] tfx_EndTimelineFX OK\n");
    return 0;
}
