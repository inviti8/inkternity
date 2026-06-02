#pragma once
// docs/design/PERF-INVESTIGATION.md (architectural fix #1, manual variant).
//
// Each libmypaint ink stroke is its own canvas component with its own
// tiled raster surface + BVH entry. On a heavy crosshatch canvas the
// component count is what makes the app sluggish — every full BVH rebuild
// scales with it, and overlapping strokes never share tiles. Flattening
// bakes many in-view raster strokes into ONE merged raster component,
// collapsing the count (and de-duplicating overlapping tiles) in a single
// undoable action the artist triggers when a region is "done".
//
// Deliberate, accepted cost: the merged component is a single raster, so
// the individual strokes can no longer be vectorized or undone one-by-one.
// Raster erase still works on the merged result, and there's no extra
// resolution loss — the bake is done at the finest source stroke's native
// pixel scale (libmypaint strokes are already raster at their draw-time
// scale), unless the region is large enough to exceed the bake-size cap,
// in which case it's downsampled with an in-app notice.

class DrawingProgram;

namespace RasterFlatten {

// Bake every libmypaint (raster) ink stroke whose world bounds intersect
// the current camera view into a single merged raster component on the
// layer being edited, replacing the originals. Undoable (place + erase).
// No-op with an in-app notice if fewer than two raster strokes are in
// view, or if the build has no libmypaint.
void flatten_ink_strokes_in_view(DrawingProgram& drawP);

}  // namespace RasterFlatten
