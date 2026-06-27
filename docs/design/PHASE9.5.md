# PHASE 9.5 — App-level Character & Scene presets (armature editor)

## Status

**SHIPPED (2026-06-27).** M1 storage module (`ArmaturePresets`) + M2 Scene tab +
M3 Character tab + M4 docs all landed. Follow-on to PHASE9 (the armature poser +
external model loading, see [PHASE9.md]). Adds two new tabs to the armature editor —
**Character** and **Scene** — where the artist saves and loads reusable presets.
Presets live at the **app/user level** (not inside any `.inkternity` file), so a
character look or a camera+light setup carries across every file and project.
Each saved preset renders in the tab as a **name + thumbnail** tile.

No save-format bump: presets are sidecar files under the per-user config dir, not
canvas data.

---

## 1. Goal & scope

Today the armature editor's customization (height, per-material colors, shape
keys) and view (camera, light, lens) are **per-instance** — baked into one
on-canvas `ARMATURE` component's `d` struct. There's no way to reuse a look or a
lighting setup on the next figure, or in another file.

PHASE 9.5 adds two cross-file preset libraries surfaced as editor tabs:

- **Character preset** = the *body look*: `height` + `materialColors` +
  `shapeSliders`. (NOT pose; NOT the baked raster.)
- **Scene preset** = the *view*: camera (`camYaw/Pitch/Dist/Tx/Ty/Tz`) + lens
  (`fovDeg`, `ortho`) + light (`lightAz/El/Int/Amb/Sky`).

Each tab lists saved presets as **name + thumbnail** tiles; clicking a tile
**applies** it to the live editor; a per-tile control **deletes** it; a header
action **saves the current state** as a new named preset.

**In scope:** the two libraries, their disk format, thumbnail generation, the two
tabs, save/load/delete, and applying presets to the live model/view.

**Out of scope (v1):** editing/renaming a preset in place (delete + re-save),
folders/tags, cloud/portal sync, "new armature directly from a character preset"
(you add an armature, open the editor, then load), and sharing presets between
users.

---

## 2. Precedent — reuse the brush "Saved Presets" machinery

This is deliberately modeled on the existing per-user brush-preset system, which
already does app-level, cross-file, thumbnailed presets:

- **Config dir:** `SDL_GetPrefPath("HEAVYMETA", "Inkternity")` →
  `main.conf.configPath` (`src/GlobalConfig.hpp:26`). Already hosts
  `settings.json`, `palettes/`, `brush_presets/`, `avatar.png`.
- **Module shape:** `src/Brushes/UserBrushPresets.{hpp,cpp}` — a stateless
  namespace of free functions (`presets_root`, `filename_slug`, `read_*_json`,
  `write_*_json`, `scan`, `save`, `remove`); JSON per preset + optional
  `<slug>.icon.png` sidecar; categories are subdirectories. Mirror this exactly.
- **Browser UI:** `src/SavedPresetsDrawer.cpp` renders tiles with a thumbnail
  (`MemoryImageDisplay` with `.imgPath`, line ~186) + name `text_button` + a
  delete control; it `scan()`s the folder each frame (cheap; files are tiny).
- **Thumbnail capture:** brushes use `SquareCanvasCaptureTool` (artist drags a
  square). **We don't need that** — see §5; we render the figure ourselves.

Net: most of this is *mirroring a proven ~320-line module*, not new infra.

---

## 3. Data already lives in the component

All fields are plain, serialization-friendly types in
`ArmatureCanvasComponent::Data` (`src/CanvasComponents/ArmatureCanvasComponent.hpp`):

```cpp
struct MatColor { std::string material; float r,g,b,a; };      // per material
// Character snapshot:
float height;                         // bone-length scale (1.0 = authored)
std::vector<MatColor> materialColors; // per-material RGBA overrides (by name)
std::vector<float>    shapeSliders;   // 22 shape-key slider values

// Scene snapshot:
float camYaw, camPitch, camDist, camTx, camTy, camTz;  // OrbitCamera
float fovDeg; bool ortho;                               // lens
float lightAz, lightEl, lightInt, lightAmb, lightSky;  // Lighting
```

The editor already holds live mirrors (`mHeight`, `mMatColors`, `mShapeSliders`,
`mCamera`, `mLight`, `mFovDeg`) and knows how to push them into the model — so
"apply preset" is just "set those, then re-render," reusing the same paths the
sliders use.

---

## 4. On-disk format

Two sibling libraries under `configPath` (nlohmann::json, like brush presets /
settings):

```
<configPath>/armature_characters/
    <slug>.json        character params
    <slug>.thumb.png   128×128 preview
<configPath>/armature_scenes/
    <slug>.json        scene params
    <slug>.thumb.png   128×128 preview
```

**Character JSON**
```json
{ "version": 1, "format": "inkternity-armature-character",
  "name": "Stocky elder",
  "height": 0.94,
  "materialColors": [ {"material":"mat_body","r":0.7,"g":0.55,"b":0.5,"a":1.0}, … ],
  "shapeSliders": [ 0.0, 0.3, -0.2, … ] }
```

**Scene JSON**
```json
{ "version": 1, "format": "inkternity-armature-scene",
  "name": "3/4 key light",
  "cam": {"yaw":0.5,"pitch":0.1,"dist":3.2,"tx":0,"ty":0.9,"tz":0},
  "lens": {"fovDeg":35,"ortho":false},
  "light": {"az":0.3,"el":0.6,"int":0.95,"amb":0.5,"sky":0.18} }
```

`version`/`format` guard forward-compat (unknown keys ignored on read, like the
brush JSON). Filename slug via a copied `filename_slug()`; a name that collides
with an existing slug is a save-time error (no silent overwrite), matching
brushes.

---

## 5. Thumbnails — free from `ArmatureBake`

We already render the figure offscreen. Reuse
`ArmatureBake::render_armature_rgba(model, viewProj, lightDir, amb, diff, sky, dim,
outRGBA)` at `dim = 128`, then `resetContext()` and PNG-encode with
`SkPngEncoder` (the brush-icon path already does this), writing `<slug>.thumb.png`.

- **Character thumb:** apply the character to the model, render under a fixed
  neutral 3/4 camera + default light (so thumbs are comparable). Restore the
  editor's live state afterward.
- **Scene thumb:** render the *current* model under the scene's own camera + lens
  + light (the thumbnail shows the angle and lighting mood).

This runs in the editor where the GL context is current (same place Bake runs), so
no deferral needed (unlike the file-dialog gotcha in PHASE9 M7).

---

## 6. UI — two new tabs

The editor tab bar (`ArmatureModalScreen::gui_layout_run`) is an `int mTab` with a
`tab(id,label,index)` helper; tabs are conditionally shown (Pose needs a skin,
Body needs the bundled rig). Append:

- **Character** → index 5. Shown only for the **bundled default rig**
  (`mBundledDefault`) — material names + the 22-slider config are default-rig-
  specific, so a character only meaningfully applies there (see Decision D2).
- **Scene** → index 6. **Always shown** — camera/light/lens are model-agnostic.

Each tab body (mirrors `SavedPresetsDrawer`):

1. **Header:** a name text field + **Save** button → snapshot current state,
   render thumb, `…Presets::save(...)`.
2. **Scrollable grid/list** of `scan()`ed presets: each tile = `MemoryImageDisplay`
   (the `.thumb.png`) + name; **click = apply** to the live editor (then
   `request_redraw()`); a small **×** = `remove()`.
3. **Empty state:** "No saved characters yet — tune the figure and Save."

Applying:
- **Character** → set `mHeight`, `mMatColors`, `mShapeSliders`; push to `mModel`
  (`set_height`, `set_material_color`, `apply_shape_sliders`).
- **Scene** → set `mCamera.*`, `mFovDeg`→`mCamera.fovY`, `mCamera.ortho`,
  `mLight.*`.

---

## 7. New / changed files

**New**
- `src/Armature/ArmaturePresets.{hpp,cpp}` — both libraries. One file, two small
  namespaces (`ArmatureCharacterPresets`, `ArmatureScenePresets`) sharing private
  helpers (`filename_slug`, a generic `scan`/`save`/`remove` over a subdir + a
  per-type JSON (de)serialize lambda). POD snapshot structs
  (`CharacterPreset`, `ScenePreset`) + a `thumbPath` populated by `scan`.
- (Optional) `src/Armature/ArmaturePresetThumb.{hpp,cpp}` — render+PNG-encode a
  128² preview, or fold into ArmaturePresets / the modal.

**Changed**
- `src/Armature/ArmatureModalScreen.{hpp,cpp}` — two tabs + their save/load/delete
  UI; snapshot/apply helpers; thumbnail render on save.
- `CMakeLists.txt` — add the new source(s).
- Docs: `MANUAL.md` (mention the Character/Scene tabs), this file's status.

**Reused as-is:** `main.conf.configPath`, `ArmatureBake::render_armature_rgba`,
`SkPngEncoder`, `MemoryImageDisplay`, the `SavedPresetsDrawer` tile/scan pattern,
the existing slider→model apply paths.

---

## 8. Milestones

- **M1 — storage + thumbnail core.** `ArmaturePresets` module (paths, slug, JSON
  I/O, scan/save/remove) for both types; the 128² render+PNG helper. No UI yet;
  unit-exercise by saving/loading a hardcoded preset. *(~1 day)*
- **M2 — Scene tab.** Save/list/apply/delete scene presets with thumbnails. The
  quick, low-risk win (11 floats + a bool; model-agnostic). *(~1 day)*
- **M3 — Character tab.** Same flow for height + materialColors + shapeSliders;
  apply to the live model; gate to the bundled rig. *(~1–1.5 days)*
- **M4 — polish + docs.** Naming UX (collision error, empty state), overwrite
  confirm, MANUAL/README, this doc → shipped. *(~0.5 day)*

**Rough total: ~3.5–4 days.** Low risk — no canvas-format change, no new storage
infra, thumbnails reuse the existing bake.

---

## 9. Decisions (LOCKED 2026-06-27 — zynx)

- **D1 — Presets are app-level, local, per-user.** ✅ Sidecars under `configPath`,
  exactly like brush presets; not synced, not in the `.inkternity` file.
- **D2 — Character presets target the bundled default mannequin (this phase).**
  ✅ Material colors key by material *name* and shape sliders by the default
  22-slider config, so they only fully apply to the default rig. The Character
  tab is **hidden for loaded/custom models**; Scene stays available for any model.
  (Best-effort apply to custom rigs is explicitly deferred.)
- **D3 — No "new figure from preset" entry yet.** ✅ You add an armature, open the
  editor, then load. A top-toolbar "new from character" is a later nicety.
- **D4 — Thumbnail size = 128².** ✅ Bigger than brush icons (64²) since a figure
  needs more detail; still tiny on disk.
- **D5 — Naming UX.** ✅ Inline text field + Save; slug collision = error toast
  (no overwrite), mirroring brushes. Rename = delete + re-save.
- **D6 — One `ArmaturePresets` module, two type-specific namespaces.** ✅ Shared
  private helpers; genericize only if a third preset kind appears.

---

## 10. Verification

1. In the editor (bundled rig): tune height/materials/shape keys, **Save** a
   character with a name → a thumbnail tile appears.
2. Close the editor, add a *new* armature, open it, **Character** tab → the saved
   tile is there; click it → the figure takes on the saved look. **Bake.**
3. Re-launch the app / open a *different* `.inkternity` file → the character is
   still listed (app-level persistence).
4. **Scene** tab: save a camera+light+lens setup; on another figure/file, apply
   it → identical framing + lighting. Thumbnail reflects the angle/mood.
5. Delete a preset (×) → tile + sidecar files removed; the editor stays clean.
6. A name colliding with an existing preset slug → save is refused with a notice.
