# Face-on producers — as-built record

**Date**: 2026-08-02
**Scope**: 19 planned metrics promoted to live, 2 retired as duplicates, from a face-on camera only
**Code**: `upper_body_metrics.{h,cpp}`, `body_rotation.{h,cpp}`, `club_delivery.{h,cpp}`, `metric_channel.h`, plus edits to `lower_body_metrics.{h,cpp}`, `pose_wrist_angle_source.{h,cpp}`, `wrist_analyzer.cpp`, `metric_providers.{h,cpp}`, `metric_catalogue_manifest.cpp`
**Content**: `core.json` (32 measures live, 2 re-pointed, 6 new `causes` edges)
**Design**: [`upper_body_face_on_metrics.md`](../design/upper_body_face_on_metrics.md), [`body_rotation_estimation.md`](../design/body_rotation_estimation.md)

---

## 1. What prompted this

An audit of the metric catalogue against what a face-on camera actually resolves. The starting
position: **49 of 109 authored measures were blocked on a missing producer, and those blocked 70 of
145 conditions** — nearly half the diagnostics library was written, cited, corridor-seated and
ungradeable. 37 descriptors carried `.planned`.

Two facts made this cheap. The pose, shaft and ball tracks are all already-live **face-on** products,
so most of the gap needed no new sensing. And the corridors were **already authored** for almost
every target measure, so the content half was largely a status flip rather than a new authoring
round.

---

## 2. What shipped

| Group | Keys | Producer |
|---|---|---|
| Upper body, frontal plane | `secondaryAxisTilt` `spineSideBend` `thoraxLateralDrift` `shoulderPlaneAngle` `elbowAlignment` `trailElbowHeight` `leadHandWidth` `leadUpperArmToChest` `leadArmToTorso` | `upper_body_metrics.cpp` (new) |
| Lower body | `feetAlignment` `comOverLeadFoot` | `lower_body_metrics.cpp` (extended) |
| Axial rotation | `pelvisRotation` `thoraxRotation` `xFactor` `xFactorStretch` | `body_rotation.cpp` (new) |
| Club delivery | `shaftAngleVsHorizontal` `attackAngle` `lowPointAhead` | `club_delivery.cpp` (new) |
| Wrist | `trailWristFlexExt` | `pose_wrist_angle_source.cpp` (extended) |
| **Retired** | `hipAlignment` → `hipLineTilt`, `shoulderAlignment` → `shoulderPlaneAngle` | — |

Catalogue **72 → 70** descriptors, **37 → 16** planned. Measures **51 → 83** live.

---

## 3. The decisions worth knowing

### 3.1 Analysis is now agnostic of session type

The body-metric stages were listed only in `wristProfile()`, which put a second, invisible gate in
front of conditions each stage already stated for itself. A swing recorded under the Swing session
produced no head, foot, lower-body or tempo metrics, and `wristSessionOk()` in
`metric_providers.cpp` then reported *"produced in Wrist Motion sessions only"* — which a golfer
reads as a statement about their equipment.

Both are gone. `appendBodyMetricStages()` lists the block once and every profile calls it; no
provider reads `sessionType`. This closes ledger `X1` in the content-extension plan and the
"constraint that caps all of this" note in the diagnostics guide §8.

### 3.2 Rotation ships as a two-tier producer, not a promise

Pelvis and thorax turn head the roadmap by a wide margin (8 measures, 11 characteristics) and no
shot has ever carried a trunk IMU. `body_rotation.cpp` prefers a bound `Pelvis` / `Thorax` stream
per segment and falls back to the cosine collapse of the hip or shoulder span in the image, resolving
`Bridged` with a **propagated** uncertainty in `MetricSeries::sigma`. Full accuracy envelope in the
design doc.

The series is an **unsigned magnitude of turn from address**. That was decided by the content, not by
the camera: `m_pelvisRotP4` is seated at +45° (the top, turned away) and `m_pelvisRotP7` at +40°
(impact, turned open), and no signed curve satisfies both.

### 3.3 Three descriptors were factually wrong

- **`attackAngle`** required `ReconstructionTier::Stereo3D` and claimed *"a down-the-line camera
  makes it fully in-plane"*. It is the reverse: the angle lives in the vertical plane containing the
  target line, which IS the face-on image plane, and a DTL camera puts that direction on its own
  optical axis. Corrected and promoted.
- **`launchDirection`** was marked `.faceOnCamera` but start direction left and right of the target
  is the depth axis. Corrected; stays planned.
- **`lowPointAhead`** said it was deferred until the measured-clubhead detector landed. It landed in
  `cbe68cd` and went default-ON in `df76fe9`, and its design doc's other blocker (the deleted v1
  ball calibration) was moot — `ball_position.cpp` yields the address ball centre and the px→mm
  ruler from the live v2 track. Both banners are now marked resolved.

### 3.4 Two duplicates retired rather than built

`shoulderAlignment` was geometrically `shoulderPlaneAngle` and `hipAlignment` was the live
`hipLineTilt`, each read at a different phase. `metric_reducer.h` exists to express exactly that
distinction. Worse than the usual duplication: the pairs carried **opposite sign conventions**, so a
reader comparing them would have concluded the golfer's hips and shoulders disagreed when they had
simply been described twice.

---

## 4. What was planned and deliberately NOT done

Three items from the approved plan were dropped once the code was in front of us, and each for a
reason found rather than assumed.

**`launchAngle` and `ballSpeed` — there is no ball-flight track.** The ball detector
(`ball_runner.cpp` + `ball_temporal.h`) is an **at-spot presence** tracker: it locks the stationary
ball, reports whether it is still at that spot and records the instant it vanishes.
`BallSample2D::center` is **always the locked spot** and never a ball in the air. Their descriptors
said "needs the ball track", which we have; what they need is a tracker that follows the ball after
it leaves. Building anything here would have been fabrication. Descriptors corrected to say so.

**`kinematicSequence` — the reduction exists; the inputs do not.** `kinematic_sequence.h` already
computes the ordered peak-speed nodes and the dashboard already consumes them. What is missing is
angular-SPEED series for the pelvis and thorax, which the new angle series make a short follow-on. It
carries **no measure and no corridor**, so promoting it would have unblocked nothing and required
inventing a wire format for `MetricType::Sequence` that nobody consumes.

---

## 5. What the tests caught

**`core_pack_test` found a real content defect the moment the producers landed.** Six conditions
became gradeable and had **no cause behind them** — `disconnection`, `hip_spin_out`,
`attack_too_shallow`, `insufficient_axis_tilt_impact`, `excessive_axis_tilt_impact`,
`abbreviated_finish`. The library would have reported each and been unable to say why. Six `causes`
edges were authored (`practice` tier, searched 2026-08-02), routing two to a thoracic-rotation
restriction, one to the lead hip, and three to conditions already in the pack. Acyclicity re-checked.

`diagnostics_catalogue_integrity_test`'s roadmap exemplar had to move for the second time: it was
`pelvisSway`, then `pelvisRotation`, and both have now left the roadmap. It is `spineForwardBend`
now — genuinely sagittal, so no clever reading of the frontal projection recovers it.

---

## 6. What is NOT validated

Say this plainly, because the tests passing is not the same claim.

- **No corpus tuning.** Every default in `upperBody.*`, `bodyRotation.*` and `clubDelivery.*` is a
  starting figure chosen for shape. `bodyRotation.spanNoisePx = 3.0` in particular sets what the app
  claims about its own confidence and rests on nothing measured.
- **No accuracy gate.** The unit tests pin **signs** and **refusal gates** — what a synthetic track
  can pin exactly — and say nothing about how close any number is to truth. SwingLab was not run.
- **The camera tier's address-square assumption is a BIAS, not noise.** A golfer set open or closed
  at address shifts every rotation reading by that amount and it does not average out. Bounding it
  against the shaft track's own address geometry is the single highest-value validation outstanding.
- **The detection engine is still dormant.** These producers put real values under corridors that
  have been dark since they were written — visible today in the corridor editor's histogram
  (`measure_sample.h`) — but nothing turns a shot into findings yet. That remains one adapter and one
  design decision away (`diagnostics_developer_guide.md` §8.2).

---

## 7. Verification performed

- `cmake --build build/tests` + `ctest -R` over the 12 directly-affected targets: **12/12 pass**,
  including four new ones (`upper_body_metrics_test`, `body_rotation_test`, `club_delivery_test`,
  and the extended `lower_body_metrics_test`).
- Full app build (`PinPointStudio`) — compiles, links, signs.
- Offline target (`swinglab_run`, `PINPOINT_BUILD_TOOLS=ON`) — compiles and links, confirming the
  three new `.cpp` were added to `_pinpoint_offline_sources` as well as the app target.
