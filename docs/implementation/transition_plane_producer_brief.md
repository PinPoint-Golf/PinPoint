# Brief: the transition_plane producer

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
failing the conic — and near-total with the synth fallback (every corpus
swing carries a synth series and a full anchor set). The two-channel design
makes the coverage gap visible instead of silent: a synth-tagged emission
*is* the record that the measured channel could not see that swing's
downswing.

## 5. Quality travels with the value

Every emission carries its own quality, so consumers gate instead of trusting:

| field | meaning |
|---|---|
| `channel` | `measured` or `synth` — never omitted, never merged |
| `iotaBackDeg`, `iotaDownDeg` | the two inclinations (delta = back − down) |
| `nodeBackDeg`, `nodeDownDeg` | major-axis direction per window |
| `nBack`, `nDown` | samples in each conic |
| `conicResidBack/Down` | median conic residual |
| `splitHalfBackDeg/DownDeg` | **measured channel only** — odd/even-frame refit disagreement (needs ≥ 24 samples; the honest per-swing error bar) |
| `anchorsBack/Down`, `anchorConfMin` | **synth channel only** — P-anchors spanning each window, and the weakest one |

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
