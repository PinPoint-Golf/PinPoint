# Lower-body metrics from a face-on camera

**Audience**: developers and content authors working on the lower-body half of the diagnostics model
**Code**: `src/Analysis/lower_body_metrics.{h,cpp}`, `LowerBodyMetricsStage` (`wrist_analyzer.cpp`), `LowerBodyMetricProvider`
**Content**: `src/Resources/diagnostics/core.json`, `norms.json`, `references.json`
**Status**: producer live; content shipped; the DETECTION ENGINE that would turn it into findings is still dormant (`diagnostics_developer_guide.md` §8)
**Written**: 2026-08-02

---

## Contents

1. [What prompted this](#1-what-prompted-this)
2. [The evidence, honestly](#2-the-evidence-honestly)
3. [The geometric problem, and the fix](#3-the-geometric-problem-and-the-fix)
4. [The frontal-plane rule](#4-the-frontal-plane-rule)
5. [What ships](#5-what-ships)
6. [Units, and why not centimetres](#6-units-and-why-not-centimetres)
7. [The content, and where the uncertainty is recorded](#7-the-content-and-where-the-uncertainty-is-recorded)
8. [The unit defect this work uncovered](#8-the-unit-defect-this-work-uncovered)
9. [What is deliberately still planned](#9-what-is-deliberately-still-planned)
10. [Open questions](#10-open-questions)

---

## 1. What prompted this

A coaching observation, put to us directly:

> From a face-on view, if the lead knee moves laterally toward the trail knee at P4 it strongly
> suggests lack of internal rotation and likely lack of depth in hip rotation — and is really useful
> when there is no down-the-line view to corroborate. It is also generally seen with lead heel lift
> at P4, especially in older players with limited hip mobility.

The observation is a good one and the instinct behind it — that a face-on camera can be made to
answer a question people normally reach for a second camera to settle — is exactly right for this
product. **No down-the-line frames are analysed anywhere in the app today**: `CameraPlacement::DownTheLine`
exists in `analysis_stage.h` and `CaptureCapabilities::fromJob()` never populates it. So "when there
is no DTL view" is not the fallback case. It is every case.

What the observation cannot do, as stated, is be measured directly. §3 is why.

---

## 2. The evidence, honestly

Split into what a source establishes and what is coaching doctrine, because the pack's provenance
tiers force that split anyway and getting it wrong is how `practice` content ends up looking cited.

### Well supported

**Trail-hip internal rotation is a real backswing requirement.** The pelvis turns away over a
planted trail foot, which the trail femur can only accommodate by rotating internally.

- **Kim, You, Kwon & Yi**, *Am J Sports Med* 2015 — [10.1177/0363546514555698](https://doi.org/10.1177/0363546514555698),
  PMID 25398245. 30 male professionals split at <20° internal rotation against ≥30°. The limited
  group showed significantly greater lumbar flexion, axial rotation in both directions, right
  lateral bending, and greater pelvic posterior tilt. **Already in `references.json` as `ref.kim2015`.**
- **Vad et al.**, *AJSM* 2004 — `ref.vad2004`, already cited. Decreased **lead** hip internal
  rotation associated with low-back-pain history in 42 professionals; **no significant trail-side
  finding**, which is why the library's two hip-restriction conditions are separate and why only the
  lead one carries an injury note.
- **Murray et al.**, *Phys Ther Sport* 2009 — roughly a 10° lead-hip internal-rotation deficit in
  golfers with low back pain.

### Two corrections to the observation as put to us

**(a) The lead-hip half is the wrong hip for P4.** The backswing demand is trail hip *internal*
rotation with lead hip *external* rotation. Lead-hip internal rotation is a downswing and
through-impact demand. The pack already had this right — `limited_trail_hip_ir` edges to
`hips_under_rotated_at_top` and `sway`, while `limited_lead_hip_ir` edges to `early_extension` and
`late_buckle` — so nothing needed changing, but the distinction is worth stating because the
face-on knee signature invites exactly this conflation.

**(b) The one empirical test of screen-against-visible-fault is negative.** Gulgin, Schulte &
Crawley, *J Strength Cond Res* 2014;28(2):534–9, PMID 24476744, DOI `10.1519/JSC.0b013e31829b2ac4`.
36 golfers, twelve physical screens against fourteen swing faults. The toe-touch, the bridge and the
overhead deep squat reached significance for early hip extension, loss of posture and slide. **The
lower-quarter rotation screen — the one that isolates hip internal rotation — was associated with no
swing fault at all.**

That does not refute the mechanism, which Kim 2015 measures directly. It caps how strongly a
restriction may be said to produce a fault you can *see*.

**This paper SUPPORTS the design rather than undermining it**, and that is why it is cited rather
than merely noted. It is the reason every edge from a hip-rotation restriction to a visible
characteristic is `moderate` and not `strong`, and the reason `lead_knee_drifts_in_at_top` is
authored as an observation with four competing causes ranked behind it instead of as a detector that
concludes a tight hip. A library that had not read this would have written one `strong` edge and
been wrong with confidence.

> **It is in `references.json` as `ref.gulgin2014`, with its title abridged.** The published title
> names a commercial screening system, and `core_pack_test` greps the raw bytes of all three content
> files for exactly that — rightly: the movements are common property of the clinical literature,
> and the packaging of them into a named system is somebody's product. So the brand is replaced by a
> bracketed editorial redaction, the identifier is left untouched so the record resolves to the full
> title in one click, and `establishes` declares the redaction rather than letting it pass silently.
> **Redact the brand, never the finding.** `core_pack_test`'s brand block documents this as the one
> sanctioned escape, because the alternative — dropping the only study that tested physical screens
> against visible swing faults — would make the library's most load-bearing negative result
> uncitable.

### Weak or indirect

**Reduced pelvic rotation depth.** Plausible and widely taught; not directly measured against hip
range anywhere found. Kim 2015 reported more *lumbar* motion and more posterior pelvic tilt in the
limited group — it did not report reduced pelvic axial rotation.

**Knee frontal-plane motion against rotational depth.** One source: **Khuyagbaatar, Purevsuren &
Kim**, *Proc IMechE Part H* 2019;233(5):554–561, [10.1177/0954411919838643](https://doi.org/10.1177/0954411919838643),
PMID 30912691. Ten low-handicap males. Trail-knee adduction from address to the top, and lead-knee
adduction from the top to the finish, both emerged as predictors of X-factor. **Read the assignment
carefully**: the backswing window in that result belongs to the *trail* knee, where the claim here is
about the *lead* knee in the same window. It establishes that knee frontal-plane motion and
rotational depth move together at all. It does not test the pairing the characteristic names.
Added as `ref.khuyagbaatar2019` and cited by `lead_knee_drifts_in_at_top`, with that caveat written
into its `establishes`.

### Coaching heuristic, no support found

**Lead heel lift.** The strongest published statement located is a narrative-review assertion that
letting the lead heel rise "*may* allow more pelvic rotation and decrease stress on the spine". Heel
lift is also a deliberate technique choice made by good players for reasons unrelated to any
restriction. The pack's `excessive_heel_lift` consequence already says this and did not need
softening.

**"Especially in older players."** The closest evidence is **Foxworth, Millar, Long, Way, Vellucci &
Vogler**, *JOSPT* 2013;43(9):660–665, [10.2519/jospt.2013.4417](https://doi.org/10.2519/jospt.2013.4417),
PMID 23886577 — twenty amateurs, young (25.1 ± 3.1) against senior (56.9 ± 4.7). Adjusted for
club-head velocity the two groups produced **comparable** hip torques, with one exception:
trail-limb hip *external* rotator torque. So one relevant capacity declines measurably with age and
the rest did not, and nothing in it touches heel lift, knee displacement or any visible fault.

**Nothing in the library encodes an age claim.** `Norm` already supports a `Cohort` of sex plus age
band and `cohortProbeOrder` resolves it, so the correct home for the age story is a senior corridor
seated from data — not a characteristic that fires more readily for older golfers. Every norm in the
set is still `n = 0`, so there is nothing to seat yet. `ref.foxworth2013` is filed as
`generalReading: true` for the same reason: it is the paper somebody will reach for when that
corridor is finally seated, and it is more equivocal than the folklore.

### No threshold exists

**There is no published figure for lead-knee lateral displacement at P4 in any unit** — not degrees,
not centimetres, not a percentage of stance. The corridor shipped here is a placeholder and its
`citation` says so in those words.

---

## 3. The geometric problem, and the fix

This is the part that changed the design.

Pelvic rotation about the vertical axis carries the lead hip **rearward and toward the trail side**.
In a face-on projection, rearward is invisible and trail-ward is not — so **a deeper turn moves the
lead knee toward the trail knee by exactly the signature the fault is supposed to have.**

```
        face-on projection, right-handed golfer
        ─────────────────────────────────────────
        (a) GENUINE TURN            (b) COMPENSATION
        pelvis rotates              pelvis barely moves
        hip  ──────►                hip  ·
        knee ──────►                knee ──────►
        ↑ both travel together      ↑ knee travels alone
        ─────────────────────────────────────────
        raw knee position:  IDENTICAL
        knee minus hip:     ~0            full deflection
```

A detector on raw lead-knee travel fires on good turns and bad ones alike. Precision does not help;
the two states are genuinely the same picture.

**What separates them is the hip.** Under a genuine turn the pelvis carries the whole limb, so the
lead hip and the lead knee travel together. Under the compensation the pelvis has not rotated and
the knee goes in on its own. So the metric is the **difference**:

```
leadKneeDrift(t) = Δx(leadKnee) − Δx(leadHip)          both referenced to address
```

Both points share the same rotation, so the first-order projection term cancels.

**It is not a proof, and the code says so.** Hip and knee sit at different radii from the axis the
pelvis turns about, so a residual survives. That residual is unquantified — which is precisely why:

- the corridor is a placeholder to be seated from a corpus, not a threshold read off a paper;
- the characteristic is named for what was **seen** (`lead_knee_drifts_in_at_top`), not for what it
  might **mean**;
- the causes are attached as `causes` edges *into* it, so `relation_resolver` ranks them and emits a
  `TestRecommendation` — rather than the detector concluding a hip restriction on its own.

That last point is the whole design. The face-on camera reports an observation; the causal graph
turns it into "screen the trail hip, it would explain three of your findings"; a physical screen or a
down-the-line view settles it. The observation never claims to be the diagnosis.

---

## 4. The frontal-plane rule

The rule that decides what belongs in this module, stated once so the next person does not have to
re-derive it:

> **A face-on camera resolves the frontal plane — image x and y — and nothing in depth.**

Everything `lower_body_metrics.cpp` produces is a frontal-plane displacement or a frontal-plane
angle. Everything left planned needs depth, or a model the pixels do not contain:

| Metric | Plane | Verdict |
|---|---|---|
| `leadKneeDrift` | frontal (x) | **produced** |
| `hipLineTilt` | frontal (x, y) | **produced** |
| `pelvisSway` | frontal (x) | **produced** |
| `pelvisLift` | frontal (y) | **produced** |
| `pelvisThrust` | **depth (z)** | stays planned — toward and away from the camera is the one direction this view cannot see |
| `leadKneeFlexion` / `trailKneeFlexion` | **sagittal** | stays planned — knee bend is nearly perpendicular to the image plane and the projection foreshortens it almost to nothing |
| `comOverLeadFoot` | needs a body-segment mass model | stays planned |
| `pelvisRotation` | rotation about the vertical axis | stays planned |

Producing any of the bottom four from this view would put a confident number on a quantity the
camera cannot see, which is the failure mode this whole document exists to avoid.

---

## 5. What ships

One new analysis stage over **COCO body keypoints 11–16** — hips, knees, ankles.

Those indices exist in **both** pose layouts. Unlike `foot_metrics`, which reads the WholeBody foot
tail (17–22) and therefore emits nothing at all on a track recorded before WB0, this module answers
on legacy 17-keypoint tracks too. That is why it is a separate stage rather than an addition to
`FootMetricsStage`: folding them together would tie the wider availability to the narrower one for
no reason but tidiness.

### Effect on the firing set

| | before | after |
|---|---|---|
| live measures | 44 | **51** |
| corridor signals that can fire | 25 | **37** |
| conditions that can fire | 25 | **35** |
| …of those, unexplainable | 0 | **0** |

Ten conditions became detectable, and none stopped being: `sway`, `slide`, `hanging_back`,
`pelvis_drift_lead_backswing`, `pelvis_sink_backswing`, `trail_hip_hike`, `off_balance_finish`,
`weight_back_at_finish`, plus the two authored here.

`backing_off_the_ball` is the one that did **not** arrive, and it is worth saying why: it rides on
`pelvisThrust`, which is the depth axis. It is in the same content neighbourhood as the rest and is
the clearest single example of §4 — the fault is real, the characteristic is authored, and this
camera cannot see it.

`weight_back_at_finish` had **no cause at all** and would have been reported and never explained.
That is trap 3b in the developer guide happening live — causal edges are authored per condition
*group* while the firing set is decided per *producer*, so a producer landing pulls conditions into
the firing set without their ever passing the group sweep. `core_pack_test` caught it, as designed.

---

## 6. Units, and why not centimetres

Every displacement here is a **percentage of the address ankle span**; angles are degrees. One unit
each, for all time.

Centimetres were considered and rejected. They need a ruler — the ball-diameter one at the ground
plane — which needs a detected ball. A metric present in centimetres on some swings and absent on
others is worse than one that is always present and body-relative. It is also the wrong reading:
**a norm in millimetres is a norm on the golfer's height.** A 190 cm player and a 160 cm player take
genuinely different stances and neither is wrong. This is the argument `foot_metrics.cpp` already
makes for `stanceWidth` as a percentage of shoulder width, and it applies unchanged.

The span is measured **ankle to ankle** rather than heel to heel so the denominator lives in the same
keypoint set as the numerators and survives a legacy track. It is close to but not the same as
`stanceWidth`'s heel-to-heel span, which is why these carry their own unit string rather than
borrowing that one.

Two consequences worth knowing:

- **The five migrated corridors were re-expressed**, at roughly 2.5 % of stance per centimetre (a
  ~40 cm ankle-to-ankle iron stance). The claim is unchanged and still a heuristic — it was never
  seated on data in either unit — and the `citation` on each says so.
- **`minStanceSpanPx` (40 px) guards the denominator.** Below it, a few pixels of keypoint noise
  divided by a few pixels of stance would emit hundreds of percent. The floor is the difference
  between "we could not measure this" and a confident absurdity.

### The sign is resolved, not assumed

Lateral channels are lead-positive per `pinpoint_sign_conventions.md` rule 2. **Which image
direction that is comes from the address geometry** — which ankle sits further along +x — rather
than from a constant. A camera can be mirrored and an operator can flip the preview; a convention
that depends on neither happening is not a convention. A hard-coded sign would report every sway and
every knee drift backwards while looking entirely healthy, which is the defect class
`axis_direction_test` exists for.

---

## 7. The content, and where the uncertainty is recorded

### The new characteristic

`lead_knee_drifts_in_at_top`, group `lateral`, `observable` / `measured`, state `draft`.

Its `consequence` carries the qualification rather than leaving it to a doc nobody reading the app
will open: that the same picture is produced by a golfer who turned deeply and one who barely turned
at all, that subtracting the hip separates them imperfectly, and that a down-the-line view or a
physical screen is what settles it.

Causes in, all `practice` tier:

| from | strength | why not stronger |
|---|---|---|
| `limited_trail_hip_ir` | moderate | `ref.gulgin2014` — the screen predicted no visible fault in 36 golfers |
| `limited_thoracic_rotation` | moderate | a genuine competing cause, and the one coaching sources attribute knee buckling to |
| `poor_pelvic_disassociation` | moderate | — |
| `limited_ankle_dorsiflexion` | weak | plausible, least evidenced |

**The confounders are modelled as competing causes, not as a caveat in prose.** That is what makes
the explanation pass useful: it ranks four candidate restrictions and recommends the screens that
would separate them, instead of the detector picking one.

Corroborates (reported and tie-breaking, never scored — `relation_resolver.h`):

- `excessive_heel_lift` — **the co-occurrence from the original observation.** This is the right
  mechanism for it: two findings that are more telling together than either alone, without inventing
  a multiplier nobody could defend.
- `hips_under_rotated_at_top`
- `trail_hip_hike`

### `trail_hip_hike` moved to a better measure

It was detected by `m_pelvisLiftTop` — the pelvis **centre** rising. But one hip riding up over the
other and the whole pelvis lifting evenly are different observations, and only the second is what
pelvis lift measures. `sig_trailHipHike` now watches `m_hipLineTiltTop`, the tilt of the hip line
itself, which is what the characteristic has always been about.

The corridor there is `mu` 10°, `sigmaHi` 7 — the trail hip normally sits about 10° above the lead
hip at the top with a driver, and around 30° is the figure widely taught as the point at which the
pelvis has hiked rather than turned; 3σ lands just past 30. Both numbers are coaching doctrine, not a
results table, and the norm's `citation` says so. The driver basis also means the per-club rows are
**missing**, not that one row covers every club.

Moving the signal left `m_pelvisLiftTop`'s high tail unwatched, which `ungradedTail` would have
reported — so `pelvis_rise_backswing` was authored for it, the mirror of the existing
`pelvis_sink_backswing` on the same axis.

### One edge that was refused, correctly

`hanging_back → weight_back_at_finish` was written as a causal edge and the validator refused it: the
pair already corroborates. It is right to. Hanging back at impact and weight back at the finish are
one event seen at two instants, not one causing the other, and a causal edge would let the same
cause be credited twice in the ranking. `poor_single_leg_balance` carries the explanation instead.

---

## 8. The unit defect this work uncovered

Found while reading the neighbouring producers, and fixed in the same change because it was live.

Three shipped measures declared `cm` over producers emitting something else:

| measure | producer emitted | corridor | what the golfer got |
|---|---|---|---|
| `m_leadHeelLiftTop` | `×frame` (~0.05) | 2 cm, ceiling | `sig_excessiveHeelLift` **could not fire on any swing ever recorded** |
| `m_headSwayBack` | `mm` (~40) | 4 ± 3 cm | **Action on every swing** |
| `m_headLiftBack` | `mm` | 0 ± 3 cm | same |

Two silent failures in opposite directions from one root. **`normUnitMismatch` was structurally
unable to see it**: it compares the norm against the *measure*, and the loader refuses a mismatch
there — so both ends of the content agreed with each other by construction. The producer was the
third party and was never in the conversation.

Fixed by making all three emit centimetres, and **absent rather than rescaled** when their ruler does
not resolve. Plus a new health check, `measureUnitMismatch`, comparing `Measure::unit` against
`MetricDescriptor::unit` — with `Rate` reducers exempt, since mph/s over a mph metric is the reducer
doing its job. `diagnostics_health_test` gates it at zero over the shipped library.

`m_lowPointAhead` was the fourth hit: content in `cm`, descriptor and `low_point_metric_design.md`
both in inches. Converted to inches — sign-conventions rule 1 gives the outside convention the
casting vote, and inches is what launch monitors quote.

**Separately**: ten `.planned` descriptors were claimed by no provider, so they resolved "no producer
available" — the reason an *unknown* key gets — instead of the roadmap reason. The test enumerated a
hand-written list, so a descriptor added with the flag and no provider entry was invisible by
construction. It now derives the list from the catalogue.

---

## 9. What is deliberately still planned

Restating §4 as a work queue, because "why didn't you just do the knee angles too" is the obvious
question:

- **`pelvisThrust`** — depth. Genuinely needs a second camera or stereo. Its descriptor already says
  so and already requires `ReconstructionTier::Stereo3D`.
- **`leadKneeFlexion` / `trailKneeFlexion`** — sagittal. A face-on projection foreshortens knee bend
  almost to nothing, and the four characteristics over them (`late_buckle`, `excessive_knee_flex`,
  `insufficient_knee_flex`, `trail_knee_straighten`) would be graded off a number that is mostly
  projection error. They are the strongest argument for a down-the-line pipeline.
- **`comOverLeadFoot`** — needs a segment-mass model, which is a different piece of work from a
  keypoint reading.
- **`pelvisRotation`** — the actual turn depth, and the thing the knee metric is a proxy *for*. It
  now leads the roadmap: one series, five reducers, seven characteristics.

---

## 10. Open questions

1. **The corridor for `leadKneeDrift` is unseated and unusually so** — no published figure exists in
   any unit, so `mu = 0, σ = 8 %` is a placeholder chosen to be wide enough not to fire on ordinary
   variation, not a re-expression of anybody's number. It must be seated from the corpus before
   anyone reads a grade off it. The corpus-share health scan (`oneBandCorpus`) is the tool.
2. **The residual after subtracting the hip is unquantified.** How much does a genuinely deep turn
   move the knee relative to its own hip? A synthetic rig or a small marker study would answer it,
   and the answer decides whether this metric is a good detector or merely a fair one.
3. **`hipLineTilt` is club-dependent** and ships one row at `any`. The 10°/30° figures are quoted for
   a driver. `clubDependentNoContext` will start reporting it the moment the descriptor's
   `howToRead` says "club-dependent" in those words; it currently does not, which is arguably
   letting it off.
4. **The session gate still applies.** `LowerBodyMetricProvider` is `wristSessionOk`-gated like every
   other pose provider, so all of this reads Unavailable in a Swing session. Ledger `X1` in the
   content-extension plan.
5. **None of it produces a finding yet.** `detect()` and `relation_resolver` remain dormant for want
   of an `IMeasureValueSource` adapter over `measure_sample`, and — the real blocker — a decision
   about where findings surface for a coach.

---

**Read next**: `docs/developer/diagnostics_developer_guide.md` (§8 for what actually runs),
`docs/design/pinpoint_sign_conventions.md` (before touching any direction),
`docs/developer/metric_catalogue_developer_guide.md` (how to add a metric).
