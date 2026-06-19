# PHASE 7 — Clipping-mask layers

## Status

**Design / scoping, pre-implementation.** Grew out of the PHASE6 "Select
Transparency" request (deferred there): zynx wants a simple masking workflow —
paint freely but have it only show inside an existing silhouette — for the
graphic novel (shading/coloring clipped to line art, textures clipped to a
shape, etc.).

This doc scopes a **clipping-mask layer**: mark a layer "clip to the layer
below," and at draw time its content is masked by the opaque area of the layer
beneath it. No pixel-selection, no marching ants, no per-tool changes — a
**render-time** mask, which fits this app's model far better than a pixel
selection (see §"Why not a pixel selection").

## Why not a pixel selection (the PHASE6 finding)

A "selection" here is a **set of object components** (move/scale/rotate), and
**nothing consumes a pixel region**. A true alpha-mask selection would be a whole
new subsystem (mask storage + marching-ants rendering) *plus* rewiring every
paint tool (brush/eraser/fill/shape) to respect it — and strokes are separate
objects, not one layer bitmap, so "paint only within the opaque area" has no
single surface to clip against mid-stroke. That's large and, until tools respect
it, useless. **Clip-to-below sidesteps all of it**: the artist paints normally;
masking happens only when the layer is composited.

## Semantics

- A layer can be flagged **clip to layer below**. When set, the layer renders
  only where the layer **directly beneath it in the same folder** is opaque (its
  alpha multiplies the clip layer's result).
- **Clip group** = a *base* layer + the run of consecutive clip-flagged layers
  immediately above it. (Mirrors Photoshop/Krita clipping masks.) All clip
  layers in the group mask against the **same base** (the bottom of the group),
  not the accumulated stack — this is the least-surprising behavior and the
  cheapest (one base render per group).
- The artist paints/edits the clip layer with **no constraints**; only its
  *display* is clipped. Alpha + blend mode on the clip layer still apply (within
  the masked region).
- **Edge cases:** a clip layer with no eligible layer below (bottom of a folder,
  or the layer below is itself hidden) renders **unclipped** (graceful no-op),
  with the UI indicating the clip is inactive.

## Data model

`DrawingProgramLayerListItem::DisplayData` (and the mirrored
`DrawingProgramLayerListItemMetaInfo`) gain one field — same pattern as
`parallaxDepth` (PHASE4):

```cpp
bool clipToBelow = false;
```

- Add to `DisplayData::serialize` (wire always carries it; same-build peers).
- `DrawingProgramLayerListItem::load_file`: read it only when
  `version >= <PHASE7 version>`; older files default to `false` → no clipping →
  **existing files unchanged** (the now-standard append-and-gate rule).
- Bump save format to **INFPNT000019 / 0.18.0**.
- Add `set_clip_to_below()` / `get_clip_to_below()` accessors + include in
  `get_metainfo`/`set_metainfo` and the undo `MetaInfo` so depth/alpha-style undo
  covers it.

## Render

All masking lives in `recursive_draw_layer_item_to_canvas`
(`DrawingProgramCache.cpp:466`) — the single layer-tree walk used by both the
window cache and the BVH node caches, so the mask is baked into the caches like
any other compositing.

Today each visible layer does: `saveLayer(alpha/blend)` → draw components →
`restore()`. Folders iterate children bottom-to-top.

Clip-group rendering (within a folder's child loop):

1. Detect a clip group: a base layer `B` followed (above) by ≥1 layers with
   `clipToBelow`.
2. Render `B` to an **offscreen image** `Bimg` (its own `saveLayer`/surface), and
   draw `Bimg` to the canvas normally (the base shows as usual).
3. For each clip layer `L` in the group, bottom-to-top:
   - `saveLayer(alpha/blend for L)`
   - draw `L`'s components
   - draw `Bimg` with **`SkBlendMode::kDstIn`** → keeps `L` only where `Bimg`'s
     alpha (the base's coverage) is non-zero. `kDstIn` uses only the source
     *alpha*, so `Bimg`'s color is irrelevant — exactly a coverage mask.
   - `restore()` (composite the masked `L` onto the parent)

Cost: one extra offscreen render of the base per clip group per (re)composite,
plus a full-region `kDstIn` pass per clip layer. Bounded and only paid where
clip layers exist.

## Interactions (must handle)

- **Draw cache (PHASE5.5):** clip masking is a *cross-layer* effect baked into
  the window/node caches. Editing the **base** must invalidate the clip layers
  above it. Invalidation is spatial (AABB), and a clip layer overlaps its base's
  region, so the existing `invalidate_cache_at_aabb` *should* already re-render
  both — **verify**, and if not, invalidate the whole clip group when any member
  changes.
- **Parallax (PHASE4):** a clip layer and its base at *different* parallax depths
  would mask under mismatched cameras. Simplest correct rule: **clipping requires
  the clip layer and its base to share the same depth** (or: ignore clip while
  any visible parallax layer triggers the uncached bypass). Pick and document.
- **Flatten (PHASE4/5.5):** flattening a clip layer should bake the **masked**
  result (what's visible). The flatten path renders components directly; it must
  apply the same base-alpha `kDstIn`, or flatten the clip group together. Define.
- **Reader mode / SKETCH layer:** unaffected, but a clip layer whose base is the
  reader-hidden SKETCH layer would lose its mask in reader mode — note it (use a
  visible base).

## UI

- Per-layer **"Clip to layer below"** toggle in the layer panel (an icon button
  next to edit/visibility/delete in `DrawingProgramLayerManagerGUI`, or a menu
  item like "Merge Down").
- Visual indicator that a layer is clipped (e.g. a small indent + down-arrow on
  the layer row, the Photoshop convention) so the relationship is legible.
- Disable/grey with a tooltip when there's no eligible base below.

## Effort estimate

| Work | Est. |
|---|---|
| Data field + accessors + metainfo/undo + serialization + version bump | ~0.5 day |
| Clip-group detection + masked composite in the layer walk | ~2 days |
| Draw-cache invalidation correctness (base→clip) | ~1 day |
| Parallax + flatten interaction rules | ~1 day |
| Layer-panel toggle + clip indicator UI | ~0.5–1 day |
| Edge cases + testing (incl. backward-compat round-trip) | ~1 day |

**Rough total: ~1 week.** Its own phase, not a tack-on — the composite
restructure + cache/parallax/flatten interactions are the bulk.

## Alternatives considered

- **Pixel-alpha selection mask** — the literal "Select Transparency." Large new
  subsystem + per-tool integration, no current consumers. Rejected (see §"Why
  not a pixel selection").
- **Alpha lock / preserve-transparency** (paint only on a layer's existing
  pixels) — awkward here: strokes are independent objects, so there's no single
  layer bitmap to lock against (except a flattened layer). Doesn't generalize.
- **Clip to a vector outline** (Skia `clipPath`) — only works when the base is
  vector; not general for raster/ink bases.

## Out of scope
- Pixel-region selections / marching ants / magic wand.
- Masking against an arbitrary (non-adjacent) layer or a dedicated mask channel.
- Per-layer raster mask channels (paintable greyscale masks) — a bigger feature
  than clip-to-below; revisit only if clip-to-below proves insufficient.

## Suggested milestones
1. **M1** — data field + serialization/version bump + accessors (no render yet;
   prove old files load unchanged).
2. **M2** — masked composite in the layer walk (clip group → `kDstIn` base
   alpha); correct at rest.
3. **M3** — draw-cache invalidation (edit base → clip updates) + parallax/flatten
   rules.
4. **M4** — layer-panel toggle + clip indicator UI.
5. **M5** — edge cases, backward-compat round-trip, docs, merge.

## Backward compatibility — NON-NEGOTIABLE
Existing `.inkternity` files load unchanged: `clipToBelow` defaults `false`
(append-and-gate in `load_file`), so every current layer composites exactly as
today. Acceptance test (M1/M5): round-trip a copy of a real pre-PHASE7 file
across the version bump before trusting originals.
