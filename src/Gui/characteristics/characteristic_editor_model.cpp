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

#include "characteristic_editor_model.h"

#include <QFile>

#include <algorithm>

using namespace pinpoint::analysis;

namespace {

QVariantMap okResult(const QString &message = QString())
{
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("message"), message);
    return r;
}

QVariantMap failResult(const QString &message)
{
    QVariantMap r;
    r.insert(QStringLiteral("ok"), false);
    r.insert(QStringLiteral("message"), message);
    return r;
}

// Read one facet map from QML into a Series + Reducer. Unknown tokens leave the facet unset, which
// previewMeasure reports rather than silently defaulting — a wrong facet is a wrong measure.
bool readFacets(const QVariantMap &f, Series &series, Reducer &reducer, QString &whyNot)
{
    if (!roleFromName(f.value(QStringLiteral("what")).toString(), series.what)) {
        whyNot = QStringLiteral("Pick what is being measured.");
        return false;
    }
    if (!quantityFromName(f.value(QStringLiteral("quantity")).toString(), series.quantity)) {
        whyNot = QStringLiteral("Pick a quantity.");
        return false;
    }
    if (!roleFromName(f.value(QStringLiteral("reference")).toString(), series.reference)) {
        whyNot = QStringLiteral("Pick what it is measured relative to.");
        return false;
    }

    const auto kind = reducerKindFromName(
        f.value(QStringLiteral("reducerKind"), QStringLiteral("at")).toString());
    if (!kind) {
        whyNot = QStringLiteral("Pick how the value is read.");
        return false;
    }
    reducer.kind = *kind;

    Phase p{};
    if (f.contains(QStringLiteral("anchor"))
        && phaseFromToken(f.value(QStringLiteral("anchor")).toString(), p))
        reducer.anchor = p;
    else
        reducer.anchor.reset();

    Phase a{}, b{};
    if (phaseFromToken(f.value(QStringLiteral("windowStart")).toString(), a)
        && phaseFromToken(f.value(QStringLiteral("windowEnd")).toString(), b))
        reducer.window = { a, b };

    if (const auto sense = extremumSenseFromName(
            f.value(QStringLiteral("sense"), QStringLiteral("max")).toString()))
        reducer.sense = *sense;

    return true;
}

// Phrase -> facet seeding. A keyword map is an acceptable v1 per the brief: the phrase seeds chip
// selections that the author then corrects, so a partial match is useful and a wrong one is cheap.
struct PhraseHint {
    const char *word;
    const char *role;
};

const PhraseHint kRoleHints[] = {
    { "pelvis", "pelvisCentre" },   { "hip", "leadHip" },        { "hips", "hipLine" },
    { "spine", "spine" },           { "back", "spine" },          { "thorax", "thoraxCentre" },
    { "chest", "thoraxCentre" },    { "shoulder", "shoulderLine" }, { "shoulders", "shoulderLine" },
    { "head", "head" },             { "neck", "neck" },           { "elbow", "leadElbow" },
    { "wrist", "leadWrist" },       { "hand", "leadHand" },       { "hands", "leadHand" },
    { "knee", "leadKnee" },         { "ankle", "leadAnkle" },     { "heel", "leadHeel" },
    { "toe", "leadToe" },           { "foot", "leadAnkle" },      { "stance", "stanceLine" },
    { "club", "shaft" },            { "shaft", "shaft" },         { "clubhead", "clubhead" },
    { "face", "clubFace" },         { "ball", "ball" },           { "arm", "leadUpperArm" },
    { "forearm", "leadForearm" },   { "thigh", "leadThigh" },     { "shin", "leadShin" },
};

const PhraseHint kReferenceHints[] = {
    { "ground", "ground" },        { "floor", "ground" },        { "target", "targetLine" },
    { "vertical", "ground" },      { "ball line", "ballLine" },  { "stance", "stanceLine" },
};

struct QuantityHint {
    const char *word;
    Quantity    q;
};

const QuantityHint kQuantityHints[] = {
    { "angle", Quantity::Angle },       { "tilt", Quantity::Angle },
    { "bend", Quantity::Angle },        { "lean", Quantity::Angle },
    { "rotation", Quantity::Rotation }, { "turn", Quantity::Rotation },
    { "height", Quantity::Height },     { "lift", Quantity::Height },
    { "speed", Quantity::Speed },       { "distance", Quantity::Distance },
    { "position", Quantity::Distance }, { "sway", Quantity::Distance },
    { "width", Quantity::Distance },
};

} // namespace

CharacteristicEditorModel::CharacteristicEditorModel(QObject *parent)
    : QObject(parent), m_provider(makeCharacteristicPackProvider())
{
    // Load the user's override file separately from the merged view: saving must write back only
    // the user's own entries, never a flattened copy of the shipped pack.
    QFile f(userPackPath());
    if (f.open(QIODevice::ReadOnly)) {
        PackLoadResult res = loadPack(f.readAll(), userPackPath());
        if (res.loaded) m_userPack = std::move(res.pack);
    }
    if (m_userPack.id.isEmpty()) {
        m_userPack.id            = QStringLiteral("user");
        m_userPack.version       = QStringLiteral("1");
        m_userPack.schemaVersion = kPackSchemaVersion;
    }
}

CharacteristicEditorModel::~CharacteristicEditorModel() = default;

void CharacteristicEditorModel::reload()
{
    m_provider = makeCharacteristicPackProvider();
    emit libraryChanged();
}

void CharacteristicEditorModel::touch()
{
    m_dirty = true;
    emit draftChanged();
}

// ── Vocabulary ──────────────────────────────────────────────────────────────

QVariantList CharacteristicEditorModel::anatomyGroups() const
{
    // Grouped select, never a free text field: 133 keypoints is too many, and the author should be
    // choosing from a curated vocabulary rather than typing a name that may not exist.
    struct Group {
        const char                *label;
        std::vector<AnatomyRole>   roles;
    };
    const std::vector<Group> groups = {
        { "Upper body", { AnatomyRole::Head, AnatomyRole::Neck, AnatomyRole::LeadShoulder,
                          AnatomyRole::TrailShoulder, AnatomyRole::ThoraxCentre, AnatomyRole::LeadElbow,
                          AnatomyRole::TrailElbow, AnatomyRole::LeadWrist, AnatomyRole::TrailWrist,
                          AnatomyRole::LeadHand, AnatomyRole::TrailHand } },
        { "Lower body", { AnatomyRole::PelvisCentre, AnatomyRole::LeadHip, AnatomyRole::TrailHip,
                          AnatomyRole::LeadKnee, AnatomyRole::TrailKnee, AnatomyRole::LeadAnkle,
                          AnatomyRole::TrailAnkle, AnatomyRole::LeadHeel, AnatomyRole::TrailHeel,
                          AnatomyRole::LeadToe, AnatomyRole::TrailToe, AnatomyRole::StanceCentre } },
        { "Lines & segments", { AnatomyRole::ShoulderLine, AnatomyRole::HipLine, AnatomyRole::Spine,
                                AnatomyRole::ThoracicSegment, AnatomyRole::LumbarSegment,
                                AnatomyRole::LeadUpperArm, AnatomyRole::TrailUpperArm,
                                AnatomyRole::LeadForearm, AnatomyRole::TrailForearm,
                                AnatomyRole::LeadThigh, AnatomyRole::TrailThigh, AnatomyRole::LeadShin,
                                AnatomyRole::TrailShin, AnatomyRole::StanceLine } },
        { "Club", { AnatomyRole::Shaft, AnatomyRole::Clubhead, AnatomyRole::ClubButt,
                    AnatomyRole::ClubFace } },
        { "Ball", { AnatomyRole::Ball } },
        { "World", { AnatomyRole::Ground, AnatomyRole::TargetLine, AnatomyRole::BallLine } },
    };

    QVariantList out;
    for (const Group &g : groups) {
        QVariantList roles;
        for (AnatomyRole r : g.roles) {
            QVariantMap m;
            m.insert(QStringLiteral("name"), roleName(r));
            m.insert(QStringLiteral("label"), roleLabel(r));
            m.insert(QStringLiteral("isDatum"), roleClass(r) == RoleClass::Datum);
            // Flag the roles no sensor can resolve, so the author knows before they build on one.
            m.insert(QStringLiteral("noSensor"), roleNeedsNonPoseSensor(r));
            roles.append(m);
        }
        QVariantMap gm;
        gm.insert(QStringLiteral("label"), QString::fromLatin1(g.label));
        gm.insert(QStringLiteral("roles"), roles);
        out.append(gm);
    }
    return out;
}

QVariantList CharacteristicEditorModel::phases() const
{
    QVariantList out;
    for (Phase p : { Phase::Address, Phase::ShaftParallelBack, Phase::MidBackswing, Phase::Top,
                     Phase::ArmParallelDown, Phase::Delivery, Phase::Impact,
                     Phase::ShaftParallelThrough, Phase::FollowThrough }) {
        QVariantMap m;
        m.insert(QStringLiteral("token"), phaseToken(p));
        m.insert(QStringLiteral("label"), phaseLabel(p));
        out.append(m);
    }
    return out;
}

QVariantList CharacteristicEditorModel::reducerKinds() const
{
    struct Row {
        ReducerKind kind;
        const char *label;
        const char *hint;
    };
    const Row rows[] = {
        { ReducerKind::At,       "At a position",   "the value at one P-position" },
        { ReducerKind::Delta,    "Change between",  "how much it changed between two positions" },
        { ReducerKind::Rate,     "Rate between",    "how fast it changed between two positions" },
        { ReducerKind::Extremum, "Peak across",     "the largest value across a span — use this when "
                                                    "a golfer could move and recover before the "
                                                    "position is sampled" },
    };
    QVariantList out;
    for (const Row &r : rows) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), reducerKindName(r.kind));
        m.insert(QStringLiteral("label"), QString::fromLatin1(r.label));
        m.insert(QStringLiteral("hint"), QString::fromLatin1(r.hint));
        m.insert(QStringLiteral("usesWindow"), reducerUsesWindow(r.kind));
        out.append(m);
    }
    return out;
}

QVariantList CharacteristicEditorModel::conditionGroups() const
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

// ── Lifecycle ───────────────────────────────────────────────────────────────

bool CharacteristicEditorModel::beginEdit(const QString &conditionId)
{
    const CharacteristicPack &merged = m_provider->pack();
    const Condition          *c      = merged.condition(conditionId);
    if (!c) return false;

    m_draft = *c;
    m_draftSignals.clear();
    m_draftMeasures.clear();
    m_draftEdges.clear();

    for (const QString &sid : c->detectedBy) {
        const Signal *s = merged.signal(sid);
        if (!s) continue;
        m_draftSignals.push_back(*s);
        for (const QString &mid : s->measures)
            if (const Measure *m = merged.measure(mid)) m_draftMeasures.push_back(*m);
    }
    for (const Edge &e : merged.edges)
        if (e.type == EdgeType::Causes && e.to == conditionId) m_draftEdges.push_back(e);

    m_editing = true;
    m_isNew   = false;
    m_dirty   = false;
    // Already overriding, or about to: either way the shipped pack is untouched.
    m_overridesCore = (m_userPack.condition(conditionId) == nullptr);

    emit draftChanged();
    return true;
}

void CharacteristicEditorModel::beginNew()
{
    m_draft = Condition{};
    m_draft.state         = ConditionState::Draft;
    m_draft.observability = Observability::Observable;
    m_draft.confirmedBy   = ConfirmedBy::Measured;
    m_draftSignals.clear();
    m_draftMeasures.clear();
    m_draftEdges.clear();

    m_editing       = true;
    m_isNew         = true;
    m_dirty         = false;
    m_overridesCore = false;
    emit draftChanged();
}

void CharacteristicEditorModel::discard()
{
    m_editing = false;
    m_dirty   = false;
    m_draft   = Condition{};
    m_draftSignals.clear();
    m_draftMeasures.clear();
    m_draftEdges.clear();
    emit draftChanged();
}

QVariantMap CharacteristicEditorModel::save()
{
    if (!m_editing) return failResult(tr("Nothing is being edited."));
    if (m_draft.label.trimmed().isEmpty()) return failResult(tr("Give it a name first."));

    if (m_draft.id.isEmpty()) m_draft.id = mintConditionId(m_draft.label);

    // A new characteristic has no id until this moment, so any cause added while drafting carries
    // an empty effect. Stamp the whole edge set now rather than trusting each addCause to have
    // known an id that did not yet exist.
    for (Edge &e : m_draftEdges) e.to = m_draft.id;

    m_draft.detectedBy.clear();
    for (const Signal &s : m_draftSignals) m_draft.detectedBy << s.id;

    // Upsert into the user pack, preserving order so the library does not reshuffle on every save.
    auto upsert = [](auto &vec, const auto &item) {
        for (auto &existing : vec)
            if (existing.id == item.id) { existing = item; return; }
        vec.push_back(item);
    };
    for (const Measure &m : m_draftMeasures) upsert(m_userPack.measures, m);
    for (const Signal &s : m_draftSignals)   upsert(m_userPack.signalDefs, s);
    upsert(m_userPack.conditions, m_draft);

    // Replace this condition's whole incoming edge set — otherwise removing a cause in the editor
    // could never take effect, because the previous edge would survive alongside the new list.
    m_userPack.edges.erase(std::remove_if(m_userPack.edges.begin(), m_userPack.edges.end(),
                                          [&](const Edge &e) { return e.to == m_draft.id; }),
                           m_userPack.edges.end());
    for (const Edge &e : m_draftEdges) m_userPack.edges.push_back(e);

    QString whyNot;
    if (!saveUserPack(m_userPack, &whyNot))
        return failResult(whyNot.isEmpty() ? tr("Could not save.") : whyNot);

    m_dirty   = false;
    m_isNew   = false;
    m_editing = false;
    reload();
    emit draftChanged();
    return okResult(tr("Saved."));
}

QVariantMap CharacteristicEditorModel::revertToShipped()
{
    if (m_draft.id.isEmpty()) return failResult(tr("Nothing to revert."));

    const QString id = m_draft.id;
    auto dropById = [&id](auto &vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&id](const auto &e) { return e.id == id; }),
                  vec.end());
    };
    dropById(m_userPack.conditions);
    m_userPack.edges.erase(std::remove_if(m_userPack.edges.begin(), m_userPack.edges.end(),
                                          [&id](const Edge &e) { return e.to == id; }),
                           m_userPack.edges.end());

    QString whyNot;
    if (!saveUserPack(m_userPack, &whyNot))
        return failResult(whyNot.isEmpty() ? tr("Could not save.") : whyNot);

    m_editing = false;
    m_dirty   = false;
    reload();
    emit draftChanged();
    return okResult(tr("Restored the shipped definition."));
}

// ── Field edits ─────────────────────────────────────────────────────────────

void CharacteristicEditorModel::setLabel(const QString &v)
{
    if (m_draft.label == v) return;
    m_draft.label = v;
    touch();
}

void CharacteristicEditorModel::setGroup(const QString &groupName)
{
    ConditionGroup g{};
    if (!conditionGroupFromName(groupName, g) || g == m_draft.group) return;
    m_draft.group = g;
    touch();
}

void CharacteristicEditorModel::setConsequence(const QString &v)
{
    if (m_draft.consequence.text() == v) return;
    m_draft.consequence.byLocale.insert(QStringLiteral("en"), v);
    touch();
}

void CharacteristicEditorModel::setInjuryNote(const QString &v)
{
    if (m_draft.injuryNote.text() == v) return;
    m_draft.injuryNote.byLocale.insert(QStringLiteral("en"), v);
    touch();
}

void CharacteristicEditorModel::setCitation(const QString &v)
{
    if (m_draft.provenance.citation == v) return;
    m_draft.provenance.citation = v;
    // A tier is only as good as its citation: clearing the citation demotes the claim rather than
    // leaving a tier the content no longer supports.
    m_draft.provenance.tier =
        v.trimmed().isEmpty() ? ProvenanceTier::Proposed : ProvenanceTier::Supported;
    touch();
}

void CharacteristicEditorModel::setState(const QString &stateName)
{
    ConditionState s{};
    if (!conditionStateFromName(stateName, s) || s == m_draft.state) return;
    m_draft.state = s;
    touch();
}

// ── Signals ─────────────────────────────────────────────────────────────────

QString CharacteristicEditorModel::attachMeasure(const QString &measureId, const QString &direction)
{
    Direction d{};
    if (!directionFromName(direction, d)) return QString();

    Signal s;
    s.id        = QStringLiteral("sig_%1_%2").arg(measureId, direction);
    s.test      = SignalTest::OutsideCorridor;   // authors no numbers; inherits the corridor
    s.measures  = { measureId };
    s.direction = d;

    for (const Signal &existing : m_draftSignals)
        if (existing.id == s.id) return s.id;

    m_draftSignals.push_back(s);

    // Carry the measure into the draft so a save writes it alongside — a signal on a measure the
    // user pack does not contain would dangle after an app update.
    const CharacteristicPack &merged = m_provider->pack();
    if (const Measure *m = merged.measure(measureId)) {
        const bool have = std::any_of(m_draftMeasures.begin(), m_draftMeasures.end(),
                                      [&](const Measure &x) { return x.id == measureId; });
        if (!have) m_draftMeasures.push_back(*m);
    }

    touch();
    return s.id;
}

void CharacteristicEditorModel::detachSignal(const QString &signalId)
{
    const auto before = m_draftSignals.size();
    m_draftSignals.erase(std::remove_if(m_draftSignals.begin(), m_draftSignals.end(),
                                        [&](const Signal &s) { return s.id == signalId; }),
                         m_draftSignals.end());
    if (m_draftSignals.size() != before) touch();
}

// ── Causes ──────────────────────────────────────────────────────────────────

void CharacteristicEditorModel::addCause(const QString &causeId, const QString &strength)
{
    Strength st{};
    if (!strengthFromName(strength, st)) st = Strength::Moderate;

    for (Edge &e : m_draftEdges)
        if (e.from == causeId) { e.strength = st; touch(); return; }

    Edge e;
    e.from     = causeId;      // cause
    e.to       = m_draft.id;   // effect — set properly on save for a new condition
    e.type     = EdgeType::Causes;
    e.strength = st;
    m_draftEdges.push_back(e);
    touch();
}

void CharacteristicEditorModel::removeCause(const QString &causeId)
{
    const auto before = m_draftEdges.size();
    m_draftEdges.erase(std::remove_if(m_draftEdges.begin(), m_draftEdges.end(),
                                      [&](const Edge &e) { return e.from == causeId; }),
                       m_draftEdges.end());
    if (m_draftEdges.size() != before) touch();
}

QVariantList CharacteristicEditorModel::candidateCauses(const QString &search) const
{
    const CharacteristicPack &p = m_provider->pack();
    const QString             q = search.trimmed().toLower();

    QVariantList out;
    for (const Condition &c : p.conditions) {
        if (c.id == m_draft.id) continue;   // nothing causes itself
        if (!q.isEmpty() && !c.label.toLower().contains(q) && !c.id.toLower().contains(q)) continue;

        QVariantMap m;
        m.insert(QStringLiteral("id"), c.id);
        m.insert(QStringLiteral("label"), c.label);
        m.insert(QStringLiteral("reach"), confirmedByName(c.confirmedBy));
        m.insert(QStringLiteral("reachLabel"), reachLabel(c.confirmedBy));
        m.insert(QStringLiteral("explains"), coverageOf(p, c.id));
        m.insert(QStringLiteral("selected"),
                 std::any_of(m_draftEdges.begin(), m_draftEdges.end(),
                             [&](const Edge &e) { return e.from == c.id; }));
        out.append(m);
    }
    // Causes that already explain a lot rank first — reuse is the defence against a graph where
    // every characteristic invents its own private cause.
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("explains")).toInt()
             > b.toMap().value(QStringLiteral("explains")).toInt();
    });
    return out;
}

// ── The measure picker ──────────────────────────────────────────────────────

QVariantMap CharacteristicEditorModel::seedFacetsFromPhrase(const QString &phrase) const
{
    const QString p = phrase.toLower();
    QVariantMap   out;

    // Longest match wins, so "shoulder line" beats "shoulder".
    int bestLen = 0;
    for (const PhraseHint &h : kRoleHints) {
        const int len = int(qstrlen(h.word));
        if (len > bestLen && p.contains(QLatin1String(h.word))) {
            out.insert(QStringLiteral("what"), QString::fromLatin1(h.role));
            bestLen = len;
        }
    }
    bestLen = 0;
    for (const PhraseHint &h : kReferenceHints) {
        const int len = int(qstrlen(h.word));
        if (len > bestLen && p.contains(QLatin1String(h.word))) {
            out.insert(QStringLiteral("reference"), QString::fromLatin1(h.role));
            bestLen = len;
        }
    }
    for (const QuantityHint &h : kQuantityHints)
        if (p.contains(QLatin1String(h.word))) {
            out.insert(QStringLiteral("quantity"), quantityName(h.q));
            break;
        }

    // A P-position in the phrase seeds the reducer's anchor.
    for (Phase ph : { Phase::Address, Phase::ShaftParallelBack, Phase::MidBackswing, Phase::Top,
                      Phase::ArmParallelDown, Phase::Delivery, Phase::Impact,
                      Phase::ShaftParallelThrough, Phase::FollowThrough })
        if (p.contains(phaseToken(ph))) {
            out.insert(QStringLiteral("anchor"), phaseToken(ph));
            break;
        }

    return out;
}

QVariantList CharacteristicEditorModel::quantitiesFor(const QString &whatRole) const
{
    AnatomyRole r{};
    if (!roleFromName(whatRole, r)) return {};

    QVariantList out;
    for (Quantity q : legalQuantitiesFor(r)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), quantityName(q));
        m.insert(QStringLiteral("label"), quantityLabel(q));
        out.append(m);
    }
    return out;
}

QVariantList CharacteristicEditorModel::referencesFor(const QString &whatRole,
                                                      const QString &quantity) const
{
    AnatomyRole r{};
    Quantity    q{};
    if (!roleFromName(whatRole, r) || !quantityFromName(quantity, q)) return {};

    QVariantList out;
    for (AnatomyRole ref : legalReferencesFor(r, q)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), roleName(ref));
        m.insert(QStringLiteral("label"), roleLabel(ref));
        m.insert(QStringLiteral("isLine"), roleClass(ref) == RoleClass::Segment
                                               || roleClass(ref) == RoleClass::Datum);
        out.append(m);
    }
    return out;
}

QVariantMap CharacteristicEditorModel::previewMeasure(const QVariantMap &facets) const
{
    QVariantMap out;
    Series      series;
    Reducer     reducer;
    QString     whyNot;

    if (!readFacets(facets, series, reducer, whyNot)) {
        out.insert(QStringLiteral("valid"), false);
        out.insert(QStringLiteral("reason"), whyNot);
        return out;
    }

    const FacetCheck fc = validateSeries(series);
    if (!fc.valid) {
        out.insert(QStringLiteral("valid"), false);
        out.insert(QStringLiteral("reason"), fc.reason);
        return out;
    }
    const ReducerCheck rc = validateReducer(reducer);
    if (!rc.valid) {
        out.insert(QStringLiteral("valid"), false);
        out.insert(QStringLiteral("reason"), rc.reason);
        return out;
    }

    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("label"), canonicalMeasureLabel(series, reducer));
    out.insert(QStringLiteral("id"), canonicalMeasureId(series, reducer));
    out.insert(QStringLiteral("viewNeeded"), viewNeededName(deriveViewNeeded(series)));
    out.insert(QStringLiteral("unit"), quantityUnitHint(series.quantity));

    // A role no sensor can resolve is a capture gap, not roadmap work, and the author should know
    // before they build a characteristic on it.
    const bool noSensor = seriesNeedsNonPoseSensor(series);
    out.insert(QStringLiteral("status"),
               measureStatusName(noSensor ? MeasureStatus::NotCapturable : MeasureStatus::NoProducer));
    if (noSensor)
        out.insert(QStringLiteral("gapReason"),
                   tr("No keypoint exists between the shoulders and the hips in any pose layout, "
                      "so this cannot be derived from pose."));

    // Reuse first: an exact SERIES match means the curve already exists, and this is another way of
    // sampling it rather than a new measure.
    const CharacteristicPack &p = m_provider->pack();
    std::vector<Series>       existing;
    QHash<QString, QString>   seriesOwner;
    for (const Measure &m : p.measures) {
        if (m.kind != MeasureKind::Composed) continue;
        existing.push_back(m.series);
        seriesOwner.insert(canonicalSeriesId(m.series), m.id);
    }

    const QString wantedMeasureId = canonicalMeasureId(series, reducer);
    for (const Measure &m : p.measures)
        if (m.kind == MeasureKind::Composed && canonicalMeasureId(m.series, m.reducer) == wantedMeasureId) {
            QVariantMap hit;
            hit.insert(QStringLiteral("id"), m.id);
            hit.insert(QStringLiteral("label"), m.label);
            hit.insert(QStringLiteral("usedBy"), 0);
            out.insert(QStringLiteral("exactMatch"), hit);
            break;
        }

    // "Did you mean" — computed on the SERIES tuple, so it fires even when the reducers differ.
    // This is the moment duplicates are cheap to prevent; afterwards nobody merges them.
    QVariantList near;
    for (const SeriesMatch &sm : findSimilarSeries(series, existing)) {
        QVariantMap n;
        n.insert(QStringLiteral("id"), seriesOwner.value(canonicalSeriesId(sm.series)));
        n.insert(QStringLiteral("label"), canonicalSeriesLabel(sm.series));
        n.insert(QStringLiteral("distance"), sm.distance);
        n.insert(QStringLiteral("sameSeries"), sm.distance == 0);
        near.append(n);
    }
    out.insert(QStringLiteral("nearDuplicates"), near);
    return out;
}

QString CharacteristicEditorModel::mintMeasure(const QVariantMap &facets)
{
    Series  series;
    Reducer reducer;
    QString whyNot;
    if (!readFacets(facets, series, reducer, whyNot)) return QString();
    if (!validateSeries(series).valid || !validateReducer(reducer).valid) return QString();

    Measure m;
    m.id         = canonicalMeasureId(series, reducer);
    m.kind       = MeasureKind::Composed;
    m.series     = series;
    m.reducer    = reducer;
    m.label      = canonicalMeasureLabel(series, reducer);
    m.unit       = quantityUnitHint(series.quantity);
    m.viewNeeded = deriveViewNeeded(series);

    if (seriesNeedsNonPoseSensor(series)) {
        m.status    = MeasureStatus::NotCapturable;
        m.gapReason = tr("No keypoint exists between the shoulders and the hips in any pose layout, "
                         "so this cannot be derived from pose.");
    } else {
        m.status = MeasureStatus::NoProducer;
    }

    const bool have = std::any_of(m_draftMeasures.begin(), m_draftMeasures.end(),
                                  [&](const Measure &x) { return x.id == m.id; });
    if (!have) m_draftMeasures.push_back(m);

    touch();
    return m.id;
}

QVariantList CharacteristicEditorModel::existingMeasures(const QString &search) const
{
    const CharacteristicPack &p = m_provider->pack();
    const QString             q = search.trimmed().toLower();

    QVariantList out;
    for (const Measure &m : p.measures) {
        const QString label = m.label.isEmpty() ? canonicalMeasureLabel(m.series, m.reducer) : m.label;
        if (!q.isEmpty() && !label.toLower().contains(q) && !m.id.toLower().contains(q)) continue;

        QVariantMap r;
        r.insert(QStringLiteral("id"), m.id);
        r.insert(QStringLiteral("label"), label);
        r.insert(QStringLiteral("status"), measureStatusName(m.status));
        r.insert(QStringLiteral("kind"), measureKindName(m.kind));
        r.insert(QStringLiteral("metricKey"), m.metricKey);
        out.append(r);
    }
    return out;
}

// ── Ids ─────────────────────────────────────────────────────────────────────

QString CharacteristicEditorModel::mintConditionId(const QString &label) const
{
    // Readable, stable, and never reused. Derived from the label once, at creation — a later
    // rename must NOT change the id, or every edge pointing at it would break.
    QString base;
    for (const QChar &ch : label.toLower())
        base += ch.isLetterOrNumber() ? ch : QLatin1Char('_');
    while (base.contains(QStringLiteral("__"))) base.replace(QStringLiteral("__"), QStringLiteral("_"));
    base = base.mid(0, 48);
    while (base.endsWith(QLatin1Char('_'))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("characteristic");

    const CharacteristicPack &p = m_provider->pack();
    QString                   id = base;
    int                       n  = 2;
    while (p.condition(id) != nullptr) id = QStringLiteral("%1_%2").arg(base).arg(n++);
    return id;
}

// ── Draft shape for QML ─────────────────────────────────────────────────────

QVariantMap CharacteristicEditorModel::draft() const
{
    QVariantMap out;
    if (!m_editing) return out;

    out.insert(QStringLiteral("id"), m_draft.id);
    out.insert(QStringLiteral("label"), m_draft.label);
    out.insert(QStringLiteral("group"), conditionGroupName(m_draft.group));
    out.insert(QStringLiteral("groupLabel"), conditionGroupLabel(m_draft.group));
    out.insert(QStringLiteral("consequence"), m_draft.consequence.text());
    out.insert(QStringLiteral("injuryNote"), m_draft.injuryNote.text());
    out.insert(QStringLiteral("citation"), m_draft.provenance.citation);
    out.insert(QStringLiteral("tier"), provenanceTierName(m_draft.provenance.tier));
    out.insert(QStringLiteral("state"), conditionStateName(m_draft.state));

    const CharacteristicPack &p = m_provider->pack();

    QVariantList sigs;
    for (const Signal &s : m_draftSignals) {
        QVariantMap sm;
        sm.insert(QStringLiteral("id"), s.id);
        sm.insert(QStringLiteral("direction"),
                  s.direction.has_value() ? directionName(*s.direction) : QString());
        sm.insert(QStringLiteral("test"), signalTestName(s.test));

        const QString mid = s.measures.value(0);
        sm.insert(QStringLiteral("measureId"), mid);

        const Measure *m = nullptr;
        for (const Measure &dm : m_draftMeasures)
            if (dm.id == mid) m = &dm;
        if (!m) m = p.measure(mid);

        sm.insert(QStringLiteral("measureLabel"),
                  m ? (m->label.isEmpty() ? canonicalMeasureLabel(m->series, m->reducer) : m->label)
                    : mid);
        sm.insert(QStringLiteral("status"), m ? measureStatusName(m->status) : QString());
        sigs.append(sm);
    }
    out.insert(QStringLiteral("signals"), sigs);

    QVariantList causes;
    for (const Edge &e : m_draftEdges) {
        const Condition *c = p.condition(e.from);
        QVariantMap      cm;
        cm.insert(QStringLiteral("id"), e.from);
        cm.insert(QStringLiteral("label"), c ? c->label : e.from);
        cm.insert(QStringLiteral("strength"), strengthName(e.strength));
        cm.insert(QStringLiteral("strengthLabel"), strengthLabel(e.strength));
        cm.insert(QStringLiteral("reachLabel"), c ? reachLabel(c->confirmedBy) : QString());
        cm.insert(QStringLiteral("offeredOnly"), c && c->confirmedBy == ConfirmedBy::Asserted);
        causes.append(cm);
    }
    out.insert(QStringLiteral("causes"), causes);

    return out;
}
