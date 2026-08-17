# HackMotion integration — implementation brief

**Status:** brief. Nothing built. Written 2026-08-15.
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

**The library is a separate team's work.** We consume it; we do not modify it. Friction found
while writing this brief is recorded in their `docs/design-review.md` (third pass, R14–R20) —
not worked around silently here.

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

### 3.5 ⚠ The first ~2 seconds of a stream have no host time

Until the clock fit has a baseline the library reports `HM_SAMPLE_NO_FIT` and
`host_time_us = HM_TIME_UNKNOWN` (`HM_CLOCK_SHORT_BASELINE` is < 2 s). Those samples **cannot
be written to the ring** — there is no timestamp to write.

Decision for Phase B: **drop them and count them**, surfacing the count as a warning rather
than absorbing it. Do not fall back to arrival time: arrival is one-sidedly late and mixing
two timebases inside one source is exactly the kind of thing that looks fine and corrupts a
capture. Two seconds at the start of a session, before anyone has swung, is an acceptable
price; silently splicing timebases is not.

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
| **A** | Transport + enumeration | A wG3 appears in Settings → IMUs and connects; explicit-UUID `BleImuTransport` lands with a Witmotion regression | ☐ |
| **B** | Live streaming → two EventBuffer sources | Two lanes in the data viewer at the right rates; no monotonicity violations; no-fit drops counted | ☐ |
| **C** | Device-native calibration flow | A coach can calibrate end to end; presence angle recorded, never scored; reconnect invalidates | ☐ |
| **D** | **Frame reconciliation** — solve `R_unit` | A known single-axis wrist motion moves one PPS DOF and leaves the others near zero, from HackMotion data | ☐ |
| **E** | Deferred history → SwingWindow | A shot produces a stitched ~800 Hz wrist lane; coverage and gaps in provenance | ☐ |
| **F** | Metric route + measure `preferKeys` | HackMotion wrist metrics produced and separately addressable; measures prefer them; corridors unchanged | ☐ |
| **G** | Validation against Witmotion | Corpus-scale agreement report; decision on whether HackMotion grades or only reports | ☐ |

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
- Drop-and-count the no-fit prefix (§3.5).

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
  `[live prefix] + [800 Hz span] + [live suffix]` is one ascending variable-rate trace.
  ⚠ We have asked the library to verify the superset claim rather than assume it (R17).
- **Persist the clock fit with the block.** `hm_clock_snapshot` by value into swing.json, so
  re-analysis a year later reproduces the day's alignment.

### Phase F — metrics and measures

**Almost all of this already exists**, built for the launch monitor. Do not invent a parallel
mechanism.

- **New metric keys, not overwritten ones.** `m_hmWristFlexExt`, `m_hmWristDeviation`,
  `m_hmWristRotation` beside `m_wristFlexExt` etc. The pair must stay separately addressable —
  that is what makes Phase G possible at all. `measure_vocabulary.h:122` says it directly:
  *"a measure still cannot validate itself."*
- **A new `MetricRoute` on each wrist metric** — `id: "hackmotion"`, `RouteQuality::Direct`,
  requirement = a HackMotion binding — so the catalogue explains availability in terms of
  method rather than missing hardware.
- **`preferKeys` on the affected measures**, HackMotion first. Authored, not inferred: the
  loader walks a list somebody wrote, best first, and takes the first key the swing carries.
  ⚠ **Every key in a ladder must carry the measure's unit** — the pack validator's
  `measureKeyUnit` enforces it; a corridor authored in degrees must mean degrees whichever
  rung answered.
- **No corridor changes.** The corridors grade a quantity, not an instrument. If HackMotion
  says the corridor is wrong, that is a Phase G finding to act on deliberately — not a thing
  to tune while wiring a route.
- ⚠ **Land dark.** The `preferKeys` entries stay empty until Phase G says the instrument is
  trustworthy. Landing the route and landing the preference are two different decisions.

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
2. **Does the stitch seam hold?** History should be a strict superset of live over the same
   span. Asked the library to measure it rather than assume (R17).
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
| 2026-08-15 | Brief written | Scope, phases and validation design. Nothing built. Friction raised with the library team as R14–R20 in `../libhackmotion/docs/design-review.md`. |
