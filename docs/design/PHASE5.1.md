# PHASE5.1 — Particle Systems, take 2: the legacy `.eff` path

Status: **M0–M3 SHIPPED (2026-06-14) — in polish.** End-to-end works in-app:
File ▸ Import FX Library (.eff) (or drag-drop) → embedded asset + parsed library
→ FX Library panel picks the active effect → particle brush (Size/Rate sliders,
Play-on-touch toggle, drag-stamping) places it → legacy-backed
ParticleCanvasComponent renders + loops live (honoring Continuous/Finite),
host-gated, save format INFPNT000016 (adds `playMode`). Smoke-tested: size +
clipping + sliders + save/reopen + parallax + reader-mode waypoint navigation
confirmed good. License = **MIT** (vendored from `damucz/timelinefx`; courtesy
confirm with Peter pending, not a blocker). Remaining = polish (see §7).
Supersedes the runtime + ingestion decisions in PHASE5.md (which targeted the
modern `.tfx` runtime); the rest of PHASE5.md's intent — atmospheric particles
that animate in reader mode — still holds.

**Playback-visibility model (2026-06-14 fix).** The AUTO play trigger (Finite
plays on becoming visible + replays on re-entry; Continuous runs while visible)
gates on **ground-truth render visibility**, not a camera guess: `draw()` sets a
`drawnSinceUpdate` flag (it only runs when the container's `should_draw` passed
for the camera the component is *actually* rendered with — main, parallax-derived,
or reader), and `update()` reads+resets it next frame. The earlier
`should_draw(world.drawData)` (main camera) gate mistriggered on parallax layers,
whose render camera differs — that was the "only the parallax-layer effect plays"
bug. Screenshot passes are excluded so they can't retrigger playback. Note: the
reserved **SKETCH** layer is hidden in reader mode (`LayerKind.hpp`), so a particle
placed there correctly won't play when reading — the particle brush now warns
(once per stroke) when the editing layer is SKETCH (Color/Ink/custom are fine).

Prereqs: PHASE5 spike work (`deps/timelinefx_legacy`, `tools/tfx_legacy_spike.cpp`
— faithful Skia render proven), PHASE4 layer system + "live-outside-cache" draw
bypass, `ResourceManager` (embedded assets), the existing brush-tool +
library-drawer precedent (`MyPaintBrushTool` + `SavedPresetsDrawer` +
`BrushCustomizationDrawer`), animated-GIF playback (the continuously-animating
canvas precedent), `Toolbar` (top-bar buttons).

---

## 1. Why a take 2 (the pivot)

PHASE5 built against the **modern** TimelineFX runtime (`timelinefxlib`, the new
3D-first editor). Hands-on with real exports proved that path unshippable today:
the current Alpha editor is **ahead of every public library commit**, its color
ramps load as garbage (particles render black), and its binary instance/property
format churns per revision (see memory `project_phase5_tfx_reassessment.md`). It
needs the maintainer to publish a matched runtime — parked.

The **legacy** path is the opposite: a **frozen, stable** `.eff`/`data.xml`
format, a render-agnostic classic C++ runtime, and a simpler per-sprite color
model. The PHASE5 spike proved we render it **faithfully in Skia, in color**
(Water Fall, Light Flare, Explosion, animated sprite-sheets). It is also the
editor the artist already owns and matches their Genies & Gems-era workflow.

### Decision log (zynx, 2026-06-14)
- **Runtime = legacy TimelineFX** (`peterigz/timelinefx`, classic C++, vendored
  at `deps/timelinefx_legacy`), driven through Skia via three hooks
  (`AnimImage` / `EffectsLibrary` / `ParticleManager::DrawSprite`).
- **Scope = 2D only.** No 3D projection, no ribbon/line/stretch primitives.
- **Not collaborative.** Host-authored; definitions sync to read-only viewers,
  each client simulates locally (carried over from PHASE5).
- **License = MIT (resolved).** The runtime is vendored from `damucz/timelinefx`
  (the MIT root; `peterigz/timelinefx` is a fork of it that dropped the LICENSE).
  We retain damucz's `LICENSE` → MIT-compliant, clear to ship under BUSL. Residual
  nuance (not a blocker): damucz's MIT covers the C++ port; the underlying
  algorithm is Peter Rigby's (he endorsed the port by forking it). A courtesy
  confirm with Peter is prudent and rides along with the partnership / offer-code
  conversation. See `deps/timelinefx_legacy/VENDORING.md`.

---

## 2. The user flow (zynx sketch, 2026-06-14)

The mental model: a `.eff` is an **imported asset library**; you pick an active
effect from it and **paint** instances onto the canvas with a particle brush.

1. **Import** — `File ▸ Import .eff (fx library)`. Reads the `.eff` zip
   (`data.xml` + shape PNGs [+ `ICONS`]), embeds it as a canvas asset.
2. **Particle brush tool** — a new tool in the tool set (proposed **magic-wand**
   icon), alongside the libmypaint brush. Selecting it makes the canvas paint
   particle effects.
3. **FX Library button** — a top-bar button **next to the brush-library button**.
   Opens the FX Library panel.
4. **FX Library panel** — lists the imported library's effects **by name**, each
   with a **checkbox** to set the *active* effect, and an **icon** thumbnail. If
   the `.eff` carries a usable per-effect icon (the `ICONS` entry), use it;
   otherwise fall back to a headless render of the effect (the spike already
   renders any effect to an image — that's our thumbnail generator).
5. **Painting** — the brush deposits the active effect onto the canvas at a low
   default **rate (~3/second)**, each placement at the current brush **size**.
   The brush has two sliders: **rate** (placement density — see §4.1) and
   **size** (the world scale applied to each stamped effect → the placement's
   `scale`).

---

## 3. Architecture (proposed)

### 3.1 Library as an embedded asset
- `File ▸ Import .eff` → read zip in-memory (need a small zip reader; `.eff` =
  PKZip of `data.xml` + PNGs + `ICONS`). Store the **raw `.eff` bytes once** as a
  `ResourceManager` asset (content-hashed, dedup-aware like images/audio).
- Placed effects reference the library by resource id + the **effect name** —
  never duplicate the library bytes per placement.
- On load, the library is parsed once into a legacy `EffectsLibrary` (our
  `SkiaLibrary`); shapes decode to `SkImage`s (our `SkiaImage`).

### 3.2 Particle brush tool
- New `DrawingProgramToolType::PARTICLEBRUSH` (+ `allocate_tool_type`), modeled
  on `MyPaintBrushTool`: a `gui_toolbox` with **rate** and **size** sliders, and
  pointer handlers that deposit effects while painting. Brush size sets each
  stamp's `scale`; rate sets placement density.
- Host-gated: in a connected session, only the server may paint (mirror the
  PHASE5 `try_add_particle_effect` gate).

### 3.3 Placed effect component
- Refactor the existing modern-stub `ParticleCanvasComponent` to be
  **legacy-backed**. Serialized data (proposed): `{ libraryResourceId,
  effectName, worldPos, scale, seed }`. (Rate is a brush-tool setting — §4.1 —
  not stored per placement.)
- A `Runtime` (PIMPL) holds the legacy `ParticleManager` + the resolved
  `Effect`; `update()` ticks it on the canvas clock; `draw()` runs our Skia
  `DrawSprite`. Reuses the PHASE4 **outside-the-cache** draw path and the
  reader-mode `focus_update → drawProg.update()` animation precedent.
- Lives on the active layer; transform/scale via the Edit tool; embedded in the
  save; animates in reader mode for free.

### 3.4 Rendering
- The spike's `DrawSprite` is the renderer: translate/rotate(deg)/scale,
  per-sprite color via `SkColorFilters::Blend(rgb, kModulate)`, additive →
  `kPlus` else `kSrcOver`, grid sprite-sheet sub-rect for animated shapes.

---

## 4. Open decisions to lock down (before refactor)

### 4.1 ✅ DECIDED — "rate" = placement rate (stamp many)
The brush **deposits separate effect-instance components** as you hold/drag, at
N per second (default ~3). The rate slider raises placement *density* → a
trail/scatter of many small effects along the stroke. Consequences:
- The rate is a **brush-tool setting**, not stored per placement.
- Each placement is its own lightweight `ParticleCanvasComponent` (lib ref +
  effect name + world pos + scale + seed). Expect **many** components per stroke
  → cap total live particles and reuse the outside-cache draw path (§6).
- **Follow-on (small):** rate measured per *time* (N/sec while painting, as
  sketched) — default. (Distance-based spacing, i.e. N per canvas unit, is the
  usual brush approach and avoids speed-dependent clumping; noted as a possible
  refinement, but time-based ships first per the sketch.)

### 4.2 ✅ DECIDED — lifetime honors the effect's authored flag
Respect the `.eff`'s own `Continuous` vs `Finite` setting
(`EffectsLibrary::Time`): snow/embers loop forever; explosions play once and
fade. No extra UI.
- **Follow-on (reader mode):** *when* does a `Finite` placement fire on
  playback? **Resolved:** on becoming visible to the current (rendered) camera —
  it plays on first view and replays on each re-entry; `Continuous` placements
  animate whenever their layer is visible. A **Play-on-touch** brush toggle defers
  the trigger until the placement is tapped. (Deterministic seed / reader-clock
  binding is still pending — §4.6, §7.3.)

### 4.3 ✅ DECIDED — Effect icons (`ICONS`)
Investigate the `.eff` `ICONS` entry (~14 KB, format TBD); use it if it yields
per-effect thumbnails, else fall back to a headless render (proven). The panel
always shows a thumbnail.

### 4.4 ✅ DECIDED — Active-effect selection
**Single** active effect: checkboxes behave radio-like (checking one unchecks the
others).

### 4.5 ✅ DECIDED — Scale, zoom, parallax
Each placement gets a default world scale (set by the brush size, §4.1), scales
with canvas zoom like any component, and **inherits its layer's PHASE4 parallax
depth**.

### 4.6 ✅ DECIDED — Determinism / reader playback
Per-placement **seed** so reader playback is stable and matches across peers.
Animation binds to the scene/waypoint clock; **Finite effects fire on
waypoint/panel arrival**, Continuous effects animate whenever their layer is
visible.

### 4.7 ✅ DECIDED — File-format version bump
New component payload → bump the `INFPNT` header + version (at M3), per the
established convention.

---

## 5. Milestones
- ✅ **M0 — Import + asset**: `File ▸ Import .eff` (+ drag-drop), in-memory zip
  read (miniz), embed as `ResourceManager` asset, parse into `LegacyFxLibrary`.
- ✅ **M1 — FX Library panel**: top-bar button + floating panel; effects by name
  + single-active radio. (Thumbnails deferred → §7.)
- ✅ **M2 — Particle brush + placement**: tool (magic-wand), Size/Rate sliders,
  rate-based drag-stamping, host-gated.
- ✅ **M3 — Component + playback**: legacy-backed `ParticleCanvasComponent`,
  save/load (INFPNT000015), Edit-tool transform, live author + reader animation,
  Continuous/Finite honored.
- ✅ **License** — MIT via `damucz/timelinefx`; no `EXCLUDE_FROM_ALL` gate needed.

## 6. Risks
- **License** (§1) — hard gate on shipping.
- **Rate semantics** (§4.1) — architecture hinges on it; resolve first.
- **Zip dependency** — need a small, license-clean zip reader (miniz/MIT, or an
  existing dep) for in-memory `.eff` reading.
- **Performance** — many placed emitters × many particles; reuse the
  outside-cache path and cap total live particles.

## 7. Polish backlog (post-M3)
Ordered roughly by value / risk:
1. ✅ **Save/load round-trip verify** — placed effects survive save → reopen
   (the component re-resolves its library from the embedded `.eff` resource via
   `resolve_fx_library`; `get_used_resources` + `remap_resource_ids` keep the
   asset from being pruned). Verified by zynx.
2. **Collab/sync verify** (deferred) — host-authored placements + the library
   resource reach read-only viewers; each client re-resolves + simulates locally.
3. **Reader-clock binding + deterministic `seed`** (§4.6) — stable, peer-matched
   playback; Finite effects fire on waypoint/panel arrival. (Today: real-dt sim,
   seed stored but unused.)
4. ✅ **Effect thumbnails** in the panel — `FxLibraryStore::thumbnail()`
   headless-renders each effect at its peak-population frame into an `SkImage`,
   cached per (library, effect) and shown via `MemoryImageDisplay`. Generated
   progressively (a few per frame) so the panel opens without stalling. The
   render manager is intentionally leaked (bounded, one-time) to sidestep the
   legacy lib's unsound nested-effect teardown — see §8. (The `.eff` `ICONS`
   entry was not needed.)
5. **Unclipped live draw** — the square-crop fix is scale-proportional bounds
   (mitigation); render outside the cache for guaranteed no-clip at any size.
6. ✅ **Parallax-depth interplay** (§4.5) — placements inherit their layer's
   PHASE4 depth and pan correctly; playback triggers off real render visibility
   so parallax-layer and base-layer effects both play (2026-06-14 fix, see the
   playback-visibility note up top). Reader-mode waypoint navigation verified.
7. **Undo grouping** — a drag deposits many components → many undo entries; group
   per stroke.
8. **Phone UI** — `gui_phone_toolbox` is empty.
9. ✅ **Docs** — README feature bullet + MANUAL section updated to the `.eff`
   flow (2026-06-14). Still TODO: remove the now-unused modern `timelinefx` link
   from `main` (legacy path superseded it).

## 8. Legacy teardown ownership fixes (vendored)
The vendored runtime's effect/particle teardown had ownership bugs (the upstream
spike leaks rather than tearing down). Both are now fixed in our vendored copy;
verified by sweeping all 24 effects of `test_eff.eff` through render **and**
teardown (`tfx_legacy_spike` with a `delete pm` per effect) — all clean.
- ✅ **`ClearInUse` use-after-free.** It dereferenced
  `GetEmitter()->GetParentEffect()` on the manager's non-pool particles — wrong
  list (pool particles live in the effect's in-use list; non-pool in the
  manager's), and a dangling effect pointer after a finite effect self-deleted
  mid-update. Removed the spurious call (`Reset()` already nulls the emitter).
- ✅ **Double `ReleaseParticle` double-free (nested effects, e.g. Smokey
  Explosion).** A *group* particle is referenced by both its effect's in-use list
  and its emitter's child list, so teardown released it twice (`Effect::Destroy`,
  then the emitter's `particle->Destroy`) → pushed to `_unused` twice →
  double-delete in `~ParticleManager`. Made `ParticleManager::ReleaseParticle`
  **idempotent** via a per-particle `_pooled` flag (set on release, cleared on
  grab; the emitter already correctly sets `_childrenOwner=false` so it never
  *deletes* particles — the manager pool owns them). Normal-sim behaviour is
  unchanged (the flag only guards a second release). This also hardens
  `ParticleCanvasComponent` teardown/replay for arbitrary effects, and let the
  thumbnail renderer drop its leak workaround and tear down normally.
