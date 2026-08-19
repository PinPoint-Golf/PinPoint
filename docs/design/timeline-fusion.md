# Timeline fusion: arbitrating the phase timeline when both the camera and the IMU testify

Written 2026-08-19 against `7e62369`. An investigation the same day into how the IMU and
camera phase models are reconciled — prompted by relabelling the chart axis to the
coaching P-system, which made the labels checkable — established the fact this document
starts from: **the published phase timeline is not fused — one producer wins wholesale,
and the other back-fills only the slots the winner left empty.** On an IMU-bound swing a
`conf 0.35` hand-orientation proxy displaces the camera's measured P6 on every swing, a
`conf 0.20` window-edge clamp wears the P10 label, and the code point that decides never
reads a confidence. This document folds that diagnosis in (Part I) and then designs the
mechanism that decides properly (Part II onward). An appendix gives the recipe for
reproducing every number here from `swing.json` alone.

---

# Part I — the diagnosis

## 1. The two witnesses, and how they combine today

Two producers locate coaching P-positions independently:

| | IMU model | Camera (club-track) model |
|---|---|---|
| Code | `phase_segmenter.cpp` (`PhaseSegmenter::segment`) | `shaft_positions.h` `locatePTimes`, assembled in `shaft_track_assembly.cpp` |
| Sees | fused accel + gyro on the lead forearm/hand | per-frame shaft θ(t) and lead-arm φ(t) in the image plane |
| Emits | P1, P3–P10 + Takeaway/Transition/MaxSpeed as `Segmentation` events | P1–P8, P10 as `analysis.club.positions[]` (P9 deferred) |
| Missing | **P2** (no `ShaftParallelBack` in its ladder) | P9 |

They meet at three code points (`wrist_analyzer.cpp`), and it is worth seeing the actual
code, because the design's whole argument is that the decision is made without looking:

**1. The camera's phase model is only CAPTURED when there is no IMU.** `ShaftStage` hands
the tracker a trace pointer only in the no-IMU case, so on an IMU-bound swing the vision
phase model is never even built (`wrist_analyzer.cpp:511`, `:516`):

```cpp
ctx.detail->shaft = ShaftTracker::track(..., ctx.hasImuStreams() ? nullptr : &strace);
...
if (!ctx.hasImuStreams()) ctx.segVision = strace.segmentation;
```

(The club's P-positions are a separate product and ARE still computed and persisted —
they are what the evidence below is measured against.)

**2. `SegResolveStage` (stage 8) picks one segmentation, whole** — no blending, no
per-event choice (`wrist_analyzer.cpp:527`):

```cpp
ctx.seg = ctx.segImu ? *ctx.segImu
        : (ctx.segVision && ctx.segVision->conf > 0.f ? *ctx.segVision : Segmentation{});
```

**3. `PositionsLadderStage` (10c) back-fills club P-positions into EMPTY slots only.**
It maps club P2→`ShaftParallelBack`, P3→`MidBackswing`, P5→`ArmParallelDown`,
P6→`Delivery`, P8→`ShaftParallelThrough` — and refuses any slot already occupied
(`positions_ladder.h:121`):

```cpp
if (seg.eventFor(m->phase)) { ++r.duplicate; continue; }
```

The header states the rule plainly: *"an IMU-path proxy wins — no arbitration, no
confidence-priority merge."* P1/P4/P7/P10 are never offered at all, because the anchors
already exist and `Segmentation::eventFor` takes the first match, so a duplicate would
shadow. (In between, `EventRefineStage` (10b) retimes Takeaway/Address from the club
track — but its `canRun` requires `!ctx.segImu.has_value()`, so it too never runs on an
IMU-bound swing.)

So on an IMU-bound swing the selection rule is, in full: **whoever got there first, where
"first" means "was an IMU bound".** A `conf 0.35` proxy beats a `conf 0.80` measurement.
The camera's measurements are computed, persisted — and discarded.

## 2. The evidence — eleven 2026-08-18 wG3 swings

These are the only swings with both witnesses (IMU-bound with a face-on camera and a
valid shaft track). Δ = the published timeline's timestamp − the club's own measurement
of the same position, in milliseconds; positive = the published event is later. All read
straight from `swing.json` (recipe in the appendix).

```
swing              P1      P2      P3      P4      P5      P6      P7      P8     P10
Wrist_01/0001  -120.7    +0.0   +31.2    +5.6    +3.5   -42.3    -0.5   +95.8 +1312.3
Wrist_01/0002  -103.7    +0.0   +31.8    -8.2    +3.6   -44.6    +3.9   +85.3 +1361.0
Wrist_01/0003   +39.4    +0.0   +31.3  +166.2    +5.5   -60.5    +1.9  -668.6       –
Wrist_01/0004   +25.3    +0.0   +29.4    +3.9    +6.4   -42.9    +1.8   +97.4 +1351.1
Wrist_01/0005  -123.8    +0.0   +27.0    -0.2    +4.8   -44.0    -2.7   +92.7 +1301.2
Wrist_02/0001  -108.3    +0.0   +13.1    -8.6    +6.9   -45.4    +2.2   +92.3 +1747.2
Wrist_02/0002   -96.9    +0.0   +15.4    -4.4    +4.9   -38.8    -2.4   +91.1 +1666.8
Wrist_02/0003   +26.9    +0.0   +14.5    -3.3    +5.7   -44.4    -2.7   +84.0 +1719.2
Wrist_02/0004   +53.8    +0.0   +11.0    -0.7    +5.9   -43.4    +1.8   +99.9 +1681.3
Wrist_02/0005  -122.4    +0.0   +13.3    -6.6    +6.5   -46.8    +2.0   +84.2 +1677.7
Wrist_02/0006   +35.3    +0.0   +14.0    -2.7    +5.5   -45.1    +2.4   +89.4 +1704.2
```

| P | median Δ | published source | published conf | club conf |
|---|---|---|---|---|
| P1 | −97 ms (bimodal) | IMU stillness **fallback** | 0.30 | 0.72–0.78 (stack fit) |
| P2 | 0 | club (the only inserted slot) | 0.55 | 0.55 |
| P3 | +15 ms | IMU forearm crossing | 0.70 | 0.55 |
| P4 | −3 ms | IMU hand/forearm vote | 0.86 | 0.75–0.80 |
| P5 | +6 ms | IMU forearm crossing | 0.70 | 0.55 |
| P6 | **−44 ms** | IMU hand-orientation **proxy** | 0.35 | 0.55 |
| P7 | +2 ms | acoustic anchor (both sides) | 1.00 | 0.45 |
| P8 | **+91 ms** | IMU forearm **proxy** | 0.35 | 0.55 |
| P10 | **+1672 ms** | IMU window-edge **clamp** | 0.20 | 0.65 |

### The four defects, position by position

**P6 — a proxy displacing a measurement, consistently 44 ms early.** The segmenter
builds Delivery from lead-hand inclination and caps its confidence at 0.35, its own
comment calling it a *"hand-orientation PROXY until the shaft refinement"*. That proxy
occupies the slot, so the club's measured P6 is discarded as a `duplicate` on every one
of the eleven swings. The effect is not noise — it is a consistent −39 to −61 ms bias
that collapses the published P5→P6 interval to ~14 ms where the club says ~64 ms.
Lead-arm-parallel and shaft-parallel in the downswing are not 14 ms apart. This is the
same club P6 that `d0c9ff2` tuned to ≤5 ms against truth on 11/11 truth swings; that
work is discarded on every IMU-bound swing by a rule that never looks at it.

**P8 — the same shape, larger and less stable.** A `conf 0.35` forearm proxy, +91 ms
median, and on `Wrist_01/0003` it lands 669 ms on the other side. The club's P8 is
available on all eleven and is never used.

**P10 — labelled, and effectively unmeasured.** The published Finish is the segmenter's
literal window-edge fallback (`Cand finish{ true, grid.back(), 0.2f }` — the comment
reads *"window-edge clamp, visibly so"*). It sits ~1.7 s past the club's finish and is
missing entirely on one swing. The clamp is honest inside the code — the confidence says
so — but nothing downstream reads that confidence before drawing the "P10" label.

**P1 — a documented twin that is not a twin here.** `event_refine.h` claims Address and
the club's P1 are *"two copies of the SAME instant, born together from
addressHoldEndFrame"*. On an IMU-bound swing they are born apart: Address comes from the
segmenter's stillness detector, P1 from the club's stack fit, and they sit ~97 ms apart
— **bimodally**: −97 to −124 ms on six swings, +25 to +54 ms on five. The design probe
sharpened this beyond the original investigation: the published Address is the
segmenter's conf-0.30 *fallback* ("continuous waggle" — Address co-timed to Takeaway) on
**all eleven** swings, and the Takeaway's own gate-shaped confidence separates the two
regimes perfectly (conf ≈ 0.81 on every −100 ms swing, conf ≈ 0.47 on every +25/+54 ms
swing). Two detector regimes, both self-labelled — nothing downstream reads the labels.

### What is working (and must not be broken)

- **P7** agrees to ±4 ms — both sides anchor on the acoustic impact, by design.
- **P4** agrees to a few ms on ten of eleven; the +166 ms outlier is `Wrist_01/0003`,
  the same swing that produced the P8 outlier and lost its P10 — one bad swing, not a
  bad position.
- **P5** is tight and consistent (+3.5 to +6.9 ms across all eleven), P3 nearly so.
- **P2** is exact by construction — it is the club's own value, inserted.
- The ladder's abstain rules do their job: insertion is monotone, bounded and counted,
  and an all-abstain pass leaves the segmentation byte-identical. The design keeps this
  discipline and extends it to replacement.

### A truth-graded check (added 2026-08-19, after markup)

All eleven swings were marked up with a full P1–P10 truth ladder the same day this
design was written, which upgrades the evidence above from "the two producers disagree"
to "and truth says which one is right". The six `Wrist_02` swings carry hi-res wrist
data and are the stratum representative of production capture; the five `Wrist_01`
swings carry lower-rate wrist data and serve as the stress stratum. First the hi-res
six — error vs truth in ms (score.py P_CHECKS convention, positive = later than truth):

| P | published (occupancy winner) | club positions | reading |
|---|---|---|---|
| P1 | +32 median, −68..+183 | +70 median, +34..+147 | both messy; the IMU's two regimes visible again |
| P2 | −9 | −9 (same value) | fine |
| P3 | −21 median | −36 median | **IMU closer on all six** — arm ownership confirmed |
| P4 | −18, spread −54..+24 | −13, spread −47..+27 | both mediocre, and their errors move *together* per swing — Top is genuinely fuzzy against a human mark |
| P5 | +21 median | +15 median | club slightly closer on all six (~6 ms) — see §4.4 |
| P6 | **−39 median** | **+6 median, worst 8** | camera ownership confirmed at truth grade |
| P7 | +2 | −0 | the anchor is the anchor |
| P8 | **+96 median** | **+7 median, worst 12** | camera ownership confirmed |
| P9 | absent | absent | **neither producer emitted P9** on any graded swing, despite the segmenter having a FollowThrough member — an emission gap, tracked separately from fusion |
| P10 | **+1686 median** | **−0 median** | the clamp is not a finish; the club's is |

The low-res `Wrist_01` stratum (five swings) tells the same story, with two additions.
The same story: P6 published −39 ms vs club ≤+24 ms; P8 published ~+99 ms vs club
≤+15 ms; P10 published +1.3 s clamp vs club within −93..−13 ms; P5 club closer on all
five (+13 vs +19 ms — making it 11/11 across both strata); P2 within −15..+2 ms; P1
messy on both sides. The first addition: **the lower IMU rate did not degrade the IMU's
measured phase timing** — its P3 (−7 ms median) and P4 (−1 ms median) are actually
*tighter* than the hi-res session's, so the concern that this stratum misrepresents the
IMU side applies to wrist-angle fidelity, not to event timing, and the stratum can grade
phase arbitration honestly.

The second addition is the most instructive single swing in the corpus.
**`Wrist_01/0003`, the degenerate, decomposes exactly along ownership lines**: at P8 the
IMU proxy is −667 ms against truth while the club is +1 ms — the huge P8 disagreement
was entirely the proxy's error, and the camera should win it *despite* the 669 ms gap.
At P4 it is precisely reversed: the club is −167 ms against truth while the IMU is
−1 ms — and ownership retains the IMU there. One broken swing, one broken slot per
witness, and the ownership table calls both correctly. This swing also killed an earlier
draft of the decision rule (see §4.3): a "keep the incumbent when the witnesses disagree
wildly" fail-safe would have kept the 667 ms-wrong proxy.

Conclusions, stated plainly. Every V1 flip (P6, P8, P10) has truth-grade support on
IMU-bound swings in both strata — previously the club P6's ≤5 ms record came from
camera-only truth swings, and it holds here. The two contested retentions split: P3
confirms ownership (the IMU beats the club by ~14–32 ms consistently, both strata), P5
gently contradicts it (the club is ~6 ms closer on all eleven swings). Eleven swings
from one golfer and one day decide P5 finally in neither direction, but they are exactly
the calibration data the V2 σ path (§9) needs.

Three things to notice in the evidence, because the whole design falls out of them:

- The IMU's failures are not noise — they are *labelled*. Every bad slot carries a
  confidence the producer itself capped to say "this is not a measurement" (0.35 proxy,
  0.30 fallback, 0.20 clamp). The information needed to lose gracefully is already there.
- The IMU's failures cluster on **shaft-defined** positions (P6/P8 = shaft parallel,
  P10 = club comes to rest). Its arm-defined positions (P3/P5 = lead arm parallel) are
  tight, because a forearm IMU measures arm elevation directly.
- One swing (`Wrist_01/0003`) is degenerate on both sides (P4 +166 ms, P8 −669 ms IMU-vs-club, P10
  missing). Any rule we write must fail safe on that swing, not just win on the ten
  good ones.

---

# Part II — the design

## 3. Why "prefer the higher confidence" is the wrong mechanism

The obvious fix — replace the occupancy test with `candidate.conf > incumbent.conf` — was
considered and rejected, and future maintainers should understand why before reaching for
it again.

**The confidences are not commensurable.** Four distinct scales share the one
`PhaseEvent.conf` float:

| Scale | Producer | What the number actually is |
|---|---|---|
| S1 | `phase_segmenter.cpp` | hand-authored per-detector base × a duration-prior gate (`gateConf`), range 0.2–0.98 |
| S2 | club positions via `shaft_track_assembly.cpp` | the **per-frame club-detection tier** sampled at the P instant: BAND ≈0.75–0.9, RAY 0.55, WEDGE 0.45, RECON 0.40, PRED 0.30 — or the B2 fit's ridge support when a milestone fit accepted |
| S3 | `phasesToSegmentation` (vision ladder) | a flat 0.5 meaning "a swing was detected" |
| S4 | `event_refine.cpp` | a three-tier at-ball evidence score, 0.30–0.90 |

S2 is the killer: the club's `conf` describes *how well the club was seen in that frame*,
and carries **zero information about how well the crossing was located in time**. A
sub-frame-precise P6 on a clean hysteresis-confirmed transit carries 0.55 because the
shaft happened to be ray-detected rather than band-locked. Comparing S1 to S2 is
comparing "how central was this event in its expected duration window" to "was the club a
band or a blob". The ordering that comparison produces is close to arbitrary — it happens
to give the right answer at P6/P8/P10 with today's constants, which makes it a **static
priority table in disguise**, and a fragile one: retune any producer's constants and the
timeline silently re-arbitrates.

There is also a consumer that treats `conf` as calibrated: `score_uncertainty.cpp:106`
inflates the score interval by `1 + (1−conf)·kConfInflate` at Top and Impact. Arbitration
must not launder one scale's numbers through another scale's slot, or published score
intervals change meaning as a side effect.

So: `conf` stays what it is documented to be — a per-producer, display-oriented quality
hint. The arbitration decides on two things that *are* comparable across producers,
because we define them to be: **measurement class** and **estimand ownership**.

---

## 4. The design

Three ideas, layered:

1. **Estimand ownership** — each P-position is *defined* by a specific observable, and
   the instrument that observes that quantity directly is the preferred witness.
2. **Measurement class** — each candidate declares whether it is a measurement, a proxy,
   or a fallback. Producers already know this about themselves; today they whisper it
   through magic conf constants. We make it a typed field.
3. **Guarded replacement** — the winner takes the slot only under the same monotonicity
   and window guards the positions ladder already enforces for insertion, plus a
   disagreement cap. On any doubt, the incumbent stays and the doubt is counted.

### 4.1 Estimand ownership

The P-system defines each position by an observable, and our two instruments differ in
how directly they see each one:

| P | Defined by | Preferred witness | Why |
|---|---|---|---|
| P1 | address stillness ends | camera (club track) | grip stillness walk-back + stack fit measures the club at rest; the IMU stillness detector fell back to "continuous waggle" on 11/11 wG3 swings |
| P2 | **shaft** parallel, backswing | camera | only the camera sees the shaft; the IMU has no P2 at all |
| P3 | lead **arm** parallel, backswing | IMU (forearm) | the forearm IMU measures arm elevation directly; the camera infers φ from pose keypoints |
| P4 | top of swing (reversal) | IMU | hand/forearm downswing-run vote + thorax cross-check; empirically ±5 ms of the club's own P4 |
| P5 | lead **arm** parallel, downswing | IMU (forearm) | as P3; empirically +3.5 to +6.9 ms vs club across all 11 |
| P6 | **shaft** parallel, downswing | camera | the IMU proxy is hand orientation, self-capped at 0.35; the club's P6 was tuned to ≤5 ms vs truth (`d0c9ff2`) |
| P7 | impact | acoustic anchor | inviolable (marker contract); both producers already anchor on it, agreement ±4 ms |
| P8 | **shaft** parallel, follow-through | camera | as P6; the IMU proxy runs +91 ms with a −669 ms outlier |
| P9 | lead **arm** parallel, follow-through | IMU | the camera defers P9 (`shaft_positions.h:439`); no conflict exists |
| P10 | swing ends (club/body at rest) | either **measured** one | the IMU's quiet-run detector is a legitimate finish when it finds one; its window-edge clamp is not a measurement at all |

The empirical table in §2 is this table's confirmation: every large IMU error sits on a
camera-owned row, and every tight IMU agreement sits on an IMU-owned row. Ownership is
not a heuristic we hope generalises — it is the physics of which sensor touches which
quantity, and the corpus already voted for it.

### 4.2 Measurement class

A small enum, carried by every candidate the arbiter sees:

```cpp
// How an event's TIME was obtained. Orthogonal to conf (which stays the
// producer-local quality hint it always was). Persisted append-only.
enum class TimingClass {
    Anchor,     // externally anchored (acoustic impact). Never displaced.
    Measured,   // located from a direct observation of the defining quantity
    Proxy,      // located from a correlated stand-in (the producer says so itself)
    Fallback,   // a clamp or default; the time is a placeholder, not an estimate
};
```

Producers label their own emissions — no inference from conf magnitudes:

- `phase_segmenter.cpp` already knows at every emission site: Impact → `Anchor`;
  Address/Takeaway/Finish found → `Measured`, their fallbacks → `Fallback`; P6 and P8 →
  `Proxy` (its own comments call them proxies); P3/P5/P9/Top/Transition/MaxSpeed →
  `Measured`.
- The club path labels by how the *time* was located: hysteresis-confirmed crossing or
  anchored milestone on measured θ samples (BAND/RAY/WEDGE tier at the crossing) →
  `Measured`; a crossing resolved on predicted/reconciled θ (PRED/RECON tier) → `Proxy`;
  the P1 stack fit → `Measured`.
- `phasesToSegmentation` (the vision hands-only ladder) labels its pm-frame events
  `Measured`, since they come from located phase-model frames.

The class ordering is absolute: `Anchor` is never displaced; `Measured` beats `Proxy`
beats `Fallback`. Ownership (§4.1) breaks `Measured` vs `Measured`. Ties — same class,
same owner, or equal times — retain the incumbent, because stability is worth more than
an unmeasurable improvement.

Why class-plus-ownership rather than a numeric score: the decision becomes *categorical
and auditable*. Every arbitration outcome can be stated in one sentence ("club P6
replaced the IMU proxy: Measured beats Proxy on a camera-owned slot") and reproduced by
hand from the persisted fields. Numeric fusion — real timing uncertainty in microseconds
— is the V2 path (§9), and it should only take the wheel after it has been calibrated
against truth, not before.

### 4.3 The decision rule

For each P-slot, the arbiter gathers up to two candidates — the incumbent event in the
resolved segmentation (if any) and the mapped club position (if any) — and decides:

```
1. No candidates .......................... slot stays empty (nothing to say)
2. One candidate .......................... insert if the ladder guards pass
                                            (today's insert-if-monotone, unchanged)
3. Incumbent is Anchor .................... incumbent stays (P7)
4. Classes differ ......................... higher class wins
5. Classes equal (both Measured) .......... the owner (§4.1 table) wins
6. Winner ≠ incumbent ⇒ guards:
   a. new time strictly inside the slot's anchor window (P2/P3 in Address..Top,
      P5/P6 in Top..Impact, P8/P10 after Impact) ......... else keep incumbent
   b. new time strictly between its retained time-neighbours in the event
      list (Takeaway, Transition, MaxSpeed included) ..... else keep incumbent
   c. |new − incumbent| ≤ the slot's dispute cap, ONLY when the incumbent is
      itself Measured ................................... else keep incumbent,
                                                           count `disputed`
      (a Proxy or Fallback incumbent gets no cap — see below)
7. Replacement = retime the EXISTING slot in place: t_us, conf, provenance,
   class all become the winner's. Phase identity, event order discipline, and
   the swing bounds are untouched.
```

The dispute cap (6c) deserves its history, because truth data killed its first draft.
The draft rule capped *every* replacement: on `Wrist_01/0003` the club P8 sits 669 ms
from the IMU proxy, and the draft reasoned that when two witnesses disagree by more than
any plausible physiology, the swing is broken and the incumbent should stay. Then the
markup arrived and said the opposite: on that swing the club's P8 is +1 ms against truth
and the proxy alone is the wreck — the cap would have preserved a 667 ms error. The
corrected reasoning: a proxy's disagreement with a measurement is evidence against the
*proxy* — the class field already says its time is a stand-in, so no magnitude of
disagreement rehabilitates it. The cap therefore applies only between two `Measured`
candidates (none exist in V1's replacement set; P1 in Phase 2 is the first real user),
where a wild gap genuinely means one instrument broke and there is no class signal to
say which. Caps come from the segmenter's duration priors, are dotted-key tunable, and
are generous — they catch a different-swing level of disagreement, never a consistent
bias. Whatever the outcome, the disagreement itself is always counted and persisted
(`disputed`, and the per-slot Δ in the decision log): the guard against a junk
*measurement* winning a slot is the window/neighbour monotonicity of 6a/6b plus the
corpus gate, not a cap that punishes the honest witness for the broken one's error.

Note what the rule does **not** do: it never averages, never nudges, never invents a
compromise timestamp. Every published time is some instrument's actual estimate, with
that instrument's provenance and confidence attached. A timeline whose events are
traceable beats one that is occasionally a few milliseconds closer.

### 4.4 What V1 actually changes, slot by slot

Applying §4.3 with today's producers on an IMU-bound swing:

| P | Incumbent (class) | Club candidate (class) | V1 outcome |
|---|---|---|---|
| P1 | IMU fallback (`Fallback`) | stack fit (`Measured`) | **held dark in V1** — see below |
| P2 | — | crossing (`Measured`) | inserted (unchanged behaviour) |
| P3 | IMU crossing (`Measured`) | crossing (`Measured`) | IMU retained (owner); club Δ logged |
| P4 | IMU vote (`Measured`) | anchored (`Measured`) | IMU retained (owner); club Δ logged |
| P5 | IMU crossing (`Measured`) | crossing (`Measured`) | IMU retained (owner); club Δ logged — truth grades the club ~6 ms closer (§2), so this retention is cheap but not free; first candidate for a V2 flip |
| P6 | IMU proxy (`Proxy`) | crossing (`Measured`) | **club replaces** (~ +44 ms) |
| P7 | acoustic (`Anchor`) | crossing (`Measured`) | anchor retained, always |
| P8 | IMU proxy (`Proxy`) | crossing (`Measured`) | **club replaces** (~ −91 ms) — uncapped: truth graded the club ≤15 ms even on the swing where the proxy was 667 ms out |
| P9 | IMU crossing (`Measured`) | — | unchanged (sole witness on paper — in practice absent from all six truth-graded ladders; an emission gap outside fusion's scope) |
| P10 | IMU clamp (`Fallback`) | anchored (`Measured`) | **club replaces** (~ −1.7 s) |
| P10 | IMU quiet-run (`Measured`) | anchored (`Measured`) | IMU retained (both measured; either is a legitimate finish, stability wins) |

On a camera-only swing nothing changes: the vision ladder carries no interior P events,
so the interior slots are pure insertions exactly as today, and the anchors it does carry
(Address/Top/Impact/Finish) come from the same phase-model frames the club positions
anchor to — same instant, tie, incumbent retained. **The camera-only corpus is therefore
expected byte-identical with fusion ON**, and the gate (§8) verifies that expectation
rather than assuming it.

**P1 is implemented but ships dark** (`refine.fusionP1` default false). The class rule
already gives the right answer — the IMU Address was the `Fallback` on all eleven wG3
swings while the club P1 is a real measurement — but P1 has a blast radius the other
slots don't: Address is the reference instant for tempo, every Address-referenced pose
metric, the replay trim (`disk_replay_source.cpp` trims on phase 0), and the
Address/Takeaway co-timing inside the segmenter. It also drags the `event_refine.h` "twin"
contract with it (Address and club P1 are documented as *"two copies of the SAME
instant"*, which is false on the IMU path — they sit ~97 ms apart, bimodally). P1
arbitration is Phase 2, with its own gate: `fidget_eval.py` truth errors, the tempo band,
and a replay-trim eyeball. Fixing or correcting the `event_refine.h` twin comment happens
then too.

---

## 5. Mechanism — where this lives in the code

The arbiter is the positions ladder, grown up. Same pipeline slot, same purity contract,
same abstain discipline — the occupancy test at `positions_ladder.h:121` becomes the
decision rule of §4.3.

**New header `src/Analysis/timeline_fusion.h`** (pure over value types, no SwingWindow,
no AnalysisContext, unit-testable standalone — the `event_refine.h` /
`positions_ladder.h` contract):

```cpp
struct TimelineFusionConfig {
    bool enabled   = tuned::refine::kFusion;     // refine.fusion
    bool p1        = tuned::refine::kFusionP1;   // refine.fusionP1 (Phase 2, dark)
    int  disputeMs = ...;   // refine.fusionDisputeMs — Measured-vs-Measured only,
                            // so unused until Phase 2 (P1) exercises it
    // fromOverrides(ov) via tuning::apply, as everywhere else
};

struct FusionDecision {         // one per contested slot, for the log + persistence
    Phase       phase;
    SegmentRole winner, loser;  // loser == Unknown for uncontested inserts
    int64_t     deltaUs;        // winner − loser (0 when uncontested)
    uint8_t     reason;         // class-beat / owner-held / disputed / guard-abstain...
};

struct TimelineFusionResult {
    bool emitted = false;       // anything inserted or replaced (⇒ seg.version = 5)
    int  inserted = 0, replaced = 0, retained = 0, disputed = 0, abstained = 0;
    std::vector<FusionDecision> decisions;
};

TimelineFusionResult fuseTimeline(Segmentation &seg,
                                  const std::vector<ShaftPosition> &positions,
                                  const TimelineFusionConfig &cfg);
```

With `cfg.enabled == false` the stage is *skipped* (gate in `canRun`, the house pattern),
and `PositionsLadderStage` runs exactly as today — code-path-identical, the byte-parity
baseline. With fusion ON, `TimelineFusionStage` replaces `PositionsLadderStage` at
pipeline slot 10c (after EventRefine, before BindDetail); insertion into empty slots is
just arbitration against an absent incumbent, so `emitPositionsLadder`'s behaviour is a
strict subset of `fuseTimeline`'s and the old function retires when the flag freezes ON.

**Type changes** (all append-only, mirroring how `provenance` was added):

- `PhaseEvent` gains `TimingClass timing` (persisted as an int field on
  `analysis.phases[]`; absent on old files ⇒ `Measured` is assumed on reload, which is
  only ever read for display).
- `ShaftPosition` gains the same field, set at assembly time from the sample tier at the
  located instant.
- `phase_segmenter.cpp` sets it at each emission site (it already knows which branch it
  took; this converts comments into data).
- `Segmentation.version = 5` ⇒ "fusion arbitrated" (append-only; `version` currently has
  no logic readers, only a diagnostics row and tests).

**Persistence of decisions**: the `FusionDecision` list is persisted compactly as
`analysis.segmentation.fusion[]` — `{p, winner, loser, dtUs, reason}`. This is cheap, and
it matters: the Part I diagnosis was conducted entirely from `swing.json`,
and the *retained*-slot deltas (club P3 +15 ms, P4/P5 corroborations) are precisely the
calibration data V2 needs. The arbitration that didn't happen is as much evidence as the
one that did. One `ppInfo` log line summarises counts, as the ladder does today.

**Invariants the mechanism must preserve:**

- `Impact` is never retimed or displaced, under any configuration (marker contract).
- `swingStartUs`/`swingEndUs` are untouched — fusion moves labels, never bounds, so
  export encode spans, replay spans, and heavy-stage scan windows cannot change. (The
  disk-replay *trim* reads the Finish event itself, so a P10 replacement legitimately
  ends the replay ~1.7 s earlier — at the actual finish instead of the window edge. This
  is intended, and it is on the gate's eyeball checklist.)
- Existing events are never reordered or dropped; a replacement that would break strict
  time-ordering against any retained event abstains.
- All-abstain ⇒ `seg` byte-identical, version included.
- Decisions gate on candidate availability and class — never on session type
  (the analysis-is-session-agnostic rule).

---

## 6. Blast radius — what moves when P6/P8/P10 move

From the consumer map (full detail in the implementation plan when written; the survey
was done against `7e62369`):

**Changes, and is supposed to change:**

- The wrist assessment grid at P6/P8: `wrist_angle_sampler.h` medians ±15 ms about the
  event — a 44 ms move is a fully disjoint sample window, so cells can change value and
  flip Ok↔Gap. Findings and `assessmentScore` follow.
- The `_p6`/`_p8` measures in `core.json` (five wrist/arm DOF deltas each), and the
  windowed reducers whose spans touch the moved rungs: `m_handSpeedP6P7` and
  `m_pelvisRotRateP6P7` (rate denominators), `m_lagAngleDown` (extremum over [p5,p6]),
  `m_leadArmToTorso` and `m_leadKneeFlex` ([p7,p8]).
- RAG detections seated on those norms; the `swing_phasegrid.json` sidecar (its
  size+mtime guard invalidates on reanalysis, so no staleness).
- Timeline stations, chart segment chips, snap targets, and the P5→P6 / P6→P7 span
  summaries in the review UI.
- Expected *improvement*: the published P5→P6 interval widens from ~14 ms to ~64 ms.
  Since the phase ladder is the diagnostics coverage bottleneck (51/152), un-collapsing
  it may unblock windowed measures; the coverage gate measures this rather than hoping.

**Does not change:** tempo (Address/Top/Impact only), the resemblance score and its
uncertainty interval (Top/Impact only — and their conf fields are untouched in V1),
Address-referenced pose metrics (head/feet/lower/upper body), club delivery (Impact),
kinematic series curves, swing bounds, and every camera-only swing in the corpus.

**Two latent inconsistencies the survey turned up**, listed here so they are decided
during implementation rather than discovered after:

1. `wristCheckpoints()` (`wrist_analysis_adapter.h:45`) maps P2 → `Takeaway(1)` while the
   diagnostics facets map P2 → `ShaftParallelBack(12)` — two surfaces disagree about what
   "P2" means, already flagged as an open question in `wrist_diagnostics_model.cpp:216`.
   Fusion makes the P2 slot more load-bearing; the checkpoint table should move to
   `ShaftParallelBack` (falling back to Takeaway when absent) as part of this work.
2. `PpTransitTimeline.qml` renders `phases[]` stations and `club.positions[]` chips from
   two independent sources. Today they visibly disagree at P6/P8/P10 on IMU swings;
   fusion makes the stations converge on the chips. No code change needed — but the gate
   should confirm the convergence visually on one swing.

---

## 7. Confidence, class, and the UI — a note for later

Nothing in the review timeline reads `PhaseEvent.conf` — a clamp-quality P10 renders
identically to an acoustic-anchored P7. That is a UI gap, not an arbitration gap, and
fusion narrows it only by making the worst labels rarer. With `TimingClass` persisted, the
UI eventually gets a principled way to fade or annotate non-measured events (`Fallback`
ticks especially) without decoding magic conf constants. Out of scope here; recorded so
the field's second consumer is anticipated.

---

## 8. Validation plan — the corpus gate

This change moves the timeline every phase-anchored measure resolves against. It needs a
gate in the mould of `d0c9ff2` / `f7a1d15` / `2247116`, run on GOLFSIMPC (the Mac has no
pose models), with the result recorded as
`docs/implementation/timeline_fusion_impl.md`.

**Prerequisite — truth markup.** The both-sources population is exactly the eleven
2026-08-18 swings (`Wrist_01` ×5, `Wrist_02` ×6): the 08-17 session directories are
empty and 08-04 has no video. The existing 15-swing full-P truth set is June/July, all
camera-only — it can regression-gate the camera side but cannot grade IMU-vs-camera
arbitration. Status:

1. The six `Wrist_02` swings (hi-res wrist data) were marked up with full P1–P10 truth
   on 2026-08-19; the truth-graded table in §2 is that markup. These are the primary
   grading set — the only swings whose IMU side is representative of production hi-res
   capture.
2. The five `Wrist_01` swings (lower-rate wrist data) were marked up the same day. The
   stratum turned out more useful than expected: the lower rate did **not** degrade the
   IMU's measured phase timing (§2), the camera-side truth doubles the n on every flipped
   slot, and the degenerate `Wrist_01/0003` delivered the verdict that rewrote the
   dispute cap (§4.3). The gate still **stratifies by session** — hi-res grades the
   ownership calls, the low-rate session stresses the fail-safe paths — but the
   stratification is now a discipline, not a quarantine.
3. `lab.py ingest` over the library root — done 2026-08-19; the manifest is an
   inventory (every dir with a `swing.json`), so the two 08-18 sessions entered
   alongside the video-less 08-04 quick captures, and consumers filter on the recorded
   facts (`videos`, `bindings`, `truth`) rather than on the manifest's membership.
   Gate populations below select explicitly — and note the rebuilt manifest now counts
   108, so "the corpus" is no longer a population by itself: gates 2–3 use the 61 swings
   frozen in the stagegate `pose2/` cache (which is what "the 61-swing corpus" always
   operationally meant), gate 4 the eleven truth-marked 08-18 swings.

**Gate ladder:**

| # | Gate | Population | Pass condition |
|---|---|---|---|
| 1 | Unit | `timeline_fusion_test` fixtures, incl. a `Wrist_01/0003`-shaped degenerate ladder and a Fallback-P10 ladder | decision table §4.3 exactly (on the degenerate: club P8 wins uncapped, IMU P4 retained by ownership); all-abstain ⇒ byte-identical seg |
| 2 | OFF parity | 61-swing corpus, pose2-pinned, `refine.fusion=false` vs baseline | `parity_diff.py` byte-identical (OFF is code-path-identical, so the byte gate is valid despite pose-injection being segmentation-blind) |
| 3 | ON, camera-only | 61-swing corpus, live-pose same-binary A/B | byte-identical, or every diff explained (expected: zero — §4.4) |
| 4 | ON, both-sources | the eleven 08-18 swings vs truth, stratified by session (hi-res `Wrist_02` is authoritative) | P6 error ≤40 ms on ≥10/11 (P_CHECKS tol 0.04 s); P8 ≤50 ms on ≥10/11; P10 ≤120 ms where truth marked; P3/P4/P5/P7 within baseline ±5 ms; zero non-monotone ladders; on the degenerate swing the club P8 wins and lands ≤50 ms of truth while P4 stays IMU (truth: −1 ms) |
| 5 | Coverage | `lab.py coverage` pre/post on both populations | no measure regresses RESOLVED→BLOCKED; P5–P6-windowed unblocks are a bonus, recorded |
| 6 | Eyeball | one 08-18 swing in the review UI | stations converge on club chips; replay ends at the real finish; nothing visually absurd |

Two procedural cautions inherited from prior gates: `score.py` renormalises when the set
of emitted phases changes, so re-baseline before diffing scores across the flip; and
`trace.jsonl` anchors can lie about what was persisted — grade from `swing.json`/
`result.json`, as the Part I diagnosis did.

**Rollout:** `refine.fusion` lands default OFF; the gate goes green; the default flips in
its own commit citing the gate evidence, exactly as `refine.positionsLadder` did on
2026-08-09. `refine.fusionP1` stays dark until the Phase 2 gate.

---

## 9. Phase 2 and beyond

In rough priority order:

1. **P1 / Address arbitration** (`refine.fusionP1`): flip the club stack-fit in over the
   IMU fallback, unify the Address/club-P1 twins (making the `event_refine.h` contract
   true on the IMU path instead of documenting a falsehood), and gate on
   `fidget_eval.py` + the tempo band + replay trim. The Takeaway co-timing needs a rule:
   Address must not cross Takeaway; if the club P1 lands after the IMU Takeaway, the
   pair is disputed and retained.
2. **Timing uncertainty in microseconds (σ_us) as the V2 currency.** The ingredients are
   computed and discarded today: the crossing slope (`bestSlope` in
   `findHorizontalCrossing` and friends) and the frame gap at the crossing give the
   camera a real temporal σ; the segmenter's gate shapes give the IMU one. σ_us is
   commensurable across producers by construction — arbitration becomes "smallest σ
   wins, class as a gate", per-swing rather than per-slot-table. V1's persisted
   `FusionDecision` deltas plus the new truth markups are exactly the calibration set.
   σ starts life *logged, not deciding*, and takes over only when the calibration holds
   up — the same measure-first discipline that held the wedge model dark.
3. **Always capture the vision ladder.** `ShaftStage` passes the tracker a trace pointer
   only when there is no IMU (`wrist_analyzer.cpp:511`), so on IMU swings the third
   witness is never even built, and `EventRefineStage` is gated off with it. Once fusion
   exists, the vision ladder's Address/Top become corroborating candidates rather than
   dead weight. Cheap to enable; sequenced after V1 so the gate isolates one change.
4. **UI class awareness** (§7): fade `Fallback` ticks, annotate provenance in the
   timeline hover.

---

## 10. Definition of done

- `docs/design/timeline-fusion.md` (this document) agreed.
- V1 implemented behind `refine.fusion` (default OFF), with `TimingClass` plumbed
  through both producers, `fuseTimeline` + `TimelineFusionStage` in place,
  `timeline_fusion_test` green, and existing ladder/refine tests untouched.
- ~~The eleven 08-18 swings marked up (full P-ladder) and ingested.~~ Done 2026-08-19.
- Gates 1–6 green, evidence recorded in `docs/implementation/timeline_fusion_impl.md`.
- Default flipped ON in its own commit citing the gate.
- The published timeline on an IMU-bound swing carries a measured P6, P8, and P10, with
  provenance and class persisted for every event, and every displaced measurement is
  recorded rather than silently discarded.

---

## Appendix — reproducing the evidence from `swing.json`

Everything in Part I is read from persisted analysis output; no rebuild is needed. The
swings live at `/mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_0{1,2}/`
(on GOLFSIMPC the same share is `C:\PinPointStudio\Mark-Liversedge`).

```python
j = json.load(open('<swing>/swing.json'))
j['analysis']['phases']            # [{phase, t_us, conf, segment}] — the published timeline
j['analysis']['club']['positions'] # [{p, t_us, conf, ...}] — the camera's own P-positions
j['analysis']['segmentation']['version']   # 4 == the positions ladder inserted something
```

Decoding:

- `phases[].phase` is the `Phase` enum (`swing_analysis.h:131`, append-only ints):
  0 Address (P1), 1 Takeaway, 2 Top (P4), 5 Impact (P7), 7 Finish (P10),
  8 MidBackswing (P3), 9 Delivery (P6), 10 MaxSpeed, 11 FollowThrough (P9),
  12 ShaftParallelBack (P2), 13 ArmParallelDown (P5), 14 ShaftParallelThrough (P8).
- `phases[].segment` is the `SegmentRole` provenance (`swing_analysis.h:46`):
  0 Unknown (the acoustic anchor), 5 LeadForearm, 6 LeadHand, 9 Club.
- `club.positions[].p` is the coaching index 1–8/10 directly; `source == 1` with
  `stackN > 0` marks the P1 stack fit.
- Δ for a position = `phases[]` timestamp − `club.positions[]` timestamp of the same P.
  Both live in the same window time domain in memory; `serializeAnalysis` subtracts
  `clock.t0_us` from both exactly once at write, so persisted values diff directly.
- On these swings Address (phase 0) and Takeaway (phase 1) are co-timed — the
  conf-0.30 Address fallback pins Address to the takeaway time — so the P1 regime
  split is read from the *Takeaway* event's confidence (≈0.81 vs ≈0.47).
- If a `result.json` sits beside `swing.json` (a swinglab `--out` reuse), read the
  NEWER file — `swinglab_run` writes `swing.json` then renames, and the rename fails
  silently into a reused output directory.
