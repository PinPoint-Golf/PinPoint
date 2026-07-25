# Sign conventions

**Every directional metric in PinPoint is positive toward the lead side.**

That is the whole rule. Lateral displacement, ball position along the stance, and anything else
whose value carries a direction rather than only a magnitude: **positive is toward the lead arm and
lead foot; negative is away from them.**

## Why this rule, and why it is written down

Three signals shipped in the seed pack pointing the wrong way, and none of them failed loudly. An
inverted signal fires happily on the wrong swings with correct-sounding consequence text attached,
and looks exactly like a detector that works. The direction audit
(`src/Diagnostics/tests/axis_direction_test.cpp`) found them by comparing each condition's own words
against its metric's stated convention — which only worked where a convention had actually been
stated. Five signals could not be checked at all, because their metric never said which way was
positive.

So the convention is not documentation-after-the-fact. It is the thing that makes a direction
auditable, and a metric that does not state it cannot carry a signal safely.

## Lead-relative, not left/right, and not "target"

The rest of the vocabulary is already handedness-invariant — lead/trail, never left/right — and this
follows it. "Positive is toward the lead side" means the same thing for a right-handed and a
left-handed golfer, and needs no `leadIsLeft` at the point of reading.

Prefer **"lead side"** over "toward the target" in metric text. They usually coincide, but the lead
side is a property of the golfer and the target is a property of the shot, and the two come apart on
a deliberately manipulated setup. The vocabulary should key on the golfer.

## What this means in practice

| metric | positive means |
|---|---|
| `ballPosition` | toward the lead foot. `0 %` is the middle of the stance; a driver sits around `+50 %` (the lead heel), a wedge around `0 %`. Unclamped — forward of the lead heel reads above `+50 %`. |
| `pelvisSway` | the pelvis has moved toward the lead side. Sway *away* in the backswing is therefore **negative**, and a slide toward the lead side in the downswing is **positive**. |
| `thoraxLateralDrift` | the chest has moved toward the lead side. The counterpart to pelvis sway, and read with it. |

Depth and rotation axes are a separate question and are **not** covered by this rule — `pelvisThrust`
is toward the ball, which is neither lead nor trail, and it states its own convention. Where an axis
is not lead-relative, say what positive means in the descriptor rather than assuming a reader can
infer it.

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

## Still undefined

`shoulderAlignment` does not state which sign is open. It is a `planned` metric, so the convention
is free to choose, and the natural reading under this rule is **positive = open** — an open shoulder
line for a right-handed golfer points left, which is the lead side. That has not been decided, and
`sig_alignmentOpen` / `sig_alignmentClosed` remain unauditable until it is. Decide it when the
producer is written, not after.
