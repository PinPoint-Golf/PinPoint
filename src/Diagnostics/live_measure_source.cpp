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

#include "live_measure_source.h"

#include "context_tree.h"

#include <utility>

namespace pinpoint::analysis {

namespace {

// Everything a launch monitor produces is namespaced. One place, because the missing-reason branch
// and hasLaunchMonitor() must agree about what counts as a launch-monitor metric or a shot with a
// monitor attached could still be told it has none.
const QLatin1String kLmPrefix("lm.");

} // namespace

QString missingReasonText(MissingKind k)
{
    // MECHANICAL PHRASING, and it is a rule rather than a preference. Every sentence here is a
    // statement about our own pipeline or the pack, so it is true whenever it is shown. The
    // tempting alternative — "the shaft was occluded", "the golfer left frame" — reads as more
    // helpful and is a guess: nothing on this path sees the video, and a confident wrong reason
    // sends a golfer to re-record a capture that was fine.
    switch (k) {
    case MissingKind::None:                     return QString();
    case MissingKind::UnknownMeasure:           return QStringLiteral("no measure with this id in the loaded pack");
    case MissingKind::NoMetricBinding:          return QStringLiteral("this measure has no metric behind it yet");
    case MissingKind::NoAnalysis:               return QStringLiteral("this swing has no analysis to read");
    case MissingKind::NoLaunchMonitor:          return QStringLiteral("no launch monitor data on this shot");
    case MissingKind::LaunchMonitorFieldAbsent: return QStringLiteral("not reported by the launch monitor on this shot");
    case MissingKind::MetricNotProduced:        return QStringLiteral("metric not produced on this capture");
    case MissingKind::PhaseNotSegmented:        return QStringLiteral("the phase this reads at was not segmented");
    }
    return QString();
}

// ── Construction ────────────────────────────────────────────────────────────

LiveMeasureSource::LiveMeasureSource(const QString &swingDir, const CharacteristicPack &pack,
                                     bool writeSidecar, const PhaseGridConfig &cfg)
    : m_grid(readPhaseGrid(swingDir, writeSidecar, cfg)), m_pack(pack)
{
    // readPhaseGrid() sets swingDir on every path it returns a grid from, but not on the empty
    // grid it returns for a missing or unreadable document — and a source that cannot say which
    // swing it failed on is unloggable.
    if (m_grid.swingDir.isEmpty())
        m_grid.swingDir = swingDir;
}

LiveMeasureSource::LiveMeasureSource(SwingPhaseGrid grid, const CharacteristicPack &pack)
    : m_grid(std::move(grid)), m_pack(pack)
{
}

// ── Reading ─────────────────────────────────────────────────────────────────

void LiveMeasureSource::note(const QString &measureId, MissingKind k) const
{
    m_reasons.insert(measureId, k);
}

std::optional<IMeasureValueSource::Value> LiveMeasureSource::value(const QString &measureId) const
{
    const Measure *m = m_pack.measure(measureId);
    if (m == nullptr) {
        note(measureId, MissingKind::UnknownMeasure);
        return std::nullopt;
    }

    // A Composed measure names facets rather than a catalogue key, and nothing in a swing.json
    // produces one — reduceOverGrid() says the same thing by returning nullopt on an empty
    // metricKey, and the point of checking first is that "there is no producer for this" and
    // "the producer ran and this capture has nothing" are different sentences for the panel.
    if (m->metricKey.isEmpty()) {
        note(measureId, MissingKind::NoMetricBinding);
        return std::nullopt;
    }

    // An empty grid is an unanalysed or unsegmented swing, and reporting each of its measures as
    // "metric not produced" would blame the producers for a swing nothing ever ran over.
    if (m_grid.isEmpty()) {
        note(measureId, MissingKind::NoAnalysis);
        return std::nullopt;
    }

    if (const std::optional<double> v = reduceOverGrid(m_grid, *m)) {
        note(measureId, MissingKind::None);

        // CONFIDENCE IS 1.0, and that is a gap rather than a claim. The swing doc records a per-
        // phase confidence and the grid does not carry it, so there is no honest per-measure number
        // to report here; inventing one from the phase ladder would put a figure in the ledger that
        // no producer stands behind. The channel is live either way — NormMeasureSource demotes an
        // inferred context through it — so a future grid that keeps phase confidence lands here
        // with nothing else to change.
        return Value{ *v, 1.0f };
    }

    // reduceOverGrid() returns one nullopt for two situations, and the panel needs them apart. The
    // discriminator is exactly the one the reduction itself uses first: no such metric, or a phase
    // the reducer wanted that the segmenter never found.
    if (m_grid.metric(m->metricKey) == nullptr) {
        if (m->metricKey.startsWith(kLmPrefix))
            note(measureId, hasLaunchMonitor() ? MissingKind::LaunchMonitorFieldAbsent
                                               : MissingKind::NoLaunchMonitor);
        else
            note(measureId, MissingKind::MetricNotProduced);
        return std::nullopt;
    }

    note(measureId, MissingKind::PhaseNotSegmented);
    return std::nullopt;
}

MissingKind LiveMeasureSource::missingKind(const QString &measureId) const
{
    return m_reasons.value(measureId, MissingKind::None);
}

QString LiveMeasureSource::missingReason(const QString &measureId) const
{
    return missingReasonText(missingKind(measureId));
}

// ── Identity ────────────────────────────────────────────────────────────────

QString LiveMeasureSource::club() const      { return m_grid.club; }
QString LiveMeasureSource::sessionId() const { return m_grid.sessionId; }
QString LiveMeasureSource::swingDir() const  { return m_grid.swingDir; }

QString LiveMeasureSource::contextId() const { return contextIdForClub(m_grid.club); }

bool LiveMeasureSource::hasLaunchMonitor() const
{
    for (const MetricPhaseGrid &mg : m_grid.metrics)
        if (mg.key.startsWith(kLmPrefix))
            return true;
    return false;
}

// ── The stack ───────────────────────────────────────────────────────────────

LiveDetection detectForSwing(const LiveMeasureSource &values, const CharacteristicPack &pack,
                             const std::shared_ptr<const INormProvider> &norms,
                             const GradePolicy &policy, const Cohort &athlete,
                             const QString &contextIdOverride)
{
    LiveDetection out;
    out.club      = values.club();
    out.contextId = contextIdOverride.isEmpty() ? values.contextId() : contextIdOverride;

    // ONE context id feeds both halves: the norm lookup (which corridor judges the number) and the
    // condition bindings (whether this kind of shot is asked the question at all). They were always
    // meant to be the same node, and nothing but this function guarantees it.
    const NormMeasureSource graded(values, norms, out.contextId, policy, &pack, athlete);

    out.result = detect(pack, graded, norms ? &norms->contexts() : nullptr, out.contextId);
    return out;
}

} // namespace pinpoint::analysis
