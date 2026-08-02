# Provenance log — the sources behind the causal graph

What every citation in `core.json` actually establishes, and — more usefully — what it does not.
The JSON carries identifiers; this file carries the reasoning, because an identifier on its own
cannot say whether a paper tested the claim or merely the mechanism beneath it.

**Verification rule.** Every identifier below was resolved against CrossRef, PubMed or arXiv and its
title, journal, authors and year confirmed to match the claim being made. Nothing here is recalled. A
plausible-looking DOI is worse than a null: the null is honest, and the wrong DOI is a lie that
survives review. The same rule bars a *number* nobody read: pass 2 moved **zero** norms to
`source: literature` for exactly this reason — see §6.

**Identifiers.** Most records carry a DOI. `S20` carries a **PMID** and no DOI, because its journal
issues none; the registry, the join and the References view all accept either, and a PMID row opens
at PubMed. Refusing it would have meant the pack could cite a paper no reader could be shown.

**Attribution rule.** No commercial organisation, product or certification body is named — here, in
`core.json`, `norms.json`, `references.json`, or in any `searchTerms`. `core_pack_test` greps the raw
content bytes of **all three** content files for it. It used to grep only `core.json`, and a
wrist-sensor vendor duly sat unnoticed in a `norms.json` citation note until pass 2; a rule enforced
over one of three files reads as enforced and is not. Two of the references below have authors
employed by a club manufacturer; they are peer-reviewed conference proceedings with DOIs, and only
the DOI is recorded. Noted so nobody "discovers" it later and reverts a legitimate citation.

---

## 1. Bibliography

| ref | DOI | Source | What it establishes |
|---|---|---|---|
| **S1** | `10.1177/0363546503261729` | Vad et al., *Am J Sports Med* 32(2), 2004 | In 42 male professional golfers, LBP history correlated with decreased **lead** hip internal rotation, FABERE distance and lumbar extension. **No significant trail-hip finding.** |
| **S2** | `10.1177/0363546514555698` | Kim et al., *Am J Sports Med*, 2015 | Golfers with hip IR <20° showed significantly greater lumbar flexion, axial rotation, right lateral bending and pelvic posterior tilt than the ≥30° group. |
| **S3** | `10.1186/s12938-015-0041-5` | Mun et al., *BioMedical Engineering OnLine* 14:41, 2015 | Lumbar–hip rotational coupling r≈0.81. Lead-hip contribution to overall rotation is markedly high **in the early downswing**; lumbar contributes more late. |
| **S4** | `10.1007/s40279-015-0429-1` | Cole & Grimshaw, *Sports Medicine*, 2016 | Review. Skilled golfers laterally slide the pelvis toward the target, **contributing to clubhead speed**; argues the lumbar spine may not safely accommodate the resulting loads. |
| **S5** | `10.1016/j.smhs.2020.03.002` | Edwards, Dickin & Wang, *Sports Med Health Sci*, 2020 | LBP risk-factor review. Professionals with LBP show more lead-side lateral bending in the backswing. Crunch-factor evidence is **conflicting**. |
| **S6** | `10.1080/02640414.2024.2319443` | *J Sports Sci*, systematic review (online 2023) | Mixed/null: **no** significant differences in hip angle, trunk angle or crunch factor in recreational golfers; elite golfers with LBP showed shorter transitions and greater peak lead-side lumbar lateral flexion. |
| **S7** | `10.1080/02640410701373543` | Myers et al., *J Sports Sci*, 2008 | X-factor and upper-torso rotational velocity are significantly related to ball velocity. |
| **S8** | `10.1080/02640414.2010.507249` | Chu, Sell & Lephart, *J Sports Sci*, 2010 | Models accounted for 44–74% of ball-velocity variance; X-factor, **delayed release of the arms and wrists**, trunk tilting and weight shifting significantly related. |
| **S9** | `10.1080/14763141.2010.535842` | Tinmark et al., *Sports Biomechanics*, 2010 | Significant proximal-to-distal temporal sequencing with successive increase in peak segment angular speed, across genders, skill levels, full and partial shots. |
| **S10** | `10.3390/app10175728` | Kwon et al., *Applied Sciences*, 2020 | 66 skilled golfers. Higher clubhead speed associated with shorter downswings, larger rotation ranges, larger hip–shoulder separation at impact, delayed transitions. **Clubhead speed NOT well associated with wrist-cock angles/ranges or X-factor/stretch.** |
| **S11** | `10.1080/14763141.2025.2547277` | *Sports Biomechanics*, 2025 | 84 elite male golfers, four wrist-release styles. Delayed-release styles showed larger uncocking velocities, longer transition, shorter early downswing, complete proximal-to-distal sequencing. |
| **S12** | `10.3390/proceedings2060249` | ISEA 12th Conference *Proceedings*, 2018 | Player and robot studies: initial launch direction falls **61–83% toward face angle**, remainder toward club path, varying by club. |
| **S13** | `10.3390/proceedings2020049027` | ISEA 13th Conference *Proceedings*, 2020 | Hertzian impact model with tangential compliance via Coulomb friction; predicts launch direction from delivered face angle and path. |
| **S14** | `10.1088/0034-4885/66/2/202` | Penner, *Reports on Progress in Physics*, 2003 | Peer-reviewed physics review: double-pendulum swing models, club–ball impact, loft/launch/spin relationships, turf interaction. |
| **S15** | `10.1038/s41598-020-79091-7` | Kim et al., *Scientific Reports*, 2021 | 20 professionals, 5 ball positions, SPM. Target-side ball position gave significantly more open shoulder angle at address, in the downswing and at impact, plus a **less in-to-out club path**. Trend reversed trail-side. |
| **S16** | `10.2165/00007256-200535050-00005` | Hume, Keogh & Reid, *Sports Medicine*, 2005 | Review of biomechanics for distance and accuracy. Background; not cited in content. |
| **S17** | `10.1123/jab.27.3.242` | Meister et al., *J Appl Biomech* 27(3):242–51, 2011 | 10 pros + 5 amateurs. **Benchmark curves** for pelvis rotation, upper-torso rotation and X-factor across the whole swing. Peak free moment/kg, peak X-factor and peak S-factor highly consistent in pros (CoV 6.8/7.4/8.4%). **Downswing initiated by reversal of pelvic rotation, then upper torso**; peak X-factor preceded peak free moment in *every* swing, in the initial downswing. Amateur deviation from pro means grows with handicap. |
| **S18** | `10.3389/fspor.2022.986281` | Zhou et al., *Front Sports Act Living* 4:986281, 2022 | Single-score index built on the S17 benchmark curves, 11 pros + 5 amateurs (handicap 4 to novice). The selected formula combines peak pelvic rotational velocity pre-impact, **pelvic rotational velocity at impact**, and peak upper-torso rotational velocity post-impact. Pros standardise to 100 ± 10; amateurs average 82 ± 4. |
| **S19** | `10.5535/arm.2018.42.5.713` | Steele et al., *Ann Rehabil Med* 42(5):713–21, 2018 | 11 pros vs 5 amateurs. Peak rotational velocities highly consistent in pros (CoV 0.086 upper torso, 0.079 pelvis). **Peak upper-torso rotational velocity and peak X-prime occur AFTER impact, in follow-through**; peak pelvic rotational velocity occurs in the downswing. **Pelvic velocity at impact reduced in amateurs (p=0.019).** |
| **S20** | PMID `30479527` | Kim et al., *J Sports Sci Med* 17(4):589–98, 2018 | 11 male professionals, 3D motion capture + two force platforms, eight ball positions mediolateral and anteroposterior. Mediolateral ball position significantly changed **shoulder rotation and club-face aim at address** and lead-foot vertical GRF, and **club-face aim, club path and angle of attack at impact**. The companion to S15 — S15 measures the address end, S20 the delivery end. **No DOI: this journal issues none.** |
| **S21** | `10.48550/arXiv.physics/0611291` | Grober & Cholewicki, arXiv, 2006 | **PREPRINT — NOT peer reviewed.** Tempo as a biomechanical clock (simple harmonic oscillator). Shaft accelerometers ~250 Hz, 10–20 five-iron swings each, over 12 playing pros / 13 teaching pros and good amateurs / 18 other golfers. **Backswing:downswing ratio centred at 3.0 in all three groups**; playing pros span 2.5–3.5 with very small per-golfer SD and swing uniformly faster. The source of the quoted 3:1 figure. **Their backswing starts at the start of MOTION, not at address.** Cited by nothing; attribution only — see §6b. |

---

## 2. Conflicting evidence — read before trusting these

**Wrist release and ball speed.** S8 finds delayed release of the arms and wrists significantly
related to ball velocity. S10 finds clubhead speed **not** well associated with wrist-cock angles or
ranges in 66 skilled golfers. The edge `casting → ball_speed_deficit` is tagged `indirect` on S8;
the conflict is not recorded in the JSON because the schema has no field for it. **No UI copy may
present that edge as settled.**

**X-factor.** Same shape one level up. S7 relates X-factor to ball velocity and is the citation on
the `xfactor_deficit` condition; S10 finds no such association, and a junior-golfer study reported a
*negative* X-factor/velocity relationship in boys. The condition ships `supported` on S7 with that
caveat living here rather than in the tier.

**Where the peaks fall.** S17 and S19 are the same research group on the same apparatus and do not
contradict each other, but they are easy to misread together. S17 places **peak X-factor** in the
*initial downswing*, preceding peak free moment in every swing. S19 places **peak upper-torso
rotational velocity and peak X-prime** *after impact*, in the follow-through. Different quantities —
an angle versus a rate — reaching their peaks at different times. Neither may be quoted as "the peak
of the swing", and any UI copy about when something peaks must name which quantity it means.

**Low back pain generally.** S6 (systematic review) found no significant differences in hip angle,
trunk angle or crunch factor between recreational golfers with and without LBP, and S5 notes crunch
factor was not reproduced by two independent methods. Every `injuryNote` in the pack is phrased as
association — "is associated with", "investigated as a possible contributor", "the findings are
mixed", "worth raising with a clinician" — which is what this evidence supports and no more. None
asserts causation, and none should be strengthened.

**S6 is the most load-bearing uncited paper in the registry.** After pass 2 nothing cites it: both
conditions that used to (`s_posture`, `early_extension`) moved to the papers that measure their
mechanism. It stays in `references.json` and renders "cited by nothing" *deliberately*, because it
is the reason every injury note is worded the way it is. Deleting it would leave the bibliography
agreeing with itself — the same reason S10 is kept.

---

## 3. Why most of the graph is `practice`

133 causal edges were searched and returned coaching consensus with no peer-reviewed test of the
named pair. That is `practice`, not `noSourceFound`: the field agrees and has never measured it,
which is a different state from the field being silent. Three cases explain the pattern:

- **Core stability and balance.** The literature links these capacities to **performance and
  handicap** — handicap-stratified strength/balance comparisons, interventions raising clubhead
  speed — and never to a named swing fault. Every `poor_core_stability → …` and
  `poor_single_leg_balance → …` edge is orthodoxy.
- **Early extension.** The widely-quoted prevalence figures come from a commercial screening body's
  internal dataset: uncitable here on principle, and unpublished on the merits. No peer-reviewed
  measurement of the pelvis-toward-ball displacement pattern was found.
- **Gear effect** (`early_extension → strike_heel`, `off_balance_finish → strike_toe`). Well
  established club physics, but the accessible treatments are vendor and trade sources. S14 covers
  impact physics generally without testing off-centre strike location against swing faults.

**The 14 symmetric edges are analytic, not searched.** A `corroborates` edge asserts that two
measures view one physical event; an `excludes` edge asserts physical impossibility within one
swing. A literature search is a category error, so they carry `practice` with **no** `searchedOn`.
See the plan's ledger `P7` — this leaves them permanently in the work queue, which wants a
`definitional` tier to fix properly.

---

## 4. The condition pass (2026-07-27) — editorial decisions

Pass 1 sourced the edges. Pass 2 sourced the **conditions**, which had been sitting at 102
`proposed` — "nobody has looked". Final distribution: **5 `supported`, 23 `indirect`, 84
`practice`, 0 `proposed`**, and every condition now records the date its search was made.

`supported` for a *condition* means something narrower than it does for an edge: **the condition's
defining variable is directly measured in a peer-reviewed source.** A condition names a state, not a
causal pair, so there is no cause-and-effect for a paper to test. That is the pass-1 precedent
(`sequence_order` on S9, `xfactor_deficit` on S7, `reverse_spine` on S5) and pass 2 keeps the bar:

- **`hip_stall` → `supported` (S19).** S19 measures pelvic rotational velocity at impact and finds
  it significantly reduced in amateurs — the exact variable `m_pelvisRotRateP6P7` reads. S18
  corroborates independently: that quantity is one of the three terms in a validated skill index.
- **`transition_rush` → `supported` (S17)**, up from pass 1's `indirect`/S11. S17 establishes the
  downswing-initiation ordering directly. S11 remains corroborating; the schema has one citation
  slot, so this note is where the second source lives.

**Three citation swaps.** All three replace a source that constrained the *wording* with one that
measures the *mechanism*:

- `s_posture`: S6 → **S1**. S1 measures lumbar extension deficit directly. S6 still governs the
  injury note's wording (see below) but is no longer the condition's citation.
- `early_extension`: S6 → **S2**. S2 measures the lumbopelvic mechanism — greater lumbar flexion and
  pelvic posterior tilt in hip-restricted golfers. S6 is a null review; it constrains claims, it does
  not support this one.
- `ball_forward → pull` and `ball_back → push` (edges): S15 → **S20**, and `indirect` → `supported`.
  S20 measures ball position and club path / face aim at impact directly, which is both ends of
  those edges. This was pass 1's flagged highest-value lead, now confirmed.

**The 20 `ballFlight` conditions stay `practice` and take no citation.** Pull, push, block, chunk,
thin, top, sky, shank are coaching vocabulary; the literature does not use them as variables. The
physics beneath them is cited on the **edges**, where the causal claim actually lives. A condition is
a name; an edge is a claim. Do not attach S12 or S14 to `slice` or `hook` as conditions.

**Injury notes — four became seven.** The schema has no per-note tier, so the reasoning is here:

- `limited_trail_hip_ir` — **the note that contradicted its own literature.** S1 found the LBP
  association for the **lead** hip and *no significant difference* on the trail side. Mirroring the
  asymmetry onto the trail hip inverted the finding. Rewritten to say plainly that the mechanism is
  a mechanism and that nothing here says a tight trail hip carries a risk. The condition itself
  stays `practice` — it is the *injury claim* that had no source, and the schema cannot tier a note
  separately. If a `noSourceFound`-per-note tier ever lands, this is the row it exists for.
- `s_posture` — rewritten to association-with-uncertainty, naming S6's null result explicitly.
- **New, all worded "investigated as a possible contributor… the findings are mixed":**
  `early_extension` (S2 mechanism), `sway` (S5), `xfactor_deficit` (S7 against S10). These are the
  characteristics the LBP literature actually circles, and saying so with the uncertainty attached
  is more honest than silence. `xfactor_deficit`'s note is careful in one extra way: the literature
  asks whether *too much* separation loads the spine, so a *deficit* is explicitly not framed as a
  risk.

**Search terms.** §2.3 of the research pass supplied group-level terms for seven groups. The
`sequence` and `release` residuals (`hip_spin_out`, `scooping`) were not in that table; their terms
were written to match the group's subject matter. The `setup` group splits on `confirmedBy ==
screened` — the latent physical screens are a range-of-motion search, the rest a setup-geometry one.

---

## 5. Definitional alignment of measures

A measure asserts *how a quantity is computed*, not *what value is normal*. It makes no causal
claim, so it takes no `ProvenanceTier` — filing a definition as `supported` would be the category
error the `Indirect` tier was invented to prevent, and `Measure` deliberately has no `provenance`
field.

What a measure **can** be wrong about is definitional alignment: whether our quantity is the
literature's quantity. That is a real correctness risk and it is recorded here rather than in the
schema. **The first two rows are the ones that would produce a wrong number, not merely a
differently-named one.**

| measure | follows | where ours diverges |
|---|---|---|
| `m_tempoRatio` | `10.48550/arXiv.physics/0611291` | **Known divergence.** Published ratios run takeaway (start of motion) → top over top → impact. Ours is **address → top**. Not the same denominator, and the address→takeaway gap is structurally bounded but so far unmeasured. Already recorded in the norm note; the 3:1 figure must not be adopted as our mu. |
| `m_spineBendAtAddress` | PMID `30479527` | S20 defines trunk flexion in the global sagittal plane, C7 to mid-PSIS, positive for flexion. Ours is bend **from vertical**. Reconcile datum *and* sign before comparing to any published figure. |
| `m_xFactorStretch` | `10.1123/jab.27.3.242` | S17 computes X-factor as upper-torso minus pelvis rotation about the vertical axis. Confirm our thorax segment definition matches — acromion line vs thorax centre. |
| `m_thoraxRotP4`, `m_pelvisRotP5`, `m_pelvisRotP7` | `10.1123/jab.27.3.242` | Segment definitions per S17: pelvis = ASIS line, upper torso = bilateral acromion line, both about the vertical axis. |
| `m_pelvisRotRateP6P7` | `10.5535/arm.2018.42.5.713` | S19 reports *peak* pelvic rotational velocity in the downswing; ours is a rate over p6→p7. Related, not identical — do not compare our figures to theirs directly. |
| `m_clubPathAtImpact`, `m_attackAngle` | `10.3390/proceedings2060249` | Path = horizontal projection of clubhead direction at impact; attack angle = vertical projection. Sign: in-to-out positive. |
| `m_faceToPath` | `10.3390/proceedings2060249` | Face angle minus club path, both relative to the target line. |
| `m_launchDirection` | `10.3390/proceedings2060249` | Horizontal direction of ball centre of gravity immediately after separation. |

Every other measure is a geometric construction of this product's own vocabulary with nothing to
align to. That is fine and is not a gap.

---

## 6. Norms — why nothing moved to `literature`

### 6a. The honesty constraint

**`source: literature` asserts that this mu and this sigma came off that paper's results table.**
Pass 2 read abstracts, publisher records and one preprint body — not results tables. So **zero of
the 149 rows moved to `literature`, deliberately.** A pass that promoted thirty rows would look more
productive and would be worse: it would put numbers nobody read behind a tier that certifies
somebody did.

What the schema does permit — `citation` is documented as "DOI/PMID, **or the note explaining a
provisional figure**" — is naming the paper each figure should be **re-seated from**, so the next
pass starts with the paper in hand rather than with the search. 28 rows gained such a note; 30 now
carry a citation, all still `heuristic`.

### 6b. `m_tempoRatio` — cited, number unchanged

The most-quoted number in golf, and the pack carried it unattributed. The note now names S21 *and*
records the two reasons it cannot be adopted: the source is a preprint, and it measures from the
start of motion where ours measures from address. Attributing it and recording why it cannot be
taken is more useful than either silence or a false `literature` tag.

### 6c. Rows that must stay `heuristic` with NO citation

`m_smashFactor`, `m_spinRate`, `m_spinAxis`, `m_strikeLocation`, the per-club `m_launchAngle` rows,
`m_lowPointAhead`, `m_impactShaftLean`. Accessible figures for all of these come from launch-monitor
vendors and robot-test houses. The numbers may well be *informed* by that data — but the tier says
where the evidence is, and "a vendor published a chart" is not `literature` under this repo's rule.
**Do not let a future pass quietly cite a vendor blog.**

Two hygiene items were recorded as notes rather than silently fixed: `m_spinRate` at `any`
(mu 5000, sigma 2000 — spans driver ≈2600 to wedge ≈9000, so wide it can never fire) is now
declared a deliberate no-op fallback, kept so an unknown club degrades to silence rather than to a
missing norm; and `m_handSpeedP6P7` (mu 20, sigma 40 mph/s — sigma twice mu is not a corridor) is
flagged for corpus re-seating with an explicit instruction not to source it from literature.

### 6d. The wrist grid

The 128 `_p1`..`_p8` rows are untouched. Their existing note already records that they were migrated
from a compiled table now deleted and that nothing pins them. S11 is the best current lead, but it
reports release-style classifications and uncocking velocities, not per-P-position corridors.
**Re-seating these needs a corpus, not a paper.**

---

## 6e. The six `causes` edges added with the face-on producers (2026-08-02)

Six conditions became gradeable the moment their producers landed and had **nothing behind them** —
`core_pack_test`'s "every condition that can fire today has at least one cause" caught it, which is
the check drawing its intended line: a `noCause` on a producer-less condition is a backlog, and a
condition that fires TODAY with nothing behind it is a defect in what ships.

All six edges are `practice` tier, searched 2026-08-02. **None is cited, and none should be** — they
are coaching doctrine, and §3 of this log explains why most of the graph is and stays that way.

| From | To | Strength | The mechanism claimed |
|---|---|---|---|
| `limited_thoracic_rotation` | `disconnection` | moderate | A chest that cannot turn makes the backswing feel short, and the arms lift and run away from it to find length. |
| `limited_thoracic_rotation` | `abbreviated_finish` | moderate | The same restriction on the through-side: the turn runs out before the follow-through does. |
| `limited_lead_hip_ir` | `hip_spin_out` | moderate | A lead hip that cannot internally rotate cannot receive a pelvis turning over a stable lead leg, so the pelvis spins out instead. Consistent with the pack's existing `limited_lead_hip_ir → early_extension` and `→ hanging_back` edges. |
| `excessive_axis_tilt_impact` | `attack_too_shallow` | moderate | More trail-side lean at impact shallows the approach. Geometric, and the pack already carries `excessive_axis_tilt_impact → low_point_behind_ball`. |
| `reverse_spine` | `insufficient_axis_tilt_impact` | moderate | Tilt set the wrong way at the top leaves none of the right kind at impact. |
| `excessive_axis_tilt_top` | `excessive_axis_tilt_impact` | moderate | Too much tilt at the top is carried into impact. |

`moderate` throughout, and deliberately: each is a plausible single mechanism for a fault with
several, and §115's editorial rule — rank causes, do not assert one — applies. Two of the six raise
`limited_thoracic_rotation`'s fan-out to 15 and one raises `limited_lead_hip_ir`'s to 9;
`core_pack_test` pins both counts so the next author sees the change rather than absorbing it.

---

## 7. Leads encountered but NOT verified — do not cite from this list

A citation must never be written from here. These were seen in reference lists and not confirmed
against a publisher record.

- **Takagi et al., *Sports Biomech* 16(3):387–98, 2017 (PMID 28554300)** — club-shaft motions and
  clubface orientation. Located and identified, but the abstract was not retrieved, so it is not
  cited anywhere. Would be the right definitional reference for the shaft measures
  (`m_shaftPlaneDelivery`, `m_shaftDirectionP2`/`P4`) and possibly an `indirect` source for
  `early_face_roll → open_face_to_path`. **Highest-value item here.**
- Horan et al., *J Biomech* 43(8):1456–62, 2010 — thorax and pelvis kinematics in the downswing,
  male vs female skilled golfers. Likely carries the sex-specific figures any per-athlete norm work
  will need.
- Kwon et al. 2013 — X-factor methodology critique. Matters because it disputes S7.
- Joyce 2017 — lower-trunk X-factor stretch vs clubhead speed (r=0.78 reported second-hand).
- Zhang & Shan, *Scand J Med Sci Sports* 24:749–57, 2014 — driver swing consistency factors.
- Smith et al., *Procedia Eng* 34:224–9, 2012 — professional coaches' perceptions of key technical
  parameters. Unusually relevant: the closest thing to a *citable* record of coaching consensus,
  which could lift a number of `practice` edges to `indirect`.
- Murray et al., *Phys Ther Sport*, 2009 — hip rotation ROM and LBP prevalence in amateur golfers.
- Sell et al. 2007 — balance/strength/flexibility by handicap band.
- Lephart et al. 2007 — exercise intervention raising torso rotational velocity and clubhead speed.
- Wells et al. 2009 — balance and core strength vs golf performance.
- Cole & Grimshaw, *Spine J*, 2014 — the crunch factor's role in golf LBP.
- Callaway et al., *Int J Sports Phys Ther*, 2012 — peak pelvis rotation speed and gluteal strength
  by handicap.
