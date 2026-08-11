# Predicting the Club from the Arm: the Wrist-Cock Curve, its Parametric Reading, and the Swing Plane Behind It

*PinPoint shaftlab programme — model note, 2026-08-11. Fitted and graded by
`tools/swinglab/wrist_cock_fit.py`, with the instrumented truth join in
`tools/swinglab/fusion_truth.py` and the projection layer in
`tools/shaftlab/plane_probe.py`. Implements the v2 table in
`src/Analysis/shaft_kinematics.h`, dark behind `shaft.wedge.kinModelV2`.
Companion to the programme report, `club_detection_from_video.md`, whose Phase 6
sets out the physical law this model puts numbers on.*

---

## 1. What this is for

The shaft tracker would like to know roughly where the club is before it looks
for it. It cannot measure θ in advance — that is the thing being solved — but it
*can* measure the lead arm from pose on every frame, including the frames where
the club is a blur. If the wrist angle between arm and club were predictable,
the arm would locate the club for free:

    θ̂(f) = φ(f) + chir · β̂(x(f))

That prediction already exists in the tracker. It centres the blur-wedge search
envelope, sets the `kinCone` off-envelope penalty, and supplies the predicted
angular rate that triggers the wedge at all.

What it had never had is data. The original table was authored by hand from the
design's expectations. This note grades it, fits a replacement, reports what the
fit turned out to depend on — which was not what we expected — and then goes one
layer deeper, to the swing plane that the whole image-plane picture is a shadow
of.

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
- Quality: 1.79° self-jitter in the downswing, measured about a local quadratic
  (§6).

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

Two changes are available — fit the curve to data, and change the variable it is
indexed by. Both help, and separating them matters, because our first reading got
the split badly wrong.

**The axes, measured directly.** Before fitting anything, ask how much each
candidate clock can possibly support: the spread of β about its own conditional
median on that axis, leave-one-swing-out. This is the floor no model on that axis
can beat.

| axis | frame-averaged | at the instants truth labels sit |
|---|---|---|
| swing progress (bs0 / top / impact anchored) | 30.4° | **67.3°** |
| address → impact, linear | 36.8° | — |
| top → impact, linear | 25.6° | — |
| **seconds before impact** | **16.1°** | **15.4°** |

    x = (t − t_impact) / 10⁶     seconds, negative through the swing, 0 at impact

Seconds-before-impact wins on both readings, and the reason is physical: **wrist
release is not a fixed fraction of the swing but an event at a roughly fixed time
before impact.** Two swings reaching the top at the same fraction but taking
different times release at the same number of milliseconds before impact and at
quite different fractions, so a progress index smears every swing's release
across every other's.

**But look at the two columns.** The progress axis is twice as bad at truth-label
instants as frame-averaged; the time axis is not. Truth labels cluster at
s ≈ 0.30–0.57 — the transition, where a human can see the shaft — and
`swingProgress` is anchored on the **top**, the noisiest event in the ladder.
Small top-placement errors blow β up precisely where labels are densest.

This is settled and should not be re-litigated.

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
at 0.6% bad against dense truth — while the *decision* rests on labels that owe
the tracker nothing. Every number is leave-one-swing-out.

**Report frame-averaged and truth side by side, always.** Where the labels sit
changes the ranking, and §10 records the case where reading the truth column
alone inverted a conclusion. The report generator now *refuses* to emit a truth
column without its frame-averaged partner.

**Name the run root.** Numbers downstream of a run root are not portable. At
`stagegate/corpm3-off` (59/61 swings usable) the previously published figures do
not reproduce:

| form | as previously published | at `corpm3-off` |
|---|---|---|
| F0 shipped table | 74.3° | **77.6°** |
| F2 refit on seconds-before-impact | 20.9° | **26.8°** |

Same 805 labels, 471 scored residuals against the earlier 463. The earlier
figures were computed at an unnamed root. Other roots are worse than merely
different: `stagegate/final1` carries no `club` block at all, and `corpoff-live`,
`off1`, `off2`, `on1` are missing the takeaway phase and lose all 61 swings.

### Label provenance — most of the "instrumented truth" was already in use

The 2026-07-05 session has instrumented stripe-fusion truth. It is *not* new
material: `truth.json` for that session **is the fusion band tier verbatim** —
9 of 10 swings match row-for-row, head positions to 0.000 px, θ to a pooled
median 0.0063°. Of the corpus labels on those swings, **565 are instrumented band
rows and only 61 are genuinely hand-placed.**

What *is* new is the **ray tier**: 468 rows, 283 of them pre-impact — the fast
frames a human cannot label at all.

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

F2 barely notices; the φ-term leaks seven times as much. §10 rejected F3 for
absorbing between-swing variation it could not predict — that was an inference
from envelope collapse, and this is the direct measurement.

---

## 6. φ provenance: an alarming disagreement that dissolves

The lab's `anchors.csv` carries a φ derived from an older pose run. It disagrees
with production φ by a downswing p90 of **51.9°** pooled — which, taken at face
value, would exceed the entire residual the model is trying to explain and would
mean every number here was φ-limited.

It is not. Three hypotheses were tested and killed:

- **Interpolation across a sparse pose?** No — the disagreement is *largest* at
  the smallest gaps (median 20.3° at 0–3 ms, falling to 11° at 10–20 ms), which
  is backwards. The pose is not sparse: 215 frames at 6.9 ms spacing is near
  per-frame over the swing window.
- **`smooth_angle`'s moving average?** No — raw and smoothed agree to 0.2°.
- **A timebase offset?** No — the best per-swing shift scatters −9 to +40 ms and
  improves the median only 13.7° → 12.2°.

The disagreement is real: two pose extractions of the same video differ that
much. **But a disagreement never says which side is wrong.** Each channel's own
jitter does, measured about a local quadratic on its own samples:

| φ source | backswing | downswing |
|---|---|---|
| production pose (raw, unsmoothed) | 3.13° | **1.79°** |
| `anchors.csv` | **0.12°** | 3.51° |

`anchors.csv` reading 0.12° in the backswing is too smooth to be a measurement —
the signature of interpolation between sparser samples — and where the swing is
fast it cannot hide, degrading to 3.51°. **Production φ stays, and at 1.79°
jitter the model's ~20° residual is not φ-limited.**

*(A note on method: the first version of this comparison scored production's
smoothed φ against anchors' unsmoothed and reported 0.23°. Smoothing suppresses
exactly the quantity being measured. The table above uses raw φ on both sides.)*

---

## 7. The parametric model

Everything above fits a **lookup table**: fifteen knots and linear interpolation.
That is an empirical curve, not a model — as many free numbers as knots, none
meaning anything alone, free to wiggle wherever data is thin. Calling it a model
would flatter it.

So: can the curve be written down? The physics says it has a shape — a wrist that
cocks once, holds, and releases — and two logistic transitions express exactly
that:

    β(t) = b₀ + A · L((t − t_c)/w_c) · (1 − r · L((t − t_r)/w_r))

with `L(z) = 1/(1 + e^(−z))`. Read the structure before the parameters: it is a
**product of two events** — the first logistic turns the lag on (cocking), the
second takes it away (the release), and `b₀` is where it starts.

Fitted under a robust loss, leave-one-swing-out, at `stagegate/corpm3-off`:

| symbol | name | value | fold spread | what it *is* |
|---|---|---|---|---|
| `b₀` | address offset | **−10.4°** | ±0.6 | the wrist angle at address — a small forward press |
| `A` | peak lag amplitude | **98.7°** | ±0.9 | total lag built; the full extent of the hinge |
| `t_c` | cock centre | **−0.702 s** | ±0.002 | *when* the wrist cocks — during the backswing |
| `w_c` | cock width | **0.089 s** | ±0.001 | *how fast* — cocking takes ≈4·w_c ≈ 0.36 s |
| **`t_r`** | **release centre** | **−0.032 s** | ±0.001 | **when the release fires — 32 ms before impact** |
| **`w_r`** | **release width** | **0.011 s** | ±0.001 | *how fast* — **≈8× faster than the cock** |
| `r` | release completeness | **0.741** | ±0.017 | how much lag is spent; ~¼ still held at impact |

*(An earlier run at an unnamed root gave b₀ −4.3°, A 90.7°, t_c −0.690 s,
w_c 0.079 s, t_r −0.038 s, w_r 0.014 s, r 0.846. The shapes agree; the levels are
run-root dependent, which is why the root is now always named.)*

The stability is the striking part: drop any swing and the release timing moves
by less than a millisecond.

### What a coach would read off this

- **`A` — how much lag you create.**
- **`t_r` — when you release it.** Later (closer to 0) means holding lag longer.
- **`w_r` — how violently.** Small means a snappier, later hit.
- **`r` — whether you're still holding at impact.** Below 1 means shaft lean,
  hands ahead of the ball; above would mean a flipped, scooped release.

That is the point of the exercise: the table fits better but says nothing, while
these seven numbers can be compared between swings, sessions and golfers.

### But the table is more accurate, and we ship the table

| | median | p10–p90 | \|err\|>30° | 3σ envelope covers |
|---|---|---|---|---|
| empirical table (15 knots) | +0.1° | **20.9°** | 6.7% | 97.0% |
| parametric (7 params) | −1.3° | 32.6° | 7.8% | 98.3% |
| shipped, hand-authored | −9.1° | 74.3° | 26.3% | 96.8% |

The parametric form is unbiased and beats the shipped table better than two to
one, but gives up ~12° to the lookup table. The reason is visible in Figure 1:
the real curve does not hold a flat plateau. It dips around 0.36 s before impact
and rises to a distinct peak at 0.14 s, and a product of two logistics cannot
express that shape. Whether that structure is real mechanics — a re-cock as the
arm changes direction at transition — or one golfer's artefact is not settled,
and it is the first thing a second athlete would tell us.

**One form is not a modelling exercise, and this section is not one.** The
logistic product was chosen by looking at the empirical curve and picking a shape
that resembled it. It fits because the curve is sigmoid-ish, and would have
fitted about as well had the mechanism been something else. No alternative
families were fitted, nothing was derived from the two-link dynamics this note
keeps invoking, and no model selection was performed. A proper pass would derive
a family from the double pendulum with a wrist torque — where the release is
largely passive once the arm decelerates, which *predicts* a functional form
rather than borrowing one — fit several families, and judge them on out-of-domain
prediction (fit the backswing, predict the release) rather than in-domain
residual. Until then the seven parameters are a compact description, not physics.

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
| shipped table | 62.5° | 96.8% |
| fitted, σ as fitted | 17.7° | 90.1% |
| **fitted, σ × 2.5** | **44°** | **97.0%** |

The fitted σ is honest about this corpus and too confident about the world: at
face value the envelope would miss the true club on one frame in ten. The shipped
table covers 96.8% by being wide enough to be nearly uninformative.

The table ships at **σ × 2.5**, the factor at which the new envelope covers as
much true wrist cock as the old while searching a narrower arc. That is the whole
claim: *the search is no less forgiving than it was, and it now points in the
right place.* The inflation is the explicit price of a corpus with one athlete in
it — the fitted centre is a measurement, the fitted spread is one golfer's
repeatability, and only the first generalises.

---

## 9. The projection layer: the swing plane behind the shadow

Everything above lives in the image plane, and that is a problem the model has
not yet accounted for.

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

Two implementation points, both learned the hard way:

- **Fit `head − grip`, not the absolute head path.** The absolute path is grip
  translation *plus* club rotation, so it is not a planar closed curve about a
  fixed centre and a conic fitted to it is badly conditioned. Split-half
  repeatability improves from 9.2° to **0.6°** in the downswing.
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

- **Precision is excellent.** Split-half repeatability (fit odd frames, fit even
  frames, compare) is **0.6–0.7° in both phases**. Against a 17° delta and a 53°
  between-swing range, the signal is far above the noise floor.
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
  entire overlap — the instrumented tier covers frames the tape is legible in,
  while `lenPx` lives in the slow phases.
- **The single-fixed-plane control is unavailable, not passed.** A single plane
  was supposed to fail, reproducing `length_model`'s documented ~2× symmetry
  violation. It does not (5.0% median relative error against a per-phase model's
  4.4%) — but only because the instrumented tier lacks the antiparallel shaft
  directions that produced the original violation. This channel cannot acquit or
  convict it.
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

**Not yet established**, and needed before the delta becomes a coaching output:

- **The sign convention is unvalidated.** The reasoning says larger ι = flatter,
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

The negative results are the valuable part of this note.

**Reading the axis comparison against truth alone** — an error of ours, not the
model's. It made refitting the progress axis look worthless (75.6° against 74.3°)
when frame-averaged it is a 22° gain. Both numbers were sitting on that axis's
floor *at the instants our labels occupy*, and we mistook a property of the label
distribution for a property of the axis. The lesson generalises: when a comparison
is scored only where truth exists, check what the truth distribution is doing
before believing the ranking.

**Adding a linear term in φ** (F3). More accurate on paper — 17.4° against F2's
20.9° — but it fails on three counts. It carries a +6.5° bias against truth while
showing only +0.3° against the tracker's own tier, the signature of a term fitted
to tracker quirks rather than mechanics. Its envelope collapses to 35% within 1σ.
And on instrumented truth it is simply worse than F2 (16.1° against 12.1° band,
23.9° against 20.5° ray) while leaking 4.9° under session holdout against F2's
0.7°. Against hand labels it had looked *better*; the label distribution was
flattering it. More accurate and less honest is not a trade this programme makes.

**Shape constraints** (F4). Imposing the one-reversal law and the in-line-at-
impact anchor changed nothing material and inherited F3's problems. The reason is
the interesting part: **the unconstrained fit already satisfies the constraints**,
rising monotonically to a single maximum and releasing monotonically to a small
positive value at impact without being told to. The physics is in the data — a
null result that is mild evidence the law is real rather than imposed.

**Per-swing calibration.** Fitting a per-swing offset on the backswing and
carrying it into the downswing *hurts*: residual in the last 250 ms rises from
22.8° to 26.6°. A golfer's backswing wrist offset does not predict their release.
The model stays a population model.

**The truth upgrade, as conceived.** Two-thirds of the "new, unused" instrumented
truth was already being graded on, copied into `truth.json` without provenance.
The real gain is the ray tier and tier separation, not volume.

**`anchors.csv` as a better φ.** Interpolated, artificially smooth where the swing
is slow, and worse than production where it is fast.

**Three explanations for the φ disagreement** — interpolation gaps, smoothing, a
timebase offset — each tested and killed before self-jitter settled it.

**Two of our own errors in the projection layer, recorded because the numbers
looked plausible.** First, the clubhead route was ruled out entirely on the
argument that `head = grip + lenPx·u(θ)` so the path "carries no new
information" — which confuses *provenance* with *structure*, since whether the
3-D path is planar is a claim the algebra does not grant. Then the conic was
fitted to the *absolute head path* in *anisotropically normalised* coordinates,
giving ι = 58.5°/39.1°, a 19.4° shift and a 4.3° agreement with foreshortening
that read as clean corroboration. **All of those were artefacts.** What caught it
was a split-half test that costs almost nothing — worth running on any new plane
estimate before trusting it.

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
that and an unseen golfer. The honest reading is that the *shape* is likely
general and the *timing* may not be.

**The truth is thin at the ends.** Hand labels concentrate where a human could
see the shaft, so the impact blur — the region the model most wants to serve — is
where truth is sparsest. The instrumented ray tier partly fixes this, and shows
the model is 1.7× worse there.

**The residual is not Gaussian.** 6.7% of samples remain worse than 30°, so the
model is usually good and occasionally quite wrong. Anything consuming it must
treat it as a soft prior with tails, never a measurement — which is why it enters
as an emission weight and an envelope, never a veto.

**The projection layer is one session.** Ten swings, one golfer, one club.

---

## 13. Reproducing every number here

```
python3 tools/swinglab/fusion_truth.py --audit \
    --lab-root /mnt/swingdata/shaftlab/lab/tape_20260705 \
    --run-root /mnt/swingdata/stagegate/corpm3-off \
    --corpus   /mnt/swingdata/Mark-Liversedge --out <dir>     # §5 provenance, §6 phi

python3 tools/swinglab/wrist_cock_fit.py /mnt/swingdata/stagegate/corpm3-off \
    --corpus /mnt/swingdata/Mark-Liversedge --out <dir> --knots 15 \
    [--lab-root /mnt/swingdata/shaftlab/lab/tape_20260705] [--holdout session]

python3 tools/shaftlab/plane_probe.py census --out <dir>      # §9 foreshortening
python3 tools/shaftlab/plane_probe.py planes --out <dir>      # §9 the plane
```

`--knots 15` is load-bearing: the CLI default is 13, the shipped table used 15,
and a run at the default is not comparable to any number here. Without
`--lab-root`, `wrist_cock_fit.py` is byte-identical to its pre-instrumented
behaviour.
