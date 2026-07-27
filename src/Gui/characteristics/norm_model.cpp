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

#include "norm_model.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>

using namespace pinpoint::analysis;

namespace {

// The grade policy presets.
//
// A NAME, not three numbers. GradePolicy is deliberately pack-wide and singular — if "Ideal" means
// one thing here and something else in a shared pack, nothing is comparable across athletes — so
// the control offers three considered settings rather than three spinboxes. The z values are shown
// beneath each so the setting is inspectable, but they are not editable: a hand-tuned policy is a
// private vocabulary.
//
// The TABLE lives in the norm layer (gradePolicyPresets(), norm.h) because the metric detail page
// and the dashboard corridors now resolve a policy by name too, and two tables would let two
// surfaces grade against different z's while showing the same word. This file translates the labels
// at the point of render, which is the only part of a preset that is UI.

QString statusLabel(MeasureStatus s)
{
    switch (s) {
    case MeasureStatus::Live:           return QObject::tr("Live");
    case MeasureStatus::Planned:        return QObject::tr("Planned");
    case MeasureStatus::NoProducer:     return QObject::tr("No producer");
    case MeasureStatus::NotCapturable:  return QObject::tr("Not measurable from capture");
    case MeasureStatus::ExternalDevice: return QObject::tr("Needs a launch monitor");
    }
    return QString();
}

// Forwards to the words that live with the enum (norm.h). Kept as a local name because this file
// reads it in several places, but it is NOT a second definition — same shape as
// resolvabilityLabel() over measureStatusLabel().
QString sourceLabel(NormSource s) { return normSourceLabel(s); }

QString originLabel(PackOrigin o)
{
    switch (o) {
    case PackOrigin::Core:      return QObject::tr("Shipped");
    case PackOrigin::LocalUser: return QObject::tr("Yours");
    case PackOrigin::Community: return QObject::tr("Community");
    }
    return QString();
}

// The ungrouped bucket. Named rather than blank so it sorts and filters like every other group —
// a measure whose metric the catalogue does not carry still has to be findable.
QString otherGroup() { return QObject::tr("Other"); }

// How a measure reads its curve, in words, for the detail header. The reducer is where the phase
// lives, so this is also what tells a reader WHY a corridor keyed on P4 found this measure.
QString reducerLabel(const Reducer &r)
{
    switch (r.kind) {
    case ReducerKind::At:
        return QObject::tr("read at %1").arg(phaseLabel(r.anchor.value_or(r.window.first)));
    case ReducerKind::Delta:
        return QObject::tr("change from %1 to %2")
            .arg(phaseLabel(r.window.first), phaseLabel(r.window.second));
    case ReducerKind::Rate:
        return QObject::tr("rate of change, %1 to %2")
            .arg(phaseLabel(r.window.first), phaseLabel(r.window.second));
    case ReducerKind::Extremum:
        return r.sense == ExtremumSense::Min
                   ? QObject::tr("lowest between %1 and %2")
                         .arg(phaseLabel(r.window.first), phaseLabel(r.window.second))
                   : QObject::tr("highest between %1 and %2")
                         .arg(phaseLabel(r.window.first), phaseLabel(r.window.second));
    }
    return QString();
}

} // namespace

NormModel::NormModel(QObject *parent)
    : QObject(parent),
      m_pack(makeCharacteristicPackProvider()),
      m_norms(sharedNormProvider()),
      m_cat(makeMetricCatalogue()),
      m_policyName(QStringLiteral("standard"))
{
}

NormModel::~NormModel() = default;

GradePolicy NormModel::policy() const { return gradePolicyByName(m_policyName); }

void NormModel::setGradePolicy(const QString &name)
{
    // Resolve through the table rather than storing what was handed in: an unknown name must land
    // on standard, not persist as itself and silently grade against the default while the control
    // shows something else.
    const QString resolved = QString::fromLatin1(gradePolicyPresetFor(name).name);
    if (resolved == m_policyName) return;
    m_policyName = resolved;
    emit gradePolicyChanged();
}

QVariantList NormModel::gradePolicies() const
{
    QVariantList out;
    for (const GradePolicyPreset &p : gradePolicyPresets()) {
        QVariantMap m;
        m.insert(QStringLiteral("name"),      QString::fromLatin1(p.name));
        m.insert(QStringLiteral("label"),     QCoreApplication::translate("NormModel", p.label));
        m.insert(QStringLiteral("hint"),      QCoreApplication::translate("NormModel", p.hint));
        m.insert(QStringLiteral("idealMaxZ"), p.policy.idealMaxZ);
        m.insert(QStringLiteral("goodMaxZ"),  p.policy.goodMaxZ);
        m.insert(QStringLiteral("watchMaxZ"), p.policy.watchMaxZ);
        out.append(m);
    }
    return out;
}

// ── Vocabularies ────────────────────────────────────────────────────────────

QString NormModel::groupOf(const Measure &m) const
{
    if (!m.metricKey.isEmpty())
        if (const MetricDescriptor *d = m_cat.descriptor(m.metricKey))
            if (!d->group.isEmpty()) return d->group;
    return otherGroup();
}

QString NormModel::unitOf(const Measure &m) const
{
    if (!m.unit.isEmpty()) return m.unit;
    if (!m.metricKey.isEmpty())
        if (const MetricDescriptor *d = m_cat.descriptor(m.metricKey))
            return d->unit;
    return QString();
}

QVariantList NormModel::groups() const
{
    // Derived from the measures that actually exist, in the catalogue's own group order, so a chip
    // never appears for a group with nothing behind it. "Other" sorts last because it is a
    // fallback, not a category.
    const CharacteristicPack &p = m_pack->pack();

    QStringList  order;
    QSet<QString> seen;
    for (const MetricDescriptor *d : m_cat.all())
        if (!d->group.isEmpty() && !seen.contains(d->group)) { seen.insert(d->group); order << d->group; }

    QSet<QString> present;
    for (const Measure &m : p.measures) present.insert(groupOf(m));

    QVariantList out;
    for (const QString &g : order) {
        if (!present.contains(g)) continue;
        QVariantMap r;
        r.insert(QStringLiteral("name"),  g);
        r.insert(QStringLiteral("label"), g);
        out.append(r);
    }
    if (present.contains(otherGroup())) {
        QVariantMap r;
        r.insert(QStringLiteral("name"),  otherGroup());
        r.insert(QStringLiteral("label"), otherGroup());
        out.append(r);
    }
    return out;
}

QVariantList NormModel::statuses() const
{
    QVariantList out;
    for (MeasureStatus s : { MeasureStatus::Live, MeasureStatus::Planned,
                             MeasureStatus::NoProducer, MeasureStatus::ExternalDevice,
                             MeasureStatus::NotCapturable }) {
        QVariantMap r;
        r.insert(QStringLiteral("name"),  measureStatusName(s));
        r.insert(QStringLiteral("label"), statusLabel(s));
        out.append(r);
    }
    return out;
}

QVariantList NormModel::contexts() const
{
    const ContextTree &tree = m_norms->contexts();

    QVariantList out;
    for (const QString &id : tree.inOrder()) {
        const ContextNode *n = tree.node(id);
        if (!n) continue;
        QVariantMap r;
        r.insert(QStringLiteral("id"),        n->id);
        r.insert(QStringLiteral("label"),     n->label);
        r.insert(QStringLiteral("parentId"),  n->parentId);
        r.insert(QStringLiteral("depth"),     tree.depth(id));
        r.insert(QStringLiteral("isDefault"), n->id == kDefaultContextId());
        out.append(r);
    }
    return out;
}

QVariantList NormModel::normSets() const
{
    // The LAYERS, not the assembled set. One row today (the shipped core set), but the shape is
    // the real one: a user set appears here the moment the corridor editor writes one, and a
    // reader has to see the two as separate things because that separation is what "your edit
    // overrides the shipped norm" means.
    //
    // `active` is a real switch now: a disabled layer is absent from the assembly, so it does not
    // appear in layers() at all and has to be added back from the disabled list to be shown as an
    // off row. A set you cannot see is a set you cannot turn back on.
    const QStringList off = disabledNormSets();

    QVariantList out;
    for (const NormSetInfo &l : m_norms->layers()) {
        QVariantMap r;
        r.insert(QStringLiteral("id"),        l.id);
        r.insert(QStringLiteral("label"),     l.label);
        r.insert(QStringLiteral("origin"),    originLabel(l.origin));
        r.insert(QStringLiteral("normCount"), l.normCount);
        r.insert(QStringLiteral("readOnly"),  l.readOnly);
        r.insert(QStringLiteral("active"),    true);
        out.append(r);
    }

    for (const QString &id : off) {
        QVariantMap r;
        r.insert(QStringLiteral("id"),        id);
        r.insert(QStringLiteral("label"),     id);
        r.insert(QStringLiteral("origin"),    tr("Switched off"));
        r.insert(QStringLiteral("normCount"), 0);
        r.insert(QStringLiteral("readOnly"),  false);
        r.insert(QStringLiteral("active"),    false);
        out.append(r);
    }
    return out;
}

void NormModel::refresh()
{
    resetSharedNormProvider();
    m_norms = sharedNormProvider();
    emit normsChanged();
}

void NormModel::setNormSetActive(const QString &normSetId, bool active)
{
    QStringList off = disabledNormSets();
    if (active) off.removeAll(normSetId);
    else if (!off.contains(normSetId)) off.append(normSetId);
    setDisabledNormSets(off);
}

void NormModel::setDisabledNormSets(const QStringList &ids)
{
    if (ids == disabledNormSets())
        return;
    // The free function in norm_provider.h, not this method — the member shadows it inside this
    // scope, and calling it here unqualified would recurse.
    ::pinpoint::analysis::setDisabledNormSets(ids);
    refresh();
}

int NormModel::measureCount() const { return int(m_pack->pack().measures.size()); }
int NormModel::normCount() const    { return int(m_norms->norms().norms.size()); }

int NormModel::editedNormCount() const
{
    // Rows a user layer supplies, over the whole assembled set. The one number that answers "how
    // far is this library from what we ship".
    int n = 0;
    for (const Norm &nm : m_norms->norms().norms)
        if (m_norms->isOverridden(nm.measureId, nm.contextId)) ++n;
    return n;
}

int NormModel::normedMeasureCount() const
{
    // Measures that RESOLVE to a norm, not measures with their own row: a driver corridor
    // inherited from full swing is still a graded measure, and counting only own rows would
    // understate how much of the library can actually fire.
    const CharacteristicPack &p = m_pack->pack();
    int n = 0;
    for (const Measure &m : p.measures)
        if (m_norms->resolve(m.id, kDefaultContextId()).found()) ++n;
    return n;
}

// ── Directory ───────────────────────────────────────────────────────────────

QStringList NormModel::usersOfMeasureIds(const QString &measureId) const
{
    const CharacteristicPack &p = m_pack->pack();

    QStringList signalsUsing;
    for (const Signal &s : p.signalDefs)
        if (s.measures.contains(measureId)) signalsUsing << s.id;

    QStringList out;
    for (const Condition &c : p.conditions)
        for (const QString &sid : c.detectedBy)
            if (signalsUsing.contains(sid)) { out << c.id; break; }
    return out;
}

QVariantList NormModel::measures(const QVariantMap &filters) const
{
    const CharacteristicPack &p = m_pack->pack();

    const QString group   = filters.value(QStringLiteral("group")).toString();
    const QString status  = filters.value(QStringLiteral("status")).toString();
    const QString hasNorm = filters.value(QStringLiteral("hasNorm")).toString();
    const QString edited  = filters.value(QStringLiteral("edited")).toString();
    const QString search  = filters.value(QStringLiteral("search")).toString().trimmed();

    QVariantList out;
    for (const Measure &m : p.measures) {
        if (!group.isEmpty()  && groupOf(m) != group) continue;
        if (!status.isEmpty() && measureStatusName(m.status) != status) continue;

        // The default context is what a row is graded against when a shot says nothing, so it is
        // the honest thing to summarise a row by.
        const NormResolution res = m_norms->resolve(m.id, kDefaultContextId());

        if (hasNorm == QLatin1String("yes") && !res.found()) continue;
        if (hasNorm == QLatin1String("no")  &&  res.found()) continue;

        // "What have I changed?" is a real question after an afternoon in the corridor editor, and
        // it has no other answer in the app.
        int editedRows = 0;
        for (const QString &cid : m_norms->overriddenContextsFor(m.id))
            if (m_norms->isOverridden(m.id, cid)) ++editedRows;
        if (edited == QLatin1String("yes") && editedRows == 0) continue;
        if (edited == QLatin1String("no")  && editedRows >  0) continue;

        if (!search.isEmpty()) {
            const bool hit = m.label.contains(search, Qt::CaseInsensitive)
                             || m.id.contains(search, Qt::CaseInsensitive)
                             || m.metricKey.contains(search, Qt::CaseInsensitive)
                             || m.aliases.filter(search, Qt::CaseInsensitive).size() > 0;
            if (!hit) continue;
        }

        QVariantMap r;
        r.insert(QStringLiteral("id"),          m.id);
        r.insert(QStringLiteral("label"),       m.label.isEmpty() ? m.id : m.label);
        r.insert(QStringLiteral("unit"),        unitOf(m));
        r.insert(QStringLiteral("group"),       groupOf(m));
        r.insert(QStringLiteral("status"),      measureStatusName(m.status));
        r.insert(QStringLiteral("statusLabel"), statusLabel(m.status));
        r.insert(QStringLiteral("metricKey"),   m.metricKey);
        r.insert(QStringLiteral("highMeans"),   m.highMeans);
        r.insert(QStringLiteral("usedBy"),      usersOfMeasureIds(m.id).size());
        r.insert(QStringLiteral("ownNormCount"),
                 int(m_norms->overriddenContextsFor(m.id).size()));

        // Has this measure been edited ANYWHERE? Counted over the measure's OWN rows, not the
        // whole tree, because an inherited row is not a second edit.
        r.insert(QStringLiteral("editedNormCount"), editedRows);
        r.insert(QStringLiteral("userEdited"),      editedRows > 0);

        r.insert(QStringLiteral("hasNorm"), res.found());
        if (res.found()) {
            const ContextNode *cn = m_norms->contexts().node(res.contextId);
            r.insert(QStringLiteral("defaultContextId"),    res.contextId);
            r.insert(QStringLiteral("defaultContextLabel"), cn ? cn->label : res.contextId);
            r.insert(QStringLiteral("normInherited"),       res.inherited);
            r.insert(QStringLiteral("idealLo"),             res.norm->idealLo());
            r.insert(QStringLiteral("idealHi"),             res.norm->idealHi());
            r.insert(QStringLiteral("weakProvenance"),      normIsWeak(*res.norm));
        } else {
            r.insert(QStringLiteral("defaultContextId"),    QString());
            r.insert(QStringLiteral("defaultContextLabel"), QString());
            r.insert(QStringLiteral("normInherited"),       false);
            r.insert(QStringLiteral("idealLo"),             0.0);
            r.insert(QStringLiteral("idealHi"),             0.0);
            r.insert(QStringLiteral("weakProvenance"),      false);
        }
        out.append(r);
    }

    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("label")).toString().localeAwareCompare(
                   b.toMap().value(QStringLiteral("label")).toString()) < 0;
    });
    return out;
}

// ── One norm, resolved ──────────────────────────────────────────────────────

QVariantMap NormModel::normAt(const QString &measureId, const QString &contextId) const
{
    QVariantMap out;

    const NormResolution res = m_norms->resolve(measureId, contextId);
    out.insert(QStringLiteral("found"), res.found());
    if (!res.found())
        return out;

    const Norm       &n  = *res.norm;
    const GradePolicy pol = policy();
    const ContextNode *cn = m_norms->contexts().node(res.contextId);

    out.insert(QStringLiteral("contextId"),     res.contextId);
    out.insert(QStringLiteral("contextLabel"),  cn ? cn->label : res.contextId);
    out.insert(QStringLiteral("inherited"),     res.inherited);
    out.insert(QStringLiteral("own"),           !res.inherited);
    out.insert(QStringLiteral("inheritedFrom"), res.inherited ? (cn ? cn->label : res.contextId)
                                                              : QString());

    // The Watch edge — where Action begins — comes from bandEdgesOf(), the one place the precedence
    // (explicit monitor bounds DOMINATE the z-derived edge, exactly as grade() applies them) is
    // stated. Showing the z edge while grading on the monitor would render a corridor the app does
    // not use.
    const NormBandEdges edges = bandEdgesOf(n, pol);

    out.insert(QStringLiteral("mu"),      n.mu);
    out.insert(QStringLiteral("idealLo"), edges.idealLo);
    out.insert(QStringLiteral("idealHi"), edges.idealHi);
    out.insert(QStringLiteral("goodLo"),  n.mu - pol.goodMaxZ * n.sigmaLo);
    out.insert(QStringLiteral("goodHi"),  n.mu + pol.goodMaxZ * n.sigmaHi);
    out.insert(QStringLiteral("watchLo"), edges.watchLo);
    out.insert(QStringLiteral("watchHi"), edges.watchHi);
    out.insert(QStringLiteral("explicitMonitor"), n.hasExplicitMonitor());

    out.insert(QStringLiteral("unit"),        n.unit);
    out.insert(QStringLiteral("n"),           n.n);
    out.insert(QStringLiteral("source"),      normSourceName(n.source));
    out.insert(QStringLiteral("sourceLabel"), sourceLabel(n.source));
    out.insert(QStringLiteral("author"),      n.author);
    out.insert(QStringLiteral("citation"),    n.citation);
    out.insert(QStringLiteral("setOn"),       n.setOn.isValid() ? n.setOn.toString(Qt::ISODate)
                                                                : QString());
    out.insert(QStringLiteral("weak"),        normIsWeak(n));
    out.insert(QStringLiteral("weakReason"),  normWeakReason(n));

    // ── Shipped vs yours ────────────────────────────────────────────────────
    //
    // Tracked, never derived by comparing numbers: a user row holding exactly the shipped values is
    // still a user row, and a value comparison would quietly un-mark it. `overridden` follows the
    // RESOLUTION, so a context inheriting the user's override reads as edited too — it is being
    // graded by the user's corridor, and saying otherwise would be false.
    const Norm *shipped = m_norms->shippedNorm(measureId, res.contextId);
    out.insert(QStringLiteral("overridden"),     res.overridden);
    out.insert(QStringLiteral("hasShipped"),     shipped != nullptr);
    out.insert(QStringLiteral("shippedIdealLo"), shipped ? shipped->idealLo() : 0.0);
    out.insert(QStringLiteral("shippedIdealHi"), shipped ? shipped->idealHi() : 0.0);
    return out;
}

// ── Detail ──────────────────────────────────────────────────────────────────

QVariantMap NormModel::measureDetail(const QString &measureId) const
{
    const CharacteristicPack &p = m_pack->pack();
    const Measure            *m = p.measure(measureId);
    if (!m)
        return {};

    QVariantMap out;
    out.insert(QStringLiteral("id"),           m->id);
    out.insert(QStringLiteral("label"),        m->label.isEmpty() ? m->id : m->label);
    out.insert(QStringLiteral("unit"),         unitOf(*m));
    out.insert(QStringLiteral("group"),        groupOf(*m));
    out.insert(QStringLiteral("kind"),         measureKindName(m->kind));
    out.insert(QStringLiteral("status"),       measureStatusName(m->status));
    out.insert(QStringLiteral("statusLabel"),  statusLabel(m->status));
    out.insert(QStringLiteral("metricKey"),    m->metricKey);
    out.insert(QStringLiteral("gapReason"),    m->gapReason);
    out.insert(QStringLiteral("viewNeeded"),   viewNeededName(m->viewNeeded));
    out.insert(QStringLiteral("reducerLabel"), reducerLabel(m->reducer));
    out.insert(QStringLiteral("aliases"),      m->aliases);

    // What this measure IS. A Composed measure names its own facets; a Provided one borrows the
    // catalogue's sentence, because the catalogue is where that metric is documented and a second
    // description would be a second thing to keep true.
    QString what;
    if (m->kind == MeasureKind::Composed) {
        what = canonicalSeriesLabel(m->series);
    } else if (!m->metricKey.isEmpty()) {
        if (const MetricDescriptor *d = m_cat.descriptor(m->metricKey)) {
            what = d->description;
            out.insert(QStringLiteral("howToRead"), d->howToRead);
        }
    }
    out.insert(QStringLiteral("what"), what);

    // The direction sentence. This is the correctness mechanism, not decoration — see
    // Measure::highMeans and docs/design/pinpoint_sign_conventions.md.
    out.insert(QStringLiteral("highMeans"), m->highMeans);

    // ── Norms by context, in tree order ──────────────────────────────────────
    //
    // EVERY context that resolves to something, not only those with their own row. A reader has to
    // be able to see that "driver" is graded at all, and by which corridor; a list showing only
    // authored rows would leave nine of the thirteen contexts looking ungraded when they are not.
    const ContextTree &tree = m_norms->contexts();
    QVariantList       norms;
    for (const QString &cid : tree.inOrder()) {
        QVariantMap row = normAt(measureId, cid);
        if (!row.value(QStringLiteral("found")).toBool()) continue;

        const ContextNode *cn = tree.node(cid);
        row.insert(QStringLiteral("askedContextId"),    cid);
        row.insert(QStringLiteral("askedContextLabel"), cn ? cn->label : cid);
        row.insert(QStringLiteral("depth"),             tree.depth(cid));
        norms.append(row);
    }
    out.insert(QStringLiteral("norms"),    norms);
    out.insert(QStringLiteral("hasNorm"),  !norms.isEmpty());
    out.insert(QStringLiteral("ownNormCount"),
               int(m_norms->overriddenContextsFor(measureId).size()));

    int editedRows = 0;
    for (const QString &cid : m_norms->overriddenContextsFor(measureId))
        if (m_norms->isOverridden(measureId, cid)) ++editedRows;
    out.insert(QStringLiteral("editedNormCount"), editedRows);

    // ── Used by ──────────────────────────────────────────────────────────────
    QVariantList usedBy;
    for (const QString &cid : usersOfMeasureIds(measureId)) {
        const Condition *c = p.condition(cid);
        if (!c) continue;
        QVariantMap r;
        r.insert(QStringLiteral("id"),         c->id);
        r.insert(QStringLiteral("label"),      c->label);
        r.insert(QStringLiteral("groupLabel"), conditionGroupLabel(c->group));
        usedBy.append(r);
    }
    out.insert(QStringLiteral("usedBy"), usedBy);

    // ── Availability ─────────────────────────────────────────────────────────
    //
    // The two failure modes read differently and must never be merged: a corridor signal with no
    // norm cannot fire even though everything else about it works, and a NotCapturable measure
    // cannot be given one at all.
    QString availability;
    if (m->status == MeasureStatus::NotCapturable) {
        availability = m->gapReason.isEmpty()
                           ? tr("No sensor this product has can resolve this measure.")
                           : m->gapReason;
    } else if (m->status == MeasureStatus::ExternalDevice) {
        // Distinct from both neighbours: a corridor here IS worth authoring — it is what the reading
        // will be graded against — but no value arrives until the golfer has the hardware AND the
        // integration lands. Saying "roadmap work" would imply we can close it on our own.
        availability = m->gapReason.isEmpty()
                           ? tr("Read from a launch monitor. Values arrive once one is connected "
                                "and the integration lands; a corridor authored now is what they "
                                "will be graded against.")
                           : m->gapReason;
    } else if (m->status == MeasureStatus::NoProducer) {
        availability = tr("Nothing produces a value for this yet — it is roadmap work.");
    } else if (m->status == MeasureStatus::Planned) {
        availability = tr("A producer is planned. The measure resolves unavailable until it lands.");
    } else if (norms.isEmpty()) {
        availability = tr("Values are produced, but no norm judges them — any signal on this "
                          "measure reports unavailable rather than firing.");
    }
    out.insert(QStringLiteral("availability"), availability);

    return out;
}

// ── The metric -> measure join ──────────────────────────────────────────────

QVariantMap NormModel::measureForMetricAtPhase(const QString &metricKey, int phase) const
{
    QVariantMap out;

    // Phase has no Count sentinel and its enumerators are not contiguous, so an int from QML is
    // round-tripped through the token vocabulary rather than range-checked. A value that is not a
    // real enumerator must report not-found, never cast into one.
    const Phase p = static_cast<Phase>(phase);
    Phase       back{};
    if (!phaseFromToken(phaseToken(p), back) || back != p) {
        out.insert(QStringLiteral("found"), false);
        return out;
    }

    const Measure *m = pinpoint::analysis::measureForMetricAtPhase(m_pack->pack(), metricKey, p);

    out.insert(QStringLiteral("found"), m != nullptr);
    if (!m)
        return out;

    out.insert(QStringLiteral("measureId"),        m->id);
    out.insert(QStringLiteral("label"),            m->label.isEmpty() ? m->id : m->label);
    out.insert(QStringLiteral("unit"),             unitOf(*m));
    out.insert(QStringLiteral("reducerKind"),      reducerKindName(m->reducer.kind));
    out.insert(QStringLiteral("deltaFromAddress"), measureIsDeltaFromAddress(*m));
    return out;
}
