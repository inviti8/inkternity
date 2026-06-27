# Vendored: cgltf (single-header glTF 2.0 loader)

Upstream: https://github.com/jkuhlmann/cgltf
License:  MIT (see `LICENSE`, © 2018–2021 Johannes Kuhlmann) — registered at
          `assets/data/third_party_licenses/cgltf`.

## Pinned revision

    tag    v1.15
    commit bbeb5b0b070ddacddac6852fb72143eb68454937
    file   cgltf.h (single header, 202865 bytes, "Version: 1.15")

Pinned to the **v1.15 release tag** (the version the PHASE9 research spike vetted,
docs/design/PHASE9.md) rather than `master` HEAD, so the loader behaviour is fixed
and reproducible.

`cgltf.h` is the exact upstream blob at that tag (downloaded from
`raw.githubusercontent.com/jkuhlmann/cgltf/v1.15/cgltf.h`).

## How it's built

Header-only. `CMakeLists.txt` adds `deps/cgltf` to the `main` target's include
dirs; **exactly one** translation unit defines `CGLTF_IMPLEMENTATION` before
including `<cgltf.h>` (see `src/Armature/ArmatureModel.cpp`). No library target,
no submodule.

## Why cgltf (not tinygltf / assimp / ufbx)

Smallest footprint, zero deps, C99, exposes `cgltf_skin` (joints, skeleton,
`inverse_bind_matrices`) and `JOINTS_0`/`WEIGHTS_0` accessors directly — which is
exactly what the hand-rolled FK + linear-blend skinning needs (PHASE9 Decision §2,
no ozz). Our default asset is plain glTF/`.glb` with embedded buffers and **no
Draco**, so cgltf loads it as-is with no extra decoders.
