# ANIMATED_IMPORTS.md — 2D skeletal rig import (SkelForm ⇄ Inkternity)

**Status:** SHIPPED (v0.14.0-rc17, 2026-09-28) — decisions locked 2026-09-26. Landed on `main`:
DragonBones rig import as a live component, playback (Auto/on-touch + clip picker), pose-aware
bounds, group→PSD export (blend modes + nested folders as PSD groups), and atlas re-skin.
Remaining: Phase 4 (a DragonBones exporter contributed to SkelForm — out of this repo). **Import a
live 2D skeletal rig onto a layer**, plus a group-level **export-for-rigging** round-trip. Import
(consume) only — NOT an in-app authoring tool.
**Owner:** Inkternity client.
**Related:** [ARMATURE-SCHEMA.md](ARMATURE-SCHEMA.md) + [PHASE9.md](PHASE9.md) (3D armature —
precedent for a runtime + on-canvas draw), [PHASE10.md](PHASE10.md) (layer groups/flip-book),
[MOTION-PATH.md](MOTION-PATH.md) (keyframe tweening), TimelineFX vendoring
([[project_timelinefx_vendoring]]), [[project_network_collab_security]].

---

## 0. Decisions (locked)

1. **Import, not authoring.** No bone editor / IK rigger / timeline in Inkternity. Artists
   rig & animate in **SkelForm**; Inkternity plays the result.
2. **Format = DragonBones JSON. Runtime = embed the MIT DragonBonesCPP core + a Skia display
   adapter.** Bones, IK, mesh deformation and skinning come from the runtime — we do not
   reimplement skeletal math. MIT is clean in a commercial app.
3. **Full skinning / mesh deformation is IN scope** (not cutout-only) — the embedded runtime
   provides it; the adapter draws the deformed, bone-weighted vertices.
4. **Frame/GIF/spritesheet import is OUT of scope** — the app already imports GIF; baked frame
   animation is not this feature. This doc is skeletal rigs only.
5. **SkelForm is the authoring front end**, fed by a **DragonBones-JSON exporter we contribute
   to SkelForm** (GPL contribution in their repo is fine; we never link their code). Target
   fidelity = what SkelForm expresses that DragonBones can carry (§6).
6. **Export-for-rigging is a group-level operation** (a character = a layer group; its child
   layers = the parts). Primary outbound format = layered **PSD** (SkelForm imports PSD).
7. **PSD export is a separate feature** (also group-level) — an enabler here, useful on its own.
8. **The re-imported animated object is opaque** — a single `SkeletalCanvasComponent` with its
   own atlas, NOT a live layer group (§2).
9. **Net-syncs like `ImageCanvasComponent`** (shared rig resource; note the untrusted-inbound
   caveat, [[project_network_collab_security]]).

---

## 1. The round-trip

```
Inkternity                         SkelForm                         Inkternity
──────────                         ────────                         ──────────
character = a LAYER GROUP
  ├─ layer: head
  ├─ layer: torso     ──export──►  import PSD ──► rig (bones/IK/
  ├─ layer: arm_L        (PSD;      mesh/skin) ──► animate
  └─ ...                 baked                        │
                         parts)                       │ export
                                                      ▼  (exporter we contribute)
                                              DragonBones JSON + atlas
                                                      │
   live SkeletalCanvasComponent  ◄────import─────────┘
   (opaque, playable, own atlas)
```

The **source group** stays the editable master. The **animated object** is a separate, derived
artifact.

---

## 2. Object identity — source group vs. animated object (the key model)

**On re-import, we do NOT preserve the separate layers.** The rig becomes one opaque
`SkeletalCanvasComponent` (its own baked atlas), placeable/transformable/playable and
composited like any component — but not an editable layer stack.

Rationale:
- Once rigged, per-part editing inside Inkternity adds little — the geometry, pivots, weights
  and bones live in the rig data, authored in SkelForm, not in Inkternity layers.
- **It deletes the hardest problem:** keeping parts as live layers would force matching the
  rig's slots back to Inkternity layers and keeping the atlas in sync with edits to a rig we
  didn't author. Opaque removes that matching problem entirely.

Editing after animation, without separate layers:
- **Re-art:** edit the source group → re-export → re-rig. The two artifacts stay decoupled.
- **Re-skin (future):** when the slot layout is unchanged, hot-swap the atlas on the existing
  rig (DragonBones **mix-and-match textures**) so texture-only edits propagate without
  re-rigging. This is the editability escape hatch and matches SkelForm's mix-and-match.
- **Bake to still:** the animated object can bake a single frame to a flat image (same pattern
  as the 3D-armature Bake / Copy-Frame) when a static drawing is wanted.

Net: **two artifacts** — editable source group + derived opaque animated object.

---

## 3. Why DragonBones + embed (landscape recap)

Surveyed 2026-09-25 on: embeddable license, raster-atlas fit, importable format, usable C++
runtime, activity.

| Tool / format | License (embed?) | C++ runtime | Verdict |
|---|---|---|---|
| **DragonBones** (JSON) | **MIT** ✅ | **MIT, renderer-agnostic** (Cocos+SFML ⇒ Skia feasible) | **Chosen** — embed runtime, add Skia adapter |
| **SkelForm** (`.skf`) | GPL-3.0 ❌ | none (Rust) | **Authoring tool**; feed us via a contributed exporter |
| **Rive** (`.riv`) | MIT ✅ | MIT (full renderer) | Fallback if vector/interactive ever wanted; vector-first |
| **Spine** (JSON) | runtime gated ❌ | gated | Format is a standard; possible future reader, our own code |

Licensing: GPL (SkelForm) can't be linked but we can contribute an exporter to it and read its
exports (formats/specs aren't copyrightable). MIT (DragonBonesCPP) embeds cleanly.
Sources: skelform.org · github.com/Retropaint/SkelForm · github.com/DragonBones/DragonBonesCPP.

---

## 4. Architecture — what we build vs. reuse

**New:**
- **`SkeletalCanvasComponent`** (sibling to `ImageCanvasComponent`): holds the rig resource id
  (embedded DragonBones JSON + atlas via ResourceManager), current animation + play state
  (style/trigger consistent with flip-book & particles: ONCE/LOOP/PING_PONG, AUTO/ON_TOUCH),
  and composites like any component (transform, parallax depth, reader-mode).
- **Vendored `deps/dragonbones` (DragonBonesCPP)** compiled isolated — pin an exact commit and
  isolate its C++ std like TimelineFX ([[project_timelinefx_vendoring]]).
- **Skia display adapter** implementing DragonBones' display/slot interface with `SkVertices`
  (textured, bone-weighted deformed triangles) sampling the atlas `SkImage`. This is the only
  substantial new rendering code; bones/IK/mesh/skinning are the runtime's job.

**Reuse (verified in-tree):**
- **ResourceManager** — the rig JSON + atlas are resources by `NetObjID` (dedup, save-embed,
  net-sync), the same way images are.
- **Save-embed precedent** — TimelineFX libraries (textures included) are embedded once in the
  save and shared; a rig+atlas embeds the same way so `.inkternity` files travel with it.
- **Per-frame update loop + reader-mode triggers** — the flip-book/particle cadence drives
  playback; AUTO/ON_TOUCH triggers already exist.
- **RasterFlatten per-layer rasterize** (`RasterFlatten.cpp`) — the export path bakes each
  child layer (vector/text/strokes/image) to a flat RGBA part with its offset.
- **Tweening math** — motion-path keyframe interpolation is the same shape the runtime applies
  to bones (reference, not literal reuse).
- Likely **pure Skia** draw (`SkVertices`), so no raw-GL pass needed (unlike PHASE9 3D); the
  raw-GL-on-Ganesh precedent ([[project_raw_gl_on_ganesh_context]]) is a fallback only.

---

## 5. Full skinning / mesh deformation

In scope (decision §0.3). The embedded runtime computes deformed vertex positions from
bone transforms + per-vertex weights each frame; the Skia adapter draws them as textured
`SkVertices`. Cutout (rigid part = 1-bone mesh) is just the degenerate case of the same path,
so there's no separate "cutout-first" phase — the adapter handles weighted meshes from the
start. IK is likewise solved inside the runtime; the adapter never sees it.

---

## 6. Export-for-rigging (group-level) + the SkelForm exporter

- **Group-level "Export for rigging"** on a character layer group: rasterize each child layer
  to a part (RasterFlatten), preserving name + placement, and write a **layered PSD** (each
  layer → a PSD layer). SkelForm imports PSD directly, so PSD is the outbound format.
  - Optional later: a group-level **DragonBones/Spine-JSON pre-rig** export (parts laid out,
    no bones) for artists who rig in those ecosystems instead of via PSD.
- **PSD export is a separate feature** (its own track): per-layer rasterize + a PSD writer,
  operating on a selected group. Useful beyond rigging (interchange with other paint apps).
- **The exporter we contribute to SkelForm** emits DragonBones JSON + atlas — that's the
  inbound format Inkternity's runtime plays. **Fidelity envelope = DragonBones format:** it
  carries bones, IK, meshes, skinning, slots, keyframed animations, and slot texture-swap
  (mix-and-match) — i.e. SkelForm's core feature set. Where SkelForm expresses something
  DragonBones JSON can't (§8), the exporter approximates or we revisit the format choice.

---

## 7. Build phases

PSD export is an independent track (§6) that can proceed in parallel; the round-trip needs it.

| Phase | Scope | Notes |
|---|---|---|
| **0 — spike** | Vendor DragonBonesCPP, stub the Skia adapter, load a sample rig, draw frame 0 on a layer | Proves the embed + core-vs-renderer separation on Skia — the biggest assumption |
| **1 — static rig component** | `SkeletalCanvasComponent`, resource/save-embed of JSON+atlas, static pose render | First real rig on canvas |
| **2 — playback (full)** | Animations, interpolation, loop/blend, **mesh deform + skinning + IK**, reader-mode/AUTO/ON_TOUCH triggers, clip selection UI | The live-rig payoff |
| **3 — group export (PSD)** | Group-level "Export for rigging" → layered PSD (part rasterize + PSD writer) | The outbound half of the round-trip; overlaps the standalone PSD-export feature |
| **4 — SkelForm exporter** | Contribute a DragonBones-JSON exporter to SkelForm | Closes the loop; upstream PR |
| **5 — UX + re-skin** | Import entry point, layer-panel affordances, atlas hot-swap (mix-and-match) for texture-only re-art | Editability escape hatch |

---

## 8. Open questions

1. **SkelForm → DragonBones fidelity.** SkelForm doesn't export DragonBones today (we add it).
   If SkelForm has features DragonBones JSON can't represent, the exporter must approximate —
   or, if fidelity matters more than the free runtime, revisit (clean-room `.skf`, or Spine).
   Needs a feature-by-feature mapping pass once we build the exporter.
2. **Export baking specifics.** Per-part **pivot/anchor** and trim (tight vs. layer-bounds),
   handling of layer blend modes / opacity / effects at bake, and particle/flip-book child
   layers inside a character group (rasterize a representative frame? disallow?).
3. **Re-skin matching.** Keying atlas hot-swap on stable slot names/dimensions — define the
   contract that makes a re-export "compatible" with an existing rig.
4. **Multiple animations in one rig.** UI for choosing/[sequencing] clips on a
   `SkeletalCanvasComponent`; do clips participate in reader-mode/waypoint flow like flip-book?
5. **Optional DragonBones/Spine-JSON pre-rig export** (§6) — build it, or PSD-only?
6. **Collab/net** — confirm `SkeletalCanvasComponent` serialize/net-sync mirrors
   `ImageCanvasComponent`; large atlases ride the existing resource path.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| DragonBones core not cleanly separable for a Skia renderer | Phase-0 spike proves it before commitment; Cocos+SFML adapters both exist ⇒ genuinely decoupled |
| SkelForm↔DragonBones feature gaps | Exporter mapping pass (§8.1); DragonBones covers the standard 2D-skeletal set |
| C++ std / build clash from vendored runtime | Isolate the TU + pin a commit, like TimelineFX |
| Export baking loses fidelity (vectors/text/effects) | Rasterize at export bounds; document what bakes vs. not (§8.2) |
| Scope creep toward authoring | Explicit non-goal (§0.1) |

---

## 10. Licensing summary

- **GPL-3.0 (SkelForm):** not linkable into Inkternity. Fine to contribute an exporter to it
  and to read files it exports.
- **MIT (DragonBonesCPP):** embeddable in a commercial app; keep the notice.
- **Spine:** runtime license-gated; format readable with our own code later if wanted.
