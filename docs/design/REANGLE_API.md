# REANGLE_API.md — calling the reangle service from Inkternity

**What this is.** The client-side contract for the hosted reangle service: send one
character drawing, get one textured `.glb` back. It is the network half of
[REANGLE_PIPELINE.md](REANGLE_PIPELINE.md) §7 — the *"one fetch, then everything
local"* boundary. Everything after the `.glb` lands (armature viewer, orbit, bake to
canvas) is covered there, not here.

**Status (2026-08-27):** service is **live and verified end to end** at
`https://img.hvym.link`, and the Inkternity client is **implemented** against this
contract — `src/AI/ReangleClient.{hpp,cpp}` (the async curl client) and
`src/AI/ReangleFlow.{hpp,cpp}` (capture → POST → place). Builds clean in the OpenGL3.3
config; not yet exercised against the live endpoint at runtime (needs `HVYM_TOOLS_KEY`
in the environment — §7).

**New since that client was written: [§11 warm leases](#11-warm-leases).** The service
can now hold a GPU worker awake so the artist pays the cold start once per session
instead of on every idle gap. That is the remaining client-side work.

**Service repo:** `../hvym-img-tools` (`docs/CLIENT.md` is the language-neutral
contract; this doc is the C++/Inkternity-specific version).

---

## 1. The whole API

One endpoint. One call per drawing.

```
POST https://img.hvym.link/tools/reangle
X-API-Key: <scoped key>
Content-Type: multipart/form-data
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `image` | file | *required* | The character drawing. Any size, any background — the service mattes it. **Max 8 MB.** |
| `mc_resolution` | int 64–**384** | `256` | Marching-cubes grid. Main quality/cost lever; see §8. |
| `backbone` | str | `triposr` | Reconstruction backbone. Leave alone. |
| `texture_size` | int 256–4096 | `2048` | Baked texture resolution. Leave at the default; see §8. |

**Success — `200`, body is the raw `.glb`:**

| Header | Meaning |
|---|---|
| `Content-Type` | `model/gltf-binary` |
| `Content-Disposition` | `attachment; filename="char.glb"` |
| `X-Cache` | `HIT` or `MISS` |
| `X-Upstream-Elapsed` | seconds of real GPU work |
| `X-Tool-Version` | pipeline version that produced the mesh |

The body is **binary**. Errors are JSON (`{"detail": "..."}`). **Branch on the status
code, not on content type** — a `.glb` and an error body are told apart by the code.

The returned `.glb` carries the artist's original art as a front-projected UV atlas
(REANGLE_PIPELINE.md §7.4), so it feeds straight into
`ArmatureModel::load_from_memory(data, size)`.

---

## 2. Timeouts — read this before writing the request

**Set the transfer timeout to at least 300 seconds.** This is the single most likely
way the integration fails, and it fails in a way that looks like a network bug.

| Situation | Time to first byte |
|---|---|
| Cache hit | ~0.05 s of work |
| Warm worker, new drawing | ~2.7 s of work |
| Cold worker, host has the image | ~48 s |
| **Cold worker, fresh host** | **up to ~260 s** |

The GPU is serverless and scales to zero. A cold worker must pull a 6.5 GB container
image before it loads a model. Measured against the live endpoint, a request after idle
took **50 s wall for 2.7 s of actual work**.

Two libcurl specifics:

- **`CURLOPT_TIMEOUT` must be ≥ 300**, not `CURLOPT_CONNECTTIMEOUT`. Connect succeeds
  immediately; it is the *response* that takes minutes.
- **Do not set `CURLOPT_LOW_SPEED_LIMIT` / `CURLOPT_LOW_SPEED_TIME`** on this request.
  During a cold start literally nothing transfers for minutes, which is exactly what
  those options exist to abort. If the shared helper sets them, clear them here.

```cpp
curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);          // whole transfer
curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);    // TCP+TLS only
curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 0L);    // disable stall abort
curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 0L);
```

**UI consequence.** A progress bar cannot be honestly filled — the server sends nothing
until it sends everything. Show an indeterminate spinner with text like *"Building 3D
proxy… the first request after idle can take a few minutes."* A bar that reaches 100%
and then sits there reads as a hang.

---

## 3. Status codes

| Code | Meaning | Client behaviour |
|---|---|---|
| `200` | success, binary `.glb` | load it |
| `401` | missing/invalid key | **configuration error** — surface it, never retry |
| `413` | image over 8 MB | downscale and resubmit (see §8) |
| `422` | malformed form fields | a bug in the request — do not retry |
| `500` | the pipeline itself failed | show `detail`; retry rarely helps |
| `502` | worker/upstream problem | retry once after a few seconds |
| `503` | proxy misconfigured server-side | surface; not the user's problem |
| `504` | exceeded the server's own budget | the job may still be running; retry (§4) |

Never show a raw `detail` string as the primary message — map to something an artist
understands, and log the detail.

---

## 4. Retries are safe, and usually free

Results are cached by `sha256(image + params)` on shared storage, so **the same drawing
with the same parameters always returns the same bytes**, and a repeat is served in
~0.05 s instead of ~2.7 s.

That makes retry-after-failure genuinely safe: a retry following a timeout normally
collects an already-finished result rather than redoing the work. This was verified in
production — a request returned `X-Cache: HIT` for a mesh built hours earlier by a
worker that no longer existed.

Recommended policy:

- `502`/`504`: retry up to twice, ~5 s apart
- `401`/`413`/`422`/`503`: never retry
- `500`: offer the artist a manual retry; do not loop

**Do not build a client-side disk cache keyed on the image.** The server already
content-addresses results, and a second cache is a second thing to invalidate when
`X-Tool-Version` changes. Cache the *loaded mesh* in memory for the session if you like.

---

## 5. Threading — follow the FileDownloader pattern

A request can block for minutes. It must never touch the UI thread.

`include/Helpers/FileDownloader.{hpp,cpp}` already establishes the house pattern:
`curl_multi` driven on a background thread, results handed back through a
`shared_ptr<DownloadData>` carrying `std::atomic<Status>` and `std::atomic<float>`. A
reangle client should mirror it:

```cpp
struct ReangleRequest {
    enum class Status { IN_PROGRESS, SUCCESS, FAILURE };
    std::vector<uint8_t>  glb;          // the .glb bytes on success
    std::string           error;        // human-readable on failure
    long                  httpCode = 0;
    bool                  cacheHit = false;
    std::atomic<Status>   status{Status::IN_PROGRESS};
};

std::shared_ptr<ReangleRequest> request_reangle(const std::vector<uint8_t>& png,
                                                int mcResolution = 256);
```

Poll `status` from the UI each frame, exactly as the armature/import paths already do.

**One caution about reuse:** `FileDownloader` exposes `disable_ssl_verification()`. Do
**not** use it here. This request carries the API key in a header; without certificate
verification that key is exposed to anyone able to intercept the connection. If a shared
code path calls it, branch so reangle keeps verification on.

---

## 6. Building the request (libcurl)

Use `curl_mime` — `curl_formadd` is deprecated.

```cpp
CURL* curl = curl_easy_init();
curl_easy_setopt(curl, CURLOPT_URL, "https://img.hvym.link/tools/reangle");

// --- auth -------------------------------------------------------------
curl_slist* headers = nullptr;
headers = curl_slist_append(headers, ("X-API-Key: " + apiKey).c_str());
curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

// --- multipart body ---------------------------------------------------
curl_mime* mime = curl_mime_init(curl);
curl_mimepart* part = curl_mime_addpart(mime);
curl_mime_name(part, "image");
curl_mime_filename(part, "drawing.png");
curl_mime_type(part, "image/png");
curl_mime_data(part, reinterpret_cast<const char*>(png.data()), png.size());

part = curl_mime_addpart(mime);
curl_mime_name(part, "mc_resolution");
curl_mime_data(part, std::to_string(mcResolution).c_str(), CURL_ZERO_TERMINATED);

curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

// --- timeouts (see section 2) ----------------------------------------
curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 0L);
curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 0L);

// --- binary-safe collection ------------------------------------------
curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_vector);
curl_easy_setopt(curl, CURLOPT_WRITEDATA, &req->glb);
curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &collect_header);
curl_easy_setopt(curl, CURLOPT_HEADERDATA, req.get());
```

The write callback must be **binary safe** — a `.glb` contains embedded NULs, so
anything that treats the buffer as a C string will truncate it:

```cpp
static size_t write_to_vector(void* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::vector<uint8_t>*>(userp);
    const size_t n = size * nmemb;
    auto* bytes = static_cast<uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + n);
    return n;
}
```

**Validate before loading.** A `.glb` starts with the ASCII magic `glTF`. Check it
before handing the buffer to `ArmatureModel::load_from_memory` — on an unexpected error
path you would otherwise pass a JSON error body to the mesh loader:

```cpp
if (req->glb.size() < 12 || std::memcmp(req->glb.data(), "glTF", 4) != 0) {
    req->error = "server did not return a valid .glb";
    req->status = ReangleRequest::Status::FAILURE;
}
```

---

## 7. The API key

The key is a **scoped** credential: all it can do is ask for a mesh. It is *not* the
RunPod account key — that one never leaves the server, which is the entire reason the
service sits behind a proxy.

- **Never commit it.** Not in source, not in a header, not in a resource file.
- Read it from configuration/environment at runtime, the same way other deployment
  settings are handled.
- **Make the base URL configurable too.** The service can move from this proxy to a
  persistent pod without any contract change; a hardcoded host forces a client rebuild.
- Treat it as spend control, not identity. It is shared across a build, and anyone who
  extracts it can spend GPU time — which is why it is rotatable server-side. Rotation
  invalidates every client using the old key, so ship it as configuration.

The key for the demo build is in the service repo's `.env` as `HVYM_TOOLS_KEY`
(gitignored). Ask for it rather than copying it into this repo.

---

## 8. Choosing `mc_resolution`

The main cost/quality lever. Measured on the live endpoint with `alice_char2.png`:

| `mc_resolution` | Work | Output size |
|---|---|---|
| `192` | ~2.0 s | ~442 KB |
| `256` *(default)* | ~2.7 s | ~729 KB |
| `320` | ~2.7 s | ~1.1 MB |

Start at `256`. It is the value everything was validated against, and the difference is
mesh density — which matters more for the depth proxy's silhouette than for anything the
artist directly sees. If the client ever exposes this, treat it as a quality setting, not
a slider the artist tunes per drawing (each distinct value is a distinct cache key, so
sweeping it defeats the cache).

### Texture resolution

`texture_size` defaults to **2048** (it was 512 in earlier builds). At 512 the
artist's linework visibly softened once the mesh was magnified on canvas —
individual strands and fine detail turned to mush against the original art.

Leave it alone unless you have a reason. It is nearly free: linework compresses
well, so the texture adds ~350–650 KB to a `.glb` whose mesh already accounts for
most of its size.

Two things not to expect from it:

- **The silhouette edge does not sharpen past ~1024**, because the alpha mask
  comes from a matting model with a 1024² input. Raising this recovers *interior*
  linework, not a crisper outline.
- **It does not change alignment.** UVs are normalised, so the projection lands
  identically at any texture size.

**`mc_resolution=512` is rejected in practice.** The API advertises up to 512 but
the worker OOMs on a 24 GB GPU above 384, returning a `502`. Treat **384 as the
real ceiling** until the service tightens its own bound.

**Downscale before upload.** The service mattes and normalizes internally, so a huge
canvas export buys nothing and costs upload time on the artist's connection. The
validated input is ~556 KB; the hard limit is 8 MB (`413` above it).

---

## 9. Known limitations

**No CORS — the Emscripten build cannot call this directly.** Verified against the live
endpoint: an `OPTIONS` preflight returns `405` with no `Access-Control-Allow-Origin`, so
a browser will refuse the request before it is sent. Native builds are unaffected. If the
WASM target needs reangle, the service must add CORS middleware — a small change to the
proxy, but a server-side one. Do not spend time debugging `emscripten_fetch` against
this; it cannot work until the server allows the origin.

**One drawing per call.** No batching. The access pattern is one call per drawing, then
all interaction is local (REANGLE_PIPELINE.md §7.1).

**Cold start is a real product concern**, not just a timeout value — see §2. For a demo,
the service operator can pre-warm the GPU (`scripts/warm.py on` in the service repo),
which removes the wait for the session.

**Pose is still the constraint.** The service does not change what
REANGLE_PIPELINE.md §4.1 says about difficult poses; a hard input can still produce a
poor proxy. `X-Tool-Version` is how you tell which pipeline built a given mesh.

---

## 10. Verifying by hand

```sh
curl -X POST https://img.hvym.link/tools/reangle \
     -H "X-API-Key: $HVYM_TOOLS_KEY" \
     -F image=@scripts/image_in/alice_char2.png \
     -F mc_resolution=256 \
     --max-time 300 -o char.glb -D -
```

Expect `200`, `Content-Type: model/gltf-binary`, and a file beginning with `glTF`.

`GET https://img.hvym.link/healthz` is unauthenticated and safe to poll as a
reachability probe. It reports whether the service is configured, never any key.

For local development without touching the hosted GPU, the service repo runs the same
contract on CPU — `uv run hvym-img-serve` on `http://localhost:8000`. Slower, identical
wire format, no key required by default.

The service repo also ships `scripts/smoke_proxy.sh`, which exercises this whole contract
(auth rejection, body limits, a real inference, cache behaviour) against any deployment.
Run it before blaming the client.

---

## 11. Warm leases

Cold start is the sharpest edge here (§2). The service now lets a client hold a
**lease** that keeps a GPU worker awake, so a header-bar "enable inference"
toggle can pay that wait once per session rather than on every idle gap.

```
POST   /warm     X-API-Key: <key>   {"lease_id": "<opaque>", "label": "<optional>"}
GET    /warm                        (no key required)
DELETE /warm     X-API-Key: <key>   {"lease_id": "<opaque>"}
```

`POST` acquires on the first call and extends thereafter. Omit `lease_id` the
first time and the server issues one; send the same id back every time after.

```json
{"lease_id": "9f2c...", "state": "warming", "ready": false,
 "elapsed_s": 12.3, "expires_at": "2026-08-27T00:14:02+00:00",
 "lease_ttl_s": 60.0, "renew_within_s": 20.0, "active_leases": 1}
```

| Field | Use in the UI |
|---|---|
| `state` | `cold` / `warming` / `warm` — drive the indicator from this |
| `ready` | `true` only when a worker can serve *now* |
| `elapsed_s` | seconds spent warming — show it; do **not** compute an ETA |
| `renew_within_s` | **renew at least this often** or the lease lapses |

### Why it is a lease and not a switch

A switch is state the client *asserts*. If Inkternity crashes, the laptop
sleeps, or the artist walks away, it stays on and keeps billing — and warm GPU
time is roughly **$1.10/hour**, about 2,000 images' worth of compute per idle
hour. A lease is state the client must keep *re-asserting*; silence means
release. **Re-POST every `renew_within_s` (20 s) while the toggle is on.** The
lease lives 60 s, so two consecutive missed renewals are survivable and an
abandoned session stops costing money within about a minute, unattended.

That asymmetry is the whole design. Getting the renewal loop right matters more
than getting the toggle right.

### Client notes

- **The renewal poll is the notification channel.** Every response carries the
  current state, so no push, no WebSocket, no reconnect logic. Poll to hold the
  lease and render whatever comes back. If the poll itself fails, show cold.
- **Do not build your own keepalive.** The proxy pings the GPU internally at a
  cadence tuned to the endpoint's idle timeout. The client's only job is the
  lease.
- `DELETE /warm` on toggle-off and on clean shutdown. It is idempotent, so a
  retry after a dropped response is safe. Skipping it is not a disaster — the
  lease lapses — but it wastes up to a minute of GPU time.
- `GET /warm` needs **no key**: it spends nothing and cannot start a worker, so
  the indicator can render before the artist opts in.
- Leases are **refcounted**; two Inkternity instances behind one proxy share a
  single warm worker.
- Against a persistent-box deployment `/warm` is a truthful no-op returning
  `state: "warm", ready: true, no_op: true` — hold a lease unconditionally and
  the same code works against either deployment.
- **Warm time is billable** and is the metered unit in the paid product. Treat
  the toggle as something the artist turns on deliberately; never default it on.

### Suggested shape

```cpp
// Mirrors ReangleClient: async, polled from the UI, never blocking.
class WarmLease {
public:
    enum class State { COLD, WARMING, WARM, ERROR };
    static void  enable(const std::string& baseUrl, const std::string& apiKey);
    static void  disable();                  // DELETE, then stop renewing
    static State state();                    // for the header-bar indicator
    static float elapsed_s();                // show while WARMING
};
```

A renewal is a small JSON POST, so it is far cheaper than a reangle — but it
still must not run on the UI thread. Reuse the `ReangleClient` worker-thread
pattern rather than adding a second threading model.
