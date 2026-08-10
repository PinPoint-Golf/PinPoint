# Address A3-pin repair (pin-gated onset reseed) — session record

*Executed 2026-08-10 (single session). Lead: the [[top_collapse_repair_impl]]
close-out's residual — 5 repaired swings reporting tempo ratio 0.5–0.8 because
the Address event sat pinned at exactly impact − 0.549 s. Commits: `8e8bdaa`
(dark v1, signal-boundary reseed) → `0c7665f` (v2, run-start reseed) →
`044afbc` (v3, pin-gated) → `2247116` (default flip). Gate runs:
`C:\PinPointStudio\p7geo\{rsdark,rson,rsdark2,rson2,rsdark3,rson3,rsflip}`.*

## What shipped

`shaft.topRepair.onsetReseed` (FROZEN ON at `2247116`) — a pin-gated onset
reseed inside `segmentPhases`, nested in the fired top repair:

- **Candidate** (recorded when the repair fires and `bs0 > top'`): the
  backswing run the two-longest ranking lost — the latest-starting
  (post-bridge, post-gate) run at/before the repaired top; fallback for a
  sub-swSpd creep backswing that never formed a run is the last
  sub-swLow → rising boundary before top'.
- **Pin gate** (the load-bearing part): Stage A's walk-back runs from the
  ranking's bs0 first; only when that onset VIOLATES the A3 near edge — the
  manufactured-Address signature (Address == Takeaway at exactly
  impact − bsMinBeforeImpactUs) — does the walk-back re-run from the
  candidate. A repaired swing whose onset the frozen A2/veto machinery
  already resolved below the edge is byte-identical.
- Stage A (A1/A2 + no-return veto) was refactored into a `walkBack(b0)`
  lambda — A3 and the onsetFloor publication stay outside, applied to
  whichever pass won. Dark reproduces the pre-reseed byte stream exactly.

## What the session actually measured (three corpus iterations)

1. **v1 (signal-boundary reseed, unconditional)**: all 15 repaired swings
   moved; near-edge pins 11 → 0 — but 8 swings landed at EXACTLY
   impact − 1.6 s: the near-edge pin traded for the far-edge one. Mechanism:
   reseeding bs0 straight to the sub-swLow boundary makes onset == bs0, so
   the veto's `[onset, bs0 − gap]` window is empty, and on real capture the
   lerped-pose speed floor (2–4 px/f through every fidget settle) walks past
   the takeaway into deep stillness.
2. **v2 (run-start reseed, unconditional)**: 0704_0010 and 0708_0005 healed
   (the recovered run start kept the veto's horizon), but 7 swings still
   railed at 1.601 s — including 0705_0003/0006, which were dark-SANE and
   REGRESSED. Key finding: the dark path's sane onsets on those swings come
   from the veto's downswing-inclusive horizon (the grip's address position
   is revisited at impact, anchoring the boundary); any pre-downswing
   horizon loses that revisit on the 0705 session, whose pre-takeaway creep
   neither dips below swLow nor revisits within the veto box.
3. **v3 (pin-gated)**: exactly the 11 near-pinned swings change, everything
   else byte-identical.

## Gate evidence (61-swing corpus, pose2-pinned, over `fda2677`)

| Check | Result |
|---|---|
| Dark byte-identity (`trflip` vs `rsdark3`) | 61/61 identical (also held at v1/v2) |
| Post-flip pure-defaults (`rsflip` vs gated `rson3`) | 61/61 identical |
| Changed swings | exactly the 11 with the near-edge pin; the 4 dark-sane repaired swings + 46 others byte-identical |
| Clean conversions | 6 (0611×4, 0704_0007, 0704_0010): Takeaway 0.84–1.03 s pre-impact, tempo 2.0–3.1, Address off the pin, Address ≠ Takeaway |
| Honest rails | 5 (0703_0002, 0705_0001/0002/0007/0008): Takeaway clamped at the far edge (1.601 s), tempo 4.1–6.1 — plausible-high replacing physically-impossible fabricated 0.46–1.16 backswings of ~0.2 s |
| Tempo census | 1–6 band 56 → **60**/61 (residual: 0705_0001 at 6.05) |
| Top/Impact/P6/P5 emissions | unchanged (top_diff: 0 swings) |
| track.valid | 61 → 61 |
| Side effect | `analysis.club.coverage` moved on the 11 (estimateSwingSpanUs shares segmentPhases — the span start now covers the real backswing) |

## Residuals / successor leads

- **The 5 far-edge rails** (0703_0002, 0705_0001/0002/0007/0008): the
  pre-takeaway creep on these swings never dips below swLow and never
  revisits within the veto box, so no walk-back start yields a
  veto-resolvable settle; A3 rails honestly at impact − 1.6 s. The signal
  frontier is the same one limiting the 0611/0705 sessions elsewhere. A
  future lever: teach the veto (or a reseed-specific variant) the
  impact-return revisit — the downswing re-crosses the address position by
  definition, which is exactly what anchors the dark-sane siblings.
- **Address walks past the A3 far edge**: the Address event walk-back
  (event_refine) starts from the Takeaway onset and is floored only by
  onsetFloor, so Address can sit earlier than impact − bsMax (measured
  imp − addr up to 2.06 s). Pre-existing behaviour, now visible.
- [[july10-impact-markup-lead]] still queued (needs corpus ingest).

## Traps confirmed this session

- An unconditional reseed is NOT safe even with a correct candidate: the
  frozen veto's quality on collapse swings depends on its HORIZON, and the
  dark horizon (the mis-picked downswing start) is accidentally
  load-bearing. Gate repairs on the pathology's signature, not the
  pathology's precondition.
- Synthetic gap/hold segments must carry the 2–4 px/f lerped-pose floor or
  fixtures pass where the corpus fails (fixture C's first draft stalled A1
  in a raw-quiet inter-burst gap that real capture never produces).
- The studio checkout's local `main` has diverged (`f015ebb` "Runbook update
  on Windows" + 2 more local commits); gate runs use detached checkouts of
  origin/main shas — do not touch the local branch.
- swinglab metric values live in `phaseSamples[].value`, not `value` (which
  is an empty per-sample array for scalar metrics) — top_diff.py's tempo
  column silently reads None.
