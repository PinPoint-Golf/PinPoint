# Norm shapes — implementation brief

Read these before writing anything, in this order:

- `src/Diagnostics/norm.h` — the grading model, and the doctrine comments (population norms,
  asymmetry, the float-edge lesson, NotMeasured ≠ passing). Every rule in there survives this work.
- `src/Diagnostics/characteristic.h` — Measure, Signal (Direction, SignalTest), `highMeans`.
- `src/Diagnostics/metric_corridor.h` — how a norm projects to what surfaces draw.
- `src/Diagnostics/norm_pack.{h,cpp}` — parse and the two validation layers (standalone +
  `validateNormsAgainst`).
- `src/Diagnostics/norm_measure_source.h` and `characteristic_engine.cpp` — where readings are
  graded and signals evaluate `onTail`.
- `src/Resources/diagnostics/norms.json` and the measure entries in `core.json`.

---

# Part 0 — fix the Ideal-band policy divergence (DO THIS FIRST)

A pre-existing defect, found while designing this work, in the exact functions Part A modifies. Fix
it and merge it BEFORE Part A, so Part A's regression gate is pinned against correct behaviour.

`bandEdgesOf()` sets `idealLo = mu − sigmaLo` / `idealHi = mu + sigmaHi`, ignoring
`policy.idealMaxZ`. `grade()` uses `withinBand(value, norm, policy.idealMaxZ)`. Under the
`standard` preset (idealMaxZ = 1.0) they coincide, which is why nothing has caught it. Under the
other two shipped presets they diverge:

- `strict` (0.75): a value at 0.9σ is DRAWN inside the green band and GRADES Good — and
  `ragOf(Good)` is Amber. Green band, amber chip, one number.
- `lenient` (1.5): a value at 1.3σ is drawn outside green and grades Ideal.

This is the disagreement `withinBand`'s own comment exists to eliminate, at whole-band scale rather
than float-epsilon scale. The `norm.h` claim that the Ideal band is policy-independent is true of
the drawing path and false of the grading path.

**Resolution:**

1. `bandEdgesOf()` applies `policy.idealMaxZ` to the ideal edges, the same way it already applies
   `watchMaxZ` to the watch edges. One edge, computed one way, on both paths — the existing
   doctrine, extended to the band it was not applied to.
2. The corridor editor's two handles stay bound to `mu ± sigma` — the norm's OWN claim, which must
   not move when a user changes a sensitivity setting. Rename what they bind to so the two ideas
   stop sharing a name: the norm's claim (`mu ± sigma`) is not the same object as the policy's
   Ideal band. The editor's existing read-only "what the grade policy makes of it" panel gains the
   policy-scaled Ideal edge alongside Good and Watch.
3. Audit every call site: some read `bandEdgesOf`, some read `norm->idealLo()` directly, and after
   this fix those are NO LONGER interchangeable. Each site must be a deliberate choice between
   "the norm's claim" and "what this policy grades as Ideal". Rendering surfaces want the latter;
   the editor's handles and any diff against a shipped or parent row want the former.
4. `MeasureReading::greenLo/greenHi` (from `norm_measure_source.h`) become policy-scaled, so the
   engine's `onTail` check and its `deviated` check finally derive from one scale. Today they do
   not, and the engine is correct only because every shipped preset has `goodMaxZ >= 1`. Add that
   as an explicit invariant with a test — `goodMaxZ >= idealMaxZ` and `watchMaxZ >= goodMaxZ` and
   all three positive — rather than leaving it as a property of three hand-written presets.

**Tests:** for each shipped preset, `bandEdgesOf` and `grade` agree at both ideal edges to ±1e-9
(extend the existing edge sweep to run per preset, not just under `standard`); a value inside the
drawn green band never carries an Amber chip under any preset; the preset ordering invariant.

**Note for Part A:** the one-sided shapes inherit this fix rather than reproducing it — a floor's
single computed edge is `mu − idealMaxZ × sigmaLo`, and the good side is Ideal by construction at
every policy.

---

# Part A — shapes

## The concept

Every norm today is one shape: a (possibly asymmetric) corridor around a target. That is right for
~90% of the catalogue and wrong for a small, important family. This work introduces **shape** as an
explicit property so a norm can be:

- **`target`** — the current behaviour, exactly. Ideal is `mu ± sigma`, both tails grade. Default.
- **`floor`** — higher is better. `mu` is the aspiration point; only the LOW tail grades; every
  value at or above `mu` is Ideal. Example: smash factor.
- **`ceiling`** — lower is better. Mirror of floor; only the HIGH tail grades. Example: a magnitude
  measure whose domain is `[0, ∞)` and whose ideal is zero (`m_leadHeelLiftTop` is the standing
  candidate, NOT converted in this work).

Plus one cross-cutting addition, valid on every shape:

- **Plausibility bounds** — optional `plausibleLo` / `plausibleHi` on a norm row. A reading outside
  them grades `NotMeasured`, flagged as implausible, never `Action`. A driver smash of 1.62 is not
  a swing finding; it is a mis-tracked ball, and grading it (either way) would launder a capture
  fault into a confident diagnosis. This is the far-edge behaviour of the open tail: floor shapes
  are open above *up to plausibility*, not open to infinity.

## Decisions already made — do not relitigate

1. **Shape is a property of the MEASURE, not the norm.** One-sidedness is semantics of the quantity
   and invariant across contexts. If shape sat on the norm, a driver row and an iron row could
   disagree about the shape of one measure, and the corridor editor could flip shape per context.
   Norm rows carry only numbers; `validateNormsAgainst` checks those numbers against the measure's
   shape exactly the way it already checks `unit`.
2. **No new grade bands.** `Ideal/Good/Watch/Action/NotMeasured` and `ragOf` are unchanged. Shape
   changes which values reach which band, never what the bands are.
3. **`GradePolicy` is unchanged and stays pack-wide.** A floor norm consults the same z ladder on
   its single graded tail.
4. **No mixture/bimodal shape, no transform functions.** Multimodality is what the context tree is
   for (the archetype rows prove it); skew is what asymmetric sigma approximates. Neither gets
   machinery here.
5. **Sequencing is NOT a norm shape — explicit non-goal.** Ordering lives in `SignalTest::Order`
   (pairwise, one condition per inversion, which is what makes it diagnosable). The continuous
   companions of the kinematic sequence — peak-to-peak gaps as % of downswing, peak-gain ratios —
   are future *measures* with ordinary `target` norms once producers exist. Nothing in this work
   touches Order.
6. **Cohort-relative bigger-is-better measures get NO norm.** Ball speed, club speed, carry: a
   population floor on these grades a golfer Action for their age. They are excluded by doctrine
   (norm.h: all norms are population norms) and wait for a separate benchmarking feature. The
   `floor` shape is for self-normalising ratios and mechanically-universal thresholds only. Record
   this in the norms.json header comment.

## Semantics — exact

`Shape { Target, Floor, Ceiling }`, with `shapeName`/`shapeFromName` following the existing
enum↔string convention. JSON key `"shape"` on the measure entry in core.json; absent ⇒ `target`.

For `Floor` (mirror everything for `Ceiling`):

- `normZ(value, norm, shape)`: `value >= mu → 0`; else `(value − mu) / sigmaLo` (negative).
  Continuous at `mu` (both formulations give 0 there). The good side reports z = 0, not a raw
  positive distance — a future 0–100 characteristic score built on z must not reward overshooting
  a floor.
- `withinBand`: `value >= mu − threshold × sigmaLo`, computed-edge, inclusive. The float-edge
  doctrine in norm.h applies verbatim: one edge, computed one way, compared inclusively on every
  path; extend the ±1e-9 edge-sweep test to one-sided norms.
- `grade`: same ladder, one tail. Explicit monitor: only `monitorLo` is legal on a floor;
  `value < monitorLo → Action`, else capped at Watch inside it — precedence unchanged.
- `sigmaHi` is meaningless on a floor. Validation (in `validateNormsAgainst`, which knows the
  measure): a floor norm whose `sigmaHi ≠ sigmaLo` is an error (an explicitly-stated-but-equal
  value is indistinguishable from the parse default and harmless); a `monitorHi` on a floor is an
  error; `plausibleHi`, if present, must sit at or above the watch edge... see next.
- Plausibility validation (all shapes): where a plausible bound and a watch edge both exist on a
  side, the plausible bound must lie at or outside the watch edge — a corridor must never extend
  into implausible territory. Plausible bounds live on the norm row (they are context-dependent:
  the physical smash cap is loft-dependent), parse like monitor bounds, and may appear singly.
- Grading with plausibility: outside `[plausibleLo, plausibleHi]` ⇒ `Grade::NotMeasured`, and the
  reading carries a distinct `implausible` flag so surfaces can say "reading outside plausible
  range — check capture" instead of "not measured". These are different statements; never merge
  the wording. `ragOf(NotMeasured)` stays Grey.

`NormBandEdges` gains `bool lowOpen, highOpen`. On the open side, set the numeric edge fields to
the ideal edge (a defined, sane value — never a sentinel; infinity and NaN must not cross into
QML) and set the flag. `MetricCorridor` gains the same flags, propagated in
`corridorForMetricAtPhase`. `marginOverride` (the SwingLab bands sweep) widens the graded side
only.

`MeasureReading` carries the openness flags and the implausible flag. In the engine's
`OutsideCorridor` case, a signal whose `Direction` points at the OPEN tail can never fire —
`fired = false` at runtime, and it is a `diagnostics_health` finding at validation time ("signal
watches a tail the norm never grades"), because an author who wrote it has misunderstood the
measure. The signal editor's direction picker should offer only the graded tail for a one-sided
measure (it already renders `highMeans`; greying is the same mechanism the facet picker uses).

## Surfaces (UI)

### PRIOR ART — one-sidedness already exists twice, decided by string-matching

Before writing anything here, read `src/Gui/shot/PpDashboardMotionZone.qml:72` and
`src/Analysis/dashboard_reductions.h:120-155`.

`PpBandRail` already has a `oneSided` property, and `railRange()` already solves the open-tail axis
problem correctly: it drops the upper corridor bounds from the domain so an aspirational ceiling
does not crush the trace into the bottom of the tile. That logic is sound and is REUSED, not
rewritten.

What is wrong is how it is decided. `PpDashboardMotionZone._isOneSided()` sniffs the unit string:

```qml
return u === "m/s" || u === "mph" || u === "°/s" || u === "deg/s"
```

A presentation-layer heuristic standing in for a semantic property of the measure. **Delete it.**
`oneSided` is fed from the measure's shape, threaded through `MetricCorridor`, like every other
corridor fact.

Separately, `SwingScorer` (`src/Analysis/swing_scorer.cpp:34-44,126-127`) carries `oneSidedDir` per
band with EXACTLY the semantics Part A specifies — `if (b.oneSidedDir > 0 && z > 0) z = 0.0;` —
tunable via SwingLab `score.<metric>.oneSidedDir`. SwingScorer is dead code (see
`metric_providers.h:84`) so it is not rewired, but it is the second independent invention of this
concept and its clamp confirms the design. Leave it frozen; do not add a third.

The lesson to carry into the code: shape is decided ONCE, on the measure, and every surface reads
it from there. Any surface that re-derives one-sidedness from a unit, a metric key or a label is a
bug.

### Corridor editor (`CorridorEditor.qml`, `norm_editor_model.{h,cpp}`) — the bulk of the work

- Shape shown read-only, with the measure's `highMeans` beside it, so the author reads "higher is
  better: more of the clubhead speed reaching the ball" rather than a bare enum word.
- One handle on the graded side. The dead handle is ABSENT, not disabled — a disabled handle
  invites the question "why can't I drag this?" on every visit.
- The open side draws to the plot edge with a fade and an explicit end-cap label ("no upper limit"
  / "no lower limit"). It must never terminate in a hard edge that reads as a bound.
- `nudgeIdealHi`/`nudgeIdealLo`, keyboard nudge, and the numeric entry field: only the graded side
  exists. Monitor editing likewise.
- **The band-width readout is meaningless one-sided.** `CorridorEditor.qml:871` renders
  `idealHi − idealLo`, and `:769` renders "%1 to %2". Both need one-sided forms: "at least 1.48",
  "no more than 12°". Same for any tooltip or accessible name built from the pair.
- **Parent / shipped diff rows** (`parentIdealLo`, `shippedIdealLo`, and the "core has moved"
  message at `norm_editor_model.cpp:821`) are two-sided by construction. One-sided forms, and
  `NormBasis` comparison ignores the ungraded side.
- **PLAUSIBILITY BOUNDS NEED UI — this was missing from the first draft of this brief.** They are
  authored data, not a compiled constant, so the editor must expose them: an optional pair of
  numeric fields in an "Implausible beyond" group, clearly separated from the corridor handles
  because they answer a different question (is this reading real?) not a grading question (is this
  swing good?). Drawn on the plot as hatched or dimmed regions OUTSIDE the corridor, visually
  distinct from Action. Live validation against the watch edge, inline, at the moment of the
  mistake — consistent with how the facet picker reports errors.
- Seat-from-swings on a floor: `mu` = sample median, `sigmaLo` = median − 16th percentile (robust);
  record `n` and provenance as today. The UI must not offer to seat the ungraded side.

### Rails and bars

- **`PpBandRail` + `railRange()`**: rewire `oneSided` from the unit sniff to shape. `railRange`
  currently handles the FLOOR case only (it drops the upper bounds); add the ceiling mirror. The
  parameter becomes a shape rather than a bool.
- **`NormativeBar.qml`** and **`PpRangeBar.qml`**: both derive their axis domain from the amber
  span (`PpRangeBar.qml:62-68`, `_amberSpan = amberHi − amberLo`), which is OPEN on a one-sided
  norm and will produce a degenerate or absurd domain. Each needs an explicit one-sided domain
  rule: anchor on the graded edge, extend the open side by a fixed fraction of the graded span past
  the furthest of (value, ideal edge), and render that side as a fade running off the track.
  Degenerate-span fallbacks must be re-checked under the new rule.
- **`PpDashboardSetupZone.qml`** and any other corridor consumer: audit for the same two-ended
  assumption. Grep for `amberHi`, `greenHi`, and arithmetic on the pair.

### Wording — a surface in its own right

- Findings, grade chips, MeasureDetail's norm row and the DAG/characteristic detail text must never
  say "above the corridor" for a floor or "below" for a ceiling. The norm row reads "at least
  1.48 (men, adult)" not "1.48 ± 0.05".
- **Implausible readings get their own wording**, distinct from both a grade and from "not
  measured": something in the register of "reading outside the plausible range — check the capture",
  with the reading shown rather than hidden. Never merge it into "not measured" (a capture gap) or
  into Action (a swing finding). This is the same distinction `norm.h` already makes between
  NotMeasured and a passing grade, one level further out.

### Signal / characteristic editor

The direction picker offers only the graded tail for a one-sided measure, greyed with a reason on
the other — the same mechanism the facet picker uses for impossible chips, and the same rationale:
reject at the moment of the mistake, not after the author has built something.

### Wrist grid

All wrist DOFs are `target` and remain so. `NormBandProvider` asserts shape == target rather than
growing `Band`; extend `Band` only if that assertion ever fires.

### UI test coverage

The QML surfaces above are largely untested today. At minimum add model-level tests for the derived
values the QML binds to: the one-sided domain rules, the one-sided readout strings, and the
plausibility field validation. A rule that lives only inside a `.qml` binding is a rule nothing can
test — the same argument `metric_corridor.h` makes for being free-standing.

## Migration and seed changes

Stage this so every existing behaviour is pinned before anything moves:

1. Shape enum + measure field + parse + validation, all measures defaulting `target`. Every
   grading function takes shape as a parameter defaulting `Target`, so `reference_bands_test` and
   the whole existing suite pass UNTOUCHED. That is the regression gate.
2. Grading semantics + edges + engine + health checks + tests (below).
3. Surfaces — split into its own sessions, they are not one item:
   3a. Rewire `oneSided` from the unit sniff to shape; ceiling mirror in `railRange`. Small,
       self-contained, and it deletes a heuristic — do it first.
   3b. `NormativeBar` / `PpRangeBar` / setup zone one-sided domain rules.
   3c. Corridor editor: handles, readouts, diff rows.
   3d. Corridor editor: plausibility bound fields and plot regions.
   3e. Wording pass + signal direction picker.
4. Seed conversion — the only content change:
   - `m_smashFactor` → `"shape": "floor"` on the measure. Norm rows keep mu/sigmaLo as they are;
     add per-context plausible caps (heuristic, physics-of-loft note in the citation field, no
     commercial sources): driver `plausibleHi` ≈ 1.56, iron ≈ 1.45, wedge ≈ 1.32. Behavioural
     delta to assert in a test: driver smash 1.55 was Good, becomes Ideal; 1.62 was Watch,
     becomes NotMeasured(implausible); 1.30 is Action before and after.
   - Annotate in norms.json comments, but DO NOT convert (author's call pending):
     `m_leadHeelLiftTop` (ceiling candidate, domain [0,∞)); `m_handSpeedP6P7` (sign convention
     unresolved — deceleration vs release-drag, stays target); `m_lagAngleDown` (two-sided: over-
     retention → flip); `m_xFactorStretch` (stays target: high tail carries lumbar-load risk).

## Tests

- norm_test: floor/ceiling grading tables; continuity at `mu` (z = 0 from both sides); good-side z
  clamps to 0; edge sweep ±1e-9 on the single computed edge; monitor precedence on a floor;
  plausible → NotMeasured with the flag set; zero-sigma degenerate on a one-sided norm.
- pack validation: sigmaHi-differs on floor, monitorHi on floor, plausible bound inside the watch
  edge, unknown shape token — each a named load error in the existing style (both sides named).
- engine: High-direction signal on a floor norm never fires and is a health finding; Low-direction
  fires exactly as a target norm's low tail would.
- corridor: openness flags propagate through `corridorForMetricAtPhase`; marginOverride widens one
  side only.
- The smash behavioural-delta test from stage 4.

---

# Part B — cohort keying (age and sex)

Separate stages from Part A; run each as its own session. Part A must be merged and green first —
shape and cohort are orthogonal (what a corridor means vs which corridor resolves) and share no
code paths except the norm key.

## The concept

A norm gains an optional **cohort** as part of its key: (measure, context, cohort). A cohort is
still a POPULATION — a segmented one — so the norm.h doctrine survives with one rewording: the
inviolable rule is "never keyed by athlete id", not "one population for everyone". Update that
header comment; a per-athlete baseline remains a different feature.

Cohort is NOT a context node. The context tree describes the SHOT (club, shot type, archetype);
age and sex are properties of the ATHLETE, resolved from the athlete record at grading time.
Folding athlete attributes into the shot tree would force the cross-product (senior × driver ×
female × …) through a single-inheritance walk that cannot express orthogonal dimensions without
duplicating rows.

## Schema

On a norm row, optional:

```json
"cohort": { "sex": "male" | "female", "age": "adult_55_64" }
```

Either field, both or neither; absent `cohort` ⇒ unqualified (matches everyone).

**The age vocabulary is CLOSED and hierarchical.** A fixed enum in code, stable tokens, changed
only by a schema version bump — never extended by pack content. This is the whole point: the model
is community-maintained and studies band age every way imaginable, so a free numeric range would
put study-specific arithmetic into the key, where nothing can reconcile it across packs.

```
junior                    under 18
adult                     18+          — authorable in its own right
  adult_18_54
  adult_55_64
  adult_65plus
```

Half-open, gapless, total. Boundary rationale, for the header comment: 55 is the seniors threshold
in UK club practice, so a golfer already knows which side of it they are on and a coach does not
have to explain the band. It sits above the point where the trunk-rotation decline literature
usually pivots, which is deliberate — a band boundary set later than the physiological one means a
55+ row describes a population that has unambiguously started to decline, rather than straddling
the onset. `junior` is deliberately one band and is the weakest of them — a 9- and a 17-year-old
are barely one population, but there is no junior corpus to split against. If one arrives it splits
at peak height velocity (~14), as a version bump.

`adult` is authorable, not merely a parent, because most provenance is no better than "adult male"
or "adult female". Without it the common case would force three duplicate rows that then drift.

**A study whose range does not match a band is mapped by the AUTHOR to the nearest band, with the
study's actual range recorded verbatim in the row's `citation` string.** The key stays comparable;
the provenance stays honest. Never widen the vocabulary to fit a paper.

Athlete model: DOB and sex are optional fields (extend `ScreenAthleteForm` / the athlete record if
either is missing today). The band is derived from DOB **at the swing date**, never stored: an
athlete ages across their own history and a stored band would grade old swings against today's
cohort. Unknown DOB, unknown sex, or a declined sex answer ⇒ only rows unqualified on that axis
match.

Athlete model: DOB and sex are optional fields (extend `ScreenAthleteForm` / the athlete record if
either is missing today). Unknown DOB or sex ⇒ only unqualified rows match. Demographics gaps
degrade to the universal corridor and STILL GRADE — never NotMeasured. "We don't know your age" and
"we could not assess this" are different statements.

## Resolution

One walk, context-major: at each node of the existing upward context walk, probe the cohort keys in
a FIXED order, most specific first, and take the first row present. Only if none is present move up
the tree.

```
1. sex + exact age band          (female, adult_55_64)
2. sex + adult                   (female, adult)          — skipped when the athlete is junior
3. exact age band                (adult_55_64)
4. adult                         (adult)                  — skipped when the athlete is junior
5. sex                           (female)
6. unqualified
```

A fixed probe order rather than a specificity score with a tie rule: a community pack must never
fail to load, or resolve unpredictably, because two rows were equally specific. Age band sits ahead
of sex at equal specificity (3 before 5) because the age effect on the ROM measures where cohorts
matter is larger and monotone, while sex differences are partly absorbed by the body-normalised
units the setup measures already use. Where both a sex-only and a band-only row exist at one node
with no combined row, `diagnostics_health` raises a WARNING naming both and inviting the combined
row — a nudge, never a load failure.

Consequences of the context-major walk, both intended:

- An unqualified `driver` row beats a senior row at `any` for a driver shot. Stance width is
  club-mechanical; if senior-driver matters, author the senior-driver row.
- A senior row at `any` resolves for a senior wherever no club-specific row exists — which is the
  ROM family, exactly where cohorts matter.

`validateNormsAgainst` gains: unknown cohort token (named in full, both the token and the legal
vocabulary); a `junior` row combined with an adult sub-band; duplicate rows on one (measure,
context, cohort) key.

`NormResolution` gains cohort provenance: which cohort answered, alongside the existing
context/inherited/overridden fields, threaded through `MetricCorridor` and `MeasureReading`.
Corridor surfaces must be ABLE to say "graded against: men 60+" — a golfer whose grades improve
after entering their DOB must read that as the corridor becoming right for them, not the app going
soft. MeasureDetail's norm row and the corridor editor show it; the dashboard does not need to.

## Band-edge cliffs — accepted

A golfer's grade can shift on their 55th birthday. Do not interpolate between bands: interpolation
implies a continuity the banded literature does not support, and the coach mediates edge cases per
the existing context principle. Cohort shifts the corridor and NEVER inverts valence — same rule as
context, restate it in the norms.json header.

## Editor and seating

- Corridor editor: cohort is part of the row identity it edits; creating a cohort-qualified
  override from an unqualified base records the base in `NormBasis` as today.
- Seat-from-swings: the author chooses the cohort the seated norm claims; the tool does not infer
  it from the athletes in the selection (a mixed-cohort selection seating a "female 60+" row is an
  authoring error the provenance `n` and scope note make visible, not something to block).

## Stages and gates

1. Schema + parse + validation + resolution, with ALL shipped rows unqualified. Regression gate:
   every existing resolution answers identically; the full suite passes untouched.
2. Provenance threading + surfaces.
3. Athlete model fields (DOB, sex) + age-at-swing-date derivation.
4. No cohort content in this work. Cohort rows arrive with the ROM literature review (thorax
   rotation, x-factor, wrist ranges — the age-decline and sex-difference cases the literature
   actually supports). Setup measures are largely cohort-invariant because they are already
   body-normalised (% shoulder width, % stance width); smash is self-normalising and stays
   universal. Note this expectation in the norms.json header so nobody pads cohorts for symmetry.

## Tests (Part B)

- The six-step probe order, exhaustively: every combination of rows present/absent at one node
  resolves to the expected row, with no configuration ambiguous.
- Context-major precedence: an unqualified narrow-context row beats a cohort-qualified broad-context
  row.
- Band derivation from DOB at the SWING date, not today — an athlete with swings either side of a
  boundary resolves different rows for them.
- Unknown DOB, unknown sex, declined sex: falls back to unqualified and still GRADES (never
  NotMeasured).
- Junior athlete never matches an `adult` row (probes 2 and 4 skipped).
- Validation: unknown token, junior+adult-sub-band, duplicate key — each a named load error; the
  sex-only/band-only-without-combined case a health WARNING, and the pack still loads.
- Provenance: the answering cohort reaches `MetricCorridor` and `MeasureReading` intact.

## Non-goals, restated (both parts)

No ordinal norm shape (Order signals + future gap measures). No per-athlete norms — cohorts are
segmented populations, never athlete-keyed. Still no norms at all for cohort-relative
speed/distance outcome measures (ball speed, club speed, carry): a cohort corridor does not fix
those; they are benchmarks, a different future feature. No free numeric age ranges and no
pack-extensible age vocabulary. No cohort interpolation. No mixture shapes, no log transforms, no
new grade bands, no change to `GradePolicy`, `ragOf`, or the wrist grid's Band.
