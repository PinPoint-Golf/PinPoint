# Upper-body metrics from a face-on camera

**Audience**: developers and content authors working on the chest, shoulder and arm half of the diagnostics model
**Code**: `src/Analysis/upper_body_metrics.{h,cpp}`, `UpperBodyMetricsStage` (`wrist_analyzer.cpp`), `UpperBodyMetricProvider`
**Content**: `src/Resources/diagnostics/core.json`, `norms.json`
**Status**: producer live; nine series shipped; the detection engine that turns them into findings is still dormant (`diagnostics_developer_guide.md` §8)
**Written**: 2026-08-02

The chest-and-arms counterpart to [`lower_body_face_on_metrics.md`](lower_body_face_on_metrics.md).
Read that first — the frontal-plane rule, the robust-address-reference shape and the
percentage-of-a-body-span unit argument are all stated there and are not repeated here.

---

## Contents

1. [What prompted this](#1-what-prompted-this)
2. [Built on the anatomy vocabulary, not on keypoint indices](#2-built-on-the-anatomy-vocabulary-not-on-keypoint-indices)
3. [What ships, and the geometry of each](#3-what-ships-and-the-geometry-of-each)
4. [Two channels that needed a decision](#4-two-channels-that-needed-a-decision)
5. [The duplicates that were retired instead of built](#5-the-duplicates-that-were-retired-instead-of-built)
6. [Signs, and how they are pinned](#6-signs-and-how-they-are-pinned)
7. [What is deliberately still planned](#7-what-is-deliberately-still-planned)

---

## 1. What prompted this

An audit of the metric catalogue against what a face-on camera actually resolves. Of 37 `.planned`
descriptors, a large majority needed no new sensing at all — the pose track that produces
`pelvisSway` and `headTilt` today carries every keypoint the upper-body channels need. Meanwhile
49 of 109 authored measures in the diagnostics pack were blocked on a missing producer, and those
blocked **70 of 145 conditions**: nearly half the library was written, cited, corridor-seated and
ungradeable.

The corridors were the surprise. They were **already authored** for almost every target measure —
so this was mostly a producer gap, not a content gap, and shipping the producers turned a status
flip into a working diagnosis rather than starting a new authoring round.

---

## 2. Built on the anatomy vocabulary, not on keypoint indices

`lower_body_metrics.cpp` predates `Diagnostics/anatomy_vocabulary.{h,cpp}` and spells its own
`kLHip = 11`. This module does not. Every point and segment resolves through `resolvePoint()` /
`resolveSegment()`, and there are three reasons, in order of importance:

**The content already names this geometry.** The composed measures in `core.json` carry a `series`
facet triple, authored before any producer existed:

| Measure | `what` | `quantity` | `reference` |
|---|---|---|---|
| `m_shoulderPlane` | `shoulderLine` | `angle` | `ground` |
| `m_thoraxDrift` | `thoraxCentre` | `distance` | `trailAnkle` |
| `m_trailElbowRise` | `trailElbow` | `height` | `shoulderLine` |
| `m_leadHandWidth` | `leadHand` | `distance` | `thoraxCentre` |
| `m_leadArmToTorso` | `leadUpperArm` | `angle` | `spine` |

**That table is the producer specification.** Building to the same vocabulary makes the join exact
rather than coincidental — `diagnostics_content_extension.md` §26 flagged precisely this risk
("confirm the facet-derived id will match the eventual producer key, or the join will silently
miss"). It is also why `m_thoraxDrift` is measured **from the trail ankle** and not from address:
the facet says so, and the measure over it is a `delta` that does the address referencing itself.

**Handedness and layout resolve once.** A role is lead or trail, never left or right, and a role
missing from a 17-keypoint track reports *why* rather than silently reading a neighbouring index.

**The overlay and the measurement agree by construction.** The vocabulary is explicitly intended to
become the single source of truth for the skeleton overlay too, so what is drawn is definitionally
what is measured.

The resolver works in whatever units the caller's points are in, so each frame is de-normalized to
pixels once into a scratch array. Both dimensions, always: keypoints arrive normalized by width and
height separately, and scaling by width alone makes every angle a statement about the aspect ratio.

---

## 3. What ships, and the geometry of each

| Metric | Geometry | Unit |
|---|---|---|
| `secondaryAxisTilt` | mid-hip→mid-shoulder line from vertical | ° |
| `spineSideBend` | shoulder-line tilt against hip-line tilt | ° |
| `thoraxLateralDrift` | chest centre along the stance line, from the trail ankle | % stance width |
| `shoulderPlaneAngle` | shoulder-line tilt from horizontal | ° |
| `elbowAlignment` | elbow-line tilt from horizontal | ° |
| `trailElbowHeight` | trail elbow's vertical height above the shoulder line | % shoulder width |
| `leadHandWidth` | lead hand to chest centre | % arm length |
| `leadUpperArmToChest` | chest centre to the lead upper-arm SEGMENT | % shoulder width |
| `leadArmToTorso` | lead upper arm against the torso axis | ° (unsigned) |

Two more landed in `lower_body_metrics.cpp` rather than here, because they read the hips, knees and
ankles that module already resolves: `feetAlignment` (the ankle-line tilt) and `comOverLeadFoot`
(the pelvis centre's distance from the lead ankle along the stance line).

**Which denominator, and why it is not a free choice.** `% shoulder width` means the Euclidean
shoulder separation, because that is what it already means in the shipped `stanceWidth`.
`% stance width` means the absolute horizontal ankle span, because that is what it already means in
the shipped `pelvisSway`. A new producer does not get to redefine an existing unit string — a
corridor declares one unit and grading never looks at it again, so a silently different scale is a
wrong answer wearing a right answer's clothes.

---

## 4. Two channels that needed a decision

### `spineSideBend` is the shoulder line against the hip line

Side bend is lateral flexion of the **thorax relative to the pelvis**, and neither pose layout
carries a keypoint between the shoulders and the hips. The tempting reading — the neck-to-pelvis
line from vertical — is not side bend at all: it is a whole-body lean, and it is already shipped as
`secondaryAxisTilt`. Two metrics computing the same number under two names would have been the
defect this batch spent most of its effort avoiding elsewhere.

So side bend is `hipLineTilt − shoulderLineTilt`: two segments that both exist, and a difference
that **cancels** the whole-body tilt. Tilt both lines together and it reads zero, which is correct —
that is a lean, not a bend. Drop only the trail shoulder and it reads positive, which is the trail
shoulder working down and under the turn. The unit test asserts both cases directly.

### `trailElbowHeight` is a VERTICAL height, not a perpendicular distance

A perpendicular distance to the shoulder line needs a cross product, and the sign of a 2-D cross
product depends on whether the lead side is image `+x` — so it silently inverts on a mirrored
camera, which is the exact failure the absolute-denominator line form exists to avoid. The vertical
height above the line at the elbow's own `x` has no such dependency, and it is also what the
coaching reading means: the elbow rising out of the turn, not its distance from an inclined axis.

---

## 5. The duplicates that were retired instead of built

`shoulderAlignment` and `hipAlignment` were in the design catalogue as separate metrics. Each was
geometrically **identical** to a line this product already carries — `shoulderPlaneAngle` and the
live `hipLineTilt` — differing only in which phase it was read at.

`metric_reducer.h` exists to express exactly that: *a series is what a producer produces; a reducer
is how it is sampled.* Its header says conflating the two "makes one producer look like four
separate roadmap items". Two descriptors for one curve is two names for one number, and here it was
worse than usual: the two carried opposite sign conventions, so a reader comparing them would have
concluded the golfer's hips and shoulders disagreed when they had simply been described twice.

Both descriptors were deleted (the catalogue went 72 → 70), and the measures `m_shoulderAlignment`
and `m_hipAlignment` now point at the surviving series with their labels and `highMeans` restated in
its convention.

`feetAlignment` was NOT retired against `toeLineAngle`, and the distinction is worth stating. The
content's own argument holds: the ankle joints are far less affected by foot flare than the toes,
and the impact read has no counterpart in an address-only scalar. It also fixes a sign defect —
`toeLineAngle` is a raw `atan2` of the lead→trail vector and inverts for a left-handed golfer.

---

## 6. Signs, and how they are pinned

Full table in [`pinpoint_sign_conventions.md`](pinpoint_sign_conventions.md). The rule here:

- **Body lines** — `shoulderPlaneAngle`, `elbowAlignment`, and `hipLineTilt` / `feetAlignment` next
  door — are positive when the **trail end sits above the lead end**, computed against the absolute
  horizontal separation so the answer does not flip for a mirrored camera.
- **`secondaryAxisTilt` is trail-positive**, the one lateral channel that deliberately breaks the
  lead-positive default. The quantity is named for the lean *away from the target*.
- **`leadArmToTorso` is unsigned.** A frontal projection cannot say which side of the torso the arm
  left on, and putting a sign on a quantity the camera did not resolve would be a fabrication.

A sign is the one thing a synthetic track can pin exactly and a corpus cannot, which is why
`upper_body_metrics_test.cpp` runs its whole sign suite a second time through a **mirrored camera
with a left-handed golfer** and asserts the numbers are unchanged. A convention that only holds for
a right-hander filmed from one side is not a convention. That test is also what makes a SwingLab
corpus pass unnecessary for this batch.

---

## 7. What is deliberately still planned

Restating the frontal-plane rule as a work queue, because "why not the rest of the upper body too"
is the obvious question:

- **`spineForwardBend`** — sagittal. The hinge from the hips is the plane a face-on camera
  foreshortens to almost nothing, and four characteristics over it would be graded off projection
  error. With the knee flexions, the strongest argument for a down-the-line pipeline.
- **`thoracicFlexion` / `lumbarExtension`** — no keypoint exists between the shoulders and the hips
  in EITHER layout, so these cannot come from the skeleton at all. They stay roadmap items rather
  than capture gaps because upper-back rounding and low-back arch are both plainly visible in the
  BACK CONTOUR of a down-the-line silhouette. That makes them a producer worth building, not a gap
  that can never close.
- **`ballBodyDistance`** — depth, across the stance line. The face-on camera's own axis.
- **`hipInternalRotation`** — needs a pelvis IMU plus thigh IMUs.

`pelvisRotation` and `thoraxRotation` used to head this list and no longer do. They are also not
frontal-plane quantities, but the image span of a body line collapses by the cosine of its turn, and
an honest estimate with a stated uncertainty beat leaving eleven characteristics dark. See
[`body_rotation_estimation.md`](body_rotation_estimation.md).
