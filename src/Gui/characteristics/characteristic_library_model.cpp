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

#include "../../Diagnostics/dag_layout.h"      // the causal DAG, laid out in C++
#include "../../Diagnostics/drill_pack.h"      // what `Condition::drills` points at
#include "../../Diagnostics/screen_pack.h"     // what `Condition::screenRef` points at
#include "../../Diagnostics/measure_sample.h"  // the phase grid the corpus check reads
#include "../../Diagnostics/norm_provider.h"   // the context tree, for binding labels
#include "../../Diagnostics/reference_pack.h"  // what `Provenance::citation` points at
#include "metric_catalogue.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

using namespace pinpoint::analysis;

namespace {

// The four status words live with the enum (characteristic.cpp's table), not here. They are read by
// the DAG's measure lane too, and two copies of a user-facing string are two copies that can drift.
QString resolvabilityLabel(MeasureStatus s) { return measureStatusLabel(s); }

// The weakest status across a condition's measures decides how the row reads: a characteristic is
// only as resolvable as its least resolvable input.
MeasureStatus weakest(MeasureStatus a, MeasureStatus b)
{
    auto rank = [](MeasureStatus s) {
        switch (s) {
        case MeasureStatus::Live:           return 0;
        case MeasureStatus::Planned:        return 1;
        case MeasureStatus::NoProducer:     return 2;
        // Weaker than NoProducer: that one needs code we can write, this one needs code AND hardware
        // the golfer may not own. Still stronger than NotCapturable, which needs something that does
        // not exist.
        case MeasureStatus::ExternalDevice: return 3;
        case MeasureStatus::NotCapturable:  return 4;
        }
        return 2;
    };
    return rank(a) >= rank(b) ? a : b;
}

} // namespace

CharacteristicLibraryModel::CharacteristicLibraryModel(QObject *parent)
    : QObject(parent)
    , m_provider(makeCharacteristicPackProvider())
    , m_norms(sharedNormProvider())
    , m_cat(makeMetricCatalogue())
    , m_policyName(QStringLiteral("standard"))
    , m_corpusWatcher(new QFutureWatcher<QVariantList>(this))
{
    connect(m_corpusWatcher, &QFutureWatcher<QVariantList>::finished,
            this, &CharacteristicLibraryModel::onCorpusFinished);
}

CharacteristicLibraryModel::~CharacteristicLibraryModel()
{
    // The worker reads only copies (a library path and value-type measures), but it writes into a
    // future this object owns — so it has to be joined before that future dies with us.
    if (m_corpusWatcher->isRunning())
        m_corpusWatcher->waitForFinished();
}

void CharacteristicLibraryModel::refresh()
{
    m_provider = makeCharacteristicPackProvider();
    resetSharedNormProvider();
    m_norms    = sharedNormProvider();
    emit healthChanged();
}

QVariantList CharacteristicLibraryModel::groups() const
{
    QVariantList out;
    for (ConditionGroup g : allConditionGroups()) {
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

// Counts what the list SHOWS, by asking the list. Two counts derived independently is how a badge
// ends up disagreeing with the page it links to.
int CharacteristicLibraryModel::healthCount() const { return int(health().size()); }

QVariantList CharacteristicLibraryModel::query(const QVariantMap &filters) const
{
    const CharacteristicPack &p = m_provider->pack();

    const QString group          = filters.value(QStringLiteral("group")).toString();
    const QString state          = filters.value(QStringLiteral("state")).toString();
    const QString reach          = filters.value(QStringLiteral("reach")).toString();
    const bool    hideProposed   = filters.value(QStringLiteral("hideProposed")).toBool();
    const bool    observableOnly = filters.value(QStringLiteral("observableOnly")).toBool();

    // Free-text search over what the directory row SHOWS, plus the ids underneath it — a hit
    // the reader cannot see in the result is a hit they cannot trust. Same shape and same
    // reasoning as NormModel::measures().
    const QString search = filters.value(QStringLiteral("search")).toString().trimmed();

    QVariantList out;
    for (const Condition &c : p.conditions) {
        if (!group.isEmpty() && conditionGroupName(c.group) != group) continue;
        if (!state.isEmpty() && conditionStateName(c.state) != state) continue;
        if (!reach.isEmpty() && confirmedByName(c.confirmedBy) != reach) continue;
        if (hideProposed && c.provenance.tier == ProvenanceTier::Proposed) continue;
        if (observableOnly && c.observability == Observability::Latent) continue;

        if (!search.isEmpty()) {
            // Aliases are searched even though the row does not show every one of them — this is the
            // deliberate exception to "a hit the reader cannot see is a hit they cannot trust". A
            // golfer types the word they were taught ("flip", "OTT", "standing up"); if that word
            // does not find the page, the library is unreachable for them, and the MATCHED alias is
            // marshalled below so the row can show why it was returned.
            bool hit = c.label.contains(search, Qt::CaseInsensitive)
                       || c.id.contains(search, Qt::CaseInsensitive)
                       || c.axis.contains(search, Qt::CaseInsensitive)
                       || conditionGroupLabel(c.group).contains(search, Qt::CaseInsensitive);
            if (!hit)
                for (const QString &a : c.aliases)
                    if (a.contains(search, Qt::CaseInsensitive)) { hit = true; break; }
            if (!hit) continue;
        }

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

        // Which alias answered, when one did. Without it a search for "flip" returns "Scooping" and
        // the reader cannot tell whether the library understood them or simply guessed.
        QString matchedAlias;
        if (!search.isEmpty() && !c.label.contains(search, Qt::CaseInsensitive))
            for (const QString &a : c.aliases)
                if (a.contains(search, Qt::CaseInsensitive)) { matchedAlias = a; break; }

        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("aliases"), c.aliases);
        r.insert(QStringLiteral("matchedAlias"), matchedAlias);
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
    // Labelled, not raw: the provenance block is the one place a reader meets this identifier, and
    // a bare "30479527" tells them nothing about what kind of thing it is.
    out.insert(QStringLiteral("citation"), citationLabel(c->provenance.citation));

    // The row this citation points at, resolved HERE rather than matched in QML. The view shows a
    // label, so it no longer holds the join key at all — and the id is the thing the References
    // view can actually scroll to. Empty when the citation resolves to nothing, which is what the
    // detail page keys its link affordance off: a link that cannot land anywhere must not look
    // like one.
    const Reference *cited = sharedReferenceSet().byCitation(c->provenance.citation);
    out.insert(QStringLiteral("citationReferenceId"), cited ? cited->id : QString());
    out.insert(QStringLiteral("author"), c->provenance.author);
    out.insert(QStringLiteral("observability"), observabilityName(c->observability));

    // The non-causal relations. The DAG draws the causal graph and only the causal graph — rank is
    // signed causal distance, and a symmetric edge has no direction to rank by — so without these
    // rows a corroborates or excludes edge would be authored, validated, consumed by the explanation
    // pass, and visible nowhere. That is the trap this guide names twice.
    auto relationRows = [&](EdgeType type) {
        QVariantList rows;
        for (const Edge &e : p.edges) {
            if (e.type != type) continue;
            const QString otherId = (e.from == conditionId) ? e.to
                                  : (e.to == conditionId)   ? e.from
                                                            : QString();
            if (otherId.isEmpty()) continue;
            const Condition *other = p.condition(otherId);
            QVariantMap r;
            r.insert(QStringLiteral("id"), otherId);
            r.insert(QStringLiteral("label"), other ? other->label : otherId);
            r.insert(QStringLiteral("groupLabel"), other ? conditionGroupLabel(other->group) : QString());
            rows.append(r);
        }
        return rows;
    };
    out.insert(QStringLiteral("corroboratedBy"), relationRows(EdgeType::Corroborates));
    out.insert(QStringLiteral("excludes"), relationRows(EdgeType::Excludes));

    // The screen RESOLVED, not just its id. `screenRef` has been marshalled since v1 and the page
    // showed the raw `screen.trailHipInternalRotation` — which tells a coach the name of a test and
    // nothing about how to run it. `screenMissing` is deliberate: a dangling reference must read as
    // a defect, not as a condition that happens to have no screen.
    if (!c->screenRef.isEmpty()) {
        if (const Screen *s = sharedScreenSet().screen(c->screenRef)) {
            QVariantMap sm;
            sm.insert(QStringLiteral("id"), s->id);
            sm.insert(QStringLiteral("label"), s->label);
            sm.insert(QStringLiteral("bodyRegion"), s->bodyRegion);
            sm.insert(QStringLiteral("protocol"), s->protocol);
            sm.insert(QStringLiteral("passCriterion"), s->passCriterion);
            sm.insert(QStringLiteral("note"), s->note);
            out.insert(QStringLiteral("screen"), sm);
        } else {
            out.insert(QStringLiteral("screenMissing"), true);
        }
    }

    QVariantList drillRows;
    for (const QString &did : c->drills) {
        const Drill *d = sharedDrillSet().drill(did);
        QVariantMap  r;
        r.insert(QStringLiteral("id"), did);
        r.insert(QStringLiteral("label"), d ? d->label : did);
        r.insert(QStringLiteral("instruction"), d ? d->instruction : QString());
        r.insert(QStringLiteral("targets"), d ? d->targets : QString());
        r.insert(QStringLiteral("note"), d ? d->note : QString());
        r.insert(QStringLiteral("missing"), d == nullptr);
        drillRows.append(r);
    }
    out.insert(QStringLiteral("drills"), drillRows);

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

QVariantMap CharacteristicLibraryModel::dag(const QString &conditionId, const QVariantMap &options) const
{
    DagLayoutOptions opt;
    auto num = [&options](const char *key, double def) {
        const QVariant v = options.value(QString::fromLatin1(key));
        return v.isValid() ? v.toDouble() : def;
    };
    opt.nodeH      = num("nodeH", opt.nodeH);
    opt.gapX       = num("gapX", opt.gapX);
    opt.gapY       = num("gapY", opt.gapY);
    opt.laneGap    = num("laneGap", opt.laneGap);
    opt.padX       = num("padX", opt.padX);
    opt.charW      = num("charW", opt.charW);
    opt.minW       = num("minW", opt.minW);
    opt.maxW       = num("maxW", opt.maxW);
    opt.depth      = int(num("depth", opt.depth));
    opt.maxPerRank = int(num("maxPerRank", opt.maxPerRank));
    opt.includeMeasures =
        options.value(QStringLiteral("includeMeasures"), opt.includeMeasures).toBool();

    const DagLayout l = layoutDag(m_provider->pack(), conditionId, opt);

    QVariantList nodes;
    for (const DagNode &n : l.nodes) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), n.id);
        m.insert(QStringLiteral("kind"), dagNodeKindName(n.kind));
        m.insert(QStringLiteral("label"), n.label);
        m.insert(QStringLiteral("rank"), n.rank);
        m.insert(QStringLiteral("x"), n.x);
        m.insert(QStringLiteral("y"), n.y);
        m.insert(QStringLiteral("w"), n.w);
        m.insert(QStringLiteral("h"), n.h);
        m.insert(QStringLiteral("latent"), n.latent);
        m.insert(QStringLiteral("offeredOnly"), n.offeredOnly);
        m.insert(QStringLiteral("reach"), n.reach);
        m.insert(QStringLiteral("reachLabel"), n.reachLabel);
        m.insert(QStringLiteral("available"), n.available);
        m.insert(QStringLiteral("unavailableReason"), n.unavailableReason);
        m.insert(QStringLiteral("coverage"), n.coverage);
        m.insert(QStringLiteral("hiddenCauses"), n.hiddenCauses);
        m.insert(QStringLiteral("hiddenEffects"), n.hiddenEffects);
        m.insert(QStringLiteral("groupLabel"), n.groupLabel);
        m.insert(QStringLiteral("statusLabel"), n.statusLabel);
        m.insert(QStringLiteral("metricKey"), n.metricKey);
        nodes.append(m);
    }

    QVariantList edges;
    for (const DagEdge &e : l.edges) {
        QVariantMap m;
        m.insert(QStringLiteral("from"), e.from);
        m.insert(QStringLiteral("to"), e.to);
        m.insert(QStringLiteral("strength"), e.strength);
        m.insert(QStringLiteral("strengthLabel"), e.strengthLabel);
        m.insert(QStringLiteral("weight"), e.weight);
        m.insert(QStringLiteral("detects"), e.detects);
        m.insert(QStringLiteral("offeredOnly"), e.offeredOnly);
        // Without these two the corroborates and excludes edges would be laid out, routed and
        // drawn — as causal lines. That is the marshaller trap in its most misleading form: not a
        // blank, but a confident wrong claim.
        m.insert(QStringLiteral("relation"), e.relation);
        m.insert(QStringLiteral("symmetric"), e.symmetric);
        m.insert(QStringLiteral("segment"), e.segment);
        m.insert(QStringLiteral("segments"), e.segments);
        m.insert(QStringLiteral("x1"), e.x1);
        m.insert(QStringLiteral("y1"), e.y1);
        m.insert(QStringLiteral("c1x"), e.c1x);
        m.insert(QStringLiteral("c1y"), e.c1y);
        m.insert(QStringLiteral("c2x"), e.c2x);
        m.insert(QStringLiteral("c2y"), e.c2y);
        m.insert(QStringLiteral("x2"), e.x2);
        m.insert(QStringLiteral("y2"), e.y2);
        m.insert(QStringLiteral("label"), e.label);
        m.insert(QStringLiteral("labelX"), e.labelX);
        m.insert(QStringLiteral("labelY"), e.labelY);
        m.insert(QStringLiteral("tip"), e.tip);
        m.insert(QStringLiteral("tipAx"), e.tipAx);
        m.insert(QStringLiteral("tipAy"), e.tipAy);
        m.insert(QStringLiteral("tipBx"), e.tipBx);
        m.insert(QStringLiteral("tipBy"), e.tipBy);
        m.insert(QStringLiteral("tipCx"), e.tipCx);
        m.insert(QStringLiteral("tipCy"), e.tipCy);
        edges.append(m);
    }

    QVariantList headings;
    for (const DagHeading &h : l.headings) {
        QVariantMap m;
        m.insert(QStringLiteral("label"), h.label);
        m.insert(QStringLiteral("x"), h.x);
        m.insert(QStringLiteral("y"), h.y);
        m.insert(QStringLiteral("w"), h.w);
        headings.append(m);
    }

    QVariantMap out;
    out.insert(QStringLiteral("nodes"), nodes);
    out.insert(QStringLiteral("edges"), edges);
    out.insert(QStringLiteral("headings"), headings);
    out.insert(QStringLiteral("width"), l.width);
    out.insert(QStringLiteral("height"), l.height);
    out.insert(QStringLiteral("focusX"), l.focusX);
    out.insert(QStringLiteral("focusY"), l.focusY);
    out.insert(QStringLiteral("truncated"), l.truncated);
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

QVariantList CharacteristicLibraryModel::screens() const
{
    const CharacteristicPack &p = m_provider->pack();

    QVariantList out;
    for (const Screen &s : sharedScreenSet().screens) {
        // What this screen would settle. A screen with nothing pointing at it is still listed —
        // the library is being written, and a row nobody uses YET is not a row that is wrong — but
        // the count is what makes the list readable in the order that matters.
        QVariantList settles;
        for (const Condition &c : p.conditions) {
            if (c.screenRef != s.id) continue;
            QVariantMap r;
            r.insert(QStringLiteral("id"), c.id);
            r.insert(QStringLiteral("label"), c.label);
            r.insert(QStringLiteral("explains"), coverageOf(p, c.id));
            settles.append(r);
        }

        QVariantMap r;
        r.insert(QStringLiteral("id"), s.id);
        r.insert(QStringLiteral("label"), s.label);
        r.insert(QStringLiteral("bodyRegion"), s.bodyRegion);
        r.insert(QStringLiteral("protocol"), s.protocol);
        r.insert(QStringLiteral("passCriterion"), s.passCriterion);
        r.insert(QStringLiteral("note"), s.note);
        r.insert(QStringLiteral("citation"), s.citation);
        // The numeric floor and its unit travel together or not at all: a bare number with no unit
        // is unreadable, and the validator refuses that pairing at load for the same reason.
        //
        // NOTHING RENDERS THESE THREE YET, and that is deliberate rather than the marshaller trap
        // it resembles. `passCriterion` is the prose a human reads and already states the figure;
        // the structured triple exists to RECORD an answer against, which needs per-athlete screen
        // storage that does not exist anywhere in the app. Shipped now so the registry's shape is
        // settled before something depends on it — but if you are hunting a field that is complete
        // on both sides and reaching nothing, this one is known.
        r.insert(QStringLiteral("hasPassValue"), s.passAtLeast.has_value());
        r.insert(QStringLiteral("passAtLeast"), s.passAtLeast.value_or(0.0));
        r.insert(QStringLiteral("unit"), s.unit);
        r.insert(QStringLiteral("settles"), settles);
        r.insert(QStringLiteral("settlesCount"), settles.size());
        out.append(r);
    }

    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const int ca = a.toMap().value(QStringLiteral("settlesCount")).toInt();
        const int cb = b.toMap().value(QStringLiteral("settlesCount")).toInt();
        if (ca != cb) return ca > cb;
        return a.toMap().value(QStringLiteral("label")).toString()
             < b.toMap().value(QStringLiteral("label")).toString();
    });
    return out;
}

QVariantList CharacteristicLibraryModel::drills() const
{
    const CharacteristicPack &p = m_provider->pack();

    QVariantList out;
    for (const Drill &d : sharedDrillSet().drills) {
        QVariantList answers;
        for (const Condition &c : p.conditions) {
            if (!c.drills.contains(d.id)) continue;
            QVariantMap r;
            r.insert(QStringLiteral("id"), c.id);
            r.insert(QStringLiteral("label"), c.label);
            answers.append(r);
        }

        QVariantMap r;
        r.insert(QStringLiteral("id"), d.id);
        r.insert(QStringLiteral("label"), d.label);
        r.insert(QStringLiteral("instruction"), d.instruction);
        r.insert(QStringLiteral("targets"), d.targets);
        r.insert(QStringLiteral("equipment"), d.equipment);
        r.insert(QStringLiteral("note"), d.note);
        r.insert(QStringLiteral("answers"), answers);
        r.insert(QStringLiteral("answersCount"), answers.size());
        out.append(r);
    }

    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const int ca = a.toMap().value(QStringLiteral("answersCount")).toInt();
        const int cb = b.toMap().value(QStringLiteral("answersCount")).toInt();
        if (ca != cb) return ca > cb;
        return a.toMap().value(QStringLiteral("label")).toString()
             < b.toMap().value(QStringLiteral("label")).toString();
    });
    return out;
}

QVariantList CharacteristicLibraryModel::references() const
{
    const CharacteristicPack &p = m_provider->pack();

    QVariantList out;
    for (const Reference &ref : sharedReferenceSet().references) {
        // The citation joins against EITHER identifier: a handful of journals issue no DOI, and a
        // strict DOI match would silently drop every claim resting on those papers — the row would
        // render "cited by nothing" while the library leaned on it.
        const auto matches = [&ref](const QString &citation) {
            if (citation.isEmpty()) return false;
            return (!ref.doi.isEmpty() && citation == ref.doi)
                || (!ref.pmid.isEmpty() && citation == ref.pmid);
        };

        // Every claim resting on this paper, and the tier each one earned. An edge cited at
        // `indirect` and one cited at `supported` make very different use of the same source, so
        // the tier travels with the row rather than being averaged away into a count.
        QVariantList cites;
        for (const Edge &e : p.edges) {
            if (!matches(e.provenance.citation)) continue;
            const Condition *from = p.condition(e.from);
            const Condition *to   = p.condition(e.to);
            QVariantMap r;
            r.insert(QStringLiteral("kind"), QStringLiteral("edge"));
            r.insert(QStringLiteral("fromId"), e.from);
            r.insert(QStringLiteral("toId"), e.to);
            r.insert(QStringLiteral("from"), from ? from->label : e.from);
            r.insert(QStringLiteral("to"), to ? to->label : e.to);
            r.insert(QStringLiteral("edgeType"), edgeTypeName(e.type));
            r.insert(QStringLiteral("tier"), provenanceTierName(e.provenance.tier));
            r.insert(QStringLiteral("tierLabel"), provenanceTierLabel(e.provenance.tier));
            cites.append(r);
        }
        for (const Condition &c : p.conditions) {
            if (!matches(c.provenance.citation)) continue;
            QVariantMap r;
            r.insert(QStringLiteral("kind"), QStringLiteral("condition"));
            r.insert(QStringLiteral("fromId"), c.id);
            r.insert(QStringLiteral("from"), c.label);
            r.insert(QStringLiteral("tier"), provenanceTierName(c.provenance.tier));
            r.insert(QStringLiteral("tierLabel"), provenanceTierLabel(c.provenance.tier));
            cites.append(r);
        }

        QVariantMap r;
        r.insert(QStringLiteral("id"), ref.id);
        r.insert(QStringLiteral("doi"), ref.doi);
        r.insert(QStringLiteral("pmid"), ref.pmid);
        r.insert(QStringLiteral("identifier"), ref.identifierLabel());
        r.insert(QStringLiteral("url"), ref.url());
        r.insert(QStringLiteral("title"), ref.title);
        r.insert(QStringLiteral("authors"), ref.authors);
        r.insert(QStringLiteral("journal"), ref.journal);
        r.insert(QStringLiteral("year"), ref.year);
        r.insert(QStringLiteral("establishes"), ref.establishes);
        r.insert(QStringLiteral("cites"), cites);
        r.insert(QStringLiteral("citeCount"), cites.size());
        out.append(r);
    }

    // By how much of the library each one holds up. That ordering IS the argument: the paper four
    // claims rest on is a different kind of object from the one cited once, and a bibliography
    // sorted alphabetically hides exactly that.
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const int ca = a.toMap().value(QStringLiteral("citeCount")).toInt();
        const int cb = b.toMap().value(QStringLiteral("citeCount")).toInt();
        if (ca != cb) return ca > cb;
        const int ya = a.toMap().value(QStringLiteral("year")).toInt();
        const int yb = b.toMap().value(QStringLiteral("year")).toInt();
        if (ya != yb) return ya > yb;
        return a.toMap().value(QStringLiteral("title")).toString()
             < b.toMap().value(QStringLiteral("title")).toString();
    });
    return out;
}

QVariantList CharacteristicLibraryModel::glossary(const QString &search) const
{
    const CharacteristicPack &p = m_provider->pack();
    const QString             q = search.trimmed();

    QVariantList out;
    for (const Condition &c : p.conditions) {
        if (!q.isEmpty()) {
            bool hit = c.label.contains(q, Qt::CaseInsensitive)
                       || c.consequence.text().contains(q, Qt::CaseInsensitive);
            if (!hit)
                for (const QString &a : c.aliases)
                    if (a.contains(q, Qt::CaseInsensitive)) { hit = true; break; }
            if (!hit) continue;
        }

        // "Commonly caused by …", straight off the causal edges. This is what makes the glossary
        // cost nothing to maintain: the entry is not written anywhere, it is the graph read out.
        QVariantList causedBy;
        for (const QString &cid : causesOf(p, c.id)) {
            const Condition *cause = p.condition(cid);
            QVariantMap      r;
            r.insert(QStringLiteral("id"), cid);
            r.insert(QStringLiteral("label"), cause ? cause->label : cid);
            causedBy.append(r);
        }

        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("aliases"), c.aliases);
        r.insert(QStringLiteral("meaning"), c.consequence.text());
        r.insert(QStringLiteral("group"), conditionGroupName(c.group));
        r.insert(QStringLiteral("groupLabel"), conditionGroupLabel(c.group));
        r.insert(QStringLiteral("causedBy"), causedBy);
        r.insert(QStringLiteral("proposed"), c.provenance.tier == ProvenanceTier::Proposed);
        out.append(r);
    }

    // Alphabetical by the name a reader would look up, not by group: a glossary is consulted, not
    // browsed, and grouping it would make the common use — "what does X mean" — the slow one.
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("label")).toString().toCaseFolded()
             < b.toMap().value(QStringLiteral("label")).toString().toCaseFolded();
    });
    return out;
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
        QString       gapReason;            // ExternalDevice: which device, in one line
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
        //
        // ExternalDevice rows DO belong here — integrating a launch monitor is work we intend to do,
        // and leaving it out would make the roadmap understate what stands between the pack and a
        // complete diagnosis. They carry `integration: true` so the view can section them: the
        // reader has to be able to tell "write this producer" from "talk to that device", and a
        // single ranked list cannot say both.

        const QString key = m.metricKey.isEmpty() ? canonicalSeriesId(m.series) : m.metricKey;
        Group        &g   = groups[key];

        if (g.samples == 0) {
            g.metricKey = m.metricKey;
            g.status    = m.status;
            g.view      = m.viewNeeded;
            g.gapReason = m.gapReason;
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
        r.insert(QStringLiteral("integration"), g.status == MeasureStatus::ExternalDevice);
        r.insert(QStringLiteral("gapReason"), g.gapReason);
        out.append(r);
    }
    // Pipeline work first, integrations after — then by how much each unblocks. Sorting them into
    // one ranked list by `blocks` alone would put "integrate a launch monitor" at the top of a list
    // a developer reads as their queue.
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const bool ia = a.toMap().value(QStringLiteral("integration")).toBool();
        const bool ib = b.toMap().value(QStringLiteral("integration")).toBool();
        if (ia != ib) return !ia;
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

namespace {

// A health row. `subject` is either an id or a "measure@context" norm key, and the two halves are
// split out when it is the latter so the view can act on the row rather than only describe it.
QVariantMap healthRow(const ValidationIssue &i)
{
    QVariantMap r;
    r.insert(QStringLiteral("code"),     i.code);
    r.insert(QStringLiteral("subject"),  i.subject);
    r.insert(QStringLiteral("message"),  i.message);
    r.insert(QStringLiteral("severity"),
             i.severity == IssueSeverity::Error ? QStringLiteral("error") : QStringLiteral("warning"));

    const int at = i.subject.indexOf(QLatin1Char('@'));
    if (at > 0) {
        r.insert(QStringLiteral("measureId"), i.subject.left(at));
        r.insert(QStringLiteral("contextId"), i.subject.mid(at + 1));
    } else {
        r.insert(QStringLiteral("measureId"), QString());
        r.insert(QStringLiteral("contextId"), QString());
    }
    return r;
}

} // namespace

QVariantList CharacteristicLibraryModel::health() const
{
    QVariantList out;

    // The characteristic pack's own warnings. Errors are excluded here as they always were: a pack
    // with errors did not load, so the library on screen is not the one they describe.
    for (const ValidationIssue &i : m_provider->report().issues) {
        if (i.severity != IssueSeverity::Warning) continue;
        out.append(healthRow(i));
    }

    // The norm set's own warnings. norm_provider.h has said since stage 1 that "warnings here ARE
    // part of the health list" — they were not, because this list only ever read the pack provider.
    if (m_norms) {
        for (const ValidationIssue &i : m_norms->report().issues) {
            if (i.severity != IssueSeverity::Warning) continue;
            out.append(healthRow(i));
        }

        // The assembled-library checks, INCLUDING the referential norm validation that nothing had
        // ever called. Those can be errors (a unit mismatch is), and unlike a pack load error they
        // describe the library that is actually on screen — so they are shown, and marked.
        for (const ValidationIssue &i : diagnosticsHealth(m_provider->pack(), *m_norms, m_cat))
            out.append(healthRow(i));
    }

    return out;
}

QVariantList CharacteristicLibraryModel::corpusHealth() const
{
    QVariantList out;
    for (const ValidationIssue &i : corpusShareHealth(m_corpusCounts))
        out.append(healthRow(i));
    return out;
}

void CharacteristicLibraryModel::setGradePolicy(const QString &name)
{
    // Resolved through the shared table, never stored as handed in — same rule as every other holder
    // of this setting: an unknown name must grade against the default AND read as the default.
    const QString resolved = QString::fromLatin1(gradePolicyPresetFor(name).name);
    if (resolved == m_policyName) return;
    m_policyName = resolved;
    // The counts were taken under the old policy, so they no longer describe what the app would say.
    // Dropped rather than re-graded in place: re-grading needs the values, and only the scan has them.
    m_corpusCounts.clear();
    m_corpusEverScanned = false;
    emit gradePolicyChanged();
    emit corpusChanged();
}

void CharacteristicLibraryModel::setLibraryRoot(const QString &root)
{
    if (root == m_libraryRoot) return;
    m_libraryRoot = root;
    // A new library invalidates what the old one said. Reported as never-scanned rather than as
    // "nothing wrong", which is the distinction corpusEverScanned exists for.
    m_corpusCounts.clear();
    m_corpusEverScanned = false;
    m_corpusSwings      = 0;
    emit corpusChanged();
}

// One pass over the library, grading every reading against every norm that resolves for it.
//
// Cheap enough to be worth doing whole: the expensive part is reading a swing's phase grid, which is
// sidecar-cached and pack-INDEPENDENT, so covering all 67 measures costs the same as covering one.
// That is exactly why measure_sample caches the grid rather than measure values.
void CharacteristicLibraryModel::startCorpusCheck()
{
    if (m_corpusWatcher->isRunning() || m_libraryRoot.isEmpty()) {
        if (m_libraryRoot.isEmpty()) {
            m_corpusEverScanned = false;
            emit corpusChanged();
        }
        return;
    }

    // Everything the worker needs, BY VALUE. It must not touch the pack or the provider: those live
    // on this thread and a refresh() would pull them out from under it.
    struct Target { Measure measure; QString contextId; Norm norm; };
    std::vector<Target> targets;
    const CharacteristicPack &pack = m_provider->pack();
    for (const Norm &n : m_norms->norms().norms) {
        const Measure *m = pack.measure(n.measureId);
        if (m == nullptr || m->status != MeasureStatus::Live) continue;
        targets.push_back(Target{ *m, n.contextId, n });
    }

    const QString     root   = m_libraryRoot;
    const GradePolicy policy = gradePolicyByName(m_policyName);

    m_corpusScanning = true;
    emit corpusChanged();

    // A cap, not a sample size — and REPORTED when it bites, because a silent cap reads as "that is
    // the whole library". Same figure the corridor editor's scan uses.
    constexpr int kMaxScan = 2000;

    m_corpusWatcher->setFuture(QtConcurrent::run([root, targets, policy]() -> QVariantList {
        // Grade counts per target, plus the swing count as the last entry — one future, so the two
        // cannot arrive out of step.
        std::vector<int> ideal(targets.size(), 0), good(targets.size(), 0),
                         watch(targets.size(), 0), action(targets.size(), 0);
        int  swings    = 0;
        bool truncated = false;

        const QDir rootDir(root);
        const QStringList athletes = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &athlete : athletes) {
            const QDir aDir(rootDir.filePath(athlete));
            for (const QString &session : aDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                const QDir sDir(aDir.filePath(session));
                for (const QString &swing : sDir.entryList({ QStringLiteral("swing_*") },
                                                           QDir::Dirs, QDir::Name)) {
                    if (swings >= kMaxScan) { truncated = true; break; }
                    const SwingPhaseGrid grid = readPhaseGrid(sDir.filePath(swing),
                                                              /*writeSidecar*/ true);
                    ++swings;
                    for (size_t t = 0; t < targets.size(); ++t) {
                        const std::optional<double> v = reduceOverGrid(grid, targets[t].measure);
                        if (!v) continue;
                        switch (grade(*v, targets[t].norm, policy)) {
                        case Grade::Ideal:  ++ideal[t];  break;
                        case Grade::Good:   ++good[t];   break;
                        case Grade::Watch:  ++watch[t];  break;
                        case Grade::Action: ++action[t]; break;
                        case Grade::NotMeasured: break;
                        }
                    }
                }
            }
        }

        QVariantList out;
        for (size_t t = 0; t < targets.size(); ++t) {
            QVariantMap r;
            r.insert(QStringLiteral("measureId"), targets[t].measure.id);
            r.insert(QStringLiteral("contextId"), targets[t].contextId);
            r.insert(QStringLiteral("ideal"),     ideal[t]);
            r.insert(QStringLiteral("good"),      good[t]);
            r.insert(QStringLiteral("watch"),     watch[t]);
            r.insert(QStringLiteral("action"),    action[t]);
            out.append(r);
        }
        QVariantMap tail;
        tail.insert(QStringLiteral("swings"),    swings);
        tail.insert(QStringLiteral("truncated"), truncated);
        out.append(tail);
        return out;
    }));
}

void CharacteristicLibraryModel::onCorpusFinished()
{
    const QVariantList res = m_corpusWatcher->future().result();

    m_corpusCounts.clear();
    m_corpusSwings = 0;
    for (const QVariant &v : res) {
        const QVariantMap r = v.toMap();
        if (r.contains(QStringLiteral("swings"))) {
            m_corpusSwings    = r.value(QStringLiteral("swings")).toInt();
            m_corpusTruncated = r.value(QStringLiteral("truncated")).toBool();
            continue;
        }
        CorpusGradeCounts c;
        c.measureId = r.value(QStringLiteral("measureId")).toString();
        c.contextId = r.value(QStringLiteral("contextId")).toString();
        c.ideal     = r.value(QStringLiteral("ideal")).toInt();
        c.good      = r.value(QStringLiteral("good")).toInt();
        c.watch     = r.value(QStringLiteral("watch")).toInt();
        c.action    = r.value(QStringLiteral("action")).toInt();
        m_corpusCounts.push_back(c);
    }

    m_corpusScanning    = false;
    m_corpusEverScanned = true;
    emit corpusChanged();
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

    // Split at the source rather than filtering the table twice: "write this producer" and
    // "integrate that device" are different kinds of work, wanted by different people, and a single
    // ranked list cannot say both without the reader assuming the wrong one.
    QVariantList rows, integrations;
    for (const QVariant &v : roadmap()) {
        if (v.toMap().value(QStringLiteral("integration")).toBool()) integrations.append(v);
        else                                                        rows.append(v);
    }

    auto table = [](const QVariantList &l) {
        QString t = QStringLiteral("| Measure | Unblocks | Status | View | Metric key |\n");
        t += QStringLiteral("|---|---:|---|---|---|\n");
        for (const QVariant &v : l) {
            const QVariantMap r = v.toMap();
            t += QStringLiteral("| %1 | %2 | %3 | %4 | `%5` |\n")
                     .arg(r.value(QStringLiteral("label")).toString())
                     .arg(r.value(QStringLiteral("blocks")).toInt())
                     .arg(r.value(QStringLiteral("statusLabel")).toString(),
                          r.value(QStringLiteral("viewNeeded")).toString(),
                          r.value(QStringLiteral("metricKey")).toString());
        }
        return t + QStringLiteral("\n");
    };

    if (rows.isEmpty()) md += QStringLiteral("_Nothing outstanding._\n\n");
    else                md += table(rows);

    if (!integrations.isEmpty()) {
        md += QStringLiteral("## Needs an external device\n\n");
        md += QStringLiteral("Also roadmap work, but the work is an INTEGRATION rather than a "
                             "producer written from our own pixels — and the golfer needs the "
                             "hardware too. Until one is connected these read \"needs a launch "
                             "monitor\" on every shot, which is the intended fallback, not a "
                             "failure.\n\n");
        md += table(integrations);
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
