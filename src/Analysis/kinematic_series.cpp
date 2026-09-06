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

#include "kinematic_series.h"

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pinpoint::analysis {

namespace {

constexpr double kPi      = 3.14159265358979323846;
constexpr double kMps2Mph = 2.2369362920544;   // 1 m/s in mph

// COCO body keypoint indices (PoseJoint) for the lead forearm.
constexpr int kLeftElbow = 7, kRightElbow = 8, kLeftWrist = 9, kRightWrist = 10;

// A face-on golf frame spans roughly this vertically — the last-ditch px→metre scale
// when no measured club length is available (keeps mph plausible, never claimed exact).
constexpr double kNominalFrameHeightM = 2.5;
constexpr double kNominalMPerPx       = 1.0 / 900.0;

// Wrap an angle into (−π, π].
double wrapPi(double a)
{
    a = std::fmod(a + kPi, 2.0 * kPi);
    if (a < 0) a += 2.0 * kPi;
    return a - kPi;
}

// Symmetric moving average, window = 2*half+1, edge-clamped.
std::vector<double> movAvg(const std::vector<double> &v, int half)
{
    const int n = int(v.size());
    if (n == 0 || half <= 0) return v;
    std::vector<double> out(size_t(n), 0.0);
    for (int i = 0; i < n; ++i) {
        double sum = 0.0;
        int cnt = 0;
        for (int k = -half; k <= half; ++k) {
            const int j = std::clamp(i + k, 0, n - 1);
            sum += v[size_t(j)];
            ++cnt;
        }
        out[size_t(i)] = sum / cnt;
    }
    return out;
}

int nearestIndex(const std::vector<int64_t> &grid, int64_t t)
{
    if (grid.empty()) return -1;
    auto it = std::lower_bound(grid.begin(), grid.end(), t);
    if (it == grid.begin()) return 0;
    if (it == grid.end())   return int(grid.size()) - 1;
    const int hi = int(it - grid.begin());
    const int lo = hi - 1;
    return (t - grid[size_t(lo)] <= grid[size_t(hi)] - t) ? lo : hi;
}

// Set Address/Top/Impact phase dots on a curve from the phase timeline; when the
// timeline is empty, a lone Impact dot at impactUs.
// A masked sample is never the site of a phase dot (MetricSeries::valid): the dot goes to
// the nearest VALID sample, which for a series masked past its P7 knot is the last sample
// of the club arriving — the number the launch-monitor comparison reads.
void addPhaseDots(MetricSeries &m, const std::vector<PhaseEvent> &phases, int64_t impactUs)
{
    if (m.t_us.empty()) return;
    const auto dot = [&](Phase p, int64_t t) {
        int i = nearestIndex(m.t_us, t);
        if (i < 0) return;
        if (!m.valid.empty() && m.valid[size_t(i)] == 0u) {
            int best = -1; int64_t bd = std::numeric_limits<int64_t>::max();
            for (int j = 0; j < int(m.t_us.size()); ++j) {
                if (m.valid[size_t(j)] == 0u) continue;
                const int64_t d = std::llabs(m.t_us[size_t(j)] - t);
                if (d < bd) { bd = d; best = j; }
            }
            if (best < 0) return;
            i = best;
        }
        m.phaseSamples.push_back({ p, m.t_us[size_t(i)], m.value[size_t(i)], QString() });
    };
    bool any = false;
    for (const Phase p : { Phase::Address, Phase::Top, Phase::Impact }) {
        for (const PhaseEvent &e : phases)
            if (e.phase == p) { dot(p, e.t_us); any = true; break; }
    }
    if (!any && impactUs >= 0) dot(Phase::Impact, impactUs);
}

// px → metre scale from the club-length fusion, falling back to the median visible
// shaft length, then a nominal frame scale. clubLenPx is a grip→head length in px, so
// clubLengthM / clubLenPx maps px → metres.
double resolveMetrePerPx(const ShaftTrack2D &shaft, const std::vector<ShaftSample2D> &track,
                         double clubLengthM)
{
    double lenPx = -1.0;
    if (shaft.lengths.fusedPx > 0.0)        lenPx = shaft.lengths.fusedPx;
    else if (shaft.measuredClubLenPx > 0.0) lenPx = shaft.measuredClubLenPx;
    else {
        std::vector<double> lens;
        lens.reserve(track.size());
        for (const ShaftSample2D &s : track)
            if (s.visibleLenPx > 0.0) lens.push_back(s.visibleLenPx);
        if (!lens.empty()) {
            std::nth_element(lens.begin(), lens.begin() + lens.size() / 2, lens.end());
            lenPx = lens[lens.size() / 2];
        }
    }
    if (lenPx > 0.0 && clubLengthM > 0.0) return clubLengthM / lenPx;
    if (shaft.frameHeight > 0)            return kNominalFrameHeightM / shaft.frameHeight;
    return kNominalMPerPx;
}

// Linear speed (mph) along a smoothed px track sampled on `t` (ascending µs).
std::vector<double> speedMph(const std::vector<int64_t> &t, const std::vector<double> &px,
                             const std::vector<double> &py, double mPerPx)
{
    const int n = int(t.size());
    std::vector<double> sp(size_t(std::max(n, 0)), 0.0);
    if (n < 2) return sp;
    // Smooth positions before differentiating (differentiation amplifies jitter).
    const std::vector<double> sx = movAvg(px, 2), sy = movAvg(py, 2);
    for (int i = 0; i < n; ++i) {
        const int a = std::max(i - 1, 0), b = std::min(i + 1, n - 1);
        const double dt = double(t[size_t(b)] - t[size_t(a)]) / 1e6;   // s
        if (dt <= 0.0) { sp[size_t(i)] = (i > 0) ? sp[size_t(i - 1)] : 0.0; continue; }
        const double dx = sx[size_t(b)] - sx[size_t(a)];
        const double dy = sy[size_t(b)] - sy[size_t(a)];
        sp[size_t(i)] = (std::sqrt(dx * dx + dy * dy) / dt) * mPerPx * kMps2Mph;
    }
    return movAvg(sp, 1);   // light 3-tap smooth of the speed itself
}

// Composed head speed (mph) on a track: |v_grip + L·θ̇·n̂|, n̂ = (−sin θ, cos θ) the
// image-plane direction of rotation, L the fused club length in px (falls back to the
// measured club length, then the sample's own visible extent). v_grip is the ±1-sample
// central difference of the grip path — the grip is smooth through impact, the head is
// not, and nothing here smooths across the step.
std::vector<double> composedHeadSpeedMph(const std::vector<ShaftSample2D> &track,
                                         const ShaftTrack2D &shaft, double mPerPx)
{
    const int n = int(track.size());
    std::vector<double> sp(size_t(std::max(n, 0)), 0.0);
    if (n < 2) return sp;
    double lenFixed = -1.0;
    if (shaft.lengths.fusedPx > 0.0)        lenFixed = shaft.lengths.fusedPx;
    else if (shaft.measuredClubLenPx > 0.0) lenFixed = shaft.measuredClubLenPx;
    for (int i = 0; i < n; ++i) {
        const int a = std::max(i - 1, 0), b = std::min(i + 1, n - 1);
        const double dt = double(track[size_t(b)].t_us - track[size_t(a)].t_us) / 1e6;
        double gvx = 0.0, gvy = 0.0;
        if (dt > 0.0) {
            gvx = (track[size_t(b)].gripPx.x() - track[size_t(a)].gripPx.x()) / dt;
            gvy = (track[size_t(b)].gripPx.y() - track[size_t(a)].gripPx.y()) / dt;
        }
        const ShaftSample2D &s = track[size_t(i)];
        const double L  = lenFixed > 0.0 ? lenFixed : std::max(s.visibleLenPx, 0.0);
        const double vx = gvx - L * s.thetaDotRadS * std::sin(s.thetaRad);
        const double vy = gvy + L * s.thetaDotRadS * std::cos(s.thetaRad);
        sp[size_t(i)] = std::hypot(vx, vy) * mPerPx * kMps2Mph;
    }
    return sp;
}

// Domain mask for the composed clubhead speed: every sample AFTER `boundaryUs` is marked
// invalid (MetricSeries::valid — drawn dashed, skipped by every reducer, never a phase dot).
// The boundary is the P7 knot of the track, where the synthesized rate steps, NOT the
// acoustic impact: the two differ by up to ~3 ms, and a boundary sample on the departing
// side would put the step back inside every window about Impact. Same mechanism as
// metric_channel.h's applyPhaseDomainMask (design §5.1), snapped to the knot instead of the
// nearest grid sample for that reason. No boundary ⇒ the series is left exactly as it was.
void maskAfter(MetricSeries &m, int64_t boundaryUs)
{
    if (m.t_us.empty() || boundaryUs < 0) return;
    bool any = false;
    for (int64_t t : m.t_us) if (t > boundaryUs) { any = true; break; }
    if (!any) return;
    if (m.valid.empty()) m.valid.assign(m.t_us.size(), 1u);
    for (size_t i = 0; i < m.t_us.size(); ++i)
        if (m.t_us[i] > boundaryUs) m.valid[i] = 0u;
}

MetricSeries makeSpeedSeries(const QString &key, const QString &label,
                             const std::vector<int64_t> &t, std::vector<double> v,
                             const std::vector<PhaseEvent> &phases, int64_t impactUs,
                             int64_t maskAfterUs = -1)
{
    MetricSeries m;
    m.key   = key;
    m.label = label;
    m.unit  = QStringLiteral("mph");
    m.t_us  = t;
    m.value = std::move(v);
    maskAfter(m, maskAfterUs);
    addPhaseDots(m, phases, impactUs);
    return m;
}

// ── lag: lead forearm (elbow→wrist) vs shaft (grip→head) ─────────────────────
// Interpolated lead-forearm direction (px) at time `t`; false if no confident cover.
bool forearmDirAt(const PoseTrack2D &pose, int64_t t, int elbowKp, int wristKp,
                  int frameW, int frameH, double &dx, double &dy)
{
    const auto &F = pose.frames;
    if (F.size() < 2) return false;
    const double W = frameW > 0 ? double(frameW) : 1.0;
    const double H = frameH > 0 ? double(frameH) : 1.0;
    size_t hi = F.size();
    for (size_t i = 0; i < F.size(); ++i) if (F[i].t_us >= t) { hi = i; break; }
    const size_t iB = std::min(hi, F.size() - 1);
    const size_t iA = iB > 0 ? iB - 1 : 0;
    const PoseFrame2D &A = F[iA], &B = F[iB];
    const float cA = std::min(A.conf[size_t(elbowKp)], A.conf[size_t(wristKp)]);
    const float cB = std::min(B.conf[size_t(elbowKp)], B.conf[size_t(wristKp)]);
    const auto vecOf = [&](const PoseFrame2D &f) {
        return QPointF((f.kp[size_t(wristKp)].x() - f.kp[size_t(elbowKp)].x()) * W,
                       (f.kp[size_t(wristKp)].y() - f.kp[size_t(elbowKp)].y()) * H);
    };
    constexpr float kConf = 0.15f;
    QPointF v;
    if (cA >= kConf && cB >= kConf && B.t_us > A.t_us) {
        const double f = std::clamp(double(t - A.t_us) / double(B.t_us - A.t_us), 0.0, 1.0);
        v = vecOf(A) * (1.0 - f) + vecOf(B) * f;
    } else if (cB >= kConf) {
        v = vecOf(B);
    } else if (cA >= kConf) {
        v = vecOf(A);
    } else {
        return false;
    }
    dx = v.x();
    dy = v.y();
    return std::hypot(dx, dy) > 1e-6;
}

// Nearest shaft direction angle (grip→head, rad) at time t on the chosen track.
bool shaftAngleAt(const std::vector<ShaftSample2D> &track, int64_t t, double &theta)
{
    if (track.empty()) return false;
    int64_t best = std::numeric_limits<int64_t>::max();
    for (const ShaftSample2D &s : track) {
        const int64_t d = std::llabs(s.t_us - t);
        if (d < best) { best = d; theta = s.thetaRad; }
    }
    return true;
}

MetricSeries buildLagSeries(const std::vector<ShaftSample2D> &track, const ShaftTrack2D &shaft,
                            const PoseTrack2D &pose, int handedness,
                            const std::vector<PhaseEvent> &phases, int64_t impactUs)
{
    MetricSeries m;
    const bool leadLeft = (handedness != 2);
    const int elbowKp = leadLeft ? kLeftElbow : kRightElbow;
    const int wristKp = leadLeft ? kLeftWrist : kRightWrist;
    for (const PoseFrame2D &f : pose.frames) {
        double dx, dy, theta;
        if (!forearmDirAt(pose, f.t_us, elbowKp, wristKp, shaft.frameWidth, shaft.frameHeight, dx, dy))
            continue;
        if (!shaftAngleAt(track, f.t_us, theta))
            continue;
        const double forearm = std::atan2(dy, dx);
        const double lagDeg  = std::fabs(wrapPi(theta - forearm)) * 180.0 / kPi;   // [0,180]
        m.t_us.push_back(f.t_us);
        m.value.push_back(lagDeg);
    }
    if (m.value.size() < 2) { m.t_us.clear(); m.value.clear(); return m; }
    m.value = movAvg(m.value, 1);
    m.key   = QStringLiteral("lagAngle");
    m.label = QStringLiteral("Lag");
    m.unit  = QStringLiteral("°");
    addPhaseDots(m, phases, impactUs);
    return m;
}

} // namespace

std::vector<MetricSeries> buildKinematicSeries(const KinematicSeriesInputs &in)
{
    std::vector<MetricSeries> out;

    // Speeds + lag need the shaft track. Prefer the dense C¹ synth channel (the display
    // track, smoother for differentiation); fall back to the measured samples.
    if (!in.shaft || !in.shaft->valid)
        return out;
    const ShaftTrack2D &shaft = *in.shaft;
    const std::vector<ShaftSample2D> &track =
        (shaft.synth.size() >= 2) ? shaft.synth : shaft.samples;
    if (track.size() < 2)
        return out;

    // Composed: the fused px span runs from the hands to the head, so it maps to the club
    // length LESS the grip-down (the tracker's own drawing convention), never the full club.
    const double lengthM = in.composed ? std::max(in.clubLengthM - in.gripDownM, 0.0) : in.clubLengthM;
    const double mPerPx  = resolveMetrePerPx(shaft, track, lengthM);
    std::vector<int64_t> t;
    std::vector<double> hx, hy, gx, gy;
    t.reserve(track.size());
    for (const ShaftSample2D &e : track) {
        t.push_back(e.t_us);
        hx.push_back(e.headPx.x());  hy.push_back(e.headPx.y());
        gx.push_back(e.gripPx.x());  gy.push_back(e.gripPx.y());
    }
    // Composed: the tail past the P7 knot (or, without a located P7, past the impact
    // instant) is out of the metric's Address→Impact domain — see maskAfter.
    int64_t boundaryUs = -1;
    if (in.composed) {
        boundaryUs = in.impactUs;
        for (const ShaftPosition &p : shaft.positions)
            if (p.p == 7) { boundaryUs = p.t_us; break; }
    }
    out.push_back(makeSpeedSeries(QStringLiteral("clubheadSpeed"), QStringLiteral("Clubhead speed"),
                                  t, in.composed ? composedHeadSpeedMph(track, shaft, mPerPx)
                                                 : speedMph(t, hx, hy, mPerPx),
                                  in.phases, in.impactUs, boundaryUs));
    out.push_back(makeSpeedSeries(QStringLiteral("handSpeed"), QStringLiteral("Hand speed"),
                                  t, speedMph(t, gx, gy, mPerPx), in.phases, in.impactUs));

    // Lag additionally needs the pose track; omit it (not fabricate) when absent.
    if (in.pose && in.pose->frames.size() >= 2) {
        MetricSeries lag = buildLagSeries(track, shaft, *in.pose, in.handedness,
                                          in.phases, in.impactUs);
        if (!lag.t_us.empty())
            out.push_back(std::move(lag));
    }

    return out;
}

} // namespace pinpoint::analysis
