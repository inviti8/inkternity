# REANGLE_CLIENT_HANDOFF.md — brief for finishing the reangle client

**Read first:** [REANGLE_API.md](REANGLE_API.md) is the contract and is current as
of 2026-08-27. [REANGLE_PIPELINE.md](REANGLE_PIPELINE.md) §7 covers everything
after the `.glb` arrives. This file is the delta: what changed on the service
side, what already exists in-tree, and what is left to build.

---

## 1. Where things stand

**The service is live and fully verified.** `https://img.hvym.link` — TLS,
authentication, upload limits, real inference, cache, and no credential leakage
all confirmed against the deployed endpoint (11/11 smoke checks). One drawing in,
one textured `.glb` out.

**The client already exists** and follows the contract:

| File | Role |
|---|---|
| `src/AI/ReangleClient.{hpp,cpp}` | async curl client; background thread, polled `status` |
| `src/AI/ReangleFlow.{hpp,cpp}` | capture → POST → place as a static ARMATURE model |

It builds clean in the OpenGL3.3 config. It has **not** been exercised against
the live endpoint at runtime — that needs `HVYM_TOOLS_KEY` in the environment.

**What is new since that client was written:** the service grew **warm leases**
(`/warm`), and that is the main remaining piece. See REANGLE_API.md §11.

---

## 2. The work

### 2.1 Run the existing client against the live service (do this first)

Nothing below is worth building on an unproven request path. Set
`HVYM_TOOLS_KEY` (ask for it — it is not in this repo) and drive one real
drawing through `ReangleFlow` end to end.

Expect **up to ~4 minutes** on the first call if the worker is cold. That is
correct behaviour, not a hang — which is exactly why the timeout must be ≥300 s
(§2). Confirm:

- the `.glb` loads via `ArmatureModel::load_from_memory`
- the artist's linework is on the mesh (not a gray mannequin — §7.3 of the
  pipeline doc covers the textured shader path)
- a second identical request returns fast with `X-Cache: HIT`

### 2.2 Warm leases — the new work

Build a `WarmLease` alongside `ReangleClient`, and a header-bar toggle:

1. **Toggle on** → `POST /warm`, keep the returned `lease_id`.
2. **While on** → re-`POST` every `renew_within_s` (20 s) with that id, on a
   background thread. The response is also the status feed.
3. **Indicator** → drive from `state`: `cold` / `warming` (show `elapsed_s`) /
   `warm`. Never render a progress bar or a countdown — the server sends nothing
   until it sends everything, and a bar that sits at 100% reads as a hang.
4. **Toggle off / app shutdown** → `DELETE /warm` with the id.

**Get the renewal loop right before the toggle.** Warm GPU time costs about
$1.10/hour; the lease exists so that a crash, a sleeping laptop, or a closed lid
stops the bill unattended. A toggle that holds state without renewing is the
failure mode this design exists to prevent.

`GET /warm` needs no API key, so the indicator can render before the artist opts
in — useful for showing "cold, first request will take a few minutes".

### 2.3 Worth doing alongside

- **Auto-warm when the reangle panel opens**, not only on the manual toggle. By
  the time the artist has framed a character and hit go, 30–60 s has passed —
  often most of a warm-host cold start, hidden for free. Same lease machinery,
  fewer forgotten toggles.
- **Surface `X-Tool-Version`** somewhere in the mesh's metadata. It identifies
  which pipeline built a given `.glb`, which is the only way to tell stale cached
  results apart after a service update.

---

## 3. Things that will waste your time if you don't know them

**Timeouts.** `CURLOPT_TIMEOUT ≥ 300`, and do **not** set
`CURLOPT_LOW_SPEED_LIMIT` — during a cold start nothing transfers for minutes,
which is precisely what that option aborts on. Already handled in
`ReangleClient`; do the same in `WarmLease`.

**Wall-clock is not the service's latency.** Measured on the live endpoint, the
same cache hit (0.028 s of actual work) took 4.5 s early in the day and 272 s
later — purely because the measuring machine's uplink degraded. Each request carries
~745 KB up and ~665 KB down. **Judge warmth and speed by `X-Upstream-Elapsed`,
never by a stopwatch**, or you will chase phantom service bugs.

**No CORS — the Emscripten build cannot call this.** Verified: an `OPTIONS`
preflight returns 405 with no `Access-Control-Allow-Origin`, so a browser refuses
before the request leaves. `ReangleClient` already fails fast on that target. Do
not debug `emscripten_fetch` against it; the fix is server-side and has not been
done.

**Errors are JSON, success is binary.** Branch on the status code, not on
content type. Validate the `glTF` magic bytes before handing the buffer to the
mesh loader, or an error body reaches `ArmatureModel::load_from_memory`.

**Do not reuse `FileDownloader::disable_ssl_verification()`.** These requests
carry the API key in a header.

---

## 4. Open questions for whoever picks this up

- **Should the warm toggle be per-artist or per-app?** Leases are refcounted
  server-side, so two instances already share one worker. The UI question is
  whether the toggle is global or per-document.
- **What does the indicator show when a request is in flight on a cold worker?**
  "Warming" and "working" look the same to the artist but are different states;
  worth deciding deliberately rather than by accident.
- **Where does `HVYM_TOOLS_KEY` come from in a shipped build?** Currently an
  environment variable. It is spend control, not identity — rotatable
  server-side, and rotation invalidates every client using the old key, so it
  must be configuration rather than a compiled-in constant.
