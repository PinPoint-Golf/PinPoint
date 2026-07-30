# Pinpoint Diagnostics — Developer Guide

**Audience**: developers working on the diagnostics model — characteristics, measures, norms, contexts, the causal graph, or the screens over them
**Location**: `src/Diagnostics/` (the model), `src/Gui/characteristics/` (the façades and views), `src/Resources/diagnostics/` (the content)
**Language**: C++17 value types and rules, Qt-only, no Qt-GUI below the façade layer
**Status**: the CONTENT layer is production and shipped. The EXECUTION layer — running a diagnosis against a real shot — is written, tested, and **not wired**. Section 8 is the precise breakdown; read it before assuming any of this runs.

---

## Contents

1. [What this subsystem is](#1-what-this-subsystem-is)
2. [Where it fits](#2-where-it-fits)
3. [The data model](#3-the-data-model)
4. [The two joins, and how resolution works](#4-the-two-joins-and-how-resolution-works)
5. [Grading](#5-grading)
6. [Detection and explanation](#6-detection-and-explanation)
7. [Providers, layering and persistence](#7-providers-layering-and-persistence)
8. [**Live, dormant, planned — the full breakdown**](#8-live-dormant-planned--the-full-breakdown)
9. [Validation and health](#9-validation-and-health)
10. [The GUI façades](#10-the-gui-façades)
11. [Adding things](#11-adding-things)
12. [Testing](#12-testing)
13. [Traps that have already cost time](#13-traps-that-have-already-cost-time)
14. [File map](#14-file-map)

---

## 1. What this subsystem is

Five registries and the rules that join them.

| Registry | Owns | Lives in |
|---|---|---|
| **Metric catalogue** | what can be measured, and what it means | `src/Metrics/` — see its own guide |
| **Characteristic pack** | measures, signals, characteristics, causal edges | `src/Diagnostics/` + `core.json` |
| **Norm set** | what normal looks like, per measure per context | `src/Diagnostics/` + `norms.json`, `contexts.json` |
| **Screen set** | the physical tests a `screenRef` names — protocol and pass criterion | `src/Diagnostics/screen_pack.*` + `screens.json` |
| **Drill set** | what a golfer does about a characteristic | `src/Diagnostics/drill_pack.*` + `drills.json` |

The last two are flat reference lists rather than provider hierarchies, and deliberately: the
pack/norm polymorphism exists so a COMMUNITY pack can be namespaced against core, and there is no
community story for a screen. They layer a user file over the shipped one and stop there.

The design rule that shapes everything: **numbers are content, not code.** No corridor is compiled in. Before stage 9 of the norms work there was a table in `reference_bands.cpp`; it was migrated behind a byte-for-byte parity gate and then deleted along with the gate, because a parity test that outlives what it compares against pins the shipped content to a frozen table and blocks the first legitimate re-seat.

The second rule: **a metric describes itself and does not judge itself.** `MetricDescriptor` carries identity, units, meaning and requirements. It carries no bands. Judgement belongs to a norm, which is keyed on a *measure* — and a measure is a metric plus the decision of which reading is meant.

---

## 2. Where it fits

```
 CONTENT (reviewable JSON, shipped in Qt resources)
   core.json          measures · signals · conditions · edges
   norms.json         norms  (measure, context) -> mu/sigma/monitor + provenance
   contexts.json      the shot-type tree
   screens.json       the physical tests a screenRef names — protocol + pass criterion
   drills.json        what a golfer does about a characteristic
        │
        │  resource_pack_provider / resource_norm_provider  (+ file_* for the user's own,
        │  merged_* to layer them)
        ▼
 MODEL  (C++ value types + rules, no Qt-GUI)
   characteristic.h        Measure · Signal · Condition · Edge  + their enums
   characteristic_pack.h   the pack, the metric->measure join, validatePack()
   norm.h                  Norm · GradePolicy · grade() · bandEdgesOf() · ragOf()
   context_tree.h          the tree, chain(), resolveContextBinding()
   metric_corridor.h       (metric, phase) -> measure -> norm -> four band edges
   characteristic_engine.h detect() -> findings          ◄ NOT WIRED (§8)
   relation_resolver.h     findings -> ranked causes     ◄ NOT WIRED (§8)
   measure_sample.h        read a measure off a stored swing (phase grid + sidecar)
   dag_layout.h            the causal graph, laid out in C++
   diagnostics_health.h    assembled-library checks
        │
        ├──────────────► Analysis/reference_bands.h  NormBandProvider
        │                   projects a Norm into a Band for the WRIST GRID  (live)
        │
        ├──────────────► Gui/review/metric_catalog.cpp
        │                   corridors for MetricDetail, PpBandRail, 3 dashboard zones  (live)
        │
        ▼
 FAÇADES (QML_ELEMENT, marshalling only — no rules)
   CharacteristicLibraryModel · CharacteristicEditorModel · NormModel · NormEditorModel
        │
        ▼
 VIEWS   Settings → Diagnostics (6 views) · Settings → Metrics · the wrist grid
```

**All logic stays in C++.** QML renders shapes and holds no rules: it does not walk the context tree, does not decide what "inherited" means, does not compute a band edge from a policy, and does not lay out the causal graph. Every one of those is a statement about correctness, and a statement about correctness written in a delegate is a statement nothing can test.

---

## 3. The data model

### Measure — a metric plus which reading

```cpp
struct Measure {
    QString       id;              // stable, never reused
    MeasureKind   kind;            // Provided (a metric key) | Composed (facet-built)
    Series        series;          // Composed only
    Reducer       reducer;         // WHERE the phase lives. At / Delta / Rate / Extremum
    QString       metricKey;       // Provided only — links into MetricCatalogue
    QString       label;
    QStringList   aliases;
    QString       unit;
    ViewNeeded    viewNeeded;      // FaceOn | DownTheLine | Any
    MeasureStatus status;          // Live | Planned | NoProducer | NotCapturable | ExternalDevice
    QString       gapReason;       // NotCapturable: why, in one line
    QString       highMeans;       // what a HIGH value means, in the measure's own words
};
```

**`ExternalDevice` is the third answer to "why is there no value", and not a shade of either
neighbour.** A launch monitor resolves face angle, spin and strike location; integrating one is work
we intend to do, so `NotCapturable` would be false. But the work is an INTEGRATION, not a producer
written from our own pixels, and a roadmap row reading "build a spin-axis producer" sends somebody
the wrong way — so it is roadmap-eligible and **sectioned apart** in `roadmap()` and
`roadmapMarkdown()`, while `captureGaps()` stays `NotCapturable`-only. The per-shot half lives on
the requirement (`MetricRequirement::launchMonitor`): a golfer with no launch monitor gets "needs a
launch monitor" through the same path a missing face-on camera takes, and the day a connector sets
`ShotContext::hasLaunchMonitor` the same metric resolves Measured with no content change. A norm on
one is legitimate — it is what the reading will be graded against — which is why `normNotCapturable`
stays scoped to `NotCapturable`.

Three fields carry more weight than their size suggests:

- **`reducer` is where the phase lives.** There is no phase field. `At{p7}` is impact; `Delta{p1→p4}` is the change to the top; `Extremum{p5..p6, Min}` is a trough across a stretch. Two measures over one metric with different reducers are *related, not duplicates* — that is the whole point of the split, and `m_leadWristAtImpact` (absolute) versus `m_leadWristFlexExt_p7` (Δ from address) is the case that proves it.
- **`highMeans` is a correctness mechanism, not decoration.** It is the sentence an author reads *instead of* picking "High" or "Low" when attaching a signal. Three signals shipped inverted because someone picked a direction against a sign convention that was unstated or the opposite of what they assumed — and an inverted signal fires happily, on the wrong swings, with correct-sounding consequence text attached. The measure picker refuses to mint a measure without one.

### Signal — the rule that watches

```cpp
enum class SignalTest { OutsideCorridor, Threshold, Order, Ratio };

struct Signal {
    QString                  id;
    SignalTest               test;
    QStringList              measures;   // 1 for corridor/threshold, 2 for order/ratio
    std::optional<Direction> direction;  // High | Low — WHICH TAIL
    std::optional<double>    threshold;  // Threshold only
};
```

`OutsideCorridor` is the preferred form: it authors no numbers and inherits the measure's norm, so re-seating a corridor from a corpus updates every signal on it. `Threshold` authors a number and needs a citation to be more than an opinion.

`direction` is not optional in spirit. It names which tail the signal watches, and an axis has two conditions on one norm — without the side check both tails would fire on any deviation in either direction.

### Condition — a characteristic (or a latent cause)

```cpp
struct Condition {
    QString                     id, label, axis;
    QStringList                 aliases;       // coach phrasing that resolves here — the glossary
    ConditionGroup              group;         // Setup … Sequence | Impact | Finish | BallFlight
    Observability               observability; // Observable | Latent | Both
    ConfirmedBy                 confirmedBy;   // Measured | Screened | Asserted
    ConditionState              state;
    QStringList                 detectedBy;    // signal ids
    LocalizedText               consequence;   // why it matters — always present
    LocalizedText               injuryNote;
    QString                     screenRef;     // Screened: which physical screen
    Provenance                  provenance;    // tier + citation + author
    std::vector<ContextBinding> bindings;      // per-context applicable/material
};
```

Three distinctions the UI must never blur:

- **`Observability`** — can it be *seen* in the swing? A `Latent` condition has no signals by definition; it is resolved by the explanation pass from what it explains, never detected.
- **`ConfirmedBy`** — how it can be *established*. `Measured` (a signal fired, the app knows), `Screened` (a physical test would settle it), `Asserted` (intent, habit, perception — knowable only by asking).
- **`ProvenanceTier`** — is it cited, or proposed? A `Proposed` condition must be badged as such.

**`Asserted` causes are offered, never concluded.** They are surfaced, visually distinct, phrased as a question, and they never count as resolving a finding. Dropping them instead would leave characteristics whose only cause is habit with an empty explanation panel, when the truthful answer is "this may simply be how they set up".

### Edge — the causal graph

```cpp
struct Edge { QString from, to; EdgeType type; Strength strength; Provenance provenance; };
// EdgeType: Causes | Corroborates | Excludes      Strength: Weak | Moderate | Strong
// An edge carries the SAME Provenance as a condition — tier, citation, searchedOn, searchTerms.
// (A bare `QString citation` is the pre-pass-1 shape; the loader still migrates it, promoting a
//  legacy top-level citation to Supported. Do not author that form.)
```

`Causes` must form a DAG — `validatePack()` refuses a cycle, because the assembled library is re-validated after every merge and one circular edge would fail every characteristic, not just the two involved. `Corroborates` is refused between a pair that already has a causal path either way: the pair would double-count when the explanation is ranked.

### Norm — what normal looks like

```cpp
struct Norm {
    QString measureId, contextId;      // the key…
    Cohort  cohort;                    // …and its optional third term. NEVER an athlete id
    double  mu, sigmaLo, sigmaHi;      // asymmetric BY DESIGN, not as an option
    std::optional<double> monitorLo, monitorHi;   // absolute Watch bounds — migrated content only
    std::optional<double> plausibleLo, plausibleHi;   // outside these the reading is NOT BELIEVED
    int        n;  NormSource source;  QString unit, author, citation;  QDate setOn;
    std::optional<NormBasis> basedOn;  // what an override was made against (user rows only)

    double claimLo() const;            // mu - sigmaLo — the norm's OWN claim, policy-free
    double claimHi() const;            // mu + sigmaHi
};
```

`claimLo()/claimHi()` are **not** the Ideal band. The Ideal band is `mu ± idealMaxZ × sigma` and comes from `bandEdgesOf()`; it moves with the grade policy and the claim does not. See §5.

**`plausibleLo/Hi` answer a different question from the corridor.** The corridor asks whether the swing was good; these ask whether the reading is real. Outside them the grade is `NotMeasured` **plus a distinct `implausible` flag**, and plausibility outranks even the monitor band — a driver smash of 1.62 is a mis-tracked ball, and grading it in either direction would launder a capture fault into a confident diagnosis. Three states, never merged: a capture GAP (nothing arrived), a swing FINDING (something arrived and was poor), a capture FAULT (something arrived that this instrument cannot produce). On the ROW rather than the measure — the opposite of `Shape` — because the physical cap is context-dependent: smash is bounded by loft.

**Shape lives on the MEASURE, not here** (`characteristic.h`). One-sidedness is a property of the quantity and cannot differ between a driver row and an iron row, so norm rows carry only numbers and `validateNormsAgainst` joins the two — exactly as it already does for `unit`. On a floor, `sigmaHi` must equal `sigmaLo` and only `monitorLo` is legal; both are named load errors.

**All norms are population norms**, and the inviolable rule is that a norm is **never keyed by athlete id**. It used to read "one population for everyone", which the optional `Cohort` retires: a cohort is a *segmented* population — men 55–64 — not a person. A norm that differs per player is a different feature with different storage and a different UI. Seating from a chosen set of swings records `n` and the scope in provenance; the result still applies to everyone in that cohort.

**Cohort shifts the corridor and NEVER inverts valence** — the same rule context obeys — and band-edge cliffs are accepted: a grade can shift on a 55th birthday, and there is no interpolation, because interpolating implies a continuity the banded literature does not support.

`monitorLo/Hi` exist ONLY to reproduce migrated content exactly. The old table added a fixed number of degrees either side of green, which is not a fixed multiple of the green half-width and so cannot be reproduced by any global z policy. Anything authored in the corridor editor omits them and derives its Watch edge from the `GradePolicy`.

`basedOn` is the base of a three-way comparison. Without it, "the shipped corridor has been revised since you overrode it" is undecidable — *your row differs from the shipped row* is also just what an override IS, so a two-way comparison would report every edit forever.

### Context — the shot-type tree

```cpp
struct ContextNode { QString id, label, parentId; };   // one root: `any`
```

Shipped shape, and the thing to know about it:

```
any ── full_swing ── {driver, fairway_wood, iron, wedge, archetype_bowed, archetype_cupped}
    ├─ partial ── {pitch, chip}
    ├─ bunker
    └─ specialty
```

**The general case lives at `any`, not at `full_swing`.** A full swing is a shot *type*, with no more claim to being the general case than a pitch has. All 44 general norm rows sit at the root. Putting them at `full_swing` is a live bug shape, not a style preference: `partial`, `bunker` and `specialty` are *siblings* of `full_swing`, so a pitch or bunker shot could not see them, and every reading on such a shot came back `NotMeasured`. `diagnostics_health_test` pins `ungradedContext` at zero to stop it recurring.

**Shot type informs a diagnosis; it never gates one.** `ContextBinding::applicable` remains as the general exclusion mechanism — it is simply always true while shot type is its only key. `material` is how context is allowed to speak: an immaterial finding is still produced, still listed, still counted in coverage, and contributes **zero** to the ranking score.

---

## 4. The two joins, and how resolution works

### Metric → measure (`characteristic_pack.h`)

```cpp
std::vector<const Measure *> measuresForMetricAtPhase(pack, metricKey, phase);   // ordered
const Measure              *measureForMetricAtPhase (pack, metricKey, phase);    // the first
```

A measure matches a phase **through its reducer**, because that is where the phase lives:

| Reducer | Matches |
|---|---|
| `At` | its anchor phase |
| `Delta` / `Rate` | the window's END phase — the measure IS the change observed there |
| `Extremum` | **never**. "The lowest lag angle between P5 and P6" is not a reading at P5 or at P6 |

Preference order is **absolute before change**, because a corridor keyed on a phase means the absolute reading there. Both `m_leadWristAtTop` (at P4) and the Δ cell name P4 on one metric; the absolute one wins.

The list form exists because the winner can carry no norm. `m_leadWristAtTop` is preferred at P4 and has no norm at all — a caller taking only the winner draws nothing at the top of the swing while the Δ measure beside it has had a corridor since v1.

### Measure → norm (`norm_provider.h`)

`INormProvider::resolve(measureId, contextId, athlete = {})` walks **up** the chain from the requested context; the nearest row wins. It is **non-virtual on purpose** — every provider must resolve identically, or a grade would depend on which layer a norm happened to be stored in.

**The walk is CONTEXT-MAJOR.** At each node of the context chain the cohort keys are probed most-specific-first — sex+band, sex+adult, band, adult, sex, unqualified (`cohortProbeOrder`, `norm.h`) — and only when none is present does it move up the tree. Two intended consequences: an unqualified `driver` row beats a senior row at `any` for a driver shot, and a senior row at `any` answers wherever no club row exists. Inverting the loops would make every cohort row shadow every club row beneath it.

`athlete` is the GOLFER's cohort, derived from their date of birth **at the swing date** (`ageBandFor`) and never stored — an athlete ages across their own history. An axis with no answer is skipped, never guessed: unknown date of birth, unknown sex or a declined answer means only rows unqualified on that axis match, and the reading **still grades** against the universal corridor. An athlete we know nothing about yields exactly one probe, so resolution costs what it did before cohorts existed.

Two fallbacks that look similar and are not:

| Requested | Behaviour |
|---|---|
| **empty** | resolves at the default context (`full_swing`); the caller marks the finding *inferred* and the engine demotes its confidence |
| **unknown to the tree** | resolves to **nothing**. Grading it against full-swing norms would be a wrong answer wearing a right answer's clothes |

### The pair, composed (`metric_corridor.h`)

```cpp
std::optional<MetricCorridor> corridorForMetricAtPhase(pack, norms, metricKey, phase,
                                                      contextId, policy = {});
```

Header-only, free-standing, and deliberately **not** inside the QML façade that consumes it — a rule that exists only inside a façade is a rule nothing can test. It walks the candidate measures in preference order and takes the first one a norm resolves for, reporting which one answered (`measureId`, `deltaFromAddress`) because the candidates are different quantities.

Returns `nullopt` — never a zeroed corridor — when no measure reads that metric at that phase, or none has a norm. "The corridor is 0 to 0" is a different claim.

---

## 5. Grading

`norm.h`, header-only, no Qt-GUI.

```cpp
enum class Grade { Ideal, Good, Watch, Action, NotMeasured };
struct GradePolicy { double idealMaxZ = 1.0, goodMaxZ = 2.0, watchMaxZ = 3.0; };
double normZ(value, norm);                 // signed, PER SIDE
Grade  grade(value, norm, policy = {});
```

`normZ` computes distance in tolerances **per side**, so an asymmetric norm grades asymmetrically. A zero tolerance on the relevant side yields infinity rather than dividing by zero: a norm admitting only its own centre is degenerate but well-defined, and the validator warns about it separately.

**Precedence in `grade()`**, when the norm carries explicit monitor bounds:

```
outside [monitorLo, monitorHi]   -> Action
otherwise                        -> the z-derived band, CAPPED AT WATCH
```

The cap is what makes migrated content exact: across all 39 migrated rows the monitor bounds are strictly tighter than 3σ, so a value inside them can never legitimately reach Action by z alone — and a value outside them was RED under the old classifier however few tolerances out it was. Both halves are needed to reproduce that.

### One projection, three consumers

```cpp
struct NormBandEdges { double idealLo, idealHi, watchLo, watchHi; };
NormBandEdges bandEdgesOf(norm, policy = {}, marginOverride = -1.0);
```

**Every edge here is policy-dependent, Ideal included.** `bandEdgesOf()` is the single definition of that precedence, consumed by `NormBandProvider` (the wrist grid), `NormModel::normAt` (the measures view) and `metric_corridor.h` (metric detail + dashboard). It replaced three copies, one of which — the corridor editor's — was drawing the wrong Watch edge for all 56 migrated rows.

**The norm's own claim is a different object and does not live here.** `Norm::claimLo()/claimHi()` is `mu ± sigma` — what the norm asserts about the population — and it never moves when a user changes the grade policy. Use it for **authoring and diff** (the corridor editor's handles, a comparison against a shipped or parent row, the validator); use `bandEdgesOf()` for anything that **renders or grades**. The two are not interchangeable, and every call site is a deliberate choice between them.

They were interchangeable until 2026-07-28, wrongly. `bandEdgesOf()` set `idealLo = mu − sigmaLo` while `grade()` applied `policy.idealMaxZ`; under `standard` (idealMaxZ = 1.0) those coincide, which is why nothing caught it for nine stages. Under `strict` (0.75) a value at 0.9σ was **drawn inside the green band and graded `Good`** — and `ragOf(Good)` is Amber, so one number produced a green band and an amber chip. Under `lenient` (1.5) a value at 1.3σ was drawn outside green and graded Ideal. This is exactly the disagreement `withinBand`'s own comment exists to eliminate, at whole-band scale rather than float-epsilon scale.

`norm_test` now sweeps both Ideal edges under every preset, and asserts `gradePolicyIsOrdered()` — `idealMaxZ > 0`, `goodMaxZ ≥ idealMaxZ`, `watchMaxZ ≥ goodMaxZ`. The engine silently depends on that ordering: `onTail` compares against the Ideal edge while `deviated` comes from `grade()`, so a preset with `goodMaxZ < idealMaxZ` would admit a reading that is a deviation on neither tail — a signal that can never fire, for a reason nothing reports. It held only because three hand-written presets happened to be ordered.

`marginOverride` is the SwingLab `bands.*` sweep. It exists because half the norms do not store their Watch edge as a margin at all, so sweeping "the margin" without it would silently do nothing on those.

### `ragOf()` and the 3-band collapse

The wrist grid renders the legacy `PpRag` (Green/Amber/Red). `ragOf(Grade)` is the single collapse, and **it is not 2:1:1**: for migrated rows Ideal is exactly the old green band, so Good AND Watch both sit inside the old amber. A surface showing a grade word and a RAG chip together will look self-contradictory — show one or the other. `reference_bands_test` sweeps every shipped norm asserting `ragOf(grade(v)) == classifyDelta(v)`, over both branches of the precedence rule.

### A signal fires on a DEVIATION

Watch or Action — **not** merely on leaving Ideal. Ideal is the middle of normal, so firing at `|z| > 1` would trip roughly a third of a normal population on every characteristic.

---

## 6. Detection and explanation

Both are written and tested. **Neither is called by the app** — see §8.

### `detect()` — `characteristic_engine.h`

```cpp
DetectionResult detect(const CharacteristicPack &pack, const IMeasureSource &source,
                       const ContextTree *contexts = nullptr, const QString &contextId = {});
```

Per condition: skip `Latent` ones (no signals by definition), skip `Retired` and `Superseded` ones,
resolve the context binding, evaluate every signal, emit a `Finding`.

**`ConditionState` gates detection on those two values and no others.** They are the states that
mean *withdrawn*, and withdrawn content must not diagnose — until this was added the engine read all
six identically, so retiring a characteristic changed a badge and nothing else. The other four are
editorial confidence rather than withdrawal, and **62 of the 112 shipped conditions are `Draft`** —
reading "not finished" as "not in use" would dark more than half the library while every census
still said it was there. A `Superseded` condition must name its successor (the validator refuses
otherwise), so the replacement is already in the pack and evaluates in its place.

Three rules that must not be softened downstream:

1. **An inapplicable condition is OMITTED** — not `NotFired` (which claims it was assessed and found absent), not `Unavailable` (which claims the app tried and could not). Both would be wrong in a way a coach could read. Nothing may backfill the gap.
2. **No corridor ⇒ `Unavailable`, never a pass.** This is the single most important branch in the engine: most of the pack has no corridor yet, and "we could not assess this" must never render as "this is fine".
3. **`material` is unweighted, never softened.** An immaterial finding is listed and counted in coverage and adds **zero** to the ranking score — zero rather than a fraction, because a fraction would be a number nobody could defend.

The measure source seam:

```cpp
class IMeasureSource {                // what detect() reads
    virtual std::optional<MeasureReading> read(const QString &measureId) const = 0;
};
class IMeasureValueSource {           // raw values, no grading
    virtual std::optional<Value> value(const QString &measureId) const = 0;
};
class NormMeasureSource final : public IMeasureSource;   // joins values + norms -> graded readings
```

### `relation_resolver.h` — the explanation pass

Fired conditions → ranked root causes + test recommendations. Two rules do the work:

1. **A characteristic with an in-pack cause is never a root.** The graph is not two clean layers — early extension causes loss of posture, casting causes scooping. Presenting a leaf as a root hands the coach a symptom and calls it the diagnosis.
2. **An `Asserted` cause is offered, never concluded.**

`TestRecommendation` ("screen this — it would explain four of your findings") is the highest-value output of the model and costs no capture hardware: the dominant causes in the pack are screen-backed, so a handful of physical tests explain most of what was detected. The `screenRef` it carries now resolves to a protocol and a pass criterion in `screens.json`, so the recommendation names a test somebody can actually run.

### The two non-causal edge types

Both are consumed by `explain()`, and the asymmetry between them is deliberate.

- **`Excludes` changes the output.** Two findings that cannot both describe one swing are resolved
  BEFORE ranking: the more confident stands, ties break on the id, and the dropped one is recorded
  in `Explanation::suppressed` with a reason rather than vanishing. Leaving both in would put a
  contradiction in front of a coach and let one cause be credited twice for two versions of one
  event. A suppressed finding is never also listed as `unexplained`.
- **`Corroborates` is reported, and breaks ties.** It never scales a score. A multiplier would mean
  inventing a number nobody could defend when asked why one cause outranked another — the same
  reason `material` contributes zero rather than a fraction. `characteristic_engine_test` asserts
  the scores are identical with and without the edge.

Both are symmetric in meaning, so `relatedBy()` reads them from either end and an author may write
the edge whichever way round they think of it.

---

## 7. Providers, layering and persistence

Both registries follow the same shape: an abstract provider, a resource-backed core, a file-backed user layer, and a merger.

| | Pack | Norms |
|---|---|---|
| Interface | `ICharacteristicPackProvider` | `INormProvider` |
| Shipped | `resource_pack_provider.cpp` (`:/diagnostics/core.json`) | `resource_norm_provider.cpp` |
| User's own | `file_pack_provider.cpp` | `file_norm_provider.cpp` |
| Layered | `merged_pack_provider.cpp` | `merged_norm_provider.cpp` |
| Assembled by | `makeCharacteristicPackProvider()` | `makeNormProvider()` / `sharedNormProvider()` |

`PINPOINT_CORE_PACK` / `PINPOINT_CORE_NORMS` / `PINPOINT_CORE_CONTEXTS` override the paths, because the Qt resource exists only inside the app binary and without that seam no standalone test or offline tool could reach shipped content.

**Rules that have already caused bugs:**

- **Context specificity BEATS layer precedence.** A user adjusting the general norm must not silently override every club-specific shipped norm beneath it. `norm_pack_test` pins this.
- **A merging caller keys off `parsed`, not `loaded`.** An overlay routinely fails *standalone* referential validation — its edges point at core conditions it does not itself contain. Keying off `loaded` made the characteristic editor start from an EMPTY user pack, and `save()` then erased every override the user had ever made.
- **A LocalUser pack REPLACES the whole incoming edge set of any condition it names as an effect.** So code that writes one edge must write the condition's whole incoming set — which is why `linkCause()` goes through `beginEdit`/`addCause`/`save` rather than appending.
- **`save()` reloads the provider**, destroying the pack any `const Condition *` points into. Copy labels out first.
- **`sharedNormProvider()` is cached process-wide.** Call `resetSharedNormProvider()` after writing a user norm set, or every reader keeps the assembly it already has and the edit looks like it did nothing. Not thread-safe against concurrent readers — call it from the UI thread.

---

## 8. Live, dormant, planned — the full breakdown

The section to read before assuming anything here runs. "Dormant" means written and tested but with no caller in the app; it is not the same as broken, and not the same as planned.

### 8.1 Code — live

| What | Consumed by |
|---|---|
| `characteristic.{h,cpp}`, `characteristic_pack.{h,cpp}` | everything; the pack loads at app start |
| `norm.h`, `norm_pack.{h,cpp}` | grading, projection, every façade |
| `context_tree.{h,cpp}` | norm resolution, the context list in the editors |
| `metric_corridor.h` | `metric_catalog.cpp` → MetricDetail, PpBandRail, dashboard Motion/Verdict/Setup zones |
| `NormBandProvider` (`Analysis/reference_bands.cpp`) | the wrist grid, live (`wrist_diagnostics_model`) and offline (`wrist_analyzer`) |
| `measure_sample.{h,cpp}` | the corridor editor's histogram, the corpus-share health scan |
| `dag_layout.{h,cpp}` | `DagView.qml` via `CharacteristicLibraryModel::dag()` |
| `diagnostics_health.{h,cpp}` | `CharacteristicLibraryModel::health()` |
| `measure_facets.{h,cpp}`, `anatomy_vocabulary.{h,cpp}` | Composed-measure vocabulary, the measure picker |
| all six providers + the two mergers | pack and norm assembly |
| 4 façades in `Gui/characteristics/` | the Diagnostics settings panel |

### 8.2 Code — DORMANT (written, tested, no caller)

| What | State | What it needs |
|---|---|---|
| **`detect()`** (`characteristic_engine`) | compiled into the app; **grep confirms no caller outside the module and its test** | a `NormMeasureSource` over a real shot, the shot's resolved context, and the chosen policy |
| **`relation_resolver`** (`resolveRelations`, ranked causes, screen recommendations) | compiled into the app; **its only `#include` is its own header** | findings from `detect()` |
| **`NormMeasureSource`** | referenced only by its own header | an `IMeasureValueSource` implementation (below) |
| **`IMeasureValueSource`** | declared; implemented **only by a test fake** | ⚠ **the real gap.** `measure_sample` provides `readPhaseGrid()` + `reduceOverGrid()` but no adapter to this interface. The corridor editor and the corpus scan both call `reduceOverGrid()` directly. Wiring the engine means writing that adapter |
| **`ContextBinding::applicable` / `material`** | resolved by `resolveContextBinding()`, honoured by `detect()`, editable in the UI | a caller of `detect()`. Also: **no shipped condition carries a binding row** (0 of 50), so both are inert in content as well |
| **`GradePolicy` reaching grading** | reaches three façades and the corpus scan | the engine (it takes a policy in its constructor already) |
| **`SignalTest::Threshold`** | implemented in `evaluate()` | content — no shipped signal uses it |
| **`SignalTest::Ratio`** | implemented in `evaluate()` | content — no shipped signal uses it |
| **`Norm::basedOn`** | written at save, read by `overrideCoreChanged` | time. No existing user row has one, so the check is correctly silent until an override is saved |
| ~~**`EdgeType::Corroborates` / `Excludes`**~~ | **no longer dormant** — 9 and 5 shipped, and `relation_resolver` consumes both | a caller of `explain()` |

**Why the engine is not wired is not laziness.** Three of the four inputs exist. The undecided one is *where findings surface for a coach* — nothing in the product specifies it, and that is a design question before it is an implementation one. Wiring it without an answer would produce a correct diagnosis nobody sees.

### 8.3 Content — the shipped numbers

**Pack** (`core.json`) — 106 measures, 83 signals, 112 conditions, 178 edges:

| Measures | | Signals | | Conditions | |
|---|---|---|---|---|---|
| Live | **44** | `outsideCorridor` | 82 | Observable | 90 |
| Planned | 51 | `order` | 1 | Latent | 22 |
| NoProducer | 2 | with a direction | 82 | `Measured` | 83 |
| ExternalDevice | 9 | | | `Screened` | 13 |
| Provided | 97 | | | `Asserted` | 16 |
| Composed | 9 | | | with an axis | 45 |
| with `highMeans` | 70 | | | with aliases | 86 |
| with a norm | 95 | | | with drills | 23 |
| | | | | with binding rows | **0** |

Reducers: 52 `at`, 42 `delta`, 10 `extremum`, 2 `rate`.
Edges: 164 `Causes`, 9 `Corroborates`, 5 `Excludes` — 69 strong, 94 moderate, 15 weak.

Provenance, after the pass of 2026-07-27 — **no `proposed` remains anywhere, and every condition
records the date its search was made**:

| tier | conditions | edges | |
|---|---|---|---|
| `supported` | 5 | 6 | the defining variable (conditions) or the named pair (edges) is directly measured |
| `indirect` | 23 | 25 | a source measures the mechanism underneath; the named claim is our reading |
| `practice` | 84 | 147 | searched, coaching consensus found, no peer-reviewed test of the named claim |
| `proposed` | **0** | **0** | nobody has looked — the tier a completed search pass empties |

`core_pack_test` no longer asserts `proposed > 0`; that was a mid-search proxy. It asserts
`practice > cited` plus an all-searched check instead. **Do not restore the old assertion** — adding
a proposed row back to satisfy it is the laundering it was written to prevent.
Groups: setup 41, ballFlight 20, armsAndClub 15, impact 9, lateral 8, posture 7, sequence 6,
release 3, finish 3.

**Norms** (`norms.json`) — 149 rows: 95 at `any`, 8 + 8 at the two archetypes, and 12 / 2 / 12 / 12 at driver / fairway wood / iron / wedge. **Every row is `source: heuristic` with `n = 0`**; 56 carry explicit monitor bounds (the migrated wrist grid + tempo), 30 carry a citation. Those citations are **notes, not identifiers** — `citation` is documented as "DOI/PMID, or the note explaining a provisional figure", and 23 of them name the paper the figure should be re-seated FROM. Nothing moved to `source: literature` in the 2026-07-27 pass, deliberately: that tier asserts the mu and sigma came off a results table, and nobody read one. See `docs/implementation/provenance_log.md` §6.

**Contexts** (`contexts.json`) — 13 nodes, one root. 7 have norms at or beneath them; the other 5 (`partial`, `pitch`, `chip`, `bunker`, `specialty`) carry none of their own and inherit from `any`, which the health list reports as harmless.

**Screens** (`screens.json`) — 13, all referenced by at least one condition. Nine carry a numeric pass floor with its unit; four are qualitative on purpose, and `reference_sets_test` asserts that so the absence of a number reads as intent rather than as unfinished content.

**Drills** (`drills.json`) — 12, attached to 23 conditions. Attached where a starter set can honestly answer the fault, not to everything — a field that is always populated is a field readers learn to skip.

### 8.4 Content — what can actually fire

**16 of the 83 signals can fire**, covering 16 conditions. Every one of the other 67 sits on a
measure that is `planned` (56), `externalDevice` (10) or `noProducer` (2) — **zero** are live with no
norm, which `core_pack_test` asserts. So the dark ones are waiting on *producers*, not on corridors.

That is double the eight signals of the previous content package, and the doubling came entirely
from a gap nobody had noticed: eleven producer keys were live and carried **no measure at all** in
the pack — head sway, head lift, head tilt, lead heel lift, the two foot flares, toe line, impact
shaft lean, hand speed, clubhead speed and backswing tempo. Putting measures and corridors on them
cost no pipeline work whatsoever.

The single `order` signal (`sig_sequenceOrder`) still cannot fire: both of its measures
(`m_pelvisRotPeak`, `m_thoraxRotPeak`) are `planned`.

**Every one of those 16 can also be EXPLAINED**, and that was not true when they were first
counted. Ten of them had no cause at all and four had neither cause nor effect — so the least
explicable part of the library was precisely the part a golfer would meet first. The gap formed at
the intersection of two healthy-looking states: the causal work went in per GROUP, while the firing
set is decided per PRODUCER, and the eleven live-but-unclaimed producer keys arrived after their
group had been wired. Nothing reported it, because `noCause` is a warning that fires 25 more times
on content nobody can capture yet. `core_pack_test` now gates it directly — scoped to what can fire,
because a `noCause` on a producer-less condition is a backlog and a condition that fires TODAY with
nothing behind it is a defect in what ships.

**A constraint that caps all of this**: most pose metrics are session-gated to Wrist Motion
(`wristSessionOk`, sessionType 1 or −1) — a documented catalogue design, not a bug — so the head,
foot and shaft-lean signals read Unavailable in a Swing session today. See the metric catalogue
guide, and ledger `X1` in the content-extension plan.

The 9 `Composed` measures (facet-built rather than metric-backed) are all `planned` or `noProducer` and none has a norm: the whole Composed path is content-complete and production-dormant.

Condition fields in use: `consequence` 112/112, `provenance` 112/112, `aliases` 86, `drills` 23,
`screenRef` 13, `injuryNote` 7, `bindings` **0**.

**References** (`references.json`) — 21, joined to the pack by `Provenance::citation` against
`doi` **or** `pmid` (exact string match, either field). One record carries a PMID and no DOI because
its journal issues none, and opens at PubMed rather than doi.org. Five are cited by nothing and are
kept on purpose: two contradict claims the pack makes, one governs the wording of every injury note,
one is background, and one is an arXiv preprint held for attribution only.

### 8.5 The honest summary

- **Everything that stores, resolves, projects, renders and edits the content is live**, and two real surfaces depend on it today: the wrist grid and the metric/dashboard corridors.
- **Everything that turns a shot into findings is dormant** — one adapter and one design decision away.
- **The numbers are all starting figures.** Nothing is seated on data; `n = 0` everywhere. Treat a shipped corridor as a hypothesis, and do not build a gate that pins one.

---

## 9. Validation and health

Four validators, plus one set of checks that spans them.

| Function | Sees | Runs where |
|---|---|---|
| `validatePack(pack)` | one characteristic pack | every load, both layers |
| `validateNormPack(norms)` | one norm set, standalone | every load; the corridor editor before a save |
| `validateContextTree(tree)` | one context tree | every load — duplicate ids, unknown parents, cycles |
| `validateNormsAgainst(norms, pack, tree)` | the ASSEMBLED library | `diagnosticsHealth()` — and nothing else, ever, until stage 10 |
| `diagnosticsHealth(pack, norms, catalogue)` | all three registries at once | `CharacteristicLibraryModel::health()` |

`validateNormsAgainst()` had owned `normUnitMismatch`, `unknownNormMeasure`, `unknownNormContext` and `normNotCapturable` since stage 1 with exactly one caller: its own test. **A check that never runs is indistinguishable from a check that passes.** If you add a referential check, add the call site in the same change.

**And a check that cannot stay silent is worse than none.** `observableNoSignal` fired on all seven
signal-less strike outcomes — `chunk`, `thin`, `top`, `sky`, `shank`, `pull_hook`, `push_slice` —
which are `Observable` (a golfer can plainly see a thin shot) and `Asserted` (we cannot measure it
from our pixels) *by design*. Seven rows describing the design is a health list people learn to
scroll past. It is now scoped by `ConfirmedBy`, exactly as `inconsistentReach` one line below it
always was. `bothTailsOneCondition`, `inconsistentReach`, `needsRevalidation` and `duplicateMeasure`
had no test in either direction, which is how that survived; all four are now gated both ways.

`health()` merges three sources — the pack validator's warnings, the norm set's own (which `norm_provider.h` had promised were "part of the health list" while they were not), and `diagnosticsHealth()`. Codes, with what each means an author should DO, are documented at the top of `diagnostics_health.h`.

Two scoping rules there are load-bearing:

- **`personalNormNoSample` is scoped to the personal layer** via `INormProvider::isOverridden` — tracked at merge time, never derived by comparing values (a user row holding the shipped numbers is still the user's). Unscoped, it opens with 39 rows of noise about migrated content that was fine yesterday.
- **`signalNoNorm` is scoped to LIVE measures.** A producer-less measure is already the roadmap's row, and reporting it here would rank it as though authoring a corridor would fix it.

`oneBandCorpus` is different in kind: it needs a pass over the swing library, so it is opt-in (`startCorpusCheck()`), runs on a worker, is capped at 2000 swings **with the cap reported**, and `corpusEverScanned` exists so "nothing found" and "nothing checked" cannot read the same.

**Do not add a check that pins shipped values.** Health checks look for wrong *shapes* — no norm, a unit mismatch, everything graded into one band — never for specific numbers. That is the same mistake the deleted parity gates made.

---

## 10. The GUI façades

Four `QML_ELEMENT` marshallers, all instantiated by `CharacteristicLibrary.qml` (the Diagnostics settings panel, `ScreenSettings.qml` panel 10). Six views hang off it: Characteristics, Measures & norms, Glossary, Screens & drills, Causes & health, and Roadmap (developer builds only).

| Façade | Owns | Notes |
|---|---|---|
| `CharacteristicLibraryModel` | read-only queries over the pack: directory, detail, DAG, roadmap, capture gaps, cause coverage, **health**, the corpus scan, and the two reference registries + the glossary | holds the pack, the norm set AND the metric catalogue, because health spans all three |
| `CharacteristicEditorModel` | the draft: one condition being authored, its signals, bindings, the measure picker, `linkCause`/`unlinkCause` + undo | a draft, deliberately separate from the read model |
| `NormModel` | measures & norms directory, `measureDetail`, `normAt`, the metric→measure join marshalled | read-only |
| `NormEditorModel` | one corridor being edited: the draft, the library scan, the histogram, save/discard/reset | writes; call `refresh()` on the readers after |

**The marshaller is a place a feature can be complete on both sides and still absent.** Stage 8 finished the DAG's headings, edge labels and arrowheads in C++ and they reached nothing, because `dag()` did not copy them into the QVariantMap. Nothing warns: QML reads `undefined` and renders nothing. **When you add a field to a value type, grep the marshaller.**

Grade policy and library paths are **bound from `AppSettings` by the hosting QML**, never read from settings inside a façade — the objects stay testable standalone. Three façades plus `MetricCatalog` each expose a `gradePolicy` property for that reason.

---

## 11. Adding things

### A norm

Author in `norms.json` (or seat it in the corridor editor). Key it on the **measure**, at the narrowest context where the claim is true. The general case goes at `any`. Unit must match the measure's — the loader refuses a mismatch naming both sides.

### A measure

Add to `core.json` or mint it in the measure picker. `Provided` needs a `metricKey`; `Composed` needs facets that pass the validity table. **Give it a `highMeans` sentence** — the picker will refuse without one, and the sentence is what a later author reads when choosing which tail a signal watches.

The reducer must name the phase the **producer** actually labels. `m_tempoRatio` asked P4 while `tempo_metrics` emits at P7, so it resolved unavailable on every swing for two stages *and* the corridor silently disappeared from two live surfaces.

### A characteristic

Needs a `consequence` (why it matters), a `ConfirmedBy`, an `Observability`, and either `detectedBy` signals or a reason it is latent. Causal edges are `Causes` from cause to effect; the validator refuses cycles, self-edges, and `Corroborates` over an existing causal path.

### A screen or a drill

Author it in `screens.json` / `drills.json`, in the `screen.` / `drill.` namespace — the join with
`Condition::screenRef` and `Condition::drills` is an exact string match, so an id outside the
namespace matches nothing and fails **silently**, which the validator refuses at load. A screen needs
a protocol and a pass criterion; a numeric pass floor needs its unit beside it. A drill needs an
instruction and a target, and the target is written as INTENT — nothing here has measured an effect
on anybody.

**Describe a screen generically, from the clinical range-of-motion literature.** No branded
screening system may be named, cited or alluded to. The movements are common property; the packaging
of them into a named system is somebody's product, and `reference_sets_test` greps the raw bytes.

### A new signal test

Add the enum, implement it in `evaluate()`, extend `validatePack()`'s arity/threshold checks, and add a case to `characteristic_engine_test`. `Threshold` and `Ratio` are already there as worked examples of the shape.

### A new health check

Put it in `diagnosticsHealth()`, document the code and its "what to DO" in the header's table, add a label in `HealthView.qml`'s `_codeLabel`, and gate it in `diagnostics_health_test` in **both** directions — the check firing when it should, and staying silent when it should not. Half the value of that test is the negative cases.

### A new file

Three targets: the app (`CMakeLists.txt`), the offline stack (`_pinpoint_offline_sources`, if `swinglab_run` or re-analyse needs it), and the test (`src/Analysis/tests/CMakeLists.txt`). A `.cpp` missing from the app link fails loudly; missing from the offline stack fails at `swinglab_run` link time.

---

## 12. Testing

All in the analyzer suite: `cmake -S src/Analysis/tests -B build/analyzer-tests` then `ctest --test-dir build/analyzer-tests`. Build with `--parallel 4` — the box OOMs above that.

| Suite | Gates |
|---|---|
| `characteristic_pack_test` | load, save, validation, every error and warning code |
| `core_pack_test` | the SHIPPED pack: ids, referential integrity, that every live corridor signal can fire, and that **everything which can fire can also be explained** — a condition detectable today with no cause is a defect, not a backlog item |
| `axis_direction_test` | every corridor signal points the way its own condition claims. The fixture carries the catalogue quote that decides each row, so a new signal with no fixture row **fails** |
| `norm_test` | the grade rule: per-side z, monitor bounds dominating in both directions, the drawn Ideal edge agreeing with the graded one **under every preset**, and the preset ordering invariant |
| `norm_pack_test` | persistence, validation, layering — that context specificity beats layer precedence, the cohort probe order exhaustively, and the band a date of birth derives at a given date |
| `seed_conversion_test` | the SHIPPED one-sided content: which band a value lands in either side of the smash-factor conversion, that shape ships on exactly one measure, and that the plausibility caps fall with loft. Pins no `mu` and no `sigma` — those are heuristics and are meant to move |
| `context_tree_test` | upward resolution, the unknown-context rule, the shipped tree |
| `measure_facets_test`, `anatomy_vocabulary_test` | the Composed vocabulary and its validity table |
| `measure_sample_test` | reading a measure off a stored swing; an unsegmented phase yields NOTHING, never a nearest-sample guess |
| `norm_measure_source_test` | values + norms → a graded reading, and the three silent failures (firing on ordinary variation, grading an unknown context against the default, an absent norm reading as a pass) |
| `characteristic_engine_test` | `detect()` and the explanation pass |
| `dag_layout_test` | no overlaps, determinism, depth bound, waypoint routing — samples every curve and fails on any box it crosses |
| `diagnostics_health_test` | every check, in both directions, plus the shipped library |
| `manifest_migration_test` | the (metric, phase) → measure → norm join for every metric that ever had a corridor |
| `reference_bands_test` | the Norm→Band projection, the archetype mechanism, and `ragOf(grade(v)) == classifyDelta(v)` over the whole shipped set |
| `wrist_norm_render_test` | the engine renders a real grid from the shipped norms — and an empty norm source greys everything, so the guard can fail |
| `reference_sets_test` | the screen and drill registries: every reference resolves both ways, each validator fires AND stays silent, and no branded screening system reached the content |
| `norm_model_test`, `norm_editor_model_test`, `characteristic_editor_test`, `diagnostics_catalogue_integrity_test` | the façades: every rule they hide is asserted here or nowhere — including that screens, drills and the glossary reach QML, which is the "complete on both sides and reaching nothing" trap |

Standalone tests reach shipped content through `pp_norm_env(<target>)` and `-DPP_CORE_PACK_PATH`.

---

## 13. Traps that have already cost time

Each of these is a bug that shipped or nearly shipped. They are here because none of them looks like a bug while you are writing it.

1. **An inverted signal is invisible.** It fires confidently, on the wrong swings, with correct-sounding consequence text. `highMeans` + `axis_direction_test` exist solely to prevent it. Three signals shipped inverted.
2. **A field can be complete on both sides and reach nothing.** Add to the marshaller in the same change, and grep it when you add to a value type.
3. **A rule can exist, be tested, and never run.** `validateNormsAgainst` for nine stages; `detect()` today. Add the call site in the same change, or write down that you did not.
3b. **Detection and explanation are authored on different axes, so the gap between them is invisible.** Causal edges go in per condition GROUP; what can actually fire is decided per PRODUCER. Add a live producer and the condition over it joins the firing set without ever passing the group sweep that would have given it a cause — which is how ten of the sixteen detectable conditions came to have no explanation while every census looked healthy. **When you make something newly detectable, check what explains it in the same change.**
3c. **A warning that fires on the design is worse than no warning.** `observableNoSignal` accused seven conditions of an omission that was the content telling the truth. Scope a check to the state it actually means, and gate it in BOTH directions — the negative case is what catches this, and four checks had neither.
4. **A corridor keyed on the wrong phase disappears silently.** The reducer must name the phase the producer labels.
5. **Do not pin shipped numbers in a test.** Two parity gates had to be deleted for exactly this. Gate *shapes* and *relationships*, not values.
6. **Unit strings can match while conventions do not.** `normUnitMismatch` compares strings and is structurally incapable of catching stance width reading ~2× its corridor in a field both sides spell `% shoulder width`.
7. **Never derive "edited" by comparing values.** A user row holding the shipped numbers is still the user's. Track it at merge time.
8. **`NotMeasured` is never a pass**, and an omitted condition is not a `NotFired` one. Three distinct states; nothing may collapse them.
9. **Inside a Repeater delegate, only the component root id resolves** — and a handler on a composite type that declares its own `id: root` cannot see even that. It throws only on click, so no binding, test or screenshot will show it. At file scope, `root.x` inside a composite is fine.
10. **A `const Condition *` dies when `save()` reloads the provider.** Copy what you need out first.
11. **Text links are `Theme.colorAccent` at rest**, body font, trailing arrow, cursor change only (`ScreenHome.qml`'s `switchLink`). Muted-until-hover is for secondary chrome, not for a way out of the page.
12. **A default setting can hide a whole-band defect, and the tests will agree with it.** The Ideal band was drawn at `mu ± sigma` and graded at `mu ± idealMaxZ × sigma` for nine stages. Under `standard` those are the same number, so nothing failed — and three separate tests had written the defect down as a requirement ("a policy change does not move the IDEAL band"). **When a value is a default, sweep the non-defaults**: `norm_test` and `norm_editor_model_test` now assert under every shipped preset, not only the shipped one.

---

## 14. File map

```
src/Diagnostics/
  characteristic.{h,cpp}        Measure · Signal · Condition · Edge, enums, label tables
  characteristic_pack.{h,cpp}   the pack, the metric->measure join, validatePack()
  norm.h                        Norm · GradePolicy · grade() · bandEdgesOf() · ragOf() · presets
  norm_pack.{h,cpp}             load/save/validate a norm set, enum spellings
  norm_provider.h               INormProvider, resolve(), shipped-vs-yours, the shared cache
  context_tree.{h,cpp}          the tree, chain(), resolveContextBinding()
  metric_corridor.h             (metric, phase) -> measure -> norm -> band edges
  norm_measure_source.h         IMeasureValueSource + NormMeasureSource        ◄ dormant
  characteristic_engine.{h,cpp} detect(), evaluate(), Finding                  ◄ dormant
  relation_resolver.{h,cpp}     ranked causes + screen recommendations         ◄ dormant
  measure_sample.{h,cpp}        phase grid + sidecar; read a measure off a swing
  screen_pack.{h,cpp}           the physical screens a `screenRef` names
  drill_pack.{h,cpp}            the drills a `drills` entry names
  measure_facets.{h,cpp}        Composed-measure vocabulary + validity table
  anatomy_vocabulary.{h,cpp}    body-part vocabulary shared with the overlays
  dag_layout.{h,cpp}            the causal graph, laid out in C++
  diagnostics_health.{h,cpp}    assembled-library checks
  {resource,file,merged}_{pack,norm}_provider.cpp   layering
  pack_provider.h               ICharacteristicPackProvider, PackOrigin

src/Gui/characteristics/        4 façades + 13 QML views
src/Resources/diagnostics/      core.json · norms.json · contexts.json · screens.json · drills.json
src/Analysis/reference_bands.{h,cpp}   NormBandProvider — Norm -> Band for the wrist grid
src/Gui/review/metric_catalog.cpp      corridors for the metric + dashboard surfaces
```

**Read next:** `docs/design/diagnostics_norms.md` (the brief), `docs/implementation/diagnostics_norms_impl_plan.md` (how it was built, its ledger, and the handoff to *Diagnosis execution, V&V*), `docs/user/pinpoint-diagnostics-guide.md` (the same model in non-technical language — useful for naming things the way users will), `docs/design/pinpoint_sign_conventions.md` (before touching any direction), and `docs/developer/metric_catalogue_developer_guide.md` (the other registry).
