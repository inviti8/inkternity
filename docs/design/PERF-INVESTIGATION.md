# Performance investigation — sluggishness with heavy libmypaint drawing

## Status

**Investigation, pre-implementation.** Reported by zynx after heavier
drawing testing: app gets very sluggish after a few minutes of use
once a substantial number of strokes are on the canvas, primarily
when drawing with the custom (libmypaint) brushes.

This doc is the code-analysis pass + ranked hypotheses + proposed
diagnostic harness. No code changes yet — those come after the
harness lands and confirms / refutes which hypotheses are the real
culprits.

## Update (2026-06-28) — layer-panel responsiveness pass (shipped)

Follow-up report from zynx: on a ~20-layer/3-group production file the
**layer editor goes unresponsive** after drawing a few hundred strokes on a
new layer (can't hide/delete layers), and the **device fan spins while
drawing**. Two load-bearing causes found in the cache draw path and fixed
(no harness needed — the code confirmed the hypotheses directly):

1. **`DrawingProgramCache::recursive_draw_layer_item_to_canvas` re-scanned the
   whole uncached set + global `unsortedComponents` once PER leaf layer**
   (`parentLayer == &layerListItem` filter) → **O(layers × components) every
   frame**. This is the sustained CPU cost (heat/fan) and the low frame-rate
   that made the layer panel feel dead. Fixed: bucket directly-drawn components
   by `parentLayer` **once per walk** in `draw_components_to_canvas`; each leaf
   draws only its own bucket. Each component is in exactly one place (one BVH
   node or unsorted), so buckets have no duplicates.
2. **Hide / alpha / blend / layer-delete called `clear_own_cached_surfaces()`**
   — nuking *every* cached node surface and forcing a whole-canvas re-rasterize
   next frame (the "can't hide/delete" hitch; also a full rebuild per alpha-
   slider tick). Fixed: those paths now call `invalidate_layer_footprint()`
   (made folder-recursive) so only the affected layer's region is invalidated.
   Set/changes to parallax depth/anchor still full-clear (footprint moves).

**Parallax-safe:** a parallax-active canvas fully bypasses the cache
(`DrawingProgram::draw` → direct tree walk when `any_visible_parallax_layer()`),
so the bucketing never runs there; footprint invalidation uses base bounds,
correct because the cache is only live when all layers sit at depth 0.

**Not done (deliberately):** the layer-delete **undo snapshot** still deep-
copies every component (`get_undo_data` → `get_data_copy`) synchronously — it
must run before the erase on live, non-thread-safe state, so it can't be made
async without an ownership-transfer rework of the netID-based undo system.
Cheap for vector strokes; the real delete hitch was the cache nuke (now gone).
Revisit if raster-heavy layer deletes still stall.

**Load feedback:** added `LoadingScreen` — a one-frame "Loading…" interstitial
shown before the (synchronous, main-thread) file load so opening a large canvas
no longer looks like a frozen window. Disk-path opens only (a buffer-backed
open carries a `string_view` that can't be deferred).

## Update (2026-06-30) — incremental BVH insertion (shipped)

Follow-up from zynx after deeper production-file testing: the app still went
**intermittently unresponsive during drawing and undo**, worsening as a layer
filled — and lowering `MINIMUM_COMPONENTS_TO_START_REBUILD` (150→300→500) only
**spaced the hangs out**, which isolated the cause: the periodic **full BVH
rebuild** (`DrawingProgram::update` → `rebuild_cache` → `internal_build`).

Root cause: `internal_build` calls **`clear_own_cached_surfaces()`**, which drops
*every* cached node surface and detaches the window cache. The next frame then
re-rasterizes the entire visible canvas in one shot (`refresh_all_draw_cache` +
`window_cache_complete_refresh`) — a content-proportional, one-frame stall that
fired every ~N strokes. Crucially, the rebuild **changes no pixels**; it only
reorganizes which node owns which stroke, so nuking valid cached pixels is pure
waste.

**Fix — `absorb_unsorted_incrementally()`:** fold accumulated `unsortedComponents`
into the *existing* BVH instead of rebuilding. Each stroke is inserted into the
**deepest existing node whose bounds already fully contain it**
(`find_deepest_containing_node` + `fully_contains_aabb`), so no node's
`bounds`/`coords`/`resolution` change → every cached surface stays valid. And
because an unsorted stroke is already composited into those caches as a direct
draw, the move is **structural only — zero re-rasterization**. A fat leaf is
re-split in place (`split_leaf_in_place`) with its own bounds held fixed, so the
node's cache (keyed by its `shared_ptr`) survives the split while the tree stays
balanced. `update()` tries absorption first and only falls back to the full
`rebuild_cache()` when it can't drain the backlog — i.e. an empty-tree bootstrap
or a batch drawn **beyond the current canvas extent** (rare; that path grows the
root bounds, which the incremental path deliberately won't do).

**Net:** the whole-canvas re-raster no longer fires during normal in-extent
drawing (or erasing — same rebuild path), only on genuine canvas expansion.
`MINIMUM_COMPONENTS_TO_START_REBUILD` now just paces the *cheap* absorption, so
it's far less sensitive. **Follow-up if needed:** grow root bounds incrementally
so frontier/expansion drawing also avoids the full rebuild.

## What happens when an artist paints a libmypaint stroke

Single stroke lifecycle (`MyPaintBrushTool::begin_stroke` →
`continue_stroke` × N motion events → `end_stroke`):

```
begin_stroke (pen-down):
  - new CanvasComponentContainer wrapping a new MyPaintLayerCanvasComponent
  - new LibMyPaintSkiaSurface (empty tile dict)
  - mypaint_surface_begin_atomic/draw_dab/end_atomic for first dab
  - layer.mark_dirty()
  - container.commit_update(drawP)
      → preupdate_component (×2 — commit_update calls it twice)
        → invalidate cache at stroke bounds (currently tiny)
        → pull component from BVH to unsortedComponents

continue_stroke (per pen-motion event, ~60 Hz during active draw):
  - libmypaint paints more dabs into the surface (allocates new tiles
    on first touch — tiles never freed for the life of the component)
  - layer.record_stroke_sample(...)  (PHASE2 M1 recording: 12 bytes/sample)
  - container.commit_update(drawP)
      → preupdate_component (×2 AGAIN)
        → invalidate cache at NEW bounds (growing as the stroke extends)
        → re-pull component from BVH to unsortedComponents (no-op if already there)

end_stroke (pen-up):
  - container.commit_update(drawP)
  - container.send_comp_update(drawP, finalUpdate=true)
      → DelayedUpdateObjectManager → wire-serialize the entire surface
        (all tiles, raw 16bpc bytes — 32 KiB per tile) and broadcast to peers
```

Per-frame draw of a single MyPaintLayer component
(`MyPaintLayerCanvasComponent::draw`):

```
- Allocate fresh SkBitmap sized to the stroke's bounding box (heap alloc)
- bmp.eraseARGB(0,0,0,0)
- LibMyPaintSkiaSurface::composite_to_bitmap:
    - iterate every allocated tile in the dict
    - per pixel in tile: 16bpc-premul → 8bpc-unpremul conversion
      (3 integer divides per pixel for color channels)
- bmp.asImage() → SkImage (potentially uploads to GPU)
- canvas->drawImage(...)
```

## Findings — ranked by likelihood

### Tier 1: Almost-certainly-load-bearing

**1. `MINIMUM_COMPONENTS_TO_START_REBUILD = 1000` keeps every raster
stroke uncached for ~5–15 minutes of drawing.**

`DrawingProgramCache::should_rebuild()` returns false until
`unsortedComponents.size() >= 1000`. The BVH + per-node cached
SkSurfaces only get built once that threshold trips. Every component
in `unsortedComponents` is rendered directly each frame via the
component's `draw()` — for MyPaintLayer that's the
allocate-bitmap-and-composite path described above. There IS a
fallback trigger (`check_rebuild_needed_from_framerate` — sustained
<30 FPS for 5s + unsorted objects for 5s), but it requires
**five sustained seconds of bad framerate before kicking** and even
then the rebuild itself is expensive.

The threshold was probably tuned for vector strokes, where direct
per-frame `drawPath` is cheap (Skia caches paths on GPU). For raster
strokes the per-frame cost per uncached component is much higher,
and the same threshold lets 1000 raster strokes pile up before any
caching benefit kicks in. At ~60–100 strokes/minute of active
drawing, that's 10–17 minutes of monotonically-degrading per-frame
draw cost.

This is the most likely primary cause of "sluggishness after a few
minutes."

**2. `MyPaintLayerCanvasComponent::draw` allocates a CPU SkBitmap +
walks every tile every frame.**

The composite path (`composite_to_bitmap`) iterates the tile dict
and copies pixel-by-pixel from 16bpc-premul to 8bpc-unpremul with 3
integer divides per pixel for color. For a stroke that touched a 4×4
tile grid (256×256 pixels), that's 65K pixels × 3 divides = ~200K
divides per stroke per frame, plus the `bmp.tryAllocPixels` heap
allocation and the `asImage()` → GPU upload.

Compounded with finding #1 (many strokes uncached for a long time),
this is the dominant per-frame cost when the canvas has many
strokes visible.

### Tier 2: Real contributors, secondary

**3. `commit_update` calls `preupdate_component` twice per
invocation.**

```cpp
void CanvasComponentContainer::commit_update(DrawingProgram& drawP) {
    drawP.preupdate_component(&(*objInfo));
    get_comp().initialize_draw_data(drawP);
    calculate_world_bounds();
    drawP.preupdate_component(&(*objInfo));   // <-- second call
}
```

Each `preupdate_component` invalidates all cache nodes overlapping
the component's bounds AND pulls the component out of its BVH node
into `unsortedComponents`. Doing this twice per mutation doubles the
cache-invalidation work. With `continue_stroke` firing
`commit_update` on every pen-motion event (~60 Hz active), the
cache thrash is significant.

Almost certainly historical — the second call probably exists to
catch state that changed between the two preupdate calls and isn't
needed anymore. Worth verifying with a quick test (remove one, see
if anything regresses) before patching.

**4. Eraser fires `send_comp_update(false)` per pen-motion segment
(rc4) — full tile data wire-serialize per segment.**

The DelayedUpdateObjectManager batches these, but each batched
broadcast still serializes the ENTIRE tile set of the modified
component. For an active eraser dragging across a stroke that
touches 16 tiles (512 KiB raw), each batch flush is a 512 KiB wire
payload. At 60 Hz pen events, even with batching, this is
substantial CPU + network work happening on the main thread.

The brush itself only broadcasts once (at `end_stroke`,
`finalUpdate=true`), so this is eraser-specific.

**5. Tile memory monotonic growth.**

`LibMyPaintSkiaSurface::on_tile_request_start` allocates tiles
lazily on first request and **never frees them**. A tile that was
once painted then fully erased still occupies its 32 KiB buffer.
Worse, any READ to an unallocated tile (e.g. libmypaint's
smudge/speed sampling) allocates the tile too. Over a long drawing
session, total RAM grows monotonically — gradually exhausting cache
locality and (eventually) physical memory.

Each stroke is its own component with its own surface, so
overlapping strokes don't share tiles either — N strokes covering
the same region pay N × tile-set memory.

### Tier 3: Possible but lower likelihood

**6. `recordedSamples_` (PHASE2 M1 stroke recording) growth.**

Each SHARP-category stroke records every pen-motion sample
(~12 bytes per sample, ~60 samples/sec during active drawing).
Long strokes can accumulate ~10–50 KB; many long strokes accumulate
into MBs of recorded-sample memory across the canvas. Not the
biggest cost, but reload-from-disk + wire-serialize amplify it.

**7. BVH rebuild cost when finally triggered.**

When `should_rebuild()` or the framerate-based fallback finally
fires, `internal_build` runs a full BVH rebuild — sorting
components, partitioning, allocating new cache surfaces. With many
hundreds of strokes pending, this is a noticeable frame hitch (and
may itself cause the next-frame framerate measurement to look bad,
potentially triggering ANOTHER rebuild a few seconds later).

**8. SkBitmap → SkImage GPU upload per frame per stroke.**

`bmp.asImage()` followed by `canvas->drawImage()` likely uploads the
image to GPU on first use. Without a persistent GPU cache for these
images, each frame re-uploads. For many visible MyPaintLayer
components, the cumulative GPU upload bandwidth becomes significant.

## Proposed diagnostic harness

To validate which findings actually dominate, ship a small
overhead-light measurement layer the artist can toggle on. All
metrics rendered in a dev-only overlay on the canvas. No external
deps; just per-frame counters + a small ring buffer.

### Metrics to capture

**Frame-time:**
- Total frame time (already tracked at `mS.lastRenderTimePoint`)
- Breakdown: time spent in `MainProgram::update`, time spent in
  `regular_draw` / `DrawingProgram::draw` / cache build / per-component
  draw loop
- Per-component-type draw time aggregate (vector vs raster vs other)

**Component / memory counters:**
- Total components in the canvas
- Components in BVH vs unsorted (key for finding #1)
- Total allocated tiles across all MyPaintLayer components
- Approximate raster memory (tile_count × 32 KiB)
- Number of cached draw surfaces in use vs `MAXIMUM_DRAW_CACHE_SURFACES`

**Cache-thrash counters:**
- Cache invalidations per second
- Cache hits vs misses per frame (count of components rendered
  direct vs served from cache)
- Time since last full BVH rebuild

**Wire-broadcast counter (for the eraser-amplification hypothesis):**
- Bytes/sec sent via NetObj on each channel
- `send_comp_update` invocations per second, broken down by component
  type and `finalUpdate` flag

**Allocation counters:**
- `LibMyPaintSkiaSurface::on_tile_request_start` allocations per
  second (new tile vs cached lookup)

### Surface shape

Toggle via a hidden dev key (e.g. F9, mirroring existing F-key
debug toggles if any exist) or a Settings → Debug "Show perf
overlay" checkbox. When on, render a small fixed-position panel in
a corner of the canvas with the live counters + a 60-frame
ring-buffer chart of frame-time.

No persistence required — this is a live observation tool for
debugging sessions, not a profiling-data-collection-and-export tool.
If we need persistent traces later, that's a separate effort.

### Surfaces touched

```
NEW src/Diagnostics/PerfOverlay.{hpp,cpp}
                                    state container + ring buffer +
                                    per-frame snapshot + overlay GUI
                                    rendering. Singleton or owned by
                                    MainProgram.

src/MainProgram.cpp::update         per-frame snapshot capture: total
                                    frame time, breakdown of update/
                                    draw phases.

src/DrawingProgram/DrawingProgram.cpp::draw
                                    instrument the per-component
                                    draw loop: count cache-hits-vs-
                                    direct, accumulate per-type time.

src/DrawingProgram/DrawingProgramCache.cpp
                                    counter increments on:
                                    invalidate_cache_at_aabb,
                                    internal_build, cache surface
                                    creation/destruction.

src/Brushes/LibMyPaintSkiaSurface.cpp::on_tile_request_start
                                    increment a tile-alloc counter
                                    (new-vs-cached) and total alloc
                                    bytes.

src/Helpers/NetworkingObjects/DelayUpdateSerializedClassManager.cpp
                                    increment per-component-type
                                    bytes-broadcast counter at every
                                    flush.

NEW src/Toolbar/PerfOverlayToggle (or settings entry)
                                    UI to toggle the overlay on/off.
```

### Estimated effort

~1 day to wire all the counters + render the overlay. No new deps.
Behind a compile-time + runtime guard so production builds aren't
paying any cost when the overlay is off.

## Quick-win fixes (if findings hold)

Once the harness confirms the hypotheses, the cheapest mitigations
to test, in order of likely impact:

1. **Lower `MINIMUM_COMPONENTS_TO_START_REBUILD`.** ✅ **DONE — now
   300.** Full history lives in the comment at the constant's
   definition (`DrawingProgramCache.cpp`). Short version: it went
   1000 → 100 (helped raster draw, but caused eraser rebuild thrash)
   → back to 1000 (eraser fixed, but heavy crosshatch files stayed
   sluggish too long) → **300**, validated live against a heavy
   crosshatch file: draw responsiveness restored, eraser regression
   did not return. A type-aware threshold (low for raster, high for
   vector) was considered but not needed — 300 holds for both. Note
   the eraser concern is raster-specific, so a raster-aware split
   would NOT have dodged it.
2. **Cache the composited SkImage per MyPaintLayerCanvasComponent**
   ✅ **DONE** — `MyPaintLayerCanvasComponent::draw` now builds the
   bitmap + SkImage once per surface mutation and reuses it until
   `mark_dirty()`. This is what made (1) safe to tune for the
   node-cache-kick-in tradeoff rather than for per-frame draw cost.
3. **Remove the duplicate `preupdate_component` call in
   `commit_update`** — pending verification.
4. **Eraser: defer `send_comp_update` to `pen-up` instead of
   per-segment** — match the brush's broadcast cadence. Mid-stroke
   sync is a nice-to-have, not load-bearing.

## Bigger architectural fixes (if quick wins aren't enough)

1. **Single shared raster surface per layer** instead of per-stroke
   surfaces. Reduces tile duplication for overlapping strokes,
   reduces per-frame component count drastically. Requires the
   stroke-recording log to live separately so StrokeVectorize and
   per-stroke ops still work.
   ✅ SHIPPED (manual variant): **Flatten Layer (View)** — the artist
   triggers the merge per-region instead of the engine maintaining one
   surface per layer. Initially ink-only (commit 691a2f5), generalized
   to every visual component type with a WYSIWYG resolution floor and
   a configurable bake cap in PHASE4 Part B (docs/design/PHASE4.md
   §10). Also the designated perf mitigation for PHASE4 parallax
   layers, which bypass the draw cache while active.
2. **Tile eviction policy** — drop tile buffers for components
   off-screen for >N seconds; rematerialize on demand from a
   compressed cold-storage (e.g. PNG-encoded tile data sitting in
   memory at ~1/10 the size).
3. **Render to a persistent GPU texture per stroke** instead of
   per-frame CPU bitmap + upload.

These are non-trivial; defer until quick wins are proven insufficient.

## Plan

1. **Ship the diagnostic harness first.** Land the counters +
   overlay; user runs a fresh sluggishness-reproduction with the
   overlay on; share a screenshot or a snapshot of the metrics
   when the slowdown is reproducible.
2. **Interpret the harness output** to confirm which Tier 1/2/3
   findings are dominating.
3. **Apply quick-win fixes** for the dominant culprits, in order
   of impact-per-LOC.
4. **Re-measure** with the overlay; iterate.
5. **Decide on architectural changes** only if quick wins fall
   short of an acceptable steady-state framerate.

No code changes from this commit. Next commit: the harness.
