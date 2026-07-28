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

// Segmentation v2/v3 signal preparation (phase_signals) against analytic
// trajectories: zero-phase filtering, envelopes, world-frame gyro, the
// swing-plane axis, inclination, stillness, and sub-grid refinement.

#include <cmath>
#include <cstdio>
#include <vector>

#include <QQuaternion>
#include <QVector3D>

#include "../phase_signals.h"

using namespace pinpoint::analysis;
namespace ps = pinpoint::analysis::phase_signals;

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

int main()
{
    const double  fs = 200.0;
    const int64_t dtUs = 5000;

    // ===== Zero-phase Butterworth =====
    std::printf("== lowpassZeroPhase ==\n");
    {
        // 2 Hz sine, well below the 10 Hz cutoff: amplitude and PHASE preserved.
        std::vector<double> x;
        for (int i = 0; i < 400; ++i)
            x.push_back(std::sin(2.0 * kPi * 2.0 * i / fs));
        const std::vector<double> y = ps::lowpassZeroPhase(x, fs, 10.0);

        // ONE period, so there is exactly one maximum to find. A 2 Hz sine at 200 Hz peaks every
        // 100 samples, so the old [100,300) window held two — at 125 and 225, bit-identical in
        // double. Which one a strict `>` scan kept was then decided by rounding: the raw signal
        // kept the first, the filtered one could keep either, and the test read a 100-sample
        // "phase shift" off a tie rather than off the filter.
        int rawPeak = 100, fltPeak = 100;
        for (int i = 100; i < 200; ++i) {
            if (x[size_t(i)] > x[size_t(rawPeak)]) rawPeak = i;
            if (y[size_t(i)] > y[size_t(fltPeak)]) fltPeak = i;
        }
        CHECK_NEAR("2 Hz peak index unchanged (zero phase)", fltPeak, rawPeak, 1.0);
        CHECK_NEAR("2 Hz amplitude preserved", y[size_t(fltPeak)], 1.0, 0.02);

        // 50 Hz sine, far above cutoff: squashed (forward+backward = |H|²).
        std::vector<double> hf;
        for (int i = 0; i < 400; ++i)
            hf.push_back(std::sin(2.0 * kPi * 50.0 * i / fs));
        const std::vector<double> yh = ps::lowpassZeroPhase(hf, fs, 10.0);
        double peak = 0.0;
        for (int i = 100; i < 300; ++i) peak = std::max(peak, std::abs(yh[size_t(i)]));
        CHECK_TRUE("50 Hz attenuated below 0.1", peak < 0.1);

        // DC offset untouched; degenerate inputs pass through.
        std::vector<double> dc(100, 7.5);
        CHECK_NEAR("DC preserved", ps::lowpassZeroPhase(dc, fs, 10.0)[50], 7.5, 1e-6);
        CHECK_TRUE("fc >= Nyquist passes through",
                   ps::lowpassZeroPhase(x, fs, 120.0) == x);
    }

    // ===== Energy envelope =====
    std::printf("== energyEnvelope ==\n");
    {
        SegmentStream s;
        for (int i = 0; i < 200; ++i) {
            s.qAnat.push_back(QQuaternion());
            s.gyroDps.push_back(QVector3D(60.f, 0.f, 80.f));   // ‖·‖ = 100 °/s
            s.accelG.push_back(QVector3D(0, 0, 1));
        }
        const std::vector<double> env = ps::energyEnvelope(s, fs);
        CHECK_NEAR("constant-rate envelope (mid)", env[100], 100.0, 0.5);
        CHECK_NEAR("constant-rate envelope (edge)", env[5], 100.0, 1.0);
    }

    // ===== World-frame gyro =====
    std::printf("== worldGyro ==\n");
    {
        // Segment pitched +90° about world X: body +Y maps to world +Z.
        SegmentStream s;
        s.qAnat.push_back(QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), 90.f));
        s.gyroDps.push_back(QVector3D(0.f, 50.f, 0.f));
        const std::vector<QVector3D> gw = ps::worldGyro(s);
        CHECK_NEAR("body +Y -> world +Z (x)", gw[0].x(), 0.0, 1e-4);
        CHECK_NEAR("body +Y -> world +Z (y)", gw[0].y(), 0.0, 1e-4);
        CHECK_NEAR("body +Y -> world +Z (z)", gw[0].z(), 50.0, 1e-3);

        // Spin about world Z with consistent body gyro: qAnat = Rz(ωt), body
        // gyro constant (0,0,ω) — world gyro must come out (0,0,ω) throughout.
        SegmentStream spin;
        const double w = 240.0;   // °/s
        for (int i = 0; i < 100; ++i) {
            const double t = i / fs;
            spin.qAnat.push_back(
                QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), float(w * t)));
            spin.gyroDps.push_back(QVector3D(0.f, 0.f, float(w)));
        }
        const std::vector<QVector3D> gs = ps::worldGyro(spin);
        double worstOff = 0.0;
        for (const QVector3D &g : gs)
            worstOff = std::max(worstOff, double((g - QVector3D(0, 0, float(w))).length()));
        CHECK_NEAR("consistent spin stays on world Z", worstOff, 0.0, 1e-2);
    }

    // ===== Principal rotation axis =====
    std::printf("== principalRotationAxis ==\n");
    {
        std::vector<int64_t> grid;
        std::vector<QVector3D> gw;
        const QVector3D n = QVector3D(1, 0, 1).normalized();
        for (int i = 0; i < 100; ++i) {
            grid.push_back(int64_t(i) * dtUs);
            // Half-sine rate profile about n̂, all positive — a downswing burst.
            const float rate = float(300.0 * std::sin(kPi * i / 99.0));
            gw.push_back(n * rate);
        }
        const QVector3D axis = ps::principalRotationAxis(gw, grid, 0, 99 * dtUs);
        CHECK_NEAR("axis . n (aligned, sign-fixed)",
                   QVector3D::dotProduct(axis, n), 1.0, 1e-3);

        // All-negative rates: sign-fix must flip so the mean projection is +.
        std::vector<QVector3D> neg;
        for (int i = 0; i < 100; ++i)
            neg.push_back(n * float(-300.0 * std::sin(kPi * i / 99.0)));
        const QVector3D axisNeg = ps::principalRotationAxis(neg, grid, 0, 99 * dtUs);
        CHECK_NEAR("axis . n (negative rates flip sign)",
                   QVector3D::dotProduct(axisNeg, n), -1.0, 1e-3);

        // No energy in the interval -> null vector.
        const QVector3D none = ps::principalRotationAxis(gw, grid, 1000000000, 2000000000);
        CHECK_TRUE("empty interval -> null axis", none.isNull());
    }

    // ===== Inclination =====
    std::printf("== inclination ==\n");
    {
        SegmentStream s;
        s.qAnat.push_back(QQuaternion());                                            // +Y horizontal
        s.qAnat.push_back(QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), 90.f));  // +Y -> +Z (up)
        s.qAnat.push_back(QQuaternion::fromAxisAndAngle(QVector3D(1, 0, 0), -90.f)); // +Y -> -Z (down)
        const std::vector<double> th = ps::inclination(s);
        CHECK_NEAR("horizontal axis -> 0", th[0], 0.0, 1e-5);
        CHECK_NEAR("axis up -> +pi/2", th[1], kPi / 2.0, 1e-4);
        CHECK_NEAR("axis down -> -pi/2", th[2], -kPi / 2.0, 1e-4);
    }

    // ===== Axial rate =====
    std::printf("== axialRate ==\n");
    {
        SegmentStream s;
        s.qAnat.push_back(QQuaternion());
        s.gyroDps.push_back(QVector3D(5.f, 30.f, -2.f));
        CHECK_NEAR("dot with +Y", ps::axialRate(s, QVector3D(0, 1, 0))[0], 30.0, 1e-4);
        CHECK_NEAR("dot with unnormalized axis",
                   ps::axialRate(s, QVector3D(0, 2, 0))[0], 30.0, 1e-4);
    }

    // ===== Stillness =====
    std::printf("== stillMask ==\n");
    {
        FusedStreams fsm;
        for (int i = 0; i < 6; ++i) fsm.timeGrid.push_back(int64_t(i) * dtUs);
        SegmentStream a, b;
        a.role = SegmentRole::LeadHand;
        b.role = SegmentRole::Pelvis;
        for (int i = 0; i < 6; ++i) {
            a.qAnat.push_back(QQuaternion());
            b.qAnat.push_back(QQuaternion());
            a.gyroDps.push_back(QVector3D(5, 0, 0));        // still hand throughout
            a.accelG.push_back(QVector3D(0, 0, 1));
            b.gyroDps.push_back(QVector3D(5, 0, 0));
            b.accelG.push_back(QVector3D(0, 0, 1));
        }
        b.gyroDps[2] = QVector3D(20, 0, 0);                 // pelvis moves at i=2
        b.accelG[4]  = QVector3D(0, 0, 1.2f);               // pelvis jolts at i=4
        fsm.segments.push_back(a);
        fsm.segments.push_back(b);
        const std::vector<uint8_t> still = ps::stillMask(fsm);
        CHECK_TRUE("quiet sample still", still[0] == 1 && still[1] == 1);
        CHECK_TRUE("any-segment gyro motion breaks stillness", still[2] == 0);
        CHECK_TRUE("any-segment accel jolt breaks stillness", still[4] == 0);

        // Threshold boundaries (strict <): exactly at threshold is NOT still.
        FusedStreams edge;
        edge.timeGrid = { 0 };
        SegmentStream e;
        e.qAnat.push_back(QQuaternion());
        e.gyroDps.push_back(QVector3D(15.f, 0.f, 0.f));
        e.accelG.push_back(QVector3D(0, 0, 1));
        edge.segments.push_back(e);
        CHECK_TRUE("gyro exactly at threshold not still",
                   ps::stillMask(edge)[0] == 0);
        edge.segments[0].gyroDps[0] = QVector3D(14.9f, 0.f, 0.f);
        CHECK_TRUE("gyro just under threshold still",
                   ps::stillMask(edge)[0] == 1);

        FusedStreams none;
        none.timeGrid = { 0, dtUs };
        CHECK_TRUE("no segments -> never still", ps::stillMask(none)[0] == 0);
    }

    // ===== Sub-grid refinement =====
    std::printf("== refinement ==\n");
    {
        // Parabolic peak at fractional index 5.3.
        std::vector<double> y;
        for (int i = 0; i < 11; ++i) y.push_back(-(i - 5.3) * (i - 5.3));
        int peak = 0;
        for (int i = 1; i < 11; ++i) if (y[size_t(i)] > y[size_t(peak)]) peak = i;
        CHECK_NEAR("parabolic peak refined", ps::refineExtremum(y, peak), 5.3, 1e-6);

        // Linear zero crossing at 4.6.
        std::vector<double> z;
        for (int i = 0; i < 10; ++i) z.push_back(double(i) - 4.6);
        CHECK_NEAR("zero crossing refined", ps::refineZeroCrossing(z, 4), 4.6, 1e-9);
        CHECK_NEAR("non-straddle left at sample", ps::refineZeroCrossing(z, 6), 6.0, 1e-9);
        CHECK_NEAR("edge index unchanged", ps::refineExtremum(y, 0), 0.0, 1e-9);

        // Fractional index -> µs on a 5 ms grid (and clamping).
        std::vector<int64_t> grid;
        for (int i = 0; i < 10; ++i) grid.push_back(int64_t(i) * dtUs);
        CHECK_NEAR("fracIndexToUs interpolates", double(ps::fracIndexToUs(grid, 4.6)),
                   23000.0, 0.5);
        CHECK_NEAR("fracIndexToUs clamps high", double(ps::fracIndexToUs(grid, 99.0)),
                   45000.0, 0.5);
        CHECK_NEAR("fracIndexToUs clamps low", double(ps::fracIndexToUs(grid, -1.0)),
                   0.0, 0.5);

        // End-to-end sub-grid accuracy: a 3 Hz sine peak refined to < 1 ms on
        // the 200 Hz grid (the A.4 "parabola brings events to ~1 ms" claim).
        std::vector<double> s;
        std::vector<int64_t> g2;
        for (int i = 0; i < 200; ++i) {
            s.push_back(std::sin(2.0 * kPi * 3.0 * (i / fs)));
            g2.push_back(int64_t(i) * dtUs);
        }
        int p = 0;   // first period only — later peaks are equal-amplitude twins
        for (int i = 1; i < 66; ++i) if (s[size_t(i)] > s[size_t(p)]) p = i;
        const int64_t tPeak = ps::fracIndexToUs(g2, ps::refineExtremum(s, p));
        CHECK_NEAR("sine peak within 1 ms of true 1/12 s",
                   double(tPeak), 1e6 / 12.0, 1000.0);
    }

    std::printf("%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
