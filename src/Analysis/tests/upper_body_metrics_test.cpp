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

// Standalone test for the upper-body frontal-plane metrics
// (src/Analysis/upper_body_metrics.{h,cpp}). Synthetic tracks only — no fixture.
// Mirrors lower_body_metrics_test.cpp in structure and style.
//
// WHAT THIS TEST IS FOR: every channel here carries a SIGN, and a sign is the one thing a synthetic
// track can pin exactly and a corpus cannot. Half the cases below therefore build a pose whose
// answer is known by construction and assert the direction, not just the magnitude — and §5 runs
// the whole suite again through a MIRRORED camera with a left-handed golfer, because a convention
// that only holds for a right-hander filmed from one side is not a convention.

#include "../upper_body_metrics.h"

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

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// COCO-17 body indices.
constexpr int kLSh = 5, kRSh = 6, kLEl = 7, kREl = 8, kLWr = 9, kRWr = 10;
constexpr int kLHip = 11, kRHip = 12, kLAnk = 15, kRAnk = 16;

constexpr int kW = 1000, kH = 1000;   // square frame: normalized == px/1000, so angles are readable

struct Upper {
    QPointF lSh, rSh, lEl, rEl, lWr, rWr, lHip, rHip, lAnk, rAnk;
};

static PoseFrame2D makeUpper(int64_t t, const Upper &p, float conf = 0.9f)
{
    PoseFrame2D f;
    f.t_us = t;
    const auto set = [&](int i, QPointF v) { f.kp[i] = v; f.conf[i] = conf; };
    set(kLSh, p.lSh);   set(kRSh, p.rSh);
    set(kLEl, p.lEl);   set(kREl, p.rEl);
    set(kLWr, p.lWr);   set(kRWr, p.rWr);
    set(kLHip, p.lHip); set(kRHip, p.rHip);
    set(kLAnk, p.lAnk); set(kRAnk, p.rAnk);
    return f;
}

// A square, level address pose. LEAD = LEFT keypoints, and the lead ankle sits at the SMALLER x, so
// the lead side is image −x — the same deliberately awkward arrangement lower_body_metrics_test
// uses, because a producer that assumed lead == +x would pass a friendlier fixture.
static Upper addressPose()
{
    Upper p;
    p.lSh  = QPointF(0.42, 0.30);   p.rSh  = QPointF(0.58, 0.30);
    // 120 px apart, INSIDE the 160 px shoulders. The fixture used to splay them to 200 px, wider
    // than the shoulders, which no address has: the arms hang together at address and separate
    // through the swing, and that ordering is the whole reason the elbow line takes an absolute
    // pixel floor rather than a ratio against its address span (§11).
    p.lEl  = QPointF(0.44, 0.42);   p.rEl  = QPointF(0.56, 0.42);
    p.lWr  = QPointF(0.48, 0.52);   p.rWr  = QPointF(0.52, 0.52);
    p.lHip = QPointF(0.45, 0.55);   p.rHip = QPointF(0.55, 0.55);
    p.lAnk = QPointF(0.44, 0.90);   p.rAnk = QPointF(0.56, 0.90);
    return p;
}

// Mirror a pose about x = 0.5 and swap left/right, which is exactly what a mirrored camera filming
// a left-handed golfer produces: the SAME posture, every image coordinate flipped.
static Upper mirrored(const Upper &p)
{
    const auto mx = [](QPointF v) { return QPointF(1.0 - v.x(), v.y()); };
    Upper m;
    m.lSh  = mx(p.rSh);   m.rSh  = mx(p.lSh);
    m.lEl  = mx(p.rEl);   m.rEl  = mx(p.lEl);
    m.lWr  = mx(p.rWr);   m.rWr  = mx(p.lWr);
    m.lHip = mx(p.rHip);  m.rHip = mx(p.lHip);
    m.lAnk = mx(p.rAnk);  m.rAnk = mx(p.lAnk);
    return m;
}

static PoseTrack2D trackOf(const std::vector<Upper> &poses)
{
    PoseTrack2D t;
    int64_t us = 0;
    for (const Upper &p : poses) {
        t.frames.push_back(makeUpper(us, p));
        us += 10000;   // 100 fps
    }
    return t;
}

// A ladder carrying the four phases this module samples. THE FINISH IS ON IT DELIBERATELY: an
// unsegmented phase now emits no sample at all, where the producer used to coast to the first frame
// and hand back a first-frame reading labelled "Finish". §9 below asserts that all four samples are
// present, and it was passing on the fabricated one — the balance measures read the finish, so a
// frame-0 value under that label would have been graded as the golfer's finish position.
static std::vector<PhaseEvent> phasesAt(int64_t addrUs, int64_t topUs, int64_t impactUs)
{
    return { { Phase::Address, addrUs, 1.f, SegmentRole::Unknown },
             { Phase::Top,     topUs,  1.f, SegmentRole::Unknown },
             { Phase::Impact,  impactUs, 1.f, SegmentRole::Unknown },
             { Phase::Finish,  impactUs + 10000, 1.f, SegmentRole::Unknown } };
}

// Value of a named series at the given phase sample.
static bool seriesAt(const std::vector<MetricSeries> &all, const char *key, Phase p, double &out)
{
    for (const MetricSeries &m : all) {
        if (m.key != QLatin1String(key)) continue;
        for (const PhaseSample &s : m.phaseSamples)
            if (s.phase == p) { out = s.value; return true; }
    }
    return false;
}

static bool hasSeries(const std::vector<MetricSeries> &all, const char *key)
{
    for (const MetricSeries &m : all)
        if (m.key == QLatin1String(key)) return true;
    return false;
}

// The five channels whose PHASE DOMAIN is Address→Impact (design §5.1, and the `domain` field on
// MetricDescriptor). Past impact the body has turned, and a frontal-plane reading of a rotating
// torso is projection rather than the quantity the metric names — so those five sample P1/P4/P7 and
// NOTHING at the finish. The other four are read at the finish (the balance and swing-width
// measures), so they keep the default Address/Top/Impact/Finish.
//
// A domain violation is not a small error: an out-of-domain sample is persisted, drawn and graded
// like any other, so it is a confident reading of a quantity that does not exist at that instant —
// the same class of fault as the ±90° body-line degeneracy the gates in this file exist for.
static bool isAddressToImpactMetric(const QString &key)
{
    return key == QLatin1String("secondaryAxisTilt")
        || key == QLatin1String("spineSideBend")
        || key == QLatin1String("thoraxLateralDrift")
        || key == QLatin1String("shoulderPlaneAngle")
        || key == QLatin1String("elbowAlignment");
}

// How many phase samples a fully segmented swing should give this series: three inside the narrowed
// domain, four outside it.
static size_t expectedPhaseSamples(const QString &key)
{
    return isAddressToImpactMetric(key) ? 3u : 4u;
}

static bool hasPhaseSample(const MetricSeries &m, Phase p)
{
    for (const PhaseSample &s : m.phaseSamples)
        if (s.phase == p) return true;
    return false;
}

// Run the producer over a two-pose track (address held, then the test pose at the Top) and return
// the emitted series.
static std::vector<MetricSeries> runOn(const Upper &addr, const Upper &top, bool leadIsLeft = true)
{
    std::vector<Upper> poses;
    for (int i = 0; i < 6; ++i) poses.push_back(addr);    // a held address for the robust reference
    for (int i = 0; i < 6; ++i) poses.push_back(top);
    const PoseTrack2D track = trackOf(poses);
    const UpperBodyResult r = trackUpperBody(track, kW, kH, leadIsLeft, 20000);
    return buildUpperBodySeries(r, phasesAt(20000, 90000, 110000));
}

int main()
{
    std::printf("=== upper body metrics ===\n");

    // ── 1. Address: a square, level pose reads ~zero on every signed channel ────────────────────
    {
        const Upper a = addressPose();
        const auto series = runOn(a, a);
        CHECK("all nine series produced", series.size() == 9);

        double v = 0.0;
        CHECK("secondaryAxisTilt present", seriesAt(series, "secondaryAxisTilt", Phase::Address, v));
        CHECK("square address ⇒ axis tilt ~0", near(v, 0.0, 0.5));
        CHECK("shoulderPlaneAngle present", seriesAt(series, "shoulderPlaneAngle", Phase::Address, v));
        CHECK("level shoulders ⇒ plane ~0", near(v, 0.0, 0.5));
        CHECK("spineSideBend present", seriesAt(series, "spineSideBend", Phase::Address, v));
        CHECK("level lines ⇒ side bend ~0", near(v, 0.0, 0.5));
        CHECK("elbowAlignment present", seriesAt(series, "elbowAlignment", Phase::Address, v));
        CHECK("level elbows ⇒ elbow line ~0", near(v, 0.0, 0.5));
        CHECK("trailElbowHeight present", seriesAt(series, "trailElbowHeight", Phase::Address, v));
        // The elbows sit 0.12 BELOW the shoulder line; the span is 0.16. −0.12/0.16 = −75 %.
        CHECK("elbow below the shoulder line reads negative", near(v, -75.0, 1.0));
    }

    // ── 2. secondaryAxisTilt is TRAIL-positive ─────────────────────────────────────────────────
    // The one lateral channel that is not lead-positive. Lead is image −x here, so leaning the
    // spine AWAY from the target moves the neck toward +x, and the answer must be POSITIVE.
    {
        const Upper a = addressPose();
        Upper t = a;
        t.lSh = QPointF(0.47, 0.30);   t.rSh = QPointF(0.63, 0.30);   // whole shoulder girdle → +x
        const auto series = runOn(a, t);

        double v = 0.0;
        CHECK("axis tilt at the top", seriesAt(series, "secondaryAxisTilt", Phase::Top, v));
        CHECK("spine leaning away from the target is POSITIVE", v > 3.0);
    }

    // ── 3. spineSideBend is the shoulder line AGAINST the hip line ─────────────────────────────
    // Tilting BOTH lines together is a whole-body lean, not side bend, and must read ~zero. Tilting
    // only the shoulders is side bend and must not.
    {
        const Upper a = addressPose();

        // Both lines tilted by the same ANGLE, which is not the same as the same rise: the
        // shoulders span 0.16 and the hips 0.10, so equal drops would be 14° and 22° and the
        // difference — the thing being measured — would not be zero. Equal ratios, equal angles.
        Upper both = a;
        both.lSh  = QPointF(0.42, 0.28);  both.rSh  = QPointF(0.58, 0.32);   // dy 0.04 / 0.16
        both.lHip = QPointF(0.45, 0.5375); both.rHip = QPointF(0.55, 0.5625); // dy 0.025 / 0.10
        double v = 0.0;
        CHECK("side bend at the top (parallel lines)",
              seriesAt(runOn(a, both), "spineSideBend", Phase::Top, v));
        CHECK("a whole-body lean is NOT side bend", near(v, 0.0, 0.6));

        Upper shoulders = a;                             // trail shoulder dropped, hips level
        shoulders.lSh = QPointF(0.42, 0.28);  shoulders.rSh = QPointF(0.58, 0.34);
        CHECK("side bend at the top (shoulders only)",
              seriesAt(runOn(a, shoulders), "spineSideBend", Phase::Top, v));
        // Trail (right, +x) shoulder LOWER ⇒ shoulder tilt negative ⇒ hip − shoulder positive.
        CHECK("trail shoulder dropping under the turn is POSITIVE side bend", v > 5.0);
    }

    // ── 4. thoraxLateralDrift is measured from the TRAIL ankle, lead-positive ──────────────────
    // NOT address-referenced: the measure over it is a Delta, so the series must carry the absolute
    // distance and let the reducer do the referencing. Address is checked non-zero for that reason.
    {
        const Upper a = addressPose();
        Upper t = a;
        t.lSh = QPointF(0.40, 0.30);   t.rSh = QPointF(0.56, 0.30);   // chest toward the LEAD side (−x)
        t.lHip = QPointF(0.45, 0.55);  t.rHip = QPointF(0.55, 0.55);
        const auto series = runOn(a, t);

        double addr = 0.0, top = 0.0;
        CHECK("thoraxLateralDrift at address", seriesAt(series, "thoraxLateralDrift", Phase::Address, addr));
        CHECK("thoraxLateralDrift at the top", seriesAt(series, "thoraxLateralDrift", Phase::Top, top));
        CHECK("absolute, not address-referenced (address is not zero)", std::fabs(addr) > 10.0);
        CHECK("chest moving toward the lead side INCREASES it", top > addr);
    }

    // ── 5. Every sign survives a mirrored camera and a left-handed golfer ──────────────────────
    // The whole point of the absolute-denominator line form and of resolving lead-ness from the
    // address geometry. Same posture, flipped image, opposite handedness — same numbers.
    {
        const Upper a = addressPose();
        Upper t = a;
        t.lSh = QPointF(0.47, 0.30);   t.rSh = QPointF(0.63, 0.30);   // lean away from the target

        const auto plain    = runOn(a, t, true);
        const auto flipped  = runOn(mirrored(a), mirrored(t), false);

        for (const char *key : { "secondaryAxisTilt", "spineSideBend", "shoulderPlaneAngle",
                                 "elbowAlignment", "trailElbowHeight", "leadHandWidth",
                                 "leadUpperArmToChest", "leadArmToTorso", "thoraxLateralDrift" }) {
            double p = 0.0, f = 0.0;
            const bool okP = seriesAt(plain, key, Phase::Top, p);
            const bool okF = seriesAt(flipped, key, Phase::Top, f);
            std::printf("    %-20s plain %8.3f   mirrored %8.3f\n", key, p, f);
            CHECK(key, okP && okF && near(p, f, 0.5));
        }
    }

    // ── 6. leadArmToTorso: zero when the arm hangs along the torso, larger as it leaves ────────
    {
        const Upper a = addressPose();
        Upper hang = a;
        // Lead arm straight down the torso line: shoulder→elbow parallel to neck→pelvis.
        hang.lSh = QPointF(0.50, 0.30);   hang.rSh = QPointF(0.50, 0.30);
        hang.lHip = QPointF(0.50, 0.55);  hang.rHip = QPointF(0.50, 0.55);
        hang.lEl = QPointF(0.50, 0.45);
        double v = 0.0;
        // Degenerate shoulder/hip lines make the tilts refuse, but the arm-vs-torso angle does not
        // depend on them — which is itself worth pinning: channels fail independently.
        const auto series = runOn(a, hang);
        CHECK("leadArmToTorso at the top", seriesAt(series, "leadArmToTorso", Phase::Top, v));
        CHECK("arm hanging along the torso reads ~0", near(v, 0.0, 2.0));

        Upper out = a;
        out.lEl = QPointF(0.28, 0.30);    // arm straight out to the lead side
        CHECK("leadArmToTorso, arm out", seriesAt(runOn(a, out), "leadArmToTorso", Phase::Top, v));
        CHECK("an arm away from the torso reads much larger", v > 60.0);
    }

    // ── 7. Refusals: no fabricated numbers ─────────────────────────────────────────────────────
    {
        // A shoulder span under the floor is a denominator that cannot carry a percentage.
        Upper narrow = addressPose();
        narrow.lSh = QPointF(0.499, 0.30);   narrow.rSh = QPointF(0.501, 0.30);   // 2 px apart
        const auto tiny = runOn(narrow, narrow);
        CHECK("a sub-floor shoulder span produces NOTHING", tiny.empty());

        // Below the confidence gate every point is unresolved, so there is no reference at all.
        PoseTrack2D dark = trackOf({ addressPose(), addressPose(), addressPose() });
        for (PoseFrame2D &f : dark.frames)
            f.conf.fill(0.05f);
        const UpperBodyResult r = trackUpperBody(dark, kW, kH, true, 0);
        CHECK("an all-low-confidence track produces NOTHING",
              buildUpperBodySeries(r, phasesAt(0, 10000, 20000)).empty());

        // Zero frame dimensions cannot de-normalize anything.
        const UpperBodyResult z = trackUpperBody(trackOf({ addressPose() }), 0, 0, true, 0);
        CHECK("zero frame dims refuse", !z.valid);

        // An empty track is not an error, it is an absence.
        const UpperBodyResult e = trackUpperBody(PoseTrack2D{}, kW, kH, true, 0);
        CHECK("an empty track refuses", !e.valid);
    }

    // ── 8. The smoothed companion track wins when it exists ────────────────────────────────────
    {
        PoseTrack2D t = trackOf(std::vector<Upper>(8, addressPose()));
        Upper tilted = addressPose();
        tilted.lSh = QPointF(0.42, 0.26);   tilted.rSh = QPointF(0.58, 0.34);
        t.smoothed = trackOf(std::vector<Upper>(8, tilted)).frames;

        const UpperBodyResult r = trackUpperBody(t, kW, kH, true, 0);
        const auto series = buildUpperBodySeries(r, phasesAt(0, 40000, 70000));
        double v = 0.0;
        CHECK("shoulderPlaneAngle from the smoothed track",
              seriesAt(series, "shoulderPlaneAngle", Phase::Address, v));
        CHECK("the SMOOTHED pose is what was measured", std::fabs(v) > 20.0);
    }

    // ── 9. Each series samples its DOMAIN's phases, and no others ──────────────────────────────
    {
        const Upper a = addressPose();
        const auto series = runOn(a, a);
        bool perDomain = !series.empty();
        for (const MetricSeries &m : series)
            perDomain = perDomain && m.phaseSamples.size() == expectedPhaseSamples(m.key);
        CHECK("Address/Top/Impact inside the narrowed domain, plus Finish outside it", perDomain);

        // THE DOMAIN RULE, asserted directly rather than inferred from a count. The five
        // Address→Impact channels must never carry a Finish reading: past impact the torso has
        // turned and a frontal-plane angle or lateral distance is measuring the rotation, not the
        // posture — a number that is wrong in a way no corridor can catch, because it looks exactly
        // like a real one. Every one of the nine used to sample the finish, so this had been in every
        // swing.json.
        bool noFinishInDomain = true;
        for (const MetricSeries &m : series)
            if (isAddressToImpactMetric(m.key))
                noFinishInDomain = noFinishInDomain && !hasPhaseSample(m, Phase::Finish);
        CHECK("NO Address→Impact channel emits a Finish sample", noFinishInDomain);

        // …and the four that are genuinely read there still do. The balance and swing-width measures
        // have no other instant to read, so narrowing them too would delete the metric.
        bool finishKept = true;
        for (const MetricSeries &m : series)
            if (!isAddressToImpactMetric(m.key))
                finishKept = finishKept && hasPhaseSample(m, Phase::Finish);
        CHECK("…while the four read at the finish keep it", finishKept);

        // …and when the ladder has NO finish, the sample is absent rather than taken at frame 0.
        // The balance measures read this phase, so a fabricated value here would be graded as the
        // golfer's finish position on any swing the segmenter failed to close out. It is the four
        // finish-samplers that carry this case now — the other five never ask for the phase — so
        // three samples each is the answer for every series either way.
        {
            const std::vector<PhaseEvent> noFinish{
                { Phase::Address, 20000,  1.f, SegmentRole::Unknown },
                { Phase::Top,     90000,  1.f, SegmentRole::Unknown },
                { Phase::Impact,  110000, 1.f, SegmentRole::Unknown } };
            std::vector<Upper> poses;
            for (int i = 0; i < 6; ++i) poses.push_back(a);
            for (int i = 0; i < 6; ++i) poses.push_back(a);
            const UpperBodyResult r = trackUpperBody(trackOf(poses), kW, kH, true, 20000);
            const auto partial = buildUpperBodySeries(r, noFinish);
            bool threeOnly = !partial.empty();
            for (const MetricSeries &m : partial)
                threeOnly = threeOnly && m.phaseSamples.size() == 3;
            CHECK("an unsegmented finish emits NO sample, not a frame-0 one", threeOnly);
        }
        CHECK("no stray keys", hasSeries(series, "leadHandWidth")
                                   && !hasSeries(series, "hipAlignment")
                                   && !hasSeries(series, "shoulderAlignment"));
    }

    // ── 10. The body lines' foreshortening gate ────────────────────────────────────────────────
    //
    // `lineTiltDeg` divides by the line's horizontal separation, so a golfer turning out of the
    // image plane collapses two joints into the same image column and the angle runs to ±90° while
    // the posture it claims to describe has not changed. On the swing that prompted this work
    // `shoulderPlaneAngle` reads +88° AT THE TOP — a GRADED phase sample. Below
    // upperBody.minShoulderSpanRatio the frame has NO shoulder line and is absent from the channel;
    // the resample still bridges it so the curve stays continuous, and MetricSeries::valid says
    // which of that curve is a measurement.
    //
    // The track rotates about the vertical from square to 80° over 20 frames, so |dx| / address |dx|
    // is exactly cos θ and the 0.40 gate bites at 66.4° — frames 16..19 of the turn, grid 22..25.
    {
        const double kPi = 3.14159265358979323846;
        const int kAddr = 6, kTurn = 20;

        const auto find = [](const std::vector<MetricSeries> &all,
                             const char *key) -> const MetricSeries * {
            for (const MetricSeries &m : all)
                if (m.key == QLatin1String(key)) return &m;
            return nullptr;
        };
        const auto maskedTail = [](const MetricSeries *m) {
            if (m == nullptr || m->valid.size() != m->t_us.size()) return false;
            for (size_t i = 0; i < m->valid.size(); ++i)
                if (m->valid[i] != (i >= 22 ? 0u : 1u)) return false;
            return true;
        };

        // Address at 20 ms; Top on a frame the gate PASSES (200 ms, θ = 58.9°); Impact and Finish
        // inside the run it REFUSES (240 / 250 ms). One sample must survive and two must not exist.
        const std::vector<PhaseEvent> phases{
            { Phase::Address, 20000,  1.f, SegmentRole::Unknown },
            { Phase::Top,     200000, 1.f, SegmentRole::Unknown },
            { Phase::Impact,  240000, 1.f, SegmentRole::Unknown },
            { Phase::Finish,  250000, 1.f, SegmentRole::Unknown } };

        // The address window must cover the HELD ADDRESS ONLY: with the shipped 250 ms the reference
        // median would be taken over the turn as well, and the ratio's denominator would be the very
        // foreshortening it is supposed to detect.
        UpperBodyConfig cfg; cfg.addrWindowUs = 30000;

        const auto rotate = [&](bool shoulders, bool hips) {
            std::vector<Upper> poses;
            for (int k = 0; k < kAddr; ++k) poses.push_back(addressPose());
            for (int k = 0; k < kTurn; ++k) {
                const double th   = 80.0 * k / double(kTurn - 1);
                const double c    = std::cos(th * kPi / 180.0);
                const double rise = 0.020 * k / double(kTurn - 1);
                Upper p = addressPose();
                if (shoulders) {
                    p.lSh = QPointF(0.50 - 0.08 * c, 0.30);
                    p.rSh = QPointF(0.50 + 0.08 * c, 0.30 - rise);
                }
                if (hips) {
                    p.lHip = QPointF(0.50 - 0.05 * c, 0.55);
                    p.rHip = QPointF(0.50 + 0.05 * c, 0.55 - rise);
                }
                poses.push_back(p);
            }
            const UpperBodyResult r = trackUpperBody(trackOf(poses), kW, kH, true, 20000, cfg);
            return buildUpperBodySeries(r, phases);
        };

        // (a) The shoulders turn; the hips stay square.
        {
            const auto series = rotate(true, false);
            const MetricSeries *sp = find(series, "shoulderPlaneAngle");
            CHECK("shoulderPlaneAngle still emitted — a gated run is not a refused metric",
                  sp != nullptr);
            CHECK("its mask is 0 across the foreshortened run and 1 elsewhere", maskedTail(sp));
            if (sp) {
                double worst = 0.0;
                for (double v : sp->value) worst = std::max(worst, std::fabs(v));
                // Ungated, the last frame reads atan2(20 px, 27.8 px) ≈ 36° and is heading for 90°.
                CHECK("nothing in the series approaches ±90°", worst < 25.0);
                // TWO different absences, and they must not be confused. This channel's domain is
                // Address→Impact, so it never asks for the Finish at all; of the three it does ask
                // for, the Impact instant sits inside the gated run and is refused. What is left is
                // Address and Top.
                bool addrAndTopOnly = sp->phaseSamples.size() == 2;
                for (const PhaseSample &s : sp->phaseSamples)
                    addrAndTopOnly = addrAndTopOnly
                                     && (s.phase == Phase::Address || s.phase == Phase::Top);
                CHECK("A PHASE INSTANT INSIDE THE INVALID RUN EMITS NO SAMPLE (Impact)",
                      addrAndTopOnly);
                CHECK("…and the out-of-domain Finish was never asked for in the first place",
                      !hasPhaseSample(*sp, Phase::Finish));
            }

            // spineSideBend is a DIFFERENCE of two tilts, so a degenerate shoulder line poisons it
            // even though the hip line is perfect.
            CHECK("spineSideBend goes with the shoulder line", maskedTail(find(series, "spineSideBend")));

            // trailElbowHeight is a HEIGHT, not a tilt, and is gated all the same: heightAboveLine
            // interpolates the shoulder line's y at the elbow's x, so it divides by the very dx that
            // is collapsing. It is the worse case of the two — an angle saturates at 90°, an
            // unbounded % shoulder width does not — so it carries shoulderPlaneAngle's mask exactly.
            CHECK("trailElbowHeight is masked on the same run as the shoulder line it divides by",
                  maskedTail(find(series, "trailElbowHeight")));

            // The elbows never turned, so their line was never gated.
            const MetricSeries *el = find(series, "elbowAlignment");
            CHECK("elbowAlignment is untouched — a different line, its own span",
                  el && el->valid.empty()
                     && el->phaseSamples.size() == expectedPhaseSamples(el->key));

            // The audited un-gated set: their divisors are address constants or Euclidean lengths,
            // which do not vanish as the body turns, so withholding them would withhold a real
            // measurement. secondaryAxisTilt divides by the VERTICAL neck→pelvis rise, and the neck
            // is the shoulders' MIDPOINT — which is exactly as well located when they foreshorten.
            for (const char *key : { "secondaryAxisTilt", "thoraxLateralDrift", "leadHandWidth",
                                     "leadUpperArmToChest", "leadArmToTorso" }) {
                const MetricSeries *m = find(series, key);
                // Sample count per the metric's DOMAIN — three for the Address→Impact pair at the
                // front of this list, four for the three that are read at the finish.
                CHECK(key, m && m->valid.empty()
                             && m->phaseSamples.size() == expectedPhaseSamples(m->key));
            }
        }

        // (b) The hips turn; the shoulders stay square. The other half of "BOTH lines valid".
        {
            const auto series = rotate(false, true);
            CHECK("spineSideBend goes with the HIP line too", maskedTail(find(series, "spineSideBend")));
            const MetricSeries *sp = find(series, "shoulderPlaneAngle");
            CHECK("…while the shoulder line, which never turned, is unmasked",
                  sp && sp->valid.empty()
                     && sp->phaseSamples.size() == expectedPhaseSamples(sp->key));
        }

        // (c) The gate that never fires changes NOTHING. This is the promise the corpus gate is
        // judged on: a swing with no gated frame serialises exactly as it did before
        // MetricSeries::valid existed. EMPTY means all valid — never an all-ones array.
        {
            const auto series = rotate(false, false);
            bool untouched = series.size() == 9;
            for (const MetricSeries &m : series)
                untouched = untouched && m.valid.empty()
                            && m.phaseSamples.size() == expectedPhaseSamples(m.key);
            CHECK("no gated frame ⇒ every mask EMPTY and every phase sample its domain allows",
                  untouched);
        }
    }

    // ── 11. The elbow line's ABSOLUTE floor, and a gated run mid-track ─────────────────────────
    //
    // Why this gate is not a ratio like the other two: the elbows are at their NARROWEST at address
    // (the arms hang together and separate through the swing), so |dx| / address |dx| is 1.0 at
    // address by construction and ≥1 after it. A ratio could never fire — least of all at address,
    // which is exactly where `elbowAlignment` is read and where a 20 px separation is pure keypoint
    // noise. Only an absolute floor can refuse the frame the metric is graded on.
    {
        const auto find = [](const std::vector<MetricSeries> &all,
                             const char *key) -> const MetricSeries * {
            for (const MetricSeries &m : all)
                if (m.key == QLatin1String(key)) return &m;
            return nullptr;
        };
        // 15 px apart on a 1000 px frame — under the 25 px floor.
        const auto narrowElbows = [] {
            Upper p = addressPose();
            p.lEl = QPointF(0.4925, 0.42);   p.rEl = QPointF(0.5075, 0.42);
            return p;
        };

        // (a) Narrow throughout ⇒ the metric does not exist. Not a 0°, not a noisy angle: absent.
        {
            std::vector<Upper> poses(12, narrowElbows());
            const UpperBodyResult r = trackUpperBody(trackOf(poses), kW, kH, true, 20000);
            const auto series = buildUpperBodySeries(r, phasesAt(20000, 60000, 100000));
            CHECK("a 15 px elbow separation produces NO elbowAlignment at all",
                  !hasSeries(series, "elbowAlignment"));
            CHECK("…and costs nothing else — the shoulder line is untouched",
                  hasSeries(series, "shoulderPlaneAngle") && series.size() == 8);
        }

        // (b) GATED is not the same as MISSING, mid-track. §10's runs sit at the tail, where the
        // extent rule marks them on its own; a run INSIDE the track can only be marked by the mask's
        // own reasoning, and the first version of that reasoning got it wrong — on a real swing a
        // 23-frame gated shoulder run came back with 30 of its frames flagged VALID because each sat
        // within 60 ms of a measurement, and a P4 sample of 25.8° was emitted from the bridge.
        //
        // Two holes, the same length (10 frames) at the same spacing (7 ms ≈ 150 fps), OPPOSITE
        // answers:
        //   frames 10–19  elbows CONFIDENT, 15 px apart ⇒ the line is refused ⇒ GATED ⇒ 0.
        //   frames 30–39  every keypoint below the confidence gate ⇒ MISSING ⇒ bridged, valid.
        {
            const int64_t dt7 = 7000;
            PoseTrack2D t;
            for (int k = 0; k < 50; ++k) {
                const bool gated = (k >= 10 && k <= 19);
                const bool dark  = (k >= 30 && k <= 39);
                t.frames.push_back(makeUpper(k * dt7, gated ? narrowElbows() : addressPose(),
                                             dark ? 0.05f : 0.9f));
            }
            UpperBodyConfig cfg; cfg.addrWindowUs = 30000;   // the reference is the opening frames
            const std::vector<PhaseEvent> phases{
                { Phase::Address, 2 * dt7,  1.f, SegmentRole::Unknown },
                { Phase::Top,     15 * dt7, 1.f, SegmentRole::Unknown },   // inside the GATED run
                { Phase::Impact,  35 * dt7, 1.f, SegmentRole::Unknown },   // inside the CONF hole
                { Phase::Finish,  49 * dt7, 1.f, SegmentRole::Unknown } };
            const UpperBodyResult r = trackUpperBody(t, kW, kH, true, 2 * dt7, cfg);
            CHECK("the ten refused frames are recorded as GATED, not merely absent",
                  r.gatedElbowLine.size() == 10 && r.gatedElbowLine.front() == 10 * dt7
                      && r.gatedElbowLine.back() == 19 * dt7);

            const auto series = buildUpperBodySeries(r, phases);
            const MetricSeries *el = find(series, "elbowAlignment");
            CHECK("elbowAlignment emitted", el != nullptr);
            if (el) {
                bool exact = el->valid.size() == 50;
                for (size_t i = 0; i < el->valid.size(); ++i)
                    exact = exact && (el->valid[i] == ((i >= 10 && i <= 19) ? 0u : 1u));
                CHECK("ALL TEN gated frames are 0 — no budget excuses refused geometry", exact);
                CHECK("the same-length CONFIDENCE hole stays valid, bridged as it always was",
                      el->valid.size() == 50 && el->valid[30] == 1u && el->valid[35] == 1u
                          && el->valid[39] == 1u);
                CHECK("the mask starts and ends VALID, so none of it came from the extent rule",
                      el->valid.size() == 50 && el->valid.front() == 1u && el->valid.back() == 1u);
                bool top = false, impact = false;
                for (const PhaseSample &s : el->phaseSamples) {
                    if (s.phase == Phase::Top)    top = true;
                    if (s.phase == Phase::Impact) impact = true;
                }
                CHECK("a phase instant inside the GATED run emits nothing", !top);
                CHECK("…while one inside the confidence hole still does", impact);
            }

            // The shoulder line was never refused on this track, and its only hole is the confidence
            // one, inside the budget. Nothing gated, nothing over budget, so: no mask at all.
            const MetricSeries *sp = find(series, "shoulderPlaneAngle");
            CHECK("a channel with nothing gated and no over-budget hole carries NO mask",
                  sp && sp->valid.empty());
        }
    }

    // ── 12. One ratio, TWO references: the whole-swing-absent path ─────────────────────────────
    //
    // `LowerBodyState::hipLineValid` is NOT how this module learns about the hip line — the two
    // modules are separate analysis stages with no shared result, so each resolves the line against
    // its own address reference. This module's reference admission test asks for shoulders, ankles
    // and the lead arm, NOT the hips, so an address whose hips were unconfident leaves it with no hip
    // denominator at all — and then `spineSideBend` is absent for the WHOLE swing while the
    // lower-body module, whose admission test does require the hips, still produces `hipLineTilt`.
    //
    // That is a real asymmetry and it is pinned here so it is documented behaviour rather than a
    // silent one. A ratio with no denominator is not a measurement, and inventing one from the
    // mid-swing frames would put the gate's denominator inside the collapse it is meant to detect.
    {
        std::vector<Upper> poses(12, addressPose());
        PoseTrack2D t = trackOf(poses);
        for (size_t i = 0; i < 6; ++i) {                    // the address block only
            t.frames[i].conf[kLHip] = 0.05f;
            t.frames[i].conf[kRHip] = 0.05f;
        }
        UpperBodyConfig cfg; cfg.addrWindowUs = 30000;      // so the reference IS that block
        const UpperBodyResult r = trackUpperBody(t, kW, kH, true, 20000, cfg);
        CHECK("the reference still resolves without the hips", r.ref.valid);
        CHECK("…but it has no hip denominator", r.ref.hipDxPx == 0.0);
        const auto series = buildUpperBodySeries(r, phasesAt(20000, 60000, 100000));
        CHECK("spineSideBend is absent for the whole swing, not gated frame by frame",
              !hasSeries(series, "spineSideBend"));
        CHECK("…while the shoulder line, which HAS its reference, is produced",
              hasSeries(series, "shoulderPlaneAngle"));
    }

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
