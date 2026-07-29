#!/usr/bin/env python3
"""Check that a Sparkle EdDSA private key matches the public key pinned in the app.

    tools/check_eddsa_pin.py ~/Documents/certs/pinpoint_release_mac_eddsa_PRIVATE.pem

Why this exists: the obvious check — sign a file with the key, then verify the
signature with the same key — proves only that the key is self-consistent. It
succeeds for ANY well-formed key, including the wrong one. What actually matters is
whether the key's public half equals src/Resources/keys/pinpoint_release_mac_eddsa.pub,
which CMake bakes into every bundle as SUPublicEDKey. A mismatch is rejected by every
installed client, and you would only discover it at the update-offer stage.

Ed25519 public-key derivation is inlined (the reference implementation) so this runs on
a stock macOS python3 with no pip install — `cryptography` is not present by default.
The private key is never printed.
"""
import base64
import hashlib
import pathlib
import sys

q = 2**255 - 19


def _inv(x):
    return pow(x, q - 2, q)


d = -121665 * _inv(121666) % q
I = pow(2, (q - 1) // 4, q)


def _xrecover(y):
    xx = (y * y - 1) * _inv(d * y * y + 1)
    x = pow(xx, (q + 3) // 8, q)
    if (x * x - xx) % q != 0:
        x = (x * I) % q
    if x % 2 != 0:
        x = q - x
    return x


_By = 4 * _inv(5)
B = [_xrecover(_By) % q, _By % q]


def _edwards(P, Q):
    x1, y1 = P
    x2, y2 = Q
    x3 = (x1 * y2 + x2 * y1) * _inv(1 + d * x1 * x2 * y1 * y2)
    y3 = (y1 * y2 + x1 * x2) * _inv(1 - d * x1 * x2 * y1 * y2)
    return [x3 % q, y3 % q]


def _scalarmult(P, e):
    if e == 0:
        return [0, 1]
    Q = _scalarmult(P, e // 2)
    Q = _edwards(Q, Q)
    if e & 1:
        Q = _edwards(Q, P)
    return Q


def _encodepoint(P):
    x, y = P
    bits = [(y >> i) & 1 for i in range(255)] + [x & 1]
    return bytes(sum(bits[i * 8 + j] << j for j in range(8)) for i in range(32))


def public_key(seed: bytes) -> bytes:
    h = hashlib.sha512(seed).digest()
    a = 2**254 + sum(2**i * ((h[i // 8] >> (i % 8)) & 1) for i in range(3, 254))
    return _encodepoint(_scalarmult(B, a))


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2

    key_path = pathlib.Path(sys.argv[1]).expanduser()
    if not key_path.is_file():
        print(f"❌ private key not found: {key_path}", file=sys.stderr)
        return 2

    repo = pathlib.Path(__file__).resolve().parent.parent
    pub_path = repo / "src/Resources/keys/pinpoint_release_mac_eddsa.pub"
    if not pub_path.is_file():
        print(f"❌ pinned public key not found: {pub_path}", file=sys.stderr)
        return 2

    # Sparkle exports the raw 32-byte seed, base64-encoded. Older exports carry the
    # 64-byte secret (seed || public); the seed is the leading 32 bytes either way.
    secret = base64.b64decode(key_path.read_bytes().strip())
    if len(secret) not in (32, 64):
        print(f"❌ unexpected key length: {len(secret)} bytes (want 32 or 64)", file=sys.stderr)
        return 2

    derived = base64.b64encode(public_key(secret[:32])).decode()
    pinned = pub_path.read_text().strip()

    print(f"derived : {derived}")
    print(f"pinned  : {pinned}")
    if derived == pinned:
        print("✅ MATCH — this key signs updates the shipped apps will accept.")
        return 0
    print("❌ MISMATCH — signatures from this key are rejected by every installed client.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
