# PinPointStudio — PPCP conformance claim

**What this implementation claims against `PPCP-CORE` 1.0, its companion documents, and `PPCP-RV` 1.0 — and the command that reproduces each claim.**

| | |
|---|---|
| Implementation | PinPointStudio, `role: host` |
| Against | `PPCP-CORE` revision 9, `PPCP-MSG`, `PPCP-ENC`, `PPCP-CONF` 1.0; `PPCP-RV` revision 8 |
| Companion | [`libppcp/docs/conformance/matrix.md`](../../libppcp/docs/conformance/matrix.md) — the programme-wide record this file feeds |
| Status | **In progress.** Sessions S1–S3: work packages H0, H1, H2, H3 and H4 |
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
cmake -S src/Ppcp/tests -B build/ppcp-tests -G Ninja
cmake --build build/ppcp-tests -j3
ctest --test-dir build/ppcp-tests --output-on-failure
```

⚠ **`ppcp_bundle_import_test` deliberately links neither OpenSSL nor the socket
transport.** Plan A10 claims a bundle is a *file transport for the same engine*;
a suite that had to link the TLS layer to read a file would have disproved that
at the link line. Its target sits above the `pp_find_openssl()` gate for the
same reason — the bundle rows must run on a box with no TLS at all.

The rest requires OpenSSL ≥ 1.1.1 development headers (`brew install openssl@3`, `apt install libssl-dev`, or `vcpkg install openssl`). The suite is Qt-free and app-free by design so that `ppcp-conform` (L14) can drive the same code headless in H8.

The same suite runs clean under AddressSanitizer and UndefinedBehaviorSanitizer, which matters more here than in most of this repository: the transport owns raw sockets, OpenSSL objects with callback-transferred ownership, and two threads.

```
cmake -S src/Ppcp/tests -B build/ppcp-asan -DPP_SANITIZE="address;undefined"
cmake --build build/ppcp-asan -j && ctest --test-dir build/ppcp-asan --output-on-failure
```

The `K_tls` these tests use is the `PPCP-RV` §10.1 vector, hardcoded **in the test file only**. No key is hardcoded in shipping code and none ever will be: L12's derivation landed in `libppcp` during this same session (`ppcp_rv_derive`, `ppcp_rv_psk_identity` and `ppcp_rv_resolve_psk_identity` in `include/ppcp/rv.h`) and H6 binds the transport's key and resolver to it. The vector stays in the test regardless — a test that derived its key with the same library it is testing would assert only that the library agrees with itself.

---

## 4. PPCP-RV tests — `RV` §9

Row format is that of [`matrix.md` §5](../../libppcp/docs/conformance/matrix.md#5-ppcp-rv-tests--rv-9). Only the **PinPointStudio** column is asserted here; the other two are reproduced from the matrix as they stood at the end of S1 and are their own teams' to move.

| Test | Method | Asserts | Work packages | `libppcp` | PinPointStudio | PinPointCapture |
|---|---|---|---|---|---|---|
| RT-4 | injected | strongest mode negotiated, never plaintext, outcome surfaced | H1, D1 | n/a | `impl` | — |
| RT-5 | paired | a second handshake with a `mu: 1` code is refused | H6, D1 | n/a | `pass — ctest --test-dir build/ppcp-tests -R ppcp_rendezvous_test` | — |
| RT-6 | injected | an expired code is reported as expired, no connection attempted | H6, D1 | n/a | `n/a — this host PUBLISHES codes and does not scan them` | — |
| RT-7 | paired | TXT carries no `Peer.id`, device name or session count | H6, D1 | n/a | `pass (browser half) — ctest --test-dir build/ppcp-tests -R ppcp_rendezvous_test` | — |
| RT-8 | paired | `rid` changes across re-registration, resolves under one `K_id` only | L12, H6 | — | `pass — ctest --test-dir build/ppcp-tests -R ppcp_rendezvous_test` | — |
| RT-9 | paired | a diagnostic export right after a pairing carries no secret and no payload | H6 | n/a | `pass — ctest --test-dir build/ppcp-tests -R ppcp_rendezvous_test` | — |
| RT-10 | injected | `session_resume` refused without a completed handshake | H1, D1 | n/a | `impl` | — |
| RT-11 | injected | unknown identity and wrong key indistinguishable | H1 | n/a | `pass — ctest --test-dir build/ppcp-tests -R ppcp_transport_test` | n/a |
| RT-12 | **review** | secrets from a platform CSPRNG at full width, protected storage, erased | H6 | n/a | `review — src/Ppcp/ppcp_rendezvous.cpp csprngBytes() and its four call sites; src/Ppcp/ppcp_pairing_store.cpp; commit 6b9b1af; reviewer unassigned` | — |
| RT-13 | **review** | a network join obtains consent for the specific network | — | n/a | `n/a — this host publishes no wifi block and joins no network` | — |
| RT-14 | static | §10.2 PSK identity; differs per connection; empty hint at TLS 1.2 | L12, H1, D1 | — | `pass — ctest --test-dir build/ppcp-tests -R ppcp_transport_test` (wire half) | — |
| RT-15 | paired | a publisher refuses a handshake past `exp`; a bad clock attempts | H6, D1 | n/a | `pass (publisher half) — ctest --test-dir build/ppcp-tests -R ppcp_rendezvous_test` | — |
| RT-16 | **review** | no `PRK` from a `mu > 1` code is persisted | H6 | n/a | `pass — ctest --test-dir build/ppcp-tests -R ppcp_rendezvous_test` (raised from `review`; see below) | — |
| RT-17 | **review** | every platform mode offered, from a capability query | H1, D1 | n/a | `review — src/Ppcp/ppcp_transport.cpp, queryTlsCapabilities() and makeContext(), commit 0496c2f; reviewer unassigned` | — |

### What H6 landed, and where the halves fall

This host is the **code publisher** (`RV` §2): it displays a code, it listens,
and the peer that scans dials it. Three things follow at once — it is the TLS
server (5.2g), it sends `hello_accept` rather than `hello` (2d), and it holds
the authoritative clock for expiry (7.3e). Every RV row above that this column
can move is moved by that role, and the rows it cannot are marked with which
half is missing rather than left blank:

- **RT-6 is `n/a` and not `impl`.** 4.4a is an obligation on the peer that
  SCANS a code, and nothing in this application scans one. The rendezvous panel
  displays; PinPointCapture scans. Adding a scanner to claim the row would be
  building a feature to satisfy a test.
- **RT-15's publisher half passes; its 4.4a1 half is the scanner's**, for the
  same reason.
- **RT-7's browser half passes.** The advertiser half needs a peer that
  advertises, and this host **does not advertise at all** — 3.5b puts the
  responder on the capture peer, and the browser here never registers a service
  and never binds 5353. What is asserted is that the dial decision reads only
  `pv`, `rn` and `rid`, that an instance name which does not derive from `rid`
  is refused (3.2a), and that an unresolvable `rid` is **not dialled** (3.4c).
- **RT-16 moves from `review` to `pass`.** 7.4f is a refusal with an observable
  outcome — `persist()` returns false and the store stays empty — so it does not
  have to be read in the code, and a row that can be tested should not be a
  review row. The predicate is `ppcp_rv_may_persist()` and this host does not
  second-guess it.
- **RT-12 stays `review`, and it is the one that matters.** Entropy quality and
  storage protection produce no observable difference on the wire. What the
  suite can show — 64 codes with no repeated `K_tls`, `K_id`, `Session.id` or
  URI — catches a secret minted once and reused, and would not catch a
  generator with a long period. The reviewer reads `csprngBytes()` (getentropy,
  full width, no fallback), its four call sites, and the keychain store.

### The QR encoder, and why there is one

`RV` 4.1d wants the code rendered at error correction level M or higher, and
this repository has no QR code anywhere: no qrencode, no ZXing, no QZXing,
nothing vendored. The choice was a new third-party dependency in an application
that already fetches ten, or ~450 lines of arithmetic that ISO/IEC 18004 fixed
in 2006. `src/Ppcp/ppcp_qr.cpp` is byte mode, level M, versions 1–20.

Verified four independent ways, because there is no decoder here to diff
against and "it produced a picture" is not evidence:

| | What | Why it cannot be faked |
|---|---|---|
| 1 | Every block's data+EC codewords evaluate to zero at `alpha^0..alpha^(n-1)` | That is the definition of a Reed-Solomon codeword; it depends on no table in the file |
| 2 | The eight level-M format strings and the version-7 and version-10 version strings, as literals | Published constants, so a BCH encoder cannot agree with itself |
| 3 | Symbol size, three finder patterns, both timing patterns, the dark module, and the block tables' internal consistency | — |
| 4 | The finished symbol is read back out — mask undone, zigzag walked, blocks de-interleaved — for 19 payload lengths | Catches the interleave, padding and placement bugs the first three miss |

Row 3 earned itself immediately: reserving the format-information area was
blanking two timing modules, which no scanner would have read and no eye would
have seen.

`ctest --test-dir build/ppcp-tests -R ppcp_qr_test`

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

Row format is that of [`matrix.md` §1](../../libppcp/docs/conformance/matrix.md). Only the **PinPointStudio** column is claimed here.

| Test | Invariant | Method | Work packages | PinPointStudio | Command |
|---|---|---|---|---|---|
| CT-I12 | I12 | fixture | H3 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_bundle_import_test` |
| CT-I14 | I14 | static | H2 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_host_peer_test` |
| CT-I15 | I15 | fixture | H3 | `impl` | `ctest --test-dir build/ppcp-tests -R ppcp_bundle_import_test` |
| CT-I16 | I16 | paired | H3 | `impl` | `ctest --test-dir build/ppcp-tests -R ppcp_bundle_import_test` |
| CT-I19 | I19 | static | H2 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_source_declaration_test` |
| CT-I34 | I34 | fixture | H3 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_bundle_import_test` |
| CT-I36a | I36 | paired | H4 | `pass` (consumer half) | `ctest --test-dir build/ppcp-tests -R ppcp_video_input_test` |
| CT-S1 | I17, I22 | injected | H4 | `impl` | `ctest --test-dir build/ppcp-tests -R ppcp_video_input_test` |
| CT-S3 | — | static | H2 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_source_declaration_test` |

**CT-I12** — `AnySubsetOfStreamsLoadsIncludingNone`. A video-only bundle, an IMU-only bundle and a Session with no Streams at all each load, produce a Session record, and yield the Captures the bundle announced. The bundles are written through `ppcp_bundle_writer`, not by hand.

**CT-I34** — `ASecondImportOfTheSameBundleAddsNothing` and `TheSameCaptureIdUnderAnotherPeerIsAnotherCapture`. The identity decision is `ppcp_capture_index` in `libppcp`, seeded from this host's ledger, so both applications run one rule. The two Captures that break a digest-keyed importer are in the fixture: a `complete` + `pending` clip with no digest yet, and an `absent` one that will never have one. A second import reports both as already held and the ledger does not grow.

**CT-I15 is `impl`, not `pass`.** Both halves are asserted — a bundle carrying a wall-clock `discontinuity` imports identically to one without it (`AWallClockStepChangesNothingAboutWhatIsImported`), and the ingest path contains no reference to `wall_utc` or `.epoch` for it to have computed from (`TheIngestPathNeverReadsTheWallClock`). What is missing is the positive half of the row's intent: this host does not yet COMPUTE an interval from an imported Session at all, so "no interval is computed from `wall`" is currently true for the uninteresting reason. It moves when H7 puts imported Captures on a timeline.

**CT-I16 is `impl`, not `pass`.** The half that is reachable is asserted: a second `session_open` naming a different `timebase_ref` does not move the Session's (`ImportingCannotMoveASessionsTimebaseRef`, MSG 4.1a). The other half — a re-solved clock mapping appearing as a NEW `TimebaseRelation` **from** the unchanged `timebase_ref` — needs relations, which is `libppcp` L9, and is not claimed.

**CT-S1 is `impl`, not `pass`, and the reason is which half of the row a host runs.**
`ppcp_video_input_test` drives two real `ppcp_peer`s in one process — a capture peer that
declares an AVFoundation-shaped camera (`nominal_frame_start`, rolling shutter) and the peer
`makeHostEngine()` builds — and moves the bytes between them one frame at a time. Of the six
assertions:

- **1** in its `nominal_frame_start` form: `canonical = t + offset + d/2`, reproduced to the
  nanosecond for three frames (`TheConversionIsAppliedWithTheProfilesOwnTiming`). The other
  three rows of §6.1's table are `libppcp` L3's arithmetic and are asserted there.
- **2 — the whole test.** Two declarations differing only in
  `frame_start_to_exposure_offset_ns` (0 and 120000) produce instants differing by exactly
  120000 ns for every frame (`TheDeclaredOffsetMovesEveryInstantByExactlyItself`).
- **3.** Doubling every exposure changes every converted instant, by exactly half the change
  (`DoublingEveryExposureChangesTheConvertedInstants`). An implementation reading the profile's
  exposure *range* rather than `AchievedFrames.exposure_ns` fails here.
- **5.** Row-`r` instants under `top_to_bottom` and `bottom_to_top`, and the `R == 1` case
  (`RowInstantsFollowTheDeclaredReadoutAndDirection`,
  `BottomToTopReversesTheRowOrderAndROneIsFlat`).
- **6.** The scalar form and an equivalent constant array produce identical instants
  (`TheScalarFormAndAConstantArrayAgreeExactly`) — the path the shipping product uses, because
  the application locks exposure.
- **4 — round trip — is not exercised, and is not this work package's.** A host inverts the
  conversion when it expresses a `t0` it decided back in a device's own convention, which is
  `capture_request` and therefore **H5**. Nothing in H4 inverts, so the row does not move to
  `pass` here.

**CT-I36a is claimed for the CONSUMER half only.** The row is stated as *paired*, over a
`preview` Stream under induced contention, and its three assertions divide by who owns them.
The two a consumer can answer are asserted here: a preview Capture announced `transfer: pending`
is **refused** (`APreviewCaptureAnnouncedPendingIsRefused`), and a shed segment arriving as
`absent` / `not_retained` is accounted as an absence and **never read as a gap**
(`AShedPreviewSegmentIsAnAbsentSegmentAndNotAGap`, I11). The third — *none reaches the bundle* —
is the producing peer's obligation and is `libppcp` L7's row; this host writes no bundles.

⚠ **The refusal had to be written here, and that is a finding.** `ppcp_capture_validate_in_stream()`
carries 5.11j, but the engine does not run it on receipt: `peer_handle()`'s
`capture_announce` arm calls `ppcp_transfer_observe_announce(..., false)`, hardcoding
`is_preview` even though `ppcp_peer_stream_find()` could resolve the Stream from the Capture's own
`stream_id`. See finding 6 below.

### What H4 landed, and the seam it deliberately did not cross

`src/Video/VideoInputPpcp.{h,cpp}` presents a connected capture peer's camera Source behind
`video_input_factory` (`Backend::Ppcp`), as a peer of `VideoInputAravis` and `VideoInputApple`
rather than a special case any consumer knows about. `queryCapabilities()` is a **pure function of
the counterpart's declaration** — resolutions, rates, formats, exposure range, and every profile's
`timing` / `geometry` / `intrinsics` carried out through `CameraCapabilities::extensions` so the
application can see the convention rather than assume one (I19). A profile declaring
`intrinsics: none` is a preview profile (5.11m) and is excluded from the capture capability set,
because 5.11l forbids a consumer selecting one for capture. `start()` opens a `video` Stream
(`shot_windowed`, per 5.11's stream-kind table) and, **where the device offers one**, a `preview`
Stream (`continuous`); `suspend()` closes the preview and leaves the capture Stream open.

**What it does not do is push clips through `videoFrameReady()`, and that is F7 honoured rather
than ignored.** The design review found REQ-HOST-1 names the wrong seam and the disposition
endorsed it — *"a PPCP peer is a session-layer participant, not a frame source"* — while naming
where `VideoInputBase` **is** right: *"the right home only for a live preview stream"*. The
mechanism makes it concrete: `CameraInstance::connectVideoInput()` stamps
`EventBuffer::nowMicros()` on **arrival**, which for a peer's clip is when the bytes finished
crossing a socket. Emitting a clip as a live frame would destroy the canonical instant this work
package exists to apply. So the live tile is the preview Stream and reaches
`videoFrameReady()`; a clip leaves as `clipReady(PpcpClip)` with its per-frame canonical instants,
its exposures, its completeness and its absent reason intact, for the session layer of H5/H7.

**And the canonical instant is not yet a host timestamp.** §6.1 converts *within* a Source's
timebase; carrying it onto `tb:host` needs a `TimebaseRelation`, which is H5's sync prober over
`libppcp` L9. Neither has landed, so `lastFrameInstantUs()` answers **0** — "I have no instant on
your clock" — and `CameraInstance` falls back to arrival stamping. A plausible number here would
be a fabricated clock mapping, indistinguishable downstream from a measured one and shaped exactly
like drift; that is I31's discipline applied to clocks
(`WithNoTimebaseRelationTheHostClockAnswerIsRefused`).

`VideoInputBase` gains one virtual, `lastFrameInstantUs()`, alongside the
`lastMeasuredExposureUs()` side channel that already exists for the same reason: a fact about the
frame that cannot travel *on* the frame. Local backends inherit 0 and are unaffected.

### What H3 landed, and what it deliberately did not

The bundle path is a **file transport**, not an importer (plan A10, `CORE` §9, `ENC` §7): `ppcp_bundle_reader` streams the frames into `ppcp_peer_feed()`, the same function the socket drives, and the peer is built by `makeHostEngine()` — the one factory both transports use. There is no message-type branch anywhere in `src/Ppcp/ppcp_bundle_transport.cpp`.

What it does **not** do is write the imported Session into the swing library as a `swing.json` beside the live captures. It cannot yet: this application's `Session.id` is a filesystem directory path and its `Shot.id` an `int` ordinal, and `CORE` 8.5c keys idempotent re-import on **opaque** ids. Clips and the ledger therefore land under `<library>/PPCP Imports/<peer>/<session>/` with their real PPCP identities, and the join waits for host work item 2. Doing it early would either duplicate a Session on the second import or throw the PPCP identity away — and I34 is precisely the invariant that would be lost.

---

## 6. Findings raised against the specification

Recorded here because they were found while building this, and dispositioned in `libppcp` (plan ground rule 3 — the specification changes first, and the change history grows).

| # | Clause | Finding |
|---|---|---|
| 1 | `CORE` §3, `ENC` §2 | With one channel per TCP connection (plan A6), nothing says **how a listener associates the connections of one peer**, nor which of them is channel 0. Two implementations will resolve it two ways and never meet. This repository groups by the pairing the PSK identity resolved to (RV 5.3b) and orders by serialising the dialler's handshakes, which needs no bytes on the wire — but it is a local convention where a clause is wanted. |
| 2 | `RV` 5.3f | *"A peer MUST NOT transcode, validate as text, or truncate an identity"* is **not achievable on the TLS 1.2 path through OpenSSL**: both its PSK callbacks pass the identity as `char *` and take its length with `strlen`. The 16 CSPRNG bytes of 5.3a carry an embedded `0x00` about one connection in sixteen, and that connection fails — intermittently, which is the worst shape available. 5.4b1 makes TLS 1.2 the *ordinary* path, not an edge case. Either 5.3a excludes `0x00` from `rn2`, or 5.3f acknowledges the limit. Characterised by a test (`IdentityWithAnEmbeddedNulCannotSurviveTheTls12Path`) rather than worked around: truncating on purpose would be the transcoding the clause forbids. |
| 3 | `ENC` §6, `CORE` 5.7 | **A payload has no declared container.** `payload_begin` carries `bytes`, a `digest` and `chunk_bytes`, and nothing that says what the bytes ARE. `CaptureProfile.format.codec` exists but is a *codec* (`hevc`), not a container (`mp4`), and it reaches a receiver three hops away — Source → CaptureProfile → Stream → Capture. A consumer that writes an imported clip to a file must therefore guess its extension, which this host does from the Stream `kind` (`ppcp_import_sink.cpp`, `extensionFor`). Either `payload_begin` gains an optional media type, or `ENC` §6 says out loud that the container is out of band. |
| 4 | `ENC` §7, `MSG` 3.3c | **Nothing requires a bundle to carry a `declare`**, yet `CORE` 8.5c scopes Capture identity by the **minting peer**, and the only place a bundle states who that is is a `declare` (or a `session_joined` nobody records offline). `MSG` 3.3c rescues the ordinary case — a peer declares before originating anything referencing a Source — but it is an inference across two documents, and a bundle of pure `capture_announce` frames is unattributable and therefore un-deduplicable. `ENC` §7 should require `declare` before any Capture-bearing frame, in the same breath as 7c's manifest rule. This host counts what it cannot attribute (`Stats::capturesUnattributable`) rather than guessing a scope. |
| 5 | `libppcp` API (not the specification) | **`ppcp_peer_feed()` gained `out_consumed`; `ppcp_peer_drain()` has no counterpart.** Inbound, the engine takes whole frames and hands the tail back, which is right and is why the pump now keeps one. Outbound, `drain` dequeues whole frames and the embedding is expected to have written all of them — so a short socket write (`CORE` T2 backpressure, which is ordinary on a bulk channel) loses bytes the engine considers sent. `PpcpHostPeer::pump()` counts the occasions rather than pretending; the fix is a `drain` that can be told how much was taken, or a peek/commit pair. |

| 6 | `libppcp` API (not the specification) — **F-H4-1** | **5.11j is enforced on the way OUT and not on the way IN.** `ppcp_peer_capture_announce()` refuses a preview Capture announced `transfer: pending` (8.1i), and `ppcp_capture_validate_in_stream()` is the function that says so. But `peer_handle()`'s `PPCP_MT_CAPTURE_ANNOUNCE` arm calls `ppcp_transfer_observe_announce(&p->transfers, &…capture, false)` — `is_preview` hardcoded — even though the Capture carries a `stream_id` and `ppcp_peer_stream_find()` would resolve the Stream the engine already recorded. So a conformant consumer must re-run the check itself or it will silently accept the thing 5.11j says it never sees, and its own transfer table will disagree with the wire. CT-I36a's consumer half is therefore an application obligation by accident rather than by design. `VideoInputPpcp::onCaptureAnnounce()` runs `ppcp_capture_validate_in_stream()` and counts the refusals. |
| 7 | `CORE` 5.11 vs `MSG` §11 — **F-H4-2** | **`closed_at` is optional on the entity and mandatory on the message, and a consumer closing a Stream cannot honestly supply it.** `CORE` 5.11 gives `Stream.closed_at` cardinality `0..1`; `MSG` §11 writes `stream_close { stream_id, closed_at: Instant, reason: Kind }` with no optionality, and `ppcp_peer_stream_close()` enforces the `MSG` reading by refusing a NULL. 5.11a1 explicitly permits the **consumer** to close — "because it no longer wants the data" — but the Stream's `timebase_id` is the *owner's*, and a consumer has no reading of that clock. Putting this host's number under the device's timebase id would be I1's defect written into the wire. Either 5.11a1 says which timebase a closing peer's `closed_at` is in, or `MSG` §11 matches 5.11 and makes it `0..1`. This host sends its own honest instant on `tb:host`; a receiver that needs it on the Stream's timebase converts through a relation. |
| 8 | `libppcp` API (not the specification) | **A consumer cannot tell which channel a payload arrived on.** `ppcp_event` carries `kind`, `msg` and `status`; the channel the frame was fed on is not among them. 5.11h asks a peer to carry preview payload on a **bulk channel distinct** from shot payload so preview never queues behind a clip — and a consumer cannot observe whether it did, so the clause is unverifiable from the receiving end. Adding the channel to `ppcp_event` costs one byte and makes 5.11h checkable. |

### Note (22 Aug 2026) — H3 has no UI

The "Import Session…" menu item and its file dialog were removed the day they
landed. This application has no menus and no native dialogs; and the user's
intent is not to import files at all — a connected capture device OFFERS its
recorded sessions (`MSG` §9 `session_offer` / `session_accept` /
`session_manifest`, Offline profile) and the host chooses from that list. The
bundle transport, import sink and ledger stay, as the engine behind that list;
the list itself is S3 (H4–H7) work. The CT rows above are unaffected — they
were never stated over a UI.

---

## 7. What H5 and H7 landed (session 3, wave 2)

Everything below is reproduced by one command:

```
cmake --build build/ppcp-tests -j3
ctest --test-dir build/ppcp-tests --output-on-failure
```

10 suites, 10 passing.

### 7.1 The rows

| Test | Invariant | Method | Work packages | PinPointStudio | Command |
|---|---|---|---|---|---|
| CT-I7 | I7 | paired | H5 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_arbitration_test` |
| CT-I8 | I8 | paired | H5 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_arbitration_test` |
| CT-I18 | I18 | static + injected | H5 | `pass` (negative half) | `ctest --test-dir build/ppcp-tests -R ppcp_live_session_test` |
| CT-I20 | I20 | static | H5 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_arbitration_test` |
| CT-I21 | I21 | static | H5 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_live_session_test` |
| CT-I35 | I35 | paired | H5 | `impl` | `ctest --test-dir build/ppcp-tests -R ppcp_arbitration_test` |
| CT-I37 | I37 | static (API surface) | H7 | `pass` | `ctest --test-dir build/ppcp-tests -R ppcp_annotation_test` |
| CT-S5 | — | paired | H5 | `impl` | `ctest --test-dir build/ppcp-tests -R ppcp_live_session_test` |
| CT-S1 | I17, I22 | injected | H4, H5 | `impl` | `ctest --test-dir build/ppcp-tests -R ppcp_video_input_test` |
| CT-I36a | I36 | paired | H4 | `pass` (consumer half) | `ctest --test-dir build/ppcp-tests -R ppcp_video_input_test` |

**⚠ Every row above is evidence from two `ppcp_peer`s in ONE PROCESS.** `tools/ppcp-sim`
(`libppcp` L13) now exists at `libppcp/build/dev/tools/ppcp-sim`, and this agent could not
execute it — the sandbox refuses a binary outside this repository, and copying a build
artefact of another repository into this tree to get round that would make the evidence about
a copy rather than about the tool. So no row here is claimed against it yet. When it does, CT-I7, CT-I8, CT-I20, CT-I21, CT-I35 and
CT-S5 become claims against a genuinely independent implementation, and interop rows 1, 5, 6,
7 and 8 become reachable at all. Nothing here is a mock — the device end is a real engine
configured as a capture peer and the host end is the one `makeHostEngine()` builds — but two
engines built from one library agree about more than two implementations do, and that is the
whole point of the pairing.

**CT-I21** — `OneSyncEstimatorPerDeclaredHostTimebase`. The prober is registered by walking
the host's own declaration, not by a constant: this host samples every Source against one
clock and gets one estimator, and a host with four free-running camera clocks would declare
four Timebases and get four, with no branch and no edit. The assertion is that the count came
from the declaration. It also asserts the negative: **no** estimator for `tb:dev`, because a
host that had one would be claiming to read a clock it cannot (I1).

**⚠ The remote half of I21 is not reachable through libppcp's API, and that is a finding.**
A responder answers `sync_probe` by stamping its ONE `ppcp_peer_config.sync_timebase`, and
`ppcp_peer_sync_add_timebase()` keys its estimators on the LOCAL timebase — so a host cannot
run two probe sequences against two clocks of the same device. A device with a camera clock
and an audio clock yields one measured relation and one that is simply absent, which 5.4b
makes a legal and honest outcome and 8.2d then excludes. See finding 9.

**CT-S5 / CORE §6.3** — `TheProbeExchangeRecoversTheOffsetAndTheSkew`. Two `ppcp_sim_clock`s
with a **4.2 s offset and 40 ppm of skew** between them, driven for a couple of minutes of
simulated time. The exchange recovers both, and neither number is ever handed to the
estimator; the responder's timebase is **learned from the first `sync_reply`** (6.1b — the
constructor was given NULL). The relation comes back `affine`, `method: estimated_online`,
with both sigmas non-zero (I3 makes a relation without them unconstructible). The tolerances
are loose on purpose: 6.3e publishes a **filtered** value, so a tight bound here would be
asserting that the filter does not exist.

It is `impl` and not `pass` because CT-S5 is stated over a real link with real network
latency, and a same-process pair has none — the RTT distribution whose left tail 6.3f filters
on is degenerate here.

**CT-I18** — `AConversionWithNoDirectRelationIsRefusedAndNeverAssumedZero`. Three assertions.
With nothing measured, `offsetToRefNs()` **refuses and writes nothing** to its output; a
fabricated zero is indistinguishable downstream from a measured mapping and shaped exactly
like drift. A Source already on `tb:host` converts with a zero offset, and that zero is a
fact rather than a fallback (I4 — identity is identity, never asserted as a relation with
`from == to`). And after the exchange has run, the conversion either found a **direct**
relation or refused; nothing composes, because `ppcp_relations_convert` applies at most one
relation and 5.4c forbids deriving A→C from A→B and B→C.

**CT-I20** — `ANonHostPeerCannotArbitrate`. `ppcp_arbiter_new()` refuses a `role: capture`
peer, and `PpcpShotBridge::start()` reports the refusal rather than degrading into something
that looks as though it worked. CONF §1d's negative half: a peer that arbitrated without
declaring Arbitrate is non-conformant, and the refusal is what stops it reaching a wire.

**CT-I8** — `TwoAcousticNominatorsFromDifferentPeersBothAppear`. The host declares **its own
microphone**, so the assertion the row exists for is reachable: a device microphone and a
host microphone nominate 3 ms apart, inside the 50 ms coincidence window, and the issued Shot
carries **both** Candidates. This is the assertion `ShotArbiter` fails — it models three fixed
modalities in fixed slots, so the second `acoustic` nomination overwrites the first and
nothing records that it happened. Two further rows: an over-wide sigma **excludes and
retains** (8.2d — exclusion is a conclusion, the Candidate remains evidence), and with no
relation at all **every** foreign Candidate is retained and none is grouped (8.2i1 — there is
not even an instant to group by, and the honest answer is a Candidate held for ever with no
Shot).

**CT-I7** — `ACandidateArrivingAfterTheShotAttachesAndT0IsNotRevised`. A late Candidate
produces no second Shot and no revision. `t0` could not have been revised even if this host
wanted to: libppcp has no setter for it anywhere, and `ppcp_shot_attach_candidate()` takes a
Candidate rather than an instant precisely so that attaching cannot move it.

**CT-I35 is `impl`.** The half that is asserted is that a late Candidate attaches and the
Shot converges. The half that is not is 8.2k/8.2l with a **device-minted** Shot — a Mint peer
issuing its own `shot` and the host attaching to it rather than competing. That needs a
device that mints, which is `PinPointCapture`'s D-series or `ppcp-sim`, and this host cannot
produce one.

**CORE §8.1 — the launch monitor row.**
`TheLaunchMonitorRowBecomesAnArrivalPairingLinkConfirmedByObserver`. The GCQuad's CSV row
becomes a `ShotLink` with `basis: arrival_pairing`, `confirmed: true`,
`confirmed_by: observer` and `foreign_system: com.foresightsports.gcquad`, and the Candidate
count **does not move**. 5.16f permits `observer` here precisely because `arrival_pairing` is
not one of the three retrospective bases — the host armed the slot when it detected the swing
and watched the row arrive, which is an observation and not a human decision. There is
deliberately **no function on `PpcpShotBridge` that can turn a launch monitor reading into a
Candidate**, which is 8.1b and 8.1e by API surface.

**CORE §8.4** — `CaptureRequestNamesT0InTheSessionTimebaseRef`. `t0` crosses in `tb:host`.
The owner inverts §6.1's conversion into its own convention at its end; a host that did it
for them would apply the correction twice (8.2a, I33) — which is CT-S1 assertion 4, and it is
still not exercised here, because nothing on this side inverts.

**CT-I37** — `NothingInAnalysisReadsAnAnnotation`. CONF §3 makes this a check on **API
surface, not behaviour**, so the test greps every `.h`/`.cpp` under `src/Analysis` for an
include of a markup header and fails if one appears. A grep is an odd-looking test and it is
the right one: no runtime assertion can say "no analysis ever reads an Annotation" about code
nobody has written yet. The failure it catches is a plausible one — a `kind: nav_anchor`
annotation looks exactly like phase data, a labelled instant on a shot, and the analysis
ladder is full of code that wants labelled instants. Reading one would turn a user's drawing
into an observation and every metric downstream would inherit it.

**CORE §5.18** — three more rows. `ThePersistedBodyComesBackByteIdentical` writes a body with
**embedded NULs and high bytes**, restarts the store, and reads it back byte for byte; a
store that had gone through a text encoder would truncate at byte ten and every other field
would still look right. `TwoDeliveryOrdersConvergeOnTheSameRevision` and
`EqualRevisionsFromTwoAuthorsAreBrokenBytewiseAndNotIgnored` assert 5.18e including the
`author_peer_id` tiebreak — the case a coach at the host and a golfer at the device create,
where without the tiebreak both hold revision 1, both produce revision 2, each ignores the
other's equal revision, and the two ends diverge permanently while each believes it
converged.

**CT-I36a's consumer half had to be rewritten, and the rewrite is itself the finding.** Until
`libppcp` L9 the suite made the device non-conformant by lying to its own engine —
`ppcp_peer_capture_announce(..., is_preview=false)` on a preview Stream. L9 closed that:
the engine now resolves the Stream from the Capture's own `stream_id` and refuses when the
flag disagrees. **Finding 6 (F-H4-1) is therefore fixed on the origination side**, and a
conformant peer can no longer produce the frame at all. The frame is now built and framed by
hand and fed straight in, which is what a genuinely non-conformant third-party peer would put
on the wire — and the consumer-side check is still `VideoInputPpcp`'s own, so the row is still
an application obligation.

### 7.2 What is wired but not exercised in the application

**⚠ SUPERSEDED BY H-COMPOSE (session 4, §9 below).** `PpcpHostService` now constructs the
`PpcpHostPeer`, owns the `Listener`, and is built in `main.cpp`. What follows is the state as
it stood at the end of session 3 and is kept because the *evidence* position it describes has
not changed: everything in §7.1 is still asserted over engines the test suite constructs, and
the application path is still unverified in the running application.

At the end of session 3, **nothing in this application constructed a `PpcpHostPeer`.** H1's
transport and H2's peer were built and tested; no screen, service or controller started one.
So the following were written, compiled only by the app target (which that work did **not**
build), and **unverified**:

- `PpcpHostPeer` now owns the live session, the arbitration bridge and the annotation store,
  drains the event ring in `pump()` and dispatches to all three, and offers
  `setDeclarationHook()` / `setRelationsHook()` / `addEventHook()`. `tick(nowNs)` runs the two
  §6.3/§7.4 schedules and 8.2h's issue hold.
- `ShotController` gains `setPpcpBridge()`, `setPpcpSourceIds()` and
  `commitArbitratedShot()`; `reportCandidate()` nominates and **returns** when the bridge is
  active, never touching `m_arbiter`.
- `PpcpOfferList.qml` in the DEVICES area of the home screen, and `ppcpOffers` as a context
  property beside `ppcpImport`. **The controller is installed detached**, so the list is
  always empty until an owner for the link exists. The component hides itself entirely rather
  than showing an empty heading, because "this device offered nothing" is a different and
  untrue statement.

**Three joins were named in the code and had no caller, all for the same reason. Two are
now called (H-compose, session 4).** `video_input_factory::registerPpcpPeer()` runs on
`PPCP_EVENT_DECLARE` from `PpcpHostService::onDeclare()` (MSG 3.3 — a peer's cameras exist the
moment it declares and at no other moment), and `VideoInputPpcp`'s offset seam is re-fed from
`PpcpLiveSession::offsetToRefNs()` on every 6.1f publish. The third — `clipReady(PpcpClip)`
into the session layer — is blocked on more than an owner and is still open; see below.

**⚠ `clipReady()` is blocked on host review item 2 and is NOT worked around.** A `PpcpClip`
carries opaque PPCP identities; this application's `Session.id` is a filesystem directory path
and its `Shot.id` an `int` ordinal, and CORE 8.5c keys idempotent re-import on **opaque** ids.
Writing a clip into the swing library today would either duplicate it on the second arrival or
throw the PPCP identity away, and I34 is precisely the invariant that would be lost. H3 made
the same call for imported bundles and landed them under `PPCP Imports/<peer>/<session>/`
instead; the live path has no equivalent yet.

**⚠ And the offset seam cannot carry the whole relation.** `setTimebaseOffsetNs()` takes a
scalar, and a `TimebaseRelation` is affine — so `offsetToRefNs()` evaluates it at one instant
and the skew term goes stale at the rate it was measured. That is why the function returns the
uncertainty beside the offset and why it is re-evaluated on every publish rather than set once.
Widening the seam to take a relation is a `VideoInputPpcp` change and belongs with whoever
owns the join.

### 7.3 What is not claimed

- **`tof_correction` is never sent.** 8.1d asks an acoustic nominator to correct for time of
  flight before emitting `at`, and to report the correction and its uncertainty. This host's
  detectors do not measure their distance to the ball, so there is no correction to report and
  **none is invented** — at 343 m/s it is ~2.9 ms per metre, and a device 2 m from the ball
  lags 5.8 ms, which is most of a frame at 150 fps. It needs a surveyed microphone position,
  which is a `Calibration` this application does not yet acquire.
- **`coincidence_window_ns` and `issue_hold_ns` are `CORE` §5.10's proposals**, not
  measurements. CORE B8 says the same of them, and says the floor must be measured **per
  nominator class**. `PpcpLiveSession::Config` holds both so that a rig measurement changes one
  number in one place.
- **`maxConversionSigmaNs` (5 ms) is a guess with a reason**, not a measurement: it is a frame
  at 200 fps and the arbitrated instant is read against video. It is this repository's number,
  never libppcp's (I14), and it is the only threshold in the arbitration path.
- **No `epoch` on `session_open`.** I15 / 5.3b make a wall-clock reading a label that is never
  used to compute an interval, and this host computes every interval from `tb:host`. Putting
  one on the arbitration frame would invite exactly the computation the clause forbids.

## 8. Further findings

| # | Clause | Finding |
|---|---|---|
| 9 | `libppcp` API — **F-H5-1** | **The remote half of I21 is unreachable.** 6.1d and I21 call for one probe sequence per timebase, and `ppcp_peer_sync_add_timebase()` keys its estimators on the LOCAL one while a responder answers with its single `ppcp_peer_config.sync_timebase`. So a host CAN run a sequence per clock of its own and CANNOT run one per clock of a device's: a phone with a camera clock and an audio clock yields one measured relation and one absent. 5.4b makes that legal and 8.2d then excludes the Candidates that needed it, so nothing is fabricated — but "one exchange per timebase" reads as symmetric and is not. Either the responder's timebase belongs in `sync_probe` as a request, or 6.1d should say that the responder chooses. |
| 10 | `libppcp` API — **F-H5-2** | **`ppcp_peer_session_params()` is NULL on the peer that ORIGINATED `session_open`.** peer.h says "as they arrived in `session_open`", and that is literally what it does — but the consequence is that a HOST cannot read back the Session it just opened: not `timebase_ref`, not `coincidence_window_ns`, not `issue_hold_ns`. Every one of those is needed by the host itself (8.2b compares against the window, 8.2h holds against the hold), so the host keeps a second copy and the two can drift, which is what a single accessor exists to prevent. 8.3g's "nothing about the Session changes" is therefore asserted at the DEVICE end in `ppcp_live_session_test`. |
| 11 | `libppcp` API — **F-H5-3** | **`ppcp_peer_config.health_report` is a PRECONDITION for liveness, not a decoration on it, and nothing says so.** peer.h documents it as "what `heartbeat_ack` carries". What actually happens without one is that every `heartbeat` is answered `error` / `profile_not_supported` with the message "no health source", so 7.4a never runs, no ack ever returns, and the sender's own link state stays `live` for ever because it is never told otherwise. Both halves of §7.4 looked broken in this suite until the harness supplied a callback; neither was. The refusal is arguably right — a peer reporting `thermal: nominal` on no evidence is the fabrication this library refuses everywhere else — but an embedding with no thermometer will silently have no liveness at all. |
| 12 | `libppcp` API — **finding 5, CLOSED** | `ppcp_peer_drain()` had no way to say "I only wrote N", so a short socket write under `CORE` T2 backpressure lost bytes the engine considered sent. L9 added `ppcp_peer_drain_peek()` / `_commit()`, and `PpcpHostPeer::pump()` now uses them. `commit` is given the exact byte count the socket accepted rather than a whole number of frames — a channel is an ordered byte stream, and rounding down to a frame boundary would re-send bytes that had already left. Recorded as closed rather than deleted, because the shape of the fix is the interesting part. |
| 13 | `libppcp` API — **finding 6 (F-H4-1), HALF CLOSED** | `ppcp_peer_capture_announce()` now resolves the Stream from the Capture's own `stream_id` and refuses when `is_preview` disagrees, so 5.11j is enforced on the way OUT and a conformant peer can no longer be made to lie. The receiving side is unchanged: a consumer must still run `ppcp_capture_validate_in_stream()` itself, and `VideoInputPpcp::onCaptureAnnounce()` does. CT-I36a's consumer half remains an application obligation. |
| 15 | `libppcp` API — **F-L13-1**, raised by team L | **`ppcp_peer_feed()` consumes unboundedly many frames per call, the event ring is four deep, and an overflow drops the OLDEST event with nothing readable to say so.** A single socket read carrying a replayed bundle loses `capture_announce` while the payload frames that reference it arrive — silently, and only under load, which is the worst shape available. `PpcpHostPeer::pump()` had exactly this bug: it fed a whole read and drained events afterwards. It now bounds each feed to one frame using the header's own `payload_len` — the idiom `PpcpBundleTransport` has always used — and drains between them. `F_L13_1_FeedingAWholeReadAtOnceLosesEventsAndOneFrameAtATimeDoesNot` asserts both halves, so the day libppcp L15 makes `feed` stop at the ring's capacity, the guard goes red and points at this row. |
| 14 | `libppcp` API — **finding 8, CLOSED** | `ppcp_event` gained `channel`, so a consumer can now check 5.11h — preview payload on a bulk channel distinct from shot payload. Not yet asserted here; it belongs with the join that consumes `clipReady()`. |

---

## 10. H8 — the conformance claim (session 4, wave 2) — **WORK IN PROGRESS**

> ⚠ **This section is a work-in-progress note, not a claim.** It records where H8 stopped so the
> next session resumes from here rather than from scratch. Nothing in it has been measured yet.

**The command this work package exists to run** — `ppcp-conform` (libppcp L14), driving a headless
`PpcpHostPeer` over **plaintext** loopback:

```sh
# 1. the headless host, listening on an ephemeral port it writes to a file
build/ppcp-tests/ppcp_conform_host --port 0 --port-file /tmp/pps.port --run-ms 60000

# 2. the instrument, from the libppcp build tree
../libppcp/build/dev/tools/ppcp-conform/ppcp-conform \
    --profiles core,capture,detect,arbitrate,live,offline,markup \
    --role host --connect 127.0.0.1:$(cat /tmp/pps.port) \
    --column PinPointStudio \
    --json  build/ppcp-tests/pps-conform.json \
    --markdown build/ppcp-tests/pps-conform.md
```

`--psk` is deliberately absent: `ppcp-sim` has no TLS **transport**, so the harness socket is
plaintext. `PPCP-RV` erratum E4 (RV 2c1) scopes 2c to the rendezvous paths and states that a test
harness socket is not one of them; `CORE` §3.2's `direct` transport is conformant plaintext.

### Done

- **A harness-only plaintext listener.** `Ppcp::Listener::setPlaintextHarness()` in
  `src/Ppcp/ppcp_transport.h`, and every line implementing it in `ppcp_transport.cpp`, sits inside
  `#if defined(PP_PPCP_PLAINTEXT_HARNESS)`. That macro comes from ONE place — the CMake option of
  the same name, **default `OFF`**, turned on only by `src/Ppcp/tests`. In a release build there is
  no plaintext code path to reach. ENC §2.1 link binding is unchanged: the dialler still mints a
  `link_id` and sends `link_bind` first on every stream, and every 2.1c refusal still applies.
- **Finding F-H8-1 — this host never sent its own `declare`, and no suite noticed.** MSG 3.3c makes
  `declare` a precondition for originating anything naming a Source, Stream or Candidate, and 3.3d
  says a host sends it *even with an empty `sources` list*. `PpcpHostService::start()` built the
  declaration and validated it; `grep -rn ppcp_peer_declare src/` outside the test tree returned
  **nothing**. Every suite in `ppcp-tests` declared by hand in its own fixture, which is exactly how
  a composition defect survives a green suite. Fixed in `PpcpHostPeer::drainEvents()`, on
  `PPCP_EVENT_CONNECTED`, once per link. **Whose defect: this host's.**
- **F-L13-1's guard inverted to the new libppcp contract** (`ppcp_live_session_test.cpp`). Recorded
  as a second finding below, because the old guard *would still have passed*.
- **Finding F-H8-2 — the old F-L13-1 guard could not tell the defect from the fix.** It asserted
  `bulkSeen < slicedSeen` and said in its own comment that it would go red when libppcp L15 landed.
  It did not: a bulk feed yields four events under **both** contracts — four survivors of a ring
  that dropped eight, or four reported before a feed that stopped. What distinguishes them is
  `ppcp_peer_events_dropped()`, `ppcp_peer_feed_stalled()` and the short `*out_consumed`, so those
  are what the test asserts now. **Whose defect: this suite's.**

### Next

- `src/Ppcp/tests/ppcp_conform_host.cpp` — the headless host executable: real `PpcpHostPeer`, real
  `makeHostEngine()`, real `PpcpIngestPolicy`, its own declaration through the real
  `PpcpSourceDeclaration::build()`, the storage callback F-H5-3 makes a precondition for §7.4, a
  `PpcpOfferController` for MSG 9.1/9.2, and a loop that pumps and ticks until the tool disconnects.
- The CMake option, the target and the `ctest` row.
- Run it, paste **every** row the tool reports — pass, fail, n/a — verbatim.

### Blocked

Nothing.

---

## 9. What H6 and H-compose landed (session 4, wave 1)

### 9.1 The rows

| Row | Command |
|---|---|
| RT-5, RT-7 (browser), RT-8, RT-9, RT-15 (publisher), RT-16 | `ctest --test-dir build/ppcp-tests -R ppcp_rendezvous_test` |
| The QR encoder behind RV 4.1d | `ctest --test-dir build/ppcp-tests -R ppcp_qr_test` |
| Every app-side PPCP translation unit still compiles | `ctest --test-dir build/ppcp-tests -R ppcp_app_tu_syntax` |

**Every refusal in the rendezvous suite is a real TLS handshake on loopback**, dialled by a
scanner built out of nothing but `ppcp_rv_uri_decode()`, `ppcp_rv_derive()` and
`ppcp_rv_psk_identity()` — the same four calls PinPointCapture will make. `RV` 5.2i is explicit
that this class of requirement is settled by observed behaviour and never by an API assertion,
and the same reasoning applies to §7.3: "the publisher invalidates the code" is a claim about
what happens when somebody dials.

### 9.2 `ppcp_app_tu_syntax`, and the hole it closes

On 22 August the application build broke on `ppcp_import_controller.cpp` and **no suite here
compiled that file**. Every row in this document links the handful of translation units it
needs; the app-only ones — the QML controllers, the device-registry adapter, the composition —
had no compiler pointed at them at all until the application was built by hand. A green
`ppcp-tests` therefore carried no information about whether the application still built, which
is the worst property a gate can have.

The row runs the compiler front end (`-fsyntax-only`) over all 21 of them with the app's
include paths and Qt: no link, no moc, ~2.4 s. It is **deliberately globbed**, and it is the
one glob in that file — a translation unit somebody forgets to list is exactly the failure
being guarded against. It was verified by **negative control**: made red by reintroducing the
22 August shape of break, green again by removing it.

`src/Gui/main.cpp` is out of scope and the file says so: it reaches whisper, ONNX Runtime,
OpenCV and Sparkle. The composition therefore lives in its own translation unit and main.cpp's
uncovered share is four lines.

### 9.3 What is wired in the application and still unverified there

`PpcpHostService` (`src/Ppcp/ppcp_host_service.{h,cpp}`) owns the `Listener`, the
`PpcpRendezvous`, the `PpcpHostPeer` and the engine. It is constructed in `main.cpp` beside
`CameraManager`, listens on 7788 (falling back to an ephemeral port), accepts on one thread
whose only job is to block in `accept()`, and pumps and ticks from a 20 ms `QTimer`.

Called from it, and **compiled but not run in the application by this work**:

- `VideoInputFactory::registerPpcpPeer()` on `declare`, then a signal that asks
  `CameraManager::enumerate()` — the registry has already been told, and the home screen's
  DEVICES list reads the registry directly on its own two-second refresh, but `CameraManager`
  snapshots at construction and has to be asked.
- `VideoInputPpcp::applyTimebaseOffsets()` on every 6.1f publish, **per Source timebase**. A
  peer with a camera clock and an audio clock has one relation per clock; a single scalar for
  the peer would fabricate whichever Source it did not describe. A lookup that answers "no
  direct relation" (5.4b, 8.2i1) **clears** that instance's mapping rather than leaving a stale
  one — a stale offset is shaped exactly like drift.
- `PpcpOfferController::attach()`, which has been installed detached since H5.

### 9.4 What is still not claimed

- **`clipReady()` is still not connected**, and for the reason §7.2 gives: a `PpcpClip` carries
  opaque PPCP identities, this application's `Session.id` is a filesystem directory path and
  its `Shot.id` an `int` ordinal, and CORE 8.5c keys idempotent re-import on opaque ids.
  Wiring it today would either duplicate the clip on re-arrival or throw the identity away.
  That is host review item 2 and H-compose does not settle it by accident.
- **`ShotController::setPpcpBridge()` is still not called.** It is a live behaviour change for
  every shot — `reportCandidate()` stops touching `m_arbiter` once a bridge is set — and this
  work cannot run the application to see it. Deliberately left for a wave that can.
- **The macOS keychain path is compiled but not exercised at runtime.** The login keychain
  cannot be unlocked from a non-Aqua session, so a test that needed it would be red on every
  headless box. The suite uses the in-memory store, whose `describe()` says in as many words
  that it is **not** protected storage. RT-12 is where the keychain path is read.
- **Discovery is browse-only and untested against a live responder.** `parseTxtRecord`,
  `pvAcceptsMajor`, `instanceNameMatchesRid` and `decideDial` are asserted; `DNSServiceBrowse`
  is not, because there is nothing on this network advertising `_ppcp._tcp`. 3.6a makes that a
  non-event by construction: discovery failure is not an error state and there is no error
  channel to report one on.

### 9.5 Findings

| # | Clause | Finding |
|---|---|---|
| 16 | `RV` 7.3a vs `CORE` T1/T2 and `ENC` §2.1 — **F-H6-1** | **`mu` cannot count handshakes, and 7.3a says it does.** "A publisher invalidates a pairing code once `mu` handshakes have completed with it. The default is one." A PPCP link is **two (optionally three) TCP connections**, each its own TLS session keyed by the same `K_tls` — that is CORE T1/T2 and ENC 2.1 and it is not optional. So the default `mu: 1`, which is the pairwise case the entire model is built around, is spent by the control channel's handshake and the bulk channel **of the same link** is then refused: every conformant PPCP-RV pairing dies on its second channel. `mu` can only mean **pairings**. This host counts links (`PpcpRendezvous::noteLinkEstablished()`, called once per accepted `PeerConnection`) and the suite asserts two resolves for one pairing so the arithmetic is visible. Suggested fix: 7.3a reads "once `mu` **pairings** have been established with it", with a note that a pairing is a link and a link is several handshakes. |
| 17 | `RV` 7.3a vs 7.5a — **F-H6-1a** | **Second-order, and it survives the fix above.** 7.5a has a reconnecting peer complete a full handshake on the same derived `K_tls` without a new code; 7.3a invalidates the code after `mu` completions; 7.5c then says `session_resume` is refused for a session whose pairing was invalidated under §7.3. Read literally, a `mu: 1` code permits one link and no reconnection at all, which makes §7.5 dead letter in the default case. This host resolves it by treating 7.3a as spending the **code** and 7.3b as ending the **pairing**, so a session survives its code and dies with its session. §7.3 should say which of the two it means. |
| 18 | H1's API — **F-H6-2, closed here** | `ResolvedPairing::pairingId` is documented in `ppcp_transport.h` as "the embedding's handle on WHICH pairing authenticated a stream", the listener has held it since H1, and there was **no accessor**. An embedding that had to act on it — and RV 7.3a's single-use defence is exactly an action on it — could not find out which code had just been used. `TransportChannel::pairingId()` and `PeerConnection::pairingId()` now expose it. |
| 19 | `RV` 7.2c vs this application — **F-H6-3** | **The application's existing secrets subsystem is not protected storage, and using it would have been the easy mistake.** `src/Secrets/SecretsManager` keeps API keys in `QSettings`, which on macOS is a plain plist in `~/Library/Preferences`, and its header says the choice was "no extra dependencies". That is right for an Azure key and wrong for a `PRK`: possession of a preferences file would become a standing ability to complete a handshake as a paired peer. H6 reaches Security.framework directly and returns **no store at all** where the platform has none, so `persist()` refuses rather than falling back to a file. Not a defect in the specification — 7.2c is clear — but the shape of the trap is worth recording, because the wrong answer was already in the repository and looked like reuse. |
| 20 | `libppcp` — **F-H5-1, closed** | The remote half of I21 is reachable: `ppcp_peer_sync_add_target()` keys sequences on the (local, remote) pair, and erratum E2 has a probe naming a timebase the responder declared answered on that timebase. Not yet exercised from this host, which has one clock and currently probes with `ppcp_peer_sync_add_timebase()`; the multi-clock device case is `libppcp`'s CT-I21. |
| 21 | `libppcp` — **F-H5-2, closed** | `ppcp_peer_session_params()` now answers on the peer that ORIGINATED `session_open`, so a host can read back the Session it just opened. `ppcp_live_session_test` asserts the two ends agree rather than asserting the host is blind, which is the property the finding was about. |
| 22 | `libppcp` — **F-H5-3, closed, and it broke six suites** | It came back as a **hard precondition**: `ppcp_peer_new()` now refuses a peer that declares `live` with no `health_report` — "a peer that has no thermometer declares no Live". That is exactly what this repository asked for, and six suites went red at once because the host and the harness peers all declared `live` and supplied nothing. `PpcpHostPeer::makeLibppcpEngine()` now builds a report from the two callbacks it already had, and `makeHostEngine()` drops `live` where the embedding supplied no reading — a profile is a promise about behaviour (2.2.2), and the bundle path has no heartbeat to answer anyway. |
| 23 | `libppcp` — **F-L13-1, closed, with a harness consequence** | `ppcp_peer_feed()` now stops rather than overrun the event ring. The consequence for a test harness is that "pipe everything, then look at the events" no longer works: three assertions in this repository had been reading whatever survived a four-deep ring. `pptest::pipe()` takes an event sink and drains between frames — which is what `PpcpHostPeer::pump()` has always done in production — and **stops rather than skipping** a frame it cannot feed, so the next one to get this wrong fails loudly. |
