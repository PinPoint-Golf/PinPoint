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

// Kinematic-sequence reduction — pure, header-only, no Qt-GUI. Given a set of
// speed series (hand / clubhead today; pelvis / thorax / lead-arm when the body-IMU
// roles and the real Sequence producer land), returns the ordered peak-speed events
// with the gaps between them: it emphasises ORDER and GAPS, not curves, and lights
// up the full proximal→distal chain automatically as more series become available.
//
// CURRENTLY CONSUMER-LESS: its UI surface (the per-shot dashboard's Sequence zone)
// was removed, and the reduction is kept, tested, for the sequence surface the model
// anticipates. Kept out of QML JS by the "reductions live in C++" rule (analysis
// pipeline guide §6.2).
// Unit-tested standalone in src/Analysis/tests/kinematic_sequence_test.cpp.

#include <QString>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pinpoint::analysis {

// One named speed series: parallel time (µs, ascending) + value arrays.
struct SeqSeries {
    QString              key;
    std::vector<int64_t> tUs;
    std::vector<double>  value;
};

// One node in the resolved sequence: the peak of a series and where it sits.
struct SeqNode {
    QString key;
    int64_t tPeakUs = 0;      // time of the peak (µs, absolute grid domain)
    double  peak    = 0.0;    // the peak value (signed; magnitude drives selection)
    double  gapMs   = 0.0;    // ms since the previous node's peak (0 for the first)
    int     order   = 0;      // 0-based position in the ordered chain
};

// Peak-value index of a series: the earliest sample of largest magnitude. Returns
// -1 for an empty series. Ties resolve to the earliest occurrence (strict >).
inline int seqPeakIndex(const SeqSeries &s)
{
    const int n = int(std::min(s.tUs.size(), s.value.size()));
    if (n == 0) return -1;
    int    best  = 0;
    double bestM = std::abs(s.value[0]);
    for (int i = 1; i < n; ++i) {
        const double m = std::abs(s.value[i]);
        if (m > bestM) { bestM = m; best = i; }
    }
    return best;
}

// Ordered peak-speed events across `series`, sorted ascending by peak time, with
// `gapMs` = the delta from the previous node's peak. Empty series are dropped, so a
// partially-instrumented capture yields the measurable nodes only — never a fake
// one. A stable sort keeps input order for exact peak-time ties.
inline std::vector<SeqNode> kinematicSequenceNodes(const std::vector<SeqSeries> &series)
{
    std::vector<SeqNode> nodes;
    nodes.reserve(series.size());

    for (const SeqSeries &s : series) {
        const int pi = seqPeakIndex(s);
        if (pi < 0) continue;                 // no data → not a node (degrade, don't fake)
        SeqNode n;
        n.key     = s.key;
        n.tPeakUs = s.tUs[pi];
        n.peak    = s.value[pi];
        nodes.push_back(n);
    }

    std::stable_sort(nodes.begin(), nodes.end(),
                     [](const SeqNode &a, const SeqNode &b) { return a.tPeakUs < b.tPeakUs; });

    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].order = int(i);
        nodes[i].gapMs = (i == 0) ? 0.0
                                  : double(nodes[i].tPeakUs - nodes[i - 1].tPeakUs) / 1000.0;
    }
    return nodes;
}

} // namespace pinpoint::analysis
