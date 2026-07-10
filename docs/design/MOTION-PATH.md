# Motion Path — build plan (flip-book transform animation, Route 3)

> **Scope + rationale live in [PHASE10.md](PHASE10.md) §B.12.** This is the
> actionable build plan for the approach zynx chose (**Route 3** — a World-visible
> editor object with a dedicated node editor, *no* `EditTool` refactor). File:line
> anchors were captured during the 2026-07-09 scoping passes; they drift, re-grep.

## What we're building

A **Motion Path**: a bezier curve bound to one flip-book group. The artist draws
it (start + end), adds/moves nodes and drags tangents like editing a shape; each
node carries **position + time + scale + easing**. It is **editor-only** (visible
only while its flip-book folder is being edited, never in viewer mode). On
playback the whole group travels the curve (translation + scale) while its frames
cycle independently.

## Route 3 + one storage refinement

Route 3 = clean object model + a small dedicated node editor (reuse the node
*math*, not the shared `EditTool` interaction — zero risk to shipped shape/text/
brush editing).

**Refinement vs. §B.12:** store the path **on the flip-book folder**, not in a
separate `MotionPathGraph`. §B.12 floated the `WaypointGraph` template, but
waypoints need a World-level graph because they're global and graph-structured;
our path is **strictly 1:1 with one folder and dies with it**. So a folder-owned
`NetObjOwnerPtr<MotionPath>` (mirroring how each layer item already owns
`nameData` / `displayData`) is simpler and removes three whole problems the World
graph would have added: no `ownerFolderId`, no index-remap of that id, no
cascade-delete wiring (erasing the folder erases the path automatically).

## Data model

New NetObj type **`MotionPath`** (own `.hpp/.cpp` under `src/DrawingProgram/Layers/`
or a new `src/MotionPath/`), owned by the flip-book folder.

```
struct MotionPath {
    // Geometry — SAME shape as the polygon system (local Vector2f + a
    // CoordSpaceHelper for world placement), so it inherits the Eigen-gotcha
    // workaround exactly as shapes do (RectangleCanvasComponent.hpp:24-54).
    CoordSpaceHelper coords;              // world placement of the path
    std::vector<Vector2f> points;         // node positions (path-local)
    std::vector<Vector2f> controlIn;      // tangent offsets from points[i]
    std::vector<Vector2f> controlOut;
    std::vector<uint8_t>  nodeType;       // 0 corner / 1 smooth / 2 cusp

    // Per-node animation channels (parallel to points):
    std::vector<float>    nodeTime;       // normalized arrival time 0..1, monotonic
    std::vector<float>    nodeScale;      // scale at this node (default 1.0), tweens
    std::vector<uint8_t>  nodeEasing;     // TransitionEasing for the segment AFTER i

    // Path-level playback:
    float duration = 2.0f;                // seconds for one full traversal
    // (play-style + trigger are READ from the owning flip-book folder — the path
    //  does not duplicate them.)

    // Transient runtime (NOT serialized/synced) — mirrors FlipbookRuntime:
    double pathProgress = 0.0;            // 0..1 along the traversal
    bool   pathReversing = false;         // ping-pong direction
};
```

- **`nodeTime`** gives pacing: the endpoint's time is the end of the traversal
  ("time of translation set when the end point is selected" — set `duration`
  there). Intermediate nodes' times control accel/hold. Default uniform
  `i/(n-1)`.
- **`nodeScale`** default 1.0, linearly (eased) interpolated between nodes — zynx's
  "scale is just a per-point field."
- **`nodeEasing`** reuses `TransitionEasing` (`Waypoint.hpp:23-29`) +
  `transition_easing_to_bezier_curve()` (`Waypoint.cpp:23-32`) — no new easing code.

## Milestones

### P1 — MotionPath NetObj + folder ownership + save/load (~2 days)

1. **Seam check first (30 min):** confirm the two patterns I'm relying on — (a) a
   layer item / folder owning an extra `NetObjOwnerPtr<T>` (model on `nameData` /
   `displayData` in `DrawingProgramLayerListItem`, incl. `register_class`,
   `reassign_netobj_ids_call`, create-message write/read); (b) how
   `DrawingProgramToolType` tools are registered + made active (model on
   `WaypointTool`), so P2's editor can be a tool.
2. **`MotionPath` type:** the struct above; `register_class` (NetObj
   constructor/update funcs, mirror `Waypoint::register_class`
   `Waypoint.cpp:270-440`); `serialize` for wire; `save_file`/`load_file`.
3. **Folder ownership:** add `NetObjOwnerPtr<MotionPath> motionPath` (null until
   added) to the flip-book folder (`DrawingProgramLayerFolder`), plus
   `has_motion_path()` / `get_motion_path()` / `ensure_motion_path()`. Wire into
   `reassign_netobj_ids_call`, `set_to_erase`, create-message write/read, and
   `get_used_resources` if needed — following the `folderList`/`displayData`
   precedents.
4. **Save/load:** in `DrawingProgramLayerListItem::save_file`/`load_file`, after the
   existing fields, write `bool hasPath` + the path if present, **gated at
   `>= 0.28.0`**. Bump `VersionConstants` **INFPNT000028 → INFPNT000029 /
   0.27.0 → 0.28.0** (map entry in `.cpp`).
5. **Verify:** create a path programmatically on a folder, save, reload, confirm
   the nodes round-trip; older files load with no path (defaults).

### P2 — dedicated node editor (`MotionPathTool`) (~3–4 days)

Model on **`WaypointTool`** (a tool that draws chrome + handles canvas input),
because tools are inherently editor-only (never active in reader mode → the
"never in viewer" requirement is free).

1. **`MotionPathTool` (new `DrawingProgramToolType`)** bound to a folder id it's
   editing. Activated from the flip-book folder GUI (P4 button), not the toolbar.
2. **Draw** (`MotionPathTool::draw`, model `WaypointTool.cpp:61-117`): build the
   `SkPath` from nodes (reuse the edge rule from `RectangleCanvasComponent.cpp:160-186`
   — cubic iff a tangent is non-zero, else line; **open**, not closed), stroke it,
   then draw handles:
   - **diamonds**, not circles (existing handles are circles,
     `EditTool.cpp:390-412` / `DrawingProgram.cpp:1108-1117`): write a small
     `draw_diamond` helper.
   - **green** first node, **red** last node, **yellow** middle nodes; brighter when
     selected. Tangent handles (small dots + arm lines) for curve nodes.
3. **Input** (mirror `EditTool` mouse logic but self-contained — this is the ~2-day
   reimpl Route 3 pays for; path editing is narrow: no fill/mask/affine/marquee):
   - click node → select (single selection is enough for v1);
   - drag node → move (`coords.get_mouse_pos()` → path-local, write `points[i]`,
     apply smooth-node tangent mirroring like `EditTool`'s `mirror`);
   - drag tangent handle → adjust `controlIn/Out[i]`;
   - click on an edge → **insert node** via the de-Casteljau split — **reuse
     `add_polygon_point`** (`RectDrawEditTool.cpp:15-92`, free functions on the same
     node arrays);
   - Make Curve / Make Corner / Smooth↔Cusp → reuse `make_node_curve` /
     `make_node_corner` (same file).
   - Every mutation → sync the owning path NetObj + push undo (a
     `MotionPathUndoAction` swapping the node arrays, model
     `EditCanvasComponentWorldUndoAction` `EditTool.cpp:251-284`).
4. **Per-node inspector** in the tool's toolbox (model `WaypointTool.cpp:194-286`):
   when one node is selected, show `slider_scalar_field` **Time** (0–1) + **Scale**
   (0.1–10) + a `DropDown` **Easing** (`transition_easing_display_names()`), plus
   path-level **Duration** (s) and **Delete Node** / **Done** buttons.
5. **Verify:** draw a path, add/move/curve nodes, set a node's time/scale/easing,
   confirm undo/redo of each; confirm handles vanish on tool switch and in reader
   mode.

### P3 — sampling + playback integration (~2 days)

1. **Sampler** `motion_path_sample(progress) → {Vector2f localPos, float scale}`:
   find the segment `[i, i+1]` where `nodeTime[i] ≤ progress ≤ nodeTime[i+1]`, local
   `u = bezier_ease(nodeEasing[i], (progress − nodeTime[i]) / (nodeTime[i+1] −
   nodeTime[i]))`, evaluate the cubic segment at `u` (reuse the Bernstein eval from
   `flatten_polygon_outline` `RectangleCanvasComponent.cpp:199-227`), and
   `scale = lerp(nodeScale[i], nodeScale[i+1], u)`.
   - **v1 = parametric-per-segment** (above), *not* global arc-length — simpler and
     avoids `SkPathMeasure` entirely; per-node time already gives pacing control.
     Arc-length (constant-speed) is a P3+ refinement (revises §B.12 open-decision
     #4). Flag in the doc.
2. **Advance the path clock** in the flip-book playback tick (extend
   `DrawingProgramLayerListItem::update_flipbook_playback`, the B-M3 function): if
   the folder has a path and the group is playing, advance `pathProgress` over
   `duration` using the **folder's** play-style (once/loop/ping-pong) + the same
   playback gate (reader mode OR preview). Independent of the frame-cycle timer
   (walk cycle loops many times over one traversal).
3. **Apply the transform** in `DrawingProgramLayerFolder::draw_flipbook_frame`:
   sample at `pathProgress` → `{localPos, scale}`; compute the world **delta** =
   `coords.from_space(localPos) − coords.from_space(points[0])`; `canvas->save()`,
   translate by the camera-projected delta, scale by `scale` about the sample point
   (pivot = current sample position), draw the chosen child, `canvas->restore()`.
   - **The fiddly bit** — getting the delta through the camera correctly (path-local
     → world via `coords`, world → screen via the live camera). Cross-check against
     how `CoordSpaceHelper::transform_sk_canvas` / a component's draw composes.
     Budget the care here.
   - The cache is already bypassed while a flip-book is live (B-M2), so the moving
     frame redraws each frame with no extra plumbing.
4. **Verify:** a 4-frame walk cycle travels a curved path, scaling up along it;
   frames keep cycling; once/loop/ping-pong all behave; Preview animates it in
   drawing mode; nothing shows in viewer mode except the animation itself.

### P4 — folder GUI entry + polish + docs (~1 day)

1. **Flip-book folder panel** (`DrawingProgramLayerManagerGUI`, the flip-book
   section from B-M4): add **"Add Motion Path"** (when none) / **"Edit Motion
   Path"** (when present) → creates a default 2-node path spanning the current view
   and activates `MotionPathTool` bound to this folder; and **"Remove Motion
   Path"**.
2. **Default path creation:** 2 nodes (start green, end red) across the view centre,
   uniform node times, scale 1.0, linear easing, `duration` 2s.
3. **Edge cases:** folder stops being a flip-book (path inert but preserved);
   <2 nodes (no motion); path present but group not playing (frames sit at
   `points[0]`, i.e. progress 0).
4. **Docs:** MANUAL/README; cross-link PHASE10 §B.12 ↔ this file; note the v1
   parametric-vs-arc-length choice.

## Open micro-decisions (resolved for v1)

| # | Decision | v1 choice |
|---|---|---|
| Storage | World-graph vs folder-owned | **Folder-owned** `NetObjOwnerPtr<MotionPath>` |
| Editor | reuse EditTool vs dedicated | **Dedicated tool** (Route 3), reuse node *math* only |
| Play-style/trigger | own vs group's | **Follows the group's** |
| Sampling | arc-length vs parametric | **Parametric-per-segment** (arc-length = later) |
| Rotation-along-tangent | in vs out | **Deferred** |
| Paths per group | 1 vs N | **1** |
| Scale pivot | anchor vs sample point | **Sample point** |

## Effort

| Milestone | Est. |
|---|---|
| P1 MotionPath NetObj + folder ownership + save bump | ~2 days |
| P2 MotionPathTool node editor (diamonds, drag/add/tangent, inspector, undo) | ~3–4 days |
| P3 sampler + playback integration (offset + scale the shown frame) | ~2 days |
| P4 folder GUI entry + edge cases + docs | ~1 day |
| **Total** | **~1.5–2 weeks** |

P2 carries the weight (the dedicated interaction) and P3's transform-through-camera
is the correctness-sensitive bit. P1 is mechanical (NetObj + serialization
precedents are well-trodden). Build P1→P2→P3→P4 with a compile+run at each; the
save-format bump lands in P1 so it's exercised early.
