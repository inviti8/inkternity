#pragma once
// docs/design/LAYER-RESOLUTION.md
//
// Flattened raster layers are MyPaint surfaces (sparse 64x64 16bpc tiles).
// A layer's coords.inverseScale is its world-units-per-pixel density, so tile
// count grows with 1/inverseScale^2: collapsing while zoomed in bakes far more
// tiles than the visible detail needs, and a canvas hits multi-GiB decompressed
// size (RAM cost + the >2 GiB load ceiling) fast.
//
// halve_layer downsamples every MyPaint raster component on the editing layer
// by 2x (box filter, in the tiles' 16bpc premultiplied space) and doubles its
// coords.inverseScale, so the layer keeps the same on-canvas size at 1/4 the
// tiles. Fixed half-steps: press again for 1/4, 1/8, ... Each call is one
// undoable, NetObj-synced step (replace components — same machinery as Flatten).
//
// Lossy and irreversible beyond one undo (the undo transiently holds the
// pre-reduction tiles). Gated on HVYM_HAS_LIBMYPAINT.

class DrawingProgram;

namespace RasterResolution {

// Halve the resolution of each MyPaint raster component on the layer being
// edited, in place (replace-with-half-res). Reports tiles/MB before->after via
// a USERINFO toast. No-op with an in-app notice when the edit target is
// unset / a folder / has no reducible raster, or the build has no libmypaint.
void halve_layer(DrawingProgram& drawP);

}  // namespace RasterResolution
