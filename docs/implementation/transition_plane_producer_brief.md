# Brief: the transition_plane producer

**SHIPPED 2026-08-11.** `src/Analysis/shaft_plane.h` + `ShaftPlaneStage`
(`wrist_analyzer.cpp`, both profiles) + `analysis.club.plane` +
`m_transitionPlaneDelta` carrying the `transition_plane` axis. Gates:
`shaft_plane_test` (51 assertions) and `shaft_plane_corpus_test` (the §8.2
golden cross-check, skips when the corpus share is unmounted). Two numbers in
this brief were wrong and are corrected in place below — §4's synth coverage
and, by implication, §3's "still says something" claim. See §10 for what the
corpus run actually reported.

**The measure shipped `live` with a placeholder corridor, NOT normless** — §6's
"live-but-normless" end state was tried first and rejected on evidence (§11).
**And the producer carries a conditioning floor the reference does not** — three
of the reference's 33 fits are degenerate, and §12 is that story.

*2026-08-11. A face-on producer for the swing-plane transition delta — the
measure `over_the_top` has been waiting for. **Experimental and normless by
design**: it observes and accumulates but can never fire a fault, which is
what licenses its dual-channel (measured + synth) sourcing. Research
grounding: `docs/research/wrist_cock_model.md` §9 and §13 (Figure 2);
ontology spec: `docs/design/swing_fault_ontology.md` §8 step 2; reference
implementation: `tools/shaftlab/plane_probe.py` (`fit_ellipse`,
`head_path_plane`, `corpus`).*

---

## 1. What, and why now

`over_the_top` is the most visible fault in the library and the ontology's
worst structural gap: `observability: observable`, `confirmedBy: measured` —
and no measure. Ontology §8 names what it wants:

    axis: transition_plane   high tail -> over_the_top

The research note derived exactly this quantity and characterised it on the
corpus: the change in swing-plane inclination between backswing and downswing,
from the shaft vector's own ellipse. Split-half repeatability 0.6–0.7° against
a 17° median signal; the sign convention corroborated in-corpus (the golfer's
known fault reads as steepening where the estimator is best grounded); the
absolute plane angle NOT calibrated — only the delta ships. This brief turns
that into a producer.

## 2. The measurement, exactly

Per swing, two windows from the phase ladder: **backswing** = takeaway → top,
**downswing** = top → impact.

In each window, take the **shaft vector** `headPx − gripPx` (image pixels) of
every usable sample and fit a conic. The axis ratio gives the plane:

    ι = arccos(minor / major)        larger ι = flatter, smaller = steeper
    delta = ι_back − ι_down          positive = the club STEEPENED in transition

Port `fit_ellipse` from `plane_probe.py` with its two load-bearing choices
intact — each is worth tens of degrees and both were learned the hard way:

- **Fit the shaft vector, never the absolute head path.** The head path is
  grip translation plus club rotation — not a planar closed curve; fitting it
  degrades split-half from 0.6° to 9.2° (worst 47.6°).
- **Normalise isotropically** (one shared scale from the pooled radial spread,
  not per-axis standard deviations). Anisotropic scaling changes the axis
  ratio and the orientation — the two quantities being measured. The earlier
  anisotropic version was wrong by up to 40°.

Reject the window when the conic is not an ellipse (discriminant ≥ 0) or has
fewer than 12 samples. **Emit the delta only when both windows fit.** Also
record the major-axis direction (the node line) per window — it carries the
plane's lean, which the ratio cannot; unresolved research, but cheap to keep.

## 3. Sample tiers: both, tagged — an experimental-measure decision

This measure is **experimental**: it ships with no norms, so it can never
fire a fault (§7). That status buys a deliberate trade the research note
would forbid for a live measure: the synth tier is admitted as a channel,
with eyes open, because for an observational measure on sparse data *yield
beats independence*.

Fit **both channels** per swing and tag every emission:

- **measured** — `ShaftTrack2D::samples` rows with `headConf > 0`, excluding
  `ShaftSynthesized` (0x100): the selection of `plane_probe.load_run`. This
  is the honest channel and the headline value whenever both its windows fit.
- **synth** — `ShaftTrack2D::synth`, the C¹ Hermite through the P-anchors.
  Its plane is really *the plane implied by the located P-positions*, smoothly
  interpolated — an inference, not an observation (the note's §13 measured
  this tier against the tracker and found it a mirror: ρ = +0.98). But the
  anchors are well-validated at this run root, the interpolated arc always
  yields a conic, and where the measured channel fails on blur-thinned
  downswings the synth channel still says *something* traceable to real
  anchor geometry. When measured fails its gates, the synth fit is the
  emitted value, tagged as such.

Recording both when both exist is itself the experiment: every swing
accumulates a measured-vs-synth pair, so the question "does the synth plane
track the measured plane?" answers itself in production data rather than
needing another lab study.

Note the neighbour for contrast: `buildKinematicSeries`
(`kinematic_series.cpp`) prefers synth for its speed/lag series because C¹
density is what differentiation wants. That preference is correct there,
admitted *here* only under experimental status, and wrong for any measure
that fires faults — the promotion rule in §9 exists to keep that boundary.

## 4. Gating — availability, never sessionType

HARD rule (standing project convention): the producer runs whenever its
inputs exist and never consults session type. Inputs: a valid `ShaftTrack2D`
with a frame size, and ladder events for takeaway, top, impact. Absent any of
those, the producer emits nothing and says why in its trace.

Expected yield today: **33 of 61 corpus swings on the measured channel**
(Figure 2) — the misses are almost all the blur-thinned downswing arc
failing the conic — ~~and near-total with the synth fallback (every corpus
swing carries a synth series and a full anchor set)~~. **Corrected on
shipping: the synth fallback is not near-total.** The synth channel fits
both windows on **50 of 61**, so emitted coverage is **50 of 61** (30
measured + 20 synth-only) and **11 swings stay silent on both channels**.
(30, not the reference's 33 — see §12.)
Carrying a synth series is not the same as that series yielding two conics,
which is what the estimate needs. The two-channel design still does its job —
it makes the coverage gap visible instead of silent: a synth-tagged emission
*is* the record that the measured channel could not see that swing's
downswing — but the gap it leaves is 11 swings wide, not zero.

## 5. Quality travels with the value

Every emission carries its own quality, so consumers gate instead of trusting:

| field | meaning |
|---|---|
| `channel` | `measured` or `synth` — never omitted, never merged |
| `iotaBackDeg`, `iotaDownDeg` | the two inclinations (delta = back − down) |
| `nodeBackDeg`, `nodeDownDeg` | major-axis direction per window |
| `nBack`, `nDown` | samples in each conic |
| `conicResidBack/Down` | median conic residual |
| `ratioBack/Down` | axis ratio (minor/major) — the fit's CONDITIONING, added on shipping; see §12 |
| `splitHalfBackDeg/DownDeg` | **measured channel only** — odd/even-frame refit disagreement (needs ≥ 24 samples; a precision estimate, and **not** a validity check — see §12) |
| `anchorsBack/Down`, `anchorConfMin` | **synth channel only** — P-anchors spanning each window, and the weakest one |

**What the split-half cannot see (added on shipping).** This brief called it
"the honest per-swing error bar". It is not: it measures REPEATABILITY, not
VALIDITY. When an arc is too short to constrain the second conic axis, both
halves collapse to the *same* elongated fit and agree closely — a bad number,
confidently held. One corpus window with ι = 89.03° returned a split-half of
**0.00°**. Conditioning is gated on the axis ratio instead; a low split-half is
evidence about precision only. §12 has the full story.

**The split-half trap on synth.** Odd and even synth samples lie on the same
Hermite, so a split-half refit on that channel reads near zero — it measures
interpolation smoothness, not repeatability, and would fake excellent
quality. Never compute it there, and never compare quality across channels:
the synth channel's honest quality is its anchor count and anchor
confidence, because anchors are all the information it has.

The corpus reference (`docs/research/data/wrist_cock_model/
transition_plane_corpus.csv`) carries the measured-channel columns — the
golden file for §8's cross-check. A downswing split-half worse than 5° — or
too few samples to compute one — marks 7 of the 33 measured-channel corpus
fits low-quality; surface the flag, don't suppress the row.

## 6. Where it lands

- **Producer**: header-only `src/Analysis/shaft_plane.h` (detector-math
  convention, unit-tested like `shaft_kinematics.h`), called from the face-on
  metric build path beside `buildKinematicSeries`. Emits a `MetricSeries` per
  scalar (`transitionPlaneDelta`, plus `swingPlaneIotaBack`/`Down`), single
  sample stamped at the top-of-backswing event. Where the quality fields live
  (side struct on the track vs. companion metric keys) is the implementer's
  call — the contract is only that they persist and reach the CSV/trace.
- **Measure**: `core.json` row `m_transitionPlaneDelta`, `kind: provided`,
  `metricKey: transitionPlaneDelta`, reducer `at` the transition anchor,
  unit °, `highMeans`: "the club steepened between backswing and downswing —
  the over-the-top direction", label carrying its experimental status
  plainly. Enters as `status: planned` with the schema; flips to `live` when
  the producer lands (the model may lead the producer; that is the
  diagnostic model's design, not a defect). Live-but-normless is the
  intended end state for now: the measure is visible, accumulating, and
  incapable of firing.
- **Condition wiring**: ontology §8's own two steps — retag `over_the_top`
  per its plan, then attach the measure as the `transition_plane` axis, high
  tail firing the fault. The §8 text is the spec; do not improvise edges.

## 7. Norms: none from this corpus

One golfer. The healthy-fit distribution (median +6.4°, p10–p90 −15…+25°,
session-structured) is *his*, partly confounded with per-session coverage
(delta vs. backswing sample count: ρ = +0.45, n = 24 — unresolved). Ship the
measure normless or with a provisional wide band clearly flagged per
`diagnostics_norms` conventions; real bands wait for more golfers — the first
shallower (better) player is simultaneously the sign falsifier, the norm
seed, and the between-golfer test. Any composed weight stays in 0..1.

## 8. Acceptance

1. **Unit tests** (`shaft_plane_test`): synthetic ellipse points → exact ι;
   short/degenerate arc → no emission; an anisotropic-normalisation
   regression pin (the 40° failure mode must stay dead).
2. **Golden cross-check**: run the producer over the corpus at
   `stagegate/corpm3-off` and match `transition_plane_corpus.csv` on the
   measured channel — same 33 swings, ι and delta within 0.1°, same 28
   falling back for the same reasons. One run, at the end.
3. **The mirror stat, recorded not gated**: on swings where both channels
   fit, report median |Δdelta| and rank correlation between them. No
   pass/fail — this number *is* the experiment, and the first corpus run
   sets its baseline.
4. **Fallback is visible**: a synth-tagged emission shows *why* the measured
   channel failed (which window, which failure) in the trace, not silence.

**Definition of done** — implementation: `shaft_plane.h` + tests + producer
wiring + `core.json` rows + the §8 retag. Doc: this brief marked shipped, and
the research note's §13 updated if the corpus yield or numbers move.

## 9. Out of scope, explicitly

- **Absolute ι as a coaching output** — uncalibrated (§9 bounds a 64° body-
  depth bias; the foreshortening cross-check disagrees by 14.9° in the
  downswing). Only the delta is defensible.
- **The node line as a headline** — recorded, not interpreted.
- **Relaxed conic gates or the absolute head path** as yield rescues on
  either channel — both are measured mistakes (9.2° split-half; the 40°
  normalisation error).
- **Promotion while synth-sourced.** The experimental bargain is: synth
  buys yield *because* the measure cannot fire. The day this measure gets
  norms and an edge to `over_the_top`, the synth channel must either have
  earned its place (the §8 mirror stat, accumulated across golfers, showing
  it tracks the measured channel) or be dropped from the headline value.
  Norms over a channel that echoes the ladder is how a fault detector ends
  up detecting its own anchor placement — that promotion needs its own gate,
  not a default.

## 10. What shipped, and what the corpus run reported

**The port is exact.** `shaft_plane_corpus_test` reads the 61 existing
`result.json` files at `stagegate/corpm3-off` — no pipeline re-run, nothing
written under the corpus root — and reproduces the reference on every column:
the same fitted **set** bar three named exclusions (§12), worst ι/δ/node error **0.0049°** against the
0.1° tolerance, sample counts exactly equal, residuals inside the file's 4-dp
rounding, and split-halves agreeing to **7.7e-13** (the sharpest check
available, since each involves four extra fits). The 28 misses match by
reason: 25 downswing-window, 1 backswing-window
(`2026-07-04_…swing_0012`), 2 with no usable shaft samples.

Two implementation notes worth keeping. The conic fit uses a **one-sided
Jacobi SVD on the design matrix**, not an eigendecomposition of DᵀD: 25 of the
28 misses are discriminant rejections and the nearest accepted ellipse sits at
b²−4ac = −7.9e-4, so squaring the condition number was not a margin worth
spending. And the node line needs a **floored** modulo — `std::fmod`
truncates where Python's `%` floors, which puts every negative node 180° out.
Both are pinned by tests.

**The mirror stat's first baseline (§8.3), recorded and not gated:**

| | |
|---|---|
| measured-channel yield | 30 / 61 |
| synth-channel yield | 50 / 61 |
| both channels fitted | 30 |
| median \|Δdelta\| between channels | **13.4°** |
| Spearman ρ (measured, synth) | **+0.377** |

**Read that as a warning, not a reassurance.** A 13.4° median disagreement
against a 17° median signal, at ρ = +0.377, is not a mirror — and it is far
weaker than the ρ = +0.98 the research note's §13 reports for its own
tracker-vs-synth comparison, which was measuring a different thing. On this
evidence the synth channel does *not* yet track the measured plane well
enough to survive §9's promotion gate. The gate stands as written; this is the
number it will be argued against, and it starts out unfavourable.

**End-to-end, on GOLFSIMPC.** The Mac has no swing carrying video (the corpus
dirs are `result.json`-only exports), so the first real emission was run on the
studio host against `corpus-0710-fusion/swing_0001`:

    club.plane  valid true, channel 0 (measured)
      measured  δ +13.76°  ι 34.58 → 20.82  n 62/41  splitHalf 1.04° / 2.06°
      synth     δ  +0.74°  ι 18.38 → 17.65  n 143/67 splitHalf −1 / −1
                                            anchors 3/4, anchorConfMin 0.45

Measured took the headline, both channels were recorded, and the §5 quality
split held in production: no split-half on synth, no anchors on measured. The
weak mirror reproduced on data that had no part in producing it — a 13.0° gap
on this swing against the 13.7° corpus median.

**One thing that run caught which no test had.** The observed ladder came back
`Address, Takeaway, ShaftParallelBack, MidBackswing, Top, ArmParallelDown,
Delivery, Impact, ShaftParallelThrough, Finish` — **with no Transition event at
all.** The `at transition` reducer therefore has nothing to bind to except the
phase label the producer stamps on its own sample; stamping `Phase::Top` (the
same instant) would have resolved nothing, silently and forever. Now pinned
both ways in `measure_sample_test`: the label resolves, and the shared
timestamp alone does not.

## 11. Why the measure is `live` with a corridor, and not normless

§6 proposed "live-but-normless" as the end state. That was implemented first
and then abandoned, because a normless measure turns out to be worse on every
axis that matters:

- **It could not be `live`.** `core_pack_test:614` asserts every LIVE corridor
  signal resolves a norm, so normless forced `status: planned` — which in this
  model has always meant *"nothing produces this yet"*. `live_measure_source_test`
  encodes exactly that (*"Nothing that has no producer reports a number"*), and
  a planned measure that DOES produce a number breaks it. Verified by injecting
  the metric into the `rich_7iron` fixture: `m_transitionPlaneDelta (planned)
  produced a value` → FAIL. It passed only because the fixtures pre-date the
  metric, i.e. it was a trap armed for whoever regenerated them.
- **It left both conditions permanently Unavailable.** With no corridor the
  engine reduces the value and then discards it, so `over_the_top` and
  `shallowing` could never be assessed — and the review strip would have
  labelled them *"metric not produced on this capture"*, which is false.
- **The corridor costs nothing in exchange.** Placeholder mu 0, sigma 25°: the
  surfacing edge (goodMaxZ = 2) is 50°, which is 1.10× the largest
  well-conditioned delta in the corpus (+45.56°, a healthy swing with axis
  ratios 0.49/0.97 and split-halves 1.04°/0.26°). Measured on the `rich_7iron`
  fixture, coverage rises **51 → 53** conditions assessable while the fired
  count is unchanged across the observed range, moving only well outside it. So
  the band is a real corridor that demonstrably fires — just not on anything
  this corpus has seen.

**Sigma was 30 before §12.** It was sized against a −58.46° reading that turned
out to be a degenerate conic. Once the conditioning floor refused that fit the
corpus maximum fell to 45.56° and the band came down with it. The lesson is in
the norm's `citation`: do not re-widen without checking that whatever justifies
it is a fit and not an artifact — **the split-half cannot tell you**.

## 12. The conditioning floor — a needle is not a plane

Added on shipping, after the corpus numbers were questioned rather than
accepted. The reference reports deltas of **−58.5°, +56.7° and +46.6°**. No
golf swing changes plane by 58°, and those three are not swings:

| swing (all 2026-07-04) | δ | bad window | ι | minor/major | n | its split-half |
|---|---|---|---|---|---|---|
| `…swing_0005` | −58.46 | down | 81.72° | 0.14 | 40 | *n/a — a sub-fit failed* |
| `…swing_0006` | +46.59 | back | 82.89° | 0.12 | 64 | 4.29° |
| `…swing_0010` | +56.74 | back | **89.03°** | **0.017** | 39 | **0.00°** |

An axis ratio of 0.017 is a straight line to within 2%. An arc too short or too
sparse to constrain the second axis still admits a conic — an arbitrarily
elongated one threaded through the points — and ι = arccos(minor/major) then
runs toward 90° and drags the delta with it. In every case it is the *sparser*
window (39–64 samples against 100–200 in a healthy backswing), and all three are
from the session §12 of the research note already flags as pathological.

**`shaft_plane.h` therefore refuses a window whose axis ratio falls below
`kMinAxisRatio` = 0.26 (ι = 75°)**, reported as `ConicReject::IllConditioned`.
This is *not* the relaxed-gate yield rescue §9 forbids — it is the opposite, a
floor on conditioning, and it costs yield rather than buying it. The threshold
sits at the gap the corpus shows: of 66 window fits, 53 are below ι 45°, six
between 45° and 60°, four between 60° and 75°, then the three needles.

**Consequences, all pinned by name in `shaft_plane_corpus_test`** so the
divergence from `plane_probe.py` reads as a decision and never as drift:

- measured-channel yield **33 → 30**; the three dropped swings are asserted by
  name and by reject reason, and the other 30 stay numerically identical.
- the floor applies to the half-fits too, so exactly one split-half is refused
  (`2026-06-11_…swing_0007` down, reference value 6.30° — already past the 5°
  mark §5 uses to flag a row as low quality). "Cannot compute" is more honest
  than an error bar derived from two needles.
- emitted coverage **52 → 50 of 61**; the mirror stat moves 13.7° → **13.4°**
  and ρ +0.435 → **+0.377**, i.e. slightly *worse*, so the artifacts had been
  flattering it.
- the placeholder corridor came down from σ 30 to **σ 25**, because σ 30 had
  been sized against the −58.46° artifact.

`ratioBack`/`ratioDown` now persist per window in `analysis.club.plane`, since
conditioning is the one quality the split-half provably cannot supply.

**The reference implementation and the research note keep the three artifacts.**
`plane_probe.py` has no such floor, so Figure 2, the 33/61 yield and the
delta spread quoted in `wrist_cock_model.md` §9 all still include them. That is
a known, recorded difference, not a thing to quietly reconcile.
