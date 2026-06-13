# PHASE5 — Particle Systems (TimelineFX runtime + editor)

Status: DESIGN (not started)
Prereqs: PHASE4 layer system (parallax depth, "live-outside-cache" draw
bypass), `ResourceManager` (embedded assets), the file-drop ingestion
pipeline, animated-GIF playback (`ImageResourceDisplay`, the existing
precedent for a continuously-animating canvas element).

A AAA-grade particle feature for atmospheric comics — rain, snow, embers,
dust, magic — authored in a dedicated curve-driven editor and played back
live in author mode and reader mode.

Decision log (zynx, 2026-06-12):
- **Runtime = TimelineFX** (`peterigz/timelinefxlib`, MIT, 1 header + 1
  cpp, no deps, render-agnostic, first-class 2D path). Chosen over
  `.pex`/Starling (too limited — linear, single-texture, two modes),
  PixiJS particle-emitter JSON (open but stale editor), three-nebula
  (3D-schema baggage), and Effekseer/PopcornFX (own renderer, editor not
  embeddable). TimelineFX uniquely pairs a real timeline/curve editor —
  the "Genies & Gems built-in editor" experience — with a drop-in
  render-agnostic C++ runtime we feed straight into Skia `drawAtlas`.
- **Not a collaborative feature.** Host-authored, single-writer. The
  emitter *definition* still syncs to read-only viewers so the audience
  sees the effect; each client simulates locally.
- **Rendered via Skia `drawAtlas`; lives outside the draw cache** (reuses
  the PHASE4 selection/parallax bypass).

## 1. Goal

Let an artist drop a designed particle effect onto the canvas as a
first-class component on a layer. It plays continuously in author mode
(live preview) and animates in reader mode bound to the waypoint clock,
so panning across a panel reveals falling snow, drifting embers, or a
shimmer of magic with real motion — not a static decal.

The effect itself is authored in the **TimelineFX editor** (external,
curve-driven: size / color / alpha / velocity / spin over particle life
as authored graphs). Inkternity *consumes and renders* the editor's
exported effect package; it does not (in v1) author effects in-engine.

## 2. Explicitly out of scope (and why) — the honest call-outs

Per the standing rule (don't ship code we know will fail; call out what
we can't realistically support):

- **True 3D particles.** The engine is 2D. We use TimelineFX's explicit
  2D path (`tfx_2d_instance_t`, `tfx_Get2dInstanceBuffer`,
  `tfx_SetEffectPosition2d`). Particles flying *toward* the camera in 3D
  is not on the table; the depth story is **per-layer parallax** (§8):
  an emitter on a far parallax layer crawls, one on a near layer streaks.
  That is pseudo-depth layering, not per-particle 3D.
- **GPU-compute sims of millions.** Skia exposes no compute path we can
  rely on here; the sim runs on the CPU (TimelineFX is multithreaded +
  SIMD, so a few thousand live particles is comfortable — plenty for
  atmosphere, not bullet-hell).
- **Collaborative co-editing of emitters.** Deliberately removed (§4).
  Single-writer host authoring; no concurrent-edit conflict resolution.
- **Live particles in SVG export.** SVG can't hold a simulation. Particle
  layers export to SVG as **nothing** (or a baked raster, §8); the
  correct capture path is raster at a chosen instant.
- **In-engine effect authoring (v1).** The editor is TimelineFX's. An
  in-app editor is a possible later phase, and because we standardize on
  the TimelineFX package format, it would be additive — the runtime
  wouldn't change.

## 3. Why TimelineFX (summary)

The runtime value of a particle *engine* is modest; the **editor** is the
feature, and no third-party editor is embeddable in our ImGui/Skia UI —
so adopting a heavyweight engine like Effekseer would mean a
renderer-integration project *and* still building our own editor.
TimelineFX sidesteps both: its editor is the authoring surface, and its
runtime is `tfx_UpdateParticleManager(pm, dt)` → a flat buffer of 2D
sprite instances we draw ourselves. We bind a library; we don't port one
and we don't fight a second renderer for the GL context.

## 4. Architecture: host-authored, definition-synced, simulated locally

Keep particle emitters as a **normal `CanvasComponent`** so they ride the
existing save/load, layer-membership, z-order and undo plumbing for free
(a separate non-synced sidecar would re-invent all of that). Then:

- **Authoring is gated to the host** in the UI — single-writer.
- **The emitter definition syncs** (small NetObj) to read-only viewers,
  so the audience sees the effect. "Host-only" means host-only *editing*,
  not host-only *visibility*. This fits the subscription-hosting /
  live-read-only-viewer model.
- **Each client simulates locally** from `(definition, seed)`.

Why this matters: single-writer **dissolves the hardest problem** —
cross-client editing determinism + conflict resolution. Particles are a
non-interactive atmospheric effect; nothing shared depends on two
machines being frame-identical, so local sims are fine even if slightly
out of phase. Determinism only has to hold **locally**, for export
reproducibility — a stored seed handles that.

Undo applies to **definition edits only** (swap effect, move emitter,
change play params), exactly like brush-parameter edits — never to
per-frame particle state.

## 5. The TimelineFX integration (grounded in the real API)

Verified against `timelinefx.h` @ `peterigz/timelinefxlib` (alpha; pin a
commit — see §9). The library is truly data-out:

**Init (once, app startup):**
```c
tfx_InitialiseTimelineFX(max_threads, memory_pool_size); // self-managed pool
```

**Load an effect package (per ParticleCanvasComponent, from the bytes
embedded in the save file):**
```c
tfx_library lib = tfx_LoadEffectLibraryFromMemory(
    data, size, shape_loader, uv_lookup, user_data);
```
- `shape_loader(filename, tfx_image_data_t* img, raw_bytes, size, user)`
  is called per shape. We decode `raw_bytes` (PNG) into a Skia image,
  pack it into our particle **atlas** `SkImage`, and stash the atlas
  index/UVs on `img->ptr` (or via `#define tfxCUSTOM_IMAGE_DATA`). This
  reuses `ResourceManager`-style embedded-asset handling.

**The package is self-contained — shapes are embedded, not linked
(verified, and load-bearing for drag-and-drop).** The TimelineFX export
is a `tfx_package_t`: a header + an **inventory**
(`tfx_package_inventory_t` of `tfx_package_entry_info_t`, each a named
"file stored in the package") + a single data dump. Custom bitmaps are
imported as *shapes* and stored as inventory entries **inside** the
package — `shape_loader` receives their raw bytes (`raw_image_data`,
`image_size`) straight from that inventory, and the loader raises
`tfxErrorCode_library_loaded_without_shape_loader` precisely because the
shapes travel with the file. So there are **no external image references
to chase**: one dropped package = effect data + every bitmap. (To verify
once licensed: that the editor's export dialog bundles shapes — i.e. no
"data-only / link external images" toggle is selected. Given the format
requires an inventory and refuses to load shapeless, the packaged export
should be the default path.)

**Spawn (host adds / on load):**
```c
tfx_particle_manager pm =
    tfx_CreateParticleManager(tfx_CreateParticleManagerInfo(setup_2d));
tfxEffectID id;
tfx_AddEffectTemplateToParticleManager(pm, effectTemplate, &id);
tfx_SetEffectPosition2d(pm, id, ex, ey); // emitter-local origin
```

**Per frame (in the component's `update()`, off `main.deltaTime`):**
```c
tfx_UpdateParticleManager(pm, deltaTimeMs);
```

**Draw (in the component's `draw()`):**
```c
tfx_2d_instance_t* sprites = tfx_Get2dInstanceBuffer(pm);
int n = tfx_GetInstanceCount(pm);
// each tfx_2d_instance_t → one drawAtlas quad
```
`tfx_2d_instance_t` carries everything `drawAtlas` needs:
| TimelineFX field | meaning | → Skia `drawAtlas` |
| --- | --- | --- |
| `position` (vec4) | x, y, stretch (z), **rotation (w)** | `SkRSXform` rotation + translate |
| `size_handle` | sprite size px + anchor/handle | `SkRSXform` scale + the source rect |
| `indexes` | image-data index (which shape) | selects the atlas `SkRect` |
| `indexes` + `curved_alpha_life` + `intensity` | color-ramp lookup + alpha curve | per-sprite `SkColor` (see note) |

Then one batched call: `canvas->drawAtlas(atlasImage, xforms, rects,
colors, n, blendMode, ...)`.

**Color-ramp note (the one real nuance):** TimelineFX delivers per-particle
color via generated *color-ramp bitmaps* + per-instance indices, designed
for a shader to sample. `drawAtlas` takes a flat per-sprite `SkColor`, so
we **CPU-sample the ramp** at each instance's life position to produce
that color (cheap — a texel fetch per particle). Blend mode comes from the
effect (additive → `SkBlendMode::kPlus`, normal → `kSrcOver`).

## 6. Render decision

- **Outside the cache.** A live emitter must not be baked into the BVH /
  window cache. Reuse the exact pattern PHASE4 already built twice
  (selected components erased from cache; parallax bypass): the particle
  component draws live on top of the cached static canvas each frame.
- **Keep-rendering signal.** While an emitter is alive and visible, it
  requests continuous redraws the same way animated GIFs do
  (`mustUpdateDraw` → redraw request). Idle/empty emitters stop asking.
- **Coordinates / infinite-zoom safety.** Simulate in **emitter-local
  float space** (TimelineFX is float). Place the result through the
  component's `CoordSpaceHelper` → world → screen via the camera, exactly
  like every other component. Particles are short-lived and local to the
  emitter, so no `WorldScalar` is needed inside the sim and infinite zoom
  is safe (the emitter's world transform scales uniformly; particle px
  sizes scale with `inverseScale`).

## 7. Data model + persistence

`ParticleCanvasComponent` (new `CanvasComponentType::PARTICLE`) stores
**only the authored definition**, not sim state:
- the TimelineFX effect-**package** bytes, re-embedded **verbatim** into
  the `.inkternity` save. Because the package already embeds its shapes in
  its inventory (§5), storing the package blob carries the bitmaps too —
  the document stays self-contained through save/load and the same blob
  syncs to viewers, with no external image references,
- the effect path/name within that package,
- emitter transform (`CoordSpaceHelper`),
- RNG **seed**,
- playback params (loop, play-in-author-mode, reader-clock binding,
  preview-time for static capture).

- **NetObj sync:** the definition is a small synced object; host is the
  only writer; viewers reconstruct the `tfx_particle_manager` locally.
- **Save-format bump:** new component type ⇒ header
  `INFPNT000013` → **`INFPNT000014`**, `VersionNumber(0,12,0)` →
  **`(0,13,0)`** (VersionConstants.hpp/.cpp + map entry). Gate the new
  field reads on `version >= VersionNumber(0,13,0)`.
- **Undo:** definition edits only (§4).

## 8. Reader-mode, parallax, flatten/export interaction

- **Reader clock binding (the payoff).** Bind the emitter's `dt` to
  reader playback time so effects play/loop while the reader sits on a
  panel and continue across transitions. Author mode shows live preview.
  Atmospheric particles + the waypoint reader is the differentiator.
- **Parallax depth.** An emitter rides its **layer's** parallax depth
  (PHASE4): put rain on a near layer to streak past, mist on a far layer
  to crawl. No new depth concept — reuse the layer property.
- **Flatten / export.** Flattening a particle layer or exporting a frame
  **rasterizes the current instant** (the only correct semantics for a
  time-based effect). Add a **preview-time scrubber** so the artist picks
  the captured instant; the stored seed makes that capture reproducible.
  This composes with the PHASE4 Flatten feature — a particle layer
  flattens to a single raster component like any other visual content.

## 9. Risks / open questions

- **Library is early-alpha** (author: "still very much a work in
  progress… give feedback on the interface"). Mitigation: **vendor it and
  pin a specific commit**; wrap the C API behind a thin Inkternity
  adapter so churn is contained to one file. M0 finding: the effect
  package format is **not** version-compatible across the library's
  history — `master` HEAD partially-loads (`some_data_not_loaded`)
  packages authored with newer code. We therefore pin the commit the
  author's `zest` renderer uses (`a5f323d`, the reference-integration +
  sample-asset revision), which loads them cleanly. Re-validate the
  artist's actual editor export against the pin; bump the pin to match the
  editor if a newer skew appears.
- **Editor cost/license to reconfirm.** The C++ lib pairs with a *new
  alpha editor* (rigzsoft "timelinefx-alpha-version"), which is **not**
  necessarily the £29.99 BlitzMax-era editor first cited. Confirm the
  current editor's price/availability/license before committing the
  authoring workflow. (Runtime is MIT and ships free regardless — this is
  an author-tooling cost, not a redistribution cost.)
- **Color-ramp CPU sampling** per particle — verify cost at target counts;
  cache ramps per effect.
- **Threading / memory pool.** TimelineFX manages its own pool and worker
  threads; start with conservative `max_threads` and confirm it composes
  with the app's threading and the single-writer model.
- **Particle/atlas caps.** Decide a live-particle ceiling and atlas size
  budget; `log()` any cap hit rather than silently dropping.
- **MIT attribution** preserved in `assets/data/third_party_licenses/`.

## 10. Milestones

- **M0 — runtime spike.** Vendor `timelinefxlib` (pinned). Hardcode one
  test `.tfx` package; `tfx_UpdateParticleManager` off `deltaTime`;
  `tfx_2d_instance_t` → `drawAtlas`; CPU color-ramp sampling; draw a fixed
  emitter at a fixed canvas point, **outside the cache**. Proves the
  render path end-to-end.
- **M1 — `ParticleCanvasComponent`.** Real component on a layer:
  emitter-local sim → `CoordSpaceHelper` → world; live preview; embedded
  package; **drop-a-`.tfx`-on-canvas** ingestion via the existing
  file-drop pipeline; host-gated authoring.
- **M2 — persistence / sync / undo.** Definition serialization + format
  bump (`INFPNT000014` / `0.13.0`); NetObj definition sync (single-writer,
  viewers simulate locally); undoable definition edits.
- **M3 — reader / parallax / capture.** Reader-clock binding; layer
  parallax depth ride-along; flatten-to-raster + export preview-time
  scrubber.

Deferred (future phase): in-app effect editor (reads/writes the TimelineFX
package; additive, runtime unchanged); per-effect performance baking
(TimelineFX pre-bake/compute path); animated thumbnails in the layer panel.
