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

// Corridor-bar reduction — pure, header-only, no Qt-GUI. The maths behind the
// review corridor bar (NormativeBar in metric detail): the value→x domain of a
// single reading against a single normative corridor. Kept out of QML JS by the
// analysis pipeline guide's rule (§6.2): QML positions and paints, C++ decides
// the numbers. (The file once carried the whole per-shot dashboard's reductions;
// the corridor bar is what outlived that surface.)
//
// The QML façade is ChartMetrics::barDomain.
// Unit-tested standalone in src/Analysis/tests/dashboard_reductions_test.cpp.

#include <algorithm>
#include <cmath>
#include <limits>

namespace pinpoint::analysis {

// The value→x domain of a horizontal corridor bar.
struct BarDomain {
    double lo    = 0.0;
    double hi    = 0.0;
    bool   valid = false;          // false ⇒ nothing finite to draw; the caller hides the bands
};

// Domain for one corridor bar.
//
// TWO-SIDED, which is 105 of the 106 shipped measures: the amber band padded by
// `padFrac` each side, so the corridor reads as a band rather than as the whole track.
// Falling back to the green band when amber is degenerate (the unconfigured default),
// then to value±1 when both are. This is exactly what the two bars did between them
// before shapes existed, and it must stay bit-identical — the one-sided rule is an
// addition, not a replacement.
//
// ONE-SIDED is the new case, and the failure it fixes is NOT the one the brief
// predicted. The brief expected the amber span to be open and the domain to come out
// degenerate or absurd. It does not: the open side's numeric edge is `mu` (the norm
// decided that, not this function), so the amber span is a healthy k×sigma and the
// domain is perfectly finite. What is wrong is subtler and worse. On a floor at
// mu = 1.48, every reading above 1.48 grades IDEAL, but the domain would stop dead at
// 1.48 — so a smash of 1.55 clamps to the last pixel of the track, indistinguishable
// from 5.0, sitting on a hard band edge that reads "the corridor ends here, you are
// outside it". The exact opposite of what it means.
//
// So the open side is anchored past the FURTHEST of (the aspiration, the reading) by
// `openFrac` of the graded span, leaving room the caller fades the band out across.
// The closed side keeps the ordinary pad, and the reading does not participate in it —
// a bad reading clamps to the graded edge there, exactly as it always has.
inline BarDomain barDomain(double greenLo, double greenHi,
                           double amberLo, double amberHi,
                           bool lowOpen, bool highOpen,
                           double value, bool hasValue,
                           double padFrac = 0.12, double openFrac = 0.35)
{
    const double amberSpan  = amberHi - amberLo;
    const double greenSpan  = greenHi - greenLo;
    const bool   amberOk    = std::isfinite(amberSpan) && amberSpan > 1e-9;
    const bool   greenOk    = std::isfinite(greenSpan) && greenSpan > 1e-9;
    const bool   valueOk    = hasValue && std::isfinite(value);

    double baseLo = 0.0, baseHi = 0.0;
    if (amberOk)      { baseLo = amberLo;     baseHi = amberHi; }
    else if (greenOk) { baseLo = greenLo;     baseHi = greenHi; }
    else if (valueOk) { baseLo = value - 1.0; baseHi = value + 1.0; }
    else              return {};               // no corridor and no reading: nothing to scale to

    const double span = std::max(1e-9, baseHi - baseLo);

    BarDomain d;
    d.lo = lowOpen  ? (valueOk ? std::min(baseLo, value) : baseLo) - openFrac * span
                    : baseLo - padFrac * span;
    d.hi = highOpen ? (valueOk ? std::max(baseHi, value) : baseHi) + openFrac * span
                    : baseHi + padFrac * span;

    if (!std::isfinite(d.lo) || !std::isfinite(d.hi) || !(d.hi > d.lo)) return {};
    d.valid = true;
    return d;
}

} // namespace pinpoint::analysis
