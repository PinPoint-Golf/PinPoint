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

// Every threshold both reads use. COACH-SET CONFIG, not view literals — these are
// judgements about when a golfer should be told their ball curved, and they belong
// somewhere a coach can be shown them. The defaults are the design brief's placeholders
// and are still awaiting confirmation from the coach who set `centreMm`.
struct LmInferenceTuning {
    double startT      = 1.0;    // ° — start direction beyond this leaves the straight window
    double curveT      = 1.0;    // ° — face-to-path beyond this is a curve
    double severeT     = 4.0;    // ° — face-to-path beyond this is hook/slice, not draw/fade
    double severeAxisT = 10.0;   // ° — |spin axis| this large is severe on its own
    double centreMm    = 3.0;    // mm — inside this in BOTH axes is flushed
    double severeVMm   = 10.0;   // mm — vertical miss beyond this is fat/thin, not high/low
};

// ── flight shape ─────────────────────────────────────────────────────────────

// The nine windows: where it started × how it curved.
//
// START DIRECTION GIVES THE WINDOW, FACE-TO-PATH GIVES THE CURVE, and the spin axis only
// corroborates. Face-to-path leads deliberately: it is a club measurement, so it survives
// a soft spin-axis reading on a low-spin strike — and a thin one is exactly the shot where
// the axis is least trustworthy and the golfer most wants to know what the face did.
struct LmFlightShape {
    bool    has = false;
    int     windowIdx = 1;    // 0 pull · 1 straight · 2 push
    int     curveIdx  = 1;    // 0 draw/hook · 1 straight · 2 fade/slice
    QString name;             // "Pull–fade", "Straight", "Hook", …
    QString evidence;         // "start 1.3° left · face +1.6° to path · finishes 5.1 yd right"
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
                                   const std::optional<double> &faceToPathDeg,
                                   const std::optional<double> &spinAxisDeg,
                                   const std::optional<double> &offlineYd,
                                   bool leftHanded = false,
                                   const LmInferenceTuning &t = LmInferenceTuning())
{
    LmFlightShape out;
    // Both ingredients required. Either one alone names half a shot, and half a shot
    // stated confidently is worse than no read at all.
    if (!startDirDeg || !faceToPathDeg
        || !std::isfinite(*startDirDeg) || !std::isfinite(*faceToPathDeg))
        return out;

    const double flip  = leftHanded ? -1.0 : 1.0;
    const double start = *startDirDeg  * flip;
    const double f2p   = *faceToPathDeg * flip;
    const bool   haveAxis = spinAxisDeg && std::isfinite(*spinAxisDeg);
    const double axis  = haveAxis ? *spinAxisDeg * flip : 0.0;

    // Severity: a big face-to-path, or a spin axis steep enough to say so by itself.
    // The axis corroborates here and only here — it can promote a draw to a hook, but
    // it can never create a curve the face-to-path did not already show.
    const bool severe = std::abs(f2p) > t.severeT
                        || (haveAxis && std::abs(axis) >= t.severeAxisT);

    out.windowIdx = start < -t.startT ? 0 : (start > t.startT ? 2 : 1);
    out.curveIdx  = f2p   < -t.curveT ? 0 : (f2p   > t.curveT ? 2 : 1);

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
    // "1.3° left", and both are true: that is where the ball went and that is what it
    // is called. Mirroring the evidence too would leave nothing on the card stating the
    // measurement itself.
    //
    // Face-to-path keeps its SIGN, because "open/closed to the path" is the language the
    // number is taught in and a coach reads the sign directly.
    QString ev = QStringLiteral("start ")
                 + lmSideText(*startDirDeg, 1, QStringLiteral("°"),
                              QStringLiteral("left"), QStringLiteral("right"));
    const QString f2pRaw = lmFormat(*faceToPathDeg, 1);
    ev += QStringLiteral(" · face ")
          + (*faceToPathDeg > 0.0 ? QStringLiteral("+") : QString())
          + f2pRaw + QStringLiteral("° to path");
    if (offlineYd && std::isfinite(*offlineYd))
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
