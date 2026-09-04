# Diagnostics dataset — gap analysis and extension brief

Target files: `src/Resources/diagnostics/core.json`, `norms.json`, `contexts.json`.
Schema authority: `src/Diagnostics/characteristic.h`, `measure_facets.h`, `anatomy_vocabulary.h`, and the validation gates in `characteristic_pack.cpp`. Run the diagnostics test suite (`core_pack_test`, `diagnostics_catalogue_integrity_test`, `norm_pack_test`) after every edit — the loader rejects unit mismatches, cycles, corroborates-shadowing-causes, single-tail axes, and unknown tokens.

Conventions for all additions:

- **Provenance discipline.** Every new condition ships `"provenance": {"tier": "proposed"}` unless a real DOI/PMID is attached. NEVER cite a commercial organisation, product, certification body, or brand (no launch-monitor vendors, no screening franchises). Domain terms are common property; attributions are not.
- **Norm discipline.** Seed values below are heuristic anchors, marked `"source": "heuristic"`. Do not invent citations. Where a peer-reviewed value is later found, update `mu`/`sigma` and the source together.
- **Launch-monitor flag.** Measures that cannot be resolved by this product's sensors (two/three global-shutter cameras + BLE IMU; skeleton, shaft, ball vocabulary) are authored with `"status": "notCapturable"` and a `gapReason` beginning `"Requires launch monitor: …"`. This is the flag mechanism — it keeps the roadmap honest (per the `MeasureStatus` header comment) while letting the fault library reference the measure. Grep `Requires launch monitor` to enumerate them.
- **`highMeans` is mandatory** on every new measure — three signals have already shipped inverted without it (see the comment block in `characteristic.h` and `docs/design/pinpoint_sign_conventions.md`).
- **Sign conventions** must follow `docs/design/pinpoint_sign_conventions.md`; do not restate them per-measure.

---

## 1. Hygiene defects in the current pack (fix before extending)

1. **23 measures have no norm row in any context** — every corridor signal over them is dead: `m_axisTiltAtTop, m_ballBodyGap, m_clubPathAtImpact, m_leadArmToTorso, m_leadHandWidth, m_leadKneeFlex, m_leadWristAtTop, m_lumbarCurve, m_pelvisLiftTop, m_pelvisRotPeak, m_pelvisSwayBack, m_pelvisSwayDown, m_pelvisSwayImpact, m_pelvisThrustDown, m_shoulderAlignment, m_shoulderPlane, m_spineBendAtAddress, m_spineBendLoss, m_thoracicCurve, m_thoraxDrift, m_thoraxRotPeak, m_trailElbowRise, m_xFactorStretch`. Seed norms are given in §5.
2. **`m_leadWristAtTop` is an orphan** — no signal references it and it has no norm. Either wire it into a `sig_cuppedAtTop` / `sig_bowedAtTop` pair (see §3.2, `across_the_line` context) or delete it; `m_leadWristFlexExt_p4` already covers the reading.
3. **Unit hygiene:** many measures carry `"unit": "-"` (e.g. `m_lagAngleDown`, `m_tempoRatio`, all `planned` measures). The norm loader hard-fails on unit mismatch; give every measure its real unit (`°`, `ratio`, `% stance width`, `cm`, `mph`) before authoring its norm.
4. **`alignment_open` / `alignment_closed` use shoulders only**… — **RESOLVED 2026-08-02, and the premise was wrong.** `feetAlignment` and `hipAlignment` producers did NOT already exist; both were `.planned`. All three measures now exist and are live, but `hipAlignment` and `shoulderAlignment` were RETIRED as metrics: each was geometrically identical to a body-line tilt already in the catalogue (`hipLineTilt`, `shoulderPlaneAngle`) read at a different phase, which `metric_reducer.h` exists to express. `m_hipAlignment` and `m_shoulderAlignment` now point at the surviving series, restated in ITS sign convention. `feetAlignment` survived as its own metric — the ankle line genuinely is not the toe line, and it fixes `toeLineAngle`'s mirror-inverting sign. Note that a face-on camera reads the APPARENT line, not true target-line alignment; every descriptor says so.
5. **No `corroborates` or `excludes` edges exist** (all 81 edges are `causes`), although the schema, cycle-detection and shadowing gates were built for them. §6 lists the first tranche.
6. **`drills` is empty on all 50 conditions.** The stated purpose of the rules is to drive the "so what" conversation; every observable fault needs at least one drill id. Establish a `drill.*` namespace now (like `screen.*`) even if drill content lands later.
7. **All 50 conditions are `tier: proposed`.** Create a research backlog: conditions whose direction/phase is well supported in the biomechanics literature (kinematic sequence order, X-factor stretch, early extension, reverse spine ↔ low-back load) should be upgraded to `supported` with a DOI as citations are verified. Do not upgrade without a verifiable DOI/PMID.
8. **Latent screened conditions have `screenRef` but no screen protocol or norm anywhere.** §7 defines them generically (clinical ROM literature only — no branded screening protocols).
9. **metricKey drift** — **RESOLVED 2026-08-02, and this item is why it went smoothly.** Six of the ten now have producers (`shoulderPlaneAngle`, `thoraxLateralDrift`, `trailElbowHeight`, `leadArmToTorso`, `leadHandWidth`, `trailWristFlexExt`) and the join did NOT silently miss — because `upper_body_metrics.cpp` was built to the geometry the `series` facets already named (`shoulderLine · angle · ground`, `thoraxCentre · distance · trailAnkle`, `trailElbow · height · shoulderLine`, …). **The facet triple was the producer specification**, authored before the producer, and treating it that way is what made the ids match. The remaining four are intentional: `thoracicFlexion` / `lumbarExtension` have no keypoint in either pose layout, `ballBodyDistance` is depth, `leadKneeFlexion` is sagittal.

---

## 2. Schema changes required (small, code-side)

1. **`ConditionGroup` needs three new members:** `Impact`, `BallFlight`, `Finish`. The current enum (`Setup, Posture, Lateral, ArmsAndClub, Release, Sequence`) cannot classify impact-quality faults, ball-flight outcome conditions, or finish/balance faults. Update `characteristic_pack.cpp` token tables, the group ordering in the GUI characteristics screens, and tests.
2. **Ball-flight outcomes are conditions, not text.** Today "consequence" is narrative only. Model outcomes (`start_left`, `launch_low`, `curve_left`, `ball_speed_deficit`, `strike_fat`, `strike_thin`…) as ordinary conditions in group `BallFlight`, detected by ball-track signals where capturable, with `causes` edges **from** swing faults **to** outcomes. This is what makes explanation top-down actually terminate at what the golfer sees, and lets the ball track corroborate skeletal/shaft findings. No new node type needed — this is the whole point of "faults and causes are the same type".
3. **Verify `Phase` token coverage** for `p2, p5, p8, p9, finish` in `phaseToken`/`phaseFromToken` — the new measures below use all of them.
4. **Add `QStringList aliases` to `Condition`** (mirror of the existing `Measure::aliases` design: "coach phrasing that resolved here; grows with use"). Coaches say *flip*, *early release*, *reverse pivot*, *standing up*; golfers search by those words. Aliases feed search/resolution in the characteristics screens AND a glossary rendering ("Scooping — also called *flipping*: …"), which is the education surface. Parse/serialise in `characteristic_pack.cpp`; include aliases in the duplicate-label lint so two conditions can't claim one term.
5. **Asserted-outcome intake ("what's your bad shot?").** The coach/golfer conversation starts at the outcome, so outcomes must be enterable before they are measurable. Convention, no schema change needed: an outcome condition whose detection is camera-capturable is `confirmedBy: measured`; one that is LM-gated ships v1 as `confirmedBy: asserted` (the golfer reports "I slice it") and flips to `measured` when an LM integration lands. The explanation pass then runs top-down from the asserted outcome and ranks its upstream `causes` chain — that is the reverse-diagnosis trace the intake needs, and it already works because outcomes are ordinary conditions. The intake UI is simply a picker over `group == ballFlight` conditions ordered by alias familiarity.
6. **Intent contexts for shaped shots.** Add `intent_draw`, `intent_fade`, `intent_straight` (default) under `full_swing` in `contexts.json`, exactly parallel to `archetype_bowed`/`archetype_cupped`: they shift the *corridor centre* for `m_launchDirection`, `m_clubPathAtImpact`, `m_faceToPath` and curvature measures. This is how *draw* differs from *hook* without a valence field: a draw is curvature inside the intended corridor, a hook is the same sign of curvature outside it. Context moves the corridor; the sign of the finding never flips — consistent with the "no valence" rule in `characteristic.h`.

---

## 3. Missing conditions

Legend: **src** = skeleton / shaft / ball / LM(launch monitor) / screen. All new conditions: `state: draft`, `tier: proposed` until reviewed.

### 3.1 Setup (group `setup`)

| id | label | detection | src | notes |
|---|---|---|---|---|
| `feet_alignment_open` / `feet_alignment_closed` | Feet aim left/right of target | corridor on `m_feetAlignment` | skeleton | producer `feetAlignment` exists; prefer `corroborates` with shoulder alignment (see §1.4) |
| `hip_alignment_open` / `hip_alignment_closed` | Hips open/closed at address | corridor on `m_hipAlignment` | skeleton | producer exists |
| `excessive_knee_flex` / `insufficient_knee_flex` | Sitting too deep / legs too straight at address | corridor on `m_kneeFlexAddress` | skeleton | pairs with `posture_too_upright` axis |
| `grip_strong` / `grip_weak` | Grip rotated strong/weak | — | screen/asserted | hand keypoints (133 layout) are too noisy on a gripped club; author as `confirmedBy: asserted`, `observability: latent`. Causes edges → face-related outcomes |
| `weight_trail_at_address` | Static weight bias toward trail foot | — | notCapturable | gapReason: "Requires force/pressure measurement; no camera proxy at address." Latent cause of `hanging_back` |

### 3.2 Backswing (groups `posture` / `armsAndClub` as fits)

| id | label | detection | src | notes |
|---|---|---|---|---|
| `inside_takeaway` / `outside_takeaway` | Club dragged inside / pushed outside in takeaway | corridor on `m_shaftDirectionP2` (DTL) | shaft | classic root cause of over_the_top (outside→re-route) and stuck (inside). Add `causes` edges accordingly |
| `early_face_roll` | Face rolled open in takeaway | corridor high on `m_leadForearmRot_p2` | skeleton | measure already **live** — zero pipeline cost |
| `steep_backswing_plane` / `flat_backswing_plane` | Shaft above/below address plane P2–P4 | corridor on `m_shaftPlaneBackswing` | shaft | `swingPlane` producer exists in catalogue |
| `across_the_line` / `laid_off` | Clubhead points right/left of target at P4 | corridor on `m_shaftDirectionP4` (DTL) | shaft | canonical top-of-swing pair; causes edges → over_the_top (laid-off→stuck, across→OTT) |
| `overswing` | Shaft past parallel at P4 | corridor high on `m_shaftAngleP4` | shaft | causes: `limited_wrist_mobility` reversal (collapse), `bent_lead_arm` |
| `bent_lead_arm` | Lead elbow bent at top | corridor low on `m_leadElbowFlex_p4` | skeleton | measure already **live**; norm missing. Causes → `loss_of_width`, `casting` |
| `trail_knee_straighten` | Trail knee loses flex P1→P4 | delta reducer on `m_trailKneeFlex` p1→p4 | skeleton | classic companion of `sway` / `trail_hip_hike`; `corroborates` sway |
| `head_drop_backswing` / `head_rise_backswing` | Head drops/rises in backswing | corridor on `m_headLift` (Δ p1→p4) | skeleton | producer `headLift` exists |
| `excessive_head_sway` | Head moves off the ball laterally | corridor on `m_headSway` (Δ p1→p4) | skeleton | producer `headSway` exists; `corroborates` `sway` |
| `excessive_heel_lift` | Lead heel lifts excessively | corridor high on `m_leadHeelLift` at p4 | skeleton | producer exists; style-dependent — wide corridor, `material: false` binding under seniors/flexibility contexts later |
| `short_backswing` | Insufficient shoulder turn at P4 | corridor low on `m_thoraxRotP4` | skeleton | distinct from `xfactor_deficit` (separation vs absolute turn); caused by `limited_thoracic_rotation` |
| `disconnection` | Lead upper arm separates from the chest ("disconnected") | corridor high on `m_leadUpperArmToChest` (extremum p1→p4) | skeleton | aliases: *arms lifting*, *loss of connection*. Distinct from `flying_elbow` (trail side) and `chicken_wing` (post-impact): this is the lead-arm/torso gap in the backswing. Causes → `over_the_top`, `loss_of_width`; `corroborates` `flying_elbow` |

### 3.3 Transition / downswing (groups `sequence`, `armsAndClub`, `lateral`)

| id | label | detection | src | notes |
|---|---|---|---|---|
| `steep_downswing_shaft` | Shaft steeper than address plane at P5–P6 | corridor high on `m_shaftPlaneDelivery` | shaft | the shaft-domain twin of `over_the_top` (which is path-domain at impact). `corroborates` over_the_top |
| `under_plane_stuck` | Shaft trapped under plane, path excessively in-to-out | corridor low on `m_shaftPlaneDelivery`; corridor high on `m_clubPathAtImpact` | shaft | opposite tail of the same axis; causes → block/hook outcomes |
| `hip_spin_out` | Pelvis rotation opens early without lateral shift | ratio/order of `m_pelvisRotP5` vs `m_pelvisSwayDown` | skeleton | causes → `under_plane_stuck`, `hanging_back` |
| `hip_stall` | Pelvis rotation stalls P6→P7 | rate reducer low on `m_pelvisRotRateP6P7` | skeleton | canonical cause of `scooping`/flip; `causes` edge → scooping |
| `deceleration` | Hand/club speed drops before impact | rate on `m_handSpeedP6P7` | skeleton | producer `handSpeed` exists; clubhead-speed version is shaft/LM (see §4) |

### 3.4 Impact (new group `impact`)

| id | label | detection | src | notes |
|---|---|---|---|---|
| `insufficient_shaft_lean` | Shaft not leaning forward at impact (irons) | corridor low on `m_impactShaftLean` | shaft | producer `impactShaftLean` **exists**; `corroborates` `scooping` (wrist view of same event) |
| `low_point_behind_ball` | Swing arc bottoms out behind the ball | corridor low on `m_lowPointAhead` | shaft+ball | producer `lowPointAhead` exists; causes → `strike_fat`, `strike_thin` |
| `attack_too_steep` / `attack_too_shallow` | Angle of attack outside club corridor | corridor on `m_attackAngle` | shaft (LM preferred) | camera-derived from club track near impact — flag accuracy; norms are strongly club-contextual (§5) |
| `open_face_to_path` / `closed_face_to_path` | Face open/closed relative to path at impact | corridor on `m_faceToPath` | **LM** | face orientation at impact is not resolvable at 150 fps with motion blur; `notCapturable`, gapReason "Requires launch monitor: face angle at impact (sub-ms event, face not tracked)". Wrist measures (`m_leadWristAtImpact`) `corroborates` only |
| `hips_closed_at_impact` | Pelvis under-rotated at P7 | corridor low on `m_pelvisRotP7` | skeleton | producer `pelvisRotation` exists |
| `insufficient_axis_tilt_impact` / `excessive_axis_tilt_impact` | Secondary axis tilt outside corridor at P7 | corridor on `m_axisTiltImpact` | skeleton | producer `secondaryAxisTilt` exists; driver vs iron corridors differ sharply |

### 3.5 Finish (new group `finish`)

| id | label | detection | src |
|---|---|---|---|
| `off_balance_finish` | Cannot hold finish; COM outside lead-foot base at P10 | corridor on `m_comOverLeadFootFinish` | skeleton |
| ~~`weight_back_at_finish`~~ | Weight remains on trail side at finish | ~~corridor on `m_pelvisSwayFinish`~~ — **detector REMOVED 2026-09-04**, condition now `confirmedBy: asserted` with no signal | skeleton |
| `abbreviated_finish` | Follow-through cut short | corridor low on `m_thoraxRotFinish` | skeleton |

### 3.6 Ball-flight and strike outcomes (new group `ballFlight`; see §2.2, §2.5, §2.6)

This is the intake vocabulary — the answer to "what's your current bad shot?" — so the **coach term is the label**, and the `consequence` text carries the plain-language definition (that field is the education surface for outcomes: what the term means, and what at impact produces it). All directions below are written for a right-handed golfer; author with a handedness transform, never duplicate mirrored conditions.

The taxonomy is the nine-flight model: **start line** (relative to target line — dominated by face angle) × **curvature** (dominated by face-to-path). Start line is camera-capturable from the ball track; curvature is LM-gated. That split decides `confirmedBy` per §2.5.

**Shot-shape outcomes** (axis: `start_line` and `curvature`):

| id | label (coach term) | definition (→ consequence text) | detection | src | confirmedBy v1 |
|---|---|---|---|---|---|
| `pull` | Pull | Starts left of target, flies straight. Face and path both left at impact. | corridor low on `m_launchDirection`, curvature ≈ 0 | ball (+LM for curvature) | measured (start), asserted (no-curve qualifier) |
| `push` | Push | Starts right of target, flies straight. Face and path both right. | corridor high on `m_launchDirection` | ball | measured |
| `block` | Block | A push, usually from swinging too much in-to-out and the face matching the path — the ball never comes back. | as `push`, with `under_plane_stuck` upstream | ball | measured |
| `slice` | Slice | Curves hard right in flight — face open to the path. The playable version is a *fade*. | corridor on `m_spinAxis` / `m_faceToPath` (high) | **LM** | asserted |
| `hook` | Hook | Curves hard left — face closed to the path. The playable version is a *draw*. Severe: *snap hook*, *duck hook*. | corridor on `m_spinAxis` / `m_faceToPath` (low) | **LM** | asserted |
| `pull_hook` | Pull-hook | Starts left AND curves left — the "smother". Face closed to both target and path. | start low + curvature low | ball+**LM** | asserted |
| `push_slice` | Push-slice | Starts right and curves further right; nothing at impact pointed at the target. | start high + curvature high | ball+**LM** | asserted |
| `launch_low` / `launch_high` | Low ball flight / Ballooning | Launch angle outside the club's corridor. Ballooning wedges/drivers cost carry. | corridor on `m_launchAngle` (club-contextual) | ball | measured |
| `ball_speed_deficit` | Lost distance | Ball speed below the athlete/club corridor. | corridor low on `m_ballSpeed` | ball | measured |

*Draw* and *fade* are deliberately **not** conditions: they are intended shapes — curvature inside the golfer's intended corridor. They live as the `intent_draw` / `intent_fade` contexts (§2.6) and in the glossary (§3.8): "a draw is a hook you meant; the difference is a corridor, not a sign."

**Strike outcomes** (axis: `strike_quality` / `strike_location`):

| id | label (coach term) | definition (→ consequence text) | detection | src | confirmedBy v1 |
|---|---|---|---|---|---|
| `chunk` | Chunk (fat, heavy, "laying the sod over it") | Ground struck before the ball; huge speed loss, ball comes up short. | `m_lowPointAhead` low + `m_ballSpeed` collapse + turf-strike launch anomaly | ball+shaft | measured |
| `thin` | Thin (blade, skull) | Leading edge into the ball's equator; screaming low flight, near-normal speed, no spin/height. | `m_lowPointAhead` low/borderline + `m_launchAngle` very low + speed near-normal | ball+shaft | measured |
| `top` | Top | Club bottoms out above the equator; ball dribbles forward. | `m_launchAngle` ≈ 0, `m_ballSpeed` collapse | ball | measured |
| `sky` | Sky (pop-up) | Teed ball struck high on the crown from a steep attack; very high, very short. Context: driver/tee only (applicability binding). | `m_launchAngle` extreme high + speed deficit + `attack_too_steep` upstream | ball | measured |
| `shank` | Shank (hosel rocket) | Struck off the hosel; ball shoots sharply right and low. The most feared miss. | `m_launchDirection` extreme high + speed collapse; body proxy: standoff loss (`m_ballBodyGap` Δ p1→p7 shrinking) | ball (+skeleton proxy); face location itself **LM** | measured (pattern), asserted (confirmed hosel strike) |
| `strike_heel` / `strike_toe` | Heel strike / Toe strike | Impact off-centre on the face; gear effect curves the ball and bleeds speed. | corridor on `m_strikeLocation` | **LM** | asserted |
| `smash_deficit` | Poor strike efficiency | Ball speed low for the delivered clubhead speed — energy lost to off-centre contact. | corridor low on `m_smashFactor` | **LM** | asserted |
| `spin_excess` / `spin_deficit` | Spinny / Knuckleball | Spin rate outside the club corridor; costs distance or control. | corridor on `m_spinRate` | **LM** | asserted |
| `carry_deficit` | Short carry | Carry below the athlete/club corridor. | corridor low on `m_carryDistance` | **LM** | asserted |

**Causal edges to author with the outcomes** (first tranche, right-handed):

- `over_the_top → pull` (face square to path) and `over_the_top → slice` (face open to path — the classic beginner pairing); `outside_takeaway → over_the_top` already chains in.
- `under_plane_stuck → block`, `→ hook` (face closing hard to save it), `→ push_slice` (face left open).
- `open_face_to_path → slice`, `→ push`; `closed_face_to_path → hook`, `→ pull`; `grip_weak → open_face_to_path`; `grip_strong → closed_face_to_path`.
- `casting / scooping → thin`, `→ launch_high`, `→ ball_speed_deficit`; `hip_stall → scooping` already chains to these.
- `low_point_behind_ball → chunk`, `→ thin` (the same low-point error, ±2 cm apart); `hanging_back → low_point_behind_ball`; `sway → low_point_behind_ball`.
- `early_extension → shank` (body toward ball closes standoff), `→ strike_heel` (LM), `→ under_plane_stuck`; `ball_too_close → shank`.
- `attack_too_steep → sky` (driver binding), `→ chunk`, `→ spin_excess`; `attack_too_shallow → thin`, `→ top`.
- `ball_forward → thin`, `→ pull` (face has time to close); `ball_back → push`, `→ launch_low`, `→ chunk`.
- `insufficient_shaft_lean → launch_high`, `→ ball_speed_deficit`; `deceleration → ball_speed_deficit`, `→ chunk`.
- `excludes`: `chunk ⊣ top`, `chunk ⊣ thin` (per swing), `pull ⊣ push`, `slice ⊣ hook`.

### 3.7 Coach-vernacular aliases for existing conditions (needs §2.4)

These are aliases, not new conditions — one concept, several names. Attach to the existing ids and render in the glossary:

| condition | aliases |
|---|---|
| `scooping` | flip, flipping, breakdown through impact, adding loft |
| `casting` | early release, throwing it from the top, throwaway, losing lag |
| `reverse_spine` | reverse pivot, leaning left at the top |
| `early_extension` | standing up, goat humping, hip thrust toward the ball |
| `loss_of_posture` | coming out of it, standing up through it |
| `over_the_top` | coming over it, out-to-in, casting over (colloquial), OTT |
| `under_plane_stuck` (new, §3.3) | stuck, trapped, dropping it too far inside |
| `hanging_back` | staying back, falling back, reverse weight shift |
| `chicken_wing` | lead-elbow breakdown, wing |
| `flying_elbow` | trail elbow flying, disconnected trail arm |
| `sway` | swaying off the ball, lateral slide back |
| `slide` | sliding (downswing), excessive lateral drive — NOTE: coaches also say "slide" for the ball-flight *slice* by mishearing; the alias resolver must keep `slide` (body) and `slice` (flight) distinct |
| `xfactor_deficit` | no separation, all-arms swing, low coil |
| `transition_rush` | quick from the top, snatching it |
| `loss_of_width` | narrow downswing, collapsing arms |
| `insufficient_set` | no wrist hinge, dead hands |
| `across_the_line` (new) | crossed at the top, pointing right |
| `laid_off` (new) | pointing left at the top |
| `steep_downswing_shaft` (new) | steep, chopping, out of position coming down |
| `disconnection` (new) | arms lifting, loss of connection, run-off |

### 3.8 Glossary rendering (education goal)

With `aliases` on conditions and definitions carried in outcome `consequence` text, the characteristics screens can render a **glossary view for free**: every condition, its coach terms, its one-paragraph plain-language meaning, and — because outcomes are in the same DAG — tappable "commonly caused by …" chains straight from the `causes` edges. No separate glossary dataset to maintain; the rule set *is* the glossary. Add this view to the settings screens alongside the DAG visual already planned.

---

## 4. Missing measures (author into `measures[]`)

All need `highMeans`, real units, and `viewNeeded`. Statuses: **live** = producer exists today; **planned** = producer feasible from current pipelines; **noProducer** = roadmap; **notCapturable** = flagged, with gapReason.

### Skeleton (pose)

| id | metricKey / facets | reducer | unit | status |
|---|---|---|---|---|
| `m_feetAlignment` | `feetAlignment` | at p1 | ° | live |
| `m_hipAlignment` | `hipAlignment` | at p1 | ° | live |
| `m_kneeFlexAddress` | lead/trail knee angle | at p1 | ° | planned |
| `m_trailKneeFlex` | trail knee flexion | Δ p1→p4 | ° | planned |
| `m_headLift` | `headLift` | Δ p1→p4 (backswing), Δ p1→p7 (impact variant) | cm | live |
| `m_headSway` | `headSway` | Δ p1→p4 | cm | live |
| `m_leadHeelLift` | `leadHeelLift` | at p4 | cm | live |
| `m_thoraxRotP4` | `thoraxRotation` | at p4 | ° | live/planned |
| `m_pelvisRotP5` / `m_pelvisRotP7` / `m_pelvisRotRateP6P7` | `pelvisRotation` | at p5 / at p7 / rate p6→p7 | ° , °/s | planned |
| `m_handSpeedP6P7` | `handSpeed` | rate/extremum p6→p7 | mph | live |
| `m_axisTiltImpact` | `secondaryAxisTilt` | at p7 | ° | planned |
| `m_comOverLeadFootFinish` | pelvis centre distance to lead ankle | at finish | cm | noProducer |
| ~~`m_pelvisSwayFinish`~~ | ~~`pelvisSway`~~ | ~~at finish~~ | — | **DELETED 2026-09-04** |
| `m_thoraxRotFinish` | `thoraxRotation` | at finish | ° | planned |
| `m_spineSideBendTop` | `spineSideBend` | at p4 | ° | planned (producer exists; corroborates reverse_spine) |
| `m_leadUpperArmToChest` | lead upper arm, distance to thorax centre | extremum p1→p4 | cm (or % torso length) | noProducer — drives `disconnection` |

> **Removed 2026-09-04 — `m_pelvisSwayFinish`, and the two signals that read it.**
> `pelvisSway` carries a **phase domain of P1–P7** (`MetricDescriptor::domain`, design
> `metric_presentation_honesty.md` §5.1): it is a lateral displacement read in the face-on image, and
> past impact the pelvis has turned far enough that the projection measures the **rotation**, not the
> translation the measure named. Reading it at the finish was a projection artefact graded against a
> heuristic corridor, so the measure, its `norms.json` row and both signals on it
> (`sig_weightBackFinish`, `sig_offBalanceFinishSway`) were deleted.
> It was **deleted rather than reclassified**: `notCapturable` is a statement about the METRIC, and
> `pelvisSway` is produced on every camera swing — `diagnostics_catalogue_integrity_test` refuses that
> classification for exactly this reason.
> `off_balance_finish` is unaffected; it keeps `sig_offBalanceFinish` on `m_comOverLeadFootFinish`, a
> distance **along** the stance line, which survives the turn and is the honest finish reading.
> `weight_back_at_finish` lost its only detector and is now `confirmedBy: asserted` with no signal —
> plainly visible, not measurable from our pixels, the same authoring as `thin`. The in-domain
> detector for the same physical fault is `sig_hangingBackPelvisDown` on `m_pelvisSwayDown`.

> **⚠ The `status` column in the tables below is FROZEN AT AUTHORING TIME (2026-07) and is now
> stale.** The face-on producer batch (2026-08-02) moved 32 measures to `live`, including
> `m_axisTiltAtTop` / `m_axisTiltImpact`, `m_thoraxDrift`, `m_shoulderPlane`, `m_trailElbowRise`,
> `m_leadHandWidth`, `m_leadUpperArmToChest`, `m_leadArmToTorso`, `m_comOverLeadFootFinish`,
> `m_spineSideBendTop`, `m_feetAlignment`, `m_hipAlignment`, `m_shaftAngleP4`, `m_lowPointAhead`,
> `m_attackAngle`, all five `m_pelvisRot*`, all three `m_thoraxRot*`, `m_xFactorStretch` and the
> seven `m_trailWristFlexExt_*`. **`core.json` is the source of truth for status; this table is the
> authoring record.** Two rows are also wrong on the merits: `m_attackAngle` is noted as needing an
> accuracy caveat versus a launch monitor, which stands, but it is a FACE-ON metric rather than one
> awaiting a second camera; and `m_lowPointAhead`'s unit is inches, not cm.

### Shaft

| id | series | reducer | unit | status |
|---|---|---|---|---|
| `m_shaftDirectionP2` | shaft angle vs target line, DTL | at p2 | ° | planned (2D shaft tracks live; DTL RMSE 2.44°) |
| `m_shaftAngleP4` | shaft vs horizontal, face-on | at p4 | ° | planned |
| `m_shaftDirectionP4` | shaft vs target line, DTL | at p4 | ° | planned |
| `m_shaftPlaneBackswing` | `swingPlane` | Δ vs address plane, p2→p4 | ° | planned |
| `m_shaftPlaneDelivery` | `swingPlane` | at p6 (vs address plane) | ° | planned |
| `m_impactShaftLean` | `impactShaftLean` | at p7 | ° | live |
| `m_lowPointAhead` | `lowPointAhead` | summary | cm | live — **estimated** off the synthesized arc, ±2 in; resolves Bridged, read across swings |
| `m_attackAngle` | `attackAngle` | at p7 | ° | planned — camera-derived; note reduced accuracy vs LM in the label; LM value preferred when connected |
| `m_clubheadSpeedImpact` | `clubheadSpeed` | at p7 | mph | planned — requires curvature correction or second camera (documented hardware limitation) |

### Ball

| id | series | unit | status |
|---|---|---|---|
| `m_launchDirection` | ball velocity azimuth vs target line | ° | planned |
| `m_launchAngle` | ball velocity elevation | ° | planned |
| `m_ballSpeed` | streak-derived speed | mph | planned (validated ~0.2–0.75 mph SD/frame; average ≥5 streaks) |

### Launch monitor — `notCapturable`, flagged

| id | unit | gapReason |
|---|---|---|
| `m_faceAngle` | ° | Requires launch monitor: face orientation at impact not optically resolvable |
| `m_faceToPath` | ° | Requires launch monitor: derived from face angle |
| `m_spinRate` | rpm | Requires launch monitor: spin not measurable over short indoor flight |
| `m_spinAxis` | ° | Requires launch monitor: as above; gates curvature outcomes |
| `m_smashFactor` | ratio | Requires launch monitor: needs validated clubhead + ball speed pair |
| `m_strikeLocation` | mm | Requires launch monitor (or face impact markers): impact position on face |
| `m_carryDistance` | yd/m | Requires launch monitor: flight model output |
| `m_dynamicLoft` / `m_spinLoft` | ° | Requires launch monitor: delivered loft at impact |

Note the catalogue already declares `faceAngle` as a metricKey — reconcile: either the producer is an LM-integration stub (then these measures are `provided` with `notCapturable` overridden once an LM connector exists) or remove the key. Decide once; don't leave both stories in the tree.

---

## 5. Missing norms (seed rows for `norms.json`)

All `"source": "heuristic"` pending citations; research task attached to each ★. Context `any` unless stated. Values are seeds for the corridor editor, not truth.

### Existing measures currently without norms

| measure | context | mu | σLo | σHi | unit | note |
|---|---|---|---|---|---|---|
| `m_spineBendAtAddress` | any | 40 | 8 | 8 | ° | forward bend from vertical ★ |
| `m_spineBendLoss` | any | 0 | 5 | 5 | ° | change p1→p7 |
| `m_axisTiltAtTop` | any | 5 | 5 | 5 | ° | trail-side tilt at p4; reverse-spine fires low ★ |
| `m_pelvisThrustDown` | any | 0 | 4 | 4 | cm | toward ball line p5→p7 |
| `m_pelvisLiftTop` | any | 0 | 2.5 | 2.5 | cm | trail hip hike |
| `m_pelvisSwayBack` | any | −2 | 3 | 3 | cm | away from target in backswing |
| `m_pelvisSwayDown` | any | 8 | 4 | 4 | cm | toward target p4→p7 |
| `m_pelvisSwayImpact` | any | 6 | 4 | 4 | cm | forward of address at p7 |
| `m_thoraxDrift` | any | 0 | 4 | 4 | cm | |
| `m_leadKneeFlex` | any | 25 | 8 | 8 | ° | at p7 |
| `m_shoulderAlignment` | any | 0 | 4 | 4 | ° | open = negative per sign doc |
| `m_shoulderPlane` | any | 90 | 8 | 8 | ° | shoulder plane vs spine; flat fires low ★ |
| `m_ballBodyGap` | iron | ~28 | 5 | 5 | cm | hands-to-thigh proxy preferred; author per club |
| `m_trailElbowRise` | any | 0 | 5 | 5 | cm | above shoulder-plane reference at p4 |
| `m_leadArmToTorso` | any | 20 | 10 | 10 | ° | at p8; chicken wing fires high |
| `m_leadHandWidth` | any | 100 | 10 | — | % of p4 width | at p5; loss of width fires low |
| `m_xFactorStretch` | any | 48 | 10 | 12 | ° | peak pelvis–thorax separation early downswing ★ (well-studied; upgrade to supported with DOI) |
| `m_pelvisRotPeak` / `m_thoraxRotPeak` | any | order test only | — | — | — | no corridor needed; already `order` signal |
| `m_lumbarCurve` / `m_thoracicCurve` | any | — | — | — | ° | noProducer — author norms when facet series is defined, else the S-/C-posture signals stay honest as unmeasured |
| `m_clubPathAtImpact` | any | 0 | 3 | 3 | ° | in-to-out positive per sign doc; OTT fires low |
| `m_leadWristAtTop` | — | — | — | — | — | delete or fold into p4 grid (§1.2) |

### New measures — key context-dependent rows

| measure | any | driver | iron | wedge | unit |
|---|---|---|---|---|---|
| `m_attackAngle` ★ | 0 ±3 | +2 ±3 | −4 ±2.5 | −5 ±3 | ° |
| `m_launchAngle` ★ | — | 13 ±3 | 17 ±4 (mid-iron) | 28 ±6 | ° |
| `m_impactShaftLean` | 5 ±4 | 0 ±4 | 8 ±4 | 10 ±5 | ° |
| `m_lowPointAhead` | +5 ±4 | −3 ±4 (behind ball OK) | +8 ±4 | +8 ±5 | cm |
| `m_axisTiltImpact` | 25 ±8 | 30 ±8 | 20 ±8 | 15 ±8 | ° |
| `m_shaftDirectionP4` | 0 ±8 | — | — | — | ° (positive = across the line) |
| `m_shaftAngleP4` | 0 ±15 | +10 ±15 | −5 ±15 | −20 ±15 | ° past parallel positive |
| `m_pelvisRotP7` | 40 ±10 | 45 ±10 | 38 ±10 | 30 ±10 | ° open |
| `m_thoraxRotP4` | 90 ±12 | 95 ±12 | 88 ±12 | 80 ±12 | ° |

Also add club-context rows for **existing** `m_ballPosition` (driver: well forward; wedge: centre) and `m_stanceWidth` (driver wide, wedge narrow) — currently they resolve only at `any`, which contradicts the design intent stated in the norms file comment.

★ = research task: locate peer-reviewed source (biomechanics journals, not vendor data), then set `source` and provenance tier. Where only vendor-published population data exists, keep `heuristic` — the values may be informed by such data but must not name it.

---

## 6. Corroborates / excludes edges (first tranche)

- `sig_headSway`-detected `excessive_head_sway` **corroborates** `sway`
- `trail_knee_straighten` **corroborates** `sway`
- `insufficient_shaft_lean` **corroborates** `scooping` (shaft vs wrist view of one event — do NOT also author causes between them; the shadowing gate will reject it anyway)
- `steep_downswing_shaft` **corroborates** `over_the_top`
- `m_spineSideBendTop` signal **corroborates** `reverse_spine`
- feet/hip alignment signals **corroborate** `alignment_open`/`alignment_closed`
- `strike_fat` **excludes** `strike_top` (same swing cannot be both)
- `across_the_line` **excludes** `laid_off`; `overswing` axis-pairs with `short_backswing`

---

## 7. Screen definitions for latent conditions (`screen.*`)

Author a small `screens.json` (or a `screens[]` block if the schema grows one) defining each `screenRef` generically with a clinical protocol description and a pass corridor from physiotherapy ROM literature — never a branded screening system. Seeds ★:

| screenRef | protocol (generic) | pass corridor |
|---|---|---|
| `screen.trailHipInternalRotation` / `lead…` | seated or prone hip IR, goniometer | ≥ 30° (typical adult 30–45°) |
| `screen.pelvicDisassociation` | standing pelvis rotation with fixed thorax | smooth independent rotation, no lumbar hinge |
| `screen.thoracicRotation` | seated trunk rotation, arms crossed | ≥ 45° per side |
| `screen.thoracicExtension` | prone press-up / wall angel | achieves neutral extension |
| `screen.thoracicKyphosis` | visual/inclinometer | fixed curve ≤ ~45° |
| `screen.shoulderFlexion` | supine shoulder flexion | ≥ 160° |
| `screen.trailShoulderExternalRotation` | 90/90 ER | ≥ 80° |
| `screen.ankleDorsiflexion` | knee-to-wall | ≥ 10 cm |
| `screen.hipExtension` | modified Thomas test | thigh reaches neutral |
| `screen.wristMobility` | flex/ext + radial/ulnar ROM | ext ≥ 60°, flex ≥ 60° |
| `screen.coreStability` | front plank / anti-rotation hold | ≥ 60 s plank without pelvic drop |
| `screen.singleLegBalance` | single-leg stance, eyes open | ≥ 25 s stable |

---

## 8. Suggested execution order for Claude Code

1. §1 hygiene fixes + §2 schema additions (groups, phase tokens) — everything else depends on them.
2. §5 norms for existing measures (unblocks 20+ already-authored signals; biggest immediate coverage win, several against **live** producers: `m_leadElbowFlex_p4` etc.).
3. §3 conditions whose measures are already **live** (`early_face_roll`, `bent_lead_arm`, `excessive_head_sway`, `head_drop_backswing`, `excessive_heel_lift`, `insufficient_shaft_lean`, `low_point_behind_ball`, `deceleration`, feet/hip alignment) — new faults at zero pipeline cost.
4. Ball-flight/strike outcome layer + causal edges (§3.6) and condition aliases (§2.4, §3.7) — turns explanation into the golfer's own vocabulary and enables the "what's your bad shot?" intake (§2.5). Include the intent contexts (§2.6).
5. Shaft-plane / top-of-swing conditions (planned shaft measures) — feeds the P1–P8 reference-model workstream.
6. LM-flagged measures as `notCapturable` stubs (§4) so the fault library is complete and the capture gap is explicit; LM-gated outcomes ship `asserted` per §2.5.
7. Screens (§7), drills namespace, glossary view (§3.8), citation backlog (★ items).
