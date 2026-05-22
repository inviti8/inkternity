# Inkternity — C2PA App-CA + Signed-Publishing Plan

Desktop-side wiring for the `hvym-cert-registry` Soroban contract
(mainnet `CAKBTT765YCBZDPU7RNPGC4C4TSXIRFHQCEBNPEQZNMJCLXAB3K6VE2G`,
testnet `CC252R637U7QXG5SSHTVHBSKB3PGKRKRP66EEI2IEVTXIQWP6EQRLH2T`,
wasm hash `a85d4e7e67757d36c7d50c180a88f07a29c7f87d6f7288c398d5734ece40d09e`).
Contract ID resolved through `hvym_registry` under the name
`hvym_cert_registry` — desktop never hardcodes the address.

Companion docs:

- `../../docs/design/DISTRIBUTION-PHASE1.md` — the precedent identity flow
  (BIP-39 mnemonic → SEP-0005 → Stellar strkey). C2PA reuses the same
  app keypair verbatim.
- `C:/Users/surfa/Documents/metavinci/hvym-market-muscle/C2PA.md` —
  architectural picture (Stellar-anchored C2PA trust list, dual-auth
  registration, leaf ephemerality, proof-of-active-use via storage rent).
- `C:/Users/surfa/Documents/metavinci/pintheon_contracts/HVYM_CERT_REGISTRY.md` —
  contract surface, canonical payload encoding (locked).
- `C:/Users/surfa/Documents/metavinci/pintheon_contracts/mock_c2pa/` —
  runnable Python end-to-end reference. `andromica/ca_generation.py` and
  `andromica/bundle.py` are drop-in templates for what we re-implement in
  C++ here.
- `C:/Users/surfa/Documents/metavinci/heavymeta_collective/C2PA_PORTAL_PLAN.md` —
  Portal-side plan (shipped 2026-05-21). Defines the bundle text we
  emit and the wire-token format we consume.

---

## 1. Why this doc isn't shorter

A lot of what a "C2PA in a desktop app" plan would normally cover
already exists in this tree:

- **App keypair.** `DevKeys::ensure_app_keypair`
  (`src/DevKeys.cpp:112`) generates an Ed25519 Stellar keypair on first
  launch via BIP-39 → SEP-0005 (`src/crypto/stellar/Stellar.hpp:44-69`),
  persists it to `<configPath>/inkternity_dev_keys.json`, and exposes
  raw 32-byte seed via `DevKeys::app_seed_bytes()` (`src/DevKeys.hpp:111`).
  C2PA registration is keyed on this. No second keypair.
- **Ed25519 sign/verify.** tweetnacl vendored
  (`deps/tweetnacl/tweetnacl.{h,c}`); used in
  `src/Subscription/TokenVerifier.cpp:5` and `StellarMnemonic.cpp:18`.
- **Wire-token shape.** `b64url(sig).b64url(payload)` sorted-keys JSON
  parser already lives at `src/Subscription/TokenVerifier.cpp:142-176`
  — C2PA token has the same shape, different schema. ~30 LoC of new
  parsing.
- **HTTP client.** `libcurl` linked (`CMakeLists.txt:292,494`); async
  pattern in `include/Helpers/FileDownloader.hpp`. Soroban POSTs ride
  the same dep — §8.2.
- **First-run hook.** `main.cpp:491-492` runs
  `devKeys.ensure_app_keypair()` + `devKeys.load()` at startup. CA gen
  slots in here, idempotently.
- **Settings panel.** `FileSelectScreen::settings_view`
  (`src/Screens/FileSelectScreen.cpp:209`) — existing "Inkternity App
  Key" card sits here; wallet panel + gateway toggle + walkthrough
  extend the same view.
- **Atomic file write.** `World::save_to_file` (`src/World.cpp:664`)
  already does tmp + rename with SEH guard.
- **Export pipeline.** `world_take_screenshot`
  (`src/WorldScreenshot.cpp:35`) encodes PNG/JPG/WEBP/SVG via Skia.
  C2PA embed slots between `encode → SkData → SDL_SaveFile`.

What's actually new: a C++ port of
`mock_c2pa/andromica/ca_generation.py` (OpenSSL libcrypto, §8.1); a
bundle emitter; a Soroban RPC submit path over libcurl with
hand-rolled scval (§8.2); per-publish leaf issuance + `c2pa-rs`
manifest embed (§8.3); an import-side verifier; the gateway toggle
(§3.2 / §B).

### 1.1 P0 prerequisite — screenshot save is broken

`world_take_screenshot` (`src/WorldScreenshot.cpp:35`) is the export
path the C2PA image-embed hook (§8.3 / I14) slots into at line 84,
between `out.flush()` and `SDL_SaveFile`. The export UI runs end-to-end
in current `main` but the file never lands on disk — failure mode
not yet root-caused; candidates include `info.filePath` arriving
empty from the caller, the Skia encode silently returning false
(`success` path logs `WORLDFATAL` to the in-app logger but doesn't
surface to the user), or `SDL_SaveFile` failing without the
exception path firing.

This blocks the entire export-signing branch (tasks I14 + the
export half of I17) from being testable or shippable. Fix lands as
**I0 in §12** before any C2PA work touches this file. Scope of I0
is strictly the bug — no signing logic, no refactor. The signing
hook integration is still I14, layered on top of a working save.

The `.inkternity` save path (`World::save_to_file`, `src/World.cpp:664`)
is independent and not affected by this bug.

---

## 2. New code surface

| File | LoC | Purpose |
|---|---|---|
| `src/C2PA/AppCa.{hpp,cpp}` | ~350 | Generate self-signed Ed25519 X.509 CA with the four extensions (CA:TRUE pathlen:0 critical, KeyUsage critical, EKU OID `1.3.6.1.4.1.42038.1.5.0` critical, SAN `URI:stellar:G…` + `URI:heavymeta:app/Inkternity` non-critical). Persist DER + key to `<configPath>/c2pa/`. Compute SHA-256 fingerprint. Mirrors `mock_c2pa/andromica/ca_generation.py::generate_app_ca` and `AppCa.{der_bytes,fingerprint_sha256,expires_at_unix}`. |
| `src/C2PA/Bundle.{hpp,cpp}` | ~80 | Emit `HVYM-CA-REG-v1` / `HVYM-CA-ROT-v1` / `HVYM-CA-REV-v1` text bundles. Round-trip through `cert_registry_bundle.py` is the cross-repo agreement gate (§9). |
| `src/C2PA/WireToken.{hpp,cpp}` | ~120 | Parse `b64url(sig).b64url(payload)` tokens minted by the Portal. Lifts shape from `src/Subscription/TokenVerifier.cpp:142-176`; new code extracts `m`/`n`/`fp`/`exp`/`k` rather than `a`/`k`/`c`/`i`/`e`/`sub`. Cross-checks payload fields against the locally-generated bundle (failure modes match `scripts/c2pa_smoke.py:146-153`). |
| `src/C2PA/SorobanSubmit.{hpp,cpp}` | ~600 | Minimal Soroban RPC client over `libcurl` POST. JSON-RPC `simulateTransaction` + `sendTransaction` + `getTransaction`. Hand-rolled scval XDR encoding for `Address` / `BytesN<32>` / `BytesN<64>` / `String` / `U64` / `Bytes` (the only types needed across all four entry points). Read views `get_app_ca` + `is_trusted` decode the response scval to a typed struct. Authoritative shape reference: `mock_c2pa/register.py:145-310`. |
| `src/C2PA/Registry.{hpp,cpp}` | ~120 | Resolve the cert-registry contract ID via `hvym_registry` (per the user-instruction discovery requirement). Reads testnet vs mainnet from existing config knob (TBD — see §10/Q3). Caches across a session. Falls back to the hardcoded testnet/mainnet IDs from §0 if the registry lookup fails (matches Portal behavior in `heavymeta_collective/config.py`). |
| `src/C2PA/LeafIssuer.{hpp,cpp}` | ~200 | Per-publish ephemeral leaf: generate Ed25519 keypair → build minimal CSR (CN/O/OU + SAN `URI:stellar:G…` + EKU `1.3.6.1.4.1.42038.1.5.0`) → sign with the app CA key → return chain `[leaf_der, ca_der]` + ephemeral leaf private key. Key never touches disk; `crypto_sign_detached` happens inline in the publish pipeline and the key goes out of scope. Mirrors `mock_c2pa/andromica/ca_generation.py::issue_member_leaf`. |
| `src/C2PA/ManifestEmbed.{hpp,cpp}` | ~400 | Wrapper around `c2pa-rs` (FFI or shell — §8.3). Two entry points: `embed_into_image_file(path, chain, leaf_key, assertions)` and `embed_into_inkternity_file(path, chain, leaf_key, assertions)`. The former is standard library use; the latter writes a sidecar `<file>.c2pa` (§C / §10/Q5). |
| `src/C2PA/Verifier.{hpp,cpp}` | ~300 | Chain walk: `c2pa-rs verify` → extract leaf + CA → confirm CA self-signed + EKU correct → extract SAN `stellar:G…` from CA → resolve on-chain `get_app_ca(app_address)` → `is_trusted(app_address, sha256(ca_der))` → compare on-chain `member_pubkey` to leaf SAN strkey. Result enum mirrors `mock_c2pa/register.py::is_trusted` semantics. |
| `src/C2PA/KeyStore.{hpp,cpp}` | ~150 | Read / write `<configPath>/c2pa/app_ca.key` (PKCS#8 PEM, file mode `0600` on POSIX, NTFS DACL restricting to the user on Windows). Also persists `app_ca.crt`, `registration_status.json` (one of `unregistered` / `pending_funding` / `pending_token` / `active` / `revoked`), and the last-known on-chain `serial`. Sits next to `inkternity_dev_keys.json` in the same `configPath` dir. No backup, no recovery — by design (§5). |
| `src/Screens/SettingsC2PASection.{hpp,cpp}` | ~500 | Wallet panel + gateway toggle + registration walkthrough + rotate/revoke buttons. Lives inside `FileSelectScreen::settings_view` (`src/Screens/FileSelectScreen.cpp:209`), below the existing "Inkternity App Key" block, gated entirely on the gateway toggle. Naming and copy per §B + §3. |
| `src/C2PA/PublishHook.{hpp,cpp}` | ~250 | The two slots that fire C2PA signing: (a) `World::save_to_file` post-write hook after the `.tmp` → final rename at `src/World.cpp:735`; (b) `world_take_screenshot` post-encode pre-`SDL_SaveFile` hook at `src/WorldScreenshot.cpp:84`. Both hooks short-circuit when gateway off or registration not `active`. |

Boundaries — what these modules MUST NOT do:

- Never persist the per-publish leaf key. It exists in
  `LeafIssuer::issue_leaf`'s return value and dies with the call
  frame. The CA key persists; the leaf key does not.
- Never call out to the Portal. The Portal does not run on the
  desktop. The desktop emits a bundle → user pastes into Portal →
  user pastes wire-token back. No HTTP between the two.
- Never reconstruct the canonical payload bytes the Portal signed.
  Decode `payload_b64` once, hold the original bytes, pass them
  through unchanged to `register_app_ca`'s `auth_payload`
  parameter. The contract reconstructs and byte-compares; we are
  not authoritative on the encoding. (See `mock_c2pa/canonical.py`
  for the locked encoding the Portal uses.)
- Never block the GUI thread on a Soroban submit. All submit /
  read calls happen on the same async pattern `FileDownloader` uses
  (`include/Helpers/FileDownloader.hpp:35`) — std::thread + atomic
  status — with a UI status reflector that polls.
- Never hardcode the contract ID. Always resolve via §2.Registry,
  with the §0 fallback if the registry lookup fails.

---

## 3. UX flow (member-facing)

### 3.1 Defaults + gateway

Ships with the gateway toggle **OFF** (§B). Until flipped, no
Stellar / wallet / "Signed publishing" copy is visible. Files
without manifests open silently; files *with* manifests also open
silently (metadata ignored).

CA keypair is still generated + persisted at first launch behind
the toggle, alongside `inkternity_dev_keys.json` in
`DevKeys::ensure_app_keypair` (`src/DevKeys.cpp:112`). Cost: one
ed25519 keygen + one self-signed cert build (sub-millisecond).
Lazy-on-flip would force a "setting up…" delay at the worst time
(user just signaled interest).

Settings → **Verifiable publishing** header. Single checkbox: *"Enable
verifiable publishing"*. Inline blurb:

> Adds a tamper-evident provenance signature to canvases and
> exported images. Verifiable against the Heavymeta cooperative's
> on-chain trust list. Free to read; requires a one-time on-chain
> registration (≈ 5 XLM) to write.

ON reveals wallet panel (§3.2), walkthrough (§3.3), rotate/revoke
(§3.5/§3.6), verifier badges (§7). If status is already `active`,
skip the walkthrough. OFF hides every C2PA UI surface but does
**not** alter the on-chain record. The word "Stellar" only appears
inside an expanded "Show advanced" disclosure.

### 3.2 Wallet panel

Below the gateway toggle when ON:

- **App funding address.** App pubkey (G…) as read-only text +
  QR (Skia path drawing, same primitive as
  `include/Helpers/CanvasShareId.hpp`). *Copy* + *Show QR* buttons.
  Reuses `DevKeys::app_pubkey()` — same G… already shown in the
  "Inkternity App Key" block. No second keypair.
- **Balance.** "Balance: X.X XLM" + *Refresh*. Async Horizon GET
  (`FileDownloader` pattern) on settings open + on every Submit
  click. 30 s session cache.
- **Funding hint.** *"You need ≈ 5 XLM in this account to register
  or maintain signed publishing."* No exchange referrals.
- **Show advanced.** Disclosure reveals secret-key export/restore
  (already wired at `src/Screens/FileSelectScreen.cpp:321` /
  `:372`) + the literal text "Stellar account".

Funding address == receive address — same app keypair per
`BYLAWS.md §3.10`. There is only one app credential; if future
sub-revenue lands on Inkternity, it lands on the same account.

### 3.3 First-run registration walkthrough

Triggered on OFF → ON when status is `unregistered`. Four
expandable cards in the same Clay layout as the existing
keypair export/restore disclosures
(`src/Screens/FileSelectScreen.cpp:241-274`).

1. **Bundle.** Pre-computed at toggle-flip from the generated CA.
   Render via `Bundle::build_register_bundle(...)` (same fields as
   `mock_c2pa/andromica/bundle.py::build_bundle`). Code block +
   *Copy* + *Open Heavymeta dashboard* (plain URL via
   `SDL_OpenURL` — see §10/Q4).
2. **Paste wire token.** `WireToken::parse(text)`; cross-check
   `payload.a` == local app pubkey, `payload.fp` == local CA
   fingerprint, `payload.k == "Inkternity"`, `payload.exp` == CA
   `notAfter`. Mismatch names the failing field
   (matches `c2pa_smoke.py:146-153`).
3. **Funding check.** Re-poll balance. If < 5 XLM, show needed
   amount, disable Submit. If ≥ 5 XLM, Submit reads "Register
   on-chain".
4. **Submit.** `SorobanSubmit::submit_register(...)`. On success,
   `registration_status.json` → `active`. On failure: surface
   `payload_mismatch` / `insufficient_balance` / `tx_failed` /
   network error.

Interruptible: closing settings preserves step in
`registration_status.json` (`pending_token`, `pending_funding`).

### 3.4 Rotation

*Rotate keys* button, visible when status is `active`. Confirm
copy: *"Replace your current signing keypair with a fresh one.
Previous signatures stay verifiable until they expire."* Steps:
generate new CA (keep old in `<configPath>/c2pa/old_ca/` 30-day
grace for local verify of older leaves) → build
`HVYM-CA-ROT-v1` bundle → repeat walkthrough steps 1–3 →
`SorobanSubmit::submit_rotate(...)`. Manual trigger only in v1
(§10/Q1); 10-year default `valid_days` per
`mock_c2pa/andromica/ca_generation.py:88` makes auto-rotation
non-urgent.

### 3.5 Revocation

*Revoke keys* button, hard confirm: *"This permanently marks your
current signing keypair as untrusted."* No bundle —
`revoke_by_app` requires only `require_auth`
(`mock_c2pa/register.py:219-235`). On success: status `revoked`,
hide publish-pipeline signing. Verifier badges keep working.

---

## 4. Funding floor + clarity

Hardcoded **"≈ 5 XLM"** in v1 per Portal-plan D3, parameterize
after testnet empirical-fee numbers land
(`HVYM_CERT_REGISTRY.md` "Empirical unknowns"). Copy:

- Idle: *"≈ 5 XLM needed to register or maintain verifiable publishing."*
- Below floor on Submit: *"Account needs ≈ 5 XLM. Current balance:
  X.X XLM."* — Submit disabled.
- Above floor: button enabled, no warning.

Funding address shown as plain text + Copy (primary) and QR-modal
(secondary). One app keypair (§3.2) — same address used for
funding + any future receive flow. Balance polling: on settings
open + on Submit. 30 s cache, no background polling.

---

## 5. CA + key persistence

| Path (under `<configPath>` per `main.cpp:359-361`) | Content | Mode |
|---|---|---|
| `c2pa/app_ca.key` | PKCS#8 PEM of CA Ed25519 private key | `0600` / NTFS user-only DACL |
| `c2pa/app_ca.crt` + `c2pa/app_ca.der` | Self-signed CA cert (PEM + DER cache; SHA-256(DER) == on-chain fingerprint) | `0644` |
| `c2pa/old_ca/` | Pre-rotation CA, 30-day grace | same |
| `c2pa/registration_status.json` | `{status, serial, expires_at, last_known_member_pubkey}` | `0644` |

Encryption-at-rest: **no** — inherits the existing on-disk
plaintext keypair model (`src/DevKeys.cpp:148-149`,
`feedback-crypto-averse-users.md`). Device security is the
perimeter. No backup, no recovery: reinstall = new CA = new
registration. Matches `DevKeys::restore_from_input`
(`src/DevKeys.cpp:190-254`) only as fallback — that path restores
the *Stellar* keypair from S…/mnemonic, NOT the CA. CA loss
without device loss = manual rotation via §3.4 with the existing
Stellar keypair as auth.

Memory hygiene mirrors `src/DevKeys.cpp:80-82` —
`std::fill(seed.begin(), seed.end(), 0)` on scope exit;
`OPENSSL_cleanse` on PEM/DER scratch buffers in new code.

---

## 6. Per-publish leaf cert

Mirrors `mock_c2pa/andromica/ca_generation.py::issue_member_leaf`.

```cpp
struct MemberLeaf {
    std::vector<uint8_t> cert_der;          // 24-hour leaf
    std::array<uint8_t, 32> private_seed;   // ephemeral; zero on dtor
    std::array<uint8_t, 32> public_key;
};
MemberLeaf issue_leaf(const AppCa& ca, std::string_view member_stellar_addr,
                       std::string_view member_canon_url = {},
                       std::chrono::hours valid_for = std::chrono::hours{24});
```

Shape: tweetnacl `crypto_sign_keypair` → OpenSSL `X509_*` build →
`X509_sign(..., NULL)` (Ed25519 sign-without-prehash, see
`mock_c2pa/andromica/ca_generation.py:178`). Extensions:
`BasicConstraints CA:FALSE critical`, `KeyUsage digitalSignature +
contentCommitment critical`, `ExtendedKeyUsage 1.3.6.1.4.1.42038.1.5.0
critical`, SAN `URI:stellar:<G…>[, URI:<canon_url>]` non-critical.

Perf: sub-50 ms per publish (keypair gen + X509_sign). Off the
GUI thread; export UI shows "Signing…" between
`encode → SkData` and `SDL_SaveFile`.

**One leaf per export / per save, not per session.** Rationale:
`notBefore` is forensic per-export timestamp; sub-50 ms is
invisible at human export rates; per-session reuse complicates
"when was this signed" with no perf gain. §10/Q6 if testnet
measurements push back.

`MemberLeaf`'s dtor `OPENSSL_cleanse`s `private_seed` + `public_key`.
Move-only, copies deleted. Used inside `ManifestEmbed::embed_*` for
the one `c2pa-rs` signing call, then out of scope. Never touches disk.

---

## 7. Verifier wrapper

Triggered on `.inkternity` open
(`src/Screens/DrawingProgramScreen.cpp:20-22`), dropped image
import (`src/World.cpp:309,336`), file-select grid preview
(NoManifest fast-skips the RPC).

Chain walk — mirrors `mock_c2pa/register.py::is_trusted` +
`heavymeta_collective/C2PA.md §6`:

1. `c2pa_rs::read_manifest(path)` → `NoManifest` if absent.
2. `chain[leaf, ca]` (size != 2 → `UnexpectedChainShape`).
3. CA must be self-signed + carry EKU OID — else `UntrustedRoot`.
4. `sha256(ca.der)` vs on-chain record fingerprint →
   `FingerprintMismatch`.
5. SAN → `app_addr` → `SorobanSubmit::get_app_ca(app_addr)` →
   `UntrustedAppInstance` / `Revoked` / `Expired`.
6. Leaf SAN strkey decode vs `record.member_pubkey` →
   `san_matches`.
7. `c2pa_rs::verify_with_trust_root(path, root=ca)` →
   `SignatureInvalid`.
8. → `Trusted` if `san_matches`, else `TrustedSanMismatch`.

UI surfaces (badges, not blocking dialogs — file always opens):

- `Trusted` → green check + short-strkey member identity.
- `TrustedSanMismatch` / `Expired` → amber.
- `Revoked` / `SignatureInvalid` / `Fingerprint*` /
  `Untrusted*` / `NoStellarBinding` / `UnexpectedChainShape` →
  red + click-for-details.
- `NoManifest` or gateway OFF → no badge.

Chain walk runs on a worker thread (FileDownloader pattern). File
opens immediately with a "verifying…" badge state; resolves when
RPC returns. One-hour in-memory session cache keyed on `app_addr
→ get_app_ca_result`. Public API: `Verifier::verify_file(path) ->
VerifyResult` so the file-select grid can call it without coupling
to the drawing screen.

---

## 8. C2PA library + Soroban client choices

### 8.1 X.509 — OpenSSL libcrypto

Add `openssl/3.6.2` to `conanfile.py` (matches libcurl/8.17.0's
current transitive choice — avoids a double-version conflict).
Rationale: X.509 extension
build is stock libcrypto one-liners (the Python reference
`mock_c2pa/andromica/ca_generation.py` maps line-for-line);
libcurl on Windows/Linux is already built against OpenSSL —
making it a direct dep removes a transitive surprise; Ed25519
`X509_sign` with `algorithm=None` (no prehash) is supported from
1.1.1+. Alternatives rejected: BoringSSL (no Conan recipe in
use, MSVC build cost), mbedTLS (weaker PKCS#8 PEM ergonomics),
rolling our own (out of scope).

OpenSSL is used **only** for X.509 build/sign/parse + DER encode.
Wire-token + payload sign/verify stays on tweetnacl
(`crypto_sign_open` in `TokenVerifier.cpp:134`,
`crypto_sign_detached` for new code). Two libs, two roles.

Emscripten: C2PA features gated off in the web build — file IO
already branches at `src/World.cpp:719-723` and
`src/WorldScreenshot.cpp:91-99`.

### 8.2 Soroban RPC — shell-out to `stellar` CLI with auto-install

**Revised 2026-05-21.** §10/Q11's original resolution to hand-roll
libcurl + scval underestimated the real porting surface — building
a `TransactionEnvelope` from scratch (signed, simulation-applied,
auth-validated) is closer to 10K LOC of stellar-sdk-equivalent
work than the ~250 LOC the plan claimed. Founder decision: pivot
to shelling out to the official `stellar` CLI as a subprocess.

Auto-install handles the "binary dependency to ship" concern:

1. **PATH probe first.** If the artist has a system `stellar` (e.g.
   from `cargo install --locked stellar-cli`), use it as-is.
2. **Cache check.** `<configPath>/c2pa/bin/stellar[.exe]` survives
   reinstalls of Inkternity and is per-config-path (per OS user).
3. **GitHub Releases auto-install.** Download
   `stellar-cli-<PINNED_VERSION>-<rust-triple>.<ext>` from
   `github.com/stellar/stellar-cli/releases`, extract via `tar` (on
   Win10 1803+ this ships as `System32\tar.exe`), move the
   resulting `stellar[.exe]` to the cache dir, set the executable
   bit on POSIX, verify with `stellar --version`. State machine:
   `Idle → Downloading → Extracting → Verifying → Done|Failed`.

`src/C2PA/StellarCli.{hpp,cpp}` is the binary manager (I8a).
`src/C2PA/SorobanSubmit.{hpp,cpp}` (I8b) is a thin wrapper around
`stellar contract invoke ... -- <fn> --arg1 v1 --arg2 v2 ...` for
each contract entry point.

```cpp
namespace C2PA::Soroban {
struct InvokeResult {
    enum class Status { Pending, Success, Failed } status;
    std::string tx_hash, error;
    std::string raw_out;     // captured stellar CLI stdout for forensics
};
InvokeResult submit_register(const StellarCli&, std::string_view rpc_url,
    std::string_view network_passphrase, std::string_view contract_id,
    std::string_view app_secret_s_strkey,    // funded source account
    BytesN<32> member_pubkey, AppKind, BytesN<32> fingerprint,
    uint64_t expires_at, uint64_t nonce,
    const std::vector<uint8_t>& auth_payload,
    const std::vector<uint8_t>& auth_signature);
std::optional<AppCaRecord> get_app_ca(const StellarCli&, ...);
bool is_trusted(const StellarCli&, ...);
}
```

Network selection (`STELLAR_NETWORK=testnet` or
`GlobalConfig::stellarNetwork` per §10/Q3) feeds the `--rpc-url`
+ `--network-passphrase` flags directly. Source signing is
delegated to the CLI: we pass `--source-account S...` and the CLI
handles tx assembly, simulation, signing, and submission. No
in-process XDR encoding survives.

Trade-offs explicitly accepted:
- **+30-50 MB per platform** when auto-install runs (the `stellar`
  binary). Acceptable for the feature it unlocks; the artist with
  a Rust toolchain installed pays zero (PATH path).
- **Subprocess overhead per call** (~200 ms cold start). Each
  Soroban round-trip on testnet is multiple seconds anyway, so
  amortized cost is invisible.
- **CLI-version drift** if Stellar ships a breaking flag change.
  `PINNED_VERSION` in `StellarCli.hpp` is the recovery knob —
  bump + ship a release.

Long-term migration path: the §10/Q11 follow-up (C-FFI to Rust
`soroban-client`) becomes attractive once we're already pulling
Rust in for `c2pa-rs`. Revisit between I13 and the next release
cycle.

### 8.3 C2PA manifest embed — `c2pa-rs` FFI (preferred) or `c2patool` shell (fallback)

`c2pa-rs` (Apache-2.0, Adobe/CAI) is the reference impl with full
PNG/JPG/WEBP support and a custom-trust-root callback (the seam
§7 needs). No current Conan recipe — build a local recipe under
`conan/c2pa-rs/`, same pattern as `conan/sdl/`, `conan/icu/`,
`conan/conan-skia/`. Fallback if the FFI proves painful under
Conan: shell to a bundled `c2patool` binary. Decision in §10/Q2.

EKU OID `1.3.6.1.4.1.42038.1.5.0` must be **critical** in the CA
cert. Reference shape: `mock_c2pa/andromica/ca_generation.py:160-164`.
Replicated via `X509_add_ext(... critical=1, "1.3.6.1.4.1.42038.1.5.0")`.

---

## 9. Cross-component agreement gates

| Gate | Reference | Validation |
|---|---|---|
| **G1 — Bundle text round-trip.** | `Bundle::to_text()` output must parse cleanly through `heavymeta_collective/payments/cert_registry_bundle.py::parse_bundle`. The Portal IS the consumer; we MUST round-trip. | `tests/test_c2pa_bundle.cpp` produces a register / rotate / revoke bundle for each `AppKind` variant + fingerprint + expiry combo, writes them to fixture files, and the Portal repo's `tests/test_cert_registry_bundle_fixtures.py` parses them. The two test suites share the fixture file as the contract. |
| **G2 — Bundle field set agreement.** | Field names + ISO-8601 vs unix-int semantics must match `heavymeta_collective/payments/cert_registry_bundle.py:107-119`. | Same fixture file as G1. |
| **G3 — `auth_payload` bytes pass through unmodified.** | The Portal signs `mock_c2pa/canonical.py::register_payload` bytes; the contract reconstructs and byte-compares. Desktop NEVER rebuilds. | `WireToken::parse` returns `auth_payload_bytes` and `auth_signature_bytes` as opaque blobs. `SorobanSubmit::submit_register` passes them through to scval `Bytes` unchanged. Test: a wire token captured from a live mint round-trips into the right scval bytes (`tests/test_c2pa_wire_token.cpp` fixture). |
| **G4 — Submit fixture.** | Submit shape matches `mock_c2pa/register.py::submit_register:145-185`. | `tests/test_c2pa_submit.cpp` builds the XDR for a known-good register invocation, then byte-compares the resulting `InvokeContract` XDR to a fixture produced by `mock_c2pa/register.py` with the same inputs. |
| **G5 — End-to-end smoke.** | The Portal-side smoke script `heavymeta_collective/scripts/c2pa_smoke.py` is the integration test we run against. | Pre-merge: run `c2pa_smoke.py --app-kind Inkternity` against a desktop-generated bundle. Desktop emits bundle → human pastes into running local Portal → mint token → paste back into desktop → desktop submits → desktop reads back via `is_trusted`. Full path verified. Documented as a release-gate step in `docs/design/BUILDS.md` (TBD; not in this commit). |

---

## 10. Open questions

Resolved 2026-05-21 by founder:

1. **CA rotation trigger.** **Resolved: manual button only in v1.**
   10-year cert lifetime defaults from `valid_days` in the Python
   reference; auto-rotate is a future enhancement.
2. **C2PA manifest embed library — FFI vs shell.** **Resolved:
   `c2pa-rs` FFI** statically linked via a new Conan recipe under
   `conan/c2pa-rs/`. Same pattern as `conan/sdl/`, `conan/icu/`,
   `conan/conan-skia/`.
3. **Network selection — env var vs config knob.** **Resolved:
   `GlobalConfig::stellarNetwork` config knob with env-var override
   for dev** (`STELLAR_NETWORK` env wins if set).
4. **Portal handoff.** **Resolved: plain URL via `SDL_OpenURL`**
   (`https://heavymeta.art/launch`). No custom protocol handler.
5. **`.inkternity` manifest placement.** **Resolved: sidecar
   `<file>.inkternity.c2pa`** placed next to the source file.
   Embedded-chunk path deferred until `c2pa-rs` ships first-class
   custom-container support.
6. **One leaf per export vs one leaf per session.** **Resolved:
   one leaf per export.** Per §6 rationale (forensic `notBefore`
   per export, sub-50 ms is invisible at human export rates).
7. **SVG export signing.** **Resolved: sidecar `<file>.svg.c2pa`.**
   Verifier handles sidecars uniformly across `.inkternity` + SVG.
8. **Gateway toggle naming.** **Resolved: "Verifiable publishing".**
   Settings header + checkbox + blurb updated in §3.1 and §4.
9. **First-run funding step.** **Resolved: do not block use of the
   app.** Gateway toggle ON reveals wallet panel + walkthrough but
   the artist can keep using Inkternity without funding. The
   funding floor disables only the Submit button.
10. **Existing per-install identity reuse vs glasswing pattern.**
    **Resolved: keep local-only model** (`inkternity_dev_keys.json`,
    existing mnemonic export/restore). Portal-stored-secret restore
    pattern is a separate structural change, not blocked on this
    plan.

11. **Soroban surface in C++.** **Re-resolved 2026-05-21: shell out
    to the `stellar` CLI with auto-install on first need** (§8.2
    revised). Initial resolution was hand-rolled libcurl + scval
    on a "~250 LoC, mirrors register.py" assumption. Implementation
    pass surfaced the actual scope: full TransactionEnvelope XDR,
    transaction simulation + footprint apply, network-passphrase
    signing, sendTransaction polling — closer to 10K LOC of work
    rather than 250. Pivot keeps the desktop submitter realistic for
    v1 while leaving the door open to the C-FFI-to-Rust follow-up
    once `c2pa-rs` lands Rust in the build (revisit between I13 and
    the next release). Auto-install handles the cross-platform
    binary-shipping concern: PATH probe first, then per-OS-user
    cache, then GitHub Releases download. Registry discovery (§9)
    rides the same CLI: one `stellar contract invoke -- get_record`
    view per session, cached.

All §10 questions resolved. Ready for implementation per §12.

---

## 11. Deliberately not in this plan

- **Contract changes.** `hvym-cert-registry` is locked
  (`HVYM_CERT_REGISTRY.md` "Architectural commitments").
- **Portal changes.** Shipped 2026-05-21
  (`heavymeta_collective/C2PA_PORTAL_PLAN.md`). If Inkternity
  integration surfaces a Portal gap, it goes to §10 here, not into
  the Portal repo from this work.
- **Andromica / glasswing desktop integration.** Separate plan
  (`mock_c2pa/andromica/` is the drop-in template for that work;
  this plan parallels it for Inkternity).
- **Pintheon C2PA.** `AppKind::Pintheon` exists on-chain for
  forward-compat; no work here touches it
  (`heavymeta_collective/C2PA.md §5.1`).
- **Standalone "verify-only tool" UX.** A future buyer-side tool
  per Portal-plan F1; out of scope for the desktop client.
- **Cooperative root CA / X.509 hierarchy beyond app-instance.**
  Architecturally rejected (`heavymeta_collective/C2PA.md §1.2`).
- **Emscripten / web build C2PA features.** Gated off in the web
  build for v1; revisit if a web-target user story emerges.
- **Encryption-at-rest for the CA key.** Matches the existing
  app-key on-disk model (`src/DevKeys.cpp:148-149`); upgrading
  both at once is a separate security review.
- **Auto-rotation.** §10/Q1 — manual button only in v1.
- **Per-leaf assertion authoring UI.** `c2pa.created`,
  `c2pa.ai-disclosure: none`, `c2pa.creative_work` are hardcoded
  at the embed call (`ManifestEmbed`) per
  `heavymeta_collective/C2PA.md §4.3`. UI to author additional
  assertions per export is deferred.

---

## 12. Implementation order

| # | Task | Est. | Deps |
|---|---|---|---|
| **I0** | **P0 — fix broken screenshot save** in `world_take_screenshot` (`src/WorldScreenshot.cpp:35`). UI runs end-to-end but no file is written. Root-cause (likely empty `info.filePath`, silent Skia encode failure, or `SDL_SaveFile` swallow), fix the underlying issue, surface user-visible error on the failure path that today just logs `WORLDFATAL`. **Blocker for I14 + export half of I17** — no point wiring a signing hook into a save path that doesn't save. Strictly bug fix, no signing logic. | ½–1 day | — |
| I1 | Add `openssl/3.6.2` to `conanfile.py` (matches the libcurl/8.17.0 transitive choice). Verify cross-platform build (Windows MSVC + macOS clang + Linux gcc). | ½ day | — |
| I2 | `src/C2PA/AppCa.{hpp,cpp}` — generate Ed25519 X.509 with the four extensions. Unit-test by parsing the output with `mock_c2pa/andromica/ca_generation.py::verify_chain` (Python smoke harness against the C++-emitted DER). | 1 day | I1 |
| I3 | `src/C2PA/KeyStore.{hpp,cpp}` — disk persistence, file modes, status JSON. | ½ day | I2 |
| I4 | `src/C2PA/Bundle.{hpp,cpp}` + cross-repo fixtures with `cert_registry_bundle.py`. **Gate G1.** | ½ day | I2 |
| I5 | `src/C2PA/WireToken.{hpp,cpp}` — parse Portal token, cross-check fields, expose opaque auth_payload bytes. **Gate G3 setup.** | ½ day | — |
| I6 | Gateway toggle: add `GlobalConfig::verifiablePublishingEnabled` field + JSON serialize. Wire to `FileSelectScreen::settings_view`. Add toggle UI; everything below it stays hidden when off. | ½ day | — |
| I7 | Wallet panel: balance polling via `FileDownloader` against Horizon `/accounts/{G…}`. Funding-address Copy + QR (reuse `CanvasShareId` Skia QR primitive). | 1 day | I6 |
| I8a | `src/C2PA/StellarCli.{hpp,cpp}` — pinned-version GitHub-Releases auto-install + subprocess invoker. Smoke flag `--c2pa-stellar-probe`. (Pivot from hand-rolled libcurl + scval per §8.2 revised + §10/Q11 re-resolved.) | 1 day | — |
| I8b | `src/C2PA/SorobanSubmit.{hpp,cpp}` — thin wrappers around `stellar contract invoke` for register/rotate/revoke + get_app_ca/is_trusted views. **Gate G4.** | 1 day | I8a |
| I9 | `src/C2PA/Registry.{hpp,cpp}` — `hvym_registry` resolve + fallback. | ½ day | I8 |
| I10 | First-run registration walkthrough UI (steps 1–4 of §3.4) wired through I2 + I4 + I5 + I7 + I8. End-to-end smoke against testnet via `heavymeta_collective/scripts/c2pa_smoke.py`. **Gate G5.** | 2 days | I2, I4, I5, I7, I8 |
| I11 | Rotation + revocation UI + submit paths (§3.5, §3.6). | 1 day | I10 |
| I12 | `src/C2PA/LeafIssuer.{hpp,cpp}` — per-publish leaf with OpenSSL X509_sign. | 1 day | I2 |
| I13 | `c2pa-rs` integration — Conan recipe under `conan/c2pa-rs/`, FFI bindings, `ManifestEmbed::embed_into_image_file` for PNG/JPG/WEBP. Resolve §10/Q2. | 3–5 days | I1 |
| I14 | Publish hook on `world_take_screenshot` (`src/WorldScreenshot.cpp:84`) for PNG/JPG/WEBP. Sidecar `.c2pa` for SVG per §10/Q7. | 1 day | **I0**, I12, I13 |
| I15 | Publish hook on `World::save_to_file` (`src/World.cpp:735`). Sidecar `<file>.c2pa` per §10/Q5. | 1 day | I12, I13 |
| I16 | Verifier wrapper (`src/C2PA/Verifier.{hpp,cpp}`) + import-side badges on `.inkternity` open + dropped-image add + file-select preview thumbnails. Threading per §7. | 2 days | I8, I13 |
| I17 | End-to-end manual QA pass on testnet: register Inkternity install → publish a canvas + export PNG → re-open both with a fresh install → verify badge says Trusted; then rotate → re-export → re-open → badge says Trusted with new keys; then revoke → re-open earlier exports → badge says Revoked. | 1 day | All |

**Total: ~19.5–22 days focused** (I0 included). Tasks I0–I11 are testable against
the deployed testnet contract + `c2pa_smoke.py` and require no new
test infra. I12–I17 need the new `c2pa-rs` Conan recipe (I13) before
they're independently runnable; until then, I12 is unit-testable
against `mock_c2pa/andromica/ca_generation.py::verify_chain` as a
Python cross-checker.

---

## 13. Patterns to mirror

| Need | Look at |
|---|---|
| First-run keypair gen + idempotent persistence | `src/DevKeys.cpp:112-163` |
| Sorted-keys JSON + b64url ed25519 wire token parse | `src/Subscription/TokenVerifier.cpp:142-176` |
| Stellar strkey encode / decode + ed25519 seed→pubkey | `src/crypto/stellar/Stellar.hpp` |
| Memzero discipline on secret bytes | `src/DevKeys.cpp:80-82` |
| Async network IO with status polling | `include/Helpers/FileDownloader.hpp` |
| Atomic file write (tmp + rename) | `src/World.cpp:726-748` |
| Skia encode pipeline for the embed hook | `src/WorldScreenshot.cpp:60-86` |
| Settings-view Clay-based card layout | `src/Screens/FileSelectScreen.cpp:209-318` |
| Disclosure-style on-demand crypto surface (matches §B constraint) | `src/Screens/FileSelectScreen.cpp:241-274` |
| Conan-vendored external lib under `conan/` | `conan/sdl/`, `conan/icu/`, `conan/conan-skia/` |
| Python cross-checker against C++ output | `tests/test_inkternity_tokens.py` (Portal repo) is the pattern; mirror for `tests/test_c2pa_bundle.cpp` + Python fixture |

---

## 14. Files to read first (in order)

1. `heavymeta_collective/C2PA_PORTAL_PLAN.md` — calibrated voice + decisions.
2. `hvym-market-muscle/C2PA.md` §3, §4.2, §4.3, §4.4, §6 — architecture + member UX target + verifier sketch.
3. `pintheon_contracts/HVYM_CERT_REGISTRY.md` — contract surface + locked canonical payload.
4. `pintheon_contracts/mock_c2pa/andromica/ca_generation.py` — the X.509 we re-implement in C++.
5. `pintheon_contracts/mock_c2pa/andromica/bundle.py` — exact text format we emit.
6. `pintheon_contracts/mock_c2pa/register.py` — Soroban submit reference.
7. `pintheon_contracts/mock_c2pa/canonical.py` — locked payload encoding (pass-through, not re-built).
8. `heavymeta_collective/payments/cert_registry_bundle.py` — Portal parser our bundle round-trips through.
9. `heavymeta_collective/scripts/c2pa_smoke.py` — integration-test target.
10. `src/DevKeys.{hpp,cpp}` — the existing keypair story we extend.
11. `src/Subscription/TokenVerifier.cpp` — the existing wire-token shape we mirror.
12. `src/crypto/stellar/Stellar.hpp` — primitives.
13. `src/World.cpp:664-790` — save path the `.inkternity` signing hook slots into.
14. `src/WorldScreenshot.cpp` — export path the image signing hook slots into.
15. `src/Screens/FileSelectScreen.cpp:209-436` — settings panel + existing keypair UX precedent.
