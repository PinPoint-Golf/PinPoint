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

// Launch monitor SESSION reductions — pure, header-only, no Qt-GUI. The maths behind
// PpLaunchMonitorPanel: for every field the device reported, the focused shot's value,
// the session mean, the session spread, and where the shot sits inside that spread.
// Sibling of dashboard_reductions.h and kept out of QML JS by the same rule (analysis
// pipeline guide §6.2): QML positions and paints, C++ decides the numbers.
//
// This is a READOUT and nothing more. There is no corridor here, no norm, no grade,
// no band colour — the only comparison a reading is put to is against the golfer's own
// other shots with the same club. That is deliberate: the panel answers "what did the
// device measure, and how repeatable was I", which is a question that needs no
// normative opinion to be worth answering.
//
// FOUR RULES DECIDE EVERY NUMBER HERE, and each of them is a decision rather than an
// implementation detail:
//
//   1. n-1. A session is a SAMPLE of a golfer's swing, not the population of it.
//   2. kMinShotsForSpread. Two shots produce an SD that means nothing and a dispersion
//      strip that lies about it. Below the floor the mean still shows, the spread does
//      not, and the caller prints an em dash rather than a number nobody should read.
//   3. A field NO scoped shot carried is omitted entirely. std::optional empty means
//      the device did not report the quantity; a tile reading 0.0 would claim it
//      measured zero. Absence and zero are different facts and the panel must not
//      conflate them.
//   4. Order is fieldDefs() order. That table is already the single enumeration point
//      for the swing.json writer, the MetricSeries emitter and the manifest
//      cross-check, so a 26th field appears on the board with no edit in this file.
//
// The formatting lives here too, beside the arithmetic rather than in the panel,
// so a future export and the board cannot disagree about what a reading looks like.
//
// The QML façade is LmSessionModel (src/Gui/launchmonitor/lm_session_model.h).
// Unit-tested standalone in src/Analysis/tests/lm_session_reductions_test.cpp.

#include "../LaunchMonitor/launch_monitor_reading.h"

#include <QHash>
#include <QString>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pinpoint::analysis {

// Fewest scoped shots that may produce a standard deviation. Three is the smallest
// n where n-1 is not 1 and the estimate stops being a restatement of the gap between
// two numbers.
inline constexpr int kMinShotsForSpread = 3;

// One scoped shot's launch monitor readings, keyed by catalogue key ("lm.spinRate").
//
// A key ABSENT means the device did not report that quantity for that shot — which is
// why this is a map and not a fixed-width row. A monitor that loses the ball on one
// strike still measured the club on it, and that shot must contribute to the club
// fields' mean while contributing nothing at all to carry's.
using LmShotValues = QHash<QString, double>;

// One field's session statistics.
struct LmFieldStats {
    QString key;                 // "lm.clubheadSpeed"
    double  latest    = 0.0;     // the FOCUSED shot's value
    double  mean      = 0.0;     // arithmetic mean over the scoped shots that have a value
    double  sd        = 0.0;     // SAMPLE standard deviation, n-1 denominator
    int     n         = 0;       // how many scoped shots carried this field
    bool    hasLatest = false;
    bool    hasSpread = false;   // n >= kMinShotsForSpread && sd > 0
    double  z         = 0.0;     // (latest - mean) / sd; 0 when !hasSpread

    // The catalogue row this came from — label, abbrev, unit, group. A pointer rather
    // than four copied strings because fieldDefs() returns a reference to a function-
    // local static that outlives every caller, and because the alternative is a second
    // walk of the same table in the model, which is exactly how the two drift apart.
    const lm::FieldDef *def = nullptr;
};

// Session statistics for every field any scoped shot carried, in fieldDefs() order.
//
// `latestIndex` selects the shot whose value is the headline — the FOCUSED swing, so
// the panel agrees with the video and the panels beside it. Out of range (nothing
// focused, or the focused shot is not in this scope) leaves every hasLatest false: the
// board still renders its means and spreads, and each tile prints an em dash where its
// value would be. A shot the monitor missed is a fact, not an error.
//
// Non-finite values are treated as not reported. A NaN that reached a swing.json would
// otherwise poison a mean, an SD and every z on the band at once.
inline std::vector<LmFieldStats> lmSessionStats(const std::vector<LmShotValues> &shots,
                                                int latestIndex)
{
    const bool haveLatestShot = latestIndex >= 0 && latestIndex < int(shots.size());

    std::vector<LmFieldStats> out;
    out.reserve(lm::fieldDefs().size());

    std::vector<double> vals;
    vals.reserve(shots.size());

    for (const lm::FieldDef &f : lm::fieldDefs()) {
        const QString key = QString::fromLatin1(f.key);

        vals.clear();
        for (const LmShotValues &s : shots) {
            const auto it = s.constFind(key);
            if (it != s.constEnd() && std::isfinite(*it))
                vals.push_back(*it);
        }
        if (vals.empty())
            continue;                      // rule 3 — no tile, rather than a tile reading zero

        LmFieldStats st;
        st.key = key;
        st.def = &f;
        st.n   = int(vals.size());

        double sum = 0.0;
        for (double v : vals) sum += v;
        st.mean = sum / double(st.n);

        // Two-pass, because the one-pass sum-of-squares form loses its significant
        // digits exactly where it matters here: a tight cluster of large readings
        // (4600 rpm ± 40) is the normal case, not the pathological one.
        if (st.n >= 2) {
            double ss = 0.0;
            for (double v : vals) { const double d = v - st.mean; ss += d * d; }
            const double var = ss / double(st.n - 1);   // rule 1
            st.sd = var > 0.0 ? std::sqrt(var) : 0.0;
        }

        // rule 2, and the sd > 0 half of it is the no-divide guard: every scoped shot
        // reading identically is a legitimate outcome (a monitor quantising to whole
        // rpm), not a reason to divide by zero.
        st.hasSpread = st.n >= kMinShotsForSpread && st.sd > 0.0;

        if (haveLatestShot) {
            const auto it = shots[size_t(latestIndex)].constFind(key);
            if (it != shots[size_t(latestIndex)].constEnd() && std::isfinite(*it)) {
                st.hasLatest = true;
                st.latest    = *it;
            }
        }

        if (st.hasSpread && st.hasLatest)
            st.z = (st.latest - st.mean) / st.sd;

        out.push_back(st);
    }
    return out;
}

// Where the dispersion strip's tick sits, as a percentage of the track.
//
// The track is a ±3 SD axis, so the centre is the mean and z = ±3 reaches the ends.
// Clamped just inside both edges because a tick at exactly 0% or 100% is half outside
// its own strip and reads as a rendering fault rather than as an outlier.
//
// The CLAMP IS NOT APPLIED TO z ITSELF — z stays the true z-score wherever it is read,
// because a statistic that silently saturates at 3 is a different number wearing the
// same name. Only the pixel position saturates, which is a fact about the strip.
inline double lmTickPercent(double z)
{
    if (!std::isfinite(z)) return 50.0;
    return std::clamp(50.0 + z / 3.0 * 50.0, 1.5, 98.5);
}

// The band spanned by ±1 SD on that same ±3 SD axis. A literal constant, not a
// computed width — one third of the axis either side of centre, whatever the data does.
inline constexpr double kLmSdBandLo = 100.0 / 3.0;          // 33.3%
inline constexpr double kLmSdBandHi = 100.0 - kLmSdBandLo;  // 66.7%

// Decimal places for a launch monitor quantity, chosen by its catalogue unit.
//
// By unit and not by magnitude: a tile whose precision changed as the golfer warmed up
// would be unreadable shot to shot, and the whole point of the board is comparability.
// The choices are the resolutions the devices themselves publish — spin to the rpm,
// smash to the hundredth, strike to the millimetre.
inline int lmDecimals(const QString &unit)
{
    if (unit == QStringLiteral("ratio")) return 2;   // 1.39
    if (unit == QStringLiteral("rpm"))   return 0;   // 4686
    if (unit == QStringLiteral("mm"))    return 0;   // -6
    if (unit == QStringLiteral("ft"))    return 0;   // 63
    if (unit == QStringLiteral("°/s"))   return 0;   // 1840
    if (unit == QStringLiteral("°"))     return 1;   // -3.5
    if (unit == QStringLiteral("yd"))    return 1;   // 166.6
    if (unit == QStringLiteral("mph"))   return 1;   // 84.5
    return 1;
}

// A number as the board prints it: fixed decimals, and a TYPOGRAPHIC MINUS (U+2212)
// rather than the hyphen QString::number emits. At the value row's 26 px a hyphen is a
// short grey dash that reads as a missing digit; the minus sign is the width of the
// figures beside it, which is the whole reason the character exists.
inline QString lmFormat(double v, int decimals)
{
    if (!std::isfinite(v)) return QStringLiteral("—");
    QString s = QString::number(v, 'f', decimals);
    // "−0.0" is an artefact of rounding a value just below zero, never a measurement —
    // a face angle of 0.04° closed is square, and printing it signed says otherwise.
    if (s.startsWith(QLatin1Char('-'))) {
        const QString digits = s.mid(1);
        const bool allZero = std::all_of(digits.cbegin(), digits.cend(), [](QChar c) {
            return c == QLatin1Char('0') || c == QLatin1Char('.');
        });
        if (allZero) s = digits;
    }
    return s.replace(QLatin1Char('-'), QChar(0x2212));
}

// ── joint spread: the dispersion ELLIPSE ─────────────────────────────────────
//
// Two fields taken together, for the regions the schematics shade behind a vector.
// Everything above is one field at a time, which is all a tile needs; a strike PATTERN
// is not two independent spreads and drawing it as one is the thing this exists to
// avoid.
//
// WHY THE COVARIANCE AND NOT JUST TWO SDs. An ellipse built from sdX and sdY alone is
// axis-aligned, and real miss patterns are not: a golfer who thins it off the heel has a
// diagonal pattern, and the DIAGONAL IS THE COACHING INSIGHT. Axis-aligned shading would
// draw that same golfer a fat symmetrical blob — wider than the truth in both directions,
// and silent about the one thing worth telling them. The extra accumulator costs a line.
//
// majorSd/minorSd/tiltDeg are the eigendecomposition of the 2x2 covariance matrix, and
// they are ONLY meaningful when the two axes share a unit AND are drawn at the same
// scale. That holds for strike location against strike height (both mm, both 2.6 px/mm)
// and does not hold for carry against offline (both yards, but drawn at 1.8 and 4.5
// px/yd). Callers in the second case use sdX/sdY and accept an axis-aligned region;
// rotating an ellipse and then scaling its axes unequally does not give the ellipse of
// the scaled data, it gives a wrong one.
struct LmPairStats {
    bool   has     = false;   // n >= kMinShotsForSpread && both spreads non-zero
    int    n       = 0;
    double meanX   = 0.0, meanY   = 0.0;
    double sdX     = 0.0, sdY     = 0.0;
    double r       = 0.0;     // correlation, −1…1
    double majorSd = 0.0, minorSd = 0.0;
    double tiltDeg = 0.0;     // of the major axis, from +x, y-down screen convention
};

// Both fields from the shots that carried BOTH. A shot the monitor read the face on but
// lost the ball for contributes to neither: a pair statistic assembled from two
// different subsets is not a statistic about any shot that was actually hit.
inline LmPairStats lmPairStats(const std::vector<LmShotValues> &shots,
                               const QString &keyX, const QString &keyY)
{
    LmPairStats out;

    std::vector<double> xs, ys;
    xs.reserve(shots.size());
    ys.reserve(shots.size());
    for (const LmShotValues &s : shots) {
        const auto ix = s.constFind(keyX), iy = s.constFind(keyY);
        if (ix == s.constEnd() || iy == s.constEnd()) continue;
        if (!std::isfinite(*ix) || !std::isfinite(*iy)) continue;
        xs.push_back(*ix);
        ys.push_back(*iy);
    }
    out.n = int(xs.size());
    if (out.n < kMinShotsForSpread)
        return out;

    double sx = 0.0, sy = 0.0;
    for (int i = 0; i < out.n; ++i) { sx += xs[size_t(i)]; sy += ys[size_t(i)]; }
    out.meanX = sx / out.n;
    out.meanY = sy / out.n;

    // Two-pass, and n-1 throughout — same reasons as the single-field path above.
    double vxx = 0.0, vyy = 0.0, vxy = 0.0;
    for (int i = 0; i < out.n; ++i) {
        const double dx = xs[size_t(i)] - out.meanX;
        const double dy = ys[size_t(i)] - out.meanY;
        vxx += dx * dx; vyy += dy * dy; vxy += dx * dy;
    }
    const double denom = double(out.n - 1);
    vxx /= denom; vyy /= denom; vxy /= denom;

    out.sdX = vxx > 0.0 ? std::sqrt(vxx) : 0.0;
    out.sdY = vyy > 0.0 ? std::sqrt(vyy) : 0.0;
    if (out.sdX <= 0.0 || out.sdY <= 0.0)
        return out;                      // a line, not an ellipse — nothing to shade
    out.r = std::clamp(vxy / (out.sdX * out.sdY), -1.0, 1.0);

    // Eigenvalues of [[vxx, vxy], [vxy, vyy]]: the variances along the pattern's own
    // two axes. Closed form for 2x2 — no iteration, no library.
    const double mid  = 0.5 * (vxx + vyy);
    const double half = 0.5 * (vxx - vyy);
    const double disc = std::sqrt(half * half + vxy * vxy);
    out.majorSd = std::sqrt(std::max(mid + disc, 0.0));
    out.minorSd = std::sqrt(std::max(mid - disc, 0.0));
    // atan2(2·vxy, vxx − vyy) / 2 is the major axis' angle. Degenerate when the pattern
    // is circular, where atan2(0,0) is 0 and any tilt is as good as any other.
    out.tiltDeg = 0.5 * std::atan2(2.0 * vxy, vxx - vyy) * 180.0 / M_PI;

    out.has = true;
    return out;
}

// The em dash every "we do not have this" reads as. One character, one meaning,
// wherever it appears on the board.
inline QString lmAbsent() { return QStringLiteral("—"); }

// The focused shot's value, or the em dash. The unit is drawn separately.
inline QString lmValueText(const LmFieldStats &st)
{
    if (!st.hasLatest || !st.def) return lmAbsent();
    return lmFormat(st.latest, lmDecimals(QString::fromUtf8(st.def->unit)));
}

// The session mean, carrying its own mu so the tile never has to caption it.
inline QString lmMeanText(const LmFieldStats &st)
{
    if (st.n <= 0 || !st.def) return lmAbsent();
    return QStringLiteral("μ ") + lmFormat(st.mean, lmDecimals(QString::fromUtf8(st.def->unit)));
}

// The spread, at the same precision as the value it describes — an SD quoted finer
// than its own measurements claims a resolution the device never had.
//
// Em dash when there is no spread to quote. The TAG STAYS on the tile either way: a
// hidden tag reflows the row, and the reader learns "not enough shots yet" from the
// dash, which is the honest answer to a question they did ask.
inline QString lmSdText(const LmFieldStats &st)
{
    if (!st.hasSpread || !st.def) return lmAbsent();
    return QStringLiteral("±") + lmFormat(st.sd, lmDecimals(QString::fromUtf8(st.def->unit)));
}

} // namespace pinpoint::analysis
