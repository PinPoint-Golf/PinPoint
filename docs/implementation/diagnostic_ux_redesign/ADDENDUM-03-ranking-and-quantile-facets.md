# Addendum 03 — ranking by reach, and facets cut from the data

Applies to `BRIEF.md`. Supersedes nothing; it adds two derived columns per condition list and a second
*kind* of facet to the rail described in Phase 1. Nothing in the toolbar, the graph or the editing
rules moves.

The question it answers, in the author's words: **"show me the things that really matter — the most
influential causes, and the effects everything funnels into."**

Every line reference below was written against `a4336d9`.

---

## What is wrong with what shipped

The Characteristics and Causes tables both carry an **Expl** column: how many other conditions this
one explains. It is computed by `coverageOf()`, which is one line —

```cpp
int coverageOf(const CharacteristicPack &pack, const QString &conditionId)
{
    return int(effectsOf(pack, conditionId).size());     // characteristic_pack.cpp:320
}
```

— and it counts **the arrows leaving this node**. Nothing further. If A causes B and B causes C,
A's Expl reads `1`.

The Causes list defaults to sorting on exactly that number (`model_browser.cpp:1503`). So on the
shipped pack:

| Condition | Expl reads | Actually reaches | …ball-flight outcomes in that reach |
|---|---:|---:|---:|
| Settled tempo habit | **1** | 29 | 11 |
| Club too short for the golfer | **2** | 26 | 10 |
| Poor core stability | 10 | 46 | 13 |
| Early extension | 5 | 9 | 5 |

"Settled tempo habit" has one arrow out of it, sorts near the bottom of a 95-row list, and reads as a
trivial piece of content. It reaches 29 conditions and eleven distinct bad shots. **The list is not
missing a filter so much as it is ranked on the wrong number**, and the number it is ranked on
systematically buries long chains — which is precisely the shape a root cause has.

The mirror failure is worse because it is silent. Ask the pack about **Slice**:

| | |
|---|---:|
| arrows out | **0** |
| arrows in | 2 |
| conditions that reach it transitively | **37** |

Slice causes nothing — it is an endpoint, the thing that happens *to* a golfer. It therefore scores
zero on every out-edge measure the panel has and cannot be surfaced by any sort or facet that exists
today. Its importance is entirely inbound: **37 of 140 conditions eventually lead to a slice.**

---

## The organising rule

> **Importance is how far a condition reaches through the graph, and reach has two directions.
> Causes rank by what they reach; effects rank by what reaches them.**

Both directions are the same existing function, `causalClosure(pack, id, downstream)`
(`characteristic_pack.h:185`), which walks the causal edges until it runs out and returns the set. The
`downstream` flag picks the direction. **No new traversal is written.**

That single rule supplies both halves of the author's question, which is why they are one addendum and
not two.

---

## Three derived numbers

Per condition, all three computed from the working assembly so an unsaved edge counts:

| Name | Definition | Answers |
|---|---|---|
| `leadsTo` | `causalClosure(id, downstream=true).size()` | how much would stop happening if this were fixed |
| `outcomes` | of that closure, how many are `ConditionGroup::BallFlight` | how many **bad shots** it explains |
| `causedBy` | `causalClosure(id, downstream=false).size()` | how much of the library funnels into this |

`outcomes` earns its place rather than duplicating `leadsTo`: limited **trail**-hip internal rotation
reaches 27 conditions and 6 ball-flight outcomes; limited **lead**-hip internal rotation reaches 27 and
**10**. Identical apparent size, materially different consequence, and the second is the one a coach
cares about.

### These are counts, and they must never become a score

The tempting move is to weight each edge by its `Strength` and sum a 0–1 influence figure. **Do not.**
This repo has refused that twice already, in comments that apply here verbatim:

- `Strength` is three-valued *because* "nobody can author 0.73 meaningfully, and rendering strength as
  a percentage would imply a probability it is not" (`characteristic.h:334`).
- `Corroboration` is deliberately reported rather than scored, because scoring "would mean inventing a
  number nobody could defend when asked why one cause outranked another" (`relation_resolver.h:66`).

A weighted score hits exactly that wall — there is no answer to *"why is this 0.71 and that 0.68"*. A
**count** always has one: here are the 29 conditions, listed. This is the same contract
`findingsCoveredBy()` exists to honour — "a ranking the user cannot interrogate is a ranking they
cannot trust" (`relation_resolver.h:102`).

If strength is wanted later, it belongs as a **subgraph switch** ("count only strong links"
re-runs the same walk over a filtered edge set), never as a weight. Still a count, still enumerable.

### Where the rule lives

`outcomeReachOf()` is added to `characteristic_pack.h` beside `coverageOf()`, because counting the
ball-flight members of a closure is a **graph rule** and `model_browser.h` is explicit that it holds
none: "every RULE here is a call into the layer that owns it… what is reimplemented is MARSHALLING,
and only marshalling." The other two are `causalClosure(...).size()` at the call site, which is calling
the owning layer rather than reimplementing it.

---

## Which column goes on which list

Not both numbers on both lists. Each list gains the number that answers **its own** question, because
a rail of every column is a filter nobody reads and the pane budget is already tight (`BRIEF.md`,
"Pane budget" — the name column is what gets starved).

| List | Gains | Why |
|---|---|---|
| **Causes** | `Leads to`, `Shots` | a cause is interesting for what it reaches |
| **Characteristics** | `Faults` | a characteristic is interesting for what converges on it |

The **facets follow the same split** — Causes offers *Most common faults* and *Knock-on effects*,
Characteristics offers *Most common outcomes* and no breadth facet. Characteristics already carries three vocabulary
facets, and a fourth and fifth push the rail past its scroll height for a question the Causes list
answers better: every condition with anything downstream is *on* that list by definition.

> **SUPERSEDED, 2026-08-03.** The Characteristics facet on `causedBy` has been **removed**, and the
> Causes facet on `outcomes` is now titled *Bad shots it reaches* with breadth words rather than
> frequency ones. `Condition::prominence` shipped in `ae9b57d` — how often a coach expects to see a
> thing — and it is faceted on both lists. Two facets one rail apart, each with a chip labelled
> **Common**, meaning "many paths funnel in here" and "about one golfer in three": that is a worse
> failure than either facet is worth, because a reader who cannot tell them apart trusts neither.
>
> Prominence keeps the word, because it is the only claim about frequency in the panel that is about
> GOLFERS rather than about the graph. Everything this addendum built stays: **the columns, the three
> sort keys and the reach computation are untouched**, so "what does the most funnel into this" is
> one header-click away. It stopped spending rail, and stopped competing for a word it was never
> quite entitled to — this document's own §"the words" section had already noticed the risk, warning
> that a chip must not "be read as a measured frequency — this counts paths through the model, not
> swings observed". Prominence is the measured-frequency claim that warning was holding the space
> for.

**All three sort keys are attached to both row types regardless**, so either list can be sorted or
faceted on any of them without a column being drawn for it. That is established precedent — `axis` is
already a sort key with no column (`model_browser.cpp:876`).

**The Causes default sort changes** from `explains` to `outcomes` descending, then `leadsTo`. This is
the fix, not a side effect: leaving the default on direct out-degree would mean shipping the ranking
this addendum exists to correct. Characteristics keeps its group → axis → label default, which encodes
the order of the swing and is deliberate.

---

## The quantile facet

The rail's existing facets are **closed vocabularies**: `facets()` picks a column, reads the cell text
of every row in it, and offers each distinct value as a chip with a count. `rows()` then filters by
matching that same cell text (`model_browser.cpp:1448`) — one source for the count and the filter, so a
chip can never say twelve and return nine. That invariant is asserted
(`model_browser_test.cpp`: *"every facet chip returns exactly the number it advertises"*) and must
survive this change.

Reach is a number, not a vocabulary, so it needs a second kind whose chips are cut from **the
distribution of the data as it currently stands**:

| Bucket | Range | Example label |
|---|---|---|
| `top` | `v ≥ p90` | Very common (28+) |
| `high` | `p75 ≤ v < p90` | Common (12–27) |
| `rest` | `0 < v < p75` | Less common (1–11) |
| `none` | `v == 0` | Nothing leads here |

Five decisions, each load-bearing:

1. **Cuts from the distribution, never hardcoded.** A chip reading "≥ 5" is reasonable over 140
   conditions and meaningless over 400 — either empty or half the list. A quantile chip keeps meaning
   *"the ones that stand out in this pack"*, and it re-cuts as the author edits. That is the whole
   reason this is not simply a numeric column you can sort.
2. **Zero is its own bucket, always.** "Nothing leads here" is a category, not a low score, and it is
   where an author goes to read the 20 characteristics nothing converges on. Note that the *cell* for
   a zero is dimmed and never warned: a genuine root cause is supposed to have nothing above it, and
   it is indistinguishable at the cell from a characteristic whose causes nobody has authored yet.
   Colouring both as a fault would badge every legitimate root in the pack. Deciding which is which is
   what the bucket is for.
3. **Percentiles by nearest rank, no interpolation.** Interpolated percentiles produce fractional cuts
   over integer counts, and a chip reading "≥ 11.75" is a chip nobody can act on.
4. **Every label states its own cut**, in brackets after the word. A chip that will not say its
   threshold is one the author cannot argue with, and they will want to argue with it. The bracket
   does a second job — see *Wording* below.
5. **Empty buckets are dropped, and a facet left with fewer than two buckets is dropped whole** — the
   rule `facets()` already applies to vocabularies ("a facet with one value filters nothing",
   `model_browser.cpp:1633`). A degenerate distribution therefore offers no chips rather than four
   useless ones.

### Wording

The names are the ones a coach already uses, not the ones the arithmetic suggests. Percentile
vocabulary is gone from the UI entirely: nobody ranks a swing fault by decile, and a chip reading
"Top tenth" made the reader translate before they could use it.

| Surface | Reads |
|---|---|
| Causes › facet on `outcomes` | ~~Most common faults~~ → **Bad shots it reaches** — A lot (8+) · Some (5–7) · A few (1–4) |
| Causes › facet on `leadsTo` | **Knock-on effects** — A lot (19+) · Some (12–18) · A few (1–11) |
| ~~Characteristics › facet on `causedBy`~~ | **REMOVED** — see the note above; `prominence` answers "how common" on both lists |
| Characteristics › column `causedBy` | **Faults** — the count of faults that eventually produce this. Kept, with its sort key |

Two decisions inside that:

- **The bucket words are per facet, not one ladder reused.** "Very common" is right for a facet
  ranking how much of the model converges somewhere and wrong for one ranking how much a fault drags
  along with it — that second question is about *breadth*, and answering it in the language of
  frequency would state something the count does not.
- **The numeric range stays in every chip**, and this is not only about being arguable. These counts
  are **paths through the model, not swings observed**. "Very common" is the honest reading of what
  the graph asserts, and it is a modelled commonality rather than a measured one; the bracketed range
  keeps a reader anchored to a count and stops the word being taken for a frequency. When the
  empirical version lands (see below) it will be a *different* facet with a real denominator, and the
  two must be distinguishable on sight.

### How the count-equals-filter invariant is kept

The bucket bounds travel **inside the option map** (`lo`, `hi`, `hasHi` alongside `value`, `label`,
`count`), and `rows()` obtains them by calling `facets()` — it does not re-derive them. One computation
feeds both the advertised count and the applied filter, by construction rather than by two functions
being kept in step. QML ignores the extra keys; `ModelTypeRail.qml` binds only `value`, `label` and
`count`, so **the rail needs no change at all**, and neither does the collapsed-rail chip row, which
reads its labels from the same spec (`DiagnosticModel.qml:153`).

Quantile facets read the **sort key**, not the cell text, which is also what lets a facet exist for a
number no column draws.

### They go first in the rail, and the rail had to grow to hold them

Appended *after* the vocabularies they were invisible, and this is worth recording because no amount
of ordering argument reaches it: `ModelTypeRail.qml` scrolls its facet list inside what was a fixed
`Theme.sp(300)`, `group` alone is nine values, and **the third facet was already below the fold before
this change**. A scroll area with nothing indicating it continues reads as a list that has ended, so
rows past the cap were not merely awkward to reach — nobody knew they existed.

Two changes, both in the rail:

- The numeric facets are **emitted first**, because ranking is what an author now opens this rail for.
- The facet list takes **the height it is asked for up to what it needs** (`fillHeight` bounded by
  `facetColumn.implicitHeight`, floored at `Theme.sp(140)`) instead of a fixed cap, and the spacer
  above it yields rather than taking every spare pixel. On a tall window every facet is simply on
  screen.

Unrelated and still true: below `Theme.sp(1225)` of panel width the rail is dropped whole and only
*active* filters appear, as chips. There is no way to add one at that width — `BRIEF.md` calls for a
filter popover there and none was built. Out of scope here, but it is the other reason an author can
report seeing no filters.

### What the shipped pack yields

| Facet | rows | max | p75 | p90 | top | high | rest | none |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Causes › Leads to | 95 | 46 | 12 | 19 | 11 | 13 | 71 | — |
| Causes › Bad shots | 95 | 13 | 5 | 8 | 10 | 20 | 65 | — |
| Characteristics › Fed by | 115 | 38 | 12 | 28 | 10 | 14 | 71 | 20 |

Ten to twenty rows in the top buckets out of ninety-five is the size an author can actually read,
which is the point of the whole exercise.

---

## Performance

`causalClosure()` scans the full edge list per frontier node (`characteristic_pack.cpp:351` states this
is deliberate — building an adjacency map would cost the walk it saves). Over 276 edges and 140
conditions, three closures per row is roughly 3.5M edge visits per `rawRows()` call, and `types()`,
`rows()` and `facets()` each trigger one.

So the triple is **memoised per condition id, in a mutable cache cleared by `invalidateDerived()`** —
the same shape `m_dirtyIds` already uses (`model_browser.h:669`), hung off the same invalidation point,
so an edit cannot leave a stale count on screen.

---

## Deliberately not in this addendum

**Empirical prevalence — how often a condition actually fires across the swing library.** It is the
honest answer to "slice is common", and this addendum ships a structural *proxy* for it (`causedBy`:
how much of the model converges on an outcome) precisely because the real thing is not available yet.

It is out of scope because **the model is not executed anywhere**. `characteristic_engine.h:34` states
it plainly: "NOT WIRED INTO THE LIVE ANALYSIS PATH… surfacing findings on a real swing is a separate
change." A prevalence facet would be a filter over numbers nothing produces.

When it does land, the shape is already visible and should be followed rather than reinvented:

- `scanCorridor()` (`model_browser.cpp:5897`) already walks every athlete/session/swing folder on a
  `QFutureWatcher`, reading the sidecar-cached phase grid. It reduces **one** measure; the missing
  piece is a small `IMeasureValueSource` over a `SwingPhaseGrid`, after which `NormMeasureSource`,
  `detect()` and `explain()` run per swing with no new analysis code.
- The denominator must be **swings where the condition was assessable**, never swings scanned. The
  engine's founding rule is "an absent measure is unavailable, never a pass"
  (`characteristic_engine.h:37`), so the facet needs three buckets — **Common · Rare · Rarely
  assessable** — and collapsing the third into either of the first two would reintroduce exactly the
  false negative dressed as a clean bill of health that the engine was written to prevent.
- `Explanation::unexplained` accumulated across the library is a free artefact of that scan: faults the
  app detects and the model has nothing to say about. That is a work queue on a par with the roadmap.
- The facet must report its own staleness — swings scanned, when — the way `corridorScanned` reports
  which measure its samples belong to. A facet quietly showing numbers from a scan three edits ago is
  worse than one greyed out with a reason.

The interesting output is then the **disagreement** between the structural and empirical rankings: high
reach that never fires is content that has never paid off; fires constantly with low reach is a fault
the model cannot explain. Neither is visible from either number alone.

---

## Verification

Per `docs/developer/diagnostics_developer_guide.md` — every new sort key, filter and façade rule gets a
case in `model_browser_test`, "or it is asserted nowhere":

- transitive reach exceeds direct coverage where the pack has a chain, with `Settled tempo habit` named
  as the regression case — it is the row the old ranking buried.
- `outcomes ≤ leadsTo` for every row, and every counted outcome really is in the BallFlight group.
- the Causes list's first row under the default sort is a top-bucket row, not a high-out-degree leaf.
- **the quantile invariant**: every quantile chip returns exactly the number it advertises — the same
  assertion the vocabulary facets already carry, extended to the new kind.
- bucket bounds do not overlap and cover the population.
- an edit that adds a causal edge changes the counts (the memo is invalidated, not stale).

Then `core_pack_test`, `diagnostics_catalogue_integrity_test`, `inspector_refresh_test`.
