# Sign conventions

There are **two** families, and which one a metric belongs to depends on what kind of thing it
measures. Both are stated here; a metric belongs to exactly one.

## 1. Displacement — positive toward the lead side

**Where something has MOVED.** Lateral displacement, ball position along the stance, and anything
else whose value is a signed distance: **positive is toward the lead arm and lead foot; negative is
away from them.**

## 2. Aim and path — positive is closed / in-to-out

**Where something POINTS or TRAVELS.** Club path, face angle, shoulder line, toe line:
**out-to-in and open are NEGATIVE; in-to-out and closed are POSITIVE.**

This is the launch-monitor convention every golfer and every device already uses, and it keeps
`face − path` carrying the sign the shot shape implies. Consistency with the outside world wins on
this axis, because the numbers get read alongside numbers we did not produce.

### They look opposed, and that is correct

For a right-handed golfer, "closed" points right — toward the *trail* side — so the aim family's
positive runs opposite to the displacement family's. That is not an inconsistency to be tidied up
later: the two measure different things (a translation versus a rotation), and each follows the
convention its own readership expects. **Do not "fix" one to match the other.**

## Why this rule, and why it is written down

Three signals shipped in the seed pack pointing the wrong way, and none of them failed loudly. An
inverted signal fires happily on the wrong swings with correct-sounding consequence text attached,
and looks exactly like a detector that works. The direction audit
(`src/Diagnostics/tests/axis_direction_test.cpp`) found them by comparing each condition's own words
against its metric's stated convention — which only worked where a convention had actually been
stated. Seven signals could not be checked at all on the first pass, because their metric never said
which way was positive. Writing these two families down is what closed that gap; the audit now
covers 28 of 30.

So the convention is not documentation-after-the-fact. It is the thing that makes a direction
auditable, and a metric that does not state it cannot carry a signal safely.

## Family 1 is lead-relative, not left/right, and not "target"

The rest of the vocabulary is already handedness-invariant — lead/trail, never left/right — and this
follows it. "Positive is toward the lead side" means the same thing for a right-handed and a
left-handed golfer, and needs no `leadIsLeft` at the point of reading.

Prefer **"lead side"** over "toward the target" in metric text. They usually coincide, but the lead
side is a property of the golfer and the target is a property of the shot, and the two come apart on
a deliberately manipulated setup. The vocabulary should key on the golfer.

## What this means in practice

**Displacement family**

| metric | positive means |
|---|---|
| `ballPosition` | toward the lead foot. `0 %` is the middle of the stance; a driver sits around `+50 %` (the lead heel), a wedge around `0 %`. Unclamped — forward of the lead heel reads above `+50 %`. |
| `pelvisSway` | the pelvis has moved toward the lead side. Sway *away* in the backswing is therefore **negative**, and a slide toward the lead side in the downswing is **positive**. |
| `thoraxLateralDrift` | the chest has moved toward the lead side. The counterpart to pelvis sway, and read with it. |

**Aim family**

| metric | positive means |
|---|---|
| `clubPath` | in-to-out. Out-to-in — the over-the-top delivery — is negative. |
| `faceAngle` | closed. Open is negative. |
| `shoulderAlignment` | closed. An open shoulder line at address is negative. |
| `toeLineAngle` | closed stance. An open stance is negative. |

A metric on neither axis belongs to neither family and **must state its own convention**:
`pelvisThrust` is toward the ball, which is neither lead nor trail nor an aim; turn metrics like
`pelvisRotation` are magnitudes of rotation, not directions of aim. Say what positive means in the
descriptor rather than assuming a reader can infer it.

## The obligation on a new metric

A metric whose value carries a direction **must state which way is positive in its own
`MetricDescriptor`** — in `description` or `howToRead`, in words, at the point someone reads it.
Not in a commit message, not in the producer's comments.

Two consequences follow, both enforced:

- `axis_direction_test` tracks how many signals ride a metric with no stated convention, and asserts
  that count can only go **down**. Adding a directional metric without a convention will eventually
  fail it.
- `Measure::highMeans` carries the same statement in the diagnostics pack, in the measure's own
  words ("further back, toward the trail foot"), so an author choosing a signal direction reads the
  meaning rather than guessing at High/Low.

## Nothing undefined

Every directional metric that carries a corridor signal now states its convention, and
`axis_direction_test` audits all 30 of them against it. The two counts it tracks — signals with no
stated convention, and signals broken in a way a direction cannot express — are asserted so they can
only go down.
