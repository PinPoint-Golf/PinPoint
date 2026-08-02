# Axial body rotation — one producer, two tiers

**Audience**: developers and content authors working on the rotation half of the diagnostics model
**Code**: `src/Analysis/body_rotation.{h,cpp}`, `BodyRotationStage` (`wrist_analyzer.cpp`), `BodyRotationProvider`
**Content**: `src/Resources/diagnostics/core.json` (9 measures), `norms.json`
**Status**: producer live; the camera tier is what runs today, the IMU tier is written and waits on a placement UX
**Written**: 2026-08-02

---

## Contents

1. [Why this is not simply "planned until we have body IMUs"](#1-why-this-is-not-simply-planned-until-we-have-body-imus)
2. [The two tiers](#2-the-two-tiers)
3. [The magnitude decision](#3-the-magnitude-decision)
4. [Where the camera tier is weak — stated, and propagated](#4-where-the-camera-tier-is-weak--stated-and-propagated)
5. [What Bridged means to the golfer](#5-what-bridged-means-to-the-golfer)
6. [Open questions](#6-open-questions)

---

## 1. Why this is not simply "planned until we have body IMUs"

Pelvis and thorax rotation head the diagnostics roadmap by a wide margin: **eight measures and
eleven characteristics** sit over them, more than any other missing producer. They also describe
rotation about the body's vertical axis, which a frontal projection cannot see directly, and
`SegmentRole::Pelvis` / `Thorax` exist in the enum but `segmentRoleForSlot()` has never mapped a
placement slot to either — so no shot recorded by this product has ever carried one.

A module built only for the ideal sensor would therefore emit nothing on every swing the product
actually records. A module built only for the camera would throw away a bound IMU the day one
arrives. The standing rule settles it:

> **Produce the measurement from whatever is available, and take the better path automatically when
> a better sensor is there.**

This module is that rule in code, and it is the reason `MetricAvailability::Bridged` exists.

---

## 2. The two tiers

Resolved **per segment, independently**. A swing with a pelvis IMU and no thorax IMU comes back with
one measured turn and one estimated one — which is the right answer. Refusing the pair because half
of it could be better measured would throw away the half that could not.

### `RotationTier::Imu`

The segment's medio-lateral axis (anatomical `+X`, per `imu_frame_contract.md` §5's shared
`solveSegment` construction) carried into world by `q_anat` and projected into the horizontal plane.
World is Z-up, so the horizontal plane is world XY, and the direction angle of that projection IS the
segment's axial orientation. Referenced to its own address direction.

This is the quantity the descriptor names, measured rather than inferred. It resolves `Measured`.

### `RotationTier::Foreshortening`

As a body line turns away from the camera its image width collapses by the cosine of the turn:

```
turn(t) = acos( clamp( w(t) / w_address , 0, 1 ) )
```

where `w` is the hip span for the pelvis and the shoulder span for the thorax, and `w_address` is a
median over the frames in the address window. It resolves `Bridged`.

The `clamp` is not defensive tidiness. A span measuring **wider** than address is noise, or a golfer
who was not square at address; without the clamp `acos` returns NaN, and with it the honest reading
is "no turn resolved" rather than an imaginary angle.

---

## 3. The magnitude decision

**The series is the UNSIGNED MAGNITUDE of turn from address, not a signed away/toward reading.**

This looks like a limitation of the camera tier — a cosine carries no sign — and it is not. It is
what the shipped corridors require, and the IMU tier follows the same convention so the two are
interchangeable to every reader:

| Measure | Reads at | `mu` |
|---|---|---|
| `m_pelvisRotP4` | the top — turned AWAY from the target | **+45°** |
| `m_pelvisRotP7` | impact — turned OPEN toward the target | **+40°** |
| `m_thoraxRotFinish` | the finish — turned fully through | **+110°** |

A signed curve cannot satisfy those without one of them being seated negative, and none is. So the
convention was already decided by the content; the producer follows it. It also means the camera
tier invents nothing: a signed camera reading would have to be manufactured from the phase ladder,
and a sign inferred from *when* rather than *what* is not a measurement.

**The cost, stated plainly.** The curve passes through zero as the body squares up in the downswing,
so a peak reducer windowed from the top to impact sees the larger of the two excursions rather than
the open one. `m_pelvisRotPeak` and `m_thoraxRotPeak` are exactly that shape. Any measure that wants
the open peak specifically must window from square to impact, not from the top. Neither of those two
carries a corridor, so nothing grades on it today — but a future author needs to know.

---

## 4. Where the camera tier is weak — stated, and propagated

**Near square the cosine is flat.** `dθ/dw = −1 / (w₀ · sin θ)` diverges as `θ → 0`, so a pixel of
span noise becomes many degrees of turn. The producer does not hide this: it propagates the span
noise through that derivative into `MetricSeries::sigma`, with `sin θ` floored at `sin 5°` so the
reported uncertainty stays finite instead of running to infinity. A reader that ignores `sigma` will
over-read small turns.

The IMU tier leaves `sigma` **unset**, and that is deliberate rather than an omission — the field's
contract is explicit that absent means "not characterised", not "zero error", and no error budget is
propagated through that path.

**Above ~70° the span has collapsed into the noise** and the estimate saturates toward the 90°
ceiling `acos` can reach. A full shoulder turn sits right at that edge, which is why the thorax
corridors are seated wide and why the unit test asserts a 90° turn lands *high* rather than exactly.

**The address frame is assumed square to the camera.** It is the only reference available, and a
golfer set open or closed biases every reading by that amount. This is a **bias, not noise**: it does
not average out across a session, and a corpus study of it is the first open question below.

**Pelvic tilt and lateral bend also shorten the apparent span** and are indistinguishable from turn
in one projection.

None of that makes the number worthless — it makes it a camera estimate, which is what `Bridged`
exists to say. It does mean **no corridor over these may be seated tighter than the method
supports**, and the shipped ones (σ 10–12°) are not.

---

## 5. What Bridged means to the golfer

`BodyRotationProvider` is the only provider that returns `Bridged`, and its reason names the
**method** rather than a missing device:

> "estimated from the face-on camera — a pelvis / thorax IMU would measure it directly"

That phrasing matters. "Needs a pelvis IMU" reads as a refusal, and a value is in fact produced.
The distinction the three states carry is: *Measured* — we measured it; *Bridged* — we produced it,
by a weaker route, and here is the route; *Unavailable* — we did not produce it, and here is what
would be needed. Collapsing Bridged into either neighbour is a lie in one direction or the other.

---

## 6. Open questions

1. **How square is address, really?** The whole camera tier rests on `w_address` being the golfer's
   square width. A corpus study against the shaft track's own address geometry would bound the bias
   and is the single highest-value validation for this producer.
2. **Should the corridors be re-seated per tier?** A camera estimate and an IMU measurement of the
   same quantity have different error, and grading both against one corridor is a compromise. The
   norm set already resolves per context; a tier axis would be a natural extension, and is not worth
   building before a single shot has carried a trunk IMU.
3. **The saturation ceiling.** Above ~70° the estimate compresses. A calibrated correction is
   possible in principle but would be fitting a curve to a corpus we do not have.
4. **`kinematicSequence` is one step away.** `kinematic_sequence.h` already computes the ordered
   peak-speed nodes and the dashboard already consumes them; what is missing is angular-SPEED series
   for the pelvis and thorax, which the angle series here make a short follow-on. It carries no
   measure and no corridor, so it was left planned rather than promoted on speculation.
