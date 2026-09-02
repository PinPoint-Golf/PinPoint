# Handover — the swing-video leg, one question from done

**Written**: 1 September 2026, end of the second hardware session.
**Updated**: 1 September 2026, late evening, unattended session — **see §0 first.**
**Audience**: a session picking this up cold. Read §0, then this file, then §10 of
`live_capture_collection_design.md`.

---

## 0. Status, 1 September 2026, 21:43 — A CLIP IS ON DISK

```
/System/Volumes/Data/mnt/swingdata/Mark-Liversedge/2026-09-01_Mark-Liversedge_Wrist_14/swing_0001/wide-cf05c062.mp4
21,930,314 bytes, HEVC 1920x1080, 718 frames; swing.json names it with frames.t_us 0…2,993,875;
ppcp-ledger.json holds cap:98aa0927… with a swingRef.   make integration-device: PROBE RESULT PASS
```

The rig in §4 went green for the first time on its eighth run of the evening, and the disk
was checked by hand (§1's rule). Two frames were pulled with ffmpeg and looked at: the phone's
view of the desk. **Over WiFi.** The cable is not yet proven — see §0.3.

### 0.1 The answer to §2's question

`outside_buffer` was **not** the ring. It was **five stacked defects, each silent**, found one
per rig run by adding a line and reading the number. In order of discovery:

| # | Where | Defect | Fix |
|---|---|---|---|
| 1 | PPS `PpcpLiveSession::publishRelations` | The host published its clock relation **once**, at its first two-exchange fit, and never again (the comment claimed "the maintenance cadence republishes it" — nothing did). The phone converted every host `t0` through a slope that was noise, extrapolated from an `observed_at` minutes old — the "47 s out" the phone's own code remembered. | Republish every 5 s, matching the phone's `HostLinkDriver.publishIntervalNs`. Test added. |
| 2 | PPC `RecordingSession.serveCaptureRequest` | Extracted **on arrival**, ~300 ms after `t0`, for a window ending 1000 ms after `t0`. The phone's own path is held by 8.2i's mint deadline; a host request had no such wait. | Wait until `t0 + post + one fragment interval + 200 ms` (bounded by the ring's depth) before reading the ring. Logs the conversion, the relation it went through and its age, and on `absent` the requested span beside what the ring held. |
| 3 | PPS `VideoInputPpcp` | **One payload reassembly slot per instance.** CORE §2 interleaves bulk and preview payloads by design; the next preview segment's `payload_begin` replaced the clip's "after 0 of 13,126,842 bytes". | Reassembly keyed per capture id, bounded, preview evicted first. Test added. |
| 4 | PPS `VideoInputPpcp` | **Announce/payload ordering race**: two TCP connections, bulk read first; "no announce, no reassembly" dropped 17 MB that had arrived whole. | Bytes are collected on the capture id; what they are is resolved at `payload_end`. `achieved_frames` is deep-copied (it holds pointers into the engine's arena). Tests added. |
| 5 | libppcp `PPCP_PEER_SCRATCH_ARENA` | The decode arena was **8 KiB**. A 3 s clip's `payload_begin` (720 frames × instant + exposure) was "undecodable"; the engine raised `PPCP_EVENT_ERROR` and **PPS never logged it**; 22 MB of chunks then arrived for a payload nobody had opened. The phone's own 2 s clips (~495 frames) squeezed under, which is why *they* landed and every host-requested clip did not. | 512 KiB, public in `peer.h`, derived from a 10 s ring at 240 fps with every per-frame series. Decode test added. PPS now logs engine error events. |

Also fixed on the way: the phone's own preview segments were being delivered to the filer as
shot-less "clips" (3301 in one run — §6's "noise" was this); the eviction table's zombie
entries after a move; the rig's `pull-diags` racing the phone's log; a host tick-stall
warning (~1.1 s stalls at camera enable are real and now visible).

### 0.2 What the numbers looked like once the lines existed

```
PHONE  capture_request t0 converted … tb:host 51041918133000 → tb:hosttime 163797621783335, -1410 ms from now;
       via relation offset 112755686.25 ms skew 6317.27 ppm (σ 1.304 ms / 119.53 ppm) observed 2.1 s before t0
PHONE  capture_request waiting … +290 ms for the post-roll to reach the ring
PHONE  capture_request answered … partial [-2000 … +1000] ms from t0, payload queued
HOST   payload begin "cap:…" [inst "src:camera:wide"] stream "str:video:src:camera:wide" shot "…" bytes 21930314 frames 718
HOST   clip delivered "cap:…" [inst "src:camera:wide"] shot "…" 21930314 bytes, 718 frames, handed to the filer
HOST   phone video landed: …/swing_0001/wide-cf05c062.mp4 21930314 bytes
```

⚠ The host's published skew still reads 6,000–14,000 ppm with σ up to 4,700 ppm after a
burst: the slope is unconstrained until maintenance exchanges spread the window. It converts
fine 2 s from `observed_at`; at 7 s it is ±24 ms. See §0.4.

### 0.3 The cable — DONE, 22:25

```
2026-09-01_Mark-Liversedge_Wrist_19/swing_0001/wide-cf05c062.mp4   21,918,545 bytes, 719 frames
link up … transport=usb   → capture_request 22:25:06 → phone video landed 22:25:10   PROBE RESULT PASS
capture_committed x 1 -> "iPhone 16" (0 still owed)
```

The real app in the foreground (`make deploy`), the host dialling usbmux, the session driven
by the probe, the shot from the host's own microphone path. Four seconds from ask to disk on
the cable against ten over WiFi; the clock gate passed in under a second at 0.25 ms. Three
more defects fell out on the way:

| # | Where | Defect | Fix |
|---|---|---|---|
| 6 | PPS `ShotController` | A **Manual shot asks no phone**: `captureRequested` is emitted only for an arbitrated PPCP Shot. | Not changed — it is arguably by design. The probe drives `reportCandidate(Acoustic)` (now `Q_INVOKABLE`, with `nowUs()`), which is the host's real microphone path: nominated, arbitrated, committed, asked. `--inject-shot-after-ms N` also sends `armAll()` at session start. |
| 7 | PPS filer / `VideoInputPpcp` | **`capture_committed` was never sent**: 8.4a's commit carries the digest, the filer never had it, libppcp refused with `invalid argument`, and the flush `break`-ed silently every 20 ms for ever (`pending_commits: 3`). | The announce's digest rides on `PpcpClip.digestHex` into the ledger row and the commit. A commit the library refuses as invalid is struck once, with a line, instead of retried. `digestToHex` added beside `digestFromHex`. |
| 8 | Rig | The host **exited on its PASS** while the device suite still had three hosted tests to dial, so the first green run reported "the DEVICE half failed". And the probe's clip counters were pushed on `phonesChanged`, so a run with no phone-list change reported "filed 0" with the clip on disk. | `--linger`: the probe stays up after a PASS and the Makefile reads the verdict from the log and ends the process. Clip-chain stats are read on demand through a provider. |

### 0.3a What is NOT done

1. **Preview `payload_begin`s with no chunk or end** trickle in (a few a minute, 0 of ~38 KB).
   `PreviewProducer` sends begin, then chunk, then end in one `perform`; when `payloadChunk`
   throws NOSPACE the begin is already on the wire. Harmless now (bounded table, preview
   evicted first, "payload table full" line) — but protocol litter, and the phone should not
   begin what it cannot chunk.
2. **The landed MP4's container timestamps are the capture clock's** (ffprobe says the file
   "starts" at 165,375 s). `swing.json`'s `frames.t_us` is rebased to 0 and PPS replays from
   that, but QuickTime will show a clip that begins 45 hours in. Rebase in the writer or the
   filer.
3. **The wired preview channel dies every ~50 s** (`preview channel: read: TLS protocol error
   — Broken pipe`, phone `posix 54`) and takes the whole link with it. The clip leg survives
   because it completes in 4 s, but this is the drop `wired-preview-drops-link` believed cured.
4. Three unattributed WiFi link losses at 36 s (20:19): phone `link lost no error reported`,
   host `close_notify`, 3 missed 1 s heartbeats. Host tick stalls are now logged; none exceeded
   1.1 s in the runs that followed.
5. `commitShot`'s unarmed message (§6.2) still says "still processing the previous shot" for
   every unarmed state.
6. The rig's device half still has no test of `serveCaptureRequest` (§10.7 #4 of the design).
7. ⚠ **Nothing is committed.** All three repos are dirty; Mark commits.

### 0.4 On clock convergence (Mark's secondary ask)

Tonight, over WiFi with min RTT 2.4 ms, the host's 5 ms gate passed **6–11 s** after
link-up, not 120 s. `PINPOINT_SYNC_TRACE=1` (exported before `make integration-device`)
shows why the old number was the link and not the estimator: `offset_sigma` sits on
`sqrt(residual² + (min_rtt/2)²)`, and the residual is what a bad radio inflates. Two things
worth doing anyway, neither built:
- The burst's 16 exchanges span 300 ms, so `skew_sigma` is ~49,000 ppm until the first
  maintenance exchange 5 s later (then 648, 126, 94 ppm). Spreading the same 16 exchanges
  over 2 s — same traffic — would give a usable slope at once.
- Both gates (host `worstSyncSigmaMsFor`, phone `offsetSigmaMilliseconds`) evaluate at
  `observed_at`, i.e. **ignore the skew term**, and can pass 1 s after link-up on a
  burst-only fit whose `sigma@5s` is 247 ms. They should evaluate the sigma at the horizon a
  clip will be asked at (a few seconds).

### 0.5 Reproducing the cable run

```sh
cd ~/Projects/PinPointCapture && make deploy          # installs + foregrounds the real app
# (or, app already installed:)  xcrun devicectl device process launch --device A39B669F-23F5-5E93-8A68-AC090EF2FADB --terminate-existing org.pinpointstudio.capture
sleep 6; pkill -f "PinPointStudio.app/Contents/MacOS"
QT_QPA_PLATFORM=offscreen PINPOINT_LOG_STDERR=1 PINPOINT_PPCP_ACCEPT_ALL=1 PINPOINT_SYNC_TRACE=1 \
  <PinPointStudio binary> --probe-qml tools/probes/ppcp_assert.qml \
  --expect-clips 1 --inject-shot-after-ms 20000 --probe-timeout-ms 150000 > pps.log 2>&1
```
⛔ The app must be foregrounded BEFORE the host starts, and never relaunched while the host is
up: a relaunch kills the link the host just dialled and the fresh app is never armed (the
21:53 attempt). Look for `link up … transport=usb`, `PROBE DRIVE arm sent`, `host acoustic
candidate reported`, `capture_request →`, `payload begin "cap:`, `phone video landed`,
`PROBE RESULT PASS`.

---

## 1. The goal, stated so it cannot be fudged

> A clip of video, cut by PinPointCapture on the phone, written into a swing folder in
> PinPointStudio's library, named by that swing's `swing.json`, and linked in the PPCP ledger.

**Done is a file on disk.** Not bytes on a wire, not a green test, not a counter.

That distinction is not pedantry. On 1 September a link teardown reported `bulk 242550681` —
243 MB genuinely crossed the cable — and it was reported to Mark as the leg working. It was not:
those payloads were for Captures whose `capture_announce` had never registered, and they were
refused and dropped before reaching the filer. **A byte counter is not a result.**

### How to know you are done

```sh
cd ~/Projects/PinPointCapture
make integration-device \
  STUDIO=~/Projects/PinPointStudio/build/Qt_6_11_1_for_macOS_Debug/PinPointStudio.app/Contents/MacOS/PinPointStudio \
  EXPECT_CLIPS=1
```

Exit 0 only. It passes when a clip is on disk, `swing.json` names it with `frames.t_us`, and the
ledger holds the capture with a `swingRef`. Then confirm by hand, because the point is the file:

```sh
find /System/Volumes/Data/mnt/swingdata/Mark-Liversedge -name '*.mov' -newermt today
python3 -c "import json;print(len(json.load(open('/System/Volumes/Data/mnt/swingdata/ppcp-ledger.json'))['captures']))"
```

---

## 2. Where it is blocked — ONE question

The host asks correctly. The phone receives the request. The phone refuses it:

```
HOST   19:25:43      capture_request → "iPhone 16" for 4 Stream(s), shot e3c8841f… pre_ms 2000 post_ms 1000
PHONE  19:25:43.531  transfer capture_request received  shot e3c8841f… pre 2000ms post 1000ms
PHONE  19:25:43.532  transfer capture_request → absent  shot e3c8841f… — outside_buffer
```

Everything up to and including the question works and is proven on hardware. Everything after it
depends on that answer.

> ### ⛔ THE OPEN QUESTION
> **Is `outside_buffer` true?**
>
> Either the phone genuinely does not hold that moment, **or we are asking about the wrong
> moment** — the host's `t0` is converted into the phone's capture timebase, and if that
> conversion lands in the wrong place the ring is correctly saying "not here" about an instant
> that never existed.
>
> **This has not been measured. Measure it before doing anything else.**

### The diagnostic that settles it

`FragmentRing.extract` (`Packages/Core/Sources/CaptureCore/Capture/FragmentRing.swift:~308-317`)
returns `.absent(reason: outsideBuffer)` when no fragment overlaps the requested range. Log, at
that point, **the requested range and the span the ring actually holds** — `overlapping` is
already in scope, and `CapturedFragment` carries `startNs`/`endNs`.

Then read them off the phone (see §4) and compare:

* **No overlap, and the ring's span is nowhere near the request** → the timebase conversion is
  wrong. Look at `RecordingSession.serveCaptureRequest` (`Sources/Platform/Capture/RecordingSession.swift:~700`),
  specifically `peer.instant(t0Ns, on: t0TimebaseId, expressedIn: PpcpTimebases.captureId)`.
  ⚠ A relation evaluated outside its own domain fabricated a ~460 ms sigma against a real phone
  in August, and PPC's own code notes a two-way relation between two since-boot clocks "can read
  minus several million milliseconds". This is the likeliest remaining cause.
* **Overlap exists but the request is wider** → design §9.4's partial problem, and the phone
  should answer `partial` rather than refusing the whole clip.
* **The ring is empty** → retention is not running when we ask, despite `armed`.

---

## 3. Three theories that are DEAD — do not spend time on them

Each was plausible from reading the code, and each was disproved by measurement. They are
recorded so nobody pays for them twice.

| Theory | How it died |
|---|---|
| **The phone's transfer drain never runs** — `startTransferring()` is called only from the arm path and captures an immutable `hosted`, so a reconnect leaves it bound to a dead link | The phone's own log shows the drain alive and sending: `transfer sent 32768 byte(s) this pass`, repeatedly, then `drain stopped`. It works. |
| **We ask for a wider window than the ring holds** (design §9.4) | `PINPOINT_PPCP_PRE_MS=300 PINPOINT_PPCP_POST_MS=300` was refused **identically**. 2000 ms and 300 ms both `outside_buffer`. Window size is irrelevant. |
| **The phone has disarmed before the request arrives**, so `retainedClip` hits `!isRetaining` and returns instantly | The `!isRetaining` branch now reports `not_retained` (this session's change). The answer is still `outside_buffer`, so the ring **was** running. |

⚠ **A one-millisecond turnaround is not proof of a guard.** That inference produced the third
theory above. `.531 → .532` is fast enough to be an in-memory index lookup.

---

## 4. The rig — use it, do not rebuild it

Built this session precisely because three theories died to measurement and none to argument.

```sh
make integration-device STUDIO=<PinPointStudio binary> [EXPECT_CLIPS=1]
```

One command, both products, real hardware, nobody in the room. It:

1. starts PinPointStudio offscreen under `tools/probes/ppcp_assert.qml`, which performs the
   procedure a person performs — **enable the phone's camera, wait for clock agreement under
   5 ms, start the capture session** — then asserts on the clip chain;
2. runs `DeviceSessionTests` on the phone, which arms itself (`model.arm()`) and injects its own
   swing (`SyntheticAudio.oneSwing`) — no hands, no noise needed;
3. pulls the phone's own diagnostics off the device;
4. leaves `~/pinpoint-diags/<stamp>-integration/` with host log, device log, phone log and the
   usbmuxd stream;
5. **fails if either half fails**, and prints a `PROBE DOCTOR` line naming the first rung of the
   chain that did not happen.

### Reading the phone

```sh
make pull-diags OUT=<dir>          # Documents/ppcp-diag.log → <dir>/ppc-diag.log
```

`PpcpLog` writes to os_log, stdout **and** this file. The file is the only one that can be read
without breaking what it measures.

### Traps, all paid for

- ⛔ **Never `devicectl … --console`.** It bridges stdout while holding a CoreDevice tunnel; the
  kernel logs `setConfigurationGated`, `AMPDeviceDiscoveryAgent` follows, every usbmux tunnel
  dies. It is not a confound, **it is the cause** of the link drops. Zero re-enumerations in the
  one run that avoided it. Hours were lost to disconnects this tool created.
- ⛔ **`idevicesyslog` carries none of `PpcpLog`.** It reads the legacy syslog relay, not the
  unified log. Do not install it expecting to see anything.
- ⚠ `/usr/bin/log`, never bare `log` — the latter is a zsh builtin that silently returns nothing.
- ⚠ **A live microphone makes the host deaf to nothing.** With `PINPOINT_PPCP_ACCEPT_ALL=1`, any
  ambient bang becomes a committed shot that occupies the pipeline for 15–40 s and starves the
  phone's real one. Mark hit this by talking in the room. Consider having the probe clear
  `appSettings.acousticShotDetectionEnabled` and use `--corroborate` instead, which injects the
  host's evidence deterministically and exercises the corroboration *rule* rather than bypassing
  it.
- ⚠ `pkill -f "PinPointStudio"` also matches Qt Creator's `clangd`. Use
  `pkill -f "PinPointStudio.app/Contents/MacOS"`.
- ⚠ Do not launch the rig with `&` *and* a background runner. The completion notification will be
  the wrapper shell, not the run, and you will read a half-finished directory.

---

## 5. What is proven on hardware

- `capture_request` goes out naming genuinely-open `shot_windowed` Streams, read from the peer's
  own Stream table, with the right window. It fans out across every connected phone.
- The phone **receives** it and answers — the answer is just `absent`.
- Clock agreement reaches 1.28–4.7 ms and the host's 5 ms arbitration gate is met.
- Shots arbitrate, corroborate and commit on the host; a session starts; the pipeline runs.
- Preview works throughout (562 segments in one run).
- The phone independently cuts and transfers its **own** captures at 12–15 MB, so its ring,
  encoder and transfer queue are all functional. It is only *our* request it refuses.

Everything downstream in PinPointStudio — `admit` → write → `frames.t_us` → `origin` →
`capture_committed` → re-analysis — is written and unit-tested and **has never run against a real
clip**. Assume it is unproven.

---

## 6. Two known defects, deliberately not fixed

Recorded rather than repaired, because the stop condition is a clip on disk and neither blocks it.

1. **`onPayloadBegin` has no ownership filter** while `onCaptureAnnounce` does
   (`src/Video/VideoInputPpcp.cpp`). `dispatchEvent` hands every event to every instance bound to
   the peer, so a non-owning instance processes a payload that is not its business and reports a
   drop. Attribution proved it: announces accepted by `[inst src:camera:wide]`, drops reported by
   `[inst src:camera:ultra_wide]`. **Noise, not fault** — but it sent this session down a false
   trail twice before the log lines said who was speaking.
2. **`commitShot`'s unarmed message is wrong.** It prints "still processing the previous shot" for
   *any* unarmed state, including "no capture session is running at all", which is a completely
   different problem. That sentence cost an hour.
3. **The phone sends no `buffer_status` at all** — zero in a full run. So the host's retention-
   target clamp has nothing to clamp against and design §9.4's open question cannot be closed from
   the host side.

---

## 7. Commits from this session

PinPointStudio: `c4fc1e4` (loud drops, document-on-landing, `frames.t_us`, ACCEPT_ALL),
`f01e44f` (probe drives the procedure, doctor mode, clip-chain counters, instance attribution),
`b833c36` (window override).
PinPointCapture: `6a52651` (transfer diagnostics, un-swallowed `try?`), `c4019c3` (`.serialized`
— the fix that made the automated suite reach a Session for the first time), `97133f1` (phone
file log, `serveCaptureRequest` instrumented), `5cc7a6a` (the rig).

Uncommitted at handover: `not_retained` honesty in `AVFoundationCaptureDevice.retainedClip`, and
the stay-armed-until-served wait in `DeviceSessionTests`.

---

## 8. How this went wrong, so it does not again

Three times this problem was reported as solved when it was not. The mechanism each time was the
same: **a proxy was read as the result.** A byte counter. A green test. A passing suite. None of
them was a file in a swing folder.

The rig in §4 exists to make that impossible — its verdict is the file, and it has never once
reported a pass. Keep it that way: if it goes green, check the disk anyway.

And when a theory arrives from reading code, **measure it before repairing it.** Three did, all
three were wrong, and each cost less than an hour only because the rig was there to kill it.
