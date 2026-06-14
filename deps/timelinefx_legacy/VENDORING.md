# Vendored: classic (legacy) TimelineFX C++ runtime

Render-agnostic C++ implementation of the **legacy** TimelineFX particle format
(`.eff` / `data.xml` from the original, stable TimelineFX editor). Used by the
PHASE5 legacy-path spike (`tools/tfx_legacy_spike.cpp`) to render legacy effect
libraries through Skia. See `docs/design/PHASE5.md` and the project memory
`project_phase5_tfx_reassessment.md`.

## Source

- `source/` — `peterigz/timelinefx`, `master` @ `cde7ac0647ba83ea76e877847fb2594517308504`
  (downloaded 2026-06-14). The render-agnostic core: `TLFX*` classes with
  `AnimImage`, `XMLLoader`, and `ParticleManager::DrawSprite` as the extension
  points. No Marmalade dependency in the core (the Marmalade bits are only in the
  upstream `timelinefx-sample`, not vendored).
- `pugixml/` — pugixml (MIT), the XML parser the default `PugiXMLLoader` uses.

## ⚠️ LICENSE — must be cleared before shipping

- **pugixml**: MIT (clear to ship).
- **`peterigz/timelinefx` core**: ships with **no LICENSE file**; the README only
  states "Copyright: Peter J. Rigby 2009-2010". The `damucz/timelinefx` fork of
  the *same* render-agnostic code carries an explicit **MIT** LICENSE, but the
  authority of that grant over the original author's code is unconfirmed.
- Inkternity is distributed under BUSL-1.1, so we **cannot ship this runtime**
  until we have an explicit license grant from Peter Rigby (RigzSoft). This is
  part of the planned partnership conversation (alongside subscriber offer-codes
  for the paid editor). Until then this target is `EXCLUDE_FROM_ALL` — spike /
  evaluation only, never linked into `main`.

## Build

Isolated C++17 static lib `timelinefx_legacy` (CMakeLists.txt). Needs
`_USE_MATH_DEFINES` for MSVC (`M_PI`). Render-agnostic: the consumer implements
`AnimImage::Load`, `EffectsLibrary::CreateLoader`/`CreateImage`, and
`ParticleManager::DrawSprite`.

## Update recipe

Re-download `source/TLFX*.{h,cpp}` from the pinned SHA and `pugixml/*` from the
repo's `include/` + `src/`. Keep this pin in sync with whatever the licensing
agreement settles on.
