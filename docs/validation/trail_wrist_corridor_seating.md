# Trail-wrist corridors: why p6 and p7 fire Action on most swings

**Status:** finding, 2026-08-20. No corridor was changed. This records what was measured and
what it means, so the next person to open `m_trailWristFlexExt_*` starts from evidence.

## The observation

`trailWristFlexExt` (`pose_wrist_angle_source.cpp buildTrailWristSeries`) carries seven live
measures, `m_trailWristFlexExt_p1` … `_p7` (`src/Resources/diagnostics/core.json`), each with a
`"source": "heuristic"` row in `norms.json`. p1 is `at p1`; p2–p7 are `delta` from p1.

Across the 79 library swings that carry a 133-keypoint pose and a full P1–P7 ladder, **200 of 553
graded readings — 36% — land on Action**, the most severe band. A measure that flags more than a
third of one golfer's readings is not grading; it is alarming.

(Measured after the plausibility limit landed, so this is not the impossible readings: the limit
already took 23 of them out of Action, and 36% is what remains.)

The failure is not spread evenly.

| measure | mu | sigmaLo | monitor | corpus p10 | median | p90 | Action | direction |
|---|---|---|---|---|---|---|---|---|
| p1 | 0 | 4 | −10..10 | 1.0 | **+8.4** | 16.4 | 35% | 27 above, 1 below |
| p2 | 15 | 10 | −1..31 | −23.6 | 10.8 | 29.0 | 25% | 13 below, 7 above |
| p3 | 32 | 10 | 16..48 | 9.9 | 23.3 | 38.3 | 25% | 16 below, 4 above |
| p4 | 45 | 12 | 27..63 | 21.6 | **42.2** | 60.7 | 23% | 10 below, 8 above |
| p5 | 40 | 12 | 22..58 | 8.2 | 34.5 | 46.4 | 23% | 16 below, 2 above |
| **p6** | 29 | 11 | 12..46 | −18.9 | **+8.0** | 19.0 | **66%** | **50 below**, 2 above |
| **p7** | 12 | 8 | −2..26 | −24.3 | **−2.1** | 8.0 | **56%** | **40 below**, 4 above |

Method: the gated curve (`pose.wristAngles.feLimitDeg` applied), sampled the way
`measure_sample.cpp` samples it — a ±15 ms windowed median at each phase instant — reduced per the
`core.json` reducers and graded per `norm.h` (outside `[monitorLo, monitorHi]` → Action, otherwise
the z-band capped at Watch; `sigmaHi` mirrors `sigmaLo` when absent).

## What the shape says

**p4 is well seated.** Corpus median +42.2° against a mu of 45°, and its Action cases split 10
below / 8 above — a corridor doing its job on a golfer who is sometimes inside it and sometimes
not. p4 is also the one row that was reasoned about explicitly rather than filled in: the header
note in `pose_wrist_angle_source.h` derives +45° from the trail wrist cupping at the top.

**p6 and p7 are not.** Both fail almost entirely in ONE direction — 50 of 52 Action cases at p6
are below the floor, 40 of 44 at p7. A corridor whose failures are one-sided is not measuring
variation around a centre; it is sitting in the wrong place, or being fed a different quantity
from the one it was written for.

The corridors expect the trail wrist to hold its cup deep into the downswing — mu 29° at p6, still
12° at p7. The camera says it has returned to roughly address by p6 (+8.0°) and slightly past it
by p7 (−2.1°).

## Which of the two is wrong

There is direct evidence, and it does not favour the camera.

The lead wrist carries the same geometry as the trail wrist, and it is the one wrist a criterion
instrument is ever worn on. Graded against HackMotion over the eleven 2026-08-18 swings:

- correlation of the camera's lead-wrist FE against the criterion: **|r| ≈ 0.61–0.74**
- a single global linear correction explains **31%** of the criterion's variance
- a correction refitted **per swing** reaches only **47%**, and its fitted scale ranges
  **−0.09 to −0.32** — a 3.5× spread across swings of one golfer in one session

So there is no stable projection gain: the camera's error is not a fixed distortion that could be
calibrated out. And its magnitude is largest exactly where p6 and p7 sit — the late downswing,
where the hands rotate hardest out of the camera plane.

The most likely reading is therefore that **p6 and p7 grade a projection artefact against an
anatomical expectation.** The corridors may also be optimistic, but that cannot be separated from
the measurement error with the data available.

## What NOT to do

**Do not re-seat these corridors on this corpus.** They are population norms and the library is
one golfer. Fitting mu to Mark's median would make his readings grade well and would say nothing
about anyone else — and it would bake a known projection artefact into the norm set as if it were
anatomy.

## Recommended next steps

1. **Reconsider `status` for the p6/p7 rows.** A measure that cannot be trusted at a position
   should not grade at that position. Demoting those two rows costs two readings per swing and
   removes the large majority of the Action noise.
2. **p1 deserves separate thought.** It is the only `at` reducer in the set, so it grades the
   ABSOLUTE apparent camera angle at address, seated at mu = 0 with a ±10° monitor. An absolute
   image-plane angle depends on where the camera was placed, so this row is camera-setup dependent
   by construction. The corpus median is +8.4° with 27 of 28 Action cases above the ceiling, which
   is what a fixed setup offset looks like.
3. **Anything that changes the curve should be judged against these numbers, not against
   "smoother".** For reference, a 6 Hz zero-phase low-pass on this curve moves 25% of the graded
   readings between bands while leaving the grade distribution essentially unchanged (Action
   129 → 127). That is churn, not improvement, until the seating above is resolved.

## Appendix — what the plausibility limit did, for reference

Corpus pass 2026-08-20, 83 swings with a 133-keypoint pose, `kFeLimitDeg = 120`:

| | |
|---|---|
| swings carrying at least one impossible frame | 63 of 83 |
| frames refused | 681 |
| worst single-frame jump in the curve (median) | 173° → 104° |
| swings carrying a >180° step | 40 → 2 |
| curve change, median sample | 0.00° on every swing |
| σ carried | 83 of 83, median 4.12°, IQR 3.25–4.94°, max 9.99° |
| grade changes | 25 of 553 (4.5%) |

Transitions: Action→Ideal 15, Action→Good 7, Action→Watch 2, Ideal→Action 1. Action fell 223 → 200.

Two residuals worth knowing, because neither is a defect in the limit:

- **The 2 surviving >180° steps** (`2026-07-04_Wrist_01/swing_0003`, `2026-07-08_Wrist_01/swing_0009`)
  are jumps between two IN-limit values — max |FE| is 111° and 115°. The limit tests a value, not a
  rate, so a frame that is individually plausible but impossible in sequence passes. A rate test
  would catch these; none has been measured, so none was added.
- **The single regression** is `2026-07-05_Wrist_02/swing_0006` at p4, +54.4° (Ideal) → +76.8°
  (Action). That swing lost 31 frames to the limit and its Top sample sat in the middle of them:
  the old +54.4° was the median of a window containing impossible readings, and it graded Ideal by
  luck. The detector failed at the top of that swing and neither number is a measurement of the
  wrist — but Action at least says so.

## Related

- `src/Analysis/pose_wrist_angle_source.h` — the σ note and the projection caveat
- `src/Core/pp_tuned_constants.h` `pose::wristAngles` — `kFeLimitDeg`, `kFcHz`, and the note that
  `kConfMin` does not filter the hand
- `docs/design/pinpoint_sign_conventions.md` — the +cup / +bow sign split between the two hands
