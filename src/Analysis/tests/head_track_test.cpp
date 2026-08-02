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

// Standalone test for WB2 head tracking (src/Analysis/head_track.{h,cpp}):
// conf-gated head centre/scale/tilt from the COCO body head keypoints, the robust
// Address reference, isotropic ×frame sway/lift (anisotropy handling on a
// non-square frame), the tilt ramp, the low-confidence gap coasting without NaN,
// and the fromOverrides plumbing. Synthetic tracks only — no fixture.

#include "../head_track.h"

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

// Build one head frame: nose(0) at (cx,cy); eyes(1,2) at ±eyeL along the eye line
// rotated by alpha; ears(3,4) at ±earL along the same line. Optional deterministic
// per-keypoint noise (amplitude noiseAmp, indexed by `seed`).
static PoseFrame2D makeHead(int64_t t, double cx, double cy, double eyeL, double earL,
                            double alpha, float eyeConf, float earConf, float noseConf,
                            double noiseAmp = 0.0, int seed = 0)
{
    PoseFrame2D f;
    f.t_us = t;
    const double ca = std::cos(alpha), sa = std::sin(alpha);
    const auto nz = [&](int i) {
        return noiseAmp == 0.0 ? 0.0 : noiseAmp * std::sin(double(seed) * 1.7 + i * 0.9);
    };
    const auto set = [&](int i, double x, double y, float c) {
        f.kp[i] = QPointF(x + nz(i * 2), y + nz(i * 2 + 1));
        f.conf[i] = c;
    };
    set(0, cx, cy, noseConf);
    set(1, cx - eyeL * ca, cy - eyeL * sa, eyeConf);   // LEye
    set(2, cx + eyeL * ca, cy + eyeL * sa, eyeConf);   // REye
    set(3, cx - earL * ca, cy - earL * sa, earConf);   // LEar
    set(4, cx + earL * ca, cy + earL * sa, earConf);   // REar
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
    // ── 1) Sway/lift ramp recovery + anisotropy (non-square 1920×1080) ──────────
    // Still address hold (6 frames) then a linear centre ramp. sway is in ×frame
    // WIDTH; lift is a normalized-HEIGHT displacement re-scaled to ×frame WIDTH, so
    // an equal normalized x/y move yields DIFFERENT ×frame values (the anisotropy
    // fix). Expected lift = Δcy_norm × (H/W).
    {
        std::printf("=== 1) sway/lift ramp + anisotropy (1920x1080) ===\n");
        const int W = 1920, H = 1080;
        const int64_t dt = 10000;   // 100 fps
        const double cx0 = 0.5, cy0 = 0.4, eyeL = 0.02;
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k <= 5; ++k)   // address hold, level
            frames.push_back(makeHead(k * dt, cx0, cy0, eyeL, 1.8 * eyeL, 0.0,
                                      0.9f, 0.9f, 0.9f, 0.0002, k));
        const double dxs = 0.004, dls = 0.006;   // per-step sway / lift (up)
        for (int s = 1; s <= 20; ++s) {
            const int k = 5 + s;
            frames.push_back(makeHead(k * dt, cx0 + s * dxs, cy0 - s * dls, eyeL,
                                      1.8 * eyeL, 0.0, 0.9f, 0.9f, 0.9f, 0.0002, k));
        }

        PoseTrack2D pose;
        pose.frames = frames;   // smoothed empty ⇒ raw-frame fallback exercised

        // Narrow the robust-reference window so only the 6-frame still hold feeds
        // the median (this synthetic ramp is far denser than a real one, where a
        // long quasi-static hold dominates the default ±250 ms window).
        HeadTrackConfig cfg;
        cfg.addrWindowUs = 30000;   // ±30 ms about the Address event ⇒ hold frames only
        const HeadTrackResult res = trackHead(pose, W, H, /*addressUs=*/2 * dt, cfg);
        CHECK("address reference resolved", res.valid);
        CHECK("one state per frame", res.states.size() == frames.size());
        CHECK("addr centre px ~ (960,432)",
              near(res.addrCenterPx.x(), 0.5 * W, 5.0) && near(res.addrCenterPx.y(), 0.4 * H, 5.0));
        // ears present ⇒ head scale = inter-ear = 2·1.8·eyeL·W
        CHECK("addr head scale ~ inter-ear px",
              near(res.addrScalePx, 2.0 * 1.8 * eyeL * W, 3.0));

        // The linear channels are centimetres and need the ruler. A round pxPerMm keeps the
        // anisotropy arithmetic below readable: cm = ×frame × W / pxPerMm / 10.
        const double pxPerMm = 4.0;
        const double toCm    = double(W) / pxPerMm / 10.0;          // ×frame → cm, = 48
        const std::vector<MetricSeries> series = buildHeadSeries(res, {} /*phases*/, pxPerMm);
        const MetricSeries *sway = findSeries(series, "headSway");
        const MetricSeries *lift = findSeries(series, "headLift");
        CHECK("headSway emitted", sway != nullptr);
        CHECK("headLift emitted", lift != nullptr);
        if (sway && lift) {
            CHECK("sway unit == cm", sway->unit == QStringLiteral("cm"));
            CHECK("grid covers every frame", sway->t_us.size() == frames.size());
            const int s = 5;                       // ramp step 5 ⇒ frame k=10
            const int64_t t = (5 + s) * dt;
            const double expSway = s * dxs * toCm;                             // 0.02 ×frame
            const double expLift = (s * dls) * double(H) / double(W) * toCm;   // anisotropy first
            CHECK("sway recovered (0.02 ×frame → cm)", near(valueAt(*sway, t), expSway, 0.08));
            CHECK("lift recovered (H/W scaled, then → cm)",
                  near(valueAt(*lift, t), expLift, 0.08));
            // A naive (no-aspect) lift would read Δcy_norm = 0.03 — the fix must NOT.
            CHECK("lift is NOT the un-scaled 0.03",
                  !near(valueAt(*lift, t), s * dls * toCm, 0.25));
        }
        // The invariant-unit rule: no ruler ⇒ ABSENT, never a different scale. headSway shipped in
        // mm against a corridor of 4 ± 3 cm and read Action on every swing ever recorded, while the
        // ×frame fallback read Ideal on every one — the same defect in both directions.
        const std::vector<MetricSeries> noRuler = buildHeadSeries(res, {}, -1.0);
        CHECK("no ruler -> headSway absent", findSeries(noRuler, "headSway") == nullptr);
        CHECK("no ruler -> headLift absent", findSeries(noRuler, "headLift") == nullptr);
        CHECK("…but headTilt is degrees and needs no ruler, so it survives",
              findSeries(noRuler, "headTilt") != nullptr);
    }

    // ── 2) Tilt ramp recovery (square 1000×1000 so px angle == eye-line angle) ──
    {
        std::printf("=== 2) tilt ramp (1000x1000) ===\n");
        const int W = 1000, H = 1000;
        const int64_t dt = 10000;
        const double eyeL = 0.03;
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k <= 4; ++k)   // address hold, level (alpha 0)
            frames.push_back(makeHead(k * dt, 0.5, 0.5, eyeL, 1.8 * eyeL, 0.0, 0.9f, 0.9f, 0.9f));
        for (int s = 1; s <= 10; ++s) {
            const int k = 4 + s;
            const double alpha = double(s) * 2.0 * kPi / 180.0;   // +2°/step
            frames.push_back(makeHead(k * dt, 0.5, 0.5, eyeL, 1.8 * eyeL, alpha, 0.9f, 0.9f, 0.9f));
        }
        PoseTrack2D pose; pose.frames = frames;
        HeadTrackConfig cfg;
        cfg.addrWindowUs = 25000;   // isolate the 5-frame level hold (see test 1 note)
        const HeadTrackResult res = trackHead(pose, W, H, /*addressUs=*/2 * dt, cfg);
        CHECK("addr tilt ~ 0", near(res.addrTiltDeg, 0.0, 0.1));

        const std::vector<MetricSeries> series = buildHeadSeries(res, {});
        const MetricSeries *tilt = findSeries(series, "headTilt");
        CHECK("headTilt emitted", tilt != nullptr);
        if (tilt) {
            CHECK("tilt unit == degrees", tilt->unit == QStringLiteral("°"));
            const int s = 6;                     // +12°
            const int64_t t = (4 + s) * dt;
            CHECK("tilt recovered (~12°)", near(valueAt(*tilt, t), 12.0, 0.2));
        }
    }

    // ── 3) Low-confidence gap coasts without NaN ────────────────────────────────
    {
        std::printf("=== 3) low-conf gap bridged, no NaN ===\n");
        const int W = 1600, H = 1200;
        const int64_t dt = 8000;
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 30; ++k) {
            const double cx = 0.5 + 0.003 * k;
            // frames 12..17 drop below the conf gate (occluded head)
            const bool gap = (k >= 12 && k <= 17);
            const float c = gap ? 0.10f : 0.9f;
            frames.push_back(makeHead(k * dt, cx, 0.45, 0.02, 0.036, 0.0, c, c, c));
        }
        PoseTrack2D pose; pose.frames = frames;
        const HeadTrackResult res = trackHead(pose, W, H, -1, {});   // fallback address ref
        CHECK("resolved via first-N fallback", res.valid);
        int nInvalid = 0;
        for (const HeadState &s : res.states) if (!s.valid) ++nInvalid;
        CHECK("gap frames marked invalid", nInvalid == 6);
        CHECK("sway channel skips the gap (24 valid samples)", res.sway.t_us.size() == 24);

        const std::vector<MetricSeries> series = buildHeadSeries(res, {}, /*pxPerMm*/ 4.0);
        const MetricSeries *sway = findSeries(series, "headSway");
        CHECK("headSway emitted", sway != nullptr);
        if (sway) {
            CHECK("resampled grid covers all 30 frames", sway->t_us.size() == 30);
            bool allFinite = true;
            for (double v : sway->value) allFinite = allFinite && std::isfinite(v);
            for (const PhaseSample &ps : sway->phaseSamples) allFinite = allFinite && std::isfinite(ps.value);
            CHECK("no NaN anywhere in the series", allFinite);
            // A bridged gap value sits between its confident neighbours (k=11 and k=18).
            const double lo = valueAt(*sway, 11 * dt), hi = valueAt(*sway, 18 * dt);
            const double mid = valueAt(*sway, 15 * dt);
            CHECK("gap value linearly bridged", mid > lo && mid < hi && std::isfinite(mid));
        }
    }

    // ── 4) Head scale: ear path vs eye-fallback path are distinguishable ────────
    {
        std::printf("=== 4) scale ear vs eye-fallback ===\n");
        const int W = 1000, H = 1000;
        // Ears at 0.05 half-offset (inter-ear 0.10), eyes at 0.02 (inter-eye 0.04).
        // Ears confident ⇒ inter-ear = 100 px. Ears below gate ⇒ inter-eye×1.8 = 72 px.
        std::vector<PoseFrame2D> earFrames, eyeFrames;
        for (int k = 0; k < 6; ++k) {
            earFrames.push_back(makeHead(k * 10000, 0.5, 0.5, 0.02, 0.05, 0.0, 0.9f, 0.9f, 0.9f));
            eyeFrames.push_back(makeHead(k * 10000, 0.5, 0.5, 0.02, 0.05, 0.0, 0.9f, 0.10f, 0.9f));
        }
        PoseTrack2D ep; ep.frames = earFrames;
        PoseTrack2D yp; yp.frames = eyeFrames;
        const HeadTrackResult er = trackHead(ep, W, H, 0, {});
        const HeadTrackResult yr = trackHead(yp, W, H, 0, {});
        CHECK("ears present ⇒ scale = inter-ear (100 px)", near(er.addrScalePx, 100.0, 0.5));
        CHECK("ears absent ⇒ scale = inter-eye×1.8 (72 px)", near(yr.addrScalePx, 72.0, 0.5));
    }

    // ── 5) fromOverrides dotted keys reach the config ──────────────────────────
    {
        std::printf("=== 5) fromOverrides ===\n");
        QVariantMap ov;
        ov.insert(QStringLiteral("head.confMin"), 0.5);
        ov.insert(QStringLiteral("head.earIpdFactor"), 2.0);
        ov.insert(QStringLiteral("head.earWidthMm"), 130.0);
        ov.insert(QStringLiteral("head.chinConfWeight"), 0.3);
        ov.insert(QStringLiteral("head.minContribPts"), 3);
        ov.insert(QStringLiteral("head.addrMinFrames"), 8);
        ov.insert(QStringLiteral("head.addrWindowUs"), qint64(500000));
        const HeadTrackConfig c = HeadTrackConfig::fromOverrides(ov);
        CHECK("confMin=0.5", near(c.confMin, 0.5, 1e-9));
        CHECK("earIpdFactor=2.0", near(c.earIpdFactor, 2.0, 1e-9));
        CHECK("earWidthMm=130.0", near(c.earWidthMm, 130.0, 1e-9));
        CHECK("chinConfWeight=0.3", near(c.chinConfWeight, 0.3, 1e-9));
        CHECK("minContribPts=3", c.minContribPts == 3);
        CHECK("addrMinFrames=8", c.addrMinFrames == 8);
        CHECK("addrWindowUs=500000", c.addrWindowUs == 500000);

        const HeadTrackConfig def = HeadTrackConfig::fromOverrides(QVariantMap{});
        CHECK("default confMin==0.30", near(def.confMin, 0.30, 1e-9));
        CHECK("default earIpdFactor==1.8", near(def.earIpdFactor, 1.8, 1e-9));
        CHECK("default earWidthMm==145.0", near(def.earWidthMm, 145.0, 1e-9));
        CHECK("default chinConfWeight==0.0 (body-only)", near(def.chinConfWeight, 0.0, 1e-9));
    }

    // ── 6) Degenerate inputs degrade gracefully ────────────────────────────────
    {
        std::printf("=== 6) degenerate inputs ===\n");
        PoseTrack2D empty;
        const HeadTrackResult r0 = trackHead(empty, 1000, 1000, -1, {});
        CHECK("empty track ⇒ not valid", !r0.valid);
        CHECK("empty track ⇒ no series", buildHeadSeries(r0, {}).empty());

        // All keypoints below the gate ⇒ no head anywhere.
        std::vector<PoseFrame2D> lowConf;
        for (int k = 0; k < 5; ++k)
            lowConf.push_back(makeHead(k * 10000, 0.5, 0.5, 0.02, 0.036, 0.0, 0.1f, 0.1f, 0.1f));
        PoseTrack2D lp; lp.frames = lowConf;
        const HeadTrackResult r1 = trackHead(lp, 1000, 1000, -1, {});
        CHECK("all-low-conf ⇒ not valid", !r1.valid);

        const HeadTrackResult r2 = trackHead(empty, 0, 0, -1, {});   // bad dims
        CHECK("zero frame dims ⇒ not valid", !r2.valid);
    }

    // ── 7) mm scaling via the inter-ear head-plane ruler ───────────────────────
    // pxPerMm = addrScalePx / earWidthMm converts the ×frame displacement to mm;
    // the caller derives it exactly as wrist_analyzer does. Square 1000×1000 so a
    // normalized x move maps straight to px with no anisotropy.
    {
        std::printf("=== 7) mm scaling (inter-ear head-plane ruler) ===\n");
        const int W = 1000, H = 1000;
        const int64_t dt = 10000;
        const double cx0 = 0.5, cy0 = 0.5, eyeL = 0.05;   // inter-ear = 2·1.8·0.05·1000 = 180 px
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k <= 5; ++k)                       // still address hold
            frames.push_back(makeHead(k * dt, cx0, cy0, eyeL, 1.8 * eyeL, 0.0,
                                      0.9f, 0.9f, 0.9f, 0.0, k));
        for (int s = 1; s <= 10; ++s)                      // sway ramp to +0.05 (= 50 px)
            frames.push_back(makeHead((5 + s) * dt, cx0 + s * 0.005, cy0, eyeL,
                                      1.8 * eyeL, 0.0, 0.9f, 0.9f, 0.9f, 0.0, 5 + s));
        PoseTrack2D pose; pose.frames = frames;

        HeadTrackConfig cfg; cfg.addrWindowUs = 30000;     // hold frames only
        const HeadTrackResult res = trackHead(pose, W, H, /*addressUs=*/2 * dt, cfg);
        CHECK("addr inter-ear scale ~ 180 px", near(res.addrScalePx, 180.0, 2.0));

        const double pxPerMm = res.addrScalePx / cfg.earWidthMm;   // = 180 / 145
        const std::vector<MetricSeries> series = buildHeadSeries(res, {}, pxPerMm);
        const MetricSeries *sway = findSeries(series, "headSway");
        CHECK("headSway emitted", sway != nullptr);
        if (sway) {
            CHECK("sway unit == cm", sway->unit == QStringLiteral("cm"));
            const int64_t t = (5 + 10) * dt;
            const double swayPx = 10 * 0.005 * W;                  // 50 px
            const double expCm  = swayPx / pxPerMm / 10.0;         // = 50·145/180/10 ≈ 4.03 cm
            CHECK("sway ~ expected cm", near(valueAt(*sway, t), expCm, 0.05));
            // Worth pinning as a number rather than only as a formula: ~4 cm is the middle of the
            // shipped corridor for this measure, which is what makes the old mm reading (40) an
            // Action on every swing rather than a merely-imprecise one.
            CHECK("…and it lands where the corridor expects, ~4 cm",
                  valueAt(*sway, t) > 3.5 && valueAt(*sway, t) < 4.5);
        }
        // No scale ⇒ ABSENT. There is no fallback unit any more.
        const std::vector<MetricSeries> raw = buildHeadSeries(res, {}, -1.0);
        CHECK("no-scale ⇒ headSway absent, not rescaled",
              findSeries(raw, "headSway") == nullptr);
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
