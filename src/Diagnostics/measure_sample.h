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

#include "measure_vocabulary.h"    // Measure
#include "../Core/pp_tuned_constants.h"
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
// "EXACT BY AGGREGATION" RESTS ON ONE RULE, and it is the reducers' rule, not this module's:
// A CANDIDATE'S SUPPORT IS QUERY-INDEPENDENT. reduceExtremum scores a sample over every VALID sample
// within its window (widening unclamped until the sample floor is met) — the neighbourhood comes
// from the CURVE, never from the search bounds. So the union of the spans' candidate sets is the
// whole window's candidate set, every candidate scores the same in a span as in a whole-window
// query, and min/max being idempotent makes the shared boundary sample harmless. That is why this
// cache can answer a multi-span window at all, and why it answers what the review chart answers.
//
// The rule was briefly broken by clamping a candidate's support to [from, to], to stop an extremum
// borrowing samples from outside a metric's domain. Measured on rich_7iron that cost 20 of 514
// engine-vs-card agreements (worst 1.45 on clubheadSpeed P1→P10 max), and it has no per-span fix:
// under a query-dependent support a sample 10 ms before a phase is scored over a truncated window
// inside its span and a full one inside the whole query, so no cached span is reusable.
//
// THE DOMAIN IS A MASK, NOT A CLAMP — that is the fix, and it belongs to the producers. A metric
// narrower than the swing marks the samples outside its Address→Impact domain `valid = 0` in
// swing.json (design §5.1), and both consumers already honour that mask: an out-of-domain sample is
// then neither a candidate nor part of anyone's support, at every window, without any reducer
// needing to know where a domain ends. See the mask note in buildPhaseGrid below.
//
// ── Sampling convention ────────────────────────────────────────────────────────────────────────
//
// A SHORT WINDOWED MEDIAN about each phase timestamp — the same convention WristAngleSampler
// already uses, from the same tuned constants. Deliberately not a second convention: the wrist grid
// and a norm seated from the corridor editor must be reading the same number off the same swing, or
// the two surfaces disagree about what the golfer did.
//
// The arithmetic itself is not written here any more: both the phase values and the span extremes
// come from `src/Analysis/series_reduce.h`, the one implementation the REVIEW CHART also reduces
// through (design §5.2). That is not tidiness — a corridor is authored by looking at the chart, so a
// card and an engine that reduced the same curve with two hand-rolled loops would eventually
// disagree about the number the corridor was drawn around, and nothing on screen would say so.
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
//
// 2: `club` now resolves through swingDocClub() (review.club, else capture.club.name, else the
//    stub). A v1 sidecar cached the review-or-stub answer and its size+mtime guard still matches,
//    since the fix rewrote no swing.json — the version bump is what retires it.
// 3: `spans` min/max are the extremes of the CENTRED-WINDOW MEAN (±extremumWindowUs/2) instead of
//    the extremes of the raw samples (design §5.2). Every Extremum measure's value therefore moves,
//    typically toward the mean by about one σ, and a v2 sidecar's size+mtime guard still matches the
//    unchanged swing.json — so as with v1, the version bump is the ONLY thing that retires the
//    stale numbers. `values` are untouched: the same ±15 ms median, now computed by reduceAt().
// 4: spans are CLOSED at both ends, a zero-width span is not stored at all, and the endpoint-median
//    seed is gone from the aggregation (see PhaseGridSpan and reduceOverGrid). Together those three
//    were making the engine disagree with the review chart on 36 of 170 extremum cases on one corpus
//    swing, by up to 9.4° — and a cache is only worth having if it answers what the chart answers.
inline constexpr int kPhaseGridSchemaVersion = 4;

struct PhaseGridConfig {
    // ±15 ms about the phase instant, the WristAngleSampler convention.
    int64_t windowHalfUs    = tuned::sampler::kWindowHalfUs;
    int     minValidSamples = 1;       // fewer in the window => this phase has no value

    // The centred window a span's extreme is the MEAN over. A one-sample outlier cannot be a peak
    // because a peak has to be there for the whole window; 40 ms is ≈5 samples where the pose grid
    // is dense and ≈2 where it is sparse (design §5.2, tuned::reduce).
    int64_t extremumWindowUs = tuned::reduce::kExtremumWindowUs;
};

// Whether two configurations would grid the same swing to the same numbers.
//
// Every field that MOVES A NUMBER is compared — the two windows and the sample floor — because they
// are what a sidecar's stored numbers were computed AT: a grid built at ±15 ms and served to a run
// configured for ±8 ms is a wrong answer carrying a matching size+mtime guard, which is the one
// failure mode a cache must not have. `minValidSamples` is in here rather than left out as "only a
// gate": raising it makes phases DISAPPEAR from the grid, which changes the spans as well as the
// values, and a cache holding the permissive answer would quietly re-admit every phase the stricter
// run had refused.
inline bool sameGridReduction(const PhaseGridConfig &a, const PhaseGridConfig &b)
{
    return a.windowHalfUs     == b.windowHalfUs
        && a.extremumWindowUs == b.extremumWindowUs
        && a.minValidSamples  == b.minValidSamples;
}

// One metric's value at one segmented phase.
struct PhaseGridValue {
    Phase   phase = Phase::Address;
    int64_t tUs   = 0;
    double  value = 0.0;
};

// The extremes of the continuous curve BETWEEN two adjacent segmented phases, CLOSED AT BOTH ENDS
// ([from, to]).
//
// It was half-open at the start until schema 4, to keep an aggregate from counting a sample twice.
// That reasoning was wrong twice over. Min and max are idempotent, so counting a sample twice
// changes no answer and half-openness bought nothing; and the review chart's own candidate set over
// a phase window IS closed, so a sample sitting exactly on the opening instant was a peak candidate
// for the card and not for the engine. On one corpus swing that alone put the two surfaces 9.4°
// apart on shaftAngleVsHorizontal P1→P4. The cache exists so that a corridor can be authored by
// looking at the chart; a boundary convention it does not share is a defect, not a detail.
//
// A ZERO-WIDTH SPAN IS NOT STORED. Two phases can share an instant — rich_7iron has Address and
// Takeaway at the same t_us — and a span from an instant to itself is not a search window, it is one
// sample wearing a window's clothes. Storing its value puts a nearly-raw reading into an aggregate
// that is otherwise all windowed means. So `spans` holds at MOST values.size() − 1 entries, and a
// span whose whole interval is bridged is absent for the same reason.
//
// "Extreme" means the extreme of the CENTRED-WINDOW MEAN, not of the raw samples (schema 3): the
// candidates are the valid samples inside the span, but each one is scored by the mean of its own
// ±extremumWindowUs/2 neighbourhood. So a single wild sample can no longer be a peak, and a peak
// that is genuinely there for 40 ms still is.
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

    // The reduction these numbers were computed under. Carried so a sidecar can be REFUSED when the
    // run asking for it is configured differently — see sameGridReduction().
    PhaseGridConfig config;

    const MetricPhaseGrid *metric(const QString &key) const;
    bool                   isEmpty() const { return metrics.empty(); }
};

// ── Building ────────────────────────────────────────────────────────────────
//
// From the `analysis` object of a swing.json — `metrics[]` (key/unit/t_us[]/value[]) and `phases[]`
// (phase/t_us/conf), the shapes swing_doc.cpp writes and wrist_analysis_adapter.cpp already reads.
//
// A metric may also carry `valid[]`, a 0/1 array parallel to `t_us` marking samples that were
// BRIDGED across a gated or absent run rather than measured (design §5.1). Those enter neither a
// phase's windowed median nor a span's extremes, and a phase whose whole window is bridged gets no
// entry — the same path an unsampled phase already takes. The key is written only when something is
// invalid, so a swing without it grids identically to a swing carrying an all-ones mask — which is
// what makes the mask additive. (It no longer means "identical to the previous release": schema 3
// moved every span. `values` are still identical either way, and that is what the mask tests pin.)
//
// The grid remembers the `cfg` it was built with, because the sidecar guard compares it.
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
//
// An Extremum is the aggregate of the SPANS the window covers and nothing else. It used to be seeded
// with the two endpoint windowed medians as well, on the reasoning that a peak sitting on a phase is
// still a peak — but a ±15 ms median and a 40 ms windowed mean are two different smoothing scales,
// and mixing them in one min/max made the engine report numbers the chart cannot produce (36 of 170
// extremum cases on one corpus swing). The endpoints lie inside the closed spans anyway, so nothing
// is lost. The medians survive only as a LAST RESORT: when no span in the window answered at all —
// every interval zero-width or wholly bridged — min/max of the two endpoint values still beats
// darkening a measure whose value is sitting right there, and it is what the chart falls back to.
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
//
// The guard also carries the REDUCTION the grid was built under (both windows and the sample floor),
// because size and mtime cannot see it: sweep `extremumWindowUs` and every sidecar in the library
// still matches its swing.json byte-for-byte while holding numbers from the previous window.
// `loadPhaseGrid` therefore takes the configuration the caller intends to use and refuses a grid
// that was not built that way.
QString phaseGridPath(const QString &swingDir);

QJsonObject    savePhaseGrid(const SwingPhaseGrid &grid, qint64 sourceSize, qint64 sourceMtimeMs);
SwingPhaseGrid loadPhaseGrid(const QJsonObject &root, qint64 sourceSize, qint64 sourceMtimeMs,
                             bool *guardOk = nullptr, const PhaseGridConfig &cfg = {});

// Read one swing's grid, preferring the sidecar and falling back to a full swing.json parse.
//
// `writeSidecar` follows SwingDocReader::readSwingSummary's contract for the same reason: a
// GUI-thread caller that must never fat-parse passes false and gets an empty grid for an unindexed
// swing, rather than a one-second stall per swing. The scan runs on a worker and passes true.
SwingPhaseGrid readPhaseGrid(const QString &swingDir, bool writeSidecar = true,
                             const PhaseGridConfig &cfg = {});

} // namespace pinpoint::analysis
