# Pinpoint Shot Detector — Developer Guide

**Audience**: Developers working on or integrating with the Pinpoint application  
**Location**: `src/IMU/impact_detector.h`, `src/Audio/onset_detector.h` + `acoustic_shot_detector.{h,cpp}`, `src/Gui/shot/shot_arbiter.h` + `shot_controller.{h,cpp}`, vision in `src/Pose/ball_detector.{h,cpp}` + `ball_temporal.h`  
**Language**: C++17 (detector math headers) / C++20 (app integration)  
**Status**: All **three** modalities are live candidate producers — IMU (P1), acoustic (P2) and vision (P3-G5, the v2 temporal ball tracker). P4 mic settings + latency calibration landed. On-hardware field tuning of the thresholds is ongoing.

---

## Contents

1. [What Shot Detection Is](#1-what-shot-detection-is)
2. [Where It Fits in Pinpoint](#2-where-it-fits-in-pinpoint)
3. [Core Concepts](#3-core-concepts)
4. [The Timestamp Model — Latency-Aware Back-Dating](#4-the-timestamp-model--latency-aware-back-dating)
5. [The IMU Impact Detector](#5-the-imu-impact-detector)
6. [The Acoustic Onset Detector](#6-the-acoustic-onset-detector)
7. [The Vision Launch Detector](#7-the-vision-launch-detector)
8. [The Arbiter — Candidate → Hold → Fuse → Commit](#8-the-arbiter--candidate--hold--fuse--commit)
9. [ShotController Integration](#9-shotcontroller-integration)
10. [Getting Started — Adding a New Detector Modality](#10-getting-started--adding-a-new-detector-modality)
11. [Configuration and Settings](#11-configuration-and-settings)
12. [Internals — Design Decisions Explained](#12-internals--design-decisions-explained)
13. [Testing](#13-testing)
14. [Common Mistakes](#14-common-mistakes)
15. [File Map](#15-file-map)

---

## 1. What Shot Detection Is

Shot detection answers one question, hands-free: **"a golf ball was just struck —
at exactly what instant?"** The answer drives the entire post-shot pipeline: the
shot marker written into the EventBuffer, the frozen `SwingWindow`, analysis,
export, and the on-screen replay all align to the committed impact timestamp.

It is a **multi-modal** problem by design (`docs/design/shotdetection.md`): no single
sensor is reliable enough alone. The implementation fuses three independent
detectors behind one funnel:

| Modality | Detector | Strength | Weakness |
|---|---|---|---|
| **Acoustic** | `OnsetDetector` (impact "click") | Sample-accurate pinpoint; small, stable capture latency | Fires on any sharp sound near the mic |
| **IMU** | `ImpactDetector` (shaft shock + swing energy) | Strong evidence a *swing* happened | ±5 ms sampling at 200 Hz; variable BLE latency; ±16 g clipping |
| **Vision** | `BallDetector` launch cliff (`ball_temporal.h`) | Direct evidence the ball *left the tee* — needs no user calibration | Frame-rate coarse; age-from-collapse estimate; fixed confidence 0.6, so it corroborates and never commits alone |

Each detector emits a *candidate* — an estimated true-impact instant plus a
confidence. The **arbiter** collects candidates in a short hold window and
commits at most one shot: when two modalities agree, or when one is decisively
strong. The manual SHOT button bypasses all of this and commits directly.

Shot detection is **not** swing analysis. It decides *that* and *when* a shot
happened; everything downstream of `shotDetected` (window capture, metrics,
scoring) belongs to the shot analyzer — see
`docs/developer/shot_analyzer_developer_guide.md`.

---

## 2. Where It Fits in Pinpoint

```
┌────────────────────────────────────────────────────────────────────────────┐
│  Pinpoint Application                                                      │
│                                                                            │
│  [WT9011DCL_BLE] ─quaternionUpdated─► [ImuInstance]                        │
│                                        ImpactDetector (math)               │
│                                        │ impactDetected(est_t, conf)       │
│                                        ▼                                   │
│                                    [ImuManager] ──────────┐                │
│                                                           │ (gated on      │
│  [AudioInput] ──audioDataReady──► [AcousticShotDetector]  │  autoDetect-   │
│   (audio thread)                   OnsetDetector (math)   │  Swing)        │
│                                    │ impactDetected       │                │
│                                    ▼                      ▼                │
│                         [TranscriptionController] ─► [ShotController]      │
│                                                       reportCandidate()    │
│  [BallDetector] ──ballLaunched──► [CameraInstance] ──►     ▲               │
│   (detector thread)                stamps absolute t       │               │
│                                    [CameraManager] ────────┘               │
│                                                       ┌──────────────┐     │
│  [SHOT button] ────triggerShot()──────────────────────►  ShotArbiter │     │
│   (manual — bypasses the hold)                        │  hold/fuse   │     │
│                                                       └──────┬───────┘     │
│                                                              │ commitShot  │
│                                              writeShotMarker │             │
│                                       ┌──────────────┐      ▼             │
│                                       │ EventBuffer  │  shotDetected ───►  │
│                                       │ shot_marker  │  [ShotProcessor]    │
│                                       └──────────────┘  post-roll → freeze │
│                                                          → analyse ∥ export│
└────────────────────────────────────────────────────────────────────────────┘
```

### The detection workflow

1. **Capture running**: the user presses Capture; the EventBuffer is `Capturing`
   and `ShotController::armed` is true.
2. **Swing happens**: the IMU on the club shaft sees a gyro-energy ramp through
   the downswing, then an accelerometer shock at impact; the microphone hears
   the impact click a few milliseconds of latency later; the ball's matched-filter
   response in the hitting-area ROI collapses off a cliff as it departs.
3. **Candidates**: each detector independently back-dates its arrival stamp by
   its own latency and calls `ShotController::reportCandidate(source, est_t, conf)`.
4. **Hold**: the first candidate opens a 200 ms arbiter window. Other modalities'
   candidates for the same strike land inside it.
5. **Commit**: at the deadline the arbiter fuses: two agreeing modalities (or one
   strong one) → `commitShot()` writes the `shot_marker_v1` entry with the
   **authoritative** timestamp (acoustic when present) and emits `shotDetected`.
6. **Pipeline**: `ShotProcessor` runs post-roll → pause → `captureSwingWindow(4 s)`
   → analysis ∥ export → replay. Its `busy` state disarms the trigger for the
   whole pipeline, and the arbiter's own refractory absorbs echoes around the
   edges.

---

## 3. Core Concepts

### Modality

An independent physical evidence channel: IMU shock, acoustic onset, vision
launch. The arbiter only counts *distinct* modalities as corroboration — two
acoustic onsets are one voice, not two.

### Candidate

`{source, est_t_us, confidence}` — a detector's claim that an impact occurred at
`est_t_us` (already back-dated, see §4) with gate strength `confidence` in
[0, 1]. Candidates are cheap; commits are expensive. Detectors should report
freely within their own gates and let the arbiter decide.

### The armed gate

`ShotController::armed()` is true only while the buffer is Capturing, the
`ShotProcessor` is idle, and session review is not active. Every path —
`triggerShot()`, `reportCandidate()`, and the final `commitShot()` — checks it,
so all sources inherit the gate and a candidate that arrives mid-pipeline is
dropped, not queued.

### The hold window

The arbiter is a tiny state machine: **Idle → Collecting → Decide**. The first
candidate opens the window (`holdMs` = 200 ms, driven by a single-shot `QTimer`
in `ShotController`); the decision happens once, at the deadline, never earlier.
Latency cost: a real shot commits ~200 ms after the first detector fires —
invisible in practice, because the `ShotProcessor` post-roll waits 500 ms anyway.

### Authoritative timestamp

When modalities agree, their `est_t` values differ by a few ms. The committed
timestamp comes from the highest-authority agreeing modality — **Acoustic >
Imu > Ball** — because audio pinpoints (sample-accurate onset, stable latency)
while IMU/vision confirm (sampling-rate and transport-latency coarse). The
`ArbSource` enum order *is* the priority order.

### Refractory

Three layers prevent double-fires: each detector has its own refractory
(IMU 200 ms, acoustic 35 ms — tuned to its physics), the arbiter has a
1.5 s post-commit refractory, and `ShotProcessor::busy` disarms everything for
the pipeline's duration. They overlap deliberately: the busy gate has edges
(post-roll start, replay end) the refractory covers.

### The shot marker

Every commit writes a 16-byte `ShotMarker` into the EventBuffer (source
`"shot_controller"`, schema `"shot_marker_v1"`) **before** emitting
`shotDetected`, with the entry's `timestamp_us` set to the impact instant
itself. The frozen `SwingWindow` then locates impact via `entriesFor(markerId)`
without payload parsing. Back-dated timestamps are safe here: per-source
monotonicity cannot be violated at shot cadence.

---

## 4. The Timestamp Model — Latency-Aware Back-Dating

This is the linchpin of the whole design. **Every EventBuffer source is stamped
at host-arrival time on one `steady_clock` (`EventBuffer::nowMicros()`), with no
latency compensation in the buffer itself.** A camera frame, an IMU packet and
an audio buffer that describe the same physical instant arrive at different
times — each delayed by its own capture chain.

Detectors therefore emit an *estimated true-impact instant*:

```
est_t_us = arrival_stamp_us − per_source_latency_us
```

| Source | Arrival stamp | Latency constant | Where it lives |
|---|---|---|---|
| IMU | `nowMicros()` at the top of the GUI-thread packet handler | `ImuInstance::kImuBleLatencyUs` (30 ms) | `src/Gui/imu/imu_instance.h` |
| Acoustic | `nowMicros()` at buffer **receipt** on the audio thread, minus `samplesAfterOnset/rate` back to the onset sample | `AppSettings::audioDeviceLatencyUs` (20 ms default, persisted) | `src/Gui/app/app_settings.h` |
| Vision | the **triggering frame's own capture time** on the buffer clock (`BallDetection::tUs`, the same value its ring entry got), minus the collapse age | `kBallLaunchLatencyUs` (24 ms ball-departure latency), plus `framesSinceCollapse × frameInterval` | `src/Pose/ball_detector.cpp` |

The acoustic path is the precision channel: the onset sample index is exact
(the truth-table test demonstrates 0-sample error), so its only error is the
device-latency constant. The IMU's BLE connection-interval jitter makes its
constant softer — which is exactly why the arbiter's match tolerance is ±40 ms
and the acoustic timestamp wins when both agree.

The vision path back-dates in two steps, and the reason is worth internalising.
The tracker only *knows* a launch happened some frames after the collapse cliff,
so the detector reports an **age** (`launchAgeUs`) rather than an instant, paired
with the frame time it noticed (`launchFrameTUs`). `CameraInstance` forms
`estImpactUs = launchFrameTUs − launchAgeUs`. Using the frame's own capture stamp
rather than "now" matters: the detector runs on its own thread behind a throttle,
so "now" at emit time carries queueing delay that has nothing to do with the ball.

The latency constants are deliberately *passed into* the detector configs, never
hard-coded in the math, so replacing the fixed estimates with cross-correlation
auto-calibration is a plumbing change only.

**Rules:**
- Stamp arrival time **first**, before any processing, on the thread the data
  arrives on.
- Compute `est_t` **before** any thread hop — a queued connection adds
  scheduling jitter that must not contaminate the estimate.
- Use only `EventBuffer::nowMicros()`. The committed timestamp must live on the
  same timeline as the ring entries it will be matched against.

---

## 5. The IMU Impact Detector

**Math**: `src/IMU/impact_detector.h` — `pinpoint::ImpactDetector`, header-only,
no Qt, no allocation in `push()`. **Live hook**: `ImuInstance` (GUI thread).

### The gates

A sample stream of `{accelMag (g), gyroMag (°/s), clubVertical, t_us}` is pushed
per IMU packet. An impact fires only when **all** gates pass:

| Gate | Default | Rejects |
|---|---|---|
| **Local max** — strict rise in, level-or-fall out (`>=` lets a clipped ±16 g plateau fire on its first sample) | — | mid-rise samples |
| **Adaptive threshold** — peak ≥ max(`accelFloorG` 4 g, `accelAdaptive` 3 × slow EMA of \|a\|) | 4 g floor | normal swing accelerations |
| **Jerk gate** — rise rate into the peak ≥ `jerkMinGps` (100 g/s) | 100 g/s | slow swells that crest above threshold without a strike's sharp edge |
| **Swing-energy gate** — in the `gyroWindowMs` (400 ms) window ending at the peak: max \|gyro\| ≥ `gyroPeakMinDps` (300 °/s) AND mean ≥ `gyroMeanMinDps` (80 °/s); requires ≥ half a window of history (no firing right after connect) | 300/80 °/s | mat/ground taps and address knocks — an accel spike with a flat gyro is not a swing |
| **Club-orientation gate** — `clubVertical` ≥ `clubVerticalMin` (0.35); the weakest-evidenced gate, permissive and disableable (`orientationGate=false`) while tuning | 0.35 | strikes claimed at implausible shaft attitudes |
| **Refractory** — `refractoryMs` (200 ms) since the last accepted impact | 200 ms | follow-through double-hits |

All windows are **milliseconds evaluated against sample timestamps**, never
sample counts — switching the sensor between 100 Hz and 200 Hz does not change
behaviour (test G proves it).

**Confidence** derives from swing energy (gyro peak over its threshold: 0.5 at
the gate, 1.0 at 2×), **never from peak g** — the WT9011's ±16 g full scale
clips real strikes, so amplitude is a binary "over threshold", not a magnitude.

### The live hook

The detector is driven from the existing GUI-thread `quaternionUpdated` handler
in `imu_instance.cpp` — the last signal per packet, so the raw accel/gyro caches
and the fused quaternion are all current:

- `gyroMag` comes from `m_imu->gyroData()` (raw, synchronous) — **not**
  `angularVelocityDps`, which is quaternion-derived, queued, and coarse.
- `clubVertical = |z|` of the fused quaternion rotating the sensor long axis
  (+Y, see `docs/design/imu_frame_contract.md`) into the +Z-up world frame.
- The hook adds **no new EventBuffer producer** — the producer/stop-barrier
  contract (CLAUDE.md) is untouched. The whole BLE chain runs on the GUI
  thread, so `impactDetected` → `ImuManager` → `main.cpp` → `ShotController`
  is a plain same-thread call chain.

### Rate

`ImuManager::createInstance()` defaults new devices to **200 Hz** (`Hz_200`)
for sharper detection (±10 ms timing at 100 Hz risks attenuating the sub-ms
shock between samples). Persisted per-device rates still win. 200 Hz BLE
throughput stability is an open hardware-verification item: watch `dataRateHz`
≈ 200 and a flat `gimbalDropCount`.

---

## 6. The Acoustic Onset Detector

**Math**: `src/Audio/onset_detector.h` — `pinpoint::OnsetDetector`, header-only,
no Qt. **Wrapper**: `AcousticShotDetector` (thin QObject), owned by
`TranscriptionController`, living on its **audio thread**.

### The per-sample pipeline (time-domain only)

```
mono float ─► one-pole high-pass (1 kHz)   impact energy is high-frequency;
        │                                  rejects rumble + speech fundamentals
        ▼
   instant-attack / exponential-release    "how loud right now", collapses
   envelope (10 ms release)                within ms after a click
        ▼
   adaptive threshold                       8 × noise-floor EMA (τ 500 ms,
   crossed FROM BELOW (attack-only)         frozen while tracking a candidate)
        ▼
   absolute amplitude gate                  hard floor on the candidate
   (minLevelAbs, from mic sensitivity)      threshold — see below
        ▼
   exponential-decay gate                   at +45 ms the envelope must have
   (decayRatioMax 0.5)                      dropped below half its peak —
        ▼                                   speech and tones fail this
   refractory 35 ms                         min inter-onset spacing
```

### The absolute amplitude gate (`minLevelAbs`)

The relative threshold alone has a failure mode that reads as a *missed impact*
rather than a false positive. In a quiet room the noise-floor EMA collapses toward
zero, so `8 × floor` does too, and any faint tick — a keyboard press, a chair —
opens a candidate. That spurious candidate's tracking and refractory windows then
**swallow the real impact** that lands inside them. So the detector keeps a hard
floor on candidate-opening that is independent of the noise floor: below
`minLevelAbs` nothing opens at all, and the impact gets its own clean candidate.

`0` disables it (pure relative behaviour). Live, it is driven from the mic
sensitivity setting on a log scale — `s=0 → 0.30` (loud events only), `s=1 → 0.01`
(very sensitive), `s=0.5 → ~0.055`. The calibration meter draws this level so the
user can sit it between their ambient noise and their club impacts.

Confirmation is delayed by `decayWindowMs` (**45 ms**), but the reported onset is
the **first threshold-crossing sample** — that is what makes audio the
sample-accurate pinpoint modality. A second hit inside the decay window folds
into the same candidate (peak update) rather than double-firing.

The window was widened from 30 ms to 45 ms because a reverberant indoor bay rings
longer than an anechoic click and was failing its own decay gate. 45 ms is the
longest horizon that still rejects the truth-table speech bursts — do not raise it
further without re-running `acoustic_shot_detector_test`.

The **attack-only crossing** rule (`m_wasBelow`) is load-bearing: without it,
the abrupt *end* of a sustained tone re-candidates while the envelope is still
high, and then passes the decay gate — because the envelope genuinely does
collapse at a cutoff. Only a rise from below the threshold can open a
candidate. See test B ("tone CUTOFF rejected").

### The wrapper and threading

`AcousticShotDetector` is the **second consumer** of
`AudioInputBase::audioDataReady` — the `AudioStreamSaver` pattern. It runs at
the device's **native rate** (44.1/48 kHz; never STT's 16 kHz downmix, which
would shave off the click's high-frequency content), in parallel with STT, and
adding it never disturbs the STT pipeline (the signal is a fan-out; STT's
silence gating does not throttle the raw stream).

`onAudioData()`:
1. Stamps `recvNow = nowMicros()` **first** (it runs on the audio thread via a
   same-thread direct connection — this stamp is the precision anchor).
2. On format change, `reset(sampleRate)` re-derives the per-sample coefficients.
3. Converts channel 0 of each frame to float (Int16/Int32/UInt8/Float
   supported) and pushes through the detector.
4. After the whole buffer: for each confirmed onset,
   `est_t = estimateImpactUs(recvNow, samplesFromOnsetToBufferEnd, rate, latency)`
   and `emit impactDetected(est_t, conf)`.

The emit happens **on the audio thread** with `est_t` already computed.
`TranscriptionController` forwards it signal-to-signal (still the audio
thread — documented on the signal); the main.cpp connection's `&shotController`
context provides the queued hop onto the GUI thread. The latency constant is a
`std::atomic` so the GUI thread can update it from `AppSettings` while the
audio thread reads it.

### When the microphone actually runs

The acoustic detector is only useful while the mic is open, and the mic used to
open only on the Audio page and in the calibration view — so acoustic detection
silently never applied during real sessions. `main.cpp` now drives
`TranscriptionController::setShotDetectionActive()` from

```
cameraManager.captureIntent()
  && appSettings.acousticShotDetectionEnabled()
  && appSettings.autoDetectSwing()
```

`captureIntent` is *session*-stable — it does not toggle per shot — so the mic
stays open across a whole capturing session and closes whenever acoustic cannot
contribute a candidate anyway.

Note the split at the arbiter boundary: the raw detector fires **always**, so the
mic-calibration meter keeps seeing onsets; only the `reportCandidate()` call is
gated on `autoDetectSwing && acousticShotDetectionEnabled`. Voice/STT is
independent and unaffected by the acoustic toggle.

---

## 7. The Vision Launch Detector

**Math**: `src/Pose/ball_temporal.h` — `TemporalBallTracker`, header-only, OpenCV
only, no Qt. **Live hook**: `BallDetector` (`src/Pose/ball_detector.{h,cpp}`), on
its own detector thread behind a `FrameThrottle`. Design:
`docs/design/ball_detection_v2.md`.

This is the **v2 self-calibrating** detector. It replaced an earlier approach that
required a per-camera user calibration profile (`BallCalProfile`, a calibration
wizard, on-disk profiles per cameraKey); none of that exists any more. If you find
references to `ball_model.h`, `ball_calibration_store.h` or
`BallCalibrationController`, they are stale — the detector learns what it needs
from the scene.

### How a launch is detected

1. **Seed the empty-mat baseline.** With the mat empty, ~1 s of frames are
   accumulated into a mean DoG response `B` over the ROI. `baselineReady` fires
   with a deep-copied `BallBaselineSnapshot` (`B`, noise level, radius estimate,
   fps, ROI).
2. **Lock the ball.** A placed ball is a novel matched-filter response against
   `B`. On acquisition, `ballLocked(x, y, radiusNorm)` fires once.
3. **Watch the spot.** Per frame, presence is the at-spot response over its
   locked baseline `L0`; present when `≥ kPresenceFrac` (0.40) of it.
4. **The cliff.** A struck ball's at-spot response collapses off a cliff. The
   tracker records the collapse frame index; `ballLaunched(launchFrameTUs,
   launchAgeUs, x, y)` fires with the age computed back to it (§4), and the
   tracker re-arms for the next ball.

Two self-healing behaviours matter when reading the code:

- **Re-arm on absence** (`kReacquireSeconds`, 0.30 s): a ball removed or occluded
  for that long releases the lock, so re-adding it re-locks. A brief occlusion
  recovers before the timer.
- **Auto re-seed** (`kReseedEmptySeconds`, 1.5 s of a quiet ROI): the one-shot
  seed bakes in whatever sits under the box at seed time. A ball already on the
  mat — a saved ROI restored on connect, the box nudged with the ball down — gets
  subtracted into `B` and is then *never novel*, i.e. never detected. Relearning
  whenever the ROI has been ball-free for a while heals that.

### Stamping and the relaunch guard

`CameraInstance` owns the conversion to an absolute impact time and the guard
against double-firing:

```cpp
const qint64 baseTUs     = launchFrameTUs >= 0 ? launchFrameTUs : nowMicros();
const qint64 estImpactUs = baseTUs - launchAgeUs;
```

A struck ball can **bounce back through the ROI**, re-lock, and fire a second
launch inside the post-roll — observed 0.53 s after impact, which overwrote the
real launch before the window-freeze snapshot. Two genuine shots are never less
than 2 s apart (1.5 s arbiter refractory plus the processing pipeline), so a
launch within 2 s of the previous one is treated as the same strike and the
**first one wins**.

The emitted confidence is a fixed **0.6** — deliberately below the arbiter's 0.8
lone-candidate floor. Vision can corroborate IMU or acoustic; it can never commit
a shot by itself. `CameraInstance` also stashes the launch *position* the detector
already computed (`ballLaunchInfo()`), which feeds the offline ball-anchor pass;
the `ballLaunched` signal itself carries only the timestamp and confidence.

The signal path is `BallDetector` → `CameraInstance` (queued; the detector runs on
its own thread) → `CameraManager::ballLaunched` → the `main.cpp` lambda →
`reportCandidate(Source::Ball, …)`, gated on `autoDetectSwing`.

> `baselineReady`, `ballLocked`, `ballLaunched` and `exposureWarning` are
> **outside** the `FrameThrottle` contract, which counts only
> `ballDetected` / `detectionSkipped` — exactly one of those two per `detect()`
> call, always. Adding a temporal signal never releases the throttle.

---

## 8. The Arbiter — Candidate → Hold → Fuse → Commit

**Math**: `src/Gui/shot/shot_arbiter.h` — `pinpoint::ShotArbiter`, header-only, no
Qt, time injected (`nowUs` parameters) so it is fully unit-testable.
`ShotController` owns one and supplies the `QTimer`.

```cpp
// First report opens the window (returns true → owner starts its timer):
bool opened = arbiter.report({ArbSource::Imu, est_t, conf}, nowUs);

// At the deadline (and only then):
ShotArbiter::Decision d = arbiter.decide(nowUs);
if (d.commit)  commitShot(fromArbSource(d.src), d.t_us);
```

### The decision policy (`decide()`)

1. Reduce to the **highest-confidence candidate per modality** (at most 3).
2. **Agreement pass**, in authority order (Acoustic, Imu, Ball): for each
   present modality, count the other modalities whose best candidate lies
   within `matchTolMs` (±40 ms) of it. The first with ≥2 agreeing wins —
   commit with **its** timestamp, `conf` = max over the agreeing set.
3. **Lone-strong pass**: no agreement → the highest-authority candidate with
   `conf ≥ strongConf` (0.8) commits alone. Its detector's own gates already
   passed decisively; this is the "1 strong + gated" rule.
4. Otherwise: no commit. The window simply evaporates.

Either way the candidate set is cleared (`decide()` always returns to Idle) and
a commit arms the **refractory** (`refractoryMs` 1.5 s): candidates reported
inside it are dropped at `report()` time — they never even open a window.

### Interaction with the rest of ShotController

- **Manual bypass**: `triggerShot()` (the SHOT button) stops the hold timer,
  cancels any pending window, calls `noteCommit(now)` so the refractory still
  covers auto candidates around the manual shot, then commits directly.
- **Disarm voids the window**: `reevaluateArmed()` on a transition to disarmed
  (capture stopped, processor busy, review entered) stops the timer and
  `cancel()`s the arbiter — a window opened while armed must not commit after
  the world changed.
- **Re-check at commit**: `commitShot()` re-checks `armed()`; the processor may
  have gone busy mid-hold.

Everything runs on the GUI thread — no locks anywhere in the chain.

---

## 9. ShotController Integration

`ShotController` (QML context property `shotController`) is the single funnel.
Its public surface, after P3:

```cpp
// Direct commit — manual path. Bypasses the hold; timestampUs −1 = "now".
Q_INVOKABLE void triggerShot(Source source = Source::Manual, qint64 timestampUs = -1);

// Auto-detector funnel — candidates flow through the arbiter.
void reportCandidate(Source source, qint64 estImpactUs, float confidence);

// The one output everything downstream hangs off:
signals: void shotDetected(Source source, qint64 timestampUs, int sessionType);
```

`Source` is `{Manual, Imu, Pose, Ball, Acoustic}`. The session type is captured
at the moment of commit (`SessionController::activeSessionType()`), carried in
both the signal and the marker payload, and consumed by the analyzer factory.

The main.cpp wiring is one lambda per modality — three of them now — each gated on
`autoDetectSwing`:

```cpp
QObject::connect(&imuManager, &ImuManager::impactDetected, &shotController,
                 [&](qint64 estImpactUs, float conf) {
    if (appSettings.autoDetectSwing())
        shotController.reportCandidate(ShotController::Source::Imu, estImpactUs, conf);
});
```

The acoustic lambda adds `&& appSettings.acousticShotDetectionEnabled()`; the ball
lambda (`CameraManager::ballLaunched`) is gated on `autoDetectSwing` alone.

`ShotProcessor` applies a per-source **post-roll** before freezing the buffer
(`postRollMsFor(Source)`) so the follow-through lands in the ring. It is no longer
uniform — a new source must be added to that switch:

| Source | Post-roll |
|---|---|
| `Manual` | 500 ms |
| `Imu` / `Pose` / `Ball` / `Acoustic` | 1250 ms |

The manual button is pressed *after* the swing is over, so it needs only enough
room to catch the tail; an auto-detected shot commits at impact and must wait out
the whole follow-through.

The window itself is `captureSwingWindow(4 s)` (trimmed from 5 s, 2026-07-19). The
window is anchored at the pause instant, so post-impact room (~1.5 s: hold +
back-date + post-roll) is unaffected and the trim only reduces pre-impact reach.
The floor is set by the analyzer's onset clamp — it never reads further back than
impact − 1.75 s — so 4 s leaves ~0.7 s of margin while cutting a fifth off the
frozen window (~190 MB of raw frame copy per camera at export, and 20 % of the
x264 encode).

---

## 10. Getting Started — Adding a New Detector Modality

All three designed modalities are now wired, so this section is the recipe for a
*fourth*. The vision path (§7) is the most recent worked example — read it
alongside this.

```cpp
// 1. Detector math: a pure header under the owning subsystem, tested
//    standalone with a truth table (see §13). Emit an estimated true-impact
//    instant, and a confidence that is NOT saturated — the arbiter's
//    lone-strong threshold is 0.8, so reserve >0.8 for genuinely decisive
//    evidence. Vision deliberately reports a flat 0.6 so it can only
//    corroborate.

// 2. Surface a signal from the owning controller (the ImuInstance /
//    AcousticShotDetector / BallDetector precedent): compute est_t on the
//    thread the data arrives on, BEFORE any queued hop. If your detector can
//    only report "N frames/samples ago", emit the age alongside the stamp of
//    the observation and let the owner subtract — do not substitute "now".

// 3. Wire it in main.cpp behind the autoDetectSwing gate (plus any
//    modality-specific enable, as acoustic does):
QObject::connect(&fooManager, &FooManager::somethingDetected, &shotController,
                 [&](qint64 estImpactUs, float conf) {
    if (appSettings.autoDetectSwing())
        shotController.reportCandidate(ShotController::Source::Foo, estImpactUs, conf);
});

// 4. Teach the plumbing about it:
//      - an ArbSource enum entry — ENUM ORDER IS AUTHORITY ORDER (shot_arbiter.h)
//      - a Source enum entry + toArbSource/fromArbSource cases (shot_controller.cpp)
//      - a postRollMsFor case (shot_processor.cpp)
//      - arbiter_test cases pinning its authority position

// 5. Add truth-table tests for the detector math (see §13) and field-verify
//    before shipping it with any weight.
```

What you do **not** do: pause/resume the buffer, write EventBuffer entries,
debounce, or check `armed` in the detector — `ShotController` owns all of that.
A detector's entire job is `(est_t, conf)` candidates within its own gates.

---

## 11. Configuration and Settings

### User-facing (`AppSettings`, Settings → General)

| Setting | Key | Default | Effect |
|---|---|---|---|
| Auto-detect swing | `General/autoDetectSwing` | **ON** (since P3) | Master gate on all three auto wirings in main.cpp. OFF = manual SHOT only, and the mic stays closed. |
| Swing detection sensitivity | `General/swingDetectionSensitivity` | `"Medium"` | Low/Medium/High → IMU `thresholdScale` 1.5/1.0/0.7 (`ImuManager::impactScaleFor`, live-updated). >1 = less sensitive. |
| Audio device latency | `General/audioDeviceLatencyUs` | 20000 | Acoustic back-dating constant; forwarded to the detector atomically, live-updated. |
| Acoustic shot detection | `General/acousticShotDetectionEnabled` | **ON** | Per-modality enable for acoustic. Gates the arbiter feed *and* whether the mic opens during a session. Independent of voice/STT. |
| Acoustic sensitivity | `General/acousticSensitivity` | 0.5 | [0,1] → `OnsetDetectorConfig::minLevelAbs` on a log scale (`0.01 × 30^(1−s)`): 0 → 0.30 (loud only), 1 → 0.01, 0.5 → ~0.055. The calibration meter draws this level. |
| Microphone | `General/audioInputDevice` | "" (system default) | Pushed at startup before the first capture, and kept live. |
| Hitting-area ROI | `camera/ballRoi` | unset | Per-cameraKey `{x, y, w, h}` map (`AppSettings::cameraBallRoi`); set via `CameraManager::setBallRoi()`. Defines where the vision detector looks. Changing it re-seeds the empty-mat baseline. |

The `autoDetectSwing` default flipped OFF→ON at P3: a single-modality auto-trigger
was judged too false-positive-prone to default on, but with cross-modal
confirmation (or a decisively-gated lone detector) it is acceptable.

### Detector constants (code)

`ImpactDetectorConfig`, `OnsetDetectorConfig` and `ArbiterConfig` are plain
structs with inline defaults — see the §5/§6/§8 tables; the vision constants
(`kPresenceFrac`, `kReacquireSeconds`, `kReseedEmptySeconds`,
`kBallLaunchLatencyUs`) are file-scope `constexpr` in `ball_detector.cpp`. They are
constructed with defaults in their owners; there is deliberately no settings
plumbing for the inner thresholds (they need field data before they deserve UI).
The externally-fed values are exactly: the two latency constants, the IMU
sensitivity scale, and the acoustic absolute-level gate.

---

## 12. Internals — Design Decisions Explained

### Why the detectors are header-only pure math

Every gate decision is testable without BLE hardware, an audio device, Qt
signals, or the app build. The standalone suites (§13) run the full truth
tables in milliseconds. The live hooks (`ImuInstance`, `AcousticShotDetector`,
`BallDetector`) contain *no* detection logic — only sampling, frame conversion,
and timestamp plumbing. Keep it that way: a tuning change must be provable in a
test before it touches a device.

### Why the IMU decision is delayed by exactly one sample

A local maximum needs one sample of lookahead (`peak > prev && peak >= cur`).
At 200 Hz that is 5 ms — irrelevant against the 200 ms arbiter hold. The
alternative (fire on threshold crossing) triggers on the rising edge of slow
swells; the jerk gate plus local-max together encode "a strike is a sharp edge,
not a crest".

### Why the IMU noise floor is an EMA, and the acoustic floor freezes

The IMU adaptive threshold tracks a slow EMA (τ 1 s) of |a| — a one-sample 16 g
spike at 200 Hz moves it by ~0.075 g, negligible, so no freeze is needed. The
acoustic floor has the opposite problem: a 30 ms impact burst is *thousands* of
loud samples at 48 kHz, enough to inflate a 500 ms-τ EMA and raise the
detector's own threshold mid-candidate — so the floor is frozen while tracking.

### Why the arbiter takes `nowUs` as a parameter

Time injection makes `decide()`/`report()`/refractory behaviour deterministic
in tests (no sleeps, no mock clocks). `ShotController` passes
`EventBuffer::nowMicros()` — the same clock as every candidate timestamp, so
refractory arithmetic and hold deadlines live on the one timeline.

### Why candidates carry confidence but commits mostly ignore it

Confidence has exactly two jobs: the lone-strong threshold (0.8) and tie
metadata in the decision log. Agreement between independent modalities is far
stronger evidence than any single detector's self-assessed confidence, so the
fuse rule is structural (count distinct agreeing modalities), not a weighted
confidence sum. Resist the urge to "improve" it into one — that reintroduces
single-modality false positives through the back door.

### Threading map

| Piece | Thread | Why |
|---|---|---|
| `ImpactDetector::push` | GUI | BLE chain originates there (`QLowEnergyController` parented, no `moveToThread`) |
| `OnsetDetector::push` | Audio | data arrives there; receipt stamp must be taken there |
| `TemporalBallTracker::push` / `BallDetector::detect` | Detector thread | frame processing is expensive; runs behind a `FrameThrottle`, queued from the preprocessor |
| `ShotArbiter`, `ShotController`, hold timer | GUI | single-threaded by construction — no locks |
| `est_t` computation | producer thread | before any queued hop (jitter must not contaminate the estimate). Vision is the exception that proves the rule: it cannot compute `est_t` on the detector thread because the *frame* stamp is the anchor, not the arrival — so it emits `(frameTUs, ageUs)` and `CameraInstance` subtracts. Either way, "now" at the far end of a queue is never used. |

---

## 13. Testing

The detector tests live in four different suites, all standalone-configurable and
all reachable from the umbrella. The Qt prefix is auto-resolved by
`tests/cmake/PinPointTests.cmake` — pass `-DCMAKE_PREFIX_PATH` only if Qt is
somewhere non-standard. See `testing_developer_guide.md`.

```bash
# All suites in one configure
cmake -S tests -B build/tests && cmake --build build/tests -j6
ctest --test-dir build/tests --output-on-failure -R 'impact|acoustic|arbiter|ball'

# Or a single suite, for fast iteration:
cmake -S src/IMU/tests   -B build/imu-tests    # impact_detector_test
cmake -S src/Audio/tests -B build/audio-tests  # acoustic_shot_detector_test
cmake -S src/Analysis/tests -B build/analysis-tests  # arbiter_test
cmake -S src/Pose/tests  -B build/pose-tests   # ball_temporal_*, contract
cmake --build build/imu-tests -j6 && ctest --test-dir build/imu-tests --output-on-failure
```

The truth tables **are** the subsystem's value — every gate exists to kill a
specific false-positive class, and each class has a named test:

| Test (suite) | Cases |
|---|---|
| `impact_detector_test` (IMU) | strike fires exactly once with back-dated `est_t`; mat tap (spike, flat gyro) rejected; waggle rejected; slow swell rejected (jerk gate); refractory collapses double-hits; orientation gate on/off; 100 Hz ≡ 200 Hz; startup guard; sensitivity scale accepts/rejects a weak swing |
| `acoustic_shot_detector_test` (Audio) | click fires once, sample-accurate (0-sample error); sustained tone rejected at start (decay gate) AND at cutoff (attack-only rule); speech-like bursts rejected; ambient noise rejected; 20 ms double-click → one onset, 100 ms → two; `estimateImpactUs` math |
| `arbiter_test` (Analysis) | 2-modal agree → one commit, acoustic timestamp; lone-weak reject; lone-strong commit; Imu > Ball authority; disagreement beyond ±40 ms; refractory drops echoes; manual `noteCommit` arms refractory; `cancel()` voids a window |
| `ball_temporal_test` (Pose) | the v2 tracker's seed → lock → presence → collapse-cliff path |
| `ball_temporal_parity_test` (Pose) | the live tracker and the offline reconstruction agree — the same guarantee `swing_window_parity_test` gives the window |
| `ball_detector_contract_test` (Pose) | the `FrameThrottle` contract: exactly one `ballDetected`/`detectionSkipped` per `detect()`, and the temporal signals never substitute for it |

`arbiter_test` lives in the Analysis suite rather than a Gui one because
`shot_arbiter.h` is pure header math with no Qt — the Analysis suite is where
header-only math without an obvious home goes.

### Hardware verification (pending — the field checklist)

- Real strikes fire with `autoDetectSwing` on; mat taps and waggles do not.
- 200 Hz BLE stability: `dataRateHz` ≈ 200, `gimbalDropCount` flat.
- Acoustic latency: mic by the impact, compare the committed acoustic timestamp
  against the simultaneous IMU spike, tune `audioDeviceLatencyUs`.
- The real acceptance metric: false positives counted over a full range
  session.

A headless middle tier also exists: replay a recorded BLE trace
(`beginRawDump`/`endRawDump`) into `ImuInstance` under
`QT_QPA_PLATFORM=offscreen` and assert exactly one `shot_marker_v1` with
`source == Imu` via `SwingWindow::entriesFor`.

---

## 14. Common Mistakes

### Wiring a detector to `triggerShot()` instead of `reportCandidate()`

`triggerShot()` is the manual path: it bypasses the hold, cancels pending
windows and commits immediately. An auto detector wired there reintroduces
single-modality false positives and silently discards other modalities'
corroboration. P3 re-pointed the P1/P2 connections for exactly this reason.

### Computing `est_t` after a queued hop

A `Qt::QueuedConnection` adds event-loop scheduling jitter (ms-scale under
load). The estimate must be fixed on the producer thread; only the already-
computed number may cross threads.

### Deriving IMU confidence from peak acceleration

The ±16 g full scale clips real strikes — a clean strike and a mishit can both
read 16 g. Amplitude is a gate, not a magnitude. Confidence comes from swing
energy.

### Gating inference at the FrameThrottle / pausing the buffer from a detector

Detection is **signal-only**. Detectors never pause, resume, or replay the
buffer — buffer state is owned exclusively by the user capture intent
(CLAUDE.md "Capture intent"), and the post-shot freeze belongs to
`ShotProcessor`.

### Adding a hot-path mutex or a new EventBuffer producer to a detector hook

The IMU hook deliberately rides the existing GUI-thread display handler, not
the `Qt::DirectConnection` ring-write lambda — the EventBuffer producer
contract (stop barriers, no mutexes on `acquireWriteSlot`) stays untouched. A
detector needs no ring access at all.

### Expressing detector windows in sample counts

`refractorySamples = 20` means 200 ms at 100 Hz and 100 ms at 200 Hz — a silent
behaviour change on a rate switch. Every window in this subsystem is
milliseconds against sample timestamps; keep new ones that way (the
rate-independence test will catch you).

### Trusting a presence signal as a launch signal

Ball *presence* is smoothed for UI stability; present→absent lags the strike
badly. Inside a ±40 ms match tolerance that is worse than no candidate at all —
it can only miss the window or, worse, match a *different* strike. The launch
candidate comes from the tracker's per-frame collapse **cliff**
(`BallDetector::ballLaunched`), never from the presence property.

### Forgetting that a struck ball can come back

It bounces off the net and rolls back through the ROI, re-locks, and fires a
second launch — observed 0.53 s after impact, and it overwrote the real launch
before the window-freeze snapshot. `CameraInstance` keeps the **first** launch of
any 2 s cluster for exactly this reason. Any new vision signal needs the same
thought: the physical event you are watching for does not stop happening after
the shot.

### Stamping a frame-derived event with "now"

The ball detector runs on its own thread behind a throttle, so the moment its
signal is *handled* is unrelated to the moment the frame was *captured*. Every
frame carries `BallDetection::tUs` — the same value its EventBuffer ring entry
got. Use it. `nowMicros()` appears in that path only as a defensive fallback for
an unknown (`-1`) frame time.

### Re-using the acoustic detector at STT's 16 kHz

The impact click's energy is high-frequency; the 16 kHz STT feed (and its
silence gating) destroys both the detection SNR and the sample-accurate
timing. The detector subscribes to the raw native-rate `audioDataReady` fan-out
for a reason.

---

## 15. File Map

> `src/Gui/` was reorganised from a flat folder into feature subfolders
> (`app/`, `cameras/`, `imu/`, `shot/`, …). Paths below are current; older
> references to `src/Gui/shot_controller.cpp` and friends predate that move.

```
src/IMU/
├── impact_detector.h           ImpactDetector — pure gate math (P1)
└── tests/
    ├── CMakeLists.txt
    └── impact_detector_test.cpp

src/Audio/
├── onset_detector.h            OnsetDetector + estimateImpactUs — pure math (P2);
│                                 minLevelAbs absolute gate, 45 ms decay window
├── acoustic_shot_detector.h    Thin QObject wrapper (audio thread)
├── acoustic_shot_detector.cpp  Receipt stamping, format conversion, emit
└── tests/
    ├── CMakeLists.txt
    └── acoustic_shot_detector_test.cpp

src/Gui/shot/
├── shot_arbiter.h              ShotArbiter — hold/fuse/commit math (P3)
├── shot_controller.h           ShotController — armed gate, triggerShot,
├── shot_controller.cpp           reportCandidate, commitShot, shot marker
└── shot_processor.{h,cpp}      Post-roll → freeze → analyse ∥ export → replay;
                                  postRollMsFor, owns the SwingWindow

src/Gui/imu/
├── imu_instance.{h,cpp}        IMU live hook + impactDetected + kImuBleLatencyUs
└── imu_manager.{h,cpp}         Signal forward + sensitivity mapping + 200 Hz default

src/Gui/cameras/
└── camera_instance.{h,cpp}     Vision hook: stamps ballLaunched to an absolute
    camera_manager.{h,cpp}        impact time, relaunch guard, ballLaunchInfo();
                                  setBallRoi

src/Gui/app/
└── app_settings.h              autoDetectSwing, swingDetectionSensitivity,
                                  audioDeviceLatencyUs, audioInputDevice,
                                  acousticShotDetectionEnabled, acousticSensitivity,
                                  cameraBallRoi

src/Gui/media/
└── transcription_controller.*  Owns AcousticShotDetector on the audio thread;
                                  setShotDetectionActive gates the mic

src/Gui/
└── main.cpp                    The three gated reportCandidate wirings

src/Analysis/tests/
└── arbiter_test.cpp            Arbiter decision table (header-only math suite)

src/Pose/                       Vision — a live candidate producer (§7)
├── ball_temporal.h             TemporalBallTracker — matched-filter core, lock,
│                                 presence, collapse cliff. Self-calibrating.
├── ball_detector.{h,cpp}       BallDetector — baseline seed/re-seed, ROI, throttle
│                                 contract, ballLaunched/ballLocked/baselineReady/
│                                 exposureWarning, BallBaselineSnapshot
└── tests/
    ├── CMakeLists.txt
    ├── ball_temporal_test.cpp           Tracker seed → lock → cliff
    ├── ball_temporal_parity_test.cpp    Live vs offline reconstruction agree
    └── ball_detector_contract_test.cpp  FrameThrottle contract
```

---

*For the research survey behind the multi-modal approach see
`docs/design/shotdetection.md`; for the implementation phases see
`docs/implementation/shot_detection_impl.md`; for the vision detector's design see
`docs/design/ball_detection_v2.md` (+ `docs/implementation/ball_detection_v2_impl_plan.md`).
Downstream of `shotDetected`, see
`docs/developer/shot_analyzer_developer_guide.md`.*
