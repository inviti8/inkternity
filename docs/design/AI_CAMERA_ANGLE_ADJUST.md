# AI_CAMERA_ANGLE_ADJUST.md

Research + design for an **AI-augmented camera-angle adjustment** feature — the
ability to take a drawn sketch and see/generate it from a different camera angle.
Target inspiration: **Adobe Illustrator "Project Turntable."** Demo context: DELF
(Hong Kong), an AI-collaboration conference; the feature should be impressive to
the AI crowd yet non-invasive to traditional artists. Timeline: aiming for a demo
this Friday.

**SCOPE LOCKED (decision):** we do **NOT** preserve vectors — the feature operates
on a **rasterized image** and returns a **raster** placed on the canvas. Rotation
is **capped at ~45°** to stay inside the models' reliable range and avoid
unreliable "invent the whole back" fabrication. This lands us squarely in **Tier
T1** (§3) — the Friday-feasible path — and removes the two hardest problems
(editable-vector output, big-angle coherence). The one remaining make-or-break is
**style preservation on line art** (§5), resolved by the test in §10.

**UPDATE (2026-08-26) — DIRECTION CHANGED to projection (Approach D, §11).** We ran
the T1 generative path end-to-end (self-hosted **Qwen-Image-Edit-2511 + Multiple-
Angles LoRA + Lightning 4-step**, on an A6000) against our own sketches. Two findings
kill it for *this* feature: (1) **style is baked into the model weights** — even with
angle-LoRA strength dialed down and denoise tuned, output drifts to the model's house
style (Disney-ish volume, invented shading); it cannot hold the artist's exact pencil
line. (2) **rotation is not continuous** — at fixed seed the result *hard-switches*
between discrete poses as denoise crosses thresholds (a jump at ~0.93), so there is no
smooth, controllable small-angle knob. Generative reangle stays useful for a one-shot
dramatic turnaround, but it is the wrong engine for a *style-locked, small, smooth*
adjustment. The chosen direction is now **Approach D: build a 3D proxy from the sketch
and re-project the artist's *own pixels* onto it, then rotate the camera within a
locked angle window** — style is preserved *by construction* because we move the
original linework rather than regenerate it. See §11.

---

## 1. The target: Adobe Project Turntable

Take a **2D vector illustration**, rotate it in 3D (spin + tilt) like a turntable,
and get **flat, editable 2D vector art at every settled angle** — not a 3D model,
not a raster. It **hallucinates occluded geometry** the artist never drew (the
signature demo: a horse drawn with 2 legs reveals 4 when rotated) and **snaps back
exactly** to the original view.

- **Status:** debuted as an **Adobe MAX 2024 Sneak** (Oct 2024) → Illustrator Beta
  → **generally available in Illustrator v30.3, 31 Mar 2026**. Generates up to
  **74 editable multi-angle views** from one drawing; built on **Firefly**.
- **Scope Adobe states:** standalone objects with clear recognizable forms
  (characters, icons, product illustrations). **Not** abstract shapes, live text,
  or heavily layered/scene art. Adobe is emphatic it does **not** produce a
  conventional editable 3D model — it makes "linked 2D artwork that simulates
  different viewpoints."
- **Team:** Adobe Research — Zhiqin Chen (presenter), Nathan Carr, Matt Fisher,
  Siddhartha Chaudhuri, et al. A follow-up, "Project Turn Style," followed.

Refs: research.adobe.com/news/turntable-and-project-turn-style-…,
helpx.adobe.com/illustrator/desktop/use-generative-ai/view-artwork-from-any-angle.html,
redsharknews.com/adobe-illustrator-turntable-generally-available.

---

## 2. How Adobe does it (method) — and why it's the hard version

The load-bearing disclosure from Adobe Research: they **initially planned to "lift
2D vector art into 3D, then rotate and flatten it back to 2D" — and abandoned it**
because it "wasn't suited to the unique qualities of vector art." Instead they
built **"a network that could reimagine the artwork as it would be drawn from
different perspectives."**

So Turntable is **not** a stitch of off-the-shelf parts and **not** depth/mesh
re-projection. It is a **bespoke, vector-native, 3D-aware *generative* network
that redraws the linework per viewpoint**, preserving colors/stroke styles/intent,
with **rotational coherence** and **exact snap-back**, trained on vector art.
(No public paper describes Turntable; Zhiqin Chen's lineage — IM-Net, BSP-Net,
Neural Dual Contouring, DECOR-GAN — is neural-implicit 3D + generative shape
modeling, the plausible internals. Treat as informed inference.)

**The five hard things Adobe actually solved, that no open tool gives you:**
1. vector **in** → editable vector **out** (real paths),
2. **rotational/temporal coherence** of the linework as it spins (stable path
   identity, not reshuffled every frame),
3. **stroke/color/style preservation**,
4. **exact snap-back** to the original,
5. **in-style hallucination** of unseen sides.

---

## 3. Blunt feasibility verdict

**Reproducing faithful, vector-preserving Turntable is deep research — not a
Friday build, and not a weeks build at that fidelity.** It's a custom model
trained on vector art plus a Firefly-scale generative stack; Adobe explicitly
dropped the naive pipeline we'd reach for first. We should not promise Turntable.

What *is* achievable falls on this spectrum:

| Tier | What it is | Feasible? | Quality on line art |
|---|---|---|---|
| **T1 — single-angle generative reangle** | "Show this drawing from another angle" via a hosted image-edit model. Not a turntable; one new view at a time. | **Yes, Friday** (hosted API, seconds) | Good *if* the right model + style-locking (see §5) — else it photo-ifies |
| **T2 — rough turntable approximation** | Image→3D mesh → orbit-render → re-vectorize. Spins, invents sides. | Weeks, rough | Blobby geometry, raster-first, style lost, paths flicker frame-to-frame → a **reference turnaround**, not editable coherent vectors |
| **T3 — faithful Turntable** | Coherent editable vector at every angle, snap-back, style preserved | **Research-grade; out of reach** | — |

**Read:** for Friday, **T1** is the honest, demoable "AI camera-angle adjustment."
**T2** is a real but lesser future feature (reference/concepting). **T3** is
Adobe's research moat.

---

## 4. Model landscape (for the T1 single-angle path)

Nearly every "change the camera angle" site is a thin wrapper over one of these:

| Model | Angle control | Open weights | License | Access | Price/img |
|---|---|---|---|---|---|
| **Qwen-Image-Edit + Multiple-Angles LoRA** | **Numeric knobs** (rotate°, tilt −1..1, push-in, wide-angle) | **Yes (20B)** | **Apache-2.0** (base + LoRA) | fal.ai, Replicate; **self-host on 24GB GPU** | ~$0.035/MP |
| **Nano Banana (Gemini 2.5/3 Flash Image)** | Natural language | No (API) | Commercial via API | Google AI Studio / Vertex, OpenRouter, fal | ~$0.039 |
| **FLUX.1 Kontext** | Natural language | dev open (12B) | **dev = non-commercial** ($999/mo for commercial) / pro API | fal, Replicate | $0.015–0.08 |
| **Seedream 4.x (ByteDance)** | Natural language | No (API) | Commercial via API | fal, Together, WaveSpeed | ~$0.027–0.03 |

The **fal.ai Qwen Multiple-Angles endpoint** is the only one with **numeric knobs
that map 1:1 to UI sliders** (`rotate_right_left`, `vertical_angle`,
`move_forward`, `wide_angle_lens`). Qwen is also the only option that is
parametric **and** Apache-2.0 **and** self-hostable — start on fal for the demo,
move in-house for a fee-free product later.

---

## 5. CRITICAL: does it survive on *line art*? (style preservation)

This is the make-or-break for a drawing app, and the answer is nuanced:

- **The raw Qwen Multiple-Angles LoRA is the *wrong* tool for flat art used bare.**
  It was trained on **Gaussian-splat photo renders** — its prior **photo-ifies**
  drawings (adds volumetric shading, "solidifies" toward realism). Every demo is
  photographic; no write-up shows it holding flat cel/line style raw.
- **Illustration-native / better options:**
  - **FLUX Kontext "Character Turnaround" LoRA** — explicitly "works best with
    stylized/illustrated characters, poorly on real photos." Open, ComfyUI — but
    inherits FLUX's **non-commercial** license (→ pro API for product).
  - **Nano Banana Pro / Seedream 4.5** — top quality + character consistency,
    native anime; but will photo-ify **if the prompt lets it** — must explicitly
    lock flat style.
  - **Qwen + a style/illustration LoRA stacked at reduced angle-strength** — the
    commercially-clean fix, but needs tuning.
- **Techniques that hold style:** reference-sheet anchoring (generate a 3-view
  sheet first), verbatim trait-locking prompts, explicit flat-style cues ("flat
  cel-shaded 2D, clean line art, no rendering"), style medium keyword **last**.
- **Hard limits:** ~**45° is the reliable rotation ceiling**; 90°/back views
  fabricate design elements (plausible guess, not derivation). **Plan for a human
  line-cleanup pass.**

**Implication:** the model choice for line art must be **validated empirically on
our own sketches** before we commit — this is the single most important next step
(§8), and it's cheap.

---

## 6. Open approximation stack (for the T2 turntable-ish version, later)

All raster/photo-trained; none vector-native:
- **Image→3D mesh (invents back/sides):** **TRELLIS** (MIT, 8–24GB), **Hunyuan3D
  2.1** (commercial-with-terms, 10–29GB), **TripoSR** (MIT, ~6GB, fastest/roughest),
  **InstantMesh**. Then re-render an orbit and vectorize with **VTracer** (MIT,
  color) or Potrace (B/W).
- **Novel-view diffusion (no mesh, closest *in spirit* to "redraw from angle"):**
  **Stable Virtual Camera** (non-commercial), **Qwen angle LoRA**.
- **Depth 2.5D (small angles only):** Depth Anything V2 → displaced mesh; breaks
  past ~20–30°, can't invent occlusion.

**Failure modes on flat/line art:** blobby/wrong geometry (flat shading misread as
depth, thin limbs collapse); raster-first with **noisy, frame-to-frame-inconsistent
re-vectorized paths** (no editable continuity → a turnaround GIF, not vectors);
**style/stroke lost**; works only on clean single recognizable objects.

---

## 7. Inkternity integration (T1 path)

- **No AI/vision/ML runtime and no outbound HTTP client exist** in the codebase
  (only P2P collab sync + a C2PA/Stellar curl shim). So T1 needs a **minimal
  outbound HTTP client + async + image up/download** built fresh.
- **Reusable pieces:** `RasterFlatten`'s `readPixels` to rasterize the selection →
  PNG for upload; `ImageCanvasComponent` to place the returned image on a new
  layer; `StrokeVectorizeTool` to optionally vectorize it (the hybrid).
- **Flow:** select strokes → rasterize → POST to the chosen endpoint with angle
  params → poll → place the result **as a reference layer** (or vectorize).
- **Non-invasive framing:** the AI result lands on its **own layer as a
  reference/draft**; the artist's original linework stays authoritative and
  untouched — it's a *3D-mannequin-style reference*, not a replacement. If
  vectorized, the artist owns editable lines. This is the "both camps" story.

---

## 8. Recommendation

1. **Don't promise Turntable (T3).** Be explicit internally and on-stage that
   we're doing AI camera-angle *reference*, not Adobe's vector turntable.
2. **Friday demo = T1**, single-angle generative reangle via the best
   line-art-preserving hosted API, result placed as a reference layer (optionally
   vectorized). No GPU box to babysit on the floor; **pre-cache 2–3 killer
   examples** as a network fallback.
3. **DO THIS FIRST, before any code (≈1 hour, a few dollars):** run **3–4 of our
   own representative sketches** through **Nano Banana Pro**, **FLUX Kontext
   turnaround LoRA**, and **Qwen + style LoRA** at ~30–45°, and eyeball **style
   preservation**. This empirically picks the model *and* confirms the feature is
   viable on our content. Everything downstream depends on this result.
4. **Sovereign (fully self-hosted) path — the product direction:** **Qwen-Image-
   Edit-2509/2511 + a Multiple-Angles LoRA + a `lightx2v` Lightning 4-step LoRA**,
   served via **ComfyUI API** or **diffusers+FastAPI** on a **single 24 GB GPU**
   (FP8 ~16 GB / GGUF Q4 ~12 GB), ~seconds/edit. **Everything Apache-2.0** (base +
   angle LoRA) → no fees, no license negotiation, ship it in-house. **Caveat for
   line art:** the angle LoRA is photo-splat-trained and **photo-ifies flat art**,
   so the sovereign line-art stack is **Qwen + angle LoRA + an illustration/style
   LoRA at reduced angle-strength** (a two-LoRA ComfyUI graph) — still fully
   self-hosted, just tuned. This is the same model the hosted angle sites run, so
   the demo API and the sovereign product are the *same engine*.
   Non-sovereign alternatives: **FLUX.1 Kontext [dev]** has a better
   *illustration-native* turnaround LoRA but a **non-commercial license** ($999/mo
   to ship); **Nano Banana / Seedream** are API-only (can't self-host). If quality
   trumps sovereignty for the demo, a commercial API is fine short-term.
5. **Roadmap:** T2 (image→3D + vectorize) as a later "rough turntable reference"
   feature; T3 stays Adobe's research delta.

---

## 9. Risks & open decisions

- **Photo-ify on line art** (the big one): mitigate by model choice + style-lock
  prompting; **resolved by the §8.3 test**, not by more research.
- **Network dependency** (demo): hosted API on conference Wi-Fi → pre-cache + have
  an offline path.
- **Authorship / "AI slop"**: mitigate with the reference-layer / vectorize-to-
  editable framing (§7).
- **~45° ceiling + occlusion invention**: keep demo angles moderate; expect a
  cleanup pass; big rotations fabricate.
- **Licensing for product:** Qwen Apache-2.0 (self-host, fee-free) vs FLUX
  non-commercial vs API-only ongoing cost — pick per priority (§8.4).
- **No editable-vector output** from any accessible tool: T1 output is raster (or
  noisy re-vectorized); accept raster reference for now — editable coherent
  vectors is the Adobe moat.

---

## 10. Immediate next step

Run the **§8.3 style-preservation test on our own sketches** (fal/Replicate/Gemini
API, ~$ a few, ~1 hour). If a model holds the drawn style at ~30–45°, T1 is a go
for Friday and we wire the minimal HTTP client + reference-layer placement. If
they all photo-ify our art, we know before writing code, and we fall back to the
strongest honest alternative (a hosted reangle framed purely as a rough reference,
or the deterministic perspective warp for flat-art tilt).

---

## 11. Approach D — AI-augmented 2.5D projection (CHOSEN)

**Core idea.** Don't ask a model to *redraw* the character at a new angle (that's
§1–§5, and it photo-ifies). Instead: use AI to build a rough **3D proxy** of the
character, **project the artist's original rasterized drawing onto it as texture**,
then **rotate the camera a small, locked amount** and render. The pixels that move on
screen are the artist's *own strokes* — style is preserved **by construction**, not by
coaxing a generative prior. This is depth-image-based rendering (DIBR) / camera
projection, "AI-augmented" only in that AI supplies the proxy geometry (and, later,
fills disoccluded slivers).

**Why it beats generative for *this* feature** (the two empirical findings, §top-update):
- *Style:* generative style is baked into weights → drift. Projection re-photographs
  the real linework → **zero style drift on visible faces**.
- *Control:* generative rotation hard-switches between discrete poses. A camera orbit
  is **continuous and exact** — 5°, 12°, 23°, snap back to 0° perfectly.
- *Trade:* projection can't invent the true back of the character; **rotating too far
  exposes the proxy's guessed sides + disocclusion holes.** So we **cap the angle** to
  the window where visible geometry is still the artist's front-projected pixels. That
  cap *is* the feature's honesty boundary, and it's exactly the "lock the degrees"
  instinct from the original brief.

**Chosen tool: DrawingSpinUp** (SIGGRAPH Asia 2024, LordLiang et al., CityU HK —
`github.com/LordLiang/DrawingSpinUp`). Purpose-built for **single character drawing →
3D**, and uniquely style-aware: it *predicts and removes the drawing's contour lines*
(FFC-ResNet/LaMa) before lifting to 3D (contours confuse multiview diffusion), then
*restores that contour style* on the rendered result — precisely the style-preservation
problem we hit. Backbone: **Wonder3D** (6-view ortho diffusion, HF
`flamehaze1115/wonder3d-v1.0`, auto-downloaded) → **instant-nsr / NeuS** (tiny-cuda-nn)
reconstruction → textured `.obj`. Runs on ~11 GB; we're prototyping on the A6000.

**Pipeline (no-animation prototype — decision: no animation for initial impl):**
1. **Matte** the sketch off its background → 512×512 **RGBA** `texture.png` (alpha =
   foreground mask). This is the only manual prep; `mv.py` derives the mask from alpha.
2. **`mv.py --uid <id>`** → Wonder3D generates 6 orthographic views (front, front-L/R,
   left, right, back) with normals + masks.
3. **`recon.py --uid <id>`** → NeuS/tcnn fuses the views into a textured mesh
   `mesh/it3000-mc512-f50000_c_r_s_cbp.obj`.
4. **Render the mesh at a small orbit** (±~15–30°) → static reangle stills.
   *(We render with a standalone renderer; DrawingSpinUp's own Blender path is Step-3
   animation, which we skip.)*
   - **Step-1 contour removal is OPTIONAL for the prototype:** `mv.py` falls back to
     `char/texture.png` when the contour-removed image is absent, so the CityU-SharePoint
     `experiments.zip` (wget-hostile) is **not** on the critical path. Contour removal
     only sharpens geometry; add it later if the proxy is muddy.

**Angle locking & disocclusion.** Small window → visible faces are the front
projection (artist's pixels, crisp). Past the window, two artifacts appear: the
Wonder3D-**generated** side/back texture (style-drifted, since Wonder3D *is* generative)
and **disocclusion holes** where rotated-away geometry reveals untextured backface.
Both are hidden by capping the orbit. If we later want a wider swing, disoccluded
slivers can be inpainted by a **texture-fill model** (StyleTex / FlexPainter /
MV-Adapter, researched under the Hunyuan3D pass) — deferred; the small-angle demo
avoids the need.

**Inkternity integration (later, T1-analogous flow of §7).**
- We already have 3D systems in-app (PHASE9 armature + raw-GL-on-Ganesh render path),
  so the *render-a-mesh-at-an-angle* half is native; only the drawing→mesh step is
  external/AI.
- Flow: select strokes → rasterize (`RasterFlatten::readPixels`) → send to the
  reconstruction service → receive mesh (or pre-rendered angle stills) → place the
  rotated result **on its own raster reference layer**. Same **non-invasive** framing as
  §7: the artist's original linework stays authoritative; the reangle is a
  3D-mannequin-style *reference*, not a replacement.
- **No animation** in the initial implementation (explicit decision) — static
  small-angle stills only.

**Open items.**
- **License:** DrawingSpinUp repo license not clearly stated — **verify before any
  productization/redistribution** (Wonder3D is CC-BY-NC for weights → also
  research/demo-only until cleared). Fine for a prototype + on-stage demo; gate the
  shipped product on a license that permits commercial use, or swap the geometry
  backbone (TRELLIS/TripoSR are MIT).
- **Proxy fidelity on thin limbs / extended poses** (e.g. the alice test's outstretched
  arm) — validate; thin geometry + T-pose limbs are Wonder3D's known weak spot.
- **Sovereign/on-device:** same story as §8.4 — the whole stack self-hosts on one
  24–48 GB GPU; on-device is plausible later (tcnn + Wonder3D quantized), mirroring the
  /lepus-/lupus approach.

**Status (2026-08-26): PROTOTYPE WORKING — depth-warp is the winner.** Ran the full
DrawingSpinUp pipeline end-to-end, sovereign, on an A6000 (isolated py3.8/torch2.0-cu118
env; tiny-cuda-nn + mesh-raycast + pytorch3d compiled). Findings:

- The pipeline reconstructs a coherent 3D character + 6-view turnaround, BUT the raw
  **mesh is too blobby** to render directly (thin arm + frilly skirt collapse — a
  fidelity ceiling of Wonder3D+NeuS, not an input problem). Its vertex-colors hold
  shading, not crisp lines.
- **Input-preprocessing does NOT fix the blobbiness** (tested empirically):
  *high-contrast* input made the mesh *flatter* + white (the gray mid-tones are
  load-bearing depth cues for Wonder3D); *line-removal* (approx. of the SharePoint-gated
  Step-1 contour remover) cleaned surface tone but left geometry unchanged. So bg-color /
  contrast / line tweaks are dead ends for geometry.
- **WINNER — depth-warp (DIBR):** use the mesh only as a rough **depth proxy** and
  inverse-warp the artist's **original drawing** horizontally by depth·tan(angle) for
  small camera angles. Result: **style preserved 100%** (every pixel is the real
  linework) with a coherent gentle turn. `scripts/image_out/drawingspinup_alice/` holds
  the artifacts (`6_depthwarp_reangle.gif` is the demo). The blobby mesh no longer
  matters — we never show it, only the depth it provides.
- **Monocular depth shortcut — TESTED, does NOT work.** Hoped Depth-Anything-V2 (MIT,
  fast, pixel-aligned) could replace the heavy 3D reconstruction as the depth source. It
  can't: DA estimates *scene* depth (a head-near/feet-far gradient — where the standing
  figure sits in space), NOT the character's **front-back relief**. Warping by it makes
  the figure **shear/lean** (head swings, feet planted) with disocclusion tears, not turn
  in place. Detrending the gradient doesn't recover usable relief. So the reangle needs a
  **real 3D reconstruction's** depth (object relief), which earns the pipeline's keep.
  Side-by-side: `scripts/image_out/drawingspinup_alice2/4_mesh_vs_DA.gif`.
- **Productization path (revised):** to lighten/license-clean the depth source (Wonder3D
  is CC-BY-NC), swap in a lighter **single-image-to-3D** that still yields true geometry
  — e.g. **TripoSR (MIT, ~seconds)** — and take depth from *that*. Monocular depth is out;
  a fast 3D model is the target. Open: disocclusion fill for wider angles (small angles
  avoid it); validate on more characters; try TripoSR depth.
- **Pose is the real constraint (validated on a 2nd char).** Medium (gray pencil) is
  fine; what breaks reconstruction is **thin protrusions** — char1's outstretched arm +
  frilly skirt collapsed to blobs, but char2 (arms-down, jeans, boots) reconstructed to a
  clean, coherent, fully-rotatable mesh. **Feature usage rule:** best results from a
  neutral stance (arms in, no thin extended limbs / frills) — which is how turnaround
  references are typically drawn anyway. Two output modes fall out: *depth-warp* (exact
  style, small angle) as the default, and *mesh turntable* (softer, wide angle) as a
  "rough 3D reference." Artifacts: `scripts/image_out/drawingspinup_alice2/`.

Repo tooling (on the pod, reusable): `prep_input.py` (isnet matte→512 RGBA),
`render_p3d.py` (headless pytorch3d flat render), `depthwarp.py` (the DIBR reangle).
