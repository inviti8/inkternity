# PHASE 10 — Flip-book layer groups + motion paths

## Status

**SHIPPED (2026-07-09).** Flip-book layer groups + bezier **motion paths** — the
"animate a group of frames" authoring feature. A folder can be set to **Flip-Book
Group** mode (its child layers become animation frames): fps, play style
(once/loop/ping-pong) × trigger (auto-on-view / on-touch), invert direction, onion
skin, and a drawing-mode Preview. An optional **Motion Path** (see
[MOTION-PATH.md]) makes the whole group **travel a drawn bezier curve** as it
plays — per-node timing (seconds), scale, easing, and rotation-along-tangent —
while its frames cycle independently. Folder Mode is a single dropdown (Normal /
Parallax Scene / Flip-Book — mutually exclusive). Save format at **INFPNT000029 /
0.28.0**.

The other feature originally scoped in this phase — **CV hand-tracking for the
armature's hands** — was **moved to [PHASE11.md]** as future work (zynx,
2026-07-09); its full scoping is preserved there.

The design/rationale below (Feature B) is retained as the delivered spec; the
motion-path build plan is [MOTION-PATH.md].

---
---

# Flip-book layer group + motion path (delivered spec)

## Implementation status

**SHIPPED, 2026-07-09.** Flip-book core (B-M1…B-M4) + the **motion-path** approach
to transform animation (the B-M5 stretch, built out as its own sub-feature — see
[MOTION-PATH.md]) + onion skin. Delivered: folder Flip-Book mode (fps / play-style
× trigger / invert / onion / Preview, Folder Mode dropdown), draw-one-frame + cache
bypass, viewer-gated playback clock, and — via a drawn bezier **Motion Path** — the
whole group travelling a curve with per-node **seconds / scale / easing /
rotation-along-tangent** while frames cycle independently. Save format
**INFPNT000029 / 0.28.0**. One deviation from the plan below: `DrawData::FlipbookGroup`
was **not** needed — frame selection is local to the folder (runtime `frameIndex`
on `DrawingProgramLayerFolder::FlipbookRuntime`); `frameIndex` is the panel/child
index directly, and invert flips the advance direction, not a coordinate remap.
The §B.11 requester/record sketch was superseded by the motion path (§B.12); §B.5's
onion-skin "out of scope" was reversed once it became clear a flip-book shows only
one frame at a time (onion is now on by default while editing).

**Parallax ⊥ flip-book (mutual exclusion, zynx 2026-07-09).** Parallax (all
children drawn as depth planes *at once*) and flip-book (one child frame *at a
time*) are opposite interpretations of the same folder's children, so a folder
can be **at most one**. The folder panel exposes a single **Folder Mode**
dropdown — *Normal / Parallax Scene / Flip-Book* — instead of two independent
checkboxes; switching modes disables the other. To combine the effects, **nest**
(a flip-book whose frames are parallax sub-folders, or a parallax plane that is
itself a flip-book) — the two group behaviors then live on different folders and
don't collide. The underlying `parallaxRefScale` and `flipbookFps` fields stay
independent (so undo restores each cleanly); only the UI enforces exclusivity.

## B.1 Product summary

A **flip-book group** is a folder variant whose child layers are treated as
sequential **animation frames**. The folder carries tunable properties:

- **fps** — frames per second.
- **play style** (how it advances) — **play-once**, **loop**, or **ping-pong**.
- **trigger** (what starts a playthrough) — **auto** (plays when the group's region
  enters view) or **on-touch** (waits for a tap on the group in viewer mode). These
  two axes are **orthogonal** and deliberately **match the particle-FX modes**
  (`ParticlePlayMode { PARTICLE_PLAY_AUTO, PARTICLE_PLAY_ON_TOUCH }`,
  `src/CanvasComponents/ParticleCanvasComponent.hpp:11`) so the two systems read the
  same to an artist (zynx, 2026-07-09).
- **frame order** — the default order is **top (first) → bottom (last)** in the
  layer panel: the topmost child layer is frame 1. An **invert-direction** toggle
  plays bottom → top instead (zynx, 2026-07-09). (Ping-pong bounces between the two
  ends regardless; invert just picks which end is frame 1.)
- **Stretch goal** — per-frame **position + scale keys** (a frame can be offset /
  scaled relative to the group), so the flip-book can also translate/zoom as it
  plays.

The old single "play mode" (once-on-camera-enter / loop / ping-pong) is now
**play style × trigger**: e.g. *once + auto* = the former "play-once-on-camera-enter";
*loop + on-touch* = start looping when tapped.

At draw time, instead of compositing all children, the group shows **only the
current frame's layer**; a per-frame clock advances the index (in panel order,
or reversed when invert-direction is set). This is the **in-place hard-cut
flipbook** — the complement to the existing waypoint-based `FRAME_ANIM.md` (which
scatters frames across the canvas and smooth-pans between them; see §B.8).

**Playback is viewer-mode-only (zynx, 2026-07-09).** The play modes
(once-on-camera-enter / loop / ping-pong) auto-run **only while reader/viewer mode
is active** — a flip-book must never animate out from under the artist while
they're drawing. In drawing mode the group is **static** by default (shows a
single frame the artist can edit; see §B.3). A per-group **"Preview" play/stop
toggle** in the group UI lets the artist *temporarily* animate the flip-book in
drawing mode to debug the timing — a transient, non-persisted override that stops
when toggled off or when they interact with a frame.

## B.2 Why this is lower-risk: the parallax-group precedent

PARALLAX-SCENES already added a **group-level behavior** to folders and shipped
it (INFPNT000021 / 0.20.0). A flip-book group is the *same six-part pattern*, so
this is pattern-matching, not net-new architecture:

| # | Parallax mechanism | Flip-book mirror |
|---|---|---|
| 1 | Fields on the shared `DisplayData` (`parallaxAnchorX/Y`, `parallaxRefScale`; `refScale==0` = "not a group") — `DrawingProgramLayerListItem.hpp:130-147` | Add `float fps`, `uint8_t playStyle`, `uint8_t triggerMode`, `bool invertDirection`, runtime `frameIndex`; sentinel `fps != 0` = "is a flip-book group" |
| 2 | Mirror into `MetaInfo` (undo/edit snapshot), `operator==` defaulted — `.hpp:17-41` | Same fields in `MetaInfo` → edits auto-undoable via `editing_layer_check()` diff |
| 3 | Context threaded through `DrawData` (`DrawData::ParallaxGroup`), folder injects into a subtree `DrawData` copy — `DrawData.hpp:33-38`, `DrawingProgramLayerListItem.cpp:262-273` | `DrawData::FlipbookGroup { active; activeFrameIndex; }`; folder injects; children read to draw/skip |
| 4 | Cache bypass while a group is live — `any_visible_parallax_layer`, `DrawingProgramLayerManager.cpp:150-157` | Flip-book-aware descendant test → bypass/invalidate the shared cache while animating |
| 5 | Folder-only GUI section ("Parallax Scene") — `DrawingProgramLayerManagerGUI.cpp:465-518` | Sibling "Flip-Book Group" section: fps slider + play-mode dropdown |
| 6 | `scale_up` rescales world-space scalars (both undo + live paths) | Only needed for the stretch position/scale keys (WorldScalar pairs) |

**Chosen approach: Option A (folder `DisplayData` fields + sentinel), NOT a new
`LayerKind`.** `LayerKind` is leaf-only in meaning today and immutable after
construction; no folder code inspects it. Mirroring parallax (a *state* on the
folder, not a kind) is less invasive and lets any folder become a flip-book.

## B.3 The one genuinely careful bit: don't drive playback via the `visible` flag

`DisplayData::visible` (`DrawingProgramLayerListItem.hpp:132`, read at draw `:254`)
is **persisted, net-synced, and cache-invalidating** — mutating it every frame
would spam network updates and pollute save state. Instead, compute a **transient,
runtime-only "effective visibility"** at draw time from the group's current
`frameIndex` (exactly how reader-mode transiently hides `SKETCH` layers,
`DrawingProgramLayerListItem.cpp:252-253`). The stored `visible` flag stays the
artist's authored per-frame toggle; the flip-book's frame selection is a separate,
non-persisted overlay applied only while the group is a live flip-book.

**Which frame shows when (display state).** The group's effective frame is picked
by context, all via that same transient overlay:

| Context | What the group shows |
|---|---|
| **Viewer/reader mode, trigger=auto** | Plays per play style (once / loop / ping-pong) the moment the region enters view — the animating `frameIndex`. |
| **Viewer/reader mode, trigger=on-touch** | Holds frame 1 until tapped, then plays per play style (§B.4). |
| **Drawing mode, Preview OFF** (default) | **Static** — a single "edit frame": the currently-selected/edited child if it belongs to the group, else the group's last-shown index (default 0). The artist pages frames by clicking them in the layer panel and edits in place. |
| **Drawing mode, Preview ON** | Temporarily animates per fps + play style (debug only), ignoring the trigger axis and selection; stops on toggle-off or on interacting with a frame. |

## B.4 Playback clock + triggers (auto / on-touch)

- **Per-frame tick — gated to playback contexts.** Advance the flip-book clock in
  `World::focus_update` (`src/World.cpp:336-349`, where `readerMode.update(deltaTime)`
  and the camera update already run each frame) or inside `DrawingProgram::update`
  — **but only when `world.readerMode.is_active()` OR the group's transient
  `previewPlaying` override is set** (zynx: real playback is viewer-mode-only; the
  Preview toggle is the drawing-mode debug escape hatch). In plain drawing mode
  with preview off, the clock does not advance and the group holds its edit frame
  (§B.3). `deltaTime` is the frame delta; `fps` → frame step. The **play style**
  (once / loop / ping-pong) is a tiny state machine structurally identical to
  `ReaderMode`'s `TransitionPhase` countdown (`src/ReaderMode/ReaderMode.hpp:123-141`);
  the **trigger** decides *when that state machine starts*.
- **trigger = auto — reuse the existing on-screen test.**
  `DrawCamera::viewingAreaGenerousCollider` (an `AABB<WorldScalar>` recomputed each
  frame, `DrawCamera.cpp:31-37`) is already the culling primitive
  (`CanvasComponentContainer::should_draw` → `SCollision::collide(worldAABB,
  cam.viewingAreaGenerousCollider)`, `CanvasComponentContainer.cpp:160`). Compute
  the group's world AABB (union of its children's bounds) and start on the **rising
  edge** (`onScreen && !wasOnScreen`). This is exactly `PARTICLE_PLAY_AUTO`'s
  "play on becoming visible" (`ParticleCanvasComponent.cpp:100`). No waypoint
  wiring needed.
- **trigger = on-touch — reuse the particle touch dispatch.** A reader-mode tap
  already routes to `DrawingProgram::trigger_touch_particles(button.pos)`
  (`World.cpp:490-495` → `DrawingProgram.cpp:986`), which hit-tests components under
  the cursor and calls `trigger_touch()`. Generalize that path (or add a sibling)
  to also hit flip-book groups whose world AABB contains the tap, setting a
  transient `pendingTouch`/`playing` flag that starts the play-style state machine
  next tick — mirroring `ParticleCanvasComponent::trigger_touch()` +
  `pendingTouch` (`ParticleCanvasComponent.cpp:97-99,151`). Touch triggering is
  already reader-mode-only, which keeps the viewer-mode-only rule intact.
- **On-touch × play-style semantics.** The trigger only *starts* the state
  machine; the **play style decides whether it ever ends** — no stop/toggle
  behavior. *once* → one pass, then rest on frame 1. *loop* / *ping-pong* → runs
  **forever** after the tap (a loop-on-touch is a valid intent — a spinning icon,
  a looping banner; an artist who wants it to stop chooses *once*). This mirrors
  particle FX **exactly**: there, finite-vs-continuous is baked into the effect
  asset and the trigger is orthogonal — here, play style is our "baked-in"
  finite/continuous choice (zynx, 2026-07-09). A re-tap simply **restarts** the
  current playthrough, matching particles ("a touch always (re)plays, regardless
  of mode", `ParticleCanvasComponent.cpp:98`).
- **The Preview override is runtime-only.** `previewPlaying` is a transient
  per-group bool — **not** in `DisplayData`/`MetaInfo`, not saved, not synced —
  toggled by the group-UI Preview button. Same "session state, not file state"
  class as the reader-mode toggle itself; resets on file close. Preview ignores the
  trigger axis (it just plays the style).

## B.5 Build (milestones)

1. **B-M1 — data model + save bump.** Add `fps` (`float`), `playStyle` (`uint8_t`
   — once/loop/ping-pong), `triggerMode` (`uint8_t` — auto/on-touch, mirroring
   `ParticlePlayMode`), and `invertDirection` (`bool`) to `DisplayData` (append to
   `serialize`, `DrawingProgramLayerListItem.hpp:144`) and `MetaInfo`;
   getters/setters with the `send_update_to_all<DisplayData>` net-update pattern;
   `is_flipbook_group()` sentinel (model on `is_parallax_group()`, `.cpp:426`).
   Bump `VersionConstants.hpp` header **INFPNT000027 → INFPNT000028**, version
   **0.26.0 → 0.27.0**; add the `>= 0.27.0` load gate
   (`DrawingProgramLayerListItem.cpp:192-209`).
2. **B-M2 — draw: show only the active frame.** `DrawData::FlipbookGroup`; folder
   injects the active index (`DrawingProgramLayerListItem.cpp:262-273`), children
   draw/skip via transient effective-visibility (§B.3); flip-book-aware cache
   bypass (`DrawingProgramLayerManager.cpp:150-157`). **Confirm the panel↔`folderList`
   index mapping here** — `DrawingProgramLayerFolder::draw` iterates `*folderList
   | reverse` for compositing, so pin down which end of `folderList` is the panel's
   top row and map "frame 1 = top" (default) / reversed (invert) onto it. Static
   "show frame N" correct before any animation.
3. **B-M3 — playback clock + play styles + triggers (viewer-gated).** Advance
   frame in `World::focus_update` **only when `readerMode.is_active()` or
   `previewPlaying`** (§B.4); once / loop / ping-pong play-style state machine;
   **trigger=auto** camera-enter rising edge **and trigger=on-touch** via the
   generalized `trigger_touch_particles` path (trigger only *starts* the play
   style; loop/ping-pong run until they leave the finite/continuous behavior of the
   style itself — §B.4). The transient `previewPlaying` per-group override
   (runtime-only) + the "hold edit frame in drawing mode" static path.
4. **B-M4 — GUI.** "Flip-Book Group" section in the folder branch
   (`DrawingProgramLayerManagerGUI.cpp:465-518`): fps `slider_scalar_field`, a
   **play-style `DropDown`** (once/loop/ping-pong), a **trigger `DropDown`**
   (auto/on-touch — same labels as the particle-brush tool for consistency,
   `ParticleBrushTool`), an **invert-direction `checkbox_field`** (default off =
   top→bottom), a **Preview play/stop `text_button`** (drives the transient
   `previewPlaying` override — debug the animation without entering reader mode),
   an enable affordance (checkbox or "New flip-book group" button at `:408`), a row
   badge (`:114-121`). Edit-state members on the GUI class (`.hpp:30-39`); undo is
   automatic once the *persisted* fields (fps/playStyle/triggerMode/invert) are in
   `MetaInfo` — the Preview toggle is session state, so it's **not** undoable and
   not in `MetaInfo`.
5. **B-M5 (stretch) — per-frame position/scale keys.** Per-child offset (**WorldScalar
   pair**, never `WorldVec` — the Eigen/undo gotcha, `.hpp:31-35`) + `float` scale;
   apply in the child's `DrawData` transform during flip-book draw; rescale the
   offsets in **both** `scale_up` paths (`.cpp:161-173` and undo `:13-27`, exactly
   as parallax rescales its anchor scalars).
6. **B-M6 — polish + docs.** Edge cases (empty group, single frame, frame
   added/removed mid-play, collab), MANUAL/README, reconcile the docs with
   `FRAME_ANIM.md` (§B.8).

## B.6 Risks

- **Cache correctness while animating.** A group swapping its visible child each
  frame can't live in the shared draw cache — needs the parallax-style bypass /
  per-frame invalidation, or the region goes stale. Lowest-risk: bypass the cache
  for a live flip-book's subtree (parallax already proves the pattern). **The
  viewer-mode gate helps here:** in ordinary drawing mode the group is static
  (§B.3), so the bypass only kicks in during reader-mode playback or while Preview
  is on — the common editing case keeps full cache benefit and only the frame
  being edited is live.
- **`visible`-flag misuse.** Driving playback through the persisted flag spams net
  + save. Mitigated by the transient effective-visibility (§B.3) — call this out
  in the impl.
- **Collab semantics.** Is playback local (like each artist's own reader-mode
  view) or synced? Recommend **local, non-synced** for v1 (fps/playStyle/triggerMode
  are synced authored state; the *current frame index* + on-touch playing-state are
  per-viewer runtime things) — matches
  how parallax derived-cameras and reader-mode are per-viewer.
- **Interaction with the draw cache's incremental BVH** (recent perf work): a
  flip-book region redrawing each frame is fine (it bypasses the cache), but
  confirm it doesn't force full BVH rebuilds — it shouldn't, since it changes
  visibility, not component membership.

## B.7 Effort estimate

| Work | Est. |
|---|---|
| B-M1 data model + MetaInfo + save bump | ~1 day |
| B-M2 draw active-frame-only + cache bypass | ~1.5–2 days |
| B-M3 playback clock + play styles + auto/on-touch triggers | ~2–2.5 days |
| B-M4 folder GUI (fps + play style + trigger + invert + preview + badge) | ~1 day |
| B-M6 polish + docs | ~1 day |
| **Core subtotal** | **~1–1.5 weeks** |
| B-M5 stretch: per-frame position/scale keys | ~2–3 days |

## B.8 Reconcile with the existing `FRAME_ANIM.md`

`docs/design/FRAME_ANIM.md` already designs frame animation — but as **waypoint
chains**: each frame is a normal `Waypoint` at a stepped camera position, played
via reader-mode `isTransition` + `stopTime`, explicitly *no new persisted state /
no file-format change*. It's a **camera-motion + onion-skin** feature, orthogonal
to this one:

- **FRAME_ANIM** = frames scattered across the canvas, camera pans between them
  (smooth transitions; §8 there explicitly defers true hard cuts).
- **Flip-book group (this)** = frames superimposed in one folder, swapped in place
  = the **hard-cut flipbook** FRAME_ANIM couldn't express.

They coexist and complement. Position this feature as the in-place counterpart,
and cross-link both docs in B-M6. (FRAME_ANIM's onion-skin is *its own* feature —
the flip-book group deliberately skips onion-skin; see §B.9.)

## B.9 Out of scope (v1)

- **Tweening / interpolation between frames** — every frame is an authored layer;
  play mode swaps them (hard cut). Position/scale keying (B-M5) interpolates the
  *transform*, not the pixels.
- **Onion-skin between frames — RE-ADDED (zynx, 2026-07-09).** Initially dropped
  (opacity-as-onion), but a flip-book shows **only one frame at a time**, so an
  artist editing a frame can't see the one below/above at all. Now: while *editing*
  a flip-book (drawing mode, not playing), the frame directly below + above the
  edit frame ghost at 0.5 alpha (`DrawingProgramLayerFolder::draw_flipbook_frame`);
  a per-group **Onion skin** toggle (transient, default on) in the panel. Only the
  group being edited ghosts (its editing layer is a direct child), to avoid
  cluttering every flip-book on the canvas.
- **Audio sync / per-frame timing markers / scrub bar.**
- **Export to GIF/MP4** — possible follow-up via `WorldScreenshot` (loop the group,
  capture each frame). Not v1.
- **Nested flip-books** — a flip-book inside a flip-book frame; defer until asked
  (the `DrawData` injection is nearest-ancestor-wins like parallax, so it wouldn't
  crash, but the semantics need thought).

## B.10 Backward compatibility

New folder fields are **appended + version-gated** (`>= 0.27.0`), so older
canvases load with defaults (`fps == 0` → not a flip-book; the folder composites
all children exactly as today). Unlike a new component *type*, appended
`DisplayData` fields on an existing structure mean **older builds still open new
canvases** — a flip-book group just reads as a plain folder there (all frames
composited, no animation). Mixed-version collab still requires matching builds for
the wire `DisplayData::serialize` (the standing caveat at
`DrawingProgramLayerListItem.hpp:137-139`).

## B.11 Stretch (B-M5) design sketch — transform animation without a timeline

**Status: design sketch, not scheduled.** This expands the B-M5 stretch (per-frame
position/scale keys) into an actual authoring model, because "animate a transform"
normally implies a timeline/curve editor — which Inkternity doesn't have and
doesn't want. Grounded in a research pass on **Deluxe Paint (Amiga III/IV)**, which
solved exactly this: it authored motion with **no timeline**, via two on-canvas
gestures. We modernize both. (Sources: DPaint IV manual + PyDPainter mechanics
reference; see the 2026-07-09 research note.)

### The problem

Each flip-book frame is a child layer drawn in place. B-M5 wants each frame to also
carry a **transform** (offset / scale / rotation) so the sequence can travel, zoom,
or spin as it plays — a bouncing ball, a title sliding in, a spinning icon. But we
have **no timeline UI** and don't want one. DPaint is the proof that you don't need
one.

### Two timeline-free authoring models (both from DPaint, modernized)

**Model A — Record motion (DPaint "Animpainting", modernized).** The artist selects
the flip-book group, hits **Record Motion**, and drags on the canvas; we sample the
pointer and write each sample as the **next frame's transform key**, auto-advancing
frames as they drag (DPaint auto-stepped frames while you painted). The hand gesture
*is* the motion path.
- *DPaint did:* capture the path's geometry, but threw away velocity — playback was
  a uniform FPS, so speed was encoded only as sample spacing.
- *We add (the modern wins):* (1) **keep true timing optionally** — timestamp
  samples so pauses/speed-ups survive, with a toggle to "resample to uniform
  frames" (DPaint behaviour) vs. "keep recorded timing"; (2) **non-destructive +
  editable** — writes editable per-frame keys, not a baked result, so a frame's key
  can be nudged or the whole path re-recorded; (3) **smoothing** — optional
  Chaikin/Catmull-Rom resample to de-jitter a shaky hand; (4) record **scale** (a
  modifier while dragging) and rotation, not just position.

**Model B — Transform-over-N (DPaint "Move requester", modernized).** A small inline
requester on the flip-book group: **ΔX, ΔY, Δscale (%), Δrotation (°)** applied
across the N frames, plus **ease** (none / in / out / in-out). It distributes the
delta over the frames (frame *i* gets the eased fraction of the total) from a start
state = the group's base. This is keyframe-free: **start + delta + count(= frame
count) + ease = a full tween**, no curve editor.
- *Keep from DPaint:* the **wireframe/ghost Preview** — draw the per-frame bounding
  boxes along the trajectory *before* committing, instant and non-destructive.
- *Drop/fix:* DPaint's opaque `Dist`/`Angle` labels and its Z-as-perspective
  overloading; use plain X/Y/scale/rotate with drag-handles + numeric fields.

The two compose: **Record** lays a rough hand path, **Transform-over-N** adds a
uniform drift/spin on top — DPaint allowed exactly this stacking.

### Data model (the load-bearing part)

Store the transform **per frame layer**, not on the folder — a frame's transform is
a natural per-layer property that travels with the layer when it's added, deleted,
or reordered (exactly how `parallaxDepth` is a per-layer property inert unless the
parent is a parallax group). Append to `DisplayData` / `MetaInfo`, gated at the next
version:
- `flipbookFrameOffsetX`, `flipbookFrameOffsetY` — **`WorldScalar` pair** (world-space
  offset; **never a `WorldVec`** — the undo-struct Eigen/SMF-trait gotcha,
  `[[project_layer_metainfo_eigen_gotcha]]`).
- `flipbookFrameScale` — `float` (1.0 = identity).
- `flipbookFrameRotation` — `float` degrees (optional; can defer).
- All **inert unless the parent folder is a flip-book group** (like depth is inert
  outside a parallax group). `scale_up` must rescale the world-space offset in both
  the live and undo paths (the parallax anchor precedent, `.cpp:161-173` + `:13-27`).

### Draw application

In `DrawingProgramLayerFolder::draw_flipbook_frame`, wrap the chosen child's draw in
a canvas transform built from that frame's keys — `canvas->save()`, apply
offset/scale/rotation **through the camera** (offset is a `WorldVec` → projected via
`CoordSpaceHelper` like any world-space transform; scale/rotation pivot around a
chosen point), `child->draw()`, `canvas->restore()`. **Pivot decision to settle:**
the group's base/anchor vs. each frame's own origin (recommend the group anchor so
scale/rotate feel like they're about the whole flip-book). Playback already advances
`frameIndex`; this just adds a transform to the frame being shown, so it animates
with zero playback-clock changes.

### Why this fits (and what we still skip)

- **No timeline, on purpose.** Both models author on the canvas (a gesture or a tiny
  numeric delta). The **Preview** button we already built is the "scrubber" — hit it
  to watch, in drawing mode, with no reader-mode round-trip.
- **Onion-skin still skipped** (per §B.9) — but note transform animation is the one
  case where seeing *adjacent* frames at their transforms genuinely helps (they're
  in different places, so opacity-as-onion is weaker here). If it bites, a minimal
  "ghost the neighbouring frames at their keyed transforms" is the smallest possible
  addition — flagged, not committed.

### Open decisions (for zynx, before any B-M5 build)

1. **Which model first?** Recommend **Transform-over-N** first — deterministic, no
   input-capture plumbing, immediately useful — then **Record** as the flashier
   follow-on.
2. **Which transform components?** Recommend **position + scale** (the doc's original
   stretch), rotation optional.
3. **Record: real-time vs step capture** — recommend **step** (frame-by-frame,
   precise) first; real-time timed capture second.
4. **Pivot** for scale/rotation — group anchor (recommended) vs. frame origin.
5. **Ghost/onion for moving frames** — defer, or the minimal neighbour-ghost above.

### Rough milestones if we proceed

- **B-M5a** — data model (per-frame transform keys) + save bump + draw application
  (static: a frame just sits at its keyed transform). ~1.5–2 days.
- **B-M5b** — Transform-over-N requester + wireframe Preview. ~2 days.
- **B-M5c** — Record Motion (step capture first). ~2–3 days.
- (Real-time timed capture, smoothing, rotation, neighbour-ghost = further options.)

## B.12 Motion-path object — the PREFERRED transform-animation approach (scoped 2026-07-09)

**Status: scoped; fork decided — Route 3 (zynx, 2026-07-09). Build plan:
[MOTION-PATH.md](MOTION-PATH.md).** This supersedes §B.11's requester/record models
as the *primary* direction (zynx's proposal); §B.11 stays as the lighter fallback
if this proves too big. The idea: instead of numeric
requesters or recorded jitter-paths, the artist **draws the motion as a bezier
curve** — direct manipulation, reusing systems we already ship.

### Concept

A **Motion Path** is a bezier curve bound to one flip-book group. Adding it puts a
line on the canvas (start + end); the artist adds/moves points and drags tangents
**exactly like editing a shape**. Handles read green (first) / red (last) / yellow
(middle) diamonds. It is **visible/editable only while its flip-book group is
selected, and NEVER rendered in viewer mode**. As the flip-book plays, the whole
group travels the curve while its frames cycle independently.

Each node carries **position + time + scale**:
- **position** — the bezier node (with `controlIn`/`controlOut` tangents).
- **time** — warps how the animation's progress distributes along the path
  (hold/accelerate), reusing the *waypoint* timing vocabulary (`stopTime`-style
  hold + `TransitionEasing`).
- **scale** — a per-node value (default 1.0) that **tweens between nodes** (zynx) —
  so the group can grow/shrink along the path. A per-node field, no curve needed.
- (**rotation** — optional freebie from the path tangent, "orient along path"; can
  defer.)

### Reuse map (what the two scoping passes found)

Shape side (`RectangleCanvasComponent` + `EditTool`, PHASE6/8):
- **Node data = four parallel arrays** `points` / `controlIn` / `controlOut` /
  `nodeType` in **local `Vector2f`** + a `CoordSpaceHelper` for world placement
  (`RectangleCanvasComponent.hpp:24-54`). A motion path uses the *same shape* and
  inherits the Eigen-gotcha workaround (local floats, not `WorldVec`). Add two
  parallel arrays: `nodeTime` (float) + `nodeScale` (float) + `nodeEasing` (uint8).
- **~70% of the editing is the generic `EditTool` handle machinery** (drag / insert
  / tangent / mirror / marquee, `EditTool.cpp:144-225`) — but its seam is the
  `CanvasComponentContainer::ObjInfo`/`obj` object model, **not** the rectangle.
- **Node math is free functions** (`make_node_curve`/`make_node_corner`/de-Casteljau
  `add_polygon_point`, `RectDrawEditTool.cpp:15-92`) — reusable on any same-shaped
  node arrays (~1 day lift-out).
- **No t-/arc-length sampler exists** and `SkPathMeasure` is used nowhere — so
  "position at progress p" is **net-new** (build the `SkPath` with the shape's
  edge rule, drive it with `SkPathMeasure::getPosTan`). Standard, low-risk.
- **Handles are circles, cyan/green, no first/last role** (`EditTool.cpp:390-412`).
  Green/red/yellow **diamonds** need a diamond draw helper + a per-handle role —
  a modest change to a *shared* struct + draw path.

Waypoint side (the editor-only-object template):
- **Editor-only rendering is a solved pattern**: `WaypointCanvasComponent::draw`
  bails on `readerMode.is_active()` (`WaypointCanvasComponent.cpp:54`); tool chrome
  in `WaypointTool::draw` is gated on selection (`WaypointTool.cpp:92`). Copy both.
- **Timing reuse is free**: `TransitionEasing {LINEAR,EASE,EASE_IN,EASE_OUT,
  EASE_IN_OUT}` (`Waypoint.hpp:23-29`) + `transition_easing_to_bezier_curve()`
  (`Waypoint.cpp:23-32`) + the `stopTime` hold idiom. Carry an easing byte per node.
- **Separate-graph storage template**: `WaypointGraph` is a World member
  (`World.hpp:78`), NetObj-registered (`World.cpp:244`), index-remapped file
  serialization (`WaypointGraph.cpp:114-216`), version-gated load. A
  `MotionPathGraph` would mirror this exactly.
- **Owner-by-NetObjID precedent**: `Edge::from/to` and `Waypoint::audioId` store
  cross-object `NetObjID`s (index-remapped on save); cascade-delete like
  `erase_waypoint_by_id` (`WaypointGraph.cpp:87-112`). A path stores its flip-book
  folder's id and dies with it.
- **Consumer pattern**: `ReaderMode::update` + `snap_camera_to_current` snapshot a
  duration, tick a local timer, apply the easing vector (`ReaderMode.cpp:178-205,
  298-370`). Mirror it to advance path progress + per-node holds.

### THE FORK (decision needed before building)

The two passes recommend opposite object models. Honest tradeoff:

- **Route 1 — Motion path is a `CanvasComponent` (`MOTION_PATH` type).** *Cheapest
  editing* — plugs into `EditTool`/`edit_start`, gets `coords`/`commit_update`/undo
  free. **But** components live inside *layers*, and a flip-book folder's children
  *are its frames* — so a path-component has no natural home (it'd either pollute
  the frame set or orphan in the tree), and "bound to a folder / never in viewer"
  needs custom gates. The placement wart is permanent.
- **Route 2 — World-owned `MotionPathGraph` + refactor `EditTool` to an interface.**
  *Cleanest model* (visibility, binding, lifetime, timing) **and** still reuses the
  editing — but the refactor rewires `EditTool`'s ~8 `objInfoBeingEdited->obj`
  call-sites to an abstract `EditableHandleObject`, touching **shipped shape / text
  / brush editing**. The right long-term decoupling; real regression blast radius.
- **Route 3 (recommended) — World-owned `MotionPathGraph` + a small dedicated node
  editor.** Clean model (Route 2's object side) with **no EditTool refactor** — the
  path draws/edits its own handles via a focused interaction (path editing is
  narrower than shapes: no fill/mask/affine/marquee needed). Reuses the node *math*
  (free functions) + curve construction, reimplements only the ~2 days of
  drag/add/tangent interaction. Zero risk to shipped shape editing.

**My recommendation: Route 3.** The object model is what you live with forever, and
Route 3 gets the clean one without destabilizing the shape/text/brush editing all
your existing objects share (given the recent regression-sensitivity, that safety
is worth the modest reimplementation). Route 2 is the "correct" decoupling but is a
separate, riskier refactor I wouldn't bundle into this feature. Route 1's
placement wart is a lasting smell. (If minimizing *new* code matters more than the
object model, Route 1 flips to the front — hence: a fork for you.)

### Playback integration (small, on top of shipped B-M3)

Two independent clocks: the **frame cycle** (N frames at `fps`, existing) and the
**path traversal** (progress 0→1 over a path duration, its own play-style/trigger —
recommend: follows the group's). Each tick, sample the path at the eased progress →
a world offset + interpolated scale → apply to the shown frame inside
`draw_flipbook_frame` (wrap the child draw in a camera-projected transform). Frames
keep cycling independently. **No change to the playback clock**, just a transform
on the drawn frame.

### Open decisions

1. **The fork** (Route 1 / 2 / 3) — recommend **3**.
2. **Path play-style/trigger** — follow the group's (recommended) or independent.
3. **Rotation-along-tangent** — include or defer (defer recommended).
4. **Sampling** — arc-length (even spacing, `SkPathMeasure`) vs. parametric-t
   (easier, uneven). Recommend arc-length + per-node time warp.
5. **Multiple paths per group?** — v1 one path per flip-book (recommended).

### Rough milestones (Route 3)

- **P1** — `MotionPathGraph` (World member, NetObj-registered, save/load gated at
  **INFPNT000029 / 0.28.0**) + owner-folder binding + cascade-delete. ~2 days.
- **P2** — node editor: draw green/red/yellow **diamond** handles (selection-gated,
  never in viewer), drag/add/tangent, per-node inspector (time + scale + easing),
  reusing the node-math free functions. ~3–4 days.
- **P3** — `SkPathMeasure` sampler + playback integration (offset+scale the shown
  frame) + preview. ~2 days.
- **Rough total: ~1.5 weeks**, vs. ~1 week for §B.11's requester/record — buys a
  far better, direct-manipulation tool.
