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
#include "../metric_channel.h"   // channelValidityMask / nearestIndex — §6d and §6e test them directly

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>
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

// ── §8's fixtures: a held address with ROUND numbers, and a smoother record to go with it ──
//
// Hips 100 px apart and level; ankles 200 px apart and level, on a 1000 px frame. Every σ §8 asserts
// is then a closed form a reader can check by hand, which is the whole point of the section: a σ that
// only agrees with the code that produced it has been tested against nothing.
static Lower sigmaPose()
{
    Lower p;
    p.lHip   = QPointF(0.45, 0.50);   p.rHip   = QPointF(0.55, 0.50);   // hip line 100 px, level
    p.lKnee  = QPointF(0.44, 0.70);   p.rKnee  = QPointF(0.56, 0.70);
    p.lAnkle = QPointF(0.40, 0.90);   p.rAnkle = QPointF(0.60, 0.90);   // stance 200 px, level
    return p;
}

// The same address with the TRAIL HIP 30 px LOWER — 16.7° of hip tilt on a 100 px line.
//
// §8a's fixture is level everywhere and a vertical spine, which leaves two things in the propagation
// completely untested: `lineTiltSigmaDeg` divides by the line's EUCLIDEAN length, and on a level line
// that is numerically identical to the |Δx| the tilt itself divides by. A mutation replacing L with
// |Δx| passes every level case. Here it does not: L = sqrt(100² + 30²) = 104.4031 px, so the two
// readings differ by 1/cos(16.7°) = 4.40 %.
//
// Only the HIP line is tilted. The ankles stay level so feetAlignment is an unchanged control in the
// same run, which is what shows the σ is computed per LINE from that line's own length rather than
// once per swing.
static Lower sigmaPoseTilted()
{
    Lower p = sigmaPose();
    p.rHip = QPointF(0.55, 0.53);      // trail hip (lead is LEFT here) 30 px lower
    return p;
}

// The smoother's per-keypoint honesty record for one frame, with a CONSTANT σ on every joint. Tier
// Meas because that is what a smoothed keypoint inside a confirmed run carries; nothing in the
// producer reads `tier` (it reads sigma > 0), and that is deliberate — see the note in §8b.
static PoseKpAux auxWith(double sigmaPx)
{
    PoseKpAux a;
    for (int k = 0; k < kWholeBodyJoints; ++k) {
        a.tier[size_t(k)]  = uint8_t(PoseTier::Meas);
        a.sigma[size_t(k)] = float(sigmaPx);
    }
    return a;
}

// A track whose SMOOTHED companion is byte-identical to its raw frames.
//
// That identity is load-bearing, not laziness: it makes every emitted value the same number with and
// without `smoothedAux`, which is exactly what lets §8e assert that adding σ moves nothing. `sigmas`
// is one entry per frame; EMPTY means no aux at all, i.e. a swing analysed before the smoother
// existed.
static PoseTrack2D sigmaTrack(const std::vector<Lower> &poses, const std::vector<double> &sigmas,
                              int64_t dtUs)
{
    PoseTrack2D t;
    for (size_t i = 0; i < poses.size(); ++i) {
        const PoseFrame2D f = makeLower(int64_t(i) * dtUs, poses[i]);
        t.frames.push_back(f);
        t.smoothed.push_back(f);
    }
    for (double sg : sigmas)
        t.smoothedAux.push_back(auxWith(sg));
    return t;
}

// A per-keypoint σ record with a DISTINCT value on every joint the module reads.
//
// §8a gives every joint the same σ, which pins no COEFFICIENT: swap the lead and trail ankle in
// comOverLeadFoot, or read the trail hip where leadKneeDrift wants the lead one, and a constant σ
// hides it completely. σ is attached to the PHYSICAL keypoint here — left is left whichever side is
// lead — so running the same fixture with `leadIsLeft` both ways is the test that each point is paired
// with its own side's σ.
static PoseKpAux auxPerJoint(double lHip, double rHip, double lKnee, double rKnee,
                             double lAnk, double rAnk)
{
    PoseKpAux a;
    for (int k = 0; k < kWholeBodyJoints; ++k)
        a.tier[size_t(k)] = uint8_t(PoseTier::Meas);
    a.sigma[size_t(kLHip)]   = float(lHip);   a.sigma[size_t(kRHip)]   = float(rHip);
    a.sigma[size_t(kLKnee)]  = float(lKnee);  a.sigma[size_t(kRKnee)]  = float(rKnee);
    a.sigma[size_t(kLAnkle)] = float(lAnk);   a.sigma[size_t(kRAnkle)] = float(rAnk);
    return a;                                  // every other joint stays 0 — this module reads none
}

// A held track carrying one prebuilt aux record on every frame.
static PoseTrack2D trackWithAux(const std::vector<Lower> &poses, const PoseKpAux &aux, int64_t dtUs)
{
    PoseTrack2D t;
    for (size_t i = 0; i < poses.size(); ++i) {
        const PoseFrame2D f = makeLower(int64_t(i) * dtUs, poses[i]);
        t.frames.push_back(f);
        t.smoothed.push_back(f);
        t.smoothedAux.push_back(aux);
    }
    return t;
}

// Relative agreement. C11 asks for 5 %; every case below passes at 1e-5 because BOTH sides are
// closed forms — the expected value is written out from the fixture's own geometry, never read back
// out of the producer.
static bool nearRel(double got, double want, double rel)
{
    return std::fabs(got - want) <= rel * std::fabs(want);
}

static const MetricSeries *findSeries(const std::vector<MetricSeries> &v, const char *key)
{
    for (const MetricSeries &m : v)
        if (m.key == QLatin1String(key)) return &m;
    return nullptr;
}

// A phase ladder. Every sample-emitting call in this file passes one, because AN UNSEGMENTED PHASE
// NOW EMITS NO SAMPLE — the producer used to coast to the first frame, which put a first-frame
// reading under an unsegmented phase's label. That was invisible while the sample list was
// Address/Top/Impact (a successful segmentation always has those three) and is a fabricated number
// the moment P2/P3/P5/P6 are asked for, which hipLineTilt and plumbBobDistance now do.
static std::vector<PhaseEvent> ladder(std::initializer_list<std::pair<Phase, int64_t>> at)
{
    std::vector<PhaseEvent> out;
    for (const auto &pr : at) {
        PhaseEvent e;
        e.phase = pr.first;
        e.t_us  = pr.second;
        out.push_back(e);
    }
    return out;
}

static bool hasPhase(const MetricSeries &m, Phase p)
{
    for (const PhaseSample &s : m.phaseSamples)
        if (s.phase == p) return true;
    return false;
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
        const auto phases = ladder({ { Phase::Address, 2 * dt },
                                     { Phase::Top,     6 * dt },
                                     { Phase::Impact,  9 * dt },
                                     { Phase::Finish, 11 * dt } });
        const auto series = buildLowerBodySeries(res, phases);

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

        // The three channels that predate the P-ladder work keep the original three-phase list, so
        // their serialized phaseSamples are byte-identical to what they were before and no corpus
        // gate has to be re-run to prove the change was additive. hipLineTilt is DELIBERATELY not
        // in this list any more — it moved to P1–P7, which is the point of that change.
        for (const char *k : { "leadKneeDrift", "pelvisSway", "pelvisLift" }) {
            const MetricSeries *m = findSeries(series, k);
            CHECK(k, m && m->phaseSamples.size() == 3);
        }
        {
            // P2/P3/P5/P6 are absent from this ladder, so hipLineTilt gets exactly the three that
            // ARE on it. The old behaviour would have put four more samples at the first frame.
            const MetricSeries *tilt = findSeries(series, "hipLineTilt");
            CHECK("hipLineTilt samples only the phases the ladder actually has",
                  tilt && tilt->phaseSamples.size() == 3);
            CHECK("…and they are P1/P4/P7, not a frame-0 P5 or P6",
                  tilt && hasPhase(*tilt, Phase::Address) && hasPhase(*tilt, Phase::Top)
                       && hasPhase(*tilt, Phase::Impact)
                       && !hasPhase(*tilt, Phase::ArmParallelDown)
                       && !hasPhase(*tilt, Phase::Delivery));
        }
    }

    // ── 6c) plumbBobDistance ───────────────────────────────────────────────
    //
    // The signed twin of comOverLeadFoot about the stance CENTRE, in inches off the ball ruler.
    // Three properties carry it: the sign is lead-positive without a leadSign term (the projection
    // runs trail ankle -> lead ankle, so a mirrored camera cannot invert it), the scale is the
    // ruler's and nothing else, and it is ABSENT rather than rescaled when the ruler does not
    // resolve.
    {
        std::printf("=== 6c) plumb bob ===\n");

        // 1 px = 1 mm makes the arithmetic legible: an offset of n px is n/25.4 inches.
        const double mmPerPx = 1.0;

        // Address, then the hips slid 0.012 of frame width (12 px) toward the LEAD side, which is
        // −x here. Ankles stay put, so the stance centre does not move.
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        for (int s = 1; s <= 5; ++s) {
            Lower p = addressPose();
            const double d = 0.012 * s / 5.0;
            p.lHip = QPointF(p.lHip.x() - d, p.lHip.y());
            p.rHip = QPointF(p.rHip.x() - d, p.rHip.y());
            frames.push_back(makeLower((5 + s) * dt, p));
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;

        const auto phases = ladder({ { Phase::Address,           2 * dt },
                                     { Phase::ShaftParallelBack, 4 * dt },
                                     { Phase::MidBackswing,      5 * dt },
                                     { Phase::Top,               6 * dt },
                                     { Phase::ArmParallelDown,   7 * dt },
                                     { Phase::Delivery,          8 * dt },
                                     { Phase::Impact,           10 * dt } });

        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg, mmPerPx);
        const auto series = buildLowerBodySeries(res, phases);
        const MetricSeries *pb = findSeries(series, "plumbBobDistance");
        CHECK("plumbBobDistance emitted when the ruler resolves", pb != nullptr);
        if (pb) {
            CHECK("its unit is inches", pb->unit == QStringLiteral("in"));
            // Address: the hip centre (0.50) sits on the stance centre (0.50) — a true plumb line.
            CHECK("hips over the stance centre read 0", near(valueAt(*pb, 2 * dt), 0.0, 0.02));
            // Moved 12 px toward the lead side ⇒ +12 mm ⇒ +12/25.4 in, POSITIVE for lead-ward even
            // though the lead side is image −x. That is the whole point of projecting along the
            // trail->lead stance line rather than taking a raw Δx.
            CHECK("hips ahead of centre, toward the LEAD side, read POSITIVE",
                  near(valueAt(*pb, 10 * dt), 12.0 / 25.4, 0.02));
            CHECK("the full P1–P7 ladder is sampled", pb->phaseSamples.size() == 7);
        }

        // The same swing seen with the lead on the other side must produce the same MAGNITUDE with
        // the opposite sign — the hips are now behind centre for that golfer.
        const LowerBodyResult mirrored = trackLowerBody(pose, W, H, false, 2 * dt, cfg, mmPerPx);
        const auto mirroredSeries = buildLowerBodySeries(mirrored, phases);
        const MetricSeries *pbm = findSeries(mirroredSeries, "plumbBobDistance");
        CHECK("mirroring the handedness flips the sign and nothing else",
              pbm && near(valueAt(*pbm, 10 * dt), -12.0 / 25.4, 0.02));

        // ⚠ ABSENT, NEVER RESCALED. A metric whose unit changes per swing cannot carry a norm: the
        // norm declares one unit and grading compares the numbers without consulting it, so a
        // fallback scale would be graded against inches in silence.
        const LowerBodyResult noRuler = trackLowerBody(pose, W, H, true, 2 * dt, cfg);
        const auto without = buildLowerBodySeries(noRuler, phases);
        CHECK("no ball ruler ⇒ no plumb bob at all",
              findSeries(without, "plumbBobDistance") == nullptr);
        CHECK("…and every other channel is unaffected", without.size() == 6);
    }

    // ── 6d) The hip LINE's foreshortening gate ─────────────────────────────
    //
    // THE CASE THIS SECTION CARRIES, and the reason the gate exists at all: `lineTiltDeg` divides by
    // the hips' horizontal separation, so a pelvis turning toward the target collapses the two hips
    // into the same image column and the angle runs to ±90° while the posture it claims to describe
    // has not changed. A real review chart shows −88° of "hip line tilt" just after impact. That is a
    // reading of the camera, and the honest answer is that the frame has NO hip line.
    //
    // The track below rotates the hips about the vertical from square to 80° over 20 frames, with the
    // trail hip rising as it goes so there IS a genuine tilt to lose. |dx| / addrHipSpan is exactly
    // cos θ, so the 0.40 gate bites at 66.4° — frames 16..19 of the rotation, grid indices 22..25.
    {
        std::printf("=== 6d) hip line foreshortening gate ===\n");
        const double kPi     = 3.14159265358979323846;
        const double hipHalf = 0.04;      // ±40 px about the stance centre ⇒ an 80 px address span
        const int    kAddr   = 6;         // held address frames, for the robust reference
        const int    kTurn   = 20;

        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < kAddr; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        for (int k = 0; k < kTurn; ++k) {
            const double th   = 80.0 * k / double(kTurn - 1);
            const double c    = std::cos(th * kPi / 180.0);
            const double rise = 0.020 * k / double(kTurn - 1);   // trail hip up to 20 px above lead
            Lower p = addressPose();
            p.lHip = QPointF(0.50 - hipHalf * c, 0.50);
            p.rHip = QPointF(0.50 + hipHalf * c, 0.50 - rise);
            frames.push_back(makeLower((kAddr + k) * dt, p));
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;

        // Top lands on a frame the gate PASSES (θ = 58.9°) and Impact inside the run it REFUSES
        // (θ = 75.8°) — the pair is the point: one sample survives and the other must not exist.
        const int64_t topUs = 20 * dt, impactUs = 24 * dt;
        const auto phases = ladder({ { Phase::Address, 2 * dt },
                                     { Phase::Top,     topUs },
                                     { Phase::Impact,  impactUs } });

        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg);
        CHECK("the hip line's ADDRESS span is measured, |dx| not a Euclidean length",
              near(res.addrHipSpanPx, hipHalf * 2.0 * W, 0.5));
        CHECK("the last four turned frames have no hip line",
              res.states.size() == size_t(kAddr + kTurn)
                  && res.states[21].hipLineValid && !res.states[22].hipLineValid
                  && !res.states[25].hipLineValid);
        CHECK("…and the channel simply does not carry them", res.hipTilt.t_us.size() == 22);

        const auto series = buildLowerBodySeries(res, phases);
        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        CHECK("hipLineTilt still emitted — a gated run is not a refused metric", tilt != nullptr);
        if (tilt) {
            // The mask, not the value, is where the absence lives: the curve stays continuous for
            // the renderer and says which of itself is a measurement.
            CHECK("a validity mask is present and parallel to t_us",
                  tilt->valid.size() == tilt->t_us.size());
            // Two rules land on the same tail here: the gate refuses 22..25, and the phase domain
            // marks everything past the Impact sample (index 24) — so 25 is 0 twice over and the
            // expectation is unchanged from before the domain existed. The domain's HEAD is open, so
            // indices 0 and 1 stay valid (§6g).
            bool maskExact = tilt->valid.size() == 26;
            for (size_t i = 0; i < tilt->valid.size(); ++i)
                maskExact = maskExact && (tilt->valid[i] == (i >= 22 ? 0u : 1u));
            CHECK("0 across the bridged run, 1 everywhere else", maskExact);

            // What the gate BOUGHT. Ungated, the last frame reads atan2(20 px, 13.9 px) ≈ 55° and is
            // heading for 90°; the last real measurement is 23.6°, and that is what the bridge holds.
            const double ungatedLast = std::atan2(20.0, 2.0 * hipHalf * W * std::cos(80.0 * kPi / 180.0))
                                       * 57.29577951308232;
            CHECK("the ungated angle really would have run away", ungatedLast > 50.0);
            double worst = 0.0;
            for (double v : tilt->value) worst = std::max(worst, std::fabs(v));
            CHECK("nothing in the series approaches ±90°", worst < 30.0);
            CHECK("the bridge HOLDS the last measurement rather than reporting the collapse",
                  near(valueAt(*tilt, 25 * dt), valueAt(*tilt, 21 * dt), 1e-9));

            CHECK("a phase instant on a valid frame still samples", hasPhase(*tilt, Phase::Top));
            CHECK("A PHASE INSTANT INSIDE THE INVALID RUN EMITS NO SAMPLE",
                  !hasPhase(*tilt, Phase::Impact));
        }

        // The ungated channels are untouched: sway, lift and knee drift are POSITIONS, which the same
        // turn distorts by projection but does not divide by a vanishing span. That is a phase-domain
        // question answered one layer up, not a validity question answered here.
        // (pelvisSway carries a mask all the same — the phase domain's ONE zero at index 25, past the
        // Impact sample. What it must NOT carry is a zero anywhere the hip line was refused.)
        const MetricSeries *sway = findSeries(series, "pelvisSway");
        bool swayUngated = sway && sway->valid.size() == 26 && sway->valid[25] == 0u;
        if (sway)
            for (size_t i = 0; i <= 24; ++i)
                swayUngated = swayUngated && sway->valid[i] == 1u;
        CHECK("pelvisSway is not gated by the hip line — only its post-Impact tail is marked",
              swayUngated && hasPhase(*sway, Phase::Impact));

        // The invariant every consumer relies on, asserted over the whole emission.
        bool noSampleOnInvalid = true;
        for (const MetricSeries &m : series) {
            if (m.valid.empty()) continue;
            for (const PhaseSample &ps : m.phaseSamples)
                noSampleOnInvalid = noSampleOnInvalid
                                    && m.valid[size_t(nearestIndex(m.t_us, ps.t_us))] == 1u;
        }
        CHECK("no phase sample anywhere lands on an invalid index", noSampleOnInvalid);
    }

    // ── 6d2) GATED is not the same as MISSING, mid-track ───────────────────
    //
    // THE CASE THE CORPUS GATE FOUND. §6d's run is at the tail, where the extent rule marks it on its
    // own; a run INSIDE the track can only be marked by the mask's own reasoning, and the first
    // version of that reasoning got it wrong. On the real 2026-08-18 swing a 10-frame gated run in
    // hipLineTilt at 7 ms spacing came back flagged VALID — every frame of it was within 60 ms of the
    // last measurement — so the bridge was drawn and graded as a reading.
    //
    // The two holes below are the same length (10 frames) at the same spacing (7 ms), and must get
    // OPPOSITE answers:
    //
    //   frames 10–19  the hips are CONFIDENT and the line is refused (80° of turn) ⇒ GATED ⇒ 0.
    //                 The geometry was seen. There is no hip tilt at that instant to hold.
    //   frames 30–39  the keypoints are below the confidence gate ⇒ MISSING ⇒ bridged, valid.
    //                 The geometry was there; we just did not see it, and holding across 70 ms of a
    //                 pelvis is honest — that is what the budget is for.
    {
        std::printf("=== 6d2) a gated run vs a confidence hole, same length, same spacing ===\n");
        const int64_t dt7 = 7000;                     // ≈150 fps, the dense zone
        // 80° of turn: |dx| collapses to 0.174 of the address span, well under the 0.40 gate.
        const auto turned = [] {
            Lower p = addressPose();
            p.lHip = QPointF(0.50 - 0.04 * 0.1736, 0.50);
            p.rHip = QPointF(0.50 + 0.04 * 0.1736, 0.48);
            return p;
        };
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 50; ++k) {
            const bool gated = (k >= 10 && k <= 19);
            const bool dark  = (k >= 30 && k <= 39);
            frames.push_back(makeLower(k * dt7, gated ? turned() : addressPose(),
                                       dark ? 0.10f : 0.9f));
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;   // the reference is the opening square frames

        // Top inside the GATED run (must emit nothing) and Impact inside the CONFIDENCE hole (must
        // still emit, from the bridge — the honest hold).
        const auto phases = ladder({ { Phase::Address,  2 * dt7 },
                                     { Phase::Top,     15 * dt7 },
                                     { Phase::Impact,  35 * dt7 } });
        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt7, cfg);
        CHECK("the ten refused frames are recorded as GATED, not merely absent",
              res.gatedHipLine.size() == 10 && res.gatedHipLine.front() == 10 * dt7
                  && res.gatedHipLine.back() == 19 * dt7);

        const auto series = buildLowerBodySeries(res, phases);
        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        CHECK("hipLineTilt emitted", tilt != nullptr);
        if (tilt) {
            // Two rules: the gate accounts for 10..19, and the phase domain marks everything PAST
            // the Impact sample (index 35). The head is open, so 0 and 1 are measurements.
            bool exact = tilt->valid.size() == 50;
            for (size_t i = 0; i < tilt->valid.size(); ++i) {
                const uint8_t want = (i > 35 || (i >= 10 && i <= 19)) ? 0u : 1u;
                exact = exact && (tilt->valid[i] == want);
            }
            CHECK("ALL TEN gated frames are 0 — no budget excuses refused geometry", exact);
            CHECK("the same-length CONFIDENCE hole stays valid inside the domain, bridged as it "
                  "always was (36..39 are 0 for the DOMAIN's tail, not for the hole)",
                  tilt->valid.size() == 50 && tilt->valid[30] == 1u && tilt->valid[34] == 1u
                      && tilt->valid[35] == 1u);
            CHECK("the tail rule is INCLUSIVE at Impact and the HEAD is open, so nothing here came "
                  "from the extent rule",
                  tilt->valid.size() == 50 && tilt->valid.front() == 1u && tilt->valid[9] == 1u
                      && tilt->valid[20] == 1u && tilt->valid[35] == 1u);
            CHECK("a phase instant inside the GATED run emits nothing", !hasPhase(*tilt, Phase::Top));
            CHECK("…while one inside the confidence hole still does", hasPhase(*tilt, Phase::Impact));
        }

        // pelvisSway has no geometric gate at all, so nothing inside its phase domain is 0: the
        // confidence hole sits inside the bridge budget and is a hold, not a fabrication. Its only
        // zeros are the post-Impact tail.
        const MetricSeries *sway = findSeries(series, "pelvisSway");
        bool swayInDomain = sway && sway->valid.size() == 50 && sway->valid[36] == 0u;
        if (sway)
            for (size_t i = 0; i <= 35; ++i)
                swayInDomain = swayInDomain && sway->valid[i] == 1u;
        CHECK("an ungated channel with a bridged hole has no 0 anywhere in its domain",
              swayInDomain && hasPhase(*sway, Phase::Impact));
    }

    // ── 6d3) channel.maxBridgeUs < 0 is the OFF-SWITCH ─────────────────────
    //
    // The documented way back to the pre-mask behaviour, and it has to actually work: a negative
    // budget must mean NO MASK, not a budget nothing can satisfy. Calling the mask with −1 would
    // fail `d <= allowance` on every sample including the measured ones, and an all-zeros mask
    // withdraws six metrics from every reducer at once. The producer guards it; this pins the guard.
    {
        std::printf("=== 6d3) maxBridgeUs < 0 disables masking ===\n");
        const double kPi = 3.14159265358979323846;
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        for (int k = 0; k < 20; ++k) {
            const double c = std::cos((80.0 * k / 19.0) * kPi / 180.0);
            Lower p = addressPose();
            p.lHip = QPointF(0.50 - 0.04 * c, 0.50);
            p.rHip = QPointF(0.50 + 0.04 * c, 0.50 - 0.020 * k / 19.0);
            frames.push_back(makeLower((6 + k) * dt, p));
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000; cfg.maxBridgeUs = -1;
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,     20 * dt },
                                     { Phase::Impact,  24 * dt } });
        const auto series = buildLowerBodySeries(trackLowerBody(pose, W, H, true, 2 * dt, cfg),
                                                 phases);
        CHECK("every channel still emitted", series.size() == 6);
        bool noMask = !series.empty();
        for (const MetricSeries &m : series) noMask = noMask && m.valid.empty();
        CHECK("no mask anywhere — not an all-zeros one", noMask);
        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        CHECK("…and every ladder phase still samples, as it did before the mask existed",
              tilt && tilt->phaseSamples.size() == 3);
    }

    // ── 6e) The gate that never fires changes NOTHING ──────────────────────
    //
    // The other half of the promise, and the one the corpus gate is judged on: a swing with no gated
    // frame must serialise exactly as it did before MetricSeries::valid existed. EMPTY means all
    // valid — never an all-ones array, which would be a new key in every swing.json for nothing.
    {
        std::printf("=== 6e) no gated frame ⇒ empty mask, unchanged series ===\n");
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        for (int s = 1; s <= 6; ++s) {
            Lower p = addressPose();
            const double f = double(s) / 6.0;
            p.lHip = QPointF(p.lHip.x(), p.lHip.y() - 0.004 * f);   // a tilt, no rotation
            p.rHip = QPointF(p.rHip.x(), p.rHip.y() - 0.016 * f);
            frames.push_back(makeLower((5 + s) * dt, p));
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;
        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg);
        // ⚠ IMPACT LANDS ON THE LAST GRID SAMPLE HERE, which is what lets this section still make
        // the promise it exists to make: the phase domain's tail rule has nothing past Impact to mark,
        // the head is open, so no mask at all — byte-identical serialisation. What the domain does
        // when the grid runs PAST impact is §6g's job.
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      8 * dt },
                                     { Phase::Impact,  11 * dt } });
        const auto series = buildLowerBodySeries(res, phases);

        CHECK("the hip line is never gated by a pure tilt",
              res.hipTilt.t_us.size() == res.states.size());
        bool allEmpty = !series.empty();
        for (const MetricSeries &m : series) allEmpty = allEmpty && m.valid.empty();
        CHECK("every series leaves `valid` EMPTY — never an all-ones array", allEmpty);

        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        CHECK("…and the tilt still reads its three ladder phases",
              tilt && tilt->phaseSamples.size() == 3);
        CHECK("…with the value the geometry says (trail 12 px above over an 80 px span)",
              tilt && near(valueAt(*tilt, 11 * dt),
                           std::atan2(12.0, 80.0) * 57.29577951308232, 0.2));
    }

    // ── 6f) channelValidityMask, on its own ────────────────────────────────
    //
    // The one place the bridge-versus-measurement rule is spelled, so it is tested where it lives
    // rather than only through a producer. 60 ms is the default budget: a one- or two-frame dropout
    // still bridges silently (that is today's behaviour and it was honest), a real run does not.
    {
        std::printf("=== 6f) channelValidityMask ===\n");
        std::vector<int64_t> grid;
        for (int k = 0; k < 20; ++k) grid.push_back(k * dt);    // 0 .. 190 ms

        // Every grid instant measured ⇒ EMPTY, not twenty ones.
        CHECK("all valid returns an EMPTY mask",
              channelValidityMask(grid, grid, 60000).empty());

        // A 150 ms hole between 40 ms and 190 ms. Distance to the NEAREST measurement, either side:
        // 90 ms is 50 ms away (bridged silently, as it always was), 100 and 130 ms are exactly 60 ms
        // away (the budget is inclusive), and only 110 and 120 ms are beyond it.
        std::vector<int64_t> holed;
        for (int64_t t : grid)
            if (t <= 4 * dt || t >= 19 * dt) holed.push_back(t);
        const std::vector<uint8_t> m = channelValidityMask(grid, holed, 60000);
        CHECK("a hole wider than the budget produces a mask", m.size() == grid.size());
        CHECK("a sample within maxBridgeUs of a measurement is VALID", m[9] == 1u);
        CHECK("…and exactly at the budget is still valid", m[10] == 1u && m[13] == 1u);
        CHECK("a sample farther than maxBridgeUs from any measurement is INVALID",
              m[11] == 0u && m[12] == 0u);
        CHECK("a measured instant is always valid", m[4] == 1u && m[19] == 1u);

        // Outside the channel's extent is invalid whatever the budget says: the value there is not a
        // bridge between two measurements, it is a constant hold past the last one.
        std::vector<int64_t> late(grid.begin() + 5, grid.end());
        const std::vector<uint8_t> tail = channelValidityMask(grid, late, 10000000);
        CHECK("before the channel starts is INVALID however generous the budget",
              tail.size() == grid.size() && tail[0] == 0u && tail[4] == 0u && tail[5] == 1u);

        CHECK("an empty channel makes every sample invalid",
              channelValidityMask(grid, {}, 60000) == std::vector<uint8_t>(grid.size(), 0u));
        CHECK("an empty grid has nothing to mark", channelValidityMask({}, grid, 60000).empty());

        // ── GATED beats the budget, at the helper's own level ───────────────
        //
        // A gated instant is 0 even when it sits ON TOP of a measurement's neighbour, because the
        // budget answers "how long may we hold a value" and a refused frame has no value to hold.
        {
            std::vector<int64_t> minusOne;
            for (int64_t t : grid)
                if (t != 5 * dt) minusOne.push_back(t);          // one frame absent, 10 ms from both
            CHECK("a one-frame hole with nothing gated is bridged in silence",
                  channelValidityMask(grid, minusOne, 60000).empty());
            const std::vector<uint8_t> g = channelValidityMask(grid, minusOne, 60000, 1.5,
                                                               { 5 * dt });
            CHECK("…and the SAME hole, declared gated, is 0",
                  g.size() == grid.size() && g[5] == 0u && g[4] == 1u && g[6] == 1u);
            CHECK("an empty gated list changes nothing",
                  channelValidityMask(grid, grid, 60000, 1.5, {}).empty());
        }

        // ── The SPARSE regime, which a fixed 60 ms gets wrong ──────────────
        //
        // PoseRunner samples the address region at addressStride 15 (≈100 ms at 150 fps) or
        // coarseStride 12 (≈80 ms), so one dropped sample there is 80–100 ms from its neighbours. A
        // fixed budget would mark it, and that is the wrong answer: across one missing sample of a
        // STILL address, holding the previous value is a hold, not a fabrication — nothing happened
        // in between to misrepresent. The allowance is therefore
        // max(maxBridgeUs, 1.5 × the local spacing).
        {
            std::vector<int64_t> sparse;
            for (int k = 0; k < 8; ++k) sparse.push_back(k * 100000);   // 100 ms grid, 0 .. 700 ms

            std::vector<int64_t> oneMissing = sparse;
            oneMissing.erase(oneMissing.begin() + 3);                   // 300 ms absent
            CHECK("one dropped sample on a 100 ms grid is a HOLD, not a fabrication",
                  channelValidityMask(sparse, oneMissing, 60000).empty());

            // Two adjacent are still within one-and-a-half spacings of a measurement…
            std::vector<int64_t> twoMissing;
            for (int64_t t : sparse)
                if (t != 300000 && t != 400000) twoMissing.push_back(t);
            CHECK("…and so are two adjacent ones",
                  channelValidityMask(sparse, twoMissing, 60000).empty());

            // …but the middle of three is 200 ms from either edge, which no spacing argument
            // excuses.
            std::vector<int64_t> threeMissing;
            for (int64_t t : sparse)
                if (t != 300000 && t != 400000 && t != 500000) threeMissing.push_back(t);
            const std::vector<uint8_t> tm = channelValidityMask(sparse, threeMissing, 60000);
            CHECK("the middle of a three-sample hole IS marked, even on a sparse grid",
                  tm.size() == sparse.size() && tm[4] == 0u && tm[3] == 1u && tm[5] == 1u);

            // And the sparse allowance must not leak into the dense zone: on an 8 ms grid the
            // spacing term is 12 ms and the 60 ms floor is what decides.
            std::vector<int64_t> dense;
            for (int k = 0; k < 40; ++k) dense.push_back(k * 8000);     // 8 ms grid, 0 .. 312 ms
            std::vector<int64_t> denseHole;
            for (int64_t t : dense)
                if (t < 80000 || t > 240000) denseHole.push_back(t);    // a 160 ms gated run
            const std::vector<uint8_t> dh = channelValidityMask(dense, denseHole, 60000);
            CHECK("a 160 ms run in the DENSE zone is still marked (60 ms floor decides)",
                  dh.size() == dense.size() && dh[20] == 0u && dh[9] == 1u && dh[39] == 1u);
        }
    }

    // ── 6g) THE PHASE DOMAIN: past impact, the sample is not a measurement ─
    //
    // Design §5.1's domain table. pelvisSway, pelvisLift, leadKneeDrift, plumbBobDistance and
    // hipLineTilt are ADDRESS→IMPACT quantities: past impact the pelvis has turned toward the target,
    // so the frontal-plane projection of a lateral quantity is measuring rotation — the +35 % sway
    // step after impact in the design's screenshot is exactly that, and it is not noise. The quantity
    // did not exist there, so the sample is marked invalid and no reducer may read it.
    //
    // ⚠ THE TAIL ONLY. The pre-Address head stays VALID, which the 5-swing gate forced and which is
    // right twice over: the chart does not clip the start side (a domain whose first phase is Address
    // is the DEFAULT first phase, so `firstNarrowed` is false and the card's window still starts at
    // the series' first sample — marking the head put the card's own start edge inside a masked run
    // and every clamped card came back `partial`), and a still golfer referenced to the address frames
    // is a real reading of address posture, which is the only evidence the still-address gate window
    // [Address − 300 ms, Address] has.
    //
    // ⚠ WHY IN THE PRODUCER AND NOT IN A REDUCER. series_reduce.h's extremum deliberately does NOT
    // clip its support to the window it was asked about, because the diagnostics engine caches an
    // extreme per (lo, hi] span while the card reduces a whole window, and the two agree only while a
    // sample's windowed mean is the same number whoever asked (W2 measured 20 disagreements in 514
    // measures with the support clipped, 0 without). So a reducer cannot close a domain leak without
    // breaking cache agreement. Marking the samples closes it once, for every consumer and every
    // query, because they stop being measurements at all.
    {
        std::printf("=== 6g) the post-Impact phase domain ===\n");
        // A pure tilt, so NOTHING is gated and the only zeros there can be are the domain's, over a
        // grid that runs past impact — which is every real swing: the window is padded to the finish.
        std::vector<PoseFrame2D> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeLower(k * dt, addressPose()));
        for (int j = 1; j <= 6; ++j) {
            Lower p = addressPose();
            const double f = double(j) / 6.0;
            p.lHip = QPointF(p.lHip.x(), p.lHip.y() - 0.004 * f);
            p.rHip = QPointF(p.rHip.x(), p.rHip.y() - 0.016 * f);
            frames.push_back(makeLower((5 + j) * dt, p));
        }
        PoseTrack2D pose; pose.frames = frames;
        LowerBodyConfig cfg; cfg.addrWindowUs = 30000;
        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, cfg);

        // Grid 0..11; Impact on index 9, so 10 and 11 are past the domain and 0..9 are not.
        const auto full = ladder({ { Phase::Address,  2 * dt },
                                   { Phase::Top,      6 * dt },
                                   { Phase::Impact,   9 * dt },
                                   { Phase::Finish,  11 * dt } });
        const auto series = buildLowerBodySeries(res, full);

        const MetricSeries *sway = findSeries(series, "pelvisSway");
        CHECK("pelvisSway emitted", sway != nullptr);
        if (sway) {
            bool shape = sway->valid.size() == 12;
            for (size_t i = 0; i < sway->valid.size(); ++i)
                shape = shape && (sway->valid[i] == (i > 9 ? 0u : 1u));
            CHECK("0 after the Impact sample, 1 everywhere up to and including it", shape);
            // THE HEAD IS OPEN. These are readings of address posture, and the still-address gate
            // window is measured on them.
            CHECK("the pre-Address samples stay VALID — the chart never clips that side",
                  sway->valid.size() == 12 && sway->valid[0] == 1u && sway->valid[1] == 1u);
            // INCLUSIVE at Impact, and it matters: the Impact reading is the one every
            // Address→Impact corridor is keyed on, and nearestIndex is the same snap the chart's
            // phase dots use, so the boundary sample the user sees is the boundary sample kept.
            CHECK("the Impact sample itself is VALID — 1 at it, 0 after it",
                  sway->valid.size() == 12 && sway->valid[9] == 1u && sway->valid[10] == 0u);
            CHECK("…and so a P7 reading is still emitted", hasPhase(*sway, Phase::Impact));
        }

        // THE VALUES DO NOT MOVE. The curve stays continuous and is still drawn (dashed, outside the
        // domain) and still hovers — only what may be REDUCED changed.
        const auto bare = buildLowerBodySeries(res, {});          // no ladder ⇒ no domain
        const MetricSeries *swayBare = findSeries(bare, "pelvisSway");
        bool sameValues = sway && swayBare && sway->value.size() == swayBare->value.size();
        if (sameValues)
            for (size_t i = 0; i < sway->value.size(); ++i)
                sameValues = sameValues && sway->value[i] == swayBare->value[i];
        CHECK("value[] is untouched — absence lives in the mask, never in the curve", sameValues);

        // A WHOLE-SWING channel is not narrowed at all: comOverLeadFoot is READ at the finish and is
        // a distance along the stance line, which survives the turn (design §5.1's table).
        const MetricSeries *com = findSeries(series, "comOverLeadFoot");
        const MetricSeries *fa  = findSeries(series, "feetAlignment");
        CHECK("comOverLeadFoot and feetAlignment carry NO mask — their domain is the whole swing",
              com && com->valid.empty() && fa && fa->valid.empty());
        CHECK("…and comOverLeadFoot still samples the FINISH", com && hasPhase(*com, Phase::Finish));

        // AN UNSEGMENTED IMPACT IS UNBOUNDED: there is no instant to mark a tail from, and marking one
        // from a guess would withdraw real measurements. With the head open, that leaves NO mask.
        const auto noImpact = buildLowerBodySeries(res, ladder({ { Phase::Address, 2 * dt },
                                                                 { Phase::Top,     6 * dt } }));
        const MetricSeries *sNoImp = findSeries(noImpact, "pelvisSway");
        CHECK("no Impact in the ladder ⇒ no marking at all, `valid` stays EMPTY",
              sNoImp && sNoImp->valid.empty());

        // And the Address end is not a bound in the first place, so a ladder without it marks exactly
        // the same tail as the full one.
        const auto noAddress = buildLowerBodySeries(res, ladder({ { Phase::Top,    6 * dt },
                                                                  { Phase::Impact, 9 * dt } }));
        const MetricSeries *sNoAddr = findSeries(noAddress, "pelvisSway");
        bool tailOnly = sNoAddr && sNoAddr->valid.size() == 12;
        if (sNoAddr)
            for (size_t i = 0; i < sNoAddr->valid.size(); ++i)
                tailOnly = tailOnly && (sNoAddr->valid[i] == (i > 9 ? 0u : 1u));
        CHECK("no Address ⇒ the same tail, because Address was never a bound", tailOnly);

        const auto neither = buildLowerBodySeries(res, ladder({ { Phase::Top, 6 * dt } }));
        const MetricSeries *sNeither = findSeries(neither, "pelvisSway");
        CHECK("no Impact and no Address ⇒ the series is left exactly as it was",
              sNeither && sNeither->valid.empty());

        // ── applyPhaseDomainMask on its own ────────────────────────────────
        //
        // Two properties the producers cannot reach today and that a future caller will: an
        // out-of-domain LABELLED sample has to go with the samples (measure_sample.cpp falls back to
        // phaseSamples where the curve has nothing to say, so a Finish reading left behind here would
        // be graded against a corridor by the very path this mask exists to starve), and a domain
        // that covers the whole grid must leave the mask EMPTY rather than all-ones.
        {
            const auto make = [&] {
                MetricSeries m;
                m.key  = QStringLiteral("probe");
                m.t_us = { 0, 10000, 20000, 30000, 40000 };
                m.value = { 1.0, 2.0, 3.0, 4.0, 5.0 };
                m.phaseSamples = { { Phase::Address, 10000, 2.0, QString() },
                                   { Phase::Impact,  30000, 4.0, QString() },
                                   { Phase::Finish,  40000, 5.0, QString() } };
                return m;
            };
            const auto ld = ladder({ { Phase::Address, 10000 }, { Phase::Impact, 30000 } });

            // The DEFAULT form — the one both producers call: tail bounded, head open.
            MetricSeries tail = make();
            applyPhaseDomainMask(tail, ld);
            CHECK("the default form marks only the tail and keeps the Impact sample",
                  tail.valid.size() == 5 && tail.valid[0] == 1u && tail.valid[1] == 1u
                      && tail.valid[3] == 1u && tail.valid[4] == 0u);
            CHECK("a LABELLED sample past Impact is dropped, the in-domain ones kept",
                  tail.phaseSamples.size() == 2 && !hasPhase(tail, Phase::Finish)
                      && hasPhase(tail, Phase::Impact) && hasPhase(tail, Phase::Address));

            // The two-sided form, which no producer uses today — kept because the head bound is a
            // legitimate request for a metric whose domain genuinely starts later than the window.
            MetricSeries both = make();
            applyPhaseDomainMask(both, ld, Phase::Address);
            CHECK("an explicit first phase marks the head as well",
                  both.valid.size() == 5 && both.valid[0] == 0u && both.valid[1] == 1u
                      && both.valid[4] == 0u);

            MetricSeries covered = make();
            applyPhaseDomainMask(covered, ladder({ { Phase::Impact, 40000 } }));
            CHECK("a tail that lands on the last sample leaves `valid` EMPTY, never all-ones",
                  covered.valid.empty() && covered.phaseSamples.size() == 3);

            MetricSeries backwards = make();
            applyPhaseDomainMask(backwards, ladder({ { Phase::Address, 30000 },
                                                     { Phase::Impact,  10000 } }),
                                 Phase::Address);
            CHECK("a ladder with Impact BEFORE Address is refused, not obeyed",
                  backwards.valid.empty() && backwards.phaseSamples.size() == 3);
        }
    }

    // ── 8) σ propagation (design §5.3, contract C11) ────────────────────────
    //
    // WHAT THESE CASES ARE FOR. `MetricSeries::sigma` is 1σ MEASUREMENT noise, and the display layer
    // rounds every printed digit to it — so a σ that is wrong by a factor makes the chart round to the
    // wrong place and say so confidently. The arithmetic is therefore pinned per series, from the
    // fixture's own geometry, with the derivation written out.
    //
    // THE ARITHMETIC, once (σ_kp = 2 px on every joint, hips 100 px apart, stance 200 px, mmPerPx 2):
    //   sqrt(σ_a² + σ_b²) = sqrt(8) = 2.8284271 px is the σ of any DIFFERENCE of two keypoints
    //   hipLineTilt      sqrt(8)/100  rad→deg  = 1.6205694°   (σ over the line's Euclidean LENGTH)
    //   feetAlignment    sqrt(8)/200  rad→deg  = 0.8102847°   (twice the lever arm ⇒ half the σ)
    //   pelvisSway/Lift  0.5·sqrt(8)/200 ·100  = 0.7071068 %  (the 0.5 is the pelvis MIDPOINT)
    //   leadKneeDrift    sqrt(8)/200 ·100      = 1.4142136 %  (knee MINUS hip: two keypoints, no 0.5)
    //
    // THE TWO PROJECTED DISTANCES carry a THIRD term that dominates both of the above, so their
    // arithmetic is written out separately. Keypoint noise in either ankle ROTATES the stance line by
    // dφ ≈ σ/L, and a point h px off that line then moves h·dφ ALONG it. Here the hip centre sits
    // h = 400 px above an L = 200 px ankle line, so h/L = 2 and the rotation terms carry FOUR times the
    // reference-ankle term. Differentiating the whole expression (see the producer) gives
    //   comOverLeadFoot  var = 0.25σ² + 0.25σ² + (1 + 4)σ² + 4σ²          = 38 px² ⇒ 6.1644140 px
    //                    ⇒ 6.1644140 · 100/200                           = 3.0822070 %
    //   plumbBob         var = 0.25σ² + 0.25σ² + (0.25 + 4)(σ² + σ²)      = 36 px² ⇒ 6.0 px exactly
    //                    ⇒ 6.0 · 2/25.4                                  = 0.4724409 in
    // C11 pinned the (h/L) = 0 readings of both — 1.2247449 % and 0.1113554 in — which understate σ by
    // 2.5× and 4.2×. Design principle 3 rules those out; the full forms are what the producer ships and
    // what these cases assert.
    {
        std::printf("=== 8a) a constant keypoint σ gives each series its closed-form σ ===\n");
        const std::vector<Lower> poses(12, sigmaPose());
        const PoseTrack2D pose = sigmaTrack(poses, std::vector<double>(12, 2.0), dt);
        // Impact ON THE LAST GRID SAMPLE, so the phase-domain tail marks nothing and every frame
        // contributes: §8d is where the mask does the work.
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      6 * dt },
                                     { Phase::Impact,  11 * dt } });
        const LowerBodyResult res = trackLowerBody(pose, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0);
        CHECK("the fixture resolved as designed (hip line 100 px, stance 200 px)",
              near(res.addrHipSpanPx, 100.0, 1e-9) && near(res.addrSpanPx, 200.0, 1e-9));

        // THE PARALLELISM INVARIANT. One σ per pushed value, on every channel — the property that
        // makes a per-sample σ safe to reduce at all. A channel that pushed a value without a σ would
        // silently pair every later σ with the wrong sample.
        CHECK("every channel's σ track is parallel to its values",
              res.kneeDrift.sigma.size()  == res.kneeDrift.t_us.size()
                  && res.pelvisSway.sigma.size()  == res.pelvisSway.t_us.size()
                  && res.pelvisLift.sigma.size()  == res.pelvisLift.t_us.size()
                  && res.hipTilt.sigma.size()     == res.hipTilt.t_us.size()
                  && res.feetAlign.sigma.size()   == res.feetAlign.t_us.size()
                  && res.comOverLead.sigma.size() == res.comOverLead.t_us.size()
                  && res.plumbBob.sigma.size()    == res.plumbBob.t_us.size());

        const auto series = buildLowerBodySeries(res, phases);
        CHECK("all seven series emitted (the ruler resolved, so plumbBob is present)",
              series.size() == 7);

        const double q2   = std::sqrt(8.0);            // σ of a difference of two 2 px keypoints
        const double toDeg = 57.29577951308232;
        // The projected distances' lever ratio, from the FIXTURE's geometry: the hip centre (y = 500)
        // sits 400 px above the ankle line (y = 900), which is 200 px long.
        const double sig2  = 4.0;                      // σ_kp² = (2 px)²
        const double lever = 400.0 / 200.0;
        const double lev2  = lever * lever;
        struct Want { const char *key; double sigma; };
        const Want wants[] = {
            { "hipLineTilt",      q2 / 100.0 * toDeg },
            { "feetAlignment",    q2 / 200.0 * toDeg },
            { "pelvisSway",       0.5 * q2 / 200.0 * 100.0 },
            { "pelvisLift",       0.5 * q2 / 200.0 * 100.0 },
            { "leadKneeDrift",    q2 / 200.0 * 100.0 },
            // ∂/∂M = û (0.25 per hip); ∂/∂A = −û + (h/L)n̂; ∂/∂B = −(h/L)n̂
            { "comOverLeadFoot",  std::sqrt(0.25 * sig2 + 0.25 * sig2
                                            + (1.0 + lev2) * sig2 + lev2 * sig2) / 200.0 * 100.0 },
            // the stance CENTRE is the reference, so BOTH ankles carry −0.5û as well as ±(h/L)n̂
            { "plumbBobDistance", std::sqrt(0.25 * sig2 + 0.25 * sig2
                                            + (0.25 + lev2) * (sig2 + sig2)) * 2.0 / 25.4 },
        };
        for (const Want &w : wants) {
            const MetricSeries *m = findSeries(series, w.key);
            char label[160];
            std::snprintf(label, sizeof(label), "%s σ = %.7f (closed form)", w.key, w.sigma);
            CHECK(label, m && m->sigma.has_value() && nearRel(*m->sigma, w.sigma, 1e-5));
        }
        // And the two absolute figures C11 quotes, spelled out so a reader can check them without
        // re-deriving anything.
        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        const MetricSeries *pb   = findSeries(series, "plumbBobDistance");
        CHECK("hip tilt over a 100 px line is 1.62° of σ, not 0.02",
              tilt && tilt->sigma && near(*tilt->sigma, 1.6205694, 5e-5));
        // Half an inch, not a tenth: the stance line's ROTATION under ankle jitter is what the number
        // is mostly made of, and C11's 0.1113554 in was that term left out. C12's display step is the
        // nicest of {1,2,5}×10ⁿ that is not BELOW σ, FLOORED AT ONE UNIT — so σ = 0.47 in does not buy
        // half-inches, it lands on the floor and the plumb bob prints in WHOLE INCHES. That is the
        // honest digit for a reading built on a 200 px line with 2 px keypoints, and it is the
        // difference this whole phase exists to make: the same curve used to print two decimals.
        CHECK("the plumb bob's σ is 0.472 in — 6.0 px exactly, through the ruler",
              pb && pb->sigma && near(*pb->sigma, 0.4724409, 5e-5));
        const MetricSeries *com = findSeries(series, "comOverLeadFoot");
        CHECK("comOverLeadFoot's σ is 3.08 % of stance — 2.5× what the (h/L)=0 form claimed",
              com && com->sigma && near(*com->sigma, 3.0822070, 5e-5));
    }

    // ── 8b) no smoother ⇒ no σ anywhere ────────────────────────────────────
    //
    // The field's contract: ABSENT means "not characterised", and every swing analysed before the
    // smoother existed has no `smoothedAux`. Writing 0 there would claim a perfect measurement, and
    // the display layer would then print every digit it has.
    {
        std::printf("=== 8b) smoothedAux empty ⇒ every sigma UNSET ===\n");
        const std::vector<Lower> poses(12, sigmaPose());
        const PoseTrack2D pose = sigmaTrack(poses, {}, dt);      // no aux at all
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      6 * dt },
                                     { Phase::Impact,  11 * dt } });
        const auto series = buildLowerBodySeries(
            trackLowerBody(pose, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0), phases);
        bool none = series.size() == 7;
        for (const MetricSeries &m : series)
            none = none && !m.sigma.has_value();
        CHECK("no series carries σ — and none carries 0 either", none);

        // A track with a smoothed companion whose σ are all ZERO is the same statement: the smoother
        // ran and produced nothing for those keypoints (its own sentinel), so there is still no budget.
        const PoseTrack2D zeros = sigmaTrack(poses, std::vector<double>(12, 0.0), dt);
        const auto zseries = buildLowerBodySeries(
            trackLowerBody(zeros, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0), phases);
        bool zn = zseries.size() == 7;
        for (const MetricSeries &m : zseries)
            zn = zn && !m.sigma.has_value();
        CHECK("σ = 0 on every joint is ABSENT, not a perfect measurement", zn);
    }

    // ── 8c) one joint unsmoothed ⇒ those frames drop out, the median does not move ──
    //
    // The rule this pins is "every joint the value was built from must report a σ". A frame where one
    // of them did not has an error budget with a hole in it, and a hole is UNKNOWN rather than small —
    // so the frame contributes nothing rather than an optimistic figure. With a constant σ elsewhere
    // the median is unchanged, which is the cleanest way to show the frames were dropped and not
    // merely down-weighted. The channels that never touch that joint keep every frame.
    {
        std::printf("=== 8c) frames with a missing joint σ are excluded ===\n");
        const std::vector<Lower> poses(12, sigmaPose());
        const double toDeg = 57.29577951308232;
        const double q2 = std::sqrt(8.0);
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      6 * dt },
                                     { Phase::Impact,  11 * dt } });

        PoseTrack2D some = sigmaTrack(poses, std::vector<double>(12, 2.0), dt);
        some.smoothedAux[3].sigma[kLHip] = 0.f;      // lead = LEFT here, so this is the LEAD hip
        some.smoothedAux[4].sigma[kLHip] = 0.f;
        const auto sseries = buildLowerBodySeries(
            trackLowerBody(some, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0), phases);
        const MetricSeries *tilt = findSeries(sseries, "hipLineTilt");
        const MetricSeries *feet = findSeries(sseries, "feetAlignment");
        const MetricSeries *sway = findSeries(sseries, "pelvisSway");
        CHECK("the median of a constant σ is unmoved by dropping two frames",
              tilt && tilt->sigma && nearRel(*tilt->sigma, q2 / 100.0 * toDeg, 1e-5)
                  && sway && sway->sigma && nearRel(*sway->sigma, 0.5 * q2 / 2.0, 1e-5));
        CHECK("a channel that never reads that joint keeps every frame",
              feet && feet->sigma && nearRel(*feet->sigma, q2 / 200.0 * toDeg, 1e-5));

        // The same joint dark on EVERY frame: now nothing contributed, so those series carry no σ at
        // all — while the ankle-only channel still does. Absence is PER SERIES, which is what makes it
        // informative rather than a global switch.
        PoseTrack2D all = sigmaTrack(poses, std::vector<double>(12, 2.0), dt);
        for (PoseKpAux &a : all.smoothedAux) a.sigma[kLHip] = 0.f;
        const auto aseries = buildLowerBodySeries(
            trackLowerBody(all, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0), phases);
        const char *hipKeys[] = { "hipLineTilt", "pelvisSway", "pelvisLift", "leadKneeDrift",
                                  "comOverLeadFoot", "plumbBobDistance" };
        bool gone = true;
        for (const char *k : hipKeys) {
            const MetricSeries *m = findSeries(aseries, k);
            gone = gone && m && !m->sigma.has_value();
        }
        const MetricSeries *feet2 = findSeries(aseries, "feetAlignment");
        CHECK("every channel that reads the lead hip loses its σ entirely", gone);
        CHECK("…and feetAlignment, which does not, keeps its own",
              feet2 && feet2->sigma.has_value());
    }

    // ── 8d) the validity mask is honoured: the post-Impact tail does not vote ──
    //
    // ⚠ THE PHASE DOMAIN'S TAIL IS THE ONLY WAY A σ ENTRY CAN EXIST AND STILL BE MASKED OUT, which is
    // why it is the case that tests the mask. A GATED frame and an OVER-BRIDGED frame both leave the
    // channel with no sample at that instant, so they carry no σ entry to exclude in the first place —
    // the sparsity does that work. Past impact the sample IS there, is drawn, and is invalid.
    //
    // Construction: eight held frames at σ = 2 px, then TWELVE more at σ = 20 px, Impact on frame 7.
    // The tail is the MAJORITY deliberately — if it voted, the median would be the high value, so the
    // low answer cannot come from luck. The channels whose domain is the whole swing (feetAlignment,
    // comOverLeadFoot) are the control: their medians DO move, which shows the exclusion comes from
    // the mask and not from the code quietly ignoring the tail.
    {
        std::printf("=== 8d) masked frames do not enter the median ===\n");
        const std::vector<Lower> poses(20, sigmaPose());
        std::vector<double> sigs;
        for (int k = 0; k < 20; ++k) sigs.push_back(k <= 7 ? 2.0 : 20.0);
        const PoseTrack2D pose = sigmaTrack(poses, sigs, dt);
        const auto phases = ladder({ { Phase::Address, 2 * dt },
                                     { Phase::Top,     5 * dt },
                                     { Phase::Impact,  7 * dt } });
        const auto series = buildLowerBodySeries(
            trackLowerBody(pose, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0), phases);

        const double q2   = std::sqrt(8.0);            // the HEAD's difference-σ (2 px keypoints)
        const double toDeg = 57.29577951308232;
        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        CHECK("the tail really is masked (so the case is testing what it claims)",
              tilt && tilt->valid.size() == 20 && tilt->valid[7] == 1u && tilt->valid[8] == 0u);
        CHECK("hipLineTilt σ is the HEAD's, though the tail is 12 frames of 20",
              tilt && tilt->sigma && nearRel(*tilt->sigma, q2 / 100.0 * toDeg, 1e-5));
        const MetricSeries *sway = findSeries(series, "pelvisSway");
        const MetricSeries *knee = findSeries(series, "leadKneeDrift");
        const MetricSeries *pb   = findSeries(series, "plumbBobDistance");
        CHECK("…and so are the other three Address→Impact channels",
              sway && sway->sigma && nearRel(*sway->sigma, 0.5 * q2 / 2.0, 1e-5)
                  && knee && knee->sigma && nearRel(*knee->sigma, q2 / 2.0, 1e-5)
                  // 6.0 px exactly through the ruler — the closed form is written out in §8a.
                  && pb && pb->sigma && nearRel(*pb->sigma, 6.0 * 2.0 / 25.4, 1e-5));

        // The control. feetAlignment and comOverLeadFoot are whole-swing quantities (design §5.1), so
        // every frame is valid for them and the 20 px tail is the majority — their σ is TEN times the
        // head's, because every formula here is homogeneous of degree one in the keypoint σ.
        const MetricSeries *feet = findSeries(series, "feetAlignment");
        const MetricSeries *com  = findSeries(series, "comOverLeadFoot");
        const double sig2 = 4.0, lever = 400.0 / 200.0, lev2 = lever * lever;
        const double comHead = std::sqrt(0.25 * sig2 + 0.25 * sig2
                                         + (1.0 + lev2) * sig2 + lev2 * sig2) / 200.0 * 100.0;
        CHECK("an UNMASKED channel does see the tail — the mask is doing the excluding",
              feet && feet->sigma && nearRel(*feet->sigma, 10.0 * q2 / 200.0 * toDeg, 1e-5)
                  && com && com->sigma && nearRel(*com->sigma, 10.0 * comHead, 1e-5));
    }

    // ── 8e) σ moves no number ──────────────────────────────────────────────
    //
    // Design §6: stages 5.1–5.3 change no persisted `value`. This is that promise for this producer,
    // and it is asserted BITWISE rather than to a tolerance — σ is read off a parallel track and
    // written to one optional field, so there is no rounding for it to hide behind.
    {
        std::printf("=== 8e) value[] and phaseSamples are bit-identical with and without σ ===\n");
        const std::vector<Lower> poses(12, sigmaPose());
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      6 * dt },
                                     { Phase::Impact,  11 * dt } });
        const auto withSig = buildLowerBodySeries(
            trackLowerBody(sigmaTrack(poses, std::vector<double>(12, 2.0), dt), W, H, true,
                           2 * dt, {}, /*mmPerPx=*/2.0), phases);
        const auto without = buildLowerBodySeries(
            trackLowerBody(sigmaTrack(poses, {}, dt), W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0),
            phases);

        bool same = withSig.size() == without.size() && !withSig.empty();
        for (size_t i = 0; same && i < withSig.size(); ++i) {
            const MetricSeries &a = withSig[i], &b = without[i];
            same = same && a.key == b.key && a.unit == b.unit
                   && a.t_us == b.t_us && a.value == b.value && a.valid == b.valid
                   && a.phaseSamples.size() == b.phaseSamples.size();
            for (size_t j = 0; same && j < a.phaseSamples.size(); ++j)
                same = same && a.phaseSamples[j].phase == b.phaseSamples[j].phase
                       && a.phaseSamples[j].t_us == b.phaseSamples[j].t_us
                       && a.phaseSamples[j].value == b.phaseSamples[j].value;
        }
        CHECK("every key, t_us, value, valid and phaseSample is identical", same);
        bool onlySigma = true;
        for (const MetricSeries &m : withSig)  onlySigma = onlySigma && m.sigma.has_value();
        for (const MetricSeries &m : without)  onlySigma = onlySigma && !m.sigma.has_value();
        CHECK("…and σ is the ONLY difference between the two runs", onlySigma);
    }

    // ── 8f) A TILTED line: σ divides by the EUCLIDEAN length, not by |Δx| ──
    //
    // §8a cannot see the difference — its lines are level, so L == |Δx| and a mutation swapping one for
    // the other passes. Tilt the trail hip 30 px (16.7° on a 100 px line) and the two readings separate
    // by 1/cos(16.7°) = 4.40 %, which this case pins from BOTH sides: the Euclidean value must match,
    // and the |Δx| value must NOT.
    //
    // Only hipLineTilt and feetAlignment are asserted here. Dropping the trail hip also moves the hip
    // MIDPOINT, so the two projected distances change for a reason that has nothing to do with the
    // line-tilt question, and asserting them would test two things at once.
    {
        std::printf("=== 8f) a tilted line: Euclidean L, not |Δx| ===\n");
        const std::vector<Lower> poses(12, sigmaPoseTilted());
        const PoseTrack2D pose = sigmaTrack(poses, std::vector<double>(12, 2.0), dt);
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      6 * dt },
                                     { Phase::Impact,  11 * dt } });
        const auto series = buildLowerBodySeries(
            trackLowerBody(pose, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0), phases);

        const double q2    = std::sqrt(8.0);
        const double toDeg = 57.29577951308232;
        const double hipL  = std::hypot(100.0, 30.0);        // 104.4031 px, from the fixture
        const MetricSeries *tilt = findSeries(series, "hipLineTilt");
        CHECK("the tilt really is 16.7°, so the case is testing what it claims",
              tilt && near(std::abs(valueAt(*tilt, 6 * dt)),
                           std::atan2(30.0, 100.0) * toDeg, 1e-6));
        CHECK("hipLineTilt σ = sqrt(8)/104.4031 · 180/π = 1.5522239°",
              tilt && tilt->sigma && nearRel(*tilt->sigma, q2 / hipL * toDeg, 1e-5));
        CHECK("…and NOT sqrt(8)/|Δx|, which is 4.40 % larger — the ∂θ/∂Δx term is carried",
              tilt && tilt->sigma && !nearRel(*tilt->sigma, q2 / 100.0 * toDeg, 1e-3));
        // The control, in the same run: the ankles were not tilted, so their σ is unchanged. σ is a
        // per-LINE quantity taken from that line's own length, not one number for the swing.
        const MetricSeries *feet = findSeries(series, "feetAlignment");
        CHECK("the still-level ankle line is unchanged at 0.8102847°",
              feet && feet->sigma && nearRel(*feet->sigma, q2 / 200.0 * toDeg, 1e-5));
    }

    // ── 8g) DISTINCT per-joint σ: the coefficients, and the handedness plumbing ──
    //
    // Every case above gives every joint the same σ, which pins no asymmetric COEFFICIENT at all: swap
    // the lead and trail ankle in comOverLeadFoot, or read the trail hip where leadKneeDrift wants the
    // lead one, and a constant σ hides it perfectly. σ here is attached to the PHYSICAL keypoint —
    // left hip 2, right hip 3, left knee 5, right knee 7, left ankle 1, right ankle 4 — and the SAME
    // fixture is run with `leadIsLeft` both ways, so the two runs are each other's control: any channel
    // whose σ fails to swap is reading the wrong side.
    //
    // ⚠ WHAT THIS CANNOT PIN, stated rather than implied. `leadKneeDrift`'s σ is sqrt(σ_knee² + σ_hip²)
    // — SYMMETRIC in its two inputs, so no fixture can detect knee and lead-hip being exchanged. There
    // is nothing there to detect: the two coefficients are genuinely both 1. What IS pinned is the
    // joint SELECTION, because reading the trail hip (3) instead of the lead hip (2) gives 2.9154759
    // against 2.6925824. The same applies to every line tilt, and to plumbBobDistance, whose two ankle
    // coefficients are equal by construction (0.25 + (h/L)² each) — its σ is deliberately asserted to
    // be the SAME in both runs, which is the honest statement about it.
    {
        std::printf("=== 8g) distinct per-joint σ, both handedness ===\n");
        const std::vector<Lower> poses(12, sigmaPose());
        const PoseKpAux aux = auxPerJoint(/*lHip=*/2.0, /*rHip=*/3.0, /*lKnee=*/5.0,
                                          /*rKnee=*/7.0, /*lAnk=*/1.0, /*rAnk=*/4.0);
        const PoseTrack2D pose = trackWithAux(poses, aux, dt);
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      6 * dt },
                                     { Phase::Impact,  11 * dt } });
        const double toDeg = 57.29577951308232;
        const double lever = 400.0 / 200.0, lev2 = lever * lever;   // as §8a: hips 400 px up, line 200
        const auto q = [](double a, double b) { return std::sqrt(a * a + b * b); };

        for (int pass = 0; pass < 2; ++pass) {
            const bool leadIsLeft = (pass == 0);
            // The σ each ROLE sees, given that the aux is pinned to physical left/right.
            const double sLHip = leadIsLeft ? 2.0 : 3.0, sTHip = leadIsLeft ? 3.0 : 2.0;
            const double sLKnee = leadIsLeft ? 5.0 : 7.0;
            const double sLAnk = leadIsLeft ? 1.0 : 4.0, sTAnk = leadIsLeft ? 4.0 : 1.0;
            const auto series = buildLowerBodySeries(
                trackLowerBody(pose, W, H, leadIsLeft, 2 * dt, {}, /*mmPerPx=*/2.0), phases);

            struct Want { const char *key; double sigma; };
            const Want wants[] = {
                // symmetric in their two inputs ⇒ the same in both passes, which is itself the claim
                { "hipLineTilt",      q(sLHip, sTHip) / 100.0 * toDeg },
                { "feetAlignment",    q(sLAnk, sTAnk) / 200.0 * toDeg },
                { "pelvisSway",       0.5 * q(sLHip, sTHip) / 200.0 * 100.0 },
                { "pelvisLift",       0.5 * q(sLHip, sTHip) / 200.0 * 100.0 },
                // asymmetric: the LEAD knee against the LEAD hip, never the trail one
                { "leadKneeDrift",    q(sLKnee, sLHip) / 200.0 * 100.0 },
                // asymmetric: the LEAD ankle is the reference (1 + (h/L)²), the trail is pure rotation
                { "comOverLeadFoot",  std::sqrt(0.25 * (sLHip * sLHip + sTHip * sTHip)
                                                + (1.0 + lev2) * sLAnk * sLAnk
                                                + lev2 * sTAnk * sTAnk) / 200.0 * 100.0 },
                // the two ankles carry EQUAL coefficients here, so this one cannot swap
                { "plumbBobDistance", std::sqrt(0.25 * (sLHip * sLHip + sTHip * sTHip)
                                                + (0.25 + lev2) * (sLAnk * sLAnk + sTAnk * sTAnk))
                                          * 2.0 / 25.4 },
            };
            for (const Want &w : wants) {
                const MetricSeries *m = findSeries(series, w.key);
                char label[192];
                std::snprintf(label, sizeof(label), "leadIsLeft=%d  %s σ = %.7f",
                              leadIsLeft ? 1 : 0, w.key, w.sigma);
                CHECK(label, m && m->sigma.has_value() && nearRel(*m->sigma, w.sigma, 1e-5));
            }

            const MetricSeries *knee = findSeries(series, "leadKneeDrift");
            CHECK("leadKneeDrift does NOT read the trail hip",
                  knee && knee->sigma && !nearRel(*knee->sigma, q(sLKnee, sTHip) / 2.0, 1e-4));
            const MetricSeries *com = findSeries(series, "comOverLeadFoot");
            CHECK("comOverLeadFoot's two ankle coefficients are NOT interchangeable",
                  com && com->sigma
                      && !nearRel(*com->sigma,
                                  std::sqrt(0.25 * (sLHip * sLHip + sTHip * sTHip)
                                            + (1.0 + lev2) * sTAnk * sTAnk
                                            + lev2 * sLAnk * sLAnk) / 2.0, 1e-4));
        }
        // The headline numbers, so the swap is visible as two figures rather than as an argument:
        // leadKneeDrift 2.6925824 % → 3.8078866 % and comOverLeadFoot 4.25 % → 4.6703854 % when the
        // lead side moves, while plumbBobDistance holds at 0.6841790 in.
        const auto left  = buildLowerBodySeries(
            trackLowerBody(pose, W, H, true,  2 * dt, {}, 2.0), phases);
        const auto right = buildLowerBodySeries(
            trackLowerBody(pose, W, H, false, 2 * dt, {}, 2.0), phases);
        const MetricSeries *kl = findSeries(left, "leadKneeDrift");
        const MetricSeries *kr = findSeries(right, "leadKneeDrift");
        const MetricSeries *pl = findSeries(left, "plumbBobDistance");
        const MetricSeries *pr = findSeries(right, "plumbBobDistance");
        CHECK("swapping the lead side MOVES the asymmetric σ …",
              kl && kr && kl->sigma && kr->sigma && near(*kl->sigma, 2.6925824, 5e-5)
                  && near(*kr->sigma, 3.8078866, 5e-5));
        CHECK("… and leaves the symmetric one alone",
              pl && pr && pl->sigma && pr->sigma && near(*pl->sigma, 0.6841790, 5e-5)
                  && near(*pr->sigma, 0.6841790, 5e-5));
    }

    // ── 8h) the parity off-switch withholds σ too ───────────────────────────
    //
    // `channel.maxBridgeUs` < 0 means "emit no validity mask at all", and its only purpose is to
    // reproduce the pre-mask bytes for a parity run. σ has to go with the masks, not beside them: a σ
    // written with no mask is a median over the gated and post-Impact frames as well — a DIFFERENT
    // number from the masked one, on a NEW key, in the one run whose whole job is to produce the old
    // bytes. §6d3 pins the mask half of the same switch.
    {
        std::printf("=== 8h) maxBridgeUs < 0 ⇒ no mask AND no σ ===\n");
        const std::vector<Lower> poses(12, sigmaPose());
        const PoseTrack2D pose = sigmaTrack(poses, std::vector<double>(12, 2.0), dt);
        const auto phases = ladder({ { Phase::Address,  2 * dt },
                                     { Phase::Top,      6 * dt },
                                     { Phase::Impact,   8 * dt } });   // a real post-Impact tail
        LowerBodyConfig off; off.maxBridgeUs = -1;
        const auto series = buildLowerBodySeries(
            trackLowerBody(pose, W, H, true, 2 * dt, off, /*mmPerPx=*/2.0), phases);
        bool none = series.size() == 7;
        for (const MetricSeries &m : series)
            none = none && m.valid.empty() && !m.sigma.has_value();
        CHECK("no mask and no σ on any series — the switch restores the old bytes whole", none);

        // The same track WITH the switch on does carry both, so the case is not passing by accident.
        const auto on = buildLowerBodySeries(
            trackLowerBody(pose, W, H, true, 2 * dt, {}, /*mmPerPx=*/2.0), phases);
        bool both = on.size() == 7;
        for (const MetricSeries &m : on) both = both && m.sigma.has_value();
        CHECK("…while the default run has σ on every one of them", both);
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
