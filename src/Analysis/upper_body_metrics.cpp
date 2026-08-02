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

#include "upper_body_metrics.h"

#include "../Diagnostics/anatomy_vocabulary.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pinpoint::analysis {
namespace {

constexpr double kRadToDeg = 57.29577951308232;
constexpr double kEps      = 1e-9;

// One frame de-normalized to PIXELS, ready to hand to the anatomy resolver.
//
// The resolver works in whatever units the caller's points are in and does not know the frame
// dimensions, so the de-normalization has to happen here — and it has to use BOTH dimensions,
// because kp arrive normalized by width and height separately. Scaling by width alone would make
// every angle a statement about the aspect ratio.
struct PxFrame {
    std::array<QPointF, kWholeBodyJoints> kp{};
    const PoseFrame2D *src = nullptr;

    KeypointFrame view() const
    {
        KeypointFrame f;
        f.kp    = kp.data();
        f.conf  = src->conf.data();
        f.count = kWholeBodyJoints;
        return f;
    }
};

PxFrame toPx(const PoseFrame2D &f, int frameW, int frameH)
{
    PxFrame out;
    out.src = &f;
    for (int i = 0; i < kWholeBodyJoints; ++i)
        out.kp[size_t(i)] = QPointF(f.kp[size_t(i)].x() * frameW, f.kp[size_t(i)].y() * frameH);
    return out;
}

double lengthOf(const QPointF &a, const QPointF &b)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    return std::sqrt(dx * dx + dy * dy);
}

// Signed tilt (deg) of a body line, POSITIVE WHEN THE TRAIL END SITS ABOVE THE LEAD END.
//
// Image y grows downward, so "trail higher" is trailY < leadY and the numerator is leadY − trailY.
// The denominator is the ABSOLUTE horizontal separation, which is what makes the sign independent
// of which image side the lead is on. The alternative — a raw atan2 of the lead→trail vector, which
// is what `toeLineAngle` uses — flips its answer for a left-handed golfer or a mirrored camera
// while describing the same posture. This is the same function `hipLineTilt` is built on, spelled
// the same way on purpose: the hip line, the shoulder line and the elbow line are one convention.
double lineTiltDeg(const QPointF &lead, const QPointF &trail)
{
    const double dx = std::abs(trail.x() - lead.x());
    if (dx <= kEps) return 0.0;                   // ends vertically stacked — no line to measure
    return std::atan2(lead.y() - trail.y(), dx) * kRadToDeg;
}

// Vertical signed height of `p` above the infinite line through a→b, in pixels, positive UP.
//
// Deliberately a VERTICAL height rather than a perpendicular distance, and deliberately expressed
// through y only. A perpendicular distance needs a cross product, whose sign depends on whether the
// lead side is image +x — so it silently inverts on a mirrored camera, which is the exact failure
// `lineTiltDeg` exists to avoid. "How far above the shoulder line" is also what the coaching
// reading means: the elbow rising out of the turn, not its distance from an inclined axis.
bool heightAboveLine(const QPointF &p, const QPointF &a, const QPointF &b, double &outPx)
{
    const double dx = b.x() - a.x();
    if (std::abs(dx) <= kEps) return false;       // the line is vertical — no y for this x
    const double lineY = a.y() + (p.x() - a.x()) * (b.y() - a.y()) / dx;
    outPx = lineY - p.y();                        // y grows down, so above the line is positive
    return true;
}

// Shortest distance from `p` to the SEGMENT a→b (clamped to the endpoints), in pixels.
double distToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const double vx = b.x() - a.x(), vy = b.y() - a.y();
    const double len2 = vx * vx + vy * vy;
    if (len2 <= kEps) return lengthOf(p, a);      // degenerate segment — fall back to the endpoint
    double t = ((p.x() - a.x()) * vx + (p.y() - a.y()) * vy) / len2;
    t = std::clamp(t, 0.0, 1.0);
    return lengthOf(p, QPointF(a.x() + t * vx, a.y() + t * vy));
}

// Unsigned angle (deg, 0–180) between two image-plane vectors.
double angleBetweenDeg(const QPointF &u, const QPointF &v)
{
    const double lu = std::sqrt(u.x() * u.x() + u.y() * u.y());
    const double lv = std::sqrt(v.x() * v.x() + v.y() * v.y());
    if (lu <= kEps || lv <= kEps) return 0.0;
    const double c = std::clamp((u.x() * v.x() + u.y() * v.y()) / (lu * lv), -1.0, 1.0);
    return std::acos(c) * kRadToDeg;
}

// Everything one frame can offer, resolved once. Each group carries its OWN validity: the shoulder
// line needs both shoulders, the axis tilt needs the spine, the drift needs both ankles and the
// chest. A frame that supplies some but not all contributes to the channels it can and is absent
// from the rest — never a zero standing in for a measurement.
struct FrameGeom {
    int64_t t_us = 0;
    bool shouldersValid = false, hipsValid = false, elbowsValid = false;
    bool spineValid = false, anklesValid = false, thoraxValid = false;
    bool leadArmValid = false, leadHandValid = false, trailElbowValid = false;

    QPointF leadShoulder, trailShoulder;
    QPointF leadHip, trailHip;
    QPointF leadElbow, trailElbow;
    QPointF leadWrist, leadHand;
    QPointF leadAnkle, trailAnkle;
    QPointF neck, pelvis, thorax;
};

FrameGeom resolveFrame(const PxFrame &px, bool leadIsLeft, double confMin)
{
    FrameGeom g;
    g.t_us = px.src->t_us;
    const KeypointFrame kf = px.view();
    const float mc = float(confMin);

    const auto pt = [&](AnatomyRole r) { return resolvePoint(r, kf, leadIsLeft, mc); };

    const ResolvedPoint ls = pt(AnatomyRole::LeadShoulder),  ts = pt(AnatomyRole::TrailShoulder);
    const ResolvedPoint lh = pt(AnatomyRole::LeadHip),       th = pt(AnatomyRole::TrailHip);
    const ResolvedPoint le = pt(AnatomyRole::LeadElbow),     te = pt(AnatomyRole::TrailElbow);
    const ResolvedPoint lw = pt(AnatomyRole::LeadWrist),     lhd = pt(AnatomyRole::LeadHand);
    const ResolvedPoint la = pt(AnatomyRole::LeadAnkle),     ta = pt(AnatomyRole::TrailAnkle);
    const ResolvedPoint nk = pt(AnatomyRole::Neck),          pc = pt(AnatomyRole::PelvisCentre);
    const ResolvedPoint tc = pt(AnatomyRole::ThoraxCentre);

    g.leadShoulder = ls.p;  g.trailShoulder = ts.p;
    g.leadHip      = lh.p;  g.trailHip      = th.p;
    g.leadElbow    = le.p;  g.trailElbow    = te.p;
    g.leadWrist    = lw.p;  g.leadHand      = lhd.p;
    g.leadAnkle    = la.p;  g.trailAnkle    = ta.p;
    g.neck         = nk.p;  g.pelvis        = pc.p;  g.thorax = tc.p;

    g.shouldersValid  = ls.valid && ts.valid;
    g.hipsValid       = lh.valid && th.valid;
    g.elbowsValid     = le.valid && te.valid;
    g.spineValid      = nk.valid && pc.valid;
    g.anklesValid     = la.valid && ta.valid;
    g.thoraxValid     = tc.valid;
    g.leadArmValid    = ls.valid && le.valid;
    g.leadHandValid   = lhd.valid;
    g.trailElbowValid = te.valid;
    return g;
}

} // namespace

UpperBodyResult trackUpperBody(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                               int64_t addressUs, const UpperBodyConfig &cfg)
{
    UpperBodyResult res;
    res.frameW = frameW;
    res.frameH = frameH;
    if (frameW <= 0 || frameH <= 0)
        return res;

    // The smoothed companion track (parallel t_us, same normalized kp) is preferred, exactly like
    // head_track, foot_metrics and lower_body_metrics — falls back to raw frames on swings analysed
    // before the smoother existed.
    const std::vector<PoseFrame2D> &frames = pose.smoothed.empty() ? pose.frames : pose.smoothed;
    if (frames.empty())
        return res;

    std::vector<FrameGeom> geom;
    geom.reserve(frames.size());
    res.grid.reserve(frames.size());
    for (const PoseFrame2D &f : frames) {
        geom.push_back(resolveFrame(toPx(f, frameW, frameH), leadIsLeft, cfg.confMin));
        res.grid.push_back(f.t_us);
    }

    // Robust (median) address reference, over the confident frames inside the Address-event window;
    // if there is no event, or nothing usable inside it, the first N usable frames. The admission
    // test is EVERYTHING the reference supplies — shoulders, hips and ankles together — for the
    // reason lower_body_metrics states: this reference sets three denominators, and a frame that
    // contributed only one of them would leave the channels scaled against different instants,
    // which is a class of error that produces plausible numbers.
    const auto usable = [](const FrameGeom &g) {
        return g.shouldersValid && g.anklesValid && g.leadArmValid;
    };

    std::vector<const FrameGeom *> ref;
    if (addressUs >= 0) {
        for (const FrameGeom &g : geom)
            if (usable(g) && std::llabs(g.t_us - addressUs) <= cfg.addrWindowUs)
                ref.push_back(&g);
    }
    if (ref.empty()) {
        for (const FrameGeom &g : geom) {
            if (!usable(g)) continue;
            ref.push_back(&g);
            if (int(ref.size()) >= cfg.addrMinFrames) break;
        }
    }
    if (ref.empty())
        return res;   // no usable upper body anywhere — leave valid == false

    std::vector<double> shoulderSpans, stanceSpans, armLens, leadAnkX, trailAnkX;
    for (const FrameGeom *g : ref) {
        // % shoulder width means the EUCLIDEAN shoulder separation, because that is what it already
        // means in the shipped `stanceWidth` metric (foot_metrics' shoulderWidthPxAt). % stance
        // width means the ABSOLUTE HORIZONTAL ankle span, because that is what it already means in
        // the shipped `pelvisSway` (lower_body_metrics' addrSpanPx). Two units, two established
        // denominators; a new producer does not get to redefine either.
        shoulderSpans.push_back(lengthOf(g->leadShoulder, g->trailShoulder));
        stanceSpans.push_back(std::abs(g->trailAnkle.x() - g->leadAnkle.x()));
        if (g->leadHandValid)
            armLens.push_back(lengthOf(g->leadShoulder, g->leadElbow)
                              + lengthOf(g->leadElbow, g->leadHand));
        leadAnkX.push_back(g->leadAnkle.x());
        trailAnkX.push_back(g->trailAnkle.x());
    }

    res.ref.shoulderSpanPx = medianOfCopy(shoulderSpans);
    res.ref.stanceSpanPx   = medianOfCopy(stanceSpans);
    res.ref.leadArmLenPx   = medianOfCopy(armLens);

    // Which image direction the lead side is, resolved from the address geometry rather than
    // assumed — a camera can be mirrored and an operator can flip the preview.
    res.ref.leadSign = (medianOfCopy(leadAnkX) <= medianOfCopy(trailAnkX)) ? -1.0 : 1.0;

    // The denominator floor. Below it every percentage is noise divided by noise, and the honest
    // answer is that nothing was measured — not a large number.
    if (res.ref.shoulderSpanPx < cfg.minShoulderSpanPx)
        return res;
    res.ref.valid = true;

    const double toPctShoulder = 100.0 / res.ref.shoulderSpanPx;
    const double toPctStance   = res.ref.stanceSpanPx > kEps ? 100.0 / res.ref.stanceSpanPx : 0.0;
    const double toPctArm      = res.ref.leadArmLenPx > kEps ? 100.0 / res.ref.leadArmLenPx : 0.0;

    for (const FrameGeom &g : geom) {
        // secondaryAxisTilt — the mid-hip→mid-shoulder line from vertical, POSITIVE AWAY FROM THE
        // TARGET. This is the one lateral channel that is trail-positive rather than lead-positive:
        // the quantity is NAMED for the lean away from the target, and flipping it to satisfy the
        // lead-positive rule would leave every coach-facing sentence about it backwards.
        if (g.spineValid) {
            const double rise = g.pelvis.y() - g.neck.y();       // y grows down: neck above ⇒ > 0
            if (rise > kEps) {
                const double towardTrail = -res.ref.leadSign * (g.neck.x() - g.pelvis.x());
                res.axisTilt.push(g.t_us, std::atan2(towardTrail, rise) * kRadToDeg);
            }
        }

        // shoulderPlaneAngle / elbowAlignment — body lines against the horizontal, one convention.
        if (g.shouldersValid)
            res.shoulderPlane.push(g.t_us, lineTiltDeg(g.leadShoulder, g.trailShoulder));
        if (g.elbowsValid)
            res.elbowLine.push(g.t_us, lineTiltDeg(g.leadElbow, g.trailElbow));

        // spineSideBend — the THORAX RELATIVE TO THE PELVIS, which is what side bend means. With no
        // keypoint between the shoulders and the hips, the honest reading of "thorax relative to
        // pelvis" is the shoulder line against the hip line: two segments that both exist, and a
        // difference that cancels the whole-body tilt secondaryAxisTilt already reports. Reading a
        // single neck-to-pelvis line here instead would make this metric a copy of that one.
        // POSITIVE = SIDE BEND TOWARD THE TRAIL SIDE (the trail shoulder dropping under the turn).
        if (g.shouldersValid && g.hipsValid) {
            const double hipTilt      = lineTiltDeg(g.leadHip, g.trailHip);
            const double shoulderTilt = lineTiltDeg(g.leadShoulder, g.trailShoulder);
            res.sideBend.push(g.t_us, hipTilt - shoulderTilt);
        }

        // thoraxLateralDrift — the chest along the stance line, measured FROM THE TRAIL ANKLE, per
        // the `thoraxCentre · distance · trailAnkle` facet the content authored. NOT
        // address-referenced: the measure over it is a Delta, which does the referencing, and a
        // producer that pre-subtracted address would make that Delta a difference of differences.
        // The stance unit vector points trail→lead, so the projection is lead-positive by
        // construction and needs no leadSign.
        if (g.thoraxValid && g.anklesValid && toPctStance > 0.0) {
            const double ux = g.leadAnkle.x() - g.trailAnkle.x();
            const double uy = g.leadAnkle.y() - g.trailAnkle.y();
            const double ul = std::sqrt(ux * ux + uy * uy);
            if (ul > kEps) {
                const double along = ((g.thorax.x() - g.trailAnkle.x()) * ux
                                      + (g.thorax.y() - g.trailAnkle.y()) * uy) / ul;
                res.thoraxDrift.push(g.t_us, along * toPctStance);
            }
        }

        // trailElbowHeight — how far the trail elbow has risen above the shoulder line.
        if (g.shouldersValid && g.trailElbowValid) {
            double hPx = 0.0;
            if (heightAboveLine(g.trailElbow, g.trailShoulder, g.leadShoulder, hPx))
                res.trailElbowHeight.push(g.t_us, hPx * toPctShoulder);
        }

        // leadHandWidth — the swing's width, against the golfer's OWN arm length so it reads as a
        // fraction of the width actually available to them rather than as an absolute distance.
        if (g.leadHandValid && g.thoraxValid && toPctArm > 0.0)
            res.leadHandWidth.push(g.t_us, lengthOf(g.leadHand, g.thorax) * toPctArm);

        // leadUpperArmToChest — connection. The chest centre's distance to the upper-arm SEGMENT
        // (clamped to its endpoints), not to the infinite line: past the elbow the arm has ended,
        // and an infinite line would keep reporting a gap to something that is not there.
        if (g.leadArmValid && g.thoraxValid)
            res.leadArmGap.push(g.t_us,
                                distToSegment(g.thorax, g.leadShoulder, g.leadElbow) * toPctShoulder);

        // leadArmToTorso — the lead upper arm against the torso, per the `leadUpperArm · angle ·
        // spine` facet. Measured against the spine pointing DOWN the torso (neck→pelvis) so that
        // an arm hanging alongside the body reads near zero and HIGHER MEANS FURTHER FROM THE
        // TORSO, which is the direction the content's `highMeans` states. Unsigned: the frontal
        // projection cannot say which side of the torso the arm left on, and pretending otherwise
        // would put a sign on a quantity the camera did not resolve.
        if (g.leadArmValid && g.spineValid) {
            const QPointF armDir(g.leadElbow.x() - g.leadShoulder.x(),
                                 g.leadElbow.y() - g.leadShoulder.y());
            const QPointF torsoDir(g.pelvis.x() - g.neck.x(), g.pelvis.y() - g.neck.y());
            res.leadArmToTorso.push(g.t_us, angleBetweenDeg(armDir, torsoDir));
        }
    }

    res.valid = !res.axisTilt.empty() || !res.shoulderPlane.empty() || !res.thoraxDrift.empty();
    return res;
}

std::vector<MetricSeries> buildUpperBodySeries(const UpperBodyResult &res,
                                               const std::vector<PhaseEvent> &phases)
{
    std::vector<MetricSeries> out;
    if (!res.valid || res.grid.empty())
        return out;

    const QString deg     = QStringLiteral("°");
    const QString pctSh   = QStringLiteral("% shoulder width");
    const QString pctSt   = QStringLiteral("% stance width");
    const QString pctArm  = QStringLiteral("% arm length");

    // NOT named `emit`: that is a Qt keyword macro, and a lambda called `emit` does not survive
    // the preprocessor.
    const auto push = [&](const MetricChannel &ch, const char *key, const char *label,
                          const QString &unit) {
        appendIfProduced(out, buildChannelSeries(res.grid, ch, QString::fromLatin1(key),
                                                 QString::fromUtf8(label), unit, phases));
    };

    push(res.axisTilt,         "secondaryAxisTilt",   "Secondary axis tilt",     deg);
    push(res.sideBend,         "spineSideBend",       "Spine side bend",         deg);
    push(res.thoraxDrift,      "thoraxLateralDrift",  "Thorax lateral drift",    pctSt);
    push(res.shoulderPlane,    "shoulderPlaneAngle",  "Shoulder plane angle",    deg);
    push(res.elbowLine,        "elbowAlignment",      "Elbow alignment",         deg);
    push(res.trailElbowHeight, "trailElbowHeight",    "Trail elbow height",      pctSh);
    push(res.leadHandWidth,    "leadHandWidth",       "Swing width at the top",  pctArm);
    push(res.leadArmGap,       "leadUpperArmToChest", "Lead arm connection",     pctSh);
    push(res.leadArmToTorso,   "leadArmToTorso",      "Lead arm to torso angle", deg);
    return out;
}

} // namespace pinpoint::analysis
