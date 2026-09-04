# Honest presentation of noisy metric curves

**Audience**: developers working on the analysis producers, the reducers (chart summary and diagnostics engine) and the review chart
**Code**: `src/Analysis/lower_body_metrics.cpp`, `upper_body_metrics.cpp`, `pose_smoother.{h,cpp}`, `metric_channel.h`, `swing_analysis.h` (`MetricSeries`); `src/Gui/review/chart_metrics.cpp`, `PpChartSummary.qml`, `PpChartPlot.qml`; `src/Diagnostics/measure_sample.cpp`
**Plan**: `docs/implementation/metric_presentation_honesty_impl_plan.md`
**Status**: design agreed in principle 2026-09-04; nothing built
**Written**: 2026-09-04

---

## Contents

1. [What prompted this](#1-what-prompted-this)
2. [The evidence](#2-the-evidence)
3. [Three different problems wearing one symptom](#3-three-different-problems-wearing-one-symptom)
4. [Principles](#4-principles)
5. [The design](#5-the-design)
6. [What this deliberately does not do](#6-what-this-deliberately-does-not-do)
7. [Definition of done](#7-definition-of-done)
8. [Decisions taken, and the ones still open](#8-decisions-taken-and-the-ones-still-open)

---

## 1. What prompted this

A review chart of the Plumb Bob preset (sway, hip line tilt, plumb bob) on a real swing. Every
curve is wavy at a frequency no pelvis can move at. Hip line tilt drops to **−88°** just after
impact. Sway spikes to **+35 %** of stance width and then settles at 12 %. The summary card
above the chart reports a sway PEAK of 34 % and a PK RATE of 39 % per 100 ms, and the P1→P7
deltas inherit whichever outlier happened to land on a phase.

The product position is explicit and it is the constraint on everything below: **be
intellectually honest with the user, and do not present a level of detail that is obviously
wrong.** Those two are not in tension once the failure is understood. Reducing measurement
error is a long-running goal and is not what this document is about. This is about what we
say while the error is what it is.

## 2. The evidence

Measured on `2026-08-18_Mark-Liversedge_Wrist_01/swing_0001/swing.json` (207 samples per
series, median spacing 8.4 ms in the dense zone, up to 102 ms at the address end). "Jitter" is
the absolute frame-to-frame change; "PK RATE shown" is what `ChartMetrics::summary` reports
for the whole swing; "range" is the raw min..max of the persisted curve.

| Series | Median jitter | 95th pct jitter | PK RATE shown | Range in series | Phase samples |
|---|---|---|---|---|---|
| `hipLineTilt` | 0.5° | 3.1° | 291°/100 ms | −85° .. +16° | P3 = +11°, P4 = +8°, P7 = −9° |
| `shoulderPlaneAngle` | 1.6° | 8.3° | 179°/100 ms | −88° .. +90° | **Top = +88°** |
| `pelvisSway` | 0.4 % | 2.1 % | 39 %/100 ms | −19 .. +42 % | |
| `plumbBobDistance` | 0.08 in | 0.5 in | 11 in/100 ms | −2.8 .. +4.6 in | |
| `leadKneeDrift` | 0.6 % | 2.6 % | 39 %/100 ms | −22 .. +41 % | |

Facts about the pipeline that bear on this, each verified in code:

- **The producers already read the smoothed pose.** `trackLowerBody`, `trackUpperBody`,
  `head_track` and `body_rotation` all take `pose.smoothed` when it is present
  (`lower_body_metrics.cpp:149` and siblings). What the chart shows is the residual **after**
  the RTS smoother, not raw ViTPose output.
- **The smoother is tuned for wrists.** One `sigmaJerk` (2.0e5 px/s³) is shared by every body
  keypoint 0–16; the per-group scales exist only for feet, face and hands
  (`pose_smoother.cpp:397-409`). The derivation in the header puts the effective window at
  ≈33 ms at 150 fps. A hip does nothing at that timescale, so the hips get the wrist's window.
- **The backswing is barely smoothed at all.** `PoseRunner` poses every frame only inside
  [impact − 500 ms, impact + 250 ms]; outside it `sparseStride = 4` (≈27 ms) and the address
  region uses `addressStride = 15`. A 33 ms window over 27 ms samples is roughly one and a half
  samples. The waviness in P1–P3 of the screenshot is that.
- **The −88° is not noise.** `lineTiltDeg` is `atan2(lead.y − trail.y, |dx|)`. After impact
  the pelvis turns toward the target, the two hips foreshorten toward the same image column,
  `dx → 0` and the angle swings to ±90°. `shoulderPlaneAngle` has the identical degeneracy and
  on this swing it fires **at the top**, which is a graded phase sample, not a chart curiosity.
- **The sway step after impact is projection, not sway.** `MetricDescriptor::stereoGain()`'s
  comment already states it: "turning the pelvis moves the APPARENT hip centre sideways with
  no sway at all". Sway is a P1–P7 quantity. The curve is drawn and reduced over P1–P10.
- **PEAK and PK RATE amplify noise by construction.** `ChartMetrics::summary` takes the raw
  argmax of |value| over every sample in the window, so it returns the largest outlier by
  definition; and PK RATE is the largest **adjacent-frame** |Δv/Δt| scaled to 100 ms. At 8 ms
  spacing, half a degree of jitter reads as 6°/100 ms and the 95th-percentile 3° reads as
  37°/100 ms. `measure_sample.cpp` builds its `PhaseGridSpan.min/max` from the same raw
  samples, so the diagnostics Extremum reducer inherits the same bias. (Its `At` reducer does
  not: it already takes a ±15 ms windowed median, `PhaseGridConfig::windowHalfUs`.)
- **The honesty plumbing exists and is unused.** `MetricSeries::sigma` (1σ measurement noise,
  "absent means not characterised, NOT zero error") is persisted, bridged to QML and rendered
  by `PpChartSummary.qml` as a "± σ" chip with a tooltip that already says the right thing.
  The smoother emits a posterior σ per keypoint per frame (`PoseKpAux::sigma`). No body-line
  or lower-body producer sets `sigma`, so the chip never appears on these cards. Only
  `body_rotation` (span-noise propagation) and tempo set it today.

## 3. Three different problems wearing one symptom

The screenshot reads as "measurement error" but it is three things, and the fixes do not
overlap:

**A. Residual jitter.** Genuine detector noise that the smoother's wrist-tuned window does not
remove from slow joints, made worse where the sampling is sparse. This is the only one of the
three that is actually about measurement, and it is the least damaging: a ±0.5° wobble on a
hip line is honest noise on a 1° display.

**B. Domain violations.** A metric evaluated where its geometry no longer means anything. The
foreshortening degeneracy of the body-line angles, and the lateral projection of a rotating
pelvis, are not errors in a measurement. They are readings of a quantity that does not exist
at that instant. Every implausible number in the screenshot (−88°, +35 %, the 88° top) is one
of these. The existing contract in `metric_channel.h` already says what to do: *"A frame whose
geometry could not be resolved is simply ABSENT — never a sentinel and never a zero"*. We are
not applying it to the cases that matter.

**C. Reducers that reward noise.** Raw argmax and adjacent-frame slope are the two statistics
most sensitive to a single bad sample. PEAK over n noisy samples is biased upward by the
expected maximum of the noise; PK RATE divides one sample's noise by one frame interval. These
would produce implausible tiles on a perfectly measured curve with ordinary noise.

## 4. Principles

1. **One curve.** The chart draws exactly the persisted series, the tooltip prints it, and the
   reducers grade it. No display-only smoothing in QML. A chart that shows one thing while the
   card and the corridors compute another is where dishonesty starts, and it hides the problem
   from the person who needs to fix the measurement.
2. **Absence over fabrication.** Where the geometry is degenerate or the metric is outside its
   phase domain, the sample is absent. This is already the producers' stated contract; this
   design extends it to the two cases that are currently letting confident absurdities through,
   and carries the absence all the way to the screen and the reducers instead of bridging over
   it.
3. **σ governs the digits.** A reading is shown no finer than its characterised noise, and the
   noise is shown beside it. That is the whole of "honest without looking wrong": a ±3°
   reading printed as 11° is honest; the same reading printed as 11.37° is not.
4. **Reducers are robust by definition, not by tuning.** A peak is the extremum of a
   short-window mean; a rate is a fitted slope over a minimum time base. Both are stated in
   the reducer's own definition and shared by every consumer, so the card and the diagnostics
   engine cannot disagree.
5. **Keep the raw.** `pose2d.frames` stays in swing.json unchanged, `pose2d.smoothed` beside
   it. Nothing in this design deletes or overwrites a measurement; it changes what is derived
   from it and how much of it we claim.

## 5. The design

### 5.1 Validity: geometric gates and phase domains (problem B)

**Geometric gate on the body-line angles.** A body-line tilt is valid only while the line's
image-plane span is a usable fraction of its address span. New config, mirroring the existing
denominator floors:

- `lowerBody.minHipSpanRatio` (default **0.40**): `|dx_hips| / addrHipSpanPx` below it ⇒ the
  hip line has no tilt this frame. Gates `hipLineTilt`, and `spineSideBend` in the upper-body
  module which reads the hip line too.
- `upperBody.minShoulderSpanRatio` (default **0.40**): the same for `shoulderPlaneAngle`,
  `elbowAlignment` and `spineSideBend`'s shoulder half.

The ratio, not an absolute pixel floor, because the address span is already measured and a
40 % collapse means the line has turned roughly 66° out of the image plane
(`acos(0.4)`), where `atan2` on a foreshortened dy/dx is measuring the camera, not the golfer.
At 0.4 the angle error from a 2 px keypoint σ on a 120 px address span is about 2.4°, which is
the same order as the residual jitter; below it the error grows as 1/ratio and passes 10° by
ratio 0.1. The value is sweepable and is not the point; that a floor exists is the point.

**Phase domain per metric.** A new descriptor field, `domain {Phase first, Phase last}`,
defaulting to the whole swing, authored in the manifest where a metric is only meaningful over
part of it. Initial authoring, the frontal-plane pelvis family:

| Metric | Domain | Why |
|---|---|---|
| `pelvisSway`, `pelvisLift`, `leadKneeDrift`, `plumbBobDistance`, `hipLineTilt` | Address → Impact (P1–P7) | past impact the pelvis has turned; the lateral projection is rotation, not translation |
| `shoulderPlaneAngle`, `elbowAlignment`, `spineSideBend`, `secondaryAxisTilt`, `thoraxLateralDrift` | Address → Impact | same argument, one segment up |
| `comOverLeadFoot` | whole swing | it is READ at the finish and is defined as a distance along the stance line, which survives the turn |

The domain does three things: the reducers search only inside it (the chart summary clamps
its window to it; the pack validator in `measure_facets.cpp` refuses a window outside it);
the chart draws the curve outside it dimmed and dashed, with no phase dots and no summary
contribution; and phase samples outside it are not emitted.

**How a series carries validity.** `MetricSeries` gains an optional parallel mask,
`std::vector<uint8_t> valid` (one per `t_us`; empty ⇒ every sample valid, so every existing
series and every existing swing.json is byte-identical). A gated frame is absent from the
channel as today; the difference is that `buildLowerBodySeries`/`buildUpperBodySeries` no
longer let `interpChannel` bridge across it silently: the grid sample is still filled (the
curve stays continuous for the renderer) but marked invalid. Consumers:

- **Chart** (`PpChartPlot.qml`): invalid runs are drawn dashed at reduced opacity; the hover
  value reads "—" there. A gap is honest; a dashed bridge is honest AND keeps the eye on where
  the curve resumes.
- **Chart summary** (`ChartMetrics::summary`): invalid samples are skipped for min/max/peak
  and rate; a window whose edges are invalid interpolates from the nearest valid samples and
  says so (`partial: true`, rendered as a "partial" chip on the card).
- **Diagnostics grid** (`measure_sample.cpp`): invalid samples do not enter a phase's windowed
  median nor a span's extremes. A phase whose window has no valid samples gets no entry, which
  is exactly the existing "no value" path.
- **Persistence**: `valid` serialises as an integer array only when at least one sample is
  invalid; `shot_processor.cpp` and `disk_replay_source.cpp` mirror it.

### 5.2 Robust reducers, one implementation (problem C)

A new Qt-only header `src/Analysis/series_reduce.h` holding the three reductions, used by
`ChartMetrics::summary` and by `buildPhaseGrid` so the card and the corridors read one number:

- **At** stays the ±15 ms windowed median the diagnostics engine already uses; the chart
  summary adopts it for its window edges instead of a linear interpolation at one instant.
- **Delta** = At(end) − At(start). Unchanged in meaning; both ends now robust.
- **Extremum** = extremum of the **centred-window mean** of the valid samples, window
  `reduce.extremumWindowUs` (default **40 ms**, ≈5 samples dense, ≈2 sparse). A one-sample
  outlier cannot be the peak because a peak has to be there for 40 ms. The reported `atUs` is
  the centre of the winning window.
- **Rate** = the largest magnitude **least-squares slope** over any sliding window of at least
  `reduce.rateWindowUs` (default **50 ms**) with at least three valid samples, never an
  adjacent-frame difference. Reported per 100 ms as today. On a still address the fitted slope
  is near zero; today it is the frame noise divided by 8 ms.

Both windows are tuned constants (`tuned::reduce`) with dark-flag overrides, so they can be
swept on the corpus without a rebuild.

The diagnostics engine's `PhaseGridSpan.min/max` becomes the windowed-mean extremes, which
changes the value of every authored Extremum measure by roughly the noise amplitude. That is
a content-visible change and is gated in the plan (a corpus before/after of every
`extremum` measure, expected direction: peaks move toward the mean by about one σ).

### 5.3 σ propagation, and σ-governed display (problem A, the honest half)

The smoother's per-keypoint posterior σ (`PoseKpAux::sigma`, pixels) is propagated through
each metric's own geometry to a per-series σ, following the pattern `body_rotation.cpp`
already uses (per-frame propagated σ, median over the swing, set on `MetricSeries::sigma`
only when computed):

| Metric | σ (per frame) |
|---|---|
| line tilt (hip, shoulder, elbow) | `sqrt(σ_lead² + σ_trail²) / span_px` radians → degrees |
| `pelvisSway`, `pelvisLift` | `0.5·sqrt(σ_lead² + σ_trail²) / addrSpanPx · 100` |
| `plumbBobDistance` | as sway, times `mmPerPx / 25.4` |
| `leadKneeDrift` | `sqrt(σ_knee² + σ_hip²) / addrSpanPx · 100` |
| `spineSideBend` | `sqrt(σ_hipTilt² + σ_shoulderTilt²)` |

σ is only set where the smoother actually produced a value (tier Meas or Pred); a series built
from raw passthrough leaves it unset, per the field's contract. Persistence and the QML bridge
already exist.

What σ then buys, in the chart layer:

- **Display step.** `ChartMetrics::formatBare(v, unit, sigma)` rounds to the nicest step
  (1, 2, 5, 10 ×10ⁿ) that is not smaller than σ, floored at the current one-unit rounding. A
  hip tilt with σ = 2.5° prints as a multiple of 5°; a plumb bob with σ = 0.1 in keeps whole
  inches. The tooltip, the summary card and the legend chip all go through this one function
  (they already do).
- **σ beside the tiles.** The existing chip stays by the unit; PEAK and PK RATE additionally
  carry "± σ" and "± σ_rate" (the fitted slope's standard error), because those two are where
  a reader's trust is decided.
- **A ±σ ribbon** behind the curve, at very low opacity, optional and off by default until it
  has been looked at on real swings. It is the only way to *show* that the wobble is inside the
  noise rather than telling the reader so.

### 5.4 Smoother window for slow joints (problem A, the measurement half)

Add a fourth per-group scale to `PoseSmootherConfig`: `legsJerkScale` / `legsSigmaScale`
over the hip–knee–ankle keypoints 11–16, default **1.0** so every existing output is
byte-identical. Then sweep `legsJerkScale` downward on the corpus, targeting an effective
window of 80–100 ms on the hips (the derivation block in `pose_smoother.cpp` gives the
window as a function of `sigmaJerk`; a scale of about 0.05–0.1 should land there), and
promote a default only with a corpus before/after plus a control run, because pose runs are
non-deterministic and ~20 metrics differ at 1e-14 between identical runs.

This is a measurement change, not a presentation one, and it is last on purpose: stages
5.1–5.3 make the chart honest at whatever the noise is, and 5.4 then reduces the noise. Doing
5.4 first would hide problem B and C behind a prettier curve.

Not proposed: a shoulder/thorax scale. The shoulders move fast through transition and the
wrist-tuned window is nearer right for them; measure before assuming.

## 6. What this deliberately does not do

- No display-only smoothing in QML (principle 1).
- No change to any persisted `value` in stages 5.1–5.3. Stage 5.1 adds absence, 5.2 changes
  what is *derived*, 5.3 adds σ. Only 5.4 moves numbers, and it is gated separately.
- No new metrics, no changes to `core.json` content beyond what the validator forces (a
  measure whose window falls outside its metric's domain is a content bug and is fixed as one).
- No change to the session wizard or the live capture path; this is the review chart and the
  offline analysis.
- The summary-card text overflow visible in the second screenshot (values overlapping their
  neighbours) is a layout bug in `PpChartSummary.qml`, independent of all of this, and is
  listed in the plan as a side item.

## 7. Definition of done

Measured on the 108-swing corpus after re-analysis, with a control run:

1. **No phase sample outside its domain**, and no `hipLineTilt` / `shoulderPlaneAngle` phase
   sample at a hip- or shoulder-span ratio below the gate. Today: at least one graded Top
   sample at +88°.
2. **PK RATE over a still address window** (Address − 300 ms → Address) reads under **2 %
   stance width** or **2°** per 100 ms on every lower-body and body-line series. Today: 39 and
   291.
3. **Every PEAK tile is a value on the drawn curve** within σ, and no PEAK in the P1–P7 domain
   exceeds the 99th percentile of the domain's samples by more than σ.
4. **Every lower-body and body-line series carries σ**, and the σ chip renders on the card.
5. **The chart and the diagnostics engine agree**: for every authored `at`/`delta`/`extremum`
   measure, the chart summary over the same window reports the same number to display
   precision.
6. `parity_diff.py` reports byte-identical swing.json for every swing whose series contain no
   invalid sample (which proves stages 5.1–5.3 are additive where they do not fire).

## 8. Decisions taken, and the ones still open

Taken in this document:

- Validity is a per-sample mask, not a per-series time range, because the foreshortening gate
  can fire mid-swing (a golfer who turns hard at the top) and a range cannot say so.
- Invalid runs are drawn dashed, not omitted. A dashed bridge keeps the curve readable and is
  visibly different from a measurement; an omitted run invites the question "did it crash".
- Reducers live in `src/Analysis`, not `src/Gui`, because the diagnostics engine is the second
  consumer and it has no Gui dependency.
- The domain is descriptor data (manifest), not pack content, because it is a property of the
  metric's geometry, and a second pack author must not be able to disagree with it.

Open, to be settled during the plan:

- The display-step rule in 5.3 could feel coarse on the degrees scale (σ = 2.5° → 5° steps).
  The alternative is to keep whole units and lean on the ± chip. **Recommendation: try the
  step rule on the Plumb Bob preset first and look at it; fall back to the chip alone if it
  reads as evasive.**
- Whether `comOverLeadFoot` genuinely survives the turn, or whether the finish reading is
  itself a projection artefact. It is unscored and read at one instant; leave it whole-swing
  until someone looks.
- The σ ribbon: keep dark until seen.
