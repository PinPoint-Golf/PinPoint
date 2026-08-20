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

#include <QString>
#include <optional>

#include "swing_analysis.h"       // ImuSegmentBinding, SegmentRole, SourceId
#include "../IMU/hm_frame.h"      // the frame constant — a wG3 binding needs no calibration
#include "../IMU/hm_unit_id.h"    // "<deviceId>#lowerArm" / "#palm" -> which segment

// ---------------------------------------------------------------------------
// Rebuilding a HackMotion segment binding from a recorded swing
// ---------------------------------------------------------------------------
//
// ⚠ A HACKMOTION BINDING IS RECONSTRUCTED, NOT READ BACK, AND IT IS FULLY
// DETERMINED. The device applies its own calibration and streams the result, so
// there is no per-user A/M solve to persist — the exporter writes no
// alignA/mountM for a wG3 lane (it gates on dev.hasCalibration) and the Witmotion
// read-back branch would find nothing. What decides the binding instead is the
// constant frame plus which unit the lane is, and both are recoverable from the
// recording: A is identity by definition, M is the frame constant, and the unit
// is in the serial.
//
// ⚠ ROLE COMES FROM THE UNIT, NOT FROM sessionType. Which arm segment a wG3's
// lower-arm board measured is a fact about the hardware; asking the capture's
// declared intent would make re-analysis depend on a flag the golfer set, and
// would silently produce nothing for a capture recorded under any other session
// type.
//
// ⚠ AND WITHOUT IT THE ELEVEN E3 FIXTURE SWINGS RE-ANALYSE TO NOTHING, because
// the live binding never ran when they were captured — their device.role is 0 and
// roleName empty. That is the case this function exists for, so a change that
// quietly stops binding them would not fail anything visible: re-analysis would
// still succeed, still write a swing.json, and simply hold no wrist metric.
//
// It lives here rather than inline in SwingDiskLoader::load() so it can be tested
// without the analyzer, OpenCV, or a swing folder on disk: every input is a value
// read out of swing.json, so a test states them directly. See
// hm_binding_recon_test.

namespace pinpoint::analysis::hm_binding {

// The device's own calibration state at capture, recorded by Phase B′ as
// `device.calibrationStateAtStart`. 2 == WR_CAL_CALIBRATED; anything else means
// the lane was streaming board placement rather than anatomy, and a binding would
// publish that as a measurement.
inline constexpr int kCalibratedState = 2;

// Why a lane could not be bound. The caller turns this into one ppWarn line —
// nothing else branches on it, and it exists so "not bound" always says WHICH
// missing fact caused it rather than arriving as an empty vector.
enum class Refusal {
    None,               // bound
    NoFrameCandidate,   // Phase D has not selected one — the lane has no anatomical frame
    SerialNamesNoUnit,  // serial is not "<deviceId>#lowerArm"/"#palm" — segment unknown
};

struct Result {
    std::optional<ImuSegmentBinding> binding;
    Refusal refusal = Refusal::None;
};

// `serial` is the IMU stream's source.serial as exported; `calibrationStateAtStart`
// is device.calibrationStateAtStart (-1 when absent); `candidate` is the selected
// Phase-D frame, defaulting to the tuned constant the live path uses.
inline Result reconstruct(SourceId source,
                          const QString &serial,
                          int calibrationStateAtStart,
                          int candidate = pinpoint::tuned::hmframe::kCandidate)
{
    Result r;

    // No frame candidate ⇒ NO BINDING AT ALL, rather than a binding whose
    // toAnatomical() returns identity. An identity binding would produce a
    // confident, plausible, meaningless wrist curve; an absent one produces
    // nothing and says so.
    if (!hm_frame::isSelected(candidate)) {
        r.refusal = Refusal::NoFrameCandidate;
        return r;
    }

    QString unitDevId;
    int     unitIdx = pinpoint::hm_unit_id::kLowerArm;
    if (!pinpoint::hm_unit_id::parse(serial, &unitDevId, &unitIdx)) {
        r.refusal = Refusal::SerialNamesNoUnit;
        return r;
    }

    ImuSegmentBinding b;
    b.source     = source;
    b.role       = unitIdx == pinpoint::hm_unit_id::kPalm ? SegmentRole::LeadHand
                                                          : SegmentRole::LeadForearm;
    b.alignA     = QQuaternion();                    // identity, by design
    b.mountM     = hm_frame::mountM(candidate);      // the frame constant
    // ⚠ Getting this wrong is silent: the conjugate is skipped at the composition
    // site and every wrist sign inverts while the curve still tracks the wrist
    // convincingly. See ImuSegmentBinding::hackMotion.
    b.hackMotion = true;

    const bool devCal = calibrationStateAtStart == kCalibratedState;
    b.anatCalibrated  = devCal;
    b.calibrated      = devCal;

    r.binding = b;
    return r;
}

} // namespace pinpoint::analysis::hm_binding
