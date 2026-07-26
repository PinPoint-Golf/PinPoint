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

#include "characteristic_library_model.h"

#include "../../Diagnostics/norm_provider.h"   // the context tree, for binding labels
#include "metric_catalogue.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

using namespace pinpoint::analysis;

namespace {

QString resolvabilityLabel(MeasureStatus s)
{
    switch (s) {
    case MeasureStatus::Live:          return QObject::tr("Live");
    case MeasureStatus::Planned:       return QObject::tr("Planned");
    case MeasureStatus::NoProducer:    return QObject::tr("No producer");
    case MeasureStatus::NotCapturable: return QObject::tr("Not measurable from capture");
    }
    return QString();
}

// The weakest status across a condition's measures decides how the row reads: a characteristic is
// only as resolvable as its least resolvable input.
MeasureStatus weakest(MeasureStatus a, MeasureStatus b)
{
    auto rank = [](MeasureStatus s) {
        switch (s) {
        case MeasureStatus::Live:          return 0;
        case MeasureStatus::Planned:       return 1;
        case MeasureStatus::NoProducer:    return 2;
        case MeasureStatus::NotCapturable: return 3;
        }
        return 2;
    };
    return rank(a) >= rank(b) ? a : b;
}

} // namespace

CharacteristicLibraryModel::CharacteristicLibraryModel(QObject *parent)
    : QObject(parent), m_provider(makeCharacteristicPackProvider())
{
}

CharacteristicLibraryModel::~CharacteristicLibraryModel() = default;

QVariantList CharacteristicLibraryModel::groups() const
{
    QVariantList out;
    for (ConditionGroup g : { ConditionGroup::Setup, ConditionGroup::Posture, ConditionGroup::Lateral,
                              ConditionGroup::ArmsAndClub, ConditionGroup::Release,
                              ConditionGroup::Sequence }) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), conditionGroupName(g));
        m.insert(QStringLiteral("label"), conditionGroupLabel(g));
        out.append(m);
    }
    return out;
}

QVariantList CharacteristicLibraryModel::states() const
{
    QVariantList out;
    for (ConditionState s : { ConditionState::Draft, ConditionState::Candidate, ConditionState::Active,
                              ConditionState::NeedsRevalidation, ConditionState::Superseded,
                              ConditionState::Retired })
        out.append(conditionStateName(s));
    return out;
}

int CharacteristicLibraryModel::characteristicCount() const
{
    const CharacteristicPack &p = m_provider->pack();
    return int(std::count_if(p.conditions.begin(), p.conditions.end(), [](const Condition &c) {
        return c.observability != Observability::Latent;
    }));
}

int CharacteristicLibraryModel::causeCount() const
{
    const CharacteristicPack &p = m_provider->pack();
    return int(std::count_if(p.conditions.begin(), p.conditions.end(), [&p](const Condition &c) {
        return !effectsOf(p, c.id).isEmpty();
    }));
}

int CharacteristicLibraryModel::edgeCount() const { return int(m_provider->pack().edges.size()); }

int CharacteristicLibraryModel::healthCount() const { return m_provider->report().warningCount(); }

QVariantList CharacteristicLibraryModel::query(const QVariantMap &filters) const
{
    const CharacteristicPack &p = m_provider->pack();

    const QString group          = filters.value(QStringLiteral("group")).toString();
    const QString state          = filters.value(QStringLiteral("state")).toString();
    const QString reach          = filters.value(QStringLiteral("reach")).toString();
    const bool    hideProposed   = filters.value(QStringLiteral("hideProposed")).toBool();
    const bool    observableOnly = filters.value(QStringLiteral("observableOnly")).toBool();

    QVariantList out;
    for (const Condition &c : p.conditions) {
        if (!group.isEmpty() && conditionGroupName(c.group) != group) continue;
        if (!state.isEmpty() && conditionStateName(c.state) != state) continue;
        if (!reach.isEmpty() && confirmedByName(c.confirmedBy) != reach) continue;
        if (hideProposed && c.provenance.tier == ProvenanceTier::Proposed) continue;
        if (observableOnly && c.observability == Observability::Latent) continue;

        // Roll the condition's measures up into one resolvability.
        MeasureStatus status       = MeasureStatus::Live;
        int           measureCount = 0;
        bool          anyMeasure   = false;
        for (const QString &sid : c.detectedBy) {
            const Signal *s = p.signal(sid);
            if (!s) continue;
            for (const QString &mid : s->measures) {
                const Measure *m = p.measure(mid);
                if (!m) continue;
                ++measureCount;
                status     = anyMeasure ? weakest(status, m->status) : m->status;
                anyMeasure = true;
            }
        }
        if (!anyMeasure) status = MeasureStatus::NoProducer;

        // The other tail of this condition's axis, so the library can group the pair rather than
        // showing them as two near-duplicate faults.
        QString axisPartner;
        if (!c.axis.isEmpty())
            for (const QString &t : tailsOfAxis(p, c.axis))
                if (t != c.id) axisPartner = t;

        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("group"), conditionGroupName(c.group));
        r.insert(QStringLiteral("groupLabel"), conditionGroupLabel(c.group));
        r.insert(QStringLiteral("axis"), c.axis);
        r.insert(QStringLiteral("axisPartner"), axisPartner);
        r.insert(QStringLiteral("reach"), confirmedByName(c.confirmedBy));
        r.insert(QStringLiteral("reachLabel"), reachLabel(c.confirmedBy));
        r.insert(QStringLiteral("reachHint"), reachHint(c.confirmedBy));
        r.insert(QStringLiteral("tier"), provenanceTierName(c.provenance.tier));
        r.insert(QStringLiteral("proposed"), c.provenance.tier == ProvenanceTier::Proposed);
        r.insert(QStringLiteral("state"), conditionStateName(c.state));
        r.insert(QStringLiteral("resolvability"), measureStatusName(status));
        r.insert(QStringLiteral("resolvabilityLabel"), resolvabilityLabel(status));
        r.insert(QStringLiteral("measureCount"), measureCount);
        r.insert(QStringLiteral("causeCount"), int(causesOf(p, c.id).size()));
        r.insert(QStringLiteral("effectCount"), int(effectsOf(p, c.id).size()));
        r.insert(QStringLiteral("isCharacteristic"), c.observability != Observability::Latent);
        out.append(r);
    }
    return out;
}

QVariantMap CharacteristicLibraryModel::detail(const QString &conditionId) const
{
    const CharacteristicPack &p = m_provider->pack();
    const Condition          *c = p.condition(conditionId);
    if (!c) return {};

    // Start from the row shape so the detail page never has to re-derive what the row already knows.
    QVariantMap out;
    for (const QVariant &v : query()) {
        const QVariantMap row = v.toMap();
        if (row.value(QStringLiteral("id")).toString() == conditionId) { out = row; break; }
    }
    if (out.isEmpty()) return {};

    out.insert(QStringLiteral("consequence"), c->consequence.text());
    out.insert(QStringLiteral("injuryNote"), c->injuryNote.text());
    out.insert(QStringLiteral("screenRef"), c->screenRef);
    out.insert(QStringLiteral("citation"), c->provenance.citation);
    out.insert(QStringLiteral("author"), c->provenance.author);
    out.insert(QStringLiteral("observability"), observabilityName(c->observability));

    QVariantList measures;
    for (const QString &sid : c->detectedBy) {
        const Signal *s = p.signal(sid);
        if (!s) continue;
        for (const QString &mid : s->measures) {
            const Measure *m = p.measure(mid);
            if (!m) continue;
            QVariantMap mm;
            mm.insert(QStringLiteral("id"), m->id);
            mm.insert(QStringLiteral("label"), m->label.isEmpty()
                                                   ? canonicalMeasureLabel(m->series, m->reducer)
                                                   : m->label);
            mm.insert(QStringLiteral("kind"), measureKindName(m->kind));
            mm.insert(QStringLiteral("metricKey"), m->metricKey);
            mm.insert(QStringLiteral("status"), measureStatusName(m->status));
            mm.insert(QStringLiteral("statusLabel"), resolvabilityLabel(m->status));
            mm.insert(QStringLiteral("gapReason"), m->gapReason);
            mm.insert(QStringLiteral("viewNeeded"), viewNeededName(m->viewNeeded));
            mm.insert(QStringLiteral("test"), signalTestName(s->test));
            mm.insert(QStringLiteral("direction"),
                      s->direction.has_value() ? directionName(*s->direction) : QString());

            // WHICH SIDE FIRES, on the read-only page. `direction` was marshalled here from the
            // start and rendered nowhere, so the one thing a reader has to check before deciding a
            // characteristic is right — does it flag too much or too little of this? — could only
            // be discovered by opening the editor. Same phrasing rule as the control that sets it
            // (directionPhrase), so the two cannot drift.
            if (s->direction.has_value()) {
                const DirectionPhrase dp = directionPhrase(*s->direction, m->highMeans);
                mm.insert(QStringLiteral("directionLabel"), dp.label);
                mm.insert(QStringLiteral("directionSentence"), dp.sentence);
            }
            mm.insert(QStringLiteral("signalId"), s->id);
            mm.insert(QStringLiteral("highMeans"), m->highMeans);
            mm.insert(QStringLiteral("usedBy"), usageOfMeasure(m->id));
            measures.append(mm);
        }
    }
    out.insert(QStringLiteral("measures"), measures);

    // A condition's causes AND its effects — the same condition is routinely both, and a detail
    // page showing only one direction hides half the graph.
    auto edgeList = [&](const QStringList &ids, bool asCause) {
        QVariantList l;
        for (const QString &id : ids) {
            const Condition *other = p.condition(id);
            if (!other) continue;
            QVariantMap e;
            e.insert(QStringLiteral("id"), id);
            e.insert(QStringLiteral("label"), other->label);
            e.insert(QStringLiteral("reach"), confirmedByName(other->confirmedBy));
            e.insert(QStringLiteral("reachLabel"), reachLabel(other->confirmedBy));
            e.insert(QStringLiteral("offeredOnly"), other->confirmedBy == ConfirmedBy::Asserted);
            for (const Edge &edge : p.edges) {
                const bool match = asCause ? (edge.from == id && edge.to == conditionId)
                                           : (edge.from == conditionId && edge.to == id);
                if (edge.type == EdgeType::Causes && match) {
                    e.insert(QStringLiteral("strength"), strengthName(edge.strength));
                    e.insert(QStringLiteral("strengthLabel"), strengthLabel(edge.strength));
                    break;
                }
            }
            l.append(e);
        }
        return l;
    };
    out.insert(QStringLiteral("causes"), edgeList(causesOf(p, conditionId), true));
    out.insert(QStringLiteral("effects"), edgeList(effectsOf(p, conditionId), false));

    // Bindings are EXCEPTIONS, so the shipped pack carries none and this list is empty for every
    // characteristic nobody has narrowed. `corridorRef` was marshalled here until stage 7 — the
    // corridor is found by the (measureId, contextId) norm join, so the key named nothing.
    const ContextTree &tree = sharedNormProvider()->contexts();

    QVariantList bindings;
    QStringList  offLabels, immaterialLabels;
    for (const ContextBinding &b : c->bindings) {
        const ContextNode *n     = tree.node(b.context);
        const QString      label = n ? n->label : b.context;

        QVariantMap bm;
        bm.insert(QStringLiteral("context"), b.context);
        bm.insert(QStringLiteral("contextLabel"), label);
        bm.insert(QStringLiteral("applicable"), b.applicable);
        bm.insert(QStringLiteral("material"), b.material);
        bm.insert(QStringLiteral("consequence"), b.consequence.text());
        bindings.append(bm);

        // Naming the subtree matters: a row at `partial` covers pitch and chip, and a summary that
        // said only "Partial swing" would read as one narrow exception rather than three.
        const bool hasKids = !tree.children(b.context).isEmpty();
        const QString phrase = hasKids ? tr("%1 and anything beneath it").arg(label) : label;
        if (!b.applicable)     offLabels << phrase;
        else if (!b.material)  immaterialLabels << phrase;
    }
    out.insert(QStringLiteral("bindings"), bindings);

    // One sentence for the detail page, composed here: whether a narrowing is worth a line at all
    // is a rule, and a rule assembled in a delegate is a rule nothing can test.
    QStringList parts;
    if (!offLabels.isEmpty())
        parts << tr("Does not apply to %1.").arg(offLabels.join(QStringLiteral(", ")));
    if (!immaterialLabels.isEmpty())
        parts << tr("Reported but not counted when ranking for %1.")
                     .arg(immaterialLabels.join(QStringLiteral(", ")));
    out.insert(QStringLiteral("appliesSummary"), parts.join(QLatin1Char(' ')));

    return out;
}

QStringList CharacteristicLibraryModel::usersOfMeasureIds(const QString &measureId) const
{
    const CharacteristicPack &p = m_provider->pack();

    QStringList signalsUsing;
    for (const Signal &s : p.signalDefs)
        if (s.measures.contains(measureId)) signalsUsing << s.id;

    QStringList out;
    for (const Condition &c : p.conditions)
        for (const QString &sid : c.detectedBy)
            if (signalsUsing.contains(sid)) { out << c.id; break; }
    return out;
}

int CharacteristicLibraryModel::usageOfMeasure(const QString &measureId) const
{
    const CharacteristicPack &p = m_provider->pack();

    QStringList signalsUsing;
    for (const Signal &s : p.signalDefs)
        if (s.measures.contains(measureId)) signalsUsing << s.id;

    int n = 0;
    for (const Condition &c : p.conditions)
        for (const QString &sid : c.detectedBy)
            if (signalsUsing.contains(sid)) { ++n; break; }
    return n;
}

QVariantList CharacteristicLibraryModel::roadmap() const
{
    const CharacteristicPack &p   = m_provider->pack();
    const MetricCatalogue     cat = makeMetricCatalogue();

    // Ranked by SERIES, not by reduced measure. One producer unblocks every reducer over its
    // series: pelvis lateral sway carries sway, slide and hanging back at three different phases,
    // so it is ONE piece of pipeline work worth three characteristics. Listing the three samples
    // separately would spread that across three rows of "unblocks 1" and bury the very item that
    // should rank top — which is the whole reason series and reducer are modelled apart.
    struct Group {
        QString       label;
        QString       metricKey;
        MeasureStatus status = MeasureStatus::NoProducer;
        ViewNeeded    view   = ViewNeeded::Any;
        int           samples = 0;          // how many reducers sit on this series
        QSet<QString> blocked;              // DISTINCT characteristics, never double-counted
    };
    QHash<QString, Group> groups;

    for (const Measure &m : p.measures) {
        if (m.status == MeasureStatus::Live) continue;
        // Capture gaps are NOT roadmap items. One row implying a producer that will never be built
        // corrupts the artefact's meaning for every other row.
        if (m.status == MeasureStatus::NotCapturable) continue;

        const QString key = m.metricKey.isEmpty() ? canonicalSeriesId(m.series) : m.metricKey;
        Group        &g   = groups[key];

        if (g.samples == 0) {
            g.metricKey = m.metricKey;
            g.status    = m.status;
            g.view      = m.viewNeeded;
            // Prefer the catalogue's own name and view requirement: a Provided measure has no
            // facets to generate a series label from, and the requirement is the catalogue's to
            // state, not ours to guess.
            if (const MetricDescriptor *d = cat.descriptor(key)) {
                g.label = d->label;
                if (d->requirement.faceOnCamera && g.view == ViewNeeded::Any) g.view = ViewNeeded::FaceOn;
            }
            if (g.label.isEmpty())
                g.label = (m.kind == MeasureKind::Composed) ? canonicalSeriesLabel(m.series) : m.id;
        }
        ++g.samples;
        for (const QString &cid : usersOfMeasureIds(m.id)) g.blocked.insert(cid);
    }

    QVariantList out;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const Group &g = it.value();
        QVariantMap  r;
        r.insert(QStringLiteral("id"), it.key());
        r.insert(QStringLiteral("label"), g.label);
        r.insert(QStringLiteral("metricKey"), g.metricKey);
        r.insert(QStringLiteral("status"), measureStatusName(g.status));
        r.insert(QStringLiteral("statusLabel"), resolvabilityLabel(g.status));
        r.insert(QStringLiteral("viewNeeded"), viewNeededName(g.view));
        r.insert(QStringLiteral("blocks"), g.blocked.size());
        r.insert(QStringLiteral("samples"), g.samples);
        out.append(r);
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const int ba = a.toMap().value(QStringLiteral("blocks")).toInt();
        const int bb = b.toMap().value(QStringLiteral("blocks")).toInt();
        if (ba != bb) return ba > bb;
        return a.toMap().value(QStringLiteral("label")).toString()
             < b.toMap().value(QStringLiteral("label")).toString();
    });
    return out;
}

QVariantList CharacteristicLibraryModel::captureGaps() const
{
    const CharacteristicPack &p = m_provider->pack();

    QVariantList out;
    for (const Measure &m : p.measures) {
        if (m.status != MeasureStatus::NotCapturable) continue;
        QVariantMap r;
        r.insert(QStringLiteral("id"), m.id);
        r.insert(QStringLiteral("label"), m.label.isEmpty()
                                              ? canonicalMeasureLabel(m.series, m.reducer)
                                              : m.label);
        r.insert(QStringLiteral("reason"), m.gapReason);
        r.insert(QStringLiteral("blocks"), usageOfMeasure(m.id));
        out.append(r);
    }
    return out;
}

QVariantList CharacteristicLibraryModel::causeCoverage() const
{
    const CharacteristicPack &p = m_provider->pack();

    QVariantList out;
    for (const Condition &c : p.conditions) {
        const int cov = coverageOf(p, c.id);
        if (cov == 0) continue;

        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("coverage"), cov);
        r.insert(QStringLiteral("reach"), confirmedByName(c.confirmedBy));
        r.insert(QStringLiteral("reachLabel"), reachLabel(c.confirmedBy));
        r.insert(QStringLiteral("reachHint"), reachHint(c.confirmedBy));
        r.insert(QStringLiteral("screenRef"), c.screenRef);
        // Screened and Behavioural causes can never be measured by this product. That is a
        // permanent property, not a pending one, and the UI must say so rather than implying a
        // producer is on the way.
        r.insert(QStringLiteral("outsideCaptureReach"), isOutsideCaptureReach(c.confirmedBy));
        r.insert(QStringLiteral("explains"), QVariant(effectsOf(p, c.id)));
        out.append(r);
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("coverage")).toInt()
             > b.toMap().value(QStringLiteral("coverage")).toInt();
    });
    return out;
}

QVariantList CharacteristicLibraryModel::health() const
{
    QVariantList out;
    for (const ValidationIssue &i : m_provider->report().issues) {
        if (i.severity != IssueSeverity::Warning) continue;
        QVariantMap r;
        r.insert(QStringLiteral("code"), i.code);
        r.insert(QStringLiteral("subject"), i.subject);
        r.insert(QStringLiteral("message"), i.message);
        out.append(r);
    }
    return out;
}

QVariantList CharacteristicLibraryModel::usersOfMeasure(const QString &measureId) const
{
    const CharacteristicPack &p = m_provider->pack();

    QStringList signalsUsing;
    for (const Signal &s : p.signalDefs)
        if (s.measures.contains(measureId)) signalsUsing << s.id;

    QVariantList out;
    for (const Condition &c : p.conditions) {
        bool uses = false;
        for (const QString &sid : c.detectedBy)
            if (signalsUsing.contains(sid)) uses = true;
        if (!uses) continue;

        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("groupLabel"), conditionGroupLabel(c.group));
        out.append(r);
    }
    return out;
}

QString CharacteristicLibraryModel::roadmapMarkdown() const
{
    const CharacteristicPack &p = m_provider->pack();

    QString md;
    md += QStringLiteral("# Swing diagnostics — measure roadmap\n\n");
    md += QStringLiteral("Generated from the diagnostics pack. Every row is work that could be "
                         "picked up: a measure some characteristic needs and nothing yet produces. "
                         "Ranked by how many characteristics it unblocks.\n\n");

    const QVariantList rows = roadmap();
    if (rows.isEmpty()) {
        md += QStringLiteral("_Nothing outstanding._\n\n");
    } else {
        md += QStringLiteral("| Measure | Unblocks | Status | View | Metric key |\n");
        md += QStringLiteral("|---|---:|---|---|---|\n");
        for (const QVariant &v : rows) {
            const QVariantMap r = v.toMap();
            md += QStringLiteral("| %1 | %2 | %3 | %4 | `%5` |\n")
                      .arg(r.value(QStringLiteral("label")).toString())
                      .arg(r.value(QStringLiteral("blocks")).toInt())
                      .arg(r.value(QStringLiteral("statusLabel")).toString(),
                           r.value(QStringLiteral("viewNeeded")).toString(),
                           r.value(QStringLiteral("metricKey")).toString());
        }
        md += QLatin1Char('\n');
    }

    // Capture gaps are deliberately a SEPARATE section, never roadmap rows. A reader has to be able
    // to take the table above at face value as a work queue; one row nobody could ever pick up
    // would corrupt that reading for every other row.
    const QVariantList gaps = captureGaps();
    if (!gaps.isEmpty()) {
        md += QStringLiteral("## Not resolvable from current capture\n\n");
        md += QStringLiteral("These are not roadmap items. No sensor this product has can resolve "
                             "them, so they need a different modality rather than a producer.\n\n");
        for (const QVariant &v : gaps) {
            const QVariantMap r = v.toMap();
            md += QStringLiteral("- **%1** — blocks %2. %3\n")
                      .arg(r.value(QStringLiteral("label")).toString())
                      .arg(r.value(QStringLiteral("blocks")).toInt())
                      .arg(r.value(QStringLiteral("reason")).toString());
        }
        md += QLatin1Char('\n');
    }

    // The screen list ships alongside, because it is the half of the picture that needs no
    // engineering at all: a handful of physical tests explain most of the library.
    md += QStringLiteral("## Causes, by how much they explain\n\n");
    md += QStringLiteral("| Cause | Explains | Reach |\n|---|---:|---|\n");
    for (const QVariant &v : causeCoverage()) {
        const QVariantMap r = v.toMap();
        md += QStringLiteral("| %1 | %2 | %3 |\n")
                  .arg(r.value(QStringLiteral("label")).toString())
                  .arg(r.value(QStringLiteral("coverage")).toInt())
                  .arg(r.value(QStringLiteral("reachLabel")).toString());
    }

    md += QStringLiteral("\n---\n\nPack `%1`, %2 characteristics, %3 causal links.\n")
              .arg(p.id).arg(characteristicCount()).arg(edgeCount());
    return md;
}

QVariantMap CharacteristicLibraryModel::exportRoadmap() const
{
    QVariantMap r;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty()) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("No Documents folder to write to."));
        return r;
    }

    const QString path = dir + QStringLiteral("/pinpoint-diagnostics-roadmap.md");
    QFile         f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("Could not write to %1.").arg(path));
        return r;
    }
    f.write(roadmapMarkdown().toUtf8());
    f.close();

    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("path"), path);
    r.insert(QStringLiteral("message"), tr("Exported to %1").arg(path));
    return r;
}
