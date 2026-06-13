# Vendored: TimelineFX C++ library

Upstream: https://github.com/peterigz/timelinefxlib
License:  MIT (see `License.txt`, © 2021 Peter Rigby) — registered at
          `assets/data/third_party_licenses/TimelineFX`.

## Pinned revision

    commit a96a36fb6f5e9d9a51ba4c28321cb86360b4c317
    date   2024-12-13
    msg    "Make sure flags are set to 0"

Vendored files are an exact copy of the upstream blobs at that commit
(verified: `git hash-object timelinefx.h` ==
`9edc5f24d9db3e6e9bdb2b0d84ec8533f95a9f9f`, the GitHub blob SHA at the pin).

Only `timelinefx.h` + `timelinefx.cpp` are needed — the library is
self-contained (its own pocket allocator, SIMD intrinsics, and
multithreading; no external dependencies). The upstream `Shaders/` folder
is a GPU-compute pre-bake path we do not use (we render on the CPU via
Skia `drawAtlas`), so it is intentionally not vendored.

## Why pinned

The library is upstream-labelled **alpha / work-in-progress** (`TFX_VERSION`
== "Alpha"); the author explicitly expects API churn. We pin a single
commit so our build is reproducible and an upstream interface change can
never silently break a checkout. To update: bump the commit above,
re-download the two files at that SHA, re-verify the blob hash, and
re-run the `tfx_spike` target (and, once it exists, the particle render
path) before committing.

## Baked-in upstream defines (note before integrating)

`timelinefx.h` hard-`#define`s these at the top (not `#ifndef`-guarded):
`tfxENABLE_PROFILING`, `TFX_THREAD_SAFE`, `TFX_EXTRA_DEBUGGING`. The last
two carry runtime overhead; revisit whether to patch them off for the
shipping `main` integration (M1) vs. this spike.

## How it is built (C++17 isolation — important)

The vendored `.cpp` is compiled as its own **C++17 static library**
(`timelinefx`, EXCLUDE_FROM_ALL) in the top-level `CMakeLists.txt`, NOT at
the project-wide C++23. Reason: at C++23 MSVC rejects the upstream source
with the C++20 *rewritten-comparison-operator ambiguity* —
`tfx_attribute_node_s::operator==` gains a synthesized reversed `y == x`
candidate that collides with the library's own `operator==` (error C2666,
seen while instantiating `tfx_bucket_array_t<tfx_attribute_node_t>::find`).
Building the TU at C++17 removes the reversed-candidate rule and the error.

This keeps the pinned source **pristine — no upstream fork**, with the
language-standard workaround confined to the build system. Consumers
(`main` at M1, `tfx_spike` today) include `timelinefx.h` only for the
`extern "C"` `tfxAPI` functions, which do not instantiate the offending
internal templates, so they remain at C++23.

`tools/tfx_spike.cpp` links the `timelinefx` lib to form the `tfx_spike`
target — a compile+link+run proof (`tfx_InitialiseTimelineFX` /
`tfx_EndTimelineFX`, verified exit 0). Integration into the `main` target
happens at PHASE5 M1 when the `ParticleCanvasComponent` render path is
written; until then the library is built only when `tfx_spike` is, and is
deliberately NOT compiled into the shipping binary.
