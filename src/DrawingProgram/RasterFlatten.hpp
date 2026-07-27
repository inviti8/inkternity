#pragma once
// docs/design/PERF-INVESTIGATION.md (architectural fix #1, manual variant)
// + docs/design/PHASE4.md §10 (generalized flatten).
//
// Each libmypaint ink stroke is its own canvas component with its own
// tiled raster surface + BVH entry. On a heavy crosshatch canvas the
// component count is what makes the app sluggish — every full BVH rebuild
// scales with it, and overlapping strokes never share tiles. Flattening
// bakes ALL of the layer's components into ONE merged raster component,
// collapsing the count (and de-duplicating overlapping tiles) in a single
// undoable action the artist triggers when a layer is "done".
//
// PHASE5.5: this was originally view-bounded (only on-screen components),
// which surprised artists — "flatten the layer" should bake the whole layer,
// not just what's on screen. It now flattens the entire active layer.
//
// PHASE4 §10 generalized this from ink-only to every *visual* component
// type (vector strokes, shapes, text boxes, images) — which also removed
// the old z-order caveat: when everything on the layer merges, nothing is
// left to interleave incorrectly. Skipped: WAYPOINT (functional
// marker, not artwork), still-downloading images (baking the placeholder
// would silently lose the real image), and currently-selected components
// (mid-manipulation).
//
// Deliberate, accepted costs: the merged component is a single raster, so
// individual strokes can no longer be vectorized or undone one-by-one,
// text loses editability, and vector content no longer survives infinite
// zoom-in — that is what "rasterize" means. Raster erase still works on
// the merged result, and the bake resolution is never coarser than BOTH
// the finest raster source's native pixel scale AND the current view
// (WYSIWYG floor) — unless the region exceeds the bake-size cap, in which
// case it's downsampled with an in-app notice.

class DrawingProgram;
class DrawingProgramLayerListItem;

namespace RasterFlatten {

// Per-axis cap on the baked bitmap so an over-zoomed-out flatten can't ask
// for a gigapixel surface. Live-tunable in Settings → Debug and persisted
// in config.json (PHASE4 §10 M0). Memory is why this exists: an 8192² bake
// is ~256 MB of readback bitmap + up to ~512 MB of transient 16-bit tiles.
extern int MAXIMUM_FLATTEN_SIZE_PX;

// Consolidate Vectors simplification strength: RDP epsilon as a fraction of each
// stroke's brush width. 0 disables simplification (exact points); larger values
// drop more points (lighter geometry, slightly looser curves). Live-tunable in
// Settings → Debug (like MAXIMUM_FLATTEN_SIZE_PX).
extern float CONSOLIDATE_SIMPLIFY_STRENGTH;

// Bake every visual component on the layer being edited into a single merged
// raster component, replacing the originals (the WHOLE layer, regardless of
// what's on screen). Undoable (place + erase). No-op with an in-app notice if
// fewer than two eligible components exist on the layer, or if the build has
// no libmypaint (the merged result is a libmypaint surface so it stays
// raster-erasable).
void flatten_layer(DrawingProgram& drawP);

// Blend-aware Merge Down (PHASE4 §11 refinement). The plain vector merge in
// DrawingProgramLayerManagerGUI moves components and so can't honor a
// non-default blend/alpha on the upper layer (a moved component would
// composite inside the lower layer's buffer with Source-Over, not against the
// real backdrop). This bakes it instead: `lower`'s components are drawn as the
// backdrop, then `upper`'s components are composited on top with the UPPER
// layer's alpha + blend mode — the exact per-layer compositing the live
// renderer applies (DrawingProgramLayerListItem::draw's saveLayer). The result
// is one raster component placed into `lower`; every baked component on BOTH
// layers is erased. The caller (merge_layer_down) must have already verified:
// both are depth-0 non-folder layers, `lower` has default alpha+blend (it is
// the backdrop, and its own blend would composite against content beneath it
// that a two-layer merge can't fold in), and `upper` holds no waypoint markers.
//
// Same rasterization costs as flatten_layer: the merged layer loses per-stroke
// vector editing and is resolution-locked at bake (WYSIWYG floor, capped by
// MAXIMUM_FLATTEN_SIZE_PX). Exact only where `lower` is the effective backdrop
// (nothing visible below it, or it's opaque) — the standard Merge-Down
// contract. Returns false, having changed nothing and logged a USERINFO
// notice, on any refusal (mid-download/mid-edit content, nothing to bake,
// surface failure) or in a build without libmypaint.
bool merge_down_baked(DrawingProgram& drawP,
                      DrawingProgramLayerListItem& upper,
                      DrawingProgramLayerListItem& lower);

// Consolidate Vectors: bake every vector brush stroke on the editing layer into
// a single VECTORGROUP component, preserving exact appearance (z-order +
// per-stroke color/alpha) while collapsing the component count that makes
// panning a heavy vector canvas sluggish. Unlike flatten_layer this stays a
// TRUE vector (infinite zoom, tiny memory) and is available in every build (no
// libmypaint / rasterization). Ignores rasters, images, text, and shapes.
// Undoable (place + erase). No-op with an in-app notice if fewer than two
// eligible vector strokes exist on the layer.
void consolidate_vectors(DrawingProgram& drawP);

// Optimize Vectors: point-reduce (RDP, strength = CONSOLIDATE_SIMPLIFY_STRENGTH)
// every consolidated VECTORGROUP on the editing layer — the separate, tunable,
// re-runnable pass that Consolidate deliberately leaves out so merging stays
// lossless. Undoable. No-op with a notice if strength is 0 or nothing to do.
void optimize_vectors(DrawingProgram& drawP);

}  // namespace RasterFlatten
