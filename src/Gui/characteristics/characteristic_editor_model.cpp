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
    : QObject(parent), m_provider(makeCharacteristicPackProvider()),
      m_core(makeResourcePackProvider()), m_norms(sharedNormProvider())
{
    // Load the user's override file separately from the merged view: saving must write back only
    // the user's own entries, never a flattened copy of the shipped pack.
    //
    // `parsed`, NOT `loaded` — the distinction is documented on PackLoadResult and getting it wrong
    // here loses data. An overlay pack routinely fails STANDALONE referential validation, because
    // its edges point at core conditions it does not itself contain; referential checks are only
    // meaningful on the assembled library. Keying off `loaded` therefore started the editor with an
    // EMPTY user pack whenever the user had an ordinary override on disk — and since save() upserts
    // into m_userPack and writes the whole thing back, the next save silently erased every other
    // override they had ever made.
    QFile f(userPackPath());
    if (f.open(QIODevice::ReadOnly)) {
        PackLoadResult res = loadPack(f.readAll(), userPackPath());
        if (res.parsed) m_userPack = std::move(res.pack);
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

QVariantList CharacteristicEditorModel::contexts() const
{
    const ContextTree &tree = m_norms->contexts();

    QVariantList out;
    for (const QString &id : tree.inOrder()) {
        const ContextNode *n = tree.node(id);
        if (!n) continue;

        const BindingResolution br = resolveContextBinding(m_draft, tree, id);
        const bool              own = ownContextBinding(m_draft, id) != nullptr;

        QVariantMap r;
        r.insert(QStringLiteral("id"),         n->id);
        r.insert(QStringLiteral("label"),      n->label);
        r.insert(QStringLiteral("parentId"),   n->parentId);
        r.insert(QStringLiteral("depth"),      tree.depth(id));
        r.insert(QStringLiteral("isDefault"),  n->id == kDefaultContextId());
        r.insert(QStringLiteral("applicable"), br.applicable);
        r.insert(QStringLiteral("material"),   br.material);
        r.insert(QStringLiteral("own"),        own);
        r.insert(QStringLiteral("inherited"),  br.found && br.inherited);

        // Name the ancestor a row was inherited FROM, not merely that it was. "Off, inherited" is
        // an invitation to hunt for the row that says so; "Off, from Partial swing" is not.
        QString fromLabel;
        if (br.found && br.inherited)
            if (const ContextNode *src = tree.node(br.contextId)) fromLabel = src->label;
        r.insert(QStringLiteral("inheritedFrom"),      br.inherited ? br.contextId : QString());
        r.insert(QStringLiteral("inheritedFromLabel"), fromLabel);
        out.append(r);
    }
    return out;
}

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
    m_bindingUndoValid = false;   // an undo from a previous characteristic is not an undo
    m_retiredSignalIds.clear();

    // Two INDEPENDENT facts, and the old single flag conflated them. "Does core ship this id?"
    // decides whether dropping the user's row restores something or destroys it; "is there a user
    // row?" decides whether there is anything to drop at all. A user's own characteristic has the
    // second and not the first, which is exactly the case that used to be deleted behind a message
    // saying it had been restored.
    m_shippedExists   = m_core && m_core->pack().condition(conditionId) != nullptr;
    m_hasUserOverride = m_userPack.condition(conditionId) != nullptr;

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

    m_editing          = true;
    m_isNew            = true;
    m_dirty            = false;
    m_shippedExists    = false;
    m_hasUserOverride  = false;
    m_bindingUndoValid = false;
    m_retiredSignalIds.clear();
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
    m_bindingUndoValid = false;
    m_retiredSignalIds.clear();
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

    // A flipped tail re-mints the signal id, so the old row is now dead weight — drop it, but only
    // once nothing in the user pack still points at it.
    for (const QString &retired : m_retiredSignalIds) {
        bool referenced = false;
        for (const Condition &c : m_userPack.conditions)
            if (c.detectedBy.contains(retired)) referenced = true;
        if (referenced) continue;
        m_userPack.signalDefs.erase(
            std::remove_if(m_userPack.signalDefs.begin(), m_userPack.signalDefs.end(),
                           [&retired](const Signal &s) { return s.id == retired; }),
            m_userPack.signalDefs.end());
    }
    m_retiredSignalIds.clear();

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
    if (!m_hasUserOverride)
        return failResult(tr("This is the shipped definition — there is nothing of yours to drop."));

    // Read before the drop: afterwards there is no way to tell which of the two things happened,
    // and the message has to say the true one.
    const bool    restores = m_shippedExists;
    const QString id       = m_draft.id;
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
    return okResult(restores
                        ? tr("Restored the shipped definition.")
                        : tr("Deleted. This characteristic was yours — nothing ships under that "
                             "name to fall back to."));
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

// ── Where it applies ────────────────────────────────────────────────────────

QVariantMap CharacteristicEditorModel::setBinding(const QString &contextId, bool applicable,
                                                  bool material)
{
    if (!m_editing) return failResult(tr("Nothing is being edited."));

    const ContextTree &tree = m_norms->contexts();
    const ContextNode *node = tree.node(contextId);
    // An id the tree does not carry is refused rather than written: a binding on a context nobody
    // can resolve is a row that will never be read and can never be found again to remove.
    if (!node) return failResult(tr("That is not a context this norm set knows about."));

    m_bindingUndo      = m_draft.bindings;
    m_bindingUndoValid = true;

    bool changed = false;
    auto upsert  = [&]() {
        for (ContextBinding &b : m_draft.bindings)
            if (b.context == contextId) {
                changed = (b.applicable != applicable) || (b.material != material);
                b.applicable = applicable;
                b.material   = material;
                return;
            }
        ContextBinding b;
        b.context    = contextId;
        b.applicable = applicable;
        b.material   = material;
        m_draft.bindings.push_back(std::move(b));
        changed = true;
    };
    upsert();

    // THE CASCADE. Switching a parent off has to clear any descendant row that says the opposite,
    // or the untick silently does not take: resolution stops at the nearest row, so an explicit
    // "applies" at `wedge` would survive an "does not apply" at `full_swing` and the author would
    // see a box they had just cleared still behaving as though it were ticked. Rows that already
    // agree are left alone — they are the author's own words about a context they thought about.
    int cascaded = 0;
    if (!applicable) {
        auto it = std::remove_if(m_draft.bindings.begin(), m_draft.bindings.end(),
                                 [&](const ContextBinding &b) {
                                     return b.applicable && b.context != contextId
                                            && tree.isDescendantOf(b.context, contextId);
                                 });
        cascaded = int(std::distance(it, m_draft.bindings.end()));
        m_draft.bindings.erase(it, m_draft.bindings.end());
    }

    if (!changed && cascaded == 0) {
        m_bindingUndoValid = false;
        return okResult();
    }

    QString message = !applicable
                          ? tr("Does not apply to %1.").arg(node->label)
                      : !material
                          ? tr("Applies to %1, but does not count when ranking.").arg(node->label)
                          : tr("Applies to %1.").arg(node->label);
    if (cascaded > 0)
        message += QLatin1Char(' ')
                   + tr("%n exception(s) beneath it cleared.", "", cascaded);

    touch();

    QVariantMap r = okResult(message);
    r.insert(QStringLiteral("cascaded"), cascaded);
    r.insert(QStringLiteral("canUndo"), true);
    return r;
}

QVariantMap CharacteristicEditorModel::clearBinding(const QString &contextId)
{
    if (!m_editing) return failResult(tr("Nothing is being edited."));
    if (ownContextBinding(m_draft, contextId) == nullptr)
        return failResult(tr("Nothing is set here — it already inherits."));

    m_bindingUndo      = m_draft.bindings;
    m_bindingUndoValid = true;

    m_draft.bindings.erase(std::remove_if(m_draft.bindings.begin(), m_draft.bindings.end(),
                                          [&](const ContextBinding &b) { return b.context == contextId; }),
                           m_draft.bindings.end());
    touch();

    // Say what it inherits NOW, resolved after the removal — the answer is the point of the action.
    const ContextTree      &tree = m_norms->contexts();
    const ContextNode      *node = tree.node(contextId);
    const BindingResolution br   = resolveContextBinding(m_draft, tree, contextId);

    QString fromLabel;
    if (br.found)
        if (const ContextNode *src = tree.node(br.contextId)) fromLabel = src->label;

    const QString label = node ? node->label : contextId;
    QVariantMap   r     = okResult(fromLabel.isEmpty()
                                       ? tr("%1 applies again, like everywhere else.").arg(label)
                                       : tr("%1 follows %2 again.").arg(label, fromLabel));
    r.insert(QStringLiteral("cascaded"), 0);
    r.insert(QStringLiteral("canUndo"), true);
    return r;
}

bool CharacteristicEditorModel::undoBindingChange()
{
    if (!m_editing || !m_bindingUndoValid) return false;
    m_draft.bindings   = m_bindingUndo;
    m_bindingUndoValid = false;
    m_bindingUndo.clear();
    touch();
    return true;
}

// ── Signals ─────────────────────────────────────────────────────────────────

QVariantList CharacteristicEditorModel::directionOptions(const QString &highMeans) const
{
    // The phrasing rule lives in the pack layer (directionPhrase, characteristic.h) because the
    // read-only detail page has to say exactly the same words as the control that sets it.
    QVariantList out;
    for (Direction d : { Direction::High, Direction::Low }) {
        const DirectionPhrase p = directionPhrase(d, highMeans);
        QVariantMap           m;
        m.insert(QStringLiteral("name"), directionName(d));
        m.insert(QStringLiteral("label"), p.label);
        m.insert(QStringLiteral("means"), p.means);
        m.insert(QStringLiteral("sentence"), p.sentence);
        out.append(m);
    }
    return out;
}

QVariantMap CharacteristicEditorModel::setSignalDirection(const QString &signalId,
                                                          const QString &direction)
{
    if (!m_editing) return failResult(tr("Nothing is being edited."));

    Direction d{};
    if (!directionFromName(direction, d)) return failResult(tr("That is not a direction."));

    Signal *sig = nullptr;
    for (Signal &s : m_draftSignals)
        if (s.id == signalId) sig = &s;
    if (!sig) return failResult(tr("That signal is not on this characteristic."));

    if (sig->direction.has_value() && *sig->direction == d) return okResult();

    const QString measureId = sig->measures.value(0);

    // The id is an OPAQUE stable key — the shipped pack proves it, since `sig_ballForward` names
    // the tail rather than deriving from it. But an id WE minted spells the direction out
    // (`sig_<measure>_<direction>`), and leaving that spelling behind after a flip would leave the
    // file saying the opposite of what the row does. So re-mint exactly that case, and only when
    // nothing else is pointing at the old id.
    const QString mintedOld = QStringLiteral("sig_%1_%2")
                                  .arg(measureId, directionName(sig->direction.value_or(Direction::High)));
    const QString mintedNew = QStringLiteral("sig_%1_%2").arg(measureId, directionName(d));

    bool shared = false;
    for (const Condition &oc : m_provider->pack().conditions)
        if (oc.id != m_draft.id && oc.detectedBy.contains(signalId)) shared = true;

    if (signalId == mintedOld && !shared && mintedNew != signalId) {
        // Refuse rather than merge: two signals with the same id would collapse, and a
        // characteristic carrying BOTH tails of one measure fires either way round, which is the
        // one shape the picker's own guidance tells an author not to build.
        for (const Signal &s : m_draftSignals)
            if (s.id == mintedNew)
                return failResult(tr("This characteristic already flags the other side of that "
                                     "measure. Remove that one first."));
        m_retiredSignalIds << signalId;
        sig->id = mintedNew;
    }

    sig->direction = d;
    touch();

    const DirectionPhrase p = directionPhrase(d, measureHighMeans(measureId));
    return okResult(p.sentence);
}

void CharacteristicEditorModel::setMeasureHighMeans(const QString &measureId, const QString &text)
{
    if (!m_editing || measureId.isEmpty()) return;

    const QString v = text.trimmed();
    for (Measure &m : m_draftMeasures)
        if (m.id == measureId) {
            if (m.highMeans == v) return;
            m.highMeans = v;
            touch();
            return;
        }

    // Not in the draft yet: take the library's copy, so a save writes the whole measure and not a
    // fragment. Editing a shared measure is exactly what this is, and the signal row says so.
    const CharacteristicPack &merged = m_provider->pack();
    if (const Measure *src = merged.measure(measureId)) {
        Measure copy = *src;
        if (copy.highMeans == v) return;
        copy.highMeans = v;
        m_draftMeasures.push_back(std::move(copy));
        touch();
    }
}

QString CharacteristicEditorModel::measureHighMeans(const QString &measureId) const
{
    for (const Measure &m : m_draftMeasures)
        if (m.id == measureId) return m.highMeans;
    if (const Measure *m = m_provider->pack().measure(measureId)) return m->highMeans;
    return QString();
}

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

QVariantMap CharacteristicEditorModel::linkCause(const QString &causeId, const QString &effectId,
                                                 const QString &strength)
{
    if (m_editing)
        return failResult(tr("Finish or discard the open edit first."));

    const CharacteristicPack &p = m_provider->pack();
    const Condition          *cause  = p.condition(causeId);
    const Condition          *effect = p.condition(effectId);
    if (!cause || !effect)  return failResult(tr("That is not in the library."));
    if (causeId == effectId) return failResult(tr("Nothing causes itself."));

    for (const Edge &e : p.edges)
        if (e.type == EdgeType::Causes && e.from == causeId && e.to == effectId)
            return failResult(tr("%1 already causes %2.").arg(cause->label, effect->label));

    // A causal path the other way already exists, so this edge would close a loop. The validator
    // would reject the assembled library — every characteristic, not just this one — so it is
    // refused here, where the reason can still be said in the user's own terms.
    if (hasCausalPath(p, effectId, causeId))
        return failResult(tr("%1 already leads to %2, so this would make the chain circular.")
                              .arg(effect->label, cause->label));

    for (const Edge &e : p.edges)
        if (e.type == EdgeType::Corroborates
            && ((e.from == causeId && e.to == effectId) || (e.from == effectId && e.to == causeId)))
            return failResult(tr("These two corroborate each other. One relationship or the other — "
                                 "a pair that both causes and corroborates counts twice when the "
                                 "explanation is ranked."));

    // Copy the labels out BEFORE editing. save() reloads the provider, which destroys the pack
    // these two point into — reading them afterwards is a use-after-free that would surface as a
    // garbled toast on a good day.
    const QString causeLabel  = cause->label;
    const QString effectLabel = effect->label;

    if (!beginEdit(effectId)) return failResult(tr("Could not open %1.").arg(effectLabel));
    addCause(causeId, strength);
    const QVariantMap r = save();
    if (!r.value(QStringLiteral("ok")).toBool()) { discard(); return r; }

    return okResult(tr("%1 now causes %2.").arg(causeLabel, effectLabel));
}

QVariantMap CharacteristicEditorModel::unlinkCause(const QString &causeId, const QString &effectId)
{
    if (m_editing)
        return failResult(tr("Finish or discard the open edit first."));

    const CharacteristicPack &p = m_provider->pack();
    const Condition          *cause  = p.condition(causeId);
    const Condition          *effect = p.condition(effectId);
    if (!cause || !effect) return failResult(tr("That is not in the library."));

    const Edge *found = nullptr;
    for (const Edge &e : p.edges)
        if (e.type == EdgeType::Causes && e.from == causeId && e.to == effectId) { found = &e; break; }
    if (found == nullptr) return failResult(tr("There is no link to remove."));

    const QString causeLabel  = cause->label;   // see linkCause: save() invalidates `p`
    const QString effectLabel = effect->label;

    // Everything needed to put it back, copied out BEFORE the write — ledger C31. The project's own
    // rule is that a recoverable removal offers an undo in the same breath, as the binding cascade
    // does; the inverse of this operation is one long-press away but that is not the same thing, and
    // it would not restore the STRENGTH, which is the part a reader cannot reconstruct from memory.
    m_edgeUndo         = *found;
    m_edgeUndoValid    = true;

    if (!beginEdit(effectId)) return failResult(tr("Could not open %1.").arg(effectLabel));
    removeCause(causeId);
    const QVariantMap r = save();
    if (!r.value(QStringLiteral("ok")).toBool()) {
        discard();
        m_edgeUndoValid = false;
        return r;
    }

    // Say what it means, not what it did to the file. Dropping the last cause leaves a
    // characteristic that can be reported and never explained, which is a validator warning and
    // worth knowing before the reader walks away from the page.
    const int   left = int(causesOf(m_provider->pack(), effectId).size());
    QVariantMap out  = left == 0
               ? okResult(tr("Removed. Nothing in the library explains %1 now.").arg(effectLabel))
               : okResult(tr("%1 no longer causes %2.").arg(causeLabel, effectLabel));
    out.insert(QStringLiteral("canUndo"), true);
    return out;
}

QVariantMap CharacteristicEditorModel::undoUnlinkCause()
{
    if (!m_edgeUndoValid)
        return failResult(tr("There is nothing to put back."));

    // Consumed whether or not it succeeds: a second attempt would re-run a link that has already
    // been refused once, and offering the same undo twice implies the first one did not happen.
    const Edge edge = m_edgeUndo;
    m_edgeUndoValid = false;

    return linkCause(edge.from, edge.to, strengthName(edge.strength));
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
            // Carried so the direction control can speak the existing measure's own words rather
            // than asking for them a second time.
            hit.insert(QStringLiteral("highMeans"), m.highMeans);
            out.insert(QStringLiteral("exactMatch"), hit);
            break;
        }

    // "Did you mean" — computed on the SERIES tuple, so it fires even when the reducers differ.
    // This is the moment duplicates are cheap to prevent; afterwards nobody merges them.
    QVariantList near;
    for (const SeriesMatch &sm : findSimilarSeries(series, existing)) {
        const QString nearId = seriesOwner.value(canonicalSeriesId(sm.series));
        QVariantMap n;
        n.insert(QStringLiteral("id"), nearId);
        n.insert(QStringLiteral("label"), canonicalSeriesLabel(sm.series));
        // Reusing one of these is choosing a DIFFERENT measure, and the tail has to be chosen
        // against that measure's own convention rather than the one being typed for this draft.
        n.insert(QStringLiteral("highMeans"), measureHighMeans(nearId));
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
    // What a HIGH value means, authored in the picker beside the tail that fires. A minted measure
    // exists to carry a signal on one of its tails, and the tail is chosen in the same breath — so
    // this is the one moment the sign convention is cheap to state and free to get right.
    m.highMeans  = facets.value(QStringLiteral("highMeans")).toString().trimmed();

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

        // The tail, in the measure's own words. Composed here rather than in the delegate: which
        // sentence belongs to which tail is a statement about sign conventions, and a statement
        // about correctness written in QML is a statement nothing can test.
        const QString      hm   = m ? m->highMeans : QString();
        const QVariantList opts = directionOptions(hm);
        const bool         isHigh =
            !s.direction.has_value() || *s.direction == Direction::High;
        const QVariantMap chosen = opts.value(isHigh ? 0 : 1).toMap();
        sm.insert(QStringLiteral("highMeans"), hm);
        sm.insert(QStringLiteral("directionLabel"), chosen.value(QStringLiteral("label")));
        sm.insert(QStringLiteral("directionSentence"), chosen.value(QStringLiteral("sentence")));

        // Blast radius: how many OTHER characteristics ride on this same measure. Editing a shared
        // measure changes all of them, and the author has to see that before they do it — a count
        // shown afterwards is not a warning, it is an explanation.
        int sharedWith = 0;
        for (const Condition &oc : p.conditions) {
            if (oc.id == m_draft.id) continue;
            for (const QString &osid : oc.detectedBy) {
                const Signal *os = p.signal(osid);
                if (os && os->measures.contains(mid)) { ++sharedWith; break; }
            }
        }
        sm.insert(QStringLiteral("sharedWith"), sharedWith);
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
