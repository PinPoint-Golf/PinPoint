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

#include "corridor_plot.h"

#include "diagnostics_health.h"   // kOneBandShare / kMinCorpusForShare — ONE threshold, not two

#include <QObject>

#include <algorithm>
#include <cmath>

namespace pinpoint::analysis {

double splitNormalPeakNormalised(double x, double mu, double sigmaLo, double sigmaHi)
{
    // Peak-normalised rather than area-normalised. The plot is about SHAPE and position against the
    // bands; scaling by area would make a tight corridor tower over a loose one for reasons that
    // have nothing to do with whether either is right, and the y axis carries no units anybody
    // reads. Both halves therefore reach 1 at mu, which is also what makes the join continuous.
    const double sigma = (x < mu) ? sigmaLo : sigmaHi;
    if (sigma <= 0.0) return (std::abs(x - mu) < 1e-12) ? 1.0 : 0.0;
    const double z = (x - mu) / sigma;
    return std::exp(-0.5 * z * z);
}

CorridorPrecision corridorPrecisionFor(const QString &unit)
{
    // Each of these is "the smallest difference the thing behind this number can actually show".
    struct Entry { const char *unit; double step; };
    static const Entry table[] = {
        // Whole degrees. Pose estimation is nowhere near a tenth of one, and every angular
        // corridor in the shipped set is stated in whole degrees already.
        { "°",                1.0 },
        // Whole millimetres, expressed in each length unit's own terms.
        { "mm",               1.0 },
        { "cm",               0.1 },
        { "yd",               1.0 },
        // Whole percent. These are body-relative fractions read off a silhouette; a tenth of a
        // percent of stance width is well under a pixel.
        { "% stance width",   1.0 },
        { "% shoulder width", 1.0 },
        { "% arm length",     1.0 },
        // Tempo and its kin are quoted to one decimal by everyone who quotes them ("3.0 to 1"),
        // and one video frame moves a tempo ratio by more than 0.01.
        { "ratio",            0.1 },
        { ":1",               0.1 },
        // Speeds and rates, at about the precision the hardware reporting them claims.
        { "mph",              1.0 },
        { "mph/s",            1.0 },
        { "°/s",             10.0 },
        { "rpm",             50.0 },
    };

    CorridorPrecision p;
    // fromUtf8, NOT QLatin1String: "°" is U+00B0, two bytes in this source file's UTF-8, and
    // comparing it as Latin-1 reads those bytes as two characters that match nothing. Every angular
    // corridor in the shipped set — 108 of them — silently kept full float precision because of it.
    for (const Entry &e : table)
        if (unit == QString::fromUtf8(e.unit)) { p.step = e.step; break; }

    // Decimals FOLLOW the step rather than being chosen beside it: two numbers that can disagree
    // about the same quantity is how a field renders 30.0 and stores 30.04.
    p.decimals = std::clamp(int(std::ceil(-std::log10(p.step))), 0, 3);
    return p;
}

double snapCorridorValue(double v, const QString &unit)
{
    const double step = corridorPrecisionFor(unit).step;
    if (step <= 0.0 || !std::isfinite(v)) return v;
    return std::round(v / step) * step;
}

CorridorPlot layoutCorridorPlot(const Norm &norm, Shape shape, const std::vector<double> &values,
                                const GradePolicy &policy, const CorridorPlotOptions &options)
{
    CorridorPlot p;
    p.width  = options.width;
    p.height = options.height;
    p.unit   = norm.unit;

    // The edges that GRADE. bandEdgesOf() is the one definition grade() applies, monitor-band
    // precedence included; recomputing them here is exactly how a surface ends up drawing a
    // corridor the app does not use.
    const NormBandEdges edges = bandEdgesOf(norm, policy, /*marginOverride*/ -1.0, shape);
    p.lowOpen  = edges.lowOpen;
    p.highOpen = edges.highOpen;

    // ── The window ──────────────────────────────────────────────────────────
    //
    // Must contain the whole graded corridor AND every reading. Cropping either would be a picture
    // that lies by omission — and the readings outside the corridor are precisely the ones an
    // author is trying to see.
    double lo = std::min(edges.watchLo, edges.idealLo);
    double hi = std::max(edges.watchHi, edges.idealHi);
    // A one-sided norm collapses its open edge to mu, so widen by the graded side's spread to give
    // the open tail somewhere to run.
    const double spread = std::max({ norm.sigmaLo, norm.sigmaHi, 1e-9 });
    lo = std::min(lo, norm.mu - policy.watchMaxZ * spread);
    hi = std::max(hi, norm.mu + policy.watchMaxZ * spread);

    for (double v : values) {
        if (!std::isfinite(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (norm.plausibleLo.has_value()) lo = std::min(lo, *norm.plausibleLo);
    if (norm.plausibleHi.has_value()) hi = std::max(hi, *norm.plausibleHi);

    if (!(hi > lo)) { hi = norm.mu + 1.0; lo = norm.mu - 1.0; }
    const double pad = (hi - lo) * 0.06;
    p.xMin = lo - pad;
    p.xMax = hi + pad;

    // A caller-supplied window wins outright — see CorridorPlotOptions. It is honoured even when it
    // crops, because the caller holding it steady is the point; the derived window above is what a
    // drag would otherwise be fighting.
    if (std::isfinite(options.windowMin) && std::isfinite(options.windowMax)
        && options.windowMax > options.windowMin) {
        p.xMin = options.windowMin;
        p.xMax = options.windowMax;
    }

    const double span = p.xMax - p.xMin;
    auto toX = [&](double value) { return (value - p.xMin) / span * p.width; };

    // ── Bands ───────────────────────────────────────────────────────────────
    //
    // Laid out left to right as regions rather than as edges, so a delegate fills them without
    // deciding what sits between two numbers. An open tail carries no band at all: on a Floor
    // measure everything above mu is Ideal by construction, and drawing Watch and Action up there
    // would state a judgement the norm does not make.
    auto addBand = [&](Grade g, double from, double to) {
        if (!(to > from)) return;
        CorridorBand b;
        b.grade = g;
        b.x     = toX(from);
        b.w     = toX(to) - toX(from);
        p.bands.push_back(b);
    };

    if (p.lowOpen) {
        addBand(Grade::Ideal, p.xMin, edges.idealHi);
    } else {
        addBand(Grade::Action, p.xMin, edges.watchLo);
        addBand(Grade::Watch, edges.watchLo, edges.idealLo);
        addBand(Grade::Ideal, edges.idealLo, p.highOpen ? p.xMax : edges.idealHi);
    }
    if (!p.highOpen) {
        addBand(Grade::Watch, edges.idealHi, edges.watchHi);
        addBand(Grade::Action, edges.watchHi, p.xMax);
    }

    p.muX      = toX(norm.mu);
    p.idealLoX = toX(edges.idealLo);
    p.idealHiX = toX(edges.idealHi);
    p.watchLoX = toX(edges.watchLo);
    p.watchHiX = toX(edges.watchHi);

    // ── The curve the norm claims ───────────────────────────────────────────
    const int steps = std::max(8, options.curveSteps);
    p.curve.reserve(size_t(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double v = p.xMin + span * (double(i) / steps);
        const double d = splitNormalPeakNormalised(v, norm.mu, norm.sigmaLo, norm.sigmaHi);
        p.curve.push_back(CorridorPoint{ toX(v), p.height - d * p.height });
    }

    // ── What the library actually did ───────────────────────────────────────
    std::vector<double> clean;
    clean.reserve(values.size());
    for (double v : values) {
        if (!std::isfinite(v)) continue;
        // Counted apart, never dropped and never graded: "we do not believe this number" and "this
        // swing was bad" are different statements, and merging them turns a capture fault into a
        // confident diagnosis.
        if (norm.isImplausible(v)) { ++p.implausible; continue; }
        clean.push_back(v);

        switch (grade(v, norm, policy, shape)) {
        case Grade::Ideal:  ++p.ideal;  break;
        case Grade::Good:   ++p.good;   break;
        case Grade::Watch:  ++p.watch;  break;
        case Grade::Action: ++p.action; break;
        case Grade::NotMeasured: break;
        }
    }
    p.n = int(clean.size());

    if (p.n > 0) {
        // The rug takes every reading, always. It is the one place raw density appears with nothing
        // done to it, which is what lets everything above it stay a single line.
        p.rug.reserve(clean.size());
        for (double v : clean) p.rug.push_back(toX(v));

        // A cap on the SCATTER, and it is REPORTED. Every reading is still counted and still on the
        // rug; only the dots thin. A scatter silently showing a third of the library would read as
        // the whole of it. Strided over the unsorted readings, so the thinned cloud keeps the shape
        // of the full one rather than favouring one end of the library.
        const int cap    = std::max(1, options.maxSamplePoints);
        const int stride = std::max(1, p.n / cap);
        p.truncated      = stride > 1;

        // Each reading lifted to the density the NORM gives it — so the dots trace the curve being
        // set, and where they gather along it is where this library falls on that claim.
        for (size_t i = 0; i < clean.size(); i += size_t(stride)) {
            const double v = clean[i];
            const double d = splitNormalPeakNormalised(v, norm.mu, norm.sigmaLo, norm.sigmaHi);
            p.samples.push_back(CorridorPoint{ toX(v), p.height - d * p.height });
        }
    }

    // ── The finding, in words ───────────────────────────────────────────────
    //
    // The whole argument for showing a distribution at all: a corridor grading almost everything
    // into one band is visibly wrong to somebody who has never heard of a standard deviation. The
    // threshold is diagnostics_health.h's, not a second one invented here, so this view and the
    // health list cannot disagree about the same corridor.
    if (p.n >= kMinCorpusForShare) {
        struct Share { Grade g; int n; const char *word; };
        const Share shares[] = { { Grade::Ideal,  p.ideal,  QT_TR_NOOP("Ideal") },
                                 { Grade::Good,   p.good,   QT_TR_NOOP("Good") },
                                 { Grade::Watch,  p.watch,  QT_TR_NOOP("Watch") },
                                 { Grade::Action, p.action, QT_TR_NOOP("Action") } };
        for (const Share &s : shares) {
            if (double(s.n) / double(p.n) < kOneBandShare) continue;
            p.note = QObject::tr("%1% of %2 readings grade %3 — this corridor is not telling "
                                 "swings apart.")
                         .arg(int(std::lround(100.0 * s.n / p.n)))
                         .arg(p.n)
                         .arg(QObject::tr(s.word));
            break;
        }
        if (p.note.isEmpty() && p.implausible > 0
            && double(p.implausible) / double(p.implausible + p.n) > 0.05)
            p.note = QObject::tr("%1 readings fall outside the believed range — check the capture "
                                 "before trusting this corridor.").arg(p.implausible);
    } else if (p.n > 0) {
        p.note = QObject::tr("Only %n reading(s) — too few to judge this corridor by.", "", p.n);
    }

    return p;
}

} // namespace pinpoint::analysis
