# PinPointStudio — PPCP conformance claim

**What this implementation claims against `PPCP-CORE` 1.0, its companion documents, and `PPCP-RV` 1.0 — and the command that reproduces each claim.**

| | |
|---|---|
| Implementation | PinPointStudio, `role: host` |
| Against | `PPCP-CORE` revision 9, `PPCP-MSG`, `PPCP-ENC`, `PPCP-CONF` 1.0; `PPCP-RV` revision 8 |
| Companion | [`libppcp/docs/conformance/matrix.md`](../../libppcp/docs/conformance/matrix.md) — the programme-wide record this file feeds |
| Status | **In progress.** Session S1 of the implementation programme: work packages H0 and H1 |
| Date | 22 August 2026 |

---

## 1. Profile set

`PPCP-CORE` §2.2.3. PinPointStudio declares seven of the eight profiles:

| Profile | Claimed | What it confers here |
|---|---|---|
| **Core** | yes | Connection, declaration, session, streams, sync, liveness |
| **Capture** | yes | Receives Captures and their payloads; produces them from host cameras |
| **Detect** | yes | Nominates Candidates from the acoustic and IMU shot detectors on host-owned Sources |
| **Arbitrate** | yes | Correlates Candidates into Shots and issues them — the profile that makes this peer a host |
| **Live** | yes | The live socket path |
| **Offline** | yes | Bundle import as a file transport (H3), reconciliation |
| **Markup** | yes | Annotations both directions |
| **Mint** | **no** | Shots with `authority: device` are parsed and never originated |

**Negative conformance** (`PPCP-CONF` §1d) is claimed as strongly as the positive:

- A `shot` with `authority: device` is parsed, honoured and **never originated** — this peer has no Mint profile.
- `session_link` is **never originated** by any implementation in this programme while `SessionLink` remains provisional (`PPCP-CORE` Annex B2).
- Every message of `PPCP-MSG` §11 decodes, including those of profiles not claimed (C1).

`PPCP-RV` conformance is claimed separately: the pairing-code path (REQUIRED of any RV implementation, RV 2a) plus service discovery in the **browser** role only (RV 3.5b). Network join (RV §6) is not provided by the host — it is the publisher-side credential and the joining peer is the device (`n/a` for RT-13).

---

## 2. Where the thresholds are

`PPCP-CORE` I14: the protocol carries no threshold, and neither does `libppcp`. Every acceptance decision this host makes — the 120 fps ingest floor, the promotion policy, what uncertainty is good enough — lives in **application code in this repository**, supplied to the library as a callback. That is a conformance property, not an implementation detail, and it is why `CT-I14` is a grep.

Nothing in `src/Ppcp/` carries a protocol constant. The only numbers in the transport are socket buffer sizes and a handshake deadline, which are this transport's and which `PPCP-CORE` §3.2 declines to constrain at all.

---

## 3. Reproducing this claim

Everything below runs on loopback with no device, no pairing code and no network:

```
cmake -S src/Ppcp/tests -B build/ppcp-tests
cmake --build build/ppcp-tests -j
ctest --test-dir build/ppcp-tests --output-on-failure
```

Requires OpenSSL ≥ 1.1.1 development headers (`brew install openssl@3`, `apt install libssl-dev`, or `vcpkg install openssl`). The suite is Qt-free and app-free by design so that `ppcp-conform` (L14) can drive the same code headless in H8.

The `K_tls` these tests use is the `PPCP-RV` §10.1 vector, hardcoded **in the test file only** until `libppcp`'s L12 derivation API lands. No key is hardcoded in shipping code, and none ever will be.

---

## 4. PPCP-RV tests — `RV` §9

Row format is that of [`matrix.md` §5](../../libppcp/docs/conformance/matrix.md#5-ppcp-rv-tests--rv-9). Only the **PinPointStudio** column is asserted here; the other two are reproduced from the matrix as they stood at the end of S1 and are their own teams' to move.

| Test | Method | Asserts | Work packages | `libppcp` | PinPointStudio | PinPointCapture |
|---|---|---|---|---|---|---|
| RT-4 | injected | strongest mode negotiated, never plaintext, outcome surfaced | H1, D1 | n/a | `impl` | — |
| RT-10 | injected | `session_resume` refused without a completed handshake | H1, D1 | n/a | `impl` | — |
| RT-11 | injected | unknown identity and wrong key indistinguishable | H1 | n/a | `pass — ctest --test-dir build/ppcp-tests -R ppcp_transport_test` | n/a |
| RT-14 | static | §10.2 PSK identity; differs per connection; empty hint at TLS 1.2 | L12, H1, D1 | — | `pass — ctest --test-dir build/ppcp-tests -R ppcp_transport_test` (wire half) | — |
| RT-17 | **review** | every platform mode offered, from a capability query | H1, D1 | n/a | `review — src/Ppcp/ppcp_transport.cpp, queryTlsCapabilities() and makeContext(), commit 0496c2f; reviewer unassigned` | — |

### Why RT-4 is `impl` and not `pass`

Three of RT-4's four assertions pass in the suite above and are named in the test file:

- **no handshake is unencrypted** (5.2f) — structural: `Connector::connect` has one return path and it produces a TLS channel or nothing. `HandshakeFailsWithMismatchedKtlsAndNeverFallsBack`.
- **the negotiated result is the strongest the pair can reach, where one end cannot reach 1.3** (5.2b1) — `EmptyPskIdentityHintAndAnHonestForwardSecrecyReport` drives an instrumented TLS 1.2-only counterpart, the shape `RV` 5.4b1 measured on the real device, and the host meets it at TLS 1.2 `0x00A8` rather than failing.
- **the outcome is surfaced** (5.4k) — asserted on both ends of every handshake test, and asserted to be free of the key and the identity (7.2b).

The fourth — **`psk_ke` is refused where both peers reach TLS 1.3** (5.2b) — is implemented twice over (OpenSSL refuses a no-DHE key exchange unless `SSL_OP_ALLOW_NO_DHE_KEX` is set, which this code deliberately does not set; and the mode is re-checked after the handshake so that a future defaults change fails the connection rather than weakening it). It is **not yet demonstrated**, because demonstrating it needs a counterpart that will *offer* `psk_ke`, and OpenSSL's client cannot be made to: it always sends a `key_share`. `RV` 5.2i is explicit that this class of assertion is settled by an instrumented counterpart or a wire capture and never by an API assertion, so the row moves when `ppcp-sim` (L13) can offer the mode, or when a capture is taken. Until then `impl` is the honest cell.

### Why RT-10 is `impl`

The transport half holds and is tested: no channel object exists until its handshake completed, so there is no path by which any byte — `session_resume` included — reaches the peer engine on an unauthenticated connection (`NoChannelExistsUntilTheHandshakeCompletes`). The message half of 7.5b — *refused ... and only for the `sid` bound to it* — cannot be asserted until something can recognise a `session_resume`, which is H2.

### What RT-14 covers here

The **wire half**: the §10.2 identity crosses as 17 raw octets with no transcoding, validation as text or truncation (5.3f — the resolver compares byte-for-byte and the vector is not valid UTF-8), and where TLS 1.2 is negotiated the server sends an **empty** `psk_identity_hint` (5.2h property 3), observed in the ServerKeyExchange body by the instrumented counterpart rather than asserted through an API (5.2i).

The **static half** — the vectors reproducing byte-for-byte, and the identity differing across connections — belongs to `libppcp` L12, which generates it. The host does not synthesise identities.

---

## 5. Core, encoding and message tests

Not yet claimed. `CT-*` rows open at H2 and later; this file gains them as the packages land, together with the `ppcp-conform` invocation of H8 that produces them from a command rather than by hand.

---

## 6. Findings raised against the specification

Recorded here because they were found while building this, and dispositioned in `libppcp` (plan ground rule 3 — the specification changes first, and the change history grows).

| # | Clause | Finding |
|---|---|---|
| 1 | `CORE` §3, `ENC` §2 | With one channel per TCP connection (plan A6), nothing says **how a listener associates the connections of one peer**, nor which of them is channel 0. Two implementations will resolve it two ways and never meet. This repository groups by the pairing the PSK identity resolved to (RV 5.3b) and orders by serialising the dialler's handshakes, which needs no bytes on the wire — but it is a local convention where a clause is wanted. |
| 2 | `RV` 5.3f | *"A peer MUST NOT transcode, validate as text, or truncate an identity"* is **not achievable on the TLS 1.2 path through OpenSSL**: both its PSK callbacks pass the identity as `char *` and take its length with `strlen`. The 16 CSPRNG bytes of 5.3a carry an embedded `0x00` about one connection in sixteen, and that connection fails — intermittently, which is the worst shape available. 5.4b1 makes TLS 1.2 the *ordinary* path, not an edge case. Either 5.3a excludes `0x00` from `rn2`, or 5.3f acknowledges the limit. Characterised by a test (`IdentityWithAnEmbeddedNulCannotSurviveTheTls12Path`) rather than worked around: truncating on purpose would be the transcoding the clause forbids. |
