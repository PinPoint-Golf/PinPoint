# Diagnostics content extension — implementation plan

Companion to the brief (`docs/design/diagnostics_content_extension.md`). The brief is the *what*;
this is the *how*, sequenced, with every repo fact it depends on verified rather than assumed.

**This is a live working document.** Update the Progress table and Session log as stages land.
Work spans multiple sessions with context clears between them — the tables below are what makes a
cold restart possible. Do not treat any stage as done until its gate has actually run green.

**Anything deferred goes in the ledger at the end of this document, not in the resume block.** The
resume block is rewritten every stage; the ledger is not.

**Read first:** `docs/developer/diagnostics_developer_guide.md` — especially §8, the live/dormant
audit. This package extends the CONTENT layer and its maintenance surfaces. It does **not** wire the
engine: `detect()` still has no caller, and that is the next package
(*Diagnosis execution, verification and validation*, handed over from
`diagnostics_norms_impl_plan.md`).

---

## Progress

| # | Stage | State | Landed |
|---|---|---|---|
| 0 | Work-package doc + brief into `docs/design/` | ☑ complete | 2026-07-27 |
| 1 | Schema seams — groups, `ExternalDevice`, LM requirement, condition aliases | ☑ complete | 2026-07-27 |
| 2 | §1 hygiene — units, `highMeans`, alignment measures, `drill.*` | ☑ complete | 2026-07-27 |
| 3 | §5 seed norms for the unnormed measures | ☑ complete | 2026-07-27 |
| 4 | Metric catalogue descriptors + `LaunchMonitorProvider` | ☑ complete | 2026-07-27 |
| 5 | §4 measures into `core.json` | ☑ complete | 2026-07-27 |
| 6 | §3.1–3.5 conditions (setup · backswing · transition · impact · finish) | ☑ complete | 2026-07-27 |
| 7 | §3.6 ball-flight and strike outcomes + causal edges | ☑ complete | 2026-07-27 |
| 8 | §6 corroborates / excludes — content **and** consumer | ☑ complete | 2026-07-27 |
| 9 | Screens + drills registries, and the maintenance surfaces | ☑ complete | 2026-07-27 |
| 10 | Documentation and the citation backlog | ☑ complete | 2026-07-27 |

State vocabulary: ☐ not started · ◐ in progress · ☑ complete (gate green) · ⚠ blocked.

## Session log

Newest last. One line per session: what landed, what the gate said, what the next session picks up.
Keep it factual — this is the handoff, not a summary.

| Date | Stages touched | Outcome |
|---|---|---|
| 2026-07-27 | 0 | Plan written and verified against the tree. Brief copied to `docs/design/diagnostics_content_extension.md`. The audit below found eight claims in the brief that do not survive contact with the repo — recorded so no session re-derives them. Nothing built yet. |
| 2026-07-27 | 1 | **Stage 1 complete — the seams, no content.** Three new `ConditionGroup`s (Impact, Finish, BallFlight), and `allConditionGroups()` added so the order lives in ONE place: it was hand-written in three (the enum, the library's filter row, the editor's picker) and a fourth group would have drifted them. `MeasureStatus::ExternalDevice` with its own colour, filter chip, picker line, measure-page availability sentence and rank (weaker than NoProducer — code AND hardware — but stronger than NotCapturable), and `roadmap()` now KEEPS those rows with `integration: true` rather than dropping them, sorted after the pipeline work and sectioned in `roadmapMarkdown()`; `captureGaps()` stays `NotCapturable`-only, so the pack still reports zero true capture gaps. The per-shot half is `MetricRequirement::launchMonitor` + `ShotContext::hasLaunchMonitor` + one line in `describeRequirement()` — a golfer with no launch monitor now gets "needs a launch monitor" through the identical path a missing face-on camera takes, which is the graceful fallback rather than a promise of one. `Condition::aliases` parses, saves, is searched (with the MATCHED alias marshalled back so a row can say why it was returned), renders on the detail page above the consequence, and is editable as one comma-separated line in the editor. Two new validator warnings: `duplicateAlias` (case-folded, and a condition's own LABEL counts as a claim) and `externalDeviceNoReason`. Analyzer suite **78/78**; app builds clean. Next: stage 2. |
| 2026-07-27 | 2 | **Stage 2 complete — and both failures it caused were the discipline gates working.** Units onto the 16 unit-less **Provided** measures, taken from the metric catalogue rather than invented. `highMeans` onto the two peak measures. `m_feetAlignment` / `m_hipAlignment` + four signals + four conditions, wired as **corroborates** into the existing shoulder pair rather than split into six alignment conditions: the feet, hips and shoulders are three readings of ONE setup decision, and three findings on a dashboard for one fact is what the brief's option (b) exists to avoid. Then `aim_bias_open` / `aim_bias_closed` were wired as their cause too — which is what makes the corroboration mean something (sibling effects of one cause, not a chain) and clears `noCause` honestly rather than by declaring them unexplainable; `hasCausalPath` stays satisfied because two siblings can reach neither each other. **Two tests went red and both were right to:** `axis_direction_test` refused the four new signals until each had a fixture row quoting a sign convention, and `hipAlignment` / `feetAlignment` did not state one — so the convention ("OPEN IS NEGATIVE AND CLOSED IS POSITIVE") went into the descriptors first and the fixture quotes it; and `diagnostics_catalogue_integrity_test` refused the new uses until the catalogue's `usedBy` recorded them. Analyzer suite **78/78**. |
| 2026-07-27 | 3 | **Stage 3 complete — and the brief's table could not be transcribed.** Nine Composed measures had no unit in the file and the loader fills those from `quantityUnitHint()`, which returns a DIMENSION ("degrees", "length") rather than a unit — so each got its real unit from the catalogue first. That exposed the reason the seed table had to be re-derived: for four of them the catalogue does not use the unit the brief assumes (`ballBodyDistance` is % shoulder width, `thoraxLateralDrift` % stance width, `trailElbowHeight` % shoulder width, `leadHandWidth` % arm length — all normalised so a tall golfer and a short one read alike) while the brief seeds them in centimetres. Writing a centimetre value into a percentage field is precisely what `normUnitMismatch` is structurally incapable of catching once both sides spell the unit the same. **Six rows re-seated against the catalogue's own prose**, the sharpest being `m_xFactorStretch`: the brief seeds 48°, which is the X-FACTOR (separation at the top), where this measure is the STRETCH (separation ADDED through transition) that the catalogue puts at "roughly 5°". At 48 the corridor would have sat ten times above the population and `sig_xfactorDeficit`, which fires LOW, would have reported a deficit on every swing ever recorded. Also re-seated: `m_axisTiltAtTop` 5→12 (below even the catalogue's ADDRESS figure), `m_pelvisSwayImpact` 6→3 ("near or just past zero by impact"), `m_shoulderPlane` 90→50 (the series is the shoulder line to GROUND, not to the spine). 20 rows, all `heuristic`, n = 0. Norm rows 68 → 88. One unexplained single-run failure of `norm_model_test` did not reproduce across three subsequent full runs — ledgered as `X6` rather than called fixed. |
| 2026-07-27 | 4 | **Stage 4 complete — the launch-monitor seam has a producer, not a placeholder.** 16 new `MetricDescriptor`s, each written sign-convention-first because `axis_direction_test` settles a signal's tail by quoting `howToRead`, so the descriptor has to exist before the measure that reads it. Two new groups (Ball flight, Strike). The nine launch-monitor metrics are deliberately **not** `.planned`: a planned metric has no producer and always resolves Unavailable, whereas these have a producer the golfer may not own — so `LaunchMonitorProvider` claims them and answers through `fromRequirement({launchMonitor}, ctx)`, which reads "needs a launch monitor" today and returns Measured the moment a connector sets `hasLaunchMonitor`, with no catalogue change. That makes it the integration's insertion point rather than a stub to delete. **`faceAngle` moved with them** and left `PlannedMetricProvider`: it had been telling two stories at once (a producer is coming; club instrumentation will do), and neither was true. The test gained a launch-monitor section asserting all four halves — requires-the-device, Unavailable without, says why, Measured with. Forward-looking `usedBy` entries for the 29 conditions stages 6–7 will add were **stripped**: the reverse index must name actual consumers, and the integrity test's forward half will demand them back as each condition lands. Analyzer suite **78/78**. |
| 2026-07-27 | 5 | **Stage 5 complete.** 37 measures — skeleton, shaft, ball and the nine launch-monitor ones with `status: externalDevice` and a `gapReason` opening `"Requires launch monitor: "`, so `grep` enumerates the integration surface. Status came from what the catalogue actually says, not the brief's table, which calls four planned metrics live. `viewNeeded` is left unset wherever the catalogue's requirement already states it — `roadmap()` reads the descriptor and upgrades to face-on itself, and a second place to state one fact is a second place for it to go stale; only the down-the-line readings, which that upgrade cannot reach, are set explicitly. Two things the loader caught: a **rate reducer needs its starting phase** as well as its window ("A change needs a starting phase"), and `m_pelvisSwayFinish` makes pelvis sway carry **four** reducers, so two count assertions were updated deliberately rather than loosened to `>=` — the claim is that these are the reducers somebody chose. Analyzer suite **78/78**. |
| 2026-07-27 | 6 | **Stage 6 complete — 38 conditions, 35 signals, 46 corridors, and four gates that earned their keep.** §3.1–3.5 in full, with norms landing WITH their signals rather than in a later pass (a corridor signal on a live measure with no norm is refused, and that dark state is what the norms work existed to end). **(1)** `axis_direction_test` refused all 35 signals until each had a fixture row quoting the descriptor that decides its tail — and writing them surfaced a genuine inversion trap: the catalogue's convention for `attackAngle` is the STRIKE DIRECTION, "higher means a more upward strike", so **`attack_too_steep` is the LOW tail**. An author matching the words "steep" and "high" would have shipped it inverted, which is exactly the defect class `highMeans` exists for. **(2)** `m_leadForearmRot_p2` and `m_leadElbowFlex_p4` are wrist-grid cells that carried no `highMeans` — harmless until `early_face_roll` and `bent_lead_arm` chose tails from them, which is ledger `X2` biting where it should. **(3)** `manifest_migration_test` refused a clubhead-speed corridor: a speed band is athlete-relative and a population figure would grade a junior against a tour player, so the measure stays and the corridor does not. **(4)** The brand-name gate fired on `attackAngle` the moment the pack came to depend on it, finding **two pre-existing vendor attributions** in the catalogue (`clubheadSpeed` and `attackAngle`); the figures are common domain and stayed, the attributions went. `usedBy` is now GENERATED from the pack by a scratch script rather than hand-maintained — the integrity test computes ground truth the same way, so hand-editing 70 reverse indexes was work with a guaranteed-correct machine answer. Analyzer suite **78/78**. |
| 2026-07-27 | 7 | **Stage 7 complete — 20 outcomes, 60 causal edges, and a real defect in the grade rule.** Two structural findings shaped the content, neither in the brief. **(1) `detect()` ORs a condition's signals** (`characteristic_engine.cpp:218`): ANY signal firing fires the condition. So the brief's strike outcomes — "low point behind AND speed collapse" — cannot be two signals, because a chunk would then fire on either half alone, which is worse than not detecting it. Outcomes with ONE discriminating measure ship `measured`; the seven needing a CONJUNCTION (`chunk`, `thin`, `top`, `sky`, `shank`, `pull_hook`, `push_slice`) ship signal-less as `asserted` — the golfer knows and the app does not, which is a true statement — and still carry their definitions and terminate the causal chains, which is what the intake needs. **(2) Two conditions on the same TAIL of one measure always fire together**, so `block` became an ALIAS of `push` rather than a second condition. Ball-speed and carry corridors were deliberately NOT authored: `manifest_migration_test` already pins that clubhead speed "has no defensible band yet" because a speed band is athlete-relative, and a population figure would grade a junior against a tour player — the same claim applies to both, so those outcomes ship dark until a per-athlete baseline exists. **The defect:** `reference_bands_test` swept a smash-factor corridor and found `grade()` and `bandEdgesOf()` disagreeing at a band edge — `bandEdgesOf` computes `mu − k·sigma` while `grade` asked `(value − mu)/sigma <= k`, and for mu 1.40 / sigma 0.08 those are not inverses in floating point, so a value ON the drawn edge graded Good while the band called it Green. **An epsilon moved the bug rather than fixing it** (a tolerance on the z comparison rounds in samples the band still excludes — the same disagreement with the sign flipped); what removed the class was `withinBand()`, comparing against edges computed the SAME way on both paths, so the two now agree by construction at every magnitude with no fudge factor. 62,658 swept samples over all 149 rows and both precedence branches, green. Also re-scoped `core_pack_test`'s cause-concentration assertion to SWING edges: the outcome layer added a tier of fault→outcome edges no screen result reaches directly (a tight trail hip does not cause a slice, it causes the delivery that causes it), and counting them would make the library look less concentrated purely for having become able to explain what the golfer saw. Threshold unchanged. Analyzer suite **78/78**. |
| 2026-07-27 | 8 | **Stage 8 complete — the two edge types now do something.** 9 corroborates and 5 excludes authored, and `relation_resolver` extended to consume both. **Excludes resolves BEFORE ranking**: two findings that cannot both describe one swing would otherwise put a contradiction in front of a coach AND let one cause be credited twice for two versions of one event. The more confident reading stands, ties break on the id, and the dropped one is RECORDED in `Explanation::suppressed` with a reason — "we saw both and kept this one" is a different statement from "we only saw this one", and a coach who cannot see the first will not trust the second. It is also excluded from `unexplained`, so one event never appears under two contradictory headings. **Corroboration is reported, never scored.** A multiplier would mean inventing a number nobody could defend when asked why one cause outranked another — the same reason `material` contributes zero rather than a fraction — so it enters the ranking only as a TIE-BREAK, which needs no magnitude, and the test asserts the scores are byte-identical to an un-corroborated run. Two of the brief's rows could not be authored as written: `trail_knee_straighten corroborates sway` is already a CAUSE (the shadowing gate would refuse it, rightly), and the axis pairs (`across_the_line`/`laid_off`, `pull`/`push`, `slice`/`hook`) state nothing the corridor does not — so exclusions were authored only where they add something, on the signal-less asserted strike outcomes where nothing else stops three being recorded at once. Both relation types now render on the detail page BELOW the DAG, because the graph ranks by signed causal distance and a symmetric relation has no direction to rank by. Analyzer suite **78/78**. |
| 2026-07-27 | 9 | **Stage 9 complete — `screenRef` and `drills` stopped being namespaces.** Two new registries (`screen_pack`, `drill_pack`) with load/save/validate, a user layer merged by id, and `PINPOINT_CORE_SCREENS` / `PINPOINT_CORE_DRILLS` overrides so standalone tests reach shipped content. Deliberately NOT behind the abstract-provider hierarchy the packs and norms use: that polymorphism exists so a COMMUNITY pack can be namespaced against core, there is no community story for reference content, and three provider classes for a flat list would be ceremony standing in for a requirement. **13 screens** described generically from clinical ROM literature — no branded screening system named, cited or alluded to, gated by a raw-bytes grep — with several deliberately QUALITATIVE (the test asserts that, so the absence of a number reads as intent rather than as unfinished work). **12 drills**, written as intent and never as a promise, attached to the 23 conditions a starter set can honestly answer rather than to everything, which would train a reader to ignore the field. §3.7's coach vernacular landed on 86 of 112 conditions — with `slide` kept on the body fault and never added to `slice`, noted in the script because the next author will be tempted to tidy it. New `reference_sets_test` (24 assertions, validators gated in both directions); new health codes `unknownScreenRef` / `unknownDrillRef` plus the four registry warnings, all labelled in `HealthView`. Settings → Diagnostics gained a **Glossary** view — which needs no dataset at all, being the rule set read out: label and aliases from the condition, meaning from `consequence`, and "commonly caused by" straight off the causal edges — and a **Screens & drills** view ranked by how much each settles, because that ordering IS the argument the model makes. The detail page now resolves the screen rather than printing its id, and a dangling ref reads as a defect. Analyzer suite **79/79**; app builds clean and starts clean headless. |
| 2026-07-27 | 10 | **Stage 10 complete — and the headline is a number.** **16 of 83 signals can fire, over 16 conditions, up from 8 of 31** — the firing set DOUBLED, and every bit of that came from a gap nobody had spotted: eleven producer keys were live and carried no measure at all in the pack (head sway, head lift, head tilt, lead heel lift, both foot flares, toe line, impact shaft lean, hand speed, clubhead speed, backswing tempo). It cost no pipeline work whatsoever. Zero signals are live-with-no-norm, which `core_pack_test` asserts. The developer guide's §8 census was wrong in every number and is rewritten; §1 now lists five registries; §3 documents `ExternalDevice` and `aliases`; §6 gains the two non-causal edge types and why one changes the output while the other only reports; §11 gains "a screen or a drill"; §12 gains `reference_sets_test`. The metric catalogue guide records that hardware the user may not own is a REQUIREMENT rather than a `planned` flag, and why both mistakes are lies. The sign-conventions doc gains the sixteen new conventions and — more usefully — **the `attackAngle` trap**, where the catalogue's convention is the strike direction so `attack_too_steep` is the LOW tail: the condition's name and the metric's sign point opposite ways, which is the whole reason the fixture table forces a quote. The user guide gains a ball-flight section (what a launch monitor adds, why chunk and thin are asserted, and why draw and fade are deliberately absent) and the search-by-coach-term note. Analyzer suite **79/79**; app builds clean and starts clean headless with zero QML errors. |

| 2026-07-27 | follow-up | **Settings reorganised, and the DAG says what its lines mean.** Mark's asks, all three real. **(1)** Metrics stopped being a settings panel and became a VIEW inside Diagnostics — a metric, the measures that read it and the corridors that judge them are one chain, and following it meant leaving the page. That renumbered the panel stack (Diagnostics 10 → 9, the System action row 11 → 10), the three deep-link functions, and `SettingsIndex`'s search entries, which gained rows for Drills and the Glossary so those views are reachable from the settings search rather than only by eye. Pill order is now Characteristics · Measures & norms · Metrics · Causes & health · Drills · Glossary, with Roadmap still far right and developer-only. **(2) The dashed lines are gone.** They were carrying the detection and offered-cause distinctions, and at this line weight a dashed cubic reads as a rendering artefact rather than a choice; colour separates all four kinds cleanly and a legend NAMES them, which a dash pattern never could. The legend lists only the kinds actually present — naming a relationship the graph does not contain teaches a reader to look for something that is not there. **(3) Corroborates and excludes are drawn.** They were laid out nowhere: rank is signed causal distance and a symmetric relation has no direction to rank by. They now join the focus's own rank, ABOVE it so the detection lane below stays clear — `dag_layout_test` caught `mLive->focus crosses rival` the moment they went below, which is exactly the crossing check earning its keep. No arrowhead (an arrow asserts a direction they do not have) and no strength word on an exclusion (the pair is incompatible or it is not). Scoped to the focus. Analyzer suite **79/79**; app builds clean and starts clean headless. |
| 2026-07-27 | follow-up 2 | **Relations became editable — add, edit, delete, undo — and doing it found a data-loss bug.** `linkRelation` / `unlinkRelation` / `editRelation` / `undoUnlinkRelation` + `relationCandidates` / `relationsOf` on `CharacteristicEditorModel`. They do NOT go through the draft: `beginEdit()` models "the effect owns its incoming causes" and loads causal edges only, so a symmetric edge held by either end would have its identity rewritten by the other end's next save. **The bug that fell out of writing that down:** `save()` erased every edge whose `to` matched the condition being edited, regardless of type — so opening a characteristic to fix a typo in its consequence silently deleted every corroborates and excludes edge pointing at it. Latent until this package authored the first fourteen. Scoped to `Causes`, with a regression test **verified red** against the old line before the fix went in. Placement is forced by the model, not chosen: ADD is on the detail page behind a candidate picker, because the validator refuses corroboration over a causal path and the nodes the DAG draws are therefore precisely the ones that cannot corroborate; edit and delete sit on both the detail rows and the DAG long-press menu, which now offers the symmetric actions INSTEAD of the causal ones for an already-related pair. Candidates are filtered rather than listed-and-refused, search reaches aliases, a shipped relation is overridable but not deletable and says so before the tap, and the two undo slots are kept separate so "put the link back" cannot restore the wrong kind. Analyzer suite **79/79**; app builds clean and starts clean headless. |
| 2026-07-27 | follow-up 3 | **Two defects from Mark's testing, and the second was the dangerous one.** **(1) Press-and-hold on a rank-0 partner did nothing.** Rank 0 used to MEAN "the focus", so a symmetric partner sharing that rank was typed `DagNodeKind::Focus` — it took the focus's coloured frame and the long-press menu skipped every item, since each is guarded on `kind !== "focus"`. Zero items, so the menu returned without opening; the odd frame and the dead press were one defect. Added `DagNodeKind::Related` and decided kind by IDENTITY rather than by rank. **(2) A relation edge silently deleted shipped CAUSES.** The merger lets a LocalUser pack replace the causal edge set of any condition it names as an effect — that erase was unscoped by type, so writing a symmetric edge naming B dropped every shipped cause of B from the assembled library. Nothing downstream could have noticed: fewer explanations, no error. Scoped to `Causes`, with symmetric edges overriding per PAIR instead, which is also what makes re-typing a shipped relation work rather than leaving two contradictory rows. Verified red before the fix. **(3) Delete now works on shipped relations.** The refusal had been honest about a real limitation — a user pack is purely additive for an edge that belongs to neither end — but "you cannot delete this from your own library" is a poor answer, so `CharacteristicPack::retiredEdges` carries tombstones, applied last by the merger, LocalUser only. Adding a relation back clears its tombstone, or the write would succeed and the merger would eat it again with a success toast on screen. Delete is on every detail row, and the DAG's removal row reads "Unlink X and Y" — the causal rows carry their signal of change in the "no longer", and "X and Y are not linked" had none, so it read as a statement of fact. Analyzer suite **79/79**; app builds clean and starts clean headless. |
| 2026-07-27 | follow-up 4 | **Zoom on the DAG — controls plus ctrl+wheel — and two interaction defects on the way there.** The zoom multiplies the FIT rather than replacing it, so 100 % always means "as it fits this panel" whatever the window width; it scales the drawing and never re-runs the layout, since re-laying out at a different character width reflows the columns into a different picture rather than a closer look at the same one. Ctrl+wheel anchors on the pointer, the buttons anchor on the view centre (a button press has no meaningful pointer position), and zoom resets to 100 % on every re-centre because a 3× view held across a tap leaves the new focus off screen and reads as the tap having done nothing. **(1) `ev.position` does not exist on a QML `WheelEvent`** — it carries `x`/`y` directly, and `position` belongs to an `EventPoint`. The TypeError killed the zoom on the first line that mattered while leaving the handler looking wired up. Confirmed against Qt's own `plugins.qmltypes` rather than from memory. **(2) The handler was a SIBLING of the Flickable**, which handles wheel itself — so whether the event arrived depended on what was under the pointer and on whether the content could scroll that way, which is why it worked over the graph and not over whitespace. Moved onto a transparent overlay filling the plot, added last so it sits above the competition; it accepts no other input, so taps, long-presses and the zoom buttons are unaffected, and `acceptedModifiers` still lets a bare wheel fall through to pan and then to scroll the page. **Three defects in this batch surfaced only on interaction** — the `Related` kind, the wrong event property, and this — none visible to a green suite, which is what ledger `X8` is about. |
| 2026-07-27 | follow-up 5 | **Causal strength is editable from the graph, and the flaky test turned out to be a real bug.** "Early extension USUALLY causes a shank" could only be changed by opening the EFFECT's edit page and finding the row in its causes list — so from the cause's own page there was no route at all, and the graph drew a word it gave the reader no way to change. `setCauseStrength()` is separate from `linkCause`, which refuses an already-linked pair: re-using it would mean defeating its own refusal, and a call that both creates and silently overwrites is one an author cannot predict from its name. It refuses a pair with no edge (it does not quietly create one) and refuses setting what is already set. The DAG's long-press menu offers the two OTHER strengths in the same words the line is labelled with, from either end of the edge, omitting the current one because a row that changes nothing wastes a press. **`X6` is closed, and both earlier diagnoses of it were wrong** — not test ordering, not a relink race, but a dangling reference into a pack owned by a `unique_ptr` temporary. Reproducing it standalone (3 in 25) is what ruled the other theories out. The same pattern had been copied into `characteristic_editor_test` by this package. Analyzer suite **79/79** across six consecutive full runs; app builds clean and starts clean headless. |
| 2026-07-27 | follow-up 6 | **A new link stayed invisible until restart, and the cause picker made you press Done.** **(1)** `CharacteristicLibraryModel` caches its provider and the editor writes through one of its own, so a new causal link was on disk and in the editor's view of the world while the page that had just asked for it kept answering from the pack it was built with. Bumping the QML revision re-ran the bindings against that same stale pack, which is exactly why it looked wired up. The re-take went into the `onLibraryChanged` handler rather than the graph handler, because EVERY write path finishes with `reload()` — save(), linkCause, unlinkCause and the three relation calls — so one place covers all routes. The corridor path had documented this rule since stage 6; the graph path never picked it up. **(2)** The cause picker toggled and stayed, making it a multi-select its own heading ("Add a cause") did not promise and leaving the author hunting for Done after they had already said what they wanted. It now commits and returns, matching the measure picker beside it, and the header button became Cancel — "Done" implied the choice needed it. Adding several causes is now several trips through the sheet; removing one still lives on the ✕ next to the row. Analyzer suite **79/79**; app builds clean and starts clean headless. |
| 2026-07-27 | follow-up 7 | **An ontology audit, and the headline is that the model could detect ten things it could not explain.** A structural sweep of the shipped pack — orphans, graph shape, field completeness, validator coverage, marshaller reach — asking what is outstanding in BUILD and TEST terms rather than what was planned. **The find: of the 16 conditions that can fire today, four were ISOLATED (no cause and no effect) and six more had no cause.** Detection without explanation hands the coach "your head moved", which the golfer already knew, and the next sentence is the entire product. The gap was invisible because it forms at the intersection of two healthy-looking states — causal work goes in per GROUP, the firing set is decided per PRODUCER, and the eleven live-but-unclaimed producer keys that doubled the firing count at stage 10 arrived after their groups had been wired. `noCause` never surfaced it because it fires 25 more times on content nobody can capture yet. **19 causal edges authored** (159 → 178), every one sourced from an existing latent cause or a consequence sentence the condition already carried: `stance_wide → sway` is what its own consequence says out loud; `head_drop_backswing → low_point_behind_ball` likewise. Two shapes needed care — `excessive_head_sway` CORROBORATES `sway`, so a causal edge between them would be refused by `corroboratesCausal`; it got `sway`'s own root causes instead, as siblings (the stage-2 alignment pattern). Same for `insufficient_shaft_lean`, which corroborates `scooping`: both now hang off `casting`. Cause concentration moved deliberately, not loosened — the five dominant causes go 9/8/6/5/5 → 10/11/6/7/6 and the `topFive >= swingEdges * 3/10` ratio holds at 40 ≥ 39. **`core_pack_test` gained the gate that stops it recurring**: every condition that can fire today has at least one cause, scoped to the firing set because a `noCause` on a producer-less condition is a backlog and one that fires TODAY is a defect in what ships. Verified red against the pre-edit pack (8 findings) before the edges went in. **Four more defects, each small and each real.** (1) `observableNoSignal` was unscoped by `ConfirmedBy` and accused all seven signal-less strike outcomes — `Observable` + `Asserted` with no signal is the truthful encoding of "the golfer can see it and we cannot measure it", and seven rows describing the design is a health list people learn to scroll past; scoped exactly as `inconsistentReach` one line below it always was, verified red both ways. (2) **`detect()` ignored `Condition::state` entirely**, so a `Retired` or `Superseded` characteristic would have gone on diagnosing exactly as it did the day it was sound. Now skipped — and ONLY those two, because 62 of 112 shipped conditions are `Draft` and reading "not finished" as "not in use" would dark more than half the library. The negative half of that test is the important half. (3) `duplicateMeasure` had no `HealthView` label and fell through to the raw camelCase, which `_codeLabel`'s own comment says must never happen. (4) `screens.json` / `drills.json` were embedded into `swinglab_run` and `swing_window_parity_test` with their loaders NOT in `_pinpoint_offline_sources` — data with no reader, which reads as content that failed to load rather than content nothing asked for. **`bothTailsOneCondition`, `inconsistentReach`, `needsRevalidation` and `duplicateMeasure` had no test in EITHER direction** against the guide's own §11 rule; all four are now gated both ways, which is what would have caught defect (1) when it was written. `passAtLeast` / `hasPassValue` / `unit` are marshalled and rendered nowhere — left as-is and DOCUMENTED at the marshaller, because the prose `passCriterion` already carries the figure and the structured triple exists to record a screen answer against, which needs per-athlete storage (`X3`). Analyzer suite **79/79**; app builds clean and starts clean headless. |

---

## ⊘ This work package is complete

All ten stages landed. Analyzer suite **79/79**; app builds clean and starts clean headless.
Stages 0–10 and the six follow-ups are **committed and pushed** through `083aad7`. The ontology
audit below (follow-up 7) is a later, separate pass — check `git log` before assuming its state.

**What it delivered.** The pack went from 67 measures / 31 signals / 50 conditions / 81 edges to
**106 / 83 / 112 / 159**, norms from 68 rows to 149, and two new registries (13 screens, 12 drills)
behind the `screenRef` and `drills` fields that had pointed at nothing. Three new condition groups,
condition aliases and the glossary they carry, a launch-monitor seam that degrades gracefully instead
of going blank, `Corroborates` and `Excludes` with a consumer rather than just a validator, and two
new views in Settings → Diagnostics.

**What it deliberately did not do.** Run a diagnosis. Nothing constructs `CharacteristicEngine`;
that remains the next package. This one made the content it will read, and the surfaces that
maintain it.

**Four defects the gates caught along the way**, each of which would have shipped silently:
a `grade()` / `bandEdgesOf()` floating-point disagreement at band edges; an `m_xFactorStretch`
corridor seeded ten times above the population (the brief gave the X-factor where the measure is the
stretch); two pre-existing vendor attributions in the metric catalogue, exposed the moment the pack
came to depend on `attackAngle`; and the `attack_too_steep` tail, which points the opposite way to
its own name.

---

## Settled decisions

Taken with Mark before any code was written. Do not reopen without him.

1. **The whole brief is this package; executing a diagnosis is not.** The engine stays dormant. Adding
   content to a dormant engine is deliberate: the pack is the reviewable artefact, and the surfaces
   that maintain it are shipped and live.
2. **Launch-monitor integration is planned work, not a permanent capture gap.** The dependency belongs
   on the *measure*, resolved by the requirement layer that already answers for a missing camera or
   IMU. So LM measures take a new `MeasureStatus::ExternalDevice`, and **no new `ConfirmedBy` value
   is needed** — an LM outcome is an ordinary `Measured` condition whose measure is not produced yet.
   Graceful fallback for a user with no launch monitor is the same code path as every other absent
   sensor, and is the default position today because no connector exists.
3. **Intent contexts (`intent_draw` / `intent_fade`) are dropped.** A shot resolves to exactly ONE
   context node, so an intent node under `full_swing` would be a *sibling* of `iron` — a driver draw
   could not be both. Draw and fade are glossary terms: a draw is curvature the golfer meant, and
   intent is not something the pack can key on. Recorded as `X4` in the ledger with the latent
   archetype-versus-club version of the same conflict.
4. **Build the code halves, and the maintenance surfaces with them.** Authoring `corroborates` edges
   that nothing reads, or a `screenRef` namespace with no registry behind it, is the trap the
   developer guide already names twice. Content that Settings cannot maintain is the missing
   ingredient, not a later polish item.

---

## Repo facts this plan is built on (verified, not assumed)

### Eight claims in the brief that do not survive contact with the tree

| Brief says | Reality |
|---|---|
| §1.9 ten composed metricKeys "exist in neither" — drift | All ten **are** catalogue descriptors (`metric_catalogue_manifest.cpp` ships 54). The brief checked `metric_providers.cpp` (producers), not the manifest (descriptors). There is no drift; the `planned`-descriptor-without-producer shape is the intended one. **Item dropped.** |
| §2.3 verify `Phase` tokens for `p2 p5 p8 p9 finish` | Already covered. `measure_facets.cpp:73-110` spells P1–P9 plus takeaway, transition, downswing, release, finish and max speed, and `phaseFromToken` iterates all fifteen. **No change needed.** |
| §5 "add club rows for `m_ballPosition` and `m_stanceWidth`" | Both already carry rows at `driver`, `fairway_wood`, `iron` and `wedge`. **Item dropped.** |
| §4 `m_feetAlignment` / `m_hipAlignment` / `m_lowPointAhead` / `m_thoraxRotP4` are **live** | All four keys sit in `PlannedMetricProvider::provides()` and resolve `Unavailable` with "planned — not yet produced in this build". They are `planned`. §8's "new faults at zero pipeline cost" tranche is materially smaller than the brief claims. |
| §1.2 `m_leadWristAtTop` is an orphan — delete it or give it a norm | **Neither.** `manifest_migration_test.cpp:159` asserts it exists *and still has no norm*, calling that "the premise of the next assertion": it is the fixture for the candidate-list rule in `corridorForMetricAtPhase()`. Giving it a norm silently repoints the wrist grid's Top corridor from the Δ-from-address cell to the absolute reading. Keep it; document why. |
| §1.3 "many measures carry `unit: "-"`" | 25 measures have an empty unit in the file, but the loader fills the 9 **Composed** ones from `quantityUnitHint()` (`characteristic_pack.cpp:726`). Only **16 Provided** measures are genuinely unit-less. |
| §2.6 intent contexts under `full_swing` | The context tree is single-parent and a shot resolves to one node — see settled decision 3. |
| §3.6 LM outcomes are `asserted` **with** corridor signals | Refused by `core_pack_test.cpp:193-206` ("no Physical/Behavioural cause carries a measure that could reach the roadmap") and warned by `validatePack`'s `inconsistentReach`. Resolved by settled decision 2 instead: they are `Measured` on `ExternalDevice` measures. |

### Two constraints the brief never mentions, both load-bearing

- **Every new corridor signal needs a fixture row in `axis_direction_test.cpp`**, quoting the metric
  catalogue's own sign convention. A signal with no row **fails the test** — that is the design.
  So every new measure needs a `MetricDescriptor` whose `howToRead` states the convention *before*
  its signal can exist. That is why stage 4 precedes stages 5–7.
- **Most pose metrics are session-gated to Wrist Motion.** `wristSessionOk(sessionType)` returns true
  only for `1` (Wrist) and `-1` (directory browse), and gates `WristMetricProvider`,
  `FootMetricProvider`, `TempoProvider`, `HeadMetricProvider`, `ShaftLeanProvider` and `ScoreProvider`.
  This is a documented catalogue design (`metric_catalogue_developer_guide.md:105`), not a bug — but
  it means new conditions over `headSway`, `leadHeelLift`, `impactShaftLean` and the foot metrics fire
  in Wrist sessions only until it is revisited. Ledgered as `X1`.

### The zero-cost win the brief missed

Eleven producer keys are genuinely live and carry **no measure at all** in the pack:

```
headSway  headLift  headTilt  leadHeelLift  leadFootFlare  trailFootFlare
toeLineAngle  impactShaftLean  handSpeed  clubheadSpeed  tempoBackswing
```

`KinematicSeriesProvider` (`clubheadSpeed`, `handSpeed`, `lagAngle`) carries **no session gate** —
only `clubTrack` — so measures over it are the only genuinely session-agnostic new work available.

### Producer reality, as of this plan

| Provider | Keys | Gate |
|---|---|---|
| `WristMetricProvider` | leadWristFlexExt, leadWristRadUln, forearmPronation, leadArmFlexion | wrist session + lead forearm/hand IMU |
| `KinematicSeriesProvider` | clubheadSpeed, handSpeed, lagAngle | club track (no session gate) |
| `FootMetricProvider` | stanceWidth, leadFootFlare, trailFootFlare, toeLineAngle, leadHeelLift, ballPosition | wrist session + face-on |
| `TempoProvider` | tempoBackswing, tempoRatio | wrist session |
| `HeadMetricProvider` | headSway, headLift, headTilt | wrist session + face-on |
| `ShaftLeanProvider` | impactShaftLean | wrist session + face-on + club track |
| `ScoreProvider` | wristScore, wristResemblance (+ swingScore, always Unavailable) | wrist session |
| `PlannedMetricProvider` | 21 keys | always Unavailable |

The shipped pack's 38 `live` measure statuses were audited against this table: **zero over-claims.**

---

## Stages

Each stage lands independently with its own green gate.

### 0 — Work-package doc  ☑
This document, plus the brief copied to `docs/design/diagnostics_content_extension.md`.

### 1 — Schema seams (code only, no content)

**Condition groups.** `ConditionGroup` += `Impact`, `BallFlight`, `Finish`. Three sites decide the
UI's group ORDER and all three are hand-written lists: `characteristic.cpp` `kGroups`,
`characteristic_library_model.cpp:92`, `characteristic_editor_model.cpp:301`.

**The launch-monitor seam** — what makes graceful fallback real rather than promised:

- `MetricRequirement::launchMonitor` (`metric_descriptor.h:41`), `ShotContext::hasLaunchMonitor`
  (`metric_provider.h:40`), and one line in `describeRequirement()` (`metric_resolver.cpp:29`)
  rendering `"a launch monitor"`. Absent hardware then resolves `Unavailable` with a reason through
  the identical path a missing face-on camera already uses, and flips to `Measured` when a connector
  lands — with no content change at that point. `Bridged` remains available for an LM that supplies
  some values and not others.
- `MeasureStatus` += **`ExternalDevice`** — a producer is intended, but it depends on hardware the
  user may not own. Sites: `measureStatusName` / `measureStatusLabel` / `measureStatusFromName`,
  the sort weight (`characteristic_library_model.cpp:52`), the roadmap filter (`:502`),
  `captureGaps()` (`:555`), `RoadmapView.qml`, and the measure picker. **Roadmap-eligible but
  sectioned separately**, so nobody reads "build a spin-axis producer" when the answer is "integrate
  a launch monitor". `NotCapturable` keeps its true meaning — nothing, ever — and the pack keeps zero
  true capture gaps.
- `normNotCapturable` (`norm_pack.cpp:308`) stays scoped to `NotCapturable`. A norm on an
  `ExternalDevice` measure is legitimate: it is what the reading will be graded against.
- New validator check `externalDeviceNoReason`, and a new `case` in `core_pack_test`'s status switch
  requiring the reason.

**`Condition::aliases`** — mirror of `Measure::aliases`. Parse/serialise in `characteristic_pack.cpp`,
marshal in **all four façades** (guide trap #2: a field can be complete on both sides and reach
nothing), include in directory search, and add a `duplicateAlias` lint so two conditions cannot claim
one term.

`ConfirmedBy` is **unchanged**: `Measured` / `Screened` / `Asserted`.

**Gate:** `characteristic_pack_test`, `core_pack_test`, `metric_catalogue_test`,
`diagnostics_health_test`; app builds.

### 2 — §1 hygiene
- Units on the 16 unit-less **Provided** measures.
- `highMeans` on `m_pelvisRotPeak` / `m_thoraxRotPeak`. The 38 wrist-grid cell measures also lack one;
  they carry no corridor signal, so ledger it (`X2`) rather than inventing 38 sentences.
- `m_feetAlignment` / `m_hipAlignment` measures + `corroborates` edges into `alignment_open` /
  `alignment_closed` — the brief's option (b): two conditions, not six.
- Record `m_leadWristAtTop` as deliberate, in the file and the guide.
- Establish the `drill.*` namespace on `Condition::drills`.

### 3 — §5 seed norms
~21 rows for measures that carry a signal but no corridor (`m_pelvisRotPeak` / `m_thoraxRotPeak` are
order-test only; `m_leadWristAtTop` stays bare). All `source: heuristic`, `n: 0`, at `any` unless the
brief states a club. ★ items become the citation backlog — **do not invent citations**.

**Gate:** `norm_pack_test`, `diagnostics_health_test` (`signalNoNorm` falls), `core_pack_test`.

### 4 — Metric catalogue descriptors
~18 new `MetricDescriptor`s, each with `description` and a `howToRead` that **states the sign
convention** — that sentence is what `axis_direction_test` quotes in stage 6. Skeleton/shaft/ball keys
join `PlannedMetricProvider::provides()`; LM keys get a `LaunchMonitorProvider` answering through
`fromRequirement({launchMonitor = true}, ctx)`, so it is already the connector's insertion point
rather than a stub to replace.

**Gate:** `metric_catalogue_test`, `diagnostics_catalogue_integrity_test` (its §2 asserts every
metricKey the pack names exists in the catalogue).

### 5 — §4 measures into `core.json`
With real units, `highMeans`, `viewNeeded`, honest `status`, and — on every `ExternalDevice` one — a
`gapReason` beginning `"Requires launch monitor: …"`, so `grep "Requires launch monitor"` enumerates
the integration surface exactly as the brief intends.

### 6 — §3.1–3.5 conditions
New conditions + corridor signals, each with an `axis_direction_test` fixture row quoting its
descriptor. Ordered by producer reality: the eleven genuinely-live unclaimed keys first, `planned`
after. `state: draft`, `tier: proposed`.

### 7 — §3.6 outcomes + causal edges
Ball-flight and strike outcomes as ordinary conditions in group `ballFlight`, all `Measured`, all
carrying real signals. Camera-resolvable ones sit on `planned` ball/shaft measures; curvature and
strike-quality ones sit on `ExternalDevice` measures and read "needs a launch monitor" until one is
connected. Then the ~30 `causes` edges from faults to outcomes.

Handedness is a **transform, never mirrored duplicate conditions**. `core_pack_test`'s cause
concentration assertions are exact values (`coverage("poor_pelvic_disassociation") == 9` etc.) —
adding edges under those ids moves the numbers, and the test must be updated deliberately, never
loosened.

### 8 — §6 corroborates / excludes: content **and** consumer
Authoring these without a consumer repeats guide trap #3.
- Author the §6 edge tranche.
- `relation_resolver.{h,cpp}`: corroboration raises a finding's confidence; `Excludes` suppresses the
  weaker of two contradicting findings **and says so**, rather than dropping it silently.
- `dag_layout.{h,cpp}` + `dag()` marshaller + `DagView.qml` render the two new edge types distinctly.
- `CharacteristicEditorModel::linkCause()` gains an edge-type choice (it writes only `Causes` today),
  keeping every existing refusal.

**Gate:** `characteristic_engine_test`, `dag_layout_test`, `characteristic_editor_test`.

### 9 — Screens + drills registries, and the maintenance surfaces
- `screens.json` / `drills.json` in `src/Resources/diagnostics/`, plus `screen_pack.{h,cpp}` /
  `drill_pack.{h,cpp}` and the resource/file/merged provider triplet each, mirroring the pack and norm
  layering (`pack_provider.h` shape, `PINPOINT_CORE_*` override so standalone tests reach shipped
  content).
- §7's 12 screens: generic clinical protocol + pass corridor from physiotherapy ROM literature.
  **No branded screening system anywhere** — `core_pack_test` greps the raw bytes.
- Settings → Diagnostics gains a **Screens** view and a **Glossary** view; the characteristic editor
  gains alias editing, a `screenRef` picker over the registry and a `drills` picker;
  `explain()`'s existing `knownScreenResults` parameter finally has a registry behind it.
- New health checks: `unknownScreenRef`, `unknownDrillRef`, `screenNoProtocol`.
- §3.7 alias content onto the existing conditions.

Screen *answer* storage and the asserted-outcome intake UI are **out of scope** (`X3`) — per-athlete
state with no home in the app today, and downstream of engine wiring.

### 10 — Documentation and the citation backlog
`diagnostics_developer_guide.md` (§8's census is wrong in every number once stage 7 lands; §3 and §11
gain the new enum values and the two registries), `metric_catalogue_developer_guide.md` (the
launch-monitor requirement), `pinpoint-diagnostics-guide.md`, and `pinpoint_sign_conventions.md`.

---

## Verification

```bash
cmake -S src/Analysis/tests -B build/analyzer-tests
cmake --build build/analyzer-tests --parallel 4        # never bare -j; the box OOMs above 4
ctest --test-dir build/analyzer-tests --output-on-failure
```

Stage-critical suites: `core_pack_test`, `characteristic_pack_test`, `axis_direction_test`,
`norm_pack_test`, `diagnostics_health_test`, `diagnostics_catalogue_integrity_test`,
`manifest_migration_test`, `characteristic_engine_test`, `dag_layout_test`, `metric_catalogue_test`,
plus the four façade tests. `reference_bands_test` and `wrist_norm_render_test` guard the live wrist
grid — if either moves, a shipped surface changed.

End of cycle: full app build, headless start, and a pass through Settings → Diagnostics driving the
new views. The DAG and editor stages need **interactions driven headlessly**, not screenshots — the
QML delegate-scope trap throws only on click.

**Do not add a test that pins shipped numbers.** Two parity gates had to be deleted for exactly that.
Gate shapes and relationships.

---

## Ledger

Open items. Rows are added, never silently removed; a closed row keeps its reasoning.

| # | Item | State |
|---|---|---|
| `X1` | Most pose metrics are session-gated to Wrist Motion, so new conditions over head / foot / shaft-lean metrics cannot fire in a Swing session. Documented catalogue design, not a bug — but it caps this package's practical reach and should be revisited when the Swing session grows a metric profile. | open — for Mark |
| `X2` | The 38 wrist-grid cell measures carry no `highMeans`. They bear no corridor signal so nothing can invert, but the field is the direction-audit mechanism and the grid will eventually want signals. | open |
| `X3` | Screen-answer storage and the asserted-outcome intake ("what's your bad shot?") have no home: nothing in the app stores a per-athlete assertion or screen result. Downstream of engine wiring. | open — next package |
| `X4` | A shot resolves to ONE context node, so club and archetype (and any future intent axis) cannot both apply. Latent today only because the archetype rows and the club rows touch disjoint measures. Adding club rows to a wrist measure, or archetype rows to a club-contextual one, makes it bite. | open — design |
| `X6` | ~~`norm_model_test` fails intermittently.~~ **CLOSED 2026-07-27 — and my two earlier diagnoses were both wrong.** It is a **dangling reference**: `const CharacteristicPack &pack = makeCharacteristicPackProvider()->pack()` binds into a pack owned by a `unique_ptr` TEMPORARY, which dies at the end of that statement. Every read after it is use-after-free, which is why it passed most of the time — it depended on whether the freed memory still held the old bytes. Reproduced standalone at 3 failures in 25 runs, which is what ruled out the ordering and relink theories. Fixed by holding the provider; 40/40 standalone and 6 clean full suites after. The same pattern had been copied into `characteristic_editor_test` by this package and is fixed there too. |
| `X7` | `m_ballBodyGap` is the least confident seed in the pack: the brief's ~28 cm could not transfer to the catalogue's "% shoulder width", and 130 % is geometry, not data. Re-seat from the corpus before anyone trusts `ball_too_close` / `ball_too_far`. Club rows are owed on it too — the brief authored it at `iron`, and it currently sits only at `any`. | open |
| `X8` | **The new QML surfaces have not been driven with synthetic clicks** — Glossary, Drills, the relocated Metrics view, and the DAG's new legend and relation lines. `GlossaryView` and `ScreensView` are compile-checked by qmlcachegen, their C++ half is asserted by `diagnostics_catalogue_integrity_test`, and every delegate handler follows the codebase's own scope rules (`root` is the component root; nothing reaches for another file-level id). But the delegate-scope trap throws only on click, and driving them needs a startup navigation hook that does not exist — adding a temporary one at the end of an unattended run is the wrong trade. **Open the Diagnostics panel and click through both views before this is trusted.** Still open after follow-up 7, which touched `HealthView`'s `_codeLabel` (a pure function, no new delegate scope) but drove nothing by hand. | open — for Mark |
| `X9` | ~~The characteristic editor cannot AUTHOR a corroborates or excludes edge.~~ **CLOSED 2026-07-27.** `linkRelation` / `unlinkRelation` / `editRelation` / `undoUnlinkRelation` own their own load-edit-write cycle against the user pack rather than borrowing the draft, because a symmetric edge belongs to neither end. Add lives on the DETAIL page with a candidate picker (the DAG cannot host it: corroboration is refused over a causal path, so the very nodes the graph draws are the ones that cannot corroborate); edit and delete are on both the detail rows and the DAG long-press menu. **It uncovered a live data-loss bug:** `save()` erased EVERY incoming edge of the condition being edited while `beginEdit()` loads only causal ones, so editing a characteristic for an unrelated reason silently deleted every symmetric edge pointing at it. Latent until this package authored the first ones. Fixed, with a regression test verified red against the old code. | closed |
| `X10` | ~~The DAG draws `Causes` only.~~ **CLOSED 2026-07-27.** Symmetric partners of the FOCUS are admitted to rank 0, above the focus so the detection lane below stays clear (the layout test caught `mLive->focus crosses rival` when they went below). They draw with no arrowhead — an arrow would assert a direction they do not have — and an exclusion carries no strength word. Scoped to the focus: a corroborates edge between two of its causes is a fact about those two, not about the thing being read. Dashed lines are gone from the whole view: colour separates the four kinds and a legend names them, listing only the kinds present. | closed |
| `X11` | **`ConditionState` gates nothing but `Retired` / `Superseded`, and that is a decision nobody has taken deliberately.** `detect()` now skips those two (follow-up 7) because they mean *withdrawn*, but `Draft`, `Candidate` and `NeedsRevalidation` all detect identically to `Active` — and **62 of 112 shipped conditions are `Draft`**. That is almost certainly right for now (gating on Draft would dark most of the library), but it means the state field is editorial metadata that changes nothing a golfer sees. If drafts should be held back from a real diagnosis, the answer is promoting them, not gating them. | open — for Mark |
| `X12` | **25 `noCause` warnings remain**, down from 34. Every firing condition now has one; the rest sit on measures with no producer, so they are a content backlog rather than a shipped defect — `core_pack_test` draws exactly that line. The wider ones worth doing next: the ball-flight outcomes that terminate no chain (`carry_deficit`, `smash_deficit`, `spin_deficit`), and the three `singleTailAxis` rows (`lumbar_curve`, `thoracic_curve`, `address_hip_hinge`) where only one side of the range is authored. | open |
| `X13` | **Cause concentration is at its floor.** `core_pack_test` asserts the five dominant causes account for ≥30 % of all swing causal edges; after follow-up 7 that is **40 of 130, against a threshold of 39**. The next batch of edges authored from anywhere OTHER than those five will break it. That is the check working as designed — it is what stops the library becoming a restated fault list — but the next author will meet it, and the answer is to ask whether the new fault really has a private cause, not to move the threshold. | open |
| `X5` | ★ citation backlog: every seeded norm is `heuristic`, `n = 0`. Conditions whose direction/phase is well supported in the literature (kinematic sequence order, X-factor stretch, early extension, reverse spine ↔ low-back load) are candidates for `supported` **only** with a verifiable DOI/PMID. | open |
