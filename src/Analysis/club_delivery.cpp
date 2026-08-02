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

} // namespace

ClubDeliveryResult trackClubDelivery(const ShaftTrack2D &shaft, const std::vector<PhaseEvent> &phases,
                                     QPointF addressBallPx, bool ballValid, double mmPerPx,
                                     const ClubDeliveryConfig &cfg)
{
    ClubDeliveryResult res;
    if (!shaft.valid || shaft.samples.empty())
        return res;

    // The measured-head subset, in time order. Everything below reads this and only this.
    std::vector<const ShaftSample2D *> m;
    m.reserve(shaft.samples.size());
    for (const ShaftSample2D &s : shaft.samples)
        if (headMeasured(s, cfg.headConfMin))
            m.push_back(&s);
    if (m.empty())
        return res;

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
    // Needs the ball (the reference the answer is stated against) and the ruler (the unit it is
    // stated in). Missing either suppresses THIS metric only — the two angles above are scale-free
    // and are unaffected.
    const int64_t impactUs = phaseTime(phases, Phase::Impact, -1);
    if (ballValid && mmPerPx > 0.0 && impactUs >= 0) {
        std::vector<const ShaftSample2D *> win;
        for (const ShaftSample2D *s : m)
            if (std::llabs(s->t_us - impactUs) <= cfg.lowPointWinUs)
                win.push_back(s);

        if (int(win.size()) >= cfg.lowPointMinSamples) {
            // The lowest point in the image is the LARGEST y. Find it, then refine to sub-frame
            // precision with a three-point parabola through its neighbours in (x, y): the arc near
            // its vertex is locally quadratic, and the true bottom almost never falls exactly on a
            // sampled frame. Without the refinement the answer quantises to the frame spacing,
            // which at impact speeds is inches.
            size_t lo = 0;
            for (size_t i = 1; i < win.size(); ++i)
                if (win[i]->headPx.y() > win[lo]->headPx.y())
                    lo = i;

            double lowX = win[lo]->headPx.x();
            if (lo > 0 && lo + 1 < win.size()) {
                const double x0 = win[lo - 1]->headPx.x(), y0 = win[lo - 1]->headPx.y();
                const double x1 = win[lo]->headPx.x(),     y1 = win[lo]->headPx.y();
                const double x2 = win[lo + 1]->headPx.x(), y2 = win[lo + 1]->headPx.y();
                const double d0 = (x0 - x1) * (x0 - x2);
                const double d1 = (x1 - x0) * (x1 - x2);
                const double d2 = (x2 - x0) * (x2 - x1);
                if (std::abs(d0) > kEps && std::abs(d1) > kEps && std::abs(d2) > kEps) {
                    // Vertex of the Lagrange quadratic through the three points.
                    const double A = y0 / d0 + y1 / d1 + y2 / d2;
                    const double B = -(y0 * (x1 + x2) / d0 + y1 * (x0 + x2) / d1
                                       + y2 * (x0 + x1) / d2);
                    if (std::abs(A) > kEps) {
                        const double vx = -B / (2.0 * A);
                        // Only accept a vertex that lies inside the bracket it was fitted through.
                        // Outside it the quadratic is extrapolating, and an extrapolated low point
                        // is a confident statement about frames that were never measured.
                        const double xlo = std::min({ x0, x1, x2 }), xhi = std::max({ x0, x1, x2 });
                        if (vx >= xlo && vx <= xhi)
                            lowX = vx;
                    }
                }
            }

            // Which image direction the target is. Taken from the CLUBHEAD ITSELF — at impact the
            // head is travelling toward the target, so the sign of its horizontal displacement
            // across the window IS the target direction. Deliberately not taken from pose
            // handedness: this module never sees a skeleton, and the head's own motion is both
            // more direct and immune to a mirrored camera.
            const double sweep = win.back()->headPx.x() - win.front()->headPx.x();
            if (std::abs(sweep) > kEps) {
                const double targetSign = sweep > 0.0 ? 1.0 : -1.0;
                res.lowPointIn    = targetSign * (lowX - addressBallPx.x()) * mmPerPx / kMmPerIn;
                res.lowPointTUs   = win[lo]->t_us;
                res.lowPointValid = true;
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

    const auto pushScalar = [&](bool ok, const QString &key, const QString &label,
                                const QString &unit, double value, int64_t atUs) {
        if (!ok) return;
        MetricSeries m;
        m.key   = key;
        m.label = label;
        m.unit  = unit;
        m.phaseSamples.push_back({ Phase::Impact, atUs, value, QString() });
        out.push_back(std::move(m));
    };

    if (!res.attackAngle.empty()) {
        const double aa = interpChannel(res.attackAngle.t_us, res.attackAngle.value, impactUs);
        pushScalar(true, QStringLiteral("attackAngle"), QStringLiteral("Attack angle"), deg, aa,
                   impactUs);
    }
    pushScalar(res.lowPointValid, QStringLiteral("lowPointAhead"), QStringLiteral("Low point"),
               QStringLiteral("in"), res.lowPointIn, res.lowPointTUs);
    return out;
}

} // namespace pinpoint::analysis
