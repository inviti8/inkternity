# AI_BILLING_INTEGRATION.md — warm-time billing, wallet-native, crypto-rails-gated

**Status:** planning → model decided; cost basis grounded in live RunPod data.
**Owners:** Inkternity client + hvym-img-tools proxy + HEAVYMETA portal.
**Depends on:** the crypto-rails opt-in (`verifiablePublishingEnabled` pattern), the
`WalletPanel`/`DevKeys` Stellar wallet, the `WarmLease` lifecycle, the proxy's `/warm` lease,
and [[project_glasswing_keypair_pattern]] (portal identity).

Related: `docs/design/MESH_REFERENCE.md`, hvym-img-tools `docs/WARMING.md` (the billing
brief this builds on), `docs/AUTH.md`, `docs/DEPLOY.md`.

---

## 1. What we're building and why

The AI tools (reangle + mesh) are live and free. To ship them as a product, the artist pays
HEAVYMETA for the GPU they spin up. The service side already decided the shape (`WARMING.md`):
**the artist pays for warm time, not per image** — active/warm GPU is the entire cost, and a
generation is ~5–11 s of GPU on a worker that costs the same warm or inferring.

**The core decision (this pass): AI is a crypto-rails feature, paid wallet-native.** It sits
behind the same opt-in that already gates verifiable publishing, and it is paid **peer-to-peer
from the user's own self-custodied Stellar wallet** — the same wallet C2PA already uses. No
fiat, no merchant-of-record, no server-held balance, no on-ramp. This is the elegant path: we
already have a wallet and signing; we use them, rather than building a fiat system beside them.

That choice dissolves the two problems we kept hitting:
- **Money-transmitter risk** — avoided by construction (see §2.4): opt-in, self-custody, and
  HEAVYMETA only ever *receives* crypto as payment for its own service.
- **Crypto-averse friction** — scoped out: AI is for users who turn crypto rails on, exactly
  the bargain verifiable publishing already makes. We are not trying to serve AI to users who
  won't touch a wallet.

---

## 2. Decisions

1. **Time-based, not token-based.** No tokens (image → mesh); cost is GPU wall-clock. The
   billable unit is **warm-seconds**.
2. **Generation is free while warm.** Marginal per-generation cost is ~$0.0025; fold it into
   the warm rate so the only meter the artist sees is "is the GPU on."
3. **Crypto-rails-gated, wallet-native.** AI billing lives behind the crypto-rails opt-in
   (mirror `verifiablePublishingEnabled`, `FileSelectScreen.cpp:574-618`) and reuses the
   **same** app wallet (`DevKeys` keypair, `WalletPanel`) as C2PA. Value is the user's
   **self-custodied XLM/USDC**. One wallet, one top-up, powers both provenance registration
   and AI warm-time. **No fiat, no MoR, no server ledger, no on-ramp.**
4. **Money-transmitter boundary — drawn explicitly.** HEAVYMETA never converts fiat and never
   custodies user funds. The artist brings their own crypto to their own wallet and pays P2P
   for a service rendered — HEAVYMETA is a *merchant receiving crypto*, not a transmitter.
   **We never run the on-ramp**; acquiring crypto is the user's / a third party's problem,
   out of scope. Opt-in + self-custody + merchant-receives-crypto is what keeps this clean.
   *(Not legal advice; confirm with counsel — but this is the well-trodden line.)*
5. **Pay-per-window via x402, from the wallet.** The proxy exposes an **x402 endpoint**: a
   `402` challenge is answered by a **signed Stellar payment from the app wallet** to
   HEAVYMETA, which extends the warm **window** (coarse, ~15 min — *not* per renewal, which
   would spam the chain). Renewals (~40 s) stay **free + identity-signed** and just keep the
   lease alive. When the paid-through window nears expiry the client pays the next one; if it
   doesn't, the lease lapses and warm stops. Obolus/MCP is **not** in this path — Obolus
   gaining Stellar is a separate agent-ecosystem play against the same endpoint.
6. **Settle on grant, not on request.** Charge from `state: "warm"` (worker actually ready),
   never from the request — a throttled/cold worker the artist waited on but never got must
   not bill.
7. **Server authoritative on time.** The proxy owns the warm window and `held_s()`; never
   trust the client clock. Minimal state: a **paid-through timestamp per identity**, not a
   full prepaid ledger.
8. **Product tradeoff, owned:** AI tools are **crypto-rails-only**. Crypto-averse users don't
   get them — the same call verifiable publishing already made, plausibly the same audience.
9. **Prerequisite: portal-issued identity.** Auth must not ride on raw `DevKeys` — it's a
   plaintext, reinstall-invalidated dev stand-in. Finishing portal identity issuance
   ([[project_glasswing_keypair_pattern]]) is the **gate** before real money (see §6).

---

## 3. Cost basis — from our live RunPod data

All figures from hvym-img-tools (`scripts/warm.py`, `docs/WARMING.md`, `docs/BENCHMARK.md`,
`docs/DEPLOY.md`), endpoint `km99b7mrj2f85r`, image `hvym-img-mesh` (mesh + reangle),
24 GB flex tier (RTX 4090-class), EU-RO-1.

| Quantity | Value | Source |
|---|---|---|
| **Warm-hold rate** | **`USD_PER_SECOND = 0.00031` → $1.116/hr → $0.0186/min** | `warm.py:37` |
| Marginal per generation | ~8 s GPU × $0.00031 = **~$0.0025** | mesh warm 4.4–4.9 s wall / 8.7 s cold upstream; reangle ~4.9 s GPU |
| Cold-start **compute** cost | **~$0** (RunPod bills execution, not the container pull; keepalive short-circuits) | `WARMING.md` "98% of a cold request is waiting for hardware" |
| Cold-start **latency** | 26 s (flip on a host with image) → up to ~260 s (fresh pull) | `WARMING.md` §measured |
| Stellar tx fee (per window payment) | 100 stroops ≈ $0.0000012 | Stellar base fee |
| Both tools warm | **one worker** (shared image) → still $1.12/hr | `DEPLOY.md:117` |

**Takeaway: warm-hold time is essentially the only cost.** Per-generation and cold-start
compute are rounding error, and warming both tools is a single worker. So:

> **Unit cost ≈ warm-minutes × $0.0186.**

### Caveats to confirm before launch (already flagged in `WARMING.md` §open)
- $1.12/hr is the **published** 24 GB flex rate, not yet reconciled against
  `/billing/endpoints` after a real day of usage.
- Whether idle time inside `idleTimeout` (≤10 s) is billed — matters for scattered on-demand
  calls, **not** for held-warm sessions (our model), so it doesn't move these numbers.

---

## 4. Pricing & margin — what the numbers look like

Cost floor is **$1.116/hr**. Margin covers: fixed proxy hosting (~$20–40/mo — trivial), the
~26–260 s cold-start latency we eat without billing, occasional throttled/failed warms, and
profit. GPU resellers typically run 1.5–4×. **Quote and settle in USDC** so the price is
USD-stable without a peg mechanism; XLM would need spot-pricing at pay time.

### 4.1 Rate vs. margin

| Multiple of GPU | $/hr charged | $/min | Gross profit / warm-hr | Gross margin |
|---|---|---|---|---|
| 1.5× | $1.67 | $0.028 | $0.56 | 33% |
| 2.0× | $2.23 | $0.037 | $1.12 | 50% |
| 2.5× | $2.79 | $0.047 | $1.67 | 60% |
| **2.7× (anchor)** | **$3.00** | **$0.05** | **$1.88** | **63%** |
| 3.2× | $3.60 | $0.06 | $2.48 | 69% |
| 4.3× | $4.80 | $0.08 | $3.68 | 77% |

**Recommended anchor: $0.05/min ($3.00/hr)** → a ~15-min warm window = **$0.75 in USDC**.
Clean, 2.7× GPU, 63% gross margin, easy to reason about ("a few cents a minute; generate all
you want").

### 4.2 Session economics @ $3.00/hr

Typical: artist warms for a drawing session, generates freely, turns it off (or idle
auto-off). Generations in the window are free to them and ~free to us.

| Session | Our GPU cost | Artist pays | Our gross profit |
|---|---|---|---|
| 15 min | $0.28 | $0.75 | $0.47 |
| 30 min | $0.56 | $1.50 | $0.94 |
| 45 min | $0.84 | $2.25 | $1.41 |
| 60 min | $1.12 | $3.00 | $1.88 |
| 90 min | $1.67 | $4.50 | $2.83 |

### 4.3 How far a wallet top-up goes (@ $0.05/min)

There are no "packs" — the artist funds their own wallet with USDC and spends it per window.
This is just how much warm time a given balance buys.

| Wallet balance | Warm time | Our GPU cost | Our gross profit | Margin |
|---|---|---|---|---|
| $5 | 100 min (1h40m) | $1.86 | $3.14 | 63% |
| $10 | 200 min (3h20m) | $3.72 | $6.28 | 63% |
| $25 | 500 min (8h20m) | $9.30 | $15.70 | 63% |

### 4.4 Monthly revenue shape @ $3.00/hr (gross $1.88/warm-hr)

COGS scales with **concurrent warm-hours**, not user count (each concurrent warm user ≈ one
worker-hour). Fixed infra (~$20–40/mo) is negligible against any of these.

| Active artists | Warm hrs each / mo | Warm-hrs | Revenue | COGS | Gross |
|---|---|---|---|---|---|
| 100 | 5 | 500 | $1,500 | $558 | $942 |
| 500 | 5 | 2,500 | $7,500 | $2,790 | $4,710 |
| 1,000 | 8 | 8,000 | $24,000 | $8,928 | $15,072 |

### 4.5 The forgotten-warm tail (bounded by design)

The lease exists precisely so a crashed/abandoned client can't bill forever. Max overbill
after the last renewal = **one TTL ≈ 60 s ≈ $0.05**, and it's the artist who'd pay it, not
us. To protect goodwill, add **idle auto-shutoff** (no generation in N min → release) so
nobody burns balance staring at the canvas. Small revenue give-back, large trust win.

---

## 5. Architecture — where it plugs in

```
Inkternity (C++, crypto rails ON)        Proxy (hvym-img-tools)        RunPod
  WarmLease::worker() ──renew (signed, free)──► /warm  ── keepalive ──► worker (warm)
  x402 client ────────pay next window (signed XLM/USDC payment)───────►
                        POST /warm/pay (402 → verify payment) ──► extend paid-through window
  ToolClient::request ──generate (free while warm)──────► /tools/{name}
  WalletPanel ─────────top-up = user sends their own USDC to their own wallet (self-custody)
```

- **Gate (client):** AI toggle + warm path appear only when crypto rails are enabled and the
  wallet has balance — mirror `verifiablePublishingEnabled` (`FileSelectScreen.cpp:599`).
- **Identity (client):** every `/warm` renewal and `/tools/*` call is **signed by the
  identity key** so the proxy knows who to hold warm / attribute. This replaces the constant
  `kLabel = "inkternity"` in `WarmLease` — the whole per-user attribution gap is that one
  string becoming a signed identity.
- **Payment (client ↔ proxy):** the x402 endpoint. `402` carries price + payee address +
  window length; the client answers with a **signed Stellar payment from the wallet**; the
  proxy verifies it landed and extends this identity's warm window. Coarse (~15 min).
- **Window accounting (proxy):** minimal — a **paid-through timestamp per identity**; warm is
  granted while `now < paid_through`; renewals refuse (→ lease lapses → AI auto-disables in
  the app) once it passes and no new payment arrives. No prepaid balance held by HEAVYMETA.
- **Top-up (client):** *not our flow.* The user funds their own wallet with their own USDC;
  `WalletPanel` just shows the balance (it already does the Horizon read). If they have no
  crypto, acquiring it is a third-party on-ramp's job — outside the app and outside our
  regulatory surface.

---

## 6. Authentication & anti-free-riding

**Threat:** someone cracks or clones Inkternity and free-rides the inference GPU.

**Non-goal: an unclonable client.** A native binary is fully untrusted — it can be
disassembled, its embedded secrets extracted, its requests replayed or reimplemented. The
model must **not** depend on client secrecy. The goal is to make free-riding *structurally
impossible*, which the pay-from-wallet model does for free.

### The reframe
Free-riding collapses into "spending a funded wallet." If every inference request is
attributed to a **funded, server-tracked identity**, a cracked client can't get *free*
inference — only spend value someone funded. The server answers two questions the client
cannot forge:
1. **Which identity/wallet does this charge?** (authenticate identity)
2. **Has it paid for the current window, and did its owner authorize this?** (verify signature)

Neither depends on the binary being secret.

### Kill the current primitive
`HVYM_TOOLS_KEY` today is a **single shared bearer secret** (`X-API-Key`), self-described as
*"spend-control, not identity, rotatable server-side"* (`GlobalConfig.hpp:91-96`). Extract it
once → free inference for everyone until rotation. **This is the exact free-ride vector.** It
must not gate billing — keep it, if at all, only as a coarse bot-gate / kill-switch.

### Reuse the rails we already have
- **Identity:** per-install `DevKeys` ed25519 keypair (`app_pubkey`/`app_secret`), upgraded
  to the **portal-issued** credential in Phase 1.
- **Signed-request auth (not bearer):** every `/warm` renewal and `/tools/*` call is signed
  by the identity key over a **server nonce + timestamp + lease_id**. Server verifies against
  the claimed pubkey. Replay-proof. Reuses the `base64url(sig).base64url(payload)` ed25519
  envelope from `Subscription/TokenVerifier` (`parse_and_check_signature`) and `C2PA/WireToken`.
  The x402 pay path is already a signed payment, so it self-authenticates the settlement.
- **Provenance / trust anchor:** portal-minted token binding {identity → account}, verified
  by the proxy via the `is_trusted` / registry pattern — distinguishes a real provisioned
  account from a raw self-minted key; enables revoke and gating.

**Sybil is free but worthless on the paid path:** mint 10,000 `DevKeys` identities — each has
an empty wallet. Inference requires a funded window → they pay. Free inference is impossible
by construction.

### The one place client integrity still matters: the free tier
Self-minted identities + N free trial-minutes each = unlimited free inference via Sybil.
**This is the real Sybil surface — not the paid path.** Bind any free grant to a
**portal-issued** identity that costs something to create (email / captcha / device check),
verified through the WireToken / `is_trusted` rails. The paid path needs none of this.

### Residual threats (all bounded)
| Threat | Outcome | Mitigation |
|---|---|---|
| Extract shared API key | Free inference today | Remove it as billing auth; coarse gate/kill-switch only |
| Clone client, reimplement requests | Still must pay per window from a funded wallet | Not free-riding — a paying customer on another client |
| Steal `app_secret` (plaintext on disk) | Drain **that user's** wallet | Portal identity + encryption at rest + session-float sub-wallet; loss capped at wallet balance |
| Share one funded key across people | Concurrent paid usage | Refcounted; can't exceed what's funded |
| Sybil the **free tier** | Free minutes ×∞ | Portal-issued, cost-to-create identity for trial grants only |

**Net:** shared key retired as the auth primitive; **signed-per-identity requests +
pay-from-wallet are the defense** (economic, robust to cracking by construction); the
**portal handshake is the trust anchor** and the only thing between us and free-tier Sybil.
No new crypto — the ed25519 wire-token + portal machinery already exists, pointed at the
inference path.

---

## 7. Build phases

| Phase | Where | Work |
|---|---|---|
| **0 — validate rate** | proxy | Reconcile $1.12/hr against `/billing/endpoints` after a real day. Confirms every number in §3–4. |
| **1 — identity** | client + portal | Portal-issued credential ([[project_glasswing_keypair_pattern]]); sign `/warm` + `/tools/*` with it. **Gate for real money.** |
| **2 — window accounting** | proxy | Paid-through timestamp per identity; grant warm while inside it; refuse renewal past it. Lightweight — metering half-exists (`Lease.label`/`held_s()`). |
| **3 — x402 endpoint** | proxy | `402` challenge → verify a signed Stellar payment landed → extend the window. |
| **4 — x402 client** | client (C++) | Native x402 in `WarmLease`/`ToolClient`: sign a Stellar payment from the wallet, retry, extend. |
| **5 — crypto-rails gate + UX** | client | Gate the AI toggle behind crypto-rails-enabled + funded (mirror `verifiablePublishingEnabled`); wallet balance, low-balance warning, auto-disable at zero, idle auto-shutoff. Reuse `WalletPanel`. |
| **6 — display** | client | Live "~$0.05/min · ~$2.40 in wallet · ~48 min" on the AI toggle; settled in USDC for stable pricing. |

---

## 8. Open questions / decisions

1. **Settlement asset: USDC (stable) or XLM (spot-priced)?** Lean **USDC** — USD-stable
   pricing with no peg mechanism, no volatility surprise mid-session.
2. **One flag or a shared "crypto rails" parent?** Reusing `verifiablePublishingEnabled`
   ships fastest; a shared parent (wallet reveal → both C2PA *and* AI available) is cleaner
   and lets someone enable the wallet for AI without wanting provenance signatures. Lean
   **parent**, but not a blocker.
3. **Window length.** ~15 min balances chain-tx frequency (4/hr) against how much unused warm
   an artist forfeits on early exit. Tunable.
4. **Warm-window vs. streaming.** A single payment per window is simple; Stellar payment
   channels could stream finer later. Not needed for v1.
5. **Free tier / trial** — N free warm-minutes to seed adoption, bound to portal identity?
   Comes straight out of the 63% margin.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Published $1.12/hr wrong | Phase 0 reconciles against real billing before pricing is fixed. |
| Auth/identity orphaned on reinstall | Phase 1 portal identity (the hard gate). |
| Forgotten warm burns wallet balance | Lease TTL bounds it to ~60 s; idle auto-shutoff on top. |
| Throttled worker, artist waits, no model | Settle on **grant**, not request — no warm, no charge. |
| XLM volatility | Settle in **USDC**; price windows in USD. |
| Double-charge on retry | Idempotent window extension keyed on the payment tx id. |
| Concurrency contention | `workersMax` scales with concurrent warm users; refcounting shares one worker where clients overlap. |
| AI unavailable to crypto-averse users | **Accepted** — AI is a crypto-rails feature (§2.8), same bargain as verifiable publishing. |
