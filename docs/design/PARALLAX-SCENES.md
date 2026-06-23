# PARALLAX-SCENES — Group-Anchored, Scale-Aware Parallax

Status: M1 + M2 SHIPPED (INFPNT000021 / 0.20.0) — verified working by zynx
2026-06-22. M3 (reader/waypoint verification scenario) outstanding.
Supersedes: the per-layer anchor model from PHASE4 Part A (§3–§4).
Prereqs: PHASE4 Part A (parallax render path + bypass), Waypoints,
Reader mode + TRANSITIONS.

> **No backward-compat constraint (zynx, 2026-06-22):** there are no
> production files using parallax yet. We are free to *move* the anchor
> off the layer and change the `DisplayData` wire/file shape without a
> compatibility shim. We still bump the `INFPNT` file version (cheap
> hygiene; old test files load with parallax disabled), but no
> dual-path load code is required.

## 1. Problem (why per-layer parallax breaks the wall-picture → scene shot)

PHASE4 renders a depth-`d` layer through a derived camera
(src/DrawingProgram/Layers/DrawingProgramLayerListItem.cpp:239):

```
derived.pos = A + (cam.pos − A) / (1 + d)          // A = per-layer anchor
```

Work out where a world point lands on screen versus the canvas plane and
the on-screen parallax offset is:

```
offset_screen = (1 − f(d)) · (cam.pos − A) / inverseScale     where f(d) = 1/(1+d)
```

The `÷ inverseScale` is the trap. `inverseScale` is world-units per
screen-pixel — it gets *small* as you zoom in — so the same depth
produces a proportionally **larger** on-screen shift the further you
zoom in. The depth number is unitless; it isn't tied to the scale you
authored at.

The intended sequence is: zoom into a small picture-on-a-wall, which
*becomes* a parallax scene. By the time the picture fills the screen,
`inverseScale` is tiny, so even the slider's smallest depth flings the
layers far apart. Worse, the no-jump anchor `A` is captured at *the
camera position when you set the depth*; if that was while zoomed out,
`(cam.pos − A)` is also huge, and the two large factors multiply.

The per-*pan* rate is already scale-invariant and fine; it's the
**static offset** (just from being zoomed in, far from the anchor) that
explodes.

## 2. Goal

Move parallax framing from the individual layer to the **group
(folder)**. A folder becomes a *parallax scene*: it stores one
**anchor** (the scene's neutral viewpoint position) and one
**reference scale** (the zoom the scene was framed at). Layers inside
keep only their **depth** (relative plane within the scene). A single
button — shown when the folder is selected — captures the anchor and
reference scale from the current camera.

This must behave correctly under **waypoint-driven reader mode**, where
the camera is continuously interpolated (pos in screen-center space,
zoom in log2 space, rotation via slerp — src/DrawCamera.cpp:74–135). The
parallax math reads the live interpolated `drawData.cam.c` every frame,
so it inherits smooth transitions for free *provided the math itself is
continuous in the camera* (it is — see §3).

## 3. The math (scale-aware, group-anchored)

Group state (per folder): anchor `G` (WorldVec) and reference scale
`Sref` (WorldScalar = `inverseScale` captured at button-press). Layer
state: depth `d` only.

Define the scale ratio, **clamped at 1**:

```
r = min(1, inverseScale / Sref)
```

Derived camera for a depth-`d` layer inside an active parallax group:

```
derived.pos          = cam.pos − r · (1 − f(d)) · (cam.pos − G)      f(d) = 1/(1+d)
derived.inverseScale = cam.inverseScale          // zoom: still no parallax
derived.rotation     = cam.rotation              // uniform, unchanged in v1
```

On-screen offset works out to:

```
offset_screen = (1 − f(d)) · (cam.pos − G) / max(inverseScale, Sref)
```

The `max(inverseScale, Sref)` denominator is the whole idea:

| Camera state | denominator | behavior |
|---|---|---|
| **Zoomed out** past `Sref` (toward the wall) | `inverseScale` | identical to PHASE4 → offset shrinks with distance → the picture looks **flat** on the wall ✓ |
| **At the scene scale** (`inverseScale ≈ Sref`) | `Sref` | natural parallax, `r ≈ 1` ✓ |
| **Zoomed in** past `Sref` | `Sref` (frozen) | magnitude **stops growing** no matter how far you zoom in ✓ |
| **At the neutral viewpoint** (`cam.pos == G`) | — | `(cam.pos − G) = 0` → offset 0 → every layer registers exactly ✓ |

So the captured anchor/scale defines the scene's **neutral viewpoint**:
press the button when the camera frames the scene as intended; parallax
is zero there and emerges smoothly as the reader/waypoint camera moves
around it. Zooming back out to the wall makes it diminish naturally, so
the picture-on-the-wall reads as a flat picture from afar. This is
exactly the wall-picture → scene transition.

### Why `r` is clamped, not raw

Raw `r = inverseScale/Sref` (no clamp) would freeze the magnitude when
zoomed in **but blow it up when zoomed out** (offset = `(1−f)(cam.pos−G)
/Sref`, and `(cam.pos−G)` grows as you zoom out toward a point). The
`min(1, …)` keeps the zoomed-out branch on the original `÷ inverseScale`
law, which correctly diminishes the effect at distance. Verified both
limits by hand; both are continuous at `inverseScale == Sref` (`r`
hits 1 exactly), so no discontinuity mid-transition.

### Precision / FixedPoint notes

- `(cam.pos − G)` is a `WorldVec` delta scaled by the bounded double
  `r·(1−f(d))` → ordinary `WorldVec::divide_double` / multiply, the same
  exact-FixedPoint path PHASE4 already uses (no precision risk; PHASE4.md
  §3).
- `r` needs `inverseScale / Sref` as a double ratio. Both are
  `WorldScalar`; compute via the existing `FixedPoint` division then
  `static_cast<double>` (the ratio is O(1) near the scene, safely in
  double range). Clamp to `[0,1]`.
- Foreground layers (`d < 0` → `f > 1` → `(1−f) < 0`) just offset the
  other direction — the formula handles near and far uniformly.

## 4. Data model

Every `DrawingProgramLayerListItem` — folder **and** layer — already
owns a `DisplayData` (src/DrawingProgram/Layers/DrawingProgramLayerListItem.hpp:106).
That is where this lives. Reinterpret/relocate the existing parallax
fields:

- **Folder `DisplayData` (the group):**
  - `WorldScalar parallaxRefScale{0};` — **0 means "not a parallax
    scene"** (the enable flag and the value in one field). Non-zero =
    active group, value = `Sref`.
  - `WorldScalar parallaxAnchorX{0}, parallaxAnchorY{0};` — the group
    anchor `G` (reuse the existing two-WorldScalar fields; the
    "no Eigen member in the recursive undo struct" constraint from
    project memory still applies, so keep them as scalar pairs, not a
    `WorldVec`).
- **Layer `DisplayData` (the plane):**
  - `float parallaxDepth = 0.0f;` — kept, unchanged semantics.
  - The per-layer `parallaxAnchorX/Y` fields are **removed** from the
    layer's meaning (anchor now comes from the group). No back-compat →
    just drop them from the layer path; the folder reuses the field
    slots.

`DrawingProgramLayerListItemMetaInfo` + undo data mirror the same
fields (depth edits and group-anchor edits are both undoable, like
alpha/blend today).

**scale_up:** both `parallaxAnchorX/Y` *and* `parallaxRefScale` are
world-scaled quantities and must be multiplied by `scaleUpAmount`
(src/DrawingProgram/Layers/DrawingProgramLayerListItem.cpp:14 already
scales the anchors; add `parallaxRefScale` there). `parallaxDepth`
stays dimensionless — untouched.

**File version:** bump `INFPNT` (follow the PHASE4 precedent —
INFPNT000013 added the parallax fields; this revises them). Per the
project memory's header-bump recipe. Old test files: `parallaxRefScale`
defaults 0 → parallax disabled → flat render.

## 5. Rendering integration

The derivation moves from "each layer reads its own anchor" to "the
group injects its anchor/scale into the subtree; each layer applies it
with its own depth." Thread the group context through `DrawData`
(the established pattern — `skipSelectedComponents`, `isSVGRender`):

```cpp
// DrawData.hpp — add:
struct ParallaxGroup {
    bool active = false;
    WorldScalar anchorX{0}, anchorY{0};
    WorldScalar refScale{1};
} parallaxGroup;
```

- **Folder draw** (src/DrawingProgram/Layers/DrawingProgramLayerFolder.cpp:9):
  if this folder's `parallaxRefScale != 0` and it's visible, make a
  local `DrawData` copy with `parallaxGroup` set from the folder's
  `DisplayData`, and pass *that* to children. A nested parallax folder
  **replaces** the context for its own subtree (nearest ancestor wins).
- **Layer draw** (DrawingProgramLayerListItem.cpp:239): replace the
  per-layer-anchor block. Apply the §3 derived camera **iff**
  `parallaxDepth != 0 && drawData.parallaxGroup.active`, using the
  group's `anchor`/`refScale` and the clamped `r`. A depth on a layer
  with no active ancestor group renders flat (depth is inert outside a
  scene — surfaced in the UI, §6).
- **Cache bypass** (`any_visible_parallax_layer`,
  src/DrawingProgram/Layers/DrawingProgramLayerManager.cpp:144): make it
  group-aware — bypass when a visible folder has `parallaxRefScale != 0`
  **and** contains a visible `parallaxDepth != 0` layer. (A parallax
  group with all-zero-depth children, or depth layers with no active
  group, need no bypass — they composite identically to the canvas
  plane.) Everything-flat canvases keep the BVH fast path bit-identically.
- **Exports/SVG:** unchanged in spirit — parallax is still a pure
  per-layer translate+scale (affine), so the screenshot/SVG walk
  (`layerMan.draw`) inherits it once the folder injects the context.

## 6. UI

Layer side panel (`DrawingProgramLayerManagerGUI`,
src/DrawingProgram/Layers/DrawingProgramLayerManagerGUI.cpp:448 is
today's per-layer depth slider):

- **Folder selected** → a "Parallax Scene" section:
  - **Enable** toggle (sets/clears `parallaxRefScale`; clearing zeroes
    it). 
  - **"Set anchor & scale to current view"** button — captures
    `anchor = world.drawData.cam.c.pos`, `refScale =
    world.drawData.cam.c.inverseScale`. This is the neutral-viewpoint
    capture. Because capture sets `cam.pos == G` at that instant, every
    child registers (offset 0) with no jump.
  - A read-only hint showing the captured scale (e.g. "scene scale:
    1:N") so the artist knows the neutral zoom.
  - Optional: **"Go to neutral view"** — `smooth_move_to(G, refScale)`
    (reuses the waypoint transition path) to fly the editor back to the
    scene's neutral framing.
- **Layer selected** → keep the **Depth** slider, but:
  - widen it to a finer/exponential feel (today it's 2-decimal,
    −0.9…10 — DrawingProgramLayerManagerGUI.cpp:455). With the scale
    normalization, depth is now scale-stable, so a modest range is
    enough, but exponential steps make small values reachable.
  - if the layer is **not** inside an active parallax group, show
    "Depth has no effect until this layer's group is a Parallax Scene."
- A small badge on folder rows that are parallax scenes, and on layer
  rows with non-zero depth (so it's obvious why content pans "wrong").

## 7. Reader mode + waypoints (the reason this is group-level)

Findings from the camera/reader investigation (anchors for the
implementer):

- One live camera per frame: `drawData.cam.c` (CoordSpaceHelper —
  pos/inverseScale/rotation), updated by `DrawCamera::update_main`
  (src/DrawCamera.cpp:74) **before** the draw walk and before
  `readerMode.update` (src/World.cpp:335–345).
- Waypoints store a `CoordSpaceHelper coords` + `windowSize`
  (src/Waypoints/Waypoint.hpp:200) — base camera only, no per-layer
  state. Reader navigation calls `cam.smooth_move_to(target.coords, …,
  speedMult, easing)` (src/ReaderMode/ReaderMode.cpp:178) and the camera
  interpolates: position via screen-center lerp, **zoom via log2 lerp**,
  rotation via slerp, with per-waypoint bezier easing.
- **Consequence:** the §3 math is a pure, continuous function of the
  live camera, so reader-mode transitions get correct parallax for free
  — *as long as we read `inverseScale` and `cam.pos` per frame*, which
  we do. The log2 zoom interpolation means `r = min(1, inverseScale/
  Sref)` sweeps smoothly through 1 exactly when the camera crosses the
  scene scale → the parallax "settles in" as the reader arrives. No
  special reader-mode code path is needed.
- **Authoring tie-in:** the natural workflow is to put a waypoint *at
  the scene's neutral viewpoint* and press "Set anchor & scale" from
  that same camera. Then `G` and `Sref` equal that waypoint's framing,
  so arriving at the waypoint = perfectly registered scene, and the
  transition waypoint before it (the wall-picture, TRANSITIONS.md
  `isTransition`) zooms in with parallax emerging. This is the headline
  use case and should be the **verification scenario** (§9).

Open question for the implementer to confirm by test: waypoints whose
stored `windowSize` differs from the live window trigger
`smooth_move_to`'s uniform-zoom adjustment (src/DrawCamera.cpp:39). The
parallax reads the *post-adjustment* live `inverseScale` each frame, so
it should remain correct, but verify the neutral viewpoint still lands
at offset≈0 when window size differs from capture.

## 8. Editing policy

Inherit PHASE4's rule unchanged for v1: a depth≠0 layer inside an active
group is **edit-locked** (left-click content tools blocked with a toast;
nav/inspect tools stay live — PHASE4.md §6 / DrawingProgram.cpp:131).
To edit, either zero the layer's depth or disable the group's parallax.

A nicer future affordance (out of scope, note only): an "edit at
neutral" mode that flies to `G`/`Sref` (where offset≈0 for all layers)
and remaps input through the derived camera — the M4 edit-at-depth idea
from PHASE4, made tractable because at the neutral viewpoint the derived
and base cameras coincide.

## 9. Milestones

- **M1 — Data move + math:** ✅ DONE (builds clean, INFPNT000021 / 0.20.0).
  Relocated anchor→folder, added `parallaxRefScale` to DisplayData +
  MetaInfo + undo `scale_up`, version gate in `load_file` (0.20 reads
  refScale; 0.12–0.19 consume the old depth+anchor and leave parallax
  inactive; pre-0.12 unchanged). Threaded `DrawData::parallaxGroup`;
  `DrawingProgramLayerListItem::draw` injects the group context on a
  parallax-group folder and applies the §3 clamped-`r` derived camera on
  depth!=0 leaves. `any_visible_parallax_layer` is group-aware via the new
  recursive `has_active_parallax_descendant`. Scaling uses a direct
  `WorldScalar` multiply, NOT `divide_double`/`multiply_double` (those
  invert their argument for |a|<1 — a latent FixedPoint bug that the old
  foreground depth<0 path silently hit; now moot). **No UI yet** — the
  existing per-layer depth slider still sets depth but it's inert until the
  layer's folder is made a parallax group (the M2 capture button).
- **M2 — UI:** ✅ DONE (builds clean). Folder "Parallax Scene" section:
  enable checkbox (captures current camera as the neutral viewpoint on
  tick; refScale=0 on untick), "Set Anchor and Scale to Current View"
  re-capture button, live "View vs scene scale: N.NNx (1.00 = neutral)"
  readout, "Go to Neutral View" (smooth_move_to anchor/refScale). Layer
  depth slider kept (−0.9…10, 0.01 step — exponential feel deferred, the
  slider helper has no log mode); shows "locked until depth 0" when in an
  active group, else "no effect until folder is a Parallax Scene". Row
  badges via name suffix: "(parallax scene)" on group folders, "(depth N)"
  on depth layers. Group-aware edit-lock + hint via new
  `target_in_active_parallax_group` / `editing_layer_in_active_parallax_group`.
- **M3 — Reader/waypoint verification:** the headline scenario — a
  transition waypoint on the wall-picture zooming into a neutral-view
  waypoint that matches the group's `G`/`Sref`; confirm parallax is zero
  at the wall (flat), emerges smoothly during the zoom, and is bounded
  and correct while panning between in-scene waypoints. Confirm
  screenshot + SVG export of an in-scene view. Confirm window-size-
  mismatch case (§7 open question).
- **Later (separate docs):** edit-at-neutral (§8); per-depth cached
  surfaces if the bypass perf cliff bites a heavy scene (PHASE4.md §5);
  rotation-aware parallax (v1 leaves rotation uniform).

## 10. Risks / open questions

- **Disabling a group mid-view jumps content.** Capture is jump-free
  (offset 0 at capture). *Clearing* `parallaxRefScale` while the camera
  is away from `G` snaps layers from their parallax offset back to the
  canvas plane. Acceptable for v1 (it's an authoring action, not a
  reader action); a no-jump-on-disable recompute is a possible polish.
- **Depth range feel.** With scale normalization the depth number is now
  stable across zoom, so the old wide range is less necessary; pick the
  range/step during M2 against the real scene (zynx call), exponential
  steps recommended.
- **Nested parallax groups** (a scene inside a scene). v1: nearest
  ancestor wins (inner group fully replaces the context). Deeper
  composition (multiplying group factors) is deferred — call it out, do
  not bolt it on.
- **Perf:** unchanged from PHASE4 — an active parallax scene takes the
  full cache bypass and redraws its layers each frame while the camera
  moves. Flatten heavy raster layers (PHASE4 Part B) is the mitigation;
  per-depth caching is the follow-up if profiling demands it.
- **Standing rule check:** nothing here attempts a known-broken path —
  parallax stays affine (no 3D pose, no dolly), editing stays locked at
  depth (no half-working input remap shipped in v1), and the zoomed-out
  limit is provably the existing, working behavior.
