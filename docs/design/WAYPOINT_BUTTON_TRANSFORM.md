# WAYPOINT_BUTTON_TRANSFORM.md — per-waypoint position + scale for reader-mode nav buttons

**Status:** scoped / decisions locked 2026-10-05. **Not yet implemented — awaiting review.**
**Owner:** Inkternity client.
**Related:** `ReaderMode` (branch overlay), `Waypoint` (skins), PHASE1.md §5a (skins),
TRANSITIONS.md (transition auto-advance), the next-stop skin fix (`ReaderMode::resolve_skin_source_waypoint`, commit 15b250b).

---

## 1. Problem

Reader-mode navigation buttons are drawn by `render_reader_branch_overlay` (`src/ReaderMode/ReaderMode.cpp`)
as a **fixed bottom-center floating row**, each a square of `BRANCH_BUTTON_SIDE = 140 px`. A button
already wears the skin of the **resolved next-stop waypoint**. But every button is the same size and
pinned to the same place, so a skinned button can't be positioned where it belongs on the page or
sized to taste. Artists want to **adjust/set each button's location and scale**.

## 2. Decisions (locked)

1. **Transform lives per-waypoint, bundled with the skin.** A button takes its skin from the resolved
   next-stop waypoint (`resolve_skin_source_waypoint`); it takes its **position + scale from the same
   waypoint**, so art + placement + size travel together and are edited in the existing waypoint
   settings panel. Known quirk (accepted for v1): it is the *destination's* transform applied on the
   *predecessor's* screen, so the same destination reached from different pages can't differ. Per-edge
   overrides are a future option (§6).
2. **Opt-in override; keep the row as the default.** Default buttons stay in the managed bottom-center
   row (zero change to existing/branch files). Only a button whose waypoint has an explicitly
   **customized** position is pulled out and placed absolutely. Scale always applies (a bigger/smaller
   button still lays out fine inside the row).
3. **Normalized screen position** (0..1 of the viewport), resolution-independent. Default ≈
   `{0.5, 0.88}` (today's bottom-center).

## 3. Data model — `Waypoint` (`src/Waypoints/Waypoint.hpp`)

New serialized fields (mirror the existing per-field getter/setter/mutable + `publish_*_update` +
version-gated load pattern):

```cpp
Vector2f buttonPos      = {0.5f, 0.88f};  // normalized screen position of the button CENTER
bool     buttonPosCustom = false;         // false = stay in the default row; true = absolute placement
float    buttonScale     = 1.0f;          // multiplies BRANCH_BUTTON_SIDE; clamp ~[0.25, 4.0]
```

- `buttonPosCustom` is an explicit flag rather than comparing `buttonPos` against a sentinel — it makes
  "is this button placed?" unambiguous and gives a clean **Reset to default** (clears the flag, leaves
  `buttonPos` wherever it was).
- Getters/setters/`mutable_*` for each; `publish_button_transform_update` (one message covering the
  three fields is fine, or one per field to match the existing granularity).
- **Persistence:** written in `save_file`; read in `load_file` gated on version `>= 0.35.0`
  (**format bump INFPNT000036 / 0.35.0**). Also carried in the net `save`/`load` (session-stable).
  Old files load with defaults → identical to today.

## 4. Rendering — `render_reader_branch_overlay` (`ReaderMode.cpp`)

Each choice already resolves its skin-source waypoint (call it `W`). Extend that lookup to also read
`W.buttonScale`, `W.buttonPosCustom`, `W.buttonPos`.

- **In-row buttons** (`!buttonPosCustom`): unchanged layout, but the button's fixed size becomes
  `BRANCH_BUTTON_SIDE * buttonScale` (per-button sizing inside the existing flex row).
- **Customized buttons** (`buttonPosCustom`): **not** added to the row. Rendered as their own
  Clay floating element centered at `buttonPos * viewport`, sized `BRANCH_BUTTON_SIDE * buttonScale`,
  at the same z as the overlay. Same `BranchChoiceElement` draw/click logic (navigates to the
  immediate target; draws the resolved skin).
- The back button and any still-default choices keep the current bottom-center row. If every choice is
  customized, the row simply renders empty/absent (plus the back button).
- Overlap is the artist's responsibility (same as any manual layout); no auto-dodge in v1.

Implementation note: `BranchChoiceElement` currently fixes its size via `CLAY_SIZING_FIXED(BRANCH_BUTTON_SIDE)`.
It will take the per-button side (and, for customized ones, a floating attach point at the normalized
position) as constructor params.

## 5. Editing UI — `WaypointTool::gui_toolbox` (selected waypoint)

Add to the existing settings block (reusing `slider_scalar_field` + `publish_*_update`):
- **Button scale** slider (`buttonScale`, 0.25–4.0).
- **Button X** / **Button Y** sliders (`buttonPos`, 0..1). Editing either sets `buttonPosCustom = true`.
- **Reset button position** — clears `buttonPosCustom` (returns the button to the default row).

(Only meaningful for a waypoint that can be a button's skin-source, i.e. a stop that something
navigates toward; harmless otherwise.) A live drag-to-place handle in reader mode is deferred (§6).

## 6. Non-goals / future
- Drag-to-place the button directly in reader mode (nicer authoring; more work).
- Per-edge/per-choice transforms (so one destination can sit differently per source page).
- Auto-dodge / collision avoidance for overlapping custom buttons.
- Anchoring to a canvas position instead of the screen (screen-space is the right default for nav UI).

## 7. Touch points
- `src/Waypoints/Waypoint.{hpp,cpp}` — 3 fields + accessors + publish + save/load (version-gated).
- `src/VersionConstants.{hpp,cpp}` — bump to INFPNT000036 / 0.35.0.
- `src/ReaderMode/ReaderMode.cpp` — per-button size + absolute placement for customized buttons;
  `BranchChoiceElement` gains size/position params.
- `src/DrawingProgram/Tools/WaypointTool.cpp` — scale + X/Y sliders + reset.

**Effort:** moderate, ~1–2 sessions. No new geometry; format bump only.
