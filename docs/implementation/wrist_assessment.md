# Wrist Motion — Position Assessment Engine & Diagnostics View

*As-built implementation notes for the **Wrist Motion diagnostics** feature: a C++ assessment engine
plus a pure-binding QML panel on a Wrist session's **Analyse** surface. It samples the loaded swing's
wrist / forearm / arm joint angles at the swing-phase checkpoints, bands each against an expected
corridor (Tier 1 RAG), runs a declarative rule engine for named faults **and** strengths
("Working well", Tier 2), and presents it all with RAG rating, trajectory strips, a phase grid, and an
explainable findings-weighted score.*

**Sources of truth.** Engine logic + assessment design:
[`docs/design/wristmotion_assessment_design.md`](../design/wristmotion_assessment_design.md). Visual
spec / data shape: `PinPointStudio_WristDiagnostics_mockup.html`. The angle **producer** the live
adapter consumes is the M1 wrist analyzer — see [`shot_analyzer_m1_wrist.md`](shot_analyzer_m1_wrist.md),
`metric_extractor.cpp`, `wrist_angles.h`.

**Status (2026-06-15)**

| Phase | Scope | State |
|---|---|---|
| **0** | Data contracts, windowed-median sampler, synthetic fixtures, harness | **done** |
| **1** | Tier-1 config bands, per-cell RAG, the read-only view, RAG/band tokens | **done** |
| **1.5** | Live-data realign: real `analysisDetail`, **phase nomenclature**, timeline-driven selection | **done** |
| **2** | Rule engine: fault findings F1–F8 + strengths, corroboration/suppression/confidence, score v2, Findings/Working-well dock | **done** |
| **3a** | **Archetype band model** (Auto-detect + manual) + **compare-to ghost** (Previous / Reference) | **done** |
| later | `PlayerBaselineBandProvider` · per-athlete reference · carousel "set as reference" action · body schematic | pending |

27 analyzer tests green (`build/analyzer-tests`); app builds clean; the panel is visually verified in
the running app (offscreen grabs, instrument light/dark). This is **core output** (golfers/coaches act
on it), so it proceeds in plan-gated phases with golden tests as the regression net.

---

## 1. Architecture

Four layers; the engine is unit-tested without Qt-GUI and the QML stays pure bindings:

```
   live data ──► wrist_analysis_adapter ──┐
   (shotReplay.analysisDetail)            │  IWristAngleSource (the input seam)
   synthetic fixtures ───────────────────┘            │
                                                       ▼
  WristAngleSampler::sample ─► PpWristAngleSet ─► WristAssessmentEngine::assess ─► PpWristAssessmentResult
   windowed median @ phase    per-cell value+      Tier-1 band → RAG  ·  Tier-2 rules → findings    RAG matrix +
   gap / gimbal / mirror      status               (ConfigReferenceBandProvider)  (AssessmentRuleRegistry)   findings + score v2
                                                       │
            QML-facing adapter (src/Gui/diagnostics, QML_ELEMENT) — WristDiagnosticsModel  ── QVariant models ──►
                                                       │
            pure-binding QML (src/Gui/diagnostics/*.qml)
            WristDiagnostics ▸ DofTrajectoryStrip · PositionAngleGrid · WristScorePill · StrengthsList · FindingsList(FindingCard)
```

- **The engine consumes a defined input contract, not the producers.** The only coupling point is
  `IWristAngleSource` (`wrist_assessment_contract.h`). The live adapter (`wrist_analysis_adapter`) and
  the synthetic fixtures both implement it, so the engine is golden-tested standalone.
- **Abstract base + factory at every seam:** `IReferenceBandProvider` + `ConfigReferenceBandProvider`
  + `makeReferenceBandProvider()`; `IWristAngleSource` + `InMemoryWristAngleSource`; `IAssessmentRule`
  + `AssessmentRuleRegistry`.
- **All logic in C++; QML is pure bindings** — `WristDiagnosticsModel` exposes `QVariantList`/scalar
  models; the QML computes only view geometry (same posture as `PpChartPlot`/`ChartMetrics`).

Namespace: all engine types are `namespace pinpoint::analysis`. GPL v2 header + `#pragma once` on every
source; `PpMessageLog`/`ppInfo()` for diagnostics (the value types are deliberately log-free).

---

## 2. Phase 0 — contracts, sampler, fixtures

### Type vocabulary (`wrist_assessment_types.h`)
- `PpJointDof` — the **14-DOF** taxonomy. Four have real producers today (`LeadWristFlexExt`,
  `LeadWristRadUln`, `LeadForearmRot`, `LeadElbowFlex` — the last only with the upper-arm IMU); the
  other ten (trail side + both shoulders) are reserved slots with no producer → Grey cells until
  instrumented. The order is part of the contract (array index).
- `PpSwingPosition` — `P1…P8`, the engine's internal **band key**. The UI labels them with phase names
  (see §4); the `P→Phase` map lives in the adapter.
- `PpHandedness` (`Unknown/Right/Left`); `PpRag` (`Ref / Green / Amber / Red / Grey`); `mirrorSign(dof)`
  (handedness mirror parity); `dofForMetricKey()` (producer key → DOF); `dofName()`.

### Input contract (`wrist_assessment_contract.h`)
Per-DOF series carry **neutral-relative degrees** (same reference as `MetricSeries.value`); the engine
derives Δ-from-address. Types: `PpJointAngleSample { t_us, valueDeg, available, confidence,
pitchProxyDeg }`, `PpJointAngleSeries { dof, present, samples[], baseConfidence }`,
`PpSwingPositionTimeline { positions[8]: { present, t_us, conf } }`, the abstract `IWristAngleSource`,
the sampled `PpWristAngleCell { valueDeg, status (Ok/Gap/Indeterminate), confidence }`, and
`PpWristAngleSet[dof][pos]` (canonical right-handed). A generic settable **`InMemoryWristAngleSource`**
backs both the live adapter and the fixtures.

**Gap vs Indeterminate** (both render Grey): Gap = no usable sample / DOF absent; Indeterminate =
samples exist but the windowed pitch-proxy ≥ `gimbalThresholdDeg` (near gimbal lock — only the middle
Euler term needs singularity checking). Live `MetricSeries` carry no pitch proxy yet (=0), so live data
never triggers Indeterminate (a future enhancement).

### Windowed-median sampler (`wrist_angle_sampler.h`)
`WristAngleSampler::sample` → per `(dof, Pn)`: no series/entry → Gap; gather available samples within
`±windowHalfUs`, `< minValidSamples` → Gap; windowed `median(pitchProxy) ≥ threshold` → Indeterminate;
else `median(values)` (deterministic `nth_element`), left-handed `× mirrorSign(dof)`, confidence =
`baseConfidence × entry.conf × mean(window confidence)`. Single tunable struct `PpWristSamplingConfig`
(`windowHalfUs 15000`, `gimbalThresholdDeg 75`, `minValidSamples 1`).

### Synthetic fixtures (`wrist_assessment_fixtures.h`)
`FixtureWristAngleSource` (alias of `InMemoryWristAngleSource`) + deterministic builders (8 Pn anchors +
symmetric jitter so the median recovers the anchor). `makeCleanSwing` / `makeCleanSwingLeftHanded`
(mirror golden) / `makeCast` / `makeFlip` / `makeOpenFaceTop` / `makeHoldingOff` / `makeOverRotation` /
`makeChickenWing` / `makeMockupDemoSwing` (the mockup's cast+flip line). Test:
`wrist_angle_sampling_test` (94 checks: median, outlier, gap, missing entry, gimbal+threshold, LH==RH
mirror, determinism).

---

## 3. Phase 1 — Tier-1 banding

### Reference bands (`reference_bands.h` / `.cpp`)
`Band { greenLo, greenHi, amberLo, amberHi, valid }`; `BandContext { archetype, club, shape }` (seam,
v1 neutral). `IReferenceBandProvider::band(dof,pos,ctx)` + `ConfigReferenceBandProvider` (compiled
table seeded from design §5; amber margin per-DOF) + `makeReferenceBandProvider(kind=Config)`. Missing
entry → `valid=false` → Grey. `classifyDelta(delta, band)` → `Green`/`Amber`/`Red`; invalid/inverted →
`Grey` (no false Green).

### Result types (`wrist_assessment_result.h`)
`PpRagCell { valueDeg, deltaDeg, rag, status, confidence, banded, bandLo, bandHi }` →
`PpDofRow { dof, present, confidence, cells[8] }` → `PpWristAssessmentResult { handedness, rows[14],
score, findings[] }`. `PpScoreBreakdown { base, total, contributions[] }`; `PpScoreContribution {
id, name, penalty }`. The finding value type `PpWristFinding` also lives here (it's a result component
— see §5).

### Tier-1 in the engine (`wrist_assessment_engine.{h,cpp}`)
`WristAssessmentEngine::assess(source, provider, cfg)`: sample → per cell `deltaDeg = value@Pn −
value@P1` (address falls back to 0 if the reference cell is missing — never fabricated); RAG `P1→Ref`,
non-Ok→Grey, no band→Grey, else `classifyDelta`. Then the Tier-2 rules + score v2 (§5).

### Theme tokens (`src/Gui/theme/Theme.qml`, all 8 themes)
`colorRagGood/Watch/Fault/None` (semantic aliases onto `colorGood/Attention/Error/Text3`) +
`colorBandGreen/Amber` (low-alpha corridor fills). Tests: `reference_bands_test`, `tier1_banding_test`.

---

## 4. Phase 1.5 — live data, phase nomenclature, timeline-driven selection

The panel consumes the focused swing's **real analysis**, speaks the review surface's **phase
vocabulary**, and takes its selected position from the **timeline playhead** — exactly like
`PpReplayCharts`.

### Live adapter (`wrist_analysis_adapter.h` / `.cpp`)
`parseAnalysisDetail(const QVariantMap&)` turns `shotReplay.analysisDetail` — `series` (`[{key, t_us[],
value[], …}]`, degrees, neutral-relative, absolute-µs) + `phases` (`[{phase, t_us, conf}]`) — into an
`InMemoryWristAngleSource`: each series whose `key` maps via `dofForMetricKey()` → a `PpJointAngleSeries`;
each checkpoint's `Phase` event → a timeline entry (absent phase → Grey checkpoint). Right-handed
(producer convention; no mirror — LH correctness is a deferred caveat).

It also owns the **checkpoint → Phase** map (the engine bands key by `PpSwingPosition`; the whole review
surface labels by named phase via `TimelineLabels::phaseFullName/phaseShortTag`):

| checkpoint | `Phase` | full / short |
|---|---|---|
| P1 | 0 Address | Address / ADR |
| P2 | 1 Takeaway | Takeaway / TKW |
| P3 | 8 Mid-backswing | Mid-backswing / MBK |
| P4 | 2 Top | Top / TOP |
| P5 | 4 Downswing | Downswing / DWN |
| P6 | 9 Delivery | Delivery / DLV |
| P7 | 5 Impact | Impact / IMP |
| P8 | 11 Follow-through | Follow-through / FLW |

(P5→Downswing and P6→Delivery are the closest named phases the segmenter emits; a checkpoint whose
phase a swing didn't produce greys out.)

### Model + view changes
- `WristDiagnosticsModel` gains `analysisDetail` (WRITE → parse + assess) and `playheadUs` (WRITE →
  derive `selectedPosition` as the checkpoint bracketing the playhead, like `TimelineLabels::activeStation`);
  `selectedPosition` is read-only. `positions` carry phase `name`/`tag`/`note` (resolved via a
  `TimelineLabels` member). `hasData` gates an empty-state. The constructor no longer auto-loads the
  demo (`loadDemo` is a dev/test affordance).
- `WristDiagnostics.qml` binds `analysisDetail: shotReplay.analysisDetail` + `playheadUs:
  shotReplay.positionUs`, shows an empty-state when no wrist swing is reviewed (mirrors `PpReplayCharts`),
  and the **in-panel scrubber is removed** (`PositionScrubber.qml` deleted). Strip x-labels and grid
  headers are phase short tags. With real data only the producer-backed DOFs appear (3 strips —
  radial-ulnar, flex-ext, forearm; elbow in the grid when the upper-arm IMU is worn).

Test: `wrist_analysis_adapter_test` (synthetic `analysisDetail` → real values/RAG; missing phase →
Grey; absent DOF → row not present).

---

## 5. Phase 2 — rule engine, findings, strengths, score v2

Tier-2 reads the Tier-1 result (per-cell Δ / RAG / confidence already computed) and emits findings.

### Rule types (`assessment_rule.h`)
`PpFindingSeverity { Good, Watch, Fault }`; `PpWristFinding { id, name, category, severity, magnitudeDeg,
weight, confidence, lowConfidence, dofs[], positions[], ballFlight[], explanation, coaching, protect,
corroboratedBy[], linkedTo }` (in `wrist_assessment_result.h`); abstract `IAssessmentRule::evaluate(const
RuleContext&) → optional<PpWristFinding>`; `RuleContext` (accessors over the result + timeline); and the
tunable `RuleTuning { confidenceFloor 0.45, scoreScale 18, severityWeightFault 1.0/Watch 0.5,
corroborationBoost 0.30, strengthsRequireAdjacentFault true }` (owned by `WristAssessmentConfig`).

### Rules + registry (`assessment_rules.h` / `.cpp`)
"Rules are data": a declarative `RuleDef` (metadata + a predicate over the context) wrapped in a generic
`PredicateRule`. `AssessmentRuleRegistry::makeDefault()` builds:

| id | fault | signal | severity from |
|---|---|---|---|
| `open_face_top` (F1) | open face at top | lead flex-ext below the face corridor @P4 | rag@P4 (archetype-aware) |
| `closed_face_top` (F2) | closed face at top | lead flex-ext above the face corridor @P4 | rag@P4 (archetype-aware) |
| `flip` (F3) | flip / scoop | lead flex-ext Δ rises P6→P7 | der<−8 Fault / <−5 Watch |
| `cast` (F4) | early release | lead radial-ulnar below corridor @P6 | rag@P6 |
| `insufficient_set` (F5) | under-set | lead radial-ulnar below corridor @P4 | rag@P4 |
| `over_rotation` (F6) | over-rotation | lead forearm above corridor @P8 | rag@P8 |
| `holding_off` (F7) | blocked release | lead forearm below corridor @P8 | rag@P8 |
| `chicken_wing` (F8, **reduced**) | chicken wing | lead elbow folds @P8 (+ forearm under-rotated) | elbow-gated; lower base conf |

Strengths (match-good rules, `severity = Good`, weight 0): `set` (radial-ulnar Green P2–P4),
`face_rotation` (forearm Green P4–P6), `width` (elbow Green P5–P7).

`run(ctx, tuning)` evaluates every rule then applies the **relationship post-pass** (design §7.4):
confidence finalisation (`ruleBase × mean input-cell confidence`, capped), **flip corroboration**
(trail-wrist flattening → ×(1+boost), neutral when Grey), **cast↔flip linkage** (`linkedTo`),
**over-rotation/holding-off mutual exclusion** (keep the higher-confidence one), **open-face suppression**
(clean downstream face → downgrade to Watch), low-confidence demotion (`< floor` → flag, not drop), and
the **strengths policy** (default: surface only when a live fault exists to protect against; the
`strengthsRequireAdjacentFault` flag relaxes to "whenever Green").

### Score v2 (`wrist_assessment_engine.cpp`, design §7.6)
`penalty_f = severityWeight(severity) × confidence × weight × scale`; `score = clamp(100 − Σ)`; the
breakdown `contributions` (per fault/watch finding) sum to the score. Strengths don't penalise.

### Model + dock UI
- `WristDiagnosticsModel` exposes `findings` (faults/watch, severity-ordered, incl. low-confidence) and
  `strengths` (Good) as `QVariantList` — each `{ id, name, severity, confidence, lowConfidence,
  positions:[phase tags], ballFlight:[…], explanation, coaching, protect, corroboratedBy:[…], linkedTo,
  seekUs }`. `seekUs` is the primary contributing checkpoint's timeline timestamp.
- `FindingCard.qml` (severity glyph/pill, phase + ball-flight chips, confidence pips, expand → why +
  coaching/protect + corroboration, linked-pair line, tap → `shotReplay.seekToUs(seekUs)`),
  `FindingsList.qml` (prioritised cards + a low-confidence toggle), `StrengthsList.qml` (green "keep"
  cards + the protect framing). `WristDiagnostics.qml` reintroduces the mockup's **2-column dock**
  (strips/grid left; "Working well" then "Findings" right), responsive — stacked on a narrow panel.

Tests: `fault_rules_test`, `strengths_test`, `relationship_rules_test`, `confidence_test`,
`composite_score_v2_test` (the v1 cell-score test was retired).

### Dock placement (`ScreenWrist.qml` + `PpModeStage.qml`)
The diagnostics is a **stage panel** (`PpModeStage`'s `dashboardDelegate`, wired Wrist-only), placed
via the toolbar **View control** (`PpViewPanel` → "Dashboard"); the camera/charts/table panels and their
video-sink wiring are untouched. `PpModeStage.active` filters to enabled **and** host-wired panels, so
enabling "Dashboard" on a screen without a delegate omits it (no empty placeholder).

---

## 5b. Phase 3a — archetype band model + compare-to ghost

**Archetype band model** (design §6 — score a valid style against its own model, not red-flagged):
- `ArchetypeBandProvider` (`reference_bands.{h,cpp}`) composes the neutral config table and shifts the
  **face DOF only** (`LeadWristFlexExt`) by `ctx.archetype`: bowed `+10°`, cupped `−10°`, neutral `0`
  (≡ config). Behind the factory (`BandProviderKind::Archetype`).
- **Auto-detect** (design §12 Q3): `BandContext.archetype == −1` = Auto. `WristAssessmentEngine::assess`
  resolves it from the lead-wrist flex-ext Δ at the Top (`detectArchetype`: `> +10°` → Bowed, `< −10°`
  → Cupped, else Neutral), bands with the resolved value, and stores `result.archetype`. The model's
  `archetype` is the **mode** (`−1` Auto default / 0 / 1 / 2); the UI shows the resolved model
  (`effectiveArchetypeName`).
- F1/F2 (open/closed face at top) were made **band-relative** (rag@P4 below/above the corridor) rather
  than fixed-Δ thresholds, so the archetype shifts the *fault*, not just the cell colour.

**Compare-to ghost** (a comparison swing overlaid on the strips):
- Model: `compareTo` (`address` / `previous` / `reference`), `previousAnalysisDetail`,
  `referenceAnalysisDetail`. `recomputeGhost()` picks the source by mode, parses + assesses it once
  (cached); each strip point gains a `ghost` = that swing's Δ at the checkpoint (band-independent).
- Data path: `ShotListModel::previousAnalysisDetail(swingDir)` (next-older sibling) and
  `analysisDetailForSwingDir(swingDir)` (a specific swing); the **reference** swingDir persists in
  `AppSettings::wristReferenceSwingDir` (`ui/` key).
- QML: a **Model** (Auto + "→ {detected}" hint) + **Compare** `PpSegmentedControl` row + a **★ Set as
  reference** toggle (marks the focused swing; persists across restart); `DofTrajectoryStrip` draws a
  dashed `Theme.colorText3` ghost polyline under the player line. Test: `archetype_bands_test`
  (neutral ≡ config; bowed/cupped shift the corridor; bowed-top Red under neutral but Green under the
  bowed model; **Auto** resolves Bowed/Cupped/Neutral from the Top).

---

## 6. CMake & build

- **App** (`CMakeLists.txt`): the diagnostics QML in `QML_FILES`; the model + engine/adapter/rules
  `.cpp` in `SOURCES`; `src/Gui/diagnostics` on the include path. `WristDiagnosticsModel` is a
  `QML_ELEMENT` (no `main.cpp` registration).
- **Tests** (`src/Analysis/tests/CMakeLists.txt`): the `WRIST_ENGINE` source set (`reference_bands` +
  `wrist_assessment_engine` + `assessment_rules`) compiled into each engine-linked test; goldens are
  inline literals.

```bash
cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
cmake --build build/analyzer-tests -j4 && ctest --test-dir build/analyzer-tests --output-on-failure
cmake --build build/Desktop_Qt_6_11_0-Debug --parallel 4    # app (≤4 jobs — OOM cap)
```

---

## 7. Sign conventions (matched to `wrist_angles.h`)

Canonical right-handed golfer; left-handed mirrored at acquisition, so the engine is handedness-agnostic.

| DOF | `+` direction | mirror parity |
|---|---|---|
| `LeadWristFlexExt` | + flexion / "bowed" (− extension / "cupped") | +1 |
| `LeadWristRadUln` | + ulnar / "hinge" (− radial) | −1 |
| `LeadForearmRot` | + pronation / "roll" (− supination) | −1 |
| `LeadElbowFlex` | + flexion magnitude (≥0) | +1 |

---

## 8. File map

| File | Layer | Purpose |
|---|---|---|
| `src/Analysis/wrist_assessment_types.h` | engine | enums + `mirrorSign`/`dofForMetricKey`/`dofName` |
| `src/Analysis/wrist_assessment_contract.h` | engine | input-contract value types + `IWristAngleSource` + `InMemoryWristAngleSource` |
| `src/Analysis/wrist_angle_sampler.h` | engine | `WristAngleSampler::sample` (windowed median, gap/gimbal/mirror) |
| `src/Analysis/wrist_assessment_fixtures.h` | engine | synthetic source builders (clean / faults / LH / demo) |
| `src/Analysis/wrist_dof_metadata.h` | engine | per-DOF display metadata (name/sub/source/poles/`hasStrip`) |
| `src/Analysis/reference_bands.h` / `.cpp` | engine | `IReferenceBandProvider` + `ConfigReferenceBandProvider` + `ArchetypeBandProvider` + factory + `classifyDelta` |
| `src/Analysis/wrist_assessment_result.h` | engine | result + `PpWristFinding` + score breakdown |
| `src/Analysis/assessment_rule.h` | engine | `IAssessmentRule`, `RuleContext`, `RuleTuning`, `PpFindingSeverity` |
| `src/Analysis/assessment_rules.h` / `.cpp` | engine | `RuleDef`/`PredicateRule` + `AssessmentRuleRegistry` (F1–F8 + strengths + relationships) |
| `src/Analysis/wrist_assessment_engine.h` / `.cpp` | engine | `assess()` — Tier-1 + Tier-2 + score v2 |
| `src/Analysis/wrist_analysis_adapter.h` / `.cpp` | engine | `parseAnalysisDetail()` + checkpoint→Phase map |
| `src/Gui/diagnostics/wrist_diagnostics_model.{h,cpp}` | model | `WristDiagnosticsModel` (`QML_ELEMENT`) — strips/grid/findings/strengths/score, playhead-driven |
| `src/Gui/diagnostics/WristDiagnostics.qml` | view | root panel — strips/grid + 2-column dock |
| `src/Gui/diagnostics/DofTrajectoryStrip.qml` | view | corridor + line + RAG markers + poles + phase labels |
| `src/Gui/diagnostics/PositionAngleGrid.qml` | view | collapsible RAG heatmap (phase columns) |
| `src/Gui/diagnostics/WristScorePill.qml` | view | reuses `PpQualityPill` + score-v2 breakdown |
| `src/Gui/diagnostics/FindingCard.qml` | view | one fault/strength card |
| `src/Gui/diagnostics/FindingsList.qml` | view | findings dock + low-confidence toggle |
| `src/Gui/diagnostics/StrengthsList.qml` | view | "Working well" dock |
| `src/Gui/theme/Theme.qml` | tokens | `colorRag*` + `colorBand*` (all 8 themes) |
| `src/Gui/session/ScreenWrist.qml` / `PpModeStage.qml` | wiring | dashboard-panel dock (Wrist-only); unwired-panel omission |
| `src/Gui/shot/shot_list_model.{h,cpp}` | data | `previousAnalysisDetail` / `analysisDetailForSwingDir` — the compare-to ghost sources |
| `src/Gui/app/app_settings.h` | settings | `wristReferenceSwingDir` — the persisted compare-to reference swing |
| `src/Analysis/tests/*_test.cpp` | tests | `wrist_angle_sampling`, `reference_bands`, `archetype_bands`, `tier1_banding`, `wrist_analysis_adapter`, `fault_rules`, `strengths`, `relationship_rules`, `confidence`, `composite_score_v2` |

---

## 9. Decisions & known limitations

- **Live data is lead-side only.** Real producers exist for lead flex-ext, radial-ulnar, forearm
  rotation, and elbow (only with the upper-arm IMU). Trail-side + shoulder DOFs have no producer → Grey;
  the rules that need them are scoped: **F1–F7** fire fully, **F8 (chicken wing) ships reduced**
  (elbow + forearm, no shoulder, elbow-gated, lower confidence), **F9–F11 are deferred**, and trail-wrist
  corroboration is a boost-when-present (tested on the fixtures, which carry trail-wrist; neutral live).
- **Strengths are curated by default** (surface only adjacent to a live fault) so the "protect this"
  signal isn't diluted; a single tunable relaxes to "show all clean". A *perfectly* clean swing therefore
  shows neither faults nor strengths under the default.
- **The mockup `rag[]`/score numbers are illustrative** (hand-authored, internally inconsistent). The
  engine reproduces the corridor + line shape and a principled RAG/score, not the verbatim mockup cells.
- **Left-handed signs are producer-unverified** (`wrist_angles.h`'s `leftArm` path is `Q_UNUSED`); the
  mirror test proves the engine's sign table is *self-consistent*, not physically correct on real LH data.
- **Approximate phase checkpoints.** P5→Downswing and P6→Delivery use the nearest named segmenter phase,
  not the exact "lead arm parallel" / "shaft parallel" instants. Live data has no gimbal pitch-proxy yet,
  so Indeterminate never triggers from live capture.

---

## 10. Next

- **`PlayerBaselineBandProvider`** — bands from the player's own reference/best shots (intra-player
  consistency, the most coaching-relevant comparison); needs a "what counts as my baseline" corpus.
- **Per-athlete reference** — the reference swingDir is a single global `AppSettings` value today;
  scope it per athlete. Plus a **carousel "set as reference"** action (it lives in the panel today).
- **Body schematic** — the Qt Quick 3D Y-bot with RAG-tinted joints (design §8.2-E), deferred until the
  IK pose is validated/tuned with SwingLab.
- **Trail-side + shoulder instrumentation** (F9–F11, full F8) once those producers exist.
