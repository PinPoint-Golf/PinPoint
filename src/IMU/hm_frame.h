/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include <QQuaternion>
#include <QVector3D>
#include <cmath>

#include "../Core/pp_tuned_constants.h"

// ---------------------------------------------------------------------------
// HackMotion anatomical frame -> PinPoint anatomical frame (Phase D)
// ---------------------------------------------------------------------------
//
// The wG3 applies its own calibration and streams quaternions ALREADY IN ITS OWN
// anatomical frame. That frame's axes are never defined by the device's protocol
// specification, and the constant rotation from it to ours is the whole of Phase
// D. This header holds that constant, the reason it can only be one of four
// things, and — importantly — the reason the acceptance test the phase was
// originally given cannot pick between them.
//
// ---------------------------------------------------------------------------
// WHY THE ANSWER IS ONE OF FOUR, AND NOT A THREE-DEGREE-OF-FREEDOM FIT
// ---------------------------------------------------------------------------
//
// Both frames are built on the same two anatomical landmarks — a limb axis and a
// flexion axis — so the map between them is an AXIS-ALIGNED rotation, not an
// arbitrary one. Two facts pin it down to four possibilities:
//
//   1. BOTH FRAMES PUT THE LIMB AXIS ON Y. Ours by construction
//      (imu_calibration::solveSegment builds e_y = long axis, distal; see
//      docs/design/imu_frame_contract.md §5).
//
//   2. THE OTHER TWO AXES CARRY THE OPPOSITE DOFs. We read flexion/extension
//      about Z and radial/ulnar deviation about X (wrist_angles.h:127-128,
//      hardware-locked 2026-06); the device reads them the other way round. So
//      the map must carry X -> ±Z and Z -> ±X.
//
// A rotation satisfying both, with determinant +1, is one of exactly four. Two
// keep the limb axis pointing distally in both frames (a ±90° rotation about it)
// and two reverse it. That is the entire solution space.
//
// ⚠ BOTH OF THOSE ARE ASSUMPTIONS ABOUT THE DEVICE, AND THE CAPTURE TESTS THEM
// RATHER THAN ASSUMING THEM THROUGH. tools/hm_frame_select.py checks each one
// against the recording it is given — the limb axis by where a deliberate
// forearm rotation actually puts the lower-arm unit's angular velocity, and the
// axis-alignment by whether any candidate can produce low cross-talk at all. If
// either check fails, this four-candidate reduction does not hold and the phase
// falls back to a general empirical solve. Do not set a candidate on a capture
// that failed them.
//
// ---------------------------------------------------------------------------
// ⚠ CROSS-TALK CANNOT SELECT THE CANDIDATE. SIGN AGAINST A DIRECTED MOTION CAN.
// ---------------------------------------------------------------------------
//
// This is the trap, and it is worth reading twice because Phase D's original
// acceptance criterion walks straight into it.
//
// That criterion was: "a pure single-axis motion must move one PinPoint DOF and
// leave the others near zero — cross-talk is the number". ALL FOUR CANDIDATES
// SCORE EXACTLY ZERO CROSS-TALK. They differ only in the SIGNS they produce, and
// so does a reversed composition order. A capture that is beautifully
// single-axis, and a decomposition that reads a textbook 0.00° on both secondary
// channels, still leaves eight possibilities standing.
//
// hm_frame_test.cpp pins this as an executable fact so it cannot be "simplified"
// back out.
//
// What DOES select is the sign of a motion whose anatomical direction was
// recorded when it was performed. Each candidate has a unique signature:
//
//     candidate   flexion   deviation   pronation
//     ---------   -------   ---------   ---------
//     C1             +          -           +
//     C2             -          +           +
//     C3             +          +           -
//     C4             -          -           -
//
// The (flexion, deviation) pair alone is already unique across the four, so TWO
// directed motions settle it — one known bow, one known ulnar deviation. In our
// convention +flexion = bowed, +deviation = ulnar, +pronation = pronation (ISB /
// Wu et al. 2005; docs/design/pinpoint_sign_conventions.md Rule 0).
//
// ⚠ SO THE CAPTURE MUST RECORD WHICH WAY EACH MOTION WENT. "Pure flexion" is not
// enough information to select a candidate; "bowed, then returned" is. An
// unlabelled capture cannot settle this no matter how clean it is.
//
// ⚠ AND DO NOT CHECK THE ANSWER AGAINST THE VENDOR'S OWN APPLICATION. It reports
// the inverse of us on bow/cup — extension positive where we are flexion
// positive, because we follow ISB and they do not. A CORRECT selection therefore
// shows an INVERTED SIGN on the primary channel next to their display. See
// pinpoint_sign_conventions.md Rule 0; the disagreement is deliberate.
//
// ---------------------------------------------------------------------------
// WHAT CROSS-TALK IS STILL FOR
// ---------------------------------------------------------------------------
//
// It is the FALSIFICATION test for this whole approach rather than the selector.
// The four-candidate reduction rests on the two frames being axis-aligned to each
// other. If a directed capture shows large cross-talk under EVERY candidate, that
// assumption is wrong and the phase falls back to a general empirical solve.
// Small residuals are expected and are not a frame error: a human performing a
// "pure" single-axis motion contributes most of it.
//
// ---------------------------------------------------------------------------
// COMPOSITION — AND WHY NOTHING DOWNSTREAM CHANGES
// ---------------------------------------------------------------------------
//
// The device streams WORLD -> BODY. Our anatomical quaternions run the other way
// (imu_calibration::toAnatomical builds A·q_raw·M, and wrist_angles.h's
// qFore⁻¹·qHand only typechecks if q maps body -> world). So the conjugate is
// part of the reconciliation, not a cosmetic fix:
//
//     q_anat_pps = q_hm* ⊗ R_ph          R_ph : PPS-anatomical -> HM-anatomical
//
// which is EXACTLY toAnatomical(A = identity, q_raw = q_hm*, M = R_ph). The
// existing helper does the work, wrist_angles.h is used unmodified, and the
// relative rotation falls out as a similarity transform:
//
//     q_rel_pps = R_ph* ⊗ q_rel_hm ⊗ R_ph
//
// ⚠ THE CONJUGATE BELONGS HERE AND NOWHERE ELSE. hm_sample_convert.h stores the
// streamed quaternion verbatim and says at length why it refuses to conjugate;
// that refusal is correct and must stay. Conjugating in two places is the same as
// conjugating in none.
//
// ⚠ hm_quat_relative() RETURNS THE OTHER ORDER. The library gives
// q_palm ⊗ q_arm*, which is a correct relative ATTITUDE in the device's own
// world->body convention and the conjugate of what this path needs. It is fit for
// the presence check (an angle, convention-blind) and unfit as a decomposition
// input. Conjugate it, or build q_rel from the two anatomical quaternions.
//
// ---------------------------------------------------------------------------
// ⚠ THIS IS A MOUNTING CONSTANT, NOT A DEVICE CONSTANT
// ---------------------------------------------------------------------------
//
// It describes where the boards sit on the strap. Move the strap and it changes.
// Any capture that selects a candidate must record the mounting that produced it.

namespace pinpoint::hm_frame {

// One entry of the solution space. `q` is R, the map HM-anatomical ->
// PPS-anatomical, as {w, x, y, z}; `basis` states where it sends the device's
// axes, which is the form that can actually be checked by eye.
struct Candidate {
    const char *name;
    const char *basis;
    float       q[4];
    int         flexionSign;
    int         deviationSign;
    int         pronationSign;
};

// sqrt(1/2), spelled out so the table reads as exact quarter turns.
inline constexpr float kS = 0.70710678f;

// ⚠ ORDER IS THE CONTRACT. pinpoint::tuned::hmframe::kCandidate indexes this
// table, so entries may be corrected but never reordered or inserted between.
inline constexpr Candidate kCandidates[4] = {
    { "C1", "Ry(-90): x->+z, y->+y, z->-x", { kS, 0.0f, -kS, 0.0f }, +1, -1, +1 },
    { "C2", "Ry(+90): x->-z, y->+y, z->+x", { kS, 0.0f, +kS, 0.0f }, -1, +1, +1 },
    { "C3", "180 about (1,0,1): x->+z, y->-y, z->+x", { 0.0f, kS, 0.0f, +kS }, +1, +1, -1 },
    { "C4", "180 about (1,0,-1): x->-z, y->-y, z->-x", { 0.0f, kS, 0.0f, -kS }, -1, -1, -1 },
};

inline constexpr int kCandidateCount = 4;

// Has a directed capture selected one yet? Until it has, a HackMotion lane has no
// anatomical frame and must not pretend to: callers keep anatCalibrated false and
// leave anatQuat at identity rather than driving a segment with a frame nobody
// has reconciled.
inline bool isSelected(int candidate = pinpoint::tuned::hmframe::kCandidate)
{
    return candidate >= 0 && candidate < kCandidateCount;
}

// R — HM-anatomical -> PPS-anatomical. The frame map itself.
inline QQuaternion frameMap(int candidate)
{
    if (!isSelected(candidate)) return QQuaternion();
    const Candidate &c = kCandidates[candidate];
    return QQuaternion(c.q[0], c.q[1], c.q[2], c.q[3]).normalized();
}

// R_ph — PPS-anatomical -> HM-anatomical. This is the value that goes in
// toAnatomical's `M` slot, which is why it is named for that role: M has always
// meant anatomical-body -> sensor-body, and for a HackMotion the "sensor" frame
// is the device's own anatomical one.
inline QQuaternion mountM(int candidate = pinpoint::tuned::hmframe::kCandidate)
{
    return frameMap(candidate).conjugated();
}

// One unit's streamed quaternion -> our anatomical convention.
//
// `qHmWorldToBody` is the quaternion exactly as the device sent it, AFTER the
// device's own calibration has been applied (before that it carries board
// placement, not anatomy, and this function's output is meaningless).
//
// Returns identity when no candidate has been selected, so an unreconciled lane
// parks rather than driving anything.
inline QQuaternion toAnatomical(const QQuaternion &qHmWorldToBody,
                                int candidate = pinpoint::tuned::hmframe::kCandidate)
{
    if (!isSelected(candidate)) return QQuaternion();
    // A · q_raw · M with A = identity: the device already referenced the pair at
    // its own pose 0, so there is no per-session world->anatomical solve to do
    // here. What that pose leaves behind is a constant offset from OUR neutral,
    // which wristRel's Address reference absorbs downstream.
    return (qHmWorldToBody.conjugated() * mountM(candidate)).normalized();
}

// The relative rotation of hand with respect to forearm, in our convention,
// from the two streamed quaternions.
//
// ⚠ Note the argument order and the conjugates. Both are load-bearing and
// neither is visible in the resulting ANGLE — see the cross-talk warning above.
inline QQuaternion wristRelFromHm(const QQuaternion &qLowerArmWorldToBody,
                                  const QQuaternion &qPalmWorldToBody,
                                  int candidate = pinpoint::tuned::hmframe::kCandidate)
{
    const QQuaternion fore = toAnatomical(qLowerArmWorldToBody, candidate);
    const QQuaternion hand = toAnatomical(qPalmWorldToBody, candidate);
    return (fore.conjugated() * hand).normalized();
}

// ---------------------------------------------------------------------------
// PRONATION RATE — the device's third wrist metric, and it is a RATE
// ---------------------------------------------------------------------------
//
// ⚠ THIS IS NOT `forearmPronation`, AND MUST NEVER BE PUBLISHED AS IT.
// Our forearmPronation is an ISB radioulnar joint ANGLE, read as the axial twist
// of the forearm with respect to the UPPER ARM (wrist_angles.h's
// forearmPronElbowFlex, driven by qUpper⁻¹·qFore). A wG3 has no upper-arm unit,
// so that angle is not available from this device at all, and calling something
// else by its name would claim an ISB conformance we would not have —
// pinpoint_sign_conventions.md is explicit that four of our metrics are ISB joint
// angles and the rest must never imply they are.
//
// What IS available is the angular RATE about the forearm's own long axis, in
// °/s. That is a whole-forearm quantity rather than a joint articulation, and it
// is the same quantity the device itself reports as its third wrist metric.
//
// ⚠ IT IS A RATE, SO THE CALIBRATION POSE DOES NOT BIAS ITS SIGN — only the
// direction of turning does. Worth stating because that pose puts the forearm
// PALM DOWN, i.e. already near full pronation, so "neutral" for this device is
// not anatomical neutral. An ANGLE referenced to it would carry that offset and
// would need saying so; a rate does not.
//
// ⚠ TAKE IT FROM THE LOWER-ARM UNIT, NOT FROM A DIFFERENCE OF THE TWO. During
// pronation both units rotate together — the wrist barely articulates about this
// axis — so differencing them cancels most of the signal being measured. This is
// the one channel where the relative quantity is the wrong one.
//
// The sign follows the selected candidate, which makes this an independent third
// check on the selection: C1/C2 preserve the device's own sense about the limb
// axis and C3/C4 reverse it.
inline float pronationRateDps(const QVector3D &gyroLowerArmDps,
                              int candidate = pinpoint::tuned::hmframe::kCandidate)
{
    if (!isSelected(candidate)) return 0.0f;
    // The device's gyro shares the quaternion's frame, and its calibration
    // re-references both together — so after calibration this vector is already
    // in the device's anatomical axes and only the constant map is needed.
    return frameMap(candidate).rotatedVector(gyroLowerArmDps).y();
}

} // namespace pinpoint::hm_frame
