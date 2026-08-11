# Predicting the Club from the Arm: the Wrist-Cock Curve, its Parametric Reading, and the Swing Plane Behind It

*PinPoint shaftlab programme — model note, 2026-08-11. Fitted and graded by
`tools/swinglab/wrist_cock_fit.py`, with the instrumented truth join in
`tools/swinglab/fusion_truth.py` and the projection layer in
`tools/shaftlab/plane_probe.py`. Implements the v2 table in
`src/Analysis/shaft_kinematics.h`, dark behind `shaft.wedge.kinModelV2`.
Supporting data in [`data/wrist_cock_model/`](data/wrist_cock_model/); every
number reproduces from the commands in §14. Companion to the programme report,
`club_detection_from_video.md`, whose Phase 6 sets out the physical law this
model puts numbers on.*

---

## 1. What this is for

The shaft tracker would like to know roughly where the club is before it looks
for it. It cannot measure θ in advance — that is the thing being solved — but it
*can* measure the lead arm from pose on every frame, including the frames where
the club is a blur. If the wrist angle between arm and club is predictable, the
arm locates the club for free:

    θ̂(f) = φ(f) + chir · β̂(x(f))

That prediction centres the blur-wedge search envelope, sets the `kinCone`
off-envelope penalty, and supplies the predicted angular rate that triggers the
wedge at all.

This note defines the model, fits it, grades it against instrumented truth, and
then goes one layer deeper — to the swing plane that the whole image-plane
picture is a shadow of.

---

## 2. The variables, and how they compose

All three angles are **image-plane angles in pixel coordinates**, measured with
`atan2(dy, dx)` in a y-down image, so they increase clockwise as drawn on screen.

### φ (phi) — the arm angle. *Measured.*

The direction of the **lead forearm**: elbow → grip.

- Source: the pose estimator (ViTPose, 133-point COCO-WholeBody). The elbow is
  keypoint 7 (left) or 8 (right); the grip is the midpoint of the two hand
  centroids. Which side leads is decided per swing from the pose, on the grip
  ordering over the address hold — never from metadata.
- Physically: where the lead arm points. **An input, never predicted.**
- Quality: **1.79°** self-jitter in the downswing, measured about a local
  quadratic — well inside the ~20° residual the model is explaining, so the model
  is not φ-limited.

### θ (theta) — the club angle. **This is what the model predicts.**

The direction of the **club shaft**: grip → clubhead. Physically, where the club
points; it sweeps well over 180° between address and impact. The tracker measures
θ when it can see the shaft and must predict it when it cannot — through impact
blur, when the shaft crosses the body, when it leaves frame. **θ is the prize.**

### ψ (psi) — the wrist angle. *Derived.*

    ψ = θ − φ

The angle between club and forearm: the wrist hinge as the camera sees it. Near
zero at address, near or beyond 90° at the top, unhinging rapidly through the
release.

### β (beta) — ψ made comparable between golfers

    β = chir · wrap180(θ − φ) = chir · ψ

where `wrap180` folds into (−180°, +180°] and **chir** is the swing's chirality:

```
chir = +1  if φ(top) − φ(takeaway) ≥ 0,  else −1
```

A right- and a left-handed golfer produce mirror-image swings; without `chir`, ψ
has opposite signs for the two and no single table serves both. With it, **β > 0
always means the club trails the arm** — lag. `chir` is computed per swing from
the pose, because metadata is often wrong and the pose cannot be.

### The composition, and the direction it runs

    θ̂ = φ + chir · β̂

(`phiClubFromBetaDeg`, `shaft_kinematics.h:199`, wrapped into [0°, 360°)). Read
as a sentence: *take where the arm points, and swing the club off it by the
predicted lag, on the side the golfer's handedness puts it.* The sign of β̂ also
decides which side of the arm the club sits on — trail side while β̂ > 0 through
the downswing, lead side once released past the arm.

```
pose  ──►  φ  (measured, per frame)
                 ╲
                  ├──►  θ̂ = φ + chir·β̂   ──►  where to search for the shaft
model ──►  β̂  ──╱
           (a function of the clock alone)
```

**The model never sees θ when predicting it.** β̂ depends only on time. That is
deliberate: a model fitted to the tracker's own θ and then used to constrain the
tracker would reinforce its own errors.

### σ_β — the companion

Alongside β̂ the model carries **σ_β(t)**, the spread of β at that instant. The
tracker searches an arc `β̂ ± k_σ·σ_β` with `k_σ = 3`, so σ sets how wide it
looks. It is fitted, then inflated (§8).

---

## 3. The physical argument

Three facts bound the model before any fitting begins.

**The club and the lead arm are a double pendulum.** ψ is anatomically bounded
and evolves smoothly, so β̂ is a function, not a scatter.

**The wrist cocks once and releases once.** Over address → impact the wrist angle
is monotone with a single reversal at the top; un-hinging in the backswing or
re-hinging in the downswing is anatomically impossible. The fitted curve should
have one interior maximum, not a wiggle.

**At impact the shaft returns into line with the lead arm.** Not exactly — there
is a forward-lean term — but close enough that β near impact is small and
positive. The corpus median at impact is +11°.

The model's valid domain is therefore **address → impact**. Past impact the wrist
re-hinges passively under centripetal load and the dominant motion becomes
forearm roll about the shaft's long axis, which a face-on camera cannot see at
all. Everything below is fitted and graded over address → impact only.

---

## 4. The clock: seconds before impact

    x = (t − t_impact) / 10⁶     seconds, negative through the swing, 0 at impact

The floor each candidate clock can support — the spread of β about its own
conditional median on that axis, leave-one-swing-out — settles the choice before
any curve is fitted. No model on an axis can beat its floor.

| axis | frame-averaged | at the instants truth labels sit |
|---|---|---|
| swing progress (bs0 / top / impact anchored) | 30.4° | **67.3°** |
| address → impact, linear | 36.8° | — |
| top → impact, linear | 25.6° | — |
| **seconds before impact** | **16.1°** | **15.4°** |

Seconds-before-impact wins on both readings, and the reason is physical: **wrist
release is not a fixed fraction of the swing but an event at a roughly fixed time
before impact.** Two swings reaching the top at the same fraction but taking
different times release at the same number of milliseconds before impact and at
quite different fractions, so a progress index smears every swing's release
across every other's.

The two columns also differ in a way that matters for how anything here is read.
The progress axis is twice as bad at truth-label instants as frame-averaged; the
time axis is not. Truth labels cluster at s ≈ 0.30–0.57 — the transition, where a
human can see the shaft — and `swingProgress` is anchored on the **top**, the
noisiest event in the ladder, so small top-placement errors blow β up precisely
where labels are densest. **A comparison scored only where truth exists is a
statement about the labels as much as the model.** Frame-averaged and truth
columns are therefore reported side by side throughout, and the report generator
refuses to emit one without the other.

Two representation details also mattered:

- **Knots must be dense where the curve moves.** The wrist holds near 90° through
  most of the downswing then releases nearly all of it in ~120 ms. Uniform
  spacing puts an 80° drop inside one linear segment. Spacing knots in √(−t) is
  worth 2°.
- **The local fit must be quadratic.** β curves hard through the release, so a
  local *linear* fit carries curvature into the knot value as bias and forces the
  window narrow, which starves σ. A local quadratic is worth a further 5°, and it
  is what brought the envelope into calibration.

---

## 5. How it is graded

**Anti-circularity.** Fitting uses measured-tier samples only — the tier graded
at 0.6% bad against dense truth. The decision rests on labels that owe the
tracker nothing. Every number is leave-one-swing-out.

**Name the run root.** Numbers downstream of a run root are not portable between
roots, so every figure here is quoted against `stagegate/corpm3-off` (59/61
swings usable) and any comparison must name its own. Some roots are unusable
rather than merely different: `stagegate/final1` carries no `club` block, and
`corpoff-live`, `off1`, `off2`, `on1` are missing the takeaway phase and lose all
61 swings.

### Label provenance

The 2026-07-05 session carries instrumented stripe-fusion truth, and the
provenance of those labels has to be stated because it changes what a comparison
means: `truth.json` for that session **is the fusion band tier verbatim** — 9 of
10 swings match row-for-row, head positions to 0.000 px, θ to a pooled median
0.0063°. Of the corpus labels on those swings, **565 are instrumented band rows
and 61 are genuinely hand-placed.** Grading "hand labels" against "fusion band"
without splitting them compares a set with itself.

What is genuinely new is the **ray tier**: 468 rows, 283 of them pre-impact — the
fast frames a human cannot label at all.

| form | track (frame-avg) | truth_hand | truth_band | fuse_band | fuse_ray |
|---|---|---|---|---|---|
| F0 shipped | 78.2° (9154) | 59.5° (44) | 36.7° (307) | 37.0° (307) | 50.9° (283) |
| F1 progress axis | 72.7° | 46.4° | 52.1° | 52.2° | 47.3° |
| **F2 seconds-before-impact** | **40.0°** | **15.3°** | **12.1°** | **12.2°** | **20.5°** |
| F3 + linear in φ | 39.1° | 21.3° | 16.1° | 16.1° | 23.9° |
| F4 + shape constraints | 39.6° | 23.1° | 17.1° | 17.1° | 22.7° |
| F5 parametric (7 params) | 40.2° | 22.8° | 16.8° | 16.8° | 24.7° |

`truth_band` and `fuse_band` are the same observations routed through two
different loaders — truth.json in radians via the corpus path, the fusion CSV in
degrees via the frame index. They agree to ≤0.1° on every form at identical
n=307, which verifies the timebase join exactly rather than approximately.

**The model is 1.7× worse in the fastest frames** (20.5° ray against 12.2° band).
Ray-tier label noise is ~1.7°, negligible against a 20° residual, so that gap is
a statement about the model, in precisely the region it most needs to serve.

**The session leak.** Leave-one-*session*-out withholds all ten swings at once:

| form | fuse_band swing-out → session-out | leak |
|---|---|---|
| F2 | 12.2° → 12.9° | **0.7°** |
| F3 | 16.1° → 21.0° | **4.9°** |

F2 barely notices; the φ-term leaks seven times as much (§10).

### The φ source

Two φ channels exist: the production pose, and the lab's `anchors.csv` from an
earlier pose run. They disagree substantially, and a disagreement alone cannot
say which is wrong — each channel's own jitter does, measured about a local
quadratic on its own samples:

| φ source | backswing | downswing |
|---|---|---|
| production pose (raw, unsmoothed) | 3.13° | **1.79°** |
| `anchors.csv` | 0.12° | 3.51° |

`anchors.csv` reading 0.12° in the backswing is too smooth to be a measurement —
the signature of interpolation between sparser samples — and where the swing is
fast it cannot hide, degrading to 3.51°. **Production φ is the source
(`--truth-phi harness`)**, and at 1.79° jitter the model's residual is not
φ-limited.

---

## 6. The shipped table's performance

| | against truth |
|---|---|
| median residual | **−12.2°** |
| p10–p90 | **77.6°** |
| \|error\| > 30° | **37.4%** |
| within 1σ of its own envelope | 50% |

It is biased and wide, and the bias alternates sign by segment, so no constant
correction fixes it. More than a third of the frames where the tracker consults
the prediction are consulting something more than 30° wrong.

Fitting the curve and changing the clock together take that to **26.8°** (F2).
Split between the two: fitting is worth about 22°, changing the clock about
another 12°.

---

## 7. The parametric model

Everything above fits a **lookup table**: fifteen knots and linear interpolation.
That is an empirical curve, not a model — as many free numbers as knots, none
meaning anything alone, free to wiggle wherever data is thin.

The physics says the curve has a shape — a wrist that cocks once, holds, and
releases — and two logistic transitions express exactly that:

    β(t) = b₀ + A · L((t − t_c)/w_c) · (1 − r · L((t − t_r)/w_r))

with `L(z) = 1/(1 + e^(−z))`. Read the structure before the parameters: it is a
**product of two events** — the first logistic turns the lag on (cocking), the
second takes it away (the release), and `b₀` is where it starts.

Fitted under a robust loss, leave-one-swing-out:

| symbol | name | value | fold spread | what it *is* |
|---|---|---|---|---|
| `b₀` | address offset | **−10.4°** | ±0.6 | the wrist angle at address — a small forward press |
| `A` | peak lag amplitude | **98.7°** | ±0.9 | total lag built; the full extent of the hinge |
| `t_c` | cock centre | **−0.702 s** | ±0.002 | *when* the wrist cocks — during the backswing |
| `w_c` | cock width | **0.089 s** | ±0.001 | *how fast* — cocking takes ≈4·w_c ≈ 0.36 s |
| **`t_r`** | **release centre** | **−0.032 s** | ±0.001 | **when the release fires — 32 ms before impact** |
| **`w_r`** | **release width** | **0.011 s** | ±0.001 | *how fast* — **≈8× faster than the cock** |
| `r` | release completeness | **0.741** | ±0.017 | how much lag is spent; ~¼ still held at impact |

The stability is the striking part: drop any swing from the fit and the release
timing moves by less than a millisecond.

### What a coach would read off this

- **`A` — how much lag you create.**
- **`t_r` — when you release it.** Later (closer to 0) means holding lag longer.
- **`w_r` — how violently.** Small means a snappier, later hit.
- **`r` — whether you're still holding at impact.** Below 1 means shaft lean,
  hands ahead of the ball; above would mean a flipped, scooped release.

That is the point of the exercise: the table fits better but says nothing, while
these seven numbers can be compared between swings, sessions and golfers.

### But the table is more accurate, and we ship the table

| | median | p10–p90 | \|err\|>30° | 3σ half-width | envelope covers |
|---|---|---|---|---|---|
| empirical table (15 knots) | −2.5° | **26.8°** | 9.6% | 25.0° | 89.8% |
| parametric (7 params) | −3.3° | 38.2° | 10.0% | 25.2° | 89.8% |
| shipped, hand-authored | −12.2° | 77.6° | 37.4% | 62.5° | 93.8% |

The parametric form beats the shipped table better than two to one but gives up
~11° to the lookup table. The reason is visible in Figure 1: the real curve does
not hold a flat plateau. It dips around 0.36 s before impact and rises to a
distinct peak at 0.14 s, and a product of two logistics cannot express that
shape. Whether that structure is real mechanics — a re-cock as the arm changes
direction at transition — or one golfer's artefact is not settled by this data,
and it is the first thing a second athlete would tell us.

**One form is not a modelling exercise, and this section is not one.** The
logistic product was chosen because it resembles the empirical curve. It fits
because the curve is sigmoid-ish, and would have fitted about as well had the
mechanism been something else. No alternative families were fitted, nothing was
derived from the two-link dynamics this note keeps invoking, and no model
selection was performed. A proper pass would derive a family from the double
pendulum with a wrist torque — where the release is largely passive once the arm
decelerates, which *predicts* a functional form rather than borrowing one — fit
several families, and judge them on out-of-domain prediction (fit the backswing,
predict the release) rather than in-domain residual. Until then the seven
parameters are a compact description, not physics.

So: **the shipped artefact is an empirical curve, and the parametric fit is what
it means.** Both come from the same harness, are reported together, and neither
is dressed as the other.

![The wrist-cock curve: the observed cloud, the fitted knots, the empirical curve and the parametric fit.](figures/wrist_cock_model.png)

***Figure 1.** Left: the signed wrist cock against seconds before impact. Grey is
the observed cloud. The **dashed blue** curve is the shipped hand-authored table,
mapped onto this axis at corpus-median tempo. The **orange** curve with markers is
the fitted 15-knot table, markers being the knots, shaded ±1σ as fitted. The
**green** curve is the 7-parameter fit. Right: residuals against the hand-placed
labels, leave-one-swing-out.*

---

## 8. The envelope, and why σ is inflated

The tracker searches centre ± k_σ·σ_β with k_σ = 3, so the number that matters is
not the textbook 68%-within-1σ but whether the *true* wrist cock falls inside the
arc actually searched.

| | median half-width | true β inside |
|---|---|---|
| shipped table | 62.5° | 93.8% |
| fitted, σ as fitted | 25.0° | 89.8% |
| **fitted, σ × 2.5** | **62.5°** | **96.4%** |

The fitted σ is honest about this corpus and too confident about the world: at
face value the envelope would miss the true club on one frame in ten. The shipped
table achieves its coverage by being wide enough to be nearly uninformative.

The table ships at **σ × 2.5**, the factor at which the new envelope covers *more*
true wrist cock than the old one at the same search width. That is the whole
claim: *the search is no less forgiving than it was, and it now points in the
right place.* The inflation is the explicit price of a corpus with one athlete in
it — the fitted centre is a measurement, the fitted spread is one golfer's
repeatability, and only the first generalises.

---

## 9. The projection layer: the swing plane behind the shadow

Everything above lives in the image plane, and the model does not yet account for
that.

**The swing does not happen in the image plane.** It happens on a plane inclined
to it, and the camera sees a flattened shadow. Under projection, a vector at
in-plane angle α on a plane inclined by ι images at

    tan θ_img = tan α · cos ι

so **uniform rotation on the swing plane images as non-uniform angular motion**.
The β we fit is a *warp* of the true wrist mechanics, and the warp is large: the
shaft's projected length varies by 1.58–2.08× within a swing, an out-of-plane
excursion of 51–61°. A first-order effect, not a correction.

Three consequences:

1. **β conflates wrist mechanics with camera geometry.** Two golfers with
   identical wrists and different planes produce different curves.
2. **ψ = θ − φ is not the projection of any single 3-D angle.** The club sweeps a
   plane about the grip; the lead arm sweeps a different, shoulder-centred plane.
   They warp differently, so their image-plane difference is not the shadow of
   one anatomical angle.
3. **Some of it is invisible.** Forearm roll about the shaft's long axis cannot be
   seen — a symmetric line does not change when rolled. The out-of-plane
   component *can* be recovered. Be precise about which is which.

### Recovering the plane

Take the **shaft vector**, head − grip, in image pixels. The club rotates about
the grip, so this vector sweeps a circle of fixed radius on the swing plane, and
a circle on a plane inclined at ι images as an **ellipse of axis ratio cos ι**:

    ι = arccos(minor axis / major axis)

by direct conic fit. **This uses no foreshortening model and no per-frame length —
only the shape traced.** And a plane fixes which side of the node line each
direction sits on, so there is no per-frame sign ambiguity to resolve.

Two requirements on the fit, both load-bearing:

- **Fit `head − grip`, not the absolute head path.** The absolute path is grip
  translation *plus* club rotation, so it is not a planar closed curve about a
  fixed centre and a conic fitted to it is badly conditioned — split-half
  repeatability 9.2° in the downswing against 0.6° for the shaft vector.
- **Normalise isotropically.** Scaling x and y by their separate standard
  deviations is an anisotropic map that changes both the axis ratio and the
  orientation — the two quantities being measured.

### What it measures

| | inclination ι | across ten swings | split-half repeatability |
|---|---|---|---|
| backswing | **40.0°** median | 25.4–60.5° | **0.7°** (max 3.9°) |
| downswing | **20.0°** median | 13.7–38.1° | **0.6°** (max 9.2°) |
| **delta (back − down)** | **+17.3°** median | −7.6° … +45.6° | 8/10 steepen |

Larger ι means the plane is tilted further from the camera — a **flatter**, more
around-the-body swing. Smaller ι means it sits closer to the image plane — more
**upright and steep**. So a **positive delta means the club steepens between
backswing and downswing**, and a negative delta means it shallows.

### Precision versus accuracy — the distinction that makes this usable

- **Precision is excellent.** Split-half repeatability — fit the odd frames, fit
  the even frames, compare — is **0.6–0.7° in both phases**. Against a 17° delta
  and a 53° between-swing range, the signal is far above the noise floor.
- **Absolute accuracy is unconfirmed.** The independent foreshortening estimate
  agrees to 7.8° median in the backswing (8/10 within 10°) but 14.9° in the
  downswing (4/10), and reads systematically *higher* — consistent with the known
  clubhead-detector under-runs and with perspective inflation. A single ι should
  not yet be quoted as a plane angle.

**This is why the delta is the right quantity.** It is a difference between two
measurements made the same way on the same swing, so common calibration bias
cancels. The over-the-top diagnostic needs the delta, not the absolute plane, and
the delta is the part that is already solid.

### The independent checks

- **Club length closes.** `lenPx / s_px_mm + r0_mm` must equal the catalogue
  940 mm, and does: pooled median **946 mm** (0.6% off), p10–p90 828–987. This is
  the only test here that checks the *data* rather than a model. But n=36 is the
  entire overlap — the instrumented tier covers the frames the tape is legible
  in, while `lenPx` lives in the slow phases.
- **The single-fixed-plane control is unavailable, not passed.** A single plane
  should fail, reproducing `length_model`'s documented ~2× symmetry violation. It
  does not (5.0% median relative error against a per-phase model's 4.4%) — but
  only because the instrumented tier lacks the antiparallel shaft directions that
  produced the original violation. This channel can neither acquit nor convict it.
- **The orthographic assumption is bounded, not corrected.** `rho_plane` assumes
  orthographic projection while `s_px_mm` and `lenPx` both fold in the pinhole
  depth scale, identically. Shoulder width bounds it: its excursion implies up to
  **64°** of apparent out-of-plane angle that is really body depth. Reported as a
  bias bar; nothing is subtracted.

### The payoff, as a hypothesis with a test attached

If the model carries an explicit plane, fitting a *nominal* plane and looking at
what is left over turns the residual into a diagnostic: a structured departure
measures how that golfer's plane differs from the reference. Swing plane is
already a headline quantity the shot analyzer wants to report, and this would
derive it from the wrist model's error term rather than a separate estimator.

The test: the plane implied by the residual must agree with the plane measured
here, and must be stable within a golfer across sessions and clubs while moving
when the swing genuinely changes. If it agrees, the model has earned a coaching
output. If not, the residual is noise wearing a physical name.

**Open before the delta becomes a coaching output:**

- **The sign convention needs validating.** The geometry says larger ι = flatter,
  but that mapping has not been confirmed against a known-plane swing or a second
  camera. One down-the-line cross-check settles it.
- **The axis ratio alone cannot tell which way the plane leans** — two planes
  tilted oppositely share a ratio. The **node line** (the ellipse's major-axis
  direction, recorded as `node_*_deg`) carries that and has not been analysed.
- **One golfer, one club, one session.** The 53° between-swing range in the delta
  is either real variability or an estimator fault, and ten swings cannot
  separate those.

---

## 10. What did not work

**Adding a linear term in φ** (F3). More accurate against hand labels than F2,
but it fails on three counts. It carries a +6.5° bias against truth while showing
only +0.3° against the tracker's own tier, the signature of a term fitted to
tracker quirks rather than mechanics. Its envelope collapses to 35% within 1σ.
And on instrumented truth it is simply worse than F2 — 16.1° against 12.1° band,
23.9° against 20.5° ray — while leaking 4.9° under session holdout against F2's
0.7°, which is the extra regressor absorbing between-swing variation it cannot
predict out of sample. Against hand labels it looked *better*; the label
distribution was flattering it. More accurate and less honest is not a trade this
programme makes.

**Shape constraints** (F4). Imposing the one-reversal law and the in-line-at-
impact anchor changed nothing material and inherited F3's problems. The reason is
the interesting part: **the unconstrained fit already satisfies the
constraints**, rising monotonically to a single maximum and releasing
monotonically to a small positive value at impact without being told to. The
physics is in the data — a null result that is mild evidence the law is real
rather than imposed.

**Per-swing calibration.** Fitting a per-swing offset on the backswing, where θ is
well measured, and carrying it into the downswing *hurts*: residual in the last
250 ms rises from 22.8° to 26.6°. A golfer's backswing wrist offset does not
predict their release. The model stays a population model, and the per-shot
auto-calibration that works for the IMU's mounting vector has no analogue here.

**The instrumented truth upgrade, as conceived.** Two-thirds of the "new, unused"
instrumented truth was already being graded on, copied into `truth.json` without
provenance. The real gain is the ray tier and tier separation, not volume.

**`anchors.csv` as a φ source.** Interpolated, artificially smooth where the
swing is slow, and worse than the production pose where it is fast.

**The clubhead-path ellipse as a check on foreshortening.** The two routes to the
plane are correlated but not interchangeable: foreshortening reads systematically
high and disagrees by 14.9° median in the downswing. The ellipse is the
estimator; foreshortening is weak corroboration at best.

---

## 11. The table

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
97° for the whole of the downswing — the lag — and then releases 85° of it in the
final 140 ms, arriving 11° from in-line at impact. Nothing in the fitting
procedure asked for that shape.

---

## 12. Status and limitations

The table is **dark**, behind `shaft.wedge.kinModelV2`, and on the evidence below
**it stays dark**. The key-off path is byte-identical (pinned in
`shaft_kinematics_test`, confirmed 61/61 on the corpus); the key-on path
regresses the tracker, and a more accurate model that makes the tracker worse is
not a model the tracker should use.

### The corpus A/B, and why it failed

Three runs on the studio machine, pose-pinned, 61 swings.

| | key off | key on | |
|---|---|---|---|
| byte-identity vs baseline | **61/61** | — | the refactor is clean |
| P5 emitted | 59/61 | 49/61 | **−10** |
| P6 emitted | 59/61 | 51/61 | **−8** |
| P8 emitted | 61/61 | 46/61 | **−15** |
| `track.valid` | 61 | 60 | −1 |
| WEDGE stamps | 581 | 360 | **−38%** |
| measured samples | 11,838 | 11,090 | −748 |
| mean coverage | 0.911 | 0.840 | **−0.071** |

**The mechanism, and it is not a bug.** The table has two consumers with
incompatible needs. As a *centre* it wants accuracy, and v2 is three times more
accurate. But the wedge *trigger* fires on predicted angular rate — the time
derivative of φ + chir·β — and there the shape matters more than the value.

The hand-authored curve declines steadily from its peak to the finish, so dβ/dt
contributes rate across the whole downswing. The fitted curve holds its lag
nearly flat and then dumps 85° in the last 140 ms — which is what the wrist
actually does — so it contributes almost no rate until the release. Frames
clearing the 720°/s trigger fall from **33.9% to 23.6%**, matching the 38% drop
in wedge stamps.

So **the fitted table starves the trigger by being right.** The threshold was
calibrated against a curve that was wrong in a way that happened to help.

**What would unblock it** — none attempted, each needing its own gate: the
trigger could read the arm's own rate rather than the prediction's, which is
arguably what it wanted all along; or `omegaMinDegS` could be recalibrated, which
re-opens a tuned constant; or the roles could be split, centre and envelope from
v2 while the trigger keeps v1. The last is least invasive and most honest about
what was measured: the *centre* improved and the *rate* did not.

### Limits on the face of this note

**One athlete.** Every number is one golfer, five sessions, one club type. The
fitted centre may encode his release timing; σ inflation is what stands between
that and an unseen golfer. The honest reading is that the *shape* of the curve is
likely general and its *timing* may not be.

**Truth is thin at the ends.** Hand labels concentrate where a human could see
the shaft, so the impact blur — the region the model most wants to serve — is
where truth is sparsest. The instrumented ray tier partly fixes this, and shows
the model is 1.7× worse there.

**The residual is not Gaussian.** Around 10% of samples remain worse than 30°, so
the model is usually good and occasionally quite wrong. Anything consuming it
must treat it as a soft prior with tails, never a measurement — which is why it
enters as an emission weight and an envelope, and never as a veto.

**Two sessions are pathological at this run root.** Tracker-tier spreads on
2026-06-11 and 2026-07-04 run to 247° and 139° for F2, against 15.7° on the
instrumented session. That is a property of those runs, not of the model, and it
is why the instrumented channel carries the weight here.

**The projection layer is one session** — ten swings, one golfer, one club.

---

## 13. Discussion: what this earns in swing assessment

Everything above was written for the tracker. This section is written for
anyone who wants to turn the model into coaching, and it tells the story of
testing that ambition — a story with a negative ending that is worth
*understanding* rather than merely knowing, because the way it fails teaches
more about per-swing measurement than a success would have.

The note delivers three artefacts in three different states. The fitted table
is accurate and stays dark, for the tracker reasons of §12. The swing-plane
delta is precise, one validation short of useful, and wanted by name
elsewhere in the codebase. And the seven parameters are a coaching vocabulary
whose central claim — that they can be read off a single swing — is tested
below, and fails in an instructive way.

### The plane delta: the shortest path into the diagnostic model

Over the top — among the commonest faults in amateur golf — is a downswing
that comes down *steeper and more outside* than the backswing went up: the
club changes plane at the top, in the wrong direction. The fault ontology
(`docs/design/swing_fault_ontology.md`) knows the fault well, calls it the
most visible one in the library, and admits it cannot detect it, because no
producer measures the one thing that defines it. It even names the measure it
is waiting for: a `transition_plane` axis — how much the plane changes
between backswing and downswing — whose high tail would fire the fault.

§9's delta is that measure. The reason it is usable *now*, while the absolute
plane angle is not, is a distinction worth internalising: a difference can be
trustworthy when neither of its ingredients is. A bathroom scale that reads
two kilograms heavy is useless for your weight and perfect for your weight
*change* — the error rides along with both readings and subtracts out. The
ellipse fit is in exactly that position. Its absolute inclination carries a
calibration bias we cannot yet confirm (§9 bounds it but does not correct
it), but the backswing and downswing planes are measured the same way, by the
same fit, on the same swing, so the shared bias cancels in their difference.
What remains is precision, and the precision is excellent: refit the plane on
half the frames and the delta moves 0.6–0.7°, against a 17° median signal.

Two things stand between the delta and that role, both already on §9's open
list. The sign convention — larger inclination means flatter — has never been
checked against a swing whose plane is independently known; one down-the-line
recording settles it. And the projection layer is ten swings of one golfer.
The first is an afternoon; the second is the corpus problem this whole note
shares.

### Why "fits the population" is not "reads a swing"

§7's parameters make coaching sense — `A` is how much lag you build, `t_r`
when you release it, `w_r` how violently, `r` how much you are still holding
at impact — and the ontology gives them somewhere to land: `casting` owns
`lag_retention`, which is `A` and `t_r`; `scooping` owns
`lead_wrist_impact`, and `r` above one is precisely the flip. The genuinely
new content is `t_r` and `w_r`, the *when* and *how hard* of the release,
which no current producer measures.

But every fit in this note is a population fit: fifty-nine swings pooled
into one curve. The striking stability of §7 — drop any swing from the pool
and the release timing moves by less than a millisecond — is a property of
the *pool*, and it is easy to over-read. The average height of a crowd can
be known to a millimetre while every individual measurement is centimetres
wrong; the stability of an average says nothing about whether one swing's
samples can produce that swing's own numbers. And a coach has no use for
the population's release timing. A coach wants *this* swing's, compared to
the last one.

There were reasons to doubt that in advance, and the note already held all
three. The release parameters live in the final 140 ms, exactly where the
model is 1.7× worse (§5) and where θ comes from the tracker through impact
blur — a per-swing fit spends the tracker's worst output on its hardest
question. The one per-swing idea already tried — calibrating each swing's
offset on its own backswing — made things worse (§10). And β mixes wrist
mechanics with camera geometry (§9), so even a perfect per-swing estimate
would confound two golfers' wrists with their planes.

Doubt is not a verdict, though. The claim deserved a test.

### Three gates: how to ask whether a fitted number is real

The test has a shape worth keeping, because it applies to any per-swing
number this programme will ever propose. Three questions, in rising order of
severity — and each can pass while the next fails, which is why all three
are needed:

**Can the swing's own data pin the number down at all?** Refit under
bootstrap resampling of the swing's frames and read off a confidence
interval, then compare that interval to how much swings actually differ
from one another. If the uncertainty on one swing is as wide as the
differences between swings, the estimate cannot tell one swing from another
and the number is decoration. This is *identifiability*.

**Does it come back the same when nothing has changed?** Ten swings from
the same golfer in the same hour should read nearly alike. If the number
scatters as much within a session as it does between sessions, it is noise
wearing a coaching name. This is *repeatability*.

**Is it measuring the swing, or the instrument?** A number can be tight and
repeatable and still be reading the camera, the lighting, the tracker's
habits — anything but the wrist. Precision is not accuracy, and the only
cure is agreement with an independent instrument, here the stripe-fusion
truth of 2026-07-05. This is *truthfulness*, and it is the gate that
decides.

The experiment fitted the parametric form per swing at three levels of
ambition — all seven parameters free; the cock-side shape (`b₀`, `t_c`,
`w_c`) frozen at population values with `A`, `t_r`, `w_r`, `r` free; and
`A` frozen too, release only — on two channels: the tracker's measured
tier, which is what production would have, and the instrumented truth,
which grades it.

**The answer is no, and the mechanism is worth walking through.** (207 fits,
100 bootstrap reps throughout; data in `data/wrist_cock_model/per_swing/`,
command in §14; every headline number below was independently recomputed
from the CSV by a second pass. The population fit reproduces §7 to three
decimals, and the harness aborts if it does not.)

Start with the one observation that explains everything else. At 150 fps the
last 100 ms before impact — where the release happens — contains fifteen
frames. The frames exist; they are simply unreadable. The shaft is a blur
streak, so the measured tier keeps a median of six of them, and the
instrumented tape, whose stripes must be legible to be decoded, keeps none
at all:

| channel | median samples, last 250 ms | last 100 ms | swings with none in last 100 ms |
|---|---|---|---|
| tracker | 26 | 6 | 5 of 59 |
| truth | 15 | **0** | **8 of 10** |

The parameters that make the model interesting live precisely in the window
neither channel can see. Everything that follows is a consequence of that
table.

**The first consequence was a trap, and it is the most transferable lesson
here.** When a swing has no samples inside the release window, moving `t_r`
changes nothing the data can see — the loss surface is *exactly* flat in
that direction. An optimizer walked onto a flat surface stops where it
stands: at its initialisation, which here is the population values, returned
to six significant figures and indistinguishable from a confident
measurement. Without a probe that checks whether the loss actually varies
along each parameter, the harness would have reported roughly 35 fabricated
per-swing "estimates" per parametrisation. The lesson generalises well
beyond wrists: an optimizer answers every question you ask it; whether the
*data* answered is a separate question, and checking it is your job, not the
optimizer's.

**The gates, in order.** Identifiability splits along the same line as the
coverage. The cock side of the curve — slow, unblurred, well-sampled —
identifies cleanly per swing: `b₀`, `A`, `t_c` and `w_c` all pass, with
`A`'s confidence interval a third of the between-swing spread. The release
side, living in the blind window, is marginal throughout.

Repeatability looked better than it was, and the way it deceived is worth
spelling out. The release parameters differed between sessions far more than
within them — exactly the signature of a real, stable, personal number. But
correlate those between-session differences with each session's sample
count and the illusion collapses: sessions with two readable frames in the
last 100 ms read `w_r` ≈ 2 ms and `t_r` ≈ 0, sessions with fourteen read
~15 ms and −35 ms. The "golfer's release" that varied between sessions was
the lighting and blur varying between sessions. When an estimate correlates
with how much data produced it, it is measuring the data supply.

Truthfulness ends the argument: **no parameter clears it.** `A` — how much
lag — is the sole marginal survivor: tracker and truth agree to a 5.5°
median gap, rank-correlate at +0.49, and put a swing on the same side of the
population value eight times in ten. And `w_r` supplies the parable that
justifies the whole three-gate design: it is the most tightly identified
parameter per swing (interval at 0.17 of the spread) and its estimates are
*uncorrelated with truth* (ρ = +0.02). A confidence interval measures how
sure the fit is of itself — never whether it is measuring the right thing.
That is why identifiability alone must never promote a parameter.

**What survives.** `A` has a plausible per-swing life, marginal today. The
verdict on the rest is carefully worded: not that per-swing release
structure does not exist, but that neither channel observes the release well
enough to say. And since the frames are there and unreadable, the constraint
is legibility per frame, not frame count. Shorter exposure attacks it (blur
is bought by shutter time, not by frame rate, and shutter is bought with
light — studio lighting is cheap next to high-speed cameras). Reading the
blur wedge itself as a *rate* measurement attacks it (a smear's angular
extent is the integral of θ̇ across the exposure — a measurement wearing the
costume of a failure). An IMU on the grip or a release-legible truth fixture
attacks it. A faster camera does not. Until one of those exists, the seven
parameters remain what §7 called them: a compact description of the
population curve, with no per-swing reading.

### The retreat to one parameter, and what it settled

There was one move left on the fitting side, and it was taken deliberately
as the last one: collapse the ambition to a single number. Freeze the entire
population curve and fit only δ, a per-swing time-shift of the release —
"this swing released 8 ms later than the population" — so that every sample
in the release region votes on one question instead of dividing itself among
three. If any per-swing release number can be estimated from this data, it
is this one.

Two new channels joined the measured tier, each carrying its own question.
The P7 *anchor* — the located impact position — entered as a single
trustworthy point at t = 0, to ask whether one good sample can stabilise
many poor ones. And the synthesized track entered to answer something this
programme needed settled anyway. `club.synth` is the display tier: a smooth
curve threaded through the located P-positions at 240 Hz so that replay
scrubs smoothly. It is coherent, it is dense, and it is explicitly excluded
from every production measurement, because it is an *inference* — between
anchors it contains exactly what the interpolation put there. Using it here
is a deliberate research exception, made to test a tempting idea head-on:
does a dense, coherent, inferred track know things the sparse measured one
does not?

One more idea makes the results readable: *leverage*. A sample constrains δ
only if the model's prediction at that sample moves when δ moves. Samples on
the long lag plateau are blind to release timing — the curve there is flat
regardless of when the release fires — so what matters is never the raw
count of samples near impact but the count with leverage on the question
being asked. (The fit itself is a deterministic grid sweep, so nothing below
is an optimizer artefact; data in `data/wrist_cock_model/per_swing/p1_*`,
command in §14, every number independently recomputed.)

The retreat worked exactly as far as the arithmetic said it should, and no
further. δ became the first per-swing quantity in this line of work to pass
identifiability — pinned to 2–3 ms on a single swing, against 8–10 ms of
between-swing spread. Three findings then close the door anyway.

*The pass only covers swings that show a release.* Nineteen to twenty-one of
fifty-nine converge; the rest pin at the search bound, which is the fit's
way of saying *there is no release in this channel's data*. Convergence is
purely a session property — 10/10 and 6/6 on the two well-covered sessions,
0–2 everywhere else.

*Even the surviving δ partly reads the camera.* Across the healthy sessions
(the ones §12 does not flag as pathological) it correlates +0.64 with the
swing's sample count in the last 250 ms — the same
contamination that fooled the repeatability gate above, now visible inside a
single parameter.

*And truth cannot grade it.* The instrumented rows all sit on the plateau:
their median count of samples with leverage on δ is zero. The truthfulness
gate is not failed but *unusable* — not a disagreement with truth, the
absence of one.

The synthesized channel answered its question cleanly along the way. It
reproduces the measured channel almost exactly — rank correlation +0.98,
median gap 1.3 ms — and converges on precisely the same swings, despite
carrying eighteen leverage samples on every one. Density created no
information, because the interpolation can only re-tell what the anchors
already said; the anchors came from the same tracker whose gaps we were
trying to fill. **A mirror, not a window** — worth one experiment to
establish, so that nobody reaches for it later. The anchor's contribution
was similarly honest: one point, taken at the uncertainty its producer
actually supplies, moves δ by 0.4 ms.

With one parameter as small as ambition gets, the per-swing story ends here
until the observation itself changes.

### What this section must not be used for

Norms. Every number here is one athlete, and diagnostic bands built from
`t_r = −32 ms` would install one golfer's release timing as everyone's
standard. The shape of the curve may be general; the timing is his. And
nothing here reopens the tracker key: the diagnostic uses and the tracker
use are separable, and on the evidence of §12 only the first is live.

---

## 14. Reproducing every number here

Supporting data is in [`data/wrist_cock_model/`](data/wrist_cock_model/).

```
python3 tools/swinglab/fusion_truth.py --audit \
    --lab-root /mnt/swingdata/shaftlab/lab/tape_20260705 \
    --run-root /mnt/swingdata/stagegate/corpm3-off \
    --corpus   /mnt/swingdata/Mark-Liversedge --out <dir>     # §5 provenance, phi

python3 tools/swinglab/wrist_cock_fit.py /mnt/swingdata/stagegate/corpm3-off \
    --corpus /mnt/swingdata/Mark-Liversedge --out <dir> --knots 15 \
    [--lab-root /mnt/swingdata/shaftlab/lab/tape_20260705] [--holdout session]

python3 tools/shaftlab/plane_probe.py census --out <dir>      # §9 foreshortening
python3 tools/shaftlab/plane_probe.py planes --out <dir>      # §9 the plane

python3 tools/swinglab/wrist_cock_fit.py /mnt/swingdata/stagegate/corpm3-off \
    --corpus /mnt/swingdata/Mark-Liversedge \
    --lab-root /mnt/swingdata/shaftlab/lab/tape_20260705 --knots 15 \
    --out docs/research/data/wrist_cock_model/per_swing \
    --per-swing --per-swing-boot 100 --per-swing-boot-p7 100  # §13 per-swing

python3 tools/swinglab/wrist_cock_fit.py /mnt/swingdata/stagegate/corpm3-off \
    --corpus /mnt/swingdata/Mark-Liversedge \
    --lab-root /mnt/swingdata/shaftlab/lab/tape_20260705 --knots 15 \
    --out docs/research/data/wrist_cock_model/per_swing \
    --p1 --p1-boot 100                                        # §13 the coda
```

`--knots 15` is load-bearing: the CLI default is 13, the shipped table used 15,
and a run at the default is not comparable to any number here. Without
`--lab-root`, `wrist_cock_fit.py` grades exactly as it did before the
instrumented channel existed.
