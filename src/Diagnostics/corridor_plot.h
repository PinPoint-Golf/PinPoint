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

#include "norm.h"

#include <QString>

#include <limits>
#include <vector>

// The corridor, as a picture: the distribution a norm CLAIMS, drawn over the readings a library
// actually contains.
//
// Layout in C++ for the same reason dag_layout.h is: the curve, the band edges and the counts are
// the only things about this surface that CAN be tested, and geometry computed inside delegate
// bindings is geometry nothing can assert. QML draws these points and positions nothing.
//
// ── What the picture claims ────────────────────────────────────────────────────────────────────
//
// THE CURVE IS SPLIT-NORMAL, NOT A BELL. `sigmaLo` and `sigmaHi` are asymmetric by design and not
// as an option — norm.h is explicit that ball position forward is tolerated far more than back, and
// the same holds for alignment and stance width. Drawing one symmetric bell would misstate every
// such corridor, and it would misstate it in the direction that matters: it would show tolerance
// the norm does not grant on the tight side. Two half-normals share a peak at `mu` and each falls
// away at its own rate, which is continuous at mu and is the standard two-piece normal.
//
// THE BANDS ARE THE BANDS THAT GRADE. Every edge comes from bandEdgesOf(), which is the one
// definition grade() itself applies — including the precedence where an explicit monitor band
// dominates the z-derived edge. A second computation here is exactly how a surface ends up drawing
// a corridor the app does not use, which norm.h documents as a bug that already happened once.
//
// THE SAMPLES RIDE ON THE CURVE ITSELF. Each reading sits at its own x, lifted to the density the
// NORM assigns that value — so the dots show where the library falls on the distribution being set.
// A corridor centred where the swings are gathers its dots around the peak; one centred elsewhere
// strands them along the floor of a tail, which is the whole question this view exists to answer.
//
// An earlier version computed a second, EMPIRICAL density and hung the dots off that, on the
// reasoning that dots on the theoretical curve lie on the line by construction and therefore say
// nothing. That reasoning confused two questions. "Is this data Gaussian?" cannot be answered this
// way — but it is not the question. "Where do these swings fall relative to what I am claiming?"
// is, and the answer is carried entirely by the dots' spread ALONG x. The empirical curve was a
// second line, with a bin count and a smoothing radius shaping what the reader saw: the histogram
// this view exists to replace, in a different coat.
//
// AN UNGRADED TAIL IS DRAWN AS UNGRADED. On a Floor or Ceiling measure only one tail grades; the
// other is open up to plausibility. That side carries no band and its curve is flagged, because a
// full bell over a one-sided measure states a judgement the norm does not make.

namespace pinpoint::analysis {

struct CorridorPlotOptions {
    double width  = 640.0;
    double height = 180.0;

    // Points along the theoretical curve. Enough to look smooth at any width a pane gives it.
    int curveSteps = 192;

    // A cap, not a sample size, and REPORTED when it bites: a silently truncated scatter reads as
    // "that is the whole library".
    int maxSamplePoints = 600;

    // A window supplied by the caller, in the measure's own units. NaN means "work it out".
    //
    // This exists for dragging. The window is normally derived from the band edges, so moving mu
    // moves the window — and then the mapping from a pointer position to a value is no longer a
    // function of the pointer: the value shifts the window, which shifts the value. A handle under
    // that feedback jumps and cannot be placed. Freezing the window for the duration of a drag makes
    // pixels mean one thing, which is the least a ruler can do.
    double windowMin = std::numeric_limits<double>::quiet_NaN();
    double windowMax = std::numeric_limits<double>::quiet_NaN();
};

struct CorridorPoint {
    double x = 0.0;   // pixels
    double y = 0.0;
};

// One graded region, laid out left to right. `grade` is the band's own word, so a delegate colours
// by meaning rather than by position.
struct CorridorBand {
    Grade  grade = Grade::NotMeasured;
    double x     = 0.0;   // pixels
    double w     = 0.0;
};

struct CorridorPlot {
    double width  = 0.0;
    double height = 0.0;

    // The window, in the measure's own units. Always spans BOTH the whole graded corridor and every
    // reading: a window that cropped either would be a picture that lies by omission.
    double  xMin = 0.0;
    double  xMax = 0.0;
    QString unit;

    std::vector<CorridorBand>  bands;
    std::vector<CorridorPoint> curve;     // what the norm claims
    std::vector<CorridorPoint> samples;   // one reading each, sitting ON that curve
    // Every reading's x, for the baseline strip. The rug is where raw density lives — unbinned,
    // unsmoothed and uninterpreted — so nothing above it has to carry that job.
    std::vector<double>        rug;

    // Handle positions, in pixels. `mu` is the aspiration; the ideal edges are what the two
    // draggable handles bind — the IDEAL band, which is `mu ∓ idealMaxZ * sigma` under the active
    // policy and not the norm's raw claim.
    double muX      = 0.0;
    double idealLoX = 0.0;
    double idealHiX = 0.0;
    double watchLoX = 0.0;
    double watchHiX = 0.0;

    bool lowOpen  = false;   // this tail does not grade
    bool highOpen = false;

    int n           = 0;
    int ideal       = 0;
    int good        = 0;
    int watch       = 0;
    int action      = 0;
    int implausible = 0;     // outside the believed range: NOT a grade, and never counted as one

    bool    truncated = false;   // more readings than maxSamplePoints; the scatter is a subset
    QString note;                // the finding, in words. Empty when there is nothing to say.

    bool hasSamples() const { return n > 0; }
};

// ── How precisely a corridor may be stated ──────────────────────────────────
//
// A corridor is only as precise as the thing measuring it, and a pose estimate is not resolving
// tenths of a degree. Left ungoverned, a drag stores 30.418273 % of stance width — a figure that
// reads as a measurement, survives into the file, and is really just where a pointer happened to
// stop. False precision in authored content is worse than coarse content: it invites the reader to
// believe a distinction nobody can make.
//
// So every corridor number snaps to a quantum chosen for its UNIT, and one table decides it, because
// the drag, the typed field, the table cell and the readout must all agree about what a value is.
struct CorridorPrecision {
    double step     = 0.01;   // the quantum a value snaps to
    int    decimals = 2;      // how it is rendered — derived from the step, never chosen apart
};

// The quantum for a unit. Unknown units deliberately get the FINEST setting rather than a guess:
// rounding a quantity we do not recognise could destroy a legitimate value, and being too precise
// about something unfamiliar is the lesser fault.
CorridorPrecision corridorPrecisionFor(const QString &unit);

// `v` rounded to that quantum. Everything that writes a corridor number goes through here.
double snapCorridorValue(double v, const QString &unit);

// `values` is every reading of this measure in the library, unsorted and unfiltered. Implausible
// ones are counted apart rather than dropped: "we do not believe this number" and "this swing was
// bad" are different statements, and merging them would launder a capture fault into a diagnosis.
CorridorPlot layoutCorridorPlot(const Norm &norm, Shape shape,
                                const std::vector<double> &values,
                                const GradePolicy         &policy  = {},
                                const CorridorPlotOptions &options = {});

// The split-normal density at `x`, normalised so the peak is 1. Exposed for the test, and because
// it is the one place the curve's shape is decided.
double splitNormalPeakNormalised(double x, double mu, double sigmaLo, double sigmaHi);

} // namespace pinpoint::analysis
