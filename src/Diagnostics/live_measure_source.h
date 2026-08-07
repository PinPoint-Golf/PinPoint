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

#include "measure_sample.h"
#include "norm_measure_source.h"

#include <QHash>
#include <QString>

#include <memory>
#include <optional>

// THE LIVE VALUE SOURCE: a recorded swing, read as measures.
//
// `IMeasureValueSource` has been the seam since the engine was written and every implementation of
// it so far has been a test fake. This is the first one that reads a swing off disk, and it is the
// point where the pack stops being a document and starts making claims about a golfer.
//
// It owns almost no arithmetic. `measure_sample.h` already parses a swing.json into a
// SwingPhaseGrid and reduces a Measure over it with nullopt-never-zero semantics; the pack already
// resolves a measureId to a Measure. This class is the JOIN of those two plus one thing neither
// could carry alone — WHY a measure produced nothing.
//
// ── Why the missing reason is a first-class output ─────────────────────────────────────────────
//
// `std::optional<Value>` is the right return type and it is a lossy one. A review panel showing a
// greyed cell has to say something in it, and the four sentences below are genuinely different
// pieces of advice: buy a launch monitor, re-record with the camera further back, wait for a
// producer we have not written, or nothing at all because the pack does not carry that measure.
// Folding them into one "not available" would make the greyed half of the panel unreadable, and
// inventing a reason we cannot support would be worse — this is why the vocabulary below is
// MECHANICAL. "Metric not produced on this capture" is a statement about our own pipeline and is
// always true when it is said. "Shaft occluded at P6" would be a guess about a camera we cannot
// see, dressed as a diagnosis, and there is no code path here that could know it.
//
// ── What this class deliberately does not do ──────────────────────────────────────────────────
//
// It attaches no corridor and no grade — that is `NormMeasureSource`, and the split is the one
// norm_measure_source.h argues for: a value source knows what the swing did and knows nothing about
// what is normal. `detectForSwing()` at the bottom of this header is the convenience that stacks
// the two for one swing, and it is the whole of what a session model needs to call.

namespace pinpoint::analysis {

// WHY a measure could not be read. The `None` member is not padding: a caller iterating the pack
// wants one lookup that answers both "did this resolve" and "why not", and an empty QString for
// success is a sentinel a reader has to know about.
//
// An ENUM as well as a sentence because the two have different lifetimes. The sentence is UI copy
// and will be reworded, translated and shortened; the token is what a test pins and what a caller
// switches on to decide whether to offer "connect a launch monitor". Deriving either from the other
// would tie a behaviour to a string somebody is going to edit.
enum class MissingKind {
    None,                       // it read; there is no reason to give
    UnknownMeasure,             // no measure with this id in the loaded pack
    NoMetricBinding,            // the measure names no metricKey — Composed, or authored ahead of a producer
    NoAnalysis,                 // the swing carries no phase grid at all: unanalysed, or unsegmented
    NoLaunchMonitor,            // an lm.* measure, and this shot has no launch-monitor data whatever
    LaunchMonitorFieldAbsent,   // there IS launch-monitor data; this device did not report this field
    MetricNotProduced,          // the metric key is absent from this capture's analysis
    PhaseNotSegmented,          // the metric is here, but a phase the reducer needs was never found
};

// The sentence a not-assessable cell shows. Generic on purpose — see the header comment on why
// these are mechanical statements about the pipeline rather than guesses about the capture.
QString missingReasonText(MissingKind k);

// One recorded swing, presented as measure values.
//
// Construction reads the phase grid, which prefers the swing's sidecar and falls back to a full
// swing.json parse (measure_sample.h). That fallback is a second on a 33 MB document, so a caller
// batching a session should build grids on a worker and hand them in through the second
// constructor rather than paying for the parse once per source.
class LiveMeasureSource final : public IMeasureValueSource {
public:
    // `writeSidecar` follows readPhaseGrid()'s contract exactly: false means "never fat-parse", and
    // an unindexed swing then reads as having no analysis rather than stalling a GUI thread.
    LiveMeasureSource(const QString &swingDir, const CharacteristicPack &pack,
                      bool writeSidecar = true, const PhaseGridConfig &cfg = {});

    // The batched form: the caller already has the grid (a session scan builds one per swing on a
    // worker) and this costs nothing.
    LiveMeasureSource(SwingPhaseGrid grid, const CharacteristicPack &pack);

    // nullopt => not produced for this swing, and `missingKind()` then says which of the reasons
    // above applies. Never zero for an absent measure — the distinction between "not assessed" and
    // "assessed and fine" is the one the whole module exists to keep.
    std::optional<Value> value(const QString &measureId) const override;

    // The reason the LAST call to value() for this id gave, or None/empty for an id that read or
    // was never asked. Recorded in a mutable map rather than returned alongside the value because
    // the seam's signature is fixed and shared with every fake that implements it — a side channel
    // that only this implementation offers is honest about which of the two is the contract.
    MissingKind missingKind(const QString &measureId) const;
    QString     missingReason(const QString &measureId) const;

    // ── Identity, for context resolution ────────────────────────────────────
    //
    // Both come from the grid, which read them out of the swing doc (measure_sample.cpp). The club
    // therefore carries the house-wide "DRIVER" stub for a shot whose review block declares none —
    // swing_doc.cpp's summaryFromRoot() applies the same default, so every surface agrees about
    // which club a swing was hit with.
    //
    // KNOWN LIMITATION, stated rather than buried: that stub means this seam cannot tell an
    // undeclared club from a declared driver, so `contextId()` never reports the undeclared case
    // and `NormMeasureSource::contextInferred` never trips through this path. A caller that knows
    // better — the review panel, which holds the user's own club selection — should pass its own
    // context id to detectForSwing() instead.
    QString club() const;
    QString sessionId() const;
    QString swingDir() const;

    // The club as a node of the context tree. Never empty: an unrecognised club resolves to the
    // default context (context_tree.h).
    QString contextId() const;

    // True when this shot carries launch-monitor data, evidenced by any `lm.` metric in the grid.
    //
    // The `launchMonitor` block of the swing doc would be the direct answer and is not consulted:
    // reading it means a second full parse of a document the grid already summarised, for a fact
    // the grid already implies. A block that produced no lm.* metrics at all is indistinguishable
    // from no block, and reports as no launch monitor — which is what a golfer's panel should say
    // either way.
    bool hasLaunchMonitor() const;

    const SwingPhaseGrid &grid() const { return m_grid; }

private:
    void note(const QString &measureId, MissingKind k) const;

    SwingPhaseGrid                  m_grid;
    const CharacteristicPack       &m_pack;
    mutable QHash<QString, MissingKind> m_reasons;
};

// ── The whole stack, for one swing ──────────────────────────────────────────
//
// values -> norms -> grades -> findings, which is the chain a session model runs per shot and the
// one thing it should not have to assemble itself. Four objects have to agree about the context id
// (the norm lookup, the inferred-context demotion, the condition bindings and what the UI says it
// graded against) and assembling them at each call site is how three of them end up agreeing.
struct LiveDetection {
    DetectionResult result;
    QString         club;        // as recorded on the swing
    QString         contextId;   // the tree node it was graded at
};

// `contextIdOverride` wins when non-empty. See LiveMeasureSource::club() for the case that needs
// it: a caller holding the user's own club selection knows something the swing doc's stub erased.
//
// The context tree comes from the norm provider, which is the same tree the norms were authored
// against — a second tree here could make a condition inapplicable at a node the norm resolved at.
LiveDetection detectForSwing(const LiveMeasureSource &values, const CharacteristicPack &pack,
                             const std::shared_ptr<const INormProvider> &norms,
                             const GradePolicy &policy = {}, const Cohort &athlete = {},
                             const QString &contextIdOverride = QString());

} // namespace pinpoint::analysis
