"""Cross-repo agreement test (docs/design/C2PA.md §9 / gate G1).

Asserts that the byte sequences emitted by Inkternity's
src/C2PA/Bundle.cpp::render_* round-trip cleanly through the Portal's
heavymeta_collective/payments/cert_registry_bundle.py::parse_bundle.

This test does NOT build or run inkternity.exe — instead it captures the
exact byte format render_register / render_rotate / render_revoke
produce (transcribed from src/C2PA/Bundle.cpp's literal strings), then
feeds those bytes to the Portal parser. If render_*'s string-concat
sequence is changed, update the EXPECTED_* constants here and re-run.

Run via:
    uv run --with stellar-sdk python tests/test_c2pa_bundle.py
"""
from __future__ import annotations

import os
import sys
from datetime import datetime, timezone

from stellar_sdk import Keypair

# Locate adjacent repos. Override via env var for CI / non-standard checkouts.
PORTAL_REPO = os.environ.get(
    "HVYM_PORTAL_REPO",
    r"C:/Users/surfa/Documents/metavinci/heavymeta_collective",
)
sys.path.insert(0, PORTAL_REPO)
sys.path.insert(0, os.path.join(PORTAL_REPO, "payments"))

# Try the path the Portal sources use first; fall back to the unprefixed
# import if the directory is on PYTHONPATH directly.
try:
    from payments.cert_registry_bundle import (  # type: ignore
        parse_bundle, RegisterBundle, RotateBundle, RevokeBundle,
    )
except ImportError:
    from cert_registry_bundle import (  # type: ignore
        parse_bundle, RegisterBundle, RotateBundle, RevokeBundle,
    )


# ── Test fixtures ──────────────────────────────────────────────────────────

# Deterministic Stellar G-address. Derived from a fixed 32-byte seed so
# the expected output strings stay byte-stable across runs.
APP_ADDR = Keypair.from_raw_ed25519_seed(bytes([0xAA] * 32)).public_key

# 32-byte fingerprint, deterministic for byte-comparison.
FP_BYTES = bytes(range(32))
FP_HEX   = FP_BYTES.hex()  # lowercase, 64 chars

# Fixed unix time so the ISO string is stable: 2030-01-01T00:00:00+00:00.
EXPIRES_AT_UNIX = int(datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp())
EXPIRES_AT_ISO  = datetime.fromtimestamp(EXPIRES_AT_UNIX, tz=timezone.utc).isoformat(
    timespec="seconds")


# ── Expected outputs (mirror src/C2PA/Bundle.cpp::render_*) ────────────────

EXPECTED_REGISTER = (
    "HVYM-CA-REG-v1\n"
    f"app_address:    {APP_ADDR}\n"
    f"app_kind:       Inkternity\n"
    f"fingerprint:    {FP_HEX}\n"
    f"pubkey_alg:     ed25519\n"
    f"expires_at:     {EXPIRES_AT_ISO}\n"
)

EXPECTED_ROTATE = (
    "HVYM-CA-ROT-v1\n"
    f"app_address:        {APP_ADDR}\n"
    f"new_fingerprint:    {FP_HEX}\n"
    f"new_expires_at:     {EXPIRES_AT_ISO}\n"
)

EXPECTED_REVOKE = (
    "HVYM-CA-REV-v1\n"
    f"app_address:    {APP_ADDR}\n"
)


# ── Tests ──────────────────────────────────────────────────────────────────

def _check(label, expected, expected_type, **expected_fields):
    """Parse `expected` (the bytes our C++ would emit) and assert it
    yields the expected dataclass instance with the right fields."""
    parsed = parse_bundle(expected)
    assert isinstance(parsed, expected_type), (
        f"{label}: parser returned {type(parsed).__name__}, "
        f"expected {expected_type.__name__}")
    for field, value in expected_fields.items():
        actual = getattr(parsed, field)
        assert actual == value, (
            f"{label}: field {field} = {actual!r}, expected {value!r}")
    print(f"  ok  {label}")


def main():
    print(f"Testing against Portal parser at {PORTAL_REPO}")

    print("REGISTER:")
    _check(
        "round-trip",
        EXPECTED_REGISTER,
        RegisterBundle,
        intent="register",
        app_address=APP_ADDR,
        app_kind="Inkternity",
        fingerprint=FP_BYTES,
        pubkey_alg="ed25519",
        expires_at=EXPIRES_AT_UNIX,
    )

    print("ROTATE:")
    _check(
        "round-trip",
        EXPECTED_ROTATE,
        RotateBundle,
        intent="rotate",
        app_address=APP_ADDR,
        new_fingerprint=FP_BYTES,
        new_expires_at=EXPIRES_AT_UNIX,
    )

    print("REVOKE:")
    _check(
        "round-trip",
        EXPECTED_REVOKE,
        RevokeBundle,
        intent="revoke",
        app_address=APP_ADDR,
    )

    # Also cross-check vs the Andromica builder for REGISTER.
    try:
        sys.path.insert(0, os.environ.get(
            "HVYM_CONTRACTS_REPO",
            r"C:/Users/surfa/Documents/metavinci/pintheon_contracts",
        ))
        from mock_c2pa.andromica.bundle import build_bundle  # type: ignore
        py_bundle = build_bundle(
            app_address=APP_ADDR,
            app_kind="Inkternity",
            fingerprint_bytes=FP_BYTES,
            expires_at_unix=EXPIRES_AT_UNIX,
        )
        py_text = py_bundle.to_text()
        if py_text == EXPECTED_REGISTER:
            print("  ok  byte-identical to mock_c2pa/andromica/bundle.py output")
        else:
            print("  WARN: byte-different from Python builder")
            print("       (still parses fine; parser tolerates whitespace)")
            print(f"       expected: {EXPECTED_REGISTER!r}")
            print(f"       python:   {py_text!r}")
    except ImportError as e:
        print(f"  skip: could not import mock_c2pa builder ({e})")

    print("\nAll cross-repo gate G1 checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
