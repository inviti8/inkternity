# MESH_REFERENCE.md — sketch → orbitable 3D reference (client design)

**What this is.** The Inkternity client-side design for the service's new
**`/tools/mesh`** endpoint: one rough sketch in, one **untextured** `.glb` out — a
3D reference the artist orbits and *draws over*, and keeps in a library to reuse
across frames and scenes. This is the companion to [REANGLE_PIPELINE.md](REANGLE_PIPELINE.md);
where reangle *moves the artist's pixels* to a new angle, mesh returns *geometry only*
and the artist supplies the linework.

**Status (2026-08-27):** endpoint is **LIVE** (`km99b7mrj2f85r`, TRELLIS, all-MIT;
verified end to end through the proxy — hvym-img-tools `docs/tools/mesh.md`). Client
**Phase 1 (ToolClient refactor) and Phase 2 (MeshFlow MVP) are built and pushed**
(branch `reangle-pipeline`, commits `f377287`, `6b2b418`); not yet runtime-tested
against the live endpoint. Phases 3–5 (per-endpoint warming, library, multi-view)
are still design.

**Service spec (authoritative for the wire):** `../../../hvym-img-tools/docs/tools/mesh.md`
and `docs/CLIENT.md`. The Inkternity-facing reangle contract
[REANGLE_API.md](REANGLE_API.md) already covers the shared HTTP behaviour (timeouts,
TLS, threading) — mesh rides the same rules.

---

## 1. Why this is mostly reuse

Because the mesh is **untextured**, the hardest client work is already done. An
untextured static model renders through `ArmatureModel`'s **existing flat/lit
shader** — the gray-mannequin look — which is exactly what a draw-over reference
should be. No texture atlas, no WYSIWYG capture, no tone handling, none of the
reangle texture saga applies.

What mesh shares with reangle, verbatim:

| Piece | Reused as-is |
|---|---|
| Load path | `ArmatureModel::load_from_memory(data, size, err, zUpToYUp)` — static model |
| Orientation | **Z-up** (TRELLIS, like TripoSR) → the `zUpToYUp` correction we built |
| Render | the flat/lit shader (`ArmatureModel::draw` non-textured branch) — gray reference |
| Editor | `ArmatureModalScreen` orbit/camera/light/lens, `open_for` |
| HTTP | the async `curl_multi` worker, TLS `NATIVE_CA`, 300 s timeout, glTF validation |
| Warm | the `WarmLease` toggle machinery (per-endpoint, see §7) |
| Config | `HVYM_TOOLS_KEY` / endpoint via `ReangleFlow::resolve_config` (rename → shared) |

What is genuinely new: a **second endpoint**, and a **library** of reusable
meshes keyed by content hash.

---

## 2. The wire contract (from the service spec)

```
POST {base}/tools/mesh        multipart/form-data
X-API-Key: <scoped key>
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `image` | file | *required* | Rough sketch (PNG/JPEG), any size/background — the service mattes it |
| `target_faces` | int 2k–200k | `20000` | Decimation target (absolute count). 20k ≈ 0.3 MB, keeps ~99.5% of the silhouette |
| `seed` | int | `0` | Fixed → reproducible + cacheable; change to reroll |

**Success — `200`, body is the raw `.glb`** (`model/gltf-binary`), **untextured**,
same wire shape as reangle → feeds straight into `load_from_memory`.

New response header to capture:

| Header | Use |
|---|---|
| `X-Cache-Key` | `sha256(image + params)` — the content address, i.e. the **library asset id** |
| `X-Cache` / `X-Tool-Version` | as reangle |

Everything else (binary body vs JSON errors, status codes, ≥300 s timeout, cold
start, retries-are-safe) is identical to REANGLE_API.md §2–§4. **Do not re-derive
it** — mesh obeys the same contract.

---

## 3. The shared-core client (the refactor)

Today `ReangleClient` owns a full `curl_multi` worker that is 95% generic. Extract
that into one **`ToolClient`** that both tools ride; the per-tool code becomes a
thin call that names the tool and supplies its fields.

### 3.1 `src/AI/ToolClient.{hpp,cpp}` (new; lifted from ReangleClient)

```cpp
namespace AI {

class ToolClient {
public:
    struct Request {
        enum class Status { IN_PROGRESS = 0, SUCCESS, FAILURE };
        std::vector<uint8_t> glb;          // binary body on success (glTF-validated)
        std::string          error;
        std::string          toolVersion;  // X-Tool-Version
        std::string          cacheKey;     // X-Cache-Key (mesh; empty for reangle)
        long                 httpCode = 0;
        std::atomic<bool>    cacheHit{false};
        std::atomic<float>   progress{0.0f};
        std::atomic<Status>  status{Status::IN_PROGRESS};
    };

    // One multipart field: a named file (the image) or a named text value (a param).
    struct Field {
        std::string name;
        std::string textValue;                 // used when bytes is empty
        std::vector<uint8_t> bytes;             // file part (image)
        std::string filename, mimeType;         // for file parts
    };

    static void init();
    static void cleanup();

    // POST {baseUrl}/tools/{toolName} with `fields`. `apiKey` → X-API-Key. Returns
    // immediately; poll the handle. Native only (WASM stub fails fast — no CORS).
    static std::shared_ptr<Request> request(const std::string& baseUrl,
                                             const std::string& apiKey,
                                             const std::string& toolName,
                                             std::vector<Field> fields);
};

}  // namespace AI
```

This is exactly `ReangleClient` today with three changes: the URL is
`/tools/{toolName}` instead of a hardcoded path; the multipart parts come from
`fields` instead of a fixed image+mc_resolution; and it captures `X-Cache-Key`.
Everything else — the background thread, `CURLSSLOPT_NATIVE_CA`, 300 s timeout with
low-speed abort off, TLS verification on (the key is in a header), binary-safe
collection, the `glTF` magic check, thread-safe `cross_platform_println` from the
worker — moves over unchanged.

### 3.2 `ReangleClient` becomes a thin wrapper

Keep `ReangleClient::request(png, baseUrl, apiKey, mcResolution)` as a one-liner
that builds the two fields and calls `ToolClient::request(..., "reangle", fields)`,
so `ReangleFlow` needs no change. (Or inline it into `ReangleFlow` and delete
`ReangleClient` — either is fine; the wrapper keeps the diff smaller.)

`ReangleClient::init/cleanup` collapse into `ToolClient::init/cleanup`, wired in
`main.cpp` where `AI::ReangleClient::init()` is today.

### 3.3 Config helper is shared already

`ReangleFlow::resolve_config` (Debug field → env → default endpoint) is tool-
agnostic. Move it to `ToolClient` (or a small `AI::hvym_config`) so both flows call
one resolver. The base URL is the same for both tools — **the proxy routes by tool
name**, so the client never learns the RunPod endpoint id.

---

## 4. The mesh flow

`src/AI/MeshFlow.{hpp,cpp}` mirrors `ReangleFlow`, minus the texture concerns:

1. **Menu action** — "Add 3D Reference (sketch)" (next to "AI Reangle (3D)" in the
   Toolbar menu), gated on the warm state exactly like reangle (§7).
2. **Input** — a rough sketch. Reuse `SquareCanvasCaptureTool`:
   - **Send an OPAQUE image — `transparentBackground = false`, same as reangle.**
     This is the proven-safe choice: reangle showed that a transparent capture
     (bare strokes, alpha = coverage) breaks the service's isnet matte and yields
     garbage, while an opaque drawing-on-paper image works. The mesh endpoint is
     not implemented yet, so nothing about its input is verified — but it almost
     certainly runs the same matte, so default to what we know works. (This is the
     one input assumption *not* to guess at; if the endpoint later proves it
     tolerates transparency, relax it then, with a measurement.)
   - **`centerOut` optional.** Unlike reangle we don't have to register a textured
     result back onto the source, so exact placement matters less — but center-out
     is still nicer framing. Keep the captured `CaptureRegion` anyway; §5 uses it.
   - Alternatively, "from selected image" for an already-imported sketch.
3. **POST** — `ToolClient::request(base, key, "mesh", {imageField, target_faces,
   seed})`, remembering the `CaptureRegion` with the in-flight request (as reangle
   does).
4. **Poll** — `MeshFlow::tick()` from `DrawingProgram::update`, same as reangle.
5. **Place** — on success, `load_from_memory(glb, size, err, /*zUpToYUp=*/true)`,
   then `place_model_component(...)` at the captured world region (§5), and
   `open_for` the orbit editor. **No bake step is required** — the reference is a
   gray model the artist draws over; it just lives on the canvas as an orbitable
   static model. (Baking a flat render to a raster layer is still available via the
   editor's Bake if the artist wants a snapshot, but it is not the point.)

The success toast: "3D reference ready — orbit it and draw over it on a new layer."

---

## 5. Placement, layers, and the reference workflow

- **Place at the captured world region** (reuse the reangle `place_model_component`
  path: `CoordSpaceHelper(regionTopLeft, sideWorld/DIM, rotation)`), so the
  reference sits where the artist sketched at a sensible size. Framing can stay the
  looser 1.3× margin (this is a reference, not a registered overlay).
- **Layer intent — DECIDED & BUILT:** the reference lands on its **own new layer
  directly ABOVE the artist's drawing** ("3D Reference"), created by
  `DrawingProgramLayerManagerGUI::create_layer_above_editing`. The workflow the artist
  described: rough sketch (bottom) → 3D reference above it → **draw the detailed
  version on a new layer on top of the reference**, then hide/remove the reference.
  Placing it above (not below) is deliberate — it's a guide to trace over, so it must
  be visible over the sketch. A mesh render is opaque only where the figure is (the rest
  is transparent), so it occludes exactly the area being traced and nothing else. The
  new layer becomes the edit target; the artist's next "new layer" then sits above it.
- **Re-editing** reloads from the embedded `.glb` bytes via the same modal
  constructor path; the `zUpToYUpHint` runtime flag we added for reangle must be
  set for mesh components too (or, preferably, the server ships Y-up and the hint
  goes away for both).

---

## 6. The library (Phase 2 — where the value is)

The service returns `X-Cache-Key: <sha256>` and content-addresses results, so **the
same sketch always yields the same mesh, instantly** (~0.05 s cache hit). That hash
is a natural asset id.

- **Persist a per-user library** of reference meshes keyed by `X-Cache-Key`:
  the `.glb` bytes live in the `ResourceManager` already (a reangle/mesh model is a
  `ResourceData` embedded in the canvas); a library adds a small index (hash →
  name/thumbnail/params) in the config dir, mirroring how brush/armature presets are
  stored (see PHASE9.5 / `SquareCanvasCaptureTool` thumbnail capture).
- **Thumbnails** for the browser: bake one flat render via `ArmatureBake` at a small
  size (the machinery already exists — it is how armature components make their
  raster).
- **An asset browser** to drop a library reference onto *any* canvas/scene — this is
  the reuse payoff ("a reference placed in twenty frames changes how a scene gets
  built").
- **Storage choice to make now:** because re-fetch is nearly free, the client can
  key the library on the hash and **re-fetch on demand** rather than storing every
  `.glb` locally — or store locally for offline. Decide in §9.

Phase-1 MVP ships without the library (orbitable reference immediately); the library
is the follow-on that makes it a tool rather than a novelty.

---

## 7. Warming is per-endpoint

TRELLIS runs on its **own image and RunPod endpoint** (16 GB+ VRAM), separate from
reangle. Consequences for our `WarmLease`:

- The proxy holds a **tool→endpoint map**; `/warm` must be able to warm a *specific*
  endpoint. Confirm with the service whether `/warm` takes a `tool` (or `endpoint`)
  argument, or whether there is a `/warm` per tool.
- The header **"AI: ready" toggle** then means *which* worker is warm. Options: warm
  only the tool the artist is about to use, warm both, or show per-tool state.
  Simplest first cut: the toggle warms whichever tool's menu the artist opened; a
  richer version shows two indicators. Decision in §9.
- Everything else in `WarmLease` (20 s renew, 60 s TTL, refcount, auto-release,
  `State::FAILED` not `ERROR` — the wingdi macro trap) is unchanged.

---

## 8. Blockers / dependencies

1. ~~Endpoint not implemented server-side~~ — **DONE. LIVE** (`km99b7mrj2f85r`),
   verified end to end through the proxy (mesh.md). `X-Tool-Version` identifies the
   pipeline that built a given mesh.
2. ~~Proxy tool→endpoint routing~~ — **DONE.** The proxy routes `/tools/mesh` to the
   TRELLIS endpoint; the client reaches it with no code aware of RunPod ids.
3. **Warm lease does not actually warm mesh** (mesh.md §6b) — a held lease reports
   `warm`, but the keepalive short-circuits before any pipeline work, so CUDA/spconv
   init is still paid on the first real request: **~57 s even when "warm"**, then 4 s
   after. A server-side fix (run one tiny inference in `init()`) is pending an image
   rebuild. Until then the client quotes ~a-minute for the first request after idle
   (MeshFlow does). Ties into Phase 3 (§7): the `/warm` contract may still need a tool
   argument once separate warming is wired.
4. **No CORS** — WASM cannot call it, same as reangle; native only.
5. **Bad-sketch failure mode** — unlike reangle there is no silhouette-IoU sanity
   check; a scribble may return a confident wrong mesh. The client should surface
   `X-Tool-Version` + let the artist reroll (`seed`) or discard cheaply. (`seed`/
   `target_faces` are sent with defaults today; no reroll UI yet.)

---

## 9. Phased build plan

1. ✅ **Refactor to `ToolClient`** (§3) — DONE (`f377287`). Shared curl core in
   `src/AI/ToolClient.{hpp,cpp}`; `ReangleClient` is a thin wrapper (its `Request` is
   now an alias of `ToolClient::Request`); reangle behavior unchanged. `resolve_config`
   was left on `ReangleFlow` (already the shared HVYM resolver used by the warm toggle)
   rather than moved — a future cleanup, not a blocker.
2. ✅ **`MeshFlow` + menu + placement** (§4–§5) — DONE (`6b2b418`). `src/AI/MeshFlow.*`,
   the "AI 3D Reference (sketch)" menu item (warm-gated), and
   `ArmatureModalScreen::load_reference_mesh_into_canvas` (untextured, perspective,
   1.3× frame). Capture helpers extracted to `ServiceCapture.hpp`. **Placed on its own
   new "3D Reference" layer directly above the drawing** (`1236736`, §5). Not yet
   runtime-tested against the live endpoint.
3. **Per-endpoint warming** (§7): teach `WarmLease` which tool/endpoint to warm — and
   fix that a "warm" lease still costs ~57 s on the first mesh job (§8.3, server-side).
4. **The library** (§6): persist by `X-Cache-Key`, thumbnails, asset browser, reuse
   across documents.
5. **Multi-view** (future): TRELLIS `run_multi_image` — reangle → draw the next
   angle → feed both back for a better mesh. Untested server-side; the most
   interesting item, parked until the endpoint and the multi-image path exist.

## 10. Open decisions

- ~~Refactor scope~~ — **DECIDED:** `ToolClient` shared core + thin `ReangleClient`
  wrapper (the smallest-diff option). Done in `f377287`.
- ~~Library now or after MVP~~ — **DECIDED:** MVP first (orbitable references), library
  is the Phase-4 follow-on.
- **Library storage:** re-fetch on demand by hash (cheap, needs network) vs store the
  `.glb` locally (offline, larger canvases)? *(open — Phase 4)*
- **Warm scope:** one toggle warming the tool-in-use, or per-tool indicators? *(open —
  Phase 3; today one warm lease gates both tools)*
- ~~Reference layer~~ — **DECIDED & BUILT (`1236736`):** its own new "3D Reference"
  layer directly ABOVE the artist's drawing (the artist draws the detailed pass on top
  of it). `create_layer_above_editing` handles it; reangle keeps active-layer placement.
- **Input:** capture-a-sketch only, or also "make reference from selected image"?
  *(open — MVP is capture-only)*
