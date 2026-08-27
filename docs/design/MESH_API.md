# MESH_API.md — calling `/tools/mesh` from Inkternity

**What this is.** The client-side wire contract and *measured* operational behaviour
of the hosted mesh service: one rough sketch in, one **untextured** `.glb` out. It is
to [MESH_REFERENCE.md](MESH_REFERENCE.md) what [REANGLE_API.md](REANGLE_API.md) is to
REANGLE_PIPELINE.md — MESH_REFERENCE covers *what the client builds*, this covers
*what the network actually does*.

**Status (2026-08-27):** the endpoint is **LIVE and verified end to end** at
`https://img.hvym.link/tools/mesh`, on its own RunPod endpoint, worker image
`hvym-img-mesh:0.3.1`, tool version `0.1.0`. Every number in this document is
measured against that deployment, not estimated.

**Two corrections to [MESH_REFERENCE.md](MESH_REFERENCE.md)**, which was written
before the endpoint shipped:

1. Its status line says *"not yet implemented server-side"*. It is implemented.
2. Its §2 says timeouts are *"≥300 s, identical to REANGLE_API.md"*. **They are not.**
   A cold mesh request measured **547 s**. 300 s is below the floor, and a client that
   uses it will fail every cold start. See §3 — this is the single most important
   thing on this page.

Its wire table (§2) is otherwise accurate and matches the server's input model exactly.

**Service repo:** `../hvym-img-tools` — `docs/tools/mesh.md` (design),
`docs/WARMING.md` (lease + measurements), `docs/CLIENT.md` (language-neutral).

---

## 1. The whole API

```
POST https://img.hvym.link/tools/mesh
X-API-Key: <scoped key>
Content-Type: multipart/form-data
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `image` | file | *required* | Rough sketch (PNG/JPEG), any size or background — the service mattes it. **Max 8 MB.** |
| `target_faces` | int 2 000–200 000 | `20000` | Decimation target, an absolute count so response size is predictable. |
| `seed` | int 0–2³¹-1 | `0` | Fixed by default so a sketch is reproducible **and cacheable**. Change it to reroll. |

Send the same opaque capture you send to reangle. There is no `mc_resolution` and no
`backbone` here — those are reangle's levers.

### Response headers, as actually returned

Measured on the live endpoint. **They differ between a cache miss and a cache hit**,
which is a trap worth knowing before you write the parser:

| Header | On `MISS` | On `HIT` |
|---|---|---|
| `Content-Type` | `model/gltf-binary` | `model/gltf-binary` |
| `X-Cache` | `MISS` | `HIT` |
| `X-Cache-Key` | sha256 — **your library asset id** | same |
| `X-Upstream-Elapsed` | `8.699` | `0.025` |
| `X-Proxy-Elapsed` | `541.362` | `124.775` |
| `X-Tool-Version` | `0.1.0` | **absent** ¹ |
| `Content-Disposition` | `attachment; filename="reference.glb"` | **absent** |

¹ Fixed server-side; it ships with the next worker image. Until then **treat both
`X-Tool-Version` and `Content-Disposition` as optional.** Do not branch on their
presence, and never derive the filename from the response — you have the bytes and
`X-Cache-Key`, which is a better asset id than any filename.

**`X-Upstream-Elapsed` is GPU work only.** It excludes queue and boot time. A `HIT`
reporting `0.025` after you waited two minutes is not a contradiction — see §3.

---

## 2. What comes back

Verified by loading a real response:

| | |
|---|---|
| Faces | **20 000** — exactly `target_faces`, always |
| Vertices | ~9 982 |
| Size | **352 KB** |
| Connected components | 1 (99.99 % of faces in the largest) |
| UVs | **none** — untextured by design |
| Orientation | **Z-up.** A chair measured `0.514 × 0.488 × 1.001`; the long axis is Z |

The Z-up finding confirms MESH_REFERENCE.md §1: reuse the existing `zUpToYUp`
correction, same as TripoSR. It loads through
`ArmatureModel::load_from_memory(data, size, err, zUpToYUp)` unchanged and renders on
the flat/lit shader — the gray-mannequin look a draw-over reference wants.

---

## 3. Timeouts — the thing to get right

**Set the client timeout to 900 s. Not 300.**

| Path | Wall clock | Of which GPU work |
|---|---|---|
| Cold (no worker running) | **547 s** | 8.7 s |
| Warm, cache MISS | ~5.4 s | 5.4 s |
| Warm, cache HIT | 4.4–4.9 s | 0.025 s |

**98 % of a cold request is waiting for hardware**, not computing. The service asks
RunPod for a GPU, RunPod has none free, and the request queues. That is why tuning
inference is pointless next to holding a worker (§4).

This is not theoretical. At the old 300 s nginx ceiling, a real cold request came back
as a **504 HTML error page after five minutes** — while the job completed successfully
on RunPod and its result was cached. The artist got an error for work that had
finished. nginx now allows 900 s; your client must too, or you have simply moved the
same bug into Inkternity.

**Corollary: a slow response is not a failed one.** Do not retry on a long wait — you
will queue a second job behind the first. Retries are safe (§5) but not free of
latency.

### The service gives up before you do — on purpose

Two ceilings sit in front of you, and the order between them is deliberate:

| Layer | Budget | What you get when it expires |
|---|---|---|
| Proxy job budget | **840 s** | `504` + JSON `{"detail": "job IN_QUEUE after ..."}` |
| nginx `proxy_read_timeout` | 900 s | `504` + an **HTML** error page |

The proxy is set to lose the race so a stuck job produces a message you can show the
artist rather than nginx's HTML. **Parse the JSON detail on a `504`** — if you get
HTML instead, something is wrong with the deployment, not the job.

This ordering was not always right. The proxy budget was 600 s against a measured
547 s cold start — 53 s of margin — and it silently outranked the 900 s nginx ceiling,
so raising nginx alone achieved less than it appeared to. Confirmed the hard way: a
deliberately constrained test returned `{"detail": "job IN_QUEUE after 600s"}` at
605 s. Both numbers ship in `hvym-img-tools`; the 840 s value reaches the live proxy
with its next image update.

---

## 4. Warm leases — per tool, and mesh needs them more than reangle does

Mesh runs on its **own RunPod endpoint**, so warming reangle does nothing for mesh
and vice versa. The tool is named explicitly:

```
POST   /warm    {"lease_id": "...", "tool": "mesh", "label": "inkternity"}   auth
GET    /warm?tool=mesh                                                       no auth
DELETE /warm    {"lease_id": "...", "tool": "mesh"}                          auth
```

`GET /warm?tool=mesh` is deliberately unauthenticated: it spends nothing, starts
nothing, and reveals no key or endpoint, so the UI can show warmth before the artist
holds a lease. Live shape:

```json
{"state":"cold","ready":false,"workers_ready":0,
 "workers":{"idle":0,"initializing":0,"ready":0,"running":0,"throttled":1,"unhealthy":0},
 "active_leases":0,"elapsed_s":0.0,"expires_at":null,
 "lease_ttl_s":60.0,"renew_within_s":20.0}
```

Lease TTL is 60 s; renew within 20 s of expiry. Everything else — why it is a lease
and not a switch, the demo-vs-product distinction — is REANGLE_API.md §11 and applies
unchanged. **Just remember to pass `"tool": "mesh"`.** Omitting it silently warms
reangle: the field defaults to `"reangle"` server-side.

**Judge warmth by `workers_ready`, never by your own stopwatch**, and do not trust a
single poll of the underlying RunPod health — see §6.

---

## 5. Caching, and why `seed` matters to the library

The cache key is `sha256(image + params + tool version)`, returned as `X-Cache-Key`.
It is a **content address**, which makes it the natural asset id for the client-side
library in MESH_REFERENCE.md §6: the same sketch at the same settings is always the
same id, so the library dedupes for free and a re-request is a cache hit rather than
a GPU bill.

This only holds because **`seed` defaults to 0**. If you let the client randomise the
seed per request, every call is a miss, the cache never helps, and each one costs a
full cold-or-warm inference. Randomise only when the artist explicitly asks to reroll,
and store the seed with the library entry so the asset stays reproducible.

Retries are safe: the same request returns the same asset from cache.

---

## 6. Operational findings the client should not have to rediscover

**The endpoint is throttled more often than it is busy.** It is pinned to one
datacenter (EU-RO-1) because the network volume holding the result cache lives there,
and within it the worker image can only run on GPUs it was compiled for. That left two
GPU pools, and RunPod frequently has neither free. Mitigations applied on 2026-08-27:
an `AMPERE_80` (A100) fallback on the mesh endpoint, and a rebuild of the reangle
image widening its compiled architectures to cover A100, H100 and Blackwell. **Neither
removes throttling — they widen the pool.** The warm lease remains the actual fix,
because a held worker cannot be throttled out from under you.

**Do not build a progress bar on RunPod's worker counts.** The service's own health
endpoint reported `inQueue: 1, completed: 15` well after a job had returned, and it
has previously reported a warm worker as cold. The trustworthy signals are the HTTP
response itself and `X-Upstream-Elapsed`. This is why `GET /warm` exists — use it
rather than inferring state.

**Don't fake the ETA.** Cold start is ~547 s when hardware is scarce and far less when
it is not, and neither the client nor the service can tell which it got. Show elapsed
time with "up to ~10 min" and flip to ready on a real signal. A counted-down fake ETA
that expires while the job is still running reads as broken.

---

## 7. Status codes and errors

Identical to [REANGLE_API.md §3](REANGLE_API.md) — **do not re-derive them.** The
essentials:

- Body is **binary on 200, JSON (`{"detail": "..."}`) on error**. Branch on the status
  code, never on content type.
- `401` — bad or missing `X-API-Key`.
- `413` — over the 8 MB upload cap.
- `422` — a parameter outside its range (e.g. `target_faces` under 2 000).
- `502` — the service reached RunPod but got something unusable.
- `504` — nginx gave up. With the 900 s ceiling this should now mean a genuinely stuck
  job, not a slow one.

---

## 8. Verifying by hand

```bash
curl -X POST https://img.hvym.link/tools/mesh \
     -H "X-API-Key: $HVYM_TOOLS_KEY" \
     -F image=@sketch.png \
     -D headers.txt -o reference.glb --max-time 900
grep -i '^x-' headers.txt
```

Expect `glTF` as the first four bytes, ~352 KB, and `x-cache-key` present. Run it twice:
the second should return in seconds with `x-cache: HIT`. If the first takes minutes,
that is §3 working as designed, not a failure.
