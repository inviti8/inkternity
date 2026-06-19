# PHASE 10 — Lightweight 3D armature poser ("Bake to canvas")

## Status

**SCOPED — not started.** Second half of the PHASE9/10 split (the other half is
the lobby carousel, [PHASE9.md]). This is the **large, architecturally-novel**
phase: Inkternity is a 2D app and gains its first 3D subsystem. Do PHASE9 first;
take a clean run at this.

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
   pose a figure. ozz *has* IK, so we can add it later as PHASE10.x without
   changing the runtime.
3. **No modal sub-app precedent exists.** Edit tools render in the toolbar;
   there's no full-screen takeover mode today. We build that shell — not hard,
   but net-new.
4. **3D joint picking + gizmos** (screen-ray → joint, drag-to-rotate) is all new
   code. Standard, but it's where the fiddly UX time goes.
5. **Default-asset rig validity + license.** Quaternius is CC0 (ideal) but we
   must **download and confirm the glTF actually contains `skin`/`joints`/
   `weights` accessors** — some packs ship skin only in `.blend`. If thin, fall
   back to Khronos CesiumMan/RiggedFigure (CC-BY, guaranteed-valid rig, one
   NOTICE attribution line). Never ship Mixamo or Ready Player Me files.
6. **ozz offline bake.** ozz loads `.ozz`, not glTF at runtime — bake via
   `gltf2ozz` (uses tinygltf, all MIT). Adds a build/asset step. **If runtime
   loading of arbitrary user models is a hard v1 requirement**, we either bundle
   the converter logic to run at load, or drop ozz for hand-rolled FK/LBS on
   cgltf (no IK). Decide at M2.
7. **GPL contamination from "learning."** mannequin.js, GLLara, Blender/Rigify,
   MakeHuman app are great UX references but their **code is off-limits**. Mine
   interaction design (drag handles, light/camera presets), never source.

---

## Decisions (recommendations baked in; confirm before M1)

1. **Load format: glTF/`.glb` (RECOMMENDED) vs. bespoke schema.** glTF gives
   artists Blender / Mixamo-export / Maya rigging for free and lets them drop in
   their own models with zero new tooling from us. A bespoke schema means we'd
   also own the rigging/authoring tool. Recommend glTF in; layer custom
   constraints on top only if needed.
2. **FK-only v1 (RECOMMENDED) vs. IK from the start.** FK ships the feature; ozz
   keeps the IK door open for PHASE10.x.
3. **Modal UI toolkit: Dear ImGui (→ ImGuizmo) vs. our Clay GUI (→ im3d).**
   Affects the gizmo choice. ImGui+ImGuizmo is the fastest 3D-tooling path but
   adds a UI stack distinct from the app's Clay GUI; Clay+im3d keeps one UI
   system. Lean **Clay + im3d** for consistency unless the 3D panel needs ImGui
   tooling depth — decide at M4.
4. **Runtime arbitrary-model loading in v1?** If yes, weigh the ozz bake step
   (risk #6). Default: ship the bundled default + glTF load that runs the
   convert step at load; revisit if it's heavy.

---

## Build (milestones, FK-first)

1. **M1 — context spike.** Hand-written GL: render a lit spinning cube to an
   offscreen FBO, `resetContext()`, read pixels, bake to a canvas image
   component (reuse the `RasterFlatten` readback → `MYPAINTLAYER` path). Proves
   risk #1 before any 3D-asset work. **Gate the whole phase on this.**
2. **M2 — load + skinned render.** Vendor cgltf + ozz; load the default
   humanoid; linear-blend skinning shader; render the rest-pose figure to the
   FBO. Decide the bake-vs-runtime-load question (risk #6) here.
3. **M3 — camera + lighting.** Orbit/pan/zoom; directional + ambient; matte
   material; framing controls.
4. **M4 — gizmos + FK posing.** Joint picking (screen-ray), rotate gizmo
   (im3d or ImGuizmo), per-joint FK rotation, reset-pose. Pick the UI toolkit
   (decision 3).
5. **M5 — modal shell + widget + Bake.** Armature widget on canvas launches the
   modal; default + user-model load; **Bake** exports the framebuffer to a
   placed image component at a chosen canvas location.
6. **M6 — polish + docs.** Edge cases, attribution NOTICE for any CC-BY asset,
   `MANUAL.md` / `README.md`, license audit of vendored deps, merge.

## Effort estimate

| Work | Est. |
|---|---|
| M1 GL-FBO→bake spike (de-risk shared context) | ~2–3 days |
| M2 vendor cgltf+ozz, load default, skinning render | ~3–5 days |
| M3 orbit camera + lighting | ~2–3 days |
| M4 joint picking + rotate gizmo + FK posing | ~4–6 days |
| M5 modal shell + widget + Bake-to-canvas | ~3–4 days |
| M6 edge cases + asset licensing + docs + merge | ~2–3 days |

**Rough total: ~3–4 weeks.** The gizmo/picking/FK UX (M4) and the modal shell
(M5) carry the weight; the context spike (M1) is the gate that must pass first.

## Out of scope (v1)

- **IK posing** (deferred to PHASE10.x; ozz supports it).
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
</content>
