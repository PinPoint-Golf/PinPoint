# Edge provenance — implementation plan

Every causal edge in the pack is a claim, and until now not one of them could say how well founded
it was. This package gives an edge the same `Provenance` a condition carries, and then does the
literature work to fill it in — including, deliberately, filling it in with *nothing found*.

**This is a live working document.** Update the Progress table and Session log as batches land.
The research spans many sessions; the tables below are what makes a cold restart possible.

**Read first:** `docs/developer/diagnostics_developer_guide.md` §3 (the data model) and the
"Traps that have already cost time" section. This package does **not** change what any edge claims
— only what the pack can say about how well founded the claim is.

---

## The rule that shapes everything

**A null result is a result, and it is only worth anything if it is dated.** `Proposed` and
`NoSourceFound` look alike and mean opposite things: the first is a task, the second is a finding.
Collapsed into one value — which is what shipped — the library cannot tell "nobody has checked" from
"we checked and the field is silent", and cannot re-open the second when the field publishes.

The second rule: **do not invent a citation.** Every identifier in the content was fetched and read
in the session that wrote it. A plausible-looking DOI is worse than a null, because a null is
honest and a wrong DOI is a lie that survives review.

---

## The tier vocabulary

**The tier says what the EVIDENCE is. `searchedOn` says whether anybody has LOOKED. They are
independent, and coupling them was a real bug** — see the session log for 2026-07-27 batch 2.

| Tier | Means | Citation | Date |
|---|---|---|---|
| `proposed` | no basis recorded at all | — | — |
| `noSourceFound` | searched; the literature is silent | none, by definition | **required** |
| `practice` | established coaching practice, never measured | none, *see below* | optional |
| `indirect` | a source supports the MECHANISM; the named pair is our inference | required | — |
| `supported` | a source tests this cause and this effect | required | — |
| `established` | reproduced across independent sources | required | — |

**The work queue is `needsLiteratureSearch()` — no date and no citation — not `tier == proposed`.**
An edge authored from coaching knowledge is `practice` on the day it is written, with no date: that
records where it genuinely came from while leaving it outstanding. Searching it later either finds a
paper (it becomes `indirect`/`supported`) or does not (it stays `practice`, now dated).

**`indirect` is the tier most edges will land on, and it exists because of what the literature
actually contains.** A paper establishes that limited hip internal rotation produces greater
lumbopelvic compensation; it does not test "early extension", because that is a coaching label and
not a measured variable. Filing such a source under `supported` would make the citation look like it
backs the edge when it backs the reasoning behind the edge.

**`practice` carries no citation and that is forced, not lazy.** The bodies asserting these claims
are commercial screening and launch-monitor organisations, and this repo's standing rule is that the
TERMS are common domain while the ATTRIBUTION must not enter the content in any form — not a
citation, not an author, not a note. `core_pack_test` greps the raw bytes for exactly that. So the
tier records the STATE of the evidence and `searchTerms` records what was looked for. Naming who
says it is both forbidden and the part that carries least information.

---

## Progress

| # | Stage | State | Landed |
|---|---|---|---|
| 1 | Schema — `ProvenanceTier` × 6, `Provenance::searchedOn`/`searchTerms`, `Edge::provenance` | ☑ complete | 2026-07-27 |
| 2 | Loader/writer, demotion rules, legacy bare-citation compat, tests | ☑ complete | 2026-07-27 |
| 3 | Research batch 1 — hip IR, reverse spine, thoracic restriction | ☑ complete | 2026-07-27 |
| 4 | Marshaller + a badge on the DAG and the detail rows | ☐ not started | |
| 5 | Research batches 2..n — the remaining 172 edges | ☑ complete | 2026-07-27 |
| 6 | Condition provenance — the same pass over 112 conditions | ◐ 9 of 112 | |
| 7 | A health check for unsearched content, and the guide | ☐ not started | |

State vocabulary: ☐ not started · ◐ in progress · ☑ complete (gate green) · ⚠ blocked.

---

## Yield, measured rather than guessed

Batch 1 searched three clusters and settled **6 edges of 178**. That rate is the plan's main input,
and it is low for reasons that will not improve:

- **Most top search hits are unusable.** Commercial screening bodies, chiropractic clinics and
  coaching sites dominate results for every one of these terms. Usable work sits in PubMed, PMC,
  *J Sports Sci*, *Sports Biomech*, *AJSM* and similar.
- **The literature measures kinematics; the pack names faults.** The join is almost always an
  inference, which is what `indirect` is for — and what stops the citation overclaiming.
- **Each edge needs its own judgement.** Kim et al. covers `limited_trail_hip_ir → loss_of_posture`
  and does **not** cover `limited_trail_hip_ir → stance_narrow`, though both hang off one condition.
  Attaching one source to a whole fan-out would be the fastest way to make this worthless.

**Do not batch by source. Batch by claim, and attach per pair.**

---

## Session log

Newest last. What was searched, what was found, what was recorded.

| Date | Batch | Outcome |
|---|---|---|
| 2026-07-27 | 1 | **Schema, loader, tests, and the first three clusters.** `ProvenanceTier` gains `NoSourceFound`, `Practice` and `Indirect`; `Provenance` gains `searchedOn` + `searchTerms`; `Edge` swaps its bare `citation` string for the shared `Provenance`. Two demotion rules, and the second is the one that matters: a cited tier with no citation demotes to proposed (`edgeTierNoCitation`), but `noSourceFound`/`practice` must NOT demote — that would destroy the finding they record — so they are gated on having a DATE instead (`searchNoDate`), because an undated null cannot be told from an unasked question. Legacy bare `citation` on an edge still parses, reading as `supported`: no shipped edge ever had one, but a community pack might, and dropping it silently would lose the only thing the old schema could say. **Verified sources: Kim et al., *Am J Sports Med* 2015, `10.1177/0363546514555698` (PMID 25398245) — limited hip IR produces significantly greater lumbar flexion, axial rotation, lateral bending and pelvic posterior tilt; and *Sports Med Health Sci* 2020, `10.1016/j.smhs.2020.03.002` (PMID 35783335) — professionals with LBP show significantly more lead-side lateral bending in the backswing.** Both fetched and read, not recalled. Kim attached `indirect` to three edges only — the ones whose effect IS one of those measured kinematics under a coaching name — and deliberately not to `stance_narrow`, `excessive_heel_lift` or `sequence_order`, which it does not reach. Three `limited_thoracic_rotation` edges recorded `practice`: searched, no test of the pair, and the claim is coaching orthodoxy. **Also found, and not yet acted on: the X-factor→speed link is contested.** `10.1080/02640414.2018.1442287` (*J Sports Sci* 2018) found no X-factor differences across club or swing effort — only X-factor *stretch* varied — and a junior-golfer study reported a NEGATIVE relationship between X-factor and club velocity in boys. That is a finding about content the pack already ships (`xfactor_deficit` and its three incoming edges) and it belongs in the ledger, not quietly in a tier. Analyzer suite **79/79**. 172 edges remain `proposed`. |

| 2026-07-27 | 2 | **The tier and the search were coupled, and it made the model incoherent.** Mark's question: if the 19 edges came from coaching knowledge, why are they `proposed` — where else would they have come from? Correct, and the cause was that `practice` demanded a `searchedOn`. So marking an honestly-authored edge as what it was would have claimed somebody had been to the literature, and the only way to stay truthful about the search was to lie about the basis and call it `proposed` — which says nobody has any grounds for it at all. Two independent questions had been welded into one field. **`searchDateRequired()` now covers `NoSourceFound` only** — that tier IS a claim about a search outcome and is meaningless undated — and `practice` may be undated, meaning "orthodoxy, nobody has checked it against the literature yet". The work queue moved off `tier == proposed` onto **`needsLiteratureSearch()`** (no date, no citation), so attribution and outstanding-ness stopped fighting each other. The 19 edges of `8d0378f` are now `practice`, undated: edge tiers read 153 proposed / 22 practice / 3 indirect, and the queue is **still 172**, unchanged by the relabel — which is the proof the decoupling worked. Ledgered as `P6`: the other 153 were authored by earlier packages and are probably `practice` too, but only their authors can say, and `proposed` at least claims nothing. Analyzer suite **79/79**. |

| 2026-07-27 | 3 | **A separate research pass closed the queue, and every DOI in it resolves.** Mark supplied `provenance_update.md` from a parallel fable/opus search. **All 12 DOIs written into content were independently resolved against CrossRef here** — title, journal and year matched the claim in every case, which is the only reason any of it was applied. 14 new refs, 29 explicit edge mappings, 9 conditions. Final split: **4 `supported`, 27 `indirect`, 147 `practice`, 0 `proposed`, 0 `noSourceFound`.** The four `supported` are the only edges where cause and effect are both measured variables in the cited study — ball position → shoulder alignment (S15, measured in both directions) and face-to-path → the two compound outcomes (S12, launch direction 61–83% toward face angle). `limited_lead_hip_ir → early_extension` was re-cited from S2 to **S3**, which is the paper that actually establishes lead-hip dominance in the early downswing rather than reaching it via pelvic tilt. **One content correction fell out of S1:** the `limited_trail_hip_ir` and `limited_lead_hip_ir` injury notes were identical, but Vad et al. found the LBP correlation on the **lead** hip with *no significant trail-hip finding* — so the trail note asserted a link the evidence specifically did not support, and now says so. The rest of the injury notes were already phrased as association ("is associated with", "most often linked with") and were left alone: S5/S6's mixed findings support exactly that phrasing and no more. **Two discrepancies in the source document, neither harmful:** its headline says 12 edges earn `indirect` while §3 lists 24 — the body is the specification and was applied; and it asks for `noSourceFound` nowhere, correctly, since every family returned consensus. Analyzer suite **79/79**; brand grep clean. |

---

## Method, for whoever picks this up

1. **Take a claim, not a source.** Pick one (cause, effect) pair, or a cluster that genuinely shares
   one mechanism.
2. **Search scholarly sources only.** Exclude commercial coaching and equipment domains; they will
   otherwise fill the first page every time.
3. **Fetch the record and read it.** Title, journal, year, PMID, DOI, and the abstract's actual
   finding. Never record an identifier you have not seen resolve.
4. **Ask what the paper MEASURED**, then decide the tier. If its variables are the pair, `supported`.
   If they are the mechanism beneath the pair, `indirect`. If it is orthodoxy with no test,
   `practice`. If there is nothing, `noSourceFound`.
5. **Record `searchTerms` whatever the answer**, so the next attempt starts where this one stopped.
6. **A source that CONTRADICTS an edge goes in the ledger, not in a tier.** There is no
   `contradicted` value by decision (2026-07-27); a contradicted edge is a content question for
   Mark, and burying it in provenance would let the library keep asserting something it now has
   reason to doubt.

---

## Ledger

| # | Item | State |
|---|---|---|
| `P1` | **The X-factor → clubhead-speed link is contested in the literature**, and the pack ships `xfactor_deficit` with three incoming edges resting on it. `10.1080/02640414.2018.1442287` found no X-factor difference across club or swing effort (only X-factor *stretch* varied); a junior-golfer study found a negative X-factor/velocity relationship in boys. Not a reason to delete anything — but the condition's consequence text should be read against this before the engine ever surfaces it. | open — for Mark |
| `P2` | **No `contradicted` tier.** Decided 2026-07-27: a source arguing against an edge is a content question, not a provenance grade, and it must not be filed where it stops being visible. If `P1`-shaped findings become common, revisit. | closed — by decision |
| `P7` | **The 14 symmetric edges sit in the work queue for a search that must never happen.** A `corroborates` edge asserts two measures view one physical event; an `excludes` edge asserts physical impossibility within one swing. Both are analytic, so they carry `practice` with no `searchedOn` — and `needsLiteratureSearch()` therefore reports them outstanding forever. The clean fix is a `definitional` tier that is settled by construction and never enters the queue. Deliberately not added in batch 3: it would be the third change to a tier vocabulary Mark has already reviewed twice, and 14 known rows is a smaller problem than a schema nobody agreed to. | open — for Mark |
| `P8` | **Two edges carry conflicting evidence and the schema cannot say so.** `casting → ball_speed_deficit` is `indirect` on S8 (delayed release relates to ball velocity) while S10 finds clubhead speed not well associated with wrist-cock angles in 66 skilled golfers; `xfactor_deficit` is `supported` on S7 while S10 and a junior-golfer study disagree. Both conflicts live in `docs/provenance_log.md` §2 because there is nowhere in the JSON to put them. **No UI copy may present either as settled.** A `conflictingEvidence` field would be the fix, and these two are its first customers. | open |
| `P9` | **`slide` may not be a fault.** S4 reports that *skilled* golfers laterally slide the pelvis toward the target and that this **contributes to clubhead speed**. The condition is framed as one — its consequence reads "Excess lateral movement… outruns the rotation", which is defensible since it names the excess, but the corridor is what actually separates functional from excessive and nobody has checked that it does. Review the consequence text and the norm together. Not touched in batch 3: rewriting a consequence is a coaching-voice judgement. | open — for Mark |
| `P6` | ~~**153 edges are still `proposed`**~~ **CLOSED 2026-07-27 by batch 3.** All of them were searched by family and recorded `practice` with the terms used; zero edges remain `proposed`. They were authored across this project's earlier packages, and if their basis was coaching knowledge — as the 19 in `8d0378f` were — then `practice` is their honest tier too and `proposed` understates them in exactly the way batch 2 fixed. Only the people who wrote them can say. Until somebody does, `proposed` is the safe reading: it claims nothing. | open — for Mark |
| `P3` | **Condition provenance is untouched.** All 112 conditions are `proposed`, and now that `searchedOn` exists, none of them records whether anybody has looked. Same pass, one registry over. | open |
| `P4` | **No health check reports unsearched content.** `proposedTier` fires on conditions; nothing counts edges whose provenance is still the default, so the work queue is invisible in the app. Stage 7. | open |
| `P5` | **The badge does not exist yet.** Until stage 4, the DAG draws `indirect`, `practice` and `supported` edges identically — which is the defect this package opened with, one level down. | open |
