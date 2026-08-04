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

#pragma once

// The launch monitor panel's two INFERRED reads — the nine-window flight shape and
// strike quality. Pure, header-only, no Qt-GUI. Sibling of lm_session_reductions.h and
// lm_flight_path.h, same rule: QML positions and paints, C++ decides the numbers.
//
// THESE ARE CLAIMS THE DEVICE DID NOT MAKE. Everything else on this panel is a reading
// passed through; these two are conclusions drawn from readings, and the whole panel's
// credibility rests on the difference being visible. Two things enforce it:
//
//   1. The UI labels both INFERRED. That is the panel's job, not this header's.
//   2. EVERY READ CARRIES ITS EVIDENCE. `evidence` is the audit trail — the measured
//      values the classification came from, in words. A reader who disagrees with
//      "Pull-fade" can see it was 1.3° left with the face 1.6° open to the path and
//      decide for themselves. It is formatted HERE, beside the thresholds that produced
//      it, so a future export cannot quote a different justification from the card.
//
// A MISSING INPUT YIELDS NO READ. Not "Straight", not "flushed" — nothing. The absence
// of a measurement and a measurement of zero are different facts (the same rule that
// governs which tiles exist at all, lm_session_reductions.h rule 3), and a default read
// would be the panel inventing the one kind of number it exists to avoid inventing.
//
// Unit-tested standalone in src/Analysis/tests/lm_inferred_reads_test.cpp.

#include "lm_session_reductions.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <optional>

namespace pinpoint::analysis {

// Local to this header so it stays independent of lm_flight_path.h, which owns the
// aerodynamics and has no business being pulled in to name a shot shape.
inline constexpr double kDegToRadIr = M_PI / 180.0;
inline constexpr double kRadToDegIr = 180.0 / M_PI;

// Every threshold both reads use. COACH-SET CONFIG, not view literals — these are
// judgements about when a golfer should be told their ball curved, and they belong
// somewhere a coach can be shown them. The defaults are the design brief's placeholders
// and are still awaiting confirmation from the coach who set `centreMm`.
struct LmInferenceTuning {
    double startT         = 1.0;   // ° — start direction beyond this leaves the straight window
    double curveT         = 2.0;   // ° — SPIN AXIS beyond this is a curve
    double severeAxisT    = 10.0;  // ° — spin axis beyond this is hook/slice, not draw/fade
    double severeCurveDeg = 4.5;   // ° — measured curvature beyond this is hook/slice
    // Fallback only, for a device that reports no spin axis. Face-to-path predicts
    // curvature far less well (see the note above lmFlightShape), so these exist to
    // produce SOME read rather than the right one.
    double f2pCurveT      = 1.0;   // °
    double f2pSevereT     = 4.0;   // °
    double centreMm       = 3.0;   // mm — inside this in BOTH axes is flushed
    double severeVMm      = 10.0;  // mm — vertical miss beyond this is fat/thin, not high/low
};

// ── flight shape ─────────────────────────────────────────────────────────────

// The nine windows: where it started × how it curved.
//
// START DIRECTION GIVES THE WINDOW, THE SPIN AXIS GIVES THE CURVE.
//
// The axis leads because it is what physically bends the ball: the Magnus force's lateral
// component is Cl·sin(axis), so the axis IS the curvature, up to spin rate and time of
// flight. Face-to-path is one step further back — it is a cause of the axis, not of the
// flight, and the gap between them is where a read goes wrong.
//
// This is a correction, and it was measured rather than reasoned. Over 25 shots the model
// originally shipped with — face-to-path leading, the axis only corroborating severity —
// disagreed with the ball on nine of them. Against the curvature the ball actually flew:
//
//     spin axis    r = +0.994          face-to-path   r = +0.845
//
// and where the two disagreed in sign, the axis was right both times: a shot with the face
// 2.9° closed to the path and the axis 2.3° right curved RIGHT, and was called a draw.
// Worse, a shot with a 16.3° axis that curved 25 yards right had a face-to-path of exactly
// 1.0° and so was called dead straight.
//
// SEVERITY IS MEASURED, NOT ASSUMED. Fade against slice, draw against hook, is a question
// about HOW MUCH the ball curved, so it is answered with how much the ball curved:
//
//     curvature = offline − carry·tan(startDir)
//
// which is the finish less where the ball would have finished had it flown straight down
// its own start line. Expressed as an angle so a wedge and a driver are comparable. The
// axis threshold is the same statement for a device that reports no distances — over the
// same 25 shots curvature ≈ half the axis, so 10° of axis and 4.5° of curvature are one
// boundary written twice, not two.
struct LmFlightShape {
    bool    has = false;
    int     windowIdx = 1;    // 0 pull · 1 straight · 2 push
    int     curveIdx  = 1;    // 0 draw/hook · 1 straight · 2 fade/slice
    QString name;             // "Pull–fade", "Straight", "Hook", …
    QString evidence;         // "start 1.3° left · axis 4.7° right · curved 8.9 yd right"
    // The curvature the name was decided from, when carry and offline were both reported.
    bool    hasCurve  = false;
    double  curveYd   = 0.0;  // + = right for a right-hander, raw (unmirrored)
    double  curveDeg  = 0.0;
};

// A signed lateral quantity as the evidence line says it: magnitude, unit, side word.
// Never a bare minus — "start −1.3°" makes a reader work out a sign convention that the
// word "left" simply tells them.
inline QString lmSideText(double v, int decimals, const QString &unit,
                          const QString &leftWord, const QString &rightWord)
{
    const QString mag = lmFormat(std::abs(v), decimals);
    // `unit` carries its own leading space where the typography wants one ("°" sets
    // tight against the figure, " yd" does not), so that is decided once by the caller
    // rather than guessed per unit here.
    //
    // A reading that ROUNDS TO ZERO gets no side word. "0.0° right" is not a fact about
    // the shot, it is the sign bit of a number too small to have one, and this line is
    // an audit trail — the one place on the panel that must not overstate what was seen.
    const bool zero = std::all_of(mag.cbegin(), mag.cend(), [](QChar c) {
        return c == QLatin1Char('0') || c == QLatin1Char('.');
    });
    if (zero)
        return mag + unit;
    return mag + unit + QStringLiteral(" ") + (v < 0.0 ? leftWord : rightWord);
}

// Classify the shot's shape.
//
// HANDEDNESS MIRRORS THE GLOSS, NEVER THE SIGN — the rule already stated for the whole
// metric catalogue in src/Metrics/metric_descriptor.h. The device records device-frame
// geometry (gcquad_csv_parser.cpp: spin axis "positive tilting right for a right-handed
// golfer"), and a ball that finished physically right finished physically right for
// either golfer. What changes is what that is CALLED: for a left-hander, right of target
// is a pull and curving right is a draw. So the inputs are negated for a left-hander to
// classify, and the stored readings are untouched.
//
// windowIdx/curveIdx are in the SAME gloss space as `name`, so the lit cell of the 3 × 3
// always agrees with the words printed beside it. A grid that stayed geometric while the
// label mirrored would show a left-hander a picture contradicting its own caption.
inline LmFlightShape lmFlightShape(const std::optional<double> &startDirDeg,
                                   const std::optional<double> &spinAxisDeg,
                                   const std::optional<double> &faceToPathDeg,
                                   const std::optional<double> &carryYd,
                                   const std::optional<double> &offlineYd,
                                   bool leftHanded = false,
                                   const LmInferenceTuning &t = LmInferenceTuning())
{
    LmFlightShape out;

    // A start direction, and SOMETHING that speaks to the curve. Either alone names half
    // a shot, and half a shot stated confidently is worse than no read at all.
    const bool haveAxis  = spinAxisDeg   && std::isfinite(*spinAxisDeg);
    const bool haveF2p   = faceToPathDeg && std::isfinite(*faceToPathDeg);
    if (!startDirDeg || !std::isfinite(*startDirDeg) || (!haveAxis && !haveF2p))
        return out;

    const double flip  = leftHanded ? -1.0 : 1.0;
    const double start = *startDirDeg * flip;
    const double axis  = haveAxis ? *spinAxisDeg   * flip : 0.0;
    const double f2p   = haveF2p   ? *faceToPathDeg * flip : 0.0;

    // HOW MUCH THE BALL ACTUALLY CURVED: the finish, less where it would have finished
    // had it flown straight down its own start line. Not the offline — a pull that never
    // curved still finishes left, and calling that a hook is the error this replaces.
    const bool haveCurve = carryYd && offlineYd
                           && std::isfinite(*carryYd) && std::isfinite(*offlineYd)
                           && *carryYd > 0.0;
    double curveYd = 0.0, curveDeg = 0.0;
    if (haveCurve) {
        const double straightYd = *carryYd * std::tan(*startDirDeg * kDegToRadIr);
        curveYd  = *offlineYd - straightYd;
        curveDeg = std::atan2(curveYd, *carryYd) * kRadToDegIr;
        out.hasCurve = true;
        out.curveYd  = curveYd;      // raw, unmirrored — it is a measurement
        out.curveDeg = curveDeg;
    }

    // Direction from the axis where there is one, because the axis is what bends the
    // ball. Face-to-path only when the device reported no axis at all.
    const double curveSignal = haveAxis ? axis : f2p;
    const double curveThresh = haveAxis ? t.curveT : t.f2pCurveT;

    // Severity from what the ball DID, or from the axis when the distances are missing.
    // The two are one boundary written twice: curvature runs at about half the axis, so
    // 10° of axis and 4.5° of curvature fire at the same shot.
    const bool severe = (haveAxis  && std::abs(axis) >= t.severeAxisT)
                        || (haveCurve && std::abs(curveDeg * flip) >= t.severeCurveDeg)
                        || (!haveAxis && std::abs(f2p) > t.f2pSevereT);

    out.windowIdx = start < -t.startT ? 0 : (start > t.startT ? 2 : 1);
    out.curveIdx  = curveSignal < -curveThresh ? 0
                  : (curveSignal >  curveThresh ? 2 : 1);

    const QString window = out.windowIdx == 0 ? QStringLiteral("Pull")
                         : out.windowIdx == 2 ? QStringLiteral("Push")
                                              : QString();
    const QString curve  = out.curveIdx == 0 ? (severe ? QStringLiteral("Hook")
                                                       : QStringLiteral("Draw"))
                         : out.curveIdx == 2 ? (severe ? QStringLiteral("Slice")
                                                       : QStringLiteral("Fade"))
                                             : QString();

    // An EN DASH joins the two, not a hyphen: "Pull–fade" is a compound of two
    // independent reads, not one hyphenated word, and at 14 px the hyphen reads as
    // punctuation inside a single term.
    if (window.isEmpty() && curve.isEmpty()) out.name = QStringLiteral("Straight");
    else if (window.isEmpty())               out.name = curve;
    else if (curve.isEmpty())                out.name = window;
    else out.name = window + QStringLiteral("–") + curve.toLower();

    // The audit trail, quoting the RAW readings — physical left and right, not the
    // mirrored gloss. For a left-hander the name says "Pull" while the evidence says
    // "1.3° left", and both are true: that is where the ball went and that is what it is
    // called. Mirroring the evidence too would leave nothing on the card stating the
    // measurement itself.
    //
    // IT CITES WHAT DECIDED THE NAME, which is why it no longer leads with face-to-path:
    // an audit trail that quoted a number the classification did not use would be a
    // justification rather than a reason. Face-to-path is still on the card, as its own
    // annotation, where a coach reads it as the thing the golfer can change.
    QString ev = QStringLiteral("start ")
                 + lmSideText(*startDirDeg, 1, QStringLiteral("°"),
                              QStringLiteral("left"), QStringLiteral("right"));
    if (haveAxis)
        ev += QStringLiteral(" · axis ")
              + lmSideText(*spinAxisDeg, 1, QStringLiteral("°"),
                           QStringLiteral("left"), QStringLiteral("right"));
    else
        ev += QStringLiteral(" · face ")
              + (*faceToPathDeg > 0.0 ? QStringLiteral("+") : QString())
              + lmFormat(*faceToPathDeg, 1) + QStringLiteral("° to path");
    if (out.hasCurve)
        ev += QStringLiteral(" · curved ")
              + lmSideText(out.curveYd, 1, QStringLiteral(" yd"),
                           QStringLiteral("left"), QStringLiteral("right"));
    else if (offlineYd && std::isfinite(*offlineYd))
        ev += QStringLiteral(" · finishes ")
              + lmSideText(*offlineYd, 1, QStringLiteral(" yd"),
                           QStringLiteral("left"), QStringLiteral("right"));
    out.evidence = ev;

    out.has = true;
    return out;
}

// ── strike quality ───────────────────────────────────────────────────────────

// Where on the face it was struck, and nothing else.
//
// SMASH DOES NOT ENTER THE CLASSIFICATION. It is shown beside the read as context
// because a golfer wants it there, but a strike is flushed or it is not by where it
// landed on the face — letting smash vote would make a fast swing's toe strike read as
// centred, which is the opposite of the thing this card exists to show.
struct LmStrikeRead {
    bool    has = false;
    QString name;        // "Flushed", "Toe", "Low", "Thin", …
    QString evidence;    // "smash +0.01 vs μ", or empty when there is no session mean
};

// Classify the strike.
//
// NO HANDEDNESS PARAMETER, deliberately. Toe and heel are parts of the clubhead, and the
// device names them from the face's own frame — a strike 3 mm toward the toe is toward
// the toe for either golfer. What DOES mirror for a left-hander is the drawing (§5.4 of
// the design brief: marker offset, axis words, shaft direction), and that is the panel's
// business, not this classification's. Adding a flip here would rename a correct reading.
inline LmStrikeRead lmStrikeQuality(const std::optional<double> &locationMm,
                                    const std::optional<double> &heightMm,
                                    const std::optional<double> &smashLatest,
                                    const std::optional<double> &smashMean,
                                    const LmInferenceTuning &t = LmInferenceTuning())
{
    LmStrikeRead out;
    if (!locationMm || !heightMm
        || !std::isfinite(*locationMm) || !std::isfinite(*heightMm))
        return out;

    const double h = *locationMm;    // heel(−) ← → toe(+)
    const double v = *heightMm;      // low(−)  ← → high(+)
    const double ah = std::abs(h), av = std::abs(v);

    if (ah <= t.centreMm && av <= t.centreMm) {
        out.name = QStringLiteral("Flushed");
    } else if (av >= ah) {
        // Vertical names it. Ties go to the vertical miss because it is the one that
        // costs distance, and it is the one a golfer can feel.
        if (av > t.severeVMm)
            out.name = v > 0.0 ? QStringLiteral("Fat") : QStringLiteral("Thin");
        else
            out.name = v > 0.0 ? QStringLiteral("High") : QStringLiteral("Low");
    } else {
        // Horizontal at any magnitude — there is no severe tier here, because a toe
        // strike is a toe strike and the millimetres are already printed on the card.
        out.name = h > 0.0 ? QStringLiteral("Toe") : QStringLiteral("Heel");
    }

    if (smashLatest && smashMean && std::isfinite(*smashLatest) && std::isfinite(*smashMean)) {
        const double d = *smashLatest - *smashMean;
        out.evidence = QStringLiteral("smash ")
                       + (d > 0.0 ? QStringLiteral("+") : QString())
                       + lmFormat(d, 2) + QStringLiteral(" vs μ");
    }

    out.has = true;
    return out;
}

} // namespace pinpoint::analysis
