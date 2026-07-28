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

#include "metric_catalog.h"

#include "../../Diagnostics/metric_corridor.h"

using namespace pinpoint::analysis;

namespace {

// Reverse of segmentRoleName() — a role-name string (as written in swing.json / shotCtx) back to the
// enum. Unknown names map to SegmentRole::Unknown (ignored by hasRole()).
SegmentRole roleFromName(const QString &name)
{
    for (int r = 0; r <= static_cast<int>(SegmentRole::Club); ++r) {
        const auto role = static_cast<SegmentRole>(r);
        if (segmentRoleName(role) == name)
            return role;
    }
    return SegmentRole::Unknown;
}

QString tierName(ReconstructionTier t)
{
    switch (t) {
    case ReconstructionTier::Angles2D:         return QStringLiteral("angles2D");
    case ReconstructionTier::Mono3DPlusImu:    return QStringLiteral("mono3DPlusImu");
    case ReconstructionTier::Stereo3D:         return QStringLiteral("stereo3D");
    case ReconstructionTier::ClubInstrumented: return QStringLiteral("clubInstrumented");
    }
    return QStringLiteral("angles2D");
}

QString stateName(MetricAvailability::State s)
{
    switch (s) {
    case MetricAvailability::Measured:    return QStringLiteral("measured");
    case MetricAvailability::Bridged:     return QStringLiteral("bridged");
    case MetricAvailability::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

// Build a ShotContext from the QML shotCtx map. An empty map yields the context-free default
// (sessionType −1, no sensors) — used by the directory "all" view where availableOnly is off.
ShotContext contextFromMap(const QVariantMap &m)
{
    ShotContext c;
    if (m.contains(QStringLiteral("tier")))
        c.tier = static_cast<ReconstructionTier>(m.value(QStringLiteral("tier")).toInt());
    c.sessionType  = m.value(QStringLiteral("sessionType"), -1).toInt();
    c.hasFaceOn    = m.value(QStringLiteral("hasFaceOn")).toBool();
    c.hasClubTrack = m.value(QStringLiteral("hasClubTrack")).toBool();
    c.hasBallTrack = m.value(QStringLiteral("hasBallTrack")).toBool();
    for (const QVariant &v : m.value(QStringLiteral("imuRoles")).toList()) {
        const SegmentRole r = roleFromName(v.toString());
        if (r != SegmentRole::Unknown)
            c.imuRoles.push_back(r);
    }
    c.band.archetype = m.value(QStringLiteral("archetype"), 0).toInt();
    c.band.club      = m.value(QStringLiteral("club"), 0).toInt();
    c.band.shape     = m.value(QStringLiteral("shape"), 0).toInt();
    return c;
}

QVariantMap availabilityMap(const MetricAvailability &a)
{
    QVariantMap m;
    m.insert(QStringLiteral("state"),  stateName(a.state));
    m.insert(QStringLiteral("reason"), a.reason);
    m.insert(QStringLiteral("tier"),   tierName(a.tier));
    return m;
}

// Compact "what produces this" hints for a directory row glyph, from the requirement.
QVariantList sourceHints(const MetricRequirement &r)
{
    QVariantList s;
    if (!r.imuRoles.empty()) s.append(QStringLiteral("imu"));
    if (r.faceOnCamera)      s.append(QStringLiteral("camera"));
    if (r.clubTrack)         s.append(QStringLiteral("club"));
    if (r.ballTrack)         s.append(QStringLiteral("ball"));
    return s;
}

// The compact directory row (design §7 query shape) + its resolved availability.
QVariantMap rowMap(const MetricDescriptor &d, const MetricAvailability &a)
{
    QVariantMap m;
    m.insert(QStringLiteral("key"),          d.key);
    m.insert(QStringLiteral("label"),        d.label);
    m.insert(QStringLiteral("shortLabel"),   d.shortLabel);
    m.insert(QStringLiteral("unit"),         d.unit);
    m.insert(QStringLiteral("type"),         metricTypeName(d.type));
    m.insert(QStringLiteral("group"),        d.group);
    m.insert(QStringLiteral("scored"),       d.scored);
    m.insert(QStringLiteral("planned"),      d.planned);
    m.insert(QStringLiteral("sources"),      sourceHints(d.requirement));
    m.insert(QStringLiteral("availability"), availabilityMap(a));
    return m;
}

} // namespace

MetricCatalog::MetricCatalog(QObject *parent)
    : QObject(parent)
    , m_catalogue(makeMetricCatalogue())
    , m_pack(makeCharacteristicPackProvider())
    , m_norms(sharedNormProvider())
    , m_policyName(QStringLiteral("standard"))
{
}

MetricCatalog::~MetricCatalog() = default;

void MetricCatalog::setGradePolicy(const QString &name)
{
    // Resolved through the shared table, not stored as handed in — same rule as NormModel: an
    // unknown name must grade against the default AND read as the default.
    const QString resolved = QString::fromLatin1(gradePolicyPresetFor(name).name);
    if (resolved == m_policyName) return;
    m_policyName = resolved;
    emit gradePolicyChanged();
}

QVariantList MetricCatalog::groups() const
{
    QVariantList out;
    for (const MetricDescriptor *d : m_catalogue.all())
        if (!out.contains(d->group))
            out.append(d->group);
    return out;
}

QVariantList MetricCatalog::types() const
{
    return { metricTypeName(MetricType::Summary), metricTypeName(MetricType::PointInTime),
             metricTypeName(MetricType::TimeSeries), metricTypeName(MetricType::Sequence) };
}

QVariantList MetricCatalog::query(const QVariantMap &filters, const QVariantMap &shotCtx) const
{
    MetricQuery q;
    if (filters.contains(QStringLiteral("type")))
        q.type = metricTypeFromName(filters.value(QStringLiteral("type")).toString());
    q.group = filters.value(QStringLiteral("group")).toString();
    if (filters.contains(QStringLiteral("scored")))
        q.scored = filters.value(QStringLiteral("scored")).toBool();
    if (filters.contains(QStringLiteral("sessionType")))
        q.sessionType = filters.value(QStringLiteral("sessionType")).toInt();
    q.availableOnly = filters.value(QStringLiteral("availableOnly")).toBool();

    // Free-text search. Applied HERE rather than in MetricQuery: the catalogue's query is a
    // structural selection over the manifest, and a substring match on display strings is a
    // directory affordance, not part of what the catalogue means. Matched over the fields a
    // row actually SHOWS — a hit the reader cannot see in the result is a hit they cannot
    // trust. Same rule as NormModel::measures().
    const QString search = filters.value(QStringLiteral("search")).toString().trimmed();

    const ShotContext ctx = contextFromMap(shotCtx);
    const bool haveCtx = !shotCtx.isEmpty();

    QVariantList out;
    for (const MetricDescriptor *d : m_catalogue.query(q, haveCtx ? &ctx : nullptr)) {
        if (!search.isEmpty()) {
            const bool hit = d->label.contains(search, Qt::CaseInsensitive)
                             || d->shortLabel.contains(search, Qt::CaseInsensitive)
                             || d->key.contains(search, Qt::CaseInsensitive)
                             || d->group.contains(search, Qt::CaseInsensitive);
            if (!hit) continue;
        }
        const MetricAvailability a =
            haveCtx ? m_catalogue.resolve(d->key, ctx) : MetricAvailability{};
        out.append(rowMap(*d, a));
    }
    return out;
}

QVariantMap MetricCatalog::descriptor(const QString &key, const QVariantMap &shotCtx) const
{
    const MetricDescriptor *d = m_catalogue.descriptor(key);
    if (!d)
        return {};

    const ShotContext ctx = contextFromMap(shotCtx);
    const bool haveCtx = !shotCtx.isEmpty();

    QVariantMap m = rowMap(*d, haveCtx ? m_catalogue.resolve(key, ctx) : MetricAvailability{});
    m.insert(QStringLiteral("description"),  d->description);
    m.insert(QStringLiteral("howToRead"),    d->howToRead);
    m.insert(QStringLiteral("flexPositive"), d->flexPositive);

    QVariantList phases;
    for (Phase p : d->phases)
        phases.append(static_cast<int>(p));
    m.insert(QStringLiteral("phases"), phases);

    // Normative: the corridor for each of the metric's phases, resolved through the NORM SET — the
    // (metric, phase) → measure → norm join, in the shot's own context. The metric descriptor no
    // longer carries corridors of its own; it describes the metric and does not judge it.
    //
    // Provenance travels with them, because the numbers are now content a user can interrogate and
    // edit: which norm, resolved at which context, inherited or not, how well founded, and whether
    // it is the shipped corridor or theirs. That replaced the descriptor's hand-authored
    // `contextNote` — a sentence naming a context ("mid-iron · neutral archetype") is exactly what
    // the context tree now states for real.
    const CharacteristicPack &pack     = m_pack->pack();
    const QString             contextId = ctx.band.resolvedContextId();
    const GradePolicy         policy    = gradePolicyByName(m_policyName);

    QVariantList corridors;
    Shape        railShape = Shape::Target;
    QString      normContextId, normSource, normSourceWords, normCitation, normWeakWhy, normMeasureId;
    bool         anyOverridden = false, anyWeak = false, anyInherited = false;
    int          normN = 0;

    for (Phase p : d->phases) {
        const std::optional<MetricCorridor> c =
            corridorForMetricAtPhase(pack, *m_norms, key, p, contextId, policy);
        if (!c)
            continue;

        QVariantMap cm;
        cm.insert(QStringLiteral("phase"),            static_cast<int>(c->phase));
        cm.insert(QStringLiteral("greenLo"),          c->greenLo);
        cm.insert(QStringLiteral("greenHi"),          c->greenHi);
        cm.insert(QStringLiteral("amberLo"),          c->amberLo);
        cm.insert(QStringLiteral("amberHi"),          c->amberHi);
        cm.insert(QStringLiteral("deltaFromAddress"), c->deltaFromAddress);
        // Which tail does not grade, and the shape it came from. Written explicitly on every
        // corridor including two-sided ones: QML reads a missing key as `undefined`, so an omitted
        // flag is indistinguishable from `false` and the one place a bug could hide is the place
        // nobody looks.
        cm.insert(QStringLiteral("lowOpen"),          c->lowOpen);
        cm.insert(QStringLiteral("highOpen"),         c->highOpen);
        cm.insert(QStringLiteral("shape"),            shapeName(c->shape));
        cm.insert(QStringLiteral("measureId"),        c->measureId);
        cm.insert(QStringLiteral("contextId"),        c->contextId);
        cm.insert(QStringLiteral("inherited"),        c->inherited);
        cm.insert(QStringLiteral("overridden"),       c->overridden);

        // Named, not just keyed — the detail page offers a link through to where this corridor is
        // DEFINED so it can be edited, and a link has to say where it goes. Per corridor rather than
        // per metric, because different phases of one metric are different measures with different
        // norms: bow/cup at the top is a Δ-from-address cell, at impact it is the absolute reading.
        if (const Measure *m = pack.measure(c->measureId))
            cm.insert(QStringLiteral("measureLabel"), m->label.isEmpty() ? m->id : m->label);
        else
            cm.insert(QStringLiteral("measureLabel"), c->measureId);
        if (const ContextNode *cn = m_norms->contexts().node(c->contextId))
            cm.insert(QStringLiteral("contextLabel"), cn->label);
        else
            cm.insert(QStringLiteral("contextLabel"), c->contextId);
        corridors.append(cm);

        // Metric-wide provenance. The named fields describe the FIRST corridor that resolved, since
        // a one-line summary has to pick one; the flags are OR-ed across all of them, so a metric
        // with a single edited or weakly-founded phase still says so. Different phases are different
        // measures and can in principle resolve at different contexts, which is why the per-corridor
        // maps above carry their own `contextId` / `inherited` / `overridden` too.
        if (normContextId.isEmpty()) {
            normContextId = c->contextId;
            normMeasureId = c->measureId;
            if (const Norm *n = m_norms->resolve(c->measureId, contextId).norm) {
                normSource      = normSourceName(n->source);
                normSourceWords = normSourceLabel(n->source);
                normCitation    = n->citation;
                normWeakWhy     = normWeakReason(*n);
                normN           = n->n;
                anyWeak         = normIsWeak(*n);
            }
        }
        anyOverridden = anyOverridden || c->overridden;
        anyInherited  = anyInherited  || c->inherited;

        // A rail is ONE strip, so it needs one answer about its shape. Different phases of a
        // metric are different measures and could in principle disagree — in practice they are the
        // same quantity read at different moments, so they do not. Unanimity is required and a
        // disagreement falls back to `target`: drawing an open tail the norm at one phase does
        // grade would state a freedom that phase does not have, which is the more dangerous of the
        // two errors.
        if (corridors.size() == 1) railShape = c->shape;
        else if (railShape != c->shape) railShape = Shape::Target;
    }

    QVariantMap normative;
    normative.insert(QStringLiteral("corridors"),  corridors);
    normative.insert(QStringLiteral("measureId"),  normMeasureId);
    normative.insert(QStringLiteral("contextId"),  normContextId);
    if (const ContextNode *cn = m_norms->contexts().node(normContextId))
        normative.insert(QStringLiteral("contextLabel"), cn->label);
    else
        normative.insert(QStringLiteral("contextLabel"), normContextId);
    normative.insert(QStringLiteral("inherited"),  anyInherited);
    normative.insert(QStringLiteral("overridden"), anyOverridden);
    normative.insert(QStringLiteral("source"),      normSource);
    normative.insert(QStringLiteral("sourceLabel"), normSourceWords);
    normative.insert(QStringLiteral("citation"),    normCitation);
    normative.insert(QStringLiteral("n"),          normN);
    normative.insert(QStringLiteral("weak"),       anyWeak);
    normative.insert(QStringLiteral("weakReason"), normWeakWhy);
    // The metric-level answer a rail binds to. THE ONLY SOURCE of one-sidedness for any surface:
    // `PpDashboardMotionZone` used to decide this by string-matching the unit, which is a
    // presentation-layer heuristic standing in for a semantic property of the measure. Anything
    // re-deriving it from a unit, a metric key or a label is a bug.
    normative.insert(QStringLiteral("shape"),      shapeName(railShape));
    normative.insert(QStringLiteral("oneSided"),   shapeIsOneSided(railShape));
    m.insert(QStringLiteral("normative"), normative);

    // Requirement (rendered for the "How it's measured" section).
    QVariantList roles;
    for (SegmentRole r : d->requirement.imuRoles)
        roles.append(segmentRoleName(r));
    QVariantMap req;
    req.insert(QStringLiteral("faceOnCamera"), d->requirement.faceOnCamera);
    req.insert(QStringLiteral("imuRoles"),     roles);
    req.insert(QStringLiteral("clubTrack"),    d->requirement.clubTrack);
    req.insert(QStringLiteral("ballTrack"),    d->requirement.ballTrack);
    req.insert(QStringLiteral("minTier"),      tierName(d->requirement.minTier));
    m.insert(QStringLiteral("requires"), req);

    QVariantList usedBy;
    for (const QString &u : d->usedBy)
        usedBy.append(u);
    m.insert(QStringLiteral("usedBy"), usedBy);

    return m;
}

QVariantMap MetricCatalog::availability(const QString &key, const QVariantMap &shotCtx) const
{
    return availabilityMap(m_catalogue.resolve(key, contextFromMap(shotCtx)));
}
