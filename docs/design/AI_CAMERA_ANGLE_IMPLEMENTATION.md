# AI_CAMERA_ANGLE_ADJUST — Implementation Plan

Concrete, phased build plan. Companion to `AI_CAMERA_ANGLE_ADJUST.md` (research +
scope). **Scope locked:** operate on a **rasterized** selection, return a **raster**
placed on its own layer; the original is never touched. Sovereign target model:
**Qwen-Image-Edit + Multiple-Angles LoRA** (Apache-2.0), self-hosted; a hosted API
(fal / Gemini) is the interchangeable fallback since the integration is
endpoint-agnostic.

**Validation done (own art):** three site tests —
- ✅ **Profile turn:** kept the full figure, the pencil line style, *and* the exact
  costume/design (apron dress, star buckle, boots). Excellent.
- ✅ **High-angle turn:** kept style + design, but **reframed/zoomed and cropped**
  the legs off and left a minor stray artifact. Acceptable with framing handling.
- ❌ **GoEnhance:** **changed the character design** (apron dress → pants + plain
  tee, accessories lost) and added a **watermark**. Disqualified — this is the
  failure mode to design against.

→ Two acceptance bars, not one: **(a) line-style preservation** *and* **(b)
character/identity preservation** (costume, accessories, proportions). Framing
control is the main *engineering* task; picking a model/prompt that holds design
(and has no watermark) is the main *model-selection* task — and a further point for
the **sovereign self-host** (no watermark, full control of the model + prompt).

**Sequencing (per user):** Phase 0 stands up and tunes the model with **zero
Inkternity edits**; only after the recipe + HTTP contract are locked do we touch
the app.

---

## Phase 0 — Stand up the model & lock the recipe (NO app edits)

**Goal:** a running endpoint + a validated prompt/param recipe + the exact
request/response contract the app will code against.

1. **GPU:** local 24 GB card if available, else rent (RunPod/Vast 4090 ~$0.4–0.7/hr).
2. **Serve:** ComfyUI with **Qwen-Image-Edit-2509/2511** + **Multiple-Angles LoRA**
   (`dx8152` natural-language or `fal` pose-token) + **`lightx2v` Lightning 4-step**
   for speed. Expose ComfyUI's `/prompt` HTTP API behind an HTTPS/auth shim. Keep
   the pod warm; pre-download weights.
3. **Batch-test on our own sketches** (reuse `Pictures/ai-camera-angle-tests`) and
   lock:
   - **Prompt template** that holds flat pencil/line style (e.g. "same flat 2D
     pencil-sketch line-art style, clean linework, no added shading/rendering,
     plain gray background") + **negative** cues to kill stray marks.
   - **Angle→param mapping = azimuth / elevation / distance** (the LoRA's native
     96-pose grid: 8 azimuths × 4 elevations × 3 distances — confirmed to be what
     multipleangles.app and the other good-result sites expose). These map 1:1 onto
     our UI knobs. Validate the usable range on *our* content — profile (~90°
     azimuth) looked fine, so the ~45° cap is conservative; find where big-rotation
     back-view fabrication starts to degrade and cap the UI there.
   - **Framing control (the key experiment):** test (a) **padding the input** with
     a margin so the subject sits smaller and reframing has room, and (b) explicit
     "keep the entire figure in frame, preserve original framing and scale"
     prompting. Measure residual scale/crop drift → informs the app-side placement
     (below).
   - **Style-LoRA decision:** if the bare angle LoRA photo-ifies, add an
     illustration/style LoRA at reduced angle-strength (two-LoRA graph). Record
     the final graph.
   - **Resolution / latency** target (≤768–1024px, Lightning 4-step).
4. **Deliverable — the HTTP contract** the app will implement against: endpoint
   URL, auth header, request (input image encoding + angle params/prompt),
   response (image encoding), and typical latency. Written down so Phase 1 is
   mechanical.

**Exit gate:** we can POST a sketch + angle and get back a result that preserves
**both line style AND character design** (costume/accessories/proportions), is
acceptably framed, watermark-free, in a few seconds, repeatably.

### Phase 0 runbook (RunPod — concrete)

**Model manifest** (HF, all Apache-2.0):
- `Qwen/Qwen-Image-Edit-2511` (base) · `fal/Qwen-Image-Edit-2511-Multiple-Angles-LoRA`
  (azimuth/elevation/distance) · `lightx2v/Qwen-Image-Edit-2511-Lightning` (4-step
  speed) · *(optional)* an illustration/style LoRA only if flat art photo-ifies.

**Deploy** (`RUNPOD_API_KEY` already in `.env`):
1. **Pod first (fastest to eyeball):** RunPod ComfyUI template on an **L40S 48 GB**
   (fits the 20B comfortably) or a 4090 24 GB. Download the manifest into
   `models/…`. Build (or import — the fal LoRA's HF page / community templates ship
   a ready graph) the workflow: `LoadImage(input_image.png)` → Qwen-Image-Edit +
   Multiple-Angles LoRA + Lightning LoRA → positive prompt containing the literal
   token `__PROMPT__` → 4-step sampler → `SaveImage`.
2. **Then Serverless (the app's target):** deploy `runpod-workers/worker-comfyui`
   with the models on a network volume; note the **endpoint ID**.

**Prompt templates (identity-locking — the thing that beat GoEnhance):**
- *Positive:* `<angle tokens>, same character design, costume and accessories,
  identical proportions; flat 2D pencil-sketch line art, clean linework, hatching
  only, no color, no shading/rendering, plain flat gray background`
- *Negative:* `color, photorealistic, 3d render, cinematic lighting, watermark,
  text, cropped, out of frame, extra limbs, different outfit, altered design`
- *Framing:* pad the input with ~15–20% margin before upload so the model has room
  and is less likely to zoom/crop.

**Validate** — export the working graph as **API-format JSON**, then run the test
client against the endpoint on our own sketches:
```
uv run python scripts/reangle_test.py \
  --endpoint <RUNPOD_ENDPOINT_ID> --workflow workflow_api.json \
  --image "C:/Users/surfa/Pictures/ai-camera-angle-tests/original.png" \
  --prompt "profile view, same character design, flat 2D pencil line art" \
  --out reangled.png
```
Iterate prompt/angle/padding across the 4 test sketches until design + style +
framing hold. **The locked `workflow_api.json` + prompt + endpoint ID + the
request/response the script exercises = the Phase 1 HTTP contract.**

---

## Phase 1 — Inkternity integration (model-agnostic core)

**Everything here reuses existing pieces; the only genuinely new code is the HTTP
call + async marshaling.**

**Reuse map:**
- **Rasterize selection → PNG:** `RasterFlatten`'s render-to-`SkSurface` +
  `readPixels` pattern; encode via Skia (`SkPngEncoder` / `SkImage::encodeToData`).
- **HTTP:** **libcurl is already linked** (C2PA/Stellar) — a small POST helper
  (`src/Helpers/HttpClient.{hpp,cpp}`), multipart or JSON+base64.
- **JSON:** nlohmann (already used in `GlobalConfig`).
- **Async pattern:** mirror **`FileDownloader`** (already used for
  `droppedDownloadingFiles`) — background request, poll on the main loop, do the
  canvas mutation on the main thread.
- **Place result:** `DrawingProgram::add_file_to_canvas_by_data(fileName, bytes,
  dropPos)` → lands as an `ImageCanvasComponent`.
- **Config:** add `aiReangleEndpoint` (URL) + `aiReangleApiKey` to `GlobalConfig`
  (save/load like the other fields); key can also come from an env var so it's not
  stored in the file.

**New pieces:**
1. `src/AI/CameraAngle.{hpp,cpp}` — the driver: gather the current selection (or
   active layer) → render to a padded `SkSurface` (padding per Phase-0 finding) →
   PNG-encode → build request → hand to the async HTTP helper.
2. `src/Helpers/HttpClient.{hpp,cpp}` — thin libcurl wrapper (POST bytes/JSON,
   headers, timeout), off the UI thread.
3. **Result handling (main thread):** decode returned image → place via
   `add_file_to_canvas_by_data` **on a NEW layer** at the original selection's
   world bounds, so the artist can rescale/reposition (this is the framing safety
   net + the non-invasive story). Undoable via the normal component-add path.
4. **Menu/tool entry:** a "AI Reangle" `menu_popup_text_button` (next to
   Consolidate/Flatten) for the MVP; upgrade to a small tool with angle controls
   in Phase 2.
5. **Friend access** for `CameraAngle` on `DrawingProgram` (selection + add-file),
   like the RasterFlatten functions.

**Exit gate:** select art in Inkternity → invoke AI Reangle → a reangled image
appears on a new layer, seconds later, without freezing the UI; undo removes it.

---

## Phase 2 — UX + demo polish

- **Angle controls:** small popup — rotate slider (± the Phase-0 ceiling) + tilt +
  push-in, mapped to the recipe; or 3–4 preset buttons (profile L/R, up-angle,
  down-angle) for a clean demo.
- **In-flight state:** spinner / progress on the pending layer (reuse the
  download-progress-bar pattern already in `DrawingProgram::draw`).
- **Errors/timeouts:** graceful toast (`Logger "USERINFO"`), no crash on network
  failure.
- **Demo safety:** point the endpoint at the self-hosted (sovereign) box; **pre-
  cache 2–3 killer results** and an offline fallback in case conference Wi-Fi or
  the pod misbehaves.
- **Non-invasive framing:** result layer named/created distinctly; original layer
  untouched; artist scales/moves the result to taste.

---

## Hosting economics & on-device architecture

### Cloud cost per reangle (bursty per-image workload — pay only per use)
| Volume | Cheapest viable setup | $/reangle | ~$/mo |
|---|---|---|---|
| **Low ~100/mo** | **fal.ai** managed `multiple-angles` (same model, zero ops), **or Modal's free $30/mo tier** | $0.037 / **$0** | ~$4 / **$0** |
| **Med ~5k/mo** | self-hosted **serverless scale-to-zero** (Modal / RunPod L40S) + one warm worker in peak hours | ~$0.005–0.015 | ~$25–200 |
| **High ~100k/mo** | dedicated **L40S pod** (RunPod $0.79/hr ≈ $569/mo) + serverless spillover; SaladCloud 4090 ($0.16/hr) as a batch floor | ~$0.006–0.012 | ~$570–1,700 |

- **Serverless (scale-to-zero) is the right shape for a "click reangle" feature** — beats an always-on pod until very high volume. The one catch is **cold start** (loading ~40 GB of weights = 20–60 s after idle); solve with FlashBoot / memory-snapshots or a single warm worker during peak hours.
- **Deals worth stacking:** **Modal $30/mo free ≈ 5–13k reangles/mo** (covers the demo + early production for $0); **NVIDIA Inception** (~$100K AWS / $150K Nebius credits, no equity); RunPod $1,000 starter; Google for Startups up to $350K.
- **Baseline recommendation:** start on **fal.ai (~4¢)** or **Modal free tier**; move to **self-hosted serverless** as volume grows; **dedicated pod** only at high scale.

### On-device (the preferred path) — real for a minority segment, via 4-bit quant
The 20B "won't fit" framing is misleading: **nobody runs it at BF16 (~40 GB)** locally. The real footprint is 4-bit.
- **The unlock — Nunchaku / SVDQuant 4-bit + 4-step Lightning:** ~**3–4 GB VRAM**, runs on **RTX 3060 12 GB** (even 6 GB), ~**19 s on a 3090**, seconds-to-low-tens on 12 GB cards. Interactive enough for a button.
- **Apple Silicon 32 GB+:** viable via **MLX / mflux** (unified memory is the enabler), but slower (~30–90 s on M3/M4 Max) and the tooling is immature. 16 GB Macs (the majority) are marginal.
- **Out of scope for the reference model:** CPU-only (minutes), and **browser/WebGPU** — a 20B MMDiT is ~20× past what WebGPU handles (~1 B models); the prior in-browser model was far smaller.
- **No smaller model matches quality** — the Multiple-Angles capability is bound to the 20B base; the lever is **quantization/steps, not a smaller model**.
- **Reach:** ~30–40 % of desktop-app users have ≥12 GB NVIDIA; ≥24 GB is low single digits; near-zero on AMD/Intel/integrated (tooling is CUDA/Metal-only). So on-device serves the **enthusiast segment**, not the majority.
- **Integration reality:** a ~4–13 GB model download + a native inference runtime (Nunchaku CUDA / MLX Metal) embedded in the C++ app. Feasible (team has shipped on-device), but **a real project, not a library call.**

### Recommended architecture: **HYBRID**
1. **Cloud-first for the demo, launch, mass market, and fallback** — fal.ai / Modal serverless. Zero client footprint, any machine, instant. **This is the Friday demo path.**
2. **On-device as a fast-follow for capable machines** (the enthusiast segment + the team's stated preference): NVIDIA ≥12 GB → Nunchaku 4-step; Apple Silicon ≥32 GB → MLX/mflux. Detect capability at runtime; cloud fallback otherwise.
3. On-device delivers exactly what makes it worth it — **privacy, zero per-image cost, offline, no rate limits** — for the users who care most; cloud covers everyone else.
- **Don't** ship on-device-*only* (strands >half of users + all AMD/integrated). **Don't** skip on-device long-term if the enthusiast segment and infra savings matter.

> **For your own testing right now:** if you have a **≥12 GB NVIDIA card**, install **Nunchaku** and the Qwen-Image-Edit + Multiple-Angles + 4-step Lightning models — you can reangle unlimited, locally, free, today (no cards, no credits). That both unblocks testing and is a direct taste of the on-device path.

## Open decisions (to settle before/at Phase 0)

- **GPU location:** local 24 GB card vs rented pod for the demo. (Rented is the
  safe default; local is cheaper if the hardware exists.)
- **Demo endpoint:** self-hosted ComfyUI (sovereign, the goal) vs a hosted API as
  fallback — the app reads a config URL either way, so this can flip late.
- **Angle UI:** free sliders vs preset buttons (presets demo cleaner).
- **API key handling:** env var (recommended, nothing stored) vs a config field.

## Risks

- **Framing/scale drift** (seen in testing): mitigated by input padding + framing
  prompt + on-its-own-layer placement the artist can adjust. Primary risk; Phase 0
  measures how well padding tames it.
- **Character/design drift** (seen with GoEnhance: outfit + accessories changed):
  the worst failure — mitigated by model choice + identity-locking prompt +
  moderate angles; the Phase-0 exit gate rejects any model that reinterprets the
  design. Self-host also avoids third-party **watermarks** (seen with GoEnhance).
- **Occasional stray artifacts** (seen): negative-prompt + minor manual erase;
  acceptable on a reference layer.
- **Network/warm-pod for the demo:** pre-cache + offline fallback.
- **Latency:** Lightning 4-step keeps it to seconds; warm-up at startup.
- **Big-angle fabrication** past the validated ceiling: cap the UI to the Phase-0
  range.

## Immediate next step

Execute **Phase 0**: stand up ComfyUI + Qwen-Image-Edit + Multiple-Angles LoRA +
Lightning on a 24 GB GPU, batch-test on our sketches, and lock the prompt + framing
recipe + HTTP contract. No Inkternity code until that gate is green.
