# Inkternity — Frame Animation Design Doc

> **Audience:** the agent (and any human contributor) working inside the Inkternity repo.
>
> **Goal of this doc:** define a minimal frame-animation authoring affordance — a camera-nudge button that snaps the editor view exactly one viewport along a chosen axis, plus a live onion-skin overlay that renders the **selected waypoint's framing rect** at low alpha, anchored to that waypoint's world-space position. **No new persisted state, no file-format change, no NetObj surface.** The artist composes animation chains out of regular waypoints, wired with regular edges, played back via the existing transition + `stopTime` machinery.

## 1. Product summary

A **Next Frame button** on the `WaypointTool` settings panel snap-pans the editor camera by exactly one viewport in the chosen axis (`+X`, `−X`, `+Y`, `−Y`), and an **Onion skin toggle** renders the currently-selected waypoint's framing rect at alpha ≈ 0.35 — **anchored to that waypoint's world-space position**, so it stays glued to the canvas as the artist free-pans / zooms / rotates their view.

That's the entire feature. Animation chains are just sequences of normal waypoints, dropped at successive camera positions. Playback uses the existing transition machinery (`isTransition = true` + a small `stopTime` on each waypoint in the chain).

```
[Frame A]  --(Next Frame +X, draw, drop)-->  [Frame B]  --(Next Frame +X, draw, drop)-->  [Frame C]
   ^                                            ^                                            ^
   selection: A while drawing B                 selection: B while drawing C                 selection: C
   onion shows A at A's world rect              onion shows B at B's world rect              onion off / shows self
```

Use case: a 6-frame walk cycle inside what reads as a single panel; a 4-frame blink; a short reveal. Today the artist has to free-pan to a "reasonable next-frame position" and eyeball alignment. Next Frame gives them a guaranteed-aligned camera step; the world-anchored onion gives them the previous frame as a registration overlay that stays put while they tweak their view.

## 2. Inheritance from existing systems

Everything load-bearing already exists:

- **Camera math** — `CoordSpaceHelper::from_space` projects local-space offsets to world space, handling rotation. Snap-panning one viewport in screen-space `+X` is `coords.pos + (from_space({windowSize.x, 0}) − from_space({0, 0}))`.
- **Camera smooth-move** — `DrawCamera::smooth_move_to` already does animated camera transitions; Next Frame just calls it with a derived target.
- **Multi-camera offscreen render** — `ButtonSelectTool::capture_skin_to_selected` constructs a temporary `DrawData` with its own `cam` and calls `main.draw_world(offCanvas, world, captureDD)` to render the world from any viewpoint. The onion overlay reuses this exact pattern with `transparentBackground = true` + `takingScreenshot = true`.
- **World-anchored canvas transform** — `CoordSpaceHelper::transform_sk_canvas(canvas, drawData)` applies the canvas matrix that maps a coord-space's local coordinates into the current view, accounting for the live camera's pan / zoom / rotation. Every `CanvasComponent::draw` uses this to render at its world location. The onion uses it to place the offscreen image at the source waypoint's world-space framing rect.
- **Per-tool `draw()` hook** — `WaypointTool::draw` is already called post-cache, post-component-render, with the cam-space transform applied. The onion composites in here, before the existing edge-preview and framing-rect drawing.
- **Selection state** — `WaypointGraph::has_selection() / get_selected() / select()` is the existing per-session pointer to "the waypoint the artist is editing." Clicking a marker focuses + selects; dropping a new waypoint selects it. The onion source IS the selected waypoint — no separate pointer.
- **Waypoint drop** — `WaypointTool::drop_waypoint` is the existing single-click "drop a marker at the current camera" path. After the camera has been snap-panned by Next Frame, dropping a waypoint at the new view is the same gesture as dropping one anywhere else.
- **Auto-advance playback** — `Waypoint::isTransition` + `Waypoint::stopTime` (TRANSITIONS.md §5) walks a chain at configurable per-frame delays. Authors toggle `isTransition` on the chain to make it play back; nothing in this feature touches reader mode.

No data model changes. No file-format bump. No NetObj surface.

## 3. Editor surface

The `WaypointTool` settings panel today shows: label, transition speed, easing dropdown, "Transition point" checkbox, stop-time slider. Append two rows below the existing transition-point block:

```
+----------------------------------+
| Label:           [_____________] |
| Transition speed [——|———————]    |
| Easing:          [Ease       ▼]  |
| ☐ Transition point               |
|   Stop time (s)  [—|———————]     |  (existing)
+----------------------------------+
| Frame step                       |
| Axis:    (+X) (−X) (+Y) (−Y)     |  <- radio, tool state
| [ Next frame ]                   |  <- button: snap-pan camera
| ☐ Onion skin                     |  <- toggle, tool state
+----------------------------------+
```

Both axis and onion preference are **tool state on `WaypointTool`** — not persisted, not synced. Closing and reopening the canvas resets to defaults (`+X`, onion off).

Both rows render whether or not a waypoint is selected. Next Frame moves the camera regardless of selection (it just snap-pans the editor view). The onion overlay only renders **when a waypoint is selected** — the selection IS the onion source.

### "Next Frame" semantics

1. Read the current editor camera (`drawP.world.drawData.cam.c`) and the current window size (`drawP.world.main.window.size`).
2. Compute the screen-space offset for the chosen axis: `{±windowSize.x, 0}` or `{0, ±windowSize.y}`.
3. Project to world space via `coords.from_space(offset) − coords.from_space({0,0})` — handles rotation correctly so the step is in *current screen-space* `+X`, not the world's `+X` axis.
4. Build a target `CoordSpaceHelper` with `pos = current.pos + worldOffset`, same `inverseScale` and `rotation`.
5. Call `drawP.world.drawData.cam.smooth_move_to(world, target, windowSize.cast<float>())`. The camera animates to the new position.
6. **Do not modify selection.** If A was selected when the artist clicked Next Frame, A stays selected — the onion stays glued to A while the artist draws frame B at the new camera position.

The artist drops a waypoint there with the existing waypoint-tool click. Edges between successive waypoints are wired the existing way (shift+click on the previous waypoint to add the edge, or use the tree-view). **Frame-animation authoring is just regular waypoint authoring with a camera-nudge button.**

### Onion-skin toggle

When the toggle is on AND a waypoint is selected, every frame `WaypointTool::draw` renders that waypoint's framing rect into an offscreen surface (through *its* camera), then composites it onto the live canvas at the waypoint's world-space framing rect. The image is glued to the canvas, not the viewport — pan, zoom, or rotate the live view and the onion stays put on the canvas underneath.

## 4. Onion-skin overlay — world-anchored render through the selection's camera

### Why world-anchored, not camera-anchored

On an infinite canvas the artist constantly free-pans / zooms / rotates the view while drawing — that's the natural ergonomics of the app. An onion overlay anchored to a *live-camera-relative offset* would slide around the moment the artist nudges their view, destroying its registration value entirely.

The onion has to be anchored in **world space** — locked to the source waypoint's framing rect — so that as the artist moves their view to find a comfortable angle, the onion stays glued to the canvas content it represents. The relationship between "what the previous frame had at world position X" and "what I'm drawing at world position X right now" is preserved no matter how the artist's view drifts.

### Render path

Inside `WaypointTool::draw`, before the existing edge-preview and framing-rect drawing:

1. Bail unless the onion toggle is on.
2. Bail when reader mode is active (`drawData.main->world->readerMode.is_active()`) — onion is editor chrome.
3. Bail unless `wpGraph.has_selection()` resolves to a valid `Waypoint` ref. Call this `src`.
4. Allocate an offscreen raster `SkSurface` sized to `src.get_window_size()` — the offscreen *is* the source frame at its own zoom. (Skip if window size is non-positive — pre-Phase-2 saves with `windowSize == {0,0}` exist; bail out cleanly.)
5. Build `DrawData oc = drawData;` and override its camera with the source frame's camera:
   - `oc.cam.c = src.get_coords()`
   - `oc.cam.set_viewing_area(src.get_window_size().cast<float>())`
   - `oc.takingScreenshot = true` (bypasses the live draw-cache, doesn't poison it)
   - `oc.transparentBackground = true` (offscreen has alpha)
   - `oc.refresh_draw_optimizing_values()`
6. `drawP.world.main.draw_world(off, drawP.world.main.world, oc)` renders into the offscreen — the source frame's content at the source frame's zoom.
7. `surface->makeImageSnapshot()` → `sk_sp<SkImage>`.
8. Composite onto the live canvas at the source's world-space framing rect:
   - `canvas->save()`
   - `src.get_coords().transform_sk_canvas(canvas, drawData)` — canvas matrix now maps the source frame's local coordinates into the live view, with all of the live camera's pan / zoom / rotation applied.
   - `canvas->drawImageRect(img, srcRect, SkRect::MakeWH(windowSize.x, windowSize.y), sampling, &paint, kFast)` — image lands exactly on the source frame's framing rect in world space.
   - `canvas->restore()`

Sketch:

```cpp
void WaypointTool::draw_onion_skin(SkCanvas* canvas, const DrawData& drawData) {
    if (!onionSkinOn) return;
    if (drawData.main && drawData.main->world && drawData.main->world->readerMode.is_active())
        return;
    if (!drawP.world.wpGraph.has_selection()) return;
    auto srcRef = drawP.world.netObjMan
        .get_obj_temporary_ref_from_id<Waypoint>(drawP.world.wpGraph.get_selected());
    if (!srcRef) return;

    const Vector<int32_t, 2> ws = srcRef->get_window_size();
    if (ws.x() <= 0 || ws.y() <= 0) return;

    SkImageInfo info = SkImageInfo::Make(ws.x(), ws.y(),
                                         kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return;
    SkCanvas* off = surface->getCanvas();

    DrawData oc = drawData;
    oc.cam.c = srcRef->get_coords();
    oc.cam.set_viewing_area(ws.cast<float>());
    oc.takingScreenshot       = true;
    oc.transparentBackground  = true;
    oc.refresh_draw_optimizing_values();
    drawP.world.main.draw_world(off, drawP.world.main.world, oc);

    sk_sp<SkImage> img = surface->makeImageSnapshot();
    if (!img) return;

    canvas->save();
    srcRef->get_coords().transform_sk_canvas(canvas, drawData);
    SkPaint p;
    p.setAlphaf(ONION_SKIN_ALPHA);    // default 0.35
    const SkRect rect = SkRect::MakeWH(static_cast<float>(ws.x()), static_cast<float>(ws.y()));
    canvas->drawImageRect(img, rect, rect, SkSamplingOptions(), &p,
                          SkCanvas::kFast_SrcRectConstraint);
    canvas->restore();
}
```

The `transform_sk_canvas` call is the load-bearing piece — it's what makes the onion stay glued to the canvas as the artist nudges their view. The "render world from a different camera" call IS the multi-camera primitive. No new abstraction.

### Workflow note: selection drives the onion source

The onion source is **whatever waypoint is currently selected.** Two implications:

1. **Selection is sticky across Next Frame.** Clicking Next Frame moves the camera but does not change selection. If A is selected and the artist clicks Next Frame, A stays selected — so the onion stays glued to A while the artist draws frame B at the new camera position.
2. **Dropping a new waypoint auto-selects it (existing behavior).** The moment the artist drops waypoint B, B becomes the selected waypoint, and the onion switches to showing B's framing rect. Since B's coords roughly match the camera B was just dropped from, B's onion overlays the live view at roughly the same position — a faint "self-overlay" that's near-neutral visually (more on this in §6).

Recommended workflow for an artist building a 6-frame strip:

```
1. Drop A.                  Toggle onion on (it overlays A onto itself harmlessly).
2. Draw frame A.
3. Next Frame +X.           Camera pans. A still selected. Onion now shows A glued to its world position (appears one viewport to the left in the new view).
4. Draw frame B.            Onion against A registers the new pose.
5. Drop B.                  B selected. Onion now overlays B onto itself.
6. Next Frame +X.           Camera pans. B still selected. Onion shows B glued to its position.
7. Draw frame C.            Onion against B registers the new pose.
8. Drop C.                  ... and so on.
```

The pattern is: **Next Frame → draw → drop**, repeated. The drop comes *last* so the previous frame stays selected (and stays the onion source) while the artist is registering against it.

If the artist wants to revise an earlier frame, they click that frame's marker (or its tree-view row) to select it — at which point the onion follows the new selection.

### Why a `CoordSpaceHelper`+ID-based source, not a chain-edge lookup

We considered making the onion source "the previous waypoint along an incoming edge to the selected waypoint." That has nicer chain semantics — but requires the artist to wire the edge before the onion appears, and forces a "first incoming edge wins" tiebreaker for branches. Selection is more direct: the artist already clicks markers to focus them; the selection IS the artist's intent about what they're working against.

### Performance caveats

- Offscreen surface reallocated every frame the toggle is on. Surface size = source frame's `windowSize`, typically the artist's window size at drop time (e.g. 1920×1080 ≈ 8 MB). Skia's raster pool handles this on desktop; on Android, polish path is a tool-cached `sk_sp<SkSurface>` keyed by `(sourceWaypointId, windowSize)` and recreated only on selection change or resize.
- `takingScreenshot = true` bypasses the draw-cache fast-path in `DrawingProgram::draw` — correct here, the offscreen pass shouldn't share state with the live cam's cached surfaces.
- Onion only renders while toggle is on AND a waypoint is selected. Default cost is zero.

## 5. Edge cases

| Case | Behavior |
|---|---|
| Onion toggle on, no waypoint selected | Onion does not render. Toggle stays on; renders again when a waypoint is selected. |
| Onion source has `windowSize == {0,0}` (pre-Phase-2 save artefact) | Bail without rendering. Don't crash. |
| Onion source has been deleted (dangling NetObjID) | `get_obj_temporary_ref_from_id` returns null; bail without rendering. |
| Artist pans the live view while onion is on | Onion stays glued to the source's world-space rect; the live view moves around the onion. **This is the entire point.** |
| Artist zooms the live view in / out | Onion scales with the canvas (since `transform_sk_canvas` includes the live camera's zoom). At extreme zoom-out the onion shrinks to subpixel; at extreme zoom-in it pixelates from the offscreen's source-zoom resolution. Acceptable for v1; if it bites, render offscreen at 2× source size for crisper zoom-in. |
| Artist rotates the live view | `transform_sk_canvas` rotates the onion correctly with the canvas. |
| Source frame's `coords.rotation` differs from live camera's rotation | Onion appears rotated relative to the live view — correct, since the source frame was authored at that rotation. |
| Source frame is in the live viewport (selection just dropped) | Onion overlays its own content at near-zero alpha effect (see §6 self-overlay note). Harmless. |
| Reader mode active | Onion overlay bails. Next Frame button still functions in editor (reader mode toggles tool visibility separately). |
| Rotated camera when clicking Next Frame | Offset projects through `coords.from_space`, so the step is in *screen-space* `+X`, not world `+X`. Verified manually with a 45°-rotated parent: stepping `+X` lands the new view immediately to the right of the previous one *on screen*. |
| Crossing a `scale_up` while panning a chain | Camera math + source waypoint's coords are both `WorldVec`/`WorldScalar`; `scale_up` rescales both halves equally; no special handling. |
| Two artists in collab session, both toggle onion skin | Each artist's onion toggle is local. They don't see each other's onion overlay. The source is each artist's *own* selection, which is local too. No sync. |
| Artist resizes window mid-chain | Subsequent Next Frame steps use the new viewport size — frames after the resize have a different step magnitude than frames before. Onion offscreen size is still the source frame's `windowSize` (captured when it was dropped), independent of the live window. |
| Artist selects a waypoint while drawing | Selection change is immediate — next frame, onion source switches. No mid-stroke disruption beyond what selection already does. |

## 6. Risks

- **Onion-skin re-render cost.** One full `draw_world` into a `windowSize`-sized offscreen per frame while toggle is on. Worst case: busy comic page rendered at full source-frame size. Toggle defaults to off; only fires when artist asks for it AND a waypoint is selected. Mitigation if slow: surface cache keyed by `(sourceWaypointId, windowSize)`; or downscale offscreen 0.5× and rely on alpha to hide the resolution loss.
- **Axis-offset math on rotated cameras (Next Frame).** Project through `coords.from_space`, not the world axes. Manual test: 45°-rotated camera, step `+X`, confirm new view is screen-right not world-right.
- **Self-overlay slight bloom.** Right after dropping a new waypoint, the live camera matches the new waypoint's coords closely. The onion renders that waypoint's content overlaid on itself. With premultiplied-alpha math and identical content, the blend result is near-identical to the original; in practice a slight bloom from re-rendered antialiased edges. Not actively harmful, but the artist may want to toggle onion off briefly when working *at* the new frame's position. Acceptable for v1.
- **Workflow ordering matters.** Drop-the-waypoint-last is the natural order; drop-first puts the artist in the awkward "self-overlay" state while drawing the new frame. The settings-panel onion checkbox sits right next to Next Frame, so a one-click toggle covers the bad cases. Document the ordering in MANUAL.md (F6).
- **Live-cam zoom + source-zoom mismatch on pixelation.** If the artist drew the source frame at a large zoom (high inverseScale) and is now zoomed-in further on the live view, the onion image's resolution can be insufficient. Offscreen render at 2× source size is a cheap mitigation; defer until artists hit it.
- **No persisted authoring intent.** Frames have no marker, no in-file record of "this is part of an animation chain." If the artist wants to identify a chain later, they label the waypoints ("walk-1, walk-2…") or eyeball the framing-rect alignment in the tree view. Acceptable for v1; revisit if artists hit it.
- **Hard-cut playback isn't supported by the existing transition machinery.** Playing back the chain uses `isTransition + stopTime + transitionSpeedMultiplier` — frames smooth-pan between camera positions. At small `stopTime` + max speed multiplier (2.0) it's a fast cross-pan, not a flipbook hard cut. **Live with this in v1**; if artists want true hard cuts after playing with the feature, add an `instantJump` flag on transitions, or widen the speed-multiplier range. Tracked in §8.

## 7. Milestones

| | Deliverable |
|---|---|
| F1 | `WaypointTool` state: `axisChoice` (enum), `onionSkinOn` (bool). Both default per session, not persisted. |
| F2 | Settings panel: 4-way axis radio + Next Frame button + Onion skin checkbox. Tool state binding. |
| F3 | Next Frame click handler: compute one-viewport offset in screen-space, project to world via `coords.from_space`, build target `CoordSpaceHelper`, call `cam.smooth_move_to`. Verify selection is *not* modified. |
| F4 | Onion-skin overlay in `WaypointTool::draw`: resolve selected waypoint as source, allocate offscreen at source's `windowSize`, render world through source's camera, composite via `source.coords.transform_sk_canvas` + alpha paint; reader-mode bail; selection / size guards. |
| F5 | Manual test pass: drop A, draw, Next Frame +X (verify A stays selected and onion stays glued to A); draw B with onion against A; drop B, verify self-overlay is visually near-neutral; build a 6-frame strip; rotate parent camera 45° and verify Next Frame pans screen-right; live-camera zoom-in / zoom-out / rotate while onion is on (verify onion stays glued to source's world position); reader-mode playback with `isTransition = true` + `stopTime = 0.1`; resize window mid-chain. |
| F6 | Docs: MANUAL.md "Frame animation" section covers Next Frame, onion toggle, the **draw-before-drop** workflow ordering, and recommended `stopTime` / `transitionSpeedMultiplier` settings for chain playback. README adds a bullet under "What Inkternity adds." |

Total surface: ~150 lines of code + docs. Touch points: `WaypointTool.{hpp,cpp}`, `docs/MANUAL.md`, `README.md`. No `Waypoint.*`, no `VersionConstants.*`, no NetObj registration changes.

F3 is the Next Frame math (must be right under rotation). F4 is the visually novel piece — `transform_sk_canvas` makes it mechanically simple, but the manual test pass in F5 is where any matrix / transform errors will surface.

## 8. Deferred until v1 is playable

These came up in the design discussion and are deliberately not in v1 — revisit after the minimal feature is in artists' hands.

- **Hard-cut playback.** True flipbook feel requires an `instantJump`-on-transition flag or a wider `transitionSpeedMultiplier` range. Easiest tweak after playing with v1's smooth-pan playback: widen the multiplier to `[0.1, 10.0]` (one-line change in `Waypoint.hpp`), letting the artist crank fast enough that frames *feel* like cuts even though they're technically smooth.
- **Per-frame onion-skin state.** Persisting "onion skin was on for this frame" would mean a flag on `Waypoint` and a file-format bump. Skipped in v1; the tool-session toggle is enough.
- **Marker variant for "this is an anim frame."** No data model means no axis-arrow on the marker. Artists label frames ("frame1, frame2") or scan the tree view. Reconsider if scanning becomes a real pain.
- **Onion-stack depth > 1.** Showing N-1 + N-2 with progressively dimmer alpha and color tint (blue past / red future). v1's source is "the selected waypoint" — a single anchor. Multi-source onion would need a separate set of pinned ids, distinct from selection. Defer.
- **Onion source ≠ selection.** A workflow where the artist locks an onion source explicitly (separate from selection) so that dropping new waypoints doesn't switch the onion. v1 collapses these into one for simplicity; if the draw-before-drop ordering proves annoying, split the pointers in v2.
- **Offscreen-at-2×-source-zoom for crisp zoom-in.** v1 renders the onion at the source's native `windowSize`. If artists report pixelation when zooming the live view in past the source's zoom, render the offscreen at 2× and rely on Skia's downsampling. Cheap, but more memory.
- **Auto-edge wiring on Next Frame.** If artists end up always going Next-Frame → drop-waypoint → shift+click-prev to wire the edge, that's a three-step gesture worth collapsing. Add a "wire from previously-selected waypoint" combined action later if the friction shows up.
- **Auto-drop waypoint after Next Frame.** Same shape: collapse the two gestures into one if artists ask. v1 keeps the move and the drop separate so Next Frame doubles as a pure camera nudge.
- **FPS-aware bulk edit.** "Set every transition in this chain to 12 fps" — would compute `stopTime` for the artist. Convenience; defer.
- **Onion-skin shown in reader mode.** It's a different feature (motion-blur cinematic effect). Out of scope.

## 9. Out of scope

- **Tweening / interpolation between frames.** Every frame is hand-drawn.
- **Audio sync, per-frame timing markers, scrub bar.** None of these.
- **Export to GIF/MP4.** Possible follow-up via the existing `WorldScreenshot` infrastructure (loop the chain, capture each frame, encode). Not v1.
