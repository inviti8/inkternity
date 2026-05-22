"""Cross-repo agreement test (docs/design/C2PA.md §9 / gate G3).

Confirms that a wire token minted by the Portal's
heavymeta_collective/payments/cert_registry_canonical.register_payload
(the same code path Portal /launch uses) decodes to the exact same
{app_address, app_kind, fingerprint, expires_at, member_pubkey, nonce}
that src/C2PA/WireToken.cpp::parse_register would extract.

We can't run inkternity.exe headlessly here, so we replicate parse_register
step-by-step in Python — the assertion is that BOTH parser implementations
extract the same fields from the same wire bytes. If WireToken.cpp's
decoding logic changes, mirror the change here.

Run via:
    uv run --with stellar-sdk python tests/test_c2pa_wire_token.py
"""
from __future__ import annotations

import base64
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

from stellar_sdk import Keypair

PORTAL_REPO = os.environ.get(
    "HVYM_PORTAL_REPO",
    r"C:/Users/surfa/Documents/metavinci/heavymeta_collective",
)
sys.path.insert(0, PORTAL_REPO)
sys.path.insert(0, os.path.join(PORTAL_REPO, "payments"))
try:
    from payments.cert_registry_canonical import register_payload, rotate_payload, revoke_payload  # type: ignore
except ImportError:
    from cert_registry_canonical import register_payload, rotate_payload, revoke_payload  # type: ignore


def _b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode("ascii")


def _b64u_decode(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def _hex_decode_32(s: str) -> bytes:
    """Mirrors src/C2PA/WireToken.cpp::hex_decode_32 — strict 64 lowercase chars."""
    if len(s) != 64:
        raise ValueError(f"hex must be 64 chars, got {len(s)}")
    for c in s:
        if c not in "0123456789abcdef":
            raise ValueError(f"hex must be lowercase, got {c!r}")
    return bytes.fromhex(s)


def parse_register_token_like_cpp(token: str) -> dict:
    """Reproduce src/C2PA/WireToken.cpp::parse_register on a Python side.
    Returns a dict with the same fields the C++ struct populates."""
    sig_b64, payload_b64 = token.split(".", 1)
    sig = _b64u_decode(sig_b64)
    payload = _b64u_decode(payload_b64)
    if len(sig) != 64:
        raise AssertionError(f"signature must be 64 bytes, got {len(sig)}")
    j = json.loads(payload)
    if j.get("i") != "register-app-ca":
        raise AssertionError(f"wrong intent: {j.get('i')!r}")
    fp = _hex_decode_32(j["fp"])
    mp = _hex_decode_32(j["m"])
    return {
        "app_address": j["a"],
        "app_kind": j["k"],
        "fingerprint": fp,
        "expires_at_unix": j["exp"],
        "member_pubkey": mp,
        "nonce": j["n"],
        "auth_payload": payload,
        "auth_signature": sig,
    }


def main():
    print(f"Testing against Portal canonical payload at {PORTAL_REPO}")

    # Deterministic test fixtures.
    member_kp = Keypair.from_raw_ed25519_seed(bytes([0xBB] * 32))
    app_kp    = Keypair.from_raw_ed25519_seed(bytes([0xAA] * 32))
    fp_bytes  = bytes(range(32))
    expires   = int(datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp())
    nonce     = 1_234_567

    # ── REGISTER ───────────────────────────────────────────────────────────
    payload = register_payload(
        app_address=app_kp.public_key,
        member_pubkey=member_kp.raw_public_key(),
        app_kind="Inkternity",
        fingerprint=fp_bytes,
        expires_at=expires,
        nonce=nonce,
    )
    sig = member_kp.sign(payload)
    token = f"{_b64u(sig)}.{_b64u(payload)}"

    parsed = parse_register_token_like_cpp(token)
    assert parsed["app_address"]     == app_kp.public_key
    assert parsed["app_kind"]        == "Inkternity"
    assert parsed["fingerprint"]     == fp_bytes
    assert parsed["expires_at_unix"] == expires
    assert parsed["member_pubkey"]   == member_kp.raw_public_key()
    assert parsed["nonce"]           == nonce
    # G3: opaque pass-through bytes must equal what the Portal signed.
    assert parsed["auth_payload"]    == payload
    assert parsed["auth_signature"]  == sig
    print("  ok  REGISTER: parse_register fields + G3 pass-through bytes")

    # Negative test: wire shape with no dot is unparseable.
    try:
        parse_register_token_like_cpp("not-a-dot-separated-token")
    except (AssertionError, ValueError, IndexError):
        print("  ok  REGISTER: no-dot wire rejected")
    else:
        raise AssertionError("expected no-dot wire to be rejected")

    # Negative test: wrong-intent token rejected.
    rev_payload = revoke_payload(app_address=app_kp.public_key, nonce=nonce)
    rev_sig = member_kp.sign(rev_payload)
    rev_token = f"{_b64u(rev_sig)}.{_b64u(rev_payload)}"
    try:
        parse_register_token_like_cpp(rev_token)
    except AssertionError:
        print("  ok  REGISTER: rotate/revoke token rejected when register expected")
    else:
        raise AssertionError("expected wrong-intent rejection")

    # Note: signature tampering is NOT a WireToken-layer check — the
    # contract verifies byte-for-byte against the reconstructed canonical
    # payload, and the desktop's job is to pass auth_payload +
    # auth_signature through unchanged (gate G3).

    # ── ROTATE ─────────────────────────────────────────────────────────────
    payload = rotate_payload(
        app_address=app_kp.public_key,
        new_fingerprint=fp_bytes,
        new_expires_at=expires,
        nonce=nonce,
    )
    sig = member_kp.sign(payload)
    token = f"{_b64u(sig)}.{_b64u(payload)}"
    sig_b64, payload_b64 = token.split(".", 1)
    j = json.loads(_b64u_decode(payload_b64))
    assert j["i"] == "rotate-app-ca"
    assert j["a"] == app_kp.public_key
    assert _hex_decode_32(j["fp"]) == fp_bytes
    assert j["exp"] == expires
    assert j["n"]   == nonce
    print("  ok  ROTATE: payload fields match")

    # ── REVOKE ─────────────────────────────────────────────────────────────
    payload = revoke_payload(app_address=app_kp.public_key, nonce=nonce)
    sig = member_kp.sign(payload)
    token = f"{_b64u(sig)}.{_b64u(payload)}"
    sig_b64, payload_b64 = token.split(".", 1)
    j = json.loads(_b64u_decode(payload_b64))
    assert j["i"] == "revoke-app-ca"
    assert j["a"] == app_kp.public_key
    assert j["n"] == nonce
    print("  ok  REVOKE: payload fields match")

    print("\nAll cross-repo gate G3 checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
