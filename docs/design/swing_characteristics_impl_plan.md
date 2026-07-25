# Swing Characteristics — implementation plan

Companion to the brief (`swing_characteristics.md`, revision 3). The brief is the *what*; this is
the *how*, sequenced, with the repo facts each step depends on already verified.

**Scope boundary, restated because it is the thing most likely to drift:** this delivers the
authoring and maintenance surface for diagnostic rules. The engine and resolver are built and
unit-tested against synthetic input so the pack's semantics are proven, but they are **never invoked
on a real swing**, never wired into `ShotAnalyzer` or `wristProfile()`, and no finding reaches the
dashboard. If a step in this plan appears to require touching the analysis path, it is wrong — stop
and re-read this paragraph.

---

## 0. Repo facts this plan is built on (verified, not assumed)

| Fact | Where | Consequence |
|---|---|---|
| `navIdx: 10` is already **System**, an action row (`action: "system"`) that emits `resourceMonitorRequested()`, not a panel | `src/Gui/settings/ScreenSettings.qml:203` | New panel takes 10; System renumbers to 11. Brief revision 1 would have collided. |
| `StackLayout` has ten children, indices 0–9; 6 and 8 are `ScreenPlaceholder` | `ScreenSettings.qml:424–434` | Append as child 10, after `MetricLibrary`. |
| `navigateToResult()` holds a **literal** `panels` array covering 0–9 | `ScreenSettings.qml:59–63` | Must be extended or search-to-panel silently no-ops. |
| `showMetricDetail(key)` sets `activeNavIndex = 9` then `Qt.callLater`s into the panel | `ScreenSettings.qml:71–77` | Copy the shape exactly for `showCharacteristicDetail(id)`. |
| No shared anatomy resolver exists | `src/Video/video_overlay_pose.cpp:168–175` | Neck/pelvis midpoints are local expressions inside a render function. **Extraction is new scope.** |
| Bone topology is hand-duplicated in QML | `PpCameraFrame.qml:313` `kBlueprintBones`, with a "keep the two in sync" comment at `video_overlay_pose.cpp:90` | Extraction collapses two copies into one. Parity test required. |
| Two pose layouts: `PoseResult::kNumKeypoints = 17` (live) and `kWholeBodyJoints = 133` (offline) | `swing_analysis.h:244,256` | Resolver takes the layout as a parameter from day one. |
| **No spine keypoint in either layout.** 0–16 COCO body (shoulders, hips, nothing between), 17–22 feet, 23–90 face, 91–132 hands | `swing_analysis.h:244–249` | C-posture (thoracic flexion) and S-posture (lumbar extension) are **not derivable from pose at all** — a capture gap, not a roadmap item. |
| Handedness convention is an int (1 right, 2 left); callers resolve `leadIsLeft = (handedness != 2)` once and pass the bool | `foot_metrics.h:37–40` | Resolver takes `leadIsLeft`, never `handedness`. |
| `BandContext { archetype, club, shape, tuning }`, all hard-defaulted | `reference_bands.h:59–64` | Widen with a context id; do not add a parallel struct. |
| Low-confidence demotion is `f.lowConfidence = f.confidence < tuning.confidenceFloor`, kept not dropped | `assessment_rules.cpp:309–311` | Reuse for inferred-context demotion. |
| Reducer vocabulary already described informally | `metric_type.h:35` — "reduced to peak / @impact / Δ / rate by the chart layer" | Promote to a shared type; do not define a second. |
| Factory seam pattern | `makeMetricCatalogue()` `metric_catalogue.h:80`; `makeReferenceBandProvider()` `reference_bands.h:119` | `makeCharacteristicPackProvider()` mirrors these. No statics. |
| QML façade shape | `metric_catalog.h:40–64` — `Q_PROPERTY QVariantList groups/types`, `Q_INVOKABLE query(filters)/descriptor(key)/availability(key)` | `CharacteristicLibraryModel` mirrors it. |
| Ten modules already carry a `tests/` dir | `src/*/tests` | `src/Diagnostics/tests` follows the same CMake shape. |

---

## 1. Sequencing

Eight phases. Phases 1–4 are pure C++ and land dark; 5–8 are UI. **Phase 5 is a shippable
stopping point** — a read-only library plus the roadmap export already pays for itself, and if the
work is interrupted there, nothing is half-built.

Each phase names its gate. A phase is not done until its gate passes.

### Phase 1 — Vocabulary and facets *(blocks everything)*

`anatomy_vocabulary.h/.cpp`, `measure_facets.h/.cpp`, the reducer type.

1. Promote the reducer vocabulary out of the chart layer into a shared header. Chart layer and
   diagnostics both consume it.
2. Author the ~30 anatomy roles, including the finer segment list the brief's §7 now carries
   (`lead/trail upper arm`, `forearm`, `thigh`, `shin`) — five seed rows need joint angles, which
   are angles *between segments* and cannot be expressed on a point. Plus `stance centre` as a
   derived midpoint alongside `pelvis centre` / `thorax centre`.
3. **Three spinal roles, two of which resolve to unavailable in both layouts.** `thoracic segment`
   and `lumbar segment` are admitted to the vocabulary because they are the correct way to describe
   C- and S-posture, and there is no keypoint between the shoulders and the hips in either layout.
   `hip hinge` (torso relative to thigh) *does* resolve. This is the case the "unavailable rather
   than a wrong index" contract exists for — get it right here and the capture-gap reporting in
   Phases 7–8 falls out for free.
4. Resolver signature: `(role, leadIsLeft, layout) → resolved point/segment | unavailable`. Both
   layouts from the start.
5. Validity table indexed on `(what-class, quantity, reference-class)`. Include the
   distance-to-line (perpendicular) vs distance-along-line (needs a point reference) distinction —
   this is what keeps `ball_too_close` and `ball_forward` structurally apart.
6. Canonical naming: series then reducer, deterministic.
7. **Extract the overlay's anatomy maths into the resolver** and re-point
   `video_overlay_pose.cpp` at it.

**Gate:** `measure_facets_test`, `anatomy_vocabulary_test`, `anatomy_overlay_parity_test` green.
Parity means the extracted resolver reproduces the current neck/pelvis midpoints and bone topology
exactly, and agrees with `PpCameraFrame.qml`'s `kBlueprintBones`. Plus a visual check on one real
swing — the overlay is the most visible thing in the app and a silent regression here is expensive.

**Risk:** this is the phase most likely to overrun, because the overlay extraction was not in the
original brief and touches shipped rendering. If it looks like it will exceed its budget, extract
the resolver *without* re-pointing the overlay, ship the duplication for now, and record it — the
diagnostics work is not blocked by the overlay still having its own copy. Do not skip the parity
test; skip the re-point.

### Phase 2 — Pack schema, loader, validator, provider seam

`characteristic.h`, `characteristic_pack.h/.cpp`, `pack_provider.h` + the three providers.

Validator rules, all of which have a test:
- referential integrity (unknown measure/condition/drill ids rejected)
- DAG — cycles rejected
- id collision rejected; version conflict handled
- `Corroborates` **illegal** between conditions with a causal path (brief §8c, R13)
- axis pairing — a tail's `axis` partner shares its series
- locale-keyed narrative strings accepted now, so it is not a migration later

`makeCharacteristicPackProvider()` assembles core + user on demand, core wins on collision. No
self-registering statics.

**Gate:** `characteristic_pack_test` green, including every rejection path. A validator whose
negative cases are untested is a validator that passes everything.

### Phase 3 — `core.json`

The characteristics (§8), cause library (§8b), edges (§8c).

Author in the repo as reviewable JSON; generate the Qt resource from it at build time (brief §13
recommendation — do this unless Mark says otherwise, since it is the only shape that lets community
contributions arrive as pull requests).

The three content questions that gated this phase are **now answered** (brief revision 3): C-posture,
S-posture and too-upright are three separate measures; hip IR splits trail/lead; ball position
references the stance centre with a club-dependent corridor. What remains open is listed in §4 below
and none of it blocks authoring.

Two things to check while authoring:

- **#20–22 may already be Live.** `pose_wrist_angle_source.h` exists; if the wrist-angle series has a
  producer and a catalogue key, casting/scooping/insufficient set bind as Provided and three of the
  seed resolve on day one rather than minting stubs.
- **Ball position is the first genuinely club-dependent corridor**, so it is the row that exercises
  `ContextBinding.corridorRef` for real. Author it per club context, expressed as a fraction of
  stance width rather than millimetres. If the binding mechanism does not survive contact with this
  one row, it will not survive the short-game work later either — treat it as the mechanism's test
  case, not just as content.

**No brand names anywhere in the output.** Several conditions carry terms popularised by a commercial
screening system. The terms are common domain and stay; the attribution does not enter `core.json`,
provenance fields, comments or commit messages, and a screening threshold from that system is neither
a citation nor a corridor (brief §8e, R22).

**Gate:** `core_pack_test` green — loads, validates, every characteristic resolves to Live or a
*named* missing measure, §8d coverage holds, and the **structural orientation assertion** passes
(every `Screened` cause has out-degree > 0 and in-degree 0; no characteristic edges *into* a
`Screened` condition). That last one is the only guard against the whole graph being transcribed
backwards — the coverage count cannot see it, because in-degree and out-degree counts are identical
under edge reversal.

### Phase 4 — Engine and resolver *(dark)*

`characteristic_engine.h/.cpp` (signals → conditions), `relation_resolver.h/.cpp` (conditions →
ranked root causes + test recommendations).

Detection consumes synthetic `MetricSeries`. The absent-measure path must resolve **Unavailable** —
never a false negative dressed as a pass, which is the failure mode that would quietly make the
whole library look like it works.

Resolver rules with teeth:
- a characteristic with an in-pack cause is never returned as a root
- `Asserted` causes are **surfaced but never counted as resolving** a finding (brief §8e, R11) —
  "never auto-selected" must not become "dropped", or `ball_forward` (habit-only) shows an empty
  panel where the truthful answer is "may simply be how they set up"
- an unknown `Screened` cause explaining ≥2 findings becomes a ranked test recommendation

**Gate:** `characteristic_engine_test`, `relation_resolver_test`, `test_recommendation_test` green.

### Phase 5 — Settings panel, read-only *(shippable)*

`characteristic_library_model.h/.cpp` (mirroring `metric_catalog.h`'s `Q_PROPERTY`/`Q_INVOKABLE`
shape), `CharacteristicLibrary.qml`, `CharacteristicRow.qml`, `CharacteristicDetail.qml`.

Integration exactly as §0 above: panel at StackLayout index 10, nav row `navIdx: 10` under the
existing `Reference` section head, **System renumbered to 11**, `panels` array in
`navigateToResult()` extended, `SettingsIndex.qml` entries at `panelIndex: 10`,
`showCharacteristicDetail(id)` mirroring `showMetricDetail`.

Tails group under their axis so the library reads as 25 axes, not 28 near-duplicates. Detail shows
causes **and** effects — the same condition is routinely both.

**Gate:** panel opens, search reaches it, deep link works, System still opens the resource monitor.
That last check is the one most likely to be forgotten and it is a user-visible break.

### Phase 6 — MeasurePicker and the sentence editor

`characteristic_editor_model.h/.cpp`, `MeasurePicker.qml`, `CharacteristicEditor.qml`,
`ContextBindingStrip.qml`.

The picker is **two steps, not one flat facet row**: pick the series, then pick how it is sampled.
Flattening them hides the distinction the roadmap depends on. On an exact series match, offer reuse
with the existing reducers listed, so the author adds a reducer to a known series rather than
minting a near-duplicate.

Phrase→facet resolution: keyword map is an acceptable v1. `src/LLM/` exists if it is cheap.

**Gate:** mint a stub end-to-end — it creates the measure, registers the planned `MetricDescriptor`
in the catalogue (not a second registry), assigns it, and closes without blocking the author.

### Phase 7 — Roadmap view and export

Ranked by **series**, not reduced measure — one producer unblocks every reducer over it, and
ranking reduced measures would spread that across four rows and understate the payoff. Export to
markdown or JSON; this artefact is meant to leave the app.

**Gate:** the export, read cold, is a usable prioritisation of pipeline work.

### Phase 8 — Health list, cause coverage, blast radius

Health list including **axes with only one authored tail** — this is how `s_posture`'s posterior
tail and `stance_narrow`'s missing consequence stay deliberate omissions rather than accidents.

Cause coverage view split by `confirmedBy`. The `Screened` block is the highest-value list in the
product: it tells a coach which three screens to run, and it needs no capture hardware.

**Gate:** the four dominant causes from §8d are visible as such without reading the JSON.

---

## 2. Build and test integration

`src/Diagnostics/tests` follows the ten existing `src/*/tests` directories. Per the repo's
conventions: modules keep source flat, tests go in a `tests/` subfolder, and the analyzer-test
pattern is the closest model for a pure-C++ module with no Qt-GUI dependency.

Build parallelism is capped at 4 on this box — use `--parallel 4`, never bare `-j`. During
edit-test loops build only the diagnostics test target; do the full app build once per cycle end.

---

## 3. What could go wrong

| Risk | Mitigation |
|---|---|
| **Overlay extraction regresses the skeleton.** Most visible surface in the app. | Parity test + visual check. Fallback: extract without re-pointing (see Phase 1). |
| **A brand name reaches `core.json`, a comment or a commit.** Several terms come from a commercial screening system. | Terms travel, attributions do not (brief §8e). Grep the pack and the diff before Phase 3 closes. |
| **Unmeasurable causes leak into the measure roadmap**, implying producers that will never be built. | `Screened`/`Asserted` conditions and `⊘` characteristics are excluded by construction and shown elsewhere (brief §9). |
| **`core.json` authored with the graph inverted.** §8c reads effect-first; `Edge` is cause-first. | Structural orientation assertion in `core_pack_test` (Phase 3 gate). The coverage assertion cannot catch this. |
| **Duplicate measures proliferate.** The failure mode that makes the library worthless within a month. | Series-level structural identity, "did you mean" at creation, usage counts on every picker row, blast radius before editing. |
| **The engine drifts into the analysis path.** Scope creep with a plausible-sounding justification. | No call site outside tests. If someone needs it live, that is a separate change with its own review. |
| **Phase 3 blocks on Mark's §13 answers.** | Ask before Phase 3 starts, not during. Phases 1–2 do not depend on them. |
| **Seeded content is uncitable and quietly badged `active`.** | Every direction inferred rather than sourced lands at tier `proposed` and the UI badges it. Do not launder it with a loosely-related citation. |

---

## 4. Open decisions needed before Phase 3

All closed (brief revision 4). Recorded so they are not re-litigated mid-implementation.

| Decision | Value |
|---|---|
| Panel label | **"Characteristics"** — in `qsTr()` strings, the search index, and file/class naming |
| `core.json` | Repo file is source of truth; Qt resource **generated from it at build time** |
| Reach labels | `Screened` → **"Physical"** ("needs a physical screen"); `Asserted` → **"Behavioural"** ("ask the golfer") |
| `s_posture` → alignment | **Edge dropped** — no defensible mechanism. Pack is **81** causal edges |
| Unauthored tails | Left unauthored by design; health list flags them as single-tail axes |
| `c_posture` | Carried, marked `⊘`, as the pack's headline capture gap |
| Run scope | **Phases 1–5**, ending at the read-only settings panel |

**Settled earlier, also not to be re-litigated:** C-posture
(thoracic flexion), S-posture (lumbar extension) and too-upright (insufficient hip hinge) are three
measures over three spinal regions, not tails of one axis. Hip internal rotation splits trail (the
backswing turn) and lead (the downswing turn) — asymmetry is the norm, and the lead-side deficit is
the more common and more consequential. Ball position is measured from the stance centre toward the
lead foot, wedge near the centre and driver near the lead heel, so the corridor is club-dependent.

---

## 5. Build status (as delivered)

Phases 1–5 complete and green; nothing committed. 65/65 CTest pass in `build/analyzer-tests`
(59 pre-existing + 6 new), and the app builds, links and starts clean under
`QT_QPA_PLATFORM=offscreen` with no QML errors.

| Phase | State | Notes |
|---|---|---|
| 1 Vocabulary + facets | **Done**, with one deferral | See below |
| 2 Pack schema/validator/providers | **Done** | Every rejection path tested |
| 3 `core.json` | **Done** | 31 characteristics, 19 causes, 81 edges, 28 measures |
| 4 Engine + resolver (dark) | **Done** | Synthetic input only; no call site outside tests |
| 5 Settings panel (read-only) | **Done** | Panel index 10, System renumbered to 11 |

New tests: `anatomy_vocabulary_test`, `anatomy_overlay_parity_test`, `measure_facets_test`,
`characteristic_pack_test`, `core_pack_test`, `characteristic_engine_test`.

### Deferred (pre-authorised by §1 Phase 1's stated fallback)

**The overlay was not re-pointed at the shared resolver.** `anatomy_overlay_parity_test` pins the
resolver to what `video_overlay_pose.cpp` draws today — the midpoint formulas, the 0.25 admission
threshold, min-of-parents confidence, and the bone topology duplicated in `PpCameraFrame.qml` — so
the re-point is provably a no-op when it happens, and drift on either side fails the build in the
meantime. What remains is editing the render path itself, which needs a visual check on a real
swing. The two hand-synced copies still exist.

### Changed from the plan, on contact with the code

- **The reducer applies to BOTH measure kinds, not just composed ones.** The catalogue's
  `pelvisSway`, `spineForwardBend`, `leadWristFlexExt` and `lagAngle` are `TimeSeries` — curves that
  still need reducing. Sway, slide and hanging back are three reducers over ONE series, so one
  producer unblocks three characteristics. Modelling them as three metrics would have been the
  parallel registry this design exists to avoid.
- **The seed binds to existing catalogue keys wherever they exist.** The catalogue turned out to
  carry 43 keys, many matching seed characteristics directly. Minting composed stubs beside them
  would have duplicated the registry. Result: 8 live measure bindings on day one, 14 planned,
  8 with no producer, 2 capture gaps — a far better picture than the brief's "expect most to mint
  as NoProducer".
- **`CharacteristicPack::signals` had to become `signalDefs`** — Qt's moc keyword macro expands
  `signals` to an access specifier, breaking every TU that includes both this header and QObject.

### Owed before this ships

- Re-point the overlay (above), with a visual check.
- Phases 6–8: MeasurePicker + sentence editor, roadmap view + export, health list and cause
  coverage view. The C++ façade already exposes `roadmap()`, `captureGaps()`, `causeCoverage()`
  and `health()` — only the QML is missing.
- Citations. Every one of the 81 edges and 50 conditions is currently tier `proposed`, which is the
  honest state (directions were reasoned from mechanics, not sourced) and produces 58 health-list
  warnings by design.
