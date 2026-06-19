# PHASE 8 — Bezier polygon nodes (selective curves)

## Status

**Design / scoping, pre-implementation.** Requested by zynx: let polygon
(Polygon-mode rectangle) vertices become **bezier curves selectively**, so a
shape can mix straight edges and smooth curves — pen-tool power for organic and
technical work. Benefits masks for free (a curved polygon is a curved mask).

Builds directly on PHASE6 polygons (draggable vertices, Add Point, vertex
selection, marquee) and the PHASE7 mask path plumbing. Honest size: **a real
feature, ~1–1.5 weeks, its own phase** — the editing UX (tangent handles,
corner↔curve conversion, smooth/cusp) is the bulk; the data/draw/collider are
moderate.

## Model

Polygon vertices become **nodes**, each:

- `pos` — the vertex (as today).
- `controlIn` / `controlOut` — tangent handle **offsets** relative to `pos`
  (zero = no tangent on that side).
- `type` — **corner** (independent or absent tangents), **smooth** (in/out
  tangents collinear, mirrored length — C1), or **cusp** (independent tangents).

Edge rule (between node A and B): if A has a non-zero `controlOut` or B a
non-zero `controlIn`, draw `cubicTo(A.pos+A.controlOut, B.pos+B.controlIn, B.pos)`;
otherwise `lineTo(B.pos)`. So today's all-corner polygon (no tangents) renders
identically — straight segments — and curves appear only where the artist adds
tangents.

## Data model

Extend `RectangleCanvasComponent::Data` (polygon mode) — keep `points`
(positions) and append parallel per-node arrays, same append-and-gate pattern as
PHASE6/7:

```cpp
std::vector<Vector2f> controlIn;    // size == points.size() (or empty == all corners)
std::vector<Vector2f> controlOut;   // tangent offsets relative to points[i]
std::vector<uint8_t>  nodeType;     // 0 corner / 1 smooth / 2 cusp
```

- Append to `save`/`save_file`/`load`; `load_file` reads them only when
  `version >= <PHASE8 version>`. Old polygons load with empty arrays →
  treated as all-corner → **unchanged**.
- Keep the arrays sized with `points` on every insert/delete (or treat empty as
  all-corner and lazily grow).
- Bump save format to **INFPNT000020 / 0.19.0**.
- `get_mask_path()` already returns the shape's built `SkPath`, so curved masks
  work with no extra code once the path includes curves.

## Draw

In `RectangleCanvasComponent::create_draw_data` polygon branch: walk the nodes,
emit `cubicTo` / `lineTo` per the edge rule, `close()`. (Replaces the current
all-`lineTo` loop.) Fill/stroke modes unchanged.

## Collider

The path is now curved, so the ear-clipping fill collider needs polygon input:
**flatten the cubics to a polyline** first (sample each curved edge into N
segments, or use `SkPath::Iter` / `SkPathMeasure`), then run the existing
ear-clipping / polyline collider on the flattened points. Outline mode: same
flattened polyline. Moderate.

## Edit UX (the bulk)

Reuses PHASE6/7 vertex handles + selection + marquee. New:

- **Convert corner ↔ curve.** With a vertex selected, a **"Make Curve"** action
  gives it symmetric tangents (smooth, derived from neighbor directions);
  **"Make Corner"** zeroes them. (Buttons in the edit panel, next to Add Point.)
- **Tangent handles.** A curve node shows two extra draggable handles
  (`pos+controlIn`, `pos+controlOut`) with **arms** (thin lines) drawn back to
  the node, like every pen tool. Dragging sets that tangent.
- **Smooth vs cusp.** For a *smooth* node, dragging one tangent mirrors the
  other (collinear, same length); a modifier (e.g. Alt) or a per-node toggle
  breaks it into a *cusp* (independent tangents).
- **Handle/anchor coupling — the key challenge.** Tangent handles are offsets
  *relative to the node*, but the EditTool's `HandleData` edits absolute points.
  Two clean options: (a) give tangent handles a `coordMatrix = translate(node.pos)`
  (the `HandleData.coordMatrix` already exists; render at `node+offset`, drag
  writes `mouse-node`) and **refresh handles** when the node moves (we have
  `refresh_point_handles` from PHASE6); or (b) recompute the tangent handle
  positions each frame. This is the main new interaction wiring.
- **Add Point on a curved edge.** Currently inserts the edge midpoint. On a
  cubic edge it should split via **de Casteljau** (so the curve shape is
  preserved and the new node gets correct tangents). MVP fallback: insert a
  corner at the geometric midpoint (slight shape change) — acceptable first cut.

## Interactions
- **Masks / Flatten / screenshots / SVG** — all path-based, so curves flow
  through with no extra work (PHASE7 already routes the shape's `SkPath`).
- **Marquee/selection** — unchanged; tangent handles are extra handles, excluded
  from vertex box-select (only anchor nodes select) to avoid confusion.

## Effort estimate

| Work | Est. |
|---|---|
| Data arrays + serialization + version bump | ~0.5 day |
| Draw: cubic/line path build | ~0.5 day |
| Collider: flatten curves → polyline → ear-clip | ~0.5–1 day |
| Edit UX: tangent handles + arms + coupling refresh | ~2–3 days |
| Convert corner↔curve + smooth/cusp logic | ~1 day |
| Add-Point de Casteljau split (or MVP midpoint) | ~0.5–1 day |
| Edge cases + backward-compat round-trip + docs | ~1 day |

**Rough total: ~1–1.5 weeks.** The tangent-handle editing + anchor coupling is
the weight; everything else is moderate and builds on existing code.

## Out of scope
- A standalone **free-draw pen tool** (click-drag to lay down a path) — nodes are
  still grown from the rect quad via Add Point, now with optional curves.
- **Bezier on the ellipse** — the ellipse is parametric (affine); curves are a
  polygon feature.
- Per-edge curve type independent of nodes (we model curves per node/tangent).

## Milestones
1. **M1** — data arrays + serialization/version bump (no curves drawn yet; prove
   old polygons load unchanged as all-corner).
2. **M2** — curved draw (cubic/line path build) + curved collider sampling.
3. **M3** — tangent handles + arms + the anchor-coupling refresh; smooth/cusp
   dragging.
4. **M4** — Make Curve / Make Corner UI; Add Point de Casteljau split.
5. **M5** — edge cases, curved-mask check, backward-compat round-trip, docs, merge.

## Backward compatibility — NON-NEGOTIABLE
Existing polygons load with empty tangent arrays → all-corner → identical to
today. Append-and-gate in `load_file`; acceptance test: round-trip a copy of a
real pre-PHASE8 file across the version bump before trusting originals.
