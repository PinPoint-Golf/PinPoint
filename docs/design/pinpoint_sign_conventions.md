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
| `toeLineAngle` | closed stance. An open stance is negative. |

> **`shoulderAlignment` and `hipAlignment` used to be in this table and are gone.** Each named a
> metric that was geometrically identical to a body-line tilt the catalogue already carried —
> `shoulderPlaneAngle` and `hipLineTilt` — read at a different phase. Two descriptors for one curve
> is two names for one number, and worse here than usual, because the two carried OPPOSITE sign
> conventions: closed-positive on one and trail-end-above-positive on the other. The measures
> `m_shoulderAlignment` and `m_hipAlignment` now point at the surviving series and are stated in
> ITS convention. `feetAlignment` survived as its own metric — the ankle line is genuinely not the
> toe line — but moved to the body-line convention below for the same reason.
>
> A face-on camera reads the APPARENT line, not true target-line alignment. The reading is still
> informative: a golfer on level ground with a foot set further from the camera shows that foot
> higher in the image, so the image-plane tilt does carry open / closed. It is a proxy, and the
> descriptors say so.

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
| `trailKneeFlexion` | more bend, matching the lead knee |
| `leadUpperArmToChest` | a larger gap — the arm further from the chest |
| `comOverLeadFoot` | further FROM the lead ankle, so a balanced finish is the low end. UNSIGNED: still back and fallen through are the same fault seen from either side |
| `attackAngle` | a more UPWARD strike |
| `lowPointAhead` | the arc bottoming out AHEAD of the ball, on the target side |
| `leadArmToTorso` | the arm further from the torso. Unsigned, 0–180°: a frontal projection cannot say which side the arm left on |
| `trailElbowHeight` | the elbow higher above the shoulder line |
| `leadHandWidth` | the hands further from the chest — a wider arc |
| `thoraxLateralDrift` | toward the LEAD side, the same convention as `pelvisSway`, of which this is the chest's counterpart |

### Body lines — ONE convention, and it does not flip

`hipLineTilt`, `shoulderPlaneAngle`, `elbowAlignment` and `feetAlignment` are all the image-plane
tilt of a line between a lead point and its trail partner, and all four mean the same thing:

> **Positive means the TRAIL end sits ABOVE the lead end.**

They are computed against the **absolute** horizontal separation — `atan2(leadY − trailY, |Δx|)` —
which is what makes the sign independent of which image side the lead is on. The alternative, a raw
`atan2` of the lead→trail vector, inverts for a left-handed golfer or a mirrored camera while
describing the same posture. `toeLineAngle` is that alternative and predates the rule; it is the
one line metric that does flip, which is half the reason `feetAlignment` exists beside it.

### The two that deliberately break the lead-positive default

| Metric | Positive means | Why not lead-positive |
|---|---|---|
| `secondaryAxisTilt` | leaning AWAY from the target — trail-side lean | The quantity is NAMED for the lean away from the target. Inverting it to satisfy the default would leave every coach-facing sentence about it backwards. |
| `trailWristFlexExt` | EXTENSION (cup) — the opposite of the lead wrist's `+ = bowed` | The hands are mirror images. Seen face-on from one side, one signed image-plane angle means flexion on the lead hand and extension on the trail hand. The shipped corridors agree: `m_trailWristFlexExt_p4` is seated at +45°, and 45° at the top is the trail wrist cupping. |

### Turn magnitudes

`pelvisRotation`, `thoraxRotation` and `xFactor` are **unsigned magnitudes of turn from address**,
not signed away/toward readings. Positive at the top AND positive at impact, passing through zero as
the body squares up. This is not a shortcut of the camera estimator — it is what the shipped
corridors require (`m_pelvisRotP4` at +45° for the top, `m_pelvisRotP7` at +40° for impact; a signed
curve cannot satisfy both). It does mean a peak reducer spanning the top to impact sees the larger
of the two excursions rather than the open one. See
[`body_rotation_estimation.md`](body_rotation_estimation.md).

## Not covered by either rule

A metric on neither axis **must state its own convention**: `pelvisThrust` is toward the ball, which
is neither lead nor trail nor an aim; turn metrics like `pelvisRotation` are magnitudes of rotation,
not directions of aim. Say what positive means rather than assuming a reader can infer it.
