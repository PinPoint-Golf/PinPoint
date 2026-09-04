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
        const MetricSeries *sway = findSeries(series, "pelvisSway");
        CHECK("pelvisSway is not gated by the hip line",
              sway && sway->valid.empty() && hasPhase(*sway, Phase::Impact));

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
            bool exact = tilt->valid.size() == 50;
            for (size_t i = 0; i < tilt->valid.size(); ++i)
                exact = exact && (tilt->valid[i] == ((i >= 10 && i <= 19) ? 0u : 1u));
            CHECK("ALL TEN gated frames are 0 — no budget excuses refused geometry", exact);
            CHECK("the same-length CONFIDENCE hole stays valid, bridged as it always was",
                  tilt->valid.size() == 50 && tilt->valid[30] == 1u && tilt->valid[35] == 1u
                      && tilt->valid[39] == 1u);
            CHECK("the mask starts and ends VALID, so none of it came from the extent rule",
                  tilt->valid.size() == 50 && tilt->valid.front() == 1u && tilt->valid.back() == 1u);
            CHECK("a phase instant inside the GATED run emits nothing", !hasPhase(*tilt, Phase::Top));
            CHECK("…while one inside the confidence hole still does", hasPhase(*tilt, Phase::Impact));
        }

        // pelvisSway has no geometric gate at all, so its only hole is the confidence one — inside
        // the budget, therefore EMPTY. Nothing was gated, nothing exceeded the budget, no mask.
        const MetricSeries *sway = findSeries(series, "pelvisSway");
        CHECK("an ungated channel with a bridged hole carries NO mask at all",
              sway && sway->valid.empty() && hasPhase(*sway, Phase::Impact));
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
        const auto phases = ladder({ { Phase::Address, 2 * dt },
                                     { Phase::Top,     8 * dt },
                                     { Phase::Impact, 11 * dt } });
        const auto series = buildLowerBodySeries(res, phases);

        CHECK("the hip line is never gated by a pure tilt",
              res.hipTilt.t_us.size() == res.states.size());
        bool allEmpty = !series.empty();
        for (const MetricSeries &m : series) allEmpty = allEmpty && m.valid.empty();
        CHECK("every series leaves `valid` EMPTY", allEmpty);

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
