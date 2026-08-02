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

// Standalone test for the lower-body frontal-plane metrics
// (src/Analysis/lower_body_metrics.{h,cpp}). Synthetic tracks only — no fixture.
// Mirrors head_track_test.cpp / foot_metrics_test.cpp in structure and style.
//
// The case that carries the design is §2: a pelvis that ROTATES carries the lead
// hip and the lead knee toward the trail side together, and the metric must read
// ~zero for it, while a lead knee that goes in ON ITS OWN must read the full
// deflection. Those two are the same picture in a face-on projection and the
// whole reason the channel is a difference rather than a position.

#include "../lower_body_metrics.h"

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

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// COCO-17 body indices (mirrors lower_body_metrics.cpp's local constants).
constexpr int kLHip = 11, kRHip = 12, kLKnee = 13, kRKnee = 14, kLAnkle = 15, kRAnkle = 16;

// One lower-body frame. Every point is normalized; the caller places them.
struct Lower {
    QPointF lHip, rHip, lKnee, rKnee, lAnkle, rAnkle;
};

static PoseFrame2D makeLower(int64_t t, const Lower &p, float conf = 0.9f)
{
    PoseFrame2D f;
    f.t_us = t;
    f.kp[kLHip]   = p.lHip;   f.conf[kLHip]   = conf;
    f.kp[kRHip]   = p.rHip;   f.conf[kRHip]   = conf;
    f.kp[kLKnee]  = p.lKnee;  f.conf[kLKnee]  = conf;
    f.kp[kRKnee]  = p.rKnee;  f.conf[kRKnee]  = conf;
    f.kp[kLAnkle] = p.lAnkle; f.conf[kLAnkle] = conf;
    f.kp[kRAnkle] = p.rAnkle; f.conf[kRAnkle] = conf;
    return f;
}

// A square, level address stance. Lead = LEFT keypoints, and the lead ankle sits at the SMALLER x,
// so the lead side is image −x and every lateral channel must come back sign-flipped.
static Lower addressPose()
{
    Lower p;
    p.lHip   = QPointF(0.46, 0.50);
    p.rHip   = QPointF(0.54, 0.50);
    p.lKnee  = QPointF(0.45, 0.70);
    p.rKnee  = QPointF(0.55, 0.70);
    p.lAnkle = QPointF(0.44, 0.90);
    p.rAnkle = QPointF(0.56, 0.90);
    return p;
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
    // The stance span is |0.56 − 0.44| = 0.12 of frame width. On a 1000-wide frame that is 120 px,
    // so one percent of stance width is 1.2 px, and a 0.012-normalized move is 10 %.
    const int W = 1000, H = 1000;
    const int64_t dt = 10000;
    const double spanNorm = 0.12;

    // ── 1) Address reference, denominator and sign ──────────────────────────
    {
        std::printf("=== 1) address reference, span, lead sign ===\n");
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        PoseTrack2D pose; pose.frames = frames;

        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;
        const LowerBodyResult res = trackLowerBody(pose, W, H, /*leadIsLeft=*/true, 2 * dt, cfg);
        CHECK("address reference resolved", res.valid);
        CHECK("one state per frame", res.states.size() == frames.size());
        CHECK("span is ankle-to-ankle at address", near(res.addrSpanPx, spanNorm * W, 0.5));
        // The whole reason this is resolved rather than assumed: a mirrored camera or a left-handed
        // golfer puts the lead side on the other end of the x axis, and a hard-coded sign would
        // then report every sway and every knee drift backwards while looking entirely healthy.
        CHECK("lead side is image −x here, so leadSign is −1", near(res.leadSign, -1.0, 1e-12));

        // The same stance with the lead on the other side must flip it, and nothing else.
        const LowerBodyResult mirrored =
            trackLowerBody(pose, W, H, /*leadIsLeft=*/false, 2 * dt, cfg);
        CHECK("lead on the other side flips the sign", near(mirrored.leadSign, 1.0, 1e-12));
        CHECK("…and does not change the denominator",
              near(mirrored.addrSpanPx, res.addrSpanPx, 1e-9));
    }

    // ── 2) THE design case: rotation vs collapse ────────────────────────────
    //
    // Face-on, these two are the same picture. Under a genuine turn the pelvis carries the lead hip
    // AND the lead knee toward the trail side by similar amounts; under the compensation the pelvis
    // has not moved and the knee goes in alone. A detector on raw knee position cannot tell them
    // apart, which is why the channel subtracts the hip.
    {
        std::printf("=== 2) pelvic rotation vs lead-knee collapse ===\n");
        const double move = 0.012;         // 10 % of stance width

        // (a) ROTATION: hip and knee both travel trailward (+x here) by the same amount.
        {
            std::vector<PoseFrame2D> frames;
            for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
            for (int s = 1; s <= 5; ++s) {
                Lower p = addressPose();
                const double d = move * s / 5.0;
                p.lHip  = QPointF(p.lHip.x()  + d, p.lHip.y());
                p.lKnee = QPointF(p.lKnee.x() + d, p.lKnee.y());
                frames.push_back(makeLower((5 + s) * dt, p));
            }
            PoseTrack2D pose; pose.frames = frames;
            LowerBodyConfig cfg; cfg.addrWindowUs = 30000;
            const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg);
            const auto series = buildLowerBodySeries(res, {});
            const MetricSeries *kd = findSeries(series, "leadKneeDrift");
            CHECK("leadKneeDrift emitted", kd != nullptr);
            if (kd)
                CHECK("a pure pelvic turn reads ~0 — the projection term cancels",
                      near(valueAt(*kd, 10 * dt), 0.0, 0.3));
        }

        // (b) COLLAPSE: the hip stays put and the knee goes in alone, by the same 10 %.
        {
            std::vector<PoseFrame2D> frames;
            for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
            for (int s = 1; s <= 5; ++s) {
                Lower p = addressPose();
                p.lKnee = QPointF(p.lKnee.x() + move * s / 5.0, p.lKnee.y());
                frames.push_back(makeLower((5 + s) * dt, p));
            }
            PoseTrack2D pose; pose.frames = frames;
            LowerBodyConfig cfg; cfg.addrWindowUs = 30000;
            const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg);
            const auto series = buildLowerBodySeries(res, {});
            const MetricSeries *kd = findSeries(series, "leadKneeDrift");
            CHECK("leadKneeDrift emitted", kd != nullptr);
            if (kd) {
                // Toward the trail side, and the lead side is −x, so the fault reads NEGATIVE.
                CHECK("a knee going in alone reads the FULL deflection, negative",
                      near(valueAt(*kd, 10 * dt), -10.0, 0.3));
            }
        }
    }

    // ── 3) Pelvis sway, lift and the hip line ──────────────────────────────
    {
        std::printf("=== 3) sway (lead-positive), lift (up-positive), hip tilt ===\n");
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        for (int s = 1; s <= 5; ++s) {
            Lower p = addressPose();
            const double f = double(s) / 5.0;
            // Away from the lead side (+x), and up (−y in image space).
            p.lHip = QPointF(p.lHip.x() + 0.012 * f, p.lHip.y() - 0.006 * f);
            p.rHip = QPointF(p.rHip.x() + 0.012 * f, p.rHip.y() - 0.018 * f);
            frames.push_back(makeLower((5 + s) * dt, p));
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;
        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg);
        const auto series = buildLowerBodySeries(res, {});

        const MetricSeries *sway = findSeries(series, "pelvisSway");
        const MetricSeries *lift = findSeries(series, "pelvisLift");
        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        CHECK("pelvisSway emitted",  sway != nullptr);
        CHECK("pelvisLift emitted",  lift != nullptr);
        CHECK("hipLineTilt emitted", tilt != nullptr);
        CHECK("units: displacements are % stance width",
              sway && sway->unit == QStringLiteral("% stance width")
                   && lift && lift->unit == QStringLiteral("% stance width"));
        CHECK("units: the tilt is degrees", tilt && tilt->unit == QStringLiteral("°"));

        if (sway)
            CHECK("swaying AWAY from the lead side is negative (rule 2)",
                  near(valueAt(*sway, 10 * dt), -10.0, 0.3));
        if (lift) {
            // Centre rise = mean of 0.006 and 0.018 = 0.012 normalized = 10 % of the span.
            CHECK("a rising pelvis centre is positive", near(valueAt(*lift, 10 * dt), 10.0, 0.3));
        }
        if (tilt) {
            // At address the hips are level, so the tilt starts at zero — this is an ABSOLUTE
            // angle, and a nonzero value at address would be a real reading, not a calibration.
            CHECK("level hips at address read 0°", near(valueAt(*tilt, 2 * dt), 0.0, 0.05));
            // The trail hip rose 0.018 and the lead hip 0.006, so the trail sits 0.012 higher over
            // a 0.08 horizontal separation: atan(0.012/0.08) ≈ 8.53°, POSITIVE for trail-above.
            const double expect = std::atan2(0.012, 0.08) * 57.29577951308232;
            CHECK("the trail hip riding above the lead reads POSITIVE",
                  near(valueAt(*tilt, 10 * dt), expect, 0.2));
        }
    }

    // ── 4) A legacy 17-kp track still produces everything ──────────────────
    //
    // The reason this module exists separately from foot_metrics. Those keypoints (17–22) are
    // WholeBody-only and a pre-WB0 track has them all at zero confidence, so the feet emit nothing
    // at all; 11–16 are COCO body and are present either way.
    {
        std::printf("=== 4) legacy 17-kp track: full output ===\n");
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) {
            PoseFrame2D f = makeLower(k * dt, addressPose());
            for (int i = 17; i < int(f.conf.size()); ++i) f.conf[i] = 0.f;   // the WholeBody tail
            frames.push_back(f);
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;
        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg);
        CHECK("resolved with no WholeBody keypoints at all", res.valid);
        // SIX now, not four: feetAlignment and comOverLeadFoot joined the module, and they read the
        // same COCO body 11–16 as everything else here — which is the point of asserting it on a
        // legacy track. A swing recorded before the WholeBody tail existed gets the full set.
        CHECK("all six channels present", buildLowerBodySeries(res, {}).size() == 6);
    }

    // ── 5) Low-confidence gap coasts without NaN ───────────────────────────
    {
        std::printf("=== 5) low-conf gap bridged, no NaN ===\n");
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 30; ++k) {
            const bool gap = (k >= 12 && k < 18);
            Lower p = addressPose();
            p.lKnee = QPointF(p.lKnee.x() + 0.0004 * k, p.lKnee.y());
            frames.push_back(makeLower(k * dt, p, gap ? 0.10f : 0.9f));
        }
        PoseTrack2D pose; pose.frames = frames;
        const LowerBodyResult res = trackLowerBody(pose, W, H, true, -1, {});   // fallback ref
        CHECK("resolved via the first-N fallback", res.valid);

        const auto series = buildLowerBodySeries(res, {});
        const MetricSeries *kd = findSeries(series, "leadKneeDrift");
        CHECK("leadKneeDrift emitted", kd != nullptr);
        if (kd) {
            CHECK("resampled grid covers all 30 frames", kd->t_us.size() == 30);
            bool allFinite = true;
            for (double v : kd->value) allFinite = allFinite && std::isfinite(v);
            for (const PhaseSample &ps : kd->phaseSamples)
                allFinite = allFinite && std::isfinite(ps.value);
            CHECK("no NaN anywhere in the series", allFinite);
            const double lo = valueAt(*kd, 11 * dt), hi = valueAt(*kd, 18 * dt);
            const double mid = valueAt(*kd, 15 * dt);
            CHECK("gap value linearly bridged", mid < lo && mid > hi && std::isfinite(mid));
        }
    }

    // ── 6) Degenerate inputs refuse rather than fabricate ──────────────────
    {
        std::printf("=== 6) degenerate inputs ===\n");
        CHECK("empty track ⇒ invalid",
              !trackLowerBody(PoseTrack2D{}, W, H, true, -1, {}).valid);
        CHECK("empty track ⇒ no series",
              buildLowerBodySeries(trackLowerBody(PoseTrack2D{}, W, H, true, -1, {}), {}).empty());

        std::vector<PoseFrame2D> zeroConf;
        for (int k = 0; k < 6; ++k) zeroConf.push_back(makeLower(k * dt, addressPose(), 0.05f));
        PoseTrack2D dark; dark.frames = zeroConf;
        CHECK("nothing confident anywhere ⇒ invalid",
              !trackLowerBody(dark, W, H, true, -1, {}).valid);

        // The denominator floor. A stance this narrow is a mis-detection, and dividing by it would
        // turn a few pixels of keypoint noise into hundreds of percent of "sway" — a confident
        // absurdity where the honest answer is that nothing was measured.
        std::vector<PoseFrame2D> narrow;
        for (int k = 0; k < 6; ++k) {
            Lower p = addressPose();
            p.lAnkle = QPointF(0.499, 0.90);
            p.rAnkle = QPointF(0.501, 0.90);      // 2 px apart on a 1000-wide frame
            narrow.push_back(makeLower(k * dt, p));
        }
        PoseTrack2D thin; thin.frames = narrow;
        const LowerBodyResult res = trackLowerBody(thin, W, H, true, -1, {});
        CHECK("a stance below the span floor ⇒ invalid, not a huge percentage", !res.valid);
        CHECK("…and emits nothing", buildLowerBodySeries(res, {}).empty());

        CHECK("zero frame size ⇒ invalid", !trackLowerBody(dark, 0, 0, true, -1, {}).valid);
    }

    // ── 6b) feetAlignment and comOverLeadFoot ──────────────────────────────
    //
    // Both arrived with the face-on producer batch. feetAlignment is the ANKLE line, and it exists
    // beside foot_metrics' toeLineAngle for two reasons: the ankles are far less affected by foot
    // flare than the toes, and the impact read has no counterpart in an address-only scalar. It
    // also uses the ABSOLUTE-denominator line form, so — unlike toeLineAngle, which is a raw atan2
    // of the lead→trail vector — its sign describes the same posture whichever way the camera was
    // pointed. That is the property asserted here.
    {
        std::printf("=== 6b) feet alignment + balance ===\n");
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        // Trail (RIGHT keypoints, the larger x) ankle set further from the camera: on level ground
        // that is HIGHER in the image, which is a closed stance and must read POSITIVE.
        for (int k = 6; k < 12; ++k) {
            Lower p = addressPose();
            p.rAnkle = QPointF(0.56, 0.87);
            frames.push_back(makeLower(k * dt, p));
        }
        PoseTrack2D track; track.frames = frames;
        const LowerBodyResult res = trackLowerBody(track, W, H, true, 2 * dt, {});
        const auto series = buildLowerBodySeries(res, {});

        const MetricSeries *fa = findSeries(series, "feetAlignment");
        CHECK("feetAlignment emitted", fa != nullptr);
        if (fa) {
            CHECK("a level stance reads 0°", near(valueAt(*fa, 2 * dt), 0.0, 0.05));
            CHECK("the trail ankle sitting higher is POSITIVE", valueAt(*fa, 10 * dt) > 5.0);
        }

        const MetricSeries *com = findSeries(series, "comOverLeadFoot");
        CHECK("comOverLeadFoot emitted", com != nullptr);
        if (com) {
            // Address: pelvis centre at x 0.50, lead ankle at 0.44, span 0.12 ⇒ 0.06/0.12 = 50 %.
            CHECK("a centred pelvis is half a stance from the lead ankle",
                  near(valueAt(*com, 2 * dt), 50.0, 2.0));
            CHECK("it is UNSIGNED — a distance, not a displacement",
                  valueAt(*com, 2 * dt) > 0.0 && valueAt(*com, 10 * dt) > 0.0);
            // It is the only channel read at the finish, so it is the only one that samples there.
            bool hasFinish = false;
            for (const PhaseSample &s : com->phaseSamples)
                if (s.phase == Phase::Finish) hasFinish = true;
            CHECK("comOverLeadFoot samples the FINISH, which is where it is read", hasFinish);
        }

        // The four original channels keep the original three-phase list, so their serialized
        // phaseSamples are byte-identical to what they were before this batch and no corpus gate
        // has to be re-run to prove the change was additive.
        for (const char *k : { "leadKneeDrift", "pelvisSway", "pelvisLift", "hipLineTilt" }) {
            const MetricSeries *m = findSeries(series, k);
            CHECK(k, m && m->phaseSamples.size() == 3);
        }
    }

    // ── 7) fromOverrides ───────────────────────────────────────────────────
    {
        std::printf("=== 7) fromOverrides ===\n");
        QVariantMap ov;
        ov.insert(QStringLiteral("lowerBody.confMin"), 0.75);
        ov.insert(QStringLiteral("lowerBody.addrMinFrames"), 9);
        ov.insert(QStringLiteral("lowerBody.addrWindowUs"), 12345);
        ov.insert(QStringLiteral("lowerBody.minStanceSpanPx"), 99.0);
        const LowerBodyConfig c = LowerBodyConfig::fromOverrides(ov);
        CHECK("confMin swept",         near(c.confMin, 0.75, 1e-12));
        CHECK("addrMinFrames swept",   c.addrMinFrames == 9);
        CHECK("addrWindowUs swept",    c.addrWindowUs == 12345);
        CHECK("minStanceSpanPx swept", near(c.minStanceSpanPx, 99.0, 1e-12));

        const LowerBodyConfig d = LowerBodyConfig::fromOverrides({});
        CHECK("empty overrides leave the frozen defaults",
              near(d.confMin, pinpoint::tuned::lowerBody::kConfMin, 1e-12)
                  && d.addrMinFrames == pinpoint::tuned::lowerBody::kAddrMinFrames);
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
