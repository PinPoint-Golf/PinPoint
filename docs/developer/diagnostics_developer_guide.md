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

Three registries and the rules that join them.

| Registry | Owns | Lives in |
|---|---|---|
| **Metric catalogue** | what can be measured, and what it means | `src/Metrics/` — see its own guide |
| **Characteristic pack** | measures, signals, characteristics, causal edges | `src/Diagnostics/` + `core.json` |
| **Norm set** | what normal looks like, per measure per context | `src/Diagnostics/` + `norms.json`, `contexts.json` |

The design rule that shapes everything: **numbers are content, not code.** No corridor is compiled in. Before stage 9 of the norms work there was a table in `reference_bands.cpp`; it was migrated behind a byte-for-byte parity gate and then deleted along with the gate, because a parity test that outlives what it compares against pins the shipped content to a frozen table and blocks the first legitimate re-seat.

The second rule: **a metric describes itself and does not judge itself.** `MetricDescriptor` carries identity, units, meaning and requirements. It carries no bands. Judgement belongs to a norm, which is keyed on a *measure* — and a measure is a metric plus the decision of which reading is meant.

---

## 2. Where it fits

```
 CONTENT (reviewable JSON, shipped in Qt resources)
   core.json          measures · signals · conditions · edges
   norms.json         norms  (measure, context) -> mu/sigma/monitor + provenance
   contexts.json      the shot-type tree
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
 VIEWS   Settings → Diagnostics (4 views) · Settings → Metrics · the wrist grid
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
    MeasureStatus status;          // Live | Planned | NoProducer | NotCapturable
    QString       gapReason;       // NotCapturable: why, in one line
    QString       highMeans;       // what a HIGH value means, in the measure's own words
};
```

Two fields carry more weight than their size suggests:

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
    ConditionGroup              group;         // Setup | Posture | Lateral | ArmsAndClub | Release | Sequence
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
struct Edge { QString from, to; EdgeType type; Strength strength; QString citation; };
// EdgeType: Causes | Corroborates | Excludes      Strength: Weak | Moderate | Strong
```

`Causes` must form a DAG — `validatePack()` refuses a cycle, because the assembled library is re-validated after every merge and one circular edge would fail every characteristic, not just the two involved. `Corroborates` is refused between a pair that already has a causal path either way: the pair would double-count when the explanation is ranked.

### Norm — what normal looks like

```cpp
struct Norm {
    QString measureId, contextId;      // the key. NEVER an athlete id
    double  mu, sigmaLo, sigmaHi;      // asymmetric BY DESIGN, not as an option
    std::optional<double> monitorLo, monitorHi;   // absolute Watch bounds — migrated content only
    int        n;  NormSource source;  QString unit, author, citation;  QDate setOn;
    std::optional<NormBasis> basedOn;  // what an override was made against (user rows only)
};
```

**All norms are population norms.** There is no per-athlete norm and nothing here should ever gain one — a norm that differs per player is a different feature with different storage and a different UI. Seating from a chosen set of swings records `n` and the scope in provenance; the result still applies to everyone using that norm set.

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

`INormProvider::resolve(measureId, contextId)` walks **up** the chain from the requested context; the nearest row wins. It is **non-virtual on purpose** — every provider must resolve identically, or a grade would depend on which layer a norm happened to be stored in.

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

The Ideal band is policy-**independent** (it is the norm's own claim, mu ± sigma). Only the Watch edge moves with the policy, and only for a norm with no explicit monitor band. `bandEdgesOf()` is the single definition of that precedence, consumed by `NormBandProvider` (the wrist grid), `NormModel::normAt` (the measures view) and `metric_corridor.h` (metric detail + dashboard). It replaced three copies, one of which — the corridor editor's — was drawing the wrong Watch edge for all 56 migrated rows.

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

Per condition: skip `Latent` ones (no signals by definition), resolve the context binding, evaluate every signal, emit a `Finding`.

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

`TestRecommendation` ("screen this — it would explain four of your findings") is the highest-value output of the model and costs no capture hardware: the dominant causes in the pack are screen-backed, so a handful of physical tests explain most of what was detected.

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
| **`EdgeType::Corroborates` / `Excludes`** | validated and refused where illegal | content — all 81 shipped edges are `Causes` |

**Why the engine is not wired is not laziness.** Three of the four inputs exist. The undecided one is *where findings surface for a coach* — nothing in the product specifies it, and that is a design question before it is an implementation one. Wiring it without an answer would produce a correct diagnosis nobody sees.

### 8.3 Content — the shipped numbers

**Pack** (`core.json`) — 67 measures, 31 signals, 50 conditions, 81 edges:

| Measures | | Signals | | Conditions | |
|---|---|---|---|---|---|
| Live | **38** | `outsideCorridor` | 30 | Observable | 31 |
| Planned | 27 | `order` | 1 | Latent | 19 |
| NoProducer | 2 | with a direction | 30 | `Measured` | 31 |
| Provided | 58 | | | `Screened` | 13 |
| Composed | 9 | | | `Asserted` | 6 |
| with `highMeans` | 27 | | | with an axis | 11 |
| with a norm | 44 | | | with binding rows | **0** |

Reducers: 38 `delta`, 20 `at`, 9 `extremum`. Edges: 32 strong, 40 moderate, 9 weak — all `Causes`.

**Norms** (`norms.json`) — 68 rows: 44 at `any`, 8 + 8 at the two archetypes, 2 each at driver / fairway wood / iron / wedge. **Every row is `source: heuristic` with `n = 0`**; 56 carry explicit monitor bounds (the migrated wrist grid + tempo), 7 carry a citation.

**Contexts** (`contexts.json`) — 13 nodes, one root. 7 have norms at or beneath them; the other 5 (`partial`, `pitch`, `chip`, `bunker`, `specialty`) carry none of their own and inherit from `any`, which the health list reports as harmless.

### 8.4 Content — what can actually fire

**8 of the 30 corridor signals can fire.** The other 22 all sit on measures that are `planned` (20) or
`noProducer` (2) — every signal on a **live** measure has a norm, which `core_pack_test` asserts. So the
dark ones are waiting on *producers*, not on corridors.

The single `order` signal (`sig_sequenceOrder`) cannot fire either: both of its measures
(`m_pelvisRotPeak`, `m_thoraxRotPeak`) are `planned`. So **8 of 31 signals in total** are capable of
firing, and that is a producer ceiling, not a content one.

The 9 `Composed` measures (facet-built rather than metric-backed) are all `planned` or `noProducer` and none has a norm: the whole Composed path is content-complete and production-dormant.

Condition fields in use: `consequence` 50/50, `provenance` 50/50, `screenRef` 13, `injuryNote` 4.
`Condition::drills` is a real field with **no content behind it anywhere** (0 of 50) — the one place
the pack is structurally ready for something nobody has authored.

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

`health()` merges three sources — the pack validator's warnings, the norm set's own (which `norm_provider.h` had promised were "part of the health list" while they were not), and `diagnosticsHealth()`. Codes, with what each means an author should DO, are documented at the top of `diagnostics_health.h`.

Two scoping rules there are load-bearing:

- **`personalNormNoSample` is scoped to the personal layer** via `INormProvider::isOverridden` — tracked at merge time, never derived by comparing values (a user row holding the shipped numbers is still the user's). Unscoped, it opens with 39 rows of noise about migrated content that was fine yesterday.
- **`signalNoNorm` is scoped to LIVE measures.** A producer-less measure is already the roadmap's row, and reporting it here would rank it as though authoring a corridor would fix it.

`oneBandCorpus` is different in kind: it needs a pass over the swing library, so it is opt-in (`startCorpusCheck()`), runs on a worker, is capped at 2000 swings **with the cap reported**, and `corpusEverScanned` exists so "nothing found" and "nothing checked" cannot read the same.

**Do not add a check that pins shipped values.** Health checks look for wrong *shapes* — no norm, a unit mismatch, everything graded into one band — never for specific numbers. That is the same mistake the deleted parity gates made.

---

## 10. The GUI façades

Four `QML_ELEMENT` marshallers, all instantiated by `CharacteristicLibrary.qml` (the Diagnostics settings panel, `ScreenSettings.qml` panel 10).

| Façade | Owns | Notes |
|---|---|---|
| `CharacteristicLibraryModel` | read-only queries over the pack: directory, detail, DAG, roadmap, capture gaps, cause coverage, **health**, the corpus scan | holds the pack, the norm set AND the metric catalogue, because health spans all three |
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
| `core_pack_test` | the SHIPPED pack: ids, referential integrity, and that every live corridor signal can fire |
| `axis_direction_test` | every corridor signal points the way its own condition claims. The fixture carries the catalogue quote that decides each row, so a new signal with no fixture row **fails** |
| `norm_test` | the grade rule: per-side z, and monitor bounds dominating in both directions |
| `norm_pack_test` | persistence, validation, layering — and that context specificity beats layer precedence |
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
| `norm_model_test`, `norm_editor_model_test`, `characteristic_editor_test`, `diagnostics_catalogue_integrity_test` | the façades: every rule they hide is asserted here or nowhere |

Standalone tests reach shipped content through `pp_norm_env(<target>)` and `-DPP_CORE_PACK_PATH`.

---

## 13. Traps that have already cost time

Each of these is a bug that shipped or nearly shipped. They are here because none of them looks like a bug while you are writing it.

1. **An inverted signal is invisible.** It fires confidently, on the wrong swings, with correct-sounding consequence text. `highMeans` + `axis_direction_test` exist solely to prevent it. Three signals shipped inverted.
2. **A field can be complete on both sides and reach nothing.** Add to the marshaller in the same change, and grep it when you add to a value type.
3. **A rule can exist, be tested, and never run.** `validateNormsAgainst` for nine stages; `detect()` today. Add the call site in the same change, or write down that you did not.
4. **A corridor keyed on the wrong phase disappears silently.** The reducer must name the phase the producer labels.
5. **Do not pin shipped numbers in a test.** Two parity gates had to be deleted for exactly this. Gate *shapes* and *relationships*, not values.
6. **Unit strings can match while conventions do not.** `normUnitMismatch` compares strings and is structurally incapable of catching stance width reading ~2× its corridor in a field both sides spell `% shoulder width`.
7. **Never derive "edited" by comparing values.** A user row holding the shipped numbers is still the user's. Track it at merge time.
8. **`NotMeasured` is never a pass**, and an omitted condition is not a `NotFired` one. Three distinct states; nothing may collapse them.
9. **Inside a Repeater delegate, only the component root id resolves** — and a handler on a composite type that declares its own `id: root` cannot see even that. It throws only on click, so no binding, test or screenshot will show it. At file scope, `root.x` inside a composite is fine.
10. **A `const Condition *` dies when `save()` reloads the provider.** Copy what you need out first.
11. **Text links are `Theme.colorAccent` at rest**, body font, trailing arrow, cursor change only (`ScreenHome.qml`'s `switchLink`). Muted-until-hover is for secondary chrome, not for a way out of the page.

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
  measure_facets.{h,cpp}        Composed-measure vocabulary + validity table
  anatomy_vocabulary.{h,cpp}    body-part vocabulary shared with the overlays
  dag_layout.{h,cpp}            the causal graph, laid out in C++
  diagnostics_health.{h,cpp}    assembled-library checks
  {resource,file,merged}_{pack,norm}_provider.cpp   layering
  pack_provider.h               ICharacteristicPackProvider, PackOrigin

src/Gui/characteristics/        4 façades + 11 QML views
src/Resources/diagnostics/      core.json · norms.json · contexts.json
src/Analysis/reference_bands.{h,cpp}   NormBandProvider — Norm -> Band for the wrist grid
src/Gui/review/metric_catalog.cpp      corridors for the metric + dashboard surfaces
```

**Read next:** `docs/design/diagnostics_norms.md` (the brief), `docs/implementation/diagnostics_norms_impl_plan.md` (how it was built, its ledger, and the handoff to *Diagnosis execution, V&V*), `docs/user/pinpoint-diagnostics-guide.md` (the same model in non-technical language — useful for naming things the way users will), `docs/design/pinpoint_sign_conventions.md` (before touching any direction), and `docs/developer/metric_catalogue_developer_guide.md` (the other registry).
