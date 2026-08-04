# Sign conventions

Three rules, in precedence order, the last being a default that applies only where the others
leave us free:

> **0. Where a PUBLISHED STANDARD covers it, follow the standard.**
> **1. Otherwise, where the outside world already has a convention, follow it.**
> **2. Otherwise, positive is toward the lead side.**

**And one invariant above all three: A SIGN'S MEANING NEVER CHANGES WITH HANDEDNESS.** Whatever
transform holds that fixed is the producer's job, never the reader's. A left-handed and a
right-handed golfer reading the same number must be told the same thing by it. What flips is the
right-handed *gloss* — in-to-out, open, draw — never the sign.

Rule 0 is why a market-leading sensor does not get to set our wrist polarity; see below. Rule 1
wins wherever no standard applies. A number that gets read next to numbers we did not produce — on a
launch monitor, in another piece of golf software, in a coach's notes — has to mean the same thing
there as here, and no amount of internal elegance is worth a golfer misreading it.

## What rule 1 covers today

**Aim and path** — where something *points* or *travels*. **POSITIVE IS TO THE RIGHT OF THE TARGET
LINE**, read looking down the line toward the target. That is what every launch monitor reports, and
it is what keeps `face − path` carrying the sign the shot shape implies.

Right of the target line is the same absolute direction for both golfers, which is what makes this
frame handedness-free. The right-handed *glosses* — in-to-out, open, draw — flip; the sign does not.

| metric | positive means | right-handed gloss |
|---|---|---|
| `clubPath` | the head travelling right of the target line | in-to-out; out-to-in is negative |
| `faceAngle` | the face pointing right of the target line | **open**; closed is negative |
| `faceToPath` | the face open relative to the path | curvature to the right — a fade |
| `launchDirection` | the ball starting right of the target line | a push; a pull is negative |
| `toeLineAngle` | *(see the caveat below — this one is an image-plane proxy, not a device number)* | closed stance |

> **`faceAngle` used to be stated here as closed-positive, and that was wrong twice over.** It was
> justified as "what every launch monitor reports", and it is the opposite of what they report:
> Foresight, TrackMan and the general ball-flight literature all state face angle **open-positive**
> for a right-handed golfer — pointing right of target. It also contradicted this table's own
> `faceToPath` row, which has always been open-positive. The arithmetic settles it without needing a
> vendor at all: a real GC Quad row reads Face to Target `6.943`, Horiz Path `1.320`, Face to Path
> `5.623`, and `6.943 − 1.320 = 5.623` exactly. Closed-positive face gives `−8.262`, which is not
> face-to-path and not anything. Rule 1 was right; the fact asserted under it was not.
>
> Nothing depended on the old wording — the only face-angle producer is the launch monitor connector,
> which reads the device's own sign and applies no negation, so the fix is to this document and to
> `m_faceAngle`'s `highMeans`, not to any stored value.

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

`toeLineAngle` is the one row here that is **not** comparable with an outside number: it is the
apparent stance line as one camera sees it, not a measured aim, and its sign flips for a mirrored
camera (see the body-line note below). It keeps closed-positive because that is what a coach reading
"open / square / closed" expects, but never place it beside a device number and expect agreement.

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

## Rule 0 — the anatomical frame follows ISB, and that outranks rule 1

Rule 1 says follow the outside world. Where the outside world has a **published standard** rather
than a market leader, the standard is what we follow — and for joint angles it does.

The lead wrist, forearm and elbow angles implement **ISB / Wu et al. 2005** (`ref.wu2005`), the
recommendation of the Standardization and Terminology Committee of the International Society of
Biomechanics. Twelve authors across institutions in five countries, published by the society itself
under its standards collection, and the companion to the Part I recommendation covering ankle, hip
and spine. It is not one laboratory's preference, which is precisely why it wins.

| Metric | Ours | ISB (Wu 2005) | |
|---|---|---|:--:|
| `leadWristFlexExt` | + flexion (bowed) | flexion + | ✓ |
| `leadWristRadUln` | + ulnar (hinge) | ulnar deviation + | ✓ |
| `forearmPronation` | + pronation | pronation + | ✓ |
| `leadArmFlexion` | + elbow flexion (magnitude) | flexion + | ✓ |

ISB specifies these as positive **for both the left and the right arm**. So the standard itself
delivers the handedness invariance this document asks for everywhere else — `mirrorSign()` in
`wrist_assessment_types.h` is *implementing* ISB, not working around it.

**A widely used commercial wrist sensor reports the inverse of us on bow/cup** — extension (cupping)
positive, flexion (bowing) negative. When a standard and a popular product disagree, **the standard
wins**: a vendor can change their convention next release, and a standard is the thing that lets two
datasets be compared at all. The disagreement is recorded in `ref.wu2005` and in
[`../reference/wristmetrics.md`](../reference/wristmetrics.md) so it stays a known difference rather
than a discovered one. Never compare a raw wrist sign across sources without checking the frame.

### What ISB does NOT govern, and why saying so protects the claim

ISB defines three-DOF rotations between segment triads built on palpable bony landmarks. **Four of
our metrics are ISB joint angles. The rest are not, and must never imply they are** — declaring
conformance for a two-dimensional apparent shoulder-line angle taken off one camera is what would
fail review, not the absence of it.

| Family | Why ISB does not govern it | Examples |
|---|---|---|
| **Club & ball** | ISB defines *human joint* motion. A clubhead is not a joint. World frame instead. | every `lm.*`, `clubPath`, `ballSpeed` |
| **Turn magnitudes** | Unsigned magnitudes of turn from address, not signed axial rotations about a defined axis. A face-on camera or one IMU gives no bony-landmark triad. | `pelvisRotation`, `thoraxRotation`, `xFactor` |
| **Image-plane body lines** | 2D *apparent* angles between two keypoints as one camera sees them. Not joint rotations at all. | `hipLineTilt`, `shoulderPlaneAngle`, `elbowAlignment`, `feetAlignment` |
| **Normalised displacements** | Not angles. Fractions of stance or shoulder width, or centimetres. | `pelvisSway`, `headSway`, `ballPosition` |
| **Composites & timings** | Derived from other metrics, or durations. | `xFactorStretch`, `tempoRatio`, the scores |

That is the honest reach of a face-on camera and a wrist IMU. Each of those metrics carries its own
stated convention in the tables above and below.

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
| `lieAngle` | the clubhead sole **toe UP** relative to the ground at impact; negative is toe down and zero is flat. Foresight's published convention, taken unchanged |
| `closureRate` | the face rotating **CLOSED** through impact — a higher positive value is a faster closing rate, a low or stable one a squarer, held-off release. Foresight's published convention. Reported by the device in dps **or** rpm; the connector converts on the header's declared unit, so ours is always °/s |
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
