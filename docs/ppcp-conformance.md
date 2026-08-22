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
