# Layer Resolution Reduction

**Status:** v1 implemented — fixed ½ steps, 16-bit in-place resample.

## Problem

Flattened (collapsed) raster layers are MyPaint surfaces: a sparse grid of
64×64 tiles, each 32 KiB (16-bit premultiplied RGBA), stored uncompressed in
RAM and in the save stream (the whole `.inkternity` is then zstd'd). A layer's
`CanvasComponentContainer::coords.inverseScale` is the **world-units-per-pixel**
density, so:

```
tile count ∝ world-area-covered ÷ inverseScale²
```

`RasterFlatten::flatten_layer` bakes at `min(finest-raster-source,
current-zoom)`. Collapsing while zoomed in therefore bakes at that fine
on-screen density, producing far more tiles than the visible detail warrants.
A canvas accumulates these until the **decompressed** size crosses multiple GiB
— which is both a RAM-load cost and (historically) the trigger for the >2 GiB
load bug (see `project_canvas_load_2gb_streambuf_bug`). Reducing a layer's
resolution is the only lever that shrinks the *in-RAM* footprint, not just the
on-disk size.

## Insight

Pixels and world-size are decoupled. Halving a raster layer's resolution =
**downsample its tiles 2×2→1 AND double `coords.inverseScale`**. The layer then
covers the identical on-canvas area with ¼ the tiles. Repeat for ¼, ⅛, …

## v1 scope

A "Reduce Layer Resolution" action (Options menu, next to Flatten Layer) that
halves the resolution of every MyPaint raster component on the **editing
layer** (same scoping as Flatten):

- Fixed **½ steps** — press again for ¼, ⅛, … (predictable; no chooser).
- **16-bit resample** — box-average 2×2 source texels directly in the tiles'
  premultiplied 16-bit space (no 8-bit `composite_to_bitmap` round-trip), so
  repeated reductions don't compound 8-bit quantization.
- Each component is **replaced** (not mutated in place) with a half-res copy,
  reusing Flatten's proven add-undo + erase-undo machinery — so Reduce is a
  single undoable step and syncs over NetObj like Flatten does.
- A USERINFO toast reports tiles and approx MB before→after, so the artist can
  watch the layer shrink and decide whether to press again.

### Out of scope (v1)
- IMAGE components (they reference a `ResourceData`; downscaling those is a
  separate resource-level operation).
- A persistent per-layer memory column in the layer panel (the before→after
  toast covers the immediate need).
- Linear-light resampling (see Quality below).
- A "flatten at chosen resolution" front-end fix.

## Resample math (16-bit, premultiplied)

Tiles are `uint16_t[64*64*4]`, interleaved RGBA, **premultiplied**. A 2× box
downsample averages four source texels per dest texel; in premultiplied space a
straight per-channel average (including alpha) is the correct box filter.

Tile alignment is exact: a dest tile `(DX,DY)` covers dest pixels
`[DX*64, DX*64+64)` → source pixels `[DX*128, DX*128+128)` → source tiles
`(2DX..2DX+1, 2DY..2DY+1)`. Because 64 is even, each **source tile downsamples
to exactly one 32×32 quadrant** of its dest tile `(floor(tx/2), floor(ty/2))`,
quadrant `(tx-2·floor(tx/2), ty-2·floor(ty/2))`. No cross-tile sampling is
needed: iterate source tiles, write each into its dest quadrant. `sum = s00 +
s10 + s01 + s11` (≤ 4·0xFFFF, fits u32), `dest = sum >> 2`. Dest tiles that end
up fully transparent are dropped to keep the store sparse.

`LibMyPaintSkiaSurface::build_halved_from(const LibMyPaintSkiaSurface& src)`
fills `*this` with the 2× downsample of `src`.

### Quality notes
- Averaging happens in the tiles' stored (gamma-encoded, premultiplied) space —
  the same space MyPaint blends in and `composite_to_bitmap` reads. Not
  linear-light "correct," but consistent with the rest of the pipeline and
  perfectly acceptable for a 2× reduction. Linear-light resampling is a future
  refinement.
- 16-bit throughout: no 8-bit round-trip, so repeated halvings stay clean.

## Component op

`RasterResolution::halve_layer(DrawingProgram&)`:
1. Resolve the editing layer (bail if unset / a folder).
2. For each non-selected `MYPAINTLAYER` component with ≥2 tiles:
   - new container (MYPAINTLAYER); `new.coords = old.coords`,
     `new.coords.inverseScale *= 2`.
   - `new.surface().build_halved_from(old.surface())`; `mark_dirty()`.
   - queue place-at-old-position (preserves z) + erase-old.
3. `add_many_components_to_specific_layer` + `commit_update`, then
   `erase_component_container` on the originals (one undo step, NetObj-synced).
4. Toast: `Reduced N layer(s): 1240 → 310 tiles (~38 MB → ~9.7 MB).`

Camera-independent (pure tile math), so unlike Flatten it does **not** bail on
parallaxed layers.

## Reversibility
Lossy and irreversible beyond one undo. The undo holds the pre-reduction tiles,
so the RAM win fully lands once that undo entry ages out of history — the same
tradeoff Flatten already has. Gated behind the `HVYM_HAS_LIBMYPAINT` build.
