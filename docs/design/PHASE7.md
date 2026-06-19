# PHASE 7 — Shape masks (Inkscape-style clip shapes)

## Status

**Design / scoping, pre-implementation.** Grew out of the PHASE6 "Select
Transparency" request. After scoping, the chosen model is a **shape mask**: a
drawn shape (rect / ellipse / polygon) flagged as a *mask* clips the rest of its
layer's content to the shape's interior — or, with **Invert**, to its exterior.
This is the **Inkscape "Clip" paradigm** (a shape defines the clip region), as
opposed to the Photoshop/Krita **clip-to-the-layer-below** model (kept as an
alternative in §"Alternatives").

Primary use case for the graphic novel: **panels** ("draw inside this box, art
outside is clipped") and region-masking — plus inverted masks to knock holes.

## Semantics

- Any **fillable vector shape** — rectangle, **polygon** (rect Polygon mode),
  ellipse (incl. skewed) — can be flagged **Use as mask** on its layer.
- A masked layer draws its **non-mask content clipped to the union of its mask
  shapes' interiors**. Per-mask **Invert** flips that shape's contribution
  (clip to its *exterior* / knock a hole).
  - Combined region = `union(non-inverted interiors)` **minus**
    `union(inverted interiors)`. If a layer has only inverted masks, the base is
    the whole layer minus those holes.
- A mask shape is **not drawn as artwork** (no fill/stroke render) — it only
  defines the region, like Inkscape consuming the clip object. It is still a
  normal, selectable, editable object (move/scale/skew/Add-Point all work), and
  shows a **distinct mask treatment** so it's visible while authoring (see
  §"Mask visual treatment").
- Masks are **per-layer** (a mask affects only the layer it's on). A layer with
  no mask shapes draws exactly as today.
- **Edge cases:** a layer whose *only* shapes are masks draws nothing (clipped
  region has no content). A mask shape off-screen still clips on-screen content
  (masks are gathered for the whole layer, not just the viewport).

## Data model

Add two flags to the shape components that can be masks — same append-and-gate
pattern as PHASE6's `polygonMode` / `affineMode`. The container only serializes
`coords` + delegates to the component (`CanvasComponentContainer.cpp:68`), so the
flags live in the shape `Data`:

```cpp
// RectangleCanvasComponent::Data and EllipseCanvasComponent::Data
bool isMask = false;
bool maskInvert = false;
```

- Append to each shape's `save`/`save_file`/`load`; `load_file` reads them only
  when `version >= <PHASE7 version>`. Old files → `isMask=false` → unchanged.
- Bump save format to **INFPNT000019 / 0.18.0**.
- Generic access for the compositor without RTTI: add virtuals on
  `CanvasComponent` (default no-op) overridden by Rectangle/Ellipse:
  ```cpp
  virtual bool is_mask() const { return false; }
  virtual bool is_mask_inverted() const { return false; }
  virtual std::optional<SkPath> get_mask_path() const { return std::nullopt; }
  ```
  `get_mask_path()` returns the shape's path in **component-local** space (rect's
  `rectPath` / polygon path / ellipse's `ellipsePath`); the compositor applies
  the component's draw transform to place it.

## Render / composite

All masking lives in `recursive_draw_layer_item_to_canvas`
(`DrawingProgramCache.cpp:466`) — the one layer-tree walk used by both the window
cache and the BVH node caches, so the mask is baked into the caches like any
other compositing.

Per visible **leaf** layer, before drawing its content:

1. **Gather the layer's mask shapes** (scan the layer's component list for
   `is_mask()` — *not* just the in-view draw set, since an off-screen mask still
   clips). Cache this list per layer, rebuilt when the layer's components change.
2. If any masks exist, build the clip in camera/draw space:
   - For each mask, transform `get_mask_path()` by the component's draw transform
     (`calculate_draw_transform`, same matrix used by `draw_with_predraw_data`).
   - `union(non-inverted)` via **SkPathOps** `Op(..., kUnion_SkPathOp, ...)`
     (or one SkPath with all contours under non-zero fill as a cheaper
     approximation). `canvas->clipPath(unionPath, kIntersect)`.
   - For each inverted mask: `canvas->clipPath(invPath, kDifference)`.
   - `saveLayer`/`save` around this so the clip is scoped to the layer.
3. Draw the layer's **non-mask** content (exclude `is_mask()` components from
   `compsToDraw`).
4. Restore the clip.

`clipPath` is the native, cheap primitive here — no offscreen base render needed
(the advantage of the shape-mask model over clip-to-below).

## Mask visual treatment (required)

Mask shapes aren't drawn as art, so the artist needs to see them. Render each
mask shape's outline as a **red dashed line** (`SkDashPathEffect`, ~2px, red)
so masks read as "special / non-printing."

- Drawn as an **author-mode overlay**, NOT baked content — i.e. in the
  non-cached overlay pass alongside selection/edit handles
  (`DrawingProgram::draw` after the window-cache blit), **not** inside
  `recursive_draw_layer_item_to_canvas`. This keeps the red line out of the draw
  cache, **flatten, screenshots/SVG export, and reader mode** (it's an editing
  aid only, like waypoint markers which hide in reader mode).
- Inverted masks get a visual tell too (e.g. a small hatch/“∅” affordance or a
  different dash), so invert state is legible at a glance.

## Flatten integration (required)

**Flatten Layer must collapse the mask into the rasterized image** — the baked
result shows the *clipped* artwork, and the mask shapes are consumed.
`RasterFlatten::flatten_layer` renders each source via `draw_with_predraw_data`
into an offscreen and reads it back (`RasterFlatten.cpp:180`), so:

1. **Exclude `is_mask()` components from the flatten `sources`** (like WAYPOINT
   is skipped) — masks are not artwork to bake.
2. Before drawing the sources into the bake surface, **apply the same clip** the
   compositor builds (union/intersect + inverted differences), in the bake
   surface's coord space.
3. Read back → a single raster of the **masked** content; erase the sources
   **and the mask shapes**.

Result: after flatten, the layer is one image with the masking permanently
applied (no live mask shapes remain) — the explicit requirement, and consistent
with flatten's "bake what you see."

## Interactions (must handle)

- **Draw cache (PHASE5.5):** a mask affects the layer's **whole** drawn region,
  not just the mask's AABB. So editing/adding/removing/moving a mask shape must
  **invalidate the whole layer's bounds** (or the union of mask + content
  bounds), not the default per-component AABB. This is the main cache subtlety.
- **Parallax (PHASE4):** mask and content at different parallax depths would clip
  under mismatched cameras. Rule: a mask clips content on the **same layer**
  (same depth by definition), so this is fine for per-layer masks; just confirm
  the derived-camera path applies the clip with the layer's camera.
- **Screenshot / SVG export:** must apply the clip (it's real compositing) but
  **not** the red overlay. Since the clip is in `recursive_draw_layer_item_to_canvas`
  (used by the screenshot path) and the overlay is separate, this falls out
  correctly — verify SVG export honors `clipPath`.
- **Selection/erase collider:** unchanged — mask shapes keep their normal
  collider so you can still select/move/edit them; clipping is display-only.

## UI

- In the shape's **edit panel** (RectDraw/EllipseDraw edit tools) and/or the
  draw-tool options: a **"Use as mask"** checkbox and an **"Invert"** checkbox
  (Invert enabled only when Use-as-mask is on).
- Optional: a small mask glyph on the layer row when a layer has active masks.

## Effort estimate

| Work | Est. |
|---|---|
| Data flags + virtuals + accessors + serialization + version bump | ~0.5 day |
| Per-layer mask gather (+ cache) and clip build (union/invert, transforms) | ~2 days |
| Composite integration + exclude masks from content draw | ~1 day |
| Draw-cache invalidation (mask edit → whole-layer) | ~1 day |
| Red-dashed overlay (author-mode, non-baked) + invert tell | ~0.5 day |
| Flatten integration (skip masks + apply clip + erase masks) | ~1 day |
| UI toggles + edge cases + backward-compat round-trip + testing | ~1.5 days |

**Rough total: ~1–1.5 weeks.** Its own phase; the weight is the per-layer mask
gather + clip build and the cache-invalidation correctness.

## Alternatives (considered, not chosen now)

- **Clip-to-the-layer-below** (Photoshop/Krita) — masks a layer by the *alpha of
  the layer beneath* (silhouette clipping). Complementary to shape masks; could
  be a later addition. Needs an offscreen base render + `kDstIn` (heavier).
- **Pixel-alpha selection mask** — large subsystem, no current consumers
  (PHASE6 finding). Rejected.
- **Raster/ink shape as mask** — would need the `kDstIn` alpha route, not
  `clipPath`. Out of scope; masks are vector shapes only.

## Out of scope
- Raster (brush-stroke) masks; folder-level masks; masking against a non-adjacent
  layer; soft/feathered or greyscale (luminance) masks. Clip is hard-edged
  (vector `clipPath`), anti-aliased by Skia but binary coverage.

## Milestones
1. **M1** — data flags + virtuals + serialization/version bump (no render; prove
   old files load unchanged).
2. **M2** — composite clip (gather masks, union/invert `clipPath`, exclude masks
   from content); correct at rest, incl. skewed-ellipse and polygon masks.
3. **M3** — draw-cache invalidation (mask edit → whole-layer refresh) + parallax/
   screenshot/SVG checks.
4. **M4** — red-dashed author overlay (+ invert tell) and the Use-as-mask /
   Invert UI toggles.
5. **M5** — flatten collapses the mask to raster (skip + clip + erase); edge
   cases, backward-compat round-trip, docs, merge.

## Backward compatibility — NON-NEGOTIABLE
Existing `.inkternity` files load unchanged: `isMask`/`maskInvert` default
`false` (append-and-gate in each shape's `load_file`), so every current shape and
layer composites exactly as today. Acceptance test (M1/M5): round-trip a copy of
a real pre-PHASE7 file across the version bump before trusting originals.
