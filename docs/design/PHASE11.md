# PHASE 11 — "New Publications" lobby carousel

## Status

**FUTURE WORK — not started.** PHASE 11 collects two deferred, independent items:
(1) the **"New Publications" lobby carousel** (rescheduled here when PHASE10 was
repurposed for the flip-book/motion-path authoring feature), and (2) **CV
hand-tracking for the armature's hands** (moved out of PHASE10 on 2026-07-09 once
flip-book shipped — its full scoping is the second half of this doc). Neither is
scheduled; both are ready to pick up. The carousel needs no save-format bump; the
hand-tracking is decision-gated (see its §A.7).

Requested by zynx: a promotional carousel in the lobby, in its own section
**"New Publications"**, used to promote & cross-pollinate the works of
Inkternity artists. v1 is a **curated list baked into the build**; each entry is
a promotional image + a deep-link to the specific publication on the Heavymeta
Portal.

Honest size: **~3–4 days, low risk.** The portal side already exists and the
lobby already has horizontal-scrolling list primitives — this is mostly
plumbing + a JSON schema + bundled art.

## Why this is low-risk (grounding)

The portal half is **already fully built** (`heavymeta_collective`):

- A "publication" is an `inkternity_canvases` row: `canvas_id` (UUID), `title`,
  `description`, `cover_cid` (IPFS), `price_usd`, plus the owning artist.
- **The deep-link already exists and is public, no auth:**
  `https://heavymeta.art/inkternity/canvas/{canvas_id}`.
- Cover art lives at `<ipfs-gateway>/ipfs/{cover_cid}`.
- The portal even has forward-compat API endpoints stubbed (design-doc "B7",
  "not yet called by desktop, schema ready") — the clean upgrade path to a
  *live* feed later.

The Inkternity half has every primitive we need:

- Lobby is `src/Screens/FileSelectScreen.cpp` (Clay immediate-mode GUI).
- `src/GUIStuff/Elements/GridScrollArea.hpp` already supports **horizontal
  scroll + horizontal clip** — that is the carousel.
- Assets bake in via `assets/data/` → `install(DIRECTORY assets/data ...)` in
  `CMakeLists.txt`, loaded at runtime with `load_file_to_string` +
  `nlohmann::json`. So "curated list baked into the build" =
  `assets/data/publications/catalog.json` + bundled images. No codegen.
- Opening the link cross-platform: **SDL3 `SDL_OpenURL()`** — one call, no new
  dependency.

## Model

A bundled catalog file lists curated publications. Each entry renders as a card
in a horizontal carousel in a new lobby section. Clicking a card opens that
publication's portal page in the system browser.

### Data: `assets/data/publications/catalog.json`

```json
{
  "version": 1,
  "publications": [
    {
      "id": "canvas-uuid-or-local-slug",
      "title": "The Work's Title",
      "artist": "Artist Display Name",
      "blurb": "One-line promo description.",
      "image": "data/publications/img/work01.jpg",
      "url": "https://heavymeta.art/inkternity/canvas/3fa85f64-...",
      "price_usd": 4.99
    }
  ]
}
```

- `image` is a **bundled local path** (see decision below), decoded at load.
- `url` is the canonical portal deep-link.
- `price_usd` optional (shown on the card if present).
- `version` lets us evolve the schema without breaking old builds.

## Decisions (recommendations baked in; override if desired)

1. **Promo images: bundle locally (RECOMMENDED) vs. fetch `cover_cid` from
   IPFS at runtime.** Bundling matches the "baked into the build" framing, has
   **no network failure mode**, guarantees the section always renders, and lets
   the promo art be *art-directed* (a banner that differs from the square cover
   thumbnail). v1 = bundle. The live-feed-from-portal path stays open for a
   future phase using the same card schema.
2. **No save-format bump.** This is lobby UI + bundled assets only; it never
   touches `.infpnt` canvas files. No `VersionConstants` change.

## Build (milestones)

1. **M1** — `catalog.json` schema + loader (parse with `nlohmann::json`, decode
   bundled images to `SkImage`). Add `assets/data/publications/` (catalog +
   `img/`) and confirm CMake installs it on all platforms. Graceful skip if the
   file is missing or an image fails to decode.
2. **M2** — "New Publications" section in `FileSelectScreen`: a horizontal
   `GridScrollArea` of image cards (cover + title + artist + optional price),
   click handler → `SDL_OpenURL(entry.url)`. Place it in `main_display()`
   alongside the existing menu branches.
3. **M3** — polish: empty-state (hide the section if the catalog is empty),
   per-card fallback image, hover/press affordance, docs (`MANUAL.md` +
   `README.md`), seed the curated list with the launch set.

## Effort estimate

| Work | Est. |
|---|---|
| Catalog schema + JSON loader + image decode | ~0.5 day |
| Bundle assets + CMake install verify (Win/macOS/Linux) | ~0.5 day |
| Lobby section + horizontal carousel + click→OpenURL | ~1–1.5 days |
| Empty/fallback states + polish + docs + seed list | ~1 day |

**Rough total: ~3–4 days.**

## Out of scope (v1)

- **Live feed from the portal.** v1 is a static bundled catalog; the portal API
  (`B7`) is stubbed but not consumed yet. Future phase.
- **In-app purchase / token entry flow.** The card links *out* to the portal,
  which already owns checkout + token minting. We do not embed Stripe.
- **In-app preview of the publication.** Clicking opens the browser; we do not
  render the canvas in-lobby.
- **Per-artist filtering / search / categories.** Curated flat list only.

## Backward compatibility

No canvas-file format change → existing files are untouched and unaffected. A
missing/empty `catalog.json` simply hides the section, so older asset bundles
degrade gracefully. The catalog's own `version` field guards future schema
changes.

---
---

# CV hand-tracking → armature hand pose (moved from PHASE10, 2026-07-09)

> Decision-gated future work. Scoping below is verbatim from the PHASE10
> research pass; nothing built.

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
