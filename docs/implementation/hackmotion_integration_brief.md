# HackMotion integration — implementation brief

**Status:** Phase A shipped 2026-08-17 (`3b4980f`). Written 2026-08-15, **before the library
existed** — see §0 for the nine places that made it wrong.
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
| 4 | Phase E "implements `deferred_sources_design.md` as written" | **That design is entirely unimplemented.** Zero of its identifiers exist in `src/`: no `reserveSourceId`, no `RamPayloadSource`, no `CompositePayloadSource`, no `Gathering` state, no `BoundImu::effectiveHz`, and `interpolateImu` is still the linear scan it warns about. Phase E *builds* the mechanism, it does not consume it. |
| 5 | Keys are `m_wristFlexExt`, `m_wristDeviation`, `m_wristRotation` | **None of those exist.** Real series keys: `leadWristFlexExt`, `leadWristRadUln`, `forearmPronation`, `leadArmFlexion`. Real measure ids: `m_leadWristFlexExt_p1..p8`, `m_leadWristRadUln_p*`, `m_leadForearmRot_p*` (note the measure stem differs from the metric key). |
| 6 | "A new `MetricRoute` on each wrist metric" *and* new keys | Pick one, and the codebase pattern is unambiguous: **new keys + `preferKeys`**, the `lm.attackAngle` shape. Adding a second route to `leadWristFlexExt` would make the pair *not* separately addressable, which is the one thing Phase G cannot do without. |
| 7 | "requirement = a HackMotion binding" | ⚠ **`MetricRequirement` cannot express that.** It knows anatomical `imuRoles` plus fixed bools, so a HackMotion route is today indistinguishable from the Witmotion one. Needs a `hackMotion` bool mirroring `launchMonitor`, a matching `ShotContext::hasHackMotion`, a `missingForRequirement` clause and a `CaptureDevice` value. |
| 8 | *(unstated)* | ⚠ **`FusedStreams::streamFor(role)` returns the FIRST match.** Two bindings with the same role silently first-wins, so **wearing both instruments at once — the entire point — is not expressible today.** Fix this before any dual-worn capture. |
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
   for a HackMotion; Witmotion keeps the bare device id. *(Phase C. Phase A pins a HackMotion to
   slot A and locks the control as an interim, which under-describes it — one wG3 fills A **and**
   B — and will collide if a Witmotion already holds A.)*
2. **The device-native calibration routine is mandatory**, not optional: forearm horizontal →
   continuous raise across the chest. **But the presentation is reused** — `BodyVizView`'s guided
   avatar for the poses (it already animates between two override quaternions and signals when the
   motion finishes, which is what the device's *watched* raise needs), then `ArmVizView` for live
   free-movement confirmation.
3. **`hm.*` keys, and HackMotion grades when present** — `preferKeys: ["hm.leadWristFlexExt"]`,
   the launch-monitor pattern. ⚠ Consequence, stated once: the graded corpus then mixes
   instruments depending on what was worn, which matters for norm-building. The *comparison* is
   unaffected — both series are produced on every dual-worn swing and compared at series level.
4. **The frame constants are solved offline** from a `.hmwire` capture of single-axis motions,
   baked into `pp_tuned_constants.h` and pinned by a unit test.
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
0.58°/5 min figure is the evidence that they differ where it matters, and **Phase G exists to
confirm that in our own captures before any HackMotion reading is allowed to grade anything.**

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
| **B** | Live streaming → two EventBuffer sources | Two lanes in the data viewer at the right rates; no monotonicity violations. ⚠ *No "no-fit drops" — §0 #1* | ☐ |
| **C** | Device-native calibration flow | A coach can calibrate end to end; presence angle recorded, never scored; reconnect invalidates. Includes unit-keyed placement (§0 decision 1) | ☐ |
| **D** | **Frame reconciliation** — solve `R_unit` | A known single-axis wrist motion moves one PPS DOF and leaves the others near zero, from HackMotion data | ☐ |
| **E** | Deferred history → SwingWindow | A shot produces a stitched **variable-rate** wrist lane — ~100 Hz over the still pre-roll, full rate through the swing (§0 #2) — with coverage, gaps and the fit in provenance. ⚠ Builds `deferred_sources_design.md`, does not consume it (§0 #4) | ☐ |
| **F** | New `hm.*` keys + measure `preferKeys` | HackMotion wrist metrics produced and separately addressable; measures prefer them; corridors unchanged. ⚠ Blocked on `streamFor` (§0 #8) | ☐ |
| **G** | Validation against Witmotion | Corpus-scale agreement report; decision on whether HackMotion grades or only reports | ☐ |

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

### Phase C — calibration flow

- `ImuCalibrationFlow.qml` gains a device-type branch (see §6.3 for the UI).
- Order is fixed and enforced: connect → **start stream** → `hm_calibration_begin()` →
  confirm horizontal → confirm raise → await result → confirm reference pose → done.
- `HmInstance` exposes `anatCalibrated` from the library's per-sample calibration state, and
  a `presenceAngleDeg` that the UI shows as *state*, not as a score.
- ⚠ `0x94` is not a verdict — it arrives for rejected attempts too. Never infer success from
  its arrival.

### Phase D — frame reconciliation (the linchpin)

Solve one constant quaternion per unit, `R_lowerArm` and `R_palm`, mapping
HackMotion-anatomical → PPS-anatomical, so that everything downstream can treat a HackMotion
lane exactly as it treats a calibrated Witmotion lane.

**Method.** Both systems worn simultaneously and calibrated. Capture a set of deliberate
single-axis wrist motions — pure flexion/extension, pure radial/ulnar deviation, pure
pronation — held slowly enough that neither timebase matters. For each, the PPS lane defines
the axis; solve the rotation that carries the HackMotion lane onto it. Then compose into the
existing path so that `q_anat_hm = R_unit · q_hm`, and `wrist_angles.h` is used **unmodified**.

**Acceptance, and it is the one that matters:** a pure single-axis motion must move one PPS
DOF and leave the others near zero. ⚠ Do **not** accept on "the wrist angle looks right" — F3
says that passes with the composition order reversed and every sign wrong.

**Deliverables:** the two constants in `pp_tuned_constants.h` behind a dotted key; a unit test
in `src/Analysis/tests/` that pins the `q_rel_pps = q_arm* ⊗ q_rel_hm ⊗ q_arm` identity on
synthetic quaternions (no hardware); and a second test that asserts the *angle* check cannot
distinguish the two orders — so nobody later "simplifies" it away.

⚠ **These constants are per mounting convention, not per device.** If the strap position
changes, they change. Record the mounting in the capture that produced them.

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

### Phase G — validation

See §8.

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
| **Frame reconciliation lands wrong and looks right** | The angle is convention-blind; every sign can be inverted with nothing failing | Phase D's single-axis acceptance, plus the two unit tests. Never accept on a plausible angle |
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
