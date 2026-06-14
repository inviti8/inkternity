# Vendored: classic (legacy) TimelineFX C++ runtime — MIT

Render-agnostic C++ implementation of the **legacy** TimelineFX particle format
(`.eff` / `data.xml` from the original, stable TimelineFX editor). Used by the
PHASE5.1 legacy-path integration; the PHASE5 spike (`tools/tfx_legacy_spike.cpp`)
renders legacy effect libraries through Skia. See `docs/design/PHASE5.1.md`.

## Source — `damucz/timelinefx` (MIT)

- `source/`, `pugixml/`, `LICENSE` — **`damucz/timelinefx`**, `master` @
  `77878f65eb7351bea501df994943000699334ea1` (re-vendored 2026-06-14). The
  render-agnostic C++ port: `TLFX*` classes with `AnimImage` / `XMLLoader` /
  `ParticleManager::DrawSprite` as the extension points. No Marmalade dependency
  in the core (Marmalade is only in the upstream `timelinefx-sample`).
- `pugixml/` — pugixml (MIT), the XML parser the default `PugiXMLLoader` uses.

## License — MIT (clear to ship)

`damucz/timelinefx` is **MIT-licensed** (root repo, `LICENSE` = "MIT License,
Copyright (c) 2019 Daniel"); see `deps/timelinefx_legacy/LICENSE`. We vendor from
it and retain the LICENSE → MIT-compliant, **clear to ship in BUSL Inkternity**.

Provenance note: `peterigz/timelinefx` (which we briefly used first) is a **fork
of `damucz/timelinefx`** that omitted the LICENSE file and is frozen at 2015;
damucz is the MIT root and newer (2019), so we use damucz.

Residual nuance (not a blocker): damucz's MIT covers the C++ *port*; the
underlying TimelineFX algorithm is Peter Rigby's original work (the file headers
carry "Copyright Peter J. Rigby 2009-2010"). Peter **forked damucz's repo into
his own account**, which reads as endorsement of the MIT port. For a commercial
product a one-line courtesy confirmation from Peter is prudent belt-and-suspenders
— but the MIT grant is a solid basis and this no longer gates shipping. (Not
legal advice; worth a real review before release.)

pugixml is independently MIT.

## Build

Isolated C++17 static lib `timelinefx_legacy` (CMakeLists.txt). Needs
`_USE_MATH_DEFINES` for MSVC (`M_PI`). Render-agnostic: the consumer implements
`AnimImage::Load`, `EffectsLibrary::CreateLoader`/`CreateImage`, and
`ParticleManager::DrawSprite`.

## Update recipe

Re-download `source/TLFX*.{h,cpp}`, `pugixml/*`, and `LICENSE` from the pinned
`damucz/timelinefx` SHA above.
