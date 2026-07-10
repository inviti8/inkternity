# PHASE 10 — Hand-tracking armature posing + Flip-book layer groups

## Status

**SCOPED — not started.** Two new authoring features, chosen (zynx, 2026-07-09)
to sharpen Inkternity as a Clip-Studio competitor. The previously-scoped "New
Publications" lobby carousel that lived here was **rescheduled to [PHASE11.md]**
(not needed yet); this phase is repurposed for the two features below.

Both are grounded against the shipped code (armature poser [PHASE9.md] +
[PHASE9.5.md]; the layer/folder tree). File:line anchors were captured during
scoping (2026-07-09) and drift — re-grep before relying on an exact line.

The two features are **independent** and can ship in either order or separately:

- **Feature A — CV hand-tracking → armature hand pose.** Pose the 3D armature's
  hands (and fingers) from a webcam, à la Clip Studio's hand-tracking. **This is
  the exploratory one** — "scope how hard it is, then decide" (zynx). The honest
  verdict below is *feasible and license-clean, moderate effort, one legal
  sign-off needed*; the go/no-go decision is called out explicitly (§A.7).
- **Feature B — Flip-book layer group.** A folder variant whose child layers are
  animation frames, with fps + play modes (once-on-camera-enter / loop /
  ping-pong). **This is the committed, lower-risk one** — it maps almost exactly
  onto the shipped parallax-group mechanism. Stretch: per-frame position/scale
  keys.

Both bump the save format only if their persisted state lands (Feature A adds
**no** new persisted fields — it writes through the existing armature pose
struct; Feature B adds folder fields → one version bump). See each part's
Backward-compatibility note.

---
---

# FEATURE A — CV hand-tracking → armature hand pose

## A.1 Product summary

Inside the existing armature editor (the full-screen `ArmatureModalScreen` from
PHASE9), the artist opens a **Hand Tracking** panel, grants webcam access, holds
their hand up to the camera, and the on-screen 3D armature's matching hand
**mirrors their finger pose in real time**. A **Capture** action freezes the
current solved pose onto the rig; **Bake** (the existing button) writes it to the
canvas like any other pose edit. It's a pose *assist*, not motion capture — the
goal is "get the hand 80% posed in two seconds instead of rotating 15 finger
joints by hand."

## A.2 The honest verdict (read first)

**Feasible, license-clean, moderate effort — but the clean path is NOT "embed
MediaPipe."** MediaPipe-the-C++-library is welded to Bazel (no CMake/Conan
package, "experimental" Windows support, JS-only WASM) and fighting it would be a
standing tax against our Conan/CMake/Emscripten build. The right architecture
uses MediaPipe's *model* (which is redistributable) on our *own* runtime:

```
SDL3 SDL_Camera  →  RGB convert  →  palm+hand-landmark ONNX model  →  ONNX Runtime
   (Zlib, we           (ours)          (Apache-2.0, OpenCV-Zoo          (MIT)
   already ship it)                     conversion of MediaPipe)
        │
        ▼
  21 landmarks/hand  →  One-Euro filter  →  analytic retarget  →  per-finger
                                              (ours, ~Kalidokit)    local quats
                                                     │
                                                     ▼
                              ArmatureModel::set_joint_pose(jointIdx, quat)  ← EXISTING
```

Every link is **MIT / BSD / Apache-2.0 / Zlib** — zero copyleft, zero
non-commercial, and (if we skip MediaPipe's code) **zero new heavyweight
dependency**. The one item needing a human legal glance is the model-weight
license (§A.6, risk 1).

## A.3 Why the codebase side is nearly free (grounding)

The PHASE9 pose pipeline is already exactly the shape a solver needs to feed:

- **Pose is name-keyed quaternions end-to-end.** `ArmatureModel::set_joint_pose(int
  jointIndex, const Eigen::Quaternionf& localPose)`
  (`src/Armature/ArmatureModel.cpp:575`) normalizes, writes xyzw, and refreshes the
  GPU skin — the exact call the manual rotate-gizmo already uses
  (`ArmatureModalScreen.cpp:498`). A solver just calls it per finger joint.
- **Finger bones are already first-class posable joints.** The default rig
  (`assets/data/models/inkternity_default_armature.glb`) has **15 finger bones per
  hand**, Mixamo/VRM-named `{side}{Thumb|Index|Middle|Ring|Little}{Proximal|Intermediate|Distal}`
  plus `{side}Hand`. All are in the pickable/posable set already
  (`build_pickable_set()`, `ArmatureModel.cpp:642`); L/R is the `left`/`right`
  name prefix. Address by `find_joint(name)` → index.
- **The bake/undo path is unchanged.** `do_bake()`
  (`ArmatureModalScreen.cpp:1008`) serializes every non-identity `joint_pose(j)`
  into the component's plain-float `PoseEntry` struct
  (`ArmatureCanvasComponent.hpp:38`, `d.pose`) and pushes an undo action. A
  hand-tracked pose is already in `mJointPose` by bake time, so it round-trips
  with **no serialization change and no new persisted fields.**
- **Eigen is free in the modal/model layer** (already uses `Eigen::Quaternionf`,
  `AngleAxisf`, `FromTwoVectors`, `joint_world_matrix()`), while the undo-tracked
  `d` struct stays plain floats — respecting `[[project_layer_metainfo_eigen_gotcha]]`.
  The solver lives in the modal/model layer and only writes plain-float quats.

So the genuinely new work is **all in the modal + a new subsystem**, not in the
pose/skin/bake/serialize core.

## A.4 What's actually new (the real work)

1. **Webcam capture via `SDL_Camera`.** Shipped in SDL 3.2.0 (Jan 2025), which we
   already depend on — **no new dependency, stays under Zlib.** Backends cover all
   targets: Media Foundation (Win), AVFoundation (mac), V4L2/PipeWire (Linux),
   Emscripten (web, incl. the browser permission flow). Frames arrive as
   `SDL_Surface`s in camera-native formats (often YUV) → we color-convert to RGB
   before inference. Young API (~1.5 yr): known rough edges (PipeWire enumeration,
   format variance, permission callbacks) — bounded integration cost, not a
   blocker. `OpenCV videoio` (Apache-2.0) is a documented fallback we likely won't
   need.
2. **Inference runtime — ONNX Runtime (MIT).** Official Emscripten/WASM build,
   `--minimal_build` to shrink, Conan-workable. Pairs directly with the ONNX hand
   model. (Alt: **ncnn**, BSD-3, lighter binary + better WASM size, if ORT-Web
   bloat bites — evaluate as a WASM-only backend later.)
3. **The model — Apache-2.0 ONNX conversion of MediaPipe Hands.** Ship the
   **OpenCV-Zoo** `palm_detection_mediapipe` + `handpose_estimation_mediapipe`
   ONNX artifacts (both HF cards state "All files … licensed under Apache 2.0").
   Two-stage: palm detect → 21-landmark regress (3D, wrist-relative metres) +
   handedness. Bundle under `assets/data/models/` like the rig. Cite *that*
   artifact's license, not Google's SPDX-less model card (§A.6 risk 1).
4. **The retarget solver (ours, ~few hundred lines).** 21 landmarks → per-finger
   local quaternions. Known, largely-solved problem (reference:
   **Kalidokit**, MIT — *port the math, don't link the TS*). Recipe: build a
   palm/wrist reference frame from wrist + MCP knuckles; per finger walk
   Proximal→Intermediate→Distal, bone direction = normalized delta between
   consecutive landmarks; `Eigen::Quaternionf::FromTwoVectors(bindDir,
   targetDirInParentLocal)` → local quat → `set_joint_pose`. Convert MediaPipe's
   frame (+X right, +Y down, image-plane Z) onto the rig's **+X-forward / +Y-up
   RH** space with a fixed change-of-basis; mirror for L/R by name prefix. The
   analytic pass gets ~80%; the last 20% (our rig's exact rest axes, anatomical
   joint-limit clamps, **One-Euro/EMA jitter filter**) is where the time goes.
   Monocular depth is noisy — finger *curl* reads well, out-of-plane *splay* is
   the weak axis.
5. **UI + capture loop in the modal.** Add a **Hand Tracking** tab
   (`ArmatureModalScreen.cpp:585` tab list; new `else if (mTab == 7)` panel near
   `:694`), gated on a poseable model **and** a build flag for the CV dep. The
   per-frame webcam pull + inference goes in `update()`
   (`ArmatureModalScreen.cpp:415`). **Critical: the modal is dirty-gated** —
   `draw()` only re-renders when `mDirty` (`:892`), so the capture loop must call
   `request_redraw()` each tick (or only when a new solve lands) or the preview
   freezes. Run inference on a **worker thread**, `set_joint_pose` +
   `request_redraw()` only when a fresh solve arrives (decouple camera fps from
   render).
6. **Webcam preview overlay (Skia).** Blit the camera `SkImage` + 21 landmark
   dots straight to the Skia canvas in `draw()` after the figure composite
   (`ArmatureModalScreen.cpp:920`, mirroring the joint-handle overlay at
   `:934-979`). The camera frame never touches the GL/FBO path, so it sidesteps
   the `resetContext()` gate entirely.

## A.5 Build (milestones) — behind a `HVYM_HAS_HANDTRACK` build flag

The whole feature compiles out when the flag is off, so the CV dependency is
**optional** (like `HVYM_HAS_LIBMYPAINT`) and never blocks a plain build.

1. **A-M1 — spike: camera → landmarks on screen (THE GATE).** `SDL_Camera` open +
   RGB convert; run the ONNX palm+landmark model through ONNX Runtime; draw the 21
   dots as a Skia overlay in the modal. No rig coupling yet. Proves the dep stack
   (SDL_Camera maturity on our 3 desktop OSes + ORT + model) before any retarget
   work. **Gate the feature on this.**
2. **A-M2 — retarget solver + live finger posing.** Landmark→local-quat math on
   the default rig; `set_joint_pose` per finger joint each solve; change-of-basis
   + L/R mirror; the armature's hand mirrors the webcam live. Single hand.
3. **A-M3 — filtering + UX.** One-Euro/EMA jitter filter; joint-limit clamps;
   worker-thread inference + `request_redraw()`-on-new-solve; Hand Tracking tab
   with start/stop + Capture; webcam preview inset. Which hand (L/R/both), which
   arm gets the pose.
4. **A-M4 — polish + licensing + docs.** `third_party_licenses/` entries + a
   `deps/<lib>/VENDORING.md` for ONNX Runtime + the model artifact (commit SHA +
   the OpenCV-Zoo Apache-2.0 line); MANUAL/README; graceful "no camera / permission
   denied" states. **Legal sign-off on the model-weight license** lands here.
5. **A-M5 (deferred, own phase) — WASM + two hands + accuracy polish.** ORT-Web +
   Emscripten `SDL_Camera` + browser permission UX + perf; second hand; accuracy
   tuning. This is the bulk of the "full path" cost — explicitly out of the MVP.

## A.6 Risks (ranked)

1. **Model-weight license paper-trail (Medium, mitigable — the classic trap).**
   Weights often carry a different license than the code. *Mitigation:* ship the
   **OpenCV-Zoo Apache-2.0** ONNX conversion and cite *its* license line (strongest
   paper trail), not Google's SPDX-less card. **Hard-avoid** YOLO-pose (AGPL,
   weights included) and OpenPose (non-commercial); MMPose weights are
   dataset-tainted (research-only datasets) — avoid too. One human legal glance
   before ship.
2. **WASM portability (Medium-High).** Desktop is straightforward; the browser
   path (ORT-Web size/perf, Emscripten `SDL_Camera` maturity, permission flow) is
   where days evaporate. *Mitigation:* **desktop-first, WASM behind the feature
   flag (A-M5).**
3. **Retarget accuracy / jitter (Medium).** Monocular depth is noisy; mapping onto
   our exact rig axes is bespoke. *Mitigation:* One-Euro filter + joint clamps;
   frame it as a pose *assist*, and let the artist fine-tune with the existing
   gizmo afterward.
4. **Cross-platform camera edge cases (Low-Medium).** `SDL_Camera` is young.
   *Mitigation:* OpenCV `videoio` fallback documented but likely unused.
5. **Per-tick inference cost (Low-Medium).** Moving the modal to continuous redraw
   + a 5–30 ms inference each frame. *Mitigation:* worker thread; solve throttle;
   the GL render cost is already proven acceptable at interactive rates.

## A.7 Decision gate (go / no-go — zynx's call)

Since this was scoped as "decide after we know the difficulty," the explicit
recommendation: **YES to the desktop MVP** (A-M1…A-M4, ~2–3 weeks, all
permissive-licensed, one legal sign-off), **DEFER WASM + two-hands** (A-M5) to a
later phase. The gate is **A-M1**: if `SDL_Camera` + ORT + the ONNX model don't
come together cleanly on Win/mac/Linux in the spike, stop before building the
solver. If the appetite is smaller, a valid trim is **desktop-only, one hand,
Capture-only (no live preview loop)** — solve on a button press instead of every
frame, which sidesteps the continuous-redraw + worker-thread work entirely.

## A.8 Effort estimate

| Work | Est. |
|---|---|
| A-M1 spike: SDL_Camera + ORT + ONNX model → landmark overlay (THE GATE) | ~2–3 days |
| A-M2 retarget solver + live finger posing (one hand) | ~4–6 days |
| A-M3 filtering + worker thread + Hand Tracking tab + preview | ~3–5 days |
| A-M4 licensing + docs + graceful states + legal sign-off | ~2–3 days |
| **Desktop MVP subtotal** | **~2–3 weeks** |
| A-M5 WASM + two hands + accuracy polish (deferred, own phase) | ~2.5–5 weeks |

## A.9 Out of scope (MVP)

- **Full-body / face tracking** — hands (+fingers) only. Body/face pose is a much
  larger CV problem for later.
- **WASM/web build, two-hand simultaneous tracking, accuracy polish** — deferred
  to A-M5.
- **MediaPipe's C++/Bazel framework** — we use the *model*, not the framework.
- **Recorded/animated capture** — single static pose per Capture, baked like any
  pose. (A future "record a finger animation" ties into Feature B / FRAME_ANIM.)
- **Retarget onto arbitrary user-loaded rigs** — MVP targets the bundled default
  rig's known finger-bone names; arbitrary rigs pose by hand.

## A.10 Backward compatibility

**No save-format change.** A hand-tracked pose is written through the existing
`d.pose` (`PoseEntry` plain floats) and the existing `do_bake()` path — identical
bytes to a hand-posed armature. Old builds open the canvas unchanged. The CV
dependency is build-bundled behind `HVYM_HAS_HANDTRACK`, never part of the canvas
format.

---
---

# FEATURE B — Flip-book layer group

## Implementation status

**Core shipped (B-M1…B-M4), 2026-07-09.** Data model + save bump (INFPNT000028 /
0.27.0), draw-one-frame + cache bypass, playback clock (once/loop/ping-pong ×
auto/on-touch, viewer-mode-gated + drawing-mode Preview), and the folder GUI
(fps / play-style / trigger / invert / Preview + row badge) are implemented and
build clean. **Remaining:** B-M5 (stretch position/scale keys) and B-M6
(MANUAL/README docs + edge-case polish), plus interactive verification on a real
canvas. One deviation from the plan below: `DrawData::FlipbookGroup` was **not**
needed — frame selection is local to the folder (runtime `frameIndex` on
`DrawingProgramLayerFolder::FlipbookRuntime`), unlike parallax which threads
context to deep descendants. `frameIndex` is the panel/child index directly;
invert flips the advance direction, not a coordinate remap.

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
- **Onion-skin between frames (dropped, zynx 2026-07-09).** Not needed — each
  child layer already has an **opacity** setting (`DisplayData::alpha`), so an
  artist who wants to see adjacent frames while drawing can just lower a frame's
  opacity and read the layers below through it. No dedicated onion overlay.
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

---
---

## Cross-feature notes

- **Independent & separately shippable.** A and B share no code; order them by
  appetite. B is the safe, fast win; A is the flashier, decision-gated one.
- **Save format:** Feature A bumps nothing; Feature B bumps once
  (INFPNT000027 → INFPNT000028 / 0.27.0). If both ship, B owns the bump.
- **Licensing discipline (Feature A):** BUSL-1.1 → MIT/BSD/Apache/Zlib/CC0 only.
  Every A dependency was license-verified during scoping (SDL_Camera Zlib, ONNX
  Runtime MIT, the hand model Apache-2.0 via OpenCV-Zoo, Kalidokit MIT as
  *reference only*). YOLO-pose (AGPL) and OpenPose (non-commercial) are
  disqualified; MMPose weights are dataset-tainted. Mirror licenses into
  `assets/data/third_party_licenses/` + `deps/<lib>/VENDORING.md` per the
  timelinefx precedent.
