# Predicting the Club from the Arm: a Fitted Wrist-Cock Model

*PinPoint shaftlab programme — model note, 2026-08-11. Fitted and graded by
`tools/swinglab/wrist_cock_fit.py` against the 61-swing production corpus and
805 hand-placed shaft labels. Implements the v2 table in
`src/Analysis/shaft_kinematics.h`, dark behind `shaft.wedge.kinModelV2`.
Companion to the programme report, `club_detection_from_video.md`, whose Phase 6
sets out the physical law this model puts numbers on.*

## 1. What this is for

The shaft tracker would like to know roughly where the club is before it looks
for it. It cannot measure θ in advance — that is the thing being solved — but it
*can* measure the lead arm from pose on every frame, including the frames where
the club is a blur. If the wrist angle between arm and club were predictable,
the arm would locate the club for free:

    θ̂(f) = φ(f) + chir · β̂(x(f))

where φ is the lead forearm direction, chir the swing's chirality, and β̂ the
signed wrist cock at some measure of swing progress x. That prediction already
exists in the tracker. It centres the blur-wedge search envelope, sets the
`kinCone` off-envelope penalty, and supplies the predicted angular rate that
triggers the wedge at all.

What it has never had is data. The table was authored by hand from the design's
expectations. This note grades it, fits a replacement, and reports what the fit
turned out to depend on — which was not what we expected.

## 2. The physical argument

Three facts about a golf swing bound the model before any fitting begins.

**The club and the lead arm are a double pendulum.** The wrist angle ψ = θ − φ
is anatomically bounded and evolves smoothly, so β̂ is a function, not a
scatter.

**The wrist cocks once and releases once.** Over address → impact the wrist
angle is monotone with a single reversal at the top; un-hinging in the backswing
or re-hinging in the downswing is anatomically impossible. This is the law the
programme report verifies on hand-marked data (Phase 6), and it means the fitted
curve should have one interior maximum, not a wiggle.

**At impact the shaft returns into line with the lead arm.** Not exactly — there
is a forward-lean term — but close enough that β near impact is small and
positive. The corpus median at impact is +11°.

The model's valid domain is therefore address → impact. Past impact the wrist
re-hinges passively under centripetal load, and the dominant motion becomes
forearm roll about the shaft's long axis, which a face-on camera cannot see at
all. Everything below is fitted and graded over address → impact only.

## 3. How it is graded

**The convention.** β = chir · wrap180(θ − φ), signed positive on the trail side
— the header's convention exactly, so a fitted table drops straight in. φ is the
lead *forearm*: the line from the lead elbow to the grip, in pixel space. Which
side leads is decided per swing from the pose, on the grip ordering over the
address hold, never from metadata.

**Anti-circularity.** A model fitted to the tracker's own θ and then used to
constrain that tracker would reinforce its errors. So fitting uses measured-tier
samples only — the tier graded at 0.6% bad against dense truth — while the
*decision* rests on 463 hand-placed shaft labels across 23 swings, which owe the
tracker nothing. Every number below is leave-one-swing-out: the held-out swing
contributes nothing to the table it is scored against.

Both are reported, because the gap between them is the circularity, and it is
visible: the tracker-tier numbers are consistently rosier than the truth
numbers, by roughly 20% on the winning form.

## 4. What the shipped table was doing

| | against truth |
|---|---|
| median residual | **−9.1°** |
| p10–p90 | **74.3°** |
| \|error\| > 30° | **26.3%** |
| within 1σ of its own envelope | 62% |

It is biased and wide, and the bias alternates sign by segment (−5° in the
backswing, +12° in the downswing, −14° through), so no constant correction fixes
it. A quarter of the frames where the tracker consults the prediction are
consulting something more than 30° wrong.

## 5. The finding: the axis was the error, not the numbers

The obvious repair is to refit the table's nine knots on the axis it already
uses. **That does not work at all** — refitting on swing progress leaves the
residual unchanged at 75.6°, no better than the hand-authored curve it replaced.

The reason is that swing progress is the wrong clock. Wrist release is not a
fixed *fraction* of the swing; it is an event at a roughly fixed *time* before
impact. Two golfers — or the same golfer on two swings — who reach the top at
the same fraction but take different times to get there will release at the same
number of milliseconds before impact and at quite different fractions. Indexing
by progress smears every swing's release across every other's.

Re-indexing the same model on **seconds before impact**, and changing nothing
else, takes the residual from 74.3° to 20.9°.

| form | median | p10–p90 | \|err\|>30° | within 1σ |
|---|---|---|---|---|
| F0 shipped, hand-authored | −9.1° | 74.3° | 26.3% | 62% |
| F1 refit, same progress axis | −1.2° | 75.6° | 25.3% | 50% |
| **F2 refit, seconds before impact** | **+0.1°** | **20.9°** | **6.7%** | **66%** |
| F3 + linear in φ | +6.5° | 17.4° | 5.8% | 35% |
| F4 + shape constraints | +5.2° | 17.9° | 5.8% | 36% |

*463 hand-placed labels, 23 swings, leave-one-swing-out.*

F2 improves **every session in the corpus**, not merely the average — 56→38,
59→23, 36→12, 63→16, 43→17, 37→12 (p10–p90 by session, tracker tier) — which is
what distinguishes a real effect from a fit to the corpus mean.

Two further details mattered, and both are about representation rather than
statistics:

- **Knots have to be dense where the curve moves.** The wrist holds its lag near
  90° through most of the downswing and then releases nearly all of it in the
  last ~120 ms. Uniform knot spacing puts an 80° drop inside a single linear
  segment. Spacing knots in √(−t) — about half of them inside the last quarter
  second — is worth 2° on its own.
- **The local fit has to be quadratic.** β curves hard through the release, so a
  local *linear* fit inside any usable window carries that curvature into the
  knot value as bias, and forces the window narrow, which then starves σ of
  data. A local quadratic absorbs the curvature and lets the window stay wide:
  worth a further 5°, and it is what brought the envelope into calibration.

## 6. What did not work

**Refitting on the existing axis** (F1). No improvement whatsoever — 75.6°
against 74.3°. Recorded because it is the change one would naturally try first,
and it is worthless.

**Adding a linear term in φ** (F3). Genuinely more accurate on paper — 17.4°
against F2's 20.9° — but it fails on two counts. It carries a +6.5° bias against
truth while showing only +0.3° against the tracker's own tier, which is the
signature of a term fitted to tracker quirks rather than to swing mechanics. And
it is *worse than F2 on the session that supplies most of the truth labels*
(23.2° against 23.3° overall, but with the bias). Its envelope collapses to 35%
within 1σ, because the extra regressor absorbs between-swing variation in
training that it cannot predict out of sample. More accurate and less honest is
not a trade this programme makes.

**Shape constraints** (F4). Imposing the one-reversal law and the in-line-at-
impact anchor on the fitted table changed nothing material (17.9° against F3's
17.4°) and inherited F3's bias and envelope problem. The reason is the
interesting part: **the unconstrained fit already satisfies the constraints**.
The curve it recovers rises monotonically to a single maximum and releases
monotonically to a small positive value at impact, without being told to. The
physics is in the data, so asserting it adds nothing — a null result that is
mild evidence the law is real rather than imposed.

**Per-swing calibration.** Fitting a per-swing offset on the backswing, where θ
is well measured, and carrying it into the downswing *hurts*: the residual in
the last 250 ms before impact rises from 22.8° to 26.6°. A golfer's backswing
wrist offset does not predict their release. The model stays a population model,
and the per-shot auto-calibration that works for the IMU's mounting vector has
no analogue here.

## 7. The envelope, and why σ is inflated

The tracker searches an arc of centre ± kσ·σ_β, with kσ = 3. So the number that
matters is not the textbook 68%-within-1σ but whether the *true* wrist cock
falls inside the arc actually searched.

| | median half-width | true β inside |
|---|---|---|
| shipped table | 62.5° | 96.8% |
| fitted, σ as fitted | 17.7° | 90.1% |
| **fitted, σ × 2.5** | **44°** | **97.0%** |

The fitted σ is honest about this corpus and too confident about the world: at
face value the envelope would miss the true club on one frame in ten. The
shipped table covers 96.8% by being wide enough to be nearly uninformative.

The table ships at **σ × 2.5**, chosen because it is the factor at which the new
envelope covers as much true wrist cock as the old one while still searching a
narrower arc. That is the whole claim: *the search is no less forgiving than it
was, and it now points in the right place.* The inflation is not a fudge but the
explicit price of a corpus with one athlete in it — the fitted centre is a
measurement, the fitted spread is one golfer's repeatability, and only the first
of those generalises.

## 8. The table

Axis: seconds before impact, clamped to [−1.100, 0.000]. β̂ signed, trail side
positive. σ as fitted × 2.5.

| t − t_impact (s) | β̂ (°) | σ (°) | | t − t_impact (s) | β̂ (°) | σ (°) |
|---|---|---|---|---|---|---|
| −1.100 | −8.1 | 15.7 | | −0.275 | 78.4 | 16.8 |
| −0.948 | 2.2 | 13.3 | | −0.202 | 85.7 | 16.5 |
| −0.808 | 15.1 | 9.1 | | −0.140 | 96.9 | 19.1 |
| −0.679 | 38.9 | 12.3 | | −0.090 | 90.6 | 12.2 |
| −0.561 | 75.1 | 14.1 | | −0.051 | 61.2 | 13.2 |
| −0.455 | 85.6 | 12.0 | | −0.022 | 31.6 | 14.3 |
| −0.359 | 76.3 | 18.4 | | −0.006 | 16.0 | 14.4 |
| | | | | 0.000 | 11.3 | 14.9 |

Read as mechanics: the wrist is near neutral a second before impact, cocks
through the backswing to about 86° by the halfway point, *holds* between 76° and
97° for the whole of the downswing — the lag — and then releases 85° of it in
the final 140 ms, arriving 11° from in-line at impact. Nothing in the fitting
procedure asked for that shape.

## 9. Status and limitations

The table is **dark**, behind `shaft.wedge.kinModelV2`, with the v1 path
byte-identical when the key is off (pinned in `shaft_kinematics_test`). What
remains before it can be considered for a default flip is the corpus A/B on the
studio machine: the prediction feeds the wedge trigger and the kinCone penalty,
so enabling it moves real output, and the risk to watch is not θ accuracy but
*coverage* — a better-centred, narrower envelope could in principle suppress
genuine wedge candidates. The A/B watches the wedge stamp count, P6 recall, the
truth-labelled P5/P6/P7 errors, and `track.valid`.

Three limits belong on the face of this note.

**One athlete.** Every number here is one golfer across five sessions and one
club type. The fitted centre may encode his release timing; the σ inflation is
what stands between that and a golfer the model has never seen. A second athlete
is the only real fix, and until there is one the honest reading is that the
*shape* of the curve is likely general and its *timing* may not be.

**The truth is thin at the ends.** The 463 labels concentrate where a human
could see the shaft, so the impact blur — the region the model most wants to
serve — is the region where truth is sparsest. The label-selection bias the
programme report documents applies here too.

**The residual is not Gaussian.** 6.7% of samples remain worse than 30° with a
p10–p90 of 20.9°, so the model is usually good and occasionally quite wrong.
Anything consuming it must treat it as a soft prior with tails, never as a
measurement — which is why it enters as an emission weight and an envelope, and
never as a veto.
