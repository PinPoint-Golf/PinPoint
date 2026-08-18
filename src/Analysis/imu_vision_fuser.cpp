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

#include "imu_vision_fuser.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "imu_calibration.h"   // toAnatomical (shared A*q*M)
#include "imu_sample.h"
#include "swing_window.h"
#include "format_descriptor.h"            // ImuFormat (nominal rate)
#include "../IMU/orientation_filter.h"    // MadgwickFilter (re-fusion)
#include "../IMU/orientation_refuser.h"   // refuseOrientationAdaptive

namespace pinpoint::analysis {

namespace {

// Re-derive a source's orientation offline from its raw accel+gyro under `cfg` (warm-started from the
// stored quat at the first sample), returning (t_us, q_refused) pairs aligned to the source's samples.
// The nominal cadence is taken from the IMU format so dt matches the live ImuBase::fuseRawImu exactly.
std::vector<std::pair<int64_t, QQuaternion>>
refuseSource(const SwingWindow &window, SourceId src, const pinpoint::RefuseConfig &cfg)
{
    std::vector<pinpoint::RefuseSample> samples;
    const std::vector<IndexEntry> entries = window.entriesFor(src);
    samples.reserve(entries.size());
    for (const IndexEntry &e : entries) {
        const SourceRing::ReadHandle h = window.payloadOf(e);
        if (!h.data || h.bytes < sizeof(ImuSample))
            continue;
        ImuSample s{};
        std::memcpy(&s, h.data, sizeof(ImuSample));
        samples.push_back(pinpoint::RefuseSample{ e.timestamp_us,
                                                  s.accel_x, s.accel_y, s.accel_z,
                                                  s.gyro_x,  s.gyro_y,  s.gyro_z,
                                                  s.quat_w,  s.quat_x,  s.quat_y, s.quat_z });
    }
    std::vector<std::pair<int64_t, QQuaternion>> seq;
    if (samples.size() < 2)
        return seq;

    pinpoint::RefuseConfig c = cfg;
    if (const auto *imf = std::get_if<ImuFormat>(&window.formatOf(src).format))
        if (imf->sample_rate_hz > 0)
            c.outputRateHz = float(imf->sample_rate_hz);

    MadgwickFilter filt(c.betaStatic);
    const pinpoint::RefuseResult r = pinpoint::refuseOrientationAdaptive(filt, samples, c);
    seq.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
        seq.emplace_back(samples[i].t_us,
                         QQuaternion(r.quat[i][0], r.quat[i][1], r.quat[i][2], r.quat[i][3]));
    return seq;
}

// Slerp a re-fused (t_us → quat) sequence at grid time t (clamped at the ends).
QQuaternion slerpAt(const std::vector<std::pair<int64_t, QQuaternion>> &seq, int64_t t)
{
    if (seq.empty())            return QQuaternion();
    if (t <= seq.front().first) return seq.front().second;
    if (t >= seq.back().first)  return seq.back().second;
    std::size_t lo = 0, hi = seq.size() - 1;
    while (hi - lo > 1) {
        const std::size_t mid = (lo + hi) / 2;
        if (seq[mid].first <= t) lo = mid; else hi = mid;
    }
    const float u = (seq[hi].first > seq[lo].first)
                        ? float(double(t - seq[lo].first) / double(seq[hi].first - seq[lo].first))
                        : 0.0f;
    return QQuaternion::slerp(seq[lo].second, seq[hi].second, u).normalized();
}

} // namespace

double ImuVisionFuser::effectiveHzFor(const SwingWindow &window, SourceId source,
                                      int64_t startUs, int64_t endUs)
{
    if (endUs <= startUs)
        return 0.0;

    // ⚠ MEASURED OVER THE SPAN ASKED FOR, NOT OVER THE WHOLE WINDOW. A stitched
    // lane's rate is not one number, and the figure that decides whether a
    // metric AT IMPACT can be computed is the one measured around impact.
    const std::vector<pinpoint::IndexEntry> es = window.entriesFor(source);
    size_t n = 0;
    for (const pinpoint::IndexEntry &e : es)
        if (e.timestamp_us >= startUs && e.timestamp_us < endUs) ++n;

    if (n < 2)
        return 0.0;   // ⚠ NOT MEASURABLE, and never "a rate of zero"
    return double(n) * 1.0e6 / double(endUs - startUs);
}

double ImuVisionFuser::peakHzFor(const SwingWindow &window, SourceId source,
                                 int64_t probeUs)
{
    const std::vector<pinpoint::IndexEntry> es = window.entriesFor(source);
    if (es.size() < 2 || probeUs <= 0)
        return 0.0;

    // ⚠ A PEAK, NOT AN AVERAGE, AND THAT IS THE WHOLE POINT. Averaging a
    // stitched lane over the window mixes ~800 Hz through the swing with ~100 Hz
    // over a 3 s still pre-roll and reports something near the base rate — which
    // would size the grid to throw away exactly the dense span the pull was
    // performed to obtain. Sliding a short probe finds that span wherever it is,
    // without depending on the impact estimate being right.
    size_t lo = 0;
    double best = 0.0;
    for (size_t hi = 0; hi < es.size(); ++hi) {
        while (es[hi].timestamp_us - es[lo].timestamp_us > probeUs)
            ++lo;
        if (hi == lo) continue;
        const int64_t span = es[hi].timestamp_us - es[lo].timestamp_us;
        if (span <= 0) continue;
        best = std::max(best, double(hi - lo) * 1.0e6 / double(span));
    }
    return best;
}

void ImuVisionFuser::highRateSpanFor(const SwingWindow &window, SourceId source,
                                     double thresholdHz, int64_t out[2])
{
    out[0] = 0;
    out[1] = 0;
    if (thresholdHz <= 0.0)
        return;

    const std::vector<pinpoint::IndexEntry> es = window.entriesFor(source);
    if (es.size() < 2)
        return;

    // The widest spacing that still counts as "above the threshold".
    const int64_t maxStepUs = int64_t(1.0e6 / thresholdHz);

    bool    found = false;
    int64_t first = 0, last = 0;
    for (size_t i = 1; i < es.size(); ++i) {
        const int64_t step = es[i].timestamp_us - es[i - 1].timestamp_us;
        if (step <= 0 || step > maxStepUs)
            continue;
        if (!found) { first = es[i - 1].timestamp_us; found = true; }
        last = es[i].timestamp_us;
    }
    if (!found)
        return;
    out[0] = first;
    out[1] = last;
}

double ImuVisionFuser::gridHzForWindow(const SwingWindow &window,
                                       const std::vector<ImuSegmentBinding> &bindings)
{
    double fastest = 0.0;
    for (const ImuSegmentBinding &b : bindings) {
        if (b.role == SegmentRole::Unknown) continue;
        fastest = std::max(fastest, peakHzFor(window, b.source, kProbeUs));
    }

    // ⚠ Nothing measurable is NOT a reason to change the grid. A window with one
    // sample per lane must produce what it always produced.
    if (fastest <= 0.0)
        return kGridHzMin;
    return std::clamp(fastest, kGridHzMin, kGridHzMax);
}

FusedStreams ImuVisionFuser::fuse(const SwingWindow &window,
                                  const std::vector<ImuSegmentBinding> &bindings,
                                  double gridHz,
                                  const pinpoint::RefuseConfig *refusion)
{
    FusedStreams out;

    // Gather usable bindings (known role + ≥2 samples) and intersect their coverage
    // with the window span so every grid instant is interpolatable for every segment.
    struct Bound {
        const ImuSegmentBinding *b;
        std::vector<std::pair<int64_t, QQuaternion>> refused;   // empty unless filter.refuse is on
    };
    std::vector<Bound> bound;
    int64_t gridStart = window.startTimestampUs();
    int64_t gridEnd   = window.endTimestampUs();
    for (const ImuSegmentBinding &b : bindings) {
        if (b.role == SegmentRole::Unknown)
            continue;
        const std::vector<IndexEntry> entries = window.entriesFor(b.source);
        if (entries.size() < 2)
            continue;
        gridStart = std::max(gridStart, entries.front().timestamp_us);
        gridEnd   = std::min(gridEnd,   entries.back().timestamp_us);
        Bound bb{ &b, {} };
        if (refusion)
            bb.refused = refuseSource(window, b.source, *refusion);
        bound.push_back(std::move(bb));
    }
    if (bound.empty() || gridEnd <= gridStart)
        return out;

    const int64_t dt = static_cast<int64_t>(1.0e6 / gridHz + 0.5);
    if (dt <= 0)
        return out;
    for (int64_t t = gridStart; t <= gridEnd; t += dt)
        out.timeGrid.push_back(t);

    for (const Bound &bd : bound) {
        SegmentStream s;
        s.role = bd.b->role;
        s.qAnat.reserve(out.timeGrid.size());
        s.gyroDps.reserve(out.timeGrid.size());
        s.accelG.reserve(out.timeGrid.size());
        // ImuSample vectors are RAW sensor-frame (imu_sample.h v2); rotate them
        // into the anatomical segment frame (v_anat = M⁻¹·v_sensor) so they share
        // qAnat's body frame. M is unit, so conjugated() is its inverse.
        const QQuaternion mountInv = bd.b->mountM.conjugated();
        QQuaternion last;            // hold-last fallback for a momentary gap
        QVector3D   lastGyro, lastAccel;
        bool haveLast = false;
        for (const int64_t t : out.timeGrid) {
            ImuSample smp{};
            QQuaternion qAnat;
            QVector3D   gyro, accel;
            if (window.interpolateImu(bd.b->source, t,
                                      reinterpret_cast<std::byte *>(&smp), sizeof(smp))) {
                // q_raw: the re-fused orientation when filter.refuse is on, else the stored quat.
                const QQuaternion qRaw = bd.refused.empty()
                    ? QQuaternion(smp.quat_w, smp.quat_x, smp.quat_y, smp.quat_z)
                    : slerpAt(bd.refused, t);
                qAnat     = imu_calibration::toAnatomical(bd.b->alignA, qRaw, bd.b->mountM);
                gyro      = mountInv.rotatedVector(QVector3D(smp.gyro_x, smp.gyro_y, smp.gyro_z));
                accel     = mountInv.rotatedVector(QVector3D(smp.accel_x, smp.accel_y, smp.accel_z));
                last      = qAnat;
                lastGyro  = gyro;
                lastAccel = accel;
                haveLast  = true;
            } else {
                qAnat = haveLast ? last : QQuaternion();   // identity until first valid sample
                gyro  = lastGyro;                          // zero until first valid sample
                accel = lastAccel;
            }
            s.qAnat.push_back(qAnat);
            s.gyroDps.push_back(gyro);
            s.accelG.push_back(accel);
        }
        out.segments.push_back(std::move(s));
    }
    return out;
}

} // namespace pinpoint::analysis
