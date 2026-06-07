# PHASE4 — Multiplane Parallax + Generalized Flatten

Status: DESIGN — not started
Prereqs: PHASE2 layer system, PHASE3, raster flatten (commit 691a2f5)

Two related layer-power features:
- **Part A — Multiplane parallax**: per-layer depth, camera pans produce
  parallax (§1–§9).
- **Part B — Generalized flatten**: "Flatten Ink (View)" grows into
  "Flatten Layer (View)" and rasterizes vector content too (§10).
  Part B also feeds Part A: flatten is the documented perf mitigation
  for raster-heavy parallax layers (§5), and after Part B it covers
  vector-heavy layers as well.

# Part A — Multiplane Parallax Layers

## 1. Goal

Give layers a **depth** property so that panning the camera produces
parallax: near layers slide past quickly, far layers crawl. An artist
draws a cloud, pushes its layer "back", draws another cloud on a nearer
layer, and panning the canvas now *feels* like moving through a sky.

This is the multiplane-camera technique (Disney 1937, After Effects
"2.5D"): the scene stays a stack of flat 2D planes; only the camera
math changes. No 3D engine, no projection, no z-buffer.

## 2. Explicitly out of scope (and why)

These were considered and deliberately deferred — do not bolt them onto
this phase without a new design doc:

- **Dolly / fly-through** ("clouds pass behind you"). Requires layers to
  cross the camera plane (scale → ∞, fade-out handling, draw-order
  re-sorting per frame) and is *bounded* navigation — it ends at the
  farthest layer — which sits awkwardly next to infinite zoom. Possible
  later as a distinct gesture (PHASE4b), never as a replacement for zoom.
- **3D plane poses** (rotate a layer 90° → road with perspective).
  Renderable in Skia via `SkM44` homographies, but drags in a second
  camera model, painter's-algorithm sorting limits for intersecting
  planes, raster-only export of 3D views. PHASE4c candidate.
- **Editing while perspective-transformed.** Screen-space brush strokes
  map to wildly non-uniform world sizes on a tilted plane, and the whole
  input/hit-test pipeline assumes invertible similarity transforms.
  Editing stays 2D, period.

## 3. The math (and why infinite zoom is safe)

The engine's zoom is a *focal-length* zoom (uniform magnification via
`cam.c.inverseScale`), and a focal zoom produces **zero parallax** by
definition — magnifying a scene magnifies all depths identically.
Parallax comes only from camera *translation*. So:

- **Zoom is untouched.** All layers magnify together, exactly as today,
  with `WorldScalar` exactness and no bound. The parallax *ratios*
  between layers depend only on their depths, not on zoom level.
- **Pan gets per-layer factors.** A layer at depth `d` responds to
  camera translation with factor `f(d) = 1 / (1 + d)`:
  - `d = 0` → factor 1 → behaves exactly like today (default).
  - `d > 0` → farther → slides less.
  - `-1 < d < 0` → nearer → slides more (foreground overlays).
  - `d` is clamped to `(-0.95, 1000)`; `d <= -1` is meaningless
    (at/behind the camera plane).

### Derived per-layer camera

For a layer with depth `d` and **anchor** `A` (a `WorldVec`, see below),
the layer is rendered with a derived camera:

```
derived.pos          = A + (cam.pos - A) * f(d)
derived.inverseScale = cam.inverseScale          // zoom: no parallax
derived.rotation     = cam.rotation              // rotation about the
                                                 // camera is uniform too
```

`f(d)` is a bounded double; scaling a `WorldVec` delta by it is ordinary
`FixedPoint` arithmetic — no precision risk.

### The anchor (no-jump rule)

Without an anchor, editing a layer's depth would make its content jump
on screen (the parallax offset changes instantly). Rule: **when the
artist edits depth, recompute `A` so the layer's current on-screen
position is preserved** — solve `A_new` from
`A_new + (cam.pos − A_new)·f(d_new) = A_old + (cam.pos − A_old)·f(d_old)`.
The layer then *behaves* differently as the camera moves, but nothing
teleports at the moment of the edit. `A` is per-layer state, synced and
saved alongside depth, and must be transformed by
`DrawingProgramLayerManager::scale_up` (depth itself is dimensionless —
scale_up leaves it alone).

## 4. Data model

Depth + anchor join the existing per-layer synced display state:

- `DrawingProgramLayerListItem::DisplayData`
  (src/DrawingProgram/Layers/DrawingProgramLayerListItem.hpp:85-92):
  add `float parallaxDepth = 0.0f;` and `WorldVec parallaxAnchor{0,0};`
  with `set_depth(...)` / `get_depth()` mirroring the
  `set_alpha`/`set_blend_mode` pattern (NetObj
  `send_update_to_all<DisplayData>`).
- `DrawingProgramLayerListItemMetaInfo` (+ undo data): add the same two
  fields so depth edits are undoable like alpha/blend edits.
- **File format**: gate loading on a new `VersionNumber` (follow the
  `LayerKind` precedent — "file-saved gated at format version >= 0.8",
  DrawingProgramLayerListItem.hpp:98). Old files load with depth 0
  everywhere → identical behavior.
- **Wire format**: extending `DisplayData::serialize` changes the wire
  shape; mixed-version collab sessions already require matching builds,
  but call it out in the release notes.
- Folders: depth on a folder applies to its whole subtree (the derived
  camera is pushed during the tree walk; nested depths do **not**
  compose in v1 — innermost non-zero wins, keep it simple).

## 5. Rendering integration

Two draw paths exist today:

1. **Live frame** — `DrawingProgram::draw` →
   `drawCache.update_and_draw_cached_canvas`
   (src/DrawingProgram/DrawingProgramCache.cpp:419). The window cache is
   already fully refreshed whenever `drawData.cam.c` changes (line 425),
   so per-frame parallax does not add a new invalidation class there.
   Inside, `draw_components_to_canvas` (line 436) splits work into
   cached BVH node images + an uncached per-layer tree walk
   (`recursive_draw_layer_item_to_canvas`, line 466) that already
   applies per-layer alpha/blend via `saveLayer`.
2. **Screenshot/export** — `layerMan.draw` walks the layer tree directly
   with one shared `DrawData`.

Plan:

- **Depth-0 layers: zero change.** They keep the full BVH node-cache
  fast path. A canvas with no depths assigned renders bit-identically
  through the code it uses today.
- **Depth≠0 layers bypass the BVH node cache** and render through the
  uncached per-layer walk with a per-layer `DrawData` whose `cam.c` is
  the derived camera (recompute `refresh_draw_optimizing_values()` per
  distinct depth, not per component). Cached BVH node surfaces are
  cross-layer composites anchored in world space — they are *wrong* the
  moment two layers move relative to each other, so they must exclude
  depth≠0 layers at `internal_build` time (partition by
  `parentLayer->get_depth() != 0`).
- **Perf expectation, stated honestly:** a parallaxed layer redraws its
  components every frame while panning (same cost class as the
  pre-cache path). For vector strokes this is fine; for raster-heavy
  ink layers the answer is **flatten first** (the PHASE-perf flatten
  collapses a layer to one component — one image blit per frame).
  If profiling shows it matters, the follow-up is a per-depth-layer
  cached surface (each depth group gets its own window-sized surface,
  refreshed on its own derived-camera change) — design it then, not now.
- **Exports work because parallax is affine.** A parallax offset is a
  pure per-layer translation+scale — *not* projective — so screenshots
  AND SVG export both support it: give each layer its derived camera in
  the `layerMan.draw` walk. (This is a real advantage of stopping short
  of 3D poses.)

## 6. Editing policy

Tools already scope to the layer being edited (brush, eraser, vectorize,
flatten — PHASE2/aeadc4e). Policy:

- **M1 (ship first): editing requires depth 0.** Selecting a depth≠0
  layer as the edit target shows a USERINFO toast: "This layer has
  parallax depth — set depth to 0 to edit it." Cheap, honest, no
  half-working input paths. (Per the standing rule: no code that
  attempts something we know is broken.)
- **M2: remap input through the editing layer's derived camera.** All
  editing-layer-scoped tools consume coordinates via one substitution
  point (the tool pipeline's `DrawData`/camera access), so drawing *on*
  a parallaxed layer lands where the cursor shows. Cross-layer
  operations (rect/lasso selection across depths) remain depth-0-only —
  a screen rect is not a single world rect when layers disagree on
  the camera.

## 7. UI

Layer side panel (`DrawingProgramLayerManagerGUI`, alongside the
existing name/alpha/blend editors): a "Depth" numeric field + slider,
range −0.9 … +10 (log-feel steps), default 0, with a reset-to-0 button.
Show a small depth badge on layer rows with non-zero depth so it's
obvious why a layer pans "wrong". Top-bar: nothing changes; the camera
controls are untouched.

## 8. Milestones

- **M1 — Data + UI:** DisplayData/metaInfo/undo fields, file version
  gate, net sync, depth field in the layer panel. No render change yet
  (depth visible but inert). Smallest reviewable step.
- **M2 — Parallax render:** BVH partition (depth≠0 excluded from node
  caches), derived-camera tree walk in both the live path and
  `layerMan.draw`, anchor no-jump math. Editing locked to depth-0.
- **M3 — Exports:** screenshot + SVG honor depth (mostly falls out of
  M2's `layerMan.draw` change; verify both).
- **M4 — Edit-at-depth:** input remap through the derived camera for
  editing-layer-scoped tools.
- **Later (separate docs):** per-depth cached surfaces (if profiling
  demands), dolly fly-through (PHASE4b), 3D plane poses (PHASE4c).

## 9. Risks / open questions

- **Raster-heavy parallax layers** are the perf cliff (see §5); flatten
  is the documented mitigation until per-depth caching exists.
- **Selection semantics** across depths (M2 locks it to depth-0; revisit
  in M4).
- **Waypoints / canvas-position shares** store camera coords; a stored
  view reproduces parallax correctly (derived cams are a pure function
  of camera + layer state), no change needed — verify in M2.
- **Reader mode / frame anim** interactions untested — both consume the
  same draw paths, expected to inherit parallax for free, verify.
- Depth on the named SKETCH/COLOR/INK layers is allowed (it's just a
  layer property) but the default workflow shouldn't push it — depth
  shines on DEFAULT user layers ("Cloud near", "Cloud far").

# Part B — Generalized Flatten (rasterize vector content)

## 10. Flatten Layer (View)

### Current state

`RasterFlatten::flatten_ink_strokes_in_view`
(src/DrawingProgram/RasterFlatten.cpp:75) collects **only
`MYPAINTLAYER`** components. Pen lines (`BRUSHSTROKE`), `RECTANGLE`,
`ELLIPSE`, `TEXTBOX`, and `IMAGE` components in the same view are
skipped — they stay as individual components, and the merged raster is
anchored at the highest source z, which is where the documented
"z-order collapses if non-raster components are interleaved" caveat
comes from.

### Goal

Rename the menu action to **"Flatten Layer (View)"** and include every
*visual* component type on the editing layer that intersects the view.
This (a) lets vector-heavy layers be collapsed for perf exactly like
ink-heavy ones, and (b) **eliminates the z-order caveat** — when
everything in view on the layer merges, nothing is left to interleave
incorrectly.

### What's included

| Type | Flattened? | Notes |
|---|---|---|
| MYPAINTLAYER | yes (today) | no resolution loss — already raster |
| BRUSHSTROKE | yes (new) | loses infinite-zoom sharpness — see resolution rule |
| RECTANGLE / ELLIPSE | yes (new) | same |
| TEXTBOX | yes (new) | loses *text editability* — acceptable: flatten is explicitly destructive-with-undo; excluding it would resurrect the z-order caveat |
| IMAGE | yes (new) | may downsample if the image's native scale is finer than the bake scale — counted in the resolution rule below |
| WAYPOINT | **never** | functional marker, not artwork; rasterizing the pin would orphan it from `wpGraph` |

The render step needs no per-type work —
`container.draw_with_predraw_data(canvas, dd, ...)` is already
type-agnostic; only the collection filter changes (skip-list:
`WAYPOINT`).

### Resolution rule (the one real design decision)

Today's rule — bake at the finest **source stroke's** draw-time scale —
is correct for raster sources (they have a native pixel grid) but
wrong for vectors: a vector line has *no* native scale and stays sharp
at any zoom, so baking it at its draw-time scale can visibly soften it
if the artist has since zoomed in. New rule:

```
targetInv = finest of:
  - every raster source's coords.inverseScale   (MYPAINTLAYER, IMAGE native scale)
  - the CURRENT camera's inverseScale           (WYSIWYG floor for vectors)
capped by kMaxFlattenDim = 8192 per axis (downsample + toast, as today)
```

i.e. the bake is always at least as sharp as what's on screen right
now. Zoom in before flattening = more detail preserved, which is an
intuitive, explainable knob for the artist. Deliberate, accepted cost
(mirror of the existing header comment): flattened vectors no longer
survive infinite zoom-in — that is what "rasterize" means, and undo
(place + erase, two steps) restores the originals.

### Bake size cap & mixed-scale content

The flatten bake is NOT tied to the screenshot tool's resolution — it
derives from source scales as above, then hits `kMaxFlattenDim = 8192`
per axis (RasterFlatten.cpp:36). That cap is where detail loss actually
happens, and the worst case is **mixed-scale content**: draw a small
element zoomed-in (fine pixel grid), zoom way out, draw more, flatten —
the merged region measured in the fine element's pixels blows past
8192, the whole bake coarsens to fit, and the small element eats the
loss (this is the "region large — baked at reduced resolution" toast).

Two-step answer:

1. **M0 — settings knob.** "Maximum flatten size (px per axis)" in
   Settings → **General** (it's a production setting — it directly
   controls how much detail survives a flatten; zynx call, 2026-06-06),
   default 8192, range 2048–16384. Memory is why the cap exists and why
   the range is bounded: an 8192² bake is ~256 MB readback + up to
   ~512 MB of transient 16-bit tiles; 16384² quadruples that. State the
   cost in the setting's help text. Live-tunable static persisted in
   config.json (under the debug json section — UI placement and
   persistence key are independent; key kept stable so early-tester
   configs don't reset).
2. **Follow-up — scale-band partitioning (the principled fix).** Instead
   of one merged component at one scale, cluster sources by
   `coords.inverseScale` into bands (e.g. each band spans ≤ 2⁴× scale
   range) and emit one merged component per band, each baked at its own
   band's native resolution within the cap. The zoomed-in element merges
   with its fine-scale neighbors at full detail; the zoomed-out strokes
   merge separately; component count still collapses; nothing
   downsamples until a single band alone exceeds the cap. Slightly
   weaker compaction (k components instead of 1), no detail loss.
   Z-order note: bands interleave in z the same way today's raster/
   vector split does — anchor each band's merged component at its own
   highest source z, accept the (much rarer) interleave caveat, and
   surface it in the toast when bands > 1.

### Unchanged semantics

- Scope: layer being edited; view AABB collision; ≥ 2 included
  components or no-op with toast.
- Result: one merged `MYPAINTLAYER` component (raster-erasable;
  vectorize correctly skips it via `strokeRecordingValid_ == false`).
- Anchor at highest source z; place-then-erase undo.

### Milestone

- **M0 (can ship before/independently of Part A):** collection filter →
  skip-list, resolution rule above, "Maximum flatten size" settings
  knob, menu rename, toolbox help-text update. Touches
  `RasterFlatten.cpp/.hpp`, the Toolbar menu entry, and the settings
  GUI. Verify: flatten a view containing ink + pen lines + a textbox +
  an image; confirm single merged component, WYSIWYG sharpness at the
  flatten-time zoom, waypoints untouched, two-step undo restores all.
- **M0.5 (when mixed-scale canvases hit the cap in practice):**
  scale-band partitioning per §10's follow-up.
