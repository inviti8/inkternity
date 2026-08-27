# REANGLE_PIPELINE.md — Style-Preserving AI Camera Reangle

**What this is.** A working, self-hosted pipeline that takes a single character
drawing and produces a small **camera-angle adjustment** (a gentle turn) that
**preserves the artist's exact linework**. It is the concrete, validated realization of
"Approach D" from [AI_CAMERA_ANGLE_ADJUST.md](AI_CAMERA_ANGLE_ADJUST.md) §11. This doc
is the standalone reference: how it works, how to reproduce it, what we learned, and how
it becomes an Inkternity feature.

**Status (2026-08-26):** prototype **working** and de-risked on a RunPod A6000. Demo
artifacts in `scripts/image_out/drawingspinup_alice2/` (char2, clean) and
`scripts/image_out/drawingspinup_alice/` (char1 + all experiments). Reusable tooling in
`scripts/reangle/`. Not yet integrated into Inkternity (see §7).

---

## 1. The core idea (and why it's built this way)

Generative reangle (Qwen-Image-Edit + angle LoRAs) **cannot** preserve an artist's exact
style — style is baked into the model weights, so output drifts to the model's house
look, and rotation hard-switches between discrete poses (see AI_CAMERA_ANGLE_ADJUST.md
§top + §5). Wrong engine for a *style-locked, small, smooth* adjustment.

**This pipeline never regenerates the art.** It:
1. builds a rough **3D proxy** of the character (AI), then
2. **re-projects the artist's own pixels** over that proxy's **depth**, shifting them by
   parallax for a small camera turn.

Every output pixel is the original drawing. **Style is preserved by construction** — we
*move* the linework, we don't *redraw* it. The price is that we can only turn a small
amount before the proxy's guessed sides / disocclusion holes show — which is exactly the
"small adjustable angle" the feature wants.

---

## 2. Pipeline overview

```
 drawing.png (any size, character on any background)
   │
   ▼  [Stage 0] matte + normalize            scripts/reangle/prep_input.py  (isnet)
 texture.png  (512×512 RGBA, character on transparent, alpha = silhouette)
   │
   ▼  [Stage 1] single-image → 3D            DrawingSpinUp: mv.py + recon.py
 mesh.obj  (vertex-colored; we use it ONLY for depth)
   │
   ▼  [Stage 2] front depth                  (rendered inside depthwarp.py, pytorch3d)
 depth map  (front orthographic, aligned to texture by silhouette bbox)
   │
   ▼  [Stage 3] parallax warp (DIBR)         scripts/reangle/depthwarp.py
 warp_-018…+018.png  →  ping-pong GIF        (the reangle)
```

- **Stage 0 — `prep_input.py`** — segments the character off its background with
  **isnet** (`isnet_dis.onnx`, same model DrawingSpinUp uses), trims to the silhouette,
  pads square, resizes to **512×512 RGBA** (alpha = foreground mask). This is the only
  input requirement; DrawingSpinUp's `mv.py` reads the alpha as the mask.
- **Stage 1 — DrawingSpinUp** (`2_charactor_reconstructor/mv.py`, then `recon.py`):
  **Wonder3D** generates 6 orthographic views (front, front-L/R, L, R, back);
  **NeuS/instant-nsr** (tiny-cuda-nn) fuses them into a textured `.obj`. We do **not** run
  DrawingSpinUp's Step-1 contour remover (SharePoint-gated, optional) or Step-3 animation
  (needs Mixamo rig). **We only need the geometry, for depth.**
- **Stage 2 — depth** — inside `depthwarp.py`: rasterize the mesh from a front
  **orthographic** camera (pytorch3d, headless CUDA — no GL/X), take the z-buffer, and
  **align it to `texture.png` by matching silhouette bounding boxes** (robust to camera
  scaling). Result: a depth map pixel-aligned to the drawing, 1 = nearest.
- **Stage 3 — DIBR warp** — `depthwarp.py`: for each angle θ, shift every pixel
  horizontally by `amp · width · (depth − 0.5) · tan(θ)` and **inverse-warp** the original
  RGBA (`cv2.remap`). Inverse warp = no holes, slight edge smear (fine at small angles).
  Compose over white, emit stills; ImageMagick assembles a ping-pong GIF.

Typical settings: angles **±18°** in 3° steps, `amp ≈ 0.13`, output 512².

---

## 3. Two output modes (a real trade)

| Mode | Script | Angle range | Style | Use |
|---|---|---|---|---|
| **Depth-warp (DIBR)** | `depthwarp.py` | small (±~20°) | **exact** — original pixels | the default; "style sacred" reangle |
| **Mesh turntable** | `render_p3d.py` | wide (±30°+, full turn) | softer — loses crisp lines | "rough 3D reference" turnaround |

`render_p3d.py` renders the vertex-colored mesh **FLAT/unlit** (pytorch3d `AmbientLights`)
so no fake 3D shading is added. Good for a big rotation when soft is acceptable; not for
crisp-style demos.

---

## 4. What we learned (design decisions, all validated on the pod)

1. **Pose, not medium, is the constraint.** Gray pencil is fine. **Thin protrusions kill
   reconstruction** — char1's outstretched arm + frilly skirt collapsed to blobs; char2
   (arms-down, jeans, boots) reconstructed to a clean, fully-rotatable mesh.
   **Feature rule: neutral stance, arms in, no thin extended limbs / frills** — how
   turnaround references are drawn anyway.
2. **Input preprocessing does NOT fix a bad mesh.** *High-contrast* input made the mesh
   *flatter* + white (gray mid-tones are load-bearing depth cues for Wonder3D).
   *Line-removal* (approx. of Step-1) cleaned surface tone but left geometry unchanged.
   Bg-color / contrast / line tweaks are dead ends. (`make_noline.py` is the line-removal
   experiment.)
3. **The mesh is a depth proxy, not a render.** It's too blobby to render as the final
   image, but its *depth* is usable — and the depth-warp only ever shows the real drawing,
   so blobbiness stops mattering.
4. **Monocular depth (Depth-Anything-V2) does NOT work here.** It estimates *scene* depth
   (head-near/feet-far gradient), not the character's **front-back relief**; warping by it
   **shears** the figure instead of turning it, and detrending doesn't recover usable
   relief. A real 3D reconstruction's depth is required. (`da_reangle.py`,
   `detrend_warp.py`; see `…/drawingspinup_alice2/4_mesh_vs_DA.gif`.)

---

## 5. Reproduction — the environment

Built on a **RunPod A6000 (48 GB)**, image `runpod/pytorch` (Ubuntu 22.04, driver 550).
Isolated so it doesn't disturb the base image. Full recipe:

```sh
# Miniconda + isolated env (system had no conda, no nvcc)
#   NOTE: recent Miniconda gates the defaults channel behind a ToS accept.
conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main
conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/r
conda create -y -n dsu -c conda-forge --override-channels python=3.8
conda activate dsu

# torch 2.0 / cu118 + the matching CUDA 11.8 toolkit (for nvcc)
pip install torch==2.0.0+cu118 torchvision==0.15.1+cu118 torchaudio==2.0.1+cu118 \
    --extra-index-url https://download.pytorch.org/whl/cu118
conda install -y -c "nvidia/label/cuda-11.8.0" cuda-toolkit

# DrawingSpinUp + its deps
git clone https://github.com/LordLiang/DrawingSpinUp.git
cd DrawingSpinUp && pip install -r requirements.txt
pip install "huggingface_hub==0.25.2"          # diffusers 0.19.3 needs cached_download (removed in hub 0.26)

# compiled CUDA extensions (Ampere sm_86)
export CUDA_HOME=$CONDA_PREFIX; export TCNN_CUDA_ARCHITECTURES=86
pip install ninja
pip install git+https://github.com/NVlabs/tiny-cuda-nn/#subdirectory=bindings/torch
git clone https://github.com/cprogrammer1994/python-mesh-raycast && \
    cd python-mesh-raycast && python setup.py develop && cd ..
# pytorch3d (prebuilt wheel for this exact combo)
pip install fvcore iopath
pip install --no-index pytorch3d \
    -f https://dl.fbaipublicfiles.com/pytorch3d/packaging/wheels/py38_cu118_pyt200/download.html

# models
#   isnet (matting/bg-removal):
wget https://huggingface.co/stoned0651/isnet_dis.onnx/resolve/main/isnet_dis.onnx \
    -O 2_charactor_reconstructor/dis_pretrained/isnet_dis.onnx
#   Wonder3D (auto-downloads from HF flamehaze1115/wonder3d-v1.0 on first mv.py run)
```

**Gotchas we hit (all fixed above / below):**
- nerfacc JIT-links against `-L$CUDA_HOME/lib64 -lcudart`, but the conda toolkit uses
  `lib/`. Fix: `export LIBRARY_PATH=$CONDA_PREFIX/lib:$LIBRARY_PATH` and
  `ln -s $CONDA_PREFIX/lib $CONDA_PREFIX/lib64`; also set
  `LD_LIBRARY_PATH=$CONDA_PREFIX/lib`.
- Point `HF_HOME=/workspace/hf_cache` so Wonder3D (~5 GB) lands on the big volume.
- Headless render: pyrender/pyglet fight X11/EGL — **use pytorch3d instead** (pure CUDA,
  no display). That's why `render_p3d.py`/`depthwarp.py` use pytorch3d.

**Run (per character):**
```sh
cd 2_charactor_reconstructor
python prep_input.py <drawing.png> ../dataset/AnimatedDrawings/preprocessed/<uid>/char
python mv.py    --uid <uid>          # Wonder3D 6-view (~2–3 min)
python recon.py --uid <uid>          # NeuS mesh    (~6–8 min)
python depthwarp.py ../dataset/AnimatedDrawings/preprocessed/<uid>/mesh/*.obj \
    ../dataset/AnimatedDrawings/preprocessed/<uid>/char/texture.png out "-18,-15,…,18" 0.13 0
```

---

## 6. Scripts (in `scripts/reangle/`)

| Script | Role |
|---|---|
| `prep_input.py` | Stage 0 — matte (isnet) → 512² RGBA `texture.png` (+ `mask.png`, preview) |
| `depthwarp.py` | Stages 2–3 — mesh → front ortho depth → DIBR parallax reangle (the product) |
| `render_p3d.py` | Mesh-turntable mode — flat/unlit pytorch3d render at arbitrary angles |
| `make_noline.py` | Experiment — approx. contour removal (line inpaint); showed input tweaks don't fix geometry |
| `da_reangle.py` | Experiment — Depth-Anything-V2 monocular-depth reangle (negative result) |
| `detrend_warp.py` | Experiment — detrend DA depth (still fails) |
| `render_orbit.py`, `render_proj.py` | Earlier render attempts (pyrender / front-projection); superseded |

`mv.py`, `recon.py`, `isnet_dis.onnx`, Wonder3D weights are DrawingSpinUp's / upstream —
not vendored here.

---

## 7. Inkternity integration — reuse the armature 3D viewer (next build)

> **The network half is now live and documented separately.** The service that
> produces the mesh runs at `https://img.hvym.link`; its client contract —
> request shape, status codes, the ≥300 s timeout a cold start needs, threading,
> and libcurl specifics — is in **[REANGLE_API.md](REANGLE_API.md)**. This
> section covers everything *after* the `.glb` arrives.

**Design decision:** don't ship a server-side reangle *slider* that round-trips an image
per angle. Instead **deliver the mesh once** and let the artist **manipulate it in
Inkternity's existing armature 3D viewer**, then **bake the chosen camera view to the
canvas** — the established armature flow. The 2D depth-warp (§2 Stage 3) becomes the *3D*
generalization: project the artist's original art onto the mesh and orbit it live,
in-app, no per-angle server call.

### 7.1 Why this beats a server slider
- **Interactive full-3D camera**, not one slider axis; the artist finds the angle.
- **All local after one fetch** — server produces the mesh; rotation + bake are on-device.
- **Reuses ~80% of the armature pipeline** (viewer, camera, static-model load, bake).
- **Self-limiting UX** for the style window (§7.4) — nicer than a hard-clamped slider.

### 7.2 What already exists in-tree (`src/Armature/`, PHASE9)
- **`ArmatureBake::render_armature_rgba(model, viewProj, light…, dim, out)`** — renders a
  model at *any* view-projection into an RGBA8 buffer for the canvas. **This is the
  bake-to-canvas step, already built** (own FBO + `GrDirectContext::resetContext()` gate).
- **Static-model path** — `ArmatureModel::load_static` / `is_static()` (M7): a flattened,
  non-rigged imported mesh where the editor "hides the Pose/Body tabs; only
  **camera/light/materials/lens** apply." A reangle mesh *is* a static model — this is the
  entry point.
- **`ArmatureModel::load_from_memory(data,size)`** — loads a `.glb` from memory, so a
  server-delivered mesh drops straight in.
- The armature editor already has camera manipulation + the create/load-model placement
  path.

### 7.3 The one real gap — textured rendering (the net-new work)
The armature renderer is **untextured**: the vertex layout is
`pos(3) normal(3) joints(4) weights(4)` (`FLOATS_PER_VERT = 14`, **no UVs**) and the
shader uniforms are `viewProj / lightDir / color / ambient / diffuse / sky` — flat
per-material color + Lambertian (the gray-mannequin look), **no sampler**. Loading our
mesh as-is renders a gray blob, not the drawing.

So we must add a **textured shader path**:
1. Extend the static-model vertex layout with **UV(2)** (→ 16 floats/vert for static
   meshes; keep the skinned layout as-is, or gate by a `hasUV` primitive flag).
2. A second GL program: sample a base-color **texture** (the artist's art) instead of the
   flat material color; keep the existing lighting if desired, or render **unlit** for the
   truest style (matches `render_p3d.py`'s AmbientLights choice — no fake 3D shading).
3. Upload the texture (glTF embeds it) and bind it in `ArmatureModel::draw` /
   `ArmatureBake`.

This is the only substantial in-app addition; everything else is reuse.

### 7.4 Critical: texture with the ORIGINAL art, front-projected
The mesh's *own* texture (Wonder3D/NeuS) is the **soft, style-lost** version — never show
it for the style path. Vertex colors won't work either (linework is too high-frequency
for per-vertex color). The mesh must carry a **UV atlas of the artist's original drawing,
front-projected** — baked by the server (§7.5). Then the viewer samples *your pixels*.

**Style validity window (same physics as the 2D warp, now interactive):** near-front the
projection is crisp and exact; as the camera orbits away, the front-projected texture
**stretches** on oblique faces and the **back is undefined**. Interactively this is
*self-limiting* — the artist rotates until it starts to smear, then backs off. Optionally
offer a **"reference" toggle** that swaps in the mesh's own (soft, full-coverage) texture
for wide turns — the two modes of §3, now as a viewer switch rather than two endpoints.

### 7.5 Server — produce a textured `.glb` once (not per-angle)
A stateless GPU service, one call per drawing:
- **`POST /reangle-mesh`** — body: rasterized selection (PNG). Returns a **`.glb`**: the
  reconstructed mesh + a **UV texture atlas holding the front-projected original art**.
- Internally: Stage 0 (matte) → Stage 1 (reconstruct) → **bake original-art UVs**:
  - *Simplest:* **front-planar UV** (`uv = normalize(vertex.xy)`), texture = the original
    front image — front-facing geometry shows crisp art (back faces mirror-smear; hidden
    by the angle window). This is the depth-warp's exact equivalent.
  - *Better:* **xatlas unwrap** + bake the front projection into the atlas (back/oblique
    regions left undefined → later filled by disocclusion inpainting, §9).
  - Export with cgltf/tinygltf; embed the texture so `load_from_memory` gets everything.
- **Cache the mesh/glb by input hash** — re-fetch is instant; only new drawings pay the
  ~10-min (Wonder3D+NeuS) or ~seconds (TripoSR, §8) reconstruction.
- Warm models at startup; deploy as RunPod **serverless** GPU (scale-to-zero) or a
  persistent 24 GB box, image = the §5 environment.
- **Client HTTP:** a minimal outbound client is net-new (in-tree today: only P2P sync +
  a C2PA/Stellar curl shim). One multipart POST + a `.glb` download.

### 7.6 End-to-end flow
1. Select strokes → rasterize via `RasterFlatten::readPixels` → PNG.
2. `POST /reangle-mesh` → receive textured `.glb` (spinner on first fetch; cached after).
3. `ArmatureModel::load_from_memory` → **static** model in the armature viewer.
4. Artist orbits the camera freely (existing viewer); the style window self-limits (§7.4).
5. **Bake** the chosen view via `ArmatureBake::render_armature_rgba` onto its **own raster
   reference layer** — original linework stays authoritative and untouched (non-invasive;
   "both camps" framing, matching AI_CAMERA_ANGLE_ADJUST.md §7).

---

## 8. Productization notes

- **Licensing — important.** **Wonder3D weights are CC-BY-NC (non-commercial)** → the
  current stack is **research/demo only**. For a shipped product, swap the geometry
  backbone for an **MIT** single-image-to-3D — **TripoSR** (MIT, ~seconds) or InstantMesh
  — and take depth from that. (Monocular depth is *not* a substitute — see §4.4.) Verify
  DrawingSpinUp's own license (unstated in repo). pytorch3d = BSD, isnet = permissive.
- **Speed.** Current reconstruction ~10 min/char (Wonder3D + 3000-iter NeuS). TripoSR
  would cut this to seconds and remove tcnn/NeuS entirely. The warp itself is already
  seconds.
- **On-device (future).** The whole thing is small enough to target on-device eventually
  (quantized 3D model + the numpy/cv2 warp), mirroring the /lepus-/lupus approach — no
  server round-trip.
- **Sovereign.** Everything runs self-hosted; no external API, no per-image fees.

---

## 9. Limitations / current roughness / tuning

- **Small-angle ceiling (~±20°).** Beyond it, the proxy's guessed sides + disocclusion
  holes appear. Wider swings need **disocclusion inpainting** (fill the revealed slivers)
  — not yet implemented; small angles avoid it.
- **Roughness at edges.** Inverse warp smears slightly at depth discontinuities; a
  forward-warp + hole-fill would be crisper but needs inpainting. `amp` trades turn
  strength vs smear.
- **Pose dependence** (§4.1) — neutral poses only, for now.
- **Resolution.** Prototype runs at 512²; the warp is resolution-independent, so a
  higher-res `texture.png` (project the original at full res, aligned to the mesh) is a
  straightforward quality win.
- **Depth polarity / framing** are handled but have CLI knobs (`depthflip`, `amp`,
  `scale`) if a new character needs adjustment.

---

## 10. Immediate next steps

1. ✅ **Validate the MIT light path (DONE).** **TripoSR** benchmarked GREEN — mesh
   directly, MIT, ~2 s warm, ~340× faster than Wonder3D+NeuS, proper object relief. It is
   the shipped backbone (hvym-img-tools `docs/BENCHMARK.md`); Wonder3D/NeuS is demo-only.
2. ✅ **Textured render path (DONE).** `ArmatureModel` static meshes now carry a UV VBO
   (attrib loc 4; the skinned rig's 14-float layout is untouched) + decode the glb's
   embedded texture (Skia, in `upload_gl`) + render through a separate **unlit** sampling
   program in `draw()` — positions are world-baked so no skinning. `ArmatureBake` unchanged.
3. ✅ **`/reangle-mesh` server (DONE).** Live at `https://img.hvym.link/tools/reangle`
   (`POST` a drawing → textured `.glb`, cached by input hash). Contract in
   [REANGLE_API.md](REANGLE_API.md).
4. ✅ **Inkternity client (DONE).** `src/AI/ReangleClient.{hpp,cpp}` (async curl) +
   `src/AI/ReangleFlow.{hpp,cpp}` — "AI Reangle (3D)" menu → `SquareCanvasCaptureTool`
   frames the character → POST → `load_reangle_mesh_into_canvas` places the textured `.glb`
   as a static model → double-click to orbit + bake. Builds clean; **runtime test against
   the live endpoint is the remaining validation** (set `HVYM_TOOLS_KEY`).
5. ✅ **Warm-lease endpoints (DONE, service side).** The proxy now exposes
   `POST`/`GET`/`DELETE /warm` — a client-held **lease** (20 s renew, 60 s TTL, refcounted,
   auto-release) that keeps a GPU worker awake so the artist pays the cold start once per
   session instead of on every idle gap. Contract in [REANGLE_API.md](REANGLE_API.md) §11.
   **Remaining CLIENT work** (see [REANGLE_CLIENT_HANDOFF.md](REANGLE_CLIENT_HANDOFF.md)):
   a `WarmLease` (mirroring `ReangleClient`'s worker-thread pattern) + a header-bar
   "enable inference" toggle, plus optional auto-warm when the reangle panel opens.
6. **Runtime-verify the client** against the live endpoint (one drawing end-to-end: `.glb`
   loads, linework is on the mesh not a gray mannequin, second request is `X-Cache: HIT`).
7. **Add disocclusion inpainting** (server-side, into the UV atlas) to push the angle
   window wider; **higher-res** original in the atlas for crisper bakes. Client-side: a
   proper in-flight spinner (MVP uses `USERINFO` toasts) and optional angle-window UX.
