# Brief: a physical model of the wrist angle

*Written 2026-08-11 as a handover into a clean session. The work it describes has
not started. Prior work, including what failed, is in
[`docs/research/wrist_cock_model.md`](../research/wrist_cock_model.md); the
harness is `tools/swinglab/wrist_cock_fit.py`.*

## What this is

Build a model of the wrist angle ψ = θ − φ — the angle between the club shaft and
the lead forearm — through a golf swing.

**The objective is to EXPLAIN the swing, while fitting closely to the data we
have.** That ordering is deliberate and it decides the work. A lookup table
already fits better than anything parametric tried so far, so if accuracy alone
were the goal this would be finished. It is not the goal. What is wanted is a
model whose *parameters mean something* — quantities a coach would recognise and
that could be compared between swings, sessions and eventually golfers — which
stays close enough to the empirical curve to be credible.

Accuracy is therefore a **constraint, not the score**: a model that explains the
swing but fits materially worse than the table has not earned its place.

## Where the previous attempt stopped, and why it was only half the job

A 15-knot lookup table was fitted and it works, but a table is an empirical
curve: as many free numbers as knots, none of them meaning anything, free to
wiggle wherever data is thin. One parametric form was then fitted — a product of
two logistics, one for the cock and one for the release — and it is *not a
modelling exercise*:

- the form was chosen by looking at the empirical curve and picking a shape that
  resembled it;
- it fits because the curve is sigmoid-ish, and would fit about as well whatever
  the underlying mechanism;
- no alternative families were fitted, nothing was derived from the two-link
  dynamics, and no model selection was performed.

It reached 32.6° against truth versus the table's 20.9°, with seven parameters
that are stable to under a millisecond across leave-one-out folds. Treat it as a
baseline to beat and a shape to explain, not as prior art to defend.

## Task 0 — the truth upgrade (blocking; do this first)

**Every number in the prior work is graded on the wrong labels**, and this is the
single highest-value change available.

The grading used 463 hand-placed shaft labels from `truth.json`. Those cluster
at swing progress 0.30–0.57 — the transition region — because that is where a
human can *see* the shaft to label it. That distribution already inverted one
conclusion: refitting the swing-progress axis looks worthless scored against
those labels (75.6° vs 74.3°) and is a 22° gain scored frame-wide. The
label-selection bias the programme report documents bit us from the inside.

Better truth already exists, unused. On the shared corpus at
`shaftlab/lab/tape_20260705/s01..s10`:

| file | what it is |
|---|---|
| `fusion/faceon_swing_fusion.csv` | **1,033 rows across 10 swings** — the instrumented-fusion truth, `frame, t_s, tier, n_match, theta_deg, …`, tiered `band` (tape pattern locked, ~0.3° median) or `ray` (~1.7°). Covers the **fast phases** where hand labels cannot go. |
| `anchors.csv` | 745 rows/swing: per-frame grip anchor (x, y, angle, flag) |
| `skeleton.csv` | 745 rows/swing: 8 joints × (x, y, conf) per frame |

The last one matters as much as the first: φ can be derived from `skeleton.csv`,
so θ and φ come from the *same* source at the *same* instants, rather than
truth-θ being matched against production-pose-φ across two pipelines.

Work required, and none of it is assumed to be trivial:

1. Verify what the columns are and what convention `theta_deg` uses (the
   production θ is `atan2(head − grip)` in image pixels; confirm the fusion CSV
   agrees, on a frame where both exist).
2. Establish the timebase mapping — the CSV is frame-indexed with `t_s`, the
   production corpus is `t_us`; the `s01..s10` naming has to be tied to the
   session/swing directories.
3. Derive φ from `skeleton.csv` using the same lead-elbow→grip definition the
   tracker uses, and confirm it agrees with the production φ where both exist.
4. **Re-baseline everything.** The empirical table, the axis floors, the
   parametric form — all of it, on the new truth. Expect the numbers to move.
   Report both gradings side by side once, then use the instrumented truth.

Grade band-tier and ray-tier separately. Band tier is roughly 0.3° and is the
closest thing to ground truth the programme has.

## Task 1 — derive candidate families from the dynamics

Do not pick shapes that look right. Start from the two-link system the whole
programme keeps invoking — lead arm and club, coupled at the wrist — and derive
what ψ(t) *must* look like under different assumptions about the wrist torque.
Families worth deriving and comparing:

- **Passive release.** The wrist holds a fixed angle until the arm decelerates,
  after which the club swings free under centrifugal load. This is the textbook
  account of the golf downswing and it *predicts* a functional form rather than
  borrowing one. Parameters: the hold angle, the release instant, and the
  inertia ratio that sets how fast the free swing proceeds.
- **Torque-limited release.** As above but with a bounded wrist torque resisting
  the release, which gives a different — and testable — release profile.
- **Constrained-then-free with a soft handover**, since a real wrist does not
  switch discontinuously.
- **The logistic product**, carried forward as the phenomenological baseline.

For each: state the assumptions, derive the form, and say in advance what would
falsify it. A family that cannot be falsified by this data should be marked as
such rather than fitted and reported.

## Task 2 — fit, and select on the right criterion

Fit every family on the same samples with the same robust loss, leave-one-swing-
out throughout. Then select on criteria that match the objective:

1. **Out-of-domain prediction.** Fit on the backswing, predict the release. A
   curve-fit will fail this and a physical model should not. This is the sharpest
   available test and the prior work never ran it.
2. **Parameter meaning.** Do the parameters move sensibly between sessions? Does
   the release instant track anything else we measure? A parameter that varies
   randomly between swings of the same golfer is a fitting artefact.
3. **Accuracy, as a constraint.** Within reach of the empirical table's
   frame-averaged 17.4° / truth 20.9° — re-baselined per Task 0.
4. **Parameter count**, as a tiebreak only.

## Task 3 — write it up

Extend `docs/research/wrist_cock_model.md` (or supersede it) with the derivation,
the families that lost and why, the falsification tests, and the parameters with
their physical reading. The negative results are the valuable part; the existing
document's "what did not work" section is the model for how to record them.

## Constraints and standing decisions

**One athlete, accepted.** Every number is one golfer, one club type, five
sessions. This is true of the entire codebase, and getting more data waits until
there is something credible to show. So: do not pretend otherwise, do not tune
anything to the point where it would only work for him, and state the limitation
on the face of any result. It also means **"which family generalises" cannot be
answered here** — the selection criteria above are the best available proxies,
and that should be said plainly rather than implied away.

**Nothing ships from this work.** The C++ wrist-cock table is committed and dark
behind `shaft.wedge.kinModelV2`, and it stays dark: the corpus A/B showed the
fitted table starves the blur-wedge trigger — it costs 38% of the wedge stamps
and 8–15 P-position emissions — because the same table feeds both the search
centre and the trigger rate, and a curve that correctly holds its lag flat
contributes almost no rate until the release. Integration is a separate problem
with its own gate, and it is not this brief's.

**Do not re-open frozen constants.** No retuning of `omegaMinDegS`, the evidence
constants, or anything else in the tracker.

## What already exists

`tools/swinglab/wrist_cock_fit.py` — fitting and grading harness. Reuse it.

- Loads a run root of `result.json` dirs plus the corpus for `truth.json`.
- Derives β = chir · wrap180(θ − φ) in the header's convention, so any fitted
  table drops into `shaft_kinematics.h` unchanged.
- Lead side is decided per swing from the pose (grip ordering over the address
  hold — the trail hand sits below the lead hand), never from metadata.
- Leave-one-swing-out throughout; forms are cloned by type (`Form.clone()`).
- Reports truth and tracker-tier gradings, phase segments, sessions, and
  envelope coverage.

`tools/swinglab/theta_psi_model.py` — the corpus shape model, which supplies the
series extraction (`arm_series`, `decide_lead_side`, `smooth_angle`) and also
carries the upper arm β and whole arm α, which the wrist work has not used and
which a two-link model will need.

**Settled, do not re-litigate:** the axis is **seconds before impact**. Its floor
is 16.1° frame-averaged against swing progress's 30.4°, and the reason is
physical — release is an event at a fixed time before impact, not at a fixed
fraction of the swing.

## Traps, each of which has already bitten

- **Never rank models on the truth column alone.** Where the labels sit changes
  the ranking. Report frame-averaged and truth side by side, always.
- **Clone forms by type in leave-one-out.** Constructing a base `Form` for a
  parametric candidate silently fits it as a two-knot straight line, which reads
  as a catastrophic model failure rather than a harness bug.
- **Knot placement confounds axis comparisons.** Give every candidate the same
  knot budget and layout before concluding anything about an axis.
- **Local linear fits carry curvature into the knot value as bias.** β moves
  more than 100° in the last tenth of a second; use a local quadratic, or the
  window has to shrink until σ is starved of data.
- **σ estimated in-sample is optimistic** whenever a regressor absorbs
  between-swing variation it cannot predict out of sample. Check envelope
  calibration, not just residual width.
- **Trace and diagnostic re-runs can disagree with the shipped analysis.** Judge
  from persisted `result.json`, never from a trace summary.
