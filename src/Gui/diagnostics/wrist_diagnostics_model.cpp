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

#include "wrist_diagnostics_model.h"

#include "../Analysis/wrist_analysis_adapter.h"
#include "../Analysis/wrist_assessment_engine.h"
#include "../Analysis/wrist_assessment_fixtures.h"
#include "../Analysis/wrist_dof_metadata.h"

#include <QVariantMap>

#include <algorithm>

using namespace pinpoint::analysis;

namespace {

// DOF display order for the strips + grid (lead-wrist lag → face → forearm roll → trail tray →
// grid-only elbow). Distinct from the PpJointDof enum order.
const PpJointDof kDisplayOrder[] = {
    PpJointDof::LeadWristRadUln,
    PpJointDof::LeadWristFlexExt,
    PpJointDof::LeadForearmRot,
    PpJointDof::TrailWristFlexExt,
    PpJointDof::LeadElbowFlex,
};

QVariantMap pointMap(const PpRagCell &c)
{
    QVariantMap m;
    m[QStringLiteral("value")]     = c.deltaDeg;
    m[QStringLiteral("rag")]       = QString::fromLatin1(ragName(c.rag));
    m[QStringLiteral("available")] = (c.status == PpCellStatus::Ok);
    m[QStringLiteral("banded")]    = c.banded;
    m[QStringLiteral("bandLo")]    = c.bandLo;
    m[QStringLiteral("bandHi")]    = c.bandHi;
    return m;
}

} // namespace

WristDiagnosticsModel::WristDiagnosticsModel(QObject *parent)
    : QObject(parent)
    , m_provider(makeReferenceBandProvider())
{
    // No data until a swing is reviewed (analysisDetail bound from shotReplay).
}

void WristDiagnosticsModel::assess(const IWristAngleSource &source)
{
    WristAssessmentConfig cfg;
    cfg.band.archetype = m_archetype;           // the selected band model (neutral / bowed / cupped)
    m_timeline = source.timeline();
    m_result   = WristAssessmentEngine::assess(source, *m_provider, cfg);
    recomputeSelected();
    emit resultChanged();
}

void WristDiagnosticsModel::setAnalysisDetail(const QVariantMap &detail)
{
    m_analysisDetail = detail;
    const InMemoryWristAngleSource source = parseAnalysisDetail(detail);
    assess(source);
}

void WristDiagnosticsModel::reassess()
{
    if (m_analysisDetail.isEmpty())
        return;
    const InMemoryWristAngleSource source = parseAnalysisDetail(m_analysisDetail);
    assess(source);
}

void WristDiagnosticsModel::setArchetype(int a)
{
    const int clamped = qBound(-1, a, 2);       // -1 = Auto
    if (clamped == m_archetype)
        return;
    m_archetype = clamped;
    reassess();                                 // re-band + re-rule the current swing
}

QString WristDiagnosticsModel::effectiveArchetypeName() const
{
    switch (m_result.archetype) {
    case 1:  return QStringLiteral("Bowed");
    case 2:  return QStringLiteral("Cupped");
    default: return QStringLiteral("Neutral");
    }
}

QVariantMap WristDiagnosticsModel::resemblance() const
{
    return m_analysisDetail.value(QStringLiteral("resemblance")).toMap();
}

void WristDiagnosticsModel::setCompareTo(const QString &mode)
{
    if (mode == m_compareTo)
        return;
    m_compareTo = mode;
    recomputeGhost();
}

void WristDiagnosticsModel::setPreviousAnalysisDetail(const QVariantMap &detail)
{
    m_previousAnalysisDetail = detail;
    recomputeGhost();
}

void WristDiagnosticsModel::setReferenceAnalysisDetail(const QVariantMap &detail)
{
    m_referenceAnalysisDetail = detail;
    recomputeGhost();
}

// The compare-to ghost = the comparison swing's Δ curve (band-independent), assessed once and cached.
void WristDiagnosticsModel::recomputeGhost()
{
    m_hasGhost = false;
    QVariantMap src;
    if (m_compareTo == QLatin1String("previous"))       src = m_previousAnalysisDetail;
    else if (m_compareTo == QLatin1String("reference")) src = m_referenceAnalysisDetail;
    if (!src.isEmpty()) {
        const InMemoryWristAngleSource ghost = parseAnalysisDetail(src);
        m_prevResult = WristAssessmentEngine::assess(ghost, *m_provider);
        m_hasGhost   = true;
    }
    emit resultChanged();
}

void WristDiagnosticsModel::loadDemo(const QString &name)
{
    const FixtureWristAngleSource source =
          name == QLatin1String("clean")      ? makeCleanSwing()
        : name == QLatin1String("cast")       ? makeCastSwing()
        : name == QLatin1String("flip")       ? makeFlipSwing()
        : name == QLatin1String("openface")   ? makeOpenFaceTopSwing()
        : name == QLatin1String("holdingoff") ? makeHoldingOffSwing()
        :                                       makeMockupDemoSwing();
    assess(source);
}

void WristDiagnosticsModel::setPlayheadUs(qint64 us)
{
    if (us == m_playheadUs)
        return;
    m_playheadUs = us;
    recomputeSelected();
}

// Selected checkpoint = the present checkpoint bracketing the playhead (greatest t_us ≤ playhead),
// falling back to the first present checkpoint. Mirrors TimelineLabels::activeStation.
void WristDiagnosticsModel::recomputeSelected()
{
    int best = -1;
    for (int c = 0; c < kNumPos; ++c) {
        const auto &e = m_timeline.positions[c];
        if (e.present && e.t_us <= m_playheadUs)
            best = c;
    }
    if (best < 0) {
        for (int c = 0; c < kNumPos; ++c)
            if (m_timeline.positions[c].present) { best = c; break; }
        if (best < 0)
            best = 0;
    }
    if (best != m_selectedPosition) {
        m_selectedPosition = best;
        emit selectedPositionChanged();
    }
}

bool WristDiagnosticsModel::hasData() const
{
    for (const PpDofRow &row : m_result.rows)
        if (row.present)
            return true;
    return false;
}

QString WristDiagnosticsModel::scoreBand() const
{
    const int s = m_result.score.total;
    if (s >= 75) return QStringLiteral("good");
    if (s >= 50) return QStringLiteral("watch");
    if (s >= 25) return QStringLiteral("weak");
    return QStringLiteral("fault");
}

QVariantList WristDiagnosticsModel::positions() const
{
    QVariantList out;
    const WristCheckpoint *cps = wristCheckpoints();
    for (int c = 0; c < kNumPos; ++c) {
        QVariantMap m;
        m[QStringLiteral("phase")]     = cps[c].phase;
        m[QStringLiteral("name")]      = m_labels.phaseFullName(cps[c].phase);
        m[QStringLiteral("tag")]       = m_labels.phaseShortTag(cps[c].phase);
        m[QStringLiteral("note")]      = QString::fromLatin1(cps[c].note);
        m[QStringLiteral("available")] = m_timeline.positions[c].present;
        out.append(m);
    }
    return out;
}

QVariantList WristDiagnosticsModel::strips() const
{
    QVariantList out;
    for (PpJointDof dof : kDisplayOrder) {
        const DofMetadata meta = dofMetadata(dof);
        const PpDofRow &row = m_result.row(dof);
        if (!row.present || !meta.hasStrip)
            continue;

        QVariantList points;
        for (int p = 0; p < kNumPos; ++p) {
            QVariantMap pm = pointMap(row.cells[p]);
            if (m_hasGhost) {
                const PpRagCell &g = m_prevResult.row(dof).cells[p];
                if (g.status == PpCellStatus::Ok)
                    pm[QStringLiteral("ghost")] = g.deltaDeg;   // previous swing's Δ at this checkpoint
            }
            points.append(pm);
        }

        QVariantMap s;
        s[QStringLiteral("name")]       = meta.name;
        s[QStringLiteral("sub")]        = meta.sub;
        s[QStringLiteral("source")]     = meta.source;
        s[QStringLiteral("confidence")] = row.confidence;
        s[QStringLiteral("poleTop")]    = meta.poleTop;
        s[QStringLiteral("poleBottom")] = meta.poleBottom;
        s[QStringLiteral("points")]     = points;
        out.append(s);
    }
    return out;
}

QVariantList WristDiagnosticsModel::gridRows() const
{
    QVariantList out;
    for (PpJointDof dof : kDisplayOrder) {
        const DofMetadata meta = dofMetadata(dof);
        const PpDofRow &row = m_result.row(dof);
        if (!row.present)
            continue;

        QVariantList cells;
        for (int p = 0; p < kNumPos; ++p) {
            QVariantMap c = pointMap(row.cells[p]);
            c[QStringLiteral("ref")] = (row.cells[p].rag == PpRag::Ref);
            cells.append(c);
        }

        QVariantMap r;
        r[QStringLiteral("name")]  = meta.name.toLower() + QLatin1Char(' ') + meta.axisName;
        r[QStringLiteral("cells")] = cells;
        out.append(r);
    }
    return out;
}

QVariantList WristDiagnosticsModel::scoreBreakdown() const
{
    QVariantList out;
    for (const PpScoreContribution &c : m_result.score.contributions) {
        QVariantMap m;
        m[QStringLiteral("label")]   = c.name;
        m[QStringLiteral("penalty")] = qRound(c.penalty);
        out.append(m);
    }
    return out;
}

QString WristDiagnosticsModel::scoreBreakdownText() const
{
    double penalty = 0.0;
    for (const PpScoreContribution &c : m_result.score.contributions)
        penalty += c.penalty;
    return QStringLiteral("%1 − %2 = %3")
        .arg(m_result.score.base)
        .arg(qRound(penalty))
        .arg(m_result.score.total);
}

QVariantMap WristDiagnosticsModel::findingMap(const PpWristFinding &f) const
{
    QVariantMap m;
    m[QStringLiteral("id")]            = f.id;
    m[QStringLiteral("name")]          = f.name;
    m[QStringLiteral("category")]      = f.category;
    m[QStringLiteral("severity")]      = QString::fromLatin1(severityName(f.severity));
    m[QStringLiteral("confidence")]    = static_cast<double>(f.confidence);
    m[QStringLiteral("lowConfidence")] = f.lowConfidence;
    m[QStringLiteral("magnitude")]     = qRound(f.magnitudeDeg);
    m[QStringLiteral("explanation")]   = f.explanation;
    m[QStringLiteral("coaching")]      = f.coaching;
    m[QStringLiteral("protect")]       = f.protect;
    m[QStringLiteral("linkedTo")]      = f.linkedTo;
    m[QStringLiteral("ballFlight")]    = f.ballFlight;
    m[QStringLiteral("corroboratedBy")] = f.corroboratedBy;

    const WristCheckpoint *cps = wristCheckpoints();
    QVariantList tags;
    for (PpSwingPosition p : f.positions)
        tags.append(m_labels.phaseShortTag(cps[static_cast<int>(p)].phase));
    m[QStringLiteral("positions")] = tags;

    // Seek target = the primary (first) contributing checkpoint's timeline timestamp,
    // and `phase` = that checkpoint's Phase enum — the vocabulary the corridor lookup
    // and TimelineLabels already speak, so a fix can be resolved to a phase without
    // re-deriving it from seekUs. -1 when the finding has no position.
    qint64 seek  = 0;
    int    phase = -1;
    if (!f.positions.empty()) {
        const int ci = static_cast<int>(f.positions.front());
        phase = static_cast<int>(cps[ci].phase);
        if (m_timeline.positions[ci].present)
            seek = m_timeline.positions[ci].t_us;
    }
    m[QStringLiteral("seekUs")] = seek;
    m[QStringLiteral("phase")]  = phase;

    // Points this finding cost the composite score (design §7.6) — the matching
    // contribution's penalty, or 0 when the finding did not score (strengths, and any
    // finding the scorer weighted out). Lets the Verdict fix line say what it is worth
    // rather than only naming itself.
    int pointsLost = 0;
    for (const PpScoreContribution &c : m_result.score.contributions)
        if (c.id == f.id) { pointsLost = qRound(c.penalty); break; }
    m[QStringLiteral("pointsLost")] = pointsLost;
    return m;
}

QVariantList WristDiagnosticsModel::findings() const
{
    std::vector<const PpWristFinding *> fs;
    for (const PpWristFinding &f : m_result.findings)
        if (f.severity != PpFindingSeverity::Good)
            fs.push_back(&f);
    std::sort(fs.begin(), fs.end(), [](const PpWristFinding *a, const PpWristFinding *b) {
        if (a->severity != b->severity) return a->severity > b->severity;   // Fault before Watch
        return a->confidence > b->confidence;                               // then most-trusted first
    });
    QVariantList out;
    for (const PpWristFinding *f : fs)
        out.append(findingMap(*f));
    return out;
}

QVariantList WristDiagnosticsModel::strengths() const
{
    QVariantList out;
    for (const PpWristFinding &f : m_result.findings)
        if (f.severity == PpFindingSeverity::Good)
            out.append(findingMap(f));
    return out;
}
