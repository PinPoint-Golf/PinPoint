# The wrist model: definition, variables, and how θ, φ and ψ interact

*Written 2026-08-11. This is the reference definition of the model — every symbol,
what it means physically, how the pieces compose, and what each fitted number
says about a golf swing. Session results are in
[`wrist_projection_layer.md`](wrist_projection_layer.md); prior fitting history is
in [`wrist_cock_model.md`](wrist_cock_model.md). The shipped C++ is
`src/Analysis/shaft_kinematics.h`.*

---

## 1. The problem in one paragraph

A single face-on camera sees the golfer from the front. From it we can measure,
frame by frame, the direction the **lead forearm** points and the direction the
**club shaft** points — two angles in the image. We cannot see depth. The club is
attached to the hands, so its direction is *mostly* determined by where the arm
is; the difference between them is the wrist. The model's job is to predict the
club's direction from the arm's, and the quantity it predicts is **θ**.

That prediction is what the shaft tracker uses to know where to look for the club
in the next frame, and it is why the model exists at all.

---

## 2. The three angles, and how they compose

All three are **image-plane angles in pixel coordinates**, measured with
`atan2(dy, dx)` in a y-down image, so they increase clockwise as drawn on screen.

### φ (phi) — the arm angle. *Measured.*

The direction of the **lead forearm**: elbow → grip.

- Source: the pose estimator (ViTPose, 133-point COCO-WholeBody). The elbow is
  keypoint 7 (left) or 8 (right); the grip is the midpoint of the two hand
  centroids.
- Physically: where the lead arm is pointing. In the backswing it rotates one
  way, in the downswing the other.
- **This is an input, never predicted.** The pose gives it directly.
- Measured quality: 1.79° self-jitter in the downswing (`fusion_truth.py --audit`).

### θ (theta) — the club angle. **This is what the model predicts.**

The direction of the **club shaft**: grip → clubhead.

- Physically: where the club is pointing. Runs through a huge range — the shaft
  sweeps well over 180° between address and impact.
- The tracker measures θ when it can see the shaft, and must *predict* it when
  it cannot: through impact blur, when the shaft crosses the body, when it leaves
  frame. **θ is the prize.**

### ψ (psi) — the wrist angle. *Derived.*

    ψ = θ − φ

The angle between club and forearm — how much the club is "cocked" relative to
the arm. Physically this is the wrist hinge as the camera sees it. At address the
club is roughly in line with the arm (ψ small); at the top the club is cocked
nearly 90° or more; through the release it unhinges rapidly back toward zero.

### β (beta) — ψ made comparable between golfers

    β = chir · wrap180(θ − φ)   = chir · ψ

where `wrap180` folds the angle into (−180°, +180°], and **chir** is ±1.

`chir` (chirality) is the swing's handedness, defined as the sign of the arm's
angular travel from takeaway to the top:

```
chir = +1  if φ(top) − φ(takeaway) ≥ 0,  else −1
```

A right-handed and a left-handed golfer produce mirror-image swings; without
`chir`, ψ would have opposite signs for the two and no single table could serve
both. Multiplying by `chir` puts every golfer in the same convention, so **β > 0
always means "the club trails the arm"** — lag — regardless of handedness.

`chir` is computed per swing from the pose, never from metadata, because
metadata is often wrong and the pose cannot be.

### The composition, and the direction it runs

The model stores β and reconstructs θ:

    θ̂ = φ + chir · β̂

(`phiClubFromBetaDeg`, `shaft_kinematics.h:199`, wrapped into [0°, 360°)).

Read it as a sentence: **take where the arm points, and swing the club off it by
the predicted lag, on the side the golfer's handedness puts it.** The sign of β̂
also decides *which side of the arm the club sits on* — trail side while β̂ > 0
through the downswing, lead side once it has released past the arm.

So the causal chain is:

```
pose  ──►  φ  (measured, per frame)
                 ╲
                  ├──►  θ̂  = φ + chir·β̂        ──► where to search for the shaft
model ──►  β̂  ──╱                                   next frame
           (predicted from the clock alone)
```

**The model never sees θ when predicting it.** β̂ depends only on time, not on
any measurement of the club. That is deliberate — a model fitted to the tracker's
own θ and then used to constrain the tracker would reinforce its own errors.

### The companion: σ_β

Alongside β̂ the model carries **σ_β(t)**, the *spread* of β at that instant —
how uncertain the lag is. It sets the width of the search wedge: the tracker
looks in an arc `β̂ ± k_σ·σ_β` with `k_σ = 3`. A narrow σ where the model is
confident makes the search cheap and precise; a wide σ where golfers differ keeps
it from missing the shaft entirely. σ is fitted, then multiplied by an inflation
factor (2.5 as shipped) because the corpus is one athlete and the envelope must
survive golfers it has never seen.

---

## 3. The clock: why "seconds before impact"

β̂ is a function of time, and *which* time matters enormously.

    x = (t − t_impact) / 10⁶     seconds, negative through the swing, 0 at impact

The obvious alternative is **swing progress** — a 0-to-1 fraction anchored on
takeaway, top and impact. It is much worse: the irreducible spread of β about its
own conditional median is 30.4° on swing progress against **16.1°** on seconds
before impact.

The reason is physical, and it is the single most useful thing prior work
established. **The release is an event at a roughly fixed time before impact, not
at a fixed fraction of the swing.** A golfer who takes a long backswing does not
release proportionally later; they release about 40 ms before impact, as they
always do. Normalising by swing duration smears that event across the population;
counting backwards from impact holds it still.

This is settled and should not be re-litigated.

---

## 4. The parametric model

The shipped table is 15 knots of a lookup curve — accurate but meaningless, as
many free numbers as knots. The parametric form replaces them with **seven
numbers that each mean something**:

    β(t) = b₀ + A · L((t − t_c)/w_c) · (1 − r · L((t − t_r)/w_r))

where `L(z) = 1/(1 + e^(−z))` is the logistic, a smooth 0→1 step.

Read the structure before the parameters. It is a **product of two events**:

- the first logistic **turns the lag on** — the wrist cocking during the backswing;
- the second **takes it away** — the release;
- `b₀` is where it starts.

### The seven parameters, physically

Fitted values are leave-one-swing-out medians at run root `stagegate/corpm3-off`,
with the fold-to-fold spread (p10–p90):

| symbol | name | value | spread | what it *is* |
|---|---|---|---|---|
| `b₀` | address offset | **−10.4°** | ±0.6 | The wrist angle at address, before anything happens. Slightly negative: the club sits marginally ahead of the forearm line at setup — a small forward press. |
| `A` | peak lag amplitude | **98.7°** | ±0.9 | How much lag the golfer builds in total. The full extent of the wrist hinge, in degrees. Roughly a right angle between club and forearm at the top. |
| `t_c` | cock centre | **−0.702 s** | ±0.002 | *When* the wrist cocks — 0.70 s before impact, i.e. during the backswing. |
| `w_c` | cock width | **0.089 s** | ±0.001 | *How fast* it cocks. The logistic's time constant: the hinge takes roughly 4·w_c ≈ 0.36 s to go from barely started to essentially complete. Cocking is a leisurely, gradual thing. |
| `t_r` | release centre | **−0.032 s** | ±0.001 | **When the release happens — 32 ms before impact.** The sharpest, most repeatable number in the model. |
| `w_r` | release width | **0.011 s** | ±0.001 | *How fast* the release is. About 11 ms — the release is roughly **eight times faster than the cock**. This asymmetry is the central fact about a golf swing's wrist action. |
| `r` | release completeness | **0.741** | ±0.017 | *How much* of the lag is spent by impact. 0.74 means about three-quarters of the built lag is released at contact — the golfer arrives with roughly a quarter of it still held. `r = 1` would mean the club is exactly in line with the arm at impact. |

### What a coach would read off this

Four of these are quantities a coach already talks about, now measured in units:

- **`A` — how much lag you create.** Bigger is more stored angle.
- **`t_r` — when you release it.** Later (closer to 0) means holding lag longer.
- **`w_r` — how violently you release.** Small means a snappier, later hit.
- **`r` — whether you're still holding at impact.** Below 1 means shaft lean —
  hands ahead of the ball; above would mean a flipped, scooped release.

That is the point of the whole exercise: the lookup table fits better but says
nothing, whereas these seven numbers can be compared between swings, sessions and
eventually between golfers.

### Its accuracy, honestly

The parametric form scores **worse** than the empirical table (38.2° against
26.8° at this run root). It loses because the real curve is not a clean product of
two steps: it dips around 0.36 s before impact and rises to a distinct peak at
0.14 s, and no product of two logistics can express that shape. Whether the dip is
a real re-cock at transition or one golfer's artefact is unresolved.

So the parametric model is **a baseline to beat and a shape to explain, not the
answer.** It was also chosen by eyeballing the empirical curve — no alternative
families were derived from two-link dynamics, and no model selection was
performed. That work has not been done.

---

## 5. The projection layer: why ψ is not the wrist angle

Everything above lives in the image plane, and that is a problem the model has
not yet accounted for.

**The swing does not happen in the image plane.** It happens on a plane inclined
to it, and the camera sees a flattened shadow. Under projection, a vector at
in-plane angle α on a plane inclined by ι images at

    tan θ_img = tan α · cos ι

so **uniform rotation on the swing plane images as non-uniform angular motion** —
running fast near one axis of the ellipse and slow near the other. The β we fit is
therefore a *warp* of the true wrist mechanics, and the warp is large: the shaft's
projected length varies by a factor of 1.6–2.1 within a swing, which is an
out-of-plane excursion of 51–61°. This is a first-order effect, not a correction.

Three consequences:

1. **β conflates wrist mechanics with camera geometry.** Two golfers with
   identical wrists and different swing planes produce different curves.
2. **ψ = θ − φ is not the projection of any single 3-D angle.** The club sweeps a
   plane about the grip; the lead arm sweeps a different, shoulder-centred plane.
   The two are warped *differently*, so their image-plane difference is not the
   shadow of one anatomical wrist angle.
3. **Some of it is invisible.** Forearm roll about the shaft's long axis cannot be
   seen — a symmetric line does not change when you roll it. The out-of-plane
   component *can* be recovered. Be precise about which is which.

---

## 6. Recovering the plane — and the delta that matters

### How it is measured

Take the **shaft vector**, head minus grip, in image pixels. The club rotates
about the grip, so this vector sweeps a circle of fixed radius on the swing plane.
A circle on a plane inclined at ι images as an **ellipse with axis ratio cos ι**.
So:

    ι = arccos(minor axis / major axis)

fitted by a direct conic fit to the shaft vector's path. **This uses no
foreshortening model and no per-frame length — only the shape traced.** And
crucially, a plane also fixes *which side* of the node line each direction sits
on, so there is no per-frame sign ambiguity to resolve.

Two implementation points, both learned the hard way this session:

- **Fit `head − grip`, not the absolute head path.** The absolute path is grip
  translation *plus* club rotation, so it is not a planar closed curve about a
  fixed centre, and a conic fitted to it is badly conditioned. Repeatability
  improves from 9.2° to **0.6°** in the downswing when the grip is removed.
- **Normalise isotropically.** Scaling x and y by their separate standard
  deviations is an anisotropic map that changes both the axis ratio and the
  orientation — the two things being measured. Doing that wrongly moved ι by up
  to 40°.

### What it measures

| | inclination ι | across ten swings |
|---|---|---|
| backswing | **40.0°** median | 25.4–60.5° |
| downswing | **20.0°** median | 13.7–38.1° |
| **delta (back − down)** | **+17.3°** median | −7.6° to +45.6° |

Larger ι means the plane is more tilted away from the camera — a **flatter**,
more around-the-body swing. Smaller ι means the plane sits closer to the image
plane — a more **upright, steeper** swing.

So a **positive delta means the club steepens between backswing and downswing**,
and a negative delta means it shallows. **Eight of the ten swings steepen.**

### Precision versus accuracy — the distinction that makes this usable

- **Precision is excellent.** Split-half repeatability (fit odd frames, fit even
  frames, compare) is **0.6–0.7° in both phases**. Against a delta of 17°, and a
  between-swing range of 53°, the signal is far above the noise floor. Differences
  are trustworthy.
- **Absolute accuracy is unconfirmed.** The independent foreshortening estimate
  agrees to 7.8° median in the backswing but 14.9° in the downswing, and is
  systematically *higher* — consistent with the known clubhead-detector under-runs
  and with perspective inflation. So a single ι should not yet be quoted as a
  plane angle in degrees.

**This is exactly why the delta is the right quantity.** It is a difference
between two measurements made the same way on the same swing, so any common
calibration bias cancels. The over-the-top diagnostic needs the delta, not the
absolute plane, and the delta is the part that is already solid.

### What is not yet established

Being explicit, because this is the part worth getting right:

- **The sign convention has not been independently validated.** The reasoning
  above says larger ι = flatter, but I have not confirmed that mapping against a
  known-plane swing or a second camera. Before this is used as a coaching output,
  it needs one validation: a down-the-line view, or a swing with a deliberately
  flat and a deliberately steep version.
- **The axis ratio alone cannot tell which way the plane leans** — two planes
  tilted in opposite directions share a ratio. The **node line** (the ellipse's
  major-axis direction, now recorded as `node_*_deg`) carries that, and it has not
  yet been analysed.
- **One golfer, one club, one session.** The 53° between-swing range in the delta
  is either real swing-to-swing variability or a fault in the estimator, and ten
  swings cannot separate those.

---

## 7. Where this goes

The plane is not a correction to be applied and forgotten. If the model carries an
explicit plane, then fitting a *nominal* plane and looking at what is left over
turns the residual into a diagnostic: a structured departure measures how that
golfer's plane differs from the reference.

That is the hypothesis worth testing, and the test is concrete: the plane implied
by the model's error must agree with the plane measured here from the shaft
vector's ellipse, and must stay stable within a golfer across sessions and clubs
while moving when the swing genuinely changes. If it agrees, the model has earned
a coaching output. If not, the residual is noise wearing a physical name.

The near-term order of work:

1. **Validate the sign convention** — one down-the-line cross-check settles
   whether positive delta is steepening.
2. **Analyse the node line**, which carries the lean direction the axis ratio
   cannot.
3. **De-project β onto the recovered plane** and re-fit, so the mechanics layer
   sees wrist motion rather than a shadow.
4. **Derive the candidate families from two-link dynamics** — passive release,
   torque-limited release, soft handover — rather than picking shapes that look
   right, and score them on **θ**, including the out-of-domain test prior work
   never ran: fit the backswing, predict the release.
