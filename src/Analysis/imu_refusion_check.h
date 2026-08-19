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

#include <algorithm>
#include <cstring>
#include <variant>
#include <vector>

#include "swing_window.h"
#include "imu_sample.h"
#include "format_descriptor.h"

#include "../IMU/orientation_filter.h"     // MadgwickFilter (header-only)
#include "../IMU/orientation_refuser.h"    // refuseOrientation / parity

// IMU data-integrity check via offline orientation RE-FUSION.
//
// Re-runs the Madgwick filter from each IMU source's recorded raw accel+gyro,
// warm-started from the stored quaternion at the window's first sample, and
// compares the re-fused trajectory to the stored live quaternion. Because
// Madgwick is deterministic and memoryless (state == the quaternion), warm-start
// re-fusion of a faithfully-recorded swing reproduces the stored quaternion to
// float precision. A LARGE disagreement therefore means the recorded raw vectors
// do NOT correspond to the stored quaternion — the IMU record is internally
// inconsistent, so the swing cannot be re-fused/re-analysed offline (and the
// stored orientation itself is suspect). The app surfaces this as a per-shot
// data-integrity warning on the carousel; SwingLab's swinglab_run exposes the
// same check as --refuse-orientation. See
// docs/validation/pipeline_validation_and_tuning.md §5.2/§5.3.1.
//
// Madgwick only: it is the production default and the only filter that warm-starts
// EXACTLY (ESKF's vendored reference quaternion is not settable). A swing captured
// under ESKF cannot be re-fused exactly, so the caller should only run this when
// the live orientation filter is Madgwick (else a false warning would fire).
//
// ⚠ AND HOST-FUSED LANES ONLY. The whole check rests on the stored quaternion being
// a fusion OF the stored vectors. A device that streams its own calibrated
// orientation — a HackMotion wG3 — breaks that premise outright, so its lanes are
// skipped per-source inside the loop below rather than checked and failed. See the
// note there; the distinction between "not checkable" and "checked and wrong" is the
// entire value of this file.
//
// Header-only and free of Qt so it is reusable by the GUI (ShotProcessor) and the
// offline tools alike.
namespace pinpoint {

struct ImuRefusionVerdict {
    bool   ok             = true;   // false only when a checkable source exceeded the threshold
    int    sourcesChecked = 0;      // IMU sources we could warm-start and compare
    double worstMaxDeg    = 0.0;    // worst per-sample geodesic disagreement (deg)
    double thresholdDeg   = 0.5;    // fail boundary

    // A data warning is raised only when we actually checked a source and it failed.
    bool warns() const { return sourcesChecked > 0 && !ok; }
};

// Re-fuse every IMU source in the frozen window and judge parity vs the stored
// quaternion. `beta < 0` uses the Madgwick production default.
inline ImuRefusionVerdict checkImuRefusion(const SwingWindow &window,
                                           double thresholdDeg = 0.5,
                                           float  beta = -1.0f)
{
    ImuRefusionVerdict v;
    v.thresholdDeg = thresholdDeg;
    const float useBeta = beta > 0.0f ? beta : MadgwickFilter().beta();

    // Enumerate IMU sources straight from the window (binding-independent: works
    // even when analysis.bindings is empty — the samples are still present).
    std::vector<SourceId> imuIds;
    for (const IndexEntry &e : window.entries())
        if (std::holds_alternative<ImuFormat>(window.formatOf(e.source_id).format)
            && std::find(imuIds.begin(), imuIds.end(), e.source_id) == imuIds.end())
            imuIds.push_back(e.source_id);

    for (const SourceId sid : imuIds) {
        // ⚠ A HACKMOTION LANE IS NOT CHECKABLE, AND FAILS SPECTACULARLY IF ASKED.
        // Same reasoning as the ESKF exclusion above, for a different reason: this
        // check assumes the stored quaternion IS a host-side Madgwick fusion of the
        // stored accel+gyro, so re-running that fusion must reproduce it. A wG3
        // streams its own calibrated ANATOMICAL orientation, and its accel column is
        // gravity-removed linear acceleration (≈0 at rest) rather than the gravity
        // vector Madgwick needs as a reference. Re-fusing those inputs produces
        // something unrelated: 179.8-180.0° disagreement, saturated rather than
        // marginal, on every HackMotion shot ever captured — a warning badge on the
        // shot card (PpShotCard.qml) saying the recording is inconsistent, when it is
        // perfectly consistent and merely not a host-side fusion.
        //
        // ⚠ PER-SOURCE, NOT WHOLE-CHECK, WHICH IS WHERE THIS DIFFERS FROM ESKF. That
        // one is a property of the session's filter setting and the caller reads it
        // once; this one is a property of an individual lane, so a window holding a
        // checkable lane beside an uncheckable one still gets checked on the half
        // that can be. Skipping here rather than at the caller also keeps
        // sourcesChecked honest: a window with only HackMotion lanes reports zero
        // checked, warns() is false, and no imuIntegrity block is written at all —
        // which is the truthful outcome, since nothing was in fact checked.
        if (const auto *imf = std::get_if<ImuFormat>(&window.formatOf(sid).format))
            if (imf->device == DeviceKind::IMU_HackMotion)
                continue;

        const std::vector<IndexEntry> entries = window.entriesFor(sid);
        if (entries.size() < 2)
            continue;

        // Nominal cadence -> dt = 1/rate, matching ImuBase::fuseRawImu.
        float rate = 200.0f;
        if (const auto *imf = std::get_if<ImuFormat>(&window.formatOf(sid).format))
            if (imf->sample_rate_hz > 0)
                rate = float(imf->sample_rate_hz);

        std::vector<RefuseSample> samples;
        samples.reserve(entries.size());
        for (const IndexEntry &e : entries) {
            const SourceRing::ReadHandle h = window.payloadOf(e);
            if (!h.data || h.bytes < sizeof(ImuSample))
                continue;
            ImuSample s;
            std::memcpy(&s, h.data, sizeof(ImuSample));   // alignment-safe
            samples.push_back(RefuseSample{ e.timestamp_us,
                                            s.accel_x, s.accel_y, s.accel_z,
                                            s.gyro_x,  s.gyro_y,  s.gyro_z,
                                            s.quat_w,  s.quat_x,  s.quat_y, s.quat_z });
        }
        if (samples.size() < 2)
            continue;

        RefuseConfig cfg;
        cfg.outputRateHz = rate;
        cfg.warmStart    = true;
        MadgwickFilter filt(useBeta);
        const RefuseResult r = refuseOrientation(filt, samples, cfg);
        if (!r.warmStarted)
            continue;   // not checkable (shouldn't happen for Madgwick)

        const ParityStats p = parity(samples, r);
        ++v.sourcesChecked;
        if (p.maxDeg > v.worstMaxDeg)
            v.worstMaxDeg = p.maxDeg;
        if (p.maxDeg >= thresholdDeg)
            v.ok = false;
    }
    return v;
}

} // namespace pinpoint
