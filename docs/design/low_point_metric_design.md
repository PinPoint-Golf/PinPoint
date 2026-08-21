# PinPoint — Low-Point-Ahead-of-Ball: Technical Design

> ## ✅ SHIPPED, 2026-08-02 — both blockers cleared
>
> This design's two stated blockers are resolved and the metric is **live** in
> `src/Analysis/club_delivery.{h,cpp}` (`ClubDeliveryStage`, `ClubDeliveryProvider`). Read the two
> historical banners below as the record of what stood in the way, not as current status.
>
> * **"Deferred until the measured-clubhead detector lands."** It landed: `clubhead_track.{h,cpp}`
>   shipped in `cbe68cd` and went default-ON in `df76fe9`. The producer reads `headPx` from
>   MEASURED samples only and refuses `ShaftHeadProjected` outright — a projected head is a rigid
>   function of the grip and the shaft angle, so its arc vertex is the grip's, not the club's.
> * **"Waits on Provenance v2 for its ball input."** It does not, and this was the discovery that
>   unblocked the work. `ball_position.cpp` already derives a robust address ball centre (a
>   component-wise median with a cluster gate) AND the ball-diameter px→mm ruler from the LIVE v2
>   track, and it yields both even when the heel pair is unusable. `ClubDeliveryStage` calls it with
>   a deliberately degenerate heel pair for exactly that reason.
>
> **As built, two things differ from §1–§6 below.** The arc vertex is refined below frame spacing
> with a three-point parabola through the lowest sample's neighbours (accepted only if it lands
> inside the bracket it was fitted through) — at impact speeds one frame of quantisation is inches.
> And the target direction is taken from the **clubhead's own horizontal travel across impact**
> rather than from pose handedness, so a mirrored camera cannot invert the sign and the metric needs
> no skeleton at all.

**Scope:** define, as a drop-in contract, the club-delivery metric *low-point
distance ahead of the ball* — how far target-side of the ball the clubhead
reaches the lowest point of its swing arc — estimated from the face-on shaft
track + the ball position, reported in **signed inches** (+ = ahead / target
side, − = behind).

**Why:** it's a coaching-valuable delivery metric that the GCQuad does not
provide, and we already track the (approximate) clubhead trajectory and the ball.

---

## 1. Definition

`lowPointAheadIn` = signed horizontal (target-line) distance, in inches, from the
ball centre to the clubhead's arc low point.

- **+** the low point is ahead of the ball (target side) — the descending-blow
  ball-then-turf pattern good iron players want.
- **−** the low point is behind the ball (fat/scoop pattern).

Per-session, per-shot; never aggregated. Criterion-referenced (a real geometric
distance), not scored against a band in v1.

## 2. Inputs — all already persisted after this change

Everything the computation needs is in `swing.json`, so it can run **offline**
(re-analysis over a corpus) with no re-capture:

| Input | Source in swing.json | Notes |
|-------|----------------------|-------|
| Clubhead trajectory | `analysis.club.samples[].head` = `[x,y]` normalized 0..1 by `frameWidth`/`frameHeight`; per-sample `t_us`, `theta`, `flags` | y is **image-down**. `flags & 0x10` (`ShaftHeadProjected`) marks projected (not measured) frames. Only written when the shaft track is valid. |
| Ball centre | `setup.ballDetection.center = [x,y]` (full-frame normalized) | co-registered with `head`; per face-on camera. Omitted when uncalibrated. |
| Ball scale | `setup.ballDetection.radiusNorm` (normalized to frame **width**) | `radiusPx = radiusNorm · frameWidth` |
| Impact | `capture.impactUs` (IMU jerk-peak; also `analysis` Impact phase) | anchors the low-point search window |
| Handedness | `athlete.handedness` | target-direction sign (with mirror) |
| Scale constant | `pinpoint::ballcal::kBallDiameterMm = 42.67` (`src/Pose/ball_model.h`) | R&A/USGA ball diameter |

The `setup.ballDetection` position is the **calibrated address ball**
(`BallCalProfile.ball.calibCenter`/`radiusPx` resolved to full-frame coords by
`CameraInstance::applyBallCalProfile`), i.e. where the ball sat before the swing
— the correct stationary reference, at the ball's ground-plane depth (so the
ball-diameter scale is exact at the measurement point).

## 3. Geometry (the deferred computation)

All in the face-on image plane (`ReconstructionTier::Angles2D`; no 3-D/ground
plane exists). Let `W = frameWidth`.

1. **px→mm scale** at the ball's depth: `mmPerPx = kBallDiameterMm / (2 · radiusNorm · W)`.
2. **Low-point frame**: over `head` samples in an impact-anchored window
   `[impactUs − Δ, impactUs + Δ]` (Δ ≈ one downswing-to-early-follow-through
   span; irons bottom out at/just after impact, driver before), take the sample
   with maximal `head.y` (lowest in the image). Prefer a **parabola-vertex fit**
   of `head.y` vs `head.x` (or vs `t`) across the few samples around that
   minimum for a sub-frame low-point `head_x_lp`.
3. **Signed distance**: `aheadMm = (head_x_lp − ball_x) · W · mmPerPx · chirality`;
   `lowPointAheadIn = aheadMm / 25.4`.
   - `chirality` (±1) folds handedness × mirror. Reuse the shaft tracker's
     resolution (`autoChirality` from pose hand-centroid ordering,
     `shaft_tracker.cpp:455-546`); handedness-only fallback `= (handedness==2) ? −1 : +1`.

## 4. Accuracy caveats (why compute is deferred)

- **The clubhead is projected, not measured.** `ShaftSample2D.headPx` is a
  ridge-terminus seed or `grip + visibleLenPx·dir(θ)` (flagged
  `ShaftHeadProjected`). The vertical arc it traces is dominated by shaft angle ×
  visible length, so its low point is a weak proxy for the true clubhead
  ground-strike low point — especially near impact (motion blur, foreshortening,
  specular dropout shrink `visibleLenPx`).
- **The real unlock is the measured clubhead detector** (Stage 2 —
  [clubhead_detection_design.md](clubhead_detection_design.md); Python exemplar
  only, not yet in the app). Once `head` is measured, this metric is a small,
  trustworthy addition; on the projected head it is an experimental estimate.

> **Update (2026-07-09).** The measured-head prerequisite above has landed:
> `src/Analysis/clubhead_track.{h,cpp}` (wired into `ShaftTracker::decideTrack`,
> default ON as of `df76fe9`) measures `head` on meas-tier frames with
> `headConf`/`headSigmaPx` — a label-grade tier (σ ≤ 10 px) is available, and
> delivery-phase confident coverage is preserved by design (the backswing
> streak confidence cap keeps low-confidence motion-blur claims out of the
> meas tier, which is exactly the phase this metric's low-point window cares
> about). This metric **remains deferred** — nothing here changes the ball
> position/provenance side (§2's "Provenance v2" blocker) or the validation
> requirement of §7 — but the accuracy caveat above is no longer the blocking
> one: the low-point search can now run against a measured `head` instead of a
> projected one whenever the Stage-2 pass reaches meas tier in the search
> window. See `docs/implementation/shaft_tracker_impl.md`'s Phase B note and
> `docs/design/clubhead_length_status.md`'s RESOLUTION block for the full
> picture.

> **Update (2026-08-21) — SHIPPED, AND NOT FROM THE MEASURED HEAD.** Everything
> above assumed the measured clubhead detector was the unlock. It was not, and
> the corpus is unambiguous about why.
>
> Across 108 recorded swings the Stage-2 head pass does not hold a measured lock
> through impact. It goes dark from roughly −45 ms to +40 ms — the club at its
> fastest, most motion-blurred, and lowest against the turf, which is exactly
> the window this metric reads. Requiring 5 measured heads inside ±60 ms of
> Impact produced a value on **9 of 108 swings**. Worse, on the three that could
> be checked the located vertex sat **+16 to +38 ms past impact** — over a metre
> beyond the ball. The gate was not filtering out bad swings; it was admitting
> bad vertices on the rare swing it fired. §4's caveat was right about the
> physics and wrong about the remedy: measuring the head does not help if the
> head cannot be measured *there*.
>
> **The metric is now read off the synthesized club arc** — `ShaftTrack2D::synth`
> (`shaft_synthesis.h`), the dense Hermite interpolation between the located
> P-anchors that the club overlay draws. Same vertex search, same sub-frame
> parabola, same ball reference and ruler; different source series. On the same
> corpus that yields a value on essentially every swing with the vertex landing
> within a few ms of impact.
>
> **What that costs, stated plainly.** The arc through impact is an
> *interpolation* between P6, P7 and P8, not an observation, so its vertex is
> pinned near the P7 anchor — what the metric really reports is where that
> anchor puts the head relative to the ball. Against a launch monitor on one
> session (2026-08-18 Wrist_02, six 7-iron swings) the arc's attack angle at
> impact was **unbiased (+0.02°) with a spread of 3.26°**; over the arc radii
> that session fitted (33–42 in) that is **±2.0 in of low point**, against a
> corridor only 3.2 in wide. The session *mean* recovered the device's own to
> 0.02° (+1.42 in against +1.43 in).
>
> So it ships as an **estimate, with the health warning attached in three
> places**: `MetricSeries::sigma = 2.0 in` rides with the number, the catalogue
> route is `RouteQuality::Estimated` so the reading resolves **Bridged** rather
> than Measured, and the route summary is shown verbatim as the reason. Read it
> across a handful of swings; distrust any single one.
>
> Two consequences worth knowing:
> * **It relaxes a stated invariant, narrowly.** `shaft_synthesis.h` called the
>   synthesized tier "excluded from every metric/scoring/estimand". That now has
>   exactly one exception, this metric. Scoring, the estimands, the plane fit and
>   the wrist channel still exclude it. The coupling that comes with it:
>   `synth.enabled=false` now takes `lowPointAhead` with it.
> * **An arc that never turns over inside the window is refused.** A vertex on
>   either end means the arc was still descending when the data ran out — what a
>   swing whose next P-anchor lands hundreds of ms away produces. That refusal
>   removed the one visibly-wrong value on the 2026-08-18 set.
>
> ⚠ **Six swings from one golfer on one session is not an error budget.** It is
> the first evidence we have, and `kLowPointSigmaIn` is a frozen constant rather
> than a per-swing propagation precisely so it cannot pretend otherwise. §7's
> validation requirement stands; re-seat the σ against a multi-session corpus
> with launch-monitor truth, and scale it by the fitted arc radius at the same
> time.

## 5. Where it slots (compute phase)

Model it on `buildShaftLeanSeries()` (`src/Analysis/wrist_analyzer.cpp:56-82`):

- Compute inside the real analyzer — **`WristAnalyzer`** today (the only analyzer
  that runs pose+shaft; Wrist sessions hit balls on a face-on camera). Emit a
  `MetricSeries` (key `lowPointAheadIn`, unit `in`) with a single phase sample at
  the low-point frame, plus the scalar in `ShotAnalysisResult.metrics`
  (key→{label,value}) → shot-card carousel. Not the session summary.
- If/when Swing (type 0) becomes a real analyzer, share the computation via a
  small helper consuming `ShaftTrack2D` + ball, callable from both.

## 6. Wiring to add AT compute time (NOT in this change)

- **`ShotAnalysisJob` ball fields** (centre normalized, `radiusNorm`, face-on
  camera source) — resolve on the UI thread in `ShotProcessor::buildAnalysisJob`
  from the face-on `CameraInstance` (`ballCalHasPosition()`/`ballCalCenterX/Y()`/
  `ballCalRadiusNorm()`), and **restore in `swing_reanalyzer.cpp`** from
  `setup.ballDetection`. (Adding these fields now would be unread dead state.)
- Close the known reanalyzer gaps this metric also wants: `clubLengthM` and
  `setup.mirrored` are not restored into the offline job today.

## 7. Validation

The GCQuad does **not** provide this metric, so ground truth is a **physical**
measurement: divot-start / mat strike-line distance from the ball. Corpus-gated
on SwingLab exactly like the shaft / ball / segmentation work
(single labelled swing = development data only; accuracy gates are corpus-scale,
per pipeline_validation_and_tuning.md). Report Limits-of-Agreement vs the
physical reference across many swings and multiple clubs before surfacing the
number in the UI.

## 8. v1 as-built (this change)

- `kBallDiameterMm = 42.67` — `src/Pose/ball_model.h`.
- `CameraInstance` resolves the calibrated ball to full-frame-normalized
  `center`/`radiusNorm` (+ `hasPosition`) — `src/Gui/cameras/camera_instance.*`.
- Persisted to `swing.json` `setup.ballDetection.{center, radiusNorm,
  positionSource}` per camera (additive; omitted when uncalibrated) —
  `src/Export/swing_exporter.*`, populated in `src/Gui/shot/shot_processor.cpp`.
- Clubhead trajectory persistence (`analysis.club.samples[].head`) was already
  present — unchanged.
