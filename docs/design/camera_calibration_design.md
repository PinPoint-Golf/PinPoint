# Camera calibration — one ceremony, every camera, and the focus that makes it mean anything

**Status:** **DESIGN, not built.** Written 2026-08-28 after a day spent finding out that a phone's
preview had never once shown a frame, and that nothing anywhere measured whether a camera was in
focus. The slot it drops into already exists — `src/Gui/calibration/CameraCalibrationFlow.qml` is
an explicit stub for "stereo / ChArUco calibration", shaped like `ImuCalibrationFlow` and **already
instantiated by both `PpCameraPanel` and the session wizard** (§9.0) — so this needs no new screen
and no edit to either host.
**Scope:** how PinPointStudio establishes, records and *continuously verifies* what it knows about
each camera's geometry and focus — across Aravis, Spinnaker, USB and a PPCP phone — and how that
fits the flow of starting a session. ⚠ It deliberately covers the operator who has a printed
ChArUco board **and** the one who has a golf ball and a tape measure.
**Not in scope:** clock synchronisation (PPCP owns it, §6.3), colour, and the analysis-side question
of which metrics may be computed at which tier — named here, decided elsewhere.

---

## 1. Why this exists, and what it is actually for

Calibration is not one thing. It answers four separate questions, and a given operator may need
only some of them:

| | Question | Needed for |
|---|---|---|
| **Scale** | how many millimetres is a pixel, at the hitting plane? | clubhead speed, low point, ball speed |
| **Distortion** | is a straight line straight? | shaft angle, especially away from frame centre |
| **Pose** | where is this camera, relative to the ball and to the other cameras? | anything 3-D, and relating a camera to the hitting plane |
| **Focus** | is the image sharp enough to measure at all? | **all three of the above** |

⛔ **Focus is not a fourth item on the list — it is the precondition for the other three.** A
solve computed from soft corners returns numbers with small residuals and large errors, and
nothing about the arithmetic will tell you. This is why focus belongs *inside* calibration rather
than beside it in a camera-settings panel.

### 1a. What today actually does

Nothing. There is no calibration anywhere in PinPointStudio, and
`ppcp_source_declaration.cpp:457` declines to offer one for exactly the right reason:

> *"No `calibration` either, until the rig exists — 5.9's uncertainty is mandatory, so there is no
> way to offer a calibration without one."*

On the phone side, `AVFoundationCaptureDevice` locks focus after an **800 ms** bounded wait and, on
timeout, `waitForConvergence` simply `return`s — so `lockControls` locks wherever the lens happened
to be **mid-hunt**, silently. `focusPointOfInterest` is never set, `lensPosition` is never read or
recorded, `setFocusModeLockedWithLensPosition` is never called, and nothing re-asserts the lock
after an interruption. A camera can therefore be soft for an entire session with no signal at
either end.

---

## 2. Principles

1. **The protocol already has the model. Use it for every backend.** `CORE` §5.9 defines a
   Calibration as `{id, source_id, kind, parameters, uncertainty, method, observed_at}` with
   `uncertainty` **mandatory** and `method ∈ {factory, per_frame, solved, user_measured,
   estimated_online}`. That is the right shape for a FLIR camera as much as for a phone, and
   adopting it internally means the PPCP half is a serialisation rather than a translation.
2. **Uncertainty is not optional and not decoration.** A calibration without an error bar cannot be
   verified, cannot be compared to a later one, and cannot tell a metric whether to trust it. The
   protocol makes this impossible to skip; we should be glad.
3. **Gate on capability, never on backend or session type.** What a camera can do about focus is a
   property of the camera, not of its driver's name or of what kind of session is running. (The
   same rule the analysis pipeline had to learn.)
4. **PinPointStudio owns the ceremony and the record. The device owns the measurement.** Where the
   full-resolution pixels are is where the solve happens.
5. **Establish rarely, verify constantly.** A calibration is a bay-level asset with a long life. A
   session *checks* it; a session never *does* it.
6. **Degrade explicitly.** An operator with no board still gets scale and focus. What they do not
   get is recorded as not-got, per swing, and analysis reads that rather than guessing.
7. **Never block a golfer.** A red calibration is declared, recorded and shown — it does not refuse
   the session. Someone hitting balls beats a perfect rig.

---

## 3. The model

### 3.1 Bay

A **Bay** is a persistent, named physical setup: the cameras in it (by role and identity), their
calibrations, the reference geometry, and the board definition in use. A session *attaches to* a
bay.

⚠ **This is the single decision that makes the session flow work.** Calibration belongs to the bay,
because that is the thing that is actually stable: the phone on its mount, the FLIR on its tripod,
the mat, the ball position. Sessions come and go against it.

### 3.2 Calibration record

One record per `(bay, camera identity, kind)`, shaped on 5.9:

```
Calibration {
    id            stable, ours
    sourceId      the camera it describes
    kind          intrinsics | extrinsics | focus        (5.9's registry is open)
    parameters    kind-specific map
    uncertainty   MANDATORY, kind-specific
    method        factory | per_frame | solved | user_measured | estimated_online
    observedAt    when it was measured
    validFor      ⚠ see §5.4 — a lens position, for a camera whose focus can move
}
```

**Camera identity** is the serial number for Aravis/Spinnaker/USB, and `peer_id + source_id` for a
PPCP camera — 8.5c scopes ids by the minting peer, so a phone's `src:camera:wide` is only unique
alongside its peer id.

### 3.3 Tiers — what an operator can get with what they have

The `method` enum already is the tier ladder. Each rung is honest about what it buys:

| Tier | Method | What the operator has | Gives | Does **not** give |
|---|---|---|---|---|
| **0** | — | nothing | pixel measurements only | scale, distortion, pose |
| **1** | `factory` / `per_frame` | a driver that reports pixel pitch and focal length; a phone that delivers per-frame intrinsics | approximate scale *if* depth is known | distortion, pose, any error bar worth having |
| **2** | `user_measured` | **any object of known size** at the hitting plane | scale at that plane, with a real uncertainty | distortion, pose |
| **3** | `solved` | a printed ChArUco or checkerboard | focal length, principal point, distortion, reprojection RMS | pose |
| **4** | `solved` + `extrinsics` | the same board, left in place | everything, in a bay frame | — |

⛔ **Every swing records the tier it was captured under.** A metric that needs scale must read that
and refuse rather than produce a number in fictional millimetres. This is the same discipline as
`5.8f`'s "a value must not be used to mean unknown".

---

## 4. The reference object

The ceremony is the same shape whatever the operator owns; only the reference differs. The flow
asks, once: **what have you got?**

### 4.1 ChArUco board — the recommended path (tiers 3 and 4)

A checkerboard with ArUco markers in the white squares. ⛔ **Preferred over a plain checkerboard for
one decisive reason:** every corner has a unique identity, so a *partial* view still contributes.
A board large enough to fill a face-on camera at 3 m will not sit wholly inside a down-the-line
camera's frame, and a plain checkerboard contributes nothing unless the whole board is visible.

- **Definition**: squares across/down, square size in mm, marker size in mm, dictionary. Entered
  in-app (⛔ no file dialogs — house rule), with a couple of printable presets.
- **Plain checkerboard is still supported** for an operator who already has one; it simply requires
  the whole board in frame, and the flow says so.

### 4.2 Known-size object — the path for everyone else (tier 2)

The operator nominates something of known size at the hitting plane and draws across it:

- ⭐ **a golf ball — 42.67 mm, and the best reference in the building.** It is spherical, so its
  projected diameter is independent of orientation; it is high-contrast against a mat; it is at
  exactly the plane that matters; and **it is present in every single shot.**
- an alignment stick (1.2 m), an A4 sheet (210 × 297 mm), a club of measured length, or a tape
  measure laid on the mat.

Uncertainty comes from the detection: a ball ~40 px across measured to ±1 px is ±2.5 % scale. That
is a real number and it must be carried, not rounded away.

### 4.3 ⭐ The ball is also a permanent, free verifier

Because a ball is in frame at address for every swing, and PinPointStudio already detects it with a
radius (`CameraInstance::ballRadius`), **every shot carries its own scale check at no cost**. If the
ball's diameter in pixels at address drifts while the bay is nominally unchanged, either the camera
moved or the ball is not where it was. That is a calibration alarm no ceremony could provide, and it
runs forever.

---

## 5. Focus

### 5.1 Three capability classes, derived from the camera and not its backend

| Class | Cameras | What we can do |
|---|---|---|
| **A — programmable** | PPCP phone | read, set and lock `lensPosition`; sweep it ourselves |
| **B — mechanical** | Spinnaker, Aravis, most C-mount | nothing by API. A human turns a ring |
| **C — uncontrollable** | many USB webcams | AF may be running and cannot be stopped |

A `FocusCapability` descriptor — `canReadPosition`, `canSetPosition`, `canLock`, `isMechanical` —
selects the path. ⛔ Never branch on `Backend::`.

### 5.2 Class A: sweep, do not autofocus

Replace autofocus with a **deterministic sweep**: step `lensPosition` across a bracketed range,
capture a frame at each, compute a sharpness metric over the reference ROI, fit a parabola about
the maximum, and lock the sub-step peak with `setFocusModeLockedWithLensPosition`.

⚠ **The curve is the evidence, and this is the whole point.** Autofocus yields a number; a sweep
yields a number *and* a reason to believe it:

- a sharp unimodal peak → real focus, and its width is the uncertainty;
- a flat curve → the ROI had no texture; the result is untrustworthy and says so;
- two peaks → glass, a reflection, or a mirror in the bay.

### 5.3 Class B: guide the human, then verify

The software cannot turn the ring, so it does the two things it can. **Guide**: a live sharpness
bar with peak-hold — "turn until it peaks, then lock the ring". **Verify**: measure after, record
the value, and alarm if it changes. Mechanical focus does not drift on its own, so once verified it
stays verified until something is knocked — which is exactly what §7 watches for.

⚠ Also tell the operator the thing they can act on: **stopping the aperture down buys depth of
field**, and depth of field is what makes focus robust to a golfer moving.

### 5.4 ⛔ Focus and intrinsics are not independent

Changing focus **changes the intrinsic matrix**. That is why `REQ-OPT-2` locks focus for the
session, and why 5.11m makes a preview profile declare `intrinsics: none` — a decimated, rescaled
view has a different matrix and must not pretend otherwise.

**Therefore a Calibration is only valid at the lens position it was solved at.** The record carries
that position (`validFor`), verification checks it, and re-focusing **invalidates the intrinsics** —
which is what 5.11a1's `calibration_changed` close reason exists to say.

### 5.5 Measuring sharpness honestly

Sharpness metrics are content-dependent, so an absolute threshold is meaningless — a blank mat
scores zero however sharp it is. Two measurements, used together:

1. **Edge-spread on a step edge** — the 10–90 % rise distance, in pixels, across the ball's limb or
   a board square. This estimates the blur circle in physical units and is self-normalising,
   because a step edge is a step edge whatever the scene.
2. ⭐ **Its anisotropy, measured at several orientations around the edge.** Defocus blur is
   isotropic (a disc); motion blur is directional (a line). One measurement therefore says *which
   blur you have* — and separates the two failure modes that are otherwise endlessly confused.

⚠ **Both must be measured at address**, where the subject is nearly still, or motion blur
contaminates the reading. Pose or ball-presence detection is the right tool for spotting that
moment — used to decide *when* to measure, never *what* to measure.

### 5.6 ⚠ What "soft" often actually is

Two things masquerade as focus and must be ruled out before anyone re-focuses anything:

- **The preview cannot show focus.** A PPCP preview is 640×360 JPEG at quality 0.6, downscaled 3×
  from 1080p. That pipeline destroys precisely the high-frequency content focus consists of. It will
  look soft when the capture is pin-sharp. The tile says `Preview` for this reason.
- **Exposure, not focus, dominates blur at impact.** `setExposureModeCustom` is never called, so
  exposure sits wherever auto landed — indoors at 240 fps, at or near the full 4.2 ms frame
  duration. A clubhead at 40 m/s travels **~17 cm** during one exposure. No focus setting fixes
  that; more light and a shorter exposure do.

---

## 6. Multiple cameras

### 6.1 ⭐ The board stays put; the cameras take turns

An earlier version of this argument said extrinsics need the board seen by two cameras
simultaneously, which is awkward in a real bay. **It does not.**

Lay the board flat at the ball position and **leave it there**. Each camera observes it whenever it
likes and solves its own pose relative to the board. Because they all reference the same stationary
object, they all land in one frame — with no simultaneous view, no shared trigger, and no clock
alignment required.

Better still, the frame is **physically meaningful**: put the board's origin at the ball and one
axis along the target line, and the bay frame is the frame the golf metrics already want.

The two conditions are checkable, and §7 checks them: the board must not move between observations,
and no camera may move afterwards.

### 6.2 What each camera contributes

Intrinsics are per-camera and independent — each can be solved alone, in any order, at any time.
Only pose needs the shared board. So a bay can be brought up incrementally: a new camera added next
week is solved on its own and placed in the existing frame without redoing anything.

---

## 7. Verification and invalidation

Establishment is a ceremony. **Verification is continuous, cheap, and is where the value actually
is.**

### 7.1 A static fiducial patch

Put a small high-frequency target (a Siemens star or a few checker squares) permanently in frame at
roughly the hitting plane — the edge of the mat, or the bay wall. Because it never moves, its
sharpness is a **pure** focus signal: no motion, no content change, no inference needed. One small
ROI per frame.

⭐ It catches two faults for the price of one: focus moving, and **the camera being bumped** — which
is at least as dangerous and currently invisible.

### 7.2 The checks, and what each costs

| Check | Cost | Catches |
|---|---|---|
| fiducial sharpness | one small ROI/frame | focus drift, camera bump |
| ball diameter at address | already computed | scale drift, camera moved, ball moved |
| `lensPosition` / `focusMode` poll | one property read/s | a lock silently lost |
| board glimpse (opportunistic) | only if the board is in frame | pose drift |

### 7.3 Declaring it

A violated calibration is **reported, not silently repaired**. It is recorded on the swing, shown
on the camera tile, and — for a PPCP camera — is what `calibration_changed` (5.11a1) exists to say.
Re-focusing or re-solving is an explicit, operator-visible act that supersedes the record.

---

## 8. Per-backend notes

### 8.1 PPCP phone

- **Intrinsics arrive free, per frame** (`REQ-OPT-7`, `kCMSampleBufferAttachmentKey_CameraIntrinsic
  Matrix`), so its native tier is 1 with `method: per_frame`. Our board does not *replace* that — it
  **cross-checks** it. Two independent estimates with uncertainties is a stronger position than
  either alone.
- ⛔ **The solve must happen on the phone.** PinPointStudio receives only 640×360 preview; solving
  intrinsics from it is meaningless, because the downscale changes the very matrix being measured
  and destroys sub-pixel corner precision. The phone detects corners, solves, and reports.
- ⚠ **There is no "please calibrate" message in PPCP, and we should not invent one yet.** The phone
  offers the action in its own UI and reports the result unprompted via `calibration_update` — which
  already exists, needs no CR, and matches the ownership logic we accepted for `stream_open`.
- PinPointStudio must start consuming `PPCP_EVENT_CALIBRATION_UPDATE`; it is unhandled today.
- Focus is class A, and §5.4 applies with full force: the phone's intrinsics move with focus, so its
  calibration is keyed to a lens position.

### 8.2 Spinnaker / Aravis

- Fixed C-mount optics: focus and aperture are **mechanical**, class B. Guide-and-verify (§5.3).
- Intrinsics are stable and worth solving properly once — tier 3 is genuinely achievable and stays
  achieved.
- Global-shutter parts remove the rolling-shutter caveat that a phone carries (`readout_ns`, 6.2,
  and `VideoInputPpcp::rowInstantNs`). Worth recording which it is.

### 8.3 USB / Qt Multimedia

- Often class C: autofocus that cannot be stopped. ⛔ **Detect it and declare it.** A camera whose
  focus we cannot hold is a tier-2 camera at best, and its swings say so. Never present an
  uncontrollable camera as locked.

---

## 9. ⭐ How this fits the flow of a new session

**The session verifies. The session never calibrates.**

### 9.0 ⭐ Both hosts already exist, and so does half the concept

This does not need a new screen, and — importantly — **it needs no edit to
`ScreenSessionWizard.qml`**, which is under a no-touch-without-approval rule. The stub is already
instantiated twice:

- `PpCameraPanel.qml:178` — `layoutMode: "compact"`, the Settings path, with `onCompleted` /
  `onCancelled` returning to the camera list;
- `ScreenSessionWizard.qml:1144` — `layoutMode: "full"`, already a step in the new-session flow.

So **implementing the flow lights up both slots and touches neither host.**

⚠ And the wizard has already reasoned about this further than expected. Beside that step sits a
`CheckRow` labelled *Triangulation* bound to `root._todo_triangulationValid` — a placeholder waiting
for exactly the extrinsics validity of §6 — and it is marked:

```qml
optional: root.anyFixedCamera
subFail:  root.anyFixedCamera ? qsTr("OPTIONAL — CAMERAS ARE FIXED IN PLACE")
                              : qsTr("NOT CONFIRMED")
```

⛔ **That is the Bay concept, already encoded.** The wizard has always known that cameras fixed in
place do not need re-calibrating every session. §3.1 gives that intuition a home and a record;
`_todo_triangulationValid` is the signal it has been waiting for.

**Bay setup — rare, deliberate.** The full ceremony, in the two slots above. A golfer walking the
wizard every session sees a *check*, not a ceremony: the wizard step's job is to show the verdict
and offer re-calibration, not to demand one.

**Starting a session — automatic, seconds, no interaction.** The wizard chooses cameras and views
as it does now. On session open, a **calibration health check** runs per camera:

1. restore the stored calibration for this bay and camera identity;
2. confirm the identity still matches (serial, or peer + source);
3. for class A, confirm `lensPosition` equals the calibrated one — re-assert if not;
4. measure the fiducial, and the ball if it is there, against their references;
5. publish a verdict.

| Verdict | Meaning | What happens |
|---|---|---|
| 🟢 **verified** | calibration restored and confirmed | nothing; proceed |
| 🟠 **degraded** | usable, with a named reason (lower tier, stale, unverifiable) | proceed, recorded on every swing |
| 🔴 **uncalibrated** | nothing valid for this camera | proceed, metrics needing scale withheld |

⛔ **None of them stops the session.** A golfer hitting balls beats a perfect rig; what matters is
that the record is honest about what was known at the time.

**During the session:** the §7 checks run continuously; a violation raises a toast on the session
screen (that is the established channel for something the operator must act on *now*) and is
recorded on subsequent swings.

**On every swing:** the calibration `id` **and a digest of it**, plus the tier and the verification
result. ⚠ By reference *and* digest, so a re-analysis six months later knows exactly which geometry
was in force and cannot silently pick up a newer one — the same concern already raised about
`swing.json` provenance.

---

## 10. Implementation staging

Each stage is independently useful, and nothing later is needed for something earlier to pay off.

| # | Stage | Notes |
|---|---|---|
| **1** | **Make the current focus failure visible.** Report `waitForConvergence` timeout instead of returning silently; read and record `lensPosition` at lock; re-assert the lock after `interruptionEnded`. | An afternoon on the phone. Tells us whether a bad lock is the real problem *before* anything is built on the assumption that it is. |
| **2** | **Calibration record + per-bay store**, shaped on 5.9. | Where focus lives. No UI yet. |
| **3** | **Link `opencv_calib3d` and `opencv_objdetect`.** | ⚠ PinPointStudio deliberately links only core/imgproc/imgcodecs to keep VTK out of the bundle — but VTK comes from `opencv_viz`, not these. Say so in the comment or someone will "fix" it back. |
| **4** | **Tier-2 known-size flow** — nominate an object, draw across it, store scale + uncertainty. | Smallest thing that gives a real answer, and it serves the operator with no board. |
| **5** | **Focus: sweep for class A, guide-and-verify for class B**, stored as `kind: focus`. | Delivers the original ask. ⚠ Do stage 1 first — it may show the fix is smaller than this. |
| **6** | **Fiducial + ball continuous verification**, and the session health check of §9. | The part that keeps it true. |
| **7** | **ChArUco intrinsics solve** for local cameras. | Tier 3. |
| **8** | **PPCP on-device solve** and `calibration_update` consumption. | Largest; do it once the shape is proven locally. |
| **9** | **Extrinsics and the bay frame.** | Unlocks multi-camera 3-D, and finally supplies `ScreenSessionWizard`'s `_todo_triangulationValid` — a placeholder that has been waiting for it. |

---

## 11. What must be measured before committing

⚠ Written down because today's lesson was that reasoning ahead of evidence is expensive.

1. **The real depth of field at bay distances.** Estimates put the iPhone wide's hyperfocal near
   3.5–4 m, which would make everything from ~1.7 m to infinity acceptably sharp and would mean a
   golfer moving is *not* a focus problem at all. Ten minutes with a tape measure and a chart
   settles it, and it decides how much of §5 is worth building.
2. **Whether a full-resolution frame at address is actually soft.** If it is not, this is a preview
   artefact plus an exposure problem, and §5.6 is the whole answer.
3. **Whether the phone can detect ChArUco corners at capture resolution without disturbing
   capture.** Decides whether stage 8 is a background task or a separate mode.
4. **Board size and square pitch** for the actual distances in the bay.
5. **Ball-diameter detection repeatability** at address — it sets the uncertainty on tier 2, and
   tier 2 is the path most operators will be on.

---

## 12. Open questions

- **Requesting a calibration over PPCP.** §8.1 proposes the phone offers it locally and reports
  unprompted, needing no CR. If a host-driven trigger is wanted later, that is a genuine protocol
  gap and a candidate CR — but it should be argued from a real need, not designed in advance.
- **A `calibration` kind registry.** `intrinsics`, `extrinsics` and `focus` are proposed here. 5.9's
  registry is open, so this costs nothing, but the names should be agreed with the protocol team
  before they appear in a bundle.
- **Which metrics may be computed at which tier.** Named in §3.3, decided in the analysis model.
- **Rolling shutter.** `readout_ns` is already declared and `rowInstantNs` already applies it;
  whether calibration should *verify* the declared value is a separate question worth asking.
