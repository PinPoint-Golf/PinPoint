# P7 impact from club-at-ball geometry — session record

*Executed 2026-08-10 (single session). Lead: the [[p6-phantom-crossing]] close-out's
residual — 3 truth swings with no P6, hypothesised to be impact-anchor casualties.
Commits: `7ee515c` (dark v1) → `079876c` (anchor-centred window) → `2009f68`
(emission arbitration) → `02cf2b0` (default flip). Gate runs: `C:\PinPointStudio\p7geo\`.*

## What shipped

`src/Analysis/impact_geom.h` — a pure header (no Qt/OpenCV) that locates the
sub-frame instant the reconciled θ(t) crosses the grip→ball direction of the A1
address-ball cluster, and arbitrates the phase model's Impact **emission** with it:

- `locateImpactGeom()` — first hysteresis-confirmed crossing of
  e(f) = wrap180(θ(f) − atan2(ball − grip(f))) through 0, sub-frame interpolated
  between consecutive VALID frames (immune to NaN/coverage gaps), with a raw-step
  seam guard (a genuine 0-transit steps small; the ±180 seam steps ~340°) so
  θ_ball+180 can never fire. The elevation-fold helpers in `shaft_positions.h`
  could NOT be reused — their fold identifies θ and θ+180, correct for "parallel
  to ground", wrong for "at the ball".
- `decideImpactFrame()` — geometry vs `pm.impact`: abstain (no geometry) / adopt
  (no anchor — the "legitimate refine path") / override (>`overrideUs` = 100 ms
  apart) / sub-frame retime (corroborated + `retime`).
- Keys `shaft.impactGeom.{enabled,retime,hystDeg,maxStepDeg,overrideUs,windowUs}`;
  `enabled` FLIPPED ON at `02cf2b0`, `retime` measured not-green and dark.
- The address ball comes from `medianGripBallLenPx`'s accepted cluster
  (`ball_anchor.h` — new `clusterBallPx` out-param), inheriting the mis-lock
  cluster gate and ankle-line/feet golf priors for free.
- Consumers: `locatePTimes` gets the corrected frame only when it satisfies
  `top < impact` (positions never regress on collapsed models); the Impact
  EVENT is retimed directly post-`phasesToSegmentation` + stable re-sort.
  Trace: `impactGeom{TUs,Frame,Applied}` in `ShaftDecideTrace` + the
  swinglab_run summary.

## What the session actually found (supersedes the lead's hypotheses)

1. **The 13–20 ms "P7 bias" is the input.** All 12 recorded truth swings carry
   `capture.impactUs` 13–22 ms EARLY vs video truth (trigger chain latency);
   the analyzer reproduces it within ~2 ms.
2. **The gross trio was never a bad anchor.** On 0611_0009 / 0703_0002 /
   0705_0001 the raw anchor is fine (one is truth-rescued verbatim). The
   corruption is `segmentPhases`' **phase-model collapse** (top ≈ fin0,
   ~250 ms late) plus `clamp(impact, top+1, ·)` dragging the emission
   +234..+362 ms past the bogus top. Hence two design corrections mid-session:
   the geometry search window is **anchor-centred** (±600 ms), never
   (top, fin0] — the model bounds are corrupt exactly where rescue is needed —
   and the arbiter compares t_geo to the **emission**, not the raw anchor
   (which corroborates and misses the clamp).
3. **Fixing impact does NOT convert the trio's P6** (the lead's central
   hypothesis, now measured false): the collapsed Top also invalidates the
   locatePTimes P6 window AND the ladder-insertion bounds. The successor lead
   is the phase-model (Top/fin0) collapse itself — 8 corpus swings carry its
   signature (Top within ~7 ms of Impact); the corrected impact instant is a
   ready-made repair anchor for it.
4. **Geometric precision ≈ ±15 ms** (trio probe −10.2 ms vs truth): decisive at
   the 100 ms override scale, marginal at the 20 ms bias scale — which is why
   `retime` stays dark (fires 6/11, mean |err| 18.3→15.9 ms but scatters
   −20..+19 ms).

## Gate evidence (61-swing corpus, pose2-pinned, all at `2009f68`)

| Check | Result |
|---|---|
| Dark byte-identity vs pre-change binary (`base` @ d0c9ff2) | 61/61 identical, twice (7ee515c and 2009f68) |
| Truth swing 0703_0002 P7 error | +234.3 ms → **−10.2 ms** |
| Other corrected emissions | 7 non-truth swings, all Top≈Impact collapse signature, −250..−360 ms |
| 11 sane truth swings | untouched (applied=kept) |
| P5/P6/P8 emission, seg.monotone, blocking rows | identical (46/46/49, nonmono 0, p5 82 / p6 44 / p8 12) |
| Coverage shift | RESOLVED 2760→2751, BLOCKED_METRIC 3453→3462: the 9 shed rows are tempo measures previously computed from the bogus impact (dark tempoRatio ≈ 116 on those swings) — fabrication removed, reported honestly |
| 0611_0009 / 0705_0001 | abstain — no accepted address-ball cluster (`lPxRejected 2` feet-corridor mis-lock; dark-lighting sparse cluster). Behaviour identical to before. |
| Post-flip pure-defaults run (`02cf2b0`, params = refine-on only) | `p7geo\flip` vs gated `on3`: **61/61 byte-identical** |

## Residuals / successor leads

- **Phase-model collapse** (the real P6-recall blocker for the trio): top/fin0
  land ~250 ms late and equal on 8/61 swings. The geometry's impact instant
  (now in the trace) bounds where Top must be — a re-derivation lever.
- **Address-ball reliability** (0611_0009 mis-lock, 0705 dark lighting): the
  same frontier already limiting P6 recall on the 0705 session.
- **The 13–22 ms trigger bias**: a capture-chain constant, better handled as
  calibration than per-swing geometry; retime can re-audition once the
  crossing model earns ~±5 ms (e.g. lead/lag-aware or clubhead-path-based).
- v1 scope note: `segmentPhases`-internal consumers (A3 onset clamp, run
  candidacy, wedge swingProgress, blur band) still see the uncorrected anchor —
  they run before the plug point.

## Traps confirmed this session

- The trace re-run's phase anchors diverge from the shipping run (known);
  `impactGeom*` summary fields describe the trace invocation — judge event
  claims by result.json only.
- `%VAR%` doesn't expand under ssh→PowerShell; studio scripts must be `.cmd`
  launched via `Invoke-CimMethod` (session-death-proof) with done-sentinels.
- A "corroborating" reference can be the wrong reference: the raw anchor
  agreed with the geometry while the emission was 234 ms out. Arbitrate the
  value consumers actually receive.
