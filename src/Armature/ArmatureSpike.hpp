#pragma once
// PHASE9 (docs/design/PHASE9.md) — M1 context spike: THE GATE.
//
// Proves the single highest-risk assumption of the whole armature phase:
// that a raw-OpenGL 3D pass can run on the app's shared GL context — which
// today is 100% Skia/Ganesh, with zero raw GL and zero resetContext() calls —
// render into its OWN FBO (own colour + own depth; the window surface has no
// depth buffer, SDL_GL_DEPTH_SIZE == 0), be read back, and bake to a canvas
// raster component, WITHOUT corrupting Skia's cached GL state on the next
// frame.
//
// It does this end-to-end with the eventual real pipeline in miniature:
//   raw GL 3.3 → lit cube into our FBO → glReadPixels → GrDirectContext::
//   resetContext() → SkBitmap (Y-flipped) → MYPAINTLAYER component placed on
//   the canvas (the exact second half of RasterFlatten::flatten_layer).
//
// This is throwaway scaffolding behind a temporary "Armature Spike (M1)" menu
// button. If the gate passes (cube bakes AND the canvas keeps rendering after),
// M2 replaces the cube with a cgltf-loaded skinned figure. If it fails, we
// learn it here for ~2 days of cost instead of mid-phase.

class DrawingProgram;

namespace ArmatureSpike {

// Render a lit cube to an offscreen FBO via raw GL, reset the Ganesh context,
// read the pixels back, and bake them to a raster component centred in the
// current view of the layer being edited. No-op with an in-app notice if the
// build lacks desktop GL 3.3 + Ganesh + libmypaint, or if there's no active
// layer to bake into.
void run_spike(DrawingProgram& drawP);

// PHASE9 M2: load the bundled default armature (cgltf) and bake the skinned
// figure at its rest pose to the canvas — same FBO/readback/bake path as the
// M1 cube. Temporary trigger; superseded by the modal at M5.
void run_armature_render(DrawingProgram& drawP);

}  // namespace ArmatureSpike
