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
| RT-12 | **review** | secrets from a platform CSPRNG at full width, erased (**storage dropped by erratum E56** — `RV` 7.2c is a SHOULD; the reviewer records where secrets are held rather than failing the row for a store that is not the platform's protected one) | H6 | n/a | `review — src/Ppcp/ppcp_rendezvous.cpp csprngBytes() and its four call sites; src/Ppcp/ppcp_pairing_store.cpp; reviewer unassigned` | — |
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

## 10. H8 — the conformance claim (session 4, wave 2)

**This is the claim.** Every row below was produced by `libppcp/tools/ppcp-conform` (work package
L14) driving this application's real host peer over a loopback socket, with `tools/ppcp-sim` as the
counterpart. Nothing in it was asserted by a test in this repository, and no row was edited.

| | |
|---|---|
| Instrument | `ppcp-conform`, libppcp `e52647e` (`ppcp-sim` the same) |
| Peer under test | `build/ppcp-tests/ppcp_conform_host` — the real `PpcpHostPeer`, headless |
| Role | `host` |
| Profiles claimed | `core capture detect arbitrate live offline markup` (Mint withheld) |
| Result | **11 of 12 applicable rows pass.** One row — CT-I6 — cannot be run against a host and is a finding against the instrument, not against this host |
| Date | 23 August 2026 |

### 10.1 Reproducing it

```sh
cmake -S src/Ppcp/tests -B build/ppcp-tests -G Ninja
cmake --build build/ppcp-tests -j3
ctest --test-dir build/ppcp-tests -R ppcp_conformance --output-on-failure
```

`ctest` drives `src/Ppcp/tests/run-conform.sh`, which is two commands:

```sh
# 1. the peer under test, headless, on an ephemeral port it writes to a file
build/ppcp-tests/ppcp_conform_host --port 0 --port-file $WORK/conform-host.port --run-ms 240000

# 2. the instrument, from libppcp's own build tree
../libppcp/build/dev/tools/ppcp-conform/ppcp-conform \
    --profiles core,capture,detect,arbitrate,live,offline,markup \
    --role host --connect 127.0.0.1:$(cat $WORK/conform-host.port) \
    --column PinPointStudio \
    --json $WORK/pps-conform.json --markdown $WORK/pps-conform.md
```

`ppcp-conform` is **not built by this repository** — `pp_require_ppcp()` forces `PPCP_BUILD_TOOLS`
OFF when the library is embedded, because this repository must not start building another
repository's command line tools (plan ground rule 1). CMake finds it in the sibling libppcp build
tree, and says so loudly if it cannot; `-DPP_PPCP_CONFORM=<path>` overrides.

### 10.2 The socket is plaintext, and that is a conformant `direct` transport

`--psk` is deliberately absent. `ppcp-sim` has no TLS **transport**: its `--psk-ke-only` mode is a
hand-built ClientHello for RT-4 and speaks no application data, so the instrument cannot reach a
TLS-only host at all. `PPCP-RV` erratum E4 (RV 2c1) scopes 2c to the rendezvous *paths* and states
that a conformance harness socket is not one of them; `PPCP-CORE` §3.2's `direct` transport is
conformant plaintext.

The listener side of that is **`Ppcp::Listener::setPlaintextHarness()`**, and every line
implementing it — in `ppcp_transport.h` and `ppcp_transport.cpp` alike — is inside
`#if defined(PP_PPCP_PLAINTEXT_HARNESS)`. That macro comes from one place: the CMake option of the
same name, declared in the application's own `CMakeLists.txt`, **default OFF**, where
`PP_SHIPPING_BUILD` makes it a `FATAL_ERROR` to turn on. `src/Ppcp/tests/CMakeLists.txt` is the only
file in this repository that sets it. A release build compiles no plaintext code path at all —
the same discipline `Connector::connect()` keeps for RV 5.2f, applied to the listener.

ENC §2.1 link binding is unchanged by it: the dialler still mints a `link_id` and sends `link_bind`
first on every stream, and every 2.1c refusal still applies. `TlsOutcome::version` reads
`plaintext-harness`, so nothing downstream and no log line can mistake it for TLS.

⚠ **The macro is set on one TARGET, not on the test directory.** `ppcp_conform_host` is the only
thing in this repository that gets it; `ppcp_transport_test`, `ppcp_link_bind_test`,
`ppcp_host_peer_test` and `ppcp_rendezvous_test` all compile `ppcp_transport.cpp` **without** it.
So "the shipping build still has no plaintext code path" is something this suite checks on every
build, not something somebody verified once by hand.

### 10.3 What the peer under test is, and what it is not

`src/Ppcp/tests/ppcp_conform_host.cpp` is the application's own code reached through the same entry
points `PpcpHostService` uses: `PpcpHostPeer`, `makeHostEngine()` (the one place that says what a
PinPointStudio peer is), `PpcpIngestPolicy` with its real 120 fps floor, `PpcpSourceDeclaration`
with its real `build()` and `validate()`, `PpcpLiveSession`, `PpcpShotBridge`,
`PpcpAnnotationStore`, `PpcpOfferController` and H1's `Listener`.

Three things it deliberately does **not** use, each stated rather than left to be discovered:

- **`PpcpHostService` itself.** It reaches `VideoInputFactory`, which pulls in the AVFoundation
  camera backend and `DeviceEnumerator` (which reaches Bluetooth through `imu_base.h`). Linking the
  device stack into a conformance harness would make the run depend on what hardware is plugged in.
  The composition it performs is performed here line for line, and the `ppcp_app_tu_syntax` row is
  what keeps the service compiling. **Both halves of every fix below landed in both places.**
- **`PpcpSourceDeclaration::hostInventory()`**, for the same reason: it reads `DeviceEnumerator`, so
  the declaration would differ between two machines. The harness declares a fixed camera and a fixed
  microphone — a microphone because CT-I8's second half needs a host Source that nominates, and a
  camera because 3.3d's symmetric declaration needs one that does not.
- **`PpcpRendezvous`.** There is no pairing code on a plaintext socket; no identity is offered and
  none is resolved. RV rows are §4 of this document and are evidence in their own right.

**The Session is the harness's decision, not the application's.** Nothing in `src/` calls
`liveSession().open()` — §7.2 has said so since H5 — so the harness opens the Session, starts the
arbiter and arms, exactly as `tools/scenarios/README.md`'s `reference-host` does. Every row that
depends on a Session is therefore evidence about `PpcpLiveSession` and the engine behind it, and not
about a screen. That gap is unchanged by this work package.

### 10.4 The rows, verbatim

**Regenerated 23 Aug 2026 against `libppcp` at `4d0e04a`** (errata E1–E29 in the normative text;
`ppcp-conform` and `ppcp-sim` rebuilt from that revision). The tool's `--markdown` output, unedited:

```
<!-- generated by ppcp-conform, 2026-08-23 — do not edit by hand -->
<!-- profiles claimed: core capture detect arbitrate live offline markup -->

| Test | Invariant | Profile | Method | PinPointStudio |
|---|---|---|---|---|
| CT-I7 | I7 | Mint, Arbitrate | paired | pass |
| CT-I8 | I8 | Mint, Arbitrate | paired | pass |
| CT-I20 | I20 | Arbitrate | paired | pass |
| CT-I21 | I21 | Live | paired | pass |
| CT-I36a | I36 | Capture | paired | pass |
| CT-S5 | I18 | Core | paired | pass |
| CT-S6 | I24 | Core | injected | pass |
| IOP-5 | I3, 8.2i1 | Core | paired | pass |
| CT-I12 | I12 | Offline | paired | pass |
| CT-S3 | I19 | Core | injected | pass |
| CT-S7 | I31 | Capture | injected | pass |
| CT-I6 | I6 | Mint | injected | n/a |

Every `pass` above came from a command; the commands are in the JSON beside this file, one per row, and each re-runs on its own.
```

What each row put on the wire, from the JSON beside it:

| Row | Counterpart declaration | Scenario | Asserted at exit | ms |
|---|---|---|---|---|
| CT-I7 | `reference-capture.json` | `late-candidate-capture` | `violations=0,t0_revisions=0,shots_rx>=1` | 6013 |
| CT-I8 | `reference-capture.json` | `nominating-capture` | `violations=0,shots_rx>=1` | 6019 |
| CT-I20 | `reference-capture.json` | `reference-capture` | `violations=0` | 5017 |
| CT-I21 | `three-timebase-capture.json` | `reference-capture` | `violations=0,relations_composed=0,probe_timebases=3` | 8020 |
| CT-I36a | `preview-capture.json` | `preview-capture` | `violations=0` | 6023 |
| CT-S5 | `three-timebase-capture.json` | `reference-capture` | `violations=0,relations_composed=0,probe_timebases=3` | 8021 |
| CT-S6 | `observer-core.json` | `observer` | `violations=0` | 5021 |
| IOP-5 | `unrelated-capture.json` | `unrelated-capture` | `violations=0,shots_rx=0` | 6014 |
| CT-I12 | `reference-capture.json` | `offer-session` | `violations=0,offers_tx>=1,accepts_rx>=1` | 8015 |
| CT-S3 | `foreign-capture.json` | `nominating-capture` | `violations=0` | 6012 |
| CT-S7 | `measured-capture.json` | `nominating-capture` | `violations=0` | 6025 |
| CT-I6 | `reference-capture.json` | `nominating-capture` | `violations=0,minted_shots_rx=0` | 6014 |

Every `violations=0` is doing more work than it looks like: `ppcp-sim` refuses, on its own account,
a `shot` re-issued with a different `t0` (I7), a message originated by a peer whose declared
profiles do not confer it (I24), `authority: host` from a `role: capture` peer (I20, 8.3d), a
malformed frame or one past the ENC §8 limit, a held relation spanning two clocks of one peer (I18,
5.4c) and a first frame on a stream that is not `link_bind` (ENC 2.1c).

### 10.5 CT-I6 — the excluded row, and how it stopped being one

**Closed 23 Aug 2026. `run-conform.sh` no longer excuses any row, and `docs/ppcp-conformance.md` no
longer explains one away.**

For two sessions this claim carried an exclusion. CT-I6 is a **negative** row (`CONF` §1d): this
host does not claim Mint, so the tool asserts it parses `shot` with `authority: device` and never
originates one. The counterpart `ppcp-conform` picked was `reference-host.json` running the
`reference-host` scenario — a peer declaring `role: host`. Against a peer under test that is **also**
`role: host`, `PPCP-CORE` 5.2b and `PPCP-MSG` 3.2c *require* the responder to answer `error` /
`role_conflict`, `PPCP-MSG` §10 marks that code **fatal**, and `ppcp-sim` counts a fatal error as a
protocol violation. The row asserted `violations=0` against a counterpart the specification required
this host to refuse. It could not pass, and a host that made it pass would have been violating I20.

That was raised as **F-H8-6, against the instrument**, and libppcp fixed the instrument in S5
(`a371748`): CT-I6 now runs a **minting capture peer** — the only kind that can send the `shot` this
host must parse and not originate — and asserts a new `minted_shots_rx` counter rather than
`shots_rx`. The distinction is the content of the row: a host declaring Arbitrate **may**
legitimately send `shot` (the message catalogue binds it to the SET Mint / Arbitrate) and under 8.2k
it re-sends the DEVICE's Shot unchanged. What Mint confers is issuing on one's **own** authority.

The row now runs `reference-capture.json` / `nominating-capture` with
`violations=0,minted_shots_rx=0`, exits 0 in 6.0 s, and is recorded **`n/a`** — which is `CONF`
§1d's verdict for a negative row that passes, not a row that was skipped. The gate in
`run-conform.sh` is now "any failing row fails this test", with no named exception.

⚠ **The lesson is worth more than the row.** The excuse was correct for two sessions and would have
stayed correct indefinitely, because it was written down carefully and reviewed. A conformance gate
with one documented exception is a gate with one documented exception; the fix was to change the
instrument, and nothing in this repository could have done that.

### 10.6 Five defects the run found, and every one was a composition defect

None of these was visible to `ppcp-tests`, and that is the whole argument of `CONF` §2c. Every suite
in this repository builds one engine, for one link, and declares by hand in its own fixture; the
first counterpart that was not us produced four of these on its first row.

- **F-H8-1 — this host never sent its own `declare`.** MSG 3.3c makes `declare` a precondition for
  originating anything naming a Source, Stream or Candidate, and 3.3d says a host sends it *even
  with an empty `sources` list — it does not skip the message*. `PpcpHostService::start()` built the
  declaration and validated it, and `grep -rn ppcp_peer_declare src/` outside the test tree returned
  **nothing**. A host that never declared cannot nominate from its own microphone (CT-I8) and gives
  a third-party device nothing to convert its instants against (I19). Fixed in
  `PpcpHostPeer::drainEvents()`, on `PPCP_EVENT_CONNECTED`, once per link — the one place both the
  application and the harness go through. **Whose defect: this host's.**

- **F-H8-2 — the F-L13-1 guard could not tell the defect from the fix.** It asserted
  `bulkSeen < slicedSeen` and said in its own comment that it would go red when libppcp L15 landed.
  It did not: a bulk feed yields four events under **both** contracts — four survivors of a ring
  that dropped eight, or four reported before a feed that stopped. Rewritten onto
  `ppcp_peer_events_dropped()`, `ppcp_peer_feed_stalled()` and the short `*out_consumed`, which are
  what actually distinguish them. **Whose defect: this suite's.**

- **F-H8-3 — the listener dropped every byte it had read past `link_bind`.** ENC 2.1a makes
  `link_bind` the *first* frame on a stream; it does not make it the only thing in the first read.
  `ppcp-sim` queues `link_bind` and `hello` together and they arrive in one TCP segment; the bind
  loop decoded the first frame, handed the channel over, and let the rest of the buffer go out of
  scope with it. Every `hello` was silently lost and no link ever got past the handshake — twelve
  rows, twelve links, zero frames received. This repository's own `Connector` writes `link_bind` and
  then nothing until its engine is pumped, which is exactly why talking to ourselves never produced
  the case. The residue now travels with the channel and is served ahead of the socket;
  `drainAvailable()` also stops at one whole frame, so a dialler that queues several no longer
  overruns the bind buffer and gets refused as malformed. **Whose defect: this host's.**

- **F-H8-4 — every `sync_probe` a device sent was answered `error`.** `HostEngineConfig::syncTimebase`
  was never set at the call site, and `ppcp_host_engine.h` is explicit that an empty one means "this
  host does not answer probes"; its own comment then says it should be `tb:host` because that is the
  only clock this application reads (I1). Eighteen errors in one eight-second run. A device could
  never measure its relation to the host, so §6.3 only ever worked in the direction this repository
  happened to test. **Whose defect: this host's.**

- **F-H8-5 — one `ppcp_peer` was built in `start()` and handed to every link.** A peer *is* the
  conversation: the counterpart's declaration, the open Session, the `msg_id` sequence and the link
  state all live in it. The first link got a Session and the next eleven were refused
  `ppcp_peer_session_open: invalid argument`, because the previous Session was still open on the
  engine and nothing had closed it — the link died rather than saying goodbye, which is the ordinary
  way a link ends. In the application this reads as *the second device to pair after a drop never
  gets a Session*. A fresh engine per link, in `PpcpHostService::adoptLink()` and in the harness.
  7.5a's resume is a different case and is not what this was: resume is the same peer on the same
  `K_tls`, opened deliberately, not inherited by whoever dials next. **Whose defect: this host's.**

### 10.7 What this claim does not cover

- **Mint stays unclaimed** and CT-I6 is the only row that would have exercised the negative half of
  it on the wire. §1's negative claim — a `shot` with `authority: device` is parsed, honoured and
  never originated — is still asserted by this repository's own suites and by nothing external.
- **The RV rows of §4 are not in this run.** A plaintext harness socket cannot exercise a pairing
  code, a PSK identity or a handshake refusal, which is the whole reason those rows have their own
  suite and their own evidence.
- **`static` and `fixture` rows are not in this run either**, by the instrument's design: they are
  decidable from a declaration or a recorded stream and belong in the implementation's own suite.
- **No screen was exercised.** The harness opens the Session the application does not yet open, and
  accepts the offer a user would tap. §7.2 and §9.3 remain the record of what is wired and still
  unverified in the application itself.
- **`ppcp_conform_host` is not a shipping binary** and cannot be built into one: it exists only when
  `PP_PPCP_PLAINTEXT_HARNESS` is ON, which a shipping configure refuses.

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

**Updated 23 Aug 2026.** `PpcpHostService` also now owns the RV §3 browser (above), keeps the
`pairingId` the live link resolved to, and publishes a `phones` list — one device row per
pairing this host holds, in the vocabulary `ResourceMonitorController` already builds for a
camera and an IMU, so a paired phone appears in the DEVICES list, the resource monitor and
Settings → Phones rather than in a bespoke table. The phone's declared `product.model` is
written against its pairing in the ordinary settings file (**not** the keychain, which holds
PRK and nothing else per 5.1c); `forgetPairing()` removes it with the key.

⚠ A row is listed for a pairing that is **persisted, or that a link actually resolved to this
run** — *not* for one with `usesRemaining == 0`. `closeSession()` sets `invalidated` and
zeroes `usesRemaining` together, so a code the user merely dismissed is indistinguishable in
the ledger from one a phone spent, and the older rule listed a device for a QR nobody had
scanned.

### 9.4 What is still not claimed

- **`clipReady()` is still not connected**, and for the reason §7.2 gives: a `PpcpClip` carries
  opaque PPCP identities, this application's `Session.id` is a filesystem directory path and
  its `Shot.id` an `int` ordinal, and CORE 8.5c keys idempotent re-import on opaque ids.
  Wiring it today would either duplicate the clip on re-arrival or throw the identity away.
  That is host review item 2 and H-compose does not settle it by accident.
- **`ShotController::setPpcpBridge()` is still not called.** It is a live behaviour change for
  every shot — `reportCandidate()` stops touching `m_arbiter` once a bridge is set — and this
  work cannot run the application to see it. Deliberately left for a wave that can.
- ~~**The macOS keychain path is compiled but not exercised at runtime.**~~ **Closed by
  erratum E56, 25 August 2026.** It read: the login keychain cannot be unlocked from a
  non-Aqua session, so a test that needed it would be red on every headless box, and RT-12 is
  where the keychain path is read. ⛔ **That left the one thing §7.4 rests on unrun on macOS
  and absent everywhere else** — `makePlatformPairingStore()` returned `nullptr` off Apple.
  `RV` 7.2c is now a SHOULD, the store is the application's own INI on every platform, and
  `ppcp_rendezvous_test` exercises it directly: round trip, erase, survival across store
  instances, a short row failing rather than yielding a zero key, and `describe()` carrying no
  key material.
- **Discovery is browse-only and untested against a live responder.** `parseTxtRecord`,
  `pvAcceptsMajor`, `instanceNameMatchesRid` and `decideDial` are asserted; `DNSServiceBrowse`
  is not, because there is nothing on this network advertising `_ppcp._tcp`. 3.6a makes that a
  non-event by construction: discovery failure is not an error state and there is no error
  channel to report one on.

  **Updated 23 Aug 2026 — it now has a caller.** `PpcpHostService::startDiscovery()`
  constructs `makePlatformBrowser()`, watches its `fd()` with a `QSocketNotifier` and runs
  `decideDial()` against `PpcpRendezvous::resolveRid` on every advertisement; a resolved
  pairing is marked `available` in the device list. Until this the browser and `decideDial()`
  were reached only from `ppcp_rendezvous_test`, so the reconnection path RV §3 describes was
  dead code in the application. What is still **not** claimed:
  - `DNSServiceBrowse` against a real responder — unchanged, and unchangeable here.
  - **Nothing dials.** `Ppcp::Connector` exists and 5.2g makes the host the TLS client on the
    discovery path, so reconnecting to a discovered phone is reachable — but auto-dialling on
    sight is a live behaviour change that cannot be seen without a phone on a real network.
    A discovered phone is *shown* as present; reconnecting to it still means §4's code.
  - `makePlatformBrowser()` returns `nullptr` off macOS, so on Windows there is no discovery
    at all. Together with `makePlatformPairingStore()` being macOS-only — no protected
    storage, so `persist()` refuses — a Windows host cannot remember a phone OR find one, and
    Settings → Phones says so rather than showing an empty list.
  - `PpcpHostService::discoveryDescription()` is in the diagnostic export for exactly this
    reason: 3.6a gives discovery no error channel, so "is this host browsing at all" is
    otherwise unanswerable when a remembered phone does not come back.

### 9.5 Findings

| # | Clause | Finding |
|---|---|---|
| 16 | `RV` 7.3a vs `CORE` T1/T2 and `ENC` §2.1 — **F-H6-1** | **`mu` cannot count handshakes, and 7.3a says it does.** "A publisher invalidates a pairing code once `mu` handshakes have completed with it. The default is one." A PPCP link is **two (optionally three) TCP connections**, each its own TLS session keyed by the same `K_tls` — that is CORE T1/T2 and ENC 2.1 and it is not optional. So the default `mu: 1`, which is the pairwise case the entire model is built around, is spent by the control channel's handshake and the bulk channel **of the same link** is then refused: every conformant PPCP-RV pairing dies on its second channel. `mu` can only mean **pairings**. This host counts links (`PpcpRendezvous::noteLinkEstablished()`, called once per accepted `PeerConnection`) and the suite asserts two resolves for one pairing so the arithmetic is visible. Suggested fix: 7.3a reads "once `mu` **pairings** have been established with it", with a note that a pairing is a link and a link is several handshakes. |
| 17 | `RV` 7.3a vs 7.5a — **F-H6-1a** | **Second-order, and it survives the fix above.** 7.5a has a reconnecting peer complete a full handshake on the same derived `K_tls` without a new code; 7.3a invalidates the code after `mu` completions; 7.5c then says `session_resume` is refused for a session whose pairing was invalidated under §7.3. Read literally, a `mu: 1` code permits one link and no reconnection at all, which makes §7.5 dead letter in the default case. This host resolves it by treating 7.3a as spending the **code** and 7.3b as ending the **pairing**, so a session survives its code and dies with its session. §7.3 should say which of the two it means. |
| 18 | H1's API — **F-H6-2, closed here** | `ResolvedPairing::pairingId` is documented in `ppcp_transport.h` as "the embedding's handle on WHICH pairing authenticated a stream", the listener has held it since H1, and there was **no accessor**. An embedding that had to act on it — and RV 7.3a's single-use defence is exactly an action on it — could not find out which code had just been used. `TransportChannel::pairingId()` and `PeerConnection::pairingId()` now expose it. |
| 19 | `RV` 7.2c vs this application — **F-H6-3, superseded by erratum E56** | **Recorded as a trap; the ruling has since gone the other way, and the finding's own reasoning contained a factual error.** It read: the application's existing secrets subsystem is not protected storage and using it would have been the easy mistake — `src/Secrets/SecretsManager` keeps API keys in `QSettings`, *"which on macOS is a plain plist in `~/Library/Preferences`"* — so H6 reached Security.framework directly and returned **no store at all** where the platform had none. ⚠ **The parenthetical was wrong**: `ppSettings()` pins `QSettings::IniFormat`, so it is one INI file on every platform and never a plist; `SecretsManager.h`'s own header said otherwise and has been corrected. ⛔ **And the conclusion cost more than the trap it avoided.** Returning no store off Apple meant **Windows and Linux could persist no pairing at all**, so `RV` §7.4 and §7.5 were dead letter on the platforms almost all of this application's users are on. libppcp erratum E56 makes 7.2c a **SHOULD**; the `PRK` now lives in the same INI, on every platform. **The half of the finding that survives is the one that was never about storage**: a `PRK` must not go behind `SecretsManager`'s environment-variable override and compile-time default — an env var that can inject a pairing root key is a defect, not reuse. The settings *handle* is reused; the secrets *facade* is not. |
| 20 | `libppcp` — **F-H5-1, closed** | The remote half of I21 is reachable: `ppcp_peer_sync_add_target()` keys sequences on the (local, remote) pair, and erratum E2 has a probe naming a timebase the responder declared answered on that timebase. Not yet exercised from this host, which has one clock and currently probes with `ppcp_peer_sync_add_timebase()`; the multi-clock device case is `libppcp`'s CT-I21. |
| 21 | `libppcp` — **F-H5-2, closed** | `ppcp_peer_session_params()` now answers on the peer that ORIGINATED `session_open`, so a host can read back the Session it just opened. `ppcp_live_session_test` asserts the two ends agree rather than asserting the host is blind, which is the property the finding was about. |
| 22 | `libppcp` — **F-H5-3, closed, and it broke six suites** | It came back as a **hard precondition**: `ppcp_peer_new()` now refuses a peer that declares `live` with no `health_report` — "a peer that has no thermometer declares no Live". That is exactly what this repository asked for, and six suites went red at once because the host and the harness peers all declared `live` and supplied nothing. `PpcpHostPeer::makeLibppcpEngine()` now builds a report from the two callbacks it already had, and `makeHostEngine()` drops `live` where the embedding supplied no reading — a profile is a promise about behaviour (2.2.2), and the bundle path has no heartbeat to answer anyway. |
| 23 | `libppcp` — **F-L13-1, closed, with a harness consequence** | `ppcp_peer_feed()` now stops rather than overrun the event ring. The consequence for a test harness is that "pipe everything, then look at the events" no longer works: three assertions in this repository had been reading whatever survived a four-deep ring. `pptest::pipe()` takes an event sink and drains between frames — which is what `PpcpHostPeer::pump()` has always done in production — and **stops rather than skipping** a frame it cannot feed, so the next one to get this wrong fails loudly. |

---

## 11. Interoperability — the `PPCP-CONF` §5 rows (session 5, wave 1)

**A different claim from §10, and §5 opens by saying why both are needed:** "conformance to the
document is necessary and not sufficient — two implementations that each pass §3 and §4 alone can
still fail to interoperate." §10 asks whether this host passes every row the instrument holds for
the profiles it claims. These rows ask CONF §5's question instead: does this host interoperate with
a peer of a **stated shape** — an observer that owns nothing, a clock declared `unrelated`, a device
that nominates, a device with a preview it throws away.

The counterpart is `libppcp/tools/ppcp-sim` throughout wave 1, over the same plaintext harness
socket §10.2 justifies. Wave 2's pairing — this host against PinPointCapture on the simulator, over
real TLS — is §11.4.

### 11.1 The rows

`ctest --test-dir build/ppcp-tests -R ppcp_interop` runs all eight. Each is
`src/Ppcp/tests/run-interop.sh ROW <ppcp_conform_host> <ppcp-sim> <scenarios> <workdir> [device-bundles] [fixtures]`,
and each asserts **on both ends**: the counterpart's view as `ppcp-sim --expect` (its exit code),
and this host's own view as the JSON summary `--summary` writes. A row that read only the
simulator's counters would pass for a host that received everything and concluded nothing; one that
read only ours would pass for a host talking to itself.

| Row | Counterpart | Principally proves (CONF §5) | Outcome |
|---|---|---|---|
| IOP-3 | **two bundles PinPointCapture wrote**, no socket | I20, I23, I16, I9 | **pass** — see §11.3 |
| IOP-3-live | `reference-capture.json` / `offer-session` | **E28** — a replayed Session is imported, not merged | **pass** — see §11.6 |
| IOP-4 | `observer-core.json` / `observer` | I24 | **pass** |
| IOP-5 | `unrelated-capture.json` / `unrelated-capture` | I3, 8.2i1, and CONF 5b | **pass** |
| IOP-6 | `reference-capture.json` / `nominating-capture`, this host nominating | I8 | **pass** |
| IOP-7 | `reference-capture.json` / `nominating-capture`, this host never issuing | I32 | **pass** |
| IOP-8 | `reference-capture.json` / `nominating-capture`, this host delayed 3 s | I35 | **pass** |
| IOP-9 | `preview-capture.json` / `preview-capture` | I36 | **pass** |
| IOP-10 | this host's own bundle, written then read | `ENC` 7a | **pass, both directions** — see §11.3 |

Measured 23 Aug 2026 against `libppcp` at **`4d0e04a`** — revision 9 plus **errata E1–E29**, all
normative — with `ppcp-conform` and `ppcp-sim` rebuilt from that revision.
`ctest --test-dir build/ppcp-tests` is **23/23**. §11.6 is what adopting E7, E9, E21, E25, E28 and
E29 changed in this repository, and what it caught.

### 11.2 What each row actually asserted, and the numbers it got

Every number below is from the row's own `--summary` JSON in `build/ppcp-tests/interop/`.

- **IOP-4 — I24.** `declares_rx 1`, `candidates_rx 0`, `shots_rx 0`, `issued 0`, `heartbeat_acks 6`,
  `errors_fatal 0`. The observer originated nothing past `hello`, `declare` and its acks; this host
  neither required it to nor treated the link as dead — the six acks are the transport still open at
  the end. The simulator's half asserts the same from the other side (`violations=0`,
  `candidates_tx=0`, `shots_tx=0`).
  - ⚠ **This row found a defect in the harness, not in the host.** The first run failed on
    `errors_fatal`, because the summary was classifying fatality from `ppcp_event::status` — which
    also carries the reason the engine itself raised an event. `MSG` §10 makes exactly **two** codes
    fatal, and `ppcp_msg_error_is_fatal()` is the answer; classifying on `status` reads every
    `profile_not_supported` as a lost link, which is precisely the answer CT-S6 assertion 2 says is
    wrong. Fixed in `count()`, and the codes are now recorded verbatim in `error_codes`.
- **IOP-5 — I3, 8.2i1, CONF 5b.** `candidates_rx 1`, `retained 1`, `issued 0`, `groups 0`, and
  **`counterpart_offsets 0`**. The last is the row: CONF 5b asks that a host "never substitutes a
  zero offset", and a substituted zero is invisible on the wire because it is a number shaped like
  every other number. So the host is asked directly — for each clock the counterpart declared,
  `PpcpLiveSession::offsetToRefNs()` is called and the answer recorded. For `tb:unrelated` it is
  `has_offset: false`, not `true` with `0`.
  - **Recorded, not a failure: `excluded` is 0 and `retained` is 1.** CONF §5's wording is "the host
    excludes and retains every Candidate". In this host's counters `excluded` is 8.2d's
    conversion-uncertainty exclusion, and a peer with **no relation at all** never reaches that
    test: the Candidate is held with no Shot, which is 8.2i1's own words ("a missing or `unrelated`
    relation leaves not even an instant to group by"). Both clauses are satisfied and the outcome is
    the one 8.2i1 describes; the two counters simply are not the same counter. Worth a sentence in
    CONF §5 that "excludes" there means 8.2i1's retention-without-grouping, not 8.2d's exclusion.
- **IOP-6 — I8.** `nominated 1` (this host's own microphone), `candidates_rx 1` (the device's),
  `issued 1`, **`max_shot_candidates 2`**, and the simulator saw the same Shot with
  `shot_candidates_max 2`. Two nominators of one `basis: acoustic` from two peers, both on one Shot
  — the assertion the `ShotArbiter` this bridge replaced could not have met, because its
  per-modality slot keeps one of them and drops the other silently.
  - The synthetic onset is raised when the device's Candidate arrives and stamped with this host's
    own clock, which on loopback is inside 5.10's 50 ms coincidence window. `tof_correction` is
    null: 8.1e forbids inventing one.
  - ⚠ **This row found a real API trap and the library caught it.** The first run passed the
    inventory's device id as the Source and was refused — `no declared Source named
    pps-conform-mic-0 (I26)` — because `PpcpSourceDeclaration` prefixes a microphone's Source id
    with `mic:`. I26 doing its job. The harness now reads the id back out of the declaration
    instead of repeating a convention that lives in another file.
- **IOP-7 — I32.** `issued 0`, `candidates_rx 1`, `shots_rx 1`, `adopted 1`. The arbiter is built
  and 8.2h is never run, so the only thing that can fire is the device's own 8.2i deadline — and
  this host then **attaches** to what it minted rather than ignoring it.
- **IOP-8 — I35.** `issued 0`, `adopted 1`, `shots_rx 1`, and the simulator `minted 1` with
  `t0_revisions 0`. The margin is stated rather than assumed, because libppcp's own log records the
  trap: the deadline is `issue_hold_ns + heartbeat_interval_ms` after the Candidate, and a host
  delayed by only a little more wins the race and the run looks like an ordinary arbitration while
  asserting nothing. Here the deadline is 1.2 s (`--issue-hold-ms 200 --heartbeat-ms 1000`) and this
  host is delayed 3 s.
- **IOP-9 — I36.** `streams_rx 3`, `preview_streams_rx 1`, `continuous_streams_rx 2`,
  `captures_rx 2`, `captures_absent 1`, `captures_not_retained 1`, and **`preview_payload_frames 0`**.
  The discarded preview is announced `absent` / `not_retained` (5.11c3) rather than as a gap, and no
  preview Capture ever carried a payload frame — a measured zero for 5.11j's "the preview does not
  reach a bundle", rather than an assumption about what the device stored.

### 11.3 The two bundle rows

Both are now the real pairing. Updated 23 Aug 2026, after the PinPointCapture agent checked its
bundles in.

**IOP-3 — two bundles the DEVICE wrote, each read twice.** `run-interop.sh` reads **every** file in
`PinPointCapture/docs/conformance/bundles/`, not the first one: the device checks in more than one
shape on purpose, and reading only the first would leave the other unread with the row still green.
Each bundle gets its own ledger and its own import root, because I34's claim is that a second read
of the *same* bundle admits nothing new — running two Sessions through one ledger would let a
miscount in either hide behind the other's totals.

| Bundle | Frames | Streams | Captures | Second read | Completeness | Clips | Commits owed |
|---|---|---|---|---|---|---|---|
| `ses-interop-one-shot.ppcpbndl` | 13 | 2 | **3** new | **0 new, 3 already held** | `partial`, asserted | 0 | 0 |
| `ses-interop-two-shots.ppcpbndl` | 20 | 2 | **6** new | **0 new, 6 already held** | `partial`, asserted | 0 | 0 |

Both: `manifest_ordered` true, `truncated` false, `digest_conflicts` 0, `captures_unattributable` 0,
owner `peer:1b4b06fd-…` on both passes, and the ledger holds the Session. Nine Captures imported in
total, none twice.

- **I34 is asserted over the ledger, because nothing on the wire could say it.** Second read: 0 new,
  and the already-held count equals the first read's total exactly.
- **I10 / `ENC` 7d — `partial` is honoured as `partial`.** The device asserts it (no camera in the
  simulator, so every Capture is `absent`/`outside_buffer`) and neither bundle is truncated, so
  there is nothing for 7d to resolve. The row asserts that an assertion and a truncation are never
  seen together without being read by hand, because 7d resolves them in exactly one direction: an
  observation may **downgrade** a Session nobody asserted anything about and may never **upgrade**
  one the owner called incomplete.
- **⚠ 5.14h — `capture_committed` is queued 0 times, and that is the conformant answer here.** The
  invariant the row asserts is **one commit per clip written**, and both bundles wrote zero clips:
  every Capture is `absent`/`outside_buffer`, so there is no payload and `PpcpImportSink` queues on
  `payload_end` and on nothing else. Queueing a commit for an absent Capture would confirm bytes
  that were never sent, and `MSG` 8.4b puts `confirmed` outside the owner's own authority precisely
  so that cannot happen. The 5.14h path itself is exercised where there *is* a payload:
  `ppcp_bundle_import_test` asserts `commitsQueued == 1`, that the queue is scoped to the **minting**
  peer (`pendingCommits("peer:dev")` non-empty, `pendingCommits("peer:someone-else")` empty), and
  that it survives a ledger reload. Nothing about that is left to the interop row.
- The row also refuses to pass on an empty read: `READ == 0` or nine-Captures-becomes-zero both fail
  it, so a bundle directory that silently emptied cannot look like a green row.

**IOP-10 — both directions, and the device closed the other one.** Phase 1 runs the IOP-6 pairing
with `--write-bundle`, so the file carries a real Session: `session_open` with **both** arbitration
parameters (5.10e — the structural statement that this Session has a host), this host's own
`declare`, the Shot it issued over two Candidates, `session_state: closed/complete` and
`session_manifest`. Five frames, 1657 bytes, checked in at
`docs/conformance/bundles/pinpointstudio-host-session.ppcpbndl`. Phase 2 reads it back through the
same offline path IOP-3 uses. **The PinPointCapture agent has now read that same file clean on its
side**, which is the direction this repository cannot assert for itself, so `ENC` 7a's "live and
file are one format" holds across both writers and both readers.

- That bundle carries **Shots and no Captures**, and the row's "at least one Capture" guard is
  deliberately scoped to IOP-3 for that reason: this host owns no capture Stream — it arbitrates
  over a device's — and a Session of Shots with no Captures is exactly what a hosted Session looks
  like from the arbitrating end. Requiring a Capture there would be requiring the host to own a
  camera.

### 11.4 Wave 2 — `run-tls-host.sh`, and what it is for

The pairing CONF 5a actually names first is reference device ↔ reference host, and neither of those
is `ppcp-sim`. That run must not go over the plaintext harness socket — it would be measuring a
transport neither product ships — so `ppcp_conform_host` gained `--tls-psk HEX --tls-identity TEXT`,
which takes the harness option off and stands up `Ppcp::Listener` with an `IdentityResolver` exactly
as `PpcpHostService` does. `src/Ppcp/tests/run-tls-host.sh` is its driver:

```sh
src/Ppcp/tests/run-tls-host.sh PORT PSK_HEX IDENTITY [RUN_MS] [SUMMARY_JSON]
```

- **PORT** — 0 takes an ephemeral one; the chosen port is written to `$SUMMARY_JSON.port` and
  printed as `run-tls-host.sh: PORT <n>`, so the dialling side can read it.
- **PSK_HEX** — `K_tls` (RV §5.1) as 64 hex characters. On the product path it is derived from the
  pairing code; here it is given, because two agents cannot share a QR code.
- **IDENTITY** — the PSK identity (RV §5.3, §10.2) to accept, as text. The literal **`any`** accepts
  every identity, which is what to use when the dialling side derives a per-connection identity
  carrying a rotating `rid` — a script cannot know that value in advance. Anything else is an exact
  match, and a mismatch is refused indistinguishably from an unknown identity (RV 5.3d).
- **RUN_MS** — default 120000. **SUMMARY_JSON** — default `./pps-tls-host.json`.

The host declares its Sources, accepts the session, opens it, arms, arbitrates, and writes the same
flat JSON summary every §11.1 row is judged from: `link_up`, `session_opened`, `arbiter_started`,
`declares_rx`, `candidates_rx`, `nominated`, `issued`, `adopted`, `late`, `excluded`, `retained`,
`max_shot_candidates`, `shot_ids`, `streams_rx`, `preview_streams_rx`, `captures_rx`,
`captures_absent`, `captures_not_retained`, `payload_frames`, `offers_rx`, `offers_accepted`,
`errors_rx`, `errors_fatal`, `error_codes`, and `counterpart_timebases` (per clock: does this host
claim a reading, and what). Exit 0 means the run completed and the summary exists — **it does not
mean the pairing passed**; the row is judged from this summary and the dialling side's report
together.

**Verified so far:** the listener completes a real TLS 1.3 external-PSK handshake —
`openssl s_client -tls1_3 -psk_identity … -psk …` negotiates `TLS_CHACHA20_POLY1305_SHA256` against
it and then goes no further, correctly, because it sends no `link_bind`. What is **not** verified is
a full PPCP session over it: that is wave 2, and it needs the other product.

### 11.5 What these rows do not cover

- **IOP-3 and IOP-10 no longer have an open half**, but they are still *bundle* rows: neither says
  anything about the live link between the two products. That is IOP-1, and it is wave 2.
- **IOP-1 and IOP-2 are not this host's to run in wave 1.** IOP-1 is the real pair (wave 2); IOP-2
  is a reference **device** against a synthetic third-party host, so PinPointStudio is not a party
  to it at all.
- **CONF 5c** — a pairing by an implementation not written by the reference team — remains open, and
  nothing here moves it.
- **The Session is still the harness's, not the application's.** §7.2 has said since H5 that nothing
  in `src/` calls `liveSession().open()`; the three decisions the `reference-host` shape needs —
  open, arm, arbitrate — are made in `ppcp_conform_host.cpp`. Every row above is therefore evidence
  about `PpcpLiveSession`, `PpcpShotBridge` and `PpcpHostPeer`, and not about a screen.
- **`--never-issue` and `--issue-delay-ms` do not call `PpcpHostPeer::tick()`.** They call its four
  other steps directly and skip or defer 8.2h's issue step — which is exactly what `ppcp-sim`'s
  `silent-host` (`SIM_F_NEVER_ISSUE`) and `late-host` (`arb_delay_ms`) do on the other side of the
  same pairing. No other row takes that path: with neither option the call **is** `tick()`.

### 11.6 Adopting libppcp L17's errata (23 Aug 2026, `4d0e04a`)

Six errata reach this repository. Each is recorded here with what changed and, where the change was
provoked by a run rather than by reading, what it caught.

**E28 / F-S5-3 — a replayed Session is imported, and the live Session does not move.** The most
serious of the six, and it was invisible from inside one implementation. `MSG` §9.1 lets a device
offer a Session it recorded earlier and replay its bundle down the link a **live** Session is
running on. The replayed `session_open` names a different `session_id`, and `CORE` 4.1a's
immutability rule was written about *the same* id — so an implementation reading it literally
guarded nothing, and the host's `timebase_ref` was silently rebound to the exporting device's clock.
Every subsequent `t0` was then expressed in a timebase the live Session had never declared, and two
Sessions' Candidates were arbitrated as one. Nothing on the wire was malformed and no counter moved.

- `ppcp_event::imported` now routes it. **`PpcpShotBridge::observe()` returns before its switch** on
  an imported frame and counts it in `Stats::importedIgnored`; the guard is one line at the top
  rather than a test per case, because the next message type added to that switch would otherwise
  inherit the defect. **`PpcpLiveSession::observe()` returns too** — a replayed `relation_update`
  relates two clocks of a Session that is over, and folding it into the live relation set would put
  a stale offset on the seam every camera reads.
- Asserted twice. `PpcpArbitration.AnImportedCandidateIsCountedAndNeverArbitrated` is the branch,
  including the negative half — the same event with the flag clear **is** arbitrated, so the test
  cannot pass for a bridge that simply stopped observing Candidates. The new **`IOP-3-live`** row is
  the real thing over a real socket, and its numbers are the point:

  | | at open | at exit |
  |---|---|---|
  | live `session_id` | `sess:2de64f8f-…` | `sess:2de64f8f-…` |
  | live `timebase_ref` | `tb:host` | `tb:host` |
  | imported `session_id` | — | `sim:device/stored` |
  | imported `timebase_ref` | — | **`tb:dev`** |

  `tb:dev` is exactly the value that used to overwrite `tb:host`. Three imported frames were kept
  away from the live arbiter, one Capture and three payload frames were received, and `issued` is 0.
  ⚠ The assertion is a **comparison, not a counter**, and "empty at both ends" is deliberately not
  "unchanged" — that would pass for a host that never opened a Session at all.

**E29 / F-S5-1 — a Candidate retained for want of a relation is reconsidered when it arrives.** 8.2d
says exclusion is a conclusion and said nothing about the relation arriving a moment later, which on
a live link it *always* does while §6.3's burst converges. `PpcpShotBridge::reconsider()` calls
`ppcp_arbiter_reconsider()`, and it is called from two places because 6.3d makes them two: from
`observe()` on a `relation_update` **arriving from the peer**, and from `PpcpHostPeer` whenever this
host's own estimator **publishes** one.

- ⚠ **The old behaviour was silent and looked correct**: no error, no Shot, and every Candidate
  present in `retainedCount()` exactly as 8.2d requires. A host could have run a whole Session
  arbitrating nothing and every assertion in `ppcp_arbitration_test` would still have passed —
  including `WithNoRelationEveryForeignCandidateIsRetainedAndNoneIsGrouped`, which asserts the
  first half of the same behaviour and is still correct.
  `ACandidateRetainedForWantOfARelationIsReconsideredWhenItArrives` is the second half.

**E25 — one range syntax for `pv` and `detail.supported`.** `RV` 3.3d: `LOW` or `LOW-HIGH`, each
endpoint `MAJOR.MINOR`, both inclusive, sharing a MAJOR; several ranges comma-separated across
MAJORs, most preferred first. `hello.versions` stays an ordered **list**.

- ⚠ **The parser this replaced would have failed a real discovery, and it passed its test.** It read
  bare MAJORs, so `1.0-1.2` worked **by accident** (both endpoints parse to 1 under `strtol`) and
  the existing test asserted exactly that case. `2.0-2.1,1.4-1.6` did not work: the high endpoint
  ran to the comma, giving the range 2..2, and a peer offering major 1 was refused with nothing on
  either side to say why. `PpcpRendezvous.ThePvRangeSyntaxOfErratumE25` covers the comma case, the
  shared-MAJOR rule, `HIGH` below `LOW`, and 3.3d's closing rule that a reader which cannot parse a
  range **ignores the advertisement rather than guessing** — so one bad component poisons the whole
  record instead of being skipped.
- This repository **parses** `pv` and does not publish one, and it never constructs
  `detail.supported`; the writer half of E25 is not ours.

**E21 — no `0x00` octet in a PSK identity.** `PpcpRendezvous::drawPskIdentity()` wraps
`ppcp_rv_psk_identity_draw()` over this class's CSPRNG, and the header says in as many words that
nothing in this application may call `ppcp_rv_psk_identity()` for a connection: that entry point
does not reject a zero-bearing draw, on purpose, so §10.2's vector still reproduces byte for byte —
which makes it correct for a vector and wrong for a socket.

- ⚠ **One draw is not evidence**, and that is why the defect survived a session of manual testing: a
  zero appears in about one identity in sixteen. `ADrawnPskIdentityCarriesNoZeroOctet` takes **256**
  draws, and then resolves a drawn identity against the publisher that issued the pairing — without
  that second half the test would pass for a draw producing 17 zero-free bytes of nonsense.
- Today this host is the **server** on the pairing-code path (the scanner dials and draws its own),
  so the entry point exists ahead of the discovery-path dial rather than behind a live call site.
  That is deliberate: the unsafe function is the one with the obvious name.

**E7 — `payload_begin.container`.** The gap this repository reported in S2 is closed, and the
comment in `ppcp_import_sink.cpp` that recorded it is replaced by the clause that answers it. The
declared IANA media type now names the clip on disk; the Stream-kind table survives only for 6g's
remaining case — raw samples the profile describes in full — so it is a fallback with a clause
behind it rather than a guess. An unrecognised media type uses its subtype verbatim rather than
being mapped to something it might not be (6h forbids inferring a container, and naming a file is
not deciding what is in it). `Stats::payloadsWithContainer` counts them, so a sender that has not
taken E7 yet is visible rather than silently falling through.

**E9 — `declare` before any frame naming a Capture, Stream, Shot or Candidate.** The host bundle
writer already emitted `session_open`, `declare`, then the Shots, so the new rule cost nothing here;
`ppcp_bundle_writer` now **enforces** it, which is the better guarantee.
`docs/conformance/bundles/pinpointstudio-host-session.ppcpbndl` is regenerated at `4d0e04a`
(1657 bytes, 5 frames, `declare` at offset 424 and the `shot` at 1030).

**What regressed: nothing.** 23/23 on the first build after the six changes, including
`ppcp_conformance` with the CT-I6 exclusion removed. Two things that could have looked like
regressions and are not: `WithNoRelationEveryForeignCandidateIsRetainedAndNoneIsGrouped` still
passes and still should — E29 is about what happens *next*, not about that assertion; and CT-I6 is
recorded **`n/a`**, which `CONF` §1d defines as a negative row that **passed**, not one that was
skipped.
