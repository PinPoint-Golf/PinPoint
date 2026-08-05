# Session Diagnostics — Design Document

**Status:** Draft for review · **Author:** design discussion, August 2026
**Feeds:** (1) Claude Code build/test brief · (2) Claude Design UX/UI brief

---

## 0. Purpose and governing principles

A session diagnostics panel that the golfer glances at after every shot. It accumulates
evidence as shots are struck, identifies recurring movement patterns and the causal
chains that link them, shows trends as the golfer responds, and — because it is
cumulative by construction — **is** the session summary at close. There is no separate
summary object, renderer, or concept.

The design divides labour in one sentence: **a shot fires findings; only a session
diagnoses.** Everything else follows from that.

Principles inherited from the diagnostics model, restated here because the session
layer must honour them at a new scale:

1. **An absent measure is unavailable, never a pass.** Session denominators count
   *assessable* shots only; conditions no shot could assess are reported as coverage,
   not as absence of fault.
2. **Report, never soften.** Immaterial or context-demoted findings are recorded and
   visible; materiality affects ranking weight only.
3. **The pack is a specification.** The session layer consumes whatever subset of the
   140 conditions is producible today and degrades honestly — ghost links, coverage
   lines — where it is not.
4. **Facts, not judgements.** The session asserts deviations, recurrence, and
   consistency with authored causal structure. It never asserts proof of causation and
   it never inverts valence by context.
5. **No fabricated numbers.** Ordinal tiers in the UI; coefficients, p-values and
   composite scores stay in the engine and the tests. (§5.6 argues there should be no
   headline session score at all.)

---

## Part A — How the model is used during the session

### A1. Per-shot detection (existing engine, first live wiring)

After each `shotProcessed`, run `detect(pack, source, contexts, shotContextId)` against
a new **live `IMeasureSource`** that adapts the shot's analysis output
(`swing.json` / `MetricSeries`, launch-monitor fields, wrist DoFs) into
`MeasureReading`s with corridors resolved through the norm providers. This is the
moment the pack's semantics meet real data for the first time — commissioned as its own
build step with its own tests (see C2.1).

Per-shot output is the existing `DetectionResult`: per condition
Fired / NotFired / Unavailable, confidence, direction, plus the driving measure's z.
Nothing at this stage is a diagnosis; it is one observation.

### A2. The session evidence ledger

The central new data structure. Per condition × per shot, the ledger records:

    { shotId, conditionId, state, confidence, direction, drivingZ,
      contextId, contextInferred, material, implausibleMeasures[] }

plus per-shot metadata (club, declared context, timestamp, launch-monitor outcome
fields when present, warm-up flag — see §5.4). The ledger is the single source of
truth: every tier, trend, chain grade and panel state is a pure reduction over it, and
it persists as `diagnostics.json` in the session folder so review mode rebuilds the
identical panel.

Exclusion rules: a reading flagged `implausible` contributes nothing for that measure
(capture fault, not swing fault). A *badly hit* shot is never excluded — mishits are
the most diagnostic swings in the session.

### A3. Evidence tiers: recurrence is the unit of diagnosis

For each condition, the firing rate over assessable shots, graded by the **Wilson
score lower bound** rather than the raw proportion (1-of-2 and 8-of-10 have similar
raw rates and very different lower bounds; the bound provides small-n honesty without
a separate rule). Floor: 3 assessable shots before any pattern claim, mirroring
`kMinShotsForSpread`.

| Tier | Rule (defaults, §4) | Meaning | UI standing |
|---|---|---|---|
| **Pattern** | Wilson LB ≥ 0.30, n ≥ 3 assessable | What the session asserts about the golfer | Card in the session picture |
| **Watching** | Fired ≥ 1×, below pattern | Seen, not yet evidence | Collapsed row; never headlines |
| **This shot** | Latest `DetectionResult` | One observation | Ephemeral strip / card markers |

The Watching tier **is** the outlier discard — done by evidence weight, not by
deleting data. A single shank stays there forever if it never recurs.

**Direction consistency.** Among a condition's firings, track direction agreement.
Below the agreement threshold (default 70%), the condition presents as
*inconsistency* ("inconsistent shaft plane"), which is coach-meaningful dispersion,
and the directional narrative is suppressed.

**Trend.** For each pattern's driving measure: per-shot z series, slope by
**Theil–Sen** with Kendall-τ significance, minimum 5 points. Theil–Sen is immune to a
single wild shot by construction; z-space makes it corridor- and context-invariant so
mixed-club sessions pool legitimately. Output is ordinal only — improving / stable /
worsening — plus recency ("last fired: 4 shots ago"). A pattern whose overall rate is
high but which has not fired in the last k assessable shots presents as **resolving**:
the panel rewards the fix rather than punishing history.

**Context pooling.** Firing is already graded against the shot's own context corridor,
so pooling across clubs at the condition level is legitimate and is the default; the
ledger keys context so a per-club filter is one tap away.

### A4. Chain instantiation: confirmatory, not exploratory

The pack's causal DAG is prior knowledge — literature-backed mechanism, fixed before
any ball is struck. The session does **not** discover or verify mechanism; it
estimates whether an authored mechanism is *active in this golfer today* (the
epistemics of clinical diagnosis). This framing also solves multiplicity: ~140
conditions imply ~10,000 pairs, which blind mining over a 15-shot session would fill
with spurious couplings; the DAG restricts testing to the authored edges among this
session's pattern-tier conditions — a handful of pre-specified confirmatory
hypotheses. **The causal network is the multiple-comparisons correction.**

Chains are confirmed link-by-link. Evidence grades, ascending:

| Grade | Evidence | Notes |
|---|---|---|
| **Present together** | Both endpoints reach pattern tier | Necessary, not sufficient — common causes exist |
| **Coherent** | Directions match edge semantics; nodes fire in the swing's own P1–P8 temporal order within shots | Exploits the near-total ordering of condition groups: the chain lights up *in sequence within a single swing* |
| **Conditionally dependent** | 2×2 table of per-shot firing: P(down \| up fired) vs P(down \| up clean); Fisher's exact at session n, Kendall-τ on paired z's for graded support (min 8 pairs) | The contingency table is already the coach sentence: "path was out-to-in on 7 of the 8 swings where the hips stalled — neutral on all 3 where they cleared" |
| **Moved together** | Upstream improvement over recent shots followed by downstream improvement in the edge's predicted direction | The session's natural quasi-experiment (n-of-1 design); the strongest evidence available without a controlled protocol, and the most motivating thing a golfer can be shown |

Two link kinds sit outside per-shot statistics and are handled explicitly:

- **Screened roots** (e.g. hip internal rotation) do not vary shot-to-shot. They are
  confirmed by *doing the screen once*; the existing `TestRecommendation` flow is
  therefore a chain-confirmation action, and the panel phrases it as such ("30-second
  hip test would anchor this chain — it would explain 3 of your patterns").
- **Unproducible intermediates** render as **ghost links** ("width at the top: not yet
  measurable") — never silently bridged. A chain must not claim continuity it lacks.

Honest limits, encoded as states rather than hidden:

- **Range restriction:** an upstream fault firing on *every* shot has no variance to
  covary with; its links cap at Coherent this session, shown as "present on every
  swing — coupling untestable today". Resolves across sessions as the fault improves.
- **Rival parents:** when two authored edges could explain a fired node and n is too
  small to adjudicate, the panel shows the *most consistent* chain with the rival one
  tap away; it never asserts uniqueness.
- **No chain probability:** link supports are dependent; multiplying them would be
  pseudo-precision. A chain's confidence *is* its per-link profile.

### A5. Session-level explanation

`explain()` runs over the **pattern-tier** fired set, not per shot — per-shot root
ranking would churn the greedy set cover on every ball. Re-rank only when pattern
membership changes. Inputs include `knownScreenResults` entered during the session
(screens done at the range count immediately). Outputs consumed: ranked roots with
coverage, offered (Asserted) causes phrased as questions, screen recommendations,
corroborations, suppressions — all already produced by `relation_resolver`.

The recommendation always targets the **most upstream confirmed node**: "fix the hip
clearance and the path fix comes free" is the whole value of a causal model over a
fault list.

### A6. Session intent: the focus contract and the declared miss *(new concept)*

Two lightweight declarations turn passive observation into structured practice:

- **Declared miss** (session start, optional): the existing bad-shot picker seeds the
  session — the model walks upstream from the declared outcome, pre-arms the candidate
  chains, and verifies the outcome against launch-monitor data when present. This is
  the coach conversation's opening question, already a stated design intent, now given
  its runtime role.
- **Focus contract** (any time, optional): the golfer or coach declares "working on X"
  — one node, usually a root or an upstream pattern. The session then structures
  itself as an **n-of-1 experiment**: shots before the declaration are baseline, shots
  after are intervention, and the panel reports the focused node *and the downstream
  propagation* ("hip clearance improving; path following"). This is what earns the
  Moved-together grade honestly, and it is the single feature that converts the panel
  from a report into a practice instrument.

Neither declaration ever changes detection or grading — intent shapes *attention and
experiment structure*, never evidence.

### A7. Cross-session memory: the fault profile *(new concept)*

Patterns confirmed across sessions accumulate into a per-athlete **fault profile**: a
prevalence history per condition (sessions seen / sessions pattern-tier, last-seen,
trend across sessions). Uses:

- **Warm start:** the next session's panel opens with "your usual patterns" as
  *expectations to test*, mitigating the cold-start shots.
- **Trait vs state:** a pattern recurring across sessions is a trait; one appearing
  today only is a state (fatigue, experiment, new club) — different coach sentences.
- **Longitudinal payoff:** "over-the-top: pattern in 8 of your last 10 sessions, absent
  in the last 2" is the retention evidence that per-session trends cannot give.

Strict rule: the profile biases **ranking and presentation only** — never firing,
never corridors. This is athlete *history*, not per-athlete norms (which remain out of
scope). Storage: per-athlete `fault_profile.json` updated at session close.

---

## Part B — What is surfaced as the session progresses

### B1. Session lifecycle (formal state machine)

| State | Trigger | Panel behaviour |
|---|---|---|
| **Cold** | shots 0–2 | This-shot strip does the work; body shows "too few swings to call a pattern" (em-dash philosophy) plus fault-profile expectations if any ("usually: over-the-top — watching") |
| **Forming** | ≥ 3 assessable | First pattern cards appear, marked *new*; flat card list |
| **Established** | ≥ 2 linked patterns with links ≥ Coherent | Body reorganises into the **chain rail**; driver footer eligible |
| **Closing** | session end | Panel freezes → summary (B7) |

Transitions are one-way ratchets within a session (no dropping back from Established
to Forming), which is itself an anti-churn rule.

### B2. Panel anatomy — three zones, stable geometry

**Zone 1 — This shot (top strip, ephemeral).** Chips for what fired on the swing just
struck, including an explicit *clean* state, and "n/a" where capture failed. Labelled
as the shot, allowed to be volatile. Once patterns exist, consider progressive
suppression: raw chips fade in favour of the per-card markers (open UX question, §5.2).

**Zone 2 — Session picture (the body; becomes the summary).**
*Forming:* one card per pattern. Card contents: coach-vernacular name; honest
recurrence ("7 of 9 measurable shots" — never a bare percentage at small n);
direction or inconsistency wording; trend arrow + last-fired recency; the pack's
consequence one-liner; **this-shot marker** (fired / clean / not measurable on the
swing just hit). Watching tier lives in a single collapsed row beneath.
*Established:* the **chain rail** — cards snap into causal order, connected by links
whose rendering encodes evidence grade: dotted (present together), solid
(conditionally dependent), filled-and-arrowed (moved together), ghosted
(unmeasurable gap). Screened roots render as anchors when entered, as a
call-to-action chip when recommended. Rival chains: one tap away, never flip-flopped.
The per-card evidence line is the contingency sentence.

**Zone 3 — Likely driver (footer, appears late).** Session-level ranked root with
coverage ("early extension — would explain 3 of your patterns"), screen
recommendation when the top root is screen-backed and unentered, Asserted causes as
questions. Empty until the pattern set has been stable for a few shots — better
absent than flickering. Tap-through: drill against the root.

### B3. The after-shot moment

The ten-second glance between balls must answer one question: **"my things — did that
swing add a tick or a cross?"** The read is the *delta*, not the panel: this-shot
markers light along the chain in swing order ("hips stalled, and everything
downstream followed — again" / "hips cleared and the path came with them"), changed
cards pulse once, nothing else moves. Under a focus contract the focused node's
marker is the headline of the moment.

### B4. Feedback cadence — a first-class policy *(new concept, literature-backed)*

Motor-learning research is unambiguous that **feedback after every trial can impair
retention** relative to reduced schedules: summary/faded KR and *bandwidth* feedback
(feedback only when performance leaves a tolerance band) produce better learning than
100%-frequency augmented feedback (Schmidt; Winstein & Schmidt; Wulf — peer-reviewed
sources to be resolved and added to `references.json`, per the provenance rules). A
panel that interrupts every shot risks creating dependence on the panel.

Cadence is therefore a policy, not an accident of implementation:

| Mode | Behaviour | Default for |
|---|---|---|
| **Every shot** | Full after-shot moment | Assessment sessions, coach present |
| **Bandwidth** | After-shot moment only when the shot fired a pattern-tier condition or broke a clean streak; otherwise a minimal "clean" acknowledgement | **Default** |
| **Block summary** | Panel quiet during a block (n shots or a declared drill set); reveals at the break | Skill-retention practice |
| **Quiet** | Ledger accumulates silently; summary at close only | Play-like simulation |

The evidence engine always runs at full rate — cadence gates *surfacing only*. This
single table is likely the most consequential UX decision in the document.

### B5. Spoken cues (TTS)

Eyes are on the ball and hands on the club between shots; a screen glance is not
always natural. The existing TTS stack (sherpa-onnx) can deliver the after-shot
moment as **one short spoken line**, generated from the same delta logic and governed
by the same cadence policy: "hips cleared — that's three in a row", "same chain
again". Rules: ≤ 8 words, ordinal language only, never during the pre-shot routine
(gate on shot-detection state), off by default, per-session toggle. The AI Coach
(src/LLM) may later phrase these lines; v1 uses templated strings from the pack's
vernacular so the feature ships without an LLM dependency.

### B6. Honesty devices

Coverage as one quiet line ("41 of 140 characteristics measurable with current
capture"), tappable for the full list. Ghost links for unproducible intermediates.
"Untestable today" for range-restricted couplings. Suppressions and corroborations
rendered per the resolver's existing rules ("seen two ways" / "we saw both and kept
this one"). Cohort caption on corridors where qualified. Inferred-context findings
carry their existing confidence demotion into ranking.

### B7. Session close = the summary

Closing freezes Zone 2 and lets it breathe: trends take final verdicts; the chain
rail keeps its per-link grades and the Moved-together badge if earned; the driver
footer becomes definitive with its screen result or outstanding recommendation; the
this-shot strip is replaced by **session bookends** — links seeking the replay to the
first / best / most-representative shot per pattern (principled selection: max and
min |z| on the driving measure among assessable shots). One addition at close:
**exemplar pairing** — side-by-side replay of the cleanest vs worst swing on the
focused chain, using the existing carousel/replay plumbing; the chain provides the
principled pair selection that generic "compare shots" lacks. Fault profile updates.
Review mode re-renders the frozen panel from `diagnostics.json` via the existing
activeModel proxy pattern.

### B8. Anti-churn rules (consolidated)

Ordering hysteresis via rank bands; tier transitions only at Wilson-bound crossings;
one-way lifecycle ratchets; chain selection sticky with rivals a tap away; driver
footer debounced by pattern-set stability; delta-rendering so the eye is drawn to
change; no continuous re-sorting, ever. The golfer must never watch the app change
its mind mid-thought.

---

## Part C — Building blocks

### C1. Existing elements, used as-is or lightly extended

| Element | Location | Role here |
|---|---|---|
| `detect()` + `IMeasureSource` seam | `src/Diagnostics/characteristic_engine` | Per-shot detection; seam gets its first live implementation |
| `relation_resolver::explain()` | `src/Diagnostics` | Session-level roots, screens, corroboration, suppression — consumed unchanged over the pattern set |
| Characteristic/norm/screen/drill/reference packs + providers | `src/Diagnostics`, `src/Resources/diagnostics` | The model; vernacular, consequences, drills, provenance |
| Context tree + bindings, cohorts | `src/Diagnostics` | Corridor resolution per shot; materiality weighting |
| `shotProcessed(shotId, swingDir)` etc. | `src/Gui/shot/shot_processor` | The session heartbeat the new model subscribes to |
| Session folder lifecycle | `beginSessionFolder`/`endSessionFolder` | Home of `diagnostics.json` |
| `MetricSeries` / `swing.json` / reanalyzer | `src/Analysis` | Measure values for the live source; review-mode reconstruction |
| Launch-monitor session reductions + shot association | `src/Analysis/lm_session_reductions.h`, LM integration | Outcome anchoring of chain tails; statistical house style to copy |
| Shot carousel, replay controllers, activeModel proxy | `src/Gui/shot`, `src/Gui/review` | Bookends, exemplar pairing, review re-render |
| FindingCard / FindingsList design language | `src/Gui/diagnostics` | Card idiom to evolve, not replace |
| Bad-shot picker intent + DAG per-node view | `src/Gui/diagnosticmodel` | Declared miss entry; chain-trail navigation on tap-through |
| TTS stack | `src/TTS` | Spoken cues (B5) |
| AppSettings singleton, NotificationService, state-machine idiom | `src/Core` | Cadence policy setting; lifecycle machine |

### C2. New elements

**C2.1 `LiveMeasureSource`** (`src/Analysis` or `src/Diagnostics`): adapts one shot's
analysis output to `read(measureId) → MeasureReading`, resolving corridors via the
norm providers, honouring shape/open tails/plausibility. Own commissioning step with
known-groups tests — this is where pack semantics meet real data first.

**C2.2 Session evidence ledger** (`session_ledger.h/.cpp`, Diagnostics): the A2
record store; append per shot; serialise/load `diagnostics.json` (schema versioned);
warm-up and context metadata.

**C2.3 `session_diagnostic_reductions.h`** (Analysis, pure, header-only, no Qt-GUI —
sibling of `lm_session_reductions.h`, same four-rules discipline): Wilson bound,
tier assignment, direction agreement, Theil–Sen/Kendall trend, recency/resolving,
2×2 link tables + Fisher exact, link-grade assignment, chain extraction (pattern
subgraph over pack edges), rank-band hysteresis, bookend selection. Standalone
unit tests; every policy number from §4 injected, not hardcoded.

**C2.4 `SessionDiagnosticsModel`** (QML façade, `src/Gui/diagnostics`): subscribes to
`shotProcessed`; owns ledger + reductions + explanation invocation policy; exposes
zones, cards, chain, deltas, lifecycle state, cadence gating; live/review switch via
the activeModel pattern.

**C2.5 Panel QML** (`src/Gui/diagnostics`): `PpSessionDiagnosticsPanel` with zone
components (`PpThisShotStrip`, `PpPatternCard`, `PpChainRail`, `PpChainLink`,
`PpDriverFooter`, `PpCoverageLine`, `PpWatchingRow`); mounted in `PpStagePanel` /
session mode alongside the LM panel; theme-aware across all six aesthetics.

**C2.6 Cadence policy**: enum in AppSettings + per-session override in the session
wizard; gates Zone rendering and TTS, never the engine.

**C2.7 Focus contract + declared miss runtime**: session-scoped intent state
(picker UI exists in intent; runtime consumption is new); baseline/intervention
split for Moved-together grading.

**C2.8 Fault profile store**: per-athlete `fault_profile.json`; updated at close;
read at session start for warm-start expectations. Ranking/presentation only.

**C2.9 Spoken-cue generator**: templated one-liners from delta logic + pack
vernacular; TTS dispatch gated on shot-state and cadence.

**C2.10 Synthetic session corpus generator** (tests): parameterised generator of
ledgers with known ground truth — planted chains, planted outliers, drift,
range-restriction, missing measures — so tier thresholds, link grading and hysteresis
are tested against known answers, and §4 tunables can be tuned empirically. This is
the test asset everything else leans on; build it first.

### C3. Suggested build phasing (for the Claude Code brief)

1. C2.10 corpus generator + C2.3 reductions (pure, fully testable, no UI)
2. C2.1 live source + known-groups validation on real recorded swings
3. C2.2 ledger + persistence + C2.4 model (live + review)
4. C2.5 panel: Cold/Forming (cards) first; chain rail second
5. C2.6 cadence + B3 after-shot delta polish
6. C2.7 focus contract, C2.8 fault profile, C2.9 TTS — each independently shippable

---

## 4. Policy numbers (all injected, all corpus-tunable)

| Tunable | Default | Note |
|---|---|---|
| Pattern Wilson LB threshold | 0.30 | Ordinal gate, not a probability claim |
| Min assessable shots for pattern | 3 | Mirrors `kMinShotsForSpread` |
| Direction agreement for directional narrative | 70% | Below → "inconsistent X" |
| Min points for trend | 5 | Theil–Sen + Kendall-τ |
| Resolving window k | 5 assessable | "Hasn't fired in last k" |
| Min pairs for conditional dependence | 8 | Most exposed to wishful reading |
| Moved-together window | 5+5 baseline/intervention | Honest only under a focus contract |
| Rank-band width (hysteresis) | 1 band | Cards move on decisive change only |
| Driver-footer stability debounce | pattern set unchanged 3 shots | |
| Warm-up default | first 3 shots down-weighted, wizard-adjustable | §5.4 |

---

## 5. Challenges and open decisions (last-chance items)

**5.1 Feedback cadence is the biggest design risk and the biggest opportunity.**
Bandwidth-default (B4) is a deliberate, literature-backed stance against the
"dashboard after every ball" instinct. It should be argued in the Design brief, not
assumed — and the references belong in the pack's bibliography.

**5.2 Should raw this-shot chips survive once patterns exist?** Progressive
suppression (chips fade; per-card markers take over) is cleaner and less noisy;
against it, the strip is where *new* faults first become visible. Recommendation:
keep the strip but render pattern-member chips as ghosts pointing at their cards.

**5.3 No composite session score.** Deliberate. A single number invites swinging *at
the panel* and cannot be defended when interrogated ("why 74?"). The chain is the
headline; the wrist donut remains a mode-local device. If a score is ever wanted, it
must be derivable and interrogable, and it is out of scope here.

**5.4 Warm-up shots.** First swings are not representative and would seed early
patterns badly. Default: down-weight (not exclude) the first 3 shots, adjustable in
the session wizard; a wedge-then-driver progression argues for club-aware handling
later. Cheap now, awkward to retrofit.

**5.5 Coach-present vs solo sessions** may deserve different defaults throughout
(cadence, vernacular density, screen prompts). The lesson-model work (Scope→…→Wrap-up)
should eventually *drive* this panel; not v1, but the model API should not preclude a
lesson controller as a second consumer.

**5.6 Statistical caveat to state plainly in-app help:** shots are not i.i.d. —
fatigue, adjustment and the feedback loop itself violate independence. The design
leans into this (Moved-together, resolving) rather than pretending otherwise, and all
inferential machinery is used as ordinal gating, not hypothesis testing. That is the
honest and defensible posture; anything stronger requires protocols (fixed-club
blocks) the focus contract can grow into.

---

## 6. Briefing notes

**For Claude Code:** phase per C3; one change item per session per established
practice. The corpus generator is the keystone test asset. Reductions follow the
`lm_session_reductions.h` house style verbatim (pure header, four-rules commentary,
standalone tests, QML positions and paints / C++ decides the numbers). All §4
tunables injected. Provenance for B4 references resolved via CrossRef/PubMed, never
recalled.

**For Claude Design:** the UX centrepiece is the **chain rail** (B2) and the
**after-shot moment** (B3) under the **cadence policy** (B4). Design for the
ten-second glance at arm's length in a bright room; the summary is the same panel
frozen, so every state from Cold to Closing must feel like one object maturing, not
five screens. Link-grade visual encoding (dotted/solid/arrowed/ghost), the honest-
recurrence caption style, delta emphasis, and the six-theme compatibility are the
hard problems. Nothing may reorder without a decisive reason (B8).
