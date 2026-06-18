# PHASE 5.5 — Rendering performance: transitions, reader mode & multi-scene projects

## Status

**Investigation + plan, pre-implementation.** Reported by zynx after first
production use. Two distinct symptoms:

1. **Transition stutter** — a detailed multi-drawing sequence chained with
   waypoints drops frames *while moving between waypoints*, even after
   flattening the pixel drawings. Worst in reader mode. Worse anticipated once
   particle FX are added (no particles in the sequence yet).
2. **Steady-state slowdown with off-screen scenes** — after drawing a detailed
   scene on its own layers, moving to a spatially-separate region (different
   layers) and drawing there turned sluggish; *hiding the first scene's layers*
   restored performance, despite that content being off-screen.

This doc is the code-analysis pass, ranked root-cause findings, and a
sequenced optimization plan. It builds on (does not repeat) the steady-state
drawing investigation in `PERF-INVESTIGATION.md`, whose Tier-1 findings
(per-stroke raster recomposite, cache threshold) are already mitigated
(per-component `SkImage` cache, threshold 300, Flatten-in-View). **PHASE5.5
covers two regimes the prior doc didn't: the camera in motion (symptom 1,
findings F1–F5) and per-layer / whole-canvas overhead that isn't viewport-culled
(symptom 2, finding F7).**

Standing constraint (carried from PHASE5): we do **not** ship code that
attempts something we know can't hold up. Where an optimization can't
realistically deliver, this doc says so instead of pretending.

---

## 1. Symptom & scope

| Aspect | Detail |
|---|---|
| Trigger A | Camera **transition between waypoints** (pan + zoom + rotate animation) → §3 F1–F4 |
| Trigger B | **Drawing with a large off-screen scene present** on other (visible) layers → §3 F7 |
| Mode | Both modes affected; **reader mode is worst for Trigger A** — it's almost entirely camera motion (auto-advance chains, navigate, branch hops) |
| Already done | Flattened pixel layers — helps steady raster cost, does **not** address either trigger here (see §2, F7) |
| Anticipated | Particle FX in a sequence will compound Trigger A (see §5 / F5) |

The key reframings:

- **Trigger A:** flattening reduces component count, but the dominant transition
  cost is not proportional to component count — it's a fixed full-window
  recomposite that happens every frame the camera moves. That's why flattening
  didn't help the waypoint stutter.
- **Trigger B:** off-screen content *is* culled from rasterization, but per-layer
  compositing (`saveLayer`) and whole-canvas BVH rebuilds are **not** viewport-
  culled — so a big off-screen scene still taxes every frame. That's why hiding
  its layers worked.

---

## 2. How rendering works today (the relevant slice)

Per frame, unconditionally (`SDL_AppIterate` → `update()` + `regular_draw()`,
`src/main.cpp:915`): no dirty-frame skipping; the app always redraws.

Drawing-program render has three paths (`DrawingProgram::draw`,
`src/DrawingProgram/DrawingProgram.cpp:1061`):

1. **Screenshot** — direct layer-tree walk, uncached.
2. **Parallax bypass** (`:1064`) — if *any* visible layer has non-zero
   parallax depth, walk the layer tree directly every frame (no cache). Each
   parallax layer renders through a per-layer **derived camera**
   (`DrawingProgramLayerListItem.cpp:229-246`).
3. **Cached path** (`:1098`) — `drawCache.update_and_draw_cached_canvas()`.

The cache has two tiers (`DrawingProgramCache.cpp`):

- **BVH node caches** — world-space surfaces, one per BVH node, each rendered
  once at the node's own coords/resolution and reused until its region is
  invalidated (`refresh_draw_cache`, `:309`). **These survive camera motion.**
- **Window cache** — one screen-sized surface holding the final composite
  (all visible node images + uncached/unsorted components + per-layer
  `saveLayer` alpha/blend passes). A static frame is **one blit** of this
  surface (`:433`).

The window cache is the fast path — and it is exactly what breaks during a
transition.

---

## 3. Root-cause findings (ranked)

### F1 — HEADLINE: the window cache fully rebuilds every frame the camera moves

`update_and_draw_cached_canvas` (`DrawingProgramCache.cpp:419`):

```cpp
else if(windowCache.attachedDrawingProgramCache != this
        || drawData.cam.c != windowCache.coords) {   // <-- exact camera-pose key
    refresh_all_draw_cache(drawData);
    window_cache_complete_refresh(drawData);          // full-window clear + recomposite
}
```

`window_cache_complete_refresh` (`:409`) clears the entire window surface and
calls `draw_components_to_canvas(..., std::nullopt)` — recompositing **every**
visible cached-node image + **every** uncached/unsorted component + **every**
per-layer `saveLayer(alpha/blend)` pass into an offscreen surface, then blits
it.

A waypoint transition animates `cam.c` continuously
(`DrawCamera::update_main`, `src/DrawCamera.cpp:74`), so `cam.c !=
windowCache.coords` is true **every frame for the whole transition**. The
window cache delivers zero reuse precisely when the camera moves.

- Static frame: 1 blit.
- Transition frame: full recomposite (N node blits + M direct component draws +
  L layer composite passes) **into an offscreen surface, then blit** — a round
  trip whose offscreen step has no reuse value during motion.

This is the waypoint-transition drop. It is independent of flattening (a
flattened layer is still recomposited every frame during motion) and it is the
single biggest lever.

### F2 — Zoom transitions are worse than pure pan

Both `refresh_all_draw_cache` (`:282`) and cached-node selection in
`draw_components_to_canvas` (`:446`) gate on
`node->coords.inverseScale <= cam.c.inverseScale`. When the transition **zooms
in** past a node's baked resolution, that node drops out of the cached set and
**all of its components draw directly** each frame (and raster
`MyPaintLayer` components recomposite at the finer detail). So a zoom-heavy
waypoint jump degrades toward "draw everything directly" mid-animation, on top
of F1.

### F3 — Parallax layers bypass the cache entirely, always

`DrawingProgram.cpp:1064`: any visible parallax layer ⇒ uncached layer-tree
walk every frame, even at rest. During a transition that stacks with the
direct-draw cost. Known/accepted from PHASE4; the documented mitigation is
Flatten-in-View. Worth confirming whether the user's sequence has parallax
layers active (likely, given the parallax player work).

### F4 — Image mipmap re-selection + async decode during zoom

`ImageResourceDisplay::camera_view_update` (`:109`) recomputes the needed
mipmap level per visible image as zoom changes and spawns decode threads
(capped by `IMAGE_LOAD_THREAD_COUNT_MAX`). A zoom transition crosses mipmap
levels mid-animation → decode-thread contention on the main thread + visible
quality pop on arrival.

### F5 — Particles (anticipated): per-frame whole-region cache invalidation

`ParticleCanvasComponent::update` (`src/CanvasComponents/ParticleCanvasComponent.cpp:117`)
calls `invalidate_cache_at_component(...)` **every frame**, plus up to 4 sim
steps/frame and O(live-particles) draw. Each on-screen effect forces its region
uncached and (today) dirties the window cache — so even at rest, one playing
effect re-triggers F1's full-window recomposite every frame. Add a transition
and every frame is a full uncached redraw. This is the compounding hit the user
correctly anticipates.

### F6 — Steady-state raster cost (background, already mitigated)

From `PERF-INVESTIGATION.md`: per-stroke recomposite and the cache threshold.
Mitigated via per-component `SkImage` cache, threshold 300, and Flatten. Listed
here only so we don't re-solve it.

### F7 — Per-layer overhead scales with visible-layer count, not viewport content

Reported separately by zynx: a detailed sequence drawn on its own set of
layers, then the viewport moved to a spatially-separate region on *different*
layers, and drawing the new section turned sluggish. **Hiding the first
sequence's layers restored performance** — even though that content was fully
off-screen.

What's actually culled vs. not:

- **Culled correctly:** BVH node traversal is viewport-gated
  (`traverse_bvh_run_function(cam.viewingAreaGenerousCollider, …)`,
  `DrawingProgramCache.cpp:441`/`:281`), and `should_draw`
  (`CanvasComponentContainer.cpp:160`) collides each component's `worldAABB`
  against the viewport. Off-screen content is **not rasterized**. The artist's
  expectation holds for pixel rendering.
- **NOT culled:** the layer-tree walk
  (`recursive_draw_layer_item_to_canvas`, `:466`) does work per *visible
  layer*, independent of whether that layer has anything on screen:

  1. **One `saveLayer(nullptr, &layerPaint)` per visible layer, every frame**
     (`:471`), gated only by `get_visible()`. Each is an offscreen-buffer
     allocation + an alpha/blend composite on `restore()`. A sequence spread
     across N layers pays N such round trips per frame while those layers are
     visible — even with 0 of their content on screen. **This is the dominant
     cause of the reported symptom**; hiding the layers skips the whole block.
  2. **The full `unsortedComponents` scan runs once per visible layer**
     (`:486`) — viewport collide culls the *draw* but not the *iteration*, so
     cost is O(visible-layers × unsorted-count). Hot while freshly-drawn strokes
     await BVH absorption.
  3. **BVH rebuilds reprocess the whole canvas.** `should_rebuild()` trips
     every 300 unsorted (`:117`); `internal_build` walks
     `get_flattened_component_list()` — *every* component in *both* sequences
     (`:142`–`158`) — and drops node caches. The off-screen first sequence
     inflates every rebuild fired while drawing the second.

  (Also `check_updateable_components()`, `DrawingProgram.cpp:676`, iterates
  particles/GIFs every frame regardless of viewport — see F5.)

This is a **distinct axis** from F1–F4: those are camera-motion costs; F7 is a
steady-state cost that scales with total canvas content and visible-layer count.
It is what makes a large project sluggish *even while standing still and
drawing*, and it will get worse as the artist adds more chained scenes on more
layers.

---

## 4. Optimization plan (ranked by impact ÷ effort)

### O1 — Bypass the window cache while the camera is in motion *(headline fix)*

When the camera is animating (`smoothMove.occurring`, or simply
`cam.c != windowCache.coords`), **skip `window_cache_complete_refresh` and draw
node caches + unsorted components straight onto the screen canvas**, exactly
like the existing parallax-bypass path (`DrawingProgram.cpp:1064`).

Why it works: during motion the offscreen window surface has zero reuse, so the
clear-composite-blit round trip is pure overhead. Drawing the (world-space,
camera-stable) node caches directly to the screen keeps the heavy lifting
cached while removing one full-window surface allocation/clear/composite/blit
per frame. When motion ends, one `window_cache_complete_refresh` re-establishes
the at-rest fast path.

- **Impact:** high — directly removes the F1 per-frame overhead.
- **Effort:** small–medium (mirrors existing parallax bypass; same
  `draw_components_to_canvas` call, different destination canvas).
- **Feasibility:** high. Low risk — it's the path the parallax case already
  takes.

### O6 — Per-layer viewport cull + `saveLayer` elision *(headline F7 fix)*

Two independent wins on the layer-tree walk (`:466`):

- **Per-layer viewport cull.** Give each layer a cached union-AABB (the union of
  its components' world bounds, maintained incrementally on add/erase/transform,
  or derived cheaply from which of its BVH nodes / unsorted comps collide the
  viewport). Before the `saveLayer` + predraw scans, skip the layer entirely
  when its union-AABB doesn't intersect `cam.viewingAreaGenerousCollider`. A
  fully off-screen layer then costs ~nothing — making "hide the old layers"
  unnecessary and directly fixing the reported symptom (F7.1 + F7.2).
- **`saveLayer` elision for plain layers.** A layer at alpha == 1.0 and normal
  blend mode needs no isolation buffer — draw its components straight to the
  destination canvas and skip `saveLayer`/`restore` (F7.1). Only layers with
  non-trivial alpha or blend pay the offscreen round trip. This helps even
  on-screen layers, which are the common case.

Secondary (F7.2): bucket `unsortedComponents` by `parentLayer` (or scan once and
dispatch) so the per-frame scan is O(unsorted) instead of
O(visible-layers × unsorted).

- **Impact:** high for real multi-scene projects (the artist's actual
  workflow). **Effort:** medium — the per-layer AABB needs correct incremental
  maintenance; the `saveLayer` elision is small and local.
- **Feasibility:** high. **Risk/caveat (standing constraint):** the union-AABB
  must stay correct under add/erase/move/flatten or off-screen content could be
  wrongly culled — validate against transforms and undo before trusting it. The
  `saveLayer` elision must check *both* alpha and blend (and any future
  layer-level effect) so we never drop an isolation buffer that's actually
  load-bearing.

Bigger architectural option (defer): **cache each layer's composite to its own
world-space surface** so a static, unchanged layer is one blit and only edited
layers re-render. This is the proper long-term answer to F7 (and pairs with the
window-cache rework in O2), but it multiplies cache memory by layer count and
overlaps heavily with O1/O2 — do not start it before O6's cheap wins are
measured.

### O5 — Defer image mipmap upgrades until the camera settles *(cheap pairing)*

During active motion, render with the already-loaded (coarser) mipmap and only
spawn higher-level decodes once the camera settles (reuse the existing
"settle" concept from `ReaderMode`'s `audioApplyDelay`). Removes F4
decode-thread contention mid-pan; sharpens on arrival (the eye can't resolve
detail mid-transition anyway).

- **Impact:** medium. **Effort:** small. **Feasibility:** high.

### O-P1/O-P2/O-P3 — Particle budgeting *(do before particles ship in sequences)*

- **O-P1:** sim + draw only effects whose AABB is in/near the viewport; pause
  off-screen effects (partly present via `drawnSinceUpdate`).
- **O-P2:** draw particles **over** the cached composite each frame instead of
  letting them invalidate the window cache — so a playing effect at rest does
  not re-trigger F1. (Composites cleanly with O1: static layers cached,
  particles drawn live on top.)
- **O-P3:** set and **enforce** a realistic concurrent-effect / live-particle
  budget. Honest call-out per the standing constraint: unbounded particle
  counts will **not** hold 60fps; we pick a budget, document it, and cull/queue
  beyond it rather than promise more.
- **Impact:** high for the anticipated regime. **Effort:** medium.
  **Feasibility:** high for culling/budget; the budget number itself needs
  on-device measurement.

### O3 — Pre-warm node caches along the transition corridor

The transition's destination camera is known at `smooth_move_to` time. Build
(or widen) the node caches covering the start, the end, and the swept corridor
**before/early in** the move so zoom transitions don't rasterize nodes
mid-animation (attacks F2's stall, not just its steady cost).

- **Impact:** medium (zoom-heavy jumps). **Effort:** medium.
  **Feasibility:** medium. Pairs with O1.

### O4 — Zoom-stable node selection (quality/perf knob)

Allow a cached node to be reused **scaled** across a limited zoom-in range
before re-rasterizing, trading slight mid-animation softness for smoothness,
then re-sharpening at rest. Directly softens F2.

- **Impact:** medium. **Effort:** medium. **Feasibility:** medium — it's a
  visual-quality tradeoff that must be validated with the artist, not assumed.

### O2 — World-space (reprojected) window cache *(expensive; last resort)*

Render the composite to an over-sized world-space surface and, during pure
**pan**, translate-and-blit it, recompositing only when the camera leaves the
padded region. Classic scroll-tile over-render.

- **Honest feasibility call-out:** a single combined window cache is **wrong**
  in world space because the current composite bakes **per-layer `saveLayer`
  alpha/blend** in viewport space (`recursive_draw_layer_item_to_canvas`,
  `:466`). A correct version must cache **per layer**, then composite layers
  each frame — materially more memory and complexity. **Zoom and rotation break
  the simple reblit entirely** (only pan is cheap). So O2 buys smooth *panning*
  only, at real cost.
- **Recommendation:** do **not** build O2 unless O1 + O3/O4 leave transitions
  below target. If we do, scope it to pan-only with per-layer caches and be
  explicit that zoom/rotate still pay full recomposite.
- **Impact:** high for pan-only. **Effort:** large. **Feasibility:** medium,
  with the per-layer caveat above.

---

## 5. Reader-mode specifics

Reader mode is dominated by camera motion, so **O1 is the headline reader-mode
win**. Reader mode already does the right structural things: SKETCH layers are
skipped (`DrawingProgramLayerListItem.cpp:222`), waypoint markers are skipped
(`WaypointCanvasComponent.cpp:51`), and editor GUI is suppressed
(`Toolbar.cpp:262` region). Additional reader-only lever:

- **Render-quality drop during motion** — disable Skia AA and/or use coarser
  mipmaps (O5) while a transition is in flight; restore at rest. The viewer
  can't see the difference mid-pan, and it directly cuts per-frame cost.

---

## 6. Measurement plan (gate before O2+)

A lightweight overlay already exists — `Toolbar::performance_metrics()`
(`src/Toolbar.cpp:1862`, toggled via Settings → Debug "show performance
metrics"): FPS, item count, coord, zoom, rotation, undo queue. The fuller
harness proposed in `PERF-INVESTIGATION.md` was **not** built; this basic panel
is all that exists.

**M0 — extend that panel** (cheaper than the old proposed harness) with the
transition-relevant counters:

- ms spent in `DrawingProgram::draw` this frame.
- `windowCacheRebuiltThisFrame` (bool) — confirms F1 firing during a move.
- cached node-blits vs direct component-draws this frame.
- mipmap decodes in flight (F4).
- active particle effects + total live particles (F5).
- visible layers total vs. visible layers actually intersecting the viewport
  (confirms F7 — the gap is wasted `saveLayer`s) + `saveLayer`s issued/frame.
- 1% / 0.1% low frame-time over a short ring buffer (steady FPS hides the
  hitches that actually hurt).

Gate: reproduce the waypoint stutter with the overlay on; confirm
`windowCacheRebuiltThisFrame` is true every transition frame and that draw-ms
spikes track it. That validates F1 before we invest in anything beyond O1.

---

## 7. Sequenced milestones

**Re-sequenced after the B-series (§9) — measurement flipped O6 ahead of O1.**

1. **M0 — Instrument.** ✅ Done. Overlay extended; baseline captured (§9). The
   B-series refuted the rebuild-loop guess, confirmed F1 as the transient cost,
   and identified the 216 `saveLayer`s as the GPU killer.
2. **M1 — O6 (folder-aware) *first*.** `saveLayer` elision for plain layers +
   folders, and viewport-cull the layer/folder walk. Attacks the dominant GPU
   cost (the 216 `saveLayer`s) *and* the steady-state off-screen-scene symptom.
   Re-measure `saveLayers` / `draw ms` / `frame ms` on the transition repro.
3. **M2 — O1.** Bypass the window-cache offscreen round-trip during motion.
   Mops up the remaining full-window clear/alloc/blit once O6 has cut the
   `saveLayer`s.
4. **M3 — O5.** Defer mipmap upgrades until the camera settles (only if image
   pop / decode contention is visible after M1–M2).
5. **M4 — Particle budgeting (O-P1/2/3).** Land before particles go into real
   sequences, so the anticipated F5 hit never reaches the user.
6. **Demoted / parked:** O3, O4 (node-cache zoom reuse) — the B-series showed
   direct draws are cheap cached blits, so F2 is not the killer; do these only
   if a future zoom-heavy case proves otherwise. O2 (world-space window cache)
   remains the last resort.

---

## 8. Risks / explicitly not attempting

- **No single world-space window cache.** Per-layer alpha/blend makes it
  incorrect; only a per-layer version is viable, and only for pan (§4 O2).
- **No promise of unbounded particles.** We set and enforce a budget (O-P3).
- **Zoom/rotate during transitions** will always pay more than pan; O3/O4
  soften it but don't make it free.
- **`drawData.cam.c != windowCache.coords` is exact** — any sub-pixel camera
  jitter (e.g. from easing) counts as motion. That's fine for O1 (we *want* to
  bypass during motion) but means we must confirm the camera truly settles to
  an identical pose at rest so the at-rest fast path re-engages. Verify in M1.

---

## 9. Baseline measurements (M0)

All numbers are **before any fix** — captured on `perf/phase5.5-render` with the
M0 overlay, by zynx, against the real production file. Screenshot of record:
`docs/design/phase5.5-baseline-transition.png`.

### B-series — full reader-mode progression (rest → motion → rest)

Four captures across one section-to-section transition in reader mode
(screenshots `phase5.5-section*.png`). All at item count 1423.

| Capture | FPS | frame / draw / update / other (ms) | window rebuilt | BVH rebuilt | blits / direct / rebuilds | saveLayers | layers vis/in-view |
|---|---|---|---|---|---|---|---|
| `section1_start` (rest) | 60 | 16.8 / **0.2** / 0.0 / 16.5 | no | no | 0 / 0 / 0 | 0 | 0 / 0 |
| `section1_before` (rest) | 62 | 16.8 / **0.2** / 0.1 / 16.5 | no | no | 0 / 0 / 0 | 0 | 0 / 0 |
| **`section2_transition`** | **2** | **549 / 259 / 0.0 / 290** | **YES (F1)** | **no** | 9 / 216 / 2 | **216** | 12 / 9 (waste 3) |
| `section2_settle` (rest) | 59 | 16.9 / **0.2** / 0.1 / 16.7 | no | no | 0 / 0 / 0 | 0 | 0 / 0 |

### What the progression proves

1. **The slowdown is 100% transient.** At rest — *both before and after* — the
   draw is **0.2 ms** with the window cache served as a single blit (window
   rebuilt = no, all counters 0), 60 fps vsync-capped. Section 2 is *not* a
   heavier scene; arriving there is fine. The entire cost exists only while the
   camera is moving. So the target is unambiguously the motion path, and any fix
   that's free at rest costs us nothing.
2. **The rebuild-loop hypothesis is dead.** `update = 0.0 ms` and
   `BVH rebuilt = no` during the worst frame. The earlier B1 guess that ~260 ms
   lived in `update()`/`rebuild_cache` is **refuted**.
3. **The ~290 ms "other" is the GPU executing the recomposite**, not a separate
   mystery cost. The frame splits 259 ms CPU (recording the draw) + 290 ms
   "other" (GPU `flushAndSubmit` + swap of those commands). Both halves are the
   *same* work: the full-window recomposite that fires every frame because the
   window cache is keyed on exact camera pose (**F1 confirmed as the dominant
   cost** — rest 0.2 ms → motion 259 ms CPU + ~290 ms GPU is entirely the
   recomposite).
4. **The GPU killer is the 216 `saveLayer`s.** `saveLayer` is among the most
   expensive Skia GPU ops (allocate an offscreen render target + blend on
   restore). 216/frame ≈ the 290 ms "other". By contrast the 216 direct draws
   are cached-`SkImage` blits (≈one textured quad each) — cheap. So the lever is
   **eliminating `saveLayer`s, not reducing direct draws.**
5. **The 216 `saveLayer`s vs 12 visible leaf layers points back at F7 — at the
   *folder* level.** `recursive_draw_layer_item_to_canvas` opens a `saveLayer`
   for every visible folder *and* leaf, and it is **not viewport-culled** — it
   descends through every visible folder in the whole tree regardless of whether
   that folder's content is on screen. With the sections organized as folders,
   the off-screen sections' folders each still pay a `saveLayer` during the
   recomposite. (This is the same mechanism behind the earlier "hiding the old
   layers fixed it" observation, now showing up during transitions too.) The
   leaf-only `waste` metric (3) undercounts it because it doesn't count folder
   `saveLayer`s.

### Revised fix order (grounded in the B-series)

- **O6 first, extended to folders.** Cut the 216 `saveLayer`s via (a)
  `saveLayer` elision for plain (alpha 1.0 + normal-blend) layers **and
  folders**, and (b) viewport-cull the layer/folder walk so off-screen sections'
  folders are skipped entirely. This attacks the dominant GPU cost (290 ms) and
  the bulk of the CPU recording, and it also fixes the separate steady-state
  symptom (Trigger B). Measure `saveLayers` and `draw ms` drop directly in the
  existing overlay.
- **O1 second.** Skip the window-cache offscreen round-trip during motion (draw
  node caches + in-view layers straight to screen). Removes the remaining
  full-window clear + alloc + blit. With O6 already cutting the `saveLayer`s,
  this mops up the recomposite overhead.
- **O4 (node-cache zoom reuse) demoted.** Direct draws are cheap (cached blits),
  so F2 is *not* the killer here — O4 is no longer needed for this symptom.

No further instrumentation round-trip is needed: O6 and O1 will show up as the
`saveLayers`, `draw ms`, and `frame ms` numbers dropping on the same repro.
- **Per-layer union-AABB correctness (O6)** — if the cached layer bounds drift
  out of sync with add/erase/move/flatten/undo, off-screen content could be
  wrongly culled (vanishing strokes). Maintain it incrementally with the same
  rigor as component `worldAABB`, and validate against transforms + undo.
- **`saveLayer` elision (O6)** must test alpha *and* blend (and any future
  layer-level effect); never drop an isolation buffer that's load-bearing, or
  blend modes render wrong.
