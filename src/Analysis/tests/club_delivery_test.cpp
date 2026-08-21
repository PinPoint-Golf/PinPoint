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

// Standalone test for the face-on club-delivery metrics (src/Analysis/club_delivery.{h,cpp}).
// Synthetic shaft tracks only — no fixture, no video, no OpenCV.
//
// THE THREE THINGS THIS TEST EXISTS TO PIN:
//
//   1. PROJECTED HEADS ARE EXCLUDED FROM THE ANGLES, not down-weighted. headPx is only sometimes a
//      measurement; without the Stage-2 head pass it is reconstructed from the grip along the shaft
//      at an assumed length, and its "velocity" is the grip's. §5 builds a track that is entirely
//      projected and asserts NOTHING comes out — because something plausible coming out is the
//      failure mode that would never be noticed.
//   2. THE SIGNS. Past parallel is positive, an upward strike is positive, a low point ahead of the
//      ball is positive — and none of them may invert on a mirrored camera. §4 swings the other way
//      across the frame and asserts the low point keeps its sign.
//   3. THE SUB-FRAME VERTEX. At impact speeds one frame of quantisation is inches of low point, so
//      the parabola refinement is not a nicety; §3 places the true vertex deliberately between two
//      samples and checks it is found there.
//   4. THE TWO CHANNELS ARE SEPARATE AND FAIL SEPARATELY. The angles read the MEASURED heads in
//      samples[]; lowPointAhead reads the SYNTHESIZED arc in synth[] and never looks at a measured
//      head at all. §7 pins that both ways round — an arc with no measured heads still produces a
//      low point, and measured heads with no arc still produce the angles — because the whole point
//      of moving the low point onto the arc was that the head detector goes dark through impact.
//   5. AN ARC THAT NEVER BOTTOMS OUT PRODUCES NOTHING. A vertex on either end of the window means
//      the arc was still descending (or still climbing) when the data ran out, and the "low point"
//      is then just the last sample. §8 pins the refusal.

#include "../club_delivery.h"

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

static ShaftSample2D sample(int64_t t, QPointF grip, QPointF head, bool measured = true)
{
    ShaftSample2D s;
    s.t_us     = t;
    s.gripPx   = grip;
    s.headPx   = head;
    s.conf     = 0.9f;
    s.flags    = ShaftMeasured;
    s.headConf = 0.8f;
    if (!measured) {
        s.flags |= ShaftHeadProjected;
        s.headConf = -1.f;          // the Stage-2 pass never ran for this sample
    }
    return s;
}

static ShaftTrack2D trackOf(std::vector<ShaftSample2D> samples,
                           std::vector<ShaftSample2D> synth = {})
{
    ShaftTrack2D t;
    t.valid       = true;
    t.frameWidth  = 1920;
    t.frameHeight = 1080;
    t.samples     = std::move(samples);
    t.synth       = std::move(synth);
    return t;
}

static std::vector<PhaseEvent> ladder(int64_t topUs, int64_t impactUs)
{
    return { { Phase::Address, 0, 1.f, SegmentRole::Unknown },
             { Phase::Top,     topUs, 1.f, SegmentRole::Unknown },
             { Phase::Impact,  impactUs, 1.f, SegmentRole::Unknown } };
}

static const MetricSeries *find(const std::vector<MetricSeries> &all, const char *key)
{
    for (const MetricSeries &m : all)
        if (m.key == QLatin1String(key)) return &m;
    return nullptr;
}

static bool scalarOf(const std::vector<MetricSeries> &all, const char *key, double &out)
{
    const MetricSeries *m = find(all, key);
    if (!m || m->phaseSamples.empty()) return false;
    out = m->phaseSamples.front().value;
    return true;
}

// A parabolic arc through the impact zone: the head sweeps in +x and its height is a quadratic
// with its vertex (the LOWEST point, so the LARGEST image y) at xVertex.
static std::vector<ShaftSample2D> arc(double xVertex, double dir, int64_t impactUs, int n = 11)
{
    std::vector<ShaftSample2D> out;
    for (int i = 0; i < n; ++i) {
        const double x  = 900.0 + dir * (i - n / 2) * 20.0;
        const double dx = x - xVertex;
        const double y  = 800.0 - 0.002 * dx * dx;         // max y at the vertex
        const int64_t t = impactUs + int64_t(i - n / 2) * 4000;
        // The grip is OFFSET from the head, so the shaft is not vertical: shaftAngleVsHorizontal
        // divides by the absolute horizontal grip→head separation and refuses a stacked pair, which
        // is correct behaviour and made the first version of this fixture emit nothing.
        out.push_back(sample(t, QPointF(x - dir * 250.0, 400.0), QPointF(x, y)));
    }
    return out;
}

// The SYNTHESIZED arc — the series lowPointAhead is actually read from. Same parabola as `arc()`,
// but flagged ShaftSynthesized and parked in ShaftTrack2D::synth, which is where club_delivery
// looks for a low point and deliberately NOT where the two angles look. In production this is the
// Hermite interpolation between the located P-anchors that the club overlay draws.
static std::vector<ShaftSample2D> synthArc(double xVertex, double dir, int64_t impactUs, int n = 11)
{
    std::vector<ShaftSample2D> out = arc(xVertex, dir, impactUs, n);
    for (ShaftSample2D &s : out) {
        s.flags    = ShaftSynthesized;
        s.headConf = -1.f;                  // a synthesized head is not a measurement, and says so
    }
    return out;
}

// A monotone descending run: the head is still falling when the window ends, so there is no turning
// point inside it. What a truncated arc looks like — the next P-anchor is hundreds of ms away.
static std::vector<ShaftSample2D> fallingArc(int64_t impactUs, int n = 11)
{
    std::vector<ShaftSample2D> out;
    for (int i = 0; i < n; ++i) {
        const double x = 900.0 + (i - n / 2) * 20.0;
        ShaftSample2D s = sample(impactUs + int64_t(i - n / 2) * 4000,
                                 QPointF(x - 250.0, 400.0),
                                 QPointF(x, 700.0 + i * 8.0));   // y grows throughout = still sinking
        s.flags    = ShaftSynthesized;
        s.headConf = -1.f;
        out.push_back(s);
    }
    return out;
}

int main()
{
    std::printf("=== club delivery ===\n");

    constexpr int64_t kImpact = 200000;
    constexpr double  kMmPerPx = 2.0;                       // 1 px = 2 mm ⇒ 12.7 px = 1 inch

    // ── 1. shaftAngleVsHorizontal: zero is parallel, positive is PAST parallel ─────────────────
    {
        // Head level with the grip: exactly parallel to the ground.
        const auto level = trackOf({ sample(0, QPointF(500, 300), QPointF(900, 300)),
                                     sample(10000, QPointF(500, 300), QPointF(900, 300)) });
        const auto r = trackClubDelivery(level, ladder(10000, kImpact), QPointF(), false, -1.0);
        // NOTE the named locals throughout this file. find() returns a pointer INTO the vector, so
        // a `find(buildClubDeliverySeries(...))` one-liner dangles the moment the full expression
        // ends. It crashed exactly that way the first time this test ran.
        const auto s = buildClubDeliverySeries(r, ladder(10000, kImpact));
        const MetricSeries *m = find(s, "shaftAngleVsHorizontal");
        CHECK("shaftAngleVsHorizontal emitted", m != nullptr);
        CHECK("head level with the grip is ZERO", m && near(m->value.front(), 0.0, 0.01));

        // Head BELOW the grip: the club has travelled past horizontal.
        const auto past = trackOf({ sample(0, QPointF(500, 300), QPointF(900, 400)),
                                    sample(10000, QPointF(500, 300), QPointF(900, 400)) });
        const auto pastSeries = buildClubDeliverySeries(
            trackClubDelivery(past, ladder(10000, kImpact), QPointF(), false, -1.0),
            ladder(10000, kImpact));
        m = find(pastSeries, "shaftAngleVsHorizontal");
        CHECK("head below the grip is PAST parallel (positive)", m && m->value.front() > 10.0);

        // Head ABOVE the grip: short of parallel.
        const auto shortOf = trackOf({ sample(0, QPointF(500, 300), QPointF(900, 200)),
                                       sample(10000, QPointF(500, 300), QPointF(900, 200)) });
        const auto shortSeries = buildClubDeliverySeries(
            trackClubDelivery(shortOf, ladder(10000, kImpact), QPointF(), false, -1.0),
            ladder(10000, kImpact));
        m = find(shortSeries, "shaftAngleVsHorizontal");
        CHECK("head above the grip is SHORT of parallel (negative)", m && m->value.front() < -10.0);

        // Swinging the other way across the frame describes the same club position.
        const auto mirroredPast = trackOf({ sample(0, QPointF(900, 300), QPointF(500, 400)),
                                            sample(10000, QPointF(900, 300), QPointF(500, 400)) });
        const auto mirroredSeries = buildClubDeliverySeries(
            trackClubDelivery(mirroredPast, ladder(10000, kImpact), QPointF(), false, -1.0),
            ladder(10000, kImpact));
        m = find(mirroredSeries, "shaftAngleVsHorizontal");
        CHECK("a mirrored camera reads the same past-parallel angle",
              m && m->value.front() > 10.0);
    }

    // ── 2. attackAngle: positive is UPWARD ─────────────────────────────────────────────────────
    {
        // A head descending as it moves: y increasing.
        std::vector<ShaftSample2D> down;
        for (int i = 0; i < 11; ++i)
            down.push_back(sample(kImpact + int64_t(i - 5) * 4000,
                                  QPointF(600 + i * 20.0, 400),
                                  QPointF(800 + i * 20.0, 700 + i * 4.0)));
        double aa = 0.0;
        auto s = buildClubDeliverySeries(
            trackClubDelivery(trackOf(down), ladder(10000, kImpact), QPointF(), false, -1.0),
            ladder(10000, kImpact));
        CHECK("attackAngle emitted", scalarOf(s, "attackAngle", aa));
        CHECK("a descending strike is NEGATIVE", aa < -5.0);

        // Rising: y decreasing.
        std::vector<ShaftSample2D> up;
        for (int i = 0; i < 11; ++i)
            up.push_back(sample(kImpact + int64_t(i - 5) * 4000,
                                QPointF(600 + i * 20.0, 400),
                                QPointF(800 + i * 20.0, 700 - i * 4.0)));
        s = buildClubDeliverySeries(
            trackClubDelivery(trackOf(up), ladder(10000, kImpact), QPointF(), false, -1.0),
            ladder(10000, kImpact));
        CHECK("attackAngle emitted (rising)", scalarOf(s, "attackAngle", aa));
        CHECK("an ascending strike is POSITIVE", aa > 5.0);

        // And the sign does not depend on which way the head is travelling across the frame.
        std::vector<ShaftSample2D> upBack;
        for (int i = 0; i < 11; ++i)
            upBack.push_back(sample(kImpact + int64_t(i - 5) * 4000,
                                    QPointF(600 - i * 20.0, 400),
                                    QPointF(800 - i * 20.0, 700 - i * 4.0)));
        s = buildClubDeliverySeries(
            trackClubDelivery(trackOf(upBack), ladder(10000, kImpact), QPointF(), false, -1.0),
            ladder(10000, kImpact));
        CHECK("attackAngle emitted (mirrored)", scalarOf(s, "attackAngle", aa));
        CHECK("a mirrored camera still reads UP as positive", aa > 5.0);
    }

    // ── 3. lowPointAhead, including the sub-frame vertex ───────────────────────────────────────
    {
        // Ball at x = 900. Vertex deliberately at 910 — 10 px, i.e. BETWEEN samples spaced 20 px.
        const auto t = trackOf(arc(910.0, +1.0, kImpact), synthArc(910.0, +1.0, kImpact));
        auto s = buildClubDeliverySeries(
            trackClubDelivery(t, ladder(10000, kImpact), QPointF(900, 800), true, kMmPerPx),
            ladder(10000, kImpact));
        double lp = 0.0;
        CHECK("lowPointAhead emitted", scalarOf(s, "lowPointAhead", lp));
        // 10 px × 2 mm/px ÷ 25.4 = 0.787 in. Frame-quantised it would read 0 or 1.57.
        CHECK("the vertex is found BETWEEN samples", near(lp, 0.787, 0.15));
        CHECK("a low point past the ball is POSITIVE", lp > 0.0);

        // THE HEALTH WARNING IS PART OF THE READING. The arc is an interpolation, not an
        // observation, so the metric ships a published 1σ; a low point with no σ would read as a
        // measurement, which is the one thing it is not.
        const MetricSeries *lpS = find(s, "lowPointAhead");
        CHECK("lowPointAhead publishes a sigma", lpS != nullptr && lpS->sigma.has_value());
        CHECK("and it is the tuned ±2 in",
              lpS != nullptr && lpS->sigma.has_value()
                  && near(*lpS->sigma, pinpoint::tuned::clubDelivery::kLowPointSigmaIn, 1e-9));
        // The two angles are measured, not estimated, and must NOT borrow the arc's uncertainty.
        const MetricSeries *aaS = find(s, "attackAngle");
        CHECK("attackAngle does not inherit that sigma",
              aaS != nullptr && !aaS->sigma.has_value());

        // Vertex BEHIND the ball — the fat / scoop signature.
        const auto behind = trackOf(arc(880.0, +1.0, kImpact), synthArc(880.0, +1.0, kImpact));
        s = buildClubDeliverySeries(
            trackClubDelivery(behind, ladder(10000, kImpact), QPointF(900, 800), true, kMmPerPx),
            ladder(10000, kImpact));
        CHECK("lowPointAhead emitted (behind)", scalarOf(s, "lowPointAhead", lp));
        CHECK("a low point behind the ball is NEGATIVE", lp < 0.0);
    }

    // ── 4. The target direction comes from the HEAD, not from handedness ───────────────────────
    // Swinging right-to-left across the frame is the same strike filmed from the other side. The
    // sign must not invert.
    {
        const auto rightward = trackOf(arc(910.0, +1.0, kImpact), synthArc(910.0, +1.0, kImpact));
        const auto leftward  = trackOf(arc(890.0, -1.0, kImpact), synthArc(890.0, -1.0, kImpact));
        double a = 0.0, b = 0.0;
        scalarOf(buildClubDeliverySeries(
                     trackClubDelivery(rightward, ladder(10000, kImpact), QPointF(900, 800), true,
                                       kMmPerPx),
                     ladder(10000, kImpact)),
                 "lowPointAhead", a);
        scalarOf(buildClubDeliverySeries(
                     trackClubDelivery(leftward, ladder(10000, kImpact), QPointF(900, 800), true,
                                       kMmPerPx),
                     ladder(10000, kImpact)),
                 "lowPointAhead", b);
        std::printf("    rightward %.3f in   leftward %.3f in\n", a, b);
        CHECK("both swings read the low point AHEAD", a > 0.0 && b > 0.0);
        CHECK("and by the same amount", near(a, b, 0.2));
    }

    // ── 5. A PROJECTED head produces nothing at all ────────────────────────────────────────────
    {
        std::vector<ShaftSample2D> projected;
        for (int i = 0; i < 11; ++i)
            projected.push_back(sample(kImpact + int64_t(i - 5) * 4000,
                                       QPointF(600 + i * 20.0, 400),
                                       QPointF(800 + i * 20.0, 700 + i * 4.0), false));
        const auto r = trackClubDelivery(trackOf(projected), ladder(10000, kImpact),
                                         QPointF(900, 800), true, kMmPerPx);
        CHECK("an all-projected track is INVALID", !r.valid);
        CHECK("and emits no series",
              buildClubDeliverySeries(r, ladder(10000, kImpact)).empty());
    }

    // ── 6. Refusals that must not cascade ──────────────────────────────────────────────────────
    {
        const auto t = trackOf(arc(910.0, +1.0, kImpact), synthArc(910.0, +1.0, kImpact));

        // No ruler: low point is absent, the two angles are unaffected. They are scale-free and
        // must not be taken down with it.
        auto s = buildClubDeliverySeries(
            trackClubDelivery(t, ladder(10000, kImpact), QPointF(900, 800), true, -1.0),
            ladder(10000, kImpact));
        CHECK("no ruler ⇒ no lowPointAhead", find(s, "lowPointAhead") == nullptr);
        CHECK("but shaftAngleVsHorizontal survives", find(s, "shaftAngleVsHorizontal") != nullptr);
        CHECK("and attackAngle survives", find(s, "attackAngle") != nullptr);

        // No ball: same story.
        s = buildClubDeliverySeries(
            trackClubDelivery(t, ladder(10000, kImpact), QPointF(), false, kMmPerPx),
            ladder(10000, kImpact));
        CHECK("no ball ⇒ no lowPointAhead", find(s, "lowPointAhead") == nullptr);
        CHECK("the angles still land", find(s, "attackAngle") != nullptr);

        // An invalid track is nothing.
        ShaftTrack2D invalid = t;
        invalid.valid = false;
        CHECK("an invalid track refuses",
              !trackClubDelivery(invalid, ladder(10000, kImpact), QPointF(900, 800), true,
                                 kMmPerPx).valid);

        // Too few ARC samples around impact to trust a vertex. (In production ±60 ms of a 240 Hz
        // synthesis is ~29 samples, so this floor catches a truncated arc rather than a sparse one.)
        const auto sparse = trackOf(arc(910.0, +1.0, kImpact),
                                    synthArc(910.0, +1.0, kImpact, 3));
        s = buildClubDeliverySeries(
            trackClubDelivery(sparse, ladder(10000, kImpact), QPointF(900, 800), true, kMmPerPx),
            ladder(10000, kImpact));
        CHECK("too few impact-zone samples ⇒ no low point", find(s, "lowPointAhead") == nullptr);
    }

    // ── 7. The two channels are separate, and fail separately ──────────────────────────────────
    // This is the whole reason the low point moved onto the arc: on a real swing the head detector
    // goes dark through impact while the arc is still there. A metric that needed both would have
    // gained nothing.
    {
        // An arc with NO measured heads at all — every sample projected. The angles must stay
        // silent (their input really is absent) and the low point must still land.
        std::vector<ShaftSample2D> allProjected = arc(910.0, +1.0, kImpact);
        for (ShaftSample2D &x : allProjected) { x.flags |= ShaftHeadProjected; x.headConf = -1.f; }
        auto s = buildClubDeliverySeries(
            trackClubDelivery(trackOf(allProjected, synthArc(910.0, +1.0, kImpact)),
                              ladder(10000, kImpact), QPointF(900, 800), true, kMmPerPx),
            ladder(10000, kImpact));
        double lp = 0.0;
        CHECK("no measured head anywhere ⇒ lowPointAhead STILL lands",
              scalarOf(s, "lowPointAhead", lp));
        CHECK("and it is the same vertex the measured fixture found", near(lp, 0.787, 0.15));
        CHECK("but the angles stay silent", find(s, "attackAngle") == nullptr
                                            && find(s, "shaftAngleVsHorizontal") == nullptr);

        // The mirror image: measured heads, no synthesized arc. Angles land, low point does not.
        s = buildClubDeliverySeries(
            trackClubDelivery(trackOf(arc(910.0, +1.0, kImpact)), ladder(10000, kImpact),
                              QPointF(900, 800), true, kMmPerPx),
            ladder(10000, kImpact));
        CHECK("no arc ⇒ no lowPointAhead", find(s, "lowPointAhead") == nullptr);
        CHECK("but the angles are unaffected", find(s, "attackAngle") != nullptr
                                               && find(s, "shaftAngleVsHorizontal") != nullptr);
    }

    // ── 8. An arc that never turns over inside the window is refused ───────────────────────────
    // The head is still sinking at the last sample, so the lowest point in the window is its final
    // one. Reporting that would be a confident statement about a bottom nothing saw — and it is
    // exactly what a swing whose next P-anchor lands hundreds of ms away produces.
    {
        const auto s = buildClubDeliverySeries(
            trackClubDelivery(trackOf(arc(910.0, +1.0, kImpact), fallingArc(kImpact)),
                              ladder(10000, kImpact), QPointF(900, 800), true, kMmPerPx),
            ladder(10000, kImpact));
        CHECK("an arc with no turning point ⇒ no low point", find(s, "lowPointAhead") == nullptr);
        CHECK("and the angles are still unaffected", find(s, "attackAngle") != nullptr);
    }

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
