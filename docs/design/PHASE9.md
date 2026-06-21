# PHASE 9 — Lightweight 3D armature poser ("Bake to canvas")

## Status

**PLANNED — grounded against the codebase; ready to start M1.** First half of the
PHASE9/10 split (the other half is the lobby carousel, [PHASE10.md]). This is the
**large, architecturally-novel** phase: Inkternity is a 2D app and gains its
first 3D subsystem. It is scheduled **first by choice** (zynx, 2026-06-21) — the
carousel is the smaller, low-risk follow-on — so take a clean, deliberate run at
this and gate it on the M1 spike.

**Planning session 2026-06-21** mapped the real rendering/screen/component/
vendoring substrate (see "Grounded architecture" below) and **locked the four
open decisions** (see "Decisions"). Net effect of the decisions: **ozz-animation
is dropped from v1** in favour of hand-rolled FK + linear-blend skinning on
cgltf, which removes the offline `.ozz` bake step and makes runtime "load your
own model" free; **Clay + im3d** is the UI/gizmo stack (no Dear ImGui). The
single biggest de-risk the grounding surfaced: **the `World` is owned by
`MainProgram`, not by the active `Screen`**, so the modal armature mode is a
plain screen swap that never destroys or reloads the canvas.

Requested by zynx: a **lightweight 3D armature program** for posing figure
references. Flow: drop an **armature widget** on the canvas → opens a modal 3D
view → load the **default rigged humanoid** or the user's own rigged model →
**pose** it (forward kinematics) → set **camera** and **lighting** → press
**Bake** → an image of the posed armature is exported to the canvas.

Honest size: **the biggest single thing we'd build** — multi-week, its own
architecture. Per the standing rule ("don't write code for something we know
will fail — call it out"), the realistic/risky boundary is drawn explicitly
below, and we **retrofit existing permissive OSS** rather than build a 3D engine
from scratch.

---

## Research spike — OSS we can drop in and retrofit

Hard filter: **Inkternity is BUSL-1.1 → no GPL/AGPL/copyleft deps.** Only
MIT / Apache-2.0 / BSD / Zlib / CC0 / CC-BY are shippable. Every candidate below
had its license verified against the actual repo LICENSE file.

### Recommended retrofit recipe

**cgltf (MIT) + ozz-animation (MIT) + ImGuizmo or im3d (MIT) + a hand-written
GL 3.3 skinning renderer, posing a Quaternius CC0 humanoid (Khronos
CesiumMan/RiggedFigure, CC-BY, as the bulletproof fallback).**

Rationale per choice:

- **ozz-animation (MIT, C++17, active, v0.16 Jan 2025)** is the core: it's the
  one runtime that actually does FK sampling + linear-blend skinning + IK
  (two-bone / look-at / foot), and it is **renderer-agnostic by design** — it
  bolts onto our own GL shader cleanly. Its bundled sample viewer is legacy
  GL/GLES2; we use the *library*, not the sample.
- **Hand-write the GL renderer.** The feature is tiny (one skinned figure,
  directional + ambient light, orbit camera, FBO readback) — ~300–500 lines of
  C++ plus a ~60-line skinning vertex shader. Critically, **no engine except
  Filament gives you skinning anyway**, and Filament is the one engine that
  **won't share our Skia-Ganesh GL context**. So an engine would mean writing
  the skinning shader ourselves *and* taking a heavy context-fighting
  dependency. Hand-written GL renders inside our context with zero conflicts and
  is trivially Emscripten-portable later.
- **cgltf (MIT, C99 single-header, active, v1.15 Feb 2025)** for loading —
  smallest footprint, exposes `cgltf_skin` (joints, skeleton,
  inverse_bind_matrices) and `JOINTS_0`/`WEIGHTS_0` directly; we translate into
  ozz's runtime structures.
- **ImGuizmo (MIT)** for rotate handles **iff we ship Dear ImGui for the modal
  UI** (its hard ImGui dep becomes free). `ROTATE` + `LOCAL` + the `deltaMatrix`
  out-param maps exactly onto per-joint FK rotation. **If we avoid ImGui, use
  im3d (MIT, zero deps, renderer-agnostic)** instead — it computes the gizmo and
  hands us vertex buffers to draw in our own GL.

**Realistic alternatives:** if assets are FBX-first instead of glTF, swap cgltf
→ **ufbx** (MIT, two-file C, includes CPU skinning eval). If we drop ImGui, gizmo
→ **im3d**. If we want to avoid ozz's offline bake entirely, drop ozz and do
FK + LBS by hand on cgltf data (more math we own, **lose IK** and ozz's
optimized runtime).

### Comparison table

| Project | Category | Lang | License | Passes BUSL? | Maintenance | Fit (1-5) | Notes |
|---|---|---|---|---|---|---|---|
| **ozz-animation** | Skinning runtime | C++17 | MIT | ✅ | v0.16 (Jan 2025) | **5** | FK+LBS+IK, renderer-agnostic. Needs offline `.ozz` bake. |
| **cgltf** | glTF loader | C99 | MIT | ✅ | v1.15 (Feb 2025) | **5** | Single-header, zero deps, direct skin/joint/IBM access. |
| fastgltf | glTF loader | C++17 | MIT | ✅ | v0.9 (Jul 2025) | 4 | Modern C++, SIMD. Smaller community. |
| tinygltf | glTF loader | C++11 | MIT | ✅ | v3.0 (2026) | 3 | What `gltf2ozz` ships with. Pulls nlohmann/json (we already have it). |
| ufbx | FBX loader + CPU skin | C99 | MIT/Unlicense | ✅ | active | 4 | Use if assets are FBX. |
| assimp | Multi-format loader | C++ | BSD-3 | ✅ (core) | v6.0.x | 2–3 | Overkill; keep nonfree flags OFF, don't ship `test/models-nonbsd`. |
| **ImGuizmo** | 3D gizmo | C++ | MIT | ✅ | active (2026) | **4** | Hard Dear ImGui dep. `ROTATE`+`LOCAL`+`deltaMatrix` ideal for FK. |
| **im3d** | 3D gizmo | C++ | MIT | ✅ | active (2025) | **5** | Zero deps, renderer-agnostic. Best if avoiding ImGui. |
| tinygizmo | 3D gizmo | C++ | Unlicense | ✅ (flag) | stale (2018) | 4 | ~1200 LoC, vendorable but unmaintained. |
| Filament | Renderer | C++ | Apache-2.0 | ✅ | v1.72 (Jun 2026) | 2 | **Owns the context** — fights shared Skia-Ganesh. Overkill. |
| bgfx | Renderer | C++ | BSD-2 | ✅ | very active | 3 | Global state + documented cross-context FBO crash. No skinning. |
| sokol_gfx | Renderer | C | zlib | ✅ | active | 4 | Does NOT own context. Good WebGPU/Emscripten path. No skinning. |
| raylib | Renderer | C | zlib | ✅ | active | 1 | Owns window+context. Architecturally wrong for embedding. |
| **Hand-written GL** | Renderer | C++ (ours) | n/a | ✅ | n/a | **5** | ~300–500 LoC + ~60-line skinning shader. Zero conflicts. |
| Quaternius Base Chars | Default asset | glTF/FBX | **CC0** | ✅ | — | — | Best default. No attribution. **Verify glTF has skin/weights on download.** |
| Khronos CesiumMan/RiggedFigure | Default asset | glb | **CC-BY 4.0** | ✅ (attrib) | maintained | — | Bulletproof rig; one NOTICE line; no Cesium logo/trademark. |
| Mixamo | Asset source | glb/FBX | Adobe ToS | ❌ | — | — | **Cannot redistribute raw rigged files.** Skip. |
| Ready Player Me | Asset source | glb | CC BY-NC-SA | ❌ | — | — | Non-commercial. Skip. |
| MakeHuman | Asset gen tool | Python | App **AGPL** / output CC0 | ⚠️ output only | active | — | Never touch the AGPL code; use external unmodified build → CC0 mesh. |
| mannequin.js | Web poser | JS/three | **GPL-3.0** | ❌ | active | — | Idea-mine UX only. |
| GLLara | Poser app | Swift/Metal | **GPLv2+** | ❌ | — | — | Idea-mine UX only. |
| Meta Animated Drawings | "poser" | Python | MIT | ✅ (off-target) | — | — | Animates a 2D drawing, not a 3D poser. Ideas only. |

### Key correction surfaced

A common assumption that `gltf2ozz` uses cgltf is **wrong** — it uses tinygltf
(verified in `src/animation/offline/gltf/gltf2ozz.cc`). Doesn't change the
recommendation (ozz's runtime pairs with any loader), but matters if we reuse
`gltf2ozz`'s loading code directly — then tinygltf is least resistance.

> **Note (decision locked):** v1 **drops ozz** (see Decisions §2). The research
> table is retained as the upgrade reference for **PHASE9.x IK** — when we want
> IK, ozz (or hand-rolled CCD/FABRIK) is the path, and this is the vetted
> shortlist. For v1 we load glTF with cgltf and do FK + LBS ourselves.

---

## Grounded architecture (codebase map, planning session 2026-06-21)

Concrete hooks verified in the tree. File:line refs are anchors for M1–M5; they
drift, so re-grep before relying on an exact line.

### Rendering substrate (for risk #1 — shared GL context)

- SDL3 GL context created in `src/main.cpp` (~`SDL_GL_CreateContext`, GL **3.3
  core**, stencil 8, **depth 0**, double-buffered). The default framebuffer id is
  captured once via `glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mS.defaultFBO)`.
- Skia Ganesh context created via `GrDirectContexts::MakeGL(...)`, owned by
  `MainStruct.ctx` and mirrored to `MainProgram::window.ctx`.
- The window `SkSurface` wraps the default FBO with
  `GrBackendRenderTargets::MakeGL(...)` at **`kBottomLeft_GrSurfaceOrigin`**
  (Y-flip awareness needed when we composite/readback). Frame =
  `ctx->flushAndSubmit()` then `SDL_GL_SwapWindow`.
- **The app makes ZERO raw-GL calls today and never calls
  `GrDirectContext::resetContext()`** — our 3D pass is the first code to touch GL
  outside Skia. Plan: render 3D into **our own FBO** (own colour + **own depth**
  attachment — the window has none), then `ctx->resetContext()` before returning
  to Skia. This is exactly the M1 spike.

### Bake-to-canvas (the M5 export — already a turnkey path)

`src/DrawingProgram/RasterFlatten.cpp` is the template; the 3D bake reuses its
second half verbatim and only swaps the *render* step for a `glReadPixels`:

1. Read our FBO pixels into an `SkBitmap`
   (`SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType)`).
   (RasterFlatten gets there via `surface->readPixels(baked, 0, 0)` at
   `RasterFlatten.cpp:200`; we get there via `glReadPixels` from our FBO.)
2. `auto* c = new CanvasComponentContainer(world.netObjMan, CanvasComponentType::MYPAINTLAYER);`
3. `c->coords = CoordSpaceHelper(worldPos, inverseScale, 0.0);`
4. `static_cast<MyPaintLayerCanvasComponent&>(c->get_comp()).surface().import_from_bitmap(baked, 0, 0); ...mark_dirty();`
5. `drawP.layerMan.add_many_components_to_specific_layer(*editLayer, {{anchor, c}});`
   then `commit_update(drawP)` on each placed component (+ undo entry).

Coordinates are `WorldVec`/`WorldScalar` fixed-point via `CoordSpaceHelper`
(`src/CoordSpaceHelper.hpp`); `inverseScale` sets pixels-per-world-unit at the
bake size.

### Modal shell (risk #3 — net-new but clean)

- `Screen` base: `src/Screens/Screen.hpp` — virtuals `update()`, `draw(SkCanvas*)`,
  `gui_layout_run()`, `input_*_callback()`. Existing screens: `FileSelectScreen`,
  `DesktopDrawingProgramScreen`/`PhoneDrawingProgramScreen`.
- Swap via `MainProgram::set_screen([](std::unique_ptr<Screen> prev){ ... })`
  (`src/MainProgram.cpp` `run_new_screen_func`). The lambda **receives the
  outgoing screen** — capture/return it to restore the canvas on Bake/cancel.
- **`World` is owned by `MainProgram` (`main.world`), independent of the active
  screen** → entering/leaving armature mode does **not** touch the canvas. This
  is the key de-risk vs. the doc's original "no modal precedent" worry.
- `ArmatureModalScreen` implements: `draw()` runs our GL 3D pass +
  `resetContext()` + Skia compositing of any 2D chrome; `gui_layout_run()` lays
  out Clay panels (camera/light/bake); `input_*` drive orbit camera + im3d gizmo.
  ReaderMode is **not** the precedent (it's an in-world overlay, not a screen).

### im3d gizmo + Clay panels (decision §3)

im3d computes the rotate gizmo and hands back vertex buffers we draw in **our own
GL pass** (the one we're already building) — no second UI framework, no second
GL-state citizen. Clay (the app's existing `GUIManager`) lays out the modal's 2D
panels exactly like the toolbar does today (`gui.element<...>` in
`gui_layout_run`). Joint picking = screen-ray vs. joint screen positions in our
`draw`/`input` code.

### Vendoring + assets (confirmed conventions)

- **cgltf** (MIT, single header) → `deps/cgltf/cgltf.h`, add
  `target_include_directories(main PRIVATE "deps/cgltf")`. No library target.
- **im3d** (MIT) → `deps/im3d/` as a small static lib (miniz pattern,
  `CMakeLists.txt` ~617) or include-dir if header-mostly. Draws via our GL.
- **Default model** → `assets/data/models/<name>.glb`; auto-bundled by the
  existing `install(DIRECTORY assets/data ...)` rules (Win/macOS/Linux/Emscripten
  all covered). Load at runtime with `load_file_to_string("data/models/...glb")`
  (CWD is set by `src/SwitchCWD.cpp`).
- **Licensing** → mirror each license into `assets/data/third_party_licenses/`
  and add a `deps/<lib>/VENDORING.md` (commit SHA + rationale), per the
  `deps/timelinefx/VENDORING.md` precedent. A CC-BY default asset (CesiumMan)
  needs one NOTICE line; a CC0 default (Quaternius) needs none.
- **C++ standard**: project is C++23. cgltf is C, im3d is small C++; neither is
  expected to need the timelinefx-style C++17 isolation, but the
  `set_target_properties(... CXX_STANDARD 17 ...)` escape hatch is there if a
  vendored TU won't build at C++23.

---

## Realistic / risky boundary (standing rule)

**Confident we can ship (v1):**

- glTF/`.glb` load format (default humanoid bundled; user can load their own).
- Raw-GL 3D renderer (linear-blend skinning) on the existing **OpenGL 3.3 core**
  context, rendering into its **own FBO**.
- **FK posing**: click a joint → rotate with a 3D gizmo. Orbit/pan/zoom camera.
  One directional + ambient light, matte material.
- New full-screen **modal "armature mode"** launched by the widget, with a
  **Bake** button exporting the framebuffer to the canvas.

**Risks flagged now, not discovered later:**

1. **Shared GL context / Skia-Ganesh state (HIGHEST risk).** Our 3D pass mutates
   GL state Ganesh caches. Render the figure to **our own FBO**, then call
   `GrDirectContext::resetContext()` before handing control back to Skia. FBOs
   can't be shared across GL contexts, so an engine that owns its own context
   can't hand us an FBO to bind in Ganesh's context — this is the decisive
   argument for hand-written GL (or sokol) over Filament/raylib/bgfx.
   **De-risk in M1** with a "spinning lit cube → baked to canvas" spike before
   building anything else.
2. **Full IK is a scope-killer.** True IK (drag a hand, the elbow solves) is a
   large sub-project. **FK-only for v1** (rotate joints directly) — enough to
   pose a figure. IK is deferred to PHASE9.x (re-evaluate ozz vs. hand-rolled
   CCD/FABRIK then; v1's hand-rolled FK/LBS doesn't preclude either).
3. **No modal sub-app precedent exists — but the Screen system makes it cheap.**
   Edit tools render in the toolbar; there's no full-screen takeover today.
   *However*, grounding confirmed a `Screen` base + `MainProgram::set_screen`
   swap, and that **`World` is owned by `MainProgram`, not the screen** — so the
   modal is a screen push/pop that never destroys the canvas. Net-new shell, but
   low-risk. (Downgraded from the doc's original framing.)
4. **3D joint picking + gizmos** (screen-ray → joint, drag-to-rotate) is all new
   code. Standard, but it's where the fiddly UX time goes.
5. **Default-asset rig validity + license.** Quaternius is CC0 (ideal) but we
   must **download and confirm the glTF actually contains `skin`/`joints`/
   `weights` accessors** — some packs ship skin only in `.blend`. If thin, fall
   back to Khronos CesiumMan/RiggedFigure (CC-BY, guaranteed-valid rig, one
   NOTICE attribution line). Never ship Mixamo or Ready Player Me files.
6. **ozz offline bake — RESOLVED by decision §2 (no longer a risk).** We dropped
   ozz, so there is no `.ozz` bake step and no runtime-conversion problem; cgltf
   parses glTF directly at load. The residual cost moved into M2: we own the
   FK/LBS skinning math and its vertex shader. (Retained here as the rationale
   trail; if PHASE9.x adds ozz for IK, the bake step returns.)
7. **GPL contamination from "learning."** mannequin.js, GLLara, Blender/Rigify,
   MakeHuman app are great UX references but their **code is off-limits**. Mine
   interaction design (drag handles, light/camera presets), never source.

---

## Decisions (LOCKED — planning session 2026-06-21)

1. **Load format: glTF/`.glb`. ✅ LOCKED.** Artists get Blender / Mixamo-export /
   Maya rigging for free and can drop in their own models with zero new tooling
   from us. cgltf (MIT, single header) is the loader.
2. **Skinning runtime: hand-rolled FK + linear-blend-skinning on cgltf — NOT
   ozz. ✅ LOCKED.** v1 is FK-only, so the skinning math is modest (traverse the
   joint hierarchy, compose local rotations, multiply by inverse-bind, upload
   skin matrices to a ~60-line vertex shader). Dropping ozz removes the offline
   `.ozz` bake step *and* makes runtime "load your own model" free (cgltf parses
   glTF directly at load). **IK is deferred to PHASE9.x**, where ozz (or
   hand-rolled CCD/FABRIK) is reconsidered — the research table above is that
   shortlist.
3. **UI/gizmo: Clay + im3d — NOT Dear ImGui. ✅ LOCKED.** The app has zero ImGui
   today; adding it would mean a second UI framework *and* a second GL-state
   citizen fighting Ganesh. im3d (MIT, zero-dep) computes the gizmo and hands us
   vertex buffers to draw in the GL pass we already own; Clay lays out the modal
   panels exactly like the toolbar does.
4. **Runtime arbitrary-model loading in v1: YES. ✅ LOCKED.** Decision §2 makes
   it nearly free — cgltf loads user glTF at runtime with no bake step. Ship the
   bundled default *and* "load your own"; harden the validation (skin/joints/
   weights present) per risk #5.

---

## Build (milestones, FK-first)

1. **M1 — context spike (THE GATE).** Hand-written GL: render a lit spinning
   cube into **our own FBO** (own colour + own depth — the window has no depth
   buffer), call `ctx->resetContext()`, `glReadPixels` into an `SkBitmap`, and
   bake it to a `MYPAINTLAYER` component via the RasterFlatten second-half path
   (`import_from_bitmap` → `add_many_components_to_specific_layer`). Proves risk
   #1 — the first raw GL in the app coexisting with Ganesh. Mind the
   `kBottomLeft` origin (Y-flip on readback). **Gate the whole phase on this.**
2. **M2 — vendor cgltf + load + hand-rolled skinned render.** Add `deps/cgltf`;
   load a bundled default `.glb`; build the joint hierarchy + inverse-bind
   matrices; FK pose eval; **linear-blend skinning vertex shader** (~60 lines)
   rendering the rest-pose figure into the M1 FBO. No ozz, no bake step. Validate
   the rig has `skin`/`joints`/`weights` (risk #5).
3. **M3 — camera + lighting.** Orbit/pan/zoom; one directional + ambient; matte
   material; framing controls. All in our GL pass.
4. **M4 — im3d gizmo + FK posing.** Vendor `deps/im3d`; joint picking
   (screen-ray vs. joint screen positions), rotate gizmo (im3d vertex buffers
   drawn in our pass), per-joint FK rotation, reset-pose.
5. **M5 — modal shell + widget + Bake.** `ArmatureModalScreen : Screen`, entered
   via `MainProgram::set_screen` (capture the outgoing screen, restore on
   exit — `World` survives). On-canvas armature widget launches it; default +
   runtime user-model load; Clay panels for camera/light/bake; **Bake** runs the
   M1 export at the chosen canvas location (+ undo entry).
6. **M6 — polish + docs.** Edge cases, `third_party_licenses/` entries +
   `deps/<lib>/VENDORING.md` for cgltf & im3d, NOTICE line if the default asset
   is CC-BY, `MANUAL.md` / `README.md`, license audit, merge.

## Effort estimate

| Work | Est. |
|---|---|
| M1 GL-FBO→bake spike (de-risk shared context) | ~2–3 days |
| M2 vendor cgltf, load default, hand-rolled FK/LBS skinning render | ~4–6 days |
| M3 orbit camera + lighting | ~2–3 days |
| M4 joint picking + im3d rotate gizmo + FK posing | ~4–6 days |
| M5 modal screen + widget + Bake-to-canvas | ~3–4 days |
| M6 edge cases + asset licensing + docs + merge | ~2–3 days |

**Rough total: ~3–4 weeks.** Dropping ozz shifts ~1 day from integration/bake
into owning the skinning math (M2), but removes the offline pipeline and unlocks
runtime model loading. The gizmo/picking/FK UX (M4) and the modal shell (M5)
still carry the weight; the context spike (M1) is the gate that must pass first.

## Out of scope (v1)

- **IK posing** (deferred to PHASE9.x; re-evaluate ozz vs. hand-rolled
  CCD/FABRIK then — v1 ships hand-rolled FK only).
- **Animation/timeline** — single static pose per bake, no keyframes.
- **PBR / textured materials / shadows** — matte + simple lighting only.
- **In-app rigging/authoring** — we load rigs, we don't create them.
- **Multiple figures per scene** — one armature per modal session in v1.
- **Re-editing a baked image as 3D** — Bake is one-way to a flat raster (like
  flatten). The 3D scene isn't stored in the canvas file in v1.

## Backward compatibility

The **Bake** output is a standard raster image component (same path as
`RasterFlatten`), so it needs **no save-format change** and loads on any build.
If a future version persists the live 3D scene (an armature *component* rather
than a baked image), *that* would need an append-and-gate version bump — out of
scope here. The armature widget/asset pipeline is build-bundled, not part of the
canvas file format.
