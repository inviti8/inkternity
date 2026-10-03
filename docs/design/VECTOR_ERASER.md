# VECTOR_ERASER.md — partial (point-level) eraser for consolidated vector objects

**Status:** scoped / decisions locked 2026-10-02. Not yet implemented.
**Owner:** Inkternity client.
**Related:** Consolidate Vectors (`VectorGroupCanvasComponent`, save version INFPNT000033),
`EraserTool`, `BrushStrokeTessellation` (`BrushTess`).

---

## 1. Problem

Building a vector drawing in Inkternity ends with **Consolidate Vectors**, which bakes a
layer's many `BRUSHSTROKE` components into one `VECTORGROUP` (one BVH entry / predraw /
collider — the perf win). But the eraser then can only **delete the whole consolidated
object**: there's no way to rub out a small part of it. Artists need to erase/delete
*portions* of a combined vector object.

## 2. Current behavior (as of this doc)

`EraserTool::erase_between_points` → `try_punch_or_mark` (`src/DrawingProgram/Tools/EraserTool.cpp`)
has two paths:
- **Raster** (`MYPAINTLAYER`): pixel-level `erase_along_segment` (destination-out dabs);
  component kept.
- **Every vector component** (`BRUSHSTROKE`, `VECTORGROUP`, shapes): marked for **whole
  deletion** (`erasedComponents.emplace(c)`), removed on `switch_tool`.

So partial erase has never existed for vectors — only the raster path is fine-grained.
`WAYPOINT` and mask components are already protected (skipped).

## 3. Key enabler

A `VECTORGROUP` is **not** a flattened blob. `VectorGroupCanvasComponent::Data.subStrokes`
keeps each source stroke's **original points** (`BrushStrokeCanvasComponentPoint {pos,
width}`), its relative `coords`, `color`, and `hasRoundCaps`
(`VectorGroupCanvasComponent.hpp:35`). `initialize_draw_data` rebuilds the batched
`SkVertices` + collider from those sub-strokes, and the file-local `make_sub_xf(s.coords)`
maps a sub-stroke's local points into group-object space. So the source geometry needed for
point-level erase is retained — we edit `subStrokes` and rebuild. **No file-format change**
(sub-strokes already serialize; only their contents change).

## 4. Decisions (locked)

1. **New eraser mode** (radio in `EraserTool::gui_toolbox`): **Object** (current whole-delete,
   default) vs **Partial (vector)**. The mode governs only vector components; raster erase
   stays pixel-level in both.
2. **Granularity = point/segment split.** In Partial mode, remove the points a sub-stroke has
   under the eraser and split the stroke into surviving runs — true "erase small parts."
3. **Target = `VECTORGROUP` only (v1).** Splits stay *inside* the group as more sub-strokes →
   a single-component edit (clean undo/net-sync, no new top-level components). Standalone
   `BRUSHSTROKE` and shapes keep whole-delete.
4. **Timing = apply on release.** Mark covered points cheaply per motion segment during the
   drag; apply the removal + split + one `initialize_draw_data` rebuild once on mouse-up.
   Bounds cost on large groups.

## 5. Design

### 5.1 UX
`EraserTool` gains an `EraserMode { OBJECT, PARTIAL_VECTOR }` (default `OBJECT`), shown as a
radio in `gui_toolbox` (the radio helper is already included). Mode persists in tool config
like `relative_width`. Cursor/overlay unchanged.

### 5.2 Data flow (accumulate → apply once)
- **During the drag** (`erase_between_points`, per segment): in `PARTIAL_VECTOR` mode, when the
  eraser collides a `VECTORGROUP` on the edited layer, **do not** mark it for whole delete.
  Instead record the eraser segment for that component, transformed into the component's
  **object space**, plus the object-space radius (computed per component via the two-point
  distance trick the raster path already uses). Accumulate as
  `unordered_map<ObjInfo*, vector<{Vector2f a, Vector2f b, float radius}>>` on the tool.
  (Raster and non-`VECTORGROUP` vectors keep today's behavior.)
- **On release** (`switch_tool`): for each accumulated group, snapshot `get_data_copy()`,
  call `erase_along_segments(segs)`, then: if the group is now empty → add it to the normal
  `erasedComponents` delete set; else `commit_update` (rebuilds batches/collider) +
  `send_comp_update` + push one undo action swapping old/new data. Then run the existing
  whole-delete pass for `erasedComponents`.

### 5.3 The erase op — `VectorGroupCanvasComponent::erase_along_segments(...)`
New method (mirrors the raster component's `erase_along_segment` shape). For each sub-stroke:
1. Map its points to group-object space via `make_sub_xf(s.coords)`.
2. Mark a point **covered** if its distance to any eraser segment ≤ that segment's radius.
   (v1: point-position test. A later refinement inserts boundary points so ends aren't ragged
   — §7.)
3. Partition the **surviving** point indices into contiguous runs. Each run of ≥2 points →
   a new `SubStroke` reusing the original `coords`/`color`/`hasRoundCaps` with the sub-range of
   points (points stay in sub-stroke-local space, so this is a slice — no re-projection). A
   run of 1 point → kept only if `hasRoundCaps` (a dot), else dropped.
4. Rebuild `d.subStrokes` from all survivors, preserving z-order. Return whether anything was
   removed (and whether the group is now empty).

Point↔segment distance is a standard clamp-to-segment in object space; the only new geometry.

### 5.4 Undo / net-sync
Single-component data change: capture `get_data_copy()` before, apply, and push a
component-data-swap undo (same shape as `EditTool::commit_edit_updates`'s snapshot or the
`DrawingProgramSelection` stroke-color undo). `send_comp_update` broadcasts; no-op offline.
One undo entry per eraser stroke (not per segment).

## 6. Scope boundaries / non-goals (v1)
- Standalone `BRUSHSTROKE` partial erase (would split one stroke into multiple stroke
  components — new components, N-component undo) — **deferred** (§7).
- Shapes (`RECTANGLE`/`ELLIPSE`/`TEXTBOX`) — whole-delete in both modes.
- Raster erase — unchanged.
- No new accurate/extreme-zoom path work: the group already falls back to a static SkPath past
  ~2^14× and erase operates on source points, so it's unaffected.

## 7. Edge cases & future refinements
- **Ragged ends:** point-position removal leaves hard ends. Acceptable v1; refine by clipping
  the two boundary segments at the eraser radius (insert interpolated points + widths).
- **Width taper** at new run ends comes from the surviving points' own widths — fine.
- **1-point survivors / dots:** keep iff round-capped, else drop.
- **Group emptied:** delete the component (reuse the whole-delete set).
- **Performance:** per-segment marking is O(points) cheap; one rebuild on release. For a very
  large group, consider rebuilding only affected batches later (not v1).
- **Standalone stroke split (future):** same point-split logic, emitting sibling `BRUSHSTROKE`
  components with multi-component undo.

## 8. Touch points
- `src/DrawingProgram/Tools/EraserTool.{hpp,cpp}` — mode enum + radio; partial-vector branch
  in `try_punch_or_mark` (accumulate instead of mark-delete); apply on `switch_tool`.
- `src/CanvasComponents/VectorGroupCanvasComponent.{hpp,cpp}` — `erase_along_segments(...)`
  (+ point↔segment helper); reuse `make_sub_xf` + `initialize_draw_data`.
- Undo: reuse the existing component-data-change undo pattern.

**Effort:** medium, ~1–2 focused sessions incl. undo + in-app test. No file-version bump.
