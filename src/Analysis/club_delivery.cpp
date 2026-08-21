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

#include "club_delivery.h"

#include <algorithm>
#include <cmath>

namespace pinpoint::analysis {
namespace {

constexpr double kRadToDeg = 57.29577951308232;
constexpr double kEps      = 1e-9;
constexpr double kMmPerIn  = 25.4;

// A sample whose headPx is a real measurement. `ShaftHeadProjected` means headPx was reconstructed
// from the grip along θ at an assumed club length; differentiating that yields the grip's motion
// wearing the clubhead's name. `headConf < 0` means the Stage-2 head pass never ran at all, which
// is a different failure from a low-confidence measurement and is refused the same way.
bool headMeasured(const ShaftSample2D &s, double headConfMin)
{
    if (s.flags & ShaftHeadProjected) return false;
    if (s.headConf < 0.f)             return false;
    return double(s.headConf) >= headConfMin;
}

// The arc's turning point inside `win`, in image x, refined below the sample spacing.
//
// The lowest point in the image is the LARGEST y. Find it, then refine with a three-point parabola
// through its neighbours in (x, y): the arc near its vertex is locally quadratic, and the true
// bottom almost never falls exactly on a sampled instant. Without the refinement the answer
// quantises to the sample spacing, which at impact speeds is inches.
//
// REFUSES A VERTEX THAT SITS ON EITHER END of the window. An interior minimum means the arc was
// seen to turn over; an endpoint one means it never did inside the window, and the "low point" is
// then just the last sample before the data ran out. That is exactly what a truncated arc looks
// like — a swing whose next P-anchor lands hundreds of ms away has no bottom in ±60 ms — and
// reporting one is a confident statement about a part of the swing nothing observed.
bool arcVertexX(const std::vector<const ShaftSample2D *> &win, double &lowX, int64_t &lowTUs)
{
    if (win.size() < 3)
        return false;

    size_t lo = 0;
    for (size_t i = 1; i < win.size(); ++i)
        if (win[i]->headPx.y() > win[lo]->headPx.y())
            lo = i;
    if (lo == 0 || lo + 1 >= win.size())
        return false;                                   // never turned over inside the window

    lowX   = win[lo]->headPx.x();
    lowTUs = win[lo]->t_us;

    const double x0 = win[lo - 1]->headPx.x(), y0 = win[lo - 1]->headPx.y();
    const double x1 = win[lo]->headPx.x(),     y1 = win[lo]->headPx.y();
    const double x2 = win[lo + 1]->headPx.x(), y2 = win[lo + 1]->headPx.y();
    const double d0 = (x0 - x1) * (x0 - x2);
    const double d1 = (x1 - x0) * (x1 - x2);
    const double d2 = (x2 - x0) * (x2 - x1);
    if (std::abs(d0) <= kEps || std::abs(d1) <= kEps || std::abs(d2) <= kEps)
        return true;                                    // coincident x — keep the sampled vertex

    // Vertex of the Lagrange quadratic through the three points.
    const double A = y0 / d0 + y1 / d1 + y2 / d2;
    const double B = -(y0 * (x1 + x2) / d0 + y1 * (x0 + x2) / d1 + y2 * (x0 + x1) / d2);
    if (std::abs(A) <= kEps)
        return true;

    const double vx = -B / (2.0 * A);
    // Only accept a vertex that lies inside the bracket it was fitted through. Outside it the
    // quadratic is extrapolating, and an extrapolated low point is a confident statement about
    // instants that were never sampled.
    const double xlo = std::min({ x0, x1, x2 }), xhi = std::max({ x0, x1, x2 });
    if (vx >= xlo && vx <= xhi)
        lowX = vx;
    return true;
}

} // namespace

ClubDeliveryResult trackClubDelivery(const ShaftTrack2D &shaft, const std::vector<PhaseEvent> &phases,
                                     QPointF addressBallPx, bool ballValid, double mmPerPx,
                                     const ClubDeliveryConfig &cfg)
{
    ClubDeliveryResult res;
    if (!shaft.valid)
        return res;

    // The measured-head subset, in time order. THE TWO ANGLES read this and only this; the low
    // point deliberately does not (see the header). An empty subset is therefore not a refusal —
    // it silences the angles and leaves the arc channel to answer for itself.
    std::vector<const ShaftSample2D *> m;
    m.reserve(shaft.samples.size());
    for (const ShaftSample2D &s : shaft.samples)
        if (headMeasured(s, cfg.headConfMin))
            m.push_back(&s);

    res.grid.reserve(m.size());
    for (const ShaftSample2D *s : m)
        res.grid.push_back(s->t_us);

    // ── shaftAngleVsHorizontal ─────────────────────────────────────────────────────────────────
    // The grip→head vector against the image horizontal. ZERO IS PARALLEL TO THE GROUND and
    // POSITIVE IS PAST PARALLEL: image y grows downward, so a head sitting BELOW the grip — which
    // at the top of the backswing means the club has travelled past horizontal — gives a positive
    // numerator. The denominator is the ABSOLUTE horizontal separation, so the reading does not
    // invert for a left-handed golfer or a mirrored camera; this is the same sign-stability rule
    // `hipLineTilt` and the upper-body body lines are built on.
    //
    // Taken from the head rather than from θ because θ carries a 180° line ambiguity that the
    // endpoint pair resolves for free.
    for (const ShaftSample2D *s : m) {
        const double dx = std::abs(s->headPx.x() - s->gripPx.x());
        if (dx <= kEps) continue;                       // shaft vertical — no angle to horizontal
        res.shaftVsHorizontal.push(s->t_us,
                                   std::atan2(s->headPx.y() - s->gripPx.y(), dx) * kRadToDeg);
    }

    // ── attackAngle ────────────────────────────────────────────────────────────────────────────
    // The vertical angle of the head's velocity, POSITIVE FOR A MORE UPWARD STRIKE. A centred
    // difference over ±velHalfSpan samples: one frame either side of a ~9 px head is mostly
    // quantisation, and this quantity is read at a single instant where that noise would land
    // squarely on the answer.
    //
    // The denominator is |dx| again. The head's horizontal travel at impact is along the target
    // line, and which way along it the golfer is swinging has no bearing on whether the strike is
    // descending — folding the sign out is what makes this handedness- and mirror-free.
    if (int(m.size()) > 2 * cfg.velHalfSpan) {
        for (int i = cfg.velHalfSpan; i < int(m.size()) - cfg.velHalfSpan; ++i) {
            const ShaftSample2D *a = m[size_t(i - cfg.velHalfSpan)];
            const ShaftSample2D *b = m[size_t(i + cfg.velHalfSpan)];
            const double dx = std::abs(b->headPx.x() - a->headPx.x());
            const double dy = b->headPx.y() - a->headPx.y();
            if (dx <= kEps && std::abs(dy) <= kEps) continue;   // stationary — no direction
            // −dy because image y grows downward: rising in the world is falling in y.
            res.attackAngle.push(m[size_t(i)]->t_us, std::atan2(-dy, dx) * kRadToDeg);
        }
    }

    // ── lowPointAhead ──────────────────────────────────────────────────────────────────────────
    // Where the arc bottoms out, relative to the ball, along the target line, in signed inches.
    //
    // READ OFF THE SYNTHESIZED ARC, not the measured heads — `shaft.synth`, the dense Hermite
    // interpolation between the located P-anchors that the club overlay draws. The header carries
    // the corpus evidence for why; the short version is that the head detector does not hold a lock
    // through impact on real swings, and the rare vertex it did produce sat tens of ms past the
    // ball. Reading the same series the overlay draws also means the number and the picture cannot
    // disagree.
    //
    // Needs the ball (the reference the answer is stated against) and the ruler (the unit it is
    // stated in). Missing either suppresses THIS metric only — the two angles above are scale-free
    // and are unaffected.
    const int64_t impactUs = phaseTime(phases, Phase::Impact, -1);
    if (ballValid && mmPerPx > 0.0 && impactUs >= 0 && !shaft.synth.empty()) {
        std::vector<const ShaftSample2D *> win;
        for (const ShaftSample2D &s : shaft.synth)
            if (std::llabs(s.t_us - impactUs) <= cfg.lowPointWinUs)
                win.push_back(&s);

        double  lowX   = 0.0;
        int64_t lowTUs = 0;
        if (int(win.size()) >= cfg.lowPointMinSamples && arcVertexX(win, lowX, lowTUs)) {
            // Which image direction the target is. Taken from the CLUBHEAD ITSELF — at impact the
            // head is travelling toward the target, so the sign of its horizontal displacement
            // across the window IS the target direction. Deliberately not taken from pose
            // handedness: this module never sees a skeleton, and the head's own motion is both
            // more direct and immune to a mirrored camera.
            const double sweep = win.back()->headPx.x() - win.front()->headPx.x();
            if (std::abs(sweep) > kEps) {
                const double targetSign = sweep > 0.0 ? 1.0 : -1.0;
                res.lowPointIn      = targetSign * (lowX - addressBallPx.x()) * mmPerPx / kMmPerIn;
                res.lowPointTUs     = lowTUs;
                res.lowPointSigmaIn = cfg.lowPointSigmaIn;
                res.lowPointValid   = true;
            }
        }
    }

    res.valid = !res.shaftVsHorizontal.empty() || !res.attackAngle.empty() || res.lowPointValid;
    return res;
}

std::vector<MetricSeries> buildClubDeliverySeries(const ClubDeliveryResult &res,
                                                  const std::vector<PhaseEvent> &phases)
{
    std::vector<MetricSeries> out;
    if (!res.valid)
        return out;

    const QString deg = QStringLiteral("°");

    appendIfProduced(out, buildChannelSeries(res.grid, res.shaftVsHorizontal,
                                             QStringLiteral("shaftAngleVsHorizontal"),
                                             QStringLiteral("Shaft angle at the top"), deg, phases));

    // attackAngle and lowPointAhead are PointInTime metrics: an empty curve carrying one Impact
    // phaseSample, the representation foot_metrics' setup scalars already use and that every
    // reader and the swing.json writer already handle without a non-empty-curve assumption.
    const int64_t fallbackUs = res.grid.empty() ? 0 : res.grid.front();
    const int64_t impactUs   = phaseTime(phases, Phase::Impact, fallbackUs);

    // `sigma` <= 0 leaves the field ABSENT, which is the "not characterised" state — never 0,
    // which would read as an exact measurement.
    const auto pushScalar = [&](bool ok, const QString &key, const QString &label,
                                const QString &unit, double value, int64_t atUs,
                                double sigma = 0.0) {
        if (!ok) return;
        MetricSeries m;
        m.key   = key;
        m.label = label;
        m.unit  = unit;
        if (sigma > 0.0)
            m.sigma = sigma;
        m.phaseSamples.push_back({ Phase::Impact, atUs, value, QString() });
        out.push_back(std::move(m));
    };

    if (!res.attackAngle.empty()) {
        const double aa = interpChannel(res.attackAngle.t_us, res.attackAngle.value, impactUs);
        pushScalar(true, QStringLiteral("attackAngle"), QStringLiteral("Attack angle"), deg, aa,
                   impactUs);
    }
    // The health warning rides WITH the number: an estimate off an interpolated arc, ±2 in.
    pushScalar(res.lowPointValid, QStringLiteral("lowPointAhead"), QStringLiteral("Low point"),
               QStringLiteral("in"), res.lowPointIn, res.lowPointTUs, res.lowPointSigmaIn);
    return out;
}

} // namespace pinpoint::analysis
