# HackMotion integration — implementation brief

**Status:** A, B, B′, C, D and E shipped — latest `0c7d128` (Phase E, 2026-08-18). **E2** (the
no-sensor backlog) and **E3** (studio verification) are next, then **F**; **G is deferred to its
own plan**. See §5's table for per-phase state. Written 2026-08-15, **before the library
existed** — see §0 for the nine places that made it wrong.
⚠ **§0 IS A SNAPSHOT OF 2026-08-17, NOT A LIVE STATUS.** Its rows record what was wrong with the
ORIGINAL text and what was true when each was written; where a row has since been overtaken by
work landing, it says so inline. Do not read an unqualified row as current.
**Scope:** add the HackMotion wG3 wrist sensor to PinPoint as a new IMU type, with its own
calibration process, feeding the Wrist session, the metric catalogue and the measure
vocabulary — and then use it as a **criterion instrument** to validate the wrist metrics we
extract from our own Witmotion placements.

**Read first**
- [`analysis_pipeline_developer_guide.md`](../developer/analysis_pipeline_developer_guide.md) —
  standing rule before any metrics work. §3 (capabilities/context/stages), §4 (the wrist
  profile), §6 (adding a stage, and landing dark).
- [`imu_frame_contract.md`](../design/imu_frame_contract.md) — §2 convention, §4 stored frame,
  §5 `q_anat = A·q_raw·M` and the joint-DOF axis table.
- [`deferred_sources_design.md`](../design/deferred_sources_design.md) — the whole thing. This
  brief is its first real consumer.
- The library's own `docs/specification.md`, `docs/design.md` and `docs/design-review.md` in
  `../libhackmotion`. Bare **§x** below is the specification; **L §x** is the library design.

**The library is a separate team's work.** We consume it; we do not modify it. ⚠ Friction found
while writing this brief was recorded as R14–R20 in a `docs/design-review.md` that **does not
exist in the published repo** — libhackmotion ships only `specification.md` and `design.md`, and
its history begins 2026-08-16, the day *after* this brief. Those review items were discharged
into the library's code and design doc rather than answered in a tracker. Do not go looking for
the file.

---

## 0. Corrections — this brief was written before the library existed

Nine findings from reading the published library and the current tree, recorded 2026-08-17 while
building Phase A. **Everything below this section is the original text; where it conflicts with
this table, this table wins.**

| # | The brief says | Actually |
|---|---|---|
| 1 | §3.5: the first ~2 s of a stream have no host time; "drop them and count them" | **Obsolete — there is no dead zone.** `clock.h` is explicit: `HM_CLOCK_HAS_FIT` is set from the *first live frame*, so every sample from the first frame onward carries a `host_time_us`. `HM_CLOCK_SHORT_BASELINE` now gates only whether the *rate* is independently fitted or seeded. `HM_SAMPLE_NO_FIT` fires only before the very first frame. Nothing to drop, no count to surface. |
| 2 | Phase E delivers "a stitched ~800 Hz wrist lane"; §10.3 "Phase E delivers 800 Hz" | **The device buffer is motion-adaptive**, floored at ~100 Hz (index step 8) and capped at ~799 Hz (step 1). A still pre-roll replays at 99.9 Hz; the swing itself replays in full. Window coverage of 16–50 % is *correct*, not a fault, and a narrower request does **not** come back denser — density is set by the motion, not the width. The §7 size estimate ("2 s × 800 Hz × 2 units ≈ 128 KB") is therefore an upper bound. |
| 3 | Issuing `a1` in place avoids the recording gap | **It does not.** The sample counter stalls for the pull's own duration — 289 ms mean across six measured pulls, 90–99 % of the bracket. Reported as `hm_history_block.self_recording_gap`, which falls *outside* `requested` by construction. The clock fit must re-anchor at every bracket close; one fit cannot span a pull. |
| 4 | Phase E "implements `deferred_sources_design.md` as written" | **True when written, and Phase E BUILT it — ⚠ this row is now historical (`0c7d128`, 2026-08-18).** The correction it made still stands: Phase E *built* the mechanism rather than consuming an existing one, which is why it was a large phase. As shipped: `RamPayloadSource`, `CompositePayloadSource`, `ShotProcessor::State::Gathering`, `BoundImu::effectiveHz` + `highRateSpanUs`, and `interpolateImu` is a per-source binary search rather than the linear scan the design warned about (73-91× on a deferred-shaped window; ~36× on an ordinary one). ⚠ **`reserveSourceId` was NOT built, deliberately** — the deferred source turned out to be a live ring producer already, so the stitched lane reuses the id it has and the composite reroutes it; see `deferred_sources_design.md` §3.3. The design note is now marked AS BUILT with all four of its open items closed. ⚠ **And "as written" was still the wrong instruction**: three things in it were wrong and are corrected in place — the median grid statistic, the single-span stitch, and that allocator. |
| 5 | Keys are `m_wristFlexExt`, `m_wristDeviation`, `m_wristRotation` | **None of those exist.** Real series keys: `leadWristFlexExt`, `leadWristRadUln`, `forearmPronation`, `leadArmFlexion`. Real measure ids: `m_leadWristFlexExt_p1..p8`, `m_leadWristRadUln_p*`, `m_leadForearmRot_p*` (note the measure stem differs from the metric key). |
| 6 | "A new `MetricRoute` on each wrist metric" *and* new keys | Pick one, and the codebase pattern is unambiguous: **new keys + `preferKeys`**, the `lm.attackAngle` shape. Adding a second route to `leadWristFlexExt` would make the pair *not* separately addressable, which is the one thing Phase G cannot do without. |
| 7 | "requirement = a HackMotion binding" | ⚠ **`MetricRequirement` cannot express that.** It knows anatomical `imuRoles` plus fixed bools, so a HackMotion route is today indistinguishable from the Witmotion one. Needs a `hackMotion` bool mirroring `launchMonitor`, a matching `ShotContext::hasHackMotion`, a `missingForRequirement` clause and a `CaptureDevice` value. |
| 8 | *(unstated)* | ⚠ **`FusedStreams::streamFor(role)` returns the FIRST match.** Two bindings with the same role silently first-wins, so **wearing both instruments at once — the entire point — is not expressible today.** Fix this before any dual-worn capture. |
| 10 | *(unstated)* | **The `0x84` sensor-count defect never reached our data, and this row exists so nobody re-derives that.** libhackmotion `f89cea4` found the count is the reply's **length**, not `data[1]`; `84 02 01` is two sensors because it carries two payload bytes, and the leading `02` is the first sensor's *location code*, which equals the count on this device **by coincidence**. The count sizes a record (header + one block per sensor), so a wrong one misplaces every field after the first block — but on this hardware both readings give 2, so every sample we have ever decoded was correct and **no capture is invalidated**. The byte that looked spare was the second sensor's location code; `sensor_map_undecoded` is now `sensor_location[]`. We read neither, so the API change cost us nothing. |
| 9 | *(unstated)* | ⚠ **The library's digest ring defaults to OFF.** With it off, a history block reports `live_overlap_samples == 0`, which means *no evidence*, not agreement — and the stitch of §7 depends on that agreement. Set `digest_ring_capacity`. *(Done in Phase A.)* |

**One thing that improved.** §8.2's 64-byte `0x94` payload is no longer opaque in the
specification: it is eight Q14 quaternions, of which q5/q6 are the **palm** at each marker and
q7/q8 the **lower arm** — ⚠ palm first, the reverse of a stream record's block order — and the
separation within each pair *is* the raise that was performed. That is exactly the axis
information §4.2's presence angle is blind to. ⚠ **But the library keeps it opaque and exposes no
API to read it**; the intended substitute is `hm_calibration_presence_event`'s medoid pair and
averaged anchor. Worth raising upstream if Phase D wants the poses themselves.

### Decisions taken since, which override the text below

1. **Unit-keyed placement** — `imuPlacement` keys become `<deviceId>#lowerArm` / `<deviceId>#palm`
   for a HackMotion; Witmotion keeps the bare device id. *(Phase C. Phase A pinned a HackMotion to
   slot A and locked the control as an interim, which under-described it — one wG3 fills A **and**
   B.)* ✅ **Done in Phase C, and the interim is retired**: one wG3 genuinely fills A and B, and
   both paths now refuse and name the blocking slot if another sensor already holds one.
2. **The device-native calibration routine is mandatory**, not optional: forearm horizontal →
   continuous raise across the chest. **But the presentation is reused** — `BodyVizView`'s guided
   avatar for the poses (it already animates between two override quaternions and signals when the
   motion finishes, which is what the device's *watched* raise needs), then `ArmVizView` for live
   free-movement confirmation.
   ⚠ **HALF DONE. The `BodyVizView` guide shipped in Phase C; the `ArmVizView` confirmation did
   NOT** — it was deferred because `HmUnit::anatQuat`/`anatCalibrated` were stubs until Phase D,
   and nobody went back once Phase D made them live. **It is E2's first item**, and until then the
   flow still tells a coach the frame "is still being solved" when it has been solved, measured
   and committed.
3. **`hm.*` keys, and HackMotion grades when present** — `preferKeys: ["hm.leadWristFlexExt"]`,
   the launch-monitor pattern. ⚠ Consequence, stated once: the graded corpus then mixes
   instruments depending on what was worn, which matters for norm-building. The *comparison* is
   unaffected — both series are produced on every dual-worn swing and compared at series level.
4. **The frame constants are solved offline** from a `.hmwire` capture of single-axis motions,
   baked into `pp_tuned_constants.h` and pinned by a unit test. ✅ **Done in Phase D**:
   `kCandidate = 1` (C2), mounting `wg3-mount1`, repeatability measured across three captures.
   ⚠ It describes ONE strap position — move the strap and it must be re-selected.
5. **One phase per session**, orchestrated: Opus decomposes, briefs and reviews; Opus/Sonnet
   agents implement. Opus for anything where being subtly wrong is invisible — frame conventions,
   threading, buffer contracts, clock alignment.

---

## 1. Why this is worth doing, stated honestly

Two reasons, and the second is the larger one.

**It is a better instrument for the wrist than what we have.** `wrist_angles.h` carries a
known limitation in its own header: *"~10–15° of FE↔RUD cross-talk remains because the
no-magnetometer heading (yaw) is unobservable between two sensors."* Two independently-fusing
6-DOF sensors cannot agree on heading, and the error lands squarely on the split between
bow/cup and hinge — the two channels the Wrist session exists to measure.

HackMotion has the same physics and, measured, does not have the same problem: under the
configuration its app uses, the **relative** angle stayed within **0.58° over five minutes**
while its two units individually drifted 1.95° and 1.13° (§6.2). The drift is substantially
common-mode. §12 says plainly that *why* it cancels so completely is not understood and should
not be assumed outside that configuration — so this is a measured property to verify in our
sessions, not a guarantee to inherit.

**It gives us a criterion for a measurement we currently cannot check.** This is the part that
outlasts the device. We ship wrist metrics graded against corridors, and we have never had an
independent reading of the same quantity on the same swing. The launch-monitor work already
built the machinery for exactly this (see Phase F, §5) — two separately-addressable keys for
one quantity, so a measure can prefer the better instrument while the validation comparison
stays possible. HackMotion is the wrist's version of that.

⚠ **And the trap, named up front.** If both systems share a failure mode, agreement is
*correlated error, not corroboration*. Both are magnetometer-free; both fuse per-unit. The
0.58°/5 min figure is the evidence that they differ where it matters.

⚠ **THIS PARAGRAPH USED TO END "…and Phase G exists to confirm that in our own captures before any
HackMotion reading is allowed to grade anything." THAT SENTENCE READ THE COMPARISON BACKWARDS AND
IS REMOVED** — it cost a planning session in August 2026, when it was quoted back as a reason to
withhold grading after Phase G was deferred. **The wG3 is the CRITERION.** It is the best wrist
measurement available to us, we accept its readings as relatively accurate, and it is what we
grade AGAINST; assessing its accuracy is not this project's job. Phase G assesses **our Witmotion
estimate** against it. The correlated-error caveat above still stands as a caveat on what an
AGREEMENT between the two would prove — it is not a licence gate on the criterion.

---

## 1a. ⚠ We report in ISB. We are not reimplementing the vendor's metrics

**This is the deliberate difference between this integration and the vendor's own application, and
it is a feature rather than a compromise.** The device is used here as a *criterion instrument* — a
better measurement of the wrist than a camera or a 6-DOF IMU pair can give — and what we report from
it is **ISB / Wu et al. 2005** (`ref.wu2005`), the recommendation of the Standardization and
Terminology Committee of the International Society of Biomechanics. Twelve authors, five countries,
published by the society under its standards collection.

The canonical statement is [`../design/pinpoint_sign_conventions.md`](../design/pinpoint_sign_conventions.md)
§Rule 0 — *a published standard outranks a popular product* — and it is enforced by
`metric_catalogue_test`, which asserts all four ISB joint angles keep ISB polarity. Nothing in this
integration may weaken that, and the phases below inherit it whether or not they mention it.

Why it matters commercially as well as technically: a vendor can change their convention in the next
release, and a standard is the thing that lets two datasets be compared at all. A number we publish
is readable next to any biomechanics literature and any other ISB-conformant system; a number in a
vendor's private convention is readable only next to that vendor.

⚠ **AND IT SETS A TRAP FOR PHASE D THAT NOTHING ELSE IN THIS DOCUMENT WOULD WARN YOU ABOUT.** The
vendor's application **reports the inverse of us on bow/cup** — extension (cupping) positive, flexion
(bowing) negative. So the most natural sanity check available during the frame solve — put the device
on, open their app, compare — **will show an inverted sign on the primary channel, and it will look
exactly like a wrong solve.** It is not. Verify against a known single-axis motion and the ISB
polarity table, never against the vendor's display.

Three consequences that are not optional:

- **Phase D's target frame is ISB**, not "some PinPoint frame". Its constant maps
  HackMotion-anatomical → **ISB**-anatomical, and the DOFs its acceptance moves are ISB's:
  flexion/extension, radial/ulnar deviation, pronation/supination, with `+flexion` = bowed,
  `+deviation` = ulnar, `+pronation` = pronation. ⚠ And those DOFs must be moved in a **recorded
  direction** — the sign is what selects, and an unlabelled motion selects nothing.
- **Phase F's `hm.*` series must land in ISB polarity.** If it carried the vendor's sign while the
  Witmotion series stayed ISB, Phase G's agreement report would show a spurious *anti*-correlation on
  bow/cup and the obvious reading of that chart would be that one instrument is broken.
- **ISB does not govern everything**, and claiming it would fail review faster than not claiming it.
  Four metrics are ISB joint angles; the club, ball, turn magnitudes, image-plane body lines and
  normalised displacements are not. The sign-conventions document has the table.

---

## 2. The five structural facts that shape every decision below

Everything in this brief follows from these. Read them once here rather than rediscovering
them in five places.

**F1 — One BLE peripheral, two segment bindings.** The device is two units on a cable: wire
block 0 is the **lower arm**, block 1 is the **palm** (§6.3, fixed by the wiring). Our
pipeline binds one `SourceId` to one `SegmentRole`, and Wrist Motion already maps slot A →
`LeadForearm`, slot B → `LeadHand` (`swing_analysis.h:58`). So **one HackMotion satisfies both
wrist slots**, and it must register **two** EventBuffer sources. This is the single biggest
departure from `ImuInstance`, which is one device = one source = one role.

**F2 — Calibration is applied on-device, in an anatomical convention we do not have.** The
device computes and applies its own mount transform; a client issues two pose markers and
reads quaternions already in the wrist's anatomical frame (§8.1). The 64-byte result payload
is deliberately undecoded. So there is no `A`/`M` to solve — but the frame we receive is
*HackMotion-anatomical*, not *PPS-anatomical* (`e_y` = long distal, `e_x` = flexion,
`e_z = e_x × e_y`). **A constant per-unit rotation between the two conventions has to be
solved empirically, once.** That is Phase D and it is the linchpin.

**F3 — The composition order is the opposite of ours, and the obvious check cannot catch it.**
§6.7: the streamed quaternion maps **world → body**, so the device's relative rotation is
`q_palm ⊗ q_arm*`. We read the wrist as `qFore⁻¹·qHand` (`imu_frame_contract.md` §5). Those
are the same rotation expressed in different frames:

```
q_rel_pps = q_arm* ⊗ q_rel_hm ⊗ q_arm
```

The **angle is identical** under both — `2·acos|q_a·q_b|` is convention-blind — so a wrist
angle that "looks sensible" proves nothing and every decomposed component sign can be wrong.
⚠ Verify against a **known single-axis motion**, never against a plausible-looking angle.

**F4 — The live stream is not the data; it is the clock reference for the data.** A 250 ms
downswing is 6 samples at the 25 Hz live rate and ~200 in the device's internal buffer (§7.6).
Real data comes from a post-hoc `a1` retrieval of a ~4.5 s window, which takes about as long
as the window spans. **HackMotion is the first deferred source**, and
`deferred_sources_design.md` was written for it.

**F5 — Neither system's timebase can validate the other's timing.** The library's own §10
measures a **2.2 ms/s** host-clock drift it cannot attribute; our Witmotion path back-dates
with a fixed `kImuBleLatencyUs = 30 ms` that nobody has measured. So the validation in Phase G
is **event-anchored, not clock-anchored**: align both traces at the phase ladder and compare
*values at P-positions*, not timings. HackMotion validates our **angles**; it validates
nothing about our **tempo or sequence**. Say so in the report.

---

## 3. Architecture — where the code goes

### 3.1 A sibling of `ImuInstance`, not a subclass of `ImuBase`

`ImuBase`/`WT9011DCL_Base` is a Witmotion register protocol with host-side fusion, host-side
anatomical solve, and one sample stream. HackMotion shares none of that: the library owns
framing, decode, fusion is on-device, calibration is on-device, and it emits two units. Fitting
it under `ImuBase` would mean adding "second unit" and "someone else applied the calibration"
concepts to a class that has one consumer and would then have two contradictory ones.

**Recommended: `HmInstance` (`src/IMU/hm_instance.h/.cpp`)** — a peer of `ImuInstance`,
owning one `hm_session` and one `BleImuTransport`, registering **two** EventBuffer sources.
`ImuManager` grows a second entry kind rather than a second manager: the Settings panel, the
wizard IMUs step, the resource monitor and `sessionImuExcluded` all key on a device id and
should keep doing so.

What it must reuse, not reimplement:
- `BleImuTransport` for the link (see §3.3 for the change it needs).
- `BleAdapterPool` for adapter selection.
- `DeviceEnumerator` for discovery, with a HackMotion filter.
- The shared `ImuManager::m_ioThread` — one session lives there, satisfying the library's
  one-thread contract.

### 3.2 The drain loop, and why the stop barrier is free

The library never calls us; we drain it (L §3.3). One `QTimer` on the I/O thread re-armed to
`hm_session_next_due_us()` after **every** call into the session, plus a drain on every
`dataReceived`. Then:

```cpp
// on the I/O thread only
hm_session_on_bytes(s, p, n, EventBuffer::nowMicros());
pump();   // poll_writes → transport, poll_live → both rings, poll_events → GUI signals
```

**This satisfies the EventBuffer producer contract by construction.** Our stop barrier is
"stop draining", which we implement ourselves: `HmInstance::stop()` runs a
`BlockingQueuedConnection` onto the I/O thread that stops the timer and disconnects
`dataReceived`, then calls `hm_session_close()`. When that returns, no ring write can be in
flight — the same guarantee `ImuInstance::stop()` gets from `detachBuffer()`, obtained more
cheaply.

### 3.3 One change `BleImuTransport` needs

`BleImuTransport::UuidConfig` derives the notify and write characteristic UUIDs by
substituting a fragment inside the service UUID (`ble_imu_transport.cpp:355-362`). That works
for Witmotion (service `…ffe5…`, notify `…ffe4…`, write `…ffe9…` — one base) and **cannot
express this device**: the data characteristic sits under a different base from its service,
and it is a *single bidirectional* characteristic rather than a notify/write pair.

Add an explicit-UUID form to `UuidConfig` (full `QBluetoothUuid`s, notify and write allowed to
be the same characteristic), keeping the fragment form for Witmotion. Small, self-contained,
and Phase A cannot start without it.

### 3.4 Two sources, one device

`HmInstance` registers two EventBuffer sources at construction, both `DeviceKind::Imu`,
schema `imu_sample_v2`, with identifiers `<deviceId>#lowerArm` and `<deviceId>#palm`. Each
drained `hm_sample` splits into two `ImuSample` writes.

⚠ **Per-source monotonicity is the constraint.** `MergerState::enforceMonotonicity`
(`event_buffer.cpp:84`) rewrites any timestamp `<= last_emitted` for that source. Live samples
carry `hm_sample.host_time_us` **mapped through the fit**, which is monotonic in index and
therefore safe — but see §3.5.

### 3.5 ~~The first ~2 seconds of a stream have no host time~~ — WRONG, see §0 #1

**This section is obsolete and its Phase B decision must not be implemented.** The library sets
`HM_CLOCK_HAS_FIT` from the **first live frame**, so there is no dead zone, no unusable prefix
and no dropped-sample budget to surface. `HM_CLOCK_SHORT_BASELINE` survives but gates only
whether the *rate* is independently fitted or seeded, and its cost shows up honestly as a wider
`residual_max_us` in `hm_clock_error_at()` rather than as missing samples.

What was right, and still is: **never fall back to arrival time.** Arrival is one-sidedly late,
and mixing two timebases inside one source is exactly the kind of thing that looks fine and
corrupts a capture. That reasoning simply no longer has a case to apply to.

Also worth knowing for Phase B: `sample_index` and the per-unit `device_time_us` are populated on
**every** sample regardless of fit state, because they are derived from the frame alone. Analysis
anchored on device time — comparing two instruments at matched events rather than on a shared
clock, which is what §8.2 asks for — never waits on the host mapping at all.

---

## 4. The calibration process — a second, device-native flow

### 4.1 What is different

| | Witmotion (ours) | HackMotion (device) |
|---|---|---|
| Who computes | Host, `imu_calibration::solveSegment` | The device (§8.1) |
| What we store | `A`, `M` per segment | Nothing — there is nothing to store |
| Physical routine | Arm-down reference + T-pose + functional axis swings | Forearm horizontal → **continuous raise** ~30° across the chest (§8.2) |
| Precondition | None beyond a connection | ⚠ **The stream must already be running** — the device observes the raise, which two static samples cannot supply |
| Evidence of success | `anatCalibrated` + mount-deviation + gravity-error thresholds | One number: the reference-pose relative angle |
| Lifetime | Session; lost on remount | Session; lost on remount, **on power cycle, and on a plain disconnect** (§8.3, 0.70° → 18.80°) |

### 4.2 ⚠ The residual is a presence check, never a quality score

§8.2 measured three calibrations on one mounting: correct routine **1.96°**, wrong axis
**6.10°**, **no raise at all 0.70°**. The calibration carrying no axis information scored
*best*, because the reference-pose angle tests only the zeroing — the half that cannot fail.

**So: never rank attempts on it, never show it as a score, never let a coach "recalibrate
until the number goes down".** Its one sound use is catching *calibration never happened or
was lost*, where the gap is an order of magnitude (applied 0.4–3.8°, uncalibrated 15–19°). The
library refuses to expose a quality API at all; our UI must not reintroduce one.

### 4.3 Reconnect is not resume

A plain BLE disconnect destroys the calibration. Our auto-reconnect must drive the binding
back to uncalibrated and **tell the coach to re-run the routine**, not silently resume. The
library makes this un-expressible (link-down forces `HM_CAL_UNCALIBRATED`); our UI must match
it rather than paper over it.

### 4.4 Wearing both systems at once

Phase G requires a golfer wearing Witmotion **and** HackMotion. Today that is two routines
back to back: ours (arm-down, T-pose, twist and flex swings) then theirs (horizontal, raise).
That is a lot of standing about.

Open question raised with the library (R19): the device zeroes at pose 0 and finds the axis
from the *raise between* the poses. If pose 0 need only be a **known** pose rather than
specifically horizontal, the two choreographies may be combinable into one. Until answered,
**assume two routines** and design the flow to run them back to back without re-prompting for
things it already knows.

⚠ **The obvious test of this is wrong, and we proposed it before catching that.** Checking
whether the presence angle still collapses after a non-horizontal pose 0 measures the
*zeroing* — §4.2's half that cannot fail — so a wrong anatomical frame would pass. **Only a
known single-axis motion tests the axis half**, comparing cross-talk against a correct
calibration. That is Phase D's acceptance test, so we answer this as a by-product: run Phase D
twice, once after a horizontal pose 0 and once after a deliberately non-horizontal one, and
report the cross-talk difference. One extra calibration on a rig already set up.

---

## 5. Phases

Each phase is a session boundary. **Every phase before G lands dark** — behind
`hackmotion.enabled` (default off) — and the pipeline gates on *devices and data*, never on
session type (`analysis_pipeline_developer_guide.md` §4).

| Phase | Deliverable | Gate to move on | Status |
|---|---|---|---|
| **A** | Transport + enumeration | A wG3 appears in Settings → IMUs and connects; explicit-UUID `BleImuTransport` lands with a Witmotion regression | ✅ `3b4980f` |
| **B** | Live streaming → two EventBuffer sources | Two lanes in the data viewer at the right rates; no monotonicity violations. ⚠ *No "no-fit drops" — §0 #1* | ✅ shipped `9bc8aee`/`17ccca0`/`cdddefa` — ⚠ **two criteria never checked**, both need a worn sensor (see E3) |
| **C** | Device-native calibration flow | A coach can calibrate end to end; presence angle recorded, never scored; reconnect invalidates. Includes unit-keyed placement (§0 decision 1) | ✅ verified on hardware — ⚠ leaves §12.3 (no state-machine test) and §12.4 (wizard matrix) open, plus the stale ArmVizView deferral (see E2) |
| **D** | **Frame reconciliation** — solve `R_unit` | A known single-axis wrist motion moves one PPS DOF and leaves the others near zero, from HackMotion data ⚠ *superseded — the real gate is the SIGN of a directed motion; see the Phase D section* | ✅ `17b0c9c` — C2 selected, repeatability measured across three captures |
| **E** | Deferred history → SwingWindow | A shot produces a stitched **variable-rate** wrist lane — ~100 Hz over the still pre-roll, full rate through the swing (§0 #2) — with coverage, gaps and the fit in provenance. ⚠ Builds `deferred_sources_design.md`, does not consume it (§0 #4) | ⚙ `0c7d128` — mechanism built and tested; ⚠ **three hardware gates deferred to E3** |
| **E2** | Backlog: everything that needs no sensor | The stale Phase-D-era deferrals are cleared, the calibration state machine has a test, and the session wizard is exercised across the sensor combinations — so E3's studio time is spent on data, not on discovering a broken wizard | ☐ |
| **E3** | **Studio verification + first look at the data** | Built and run on the studio PC with a worn sensor: E's three gates, B's two unverified criteria, and a short findings note on what the wrist actually does at full rate. ⚠ **Keep the raw captures** — they become F's development fixture, so F needs no second trip | ☐ |
| **F** | New `hm.*` keys + measure `preferKeys` | HackMotion wrist metrics produced and separately addressable; measures prefer them; corridors unchanged. ⚠ Blocked on `streamFor` (§0 #8) | ☐ |
| **G** | ~~Validation against Witmotion~~ | ⚠ **DEFERRED ENTIRELY — it needs its own plan, and it is NOT a gate on anything in this one.** See the Phase G section for why the deferral costs nothing here | ⏸ deferred |

**Phase A, as shipped.** Explicit-UUID `BleImuTransport` + the first MTU plumbing in the tree;
`hm_looks_like_hackmotion()` discovery with the 90 s window; an `ImuVendor` discriminator on
`Device`; `ImuDeviceBase` so `ImuManager` holds two device kinds, with a **plural** `sourceIds()`;
`HmInstance` owning one `hm_session` on the shared I/O thread with the drain loop and stop
barrier; and `HmUnit` × 2 duck-typing as an `ImuInstance` so `ImuVizView`/`ArmVizView` need no
change. No EventBuffer source is registered and no sample is written — that is Phase B.

**Where a session BREAKS:** A→B (transport must be proven before framing), C→D (you cannot
solve the frame until you can calibrate), D→E (do not stitch a lane whose frame is unresolved),
F→G (do not validate a metric whose route has not landed).

### Phase A — transport and enumeration

- `BleImuTransport` explicit-UUID form (§3.3). Regression-test the Witmotion path.
- `DeviceEnumerator::ImuBleScanner` gains a HackMotion filter using the library's published
  constants (`HM_ADVERTISED_LOCAL_NAME`, `hm_looks_like_hackmotion()`).
- **⚠ Raise the scan window 30 s → 90 s** (`device_enumerator.cpp:135`). §2.1: the sensor
  advertises for only a few seconds after a button press. This also improves Witmotion
  discovery and costs nothing when a device appears immediately.
- `HmInstance` skeleton: session create, link up/down, bring-up, keepalive, drain loop. No
  EventBuffer writes yet.
- MTU: the library refuses below 96. Surface that as its own error, not a generic failure.

### Phase B — live streaming

- Two source registrations; split `hm_sample` → two `ImuSample`s.
- ⚠ **`ImuSample` stores accel in *g* and gyro in *°/s*, raw sensor frame** (`imu_sample.h`).
  The library gives linear acceleration in **m/s²**, gravity-removed. Convert, and note in
  provenance that this lane's accel is *not* the same physical quantity as a Witmotion lane's.
  Nothing downstream should compare them.
- ⚠ **Never average or aggregate the two units' accelerations.** They sit 3–8 cm apart and
  differ by ω²r — measured at 31–51 m/s² through a swing (§6.4). They are supposed to disagree.
- Carry `skew_us` (a stable 0.92 ms, worth ~0.9° at 1,000 °/s) into provenance rather than
  pairing the two blocks as simultaneous.
- ~~Drop-and-count the no-fit prefix (§3.5).~~ ⚠ **Nothing to drop — §0 #1.**
- ⚠ **Size the live ring from how often the host drains, not from a rate.** There is no live rate
  ceiling: 25 Hz at rest and 100 Hz in motion are two modes of a continuum, and dense bursts reach
  the full internal rate in *every* session containing motion. A ring sized from an assumed
  maximum is wrong under exactly the conditions that matter.

### Phase B′ — the addendum, and why it is not part of Phase B

Phase B shipped before the loss was noticed, so this is separate work that runs **before Phase D**:
the live lane is recording today and every swing it has written is affected. The finding, the
reason the record is not simply widened, and the two carriers that replace that are in §7 above and
in the plan's Phase B′.

Two things worth stating here because they are easy to get backwards:

- **Window-constant vs per-sample is the axis that decides where a field goes**, not how important
  it is. Calibration state and the config byte cannot change inside a swing — the link drop that
  would change calibration also ends the stream — so they belong in the stream's `device` object,
  where the plumbing already runs per unit id. Pinning and `QUAT_NORM_SUSPECT` can occur on any
  individual sample, so they need something that a window can *select* from.
- ⚠ **`NO_FIT` skips are counted, never ringed.** A sample skipped for having no mapped host time
  has, by definition, no host time to place it in a window with. A cumulative count is the honest
  form; a ring entry would need a timestamp it cannot have.

### Phase C — calibration flow

- `ImuCalibrationFlow.qml` gains a device-type branch (see §6.3 for the UI).
- Order is fixed and enforced: connect → **start stream** → `hm_calibration_begin()` →
  confirm horizontal → confirm raise → await result → confirm reference pose → done.
- `HmInstance` exposes `anatCalibrated` from the library's per-sample calibration state, and
  a `presenceAngleDeg` that the UI shows as *state*, not as a score.
- ⚠ `0x94` is not a verdict — it arrives for rejected attempts too. Never infer success from
  its arrival.

### Phase D — frame reconciliation (the linchpin)

Establish the constant quaternion mapping HackMotion-anatomical → PPS-anatomical, so that
everything downstream can treat a HackMotion lane exactly as it treats a calibrated Witmotion
lane.

⚠ **THIS IS ONE ROTATION, NOT TWO.** The `R_lowerArm` / `R_palm` framing this section used to
carry is retired: the device zeroes the relative rotation at its own pose 0, so the two units
share a frame and one constant serves both. The plan's own §8.3 evidence — the presence angle
collapsing to 0.36–0.79° once calibration is applied — is what settles it. ⚠ With the caveat
that this settles **one R or two** and says nothing whatever about where that shared frame's
axes point, which is the entire remaining question.

⚠ **AND IT IS A SELECTION AMONG FOUR, NOT A CONTINUOUS FIT.** Both frames are built on the same
two anatomical landmarks — a limb axis and a flexion axis — so the map between them is
axis-aligned, and a determinant-+1 rotation carrying `X → ±Z` and `Z → ±X` is one of exactly
four. `src/IMU/hm_frame.h` holds the table, the derivation, and the two assumptions it rests
on; `hm_frame_test.cpp` pins the properties on synthetic quaternions with no hardware.

⚠ **"PPS-anatomical" HERE MEANS ISB — see §1a**, and the DOFs the acceptance test moves are ISB's,
with `+flexion` = bowed, `+deviation` = ulnar, `+pronation` = pronation. Leaving the target frame
unnamed would repeat, in our own document, the exact omission that makes this phase necessary in the
first place: §8.1 never defines the device's own anatomical convention either.
⚠ **And do not sanity-check the solve against the vendor's application** — it reports the inverse on
bow/cup, so a correct solve looks wrong there. §1a has the trap in full.

⚠ **Which block is which is now settled and no longer a Phase D prerequisite.** libhackmotion
`f89cea4` gives three independent routes where there was one plus the cable: the `0x84` sensor map's
lever arms (0 → 0.00 m, 1 → 0.10 m, 2 → 0.26 m — no reading puts a 0.26 m segment on a hand); the
`0x94` payload's per-unit pose fields matched **blind** against both blocks (palm fits block 1 by
7.0°/8.2°, arm fits block 0 by 3.8°/2.5°, both attempts agreeing on all four); and acceleration
radius at 4,996 of 5,487 moving samples across 30 swing captures. The accel check is now a cheap
confirmation rather than a gate — and it needs **peaks over a genuine swing**: 91.1 % is a strong
majority and not a per-sample rule, the margin grows with the vigour of the motion, and at rest,
where both units read ≈0, it says nothing at all.

**Method.** Capture a set of deliberate single-axis wrist motions — flexion/extension,
radial/ulnar deviation, forearm rotation — performed slowly, so that neither timebase matters
and the axis being isolated is not smeared. ⚠ **Record the DIRECTION of each one** (see the
acceptance below; an unlabelled capture cannot settle this). Then run
`tools/hm_frame_select.py` over the recording, which reports what each candidate makes of each
motion and names the survivor.

⚠ **Both scripts need an interpreter that is not this machine's `python3`.** `hm_capture.py`
needs `bleak` for the radio, and the Homebrew Python is PEP 668 externally-managed so it cannot
be installed into. A shared virtualenv sits at `~/Projects/.venv-hackmotion` — outside both
repos, so neither `.gitignore` has to know about it.

⚠ **PinPoint must be disconnected** — the device allows one connection at a time. ⚠ **And the
calibration must be in the SAME connection as the motions**: it does not survive a disconnect or
a power cycle, so it cannot be done beforehand.

#### ⚠ The capture protocol lives in `hm_frame_select.py --protocol`, and not in prose here

`~/Projects/.venv-hackmotion/bin/python tools/hm_frame_select.py --protocol` prints it. It is
kept there rather than duplicated here **because the first attempt at this capture failed on
exactly that duplication**: `hm_capture.py` prints its own agenda while recording — *hold still,
one single-axis move, a fast flick, a few swings* — which is **libhackmotion's protocol-spec
reconciliation, a different job**. An athlete following the screen produces a recording that
cannot select a candidate, and the first real capture did precisely that: the wrist was swept
bow-*through*-to-cup so only 3 of 15 flexion bursts shared a sign, deviation got one burst, and
the one burst that read as rotation sat 68° off the limb axis. ⚠ **So the two on-screen
instruction sets disagree, the capture tool's is the louder one, and it is the wrong one for
this phase.** Say so to whoever performs it.

The three rules that capture has to obey, and each of them killed the first attempt:

1. ⚠ **One direction from neutral, then back — never a sweep through it.** The SIGN is the only
   thing that selects; a motion passing through neutral to the other side produces both signs
   and settles nothing.
2. ⚠ **Slowly.** Slowness matters more than range; a fast motion smears the axis being isolated,
   and cross-talk is how a capture gets rejected.
3. ⚠ **Include a deliberate forearm rotation.** It is the only test of the limb-axis assumption,
   and without it the tool reports `NO EVIDENCE` rather than confirming it.

The composition needs no new machinery: the device streams world→body and our anatomical
quaternions run body→world, so

```
q_anat_pps = toAnatomical(A = identity, q_raw = q_hm*, M = R_ph)
```

is the **existing** `imu_calibration::toAnatomical`, with the constant in the `M` slot and
`wrist_angles.h` used **unmodified**. `A` is identity by design rather than as a stub — the
device already referenced the pair at its own pose, and the constant offset from our neutral is
what `wristRel`'s Address reference absorbs.

⚠ **The conjugate belongs at that one site.** `hm_sample_convert.h` stores the streamed
quaternion verbatim and explains at length why it refuses to conjugate; that refusal is
correct. Conjugating in two places is the same as conjugating in none. And note
`hm_quat_relative()` returns `q_palm ⊗ q_arm*`, the conjugate of what a decomposition needs —
correct in its own convention, and unfit as a decomposition input.

#### Acceptance — and the original criterion here provably could not accept

This section used to read: *"a pure single-axis motion must move one PPS DOF and leave the
others near zero"*. ⚠ **THAT TEST CANNOT SELECT A CANDIDATE. ALL FOUR SCORE EXACTLY ZERO
CROSS-TALK, AND SO DOES A REVERSED COMPOSITION ORDER.** They differ only in the SIGNS they
produce. A capture that is beautifully single-axis, decomposed to a textbook `0.00°` on both
secondary channels, still leaves eight possibilities standing. `hm_frame_test.cpp` §C asserts
this as an executable fact so it cannot be quietly reintroduced.

So acceptance is **two** things, and neither substitutes for the other:

1. **Cross-talk — as FALSIFICATION, not selection.** It tests the axis-alignment assumption the
   four-candidate reduction rests on. If every candidate shows large cross-talk, the reduction
   is wrong and the phase falls back to a general empirical solve. Small residuals are expected:
   a human performing a "pure" single-axis motion contributes most of them.
2. **Sign against a DIRECTED motion — this is the selector.** Each candidate has a unique
   `(flexion, deviation)` signature, so **two labelled motions settle it**: one known bow, one
   known ulnar deviation. In our convention `+flexion` = bowed, `+deviation` = ulnar,
   `+pronation` = pronation.

⚠ **"Pure flexion" is not enough information; "bowed, then returned" is.** This is the single
most important change to this phase, and the reason the capture protocol needs the athlete to
record directions rather than just clean motions.

⚠ There is a third assumption — that the device puts its limb axis on Y — which only a
deliberate forearm **rotation** tests. `hm_frame_select.py` checks it against the recording and
says `NO EVIDENCE` rather than `ok` when the capture contains no such motion.

⚠ Do **not** accept on "the wrist angle looks right" — F3 says that passes with the composition
order reversed and every sign wrong. ⚠ And do **not** sanity-check against the vendor's
application: it reports the inverse on bow/cup, so a **correct** selection shows an inverted
sign on the primary channel there. §1a has the trap in full.

⚠ **A degeneracy worth knowing:** reversing the composition order maps one candidate's
signature onto another's. That is harmless — only the composite transform is observable and
only the composite ships — but it does mean "frame" and "order" cannot be separated from
flexion and deviation alone. The pronation sign separates them, which is one more reason to
capture a rotation.

**Deliverables:**

- `src/IMU/hm_frame.h` — the candidate table, the composition, the derivation, and the two
  assumptions it rests on.
- `pinpoint::tuned::hmframe::kCandidate` in `pp_tuned_constants.h`, behind the
  `"hmframe.candidate"` dotted key. ⚠ Only the SELECTION is tuned; the candidates themselves are
  a structural fact about the two frames and stay at their source of truth, for the same reason
  `wrist_angles.h` keeps its own axis choices. Defaults to `kCandidateUnset` — a HackMotion lane
  reports **no** anatomical frame until a directed capture has chosen, rather than a plausible
  default.
- `src/Analysis/tests/hm_frame_test.cpp` — five sections, no hardware. The similarity identity
  by two independent routes; ⚠ that the angle cannot distinguish the two orders; ⚠ that
  cross-talk cannot distinguish the four candidates; that the sign signature can; and that the
  unselected state yields identity rather than a guess.
- `tools/hm_frame_select.py` — reads a `.hmwire` capture and names the surviving candidate.
  It does **no** wire decoding: libhackmotion owns every byte of that, and this tool owns only
  the frame maths.
- `HmUnit::anatQuat` / `mountM` / `anatCalibrated` wired, where `anatCalibrated` is the
  **conjunction** of the device having applied its calibration and a candidate having been
  selected. Either alone produces a quaternion that moves convincingly and means nothing.

⚠ **A test that asserts a check does NOT work is an unusual thing to write, and both of the
warned sections above are that.** They are what stands between a future reader and accepting a
mirrored pipeline on a beautiful-looking capture — the more so because this brief itself used to
recommend the check that cannot accept.

⚠ **The constant is per mounting convention, not per device.** If the strap position changes, it
changes. Record the mounting in the capture that produced it; `hm_frame_select.py` takes a
`--mounting` string and says so loudly when it is not given.

**Pronation, and what this device cannot give us.** `forearmPronation` is an ISB radioulnar
joint angle read against the **upper arm**, and a wG3 has no upper-arm unit — so that metric is
not available from this device at all, and nothing may be published under its name. What is
available is the angular **rate** about the forearm's long axis (`HmUnit::pronationRateDps`,
°/s), taken from the lower-arm unit alone. ⚠ Not from a difference of the two: during pronation
both units turn together and the wrist barely articulates, so differencing them cancels most of
the signal. Whether that rate earns a catalogue metric of its own is a separate design question
with its own sign-convention review, and is deliberately not answered here.

### Phase E — deferred history

This is `deferred_sources_design.md`'s first real consumer; implement it as written. Specific
to HackMotion:

- **Reserve at detection, collect at the gather.** `ShotController` commits a shot →
  `hm_history_reserve()` with a 3 s pre / 1.5 s post window (§7.6's recommendation, and the
  library's default). Retrieval then overlaps the post-roll we already take.
- **Gather deadline ≥ 6 s.** A pull takes about as long as its window spans, and the library
  serialises them. `deferred_sources_design.md` §6 lists this as open; it is now answerable.
- ⚠ **A second ball struck within ~3 s is the failure case.** The buffer is ~7.5 s and a pull
  takes ~4.5 s. The library warns (`HM_EV_HISTORY_EVICTION_RISK`); surface it to the coach
  rather than discovering a holed second swing later.
- ⚠ **Coverage is intervals and density, never a count.** An over-wide request comes back
  *holed*, not clamped — 33–58% coverage with no error (§7.1). Gate analysis stages on the
  measured effective rate in the window (`BoundImu::effectiveHz`, design §4.4), and write
  `largest_gap_us` into provenance: it is the number that decides whether impact survived.
- **One lane, stitched.** History is a superset of live over the same span, so the stitched
  `[live prefix] + [high-rate span] + [live suffix]` is one ascending variable-rate trace.
  ⚠ The superset claim is now **measured on every pull** rather than assumed: the block carries
  `live_overlap_samples` / `live_overlap_mismatches` (first evidence: 234 indices, 0 mismatches).
  ⚠ A zero *sample* count means **no evidence**, not agreement — always read the pair together,
  and see §0 #9 for the ring that has to be on for the check to happen at all.
- ⚠ **The pull's own recording hole** (§0 #3) is `self_recording_gap` and falls *outside*
  `requested`. A consumer stitching a session has no other way to know a span was never recorded
  rather than merely never requested.
- **Persist the clock fit with the block.** `hm_clock_snapshot` by value into swing.json, so
  re-analysis a year later reproduces the day's alignment.

### Phase E2 — the backlog that does not need a sensor

**Why it exists.** Development has run away from the studio, and unverified work has piled up
across three phases. E2 clears everything that can be cleared on a desk, so E3's studio time is
spent collecting data rather than discovering that the session wizard refuses to start.

1. ⚠ **The ArmVizView deferral is stale, and it is currently telling a coach something untrue.**
   Phase C deliberately left out the live free-movement confirmation, with this reason
   (`ImuCalibrationFlow.qml`): *"ArmVizView reads anatCalibrated + anatQuat and BOTH ARE STUBS ON
   AN HmUnit UNTIL PHASE D"*. That was correct then. **Phase D shipped and made both live** —
   `anatQuat` is now built from the selected frame whenever the device is calibrated, and
   `kCandidate = 1` is baked in — and nobody went back. Three consequences:
   - The flow still displays *"Live arm tracking from this sensor is not available yet — the
     conversion to ours is still being solved."* ⚠ It is not still being solved. It was solved,
     measured across three captures and committed. **This is visible in the product.**
   - The free-movement confirmation step §4's decision 2 called for is still missing.
   - Two code comments (`ArmVizView.qml`, `ImuCalibrationFlow.qml`) still describe the anatomical
     frame as a stub and will mislead the next reader.

   ⚠ **`ArmVizView` itself needs no work.** It already resolves the per-UNIT object for a
   HackMotion slot and reads `anatCalibrated`/`anatQuat`, so a calibrated wG3 should already drive
   the capture-page arm today. This is a text-and-wiring cleanup, not new machinery — but confirm
   that claim by running it rather than by reading it.

2. **Test the calibration state machine** — §12.3, raised by the user at the end of Phase C
   ("not convinced the states are rock solid") with no direct evidence of a fault, which is still
   the right level of confidence. That section lists what a test has to cover; all of it is
   reachable by driving `HmInstance`'s signals with no device attached.

3. **The session wizard across the sensor combinations** — §12.4's matrix. ⚠ **This one protects
   the trip.** The studio session goes through that wizard, and only the HackMotion-only Wrist
   path has ever been exercised on hardware; a combination that refuses to start would cost a day.

4. **Make the studio build reproducible.** ⚠ The build prefers a sibling `../libhackmotion`
   checkout and otherwise fetches the **`main` branch** from GitHub, so this Mac and the studio PC
   can silently build against different library revisions and a studio result would not be
   attributable. Check the About box provenance, or pin. Also confirm `hackmotion/enabled` — it
   defaults OFF.

### Phase E3 — studio verification, and the first real look at the data

**Built and run on GOLFSIMPC, with the sensor worn.** ⚠ **Keep every raw capture.** They become
Phase F's development fixture, which is what stops F needing a studio trip of its own.

**The gates it clears.** Phase E's three:
- a real shot producing a stitched variable-rate lane — ⚠ with density measured over **impact
  ±125 ms**, NOT the block-level figure, which is the wrong scope (`history.h`);
- a shot with the device disconnected, producing a window byte-identical to today's;
- two balls struck inside ~3 s, raising the eviction warning at capture time.

Plus Phase B's two, which have been outstanding since that phase shipped:
- the two lanes appearing in the **data viewer** under their aliases;
- the palm reading several g more than the lower arm through a downswing. ⚠ This is the Phase B
  failure mode that looks entirely plausible when it is wrong.

**Three risks to front-load, in this order:**

1. ⚠ **WINDOWS BLUETOOTH HAS NEVER BEEN EXERCISED.** Everything from A to D was verified on the
   Mac. There is no platform-specific code in `BleImuTransport`, which is encouraging, but Qt's
   Bluetooth stack behaves differently on Windows and this path has never run there. **Check it
   first** — if the sensor will not connect, nothing else in the trip happens.
2. ⚠ **THE FRAME CONSTANT IS A PROPERTY OF THE STRAP POSITION.** `kCandidate = 1` describes
   mounting `wg3-mount1` from the 18 Aug capture taken on the Mac, and the constant's own comment
   says so: *"If the strap position changes, it changes."* Strap it differently and every wrist
   angle is wrong **and looks entirely plausible**, which is Phase D's central lesson. Reproduce
   the mounting — photograph it beforehand — or re-run `hm_frame_select.py` in the studio.
3. **Part of Phase E cannot be verified here, and that is expected.** The binding loop still skips
   `HmInstance` (Phase F's work), so the stitched lane never becomes a binding: the fuser never
   sees it and the derived grid rate never rises. E3 proves the data ARRIVES and is RECORDED
   honestly; it does not prove anything downstream consumes the density.

#### The findings note — deliberately slim, and it answers an open question

⚠ **THIS IS NOT AN ASSESSMENT OF THE DEVICE'S ACCURACY, and framing it as one would be a category
error.** The wG3 is the CRITERION here — the best wrist measurement available to us and the thing
our own estimate is judged against (§1). We are not here to check whether it is right. What is
genuinely interesting is what it SHOWS, because nobody has been able to see a wrist at this rate
before: the live link only ever gave a handful of samples through a downswing.

1. **What does the wrist actually do through impact, at full rate?** Flexion, deviation and
   rotation across the quarter-second that matters, at ~800 Hz against the ~100 Hz we would
   otherwise have. ⚠ **This answers the question `deferred_sources_design.md` §4.3 is explicit
   that landing the mechanism does NOT answer** — what rate the wrist metrics actually need. If
   the values at the P-positions are indistinguishable between the two rates, the retrieval buys
   less than assumed and we should say so; if they differ, that is the justification for
   everything Phase E built.
2. **Does the relative angle hold still?** §6.2 measured the two units drifting individually
   (1.95° and 1.13° over five minutes) while the RELATIVE angle stayed within 0.58°. A stationary
   hold reproduces that in our setup or does not. ⚠ §12 of the specification says plainly that
   *why* it cancels so completely is not understood and should not be assumed outside the vendor's
   configuration — so this is a property to check, not one to inherit.
3. **Do the excursions look like a golf swing?** Plain plausibility on the measured flexion and
   deviation ranges. Cheap, and it is the check that catches a mounting or frame problem that
   every other number would carry silently.

### Phase F — metrics and measures

**Almost all of this already exists**, built for the launch monitor. Do not invent a parallel
mechanism.

- ⚠ **First, make both instruments bindable at once — §0 #8.** `FusedStreams::streamFor(role)`
  returns the first match, so a swing wearing both silently drops one. Add an instrument
  discriminator to `ImuSegmentBinding` / `SegmentStream` and make the lookup instrument-aware.
  Then the comparison is nearly free: `ImuVisionFuser::fuse()` already takes a *binding vector* and
  `MetricExtractor::extract()` already takes a `FusedStreams`, so partition the bindings by
  instrument and run both through the **identical wrist maths**, emitting the second under an
  `hm.` prefix. Any difference between the two series is then the instrument, not the arithmetic.
- **New metric keys, not overwritten ones** — `hm.leadWristFlexExt`, `hm.leadWristRadUln`,
  `hm.forearmPronation` beside ours. ⚠ The names in the original text were wrong; see §0 #5.
  The pair must stay separately addressable — that is what makes Phase G possible at all.
  `measure_vocabulary.h` says it directly: *"a measure still cannot validate itself."*
- **Their own descriptors with a `RouteMethod::Device` route**, the `lm.attackAngle` shape — not a
  second route on the existing metric (§0 #6). ⚠ The requirement axis does not exist yet (§0 #7).
- **`preferKeys` on the affected measures**, HackMotion first. Authored, not inferred: the
  loader walks a list somebody wrote, best first, and takes the first key the swing carries.
  ⚠ **Every key in a ladder must carry the measure's unit** — the pack validator's
  `measureKeyUnit` enforces it; a corridor authored in degrees must mean degrees whichever
  rung answered.
- **No corridor changes.** The corridors grade a quantity, not an instrument. If HackMotion
  says the corridor is wrong, that is a Phase G finding to act on deliberately — not a thing
  to tune while wiring a route.
- ⚠ ~~**Land dark.** The `preferKeys` entries stay empty until Phase G says the instrument is
  trustworthy.~~ **Overridden — see §0 decision 3.** HackMotion grades when present, following the
  launch monitor exactly. Landing the route and landing the preference remain two different
  decisions; they are simply both taken now. The consequence — a graded corpus that mixes
  instruments by what was worn — is recorded there, and does not affect the Phase G comparison,
  which happens at series level on swings carrying both.
  ⚠ **AND PHASE G'S DEFERRAL DOES NOT CHANGE THIS — the question was raised and settled on
  2026-08-18.** The temptation is to reason "G is deferred, so the instrument is unvalidated, so
  it must not grade". That inverts the relationship: the wG3 is the CRITERION, not the candidate.
  Deferring G defers what we know about **our own** wrist estimate, not what we know about the
  HackMotion. Enable the preference.

### Phase G — DEFERRED ENTIRELY, and it needs its own plan

**Decided 2026-08-18.** This plan exists to add HackMotion support. Phase G is a different job —
a corpus-scale instrument comparison with its own protocol, its own subject and mounting
bookkeeping, and its own statistics — and folding it in here would swell a plan that is nearly
finished. §8 stays as the design sketch for whoever picks it up.

⚠ **AND IT IS NOT A GATE ON ANYTHING IN THIS PLAN — the reverse of what §1's original wording
implies.** That wording ("Phase G exists to confirm that in our own captures before any HackMotion
reading is allowed to grade anything") reads the comparison backwards, and §0's decision 3 already
overrode it: **the wG3 IS the criterion for wrist measurement.** It is the best product available
to us, we accept its readings as relatively accurate, and it is what we grade AGAINST. Phase G
does not put the HackMotion on trial — **it assesses OUR Witmotion estimate against it**, which is
a question about our own sensors and can wait.

So Phase F enables `preferKeys` as §0 decision 3 says: HackMotion grades when present, our
estimate grades when it is not. ⚠ The one consequence to keep carrying is the one recorded there —
**the graded corpus then mixes instruments depending on what was worn**, which matters for
norm-building. The separately-addressable `hm.*` keys are what make that recoverable after the
fact, which is one more reason they must not be collapsed into the existing ones.

See §8 for the design as sketched.

---

## 6. What changes on screen, view by view

*(Stated explicitly, per the plan convention — a schema change is not a UI change.)*

**6.1 Settings → IMUs (`ImusPanel.qml`).** A HackMotion appears as one device row like any
other, with vendor/model from the library's device info. Its test panel differs: **two**
`ImuVizView` cubes side by side, labelled *Lower arm* and *Palm*, driven from the two units of
one instance. Battery from `HM_EV_BATTERY`. No output-rate control — the rate is adaptive and
not ours to set; show the measured rate instead. No Zero button — there is no host-side zero.

**6.2 Session wizard, IMUs step.** A HackMotion satisfies **slot A and slot B together**. The
requirement rows must show one device filling two slots rather than showing slot B unfilled —
otherwise the wizard reads as "you are missing a sensor" to someone wearing everything asked
of them. If both a HackMotion and Witmotion wrist sensors are present, the coach chooses which
pair is bound; the other is captured but unbound (Phase G's normal state).

**6.3 Session wizard, Calibrate step (`ImuCalibrationFlow.qml`).** A device-type branch. The
HackMotion path is: *"Rest your forearm horizontal, wrist straight"* → confirm → *"Now raise
your arm smoothly across your chest"* → confirm → applying → *"Return to the first position"*
→ done. Three differences a coach will notice and must be told:
- ⚠ The raise is **watched continuously**, so it must be one smooth motion, not two poses.
- The device **vibrates** when the link comes up (§9.5) — use it as the connection
  confirmation rather than a spinner.
- If both systems are worn, the flows run back to back (§4.4).

**6.4 Discovery prompt.** ⚠ **Arm the scan first, then prompt.** The sensor advertises for a
few seconds after a button press and then stops. And *"if the sensor has been asleep, the
first press only wakes it"* — the prompt must say **press, pause, press again**. This is the
first-run path; getting it wrong reads to the user as "the device doesn't work".

**6.5 Wrist session screen (`ScreenWrist`).** Nothing structural. The wrist readouts are
driven by whichever lane is bound. During Phase G, a second, muted trace for the unbound
system — the same "reference instrument beside our own estimate" presentation the launch
monitor uses.

**6.6 Toolbar / capture.** One new state a coach can hit: the post-shot gather now takes
noticeably longer with a deferred source. The ANALYSING indicator must cover *Gathering*, and
⚠ the athlete must not tee up during it — `busy()` already disarms SHOT, but the label needs
to say why.

**6.7 Eviction warning.** If a second shot is struck while a pull is in flight and its range is
at risk, say so at the time. A holed second swing discovered during review is unrecoverable.

**6.8 Data viewer.** Two new IMU lanes per HackMotion, alias-named *Lower arm* / *Palm*. The
BLE-aware gap rule applies. A stitched lane changes rate mid-window — the viewer must render
that as a real property, not as a glitch.

---

## 7. Schema and provenance

- **`ImuSample` is unchanged.** Two sources of the existing 40-byte v2 struct. No schema bump.
  ⚠ **AND THAT DECISION IS WHY THE CAPTURE PATH IS LOSSY — read Phase B′ before relying on this
  line.** Ten floats and a host timestamp is the whole of what reaches `swing.json`. The *numbers*
  survive intact (accel is raw counts × 0.001 exactly; the quaternion is i16/16384, exact in
  float32; gyro comes from the library's config-aware scaled field, not a guessed divisor) — but
  every field of `hm_sample` that says **whether to trust those numbers** stops at
  `hm_instance.cpp:1339`. `sample.h` states the cost of the worst one plainly: the calibration
  transform *"is applied ON-DEVICE and is not recoverable later, so if the recording does not carry
  this flag the mistake is permanent and invisible."* Also dropped: `pinned_mask` (int16 saturates
  rather than wraps, so a clipped peak is a plausible flat top), `device_time_us`, `sample_index`,
  `stream_id`, `source`, and the rest of the `hm_sample_flag` word.
  ⚠ **Do not "fix" this by widening the record.** A prefix-compatible `imu_sample_v2 + extras` is
  invisible to every consumer except `swing_window.cpp:100`, which compares `h.bytes` to
  `sizeof(ImuSample)` with **exact equality** and would silently drop the HackMotion lane out of
  analysis — in the interpolation pre-stage shared with Witmotion. Phase B′ carries the
  window-constant facts in `SwingImuDeviceInfo` and the rare exceptions in a bounded ring instead;
  Phase E's sidecar owns the genuinely per-sample fields.
- **`ImuSegmentBinding` gains a calibration *kind*.** Today it carries `alignA`/`mountM` plus
  `mountDeviationDeg`/`mountGravityErrorDeg`, all artefacts of a host-side solve that does not
  exist for HackMotion. Add `calibrationSource` (`HostSolved` | `DeviceApplied`) and
  `presenceAngleDeg`; leave the host-solve fields at their defaults for a HackMotion binding
  rather than filling them with something plausible. ⚠ The composite `calibrated` gate must
  branch on the kind — a HackMotion binding is calibrated when the device says so, not when
  two mount thresholds it never had are satisfied.
- **swing.json gains a per-stream provenance block** (already planned in
  `deferred_sources_design.md` §4.6): the high-rate span, `achieved_hz`, coverage intervals,
  `largest_gap_us`, the `hm_clock_snapshot`, pinned-sample counts, the stream config byte, and
  the calibration span including `spans_transition`.
- **Use the binary/csv sidecar for high-rate streams.** 2 s × 800 Hz × 2 units is ~128 KB
  binary and about a megabyte inlined as JSON, against a corpus where swing.json size is
  already a live complaint.
- **Re-analysis determinism:** the fit is persisted *with* the block, never queried afterwards.

---

## 8. Validation design — using HackMotion as a criterion

### 8.1 What is being asked

Not "is HackMotion right". The question is: **how far, and in what pattern, does our Witmotion
wrist estimate differ from an independent instrument on the same swing?** — with a specific
prior hypothesis: the difference concentrates in the FE↔RUD split, because that is where the
unobservable-heading limitation lands.

### 8.2 The design

- **Event-anchored, not clock-anchored** (F5). Both traces are placed on the phase ladder and
  compared at P-positions: Address, Top, Delivery, Impact. Absolute host time is not used, and
  therefore neither system's unmeasured timebase error contributes.
- **Corpus scale, always.** ⚠ *One labelled swing is development data.* An agreement figure
  from a handful of swings says nothing; the accuracy claim is corpus-scale, per subject, with
  the mounting recorded.
- **Report the pattern, not just the RMS.** Bland–Altman per DOF (bias and limits of
  agreement), and the FE/RUD cross-correlation specifically — a rotation of the disagreement
  into the FE↔RUD plane is the predicted signature and is far more informative than a scalar.
- **⚠ Verify the common-mode cancellation in our own captures** before trusting anything.
  Reproduce §6.2's measurement: a stationary hold, several minutes, both systems. If our
  HackMotion units drift relative to each other the way ours do, the criterion is not a
  criterion and Phase G stops there.
- **Record the mounting.** Both `R_unit` constants and the agreement figures are properties of
  a strap position. A study that does not record it cannot be repeated.

### 8.3 What the outcome licenses

| Outcome | What we do |
|---|---|
| Agreement within corridor granularity, no structured pattern | Enable `preferKeys`. HackMotion grades when present; our estimate grades when not. |
| Structured disagreement concentrated in FE↔RUD | The finding is *ours*, not the device's — it quantifies the heading limitation for the first time and becomes the specification for fixing it. Do **not** enable preference until it is understood. |
| Disagreement without structure | Neither instrument is trusted until the source is found. Report and stop. |

⚠ In no outcome do we tune our corridors to match HackMotion. Corridors are criterion-referenced
to coaching doctrine; an instrument comparison is evidence about *instruments*.

---

## 9. Risks

| Risk | Why it bites | Mitigation |
|---|---|---|
| **Frame reconciliation lands wrong and looks right** | The angle is convention-blind; every sign can be inverted with nothing failing | Phase D's DIRECTED-sign acceptance, plus `hm_frame_test.cpp`. ⚠ Never accept on a plausible angle — and never on cross-talk either, which all four candidates pass |
| **Correlated error mistaken for corroboration** | Both systems are magnetometer-free | §8.2's stationary-hold check, run first |
| **A second shot inside 3 s silently loses data** | ~7.5 s buffer, ~4.5 s pull, serialised | Surface `HM_EV_HISTORY_EVICTION_RISK` at capture time |
| **Holed pull read as a short one** | The device holes rather than clamps, with no error | Gate on measured `effectiveHz` and `largest_gap_us`, never on sample count |
| **Calibration silently inherited across a reconnect** | Costs the calibration but not the connection | The library forbids it; our UI must match rather than smooth it |
| **Session ends at 5.0 minutes** | The idle timer runs while connected and streaming does not prevent it | The library owns the 30 s keepalive unconditionally; do not add a way to disable it |
| **Two calibration routines exhaust the athlete** | Phase G needs both systems worn | §4.4; resolve R19 with the library |

---

## 10. Open questions

1. **R19 — can the two calibration choreographies be combined?** Depends on whether the
   device's pose 0 must be horizontal or merely known. Unanswered, and **ours to answer**:
   Phase D run twice settles it (§4.4). ⚠ Not by the presence angle — that measures the half
   that cannot fail.
2. ~~**Does the stitch seam hold?**~~ **ANSWERED, and it now ships as a standing measurement.**
   History *is* a strict superset of live over the same span — 234 indices across six real pulls,
   0 mismatches — and every block reports its own `live_overlap_samples` /
   `live_overlap_mismatches` rather than the conclusion. One capture, one unit, which is exactly
   why the counter ships instead of the claim. ⚠ Requires the digest ring (§0 #9).
3. **What analysis rate do the wrist metrics actually need?** `deferred_sources_design.md` §4.3
   is explicit that landing the mechanism does not answer this. Phase E delivers 800 Hz; what
   uses it is a separate question and should not be assumed.
4. **Trail wrist?** One HackMotion covers the lead wrist. `UpperBodyMetrics` already produces
   `trailWristFlexExt` from pose. Out of scope here; worth knowing the asymmetry exists.
5. **How many devices does the studio have, and do the two units' `R_unit` constants transfer
   between them?** §12 of the specification warns that every measurement in it comes from a
   single unit.

---

## 11. Session log

| Date | Session | What happened |
|---|---|---|
| 2026-08-15 | Brief written | Scope, phases and validation design. Nothing built. Friction raised with the library team as R14–R20 — ⚠ in a `design-review.md` that was never published; see the note under **Read first**. |
| 2026-08-17 | Phase A — transport, discovery, settings | Shipped `3b4980f`, verified on hardware: the wG3 is discovered, connects, streams and drives two live orientation cubes; Witmotion regression clean. §0 written — nine corrections found by reading the published library and the current tree, four of which change what gets built. Decisions taken on placement keying, the calibration routine and its reused presentation, the `preferKeys` direction, and where the frame constants get solved. ⚠ Interim: a HackMotion is pinned to slot A and the control locked, which under-describes a device that fills A **and** B — Phase C's unit-keyed placement is the fix. |
| — | Phase B — two live lanes | Shipped `9bc8aee`, `17ccca0`, `cdddefa`; ⚠ **no log row was written at the time**, so what that session found is recorded only in the commits. Two lanes record and appear in the resource monitor with counts climbing. ⚠ Two of its stated acceptance criteria are STILL UNVERIFIED and need a worn sensor: the two lanes appearing in the **data viewer** under their aliases, and the palm reading several g more than the lower arm through a downswing. The second is the Phase B failure mode that looks entirely plausible when it is wrong. |
| 2026-08-17 | Phase C — device-native calibration, guided | **Verified on hardware** — a coach calibrates end to end, and failed attempts were exercised too and read correctly. Five units, orchestrated: the calibration state machine on `HmInstance`, the unit-keyed placement resolver on `ImuManager`, the QML placement migration, the guided device-native flow, and the `hackmotion/enabled` flag. App build clean, `qml_reactivity_test` green. **Interim retired:** placement is now keyed `<deviceId>#lowerArm` / `<deviceId>#palm`, one wG3 genuinely fills A and B, and both wizard rows read as the two units of one peripheral. **Three defects found in review that no agent could have seen:** (1) neither the pin nor the migration checked whether *another* sensor held A or B, so a wG3 arriving beside two configured Witmotions double-claimed both letters and resolution fell to key sort order with the displaced sensor never mentioned — both paths now refuse and name the blocking slot; (2) the resolver funnel constructed an `AppSettings` (several hundred `QSettings` reads) three times per 30 Hz tick; (3) the startup scan bypassed the feature-flag push. **Two things the library forced that the plan had not anticipated:** `HM_WARN_PRESENCE_NOT_MEASURED` needs its own property — the phase reaches `COMPLETE` either way, so a check that never ran reads as success from any other combination of state; and `HM_WARN_CALIBRATION_INDETERMINATE` must leave the state untouched, because an angle between §8.2's two populations is evidence of neither and guessing there is exactly how a presence check becomes a quality score. ⚠ **THE GUIDE POSES WERE WRONG, AND THE SPEC'S WORDING IS NOT ENOUGH TO GET THEM RIGHT.** §8.2 says "forearm horizontal" then "raised ~30° across the chest", which was read as: pose 0 forearm pointing FORWARD, pose 1 sweeping horizontally inward by shoulder internal rotation. The actual routine, per the athlete who performs it: **pose 0 forearm ACROSS THE CHEST, palm down; pose 1 the forearm ELEVATED 30° with the elbow stationary.** Two consequences worth recording because neither is obvious from the text. (1) The travel is a **forearm** rotation, not an upper-arm one — "elbow in the same position" fixes the upper arm, since the elbow sits at the end of that bone — so `BodyVizView` had to learn to animate `leadForeArmOverrideRotation`; with the upper-arm override identical in both poses, the existing trigger meant the guide would never have animated at all. (2) The motion is in a near-FRONTAL plane, so the default face-on camera is the right *direction* and only needed to move closer; the off-axis guide camera added for the forward-pointing reading was removed. Derived using the composition `body_pose_adapter.cpp` already uses (`forearm_local = conj(upperArm_world) ⊗ forearm_world`, cpp:216) with that file's pre-baked parent chain, cross-checked against the live node chain to 1e-6, and verified in the running scene: pose 0 wrist level with the elbow to 0.0°, pose 1 lifted exactly 0.2761·sin30°, elbow unmoved, dorsal marker above palmar throughout, both handednesses mirrored. ⚠ **AND THE FIRST CORRECTION WAS STILL WRONG.** Tilting the FOREARM forward to keep it out of the torso makes it read as pointing out diagonally, not across the body. The fix — the user's diagnosis — is to flex the **UPPER ARM** forward (`hmCalUpperArmFwdDeg`, 40°) so the elbow comes out in front, letting the forearm lie straight across and still clear the torso: pose 0's forearm direction is then exactly `(-1, 0, 0)`, wrist at identical y and z to the elbow. That also makes `hmCalUpperArmQuat` handed for the first time — the flexion puts non-zero y and z in it, so the `(w,x,-y,-z)` mirror is no longer a no-op. Both a presentation constant, chosen by rendering, not from the spec. **Two further defects found only by RUNNING it.** (1) The guide animated the WRONG SEGMENT: `resetArmAnimation()` set `_leadArmFrom` but not `_leadArmTo`, and the handler that would have set it returns early while `animateLeadArm` is false — the exact state a caller is in while posing a start position — so the upper arm still held `_leadArmTo = identity`, the T-pose, and the raise swung the whole arm out while the forearm kept its relative angle. Anchored `to = from` in the reset; the protocol itself is a follow-up. (2) ⚠ **THE DEVICE REPORTS SUCCESS FOR A ROUTINE NOBODY PERFORMED**, and no amount of reading the presence angle fixes it — §8.2's no-raise attempt scored the BEST of three. The flow now measures the lower-arm unit's own angular travel between the markers from the live stream (that unit sits on the segment this routine rotates, so its travel *is* the raise) and **aborts before `a2 01`** below 15°, leaving the device untouched rather than applying a transform whose axis is undetermined; shown as evidence, never as a score. ⚠ Open, with no evidence either way: the calibration flow's state machine has **no test** — see §12.3. |
| 2026-08-18 | Phase D — frame reconciliation | **COMPLETE. C2 selected on hardware, and every assumption under it MEASURED rather than assumed.** The phase collapsed twice: ONE rotation, not two (the device zeroes the pair at its own pose), and a SELECTION AMONG FOUR rather than a continuous solve (both frames are built on a limb axis and a flexion axis, so the map is axis-aligned, and a det-+1 rotation carrying `X → ±Z`, `Z → ±X` is one of exactly four). ⚠ **AND THE ACCEPTANCE TEST THIS BRIEF SPECIFIED PROVABLY COULD NOT ACCEPT** — all four candidates score EXACTLY zero cross-talk, as does a reversed composition order; they differ only in SIGN. What selects is the sign of a motion whose direction was RECORDED when performed. **Result: C2, `Ry(+90°)`, `x→−z, y→+y, z→+x`**, mounting `wg3-mount1`, capture `phased2.hmwire`. Evidence: calibration applied (relative angle 6.82° → 2.03° across the `0x94`); axis roles measured from the JOINT RATE — flexion joint axis 19.7°/14.5° from device X, deviation 16.5°/14.4° from device Z, each ≥74.6° from the nearest other axis; limb axis at 2.0° from device Y, 99% single-axis; 2/2 bursts agreed in each DOF. ⚠ C2 inverts flexion against the device's own sense, which is the EXPECTED result — we report ISB, the vendor reports the inverse on bow/cup. **Three defects found only by running the tool on a real capture, each of which gave a confident wrong answer first.** (1) ⚠ The per-sample `calibration` flag is UNUSABLE ON A REPLAY — it is stamped from the session's own state machine, which advances only because the LIBRARY issued the pose markers, and `hm_capture.py` is a standalone recorder that writes `a2 00`/`a2 01` itself. A replaying session therefore stamps every sample UNCALIBRATED however good the capture, and it rejected one whose `0x94` was plainly on the wire. The boundary now comes from the wire, with the relative-angle collapse as evidence the transform was APPLIED and not merely emitted. (2) ⚠ Excursion must be measured FROM NEUTRAL, not from each burst's start: a directed motion is performed and returned, so a burst-relative delta makes the outward half positive and the return negative, and one bow yields both signs. (3) ⚠ Cross-talk is the WRONG falsification quantity — a human 'pure' single-axis motion contributes 15-20° of real off-axis movement, so a good capture looks alarming. The joint RATE axis is the direct test and is what the tool now reports. **A capture-protocol defect the athlete found:** `hm_capture.py` prints its OWN agenda while recording (hold still / a flick / a few swings — libhackmotion's protocol-spec reconciliation, a different job), so the two on-screen instruction sets disagree and the capture tool's is the louder one. The first capture followed it and could not select. The protocol now lives in `hm_frame_select.py --protocol` as its single source. ⚠ Also from the athlete: the rotation step starts PALM DOWN because that is the calibration pose, so the forearm is already near full pronation and the motion available is SUPINATION. The pronation channel is a RATE, so its sign is unbiased by that — an ANGLE referenced to that pose would carry the offset and would have to say so. App builds and signs; `hm_frame_test` 5 sections green, `imu_calibration_test` and `live_wrist_angles_test` green, IMU suite 7/7. **REPEATABILITY MEASURED, and it holds:** three captures on one uninterrupted mounting (`phased2/3/4.hmwire`), each with its own run of the calibration routine, all select C2 — the two richer ones on all THREE DOFs including the rotation. So the selection is a property of the mounting and not of the calibration attempt, which is what baking it in requires. ⚠ **But they also show a REPRODUCIBLE ~17-18° residual, and it must NOT be corrected away.** Real single-axis motions sit 17.0/13.9/20.9° off device X for flexion and 15.5/21.6/18.2° off device Z for deviation, pointing the same way each time (deviation means agree to 3.7-10.2°). That is a measurement of the GOLFER, not the device: both frames here are CONVENTIONAL — landmark-defined, not measured helical axes — and a real wrist's axes are oblique to them; `wrist_angles.h` already records 10-15° of the same family on our own Witmotion lane. Folding it into R would bake one golfer's anatomy into a mounting constant, which is exactly the one-golfer trap that stalled the earlier per-swing wrist work. ⚠ **And one more classifier defect the repeat captures exposed**, which had every rotation in all three captures reading as FAIL: a forearm rotation was recognised by the wrist STAYING STILL (small flexion AND small deviation AND a large rate), and a real supination fails that — the wrist is not a rigid coupling and picks up 10-20° of genuine flex/dev while the forearm turns. So every rotation was filed as flexion or deviation, where it then failed the axis test for the entirely correct reason that a rotation sits on neither X nor Z — an artefact that reads exactly like the reduction being falsified. Recognised now by the LOWER-ARM unit's own rate, which is the direct test. ⚠ Note the rotations were performed with the ELBOW BENT (travel MEASURED at 95-103° by integrating the lower-arm rate about its own limb axis, rather than left at the estimate) and that is the better test, not a compromise: a bent elbow stops the shoulder substituting for the forearm, and only the AXIS and the SIGN are used here, never the range. |
| 2026-08-18 | Phase E — deferred history | **MECHANISM BUILT AND TESTED; ⚠ THE HARDWARE GATES ARE STILL OUTSTANDING.** `deferred_sources_design.md` is promoted to AS BUILT and all four of its open items are closed — three answered, one (reserved ids) closed as NOT NEEDED, because the motivating deferred source was already a live ring producer and the stitched lane simply reuses the id it has. **The prerequisite landed first and alone, and it was worth the ordering:** `interpolateImu` went from a linear scan to a per-source binary search — **73-91× on a deferred-shaped window**, with `swing_window_parity_test` reporting `187 agree, 0 mismatch` across the RAM and disk backings. ⚠ **AND IT IS NOT A HACKMOTION OPTIMISATION: an ORDINARY capture with no deferred source anywhere — two cameras, two 100 Hz sensors, the old 200 Hz grid — measures ~36× faster too.** That function sits on the pre-stage every inertial capture runs through, so every existing swing and every corpus re-analysis benefits whether or not a wrist sensor was worn; the phase merely forced a fix that was already owed. ⚠ **A measurement-method defect worth recording:** the first version of this comparison timed the old build and the new build separately, and machine load moves these timings by 3x between runs — enough to invent or erase a speed-up. Both are now timed in ONE run on ONE window, with the reference linear scan kept in the test permanently for the purpose. Shipped: `RamPayloadSource` (lifted out of `SwingDiskSource` so the live and offline paths share ONE implementation), `CompositePayloadSource`, `EventBuffer::makeRingPayloadSource()`, a `Gathering` state between PostRoll and Processing, reserve-at-detection / collect-at-the-gather on `HmInstance`, the per-interval stitch, a data-derived grid rate, and a per-stream history provenance block in swing.json carrying coverage, density, achieved rate, largest gap, the three gap kinds, `self_recording_gap`, the live-overlap PAIR and the clock fit by value. **Decisions taken with the user:** `alignment_budget_us` = 4,167 µs (one 240 fps frame — refuse rather than misalign, and record the refusal); grid rate = peak over the swing span clamped [200, 800]; wire-byte recording explicitly OUT of scope and written up as follow-up §3a. **⚠ THREE THINGS IN THE DESIGN WERE WRONG AND ARE CORRECTED IN PLACE.** (1) §4.3's MEDIAN sample interval is the wrong statistic for a stitched lane — its median across a 4 s window is dominated by the still pre-roll, so it would have sized the grid to discard the very dense span the 4.5 s pull was performed to obtain; the ship is a PEAK over a 250 ms sliding probe. ⚠ And the 200 Hz FLOOR is load-bearing rather than cosmetic: deriving with no floor takes an ordinary 100 Hz Witmotion capture DOWN from today's 200 and moves the whole graded corpus, so the parity test now asserts the floor binds. ⚠ A uniform grid is also mandatory — `phase_segmenter.cpp:224` computes `1e6/(grid[1]-grid[0])`, so the theoretically better variable-density grid would hand it a wrong rate with nothing reporting an error. (2) §4.7's `[prefix][span][suffix]` assumes ONE contiguous retrieval; the device HOLES rather than clamps, so the merge is per DELIVERED INTERVAL — deferred inside, live outside — which fills the holes from live instead of leaving them empty. ⚠ `delivered[]` is HALF-OPEN and the test pins the exact boundary instant, not a count. (3) §3.3's reserved-id allocator was never needed. **And two defects found only by writing the gates rather than reasoning about them.** ⚠ The first perf gate was `EXPECT_LT(us, 150'000)` from an ESTIMATE that a linear scan would take hundreds of ms — it takes 14 ms, so **the check meant to catch the regression passed for the unfixed code**. Lesson 1 of Phase D, reproduced exactly, in the gate rather than in the feature; the threshold now comes from the two measured numbers. ⚠ And `after - 1` is the WRONG `prev` for the binary search: among entries sharing the largest timestamp ≤ target the old scan kept the FIRST, `upper_bound()-1` keeps the LAST — impossible on the live path where the merger forbids duplicate timestamps, expressible on the disk and stitched paths, and a silently different interpolation there. ⚠ **A third hazard the design flagged and the tree does not enforce:** `EventBuffer::resume()` does not check `swing_window_live_` at all and clears the rings unconditionally; the ONLY backstop is `CameraManager::resumeBuffer()`. So raising the guard at the PAUSE instant — which `makeRingPayloadSource()` now does — is precisely what makes that existing backstop cover the whole multi-second gather, and no change to `resume()` was needed. Both abandon paths (`abortToIdle`, `finishNowBlocking`) release it, or the buffer could never resume again. ⚠ **Also caught by construction rather than in the field:** the window bounds are frozen AT THE PAUSE, because `kWindowDuration` is a TRAILING span and resolving it against a post-gather `now` would slide the window seconds past the swing and snapshot nothing, with no error. Tests: Buffer suite 8/8 with 14 `swing_window_test` cases (6 new — bracket parity vs an independent linear reference, the cost measurement, the resume guard across the gather, composite routing, and three stitch cases including the holed pull and the half-open edge); analysis suite 96/98, ⚠ the two failures (`live_measure_source_test`, `axis_direction_test`) VERIFIED PRE-EXISTING by stashing the branch and re-running; `swing_window_parity_test` PASS. App builds and signs. **⚠ STILL OUTSTANDING, and the phase is not done without them — all three need the worn sensor:** a real shot producing a stitched variable-rate lane with density measured over impact ±125 ms (NOT the block-level figure, which is the wrong scope); a disconnected-device shot proving the window is byte-identical to today's; and two balls inside ~3 s proving the eviction warning reaches the coach. |

---

## 12. Follow-ups — after G, not during

Deferred deliberately — none of these blocks a phase from landing. Items 1 and 2 are here
because the cost of *not* doing them has already been paid twice; items 3 and 4 are gaps in
what has been **verified**, recorded at the confidence the evidence actually supports rather
than talked up or waved away.

### 1. Replace `BodyVizView`'s arm-animation protocol with one explicit call

**Raised at the end of Phase C, deferred by the user to avoid regression surface on a flow that
now works.**

The guide animation is driven by six mutable properties — `_leadArmFrom/_leadArmTo`,
`_leadForeArmFrom/_leadForeArmTo`, `_leadArmP/_leadArmT` — through an implicit call order:
`resetArmAnimation()`, then set `animateLeadArm`, then change a target property so an
`onChanged` handler captures the destination. ⚠ **Those handlers return early while
`animateLeadArm` is false**, which is exactly the state a caller is in while posing a *starting*
position, so a destination survives from whatever ran last and nothing anywhere detects it.

Two incidents, both expensive:

- The original body-pose work ("hours to get right", per the user).
- Phase C: the HackMotion raise retargets ONLY the forearm, so the upper arm still held
  `_leadArmTo = identity` — the T-pose — and the raise swung the whole arm out to the side while
  the forearm kept its relative angle. Shipped in one build; caught on hardware, not by review.
  Patched by anchoring `to = from` inside `resetArmAnimation()`, which makes "a segment nobody
  retargeted does not move" true by construction. That is a guard, not a cure.

**The cure:** one call that takes everything, so there is no reset, no ordering, no early-return
and nothing to leave behind.

```qml
startArmAnimation({ armFrom, armTo, foreFrom, foreTo, durationMs })
```

**Regression surface, which is why it is deferred:** `BodyVizView` plus both callers in
`ImuCalibrationFlow` — including the Witmotion intro chain (`introUp` → `introDown` → `raise`),
which is shipped, working, and hardware-verified. Do it with the offscreen frame-capture harness
below, on both routines, before and after.

### 2. Land the offscreen pose/animation harness in `tools/`

Currently throwaway scratch. It is a standalone QML scene that loads `src/Resources/body/*.glb`,
poses the chain, and grabs frames to PNG — run under Qt's own `qml` binary, so it needs no
CMake wiring. It is what caught the animation bug above (frame-by-frame capture of the slerp
path), and what settled the guide poses when reasoning from source had produced a confidently
wrong routine that passed its own numeric self-check.

⚠ Three traps worth keeping in the file's header comment, all of which cost silent blank renders:
Qt Quick 3D's default **`clipNear` is 10**, which clips a metre-scale body to nothing; `View3D`
needs an explicit `camera:` binding when it is inside a `Repeater`; and
`QT_QPA_PLATFORM=offscreen` forces the software backend, where Quick 3D renders nothing at all —
use a real window.

### 3. Test the HackMotion calibration state machine

**Raised by the user at the end of Phase C: "not convinced the states for the calibration flows are
rock solid — you had issues with this in the past", with no direct evidence of a fault.** That is
the right level of confidence to record, because nothing currently justifies more.

There is **no test** of it. The machine spans `HmSessionWorker` → queued signals → `HmInstance`
properties → `ImuCalibrationFlow`'s `hmStep`/`_hmEvaluate`, and every transition is driven by a
library event arriving asynchronously. Its correctness today rests on review plus one hardware run.

What a test would have to cover, all of it reachable by driving `HmInstance`'s signals directly
without a device:

- `COMPLETE` with each of `HM_CAL_CALIBRATED` / `UNKNOWN` / `UNCALIBRATED`.
- ⚠ `HM_CAL_ABORT_CALLER` arriving on a transition to **COMPLETE**, not ABORTED — the case where
  "abort reason set" must NOT read as "the routine failed".
- `presenceNotMeasured` — the phase reaches COMPLETE either way, so this is the only thing
  separating "we could not check" from "we checked and it passed".
- Presence event arriving **after** the phase event, and before it. `_hmEvaluate()` is written to
  be order-independent and idempotent; nothing proves it stays that way.
- The stale-state guard: state/phase/abortReason persist from the PREVIOUS attempt until the
  library moves them, which is why the verdict refuses to read below `hmStep` 3/4. A test that
  begins a second routine while the first attempt's COMPLETE is still latched is the one that
  would catch a regression here.
- `calibrationInvalidated()` mid-routine and post-success.
- Refusals: `HM_ERR_NO_STREAM`, and `HM_ERR_BUSY` once Phase E makes it reachable.

### 3a. ⚠ THE APP STILL RECORDS NO WIRE BYTES — a deliberate Phase E decision, not an oversight

**Raised and decided during Phase E (2026-08-18): explicitly OUT of that phase's scope, recorded
here rather than drifted past.**

`HM_BUILD_RECORD` is forced ON (`CMakeLists.txt:286`) and `hackmotion_record` is linked into the
offline targets — but **nothing calls it**, and `hm_instance.cpp` never opens a recording. Both
`record.h` and `session.h` lead with *"RECORD THE WIRE BYTES"*, and the library's whole
re-decode-later position depends on it.

⚠ **The cost has already been paid at least once.** Every app-driven calibration so far has
discarded its payload — including every `0x94`, the one artefact Phase D wanted and could not
have, because the library keeps it opaque and exposes no API to read it. A recording would have
made those poses recoverable after the fact.

What it needs when it is done: a per-session (or per-calibration) recording opened on the I/O
thread, a path under the session folder, a size/retention policy, and a decision about whether the
swing captures are recorded too or only the calibration routines. None of it is hard; all of it is
new surface, which is why it did not ride along with a phase already touching the buffer, the shot
pipeline and the exporter.

### 4. The start-session wizard path, across the IMU/HackMotion combination matrix

**Raised by the user at the end of Phase C: the wizard path has not been tried, and it is complex
given the combinations we might encounter.** Phase C rewrote six resolution sites in
`ScreenSessionWizard.qml` (`imusOk`, `imusAllConnected`, `readinessIssues`, `canConnect`,
`startConnect()`, the per-slot `CheckRow`) onto the unit-keyed resolver. Only the HackMotion-only
Wrist path has been exercised on hardware.

⚠ **A LATENT COUPLING TO PAY OFF AT THE FIRST NEW SESSION TYPE — NOT A LIVE DEFECT.**
`ImuManager::setPlacementForDevice()` pins a HackMotion to **A and B unconditionally**, with no
notion of session type, and `imuPlacement` is global rather than per-type. That encodes
"HackMotion ⇒ lead forearm + lead hand", which is true for **Wrist Motion, the only session type
implemented today** — `segmentRoleForSlot()` (`swing_analysis.h:58-66`) resolves slots for
`sessionType == 1` and returns `SegmentRole::Unknown` for every other type, which is the tree
saying the same thing.

The reason it is worth writing down is that `ScreenSessionWizard.qml:264-289` already declares a
requirement table for four types, and the letters mean different anatomy in each:

| type | A | B | C |
|---|---|---|---|
| 0 Swing Analysis | Thorax | Lumbar spine | T12 junction |
| 1 Wrist Motion *(implemented)* | Forearm | Hand | Upper arm *(optional)* |
| 2 Ground Forces | Lead thigh | Trail thigh | Lumbar spine |
| 3 AI Coach | Thorax | Lumbar spine | T12 junction |

So the day a second type is implemented, an unconditional A+B pin would have a wrist sensor
satisfying *"IMU A — Thorax"* and *"IMU B — Lumbar spine"* — and since `imusOk` /
`imusAllConnected` only ask whether the slot resolves, the readiness gate would pass. The work is
part of implementing that session type, not a fix to make now.

The design question to settle then: does a wG3's A+B assignment belong to the Wrist session only
(gate the pin and the requirement matching on session type), or should placement become
per-session-type — the more honest model, since the letters already are, but a larger change that
touches Witmotion and migrates persisted settings? Do not paper over it by hiding the rows.

**The matrix to test once that is decided.** Each of these should be checked in the wizard AND at
the calibrate step, for a right- and a left-handed athlete:

1. Witmotion only — A; A+B; A+B+C. **The regression case**: the shipped path now resolves through
   the new unit-keyed resolver.
2. HackMotion only — fills A+B, C empty (optional for Wrist).
3. HackMotion on A+B **plus** a Witmotion on C. The genuinely useful pairing — wG3 for the wrist,
   Witmotion for the upper arm — and the first case with both vendors live at once.
4. A Witmotion already holding A or B when a wG3 appears. `setPlacementForDevice()` refuses and
   names the blocking slot; check the wizard says something true rather than showing an empty row.
5. A wG3 present but never assigned, alongside Witmotions on A and B.
6. A wG3 assigned, then session-disabled by the row toggle. ⚠ Both rows must move together (one
   peripheral), and `canConnect`/`startConnect()` must skip it — `assignedDevices()` dedups by
   device so the wG3 is not connected twice, which is the bug that would otherwise fire two
   connects 2 s apart.
7. Nothing assigned at all.
8. *(Only once a second session type is implemented — see the coupling above.)*
