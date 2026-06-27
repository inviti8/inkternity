# Inkternity Default Armature — Model & System Schema

Companion to [PHASE9.md](PHASE9.md) (the 3D armature poser). This doc is the
**ground-truth schema** for the default humanoid asset
(`assets/data/models/inkternity_default_armature.glb`) and the shape-key / posing
/ loading system built around it. The "model as built" section is **verified by
direct inspection** of the alpha `.glb` (Blender glTF I/O v5.1.20), not from the
authoring notes — where the two disagree, see §6 Discrepancies.

> **Status: DRAFT for lock-down (2026-06-26).** Alpha rig delivered by zynx.
> Schema captured; **§7 Open Questions must be answered before M2/M5 coding
> starts.**

---

## 1. The model as built (verified from the `.glb`)

### File facts

| Property | Value |
|---|---|
| File | `assets/data/models/inkternity_default_armature.glb` (~10.4 MB) |
| Generator | Khronos glTF Blender I/O v5.1.20 (Blender 4.x) |
| glTF version | 2.0, single scene |
| `extensionsUsed` / `extensionsRequired` | **none** |
| **Draco compression** | **NONE** ✅ (plain cgltf can load it) |
| Counts | 91 nodes, **1 mesh / 6 primitives**, 5 materials, **1 skin / 89 joints**, **40 morph targets**, 0 animations, 0 images |
| Units / up | metres, **+Y up** (height is the Y axis) |
| **Forward axis** | **+X** (figure faces +X — the shallow bbox axis) |
| Bounding box | min `[-0.098, -0.011, -0.906]` · max `[0.246, 1.726, 0.906]` |
| Size (X,Y,Z) | `[0.344, 1.737, 1.811]` → **height ≈ 1.737 m**, arm-span ≈ 1.811 m |
| Feet | on the ground plane (Y ≈ 0) |

**Orientation consequence for the loader:** the figure faces **+X**, so the
default "front" camera sits on the **+X axis looking toward −X** at the bbox
centre `(~0.07, ~0.86, 0)`. (This **differs from the PHASE9 build sheet**, which
specified +Z-forward — see §6 D1.)

### Skeleton (1 skin, 89 joints)

VRM-Humanoid bone *vocabulary* (matches PHASE9 Decision §5), no VRM extensions —
just named glTF nodes. Hierarchy (abridged):

```
Armature
├─ mesh_body            (mesh=0, skin=0)
├─ root
│   └─ hips
│       ├─ leftUpperLegBase → leftUpperLeg → leftlowerLeg → leftFoot → [5 toes × Metacarpal/Proximal/Distal]
│       ├─ spine → chest → upperChest
│       │     ├─ neck → head → { jaw, leftEar, rightEar }
│       │     ├─ leftShoulderBase → leftShoulder → leftUpperArm → leftLowerArm → leftHand → [5 fingers × Proximal/Intermediate/Distal]
│       │     └─ rightShoulderBase → rightShoulder → rightUpperArm → rightLowerArm → rightHand → [5 fingers × …]
│       └─ rightUpperLegBase → rightUpperLeg → rightLowerLeg → rightFoot → [5 toes × …]
└─ neutral_bone         (Blender exporter artifact — see §6 D5)
```

Notable structure:
- **Standard VRM humanoid set present**: hips, spine, chest, **upperChest**,
  neck, head, L/R shoulder→hand, 15 finger bones/hand, L/R upperLeg→foot.
- **Extra (non-VRM) posing/helper bones**: `root`, `*ShoulderBase`,
  `*UpperLegBase`, `jaw`, `leftEar`, `rightEar`, and **full per-toe
  articulation** (5 toes × 3 bones × 2 feet = **30 toe bones** + `*Foot`). This
  is *far beyond* "simplified" — see §6 D2.
- **76 / 89 joints actually skin vertices.** The 13 weightless joints are the toe
  `*Metacarpal` bones, the `*ShoulderBase`/`rightUpperLegBase` helpers — pure FK
  helpers / unused.
- `skin.skeleton` is **unset** → derive the skeleton root (`root`, node 87)
  ourselves.
- **Default loaded pose = the authored node TRS** (a T/A-pose; arm-span ≈ height).
  This *is* the seed pose for a freshly-created `ARMATURE` component (PHASE9
  Decision §8); the bind pose lives in the inverse-bind matrices.

### Mesh, primitives & materials

One mesh `shape_body`, split into **6 primitives by material slot** (this is the
material-slot→primitive split PHASE9 Decision §9 calls for). All primitives carry
`POSITION, NORMAL, TEXCOORD_0, JOINTS_0, WEIGHTS_0` and **all 40 morph targets**.

| Prim | Material | Verts | Skin influences | Notes |
|---|---|---|---|---|
| 0 | `mat_joints` | 15 120 | rigid (1) | visible ball-joints of the mannequin |
| 1 | `mat_body` | 22 344 | **soft, up to 4** (mean 1.14) | the body shell — **needs real LBS** |
| 2 | **none** | 240 | 1–2 | forehead region (Y≈1.38–1.47); **no material** — see §6 D6 |
| 3 | `mat_eyes` | 56 | rigid (1) | |
| 4 | `mat_inner_mouth` | 30 | 1–2 | |
| 5 | `mat_ears` | 1 292 | rigid (1) | single ear set (no separate animal/human sets — §6 D10) |

- **Max bone influences ≤ 4** across every primitive ✅ (no `JOINTS_1`) — matches
  the single-vec4 LBS shader requirement.
- **Weights normalised** (Σ = 1.0).
- Materials: 5, all `doubleSided = true`, `alphaMode = OPAQUE`, `baseColorFactor`
  set (blue-grey mannequin palette), **no textures**. Matte lighting applies over
  base color. The renderer must **render double-sided** (or disable backface
  cull) to match authoring intent.

### Morph targets (40 shape keys)

Stored as glTF morph targets; names in `mesh.extras.targetNames` (cgltf reads
these). All 40 the author listed are present (full mapping in §2). One naming
mismatch in the lip morphs — §6 D4.

---

## 2. Customization controls (shape keys, colors, height)

Per-instance customization is three axes — **shape-key sliders**, **material
colors**, and **character height** — all laid out as **collapsible sections** in
the editor and all saved into the `ARMATURE` instance-state (§5).

### Shape-key sliders

The 40 morphs are driven by **22 sliders** in two flavours:

- **Binary set** — one slider blends *between two opposing* morphs. Range
  `s ∈ [−1, +1]`, **default 0 (centre)**. `weight(Lmorph) = max(0, −s)`,
  `weight(Rmorph) = max(0, +s)`. At centre both are 0 (the neutral base mesh).
- **Singular** — one slider drives *one* morph. Range `u ∈ [0, 1]`, **default 0
  (left, no influence)**, full influence at 1. `weight(morph) = u`.

> **Morph application order (impl note, PHASE9 M2):** apply morph deltas to the
> base mesh **before** linear-blend skinning, per PHASE9's feature→schema table.

### Slider → morph mapping (uses the **actual** morph names in the `.glb`)

UI is laid out in **collapsible sections** (one per group below).

**Body**
| Slider | Type | Left (−) | Right (+) |
|---|---|---|---|
| Gender | binary | `mesh_body_female` | `mesh_body_male` |
| Build | binary | `mesh_body_thin` | `mesh_body_fat` |

**Head**
| Slider | Type | Right (drives) |
|---|---|---|
| Round Head | singular | `mesh_head_round` |
| Egg Head | singular | `mesh_head_egg` |
| Box Head | singular | `mesh_head_box` |
| Smooth Head | singular | `mesh_head_smooth` |

**Eyes**
| Slider | Type | Left (−) | Right (+) |
|---|---|---|---|
| Eye Shape | binary | `mesh_eyes_almond` | `mesh_eyes_monolid` |
| Eye Size | binary | `mesh_eyes_small` | `mesh_eyes_large` |
| Eye Tilt | binary | `mesh_eyes_tilt_in` | `mesh_eyes_tilt_out` |
| Eye Depth | binary | `mesh_eyes_sunken` | `mesh_eyes_bulge` |
| Eye Distance | binary | `mesh_eyes_together` | `mesh_eyes_apart` |
| Eye Asymmetry | binary | `mesh_eyes_left_down` | `mesh_eyes_right_down` |

**Nose**
| Slider | Type | Left (−) | Right (+) |
|---|---|---|---|
| Nose Size | binary | `mesh_nose_small` | `mesh_nose_wide` |
| Nose Length | binary | `mesh_nose_flat` | `mesh_nose_long` |
| Nose Height | binary | `mesh_nose_short` | `mesh_nose_tall` |

**Mouth**
| Slider | Type | Left (−) | Right (+) |
|---|---|---|---|
| Upper Lip | binary | `mesh_mouth_upper_lip_thin` | `mesh_mouth_upper_lip_thick` |
| Lower Lip | binary | `mesh_mouth_lower_lip_thin` | `mesh_mouth_lower_lip_thick` |
| Mouth Size | binary | `mesh_mouth_narrow` | `mesh_mouth_wide` |
| Mouth Depth | binary | `mesh_mouth_sunken` | `mesh_mouth_protrude` |

**Expressions**
| Slider | Type | Left (−) | Right (+) |
|---|---|---|---|
| Frown / Smile | binary | `mesh_expression_frown` | `mesh_expression_smile` |
| Brows | binary | `mesh_expression_brows_down` | `mesh_expression_brows_up` |
| Eyes (open/close) | binary | `mesh_expression_eyes_closed` | `mesh_expression_eyes_wide` |

22 sliders → all 40 morphs accounted for (4 body + 4 head + 12 eyes + 6 nose +
8 mouth + 6 expressions).

### Slider config is data, not code

Author the table above as a **slider-definition resource** (group, label, type,
left/right morph names) resolved against `targetNames` at load. This keeps the UI
generic and lets a user-loaded model ship its own slider config later. Drives the
collapsible-section UI directly.

### Material colors

The model uses **5 named materials**; the editor exposes a **Colors** collapsible
section with one color swatch per material, **defaulting to the glb's authored
`baseColorFactor`**. An edit overrides the base color at draw (a per-primitive
uniform); the renderer keeps its matte lighting over the chosen color.

| Material | UI label | Authored baseColor (≈) |
|---|---|---|
| `mat_body` | Body | blue-grey `(0.27, 0.28, 0.80)` |
| `mat_joints` | Joints | dark blue `(0.14, 0.15, 0.41)` |
| `mat_eyes` | Eyes | near-black `(0.02, 0.02, 0.05)` |
| `mat_inner_mouth` | Inner Mouth | near-black `(0.02, 0.02, 0.05)` |
| `mat_ears` | Ears | blue-grey (same as Body) |

- **Color model**: RGB; alpha stays opaque for v1 (all materials are `OPAQUE`).
- **Storage** (instance-state, §5): `{materialName → rgb}`; a material absent from
  the map uses its authored `baseColorFactor`. Keyed by **material name** — the
  same name-keyed pattern as morphs/bones — so colors survive save/load + re-edit
  and apply to **arbitrary loaded meshes** too (enumerate their materials).
- **Per-swatch reset** restores the authored color. Primitive 2 (no material, §6
  D6) gets its swatch once its default material is assigned.

### Character height

> **Decision (LOCKED 2026-06-26 — zynx): height is a single slider driving uniform
> bone-length scaling of the skeleton.** Honours PHASE9's authoring-table choice
> ("bone-length scaling, not uniform root scale"). One **Height** slider in the
> editor; the value saves in instance-state.

- **Mechanism**: scale the joints' **local translations** down the skeleton by the
  height factor at pose time (rest-pose lengths × factor). Because this changes
  *locals* and leaves the inverse-bind matrices untouched, it composes cleanly with
  LBS (PHASE9 authoring table): the soft body shell (≤4-influence `mat_body`)
  stretches across the lengthened bones while the rigid joint-balls (D8) stay their
  own size and simply sit further apart — the intended wooden-mannequin look, **no
  gaps** (the shell spans every joint).
- **v1 scope = uniform factor** (all bone lengths × the same `k`): preserves
  proportions, just taller/shorter. **Per-region proportions** (e.g. longer legs
  independent of torso) are a post-v1 refinement, not v1.
- **UI**: slider labelled in **metres**, default **1.737 m (= 100%, the authored
  height)**, suggested range **~1.40–2.10 m**. Display the live metre value.
- **Order of operations** (M2): morph deltas → **height (bone-length) scaling** →
  FK pose → LBS. Morphs deform in mesh/bind space first; height adjusts bone
  locals; FK applies the pose; LBS skins.
- **Fallback** (if bone-length scaling proves fiddly at M2): uniform scale of the
  armature root is the gap-free degenerate case — same visual for a uniform factor,
  but loses the path to per-region proportions. Decide at M2 if needed.

---

## 3. Loader & runtime requirements

1. **Load** with cgltf (PHASE9 M2). Validate `skin` + `JOINTS_0`/`WEIGHTS_0`
   present (✅), reject `>4` influences (✅), reject Draco (✅).
2. **Orientation/camera**: +Y up, **+X forward**; default front camera on +X
   looking to −X at bbox centre; orbit/pan/zoom around that target.
3. **Canonical bone map** (name → canonical enum, PHASE9 Decision §5): match by
   name, **trimming leading/trailing whitespace and case-normalising** (§6 D3).
   Humanoid subset maps to the enum; extras (`*Base`, `jaw`, ears, per-toe) are
   kept as free-form posable nodes.
4. **Skinning**: hand-rolled FK + LBS, ≤4 influences, morphs applied before LBS.
5. **Materials**: use `baseColorFactor`, matte lighting, **double-sided**;
   primitive 2 has no material → assign a sensible default (§6 D6).
6. **Posing handles** (LOCKED — §7 Q1): the gizmo exposes a **curated set** —
   body/spine/hips, head + jaw, both shoulders→hands incl. **per-finger** joints,
   and legs; **each foot collapses to one per-foot handle** (the `leftFoot`/
   `rightFoot` bone; the 30 toe bones stay loaded/skinned but are not individually
   surfaced). `*Base` helpers,
   ears bones, and `neutral_bone` are not posing handles.

---

## 4. Arbitrary (non-rigged) mesh loading — reference blockout

New requirement (zynx, 2026-06-26): the 3D editor also **loads arbitrary
meshes** so artists can block out an environment in 3D and bake it as drawing
reference. Scope:

- Same cgltf path; **no skin required**. A mesh with no `skin`/`joints` loads as
  static geometry — render with camera + lighting, **no posing UI** (no skeleton
  → gizmo panel hidden, shape-key panel hidden unless it has morphs).
- Same **Bake** path to canvas (PHASE9 M1/M5) — it's just a render → `glReadPixels`
  → raster component. WYSIWYG square framing unchanged.
- **Open question** whether a static-mesh import is also a re-editable `ARMATURE`
  component (storing camera/transform) or a one-way bake — see §7 Q? in PHASE9
  terms; default proposal: same component, `pose` empty, only camera/material
  state. Confirm at M5.
- Out of scope as before: PBR/textures beyond base color, multiple meshes per
  session (load one at a time).

---

## 5. Instance-state — what the `ARMATURE` component saves

Slots into the PHASE9 `CanvasComponentType::ARMATURE` `d` struct (Decision §7),
serialized append-and-gate like shapes. Keep Eigen/`WorldVec` out of undo-tracked
members ([[project_layer_metainfo_eigen_gotcha]]) — use plain float arrays.

- **Rig ref**: bundled-default id, or embedded custom-rig bytes (size strategy at
  M5).
- **Pose**: `{canonicalBone → quaternion}` + named extras (free-form bones) +
  optional root transform. Default = authored T/A-pose.
- **Shape params**: the 22 slider values (18 binary ∈[−1,1], 4 singular ∈[0,1]).
- **Material colors**: `{materialName → rgb}` overrides (§2 Material colors);
  absent = authored `baseColorFactor`.
- **Height**: the height slider value (uniform bone-length scale factor `k`,
  default 1.0 = 1.737 m; §2 Character height).
- **Part visibility**: enabled optional primitives by material/name (Decision §9;
  currently just `mat_ears` + the no-material prim — §6 D10).
- **Camera**: orbit/target/zoom (square viewport, Decision §8).

---

## 6. Discrepancies (model vs. provided notes / PHASE9 build sheet)

| # | Discrepancy | Impact | Proposed resolution |
|---|---|---|---|
| **D1** | **Forward axis is +X**, but PHASE9 build sheet specifies +Z-forward (−Y in Blender). | Camera/loader default-view direction. | **Adopt +X** (the model is built); correct the PHASE9 build sheet. *(§7 Q2)* |
| **D2** | Notes say "simplified bones," but rig has **89 joints incl. 30 per-toe bones + ears + jaw + `*Base` helpers**. | Poser UI: 89 FK handles is unusable raw. | Curate an exposed posing set. *(§7 Q1)* |
| **D3** | Bone-name hygiene: **trailing space** in `leftUpperLeg `/`rightUpperLeg `; casing `leftlowerLeg` (vs `leftLowerArm`); toes use `Mid`/`Toe` vs fingers `Middle`. (The ankle bone was renamed `leftToes`/`rightToes` → `leftFoot`/`rightFoot` 2026-06-27.) | Exact-name canonical mapping breaks. | Normalise on load (trim+case); remaining quirks still pending a clean re-export. *(§7 Q3)* |
| **D4** | Lip morphs are `mesh_mouth_upper_lip_*` / `mesh_mouth_lower_lip_*`; notes wrote `mesh_upper_lip_*` / `mesh_lower_lip_*`. | Slider config must use real names. | Schema §2 uses the **actual** names (done). |
| **D5** | `neutral_bone` (joint 88) is a Blender-exporter artifact (mesh-as-child / non-deform). | Stray joint. | Ignore in UI; harmless to skinning. Optionally clean source. |
| **D6** | **Primitive 2 (240 verts, forehead) has no material.** | Renders with no base color. | Identify (eyebrows? lashes?); assign default material + decide visibility. *(§7 Q4)* |
| **D7** | 13 joints carry **no skin weights**; L/R asymmetry (`leftUpperLegBase` weighted, `rightUpperLegBase` not). | Cosmetic; helpers fine. | Keep as FK helpers; note the asymmetry. |
| **D8** | Skinning is **mixed**: body soft (≤4), joints/eyes/ears rigid (1). | None — ≤4 LBS handles both. | Confirm intent (wooden-mannequin rigid segments + soft body). |
| **D9** | Height ≈ **1.737 m** (build sheet said 1.8 m baseline). | Trivial. | Use 1.737 m as 100% reference. |
| **D10** | **No separate animal/human ear sets** — one `mat_ears` primitive only. | Decision §9 show/hide has only this prim (+ the no-mat prim) to toggle. | Confirm whether animal-ears variant is still coming. *(§7 Q5)* |
| **D11** | All materials `doubleSided = true`. | Renderer must not backface-cull (or render 2-sided). | Render double-sided. |
| **D12** | `skin.skeleton` unset. | Must derive root. | Use `root` (node 87). |

---

## 7. Decisions (LOCKED 2026-06-26 — zynx) + remaining opens

**Locked:**

- **Q1 — Posing joint set: core + fingers, toes collapsed.** ✅ The gizmo exposes
  the body/spine/head/jaw, both arms incl. **per-finger** articulation, and legs —
  but each foot collapses to a single **per-foot handle** (the `leftFoot`/
  `rightFoot` bone; the 30 toe bones are not individually exposed). The full
  hierarchy is still loaded/skinned;
  this is purely which handles the UI surfaces. (See §3.6.)
- **Q2 — Orientation: accept +X-forward.** ✅ Model stays as-built; the **PHASE9
  build sheet is corrected** to +X and the loader's default camera looks down +X.
  No re-export. (Resolves D1.)
- **Q3 — Names: normalise on load now, fix source later.** ✅ The canonical map
  trims leading/trailing whitespace, is case-insensitive, and maps via our own
  name table; the alpha works as-is. Clean the names at the **next Blender
  export** (also drop `neutral_bone`). (Resolves D3, D5.)
- **Q5 — Ears: single set, on/off toggle for v1.** ✅ `mat_ears` is one optional
  part with a show/hide toggle; animal/human ear variants deferred. Decision §9's
  multi-part machinery stays in the schema but v1 has one toggleable part (plus the
  no-material prim once identified). (Resolves D10.)

**Still open (non-blocking — settle by M5):**

- **Q4 — Primitive 2 (no material, 240 verts, forehead).** Need zynx to identify
  the piece (eyebrows? lashes?) and pick a default material + whether it's
  always-on or a toggle. Until then the loader assigns a default matte material and
  renders it always-on.
- **Q6 — Static-mesh import (§4).** Default adopted: arbitrary reference meshes
  load as the **same `ARMATURE` component with an empty pose** (stores camera +
  material only), one-way bake available too. Confirm at M5 when the modal firms
  up.
</content>
