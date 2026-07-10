# PHASE 10.1 — Particle secondary motion on motion-path groups

## Status

**SCOPED — decision-gated. Feasible, but more involved than the earlier "cool if
it did" framing (zynx, 2026-07-09).** The cheap path I'd hoped for — feed the
motion path's velocity to the emitter as an *inherited spawn velocity* — **does
not exist in the runtime we actually ship**. There is a viable path
(sim-position-along-path), but it carries three real constraints. Read §Verdict
before greenlighting. Follow-on to the shipped flip-book/motion-path feature
([PHASE10.md] + [MOTION-PATH.md]). No save-format bump (all runtime behaviour).

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

## Recommended architecture (if greenlit)

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

**Feasible, but a "yes-but," not a slam dunk.** Best suited to **short/medium
paths with non-relative effects**; long cross-canvas traverses fight the
local-footprint/clip model. The clean velocity-inheritance path isn't available
without forking the vendored runtime. Recommendation: **worth doing only if you
have a concrete use case for particle trails on paths and accept the
short-path/non-relative constraints** — otherwise defer. It's polish on an
already-complete feature, not a gap. If greenlit, take the "move-the-effect,
particle-frame translate via sim, grow the bounds" architecture above.

## Out of scope

- Patching the vendored legacy `TLFXEmitter.cpp` to add true velocity inheritance
  (forks the dependency; only revisit if move-the-effect proves inadequate).
- Swapping to the modern `deps/timelinefx/` runtime (a large, separate migration).
- Trail on **relative-particle** effects (architecturally can't lag; would need a
  per-effect authoring change).
