# Brief: a physical model of the wrist angle

*Written 2026-08-11 as a handover into a clean session. Prior work, including what
failed, is in [`docs/research/wrist_cock_model.md`](../research/wrist_cock_model.md);
the harness is `tools/swinglab/wrist_cock_fit.py`.*

> ## AMENDMENT 2026-08-11 — five of this brief's premises are false
>
> Task 0 and the projection layer have now been executed; results and the commands
> that produce them are in
> [`docs/research/wrist_projection_layer.md`](../research/wrist_projection_layer.md).
> Five load-bearing claims below did not survive contact with the data. They are
> left in place, because the reasoning around them is still worth reading, but do
> not act on them:
>
> 1. **"Better truth already exists, unused" (Task 0) is ~2/3 wrong.** For the
>    2026-07-05 session `truth.json` **is** the fusion band tier verbatim — 9 of 10
>    swings match row-for-row, head positions to 0.000 px, θ to 0.0063°. 565 of the
>    corpus labels on those swings are instrumented band rows; only 61 are hand
>    placed. The genuinely new material is the **ray tier** (283 pre-impact rows)
>    and tier/provenance separation, not volume.
> 2. **"Derive φ from `skeleton.csv`" is impossible, and the motivation for it is
>    wrong.** That file holds 8 body joints because it is a clutter mask. The
>    production pose is **133-point COCO-WholeBody (ViTPose)** with elbows, wrists
>    and 21 landmarks per hand. Separately, `anchors.csv` is *not* the better φ: it
>    is interpolated (0.12° backswing jitter — too smooth to be a measurement) and
>    degrades to 3.51° in the downswing against production's 1.79°. Production φ
>    stays. Prior numbers are **not** φ-limited.
> 3. ~~The clubhead-path ellipse is not an independent observable.~~ **WITHDRAWN —
>    this was my error, not the brief's.** The head path IS planar and the ellipse
>    DOES recover the plane — from the SHAFT VECTOR (head − grip), not the absolute
>    head path: ι = 40.0° median in the backswing, 20.0° in the downswing, a
>    **+17.3° shift**, at 0.6–0.7° split-half repeatability. Foreshortening
>    corroborates in the backswing (7.8° median) but not the downswing (14.9°),
>    and reads systematically high. The mistake was
>    confusing provenance (`head = grip + lenPx·u(θ)`) with structure (whether the
>    3-D path is planar — a real claim the algebra does not grant).
> 4. **"`lenPx` runs 189→414 px, 2.2×" does not reproduce.** At `stagegate/corpm3-off`
>    the ratio is 1.58–2.08 (median 1.77), so "approaching 60°" survives but the
>    figures do not. Worse, the absolute level moves **36% between swings of one
>    session with one club** — stage-2 drift no plane can explain.
> 5. **"Forearm roll cannot be seen at all"** is sound for the shaft but overstated
>    in general — the hand is not axially symmetric. It is not pursued, because the
>    hand keypoints are unreliable, but treat it as unproven rather than settled.
>
> **Two additions this brief did not have.** The lead **forearm's** projected length
> is a fourth observable, free and already captured, foreshortening *harder* than the
> shaft (2.07–2.51× vs 1.58–2.08). With it the wrist becomes a 2-DOF relationship
> between two 3-D lines — ψ₃ (bend magnitude) and χ (bend direction) — identifiable
> rather than free. And `tools/shaftlab/length_model.py` already implements this
> brief's projection geometry (`rho_plane`, families M0–M4); start from M2/M3.
>
> **Status: the plane is measurable and the back-to-down DELTA is the prize.**
> Precision 0.6–0.7°, delta +17.3° median, 8/10 swings steepening — signal far above
> the noise floor. Absolute calibration is NOT yet confirmed and the sign convention
> (larger ι = flatter) needs one down-the-line cross-check before it becomes a
> coaching output. The full model definition is in
> [`docs/research/wrist_model_definition.md`](../research/wrist_model_definition.md).
> θ, the shaft angle, is what the whole layer exists to explain.

## What this is

Build a model of the wrist angle ψ = θ − φ — the angle between the club shaft and
the lead forearm — through a golf swing. Two layers, kept separate: the
**mechanics** of what the wrist does on its own plane, and the **projection**
that a single face-on camera imposes on it. The previous attempt collapsed the
two and fitted a shape to the shadow.

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
  dynamics, and no model selection was performed;
- and it was fitted to image-plane angles as though they were mechanics, with no
  projection layer at all — see "The hidden state" below, which is the reason
  this brief exists rather than a note saying "try more shapes".

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

## The hidden state: we measure projections, not mechanics

**This is the structural point the previous attempt missed entirely, and it
changes what the model has to be.**

θ and φ are image-plane angles from a single face-on camera. The swing does not
happen in the image plane — it happens on a plane inclined to it, and the club
sweeps around that plane while we watch its shadow. Under orthographic
projection, a vector at in-plane angle α on a plane inclined by τ images at

    tan θ_img = tan α · cos τ

so **uniform rotation in the swing plane images as non-uniform angular motion**,
running fast near one axis of the ellipse and slow near the other. The observed
β(t) is therefore a warp of the true wrist mechanics, and the warp is large: the
production `lenPx` runs 189→414 px within a single swing, a 2.2× foreshortening
range, which is out-of-plane excursion approaching 60°. This is a first-order
effect, not a correction.

Three consequences, each of which undermines the naive model:

- **β_observed conflates wrist mechanics with projection geometry.** Two golfers
  with identical wrists and different swing planes produce different curves, and
  the same golfer's plane is not even constant within a swing — backswing and
  downswing planes differ, which is the classic shift a coach looks for.
- **ψ = θ − φ is not a projection of the anatomical wrist angle.** The club
  sweeps a plane about the grip; the lead arm sweeps a different, shoulder-centred
  plane. The two are warped *differently*, so their image-plane difference is not
  the projection of any single 3-D angle.
- **Some of what we want is invisible.** Forearm roll about the shaft's long axis
  cannot be seen at all — the shaft is axially symmetric, so rolling it does not
  move the line. The out-of-plane component *can* be recovered. Be precise about
  which is which and do not claim the first.

### The hidden state is over-determined, which is what makes it tractable

The saving grace is that the same τ which warps the angle also sets the projected
length:

    L_img = L · √(cos²α + sin²α · cos²τ)

So **foreshortening and angular warp are two readings of one hidden variable**.
That turns the plane from a free parameter into a constrained one, and we already
measure it three independent ways:

| observable | where | what it constrains |
|---|---|---|
| projected shaft length | `lenPx` in production samples (310/745 per swing); `s_px_mm` in the fusion truth (60 band rows/swing) | out-of-plane angle directly |
| clubhead path ellipse | `head_x, head_y` in the fusion truth, and the production head track | plane inclination from the axis ratio |
| arm-vs-club warp difference | φ and θ together | the two planes' relative orientation |

### The discipline this demands

Latent state always improves fit. That is exactly how the previous attempt's
φ-term failed — it absorbed between-swing variation in training and could not
predict out of sample. So the rule here is firm: **the recovered plane must be
checked against the independent observables above, not merely fitted.** A model
whose hidden plane disagrees with the head-path ellipse and the foreshortening is
absorbing something else, and its improved residual is worthless.

### The payoff: the error becomes the measurement

If the model carries an explicit plane, then fitting a *nominal* plane and
looking at what is left over turns the residual into a diagnostic: a structured
departure measures how that golfer's plane differs from the reference. Swing
plane is already one of the headline quantities the shot analyzer wants to
report, and this would derive it from the wrist model's error term rather than
from a separate estimator.

Treat that as a hypothesis with a test attached, not a promise. The test: the
plane implied by the residual must agree with the plane measured from the head
ellipse and from foreshortening, and must be stable within a golfer across
sessions and clubs while moving when the swing genuinely changes. If it agrees,
the model has earned a coaching output. If it does not, the residual is noise
wearing a physical name.

## Task 1 — derive candidate families from the dynamics

Do not pick shapes that look right. Start from the two-link system the whole
programme keeps invoking — lead arm and club, coupled at the wrist — and derive
what ψ(t) *must* look like under different assumptions about the wrist torque.

**Derive the mechanics in 3-D and project them, in that order.** The model has
two layers and they must stay separate: a mechanics layer that says what the
wrist does on its plane, and a projection layer that says what a face-on camera
then sees. Fitting a shape straight to image-plane β — which is what has been
done so far — bakes one golfer's swing plane into what is supposed to be a
statement about wrists. Families worth deriving and comparing, all at the
mechanics layer:

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

The projection layer is common to all of them, so it can be built and validated
**once, before any mechanics family is fitted** — and it should be, because it is
independently testable. Recover the plane from the head-path ellipse and from
foreshortening on each swing, check the two against each other, and only then ask
what mechanics explain the de-projected curve. If the two plane estimates
disagree, stop and find out why: everything downstream inherits it.

## Task 2 — fit, and select on the right criterion

Fit every family on the same samples with the same robust loss, leave-one-swing-
out throughout. Then select on criteria that match the objective:

1. **Out-of-domain prediction.** Fit on the backswing, predict the release. A
   curve-fit will fail this and a physical model should not. This is the sharpest
   available test and the prior work never ran it.
2. **Hidden state agreeing with independent measurement.** The plane the model
   recovers must match the plane from the head-path ellipse and from
   foreshortening. This is the check that separates a physical latent variable
   from a regressor that merely soaks up variance, and it is the difference
   between this attempt and the last one.
3. **Parameter meaning.** Do the parameters move sensibly between sessions? Does
   the release instant track anything else we measure? A parameter that varies
   randomly between swings of the same golfer is a fitting artefact.
4. **Accuracy, as a constraint.** Within reach of the empirical table's
   frame-averaged 17.4° / truth 20.9° — re-baselined per Task 0. Note that the
   empirical table has the projection baked into it, so a mechanics model plus an
   honest projection should be able to *beat* it once the plane is carried
   explicitly; if it cannot, that is informative about which layer the error
   really lives in.
5. **Parameter count**, as a tiebreak only.

## Task 3 — write it up

Extend `docs/research/wrist_cock_model.md` (or supersede it) with the derivation,
the families that lost and why, the falsification tests, and the parameters with
their physical reading — mechanics and projection reported separately, so a
reader can see which layer each number belongs to. If the residual-as-plane-
diagnostic hypothesis survives its test, it is a result in its own right and
belongs in the programme report as well. The negative results are the valuable part; the existing
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
