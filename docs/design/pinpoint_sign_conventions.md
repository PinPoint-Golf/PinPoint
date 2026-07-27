# Sign conventions

One rule, with a default that applies only where the rule leaves us free:

> **1. Where the outside world already has a convention, follow it.**
> **2. Otherwise, positive is toward the lead side.**

Rule 1 wins whenever it applies. A number that gets read next to numbers we did not produce — on a
launch monitor, in another piece of golf software, in a coach's notes — has to mean the same thing
there as here, and no amount of internal elegance is worth a golfer misreading it.

## What rule 1 covers today

**Aim and path** — where something *points* or *travels*. **Out-to-in and open are NEGATIVE;
in-to-out and closed are POSITIVE.** This is what every launch monitor reports, and it keeps
`face − path` carrying the sign the shot shape implies.

| metric | positive means |
|---|---|
| `clubPath` | in-to-out. Out-to-in — the over-the-top delivery — is negative. |
| `faceAngle` | closed. Open is negative. |
| `shoulderAlignment` | closed. An open shoulder line at address is negative. |
| `toeLineAngle` | closed stance. An open stance is negative. |

**Ball position along the stance** — **`0 %` at the LEAD heel, `100 %` at the trail heel**, so a
high value means the ball is further BACK. This is the scale other golf software uses. Unclamped:
forward of the lead heel is a real driver setup and reads below `0 %`.

| club | roughly |
|---|---|
| driver | near `0 %` — off the lead heel |
| iron | around `33 %` |
| wedge | around `50 %` — the middle of the stance |

## What rule 2 covers

**Displacement** — where something has *moved*, and where nobody outside has a convention to match.
**Positive is toward the lead arm and lead foot; negative is away from them.**

| metric | positive means |
|---|---|
| `pelvisSway` | the pelvis has moved toward the lead side. Sway *away* in the backswing is therefore **negative**, and a slide toward the lead side in the downswing is **positive**. |
| `thoraxLateralDrift` | the chest has moved toward the lead side. The counterpart to pelvis sway, and read with it. |

Lead-relative rather than left/right, matching the rest of the vocabulary: the same statement holds
for a right- and a left-handed golfer and needs no `leadIsLeft` at the point of reading. Prefer
**"lead side"** over "toward the target" in metric text — the lead side is a property of the golfer
where the target is a property of the shot, and the two come apart on a manipulated setup.

## The conventions point different ways, and that is correct

For a right-handed golfer "closed" points to the *trail* side, and ball position counts *up* toward
the trail foot — both opposite to displacement's lead-positive. **That is not an inconsistency to be
tidied up.** Each follows the convention its own readership expects, which is exactly what rule 1
asks for. Do not "fix" one to match another.

## Why this is written down at all

Three signals shipped in the seed pack pointing the wrong way, and none of them failed loudly. An
inverted signal fires happily on the wrong swings with correct-sounding consequence text attached,
and looks exactly like a detector that works. The direction audit
(`src/Diagnostics/tests/axis_direction_test.cpp`) found them by comparing each condition's own words
against its metric's stated convention — which only worked where a convention had actually been
stated. Seven signals could not be checked at all on the first pass, because their metric never said
which way was positive. Writing the rule down is what closed that gap; the audit now covers all 30.

## The obligation on a new metric

A metric whose value carries a direction **must state which way is positive in its own
`MetricDescriptor`** — in `description` or `howToRead`, in words, at the point someone reads it.
Not in a commit message, not in the producer's comments. If rule 1 applies, say which outside
convention it follows.

Two consequences, both enforced:

- `axis_direction_test` asserts that **zero** signals ride a metric with no stated convention. A new
  entry there is a metric that shipped without saying.
- `Measure::highMeans` carries the same statement in the diagnostics pack, in the measure's own
  words — *"further back, toward the trail foot"* — so an author choosing a signal direction reads
  the meaning rather than guessing at High/Low. Asserted present on every signal-bearing measure.

## The trap this cost time on, once

**`attackAngle` reads the STRIKE DIRECTION, not steepness.** Its descriptor says "higher means a
more upward strike", so **`attack_too_steep` is the LOW tail** — the condition's name and the
metric's sign point opposite ways, and an author matching the words *steep* and *high* would ship it
inverted. That is precisely the defect class `highMeans` and the fixture table exist for, and it
survived only because writing the fixture row forced somebody to quote the descriptor.

The general lesson: **a condition's NAME is not evidence about its tail.** Read the measure's own
sentence, every time, even when the answer looks obvious.

## Conventions added with the ball-flight layer

All written right-handed; handedness is a transform applied at read time, never a mirrored duplicate.

| Metric | Positive means |
|---|---|
| `launchDirection` | right of the target — so a pull is the LOW tail, a push the high one |
| `launchAngle` | a higher launch |
| `ballSpeed`, `carryDistance`, `clubheadSpeed` | faster / further |
| `faceToPath` | the face OPEN to the path — curvature to the right |
| `spinAxis` | tilted right — a fade or a slice |
| `spinRate` | more spin |
| `smashFactor` | a more efficient strike |
| `strikeLocation` | toward the TOE; negative toward the heel |
| `dynamicLoft`, `spinLoft` | more loft delivered / a larger loft-to-path angle |
| `shaftDirection` | pointing right of the target — across the line at the top, outside in the takeaway |
| `shaftAngleVsHorizontal` | past parallel; zero IS parallel to the ground |
| `hipAlignment`, `feetAlignment` | closed — following `shoulderAlignment` and club path, where open is negative |
| `trailKneeFlexion` | more bend, matching the lead knee |
| `leadUpperArmToChest` | a larger gap — the arm further from the chest |
| `comOverLeadFoot` | further FROM the lead ankle, so a balanced finish is the low end |

## Not covered by either rule

A metric on neither axis **must state its own convention**: `pelvisThrust` is toward the ball, which
is neither lead nor trail nor an aim; turn metrics like `pelvisRotation` are magnitudes of rotation,
not directions of aim. Say what positive means rather than assuming a reader can infer it.
