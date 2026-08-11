# The swing-fault ontology — a review, and a proposed repair

**Status**: SHIPPED in `ae9b57d`, with four corrections — **read §11 before acting on §5–§9.** The
body of this document is left as the review it was, because the argument is what makes the fields
defensible and a rewritten proposal reads as though it were always obvious. Every number in §1–§10
was read at `69b37b6` and is therefore pre-change; §11 carries the shipped figures.
**Occasioned by**: "over the top and early extension are both incredibly common swing faults, yet
one is a characteristic and one is a cause."
**Reviewed from**: the coaching side. What a coach names, what a coach measures, and what a coach
would say if handed this model's output on a range mat.

---

## 1. The presenting complaint, checked

It is worse than stated.

| | `early_extension` | `over_the_top` |
|---|---|---|
| group | `posture` | `armsAndClub` |
| observability | **observable** | **latent** |
| confirmedBy | **measured** | **asserted** |
| axis | `pelvis_thrust` | — none — |
| detected by | `sig_earlyExtension` | — nothing — |
| on measure | `m_pelvisThrustDown`, **planned** | — none — |
| in-edges / out-edges | 6 / 5 | 13 / 5 |
| can it fire today | **no** | **never, by construction** |

The two are treated as different kinds of thing, and the usual defence — *we have a producer for one
and not the other* — does not hold. **Neither can fire today.** `m_pelvisThrustDown` is `planned`, so
early extension is exactly as undetectable as over the top is. The difference between them is not
capture capability, not evidence, and not coaching. It is an authoring accident that has since been
built on.

Three consequences of the accident, in ascending order of seriousness.

**(a) `Observability` and `ConfirmedBy` no longer mean what their headers say.** `characteristic.h`
defines `Latent` as "it cannot be seen in the swing; it is inferred from what it explains", and
`Asserted` as "intent, habit, perception". Over the top is neither. It is the most *visible* fault in
amateur golf — a coach standing down the line sees it before the club reaches the ball — and it is
not an intent, a habit or a perception. It is a movement. It got those two tags because nobody wrote
it a measure, so the fields have quietly become a record of **producer coverage** rather than a
statement about the thing.

**(b) The schema already has the right cell, and over the top is not in it.** Seven shipped
conditions are `observable` + `asserted` — `chunk`, `thin`, `top`, `sky`, `shank`, `pull_hook`,
`push_slice`. That cell means precisely *a golfer or coach can plainly see this; our pixels cannot
resolve it*. That is over the top's situation exactly. So even without any schema change, the
current tagging is wrong on the schema's own terms.

**(c) The model is structurally incapable of concluding the most common fault in the game.**
`relation_resolver.h` rule 2: an `Asserted` cause is **offered, never concluded**, and never counts
as resolving a finding (`relation_resolver.cpp:179,196`). So on a swing that fires
`out_to_in_path` + `chicken_wing` + `strike_heel` + `attack_too_steep` — the textbook over-the-top
pattern, four findings, one obvious cause — the resolver will refuse to name over the top and will
instead conclude a hip-mobility screen. Not because the evidence points there. Because of a field
value set for an unrelated reason.

---

## 2. The actual defect: two patterns for one thing, and no rule for choosing

The library contains two competing ways of representing a named swing fault. Both are defensible.
Neither is written down, and the pack picks between them per condition.

**Pattern A — the fault *is* the measurement.** The coach's name is the node; the node owns an
`axis` on a measure; the opposite tail becomes a second condition, usually one no coach has a word
for.

> `early_extension` owns `pelvis_thrust`; its other tail is `backing_off_the_ball`.
> `casting` owns `lag_retention`; its other tail is `excessive_lag`.
> Likewise `sway`, `reverse_spine`, `scooping`, `chicken_wing`.

**Pattern B — the fault sits *above* the measurement.** The coach's name is a causeless, signal-less
node; the measurable signatures are separate conditions beneath it.

> `over_the_top` → causes `out_to_in_path` (which owns `club_path` on `m_clubPathAtImpact`)
> and is corroborated by `steep_downswing_shaft` (which owns `delivery_plane` on `m_shaftPlaneDelivery`).

Pattern B is arguably the *better* model — it separates the movement from the number, which is what
a coach actually does. The problem is not that B exists. **The problem is that the pack uses A for
thirteen of the fourteen canonical faults and B for one, and nothing in the schema records which one
was used or why.**

Here is the canonical common-fault list — the terms are common domain, the packaging of them into a
named twelve belongs to somebody else and does not enter this repo — read against the pack as it
ships:

| fault | group | observability | confirmedBy | axis | fires today |
|---|---|---|---|---|---|
| S-posture | setup | observable | measured | `lumbar_curve` | no (noProducer) |
| C-posture | setup | observable | measured | `thoracic_curve` | no (noProducer) |
| Loss of posture | posture | observable | measured | `spine_bend_change` | no (planned) |
| Flat shoulder plane | posture | observable | measured | `shoulder_plane` | **yes** |
| Early extension | posture | observable | measured | `pelvis_thrust` | no (planned) |
| **Over the top** | **armsAndClub** | **latent** | **asserted** | **—** | **never** |
| Sway | lateral | observable | measured | `pelvis_sway_backswing` | **yes** |
| Slide | lateral | observable | measured | — | **yes** |
| Reverse spine angle | posture | observable | measured | `axis_tilt_top` | **yes** |
| Hanging back | lateral | observable | measured | — | **yes** |
| Casting | release | observable | measured | `lag_retention` | **yes** |
| Scooping | release | observable | measured | `lead_wrist_impact` | **yes** |
| Chicken wing | armsAndClub | observable | measured | — | **yes** |
| Forward lunge | lateral | observable | measured | — | **yes** |

One row is different from the other thirteen in four columns at once. That is not a modelling
decision; it is a row nobody revisited.

Note the second thing this table shows: **these fourteen are spread across five groups and there is
no query that returns them.** A coach's first question of any fault library — *what are the common
ones?* — has no answer in this schema.

---

## 3. Four more symptoms of the same root cause

The over-the-top asymmetry is the visible edge of a single structural gap: **`ConditionGroup` is an
anatomy-and-phase taxonomy being asked to carry an ontological one, and it cannot.**

### 3.1 More than half the `setup` group is not setup

46 conditions carry `group: setup`. Of those:

| what they actually are | count |
|---|---|
| genuine static setup positions (ball position, stance, alignment, address posture) | 20 |
| **physical capacities** (`limited_ankle_dorsiflexion`, `poor_core_stability`, …) | **14** |
| **intents, beliefs and habits** (`trying_to_lift_the_ball`, `aim_bias_open`, `tempo_habit`, grip) | **9** |
| **equipment** (`club_too_short`) | **1** |
| **motion faults filed in the wrong group** (`inside_takeaway`, `outside_takeaway`) | **2** |

No coach files "limited ankle dorsiflexion" under *setup*. It is there because the enum offers
nowhere else. `setup` has become the null group — which is exactly what happens when a taxonomy is
missing a dimension.

### 3.2 Every diagnosis will terminate at a physical screen

33 conditions are causal roots (no in-edge, at least one out-edge). **24 of them are unmeasurable** —
14 screened capacities, 10 asserted intents. Combined with rule 1 of the resolver — *a
characteristic with an in-pack cause is never a root* — the arithmetic is forced: for almost any
real swing, the concluded root cause will be a mobility screen.

That is a defensible clinical stance only if the evidence supports it, and the library's own
citation says it does not. `ref.gulgin2014` (PMID 24476744) is the one empirical test of physical
screens against visible swing faults, and its central negative finding — the rotation screen
associated with **no** swing fault at all — is already why every hip-restriction edge in this pack is
`moderate` rather than `strong` (`docs/design/lower_body_face_on_metrics.md`). The graph shape
quietly undoes that care: the screens are weakly linked but they are the only things left standing
when everything with an in-edge has been demoted.

**What a coach wants back is the fault**, with the capacity offered behind it: *"you are coming over
the top; limited lead-hip internal rotation may be why — screen it."* The model has all three facts
and no way to say them in that order, because it cannot tell a fault from a capacity.

### 3.3 Reach-ranking has no prevalence prior, and the guide already says so

`ADDENDUM-03` replaced out-degree ranking with causal reach, correctly. But reach is pure graph
topology, and on the shipped pack it puts **`tempo_habit` — reaching 29 conditions and 11 bad shots
— above `early_extension`, which reaches 9 and 5.** An experienced coach reading a list with "settled
tempo habit" at the top and early extension halfway down will close the panel.

The missing term is named explicitly in the developer guide, in the discussion of `strengthWeight()`:

> **Not P(cause | effect)**, which is what ranking causes from an observed effect actually wants.
> Bayes needs a base rate for how common each cause is, and no `Condition` carries one.

That base rate is the "big twelve" intuition, stated as a quantity. It is not a new idea to be
argued for — **it is a hole the code has already documented and worked around.**

### 3.4 The `axis` mechanism applies unevenly, so its health checks do too

45 conditions hold an axis; 48 hold none. A fault modelled by Pattern A gets an axis and is covered
by `bothTailsOneCondition` and `inconsistentReach`. A fault modelled by Pattern B gets neither, and
no check notices. Over the top has no axis and no check will ever ask it for one.

Worse, Pattern A **manufactures conditions coaches do not have words for** to fill the opposite tail:
`backing_off_the_ball`, `arms_over_connected`, `over_rotation_at_top`, `locked_lead_arm`,
`club_short_of_parallel`, `excessive_lag`, `pelvis_sink_backswing`. These are legitimate readings —
the other tail of a corridor is a real state — but they sit in the same list, with the same weight
and the same visual treatment, as *casting* and *sway*. There is no field that separates the thing a
lesson is about from the thing a corridor happens to have two ends of.

---

## 4. What the ontology actually is, from the coaching side

A coach carries six kinds of thing, and does something different with each. This is not a taxonomy
invented for the schema; it is the order of a lesson.

| kind | what it is | how it is established | what you do about it |
|---|---|---|---|
| **Outcome** | what the ball did | ball flight, strike | nothing directly — it is the reason for the conversation |
| **Delivery** | the geometry at impact that produced the outcome | measured: path, face-to-path, attack angle, low point, shaft lean, strike location | nothing directly — it is the scoreboard of the downswing |
| **Fault** | a movement error in the swing | seen, then measured | **this is what the lesson is about** |
| **Setup** | a static state before the swing starts | seen, measured | told and rehearsed — often fixed inside one session |
| **Capacity** | what the body can and cannot do | physically screened | referred, or trained over months |
| **Intent** | what the golfer is trying to do or believes | asked | talked about |

(Plus **Equipment**, which is one condition today and is genuinely none of the above.)

Two things fall straight out of this table that the current model cannot express.

**Delivery is not a fault, and it is not an outcome.** `out_to_in_path`, `open_face_to_path`,
`attack_too_steep`, `low_point_behind_ball` form a nearly *closed determinant set*: given path, face,
attack angle, low point, strike location and speed, the ball flight is fully determined. They are the
ball-flight laws. Modelling them as ordinary conditions in `armsAndClub` and `impact` loses the one
property that makes them useful — that a fault is *explained by* which of them it broke, and every
outcome is *fully explained* by them. The whole graph has a waist at that layer and nothing marks it.

**Fault and Capacity are being ranked against each other on one list.** They are not commensurable.
A capacity is a *constraint on the search*, not an answer.

Applying the six kinds to the shipped pack as a first pass:

| kind | conditions |
|---|---|
| Fault | 64 |
| Outcome | 20 |
| Setup | 20 |
| Delivery | 17 |
| Capacity | 14 |
| Intent | 9 |
| Equipment | 1 |

Cross-tabulated against the existing `ConditionGroup`, the new axis is **not** redundant: `setup`
splits four ways (20 Setup / 14 Capacity / 9 Intent / 1 Equipment / 2 misfiled Faults) and
`armsAndClub` splits two ways (20 Fault / 4 Delivery). 26 of 145 conditions carry information the
group cannot express, and those 26 include **every physical screen and every intent node in the
library**.

---

## 5. Proposal A — `ConditionKind`, orthogonal to `ConditionGroup`

Add one enum. Do not touch `ConditionGroup`: it is the anatomy-and-phase axis, it orders the library
in swing order, and it is doing that job correctly. The two are orthogonal facets that were jammed
into one field.

```cpp
// WHAT KIND OF THING this is. Orthogonal to ConditionGroup, which says WHERE and WHEN.
//
// The distinction the library could not previously make: a movement error a lesson is about, an
// impact geometry that is merely the scoreboard, and a physical capacity that constrains what is
// coachable at all. All three used to be `Condition` with a group and two epistemic flags, and the
// flags drifted into recording producer coverage instead of the nature of the thing.
enum class ConditionKind {
    Fault,      // a movement error in the swing. What a lesson is about.
    Setup,      // a static state before the swing starts.
    Delivery,   // impact geometry — path, face, attack angle, low point, strike. The scoreboard.
    Outcome,    // what the ball did.
    Capacity,   // what the body can do. Screened, never measured from pixels.
    Intent,     // what the golfer is trying to do or believes. Asked, never concluded.
    Equipment,  // the clubs.
};
```

**Then the three existing fields get their meanings back**, because each is doing one job again:

- `ConditionKind` — what sort of thing it is. **Intrinsic. Never changes when a producer lands.**
- `Observability` — can it be seen in the swing. Intrinsic to the movement.
- `ConfirmedBy` — how it is *established today*. **This is the one that is allowed to move**, and it
  moves when a producer or a launch-monitor connector lands. Exactly as `MeasureStatus` already does.

That last line is the repair. The reason over the top drifted is that `ConfirmedBy` had no
kind field beside it, so it absorbed a claim about the thing when all it ever meant was a claim about
today's capture.

**Invariants worth gating in `validatePack()`** (all satisfiable by the shipped pack after the
migration in §8):

| rule | why |
|---|---|
| `Capacity` ⇒ `confirmedBy == Screened` and `observability == Latent` | a capacity is by definition not visible in the swing |
| `Intent` ⇒ `confirmedBy == Asserted` | you can only ask |
| `Fault` ⇒ `observability == Observable` | if it cannot be seen in the swing it is not a swing fault; it is a capacity or an intent |
| `Outcome` ⇒ `group == BallFlight` | keeps the existing group honest |
| a `Fault` may not cause a `Capacity` | the arrow only runs one way; a swing does not change what your hip can do |
| a `Delivery` may not cause a `Fault` | the scoreboard does not cause the movement |

The last two are real content checks. The pack currently has no way to state either, and both are
authoring errors a tired reviewer will make.

---

## 6. Proposal B — `prominence`: the subjective ranking, done so it can be wrong

The ask is a curated "big twelve". The answer is: **do not author the list, author the ranking; the
list is then a query** — `kind == Fault`, sorted by prominence, top twelve. This costs no maintenance
and — usefully — it dodges the branding problem entirely, because there is no list to accidentally
reproduce. There is only a ranking that this library owns and can defend.

### Author it in words, store a rung, weight it as one number in 0..1

Exactly the pattern `Strength` established at `42fa144`, and for the same reasons:

```cpp
// How often this condition is present in the population a coach actually sees.
// Read as a frequency, stored as a rung, weighted as P(condition) in 0..1.
enum class Prominence { Rare, Uncommon, Occasional, Common, Ubiquitous };
double prominenceWeight(Prominence);   // 0.02 / 0.06 / 0.15 / 0.30 / 0.50
```

Neither bound is available, for the same two reasons `strengthWeight()` excludes them: 0 is
indistinguishable from the field's absence, and 1 is absorbing under Bayes and no authored coaching
claim earns it.

Provenance discipline is the point, not a footnote:

- **There is no peer-reviewed prevalence study of named swing faults.** Every value ships
  `tier: practice`. Any UI that renders a rank must be able to say where the rank came from, and the
  honest answer is *this library's editorial judgement*.
- **It must be re-seatable.** Prominence is a *prior*. The moment the swing library has volume, the
  posterior is measurable — count how often each condition fires across the corpus, the way
  `startCorpusCheck()` already sweeps for `oneBandCorpus`. Then prominence gains `source` and `n`
  exactly as a `Norm` has, and the authored ladder becomes what it always was: a starting figure.
- **The seat will be biased and must say so.** Corpus prevalence can only count what can *fire*, so
  it will systematically under-rate over the top (never), early extension (planned) and every
  outcome behind a launch monitor. Report the scope with the number, the way the 2000-swing cap is
  reported.
- **Do not build a gate that pins a prominence value.** Trap 5 applies unchanged.

### Where it is consumed

1. **Ranking.** `RankedCause::score` becomes `prominence(cause) × Σ strengthWeight(path)` — the base
   rate the guide names as missing. This is the change that stops `tempo_habit` outranking early
   extension.
2. **The library, the glossary and the intake picker**, all of which currently sort on id or on
   graph topology.
3. **Curriculum** — "the twelve worth learning first" is a query, and it stays current as the
   ranking is re-seated.

**Owed measurement before any of that ships.** The `Strength` re-cut set the precedent and the bar:
it was landed only after measuring that the rescale preserved 98% of cause pairings on the shipped
pack and that every reordered pair was a near-tie. A prominence term multiplying into `score` will
reorder far more than that — deliberately — so the obligation is to *measure and publish the
reordering*, not to assert that it looks better. Anything that moves these numbers owes the same.

---

## 7. What it changes downstream

| surface | change |
|---|---|
| `characteristic.h` / `.cpp` | one enum, one field, two label tables |
| `characteristic_pack.cpp` | parse, serialise, and the six new validator rules |
| `relation_resolver.cpp` | kind-aware ranking: conclude the best `Fault`, offer `Capacity` behind it |
| `model_browser.cpp` | a facet, a column, a sort — and **grep the marshaller** (trap 2) |
| `dag_layout.cpp` | kind is the natural swim-lane; the graph currently lays out by group |
| `diagnostics_health.cpp` | `faultNoProminence`, `kindEdgeDirection`, `faultNotObservable` |
| content | one `kind` and one `prominence` per condition, 145 rows |

Blast radius is small — `ConditionGroup` is referenced in four non-test files — and the content edit
is mechanical for 119 of 145 rows.

**What it does *not* change**: the causal graph. This is the part that makes the proposal cheap and
the part worth stating plainly. Over the top already causes `out_to_in_path`, is already corroborated
by `steep_downswing_shaft`, and is already caused by `hips_under_rotated_at_top` and
`late_pelvis_rotation`. **The graph was authored correctly by a coach's instinct and then tagged
wrongly by a schema that had no cell for it.** Almost nothing moves; things get names.

---

## 8. The over-the-top repair, specifically

> **DONE, both steps (2026-08-11).** Step 1 shipped earlier and went further than
> this section asked — `over_the_top` is `observable`/`measured`, not
> `observable`/`asserted`. Step 2 shipped with the transition_plane producer
> (`docs/implementation/transition_plane_producer_brief.md`), but **the measure is
> not the one sketched below**. What landed is `m_transitionPlaneDelta`: a
> **face-on** reading, reducer `at` the `transition` anchor, measuring the change
> in swing-plane *inclination* between the backswing and downswing windows from
> the shaft vector's own ellipse. The `swingPlane`-based `delta p4→p5` row this
> section specified was retired unbuilt — it needed depth from a down-the-line
> camera that nothing produces, so it could never have fired. Both tails are
> wired as described (`sig_overTheTop` high, `sig_shallowing` low), and the
> `kind: delivery` treatment of the shallowing trap below is exactly what was
> authored. The measure ships `status: live` with a **provisional placeholder
> corridor** (mu 0, sigma 25°) set deliberately wider than anything the corpus
> contains — its surfacing edge is 50° against a widest well-conditioned swing
> of 45.6°.
> So both tails are genuinely assessable rather than permanently Unavailable
> (corpus coverage 51 → 53 conditions), while nothing real reaches the band. The
> characterisation behind it is still one golfer, so that corridor is to be
> **replaced, not tightened**; see the brief's §7 and §9, and the norm row's own
> citation, which records why sigma 30 rather than the ~16 the data would imply.

Two steps, and the first needs no schema change at all.

**Step 1 — today, no code.** Retag `over_the_top` from `latent`/`asserted` to `observable`/`asserted`
— the cell seven ballFlight conditions already occupy. It becomes concludable by the resolver's own
rules the moment it is `observable`, and it stops claiming to be an intent.

**Step 2 — give it the measure it should always have had.** Over the top is not the same event as
`steep_downswing_shaft`: a golfer can be steep at P6 from a steep *backswing* without ever going over
the top. The fault is the **outward re-route at the start of the downswing** — P4 to P5 — and it
wants its own measure:

```
m_transitionPlaneShift   swingPlane, Delta p4→p5, degrees, status: planned
                         highMeans: "the shaft moved OUTSIDE the plane it was on at the top —
                                     the club is working out toward the ball line, not down"
axis: transition_plane   high tail -> over_the_top
                         low tail  -> shallowing
```

Then `over_the_top` becomes `observable`/`measured` on a `planned` measure — exactly early
extension's situation — and the two faults are finally the same kind of thing. It joins the roadmap
as a producer row instead of sitting outside it forever.

**A trap in that second tail, worth writing down before somebody authors it.** The under-plane tail at
P5 is *shallowing* — the move good players make deliberately, and one of the most coached
improvements of the last decade. It is not a fault. Under the current schema it would be minted as
one, because Pattern A's opposite tail always is. Under the proposed schema it is `kind: Delivery`
with a low prominence and no `injuryNote`, and the corridor is wide. **This is the clearest single
argument for the kind field**: without it, the model is about to manufacture a fault out of a
technique coaches teach.

---

## 9. What I would not do

- **Do not fold `ConditionGroup` into the kind axis.** Group answers *where and when* and orders the
  library in swing order. Both facets are needed; the failure was having only one.
- **Do not add a `bigTwelve: true` flag.** A hand-maintained list rots, cannot be re-seated from a
  corpus, and is the one form of this idea that could be accused of reproducing somebody's product.
  A ranking is defensible; a list of twelve is a copy.
- **Do not add a valence field to make shallowing "good".** The no-valence rule in
  `characteristic.h` is right and the kind axis is the correct instrument: shallowing is not a
  beneficial *fault*, it is not a fault.
- **Do not wire the kind axis into `detect()`.** Detection is per-signal and stays that way. Kind
  belongs to ranking and presentation. Nothing here justifies wiring the engine, which still waits on
  the one adapter and the one design decision in §8.2 of the developer guide.
- **Do not migrate content and schema in one commit.** The enum plus a default, the validator rules
  in a second change, the content in a third — so a failing gate names one cause.

---

## 10. Open questions for the author

1. **Is `Delivery` a kind, or is it the `impact` group doing its job?** The argument for a kind is
   that path and face-to-path are *determinants*, not faults, and four of them sit in `armsAndClub`
   rather than `impact`. The argument against is that it is nearly recoverable from the group today.
2. **Should `Setup` be a kind at all, or a `Fault` with a phase of P1?** A coach does distinguish
   "setup fault" from "swing fault" — different fix modality, different timescale — but the word
   *fault* applies to both.
3. **How coarse should `Prominence` be?** Five rungs mirrors `Strength`; three (`Ubiquitous /
   Common / Rare`) may be all anybody can honestly author on a `practice`-tier claim.
4. **Does prominence vary by cohort and context?** A tour player's fault distribution is not a
   beginner's. `Norm` already solved this shape with `(context, cohort)` and the machinery exists —
   but a prominence keyed three ways is 145 × 13 × 6 authoring slots, which is how a field becomes
   one nobody fills in.

---

## 11. What actually shipped — `ae9b57d`, 2026-08-03

All seven kinds, the five-rung ladder, the over-the-top repair, four new warnings and two new load
errors. Every one of the 146 conditions carries a kind and a prominence.

| kind | n | | kind | n |
|---|---:|---|---|---:|
| Fault | 69 | | Capacity | 14 |
| Setup | 20 | | Delivery | 13 |
| Outcome | 20 | | Intent | 9 |
| | | | Equipment | 1 |

The four open questions of §10 were answered: Delivery ships as a kind (Q1); Setup does too, and
S- and C-posture went with it as address postures rather than swing faults (Q2); the ladder has five
rungs (Q3); and prominence is **not** keyed by cohort or context (Q4) — a single global rung, on the
argument this document already made, that 146 × 13 × 6 slots is how a field becomes one nobody fills.

### The four corrections implementation made to this proposal

**1 · The weight ladder is 0.05 / 0.10 / 0.20 / 0.35 / 0.60, not §6's 0.02–0.50.** Two reasons. A
0.02 floor claims to discriminate one-in-fifty from one-in-seventeen on a judgement with no study
behind it, and a rung nobody can author against a real row gets picked by feel and then defended by
its number. And the spread matters more than the values: 25× against `strengthWeight`'s 9.5× would
let a Ubiquitous cause covering one weak finding outrank a Rare cause covering two very strong ones —
which is this document's own complaint about ranking by topology, with the sign flipped. At 12× the
two terms stay commensurable.

**2 · Delivery is 13 conditions, not §4's 17.** Axis tilt at impact, pelvis rotation at impact and
the bowed lead wrist are body and hand positions that *cause* delivery; they are Faults. The narrower
set is the club's geometry only. This is not a tidiness point: with the wider set, three shipped
edges trip a Delivery→Fault rule and with the narrow one exactly one does.

**3 · `deliveryCausesFault` was not shipped, because the invariant is unsound.** §5 proposed "a
Delivery may not cause a Fault — the scoreboard does not cause the movement". Its one counterexample
on the shipped graph, `under_plane_stuck → face_held_open_impact`, is a *true* coaching claim: being
stuck under the plane makes you hold the face open to save it. **Compensation chains are real**, and
a delivery error routinely produces a rescue movement. What shipped instead is `outcomeHasEffect` —
an Outcome with an outgoing causal edge — which is sound, because what the ball did genuinely cannot
cause the swing that produced it. `screenedHasCause` was left keyed on `ConfirmedBy` rather than
moved to `Capacity`: its value is that it is unscoped, and it guards the field `relation_resolver`
actually reads.

**4 · `faultNoProminence` is unimplementable and was replaced.** Prominence has five legitimate
values and no sentinel, so a row nobody authored and a row somebody authored at the default rung are
the same bytes once loaded — and 58 shipped conditions sit at that rung on purpose. Any validator
predicate either accuses those 58 or reports nothing. `core_pack_test` asks the question where it is
answerable, by reading the shipped JSON as raw text.

### What the prevalence field made visible on day one

The strongest evidence the field was worth adding, and it was invisible before there was a base rate
to sort on: **prominence and detectability run inversely at the top.**

| rung | fires | dark | |
|---|---:|---:|---|
| Ubiquitous | 1 | 3 | **25%** |
| Common | 19 | 3 | 86% |
| Occasional | 18 | 6 | 75% |
| Uncommon | 14 | 2 | 87% |
| Rare | 3 | 0 | 100% |

Every rare fault in the library is detectable and three of the four commonest are not — `over_the_top`,
`early_extension` and `loss_of_posture` are all on planned measures, while `casting` alone fires. The
mechanism is not mysterious: the frequent faults are whole-body events needing depth, and the rare
ones tend to be single-joint readings a face-on camera already resolves.

**This is a priority ordering, not a defect list**, and it is what the model is for. The library is
authored ahead of its producers deliberately, so that it can be reviewed with coaches and then used
to decide which producer earns building next. A health check reporting these six rows was considered
and rejected for exactly that reason — a warning that a modelled item is not yet implemented restates
the purpose of the exercise.

### The ranking now uses it

`score(cause) = P(cause) × Σ P(effect | cause)`, entering at `rankWeight()`. Measured with
`rank_shift_report`, which runs `explain()` twice over the same code — once on the shipped pack, once
on a copy at a single rung, since uniform prominence is a scalar multiple and therefore *is* the
pre-change ordering. Over 409 ranked synthetic finding sets and 1328 cause pairings: **78.9%
preserved, and only 7.1% reordered by score** — the remaining reorderings follow from the greedy
cover picking a different cause first and are consequences of that pick rather than independent
judgements.

Do not compare that to the strength re-cut's 98%. That was a rescale of an existing term; this is a
new one, and a new term that reordered nothing would not have been worth adding. The question is
whether the reorderings are defensible, and the widest evidence margin prominence overturns is 2.7×
against a 3× prevalence ratio — the two ladders behaving commensurably, which is what §6's spread
argument was for. The top of the list is `early_extension` over `late_pelvis_rotation`,
`out_to_in_path` over `late_pelvis_rotation`, and `over_the_top` over `poor_core_stability` — which
is, precisely, this document's §3.2 complaint being answered.

---

## 12. The duplication review — shortcut edges audited, twins separated (2026-08-07)

**Occasioned by**: "the model contains duplication and has added complexity of limited value."
The review swept the whole graph structurally — parallel edges, reciprocal edges, corroborates
shadowing a causal path, dangling screen/drill/citation references, duplicated norm corridors —
and found the enforced invariants holding on all of them. What it did find is below, with what was
done about each. Figures in this section were read at 152 conditions / 334 edges and the section's
changes take the pack to 338.

### 12.1 Fifty-five triangles, and the rule that makes them legal

55 of the 311 causal edges are transitive shortcuts: `A → C` where a causal chain `A → B → C` also
ships. Under this resolver none of them is redundant — coverage is deliberately ONE HOP
(`findingsCoveredBy` → `effectsOf`), so a screened capacity's direct edges are precisely what let a
recommendation say "this screen would explain four of your findings". But a graph that holds both
chains and shortcuts is doing two jobs at once, and nothing said which job a given edge was doing.
Now something does: **an edge is an independent claim, never a coverage device** — normative at the
`Edge` struct (`characteristic.h`), which is where an author writing one is looking.

Each of the 55 was read against that rule. They fall into four classes, and every one survives:

| class | n | verdict, with exemplars |
|---|---:|---|
| Screened-capacity fan-out | 21 | The load-bearing class. `limited_lead_hip_ir → early_extension`, `poor_pelvic_disassociation → sway` — each is screen doctrine stated directly, and each is what makes the capacity's screen recommendation reach the findings it should. |
| Fault-to-fault compensation | 21 | `over_the_top → strike_heel`, `casting → thin`, `posture_too_bent → shank` — coaching states these without mentioning the mediator, which is the test the rule sets. |
| Setup and intent geometry | 9 | `ball_back → launch_low`, `ball_too_close → shank`, `grip_strong → closed_face_to_path` — first-order geometry a coach teaches as one step. |
| Delivery-to-outcome laws | 4 | `under_plane_stuck → push`, `excessive_shaft_lean → launch_low` — ball-flight law shortcuts; the delivery mediator exists but the claim is made directly everywhere it is taught. |

**Zero removed.** That is a finding, not a formality: the pack's shortcuts were all written as
claims, none for reach. The rule exists so the next fifty-five are too.

### 12.2 Conditions the graph could not tell apart

Seven sets of conditions shared identical edge neighbourhoods — as candidate causes the resolver
could order them only alphabetically. Three were accidents of thin authoring and got the edge each
one's own `consequence` text already claimed:

- `inside_takeaway` said "throwing it out and over, **or** by getting stuck underneath" but only
  the stuck half was an edge. Now also `→ over_the_top`, separating it from
  `flat_backswing_plane`.
- `steep_backswing_plane` said "sets up a steep, out-to-in delivery" but pointed only at
  `over_the_top`. Now also `→ steep_downswing_shaft`, separating it from `outside_takeaway` and
  `across_the_line`.
- `flying_elbow` said "requires a re-route to deliver the club" and caused **nothing** — a fault
  whose own text promises a consequence the graph never stated. Now `→ across_the_line`, which
  also separates `across_the_line` from `outside_takeaway`.

The remaining four sets are deliberate, and saying so here is the fix:

- `laid_off` / `shallowing` — both express "club under the plane coming down"; the distinction is
  carried by kind (Fault vs Delivery) and by their measures, not by their edges, and shallowing's
  own text says it is not a fault at all.
- `excessive_knee_flex` / `pelvis_thrust_backswing` / `weight_in_heels_address` /
  `limited_trail_ankle_dorsiflexion` — four genuinely different address-and-backswing precursors
  of one downswing fault. Interchangeable as covers, separated by kind, confirmedBy and strength,
  which is what the resolver ranks on.
- `ball_too_far` / `thoracic_kyphosis` — a setup and a capacity that produce the same posture;
  separated by kind, confirmedBy and strength. The pair is the model's own point: the coach fixes
  one with a ball position and the other with a referral.
- The `feet_alignment` / `hip_alignment` open and closed pairs — parallel segment readings of one
  aim, corroborating `alignment_*` by design.

### 12.3 Two one-line repairs

- "standing up going back" answered to both `pelvis_rise_backswing` and `head_rise_backswing` — a
  live `duplicateAlias` warning, and a search term that led to whichever loaded first. The pelvis
  condition owns it (it is the whole-body phrase); the head condition now answers to "head comes
  up going back".
- `insufficient_knee_flex` was the pack's only edgeless condition — it could fire and then land in
  `unexplained`, explaining nothing, while its own text claimed "the arms and shoulders end up
  supplying the speed". That claim is now its edge: `→ ball_speed_deficit`, weak, practice tier
  (searched 2026-08-07, nothing peer-reviewed tests the address-flex-to-speed link directly).

### 12.4 What the review deliberately left alone

Two apparent duplications are working as designed and are now documented where an author will meet
them. The launch-monitor measure pairs (`m_ballSpeed` / `m_lmBallSpeed` and kin) exist because a
monitor-less user still needs every ball-flight quantity producible from our pixels, and a user
with a monitor gets an independent reading to validate our estimate against — one merged measure
could do neither job; the full argument sits with `MeasureStatus` in `measure_vocabulary.h`. And
the signal layer's near-1:1 shape (121 of 122 signals are one corridor test on one measure) is a
stage, not a ceiling — signals grow multi-measure tests in later iterations, which is why the
layer is not folded into the conditions it serves.
