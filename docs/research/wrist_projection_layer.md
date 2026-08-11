# The truth upgrade and the projection layer

*Written 2026-08-11. This is Task 0 and the projection layer of
[`docs/implementation/wrist_model_brief.md`](../implementation/wrist_model_brief.md).
No mechanics family is fitted here; nothing ships. Prior work is in
[`wrist_cock_model.md`](wrist_cock_model.md).*

Every number below is reproducible from a command in this document, against a
**named** run root.

**The result: the swing plane is recoverable, precisely, and the backswing-to-
downswing shift is measurable.** Fitting an ellipse to the *shaft vector*
(head − grip) gives the plane inclination with **0.6–0.7° split-half
repeatability** in both phases: ι = 40.0° median in the backswing, 20.0° in the
downswing, a **+17.3° median shift**, eight of ten swings steepening. The plane
also fixes the out-of-plane sign, so the per-frame ambiguity does not arise.
**θ, the shaft angle, is what all of this exists to explain.**

The full model definition — every variable, and how θ, φ and ψ compose — is in
[`wrist_model_definition.md`](wrist_model_definition.md).

The truth upgrade, by contrast, turned out to be mostly already in use.

## How to reproduce

```
python3 tools/swinglab/fusion_truth.py --audit \
    --lab-root /mnt/swingdata/shaftlab/lab/tape_20260705 \
    --run-root /mnt/swingdata/stagegate/corpm3-off \
    --corpus   /mnt/swingdata/Mark-Liversedge --out <dir>          # gates A1-A3

python3 tools/swinglab/wrist_cock_fit.py /mnt/swingdata/stagegate/corpm3-off \
    --corpus /mnt/swingdata/Mark-Liversedge --out <dir> --knots 15 \
    [--lab-root /mnt/swingdata/shaftlab/lab/tape_20260705] [--holdout session]

python3 tools/shaftlab/plane_probe.py census --out <dir>           # gate B0
python3 tools/shaftlab/plane_probe.py planes --out <dir>           # gates B1-B6
```

`--knots 15` is load-bearing: the CLI default is 13, the shipped table used 15,
and a run at the default is not comparable to any published number. The run root
matters too — `stagegate/final1` carries no `club` block and is rejected outright;
`corpoff-live`, `off1`, `off2`, `on1` are missing the takeaway phase and lose all
61 swings. `corpm3-off` gives 59/61.

## Baseline at this run root does not match the published numbers

| form | published truth | here (`corpm3-off`) |
|---|---|---|
| F0 shipped table | 74.3° | **77.6°** |
| F2 refit on seconds-before-impact | 20.9° | **26.8°** |

Same 805 labels, 471 scored residuals against the doc's 463. The published
figures were computed at an unnamed run root and do not reproduce at this one.
Nothing downstream of a run root should be quoted without naming it.

---

## Truth provenance: the upgrade was mostly already in use

**Gate A1.** For the 2026-07-05 session, `truth.json` *is* the instrumented band
tier, verbatim — not a similar set, the same observations:

| | |
|---|---|
| swings whose label count equals their band-row count exactly | **9 of 10** |
| max head-position difference on those rows | **0.000 px** |
| pooled median \|Δθ\| (gate A2, threshold 0.05°) | **0.0063°** — PASS |

Only `swing_0001` carries genuine hand labels (121 = 60 band + 61 hand). Across
the lab-covered swings, **565 of the corpus labels are instrumented band rows and
61 are hand-placed.**

So the brief's "better truth already exists, unused" is substantially wrong. What
is genuinely new is the **ray tier — 468 rows, 283 of them pre-impact** — the fast
frames a human cannot label, plus the ability to separate the tiers and to know
which label came from where.

That distinction is not pedantry. Grading "hand labels" against "fusion band"
without it compares a set with itself and reads the agreement as corroboration.

## The φ audit: an alarming disagreement that dissolves under measurement

**Gate A3.** The harness's φ and `anchors.csv`'s φ disagree badly — downswing p90
pooled median **51.9°**, individual swings reaching 78°. Taken at face value that
would exceed the entire residual the model is trying to explain, and would mean
every prior number was φ-limited.

It is not. Three hypotheses were tested and killed:

- **Interpolation across a sparse pose?** No — the disagreement is *largest* at
  the smallest gaps (median 20.3° at 0–3 ms, falling to 11° at 10–20 ms), which is
  backwards. The pose is not sparse either: 215 frames at 6.9 ms spacing is
  near per-frame over a ~1.5 s window.
- **`smooth_angle`'s moving average?** No — raw and smoothed agree to 0.2°
  (downswing median 18.0° vs 18.2°).
- **A timebase offset?** No — the best per-swing shift scatters −9 to +40 ms and
  improves the median only 13.7° → 12.2°.

The disagreement is real and irreducible: two pose extractions of the same video
differ that much. **But a disagreement never says which side is wrong.** Each
channel's own jitter does, measured about a local quadratic on its own samples:

| φ source | backswing | downswing |
|---|---|---|
| production pose (raw, unsmoothed) | 3.13° | **1.79°** |
| `anchors.csv` | **0.12°** | 3.51° |

`anchors.csv` reading 0.12° in the backswing is too smooth to be a measurement —
that is the signature of interpolation between sparser pose samples, and where
the swing is fast it cannot hide, degrading to 3.51° against production's 1.79°.

**Verdict: keep production φ (`--truth-phi harness`). Production φ jitter of 1.79°
is far inside the ~20° residual, so prior numbers are NOT φ-limited.** The biggest
risk the plan carried is closed, in the reassuring direction.

*(Methodological note: the first version of this comparison scored production's
**smoothed** φ against anchors' unsmoothed and got 0.23°. Smoothing suppresses
exactly the quantity being measured. The table above uses raw φ on both sides.)*

## Re-baseline

Leave-one-swing-out, `corpm3-off`, `--knots 15`, domain to-impact. Every truth
column printed beside its frame-averaged partner — the report generator now
*refuses* to emit one without the other, because that trap has bitten before.

| form | track (frame-avg) | truth_hand | truth_band | fuse_band | fuse_ray |
|---|---|---|---|---|---|
| F0 shipped | 78.2° (9154) | 59.5° (44) | 36.7° (307) | 37.0° (307) | 50.9° (283) |
| F1 progress axis | 72.7° | 46.4° | 52.1° | 52.2° | 47.3° |
| **F2 seconds-before-impact** | **40.0°** | **15.3°** | **12.1°** | **12.2°** | **20.5°** |
| F3 + linear in φ | 39.1° | 21.3° | 16.1° | 16.1° | 23.9° |
| F4 + shape constraints | 39.6° | 23.1° | 17.1° | 17.1° | 22.7° |
| F5 parametric (7 params) | 40.2° | 22.8° | 16.8° | 16.8° | 24.7° |

**Gate A4 passes exactly.** `truth_band` and `fuse_band` are the same observations
routed through two different loaders — truth.json in radians via the corpus path,
the fusion CSV in degrees via the frame index. They agree to ≤0.1° on every form
at identical n=307. The timebase join is verified, not assumed.

**Gate A5 — the new material.** `fuse_ray` is 283 pre-impact rows the corpus never
had. F2 scores **20.5° there against 12.2° on band**. Ray-tier label noise is
~1.7°, negligible against a 20° residual, so that gap is a statement about the
*model*: it is 1.7× worse in the fastest frames, precisely where hand labels could
never go and where the release happens.

**Gate A6 — the session leak.** Leave-one-session-out withholds all ten swings:

| form | fuse_band swing-out → session-out | leak |
|---|---|---|
| F2 | 12.2° → 12.9° | **0.7°** |
| F3 | 16.1° → 21.0° | **4.9°** |

F2 barely notices. F3 — the φ-term — leaks seven times as much. `wrist_cock_model.md`
rejected F3 for absorbing between-swing variation it could not predict out of
sample; that was an inference from envelope collapse, and it is now a direct
measurement on instrumented truth. **F3 is also worse than F2 on every instrumented
channel** (16.1° vs 12.1° band, 23.9° vs 20.5° ray), where against hand labels it
had looked *better*. The label distribution was flattering it.

---

## Degrees of freedom: what is identifiable, and what is not

ψ = θ − φ is one image-plane scalar, but the wrist is a 2-DOF joint plus forearm
pronation. A latent variable earns its place only when an independent observable
pins it — free DOFs always improve fit, which is how F3 failed. So this is an
observable count:

| observable | source | pins |
|---|---|---|
| **clubhead path shape** | **head track (measured; `lenPx` derives from it)** | **the plane — inclination AND sign** |
| θ | fusion `theta_deg`, samples | shaft image direction |
| φ | pose elbow→grip | forearm image direction |
| shaft foreshortening | `lenPx`, `s_px_mm` | corroboration of the plane |
| forearm foreshortening | elbow→grip px, every pose frame | corroboration, and the forearm's tilt |

**The plane is the route; foreshortening is the check.** Once the plane is known,
a shaft direction's out-of-plane component and its *sign* both follow from where
it sits relative to the node line — there is no per-frame branch to pick. That is
the whole force of the brief's "over-determined hidden state" argument.

The forearm channel is real and worth recording — it foreshortens *harder* than
the shaft (**2.07–2.51×** pre-impact against 1.58–2.08) and exists on every pose
frame — but it is supporting evidence, not the mechanism.

**Declared unidentifiable and never fitted:** axial roll of the shaft and forearm
pronation. A line carries no roll. The 133-point pose does carry 21 hand landmarks
that could in principle see them, but the hand keypoints are not trustworthy
through a fast swing and are used here only as an accuracy cross-check.

## The projection layer

**Gate B1 — PASS, on thin data.** The one test that checks the data rather than a
model, and the only one with an answer known in advance:
`lenPx / s_px_mm + r0_mm` must equal the 940 mm club length.

> pooled n=36 · median **946 mm** (0.6% off) · p10–p90 828–987 (17% of median)

The two radius channels are genuinely independent — a stage-2 head detector and
tape band spacing — and they agree. But **n=36 is the whole overlap**: fusion
covers frames ~397–600 while `lenPx` lives in the slow phases, so there are only
2–7 co-occurring frames per swing.

**Gate B0 — the `lenPx` re-derivation.** The brief's "189→414 px, a 2.2× range" is
unsourced and does not reproduce. At `corpm3-off`, within ±1.1 s of impact:

> ratio p95/p05 **1.58–2.08** (median 1.77) → χ **51–61°**

So "out-of-plane excursion approaching 60°" survives; the specific numbers do not.
The finding that matters is different and worse: **the absolute level moves 36%
between swings of one session with one club, minutes apart** (p95 runs 287→390 px).
No plane explains that. It is stage-2 self-fit drift, and it must be resolved
before `lenPx` is trusted as a plane observable at all.

**Gate B2 — the plane is recovered, with high precision and unconfirmed absolute
calibration.** The club rotates about the grip, so the **shaft vector**
(head − grip) sweeps a circle of fixed radius on the swing plane, which images as
an ellipse of axis ratio cos ι. The plane comes from that *shape* — no
foreshortening, no per-frame length, and no sign ambiguity.

| | inclination ι | across ten swings | split-half |
|---|---|---|---|
| backswing | **40.0°** median | 25.4–60.5° | **0.7°** (max 3.9°) |
| downswing | **20.0°** median | 13.7–38.1° | **0.6°** (max 9.2°) |
| **delta (back − down)** | **+17.3°** median | −7.6…+45.6° | 8/10 steepen |

Corroboration against foreshortening, **per phase**: backswing agrees to 7.8°
median (8/10 within 10°) — **pass**; downswing to 14.9° (4/10) — **fail**. The
bias is one-signed, foreshortening always reading higher, which is what the known
clubhead-detector under-runs and perspective inflation would both do. Treat the
ellipse as the estimator and foreshortening as weak corroboration only.

**Precision is not accuracy, and the distinction is what makes this usable.**
Repeatability of 0.6–0.7° against a 17° delta means *differences* are solid;
absolute ι is not yet calibrated and should not be quoted as a plane angle. The
over-the-top diagnostic needs the delta, where common bias cancels.

> **Corrections, in the order they were made.**
> 1. The first version recorded B2 as *unrunnable*, arguing that
>    `head = grip + lenPx·u(θ)` so the path "carries no new information". That
>    conflated **provenance** with **structure**: whether the 3-D path is *planar*
>    is a claim the algebra does not grant, and it is testable. The causal
>    direction was also backwards — stage 2 *detects the clubhead*, and `lenPx` is
>    derived from that detection.
> 2. The second version fitted the conic in **anisotropically normalised**
>    coordinates, which changes an ellipse's axis ratio and orientation — the two
>    quantities being measured. It reported ι = 58.5°/39.1° and a 4.3° agreement
>    with foreshortening. **Those numbers were artefacts and are withdrawn.**
> 3. The same version fitted the **absolute head path**, which is grip translation
>    plus club rotation and therefore not a planar closed curve. Split-half
>    repeatability was 9.2° in the downswing, worst case 47.6°. Fitting the shaft
>    vector instead brought it to 0.6°.
> 4. B2 originally compared a per-phase ι against a χ pooled over the whole swing —
>    different quantities over different domains. Now matched per phase.

**Gate B4 — INCONCLUSIVE, and the honest reading is not a pass.** M1 (single fixed
plane) was supposed to fail, reproducing `length_model`'s documented ~2× symmetry
violation. It does not: 5.0% median relative error against M2's 4.4% and M3's
5.0%. The ordering is right by 0.6 points, which is nothing. The cause is coverage
— the band tier spans only frames where the tape is legible, so the antiparallel
shaft directions that produced the original violation are largely absent. **This
channel does not challenge M1, so it can neither acquit nor convict it.**

**Gates B5/B6 — the per-frame de-projection is superseded.** Pre-impact, ψ_img
spans 87.0° and ψ₃ spans 46.5° with 0.8° jitter; χ is smooth (2.5° jitter) and
adequately conditioned (ψ₃ bottoms out at 23.4°). Neither pre-registered failure
mode fired.

But this route resolved the out-of-plane sign per frame, pinning both signs to +1
as a placeholder — and **that whole approach was the wrong question.** With the
plane recovered above, the sign is not a free per-frame choice at all: it follows
from which side of the node line each direction sits on. The per-frame
reconstruction should be rebuilt on the plane, and ψ₃/χ as computed here should be
treated as superseded rather than provisional.

The forearm channel stands on its own as a measurement — elbow→grip foreshortens
**2.07–2.51×** pre-impact, harder than the shaft's 1.58–2.08, on every pose frame
— but it is a *supporting* observable, not the route to the plane.

**The perspective bound.** `rho_plane` assumes orthographic projection while
`s_px_mm` and `lenPx` both fold in the pinhole depth scale `f/Z`, identically — so
B1 cannot separate them. Shoulder width from `skeleton.csv` bounds it: its
excursion implies up to **64°** of apparent out-of-plane angle that is really body
depth. That is a bias bar on every angle above. Nothing was subtracted.

---

## What did not work

- **The truth upgrade, as conceived.** Two-thirds of the "new" truth was already
  being graded on. The real gain is the ray tier and provenance, not volume.
- **My first reading of the head-path ellipse.** I ruled out the brief's primary
  cross-check by confusing provenance with structure, and reported the resulting
  per-frame sign ambiguity as the session's blocker. Both were wrong: the path is
  planar, the ellipse recovers the plane, and the plane fixes the sign. The
  brief's design was right and the error was mine.
- **The M1 negative control.** Unavailable on this channel for lack of
  antiparallel coverage — not satisfied, unavailable.
- **`anchors.csv` as a better φ.** Interpolated, and worse than production where
  the swing is fast.
- **Three explanations for the φ disagreement** — interpolation gaps, smoothing,
  and a timebase offset — each tested and killed before the jitter measurement
  settled it.
- **My own first φ comparison**, which scored a smoothed channel against an
  unsmoothed one and flattered production by an order of magnitude.
- **The per-frame sign disambiguation**, attempted and then made moot — it was
  the wrong question, since a plane determines the sign globally.

## Limits, on the face of the results

- **One golfer, one club, ten instrumented swings from a single session.** The
  fusion channel cannot speak to between-session or between-golfer behaviour, and
  nothing here can answer "which family generalises".
- **The B1 overlap is 36 frames.** Every claim that the two radius channels agree
  rests on 2–7 frames per swing.
- **Fusion can never re-baseline the backswing** — it starts at −0.8 s.
- **`R_fore` has no catalogue length.** Its scale is a censored upper-percentile
  estimate; every χ inherits that assumption.
- **Orthographic projection is assumed and is wrong** at up to the 64° apparent
  angle bounded above.
- **The hand axis is unreliable** and was used only as an accuracy indication.
- **s05/s10** are flagged in the dataset's own `RESULTS.md` for stage-2 counts —
  that is `lenPx`, not fusion θ; their band counts (68, 55) are healthy. Kept in
  Part A, treated as suspect in Part B.

## Where this leaves the brief

Task 0 is complete but smaller than advertised. **The plane is measurable to
0.6–0.7°**, and the backswing-to-downswing delta — the coaching quantity — is
+17.3° median with eight of ten swings steepening, far above that noise floor.
Absolute calibration is not confirmed: foreshortening corroborates the backswing
(7.8° median) but not the downswing (14.9°), reading systematically high. B1
passes on thin data; B4 remains unavailable for lack of antiparallel coverage.

**θ is the objective.** The plane exists to explain the shaft angle — the quantity
the tracker predicts as `θ̂ = φ + chir·β̂`. The next session should:

1. **Validate the sign convention** — that larger ι means flatter — with one
   down-the-line cross-check, and analyse the **node line** (now recorded), which
   carries the lean direction the axis ratio cannot. Only then is the delta a
   coaching output.
2. **Rebuild the shaft reconstruction on the plane**, not on per-frame
   foreshortening. The plane determines the out-of-plane sign, so the ambiguity
   this document originally called the blocker does not arise.
2. **Fit the mechanics families to the de-projected wrist angle** and score them
   on θ, including the out-of-domain test the brief specifies (fit the backswing,
   predict the release) that prior work never ran.
3. **Test the residual-as-plane-diagnostic hypothesis** — whether a golfer's plane
   departure from a nominal plane is what the model's error measures. The
   per-swing plane estimates here are the reference that makes that testable.

Two measurement faults should be fixed alongside, neither blocking: the 36%
between-swing drift in `lenPx` level, and finding a channel with enough
antiparallel coverage to genuinely challenge a single-plane model.
