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

// Standalone test for the club-kinematics display series
// (src/Analysis/kinematic_series.{h,cpp}): clubhead/hand speed (mph) from the shaft
// track and the lead-forearm-vs-shaft lag angle from pose. Synthetic tracks only —
// constant-velocity motion so the mph scale and the lag geometry are exact-checkable,
// plus the product-absent (no shaft / no pose) omission contract and synth preference.

#include "../kinematic_series.h"

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;

#define CHECK(label, cond)                                        \
    do {                                                          \
        const bool ok = (cond);                                   \
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);  \
        if (!ok) ++g_fail;                                        \
    } while (0)

static constexpr double kPi      = 3.14159265358979323846;
static constexpr double kMps2Mph = 2.2369362920544;

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static const MetricSeries *find(const std::vector<MetricSeries> &v, const char *key)
{
    for (const MetricSeries &m : v)
        if (m.key == QLatin1String(key)) return &m;
    return nullptr;
}

// N constant-velocity shaft samples on a 10 ms grid: head moves at headVpx px/s (+x),
// grip at gripVpx px/s (+x); thetaRad constant. lengths.fusedPx pins the px→m scale.
static ShaftTrack2D makeShaft(int n, double headVpx, double gripVpx, double thetaRad,
                              bool fillSynth = false, double synthHeadVpx = 0.0)
{
    ShaftTrack2D s;
    s.valid       = true;
    s.frameWidth  = 1000;
    s.frameHeight = 1000;
    s.lengths.fusedPx = 1000.0;   // 1 m club spans 1000 px ⇒ 0.001 m/px
    const double dt = 0.010;      // 10 ms
    auto mk = [&](double headV) {
        std::vector<ShaftSample2D> out;
        for (int i = 0; i < n; ++i) {
            ShaftSample2D e;
            e.t_us         = int64_t(i) * 10'000;
            e.headPx       = QPointF(200.0 + headV * dt * i, 100.0);
            e.gripPx       = QPointF(100.0 + gripVpx * dt * i, 300.0);
            e.thetaRad     = thetaRad;
            e.visibleLenPx = 1000.0;
            out.push_back(e);
        }
        return out;
    };
    s.samples = mk(headVpx);
    if (fillSynth) s.synth = mk(synthHeadVpx);
    return s;
}

// Pose track with the lead-left forearm (elbow 7 → wrist 9) pointing +x (angle 0 in px
// space) at full confidence, on the same 10 ms grid.
static PoseTrack2D makePose(int n)
{
    PoseTrack2D p;
    for (int i = 0; i < n; ++i) {
        PoseFrame2D f;
        f.t_us = int64_t(i) * 10'000;
        f.kp[7] = QPointF(0.40, 0.50);   // left elbow
        f.kp[9] = QPointF(0.60, 0.50);   // left wrist  → forearm dir +x
        f.conf[7] = 1.0f;
        f.conf[9] = 1.0f;
        p.frames.push_back(f);
    }
    return p;
}

int main()
{
    std::printf("=== kinematic_series_test ===\n");

    constexpr int N = 50;
    const int64_t impactUs = 250'000;   // mid-window

    // 1. Full inputs: three series, correct identity, matching sizes.
    {
        ShaftTrack2D shaft = makeShaft(N, /*headV*/ 1000.0, /*gripV*/ 400.0, kPi / 2.0);
        PoseTrack2D  pose  = makePose(N);
        KinematicSeriesInputs in;
        in.shaft = &shaft; in.pose = &pose; in.impactUs = impactUs;
        in.handedness = 1; in.clubLengthM = 1.0;
        const std::vector<MetricSeries> out = buildKinematicSeries(in);

        CHECK("three series produced", out.size() == 3);
        const MetricSeries *ch = find(out, "clubheadSpeed");
        const MetricSeries *hd = find(out, "handSpeed");
        const MetricSeries *lg = find(out, "lagAngle");
        CHECK("clubheadSpeed present, mph", ch && ch->unit == QLatin1String("mph")
                                            && ch->label == QLatin1String("Clubhead speed"));
        CHECK("handSpeed present, mph",     hd && hd->unit == QLatin1String("mph"));
        CHECK("lagAngle present, degrees",  lg && lg->unit == QString::fromUtf8("°"));
        CHECK("parallel t/value arrays",    ch && ch->t_us.size() == ch->value.size()
                                            && ch->value.size() == size_t(N));

        // 2. mph scale exact on the constant-velocity interior (smoothing preserves the
        //    slope of a linear ramp). head 1000 px/s × 0.001 m/px = 1 m/s = 2.23694 mph;
        //    hand 400 px/s = 0.89477 mph.
        CHECK("clubhead mph exact (interior)", ch && near(ch->value[25], 1000.0 * 0.001 * kMps2Mph, 0.01));
        CHECK("hand mph exact (interior)",     hd && near(hd->value[25], 400.0 * 0.001 * kMps2Mph, 0.01));
        CHECK("speeds non-negative",           ch && hd &&
              *std::min_element(ch->value.begin(), ch->value.end()) >= 0.0 &&
              *std::min_element(hd->value.begin(), hd->value.end()) >= 0.0);

        // 3. Lag geometry: forearm +x (0 rad) vs shaft π/2 ⇒ 90°, and always in [0,180].
        CHECK("lag = 90° (forearm ⟂ shaft)", lg && near(lg->value[25], 90.0, 0.5));
        bool lagInRange = lg != nullptr;
        if (lg) for (double v : lg->value) lagInRange = lagInRange && v >= 0.0 && v <= 180.0;
        CHECK("lag in [0,180]", lagInRange);

        // 4. Impact phase dot set from impactUs.
        bool hasImpact = false;
        if (ch) for (const PhaseSample &ps : ch->phaseSamples)
            hasImpact = hasImpact || ps.phase == Phase::Impact;
        CHECK("clubhead carries an Impact phase dot", hasImpact);
    }

    // 5. No shaft ⇒ nothing (never fabricated).
    {
        PoseTrack2D pose = makePose(N);
        KinematicSeriesInputs in;
        in.shaft = nullptr; in.pose = &pose; in.impactUs = impactUs;
        CHECK("no shaft ⇒ empty", buildKinematicSeries(in).empty());
    }

    // 6. Invalid shaft (valid=false) ⇒ nothing.
    {
        ShaftTrack2D shaft = makeShaft(N, 1000.0, 400.0, kPi / 2.0);
        shaft.valid = false;
        KinematicSeriesInputs in; in.shaft = &shaft; in.impactUs = impactUs;
        CHECK("invalid shaft ⇒ empty", buildKinematicSeries(in).empty());
    }

    // 7. Shaft but no pose ⇒ speeds only, no lag.
    {
        ShaftTrack2D shaft = makeShaft(N, 1000.0, 400.0, kPi / 2.0);
        KinematicSeriesInputs in; in.shaft = &shaft; in.pose = nullptr; in.impactUs = impactUs;
        const std::vector<MetricSeries> out = buildKinematicSeries(in);
        CHECK("no pose ⇒ two series (speeds only)", out.size() == 2);
        CHECK("no pose ⇒ no lag", find(out, "lagAngle") == nullptr);
    }

    // 8. Synth channel preferred over samples: synth head velocity drives the speed.
    {
        ShaftTrack2D shaft = makeShaft(N, /*samples head*/ 1000.0, 400.0, kPi / 2.0,
                                       /*fillSynth*/ true, /*synth head*/ 2000.0);
        KinematicSeriesInputs in; in.shaft = &shaft; in.impactUs = impactUs; in.clubLengthM = 1.0;
        const std::vector<MetricSeries> out = buildKinematicSeries(in);
        const MetricSeries *ch = find(out, "clubheadSpeed");
        CHECK("synth preferred (2000 px/s ⇒ ~4.47 mph)",
              ch && near(ch->value[25], 2000.0 * 0.001 * kMps2Mph, 0.02));
    }

    // 7. Composed clubhead speed: |v_grip + L_fused·θ̇·n̂| from the track's own rate —
    //    a pure rotation about a static grip at 5 rad/s with a 1 m (1000 px) fused length
    //    is 5 m/s everywhere, whatever the head path or the visible extent say.
    {
        ShaftTrack2D shaft;
        shaft.valid = true; shaft.frameWidth = 1000; shaft.frameHeight = 1000;
        shaft.lengths.fusedPx = 1000.0;
        const double omega = 5.0;
        for (int i = 0; i < N; ++i) {
            ShaftSample2D e;
            e.t_us         = int64_t(i) * 10'000;
            e.thetaRad     = omega * 0.010 * i;
            e.thetaDotRadS = omega;
            e.gripPx       = QPointF(100.0, 300.0);
            e.visibleLenPx = 2000.0;                                     // must be IGNORED
            e.headPx       = QPointF(100.0 + 2000.0 * std::cos(e.thetaRad),
                                     300.0 + 2000.0 * std::sin(e.thetaRad));
            shaft.samples.push_back(e);
        }
        KinematicSeriesInputs in;
        in.shaft = &shaft; in.impactUs = impactUs; in.clubLengthM = 1.0; in.composed = true;
        in.gripDownM = 0.0;   // exact numbers below; the grip-down scaling is checked separately
        const std::vector<MetricSeries> out = buildKinematicSeries(in);
        const MetricSeries *ch = find(out, "clubheadSpeed");
        bool flat = ch && ch->value.size() == size_t(N);
        for (size_t i = 0; flat && i < ch->value.size(); ++i)
            flat = near(ch->value[i], 5.0 * kMps2Mph, 1e-6);
        CHECK("composed: pure rotation ⇒ L_fused·ω everywhere, visible extent ignored", flat);
        // The px→m scale maps the fused span to (clubLengthM − gripDownM), not the full club.
        in.gripDownM = 0.13;
        const std::vector<MetricSeries> outG = buildKinematicSeries(in);
        const MetricSeries *chg = find(outG, "clubheadSpeed");
        CHECK("composed: gripDownM scales the speed by (L − gripDown)/L",
              chg && near(chg->value[25], 0.87 * 5.0 * kMps2Mph, 1e-6));
        in.gripDownM = 0.0;
        // Without a P7 anchor the domain mask falls to the impact instant: samples after it are
        // invalid, the Impact dot sits on the last valid one.
        CHECK("composed: samples after impact masked invalid",
              ch && ch->valid.size() == size_t(N) && ch->valid[24] == 1u && ch->valid[25] == 1u
                 && ch->valid[26] == 0u && ch->valid[N - 1] == 0u);
        bool dotOk = false;
        if (ch) for (const PhaseSample &ps : ch->phaseSamples)
            if (ps.phase == Phase::Impact) dotOk = (ps.t_us == impactUs);
        CHECK("composed: Impact dot on the last valid sample", dotOk);
        // With a located P7 the boundary is the knot, even when impact is later.
        ShaftPosition p7; p7.p = 7; p7.t_us = 230'000;
        shaft.positions.push_back(p7);
        const std::vector<MetricSeries> outP = buildKinematicSeries(in);
        const MetricSeries *chp = find(outP, "clubheadSpeed");
        bool knot = chp && chp->valid.size() == size_t(N) && chp->valid[23] == 1u && chp->valid[24] == 0u;
        if (chp) for (const PhaseSample &ps : chp->phaseSamples)
            if (ps.phase == Phase::Impact) knot = knot && ps.t_us == 230'000;
        CHECK("composed: mask boundary is the P7 knot, Impact dot moves onto it", knot);
        shaft.positions.clear();
        // The differentiated path carries no mask at all (byte-identical legacy series).
        in.composed = false;
        const std::vector<MetricSeries> outD = buildKinematicSeries(in);
        const MetricSeries *chd = find(outD, "clubheadSpeed");
        CHECK("differentiated: no validity mask", chd && chd->valid.empty());
        in.composed = true;
        // Differentiated path (composed=false) reads the 2000 px extent instead — the
        // two modes are genuinely different producers.
        in.composed = false;
        const std::vector<MetricSeries> out2 = buildKinematicSeries(in);
        const MetricSeries *ch2 = find(out2, "clubheadSpeed");
        CHECK("differentiated: reads the head path (≈ 10 m/s here)",
              ch2 && near(ch2->value[25], 10.0 * kMps2Mph, 0.05 * kMps2Mph));
        // Composed with a moving grip along n̂ adds linearly: grip +x at 1000 px/s, θ = π/2
        // ⇒ n̂ = (−1, 0) ⇒ |1000 − 5000| px/s = 4 m/s.
        ShaftTrack2D shaft2 = shaft;
        for (int i = 0; i < N; ++i) {
            shaft2.samples[size_t(i)].thetaRad = kPi / 2.0;
            shaft2.samples[size_t(i)].gripPx   = QPointF(100.0 + 1000.0 * 0.010 * i, 300.0);
        }
        in.shaft = &shaft2; in.composed = true;
        const std::vector<MetricSeries> out3 = buildKinematicSeries(in);
        const MetricSeries *ch3 = find(out3, "clubheadSpeed");
        CHECK("composed: grip velocity adds vectorially (|1000 − 5000| px/s ⇒ 4 m/s)",
              ch3 && near(ch3->value[25], 4.0 * kMps2Mph, 1e-6));
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
