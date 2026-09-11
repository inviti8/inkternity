# AI_BILLING_PHASE4.md — the x402 client (native pay-per-window)

**Status:** plan, ready to build. Child of `AI_BILLING_INTEGRATION.md` (the
authoritative decision doc) — this is the implementation plan for its **Phase 4**.
**Depends on:** Phase 1 signed identity (shipped, `ff02443`), the proxy's x402
endpoints (live on `img.hvym.link` 0.4.0, enforcement flags OFF).
**Cross-repo contract:** hvym-img-tools `docs/{X402_BILLING,CLIENT,X402_CLIENT_HANDOFF}.md`.

---

## 0. What Phase 4 is

Make the client **pay for a warm window on-chain when the proxy asks**. Today a
`POST /warm` (or `/tools/*`) can return `402` with an x402 challenge; the client
must answer it with a signed Stellar USDC payment, then continue as before. When
this lands and works, the proxy flips `HVYM_REQUIRE_PAYMENT` and the feature is
live.

**Governing constraints (settled — do not re-litigate):**
- **Self-fund.** The artist funds their own DevKeys wallet with XLM + USDC. There
  is **no in-app on-ramp** (`AI_BILLING_INTEGRATION.md` §5). Acquiring crypto is
  out of scope.
- **Pay from the DevKeys wallet.** The payment source account **is** the identity
  in `X-Ink-Pubkey`. The proxy enforces this (`HVYM_BIND_PAYER_IDENTITY` ON,
  confirmed 2026-09-11). One keypair, no second account.
- **No XDR port.** Reuse the `stellar` CLI subprocess the C2PA system already
  ships (`StellarCli`), per the founder decision of 2026-05-21. See §2.
- **Crypto-averse copy.** Trustline setup and payment are silent/plain-language;
  no seeds, no "sign a transaction" jargon at the UI ([[feedback_crypto_averse_users]]).

---

## 1. What already exists to build on (verified in-tree)

| Piece | Where | Reused for |
|---|---|---|
| Subprocess `stellar` CLI: probe → cache → auto-install (pinned `23.4.1`), `invoke(argv)` → `{exit_code, stdout}`, `ScopedJsonFile` arg staging | `src/C2PA/StellarCli.hpp/.cpp` | running every payment / trustline command |
| Signed-txn submit pattern: `base_invoke_args` → `invoke` → `find_tx_hash` (bounded 64-hex scrape), network presets (`config_for_env_or_global`) | `src/C2PA/SorobanSubmit.cpp` | the shape our `payment`/`change_trust` wrappers mirror |
| Request signing: `X-Ink-Pubkey` + `X-Ink-Auth` over a canonical payload for any (method, path, tool, lease) | `src/AI/RequestSigner.hpp/.cpp` | signing `POST /warm/pay` |
| Wallet identity every launch: `app_pubkey()` (G), `app_secret()` (S-strkey), `app_seed_bytes()`, `is_loaded()` | `src/DevKeys.hpp` | the payer key handed to `--source-account` / `--sign-with-key` |
| Stellar strkey/BIP-39/SEP-0005 primitives (read-only, no network) | `src/crypto/stellar/Stellar.hpp` | validating addresses, deriving pubkey if needed |
| Warm lease loop: renew thread, `post_warm` (curl), `WarmResult`, per-tool state | `src/AI/WarmLease.cpp` | the 402 interception point |
| Tool call path: `ToolClient::request` (curl multipart) | `src/AI/ToolClient.cpp` | the second 402 interception point |

**Note the gap this closes:** `WarmLease::post_warm` currently maps **any**
non-200 (402 included) to `ok=false` → `State::FAILED` → 3 s retry. Left as-is
under `HVYM_REQUIRE_PAYMENT`, the client would silently hammer 402s. Phase 4
teaches it to recognize and answer a 402.

---

## 2. The CLI recipes (verified against `stellar 23.4.1`)

All commands run through `StellarCli::invoke(argv)`. Network args come from the
challenge / `config_for_env_or_global`. **Amounts are in stroops** (1 USDC =
`10_000_000`); convert the quote's decimal string with exact fixed-point (never
float). Asset format is `CODE:ISSUER`, e.g. `USDC:GA5ZSEJY…`.

### 2.1 Trustline (once, before the wallet can hold USDC)
One-shot build+sign+send — no memo needed:
```
stellar tx new change-trust
    --line USDC:<issuer>
    --source-account <S-secret>        # signs locally
    --rpc-url <rpc> --network-passphrase <passphrase>
```
`--limit` omitted = max. Idempotent-ish: re-running an existing trustline is a
cheap no-op/failure we can ignore. **The `op_no_trust` trap** (handoff §"five
things"): without this, a USDC payment fails at the *sender* and nothing appears
anywhere. So trustline-first is mandatory.

### 2.2 Payment with memo (the crux — 5-step pipeline)
`stellar tx new payment` has **no `--memo` flag** in 23.4.1, and `tx update` only
edits the sequence number. The memo (a `MEMO_TEXT` that binds the payment to the
quote — without it the proxy settles nothing) must be injected via decode→encode:

```
# 1. build unsigned (CLI fetches sequence from RPC — needs --rpc-url even here)
XDR=$(stellar tx new payment --build-only \
        --source-account <G-pubkey> \
        --destination <pay_to> --asset USDC:<issuer> --amount <stroops> \
        --rpc-url <rpc> --network-passphrase <passphrase>)
# 2. decode to JSON
JSON=$(stellar tx decode <<< "$XDR")
# 3. inject memo in C++ (nlohmann): set the envelope's memo to MEMO_TEXT=<memo>
# 4. re-encode
XDR2=$(stellar tx encode <<< "$JSON_WITH_MEMO")
# 5. sign, then send
SIGNED=$(stellar tx sign --sign-with-key <S-secret> \
            --network-passphrase <passphrase> <<< "$XDR2")
stellar tx send --rpc-url <rpc> --network-passphrase <passphrase> <<< "$SIGNED"
```
`tx send` output → `find_tx_hash` → the 64-hex `X-Payment` value.

> **Task #1 memo shape — RESOLVED (2026-09-11, verified against testnet 23.4.1).**
> The envelope is a V1 `TransactionEnvelope`; decode/encode JSON is:
> `{"tx": {"tx": { …, "memo": <M>, "operations":[…] }, "signatures":[…]}}`.
> `<M>` is the string `"none"` for MEMO_NONE and `{"text":"<utf8>"}` for
> MEMO_TEXT. Injector sets `env["tx"]["tx"]["memo"] = {"text": <challenge.memo>}`.
> Round-trips cleanly through `encode`→`decode`. **Guard:** MEMO_TEXT is ≤ 28
> bytes — validate the challenge's `memo` fits and error loud if not (never
> truncate; a wrong memo settles nothing).

> **Simplification to evaluate at task #1:** if `stellar tx sign`/`send` accept a
> file arg instead of stdin cleanly on Windows via `StellarCli`, prefer temp
> files (`ScopedJsonFile` pattern) over stdin piping — SDL_CreateProcess stdin
> wiring is less battle-tested here than the file-arg path C2PA already uses.

---

## 3. New code

### 3.1 `src/AI/StellarPay.{hpp,cpp}` — CLI wrapper (mirrors `SorobanSubmit`)
```cpp
namespace AI {
struct PayResult { bool ok=false; std::string tx_hash; std::string error; std::string raw; };

class StellarPay {
public:
    // Uses the shared StellarCli (see §3.4) + network from the challenge.
    static bool has_trustline(const std::string& horizonOrRpc,
                              const std::string& account, const std::string& assetCode,
                              const std::string& issuer);          // read-only probe (§4)
    static PayResult ensure_trustline(const std::string& issuer,
                              const std::string& rpc, const std::string& passphrase);
    static PayResult pay(const std::string& payTo, const std::string& issuer,
                         const std::string& amountStroops, const std::string& memo,
                         const std::string& rpc, const std::string& passphrase);
};
}
```
Amount conversion helper: quote decimal string → stroops (exact).

### 3.2 `src/AI/X402Challenge.{hpp,cpp}` — parse + orchestrate
```cpp
struct X402Challenge {
    std::string asset, issuer, amount, payTo, network, memo, priceId, horizon;
    double windowS = 0;
    static std::optional<X402Challenge> parse(const std::string& body);  // from the 402 JSON
};

// One attempt to satisfy a challenge for `tool`. Returns true iff /warm/pay 200'd.
bool settle_window(const X402Challenge&, const std::string& tool,
                   const std::string& baseUrl, const std::string& apiKey);
```
`settle_window` = (ensure trustline) → `StellarPay::pay` → `POST /warm/pay` with
`X-API-Key`, `X-Ink-*` signed as `("POST","/warm/pay",tool,"")`, `X-Payment:<hash>`,
body `{"price_id":…, "tool":…}`. On the response: `200` = paid (done); `402` =
a specific check failed (`detail` says which; a fresh challenge rides along).

### 3.3 Wire into `WarmLease` (and `ToolClient`)
- Generalize `append_signed_headers` to take the path (currently hardcodes
  `/warm`) so it serves `/warm/pay` too.
- In `post_warm`: on `http == 402`, parse the body → `X402Challenge`. If parse ok
  and we haven't already got an in-flight settlement for this window, run
  `settle_window` **outside the lock** (it's a 5–10 s CLI round-trip), then return
  a soft result so the worker retries `post_warm` promptly rather than dropping to
  `FAILED`. Add `State::PAYING` for UI.
- `ToolClient::request`: same 402 handling. In practice a live window means
  `/tools/*` won't 402, but the note says both can — centralize via `settle_window`.

### 3.4 Shared `StellarCli` ownership (decision needed — see §9 Q1)
`StellarCli` is currently owned by `C2PA::RegistrationFlow`, lazily built on the
verifiable-publishing toggle. AI billing can be enabled independently, so it needs
a CLI instance too. Two probes/installs of the same binary is wasteful but
harmless. **Proposed:** hoist one `StellarCli` to a process-lifetime holder both
C2PA and billing borrow (single probe, single auto-install, one "Setting up…"
surface). Fallback: give billing its own instance.

---

## 4. Client-side window state (settle-on-grant — do NOT count down locally)

The proxy is authoritative on time. After a successful `/warm/pay`, the window
clock **starts when a worker actually reaches `warm`** for this wallet, not at
payment. Until then the proxy returns `paid_through: null` with
`pending_credit_s: 900` — **that is correct, not an error** (handoff §"five
things" #4). So the client:
- does **not** compute expiry from payment time;
- keeps renewing; each `/warm` 200 carries the live window view — trust it;
- treats the **next genuine 402** as "time to buy the next window", and only then
  pays again. Never pre-pays off a local timer.

Guard against double-pay: once `settle_window` 200s for a `price_id`, remember
that `price_id` as consumed for this session; ignore repeat 402s carrying the same
`price_id` until a new one appears (tx-hash resubmit is idempotent server-side —
returns `200 credited:false` — so a lost response is safe to retry, but we
shouldn't spam fresh payments).

---

## 5. Funding & trustline UX (self-fund model)

No on-ramp. The app's job is only to **tell the artist what their own wallet
needs** and to **set up the trustline for them** once funds arrive:
- **Balance read** — reuse the on-demand Horizon GET already in `FileSelectScreen`
  (the WalletPanel balance probe) to show XLM + USDC and detect the trustline.
- **Trustline** — the first time AI is enabled with a funded wallet and no USDC
  trustline, run §2.1 silently ("Setting up payments…"). Needs ~1.5 XLM
  (base reserve + 1 trustline + fees) present first.
- **Copy, crypto-averse:** surface an address to fund and a plain "you need about
  $X of USDC and a little XLM for fees"; never mention seeds. Exact wording TBD
  with the UX pass (Phase 5/6 territory; Phase 4 just needs the mechanism).

---

## 6. Concurrency & failure

- All CLI calls run on the existing `WarmLease` worker thread, **outside** `gMutex`
  (that thread already does network outside the lock). A payment is infrequent
  (~once per window) so a brief block is fine — **but** a 5–10 s payment must not
  starve the *other* tool's renewal past its ~60 s TTL. If both tools are held,
  run settlement without holding back the other's renew tick (settle for tool A
  shouldn't delay renew for tool B). Consider a dedicated short-lived settle
  thread if interleaving proves awkward.
- Every failure is recoverable and visible: trustline missing, insufficient XLM,
  insufficient USDC, RPC timeout, CLI spawn failure → distinct `State`/message,
  never a silent stall. Insufficient funds → clear "add funds" state, stop
  retrying the payment (retrying won't help), keep the toggle honest.

---

## 7. The five gotchas → concrete handling

1. **Fresh nonce per request incl. retries** — `RequestSigner::sign_request`
   already generates a fresh nonce each call; just never cache/reuse a header set.
2. **Sign the app-visible path** — `/warm`, `/tools/<tool>`, `/warm/pay`. No host,
   no query. (Generalize `append_signed_headers` accordingly.)
3. **Clock skew (±120 s)** — surface a clear error if `/warm/pay` 402s on
   freshness; optionally warn if the machine clock looks off. Don't silently loop.
4. **Settle-on-grant** — §4. `pending_credit_s` with `paid_through:null` is normal.
5. **Idempotent tx-hash resubmit** — safe to re-`POST /warm/pay` the same hash on a
   dropped response (`200 credited:false`). Use this for the lost-response retry.

---

## 8. Config / env

| Key | Purpose | Default |
|---|---|---|
| existing `stellarNetwork` (GlobalConfig) + `STELLAR_NETWORK` env | mainnet vs testnet for pay commands | Mainnet |
| existing `hvymToolsKey` / `HVYM_TOOLS_KEY`, `HVYM_TOOLS_ENDPOINT` | coarse gate + base URL | `img.hvym.link` |
| (none new required) | challenge carries asset/issuer/payTo/memo/priceId/rpc | — |

The challenge is self-describing (issuer, payTo, network, horizon), so no new
hardcoded Stellar config is required. Testnet vs mainnet for *building* the tx
follows `config_for_env_or_global`, cross-checked against the challenge's `network`
(refuse to pay if they disagree — prevents quoting a price on the wrong network).

---

## 9. Open questions / decisions

1. **Shared vs. dedicated `StellarCli`** (§3.4). Lean: shared, hoisted to
   process lifetime. Confirm.
2. **Trustline timing** — set it up eagerly when AI is first enabled + funded, or
   lazily on the first `op_no_trust`-class failure? Lean eager (fewer surprises),
   but it spends a tiny bit of XLM before the artist has paid for anything.
3. **Auto-pay vs. confirm** — pay the 402 automatically (smooth, "a few cents"),
   or show a one-tap confirm the first time / per window? Product call; affects
   whether warm can silently spend. Lean: auto within a session the artist opted
   into, with a visible running cost (Phase 6 display).
4. **Amount to send** — exactly the quote, or a small buffer? Lean exact; the quote
   is authoritative and the proxy checks `>= amount`.

---

## 10. Task breakdown (ordered, each independently testable)

1. **Confirm memo JSON shape** (§2.2 task #1) against testnet; write the stroops
   converter + a `stellar_pay` smoke test that pays testnet USDC by hand.
2. **`StellarPay`** (§3.1) — `ensure_trustline`, `pay`, `has_trustline`. Test
   against testnet issuer `GBBD47IF6LWK7P7MDEVSCWR7DPUWV3NY3DTQEVFL4NAT4AQH3ZLLFLA5`.
3. **`X402Challenge::parse`** (§3.2) — unit-test against the sample body in the
   handoff.
4. **`settle_window`** (§3.2) + generalized signed headers for `/warm/pay`.
5. **WarmLease 402 interception** (§3.3) + `State::PAYING`; then `ToolClient`.
6. **Window state guard** (§4) — no double-pay, settle-on-grant respected.
7. **Trustline/funding UX hooks** (§5) — mechanism only; copy in Phase 5/6.
8. **End-to-end on testnet** against a local proxy with `HVYM_REQUIRE_PAYMENT=1`
   and `HVYM_STELLAR_NETWORK=testnet` — mirror `scripts/x402_testnet_rehearsal.py`
   and `scripts/x402_testnet_real_usdc.py` from hvym-img-tools (the reference
   client that's already green: 25/25 and 11/11).

**Coordination:** proxy flips `HVYM_REQUIRE_SIGNED_IDENTITY` any time (client
already signs — harmless). It flips `HVYM_REQUIRE_PAYMENT` only after step 8 and
our go-ahead, and after Phase 0 rate reconciliation.

---

## 11. Risks

| Risk | Mitigation |
|---|---|
| Memo JSON shape wrong → payments settle nothing | Task #1 confirms it empirically before any pay code ships; testnet e2e (step 8) proves the whole loop before mainnet. |
| `op_no_trust` invisible failure | Trustline-first (§2.1) + `has_trustline` precheck; distinct error state. |
| Payment blocks the other tool's renewal past TTL | Settlement off the critical renew path (§6). |
| Double-charge on retry storms | Idempotent tx-hash + `price_id` consumed-guard (§4). |
| Wrong network (mainnet price paid on testnet or vice-versa) | Cross-check challenge `network` vs config before building (§8). |
| Insufficient funds loops forever | Detect + stop retrying + clear "add funds" state (§6). |
| CLI absent / auto-install fails offline | `StellarCli` already surfaces install stage/errors; AI stays disabled with a clear reason, exactly like C2PA. |
