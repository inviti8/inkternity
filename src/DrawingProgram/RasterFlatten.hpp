#pragma once
// docs/design/PERF-INVESTIGATION.md (architectural fix #1, manual variant)
// + docs/design/PHASE4.md §10 (generalized flatten).
//
// Each libmypaint ink stroke is its own canvas component with its own
// tiled raster surface + BVH entry. On a heavy crosshatch canvas the
// component count is what makes the app sluggish — every full BVH rebuild
// scales with it, and overlapping strokes never share tiles. Flattening
// bakes the in-view components into ONE merged raster component,
// collapsing the count (and de-duplicating overlapping tiles) in a single
// undoable action the artist triggers when a region is "done".
//
// PHASE4 §10 generalized this from ink-only to every *visual* component
// type (vector strokes, shapes, text boxes, images) — which also removed
// the old z-order caveat: when everything in view on the layer merges,
// nothing is left to interleave incorrectly. Skipped: WAYPOINT (functional
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

namespace RasterFlatten {

// Per-axis cap on the baked bitmap so an over-zoomed-out flatten can't ask
// for a gigapixel surface. Live-tunable in Settings → Debug and persisted
// in config.json (PHASE4 §10 M0). Memory is why this exists: an 8192² bake
// is ~256 MB of readback bitmap + up to ~512 MB of transient 16-bit tiles.
extern int MAXIMUM_FLATTEN_SIZE_PX;

// Bake every visual component whose world bounds intersect the current
// camera view into a single merged raster component on the layer being
// edited, replacing the originals. Undoable (place + erase). No-op with
// an in-app notice if fewer than two eligible components are in view, or
// if the build has no libmypaint (the merged result is a libmypaint
// surface so it stays raster-erasable).
void flatten_layer_in_view(DrawingProgram& drawP);

}  // namespace RasterFlatten
