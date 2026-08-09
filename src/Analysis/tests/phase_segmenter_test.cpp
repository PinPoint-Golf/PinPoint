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

// PhaseSegmenter v2 (pass-1 inertial ladder) against a parameterized synthetic
// swing whose gyro is finite-differenced from the SAME quaternion track the
// segmenter sees — the two can never disagree (design A.8). Adversarial cases:
// waggle + regrip before takeaway, truncated finish, missing pelvis,
// hand-only / forearm-only, continuous pre-shot motion, 100 vs 200 Hz grids.

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include <QQuaternion>
#include <QVector3D>

#include "../phase_segmenter.h"

using namespace pinpoint::analysis;

static int g_fail = 0;
static constexpr double kPi = 3.14159265358979323846;

#define CHECK_NEAR(label, got, want, tol)                                              \
    do { double g = (got), w = (want); bool ok = std::abs(g - w) <= (tol);             \
         std::printf("  [%s] %-44s got %10.4f  want %10.4f (tol %g)\n",                \
                     ok ? "PASS" : "FAIL", label, g, w, double(tol));                  \
         if (!ok) ++g_fail; } while (0)

#define CHECK_TRUE(label, cond)                                                        \
    do { bool ok = (cond);                                                             \
         std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);                      \
         if (!ok) ++g_fail; } while (0)

namespace {

// ── Synthetic swing profile ──────────────────────────────────────────────────
// One scalar swing angle φ(t) (deg) about the plane normal; segments differ by
// amplitude scale and base orientation. Knots (seconds):
struct SwingTimes {
    double waggleStart = 0.5, waggleEnd = 0.9;   // pre-shot waggle burst
    double takeaway    = 1.9;                    // motion onset (= Address end)
    double top         = 3.18;                   // backswing reversal
    double impact      = 3.5;
    double followEnd   = 4.0;                    // rotation decayed to rest
    double clipEnd     = 5.0;                    // window end
    double regripStart = 0.0, regripEnd = 0.0;   // optional second pre-shot burst
    double waggleAmp   = 8.0;                    // deg
    bool   continuousMotion = false;             // no stillness anywhere pre-swing
};

double smoothstep(double u) { return u <= 0 ? 0.0 : u >= 1 ? 1.0 : u * u * (3.0 - 2.0 * u); }

double phiAt(double t, const SwingTimes &st)
{
    double phi = 0.0;
    if (t < st.takeaway) {
        auto burst = [&](double a, double b, double amp) {
            if (t < a || t > b) return 0.0;
            const double u = (t - a) / (b - a);
            return amp * std::sin(2.0 * kPi * 2.5 * (t - a)) * std::sin(kPi * u) * std::sin(kPi * u);
        };
        phi += burst(st.waggleStart, st.waggleEnd, st.waggleAmp);
        if (st.regripEnd > st.regripStart)
            phi += burst(st.regripStart, st.regripEnd, st.waggleAmp);
        if (st.continuousMotion)
            phi += 2.5 * std::sin(2.0 * kPi * 2.0 * t);   // ~30 °/s everywhere
        return phi;
    }
    if (t < st.top)        // backswing: 0 -> −120°, C1 at both ends
        return -120.0 * smoothstep((t - st.takeaway) / (st.top - st.takeaway));
    if (t < st.impact) {   // downswing: accelerates INTO impact (peak speed at the ball)
        const double u = (t - st.top) / (st.impact - st.top);
        return -120.0 + 140.0 * u * u;
    }
    if (t < st.followEnd)  // follow-through: +20° -> +90°, decaying to rest
        return 20.0 + 70.0 * smoothstep((t - st.impact) / (st.followEnd - st.impact));
    return 90.0;
}

// Build a segment: qAnat = R_planeNormal(scale·φ(t)) · base, with body gyro
// finite-differenced from that exact track and accel = gravity in body frame.
SegmentStream makeSegment(SegmentRole role, const std::vector<int64_t> &grid,
                          const QVector3D &planeNormal, const QQuaternion &base,
                          double scale, const SwingTimes &st,
                          const std::function<double(double)> &extraAngle = {})
{
    SegmentStream s;
    s.role = role;
    const size_t n = grid.size();
    s.qAnat.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(grid[i]) * 1e-6;
        double ang = scale * phiAt(t, st);
        if (extraAngle)
            ang += extraAngle(t);
        s.qAnat.push_back(
            QQuaternion::fromAxisAndAngle(planeNormal, float(ang)) * base);
    }
    s.gyroDps.assign(n, QVector3D());
    s.accelG.assign(n, QVector3D());
    for (size_t i = 0; i < n; ++i) {
        s.accelG[i] = s.qAnat[i].conjugated().rotatedVector(QVector3D(0, 0, 1));
        if (i + 1 >= n) { if (i) s.gyroDps[i] = s.gyroDps[i - 1]; continue; }
        const QQuaternion dq = (s.qAnat[i].conjugated() * s.qAnat[i + 1]).normalized();
        const QVector3D v(dq.x(), dq.y(), dq.z());
        const double ang = 2.0 * std::atan2(double(v.length()), double(std::abs(dq.scalar())));
        const double dt  = double(grid[i + 1] - grid[i]) * 1e-6;
        if (v.length() > 1e-9 && dt > 0)
            s.gyroDps[i] = v.normalized()
                         * float(ang / dt * 180.0 / kPi * (dq.scalar() < 0 ? -1.0 : 1.0));
    }
    return s;
}

// Pelvis: axial rotation about its own +Y long axis — reverses BEFORE Top.
SegmentStream makePelvis(const std::vector<int64_t> &grid, const SwingTimes &st,
                         double reversalS)
{
    SegmentStream s;
    s.role = SegmentRole::Pelvis;
    const size_t n = grid.size();
    for (size_t i = 0; i < n; ++i) {
        const double t = double(grid[i]) * 1e-6;
        double chi = 0.0;
        if (t >= st.takeaway && t < reversalS)
            chi = -45.0 * smoothstep((t - st.takeaway) / (reversalS - st.takeaway));
        else if (t >= reversalS && t < st.impact)
            chi = -45.0 + 85.0 * smoothstep((t - reversalS) / (st.impact - reversalS));
        else if (t >= st.impact)
            chi = 40.0;
        s.qAnat.push_back(QQuaternion::fromAxisAndAngle(QVector3D(0, 1, 0), float(chi)));
    }
    s.gyroDps.assign(n, QVector3D());
    s.accelG.assign(n, QVector3D());
    for (size_t i = 0; i + 1 < n; ++i) {
        const QQuaternion dq = (s.qAnat[i].conjugated() * s.qAnat[i + 1]).normalized();
        const QVector3D v(dq.x(), dq.y(), dq.z());
        const double ang = 2.0 * std::atan2(double(v.length()), double(std::abs(dq.scalar())));
        const double dt  = double(grid[i + 1] - grid[i]) * 1e-6;
        if (v.length() > 1e-9 && dt > 0)
            s.gyroDps[i] = v.normalized()
                         * float(ang / dt * 180.0 / kPi * (dq.scalar() < 0 ? -1.0 : 1.0));
        s.accelG[i] = s.qAnat[i].conjugated().rotatedVector(QVector3D(0, 0, 1));
    }
    if (n) {
        s.accelG[n - 1] = s.qAnat[n - 1].conjugated().rotatedVector(QVector3D(0, 0, 1));
        if (n > 1) s.gyroDps[n - 1] = s.gyroDps[n - 2];
    }
    return s;
}

std::vector<int64_t> makeGrid(double endS, int64_t dtUs)
{
    std::vector<int64_t> g;
    for (int64_t t = 0; t <= int64_t(endS * 1e6); t += dtUs)
        g.push_back(t);
    return g;
}

// The standard rig: a face-on vertical swing plane (normal = world X — the
// pendulum view), both arm segments starting from the hanging base Rx(−80°).
// With q = Rx(scale·φ − 80°), the +Y inclination is exactly asin(sin(ψ)),
// ψ = scale·φ − 80° — so every "parallel" crossing has a closed-form truth:
// ψ ≡ 0 (mod 180°), i.e. scale·φ = −100° (backswing/downswing) or +80°
// (follow-through).
FusedStreams makeStreams(const SwingTimes &st, int64_t dtUs, bool withHand = true,
                         bool withFore = true, bool withPelvis = true)
{
    FusedStreams fs;
    fs.timeGrid = makeGrid(st.clipEnd, dtUs);
    const QVector3D n = QVector3D(1, 0, 0);
    const QQuaternion hang = QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), -80.f);
    if (withHand)
        fs.segments.push_back(makeSegment(SegmentRole::LeadHand, fs.timeGrid, n, hang, 1.0, st));
    if (withFore)
        fs.segments.push_back(makeSegment(SegmentRole::LeadForearm, fs.timeGrid, n, hang, 0.95, st));
    if (withPelvis)
        fs.segments.push_back(makePelvis(fs.timeGrid, st, 3.10));
    return fs;
}

// Invert the φ profile numerically for ground-truth crossing times.
double timeWherePhi(double target, double fromS, double toS, const SwingTimes &st)
{
    double lo = fromS, hi = toS;
    const bool rising = phiAt(toS, st) > phiAt(fromS, st);
    for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        if ((phiAt(mid, st) < target) == rising) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

double evT(const Segmentation &s, Phase p)
{
    const PhaseEvent *e = s.eventFor(p);
    return e ? double(e->t_us) * 1e-6 : -1.0;
}
float evConf(const Segmentation &s, Phase p)
{
    const PhaseEvent *e = s.eventFor(p);
    return e ? e->conf : -1.f;
}

} // namespace

int main()
{
    const SwingTimes base;
    const int64_t impactUs = int64_t(base.impact * 1e6);

    // ===== 1. Full rig, 200 Hz: the whole ladder =====
    std::printf("== full ladder (hand+forearm+pelvis, 200 Hz, waggle) ==\n");
    {
        const FusedStreams fs = makeStreams(base, 5000);
        const Segmentation seg = PhaseSegmenter::segment(fs, impactUs);

        CHECK_NEAR("Impact (anchor)", evT(seg, Phase::Impact), base.impact, 0.001);
        CHECK_NEAR("Top", evT(seg, Phase::Top), base.top, 0.015);
        CHECK_NEAR("Takeaway", evT(seg, Phase::Takeaway), base.takeaway, 0.06);
        CHECK_NEAR("Address (waggle-proof)", evT(seg, Phase::Address), base.takeaway, 0.06);
        CHECK_TRUE("Address conf high (not the waggle fallback)",
                   evConf(seg, Phase::Address) >= 0.8f);
        CHECK_NEAR("Transition (pelvis leads)", evT(seg, Phase::Transition), 3.10, 0.02);
        CHECK_NEAR("Finish (decay-to-still onset)", evT(seg, Phase::Finish),
                   base.followEnd, 0.08);
        // The filtered-envelope peak of a corner maximum (speed cliff at the
        // ball) shifts slightly early — inherent to the A.5 envelope spec.
        CHECK_NEAR("MaxSpeed (peaks into impact)", evT(seg, Phase::MaxSpeed),
                   base.impact, 0.06);

        // Geometric checkpoints against closed-form truths (forearm scale
        // 0.95 ⇒ parallel at φ = −100/0.95; hand scale 1 ⇒ φ = −100, +80).
        CHECK_NEAR("MidBackswing (P3, forearm parallel up)",
                   evT(seg, Phase::MidBackswing),
                   timeWherePhi(-100.0 / 0.95, base.takeaway, base.top, base), 0.025);
        CHECK_NEAR("ArmParallelDown (P5, forearm parallel down)",
                   evT(seg, Phase::ArmParallelDown),
                   timeWherePhi(-100.0 / 0.95, base.top, base.impact, base), 0.025);
        CHECK_NEAR("Delivery proxy (hand parallel down)",
                   evT(seg, Phase::Delivery),
                   timeWherePhi(-100.0, base.top, base.impact, base), 0.025);
        CHECK_TRUE("Delivery proxy conf capped <= 0.4",
                   evConf(seg, Phase::Delivery) <= 0.4f);
        CHECK_NEAR("ShaftParallelThrough (P8, forearm parallel through)",
                   evT(seg, Phase::ShaftParallelThrough),
                   timeWherePhi(80.0 / 0.95, base.impact, base.followEnd, base), 0.025);
        CHECK_TRUE("P8 forearm proxy conf capped <= 0.4",
                   evConf(seg, Phase::ShaftParallelThrough) <= 0.4f);

        // Chain monotone; bounds wrap the swing; overall conf usable.
        for (size_t k = 1; k < seg.events.size(); ++k)
            if (seg.events[k].t_us < seg.events[k - 1].t_us) {
                std::printf("  [FAIL] chain not monotone at %zu\n", k);
                ++g_fail;
            }
        CHECK_NEAR("swingStart = Address - pad",
                   double(seg.swingStartUs) * 1e-6, evT(seg, Phase::Address) - 0.25, 0.01);
        CHECK_NEAR("swingEnd = Finish + pad",
                   double(seg.swingEndUs) * 1e-6, evT(seg, Phase::Finish) + 0.25, 0.01);
        CHECK_TRUE("overall conf usable (> 0.5)", seg.conf > 0.5f);
        CHECK_TRUE("provenance recorded (Top from hand)",
                   seg.eventFor(Phase::Top)->provenance == SegmentRole::LeadHand);
    }

    // ===== 2. Waggle + regrip: Address survives both =====
    std::printf("== waggle + regrip ==\n");
    {
        SwingTimes st = base;
        st.regripStart = 1.30; st.regripEnd = 1.52;   // still 1.52..1.9 = 380 ms >= 300
        const Segmentation seg = PhaseSegmenter::segment(makeStreams(st, 5000), impactUs);
        CHECK_NEAR("Address ignores waggle AND regrip",
                   evT(seg, Phase::Address), st.takeaway, 0.06);
        CHECK_TRUE("Address conf still high", evConf(seg, Phase::Address) >= 0.8f);
    }

    // ===== 3. Continuous pre-shot motion: honest low-conf fallback =====
    std::printf("== continuous pre-shot motion ==\n");
    {
        SwingTimes st = base;
        st.continuousMotion = true;
        const Segmentation seg = PhaseSegmenter::segment(makeStreams(st, 5000), impactUs);
        CHECK_TRUE("Address falls back to Takeaway at low conf",
                   evConf(seg, Phase::Address) <= 0.35f);
        CHECK_NEAR("Top unaffected by pre-shot noise", evT(seg, Phase::Top), base.top, 0.015);
    }

    // ===== 4. Truncated finish: clamp, visibly =====
    std::printf("== truncated finish ==\n");
    {
        SwingTimes st = base;
        st.clipEnd = st.impact + 0.3;   // window ends 300 ms after impact
        const Segmentation seg = PhaseSegmenter::segment(makeStreams(st, 5000), impactUs);
        CHECK_NEAR("Finish clamps to window edge",
                   evT(seg, Phase::Finish), st.clipEnd, 0.006);
        CHECK_TRUE("clamped Finish conf <= 0.25", evConf(seg, Phase::Finish) <= 0.25f);
        CHECK_TRUE("overall conf reflects the clamp", seg.conf <= 0.25f);
        CHECK_NEAR("Top still fine", evT(seg, Phase::Top), base.top, 0.015);
    }

    // ===== 5. Sensor degradation =====
    std::printf("== degradation ==\n");
    {
        const Segmentation noPel =
            PhaseSegmenter::segment(makeStreams(base, 5000, true, true, false), impactUs);
        CHECK_TRUE("no pelvis -> Transition omitted (not synthesized)",
                   noPel.eventFor(Phase::Transition) == nullptr);

        const Segmentation handOnly =
            PhaseSegmenter::segment(makeStreams(base, 5000, true, false, false), impactUs);
        CHECK_NEAR("hand-only Top", evT(handOnly, Phase::Top), base.top, 0.015);
        CHECK_TRUE("hand-only: forearm geometric events omitted",
                   handOnly.eventFor(Phase::MidBackswing) == nullptr
                   && handOnly.eventFor(Phase::ShaftParallelThrough) == nullptr);

        const Segmentation foreOnly =
            PhaseSegmenter::segment(makeStreams(base, 5000, false, true, false), impactUs);
        CHECK_NEAR("forearm-only Top", evT(foreOnly, Phase::Top), base.top, 0.015);
        CHECK_TRUE("forearm-only: hand Delivery proxy omitted",
                   foreOnly.eventFor(Phase::Delivery) == nullptr);

        FusedStreams empty;
        empty.timeGrid = makeGrid(base.clipEnd, 5000);
        const Segmentation none = PhaseSegmenter::segment(empty, impactUs);
        CHECK_TRUE("no streams -> clamp fallback at conf 0",
                   none.conf == 0.0f && none.eventFor(Phase::Impact) != nullptr);
        CHECK_TRUE("no streams -> bounds are the full window",
                   none.swingStartUs == empty.timeGrid.front()
                   && none.swingEndUs == empty.timeGrid.back());
    }

    // ===== 6. 100 Hz grid: same answers, coarser samples =====
    std::printf("== 100 Hz grid ==\n");
    {
        const Segmentation seg = PhaseSegmenter::segment(makeStreams(base, 10000), impactUs);
        CHECK_NEAR("Top @ 100 Hz", evT(seg, Phase::Top), base.top, 0.02);
        CHECK_NEAR("Takeaway @ 100 Hz", evT(seg, Phase::Takeaway), base.takeaway, 0.06);
        CHECK_NEAR("Finish @ 100 Hz", evT(seg, Phase::Finish), base.followEnd, 0.1);
    }

    std::printf("%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
