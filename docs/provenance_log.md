# Provenance log — the sources behind the causal graph

What every citation in `core.json` actually establishes, and — more usefully — what it does not.
The JSON carries DOIs; this file carries the reasoning, because a DOI on its own cannot say whether
a paper tested the claim or merely the mechanism beneath it.

**Verification rule.** Every DOI below was resolved against CrossRef and its title, journal and year
confirmed to match the claim being made. Nothing here is recalled. A plausible-looking DOI is worse
than a null: the null is honest, and the wrong DOI is a lie that survives review.

**Attribution rule.** No commercial organisation, product or certification body is named — here, in
`core.json`, or in any `searchTerms`. `core_pack_test` greps the raw content bytes for it. Two of the
references below have authors employed by a club manufacturer; they are peer-reviewed conference
proceedings with DOIs, and only the DOI is recorded. Noted so nobody "discovers" it later and reverts
a legitimate citation.

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

**Low back pain generally.** S6 (systematic review) found no significant differences in hip angle,
trunk angle or crunch factor between recreational golfers with and without LBP, and S5 notes crunch
factor was not reproduced by two independent methods. Every `injuryNote` in the pack is already
phrased as association — "is associated with", "most often linked with", "worth raising with a
clinician" — which is what this evidence supports and no more. None asserts causation, and none
should be strengthened.

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

## 4. Leads encountered but NOT verified — do not cite from this list

A citation must never be written from here. These were seen in reference lists and not confirmed
against a publisher record.

- Murray et al., *Phys Ther Sport*, 2009 — hip rotation ROM and LBP prevalence in amateur golfers.
- Sell et al. 2007 — balance/strength/flexibility by handicap band.
- Lephart et al. 2007 — exercise intervention raising torso rotational velocity and clubhead speed.
- Wells et al. 2009 — balance and core strength vs golf performance.
- **Kim et al., *J Sports Sci Med*, 2018 — ball position and clubhead kinematics.** S15's companion;
  would likely lift `ball_forward → pull` and `ball_back → push` from `indirect` to `supported`.
  **Highest-value item here.**
- Cole & Grimshaw, *Spine J*, 2014 — the crunch factor's role in golf LBP.
- Callaway et al., *Int J Sports Phys Ther*, 2012 — peak pelvis rotation speed and gluteal strength
  by handicap.
