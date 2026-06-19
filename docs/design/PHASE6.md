# PHASE 6 — Editable polygon shapes + skewable ellipses

## Status

**Design / scoping, pre-implementation.** Requested by zynx for blocking in
architectural, mechanical, and environmental elements in the graphic novel.
Two additions to the existing shape tools:

1. **Rectangle → Polygon mode** — a toggle on the rect tool. On, the shape is a
   polygon that starts as the dragged rect's 4 corners; every corner is a
   draggable point, and an **Add Point** action inserts a new vertex between two
   selected adjacent vertices, so an N-gon is built up incrementally from the
   quad. Covers ~99% of blocking needs without a full pen tool.
2. **Ellipse → shear/skew** — the ellipse can be sheared/skewed (affine), not
   just axis-aligned. Full true-perspective inscribing is explicitly out of scope.

## Why this is tractable

The hard part already exists. Shapes are simple parametric components drawn via
`SkPath`, and there is a proven **draggable point-handle** system
(`EditTool::HandleData` + `add_point_handle`, `src/DrawingProgram/Tools/EditTool.*`)
already wired into selection, **undo**, and serialization. Rectangle and ellipse
already expose their two bbox corners as draggable handles
(`RectDrawEditTool.cpp:51`, `EllipseDrawEditTool.cpp:53`). `ImageEditTool` shows
the same handles used with per-handle affine transforms — the precedent for the
ellipse skew. So this is "store more points / an affine + register more handles,"
not "build a vertex editor from scratch."

The genuinely **new** piece is **vertex selection** (clicking a handle to select
it, vs. dragging to move it), which Add Point needs. Everything else extends
existing patterns.

---

## Feature 1 — Rectangle Polygon mode

### Behavior
- **Toggle "Polygon mode"** in the rect tool options. Off (default): the existing
  axis-aligned rounded rectangle, unchanged. On: a drag creates a **4-vertex
  polygon** at the rect's corners.
- **All vertices draggable** via the existing handle system (one handle per
  vertex; corners are free — no p1<p2 constraint).
- **Add Point**: when exactly **two adjacent vertices are selected**, a button
  inserts a new vertex at the **midpoint of the edge** between them, splitting it.
  Repeated use grows the polygon (4 → 5 → 6 …).
- **Vertex selection** (new): clicking a vertex handle *without dragging* toggles
  its selection (highlighted in a distinct color); dragging still moves it.
- **(Optional, nice-to-have)** Delete Point: with one vertex selected, remove it
  (floor at 3 vertices). Not required for the 99% case; include only if cheap.

### Data model (`RectangleCanvasComponent::Data`, `RectangleCanvasComponent.hpp`)
Add:
```cpp
bool polygonMode = false;
std::vector<Vector2f> points;   // closed polygon, ordered; used iff polygonMode
```
Keep `p1/p2/cornerRadius/strokeWidth/fillStrokeMode/colors`. In polygon mode,
`p1/p2` are unused for drawing (kept for back-compat / bbox fallback);
`cornerRadius` is ignored in polygon mode for the MVP (rounded polygon corners
are a later nicety).

### Draw (`RectangleCanvasComponent.cpp:44`, path build)
- polygonMode → `SkPathBuilder`: `moveTo(points[0])`, `lineTo` the rest,
  `close()`. Reuse the existing fill/stroke-mode logic verbatim
  (`draw()` already paints `rectPath`; just build `rectPath` from the polygon).
- else → existing `SkRRect` path. No change.

### Collider (`RectangleCanvasComponent.cpp:90`, used for select/erase hit-test)
- **Outline mode**: closed polyline of `points`.
- **Fill mode**: arbitrary polygons can be **concave** (L-shapes are common in
  architecture), so the current 2-triangle rect collider is wrong. Need either
  **ear-clipping triangulation** → triangles, or a polygon collider with an
  even-odd **point-in-polygon** test. **Decide which `SCollision` supports**;
  ear-clipping is the safe general answer. *This is the main non-trivial bit of
  Feature 1.*

### Edit tool (`RectDrawEditTool.cpp:51`)
- polygonMode → loop `points` and `add_point_handle({&points[i], …})` (N handles)
  instead of the two bbox handles.
- Add a per-edit-session **selectedVertices** set + click-to-select handling in
  `EditTool` (click-vs-drag disambiguation via a small movement threshold).
- Highlight selected handles (variant of `draw_drag_circle`,
  `EditTool.cpp:307`).

### UI
- Rect tool options panel (Toolbar): **"Polygon mode"** checkbox.
- Polygon edit affordance: **"Add Point"** button — enabled only when exactly two
  *adjacent* vertices are selected (otherwise greyed with a tooltip). Optional
  "Delete Point" when one is selected.

### Open decisions
- **Adjacency rule for Add Point**: require the two selected vertices to share an
  edge. If non-adjacent are selected, disable the button (simplest) vs. insert by
  index order (ambiguous) — recommend **disable + tooltip**.
- **Self-intersecting polygons** (dragging a vertex across the shape): fill render
  uses even-odd/nonzero (fine visually); the collider may be imperfect — accept
  and note, don't gate on it.

---

## Feature 2 — Ellipse shear / skew

### Representation
Move the ellipse from an axis-aligned bbox (`p1/p2`) to an **affine** form:
**center + two semi-axis vectors** (`axisA`, `axisB`). Dragging an axis endpoint
rotates/scales that axis; when the two axes stop being perpendicular you get a
**sheared/skewed** ellipse. This single model gives rotation, non-uniform scale,
and shear with three handles (center, axisA tip, axisB tip).

### Data model (`EllipseCanvasComponent::Data`, `EllipseCanvasComponent.hpp`)
Add:
```cpp
bool   affineMode = false;          // false = legacy axis-aligned p1/p2
Vector2f center;                    // used iff affineMode
Vector2f axisA;                     // semi-axis vector (center -> tip)
Vector2f axisB;                     // semi-axis vector (center -> tip)
```
Legacy ellipses (affineMode false, or loaded from old files) keep using `p1/p2`.
A drag with the tool in skew-capable mode initializes `center`/`axisA`/`axisB`
from the bbox (axisA = (w/2,0), axisB = (0,h/2)).

### Draw (`EllipseCanvasComponent.cpp:36`/`:52`)
- affineMode → build the unit oval (or a p1/p2 oval), construct an `SkMatrix`/
  affine from `[axisA | axisB | center]`, `canvas->concat(matrix)` (save/restore)
  and draw the oval — or pre-transform the path points. Skia renders the sheared
  oval correctly.
- else → existing `addOval` path.

### Collider (`EllipseCanvasComponent.cpp:79`)
- The current collider already samples ~20–40 points around the oval. In
  affineMode, **transform those sample points by the same affine** → a skewed
  polyline collider. Low effort (reuse the sampling, apply the matrix).

### Edit tool (`EllipseDrawEditTool.cpp:53`)
- affineMode → handles for **center**, **axisA tip**, **axisB tip** (3 handles).
  Dragging center translates; dragging a tip rotates/scales/shears.
- Optionally keep showing the bbox handles for legacy ellipses.

### Open decisions
- **Migration**: old ellipses load as legacy (p1/p2). Either keep both code paths
  indefinitely, or convert to affine on first edit. Recommend: keep both; only
  set affineMode when the artist uses a skew handle.
- **Degenerate guard**: prevent zero-area / collinear axes (clamp a minimum).

---

## Serialization & file version (both features)

Components serialize fields in order in `save`/`load` (wire, same-build peers) and
`save_file`/`load_file(VersionNumber)` (disk, version-gated) —
`RectangleCanvasComponent.cpp:14`, `EllipseCanvasComponent.cpp:12`.

Recipe (same as the MyPaintLayer recording precedent):
- **Append** the new fields to `save`/`save_file` (never reorder existing).
- In `load_file`, read the new fields only when `version >= <PHASE6 version>`;
  older files leave the defaults (polygonMode/affineMode = false → unchanged
  look).
- `load` (wire) reads them unconditionally (both peers are the same build).
- **Bump the INFPNT file version** for the new on-disk fields, per the
  established convention.

Undo and network sync come for free: the EditTool already captures component
state for undo, and components broadcast via the same serialization.

### Backward compatibility — NON-NEGOTIABLE requirement

Existing `.inkternity` files (zynx's in-progress graphic novel) **must load
unchanged with no migration, flatten, or rasterize step, and no data loss.**
This is guaranteed by the append-and-gate recipe above: old files have none of
the new bytes, so `load_file` skips them and `polygonMode`/`affineMode` stay
`false` → existing rectangles and ellipses behave exactly as before. Hard rules:
- **Only append** new fields to `save`/`save_file`; never reorder or insert into
  the existing field sequence.
- **Gate every new read** in `load_file` on `version >= <PHASE6 version>`.
- Acceptance test (part of M1/M5): open a **copy** of a real pre-PHASE6 file,
  confirm rects/ellipses render identically and round-trip (load → save → load)
  cleanly across the version bump, before trusting it on originals.

(The one direction that is *not* guaranteed — a new-build file opened in an
*older* build — is the normal "use the current version" expectation and does not
affect loading existing files into the new build.)

---

## Effort estimate

| Work | Est. |
|---|---|
| **F1 — Polygon**: data + draw | ~0.5 day |
| F1: concave-capable collider (ear-clip / point-in-poly) | ~0.5–1 day |
| F1: N handles + **vertex selection** (the new interaction) | ~1 day |
| F1: Add Point logic + tool-options UI (+ optional Delete) | ~0.5 day |
| F1: serialization + version bump + testing | ~0.5 day |
| **F2 — Ellipse skew**: affine model + draw | ~0.5 day |
| F2: collider transform + 3 handles | ~0.5 day |
| F2: serialization + version + testing | ~0.25 day |

**Rough totals: Feature 1 ≈ 2.5–3 days, Feature 2 ≈ 1–1.5 days.** Undo/network
are free via existing infra.

## Reuse vs. new

- **Reuse:** `HandleData`/`add_point_handle`, EditTool drag + undo capture,
  `draw_drag_circle`, fill/stroke-mode draw, serialization/version pattern,
  `ImageEditTool` affine-handle precedent, the existing oval-sampling collider.
- **New:** variable-length polygon point list + (concave) collider; **vertex
  selection** in EditTool; Add Point UI/logic; the ellipse affine representation.

## Suggested milestones

1. **M1 — Polygon renders & saves.** Data fields, polygon draw path, toggle on the
   rect tool, serialization + version bump. (No editing yet.)
2. **M2 — Polygon editable.** N vertex handles + concave collider; selectable,
   draggable, erasable.
3. **M3 — Add Point.** Vertex selection (click-vs-drag) + Add Point (and optional
   Delete) with adjacency rule + UI.
4. **M4 — Ellipse skew.** Affine model, sheared draw, transformed collider, 3
   handles, serialization + version.
5. **M5 — Polish + tests.** Edge cases (self-intersection, degenerate axes), tool
   options UX, save/load round-trip across the version bump, collab sync check.

## Out of scope (called out)
- **True perspective** ellipses/quads (projective, non-affine) — affine shear only.
- **Rounded polygon corners** — `cornerRadius` ignored in polygon mode for MVP.
- A standalone **pen tool** — polygons are grown from the rect quad via Add Point,
  not free-drawn.
