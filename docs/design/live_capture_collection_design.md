# Live capture collection — the missing swing-video leg

**Status**: Phases 0–2 built and committed. ⛔ **The video leg is NOT proven end to end — no
clip has ever been filed.**
➡ **START AT `live_capture_handover.md`**, beside this file. It states the one remaining open
question, the diagnostic that settles it, three theories already disproved by measurement, and
how to run the automated rig. This document is the design; that one is the current position.
**Written**: 1 September 2026, from the manual PPS + PPC wrist session of that afternoon.
**Amended**: 1 September 2026 — see §0.5. Seven statements below were wrong or have been overtaken;
they are corrected in place and listed there.
**Touches**: PinPointStudio. **Not** PinPointCapture and **not** libppcp: both are complete for
the core loop (§3), and the two items once listed against them are hardware verification, not code.

> **This document is self-contained.** It is written to be picked up by a session with no prior
> context: §0 orients, §1–§4 establish the problem and the evidence, §5–§7 are the design, §8
> is the work broken down with acceptance criteria, §9 lists what is genuinely unresolved, and
> **§10 is where it actually stands after the first hardware session — start there.**
> Every file reference was verified against the tree at commit `06e92fe`.

---

## 0. Orientation — start here

### 0.1 The three repositories

| Repo | Path | Role |
|---|---|---|
| **PinPointStudio** (PPS) | `~/Projects/PinPointStudio` | The desktop host. Qt 6.11 / C++20. **All the work is here.** |
| **PinPointCapture** (PPC) | `~/Projects/PinPointCapture` | The iOS capture app. Swift. Read-only for this work. |
| **libppcp** | `~/Projects/libppcp` | Shared C11 sans-I/O protocol library, embedded from the sibling checkout. Read-only for this work. |

PPS resolves libppcp from `../libppcp` automatically (`PP_LIBPPCP_LOCAL=ON` by default). If the
sibling checkout is missing, the whole PPCP transport silently does not build and none of this
work is reachable — check the configure log for `libppcp: not embedded`.

### 0.2 Build and test

```sh
cd ~/Projects/PinPointStudio
cmake --build build/Qt_6_11_1_for_macOS_Debug --target PinPointStudio -j 8   # ~47 s
ctest --test-dir build/ppcp-tests -R 'ppcp_video_input|ppcp_arbitration|ppcp_host_service'
```

⚠ **Always pass an explicit `-j`.** A bare `-j` (or bare `ninja`) is unbounded and OOMs the
machine.
⚠ **Never run a test binary bare** — run it through `ctest`, which supplies environment the
suites need. A bare binary fake-fails or segfaults.
⚠ **Do not use the `macos-release-arm64` preset** — it is serial Makefiles and takes forever.
⚠ **Budget 2–3 builds for a task**, not one per edit. Run only the directly affected suites,
once, at the end.

The PPCP suite is 28 tests (`ctest --test-dir build/ppcp-tests -N` to list). The ones this work
touches: `ppcp_video_input_test`, `ppcp_arbitration_test`, `ppcp_host_service_test`,
`ppcp_bundle_import_test`.

⚠ `ppcp_host_service_test` (#17) times out roughly two runs in three when it follows the
advertise suite. One sample either way will mislead you.

### 0.3 Ground rules that bite this work specifically

- **One log.** Everything diagnostic goes through `ppWarn()` / `ppInfo()` / `ppDebug()` to the
  single application log. No device-specific channels. Act-now messages are session-screen
  toasts — which §7 is entirely about.
- **No menus, no native dialogs.** No `FileDialog`/`FolderDialog`, no menu items. Any "fix this"
  affordance in §7 must be in-app navigation (e.g. open the Storage settings panel).
- **Do not touch `ScreenSessionWizard.qml` or `ImuCalibrationFlow.qml`** without explicit
  per-change approval.
- **Analysis is session-agnostic.** Gate on available data and devices, never on `sessionType`.

### 0.4 Read these before writing code

In this order. They carry the reasoning that makes the rest of this document short.

1. `src/Ppcp/ppcp_host_service.cpp:195` — why `clipReady()` is not connected. ~15 lines.
2. `src/Ppcp/ppcp_import_ledger.h` — the whole header. This is the mechanism §5 extends.
3. `src/Ppcp/ppcp_import_sink.h:36-46` — the "what is not here" note; the same refusal, for bundles.
4. `src/Video/VideoInputPpcp.h:91-118` — `PpcpClip`, what actually arrives.
5. `src/Video/VideoInputPpcp.cpp:1533` — `deliver()`, the preview/clip fork.
6. `src/Gui/shot/shot_controller.cpp:470-510` — the arbitrated-shot path, where the ask belongs.
7. `src/Gui/cameras/camera_instance.cpp:738-758` — why a preview frame must never reach the ring.
8. `src/Export/swing_paths.h` — the derived identity, the other half of the clash.

### 0.5 What changed since this was written

Verified against the tree on 1 September 2026, after §0.4's reading and a pass over all three
repos. Each is corrected in place below; they are collected here because a session picking this
up cold would otherwise trip on them in order.

1. **The swing directory does not exist at commit time.** §5.3's sequence showed `commitShot`
   allocating it. It is allocated in `buildSwingExportJob()` (`shot_processor.cpp:1668`), called
   from `startSwingSave()` at `finishGatherAndLaunch` — **4–11 s after the shot**, behind the
   post-roll and a 7 s deferred-history gather. Waiting for it before asking the phone would burn
   that much of a finite ring for nothing. **Ask at `shotDetected`; file when the directory
   lands.**
2. **A correlation key is needed and does not exist.** `shotDetected(source, impactUs,
   sessionType)` carries no PPCP shot id, and `shotProcessed(int shotId, QString swingDir)` uses
   an `int` ordinal, so PPCP-shot-id → swingDir is not currently expressible. A single-slot
   correlation is sound and provable: `armed()` includes `!m_processorBusy`
   (`shot_controller.cpp:120`), so exactly one shot is ever in flight.
3. **`PpcpClip` has no container field.** `onPayloadBegin()` never reads
   `payload_begin.container`, so H-d's "extension from `payload_begin.container`" needs the field
   added to `PpcpClip` first. ⚠ The comment at `VideoInputPpcp.cpp:1553` claiming ENC §6 declares
   no container is **stale** — ENC 6g / erratum E7 carries one and `ppcp_import_sink.cpp:228`
   already reads it.
4. **Stream ids are hashed.** The real shape is `st:<16 hex of SHA1(peerId|sourceId)>:<video|
   preview>`, from `VideoInputPpcp::streamIdFor()`. ⚠ The comment at `VideoInputPpcp.cpp:886` and
   the literal in `ppcp_arbitration_test.cpp:428` both show the pre-hash `st:<peer>:<src>:video`
   and would mislead anyone building an id by hand. **Use the helper.**
5. **Open question §9.4 is answered — CR-02 landed after this was written.**
   `PPCP_EVENT_BUFFER_STATUS` (MSG 5.6) carries a per-stream `retained_from` and retention
   target, already decoded into `BufferMargin` at `ppcp_live_session.cpp:406`. The host can clamp
   `pre`/`post` to what the phone actually retains instead of taking `outside_buffer` for the
   whole clip.
6. **L-b is answered; libppcp needs no change.** `PPCP_EVENT_SESSION_RESUME` delivers
   `ev.msg->body.session_resume.pending[]` to a host like any other event. L-b is a
   documentation and verification item, not an API finding.
7. **PPC mints a fresh `captureId` per request** (`RecordingSession.swift:704`), so a re-request
   after a timeout yields a *different* opaque id and ledger dedupe on `captureId` alone will not
   catch it — dedupe must also hold at the swing level (shot id + alias). It also **ignores the
   requested `streamIds`** and serves its single `videoStream`.

And one structural finding that shapes H-c: **`activeShotBridge()` returns the FIRST phone's
bridge** (`ppcp_host_service.cpp:788`, a limitation its own header documents), so a request
issued through `ShotController`'s single pointer can never name a second phone's Streams.

---

## 1. The flaw, in one paragraph

A phone paired to PinPoint Studio can join a live session as a camera, and today it never
delivers a single frame of swing video. The host detects the shot, corroborates it, freezes a
window, runs the analyzer and writes a swing — and the phone, which is holding the footage in a
ring buffer and is fully able to serve it, is never asked for it. The leg that asks is not
built, the leg that would store the answer is deliberately disconnected, and neither the log nor
the UI says so. What the user sees is a shot that "failed to save", which is a different
statement and an untrue one.

---

## 2. The evidence

A wrist session on 1 September 2026: one iPhone 16 over USB, audio-triggered shots, PPS at
`06e92fe`, PPC at `c382888`. Raw logs preserved at `PinPointCapture/diags/2026-09-01-wrist-ppcp/`.

| | |
|---|---|
| Shot events offered by the phone | 16 |
| Refused — no host detector agreed within 50 ms | 4 |
| Arbitrated | 12 |
| Dropped, "still processing the previous shot" | 2 |
| Reached the shot pipeline → windows captured | 10 |
| **Windows containing any camera frame** | **0** |
| **Bytes ever moved on the bulk channel** | **0** |

The decisive log line, on link teardown:

```
[ppcp] link ended for "iPhone 16" — … | bytes in/out — control 36183/19267  bulk 0/0  preview 377144/0
```

`bulk 0/0` is the finding in four characters: the bulk channel exists, is open, and has never
carried a byte. Preview moved 377 KB in the same period.

Everything either side of the missing leg worked: pairing, the cabled link, TLS-PSK, preview at
640×360@30, clock convergence to 0.55 ms (well inside the 5 ms gate), shot arbitration, and
acoustic corroboration.

⚠ A second, unrelated fault ran through the same session: `/mnt/swingdata` was not mounted (a
macOS upgrade had wiped the `/mnt` line from `/etc/auto_master`), so all 10 exports failed
anyway. That is an environment fault, **not** part of this design — but the way it was reported
is, and §7 is about that.

---

## 3. What is built and what is not

This is the part that changes the shape of the work, and it is the opposite of what the symptom
suggests.

| Leg | Where | State |
|---|---|---|
| Open a capture Stream, `shot_windowed` | PPS `VideoInputPpcp::start()`, `VideoInputPpcp.cpp:931` | **built** |
| Ask for a clip around a `t0` (MSG 7.3) | PPS `PpcpShotBridge::requestCapture()`, `ppcp_shot_bridge.h:252` | **built, never called** |
| Carry the request | libppcp `ppcp_peer_capture_request()`, `include/ppcp/shot.h:315` | **built** |
| Convert `t0` into the device's timebase | PPC `RecordingSession.serveCaptureRequest:706` | **built** |
| Cut the clip from the rolling ring | PPC `RingBufferRecorder` / `device.retainedClip` | **built** |
| Answer "I don't have it" (`outside_buffer`) | PPC `peer.captureAbsent:713` | **built** |
| Announce + transfer the payload | libppcp `capture_announce`, `payload_begin_as`, `payload_chunk` | **built** |
| Receive, reassemble, attach per-frame instants | PPS `onPayload*` → `PpcpClip`, `VideoInputPpcp.h:91` | **built** |
| Hand the clip to the session layer | PPS `clipReady()`, `VideoInputPpcp.h:403` | **built, deliberately not connected** |
| File the clip against a swing | PPS | **does not exist** |
| Tell the phone it is safely held | libppcp `ppcp_peer_capture_committed()`, `peer.h:604` | **built; PPS uses it for bundles only** |
| Show which shots are still arriving | PPC transfer table + `ShotSyncState` | **built on the phone, absent on the host** |

**So this is not a protocol gap and not a phone gap.** libppcp needs nothing new for the core
loop. PinPointCapture needs nothing new for the core loop. Two joins inside PinPointStudio are
missing, and one design question sits behind them.

⚠ **Re-confirmed on 1 September against all three repos at `bc9391b` (libppcp) and `c382888`
(PPC), both clean.** The one thing the table above overstates is the receiving end: `PpcpClip`
arrives *without* the payload's container, so H-d needs that field added before it can honour
ENC 6g (§0.5 #3).

**Proof that the phone half is real**, `AppModel.swift:1802` → `RecordingSession.swift:700`:
`serveCaptureRequest` converts `t0` into `PpcpTimebases.captureId`, calls
`device.retainedClip(aroundNs:preNs:postNs:)`, persists into its own bundle, and announces
through the same sink its own detections use — *"live bytes are bundle bytes is not conditional
on who asked"*. Where the moment has rolled out of the ring it replies `captureAbsent` with
`outsideBuffer`. It is complete, and it has never once been asked.

---

## 4. Why it was never built

Two reasons, different in kind. The distinction matters because only one was a decision.

### 4.1 The receiving end was parked on purpose, with reasons

`ppcp_host_service.cpp:195` states it plainly — *"clipReady() IS DELIBERATELY NOT CONNECTED, AND
THAT IS NOT AN OVERSIGHT"* — and `docs/ppcp-conformance.md` repeats it in §7.2 and §9.4. The
reason is an identity clash:

- **PPCP identity is opaque.** A Capture is identified by `Capture.id`, scoped by `Session.id`
  and the minting `Peer.id` (invariant I34, CORE 8.5c). All three are opaque strings the device
  mints. The spec is explicit that the digest is *not* the identifier — an `absent` Capture has
  no payload and therefore no hash, and absent captures are the most important content of a
  partial session.
- **PinPoint Studio identity is derived.** `SwingPaths::allocateSwingDir()` composes a session
  folder from athlete, date, naming pattern and a per-day counter —
  `2026-09-01_Mark-Liversedge_Wrist_01` — and a swing is `swing_0007` plus an `int` ordinal. Not
  opaque, not minted, and it changes if the user changes a naming preference.

Store a clip under the derived identity and the opaque one is lost, so the second arrival of the
same Capture duplicates it. Store it under the opaque identity and it is not in the swing
library. Work package H3 hit this for imported *bundles* and refused to bodge it: clips land
under `PPCP Imports/<peer>/<session>/` with their real ids, and the join to the swing library
was left open as **host review item 2**. The live path inherited the refusal without inheriting
a landing site.

**This was the right call and this document does not reverse it.** What follows is not a
proposal to merge the two identity schemes.

### 4.2 The asking end fell through unrecorded

`docs/ppcp-conformance.md` keeps an explicit register of joins that exist in code with no
caller. It names three: `registerPpcpPeer()`, the offset seam, and `clipReady()`. Two were
closed in H-compose. `clipReady` is openly parked.

`requestCapture()` is on no list, and the string appears **zero times** in the conformance
document. What is claimed for CORE §8.4 is the message *shape* only —
`CaptureRequestNamesT0InTheSessionTimebaseRef`, a static test that `t0` crosses in `tb:host` —
followed by the clause *"still not exercised here, because nothing on this side inverts"*. That
half-sentence is the only trace anywhere that this host never issues a capture request, and it
reads as a scope note about a test row.

**The lesson worth keeping:** the join blocked by a real design question was written down three
times; the join blocked by nothing at all was written down nowhere. Parked work is visible in
this codebase. Work never started *behind* parked work is not.

---

## 5. The design

### 5.1 Governing principle: link, do not merge

CORE 8.5a/8.5b already says what to do, and `ppcp_import_ledger.h` already quotes it:
reconciliation **creates links** and *"no entity is rewritten or merged"*.

PPCP identity stays opaque and authoritative on the PPCP side. PinPoint Studio's swing identity
stays derived and authoritative on the library side. A **link table** relates them, owned by
neither, and each side can be rebuilt from the other's records.

This is not a new mechanism. **`PpcpImportLedger` is that link table.** It already:

- keys on `CaptureKey { peerId, sessionId, captureId }` — I34's three parts, in scope order;
- records `localPath`, whose own comment reads *"where the clip landed, **alongside
  swing.json**"*;
- enforces idempotency through `admit()` → `Recorded` / `AlreadyHeld` / `DigestConflict`;
- queues what is owed back with `queueCommitted()` / `pendingCommits()` / `clearCommitted()`;
- keeps the real opaque id while sanitising only the *filename*, because "opaque" includes `/`.

> **The single largest decision in this document: extend the existing ledger to the live path
> rather than inventing a live-path store.** One identity mechanism, one set of invariants, one
> place where I34 is enforced — and the offline and live paths become the same code with a
> different transport, which is exactly what `PpcpImportSink` was built for (plan A10, CORE §9:
> *"a Capture that arrived in a file and a Capture that arrived on a wire reach this class
> through one code path and there is no branch in here that could tell them apart"*).

### 5.2 The mapping

Two records, each written by the side that owns it, neither derivable from the other.

**In the ledger** — `PpcpImportLedger::CaptureRecord` gains one field:

```cpp
struct CaptureRecord {
    CaptureKey   key;            // opaque, from the wire — unchanged
    std::string  digestHex;
    Completeness completeness;
    std::string  localPath;      // now may point INTO the swing library
    SwingRef     swingRef;       // NEW — the derived identity, empty for bundles
};
struct SwingRef {               // NEW
    std::string sessionDir;      // "2026-09-01_Mark-Liversedge_Wrist_01"
    std::string swingId;         // "swing_0007"
    std::string streamAlias;     // "phoneWide" — which streams[] element
};
```

**In `swing.json`**, as an additive block on the existing `streams[]` element. The schema is
already forward-compatible — *"a new stream type later is just another element of `streams[]` —
readers must ignore unknown kinds"* (`docs/developer/swing_export_developer_guide.md` §6) — and
a phone camera is an ordinary `kind:"video"` stream with a provenance block:

```json
{
  "kind": "video", "alias": "phoneWide", "file": "phoneWide.mp4",
  "origin": {
    "transport":    "ppcp",
    "peerId":       "peer:1f2e4491-…",
    "sessionId":    "ses:…",
    "captureId":    "cap:…",
    "streamId":     "st:…:video",
    "transfer":     "complete",
    "completeness": "complete",
    "committedAt":  "2026-09-01T13:46:55.221Z"
  },
  "frames": { "count": 610, "t_us": [ … ] }
}
```

`origin.transfer` is this host's view of the exchange and takes one of:
`requested` · `arriving` · `complete` · `absent` · `timeout` · `failed`.
`origin.completeness` is the **owner's** assertion (CORE 5.14, I10) and is never inferred from
what arrived.

A lost ledger can be rebuilt by walking `swing.json` files. A lost `swing.json` leaves the ledger
still able to prevent a duplicate. That redundancy is the point of linking rather than merging.

⚠ **`origin` is recorded, never interpreted by the analyzer.** It is provenance, like the
existing `capture.host` block. Nothing in `src/Analysis` may key on it. (There is a precedent
test for exactly this shape of rule — `CT-I37` greps `src/Analysis` for forbidden includes.)

### 5.3 The collection sequence

The one hard constraint: **a shot must never block on the network.** The pipeline is already
unavailable for 15–40 s per shot — `shot_controller.cpp` says so at the `droppedBusy` counter,
and *"a golfer hitting a bucket will outrun it"*. Adding a transfer to the critical path makes
that worse and hands the phone the power to stall the host.

So the swing is written twice: once immediately from what is local, once again when the clip
lands.

```
 shot arbitrated  (ShotController::onArbitratedShot, shot_controller.cpp:503)
   │
   ├─► commitShot(Source::Ppcp, tUs)          ← unchanged, still synchronous
   │     └─ writes a ShotMarker, emits shotDetected. ⚠ NO SWING DIR YET:
   │        allocation is 4-11 s away, in buildSwingExportJob() behind the
   │        post-roll and the 7 s history gather (§0.5 #1).
   │
   ├─► requestCapture(shotId, t0 in tb:host, streamIds, preNs, postNs, &err)
   │        ⭐ ISSUED HERE, AT shotDetected — NOT after the swing dir exists.
   │        The phone's ring is finite and every second of waiting spends it.
   │        ⚠ Fan out across PpcpHostService::m_phones, not through
   │        ShotController's single bridge pointer (§0.5, last para).
   │        ⚠ streamIds from VideoInputPpcp::streamIdFor(), never hand-built.
   │        ⚠ Clamp pre/post to the phone's declared BufferMargin (§0.5 #5).
   │                                                        ← THE MISSING CALL (H-c)
   │
   ├─► (seconds later) ShotProcessor allocates the swing dir and writes
   │     swing.json with the local streams; the phone stream is recorded
   │     "origin.transfer": "requested". The pending request is bound to the
   │     dir here, via the single-slot correlation of §0.5 #2. A clip that
   │     arrives BEFORE this point is parked, not dropped.
   │
   │        ┌── phone: convert t0 into its own capture timebase
   │        ├── phone: cut the clip from the ring
   │        ├── phone: capture_announce + payload_*    …or capture_absent(outside_buffer)
   │        └── (seconds later, asynchronously)
   │
   ├─► VideoInputPpcp::clipReady(PpcpClip)                  ← THE MISSING CONNECTION (H-d)
   │     ├─ ledger.admit(key)  →  AlreadyHeld ? stop, silently. I34.
   │     ├─ write payload to <swingDir>/<alias>.<ext>
   │     │     ⚠ extension from payload_begin.container (an IANA media type, ENC 6g /
   │     │       erratum E7). 6h FORBIDS inferring it from format.codec, from Stream.kind,
   │     │       or by sniffing.
   │     │     ⛔ PpcpClip DOES NOT CARRY IT YET — onPayloadBegin() ignores
   │     │       b.container. Add the field first; see §0.5 #3.
   │     ├─ rewrite that streams[] element: frames.t_us from clip.canonicalNs, rebased to t0
   │     ├─ ledger.setLocalPath(key, path, digest) + swingRef; ledger.save()
   │     ├─ peer capture_committed(captureId, digest)   ← only once DURABLY written (§5.5)
   │     └─ queue re-analysis of that swing
   │
   └─► re-analysis completes → carousel row gains video, thumbnail, camera-derived metrics
```

**Re-analysis is not new machinery.** `SwingDiskLoader::load(swingDir)`
(`src/Analysis/swing_reanalyzer.h:78`) already reconstructs a disk-backed `SwingWindow` from
`swing.json` plus its media sidecars and re-runs the production analyzer; `ReanalysisController`
already holds a batch back while a live shot is processing (`main.cpp`, `setLiveBusy`). A clip
landing is exactly the "this swing changed on disk, analyse it again" case that path exists for.

**`absent` is an answer, not a failure.** If the phone replies `capture_absent` /
`outside_buffer` — the golfer hit twice in three seconds, or the ring had rolled — the stream
element becomes `"transfer": "absent", "reason": "outside_buffer"`, no re-analysis is queued,
and nothing is retried. I10: completeness is asserted by the owner and never inferred by the
receiver.

### 5.4 Deadlines, and what a pending swing looks like

- A request neither answered nor refused within a bounded window — **proposal: 90 s**, or link
  loss, whichever comes first — becomes `"transfer": "timeout"`. The ledger keeps the key, so a
  later arrival is still recognised rather than duplicated.
- On reconnect the host re-requests nothing automatically. PPC already carries pending Captures
  through `session_resume`; the host should consult that rather than replay its own guesses.
  ⚠ Depends on **L-b** — see §9.
- A swing with a pending clip is **not** an error row on the carousel. It is a normal row with a
  "video arriving" affordance that completes on its own.

### 5.5 What the phone gets back, and why it is not optional

`ShotSyncState` on the phone has five states: `onDevice`, `sending`, `delivered`, `inStudio`,
`failed`. `AppModel.swift` is explicit: *"`.inStudio` comes from `capture_committed` and from
nothing else"* — 5.14h makes it the receiver's statement, and 8.4b forbids an owner claiming it
on its own authority. **PPS has never sent one over a live link.**

Two consequences, both already visible in PPC's own code:

1. The phone's UI can never truthfully say a swing reached the Studio.
2. Under I38 a device may not evict a Capture that never reached `confirmed`. **A phone used all
   season fills up and cannot clear itself.** `ppcp_import_ledger.h` makes exactly this argument
   for the offline path — *"a device that can never reach `confirmed` can never evict anything
   under I38, so its storage fills across a season"* — and the live path has the same defect for
   the same reason.

So `capture_committed` is not a nicety at the end of the sequence; it is the half of the
contract that makes the phone's storage tractable. Send it when the bytes are **durably** held —
written and flushed — not when received (MSG 8.4a). `queueCommitted` / `clearCommitted` already
model "owed, and owed until the owner has actually had it", so a commit lost with a dying link
is still owed.

### 5.6 Preview stays out of the ring, and that stays true

`camera_instance.cpp:738-758` gates the EventBuffer write, and its comment explains why a
preview frame must never reach the ring: it is 640×360 against a capture Stream's 1080p240, and
`lastFrameInstantUs()` answers 0 without a timebase relation, so it would land stamped with its
*arrival* time — *"frames in the swing library that claim to be capture and are not"*.

**Nothing here relaxes that.** The live tile keeps its preview; the swing gets the clip; they
never mix.

⚠ But the gate reads `m_eventBuffer && m_sourceId != kInvalidSourceId &&
m_eventBuffer->isCapturing() && !m_previewOnly` — it tests **instance mode**, not frame
provenance, and a PPCP session controller is not preview-only. The rule is currently enforced by
accident. It should test provenance. See §9.3.

### 5.7 Where the ledger lives

Today it is at `<athleteLibraryPath>/PPCP Imports/ppcp-import.json`, loaded once in
`PpcpHostService`'s constructor and shared by every phone. That is right for bundles and wrong
for live captures landing in the swing library.

**Proposal**: the file moves to `<athleteLibraryPath>/ppcp-ledger.json` and covers both paths,
with `localPath` pointing wherever the bytes went — under `PPCP Imports/` for a bundle, into a
swing folder for a live capture. One ledger, two landing sites, one identity rule.

⚠ This is a migration: read any existing `PPCP Imports/ppcp-import.json`, fold it in, leave the
old file in place. `swingRef` is absent on every legacy record, correctly — those captures are
not in a swing.

---

## 6. Failure semantics

Stated once, because §7 depends on classifying these correctly.

| Situation | Is it an error? | What the user is told |
|---|---|---|
| Phone replies `absent` / `outside_buffer` | **No.** A first-class answer (I10) | One info line on the shot: no phone video, the moment was outside the phone's buffer |
| Transfer still in flight | **No.** A state | "video arriving" on the row |
| Transfer times out | Warn | Named on the row; the swing stays valid |
| Link drops mid-transfer | **No.** Expected | Row shows pending; resolved on reconnect (§5.4) |
| `DigestConflict` from the ledger | **Yes** — same identity, different content | Log only; never merged, never overwritten |
| Swing folder cannot be created | **Yes**, and it is a *condition* (§7) | One latched notification, not one per shot |
| No capture Stream open when a shot arrives | **Yes**, ours | Log; the shot still saves without phone video |

---

## 7. Error reporting: toasts, and the cascade

### 7.1 What happened on 1 September

`/mnt/swingdata` was not mounted. One root cause, an environment fault. What the user got:

- 10 × `swingSaveFailed` → the same toast, re-shown 10 times with identical text.
- 4 × `shotRefused` → a second toast, stacked by hand at `saveErrorToast.y - height - sp(10)`.
- 10 × `shotFailed` → routed to the launch-monitor controller, **no toast at all**.

`PpToast.show()` (`src/Gui/components/PpToast.qml:55`) sets the text, sets `visible = true`, and
restarts a hide timer. So ten failures produced **one** toast that never went away, showing the
tenth message, with no indication it had happened ten times. The user's own reading afterwards
was *"no shots were ever recorded"* — true, but for reasons the screen never gave.

### 7.2 The three defects, separately

1. **No identity.** A notification is a string. Two occurrences of one fault are
   indistinguishable from two different faults, so nothing can be counted or coalesced.
2. **No causality.** "Swing save failed", "export skipped" and "analysis degraded" are three true
   statements about one fault, and the model cannot say the second and third are *consequences*
   of the first.
3. **Conditions are reported as events.** "The library path is unwritable" does not stop being
   true after 21 ticks of a timer. It is a state of the machine, reported ten times as though it
   were ten pieces of news.

### 7.3 What exists today

Nine `PpToast` instances across the QML tree; four in `src/Gui/shell/Main.qml` —
`saveErrorToast` (787), `analysisErrorToast` (796), `deviceOnlyToast` (822), `shotRefusedToast`
(870) — each wired to its own signal, each positioned by hand relative to `saveErrorToast.y`.
The C++ side emits `ShotProcessor::swingSaveFailed`, `::analysisFailed`, `::shotFailed` and
`ShotController::shotRefused`, all carrying a `QString`.

### 7.4 The model

Replace *N QML toast instances wired to N signals* with **one sink** that everything session- or
shot-scoped posts to. Per-panel toasts for local confirmations (copy-to-clipboard, a calibration
saved) stay where they are; they are not this problem.

```cpp
struct Notification {
    QString  id;        // STABLE KEY, e.g. "swing.save.no-directory" — never the message text
    Severity severity;  // Error | Warn | Info | Progress
    Kind     kind;      // Event | Condition
    QString  title;     // "Swings are not being saved"
    QString  detail;    // "…/mnt/swingdata/… could not be created"
    QString  cause;     // "" or the id this is a consequence of
    Action   action;    // optional { label, invocable }
};
```

The key is `id`. Everything below is only possible because it exists.

### 7.5 Four rules

**R1 — Coalesce on `id`.** A repeat updates the live notification in place and increments a
count: *"Swings are not being saved (×10)"*. Never a second toast, never a silently restarted
timer with new text. The count is the diagnostically valuable part and today it is exactly what
is lost.

**R2 — Suppress consequences whose cause is live.** A notification naming a `cause` that is
currently displayed, or was within a short grace window, is **recorded to the log and not
shown**. `shot.export.skipped` names `swing.save.no-directory`; while the cause is on screen the
consequence stays silent. This is the direct answer to a cascade: **report the cause, count the
consequences.**

⚠ The inverse must hold too: a consequence arriving with **no** live cause is shown on its own
merits. Suppression is a statement about what the user already knows, not a permanent ranking.

**R3 — Conditions latch; events expire.** A `Condition` stays visible until resolved or
dismissed and carries an action where one exists ("Open Storage settings"). Subsequent shots
consult the live condition instead of raising anything. An `Event` keeps today's auto-hide.

**R4 — One terminal notification per shot, not one per stage.** Invert the current flow: the
pipeline emits a single statement of what the shot *became*.

| Outcome | Severity | Sentence |
|---|---|---|
| Everything worked | *(silent — the carousel row is the feedback)* | |
| Saved, phone video still arriving | Progress | "Shot 7 saved — video arriving from iPhone" |
| Saved, phone had no footage | Info | "Shot 7 saved without phone video — the moment was outside the phone's buffer" |
| Saved, analysis degraded | Warn | "Shot 7 saved — analysis incomplete" |
| Not saved | Error | "Shot 7 not saved — swings are not being saved" *(names the condition)* |

Stage-level detail keeps going to the application log — one log, `ppWarn`, as the house rule
has it. The toast is the act-now half.

### 7.6 What 1 September would have looked like

- **One** error toast at the first shot: *"Swings are not being saved — the folder
  `/mnt/swingdata/…` could not be created"*, latched as a condition, with an action opening
  Storage settings.
- A count on it as shots 2–10 arrived: **(×10)**.
- The four corroboration refusals unchanged — different `id`, different cause, genuinely
  separate news.
- Nothing about export, analysis or thumbnails on screen; all of it in the log.
- And, once §5 lands, **a phone-video statement that today does not exist at all**.

### 7.7 The preflight, which is worth more than all of the above

Every fault in §7.1 was knowable **before the first shot**. The library root is resolvable at
session start; whether it is writable is one `QDir().mkpath()` of a probe directory.

**A session must not start into a state where every shot is guaranteed to fail.** At session
start, check:

- the library root exists and is writable;
- a declared phone camera has a live link;
- disk headroom against an estimate for the session.

A failure is a **condition** raised before the user swings, with an action — not ten identical
toasts afterwards. Of everything in this document this is the change that would have saved the
most of 1 September, and it is independent of all the PPCP work.

---

## 8. Work items, with acceptance criteria

### Phase 0 — independent of PPCP; do it first

| # | Item | Done when |
|---|---|---|
| **H-g** | ✅ **DONE** (`f595eb9`). `NotificationCenter` + `PpNotificationHost`; the four `Main.qml` toasts migrated; `shotRefused` split by id, `swingSaveBlocked` split from `swingSaveFailed`, R4's `shotOutcome` added | Met. `notification_center_test` in the headless `gui-tests` target covers all three cases, plus condition-latching and a **transitive** cause chain (see note below) |
| **H-h** | ✅ **DONE** (`f595eb9`). Hung off a new `SessionController::sessionStarted` — both start paths already funnel through it, so **no QML change** and nothing near the wizard. `beginSessionFolder` now keeps the `beginSession` return it discarded | Met, verified against the real app offscreen: an unwritable library raises one latched condition before any shot, ten failing shots add nothing to the screen, a writable one raises nothing |

> **Two things Phase 0 learned that this document did not say.**
>
> **Suppression must follow the CHAIN.** The cascade is three deep — an unwritable root suppresses
> "no swing folder", which is in turn the cause the shot-level failures name. With one-hop
> suppression the outer links surfaced the moment the middle went quiet, which is the defect R2
> exists to remove. `causeIsLive()` now recurses, bounded.
>
> **`phone.no-link` from §7.7 was deliberately NOT built.** Camera config persists per phone, but
> a real settings file holds *six* historical peer uuids, so the naive check raises a condition
> for every phone ever paired — the exact noise this work removes. It needs a notion of "cameras
> this session expects", which does not exist. Left open on purpose.

### Phase 1 — identity groundwork, no behaviour change

| # | Item | Done when |
|---|---|---|
| **H-a** | `SwingRef` on `CaptureRecord`; ledger root moves to `<library>/ppcp-ledger.json`; migrate the old file. **Also fix the dual-writer hazard**: `PpcpHostService::m_importLedger` and a function-local ledger in `PpcpImportController::importBundle()` (`ppcp_import_controller.cpp:65-76`) write the same file through independent copies, last writer wins, nothing reloads | `ppcp_bundle_import_test` still passes unchanged; a new case round-trips a record carrying a `swingRef` through save/load; an old `ppcp-import.json` is folded in and its records keep their identity |
| **H-b** | `origin` block written and read in `swing.json`; `SwingDocWriter` support | `swing.json` round-trip test (Analysis suite) covers a stream with `origin`; a reader that does not know `origin` still loads the swing |

### Phase 2 — the loop. ⛔ Land H-c and H-d together

> **H-c without H-d pulls video across the link and drops it. If they cannot land together,
> land neither.**

| # | Item | Done when |
|---|---|---|
| **H-c** | Call `requestCapture()` from the arbitrated-shot path **at `shotDetected`, NOT once the swing dir exists** (§0.5 #1). Fan out across `PpcpHostService::m_phones`, not `activeShotBridge()`. Stream ids from `streamIdFor()`. Clamp the window to the declared `BufferMargin` | With a phone attached, one shot produces one `capture_request` naming the open capture Stream and the window; `bulk` byte counts become non-zero. With TWO phones, both are asked |
| **H-d** | Add the container field to `PpcpClip` (§0.5 #3), then connect `clipReady()` → `admit` → write payload → rewrite the stream element → `capture_committed`. A clip arriving before the swing dir exists is parked, not dropped | A clip lands in the swing folder with the container from `payload_begin`; the ledger holds the key with a `swingRef`; a **replayed** identical Capture is `AlreadyHeld` and writes nothing; PPC's row reaches `inStudio` |
| **L-a** | Assert 5.11h — preview payload on a bulk channel distinct from shot payload, using `ppcp_event.channel` (conformance finding 14 says this belongs with this join) | New case in `ppcp_video_input_test` |

### Phase 3 — the finish

| # | Item | Done when |
|---|---|---|
| **H-e** | Queue re-analysis on a landed clip; carousel row updates in place | A swing that arrived videoless gains video and camera-derived metrics without user action, and the stage is never stolen (§9.6) |
| **H-f** | `requested` / `arriving` / `absent` / `timeout` states on the row and in `swing.json` | Each state is reachable in a test; `absent` is presented as information, never as an error (§6) |
| **C-a** | *(PPC)* Verify `outside_buffer` on hardware — written but, per `RingBufferRecorder`'s own comment, never exercised on a device | A request whose `t0` predates the ring is answered `absent`, not with silence or a truncated clip |
| **C-b** | *(PPC)* Confirm the transfer table drives the per-shot sync UI once commits arrive | A shot visibly transitions `sending` → `delivered` → `inStudio` on the phone |
| **L-b** | ✅ **ANSWERED, no libppcp change** (§0.5 #6). `PPCP_EVENT_SESSION_RESUME` delivers `ev.msg->body.session_resume.pending[]` to a host like any other event | Met by inspection. What remains is using it in §5.4, which is PPS work |

### Reproducing the manual test

Full instructions are in memory under `ppcp-manual-test-diag-rig`. In short: three
ms-timestamped streams, phone first, host second.

```sh
# phone stdout (PpcpLog prints to BOTH os_log and stdout; only stdout reaches devicectl)
xcrun devicectl device process launch --device <udid> --terminate-existing --console \
  org.pinpointstudio.capture 2>&1 | perl ts.pl > ppc.log
# host
PINPOINT_LOG_STDERR=1 PINPOINT_SYNC_TRACE=1 \
  build/Qt_6_11_1_for_macOS_Debug/PinPointStudio.app/Contents/MacOS/PinPointStudio 2>&1 \
  | perl ts.pl > pps.log
# USB, to tell our drop from macOS re-enumerating the phone
/usr/bin/log stream --style compact --info --predicate \
  'process == "usbmuxd" OR eventMessage CONTAINS[c] "setConfigurationGated"' > sys-usb.log
```

⚠ `/usr/bin/log`, never bare `log` — it is a zsh builtin and returns nothing, silently.
⚠ `devicectl --console` holds a CoreDevice tunnel (`anri0`) and is a **confound for
link-stability questions**. It is fine for this work, which is about whether video moves at all.
⚠ `PINPOINT_SYNC_TRACE` emits a line per second — `grep -v ppcp-sync` for the narrative.

The fastest read of success is the byte counter on link teardown: `bulk 0/0` means the leg is
still not working.

---

## 9. Open questions

1. **Reconnect semantics.** After a link drop, who re-drives an outstanding request — the host
   re-requesting, or the phone re-offering through `session_resume`'s pending Captures? The
   phone's list is authoritative about what it still holds. ~~Depends on **L-b**.~~ **L-b is
   answered (§0.5 #6): the list is readable.** The question is now purely a policy one — and
   §0.5 #7 sharpens it, because a host re-request produces a *new* `captureId` and so cannot be
   deduplicated against the outstanding one by identity alone.
2. **Two phones, one shot.** ⭐ **DECIDED: fan out.** The request is issued per-phone across
   `PpcpHostService::m_phones` and the swing carries one `streams[]` element per phone, because
   `activeShotBridge()` returns only the FIRST phone's bridge and a design that asks one phone
   would need rewriting the moment a second is used. Arbitration already spans peers, and
   `requestCapture` is CORE §8.4's *orphan* request precisely so a host can ask a device that
   never nominated the shot. **Still unproven:** whether analysis should treat two phone cameras
   as two cameras, and no two-phone capture has ever been run.
3. **Preview-to-ring is enforced by accident.** §5.6 — the gate tests instance mode, not frame
   provenance. It should test provenance. Small, and it protects the swing library from exactly
   the class of fault this document is about.
4. ~~**Where the request's `pre`/`post` window comes from.**~~ ✅ **ANSWERED by CR-02, which
   landed after this was written (§0.5 #5).** The ring depth *is* declared now:
   `PPCP_EVENT_BUFFER_STATUS` (MSG 5.6) carries a per-stream `retained_from` and retention
   target, and PPS already decodes it into `BufferMargin` at `ppcp_live_session.cpp:406`. The
   host clamps `pre`/`post` to what the phone says it still holds, rather than asking blind and
   taking `outside_buffer` for the whole clip. What remains is only choosing the host-side window
   (`m_windowStartUs .. m_windowEndUs`) and intersecting the two.
5. **Ledger growth.** One record per capture per session, for ever, in one JSON file. Fine for a
   season, unproven for years. A compaction rule will be needed eventually — but I34 means
   records cannot simply be dropped. ⚠ Related and now decided: the **dual-writer** hazard is
   fixed as part of H-a, not left as a finding.
6. **Does a landed clip re-open the swing for the user mid-session?** Re-analysis takes 15–40 s
   and the golfer is probably hitting again. Proposal: update the row silently, never steal the
   stage. Unresolved.

---

## 10. Where this actually stands — hardware session, 1 September 2026

> **Read this before believing §8's commit messages.** Phases 0–2 are committed. The video leg
> is **NOT proven end to end**, and the Phase 2 commit (`48dfc7e`, "The phone is finally asked
> for the swing, and the answer is kept") overstates it: what is proven is the **asking**.

### 10.1 The one number that matters

**No clip has ever been filed.** Checked against the artefacts, not against recollection, at the
end of the session:

| | |
|---|---|
| `phone video landed` lines, all runs | **0** |
| Video files in any swing folder from that day | **0** |
| Ledger `captures` / `sessions` / `pending_commits` | **0 / 0 / 0** |
| `swing.json` files written that day | **0** |

One run showed `bulk 242550681/1596` on link teardown — 242 MB genuinely crossed the wire. It is
easy to read that as success and it is not: those payloads arrived for Captures whose
`capture_announce` had not registered, so `onPayloadBegin` refused them (no stream, no profile,
no `achieved_frames`) and they were dropped before reaching `PpcpClipFiler`. Bytes moving is not
a swing you can open.

### 10.2 What IS established on hardware

- **H-c works.** `capture_request` goes out naming genuinely-open `shot_windowed` Streams, read
  from the peer's own Stream table, with `pre_ms 2000 post_ms 1000`.
- **The phone answers correctly.** `capture_announce` arrives anchored to the right `shot_id`
  (I27), on `str:video:src:camera:wide`.
- **And says it has not sent.** `completeness: partial` (1), `transfer: pending` (0) — libppcp's
  own words for *"held locally, unsent"*. No payload follows.
- Everything downstream in PPS — admit, write, `frames.t_us`, `origin`, `capture_committed`,
  re-analysis — is written and unit-tested and **has never run against a real clip**.

### 10.3 The blocking problem for the next session

⛔ **The automated hardware loop cannot reach a Session, and fixing that unblocks everything
else.**

`DeviceSessionTests.aHostedSwingProducesAClip` ("Test 1 of the hardware list, automated") is the
right tool: it arms through `model.arm()` and injects a swing through `SyntheticAudio.oneSwing`,
so it needs **neither a person's hands nor a real impact**. Run with `make test-device` against a
running PinPointStudio.

It fails identically every time at `#require(await link.hostSession)` — `session_open` never
arrives — because the host refuses its link:

```
this phone already has a link — keeping the one it has and closing the newcomer.
One phone, one link (design §6.1)
```

Something already holds a link when the test dials. Tried and did **not** fix it: suppressing the
app's own auto-search under test (`AppModel.isUnderTest`, uncommitted), and
`PINPOINT_PPCP_WIRED=0` on the host. The first link's owner is still unidentified — that is the
next thing to find. It is a harness fault, not a product fault, and until it is fixed every
hardware answer needs a person in the room.

### 10.4 The leading hypothesis for the missing payload

Unproven, but it fits both observations — including why one run moved 242 MB and the others moved
nothing.

`RecordingSession.startTransferring()` is called from **exactly one place**
(`AppModel.swift`, the arm path), and it captures `hosted` from `control`, which is `public let`
— immutable for the life of the session. Nothing re-calls it on reconnect. A link that changes
under a live RecordingSession therefore leaves the drain loop bound to a stale context, pumping a
queue nothing enqueues to.

The run that moved bytes had armed **after** its last reconnect. The runs that moved none had
not.

⚠ And the loop swallowed every error: `let sent = (try? await …) ?? 0`, then a 20 ms sleep, for
ever. A clip that could never encode retried silently fifty times a second. Instrumented now (see
10.6) but never read, because the phone's stdout was unavailable — see 10.5.

### 10.5 Environment, and the traps that cost hours

- ⛔ **`devicectl … --console` re-enumerates the phone.** The kernel logs
  `setConfigurationGated` with `AMPDeviceDiscoveryAgent` on its heels, the device number
  increments, and every usbmux tunnel dies — reported by PPS as `Broken pipe` and by PPC as
  `channelClosed(peerClosed)`. §8 already warned this tunnel is a confound; it is worse than a
  confound, it is the cause. **Zero re-enumerations in the run that did not use it.**
- ⚠ **`idevicesyslog` does NOT carry `PpcpLog`.** It reads the legacy syslog relay, not the
  unified log, so the `Logger` half of `PpcpLog.emit` is invisible through it. Only
  `devicectl --console` bridges the `print()` half — which is the tool that destabilises USB.
  **Getting the phone's diagnostics without breaking the link is an unsolved problem**; a
  file-based sink pulled with `devicectl device copy from` would settle it.
- The "iPhone USB" tether service is still correctly disabled; that earlier fix held.
- `PINPOINT_PPCP_ACCEPT_ALL=1` (new, host) records uncorroborated Shots so a desk test is
  possible. ⛔ Never in a real session. It warns on every Shot it lets through.
- Corroboration refusing 12 of 15 desk Shots is **the rule working**, not a fault: the phone's
  impact detector fires on handling the Mac's microphone never hears, giving deltas of 0.6–3.4 s.
- A shot with no local camera and no IMU produces `window captured — 1 entries, 0 camera
  track(s)`, fails both analysis and export, and leaves an **empty swing folder** — no
  `swing.json` at all. That is the shot a phone clip matters most for.

### 10.6 Uncommitted work at handoff

Both trees are dirty. None of it is speculative; all of it was written against an observed
failure.

**PinPointStudio** — `swing_doc.{h,cpp}`, `ppcp_clip_filer.cpp`, `shot_controller.cpp`,
`VideoInputPpcp.cpp`, `ppcp_clip_filer_test.cpp`:
- Silent payload drops in `onPayloadBegin` now name which precondition failed. ⚠ Suppressed for
  un-announced captures, which arrive at preview rate — the first version logged **9205 lines in
  one short session**.
- `capture_announce` logged once per accepted non-preview announce.
- The filer creates a `swing.json` when a clip lands and the pipeline left none — never
  speculatively, only when there are real bytes.
- `frames.t_us` written, rebased on the clip's own first frame. **Without it the file is
  unopenable**: `SwingDiskLoader` and `DiskReplaySource` both skip a video element with no
  frames. ⚠ Not rebased to the shot's `t0`, deliberately — that needs a TimebaseRelation, and
  evaluating one outside its domain fabricated a 460 ms sigma in August.
- `PINPOINT_PPCP_ACCEPT_ALL`.
- A test case for the empty-folder path. ⚠ The earlier tests all wrote a document first, which is
  exactly why they missed it.

**PinPointCapture** — `PpcpLog.swift`, `RecordingSession.swift`, `LiveDetectionSink.swift`,
`HostedSessionContext.swift`, `AppModel.swift`:
- A `ppcp.transfer` log category, because the payload transfer had no voice at all.
- The drain loop's `try?` replaced with a caught, logged error, plus `drain started` /
  `drain stopped` / `NOT started` / `STALLED (n jobs queued, nothing sent for 5s)`.
- An announce with no payload provider says so — from the host it is indistinguishable from a
  transfer that has not started.
- `AppModel.isUnderTest` suppressing the app's auto-search. **Did not fix the duplicate link**;
  keep or discard on the evidence.

### 10.8 Landed — 1 September 2026, 21:42 (added by the unattended session)

`make integration-device … EXPECT_CLIPS=1` → **PROBE RESULT PASS — filed 1 clip(s) into 1
shot(s)**, on its eighth run of the evening. On disk, checked by hand:
`2026-09-01_Mark-Liversedge_Wrist_14/swing_0001/wide-cf05c062.mp4` (21,930,314 bytes, 718
frames, `frames.t_us` 0…2.99 s, `origin.completeness: complete`, ledger capture with a
`swingRef`). Over WiFi. The whole account — five stacked silent defects across all three
repos, and what remains (the cable, preview orphan begins, MP4 timestamps not rebased) — is
`live_capture_handover.md` §0. 10.3 is fixed (`c4019c3`); 10.4's hypothesis was wrong (the
drain ran; the answers were `absent` for a different reason, and then the bytes were lost on
the host four different ways); 10.7 #1–#3 are done, #4 still open.

### 10.7 What to do first

1. **Find who holds the first link** when `make test-device` dials, and stop it. Nothing else can
   be answered repeatably until the automated loop reaches a Session.
2. **Get the phone's diagnostics off the device without `devicectl --console`** — most likely a
   file sink plus `devicectl device copy from`. 10.4's hypothesis is one log line away from
   settled, and that line cannot currently be read.
3. Then re-run the loop and read `ppcp.transfer`. If it says `NOT started` or `STALLED`, 10.4 is
   the answer and the fix is to re-arm the drain on reconnect.
4. **Write a test for `serveCaptureRequest`** — the phone's whole serve-the-host path has none,
   which is the same hole `requestCapture` had on the host side.
