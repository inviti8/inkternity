# PHASE 10.1 — Particle secondary motion on motion-path groups

## Status

**SHIPPED (2026-07-09) — as a dedicated Anim-FX group.** A 4th Folder Mode:
every particle FX inside the folder travels the folder's `MotionPath` with real
secondary motion (the emitter moves in the sim while non-relative particles keep
their spawn coords → trail); multiple FX preserve their painted relative
arrangement (all get the same world delta). Reuses the shipped `MotionPath` +
`MotionPathTool` editor; a Preview toggle plays it in drawing mode. The
sim-position-along-path mechanism was chosen because the runtime we ship has **no
velocity-inheritance API** (§crux). Save format **INFPNT000030-31 / 0.29.0-0.30.0**
(the `animFxGroup` flag + a per-effect `boundsScale`). Delivered as FX-M1 (mode
scaffolding) + FX-M2 (the driver); a post-ship fix corrected the FX clip bounds
(the container's cached `worldAABB` wasn't refreshing as the Anim-FX collider
grew, so the effect clipped out of a frozen box) and exposed a **Bounds size**
slider on the particle brush. Two inherent constraints stand, as scoped:
**only non-relative effects trail** (relative-particle `.eff`s slide rigidly) and
**a long-path trail is a large animated region** (best for short/medium paths).

The scoping below is retained as the rationale trail (why a dedicated group, why
sim-position-along-path, the constraints).

## Goal

When a **particle effect** sits on a frame of a flip-book group that **travels a
motion path**, make the particles show **secondary motion** — trail / lag behind
the moving emitter (inertia), like embers streaming off a moving torch — instead
of sliding rigidly as a "sticker." Today they slide rigidly: the particle sim runs
in a **fixed local space** (`ParticleCanvasComponent.cpp:63-65`, `SetOrigin(0,0,
localScale)`), and the motion path is applied as a **draw-time canvas transform**
around the rendered frame (`DrawingProgramLayerListItem.cpp:340-361`) — it never
reaches the simulation, so every sprite (emitter + already-spawned particles)
moves together.

## The crux finding — no velocity inheritance in our runtime

**Two TimelineFX trees are vendored, and the attractive API is in the wrong one:**
- `deps/timelinefx/` — a **modern single-header C API** with
  `tfx_SetEffectVelocityMultiplier` / `tfx_MoveEffect` (`timelinefx.h:10525,10678`).
  **We do NOT compile against this.**
- `deps/timelinefx_legacy/` — the **legacy `TLFX::` C++ classes** the component
  actually uses (`LegacyFxRenderer : TLFX::ParticleManager`).

In the **legacy** runtime, particle spawn **explicitly zeroes the velocity vector**
and seeds speed only from the effect's velocity *graph* along the emission angle
(`TLFXEmitter.cpp:1237-1245`) — **no fraction of the emitter's own motion is ever
passed to spawned particles.** `Effect::SetVelocity` is a *scalar graph multiplier*,
not directional inheritance. So inherited-spawn-velocity secondary motion is
**not reachable via the API**; it would require **patching the vendored
`TLFXEmitter.cpp`** (forking the dependency).

## The viable approach — sim-position-along-path

Move the **effect's own position** along the path in sim space each frame (via
`Effect::SetX/SetY`, `TLFXEffect.h:793-794`), instead of leaving it at the origin.
Because non-relative particles are placed at **absolute world coords at spawn and
then integrate their own velocity independently of the emitter**, an emitter that
walks forward each frame **naturally leaves earlier particles behind → trail.**
This is the real trail mechanism, and it works with the legacy runtime as-is (no
vendored patch strictly required).

Per-frame path velocity is a cheap finite-difference of the existing sampler:
`MotionPath::sample(p) − sample(p − dt)` (the sampler returns `{pos, scale,
rotationDeg}` with **no velocity** today, `MotionPath.hpp:62`), converted world→sim
by `1/localScale`.

> **Do NOT use `SetOrigin` for this** — it's a *camera pan* (`_camtx/_camty`,
> `TLFXParticleManager.cpp:203-205`) that moves **all** particles rigidly = no
> trail. The lever is the **effect position**, not the manager origin.

## The three hard constraints (why it's not a clean win)

1. **The trail must live in the component's LOCAL footprint — which is the real
   limiter.** The sim is local to the particle component; a trail is only "real" if
   the emitter genuinely traverses the journey *in sim space*, spreading particles
   across it. But the component's **cache-clip bounds** are ~`max(400, localScale*250)`
   local units (`ParticleCanvasComponent.cpp:171-184`) — anything past that is
   **cropped**. So: it works cleanly for **short / local motion** (the emitter
   wanders within a reasonable footprint), but a **long cross-canvas traverse** needs
   the component footprint (and its per-frame invalidation rect + BVH bounds) grown to
   the path's extent — which is exactly the kind of large uncached region the app is
   perf-sensitive about. **Fundamental tension:** a real trail needs the sim to span
   the journey; the draw-transform (which keeps the component small) can't produce a
   trail at all. There is no free lunch — only "short trail cheaply" vs. "long trail
   expensively."

2. **Authoring dependency — only *non-relative* effects trail.** The lag emerges
   only if the effect's emitters spawn **non-relative** particles (`SetRelative`,
   `TLFXEmitter.cpp:963`). A relative-particle effect lives in the effect's local
   frame and transforms rigidly → still a sticker. This is per-`.eff` and must be
   audited / forced.

3. **Double-count with the existing draw-transform.** The motion-path draw-transform
   already carries the whole frame (emitter + particles) by the full path delta. If we
   *also* move the emitter in sim, the two compose — for particles to appear to lag in
   world space you must **drive the translate from the sim and drop it from the
   draw-transform for particle frames** (a special case — particle frames then travel
   by a different mechanism than raster frames in the same group), or reconcile with a
   negative sim velocity (finicky sign/scale/rotation + the 0–4 variable sub-steps and
   1-frame visibility lag, `ParticleCanvasComponent.cpp:106-114`). Easy to get visibly
   wrong (reverse-slide / double-speed).

## Simpler alternative — a dedicated Anim-FX group (RECOMMENDED, zynx 2026-07-09)

Rather than tack particle secondary-motion onto flip-book groups (whose
frames-shown-one-at-a-time + rigid draw-transform model *causes* the double-count
and the mixed-mechanism wart), add a **dedicated group type — Anim-FX** — designed
from the start for "move an FX along a path." This removes the two worst frictions:

| §-above friction (on flip-book) | On an Anim-FX group |
|---|---|
| **Double-count** with the motion-path draw-transform (§constraint 3) | **Eliminated** — travel is 100% sim-driven; there is no draw-transform to reconcile. |
| **Two travel mechanisms in one group** (particle vs raster frames) | **Eliminated** — the group is homogeneous; it exists to move an FX. |
| **Fighting the component's small fixed footprint** (§constraint 1) | **Honest, not a wart** — the group's bounds *are* the path bbox by design (size it to the path up front). |

**It reuses what shipped:** the folder-owned `MotionPath` + the entire
`MotionPathTool` editor (draw / curve / tangents / undo) drop straight in — an
Anim-FX folder owns a path exactly like a flip-book folder does, so the
path-authoring half is essentially free.

**Still inherent (unchanged by the group type):**
- **Only non-relative effects trail** (§constraint 2) — the lag is the sim keeping
  particles at their spawn world-coords; a relative-particle `.eff` still moves
  rigidly. Per-effect authoring constraint, documented.
- **A trail across a long path is a large animated region** — real trailing needs
  particles spread across the whole journey, so the uncached redraw area scales with
  path length (vs. the flip-book's cheap small-relocated region *with no trail*).
  Fine for short/medium paths; expensive for a long cross-canvas sweep. Now an
  explicit, expected property rather than a surprise.

**Shape:** a new **Folder Mode → Anim-FX** (4th mode alongside Normal / Parallax /
Flip-Book; mutually exclusive). The folder owns a `MotionPath`; each frame it
samples the path and drives the particle effect's sim position (`Effect::SetX/SetY`,
`TLFXEffect.h:793`) along it → native trailing; renders through the sim (no
draw-transform); its bounds/clip are sized to the path bbox. **Open impl questions:**
(a) how the group drives its child particle effect — expose a "set effect position
along path" hook on `ParticleCanvasComponent` (its effect handle is a local in
`play_effect`, `ParticleCanvasComponent.cpp:76`); (b) who owns the path-sized
bounds/clip (the group, overriding the component's `localScale*250` default).

**Effort ~3–5 days** — similar to the flip-book tack-on but a cleaner result (no
double-count tuning), plus the path editor is reused. This is the recommended
route if the feature is greenlit.

## Recommended architecture on flip-book (only if NOT doing the Anim-FX group)

For a **particle frame** of a motion-path group, replace the draw-transform's
**translate** with **sim-space effect motion** (keep scale/rotate on the
draw-transform), so travel comes from the emitter genuinely walking the path — no
double-count, real trail. Accept that the particle component's **bounds grow to the
path extent** (document the perf cost; best for short/medium paths). Keep raster
frames on the draw-transform. This is the cleanest split but means **two travel
mechanisms coexist in one group** (particle vs raster frames), which is the main
architectural wart.

Effort: **~3–6 days.**
- Path-velocity/position wiring into `ParticleCanvasComponent::update` (needs a
  handle to the spawned `eff`, currently local in `play_effect`, `:76`), world→sim
  conversion, `Effect::SetX/SetY`. ~1–2 days.
- Reconcile with the draw-transform (particle-frame translate via sim). ~1–2 days
  of tuning.
- Grow collider / cache-clip / invalidation bounds to the trail extent
  (`create_collider`, `get_obj_coord_bounds`). ~1 day.

## Risks

1. **API gap (highest):** no inheritance → accept "move-the-effect" semantics, or
   fork `TLFXEmitter.cpp`. We recommend the former.
2. **Footprint/clip sizing:** long paths → large uncached region (perf).
3. **Authoring:** only non-relative effects trail; must audit `.eff`s.
4. **Double-count tuning:** the sim/draw-transform reconciliation is finicky.
5. **Version skew:** the tempting velocity API is in the *unused* modern tree —
   any scoping that assumes it is wrong.

## Verdict

**Feasible, and a **dedicated Anim-FX group** is the clean way to do it** — it
eliminates the double-count and mixed-mechanism frictions and reuses the shipped
motion-path editor, so it's ~3–5 days for a genuinely clean result. Two inherent
constraints survive any architecture: **only non-relative effects trail**, and **a
long-path trail is a large animated (uncached) region** — so it's best for
**short/medium paths**. The tempting velocity-inheritance API is in the *unused*
modern TimelineFX tree; our runtime can't inherit velocity without forking the dep.

Recommendation: **greenlight only with a concrete use case** for FX trailing along
a path (embers off a moving object, a comet, a magic sweep). It's polish on an
already-complete feature, not a gap — but if wanted, build it as the **Anim-FX
group**, not a flip-book bolt-on.

## Out of scope

- Patching the vendored legacy `TLFXEmitter.cpp` to add true velocity inheritance
  (forks the dependency; only revisit if move-the-effect proves inadequate).
- Swapping to the modern `deps/timelinefx/` runtime (a large, separate migration).
- Trail on **relative-particle** effects (architecturally can't lag; would need a
  per-effect authoring change).
