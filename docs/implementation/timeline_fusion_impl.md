# Timeline fusion V1 — the corpus gate

Run 2026-08-19 on GOLFSIMPC against `ff27b06`, the V1 implementation of
`docs/design/timeline-fusion.md`. Gates 1–5 are **green**; gate 6's two
measurable halves are green and its human half is outstanding. The evidence
below is what the default flip should cite.

---

## 1. What was run

| | |
|---|---|
| Host | GOLFSIMPC (the Mac has no pose models — a re-analysis there halts at 2 ms with "no pose data") |
| Build | Release, `build/Release-Installer` (VS 18 / jom, `PINPOINT_BUILD_TOOLS=ON`), DLL set + `models/vitpose-b-wholebody.onnx` colocated with the exe |
| Baseline binary | `7e62369` — the last pre-fusion commit, kept as `swinglab_run_base.exe` |
| Fusion binary | `ff27b06` — `swinglab_run.exe` |
| Driver | `stagegate/parity_run.py` (session-disambiguated run dirs; `lab.py run` flattens by swing name and collides across sessions) |
| Workspace | `C:\PinPointStudio\fusiongate` |

**Populations.** The rebuilt manifest counts 108, so membership is selected
explicitly rather than inherited (`fusiongate/fusion_manifest.py`):

- `pop-61` — the 61 swings frozen in the `stagegate/pose2/` cache. That cache is
  what "the 61-swing corpus" has always operationally meant. All camera-only.
- `pop-0818` — the eleven 2026-08-18 wG3 swings, the only both-witness
  population, all carrying a full P1–P10 markup.

**Runs.** Every 61-swing run injects the frozen `pose2/` pose so CUDA
nondeterminism cannot reach the diff.

| id | binary | `refine.fusion` | population | pose | wall |
|---|---|---|---|---|---|
| A | baseline | (absent) | 61 | pinned | 334 s |
| B | fusion | false (default) | 61 | pinned | 330 s |
| C | fusion | true | 61 | pinned | 329 s |
| D | fusion | false | 11 | live | 52 s |
| E | fusion | true | 11 | live | 48 s |
| F | fusion | false | 11 | pinned | 30 s |
| G | fusion | true | 11 | pinned | 30 s |

0 runner failures in all seven runs.

---

## 2. The gate ladder

| # | Gate | Result |
|---|---|---|
| 1 | Unit — `timeline_fusion_test`, 12 blocks | **PASS**, 0 failures. Decision table, both guards, the Measured-only dispute cap, the `Wrist_01/0003`-shaped degenerate, the Fallback-P10 ladder, all-abstain byte identity, camera-only parity with `emitPositionsLadder`, dark defaults |
| 2 | OFF parity — A vs B, `parity_diff.py` | **PASS**, 61 compared, **61 byte-identical**, 0 unpaired |
| 3 | ON, camera-only — B vs C | **PASS**, 0 phase events moved, 0 residual diffs; the only changes are the two intended additive ones |
| 4 | ON, both-sources — F vs G vs truth | **PASS** on every stated condition (§4 below) |
| 5 | Coverage — `lab.py coverage` pre/post | **PASS**, RESOLVED identical on both populations (2921 on the 61, 542 on the eleven); no measure regressed |
| 6 | Eyeball | **PARTIAL** — stations/chips and the replay trim measured (§6); "nothing visually absurd" still needs a human |

### Gate 2 — the parity baseline holds

`refine.fusion=false` on the fusion binary reproduces the pre-fusion binary
byte-for-byte across all 61 swings. This is the claim the whole rollout rests on
and it is unqualified: the `TimingClass` plumbing, the new stage, and the
serialization changes are all inert when the flag is dark.

It is also what the two design deviations were for. `phases[].timing` is written
only at `segmentation.version >= 5`, and `ShaftPosition::timing` is never
serialized; had either been unconditional, every phase of every swing would have
gained a key and this row would read FAIL.

### Gate 3 — camera-only is exactly the present ladder, plus an audit trail

`parity_diff.py` stops at the first differing leaf, which is always the additive
`phases[].timing`, so the comparison was redone structurally
(`fusiongate/fusion_diff.py`), enumerating *every* differing leaf with the three
intended additions set aside:

```
61 pairs compared
segmentation.version:  4 -> 5   x61
phase events whose t_us MOVED: 0
residual diffs outside {phases[].timing, segmentation.fusion, segmentation.version}: 0
```

Per-slot, the arbiter's own audit trail over the 61:

| slot | outcome | n | median &#124;Δ&#124; |
|---|---|---|---|
| P2 | Inserted | 61 | 0 |
| P3 | Inserted | 61 | 0 |
| P4 | TieHeld | 60 | 0 |
| P4 | ClassHeld | 1 | 0 |
| P5 | Inserted | 59 | 0 |
| P6 | Inserted | 59 | 0 |
| P7 | AnchorHeld | 61 | 0 |
| P8 | Inserted | 61 | 0 |
| P10 | TieHeld | 53 | 0 |
| P10 | ClassHeld | 8 | 0 |

301 insertions — the same five interior rungs the positions ladder emits — and
every anchor slot decided at Δ 0, because the club positions and the vision
ladder are born from the same phase-model frames. The 9 `ClassHeld` are club
landmarks whose frame sat on a coast/predict sample, so the candidate was
`Proxy` against a `Measured` incumbent; they were at Δ 0 anyway, so the class
rule cost nothing and the outcome is unchanged either way.

**Deviation from §8.** The design specifies live pose for gate 3. It was run
pose-pinned instead: a same-binary A/B is *more* rigorous with the pose frozen,
because CUDA jitter is removed from the comparison rather than dressed up as a
fusion effect — and §5 below shows exactly what that jitter looks like when it
is left in. Gate 2 is unaffected (it was always pinned).

**§8 said gate 3 should be "byte-identical, or every diff explained".
Byte-identity is unreachable by construction** — `version` 4→5 and the additive
`segmentation.fusion[]` are both intended products of a fusion pass. Every
substantive field is identical, which is the claim that matters.

---

## 3. Gate 4 — the truth-graded eleven

Signed error vs the hand markup, in ms, positive = published later than truth
(`score.py`'s P_CHECKS convention). `fusiongate/fusion_grade.py`, runs F vs G.

### `Wrist_02` — hi-res, the authoritative stratum (6 swings)

| | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 | P10 |
|---|---|---|---|---|---|---|---|---|---|
| median OFF | +32 | −9 | −21 | −18 | +21 | **−39** | +2 | **+96** | **+1686** |
| median ON | +32 | −9 | −21 | −18 | +21 | **+6** | +2 | **+7** | **−0** |
| worst OFF | 183 | 12 | 43 | 54 | 23 | 42 | 4 | 108 | 1697 |
| worst ON | 183 | 12 | 43 | 54 | 23 | **8** | 4 | **12** | **54** |

### `Wrist_01` — low-rate, the stress stratum (5 swings)

| | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 | P10 |
|---|---|---|---|---|---|---|---|---|---|
| median OFF | −7 | −12 | −7 | −1 | +19 | **−39** | +2 | **+99** | **+1280** |
| median ON | −7 | −12 | −7 | −1 | +19 | **+4** | +2 | **+5** | **−54** |
| worst OFF | 153 | 15 | 21 | 29 | 22 | 41 | 6 | **667** | 1491 |
| worst ON | 153 | 15 | 21 | 29 | 22 | 24 | 6 | **15** | 1491 |

Every median the design predicted from `swing.json` alone is reproduced by the
running arbiter, to the millisecond: club P6 +6 (design +6), club P8 +7 (design
+7), club P10 −0 (design −0).

### Pass conditions

```
p6  <= 40 ms on >= 10/11 : 11/11 PASS
p8  <= 50 ms on >= 10/11 : 11/11 PASS
p10 <= 120 ms on >= 10/11: 10/11 PASS   miss: Wrist_01/0003
retained slots within baseline ±5 ms:
  p1 0 ms · p2 0 ms · p3 0 ms · p4 0 ms · p5 0 ms · p7 0 ms   (p9 not emitted)
non-monotone ladders: 0 PASS
```

The retained slots did not move by ±5 ms; they moved by **exactly 0 ms** on all
eleven. Fusion touches the three slots it claims to touch and nothing else.

### `Wrist_01/0003`, the degenerate — both of its lessons landed

The swing that rewrote the dispute cap behaves as the design argued it must:

- **P8: −667 ms → +1 ms.** The club P8 sits 669 ms from the IMU proxy and won
  anyway, because the incumbent was `Proxy` and a proxy gets no cap. The
  cap-everything draft would have preserved the 667 ms error. Recorded as
  `OwnerBeat` — the club's own P8 was `Proxy` here too (its crossing landed on a
  predicted sample), so the classes tied and *ownership* decided it: the
  instrument that can see the shaft wins the shaft-defined slot. That is the
  Proxy-vs-Proxy path, firing on real data.
- **P4: retained at the IMU's value** (truth −1 ms) even though the club's P4 is
  167 ms out. `OwnerHeld`, Δ +166 ms logged rather than discarded.

**The one miss is honest.** P10 stays at +1491 ms on this swing because
`locatePTimes` emitted no club P10 at all — its positions run `1,2,3,4,5,6,7,8`,
since P10 is only emitted when it is genuinely last in time and this swing's P8
follow-through crossing falls after `fin0`. With no candidate there is nothing
to arbitrate, so the window-edge clamp keeps the slot — and now carries
`timing = Fallback` in the file, so a consumer can see it is not a measurement.
That is a *detector recall* gap in `shaft_positions.h`, not a fusion failure,
and it is the same class of finding as the P6 no-recall residual tracked
separately. Gate 4 passes as specified (≥10/11).

### Per-slot decisions across the eleven

| slot | outcome | n | median &#124;Δ&#124; ms | design's Part I figure |
|---|---|---|---|---|
| P2 | Inserted | 11 | 0 | 0 (exact by construction) |
| P3 | OwnerHeld | 11 | 15 | +15 |
| P4 | OwnerHeld | 11 | 4 | −3 |
| P5 | OwnerHeld | 10 | 6 | +3.5…+6.9 |
| P5 | ClassHeld | 1 | 5 | — |
| P6 | ClassBeat | 10 | **44** | **−44** |
| P6 | OwnerBeat | 1 | 61 | — |
| P7 | AnchorHeld | 11 | 2 | +2 |
| P8 | ClassBeat | 10 | **92** | **+91** |
| P8 | OwnerBeat | 1 | 669 | −669 |
| P10 | ClassBeat | 10 | **1678** | **+1672** |

The right-hand column was measured from persisted `swing.json` before any of
this code existed. The arbiter reproduces it from an independent path.

---

## 4. Gate 5 — coverage

```
61-swing corpus   OFF: RESOLVED=2921 BLOCKED_PHASE=79 BLOCKED_METRIC=3405 NO_LM=1525
                  ON : RESOLVED=2921 BLOCKED_PHASE=79 BLOCKED_METRIC=3405 NO_LM=1525
eleven 08-18      OFF: RESOLVED=542  BLOCKED_PHASE=11 BLOCKED_METRIC=602  NO_LM=275
                  ON : RESOLVED=542  BLOCKED_PHASE=11 BLOCKED_METRIC=602  NO_LM=275
```

No measure regressed RESOLVED→BLOCKED. **The hoped-for bonus did not
materialise**, and that is worth recording rather than quietly omitting: §6
suggested un-collapsing P5→P6 might unblock windowed measures, but the phase
blocker on these swings is `transition` (11/11), not P5 or P6, so widening the
P5→P6 interval had nothing to unblock. The coverage bottleneck is elsewhere.

---

## 5. Blast radius — measured, not predicted

Runs F vs G (pose pinned), every differing leaf, index-stripped
(`fusiongate/fusion_keys.py`):

```
 153  analysis.metrics[].phaseSamples[].t_us
 153  analysis.metrics[].phaseSamples[].value
  32  analysis.phases[].t_us
  32  analysis.phases[].conf
  32  analysis.phases[].segment
  11  analysis.assessment.findings[].confidence
  11  analysis.assessment.findings[].magnitudeDeg
  10  analysis.assessment.findings[].lowConfidence
```

Every one of those is on §6's "changes, and is supposed to change" list, and the
32 phase moves are exactly the 30 `ClassBeat` + 2 `OwnerBeat` decisions.

Nothing on the "does not change" list moved. In particular there are **no
`metrics[].value[]` diffs** — the metric curves are untouched; only the labelled
dots at the moved rungs re-sample. No `analysis.score`, no resemblance, no score
uncertainty, no `segmentation.swingStartUs/swingEndUs`, no tempo, no `club.*`.

The assessment finding moved the way you would want: `magnitudeDeg` 59.5° →
45.1° and `lowConfidence` **true → false** on ten of eleven. It had been
sampling the wrong instant.

**A caution for the next gate.** The first pass of this comparison used live
pose on both arms and showed extra diffs in `club.plane.*` and `club.lengths.*`
— fields fusion cannot touch, since it runs after the shaft track is finished.
Those were CUDA pose jitter at the 1e-10 level, and they vanished entirely once
the pose was pinned. Any A/B on this pipeline that is not pose-pinned will
manufacture diffs that look causal and are not.

---

## 6. Gate 6 — the two halves that can be measured

The eyeball gate asks three things. Two are measurable from `result.json` and
were measured (`fusiongate/fusion_effect.py`, all eleven):

- **Stations converge on chips.** The worst gap between a `phases[]` station and
  its `club.positions[]` chip at P6/P8/P10 goes from **median 1667 ms / max 1747
  ms → 0 ms on every swing**. `PpTransitTimeline.qml` draws those two from
  independent sources; they now agree exactly.
- **The replay ends at the real finish.** `disk_replay_source` trims on the
  Finish event, which moves from ~3.97–4.49 s to ~2.63–2.82 s — 1.3–1.7 s
  earlier, at the club's measured finish. `Wrist_01/0003` correctly stays put
  (no club P10 candidate).
- **The P5→P6 collapse is undone.** 12–15 ms → 53–74 ms on every swing (§6
  predicted ~14 → ~64).

**Outstanding: "nothing visually absurd" needs a human** on one 08-18 swing in
the review UI. That is the only gate row this run cannot close.

---

## 7. Verdict

Gates 1–5 green, gate 6 green on everything not requiring eyes. Every V1 claim
in the design is confirmed on truth-graded data in both strata, the parity
baseline is unqualified, and the blast radius is exactly the predicted set.

Recommended: flip `tuned::refine::kFusion` to `true` in its own commit citing
this document, after the gate-6 eyeball.

Two follow-ups this gate surfaced, neither blocking:

1. **Club P10 recall** — `locatePTimes` omits P10 whenever the P8 crossing falls
   after `fin0`, which cost the flip on `Wrist_01/0003`. A detector finding for
   `shaft_positions.h`, tracked separately.
2. **`refine.fusionDisputeMs` is still untested by data.** It never bound: every
   V1 replacement displaced a `Proxy` or a `Fallback`, which are uncapped by
   design. Its first real user is P1 arbitration in Phase 2, and the 300 ms
   default should be calibrated then rather than assumed now.
