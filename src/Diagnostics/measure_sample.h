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

#include "characteristic.h"        // Measure
#include "../Metrics/metric_reducer.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>
#include <vector>

// Reading a MEASURE off a swing that is already on disk.
//
// This is the missing half of norm_measure_source.h. `IMeasureValueSource` declares the seam — "a
// value source knows what the swing did and knows nothing about what is normal" — but until now
// nothing implemented it outside a test fake, so nothing in the app could put a real number next to
// a norm. The corridor editor's live histogram is not decoration: a corridor grading almost
// everything Action is only visibly wrong if there are real swings drawn beneath it.
//
// ── Why a phase grid and not the measures themselves ────────────────────────────────────────────
//
// The obvious cache is "measureId -> value per swing". It is the wrong one: the pack is EDITABLE,
// so minting a measure would stale every sidecar in the library and force a full re-parse of a
// 2 GB corpus to look at one new corridor.
//
// A measure is a metric series reduced at phases, and the phases are a property of the SWING, not
// of the pack. So the cache stores the grid the reduction reads — per metric, the value at each
// segmented phase plus the extremes between adjacent phases — and the reduction happens in memory.
// New measures over already-produced metrics then cost nothing, which is exactly the case the
// editor creates.
//
// ── Why adjacent-phase spans ───────────────────────────────────────────────────────────────────
//
// An Extremum reducer searches a window for a peak, which the phase values alone cannot answer: a
// pelvis that sways back and recovers before the next phase has a peak no endpoint sees, and that
// is the entire reason Extremum is first-class (metric_reducer.h). Storing min/max between each
// pair of ADJACENT segmented phases makes any phase-bounded window exact by aggregation, at a cost
// linear in phases rather than quadratic.
//
// ── Sampling convention ────────────────────────────────────────────────────────────────────────
//
// A SHORT WINDOWED MEDIAN about each phase timestamp — the same convention WristAngleSampler
// already uses, from the same tuned constants. Deliberately not a second convention: the wrist grid
// and a norm seated from the corridor editor must be reading the same number off the same swing, or
// the two surfaces disagree about what the golfer did.
//
// Where the curve has nothing in the window, the metric's own `phaseSamples` reading is used
// instead. That fallback is not a nicety: an entire CLASS of metric ships with an empty curve and
// nothing but phaseSamples — every setup metric in a real swing.json (stanceWidth, ballPosition,
// tempoRatio, the foot-flare and toe-line angles) is read once at a position and has no curve to
// sample. Reading only the curve produced nothing for all of them, and the symptom is
// indistinguishable from "no swing carries this measure".

namespace pinpoint::analysis {

// The schema this build writes. A sidecar declaring a HIGHER version is ignored and rebuilt rather
// than partially read — it is a pure cache, so discarding it is always safe.
inline constexpr int kPhaseGridSchemaVersion = 1;

struct PhaseGridConfig {
    int64_t windowHalfUs    = 15000;   // ±15 ms about the phase instant (tuned::sampler)
    int     minValidSamples = 1;       // fewer in the window => this phase has no value
};

// One metric's value at one segmented phase.
struct PhaseGridValue {
    Phase   phase = Phase::Address;
    int64_t tUs   = 0;
    double  value = 0.0;
};

// The extremes of the continuous curve BETWEEN two adjacent segmented phases. Half-open at the
// start and closed at the end ((from, to]) so aggregating consecutive spans counts no sample twice;
// the endpoint values themselves come from the windowed medians, not from here.
struct PhaseGridSpan {
    Phase  from = Phase::Address;
    Phase  to   = Phase::Address;
    double min  = 0.0;
    double max  = 0.0;
};

struct MetricPhaseGrid {
    QString                     key;
    QString                     unit;
    std::vector<PhaseGridValue> values;   // ascending by time; one per segmented phase with samples
    std::vector<PhaseGridSpan>  spans;    // between consecutive `values` entries

    const PhaseGridValue *at(Phase p) const;
};

// One swing, reduced to everything a measure could need from it.
struct SwingPhaseGrid {
    QString swingDir;
    QString sessionId;        // the session directory name, for the draw-from filter
    QString club;
    qint64  wallclockMs = 0;
    int     ordinal     = 0;

    std::vector<MetricPhaseGrid> metrics;

    const MetricPhaseGrid *metric(const QString &key) const;
    bool                   isEmpty() const { return metrics.empty(); }
};

// ── Building ────────────────────────────────────────────────────────────────
//
// From the `analysis` object of a swing.json — `metrics[]` (key/unit/t_us[]/value[]) and `phases[]`
// (phase/t_us/conf), the shapes swing_doc.cpp writes and wrist_analysis_adapter.cpp already reads.
SwingPhaseGrid buildPhaseGrid(const QJsonObject &analysis, const PhaseGridConfig &cfg = {});

// ── Reduction ───────────────────────────────────────────────────────────────
//
// nullopt => this measure could not be produced FOR THIS SWING: no such metric, or a phase the
// reducer needs was never segmented. Never a zero, because "not assessed" and "assessed and fine"
// are different statements and the whole Diagnostics module exists to keep them apart.
//
// Extremum semantics, made explicit because metric_reducer.h's own comment is ambiguous: the result
// is the SIGNED deviation `extremum(window, sense) - value(anchor)`, not `max |value - anchor|`. An
// absolute deviation cannot carry a `sense`, and every anchored Extremum in the shipped pack means
// the signed reading — m_pelvisSwayBack is the most negative sway relative to address, not the
// largest excursion in either direction.
std::optional<double> reduceOverGrid(const SwingPhaseGrid &grid, const Measure &m);

// The same, for a bare metric key + reducer, so a caller with no Measure in hand (the sidecar
// tests, a future producer) reaches the identical arithmetic.
std::optional<double> reduceOverGrid(const SwingPhaseGrid &grid, const QString &metricKey,
                                     const Reducer &r);

// ── Sidecar ─────────────────────────────────────────────────────────────────
//
// <swingDir>/swing_phasegrid.json — a few KB beside a 32 MB swing.json. Guarded on the source
// file's size and mtime exactly as swing_summary.json is: a stale guard means rebuild, never a
// silently wrong number. Always safe to delete.
QString phaseGridPath(const QString &swingDir);

QJsonObject    savePhaseGrid(const SwingPhaseGrid &grid, qint64 sourceSize, qint64 sourceMtimeMs);
SwingPhaseGrid loadPhaseGrid(const QJsonObject &root, qint64 sourceSize, qint64 sourceMtimeMs,
                             bool *guardOk = nullptr);

// Read one swing's grid, preferring the sidecar and falling back to a full swing.json parse.
//
// `writeSidecar` follows SwingDocReader::readSwingSummary's contract for the same reason: a
// GUI-thread caller that must never fat-parse passes false and gets an empty grid for an unindexed
// swing, rather than a one-second stall per swing. The scan runs on a worker and passes true.
SwingPhaseGrid readPhaseGrid(const QString &swingDir, bool writeSidecar = true,
                             const PhaseGridConfig &cfg = {});

} // namespace pinpoint::analysis
