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

// Standalone test for WB3 setup + footwork metrics (src/Analysis/foot_metrics.{h,cpp}):
// conf-gated stance width / per-foot flare / toe-line angle (address, phaseSamples-
// only scalars), the lead-heel-lift Address-referenced curve (isotropic ×frame,
// anisotropy handling on a non-square frame, gap-bridged resample, no NaN), the
// legacy 17-kp no-output contract, and the fromOverrides plumbing. Synthetic
// tracks only — no fixture. Mirrors head_track_test.cpp's structure/style.

#include "../foot_metrics.h"

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

static constexpr double kPi = 3.14159265358979323846;

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// COCO-WholeBody foot keypoint indices (mirrors foot_metrics.cpp's local constants).
constexpr int kLBigToe = 17, kLHeel = 19;
constexpr int kRBigToe = 20, kRHeel = 22;
constexpr int kLShoulder = 5, kRShoulder = 6;   // COCO-17 body

// Shoulders for the fixture. stanceWidth is now a percentage OF SHOULDER WIDTH, so a frame with
// feet but no shoulders has no denominator and the metric is legitimately absent — every fixture
// that expects a stance reading has to carry them.
static constexpr double kLShoulderX = 0.44, kRShoulderX = 0.56, kShoulderY = 0.35;

// Build one feet frame: left heel/bigtoe + right heel/bigtoe at given normalized
// points, with independent per-foot confidences, plus a shoulder pair.
static PoseFrame2D makeFeet(int64_t t, QPointF lHeel, QPointF lToe, QPointF rHeel, QPointF rToe,
                           float lConf, float rConf, float shoulderConf = 0.9f)
{
    PoseFrame2D f;
    f.t_us = t;
    f.kp[kLHeel] = lHeel;   f.conf[kLHeel] = lConf;
    f.kp[kLBigToe] = lToe;  f.conf[kLBigToe] = lConf;
    f.kp[kRHeel] = rHeel;   f.conf[kRHeel] = rConf;
    f.kp[kRBigToe] = rToe;  f.conf[kRBigToe] = rConf;
    f.kp[kLShoulder] = QPointF(kLShoulderX, kShoulderY); f.conf[kLShoulder] = shoulderConf;
    f.kp[kRShoulder] = QPointF(kRShoulderX, kShoulderY); f.conf[kRShoulder] = shoulderConf;
    return f;
}

static const MetricSeries *findSeries(const std::vector<MetricSeries> &v, const char *key)
{
    for (const MetricSeries &m : v)
        if (m.key == QLatin1String(key)) return &m;
    return nullptr;
}

static double valueAt(const MetricSeries &m, int64_t t)
{
    for (size_t i = 0; i < m.t_us.size(); ++i)
        if (m.t_us[i] == t) return m.value[i];
    return std::nan("");
}

int main()
{
    // ── 1) Stance width recovery + anisotropy (non-square 1920×1080) ────────
    // Heels differ in both x and y (norm); a naive normalized-only Euclidean
    // distance would mix the two axes incorrectly — the fix computes the real
    // px distance via (frameW, frameH) separately, then re-normalizes by the
    // single reference dimension frameW (matches head_track's sway/lift).
    {
        std::printf("=== 1) stance width + anisotropy (1920x1080) ===\n");
        const int W = 1920, H = 1080;
        const int64_t dt = 10000;
        const QPointF lHeel(0.45, 0.90), lToe(0.45, 0.80);
        const QPointF rHeel(0.55, 0.92), rToe(0.55, 0.82);
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k)
            frames.push_back(makeFeet(k * dt, lHeel, lToe, rHeel, rToe, 0.9f, 0.9f));

        PoseTrack2D pose;
        pose.frames = frames;
        const FootMetricsResult res = trackFeet(pose, W, H, /*leadIsLeft=*/true, /*addressUs=*/2 * dt, {});
        CHECK("address reference resolved", res.valid);
        CHECK("stance width resolved", res.setup.stanceWidthValid);

        const double expected = std::hypot(0.10 * W, 0.02 * H) / W;
        CHECK("stance width recovered (anisotropic)",
              near(res.setup.stanceWidthXFrame, expected, 0.0008));
        const double naive = std::hypot(0.10, 0.02);   // wrong: mixes norm x/y directly
        CHECK("stance width is NOT the naive normalized-only distance",
              !near(res.setup.stanceWidthXFrame, naive, 0.0008));

        // The address heel pair, exported for ball_position. Same reference
        // frames as `widths`, so a consumer projecting onto this line shares
        // stanceWidth's denominator by construction.
        CHECK("address heel pair exported", res.setup.heelsValid);
        CHECK("lead heel is the lead foot in px",
              near(res.setup.leadHeelPxAddr.x(), 0.45 * W, 1e-6)
                  && near(res.setup.leadHeelPxAddr.y(), 0.90 * H, 1e-6));
        CHECK("trail heel is the trail foot in px",
              near(res.setup.trailHeelPxAddr.x(), 0.55 * W, 1e-6)
                  && near(res.setup.trailHeelPxAddr.y(), 0.92 * H, 1e-6));
        // The exported pair must reproduce the reported stance width exactly.
        CHECK("heel pair separation == stanceWidth",
              near(std::hypot(res.setup.trailHeelPxAddr.x() - res.setup.leadHeelPxAddr.x(),
                              res.setup.trailHeelPxAddr.y() - res.setup.leadHeelPxAddr.y()) / W,
                   res.setup.stanceWidthXFrame, 1e-9));

        // ── units: stanceWidth is ALWAYS % shoulder width ───────────────────
        // The unit used to switch between "mm" and "×frame" per swing depending on whether the
        // ball ruler resolved. A metric whose unit changes per swing cannot carry a norm — the
        // norm declares one unit and the loader rejects a mismatch — so the unit is now invariant
        // and the metric is ABSENT when its denominator is missing.
        const std::vector<MetricSeries> off = buildFootSeries(res, {}, /*mmPerPx*/ -1.0);
        const MetricSeries *swOff = findSeries(off, "stanceWidth");
        CHECK("no ruler -> stanceWidth is still emitted (it does not need the ruler)",
              swOff != nullptr);
        CHECK("no ruler -> unit is still % shoulder width",
              swOff && swOff->unit == QStringLiteral("% shoulder width"));
        CHECK("no ruler -> no absolute reading at all, rather than a different unit",
              findSeries(off, "stanceWidthMm") == nullptr);

        const double mmPerPx = 2.0;
        const std::vector<MetricSeries> on = buildFootSeries(res, {}, mmPerPx);
        const MetricSeries *swOn = findSeries(on, "stanceWidth");
        CHECK("ruler -> stanceWidth unit is UNCHANGED by the ruler",
              swOn && swOn->unit == QStringLiteral("% shoulder width"));
        CHECK("ruler -> the percentage itself is unchanged by the ruler",
              swOn && swOff && near(swOn->phaseSamples[0].value, swOff->phaseSamples[0].value, 1e-12));

        // The millimetre reading moved to its own metric, so both units stay invariant.
        const MetricSeries *mmOn = findSeries(on, "stanceWidthMm");
        CHECK("ruler -> the absolute reading appears as its own metric", mmOn != nullptr);
        CHECK("ruler -> and it is real millimetres",
              mmOn && mmOn->unit == QStringLiteral("mm")
                   && near(mmOn->phaseSamples[0].value,
                           res.setup.stanceWidthXFrame * W * mmPerPx, 1e-9));

        // The percentage is the stance over the shoulders, both in the same px space.
        CHECK("shoulder width resolved from the address frames", res.setup.shoulderWidthValid);
        CHECK("stanceWidth == stance / shoulders x 100",
              swOn && res.setup.shoulderWidthPx > 0.0
                   && near(swOn->phaseSamples[0].value,
                           res.setup.stanceWidthXFrame * W / res.setup.shoulderWidthPx * 100.0,
                           1e-9));

        // leadHeelLift shares the ruler and the depth plane but is deliberately
        // NOT converted — it is a separately-shipped metric with its own gate.
        const MetricSeries *liftOn = findSeries(on, "leadHeelLift");
        CHECK("leadHeelLift stays ×frame even with a ruler",
              liftOn && liftOn->unit == QStringLiteral("×frame"));
        // Every other setup scalar is an angle and must be untouched by the ruler.
        const MetricSeries *flareOn = findSeries(on, "leadFootFlare");
        CHECK("angles are unaffected by the ruler",
              flareOn && flareOn->unit == QStringLiteral("°"));
    }

    // ── 2) Flare + toe-line angle recovery (square 1000×1000) ───────────────
    {
        std::printf("=== 2) flare + toe-line angles (1000x1000) ===\n");
        const int W = 1000, H = 1000;
        const int64_t dt = 10000;
        const double r = 0.05;
        const auto rot = [](double deg) {
            const double rad = deg * kPi / 180.0;
            return QPointF(std::cos(rad), std::sin(rad));
        };
        const QPointF lHeel(0.50, 0.60), rHeelP(0.70, 0.60);
        const QPointF ldir = rot(-100.0), rdir = rot(-80.0);
        const QPointF lToe(lHeel.x() + r * ldir.x(), lHeel.y() + r * ldir.y());
        const QPointF rToe(rHeelP.x() + r * rdir.x(), rHeelP.y() + r * rdir.y());

        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k)
            frames.push_back(makeFeet(k * dt, lHeel, lToe, rHeelP, rToe, 0.9f, 0.9f));

        PoseTrack2D pose;
        pose.frames = frames;
        const FootMetricsResult res = trackFeet(pose, W, H, true, 2 * dt, {});
        CHECK("lead flare resolved", res.setup.leadFlareValid);
        CHECK("trail flare resolved", res.setup.trailFlareValid);
        CHECK("toe-line resolved", res.setup.toeLineValid);
        CHECK("lead flare ~ -100 deg", near(res.setup.leadFlareDeg, -100.0, 0.3));
        CHECK("trail flare ~ -80 deg", near(res.setup.trailFlareDeg, -80.0, 0.3));
        CHECK("toe-line ~ 0 deg (both bigtoes level)", near(res.setup.toeLineDeg, 0.0, 0.3));
    }

    // ── 3) Lead-heel-lift ramp recovery (1920×1080) ─────────────────────────
    // Still address hold (6 frames) then the lead heel rises (toe fixed) while
    // the trail foot stays low-confidence throughout (never contributes).
    {
        std::printf("=== 3) lead-heel-lift ramp (1920x1080) ===\n");
        const int W = 1920, H = 1080;
        const int64_t dt = 10000;
        const QPointF lToeFixed(0.50, 0.80);
        const QPointF trailHeel(0.70, 0.85), trailToe(0.70, 0.80);
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k <= 5; ++k)
            frames.push_back(makeFeet(k * dt, QPointF(0.50, 0.85), lToeFixed,
                                      trailHeel, trailToe, 0.9f, 0.1f));
        for (int s = 1; s <= 20; ++s) {
            const int k = 5 + s;
            frames.push_back(makeFeet(k * dt, QPointF(0.50, 0.85 - 0.004 * s), lToeFixed,
                                      trailHeel, trailToe, 0.9f, 0.1f));
        }

        PoseTrack2D pose;
        pose.frames = frames;
        FootMetricsConfig cfg;
        cfg.addrWindowUs = 30000;   // isolate the 6-frame level hold (see head_track_test note)
        const FootMetricsResult res = trackFeet(pose, W, H, /*leadIsLeft=*/true, /*addressUs=*/2 * dt, cfg);
        CHECK("address reference resolved", res.valid);
        CHECK("lead flare resolved (heel+toe both confident)", res.setup.leadFlareValid);
        CHECK("trail foot never confident -> stanceWidth absent", !res.setup.stanceWidthValid);
        CHECK("trail foot never confident -> toe-line absent", !res.setup.toeLineValid);

        const std::vector<MetricSeries> series = buildFootSeries(res, {});
        const MetricSeries *lift = findSeries(series, "leadHeelLift");
        CHECK("leadHeelLift emitted", lift != nullptr);
        if (lift) {
            CHECK("unit == ×frame", lift->unit == QStringLiteral("×frame"));
            CHECK("grid covers every frame", lift->t_us.size() == frames.size());
            const int s = 5;                       // ramp step 5 ⇒ frame k=10
            const int64_t t = (5 + s) * dt;
            const double expLift = (0.004 * s) * double(H) / double(W);   // = 0.01125
            CHECK("lift recovered (~0.01125 ×frame, + = heel up)",
                  near(valueAt(*lift, t), expLift, 0.0006));
        }
        CHECK("no setup scalars from a foot that never resolves", !res.setup.trailFlareValid);
    }

    // ── 4) Low-confidence gap coasts without NaN ────────────────────────────
    {
        std::printf("=== 4) low-conf gap bridged, no NaN ===\n");
        const int W = 1600, H = 1200;
        const int64_t dt = 8000;
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 30; ++k) {
            const double y = 0.85 - 0.001 * k;   // slow, monotone heel rise
            const bool gap = (k >= 12 && k <= 17);
            const float c = gap ? 0.10f : 0.9f;
            frames.push_back(makeFeet(k * dt, QPointF(0.50, y), QPointF(0.50, 0.80),
                                      QPointF(0.70, 0.85), QPointF(0.70, 0.80), c, 0.1f));
        }
        PoseTrack2D pose;
        pose.frames = frames;
        const FootMetricsResult res = trackFeet(pose, W, H, true, -1, {});   // fallback address ref
        CHECK("resolved via first-N fallback", res.valid);
        int nInvalid = 0;
        for (const FootState &s : res.states) if (!s.leadValid) ++nInvalid;
        CHECK("gap frames marked invalid", nInvalid == 6);

        const std::vector<MetricSeries> series = buildFootSeries(res, {});
        const MetricSeries *lift = findSeries(series, "leadHeelLift");
        CHECK("leadHeelLift emitted", lift != nullptr);
        if (lift) {
            CHECK("resampled grid covers all 30 frames", lift->t_us.size() == 30);
            bool allFinite = true;
            for (double v : lift->value) allFinite = allFinite && std::isfinite(v);
            for (const PhaseSample &ps : lift->phaseSamples) allFinite = allFinite && std::isfinite(ps.value);
            CHECK("no NaN anywhere in the series", allFinite);
            const double lo = valueAt(*lift, 11 * dt), hi = valueAt(*lift, 18 * dt);
            const double mid = valueAt(*lift, 15 * dt);
            CHECK("gap value linearly bridged", mid > lo && mid < hi && std::isfinite(mid));
        }
    }

    // ── 5) Legacy 17-kp track yields no output (never garbage) ──────────────
    {
        std::printf("=== 5) legacy 17-kp track -> no output ===\n");
        std::vector<PoseFrame2D> legacy;
        for (int k = 0; k < 10; ++k) {
            PoseFrame2D f;
            f.t_us = k * 10000;
            // Only a body keypoint set (irrelevant here) — feet (17-22) left
            // default (conf 0), exactly what a pre-WB0 swing.json load produces.
            f.kp[11] = QPointF(0.45, 0.55);
            f.conf[11] = 0.9f;
            legacy.push_back(f);
        }
        PoseTrack2D pose;
        pose.frames = legacy;
        const FootMetricsResult res = trackFeet(pose, 1280, 720, true, -1, {});
        CHECK("legacy track -> not valid", !res.valid);
        CHECK("legacy track -> no series emitted", buildFootSeries(res, {}).empty());
    }

    // ── 6) fromOverrides dotted keys reach the config ──────────────────────
    {
        std::printf("=== 6) fromOverrides ===\n");
        QVariantMap ov;
        ov.insert(QStringLiteral("foot.confMin"), 0.5);
        ov.insert(QStringLiteral("foot.addrMinFrames"), 8);
        ov.insert(QStringLiteral("foot.addrWindowUs"), qint64(500000));
        const FootMetricsConfig c = FootMetricsConfig::fromOverrides(ov);
        CHECK("confMin=0.5", near(c.confMin, 0.5, 1e-9));
        CHECK("addrMinFrames=8", c.addrMinFrames == 8);
        CHECK("addrWindowUs=500000", c.addrWindowUs == 500000);

        const FootMetricsConfig def = FootMetricsConfig::fromOverrides(QVariantMap{});
        CHECK("default confMin==0.30", near(def.confMin, 0.30, 1e-9));
        CHECK("default addrMinFrames==5", def.addrMinFrames == 5);
        CHECK("default addrWindowUs==250000", def.addrWindowUs == 250000);
    }

    // ── 7) Degenerate inputs degrade gracefully ────────────────────────────
    {
        std::printf("=== 7) degenerate inputs ===\n");
        PoseTrack2D empty;
        const FootMetricsResult r0 = trackFeet(empty, 1000, 1000, true, -1, {});
        CHECK("empty track -> not valid", !r0.valid);
        CHECK("empty track -> no series", buildFootSeries(r0, {}).empty());

        // All keypoints below the gate ⇒ no foot anywhere.
        std::vector<PoseFrame2D> lowConf;
        for (int k = 0; k < 5; ++k)
            lowConf.push_back(makeFeet(k * 10000, QPointF(0.45, 0.90), QPointF(0.45, 0.80),
                                       QPointF(0.55, 0.90), QPointF(0.55, 0.80), 0.1f, 0.1f));
        PoseTrack2D lp;
        lp.frames = lowConf;
        const FootMetricsResult r1 = trackFeet(lp, 1000, 1000, true, -1, {});
        CHECK("all-low-conf -> not valid", !r1.valid);

        const FootMetricsResult r2 = trackFeet(empty, 0, 0, true, -1, {});   // bad dims
        CHECK("zero frame dims -> not valid", !r2.valid);
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
