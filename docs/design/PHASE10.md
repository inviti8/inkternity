# PHASE 10 — "New Publications" lobby carousel

## Status

**SCOPED — not started.** Second half of the PHASE9/10 split (the other half is
[PHASE9.md], the 3D armature poser). This phase is the small, low-risk one —
**scheduled after the armature** (zynx, 2026-06-21): it ships a promotional
carousel into the lobby to cross-pollinate Inkternity artists' works. No
save-format bump (touches the lobby, not canvas files).

Requested by zynx: a promotional carousel in the lobby, in its own section
**"New Publications"**, used to promote & cross-pollinate the works of
Inkternity artists. v1 is a **curated list baked into the build**; each entry is
a promotional image + a deep-link to the specific publication on the Heavymeta
Portal.

Honest size: **~3–4 days, low risk.** The portal side already exists and the
lobby already has horizontal-scrolling list primitives — this is mostly
plumbing + a JSON schema + bundled art.

## Why this is low-risk (grounding)

The portal half is **already fully built** (`heavymeta_collective`):

- A "publication" is an `inkternity_canvases` row: `canvas_id` (UUID), `title`,
  `description`, `cover_cid` (IPFS), `price_usd`, plus the owning artist.
- **The deep-link already exists and is public, no auth:**
  `https://heavymeta.art/inkternity/canvas/{canvas_id}`.
- Cover art lives at `<ipfs-gateway>/ipfs/{cover_cid}`.
- The portal even has forward-compat API endpoints stubbed (design-doc "B7",
  "not yet called by desktop, schema ready") — the clean upgrade path to a
  *live* feed later.

The Inkternity half has every primitive we need:

- Lobby is `src/Screens/FileSelectScreen.cpp` (Clay immediate-mode GUI).
- `src/GUIStuff/Elements/GridScrollArea.hpp` already supports **horizontal
  scroll + horizontal clip** — that is the carousel.
- Assets bake in via `assets/data/` → `install(DIRECTORY assets/data ...)` in
  `CMakeLists.txt`, loaded at runtime with `load_file_to_string` +
  `nlohmann::json`. So "curated list baked into the build" =
  `assets/data/publications/catalog.json` + bundled images. No codegen.
- Opening the link cross-platform: **SDL3 `SDL_OpenURL()`** — one call, no new
  dependency.

## Model

A bundled catalog file lists curated publications. Each entry renders as a card
in a horizontal carousel in a new lobby section. Clicking a card opens that
publication's portal page in the system browser.

### Data: `assets/data/publications/catalog.json`

```json
{
  "version": 1,
  "publications": [
    {
      "id": "canvas-uuid-or-local-slug",
      "title": "The Work's Title",
      "artist": "Artist Display Name",
      "blurb": "One-line promo description.",
      "image": "data/publications/img/work01.jpg",
      "url": "https://heavymeta.art/inkternity/canvas/3fa85f64-...",
      "price_usd": 4.99
    }
  ]
}
```

- `image` is a **bundled local path** (see decision below), decoded at load.
- `url` is the canonical portal deep-link.
- `price_usd` optional (shown on the card if present).
- `version` lets us evolve the schema without breaking old builds.

## Decisions (recommendations baked in; override if desired)

1. **Promo images: bundle locally (RECOMMENDED) vs. fetch `cover_cid` from
   IPFS at runtime.** Bundling matches the "baked into the build" framing, has
   **no network failure mode**, guarantees the section always renders, and lets
   the promo art be *art-directed* (a banner that differs from the square cover
   thumbnail). v1 = bundle. The live-feed-from-portal path stays open for a
   future phase using the same card schema.
2. **No save-format bump.** This is lobby UI + bundled assets only; it never
   touches `.infpnt` canvas files. No `VersionConstants` change.

## Build (milestones)

1. **M1** — `catalog.json` schema + loader (parse with `nlohmann::json`, decode
   bundled images to `SkImage`). Add `assets/data/publications/` (catalog +
   `img/`) and confirm CMake installs it on all platforms. Graceful skip if the
   file is missing or an image fails to decode.
2. **M2** — "New Publications" section in `FileSelectScreen`: a horizontal
   `GridScrollArea` of image cards (cover + title + artist + optional price),
   click handler → `SDL_OpenURL(entry.url)`. Place it in `main_display()`
   alongside the existing menu branches.
3. **M3** — polish: empty-state (hide the section if the catalog is empty),
   per-card fallback image, hover/press affordance, docs (`MANUAL.md` +
   `README.md`), seed the curated list with the launch set.

## Effort estimate

| Work | Est. |
|---|---|
| Catalog schema + JSON loader + image decode | ~0.5 day |
| Bundle assets + CMake install verify (Win/macOS/Linux) | ~0.5 day |
| Lobby section + horizontal carousel + click→OpenURL | ~1–1.5 days |
| Empty/fallback states + polish + docs + seed list | ~1 day |

**Rough total: ~3–4 days.**

## Out of scope (v1)

- **Live feed from the portal.** v1 is a static bundled catalog; the portal API
  (`B7`) is stubbed but not consumed yet. Future phase.
- **In-app purchase / token entry flow.** The card links *out* to the portal,
  which already owns checkout + token minting. We do not embed Stripe.
- **In-app preview of the publication.** Clicking opens the browser; we do not
  render the canvas in-lobby.
- **Per-artist filtering / search / categories.** Curated flat list only.

## Backward compatibility

No canvas-file format change → existing files are untouched and unaffected. A
missing/empty `catalog.json` simply hides the section, so older asset bundles
degrade gracefully. The catalog's own `version` field guards future schema
changes.
