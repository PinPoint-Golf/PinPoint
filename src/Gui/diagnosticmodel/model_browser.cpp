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

#include "model_browser.h"

#include "../../Diagnostics/corridor_plot.h"
#include "../../Diagnostics/dag_layout.h"
#include "../../Diagnostics/diagnostics_health.h"
#include "../../Diagnostics/drill_pack.h"
#include "../../Diagnostics/measure_sample.h"
#include "../../Diagnostics/reference_pack.h"
#include "../../Diagnostics/screen_pack.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QDir>
#include <cmath>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrent>
#include <QJsonDocument>

#include <algorithm>

using namespace pinpoint::analysis;

namespace {

// ── Type keys ───────────────────────────────────────────────────────────────
//
// One spelling, here. These strings cross the QML boundary in six directions (the rail, the table,
// the inspector, the trail, search results and every edit call), and a second copy of any of them
// is a navigation that silently does nothing.
const QString kCharacteristics = QStringLiteral("characteristics");
const QString kCauses          = QStringLiteral("causes");
const QString kMeasures        = QStringLiteral("measures");
const QString kSignals         = QStringLiteral("signals");
const QString kLinks           = QStringLiteral("links");
const QString kScreens         = QStringLiteral("screens");
const QString kDrills          = QStringLiteral("drills");
const QString kReferences      = QStringLiteral("references");
const QString kCorridors       = QStringLiteral("corridors");
const QString kMetrics         = QStringLiteral("metrics");
const QString kHealth          = QStringLiteral("health");
// Not a content type: the shape of the flat result list that REPLACES the table while the search
// field has something in it. It is a type key only so that columns() answers for it and the table
// delegate needs no special case.
const QString kSearch          = QStringLiteral("search");

// A link's id. Edges have no id of their own — they are identified by their endpoints and type — so
// one is composed, and it has to round-trip because every edit call names a row by id.
QString edgeId(const QString &from, const QString &to, EdgeType t)
{
    return QStringLiteral("%1|%2|%3").arg(from, to, edgeTypeName(t));
}
bool splitEdgeId(const QString &id, QString &from, QString &to, EdgeType &t)
{
    const QStringList parts = id.split(QLatin1Char('|'));
    if (parts.size() != 3) return false;
    if (!edgeTypeFromName(parts.at(2), t)) return false;
    from = parts.at(0);
    to   = parts.at(1);
    return true;
}

// A cell. Every table cell in the panel is built by this, so the shape cannot drift between types.
//
// `kind` is what the EDITOR is, not what the value is: "text" types in place, "enum" opens a
// one-click list, "int" is a spin field, "none" cannot be edited. `own` says the value is the
// user's own rather than the shipped one, which is what the per-field "yours vs shipped" marker
// keys off.
QVariantMap cell(const QString &text, const QString &tone = QString(), bool mono = false,
                 const QString &align = QStringLiteral("left"))
{
    QVariantMap c;
    c.insert(QStringLiteral("text"), text);
    c.insert(QStringLiteral("tone"), tone);
    c.insert(QStringLiteral("mono"), mono);
    c.insert(QStringLiteral("align"), align);
    c.insert(QStringLiteral("kind"), QStringLiteral("none"));
    c.insert(QStringLiteral("editable"), false);
    c.insert(QStringLiteral("own"), false);
    return c;
}

// Make a cell editable. Separate from cell() so that a read-only surface reads as read-only in the
// source: a cell is inert unless somebody says otherwise.
QVariantMap &editable(QVariantMap &c, const QString &field, const QString &kind,
                      const QVariant &value, const QVariantList &options = {})
{
    c.insert(QStringLiteral("field"), field);
    c.insert(QStringLiteral("kind"), kind);
    c.insert(QStringLiteral("value"), value);
    c.insert(QStringLiteral("options"), options);
    c.insert(QStringLiteral("editable"), true);
    return c;
}

QVariantMap option(const QString &value, const QString &label)
{
    QVariantMap o;
    o.insert(QStringLiteral("value"), value);
    o.insert(QStringLiteral("label"), label);
    return o;
}

QVariantMap column(const QString &key, const QString &title, int width, bool flex = false,
                   const QString &align = QStringLiteral("left"), bool mono = false)
{
    QVariantMap c;
    c.insert(QStringLiteral("key"), key);
    c.insert(QStringLiteral("title"), title);
    c.insert(QStringLiteral("width"), width);
    c.insert(QStringLiteral("flex"), flex);
    c.insert(QStringLiteral("align"), align);
    c.insert(QStringLiteral("mono"), mono);
    c.insert(QStringLiteral("sortable"), true);
    return c;
}

// The enum vocabularies, as option lists. From C++ so a picker cannot invent a token that the
// loader will then refuse.
QVariantList groupOptions()
{
    QVariantList l;
    for (ConditionGroup g : allConditionGroups())
        l.append(option(conditionGroupName(g), conditionGroupLabel(g)));
    return l;
}
QVariantList reachOptions()
{
    QVariantList l;
    for (ConfirmedBy c : { ConfirmedBy::Measured, ConfirmedBy::Screened, ConfirmedBy::Asserted })
        l.append(option(confirmedByName(c), reachLabel(c)));
    return l;
}
QVariantList tierOptions()
{
    QVariantList l;
    for (ProvenanceTier t : { ProvenanceTier::Proposed, ProvenanceTier::NoSourceFound,
                              ProvenanceTier::Practice, ProvenanceTier::Indirect,
                              ProvenanceTier::Supported, ProvenanceTier::Established })
        l.append(option(provenanceTierName(t), provenanceTierLabel(t)));
    return l;
}
QVariantList strengthOptions()
{
    QVariantList l;
    for (Strength s : { Strength::Weak, Strength::Moderate, Strength::Strong })
        l.append(option(strengthName(s), strengthLabel(s)));
    return l;
}
QVariantList relationOptions()
{
    QVariantList l;
    l.append(option(QStringLiteral("causes"), QObject::tr("causes")));
    l.append(option(QStringLiteral("corroborates"), QObject::tr("corroborates")));
    l.append(option(QStringLiteral("excludes"), QObject::tr("excludes")));
    return l;
}
QVariantList statusOptions()
{
    QVariantList l;
    for (MeasureStatus s : { MeasureStatus::Live, MeasureStatus::Planned, MeasureStatus::NoProducer,
                             MeasureStatus::ExternalDevice, MeasureStatus::NotCapturable })
        l.append(option(measureStatusName(s), measureStatusLabel(s)));
    return l;
}
// Both the token and the label come from norm.h — normSourceName() writes the file and
// normSourceLabel() is the UI's own words. Only the option LIST is assembled here.
QVariantList normSourceOptions()
{
    QVariantList l;
    for (NormSource s : { NormSource::Heuristic, NormSource::Seated, NormSource::Literature,
                          NormSource::Imported })
        l.append(option(normSourceName(s), normSourceLabel(s)));
    return l;
}

QVariantList directionOptionList()
{
    QVariantList l;
    l.append(option(QStringLiteral("high"), QObject::tr("Too much")));
    l.append(option(QStringLiteral("low"), QObject::tr("Too little")));
    return l;
}
QVariantList stateOptions()
{
    QVariantList l;
    for (ConditionState s : { ConditionState::Draft, ConditionState::Candidate, ConditionState::Active,
                              ConditionState::NeedsRevalidation, ConditionState::Superseded,
                              ConditionState::Retired })
        l.append(option(conditionStateName(s), conditionStateName(s)));
    return l;
}

// Resolvability drives the row dot. A capture gap is deliberately NOT the fault colour: it is not a
// failure, it is an honest statement that no sensor we have can see this. Returned as a tone NAME
// rather than a colour, so the delegate keeps it in Theme tokens.
QString statusTone(MeasureStatus s)
{
    switch (s) {
    case MeasureStatus::Live:           return QStringLiteral("good");
    case MeasureStatus::Planned:        return QStringLiteral("watch");
    case MeasureStatus::NoProducer:     return QStringLiteral("fault");
    case MeasureStatus::ExternalDevice: return QStringLiteral("watch");
    case MeasureStatus::NotCapturable:  return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

// The weakest status across a condition's measures: a characteristic is only as resolvable as its
// least resolvable input.
MeasureStatus weakest(MeasureStatus a, MeasureStatus b)
{
    auto rank = [](MeasureStatus s) {
        switch (s) {
        case MeasureStatus::Live:           return 0;
        case MeasureStatus::Planned:        return 1;
        case MeasureStatus::NoProducer:     return 2;
        case MeasureStatus::ExternalDevice: return 3;
        case MeasureStatus::NotCapturable:  return 4;
        }
        return 2;
    };
    return rank(a) >= rank(b) ? a : b;
}

// Every entity in a pack, keyed by id, as JSON — the basis of the dirty diff. savePack() is the one
// serialiser the pack has, so the comparison is against exactly the bytes a save would write.
QHash<QString, QJsonObject> entitiesOf(const CharacteristicPack &p)
{
    QHash<QString, QJsonObject> out;
    const QJsonObject           root = savePack(p);
    for (const QString &key : { QStringLiteral("measures"), QStringLiteral("signals"),
                                QStringLiteral("conditions") })
        for (const QJsonValue &v : root.value(key).toArray()) {
            const QJsonObject o = v.toObject();
            out.insert(o.value(QStringLiteral("id")).toString(), o);
        }
    // Edges have no id, so they are keyed on the composed one — which is what the table shows them
    // under, so a dirty edge marks the row the author actually edited.
    for (const QJsonValue &v : root.value(QStringLiteral("edges")).toArray()) {
        const QJsonObject o = v.toObject();
        EdgeType          t = EdgeType::Causes;
        edgeTypeFromName(o.value(QStringLiteral("type")).toString(), t);
        out.insert(edgeId(o.value(QStringLiteral("from")).toString(),
                          o.value(QStringLiteral("to")).toString(), t),
                   o);
    }
    return out;
}

// The same trick for the two flat sets: serialise a one-row set with the real serialiser and keep the
// row's own object, so the comparison is against exactly the bytes a save would write rather than a
// hand-written field list that drifts the first time a field is added to Screen or Drill.
QHash<QString, QJsonObject> screenEntities(const ScreenSet &set)
{
    QHash<QString, QJsonObject> out;
    for (const Screen &s : set.screens) {
        ScreenSet one;
        one.screens.push_back(s);
        const QJsonArray arr = QJsonDocument::fromJson(saveScreenSet(one))
                                   .object()
                                   .value(QStringLiteral("screens"))
                                   .toArray();
        if (!arr.isEmpty()) out.insert(s.id, arr.first().toObject());
    }
    return out;
}

QHash<QString, QJsonObject> drillEntities(const DrillSet &set)
{
    QHash<QString, QJsonObject> out;
    for (const Drill &d : set.drills) {
        DrillSet one;
        one.drills.push_back(d);
        const QJsonArray arr = QJsonDocument::fromJson(saveDrillSet(one))
                                   .object()
                                   .value(QStringLiteral("drills"))
                                   .toArray();
        if (!arr.isEmpty()) out.insert(d.id, arr.first().toObject());
    }
    return out;
}

// One norm row as JSON, for the dirty diff. saveNormPack() serialises a whole pack, so a
// single-row pack is wrapped and unwrapped — the point is to compare exactly the bytes a save would
// write, rather than a hand-written field-by-field comparison that drifts when a field is added.
QJsonObject normJson(const Norm &n)
{
    NormPack one;
    one.id = QStringLiteral("x");
    one.norms.push_back(n);
    const QJsonArray rows = saveNormPack(one).value(QStringLiteral("norms")).toArray();
    return rows.isEmpty() ? QJsonObject() : rows.first().toObject();
}

bool matches(const QString &haystack, const QString &needle)
{
    return !needle.isEmpty() && haystack.contains(needle, Qt::CaseInsensitive);
}

// What a metric needs before it can produce anything, in the words a user would meet elsewhere.
// Stated positively and in order of how often it is the blocker.
QStringList metricNeeds(const MetricRequirement &r)
{
    QStringList needs;
    if (r.faceOnCamera)  needs << QObject::tr("face-on camera");
    if (r.clubTrack)     needs << QObject::tr("club tracking");
    if (r.ballTrack)     needs << QObject::tr("ball tracking");
    if (r.launchMonitor) needs << QObject::tr("launch monitor");
    if (!r.imuRoles.empty())
        needs << QObject::tr("%n IMU(s)", "", int(r.imuRoles.size()));
    return needs;
}

// The singular noun for one row of a type — the Type column in the cross-type result list. Singular
// because it labels ONE object ("Measure"), where the rail labels a collection ("Measures").
QString typeLabelFor(const QString &type)
{
    if (type == kCharacteristics) return QObject::tr("Characteristic");
    if (type == kCauses)          return QObject::tr("Cause");
    if (type == kMeasures)        return QObject::tr("Measure");
    if (type == kSignals)         return QObject::tr("Signal");
    if (type == kLinks)           return QObject::tr("Causal link");
    if (type == kScreens)         return QObject::tr("Screen");
    if (type == kDrills)          return QObject::tr("Drill");
    if (type == kReferences)      return QObject::tr("Reference");
    if (type == kMetrics)         return QObject::tr("Metric");
    if (type == kHealth)          return QObject::tr("Health");
    return type;
}

} // namespace

// ── Construction ────────────────────────────────────────────────────────────

ModelBrowser::ModelBrowser(QObject *parent)
    : QObject(parent)
    , m_core(makeResourcePackProvider())
    , m_norms(sharedNormProvider())
    , m_cat(makeMetricCatalogue())
    , m_policyName(QStringLiteral("standard"))
    , m_corridorWatcher(new QFutureWatcher<QVariantList>(this))
{
    connect(m_corridorWatcher, &QFutureWatcher<QVariantList>::finished,
            this, &ModelBrowser::onCorridorScanFinished);

    // The user's override file, read separately from the merged view — saving must write back the
    // user's own entries and never a flattened copy of the shipped pack.
    //
    // `parsed`, NOT `loaded`. An overlay routinely fails STANDALONE referential validation because
    // its edges point at core conditions it does not contain; keying off `loaded` would start with
    // an empty working copy and the first save would erase every override the user had.
    QFile f(userPackPath());
    if (f.open(QIODevice::ReadOnly)) {
        PackLoadResult res = loadPack(f.readAll(), userPackPath());
        if (res.parsed) m_savedUser = std::move(res.pack);
    }
    if (m_savedUser.id.isEmpty()) {
        m_savedUser.id            = QStringLiteral("user");
        m_savedUser.version       = QStringLiteral("1");
        m_savedUser.schemaVersion = kPackSchemaVersion;
    }
    m_working = m_savedUser;

    // The user norm set, read the same way and for the same reason. `parsed`, not `loaded`: a user
    // layer routinely fails standalone referential checks because its rows name measures it does
    // not itself contain, and keying off `loaded` would start with an empty set and let the first
    // save erase every corridor the user had authored.
    QFile nf(userNormPath());
    if (nf.open(QIODevice::ReadOnly)) {
        NormPackLoadResult res = loadNormPack(nf.readAll(), userNormPath());
        if (res.parsed) m_savedNorms = std::move(res.pack);
    }
    if (m_savedNorms.id.isEmpty()) {
        m_savedNorms.id      = QStringLiteral("user");
        m_savedNorms.version = QStringLiteral("1");
    }
    m_workingNorms = m_savedNorms;

    // The screen and drill layers, read the same way and for the same reason.
    m_savedScreens = loadUserScreenSet();
    if (m_savedScreens.id.isEmpty()) {
        m_savedScreens.id      = QStringLiteral("user");
        m_savedScreens.version = QStringLiteral("1");
    }
    m_workingScreens = m_savedScreens;

    m_savedDrills = loadUserDrillSet();
    if (m_savedDrills.id.isEmpty()) {
        m_savedDrills.id      = QStringLiteral("user");
        m_savedDrills.version = QStringLiteral("1");
    }
    m_workingDrills = m_savedDrills;

    rebuild();
}

ModelBrowser::~ModelBrowser()
{
    // The worker reads only copies, but it writes into a future this object owns — so it has to be
    // joined before that future dies with us.
    if (m_corridorWatcher->isRunning()) m_corridorWatcher->waitForFinished();
}

// ⚠ THIS INVALIDATES EVERY POINTER INTO THE ASSEMBLY. m_assembled, m_norms, m_screens and m_drills
// are all replaced wholesale, so a `const Condition *`, `Measure *`, `Screen *`, `Drill *`,
// `ContextNode *` or a `const ContextTree &` taken before this call is dangling after it — as is the
// reference `pack()` returned.
//
// Every writer in this file ends with rebuild() and then names what it did, which puts the temptation
// in the same six lines every time: resolve a label from a pointer AFTER the rebuild that freed it.
// It crashed exactly once that way, inside QString::arg() rather than at the dereference — so the
// stack blamed the formatting and the fault was three lines earlier.
//
// The rule: resolve pointers to QStrings BEFORE calling this. `m_working`, `m_workingNorms`,
// `m_workingScreens` and `m_workingDrills` are members and survive this call, so pointers into those
// are safe ACROSS IT.
//
// They are not safe across a RESTORE. The refusal paths undo a copy-on-write with `m_working =
// before`, and a vector copy-assignment from a shorter source destroys the surplus elements — which
// is exactly the row workingCondition() had just appended. A pointer to it then addresses a
// destroyed QString inside a buffer the vector still owns, so the read is undefined behaviour that
// AddressSanitizer cannot see: nothing was freed. Same rule, same fix — resolve the label first.
void ModelBrowser::rebuild()
{
    std::vector<std::unique_ptr<ICharacteristicPackProvider>> overlays;
    overlays.push_back(makeMemoryPackProvider(m_working, tr("Your edits")));
    m_assembled = makeMergedPackProvider(makeResourcePackProvider(), std::move(overlays));

    // The norm side, assembled from the WORKING copy for the same reason the pack is: a corridor
    // edited but not saved has to be what the table, the inspector and the health strip all read,
    // or editing looks like nothing happening.
    auto coreNorms = makeResourceNormProvider();
    // The context tree comes from core — a user norm set does not carry one, and resolution is
    // meaningless without it.
    const ContextTree tree = coreNorms->contexts();
    std::vector<std::unique_ptr<INormProvider>> normOverlays;
    normOverlays.push_back(makeMemoryNormProvider(m_workingNorms, tree, tr("Your corridors")));
    m_norms = makeMergedNormProvider(std::move(coreNorms), std::move(normOverlays));

    // Screens and drills, assembled the same way: core, then the working layer over it by id. A user
    // row REPLACES the shipped one rather than merging field by field — half a protocol from one
    // movement with another's pass criterion would be worse than either.
    m_screens = coreScreenSet();
    for (const Screen &s : m_workingScreens.screens) {
        auto it = std::find_if(m_screens.screens.begin(), m_screens.screens.end(),
                               [&](const Screen &x) { return x.id == s.id; });
        if (it != m_screens.screens.end()) *it = s;
        else                                m_screens.screens.push_back(s);
    }
    m_drills = coreDrillSet();
    for (const Drill &d : m_workingDrills.drills) {
        auto it = std::find_if(m_drills.drills.begin(), m_drills.drills.end(),
                               [&](const Drill &x) { return x.id == d.id; });
        if (it != m_drills.drills.end()) *it = d;
        else                              m_drills.drills.push_back(d);
    }

    invalidateDerived();
}

const CharacteristicPack &ModelBrowser::pack() const { return m_assembled->pack(); }

void ModelBrowser::invalidateDerived()
{
    m_dirtyIdsValid = false;
    emit modelChanged();
}

void ModelBrowser::refresh()
{
    // Re-take the shared provider so the REST of the app sees whatever landed elsewhere, then
    // rebuild our own assembly over the working copies. This object deliberately does not read the
    // shared norm provider for its own answers: that one reflects the file, and this panel has to
    // answer from the draft.
    resetSharedNormProvider();
    rebuild();
}

// ── Dirty tracking ──────────────────────────────────────────────────────────

const QSet<QString> &ModelBrowser::dirtyIds() const
{
    if (m_dirtyIdsValid) return m_dirtyIds;

    // Derived by diffing the working pack against the saved one, never tracked by hand. undo(),
    // undoTo() and revert() all move the working copy wholesale, and a hand-kept touched set would
    // drift out of step the first time any of them ran — leaving rows badged as edited that are
    // byte-identical to the file.
    const QHash<QString, QJsonObject> saved   = entitiesOf(m_savedUser);
    const QHash<QString, QJsonObject> working = entitiesOf(m_working);

    m_dirtyIds.clear();
    for (auto it = working.constBegin(); it != working.constEnd(); ++it)
        if (!saved.contains(it.key()) || saved.value(it.key()) != it.value())
            m_dirtyIds.insert(it.key());
    // Removals are edits too. An override the author deleted is unsaved work exactly as an addition
    // is, and a status bar that counted only additions would say "nothing to save" over a pending
    // deletion.
    for (auto it = saved.constBegin(); it != saved.constEnd(); ++it)
        if (!working.contains(it.key())) m_dirtyIds.insert(it.key());

    // The norm side, keyed the way the corridor table shows them so a dirty corridor marks the row
    // the author actually edited.
    QHash<QString, QJsonObject> savedNorms, workingNorms;
    for (const Norm &n : m_savedNorms.norms)
        savedNorms.insert(corridorId(n.measureId, n.contextId), normJson(n));
    for (const Norm &n : m_workingNorms.norms)
        workingNorms.insert(corridorId(n.measureId, n.contextId), normJson(n));

    for (auto it = workingNorms.constBegin(); it != workingNorms.constEnd(); ++it)
        if (!savedNorms.contains(it.key()) || savedNorms.value(it.key()) != it.value())
            m_dirtyIds.insert(it.key());
    for (auto it = savedNorms.constBegin(); it != savedNorms.constEnd(); ++it)
        if (!workingNorms.contains(it.key())) m_dirtyIds.insert(it.key());

    // Screens and drills, diffed the same way and against the same serialiser a save would use, so
    // an edit that round-trips to identical bytes does not leave a row badged as unsaved.
    const auto diffSet = [this](const QHash<QString, QJsonObject> &saved,
                                const QHash<QString, QJsonObject> &working) {
        for (auto it = working.constBegin(); it != working.constEnd(); ++it)
            if (!saved.contains(it.key()) || saved.value(it.key()) != it.value())
                m_dirtyIds.insert(it.key());
        for (auto it = saved.constBegin(); it != saved.constEnd(); ++it)
            if (!working.contains(it.key())) m_dirtyIds.insert(it.key());
    };
    diffSet(screenEntities(m_savedScreens), screenEntities(m_workingScreens));
    diffSet(drillEntities(m_savedDrills), drillEntities(m_workingDrills));

    m_dirtyIdsValid = true;
    return m_dirtyIds;
}

bool ModelBrowser::dirty() const { return !dirtyIds().isEmpty(); }
int  ModelBrowser::unsavedCount() const { return dirtyIds().size(); }

// Counted over the DRAFT rather than the file, because these two figures answer "what would a reset
// take away", and an edit made a minute ago is as much a loss as one saved last week. sourceOf() is
// the one place that decides whose content an id is, and both counts go through it rather than
// re-deriving the answer.
int ModelBrowser::countBySource(const QString &want) const
{
    int                               n    = 0;
    const QHash<QString, QJsonObject> mine = entitiesOf(m_working);
    const CharacteristicPack         &core = m_core->pack();

    // Deliberately NOT sourceOf() per id for the pack half. sourceOf() re-serialises the whole working
    // pack on every call to answer "is this yours?", and here the answer is already yes for every key —
    // they came out of the working pack. So the only question left is whether core has it too, and
    // these two properties are re-evaluated on every modelChanged.
    for (auto it = mine.constBegin(); it != mine.constEnd(); ++it) {
        const QString &id = it.key();
        QString        from, to;
        EdgeType       type   = EdgeType::Causes;
        bool           inCore = false;
        if (splitEdgeId(id, from, to, type)) {
            for (const Edge &e : core.edges)
                if (e.from == from && e.to == to && e.type == type) { inCore = true; break; }
        } else {
            inCore = core.condition(id) || core.measure(id) || core.signal(id);
        }
        if ((inCore ? QStringLiteral("both") : QStringLiteral("yours")) == want) ++n;
    }
    for (const Norm &norm : m_workingNorms.norms)
        if (sourceOf(corridorId(norm.measureId, norm.contextId)) == want) ++n;
    for (const Screen &s : m_workingScreens.screens)
        if (sourceOf(s.id) == want) ++n;
    for (const Drill &d : m_workingDrills.drills)
        if (sourceOf(d.id) == want) ++n;
    return n;
}

int ModelBrowser::overriddenCount() const { return countBySource(QStringLiteral("both")); }
int ModelBrowser::authoredCount() const { return countBySource(QStringLiteral("yours")); }

QString ModelBrowser::sourceOf(const QString &id) const
{
    const CharacteristicPack &core = m_core->pack();

    // A corridor lives in the OTHER registry, so it cannot be answered by looking in the pack —
    // which is what this did, and every corridor therefore read as shipped however hard it had been
    // edited. The Source column and the inspector header both key off this.
    {
        QString mid, ctx;
        if (splitCorridorId(id, mid, ctx)) {
            bool mine = false;
            for (const Norm &n : m_workingNorms.norms)
                if (n.measureId == mid && n.contextId == ctx) mine = true;
            const bool shipped = m_norms->shippedNorm(mid, ctx) != nullptr;
            if (mine && shipped) return QStringLiteral("both");
            if (mine)            return QStringLiteral("yours");
            return QStringLiteral("shipped");
        }
    }

    // Screens and drills live in their own flat sets, so they cannot be answered from the pack
    // either. Same three states, read off the same two layers the rows are assembled from.
    if (id.startsWith(QStringLiteral("screen."))) {
        const bool shipped = coreScreenSet().screen(id) != nullptr;
        const bool mine    = m_workingScreens.screen(id) != nullptr;
        if (mine && shipped) return QStringLiteral("both");
        if (mine)            return QStringLiteral("yours");
        return QStringLiteral("shipped");
    }
    if (id.startsWith(QStringLiteral("drill."))) {
        const bool shipped = coreDrillSet().drill(id) != nullptr;
        const bool mine    = m_workingDrills.drill(id) != nullptr;
        if (mine && shipped) return QStringLiteral("both");
        if (mine)            return QStringLiteral("yours");
        return QStringLiteral("shipped");
    }

    QString from, to;
    EdgeType type = EdgeType::Causes;
    bool     inCore = false;
    if (splitEdgeId(id, from, to, type)) {
        for (const Edge &e : core.edges)
            if (e.from == from && e.to == to && e.type == type) { inCore = true; break; }
    } else {
        inCore = core.condition(id) || core.measure(id) || core.signal(id);
    }

    const QHash<QString, QJsonObject> mine = entitiesOf(m_working);
    const bool                        inUser = mine.contains(id);

    if (inCore && inUser) return QStringLiteral("both");
    if (inUser)           return QStringLiteral("yours");
    if (inCore)           return QStringLiteral("shipped");
    // Reachable for the reference registry, which is imported and stays read-only.
    return QStringLiteral("shipped");
}

// ── Census ──────────────────────────────────────────────────────────────────

QVariantList ModelBrowser::types() const
{
    struct T { QString key; QString label; QString hint; };
    static const std::vector<T> defs = {
        { kCharacteristics, tr("Characteristics"), tr("named states of the swing") },
        { kCauses,          tr("Causes"),          tr("conditions that explain others") },
        { kMetrics,         tr("Metrics"),         tr("the numbers the pipeline produces") },
        { kMeasures,        tr("Measures"),        tr("what the app can actually read") },
        { kSignals,         tr("Signals"),         tr("the test that identifies a condition") },
        { kLinks,           tr("Causal links"),    tr("every claim the graph makes") },
        { kScreens,         tr("Screens"),         tr("physical tests, no capture needed") },
        { kDrills,          tr("Drills"),          tr("what a golfer does about it") },
        { kReferences,      tr("References"),      tr("what the library rests on") },
        { kCorridors,       tr("Corridors"),       tr("what good looks like, per context") },
        { kHealth,          tr("Health"),          tr("what is wrong with this draft") },
    };

    QVariantList out;
    for (const T &t : defs) {
        QVariantMap m;
        m.insert(QStringLiteral("key"), t.key);
        m.insert(QStringLiteral("label"), t.label);
        m.insert(QStringLiteral("hint"), t.hint);
        // What ONE of these is called, for the context bar's `+ New drill`. The rail labels a
        // collection and the button names an object, and typeLabelFor() already draws that
        // distinction — so the button reads it rather than keeping a second table in QML.
        m.insert(QStringLiteral("one"), typeLabelFor(t.key));
        // Counted by asking the row list. Two counts derived independently is how a badge ends up
        // disagreeing with the page it links to.
        m.insert(QStringLiteral("count"), rawRows(t.key).size());
        out.append(m);
    }
    return out;
}

int ModelBrowser::totalObjects() const
{
    // Summed, never hardcoded. Health is excluded: it is a view of what is WRONG with the objects,
    // not an object, and counting it would make the total move when somebody fixed something.
    int n = 0;
    for (const QVariant &v : types()) {
        const QVariantMap t = v.toMap();
        if (t.value(QStringLiteral("key")).toString() == kHealth) continue;
        n += t.value(QStringLiteral("count")).toInt();
    }
    return n;
}

QString ModelBrowser::packLabel() const
{
    const CharacteristicPack &core = m_core->pack();
    return tr("%1 v%2 · schema %3").arg(core.id, core.version).arg(core.schemaVersion);
}

// ── Columns ─────────────────────────────────────────────────────────────────

QVariantList ModelBrowser::columns(const QString &type) const
{
    QVariantList c;
    if (type == kCharacteristics) {
        c.append(column(QStringLiteral("name"), tr("Name"), 240, /*flex*/ true));
        c.append(column(QStringLiteral("group"), tr("Group"), 120));
        c.append(column(QStringLiteral("measures"), tr("Meas"), 52, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("causes"), tr("Caus"), 52, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("explains"), tr("Expl"), 52, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("reach"), tr("Reach"), 100));
        c.append(column(QStringLiteral("evidence"), tr("Evidence"), 128));
    } else if (type == kCauses) {
        c.append(column(QStringLiteral("name"), tr("Name"), 240, true));
        c.append(column(QStringLiteral("group"), tr("Group"), 120));
        c.append(column(QStringLiteral("explains"), tr("Explains"), 68, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("reach"), tr("Reach"), 100));
        c.append(column(QStringLiteral("screen"), tr("Screen"), 150));
        c.append(column(QStringLiteral("evidence"), tr("Evidence"), 128));
    } else if (type == kMeasures) {
        c.append(column(QStringLiteral("name"), tr("Name"), 240, true));
        c.append(column(QStringLiteral("unit"), tr("Unit"), 96));
        c.append(column(QStringLiteral("anchor"), tr("Anchor"), 86, false, QStringLiteral("left"), true));
        c.append(column(QStringLiteral("status"), tr("Status"), 96));
        c.append(column(QStringLiteral("readBy"), tr("Read by"), 68, false, QStringLiteral("right"), true));
    } else if (type == kMetrics) {
        c.append(column(QStringLiteral("name"), tr("Name"), 240, /*flex*/ true));
        c.append(column(QStringLiteral("key"), tr("Key"), 170, false, QStringLiteral("left"), true));
        c.append(column(QStringLiteral("unit"), tr("Unit"), 80));
        c.append(column(QStringLiteral("group"), tr("Group"), 140));
        c.append(column(QStringLiteral("needs"), tr("Needs"), 160));
        c.append(column(QStringLiteral("readBy"), tr("Read by"), 68, false, QStringLiteral("right"), true));
    } else if (type == kSignals) {
        c.append(column(QStringLiteral("name"), tr("Id"), 240, true, QStringLiteral("left"), true));
        c.append(column(QStringLiteral("test"), tr("Test"), 130));
        c.append(column(QStringLiteral("direction"), tr("Direction"), 104));
        c.append(column(QStringLiteral("measures"), tr("Measures"), 220));
        c.append(column(QStringLiteral("usedBy"), tr("Used by"), 68, false, QStringLiteral("right"), true));
    } else if (type == kLinks) {
        // Only the FROM column flexes. Two flexible columns would each be handed the whole slack
        // and overlap; the cause is the end an author scans down, so it is the one that grows.
        c.append(column(QStringLiteral("from"), tr("From"), 200, /*flex*/ true));
        c.append(column(QStringLiteral("to"), tr("To"), 210));
        c.append(column(QStringLiteral("relation"), tr("Relation"), 112));
        c.append(column(QStringLiteral("strength"), tr("Strength"), 100));
        c.append(column(QStringLiteral("evidence"), tr("Evidence"), 128));
    } else if (type == kScreens) {
        // `Name`, not `Id`. The column showed the label all along and called it an id, which was
        // harmless while nothing could be typed into it and misleading the moment something could.
        c.append(column(QStringLiteral("name"), tr("Name"), 220, true));
        c.append(column(QStringLiteral("region"), tr("Region"), 130));
        c.append(column(QStringLiteral("pass"), tr("Passing looks like"), 240));
        c.append(column(QStringLiteral("settles"), tr("Settles"), 220));
        c.append(column(QStringLiteral("settlesCount"), tr("N"), 48, false, QStringLiteral("right"), true));
    } else if (type == kDrills) {
        c.append(column(QStringLiteral("name"), tr("Name"), 220, true));
        c.append(column(QStringLiteral("targets"), tr("Trying to change"), 240));
        c.append(column(QStringLiteral("equipment"), tr("Equipment"), 150));
        c.append(column(QStringLiteral("answers"), tr("Answers"), 200));
        c.append(column(QStringLiteral("answersCount"), tr("N"), 48, false, QStringLiteral("right"), true));
    } else if (type == kReferences) {
        c.append(column(QStringLiteral("name"), tr("Citation"), 240, true));
        c.append(column(QStringLiteral("identifier"), tr("Identifier"), 150, false, QStringLiteral("left"), true));
        c.append(column(QStringLiteral("supports"), tr("Supports"), 260));
        c.append(column(QStringLiteral("tier"), tr("Tier"), 128));
    } else if (type == kCorridors) {
        c.append(column(QStringLiteral("name"), tr("Measure"), 240, /*flex*/ true));
        c.append(column(QStringLiteral("context"), tr("Context"), 120));
        c.append(column(QStringLiteral("mu"), tr("Aspiration"), 92, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("sigmaLo"), tr("Tol −"), 74, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("sigmaHi"), tr("Tol +"), 74, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("unit"), tr("Unit"), 96));
        c.append(column(QStringLiteral("source"), tr("Source"), 100));
    } else if (type == kSearch) {
        // The 4b shape: one row per object whatever it is, so "hip" answers across every content
        // file at once. Type leads, because the first thing a reader needs is what kind of thing
        // they are looking at.
        c.append(column(QStringLiteral("type"), tr("Type"), 104));
        c.append(column(QStringLiteral("name"), tr("Name"), 240, /*flex*/ true));
        c.append(column(QStringLiteral("identifier"), tr("Identifier"), 150, false,
                        QStringLiteral("left"), true));
        c.append(column(QStringLiteral("connected"), tr("Connected to"), 300));
        c.append(column(QStringLiteral("links"), tr("Links"), 62, false, QStringLiteral("right"), true));
        c.append(column(QStringLiteral("status"), tr("Status"), 90));
    } else if (type == kHealth) {
        c.append(column(QStringLiteral("severity"), tr("Severity"), 90));
        c.append(column(QStringLiteral("code"), tr("Code"), 170, false, QStringLiteral("left"), true));
        c.append(column(QStringLiteral("subject"), tr("Subject"), 200, false, QStringLiteral("left"), true));
        c.append(column(QStringLiteral("message"), tr("Message"), 320, true));
    }
    return c;
}

// ── Rows ────────────────────────────────────────────────────────────────────

QVariantMap ModelBrowser::conditionRow(const Condition &c, bool asCause) const
{
    const CharacteristicPack &p = pack();

    // Roll this condition's measures up into one resolvability, and count them.
    MeasureStatus status     = MeasureStatus::Live;
    int           measureN   = 0;
    bool          anyMeasure = false;
    for (const QString &sid : c.detectedBy) {
        const Signal *s = p.signal(sid);
        if (!s) continue;
        for (const QString &mid : s->measures) {
            const Measure *m = p.measure(mid);
            if (!m) continue;
            ++measureN;
            status     = anyMeasure ? weakest(status, m->status) : m->status;
            anyMeasure = true;
        }
    }
    if (!anyMeasure) status = MeasureStatus::NoProducer;

    QVariantMap nameCell = cell(c.label.isEmpty() ? c.id : c.label);
    editable(nameCell, QStringLiteral("label"), QStringLiteral("text"), c.label);

    QVariantMap groupCell = cell(conditionGroupLabel(c.group));
    editable(groupCell, QStringLiteral("group"), QStringLiteral("enum"),
             conditionGroupName(c.group), groupOptions());

    QVariantMap reachCell = cell(reachLabel(c.confirmedBy));
    editable(reachCell, QStringLiteral("reach"), QStringLiteral("enum"),
             confirmedByName(c.confirmedBy), reachOptions());

    QVariantMap tierCell = cell(provenanceTierLabel(c.provenance.tier),
                                c.provenance.tier == ProvenanceTier::Proposed
                                    ? QStringLiteral("warn") : QString());
    editable(tierCell, QStringLiteral("tier"), QStringLiteral("enum"),
             provenanceTierName(c.provenance.tier), tierOptions());

    QVariantList cells;
    cells.append(nameCell);
    cells.append(groupCell);
    if (asCause) {
        cells.append(cell(QString::number(coverageOf(p, c.id)), QString(), true, QStringLiteral("right")));
        cells.append(reachCell);
        cells.append(cell(c.screenRef, c.screenRef.isEmpty() ? QStringLiteral("dim") : QString()));
        cells.append(tierCell);
    } else {
        cells.append(cell(QString::number(measureN), QString(), true, QStringLiteral("right")));
        cells.append(cell(QString::number(causesOf(p, c.id).size()), QString(), true, QStringLiteral("right")));
        cells.append(cell(QString::number(effectsOf(p, c.id).size()), QString(), true, QStringLiteral("right")));
        cells.append(reachCell);
        cells.append(tierCell);
    }

    QVariantMap r;
    r.insert(QStringLiteral("id"), c.id);
    r.insert(QStringLiteral("type"), asCause ? kCauses : kCharacteristics);
    r.insert(QStringLiteral("label"), c.label.isEmpty() ? c.id : c.label);
    r.insert(QStringLiteral("dot"), statusTone(status));
    r.insert(QStringLiteral("cells"), cells);
    // Sort keys the display strings cannot provide: a group must sort in SWING ORDER, which is what
    // the enum order is, and sorting the labels alphabetically would put Ball flight first.
    QVariantMap keys;
    keys.insert(QStringLiteral("group"), int(c.group));
    keys.insert(QStringLiteral("measures"), measureN);
    keys.insert(QStringLiteral("causes"), int(causesOf(p, c.id).size()));
    keys.insert(QStringLiteral("explains"), coverageOf(p, c.id));
    keys.insert(QStringLiteral("evidence"), int(c.provenance.tier));
    keys.insert(QStringLiteral("reach"), int(c.confirmedBy));
    keys.insert(QStringLiteral("name"), c.label.isEmpty() ? c.id : c.label);
    // The axis pair, so the two tails of one measure sort adjacent rather than as near-duplicates
    // scattered through the group.
    keys.insert(QStringLiteral("axis"), c.axis);
    r.insert(QStringLiteral("sortKeys"), keys);
    r.insert(QStringLiteral("searchText"),
             QStringList{ c.label, c.id, c.axis, conditionGroupLabel(c.group),
                          c.aliases.join(QLatin1Char(' ')), c.consequence.text() }
                 .join(QLatin1Char(' ')));
    return r;
}

QVariantMap ModelBrowser::measureRow(const Measure &m) const
{
    // The derived name. Nine shipped measures carry an empty label and eight of them are planned or
    // noProducer — exactly the rows an author is hunting for, so they must not render blank. One
    // rule, in the pack layer, rather than a fallback per view.
    const QString label = measureDisplayLabel(m);

    QString anchor;
    switch (m.reducer.kind) {
    case ReducerKind::At:
        anchor = phaseLabel(m.reducer.anchor.value_or(m.reducer.window.first));
        break;
    case ReducerKind::Delta:
        anchor = QStringLiteral("Δ %1→%2").arg(phaseLabel(m.reducer.window.first),
                                               phaseLabel(m.reducer.window.second));
        break;
    case ReducerKind::Rate:
        anchor = QStringLiteral("d/dt %1→%2").arg(phaseLabel(m.reducer.window.first),
                                                  phaseLabel(m.reducer.window.second));
        break;
    case ReducerKind::Extremum:
        // Never a single phase, and it must not be shown as one: "the lowest lag angle between P5
        // and P6" is not a reading AT P5 or at P6, and labelling it with either would attach the
        // column's meaning to the wrong number.
        anchor = (m.reducer.sense == ExtremumSense::Min ? QStringLiteral("min %1→%2")
                                                        : QStringLiteral("max %1→%2"))
                     .arg(phaseLabel(m.reducer.window.first), phaseLabel(m.reducer.window.second));
        break;
    }

    QVariantMap nameCell = cell(label);
    editable(nameCell, QStringLiteral("label"), QStringLiteral("text"), m.label);

    QVariantMap unitCell = cell(m.unit, m.unit.isEmpty() ? QStringLiteral("dim") : QString());
    editable(unitCell, QStringLiteral("unit"), QStringLiteral("text"), m.unit);

    QVariantMap statusCell = cell(measureStatusLabel(m.status), statusTone(m.status));
    editable(statusCell, QStringLiteral("status"), QStringLiteral("enum"),
             measureStatusName(m.status), statusOptions());

    const int users = measureUsers(m.id);

    QVariantList cells;
    cells.append(nameCell);
    cells.append(unitCell);
    cells.append(cell(anchor, QStringLiteral("dim"), true));
    cells.append(statusCell);
    // Read by nothing is the interesting case — 42 measures in the shipped pack — so it is toned
    // rather than rendered as an ordinary zero.
    cells.append(cell(QString::number(users), users == 0 ? QStringLiteral("warn") : QString(), true,
                      QStringLiteral("right")));

    QVariantMap r;
    r.insert(QStringLiteral("id"), m.id);
    r.insert(QStringLiteral("type"), kMeasures);
    r.insert(QStringLiteral("label"), label);
    r.insert(QStringLiteral("dot"), statusTone(m.status));
    r.insert(QStringLiteral("cells"), cells);

    QVariantMap keys;
    keys.insert(QStringLiteral("name"), label);
    keys.insert(QStringLiteral("unit"), m.unit);
    keys.insert(QStringLiteral("anchor"), anchor);
    keys.insert(QStringLiteral("status"), int(m.status));
    keys.insert(QStringLiteral("readBy"), users);
    r.insert(QStringLiteral("sortKeys"), keys);
    r.insert(QStringLiteral("searchText"),
             QStringList{ label, m.id, m.unit, m.metricKey, m.highMeans,
                          m.aliases.join(QLatin1Char(' ')) }
                 .join(QLatin1Char(' ')));
    return r;
}

QVariantMap ModelBrowser::signalRow(const Signal &s) const
{
    const CharacteristicPack &p = pack();

    QStringList measureLabels;
    for (const QString &mid : s.measures)
        if (const Measure *m = p.measure(mid)) measureLabels << measureDisplayLabel(*m);
        else                                   measureLabels << mid;

    int users = 0;
    for (const Condition &c : p.conditions)
        if (c.detectedBy.contains(s.id)) ++users;

    QString directionText = tr("—");
    if (s.direction.has_value()) {
        // The measure's OWN words where it has them: three signals shipped inverted because an
        // author read High/Low against a sign convention that was unstated. Same phrasing rule as
        // the control that sets it, so the two cannot drift.
        const Measure *m = s.measures.isEmpty() ? nullptr : p.measure(s.measures.first());
        directionText = directionPhrase(*s.direction, m ? m->highMeans : QString()).label;
    }

    QVariantMap dirCell = cell(directionText, s.direction.has_value() ? QString() : QStringLiteral("dim"));
    if (s.direction.has_value())
        editable(dirCell, QStringLiteral("direction"), QStringLiteral("enum"),
                 directionName(*s.direction), directionOptionList());

    QVariantList cells;
    cells.append(cell(s.id, QString(), true));
    cells.append(cell(signalTestName(s.test)));
    cells.append(dirCell);
    cells.append(cell(measureLabels.join(QStringLiteral(" · "))));
    cells.append(cell(QString::number(users), users == 0 ? QStringLiteral("warn") : QString(), true,
                      QStringLiteral("right")));

    QVariantMap r;
    r.insert(QStringLiteral("id"), s.id);
    r.insert(QStringLiteral("type"), kSignals);
    r.insert(QStringLiteral("label"), s.id);
    r.insert(QStringLiteral("dot"), users == 0 ? QStringLiteral("watch") : QStringLiteral("good"));
    r.insert(QStringLiteral("cells"), cells);

    QVariantMap keys;
    keys.insert(QStringLiteral("name"), s.id);
    keys.insert(QStringLiteral("test"), int(s.test));
    keys.insert(QStringLiteral("direction"), directionText);
    keys.insert(QStringLiteral("measures"), measureLabels.join(QLatin1Char(' ')));
    keys.insert(QStringLiteral("usedBy"), users);
    r.insert(QStringLiteral("sortKeys"), keys);
    r.insert(QStringLiteral("searchText"),
             QStringList{ s.id, measureLabels.join(QLatin1Char(' ')), signalTestName(s.test) }
                 .join(QLatin1Char(' ')));
    return r;
}

QVariantMap ModelBrowser::edgeRow(const Edge &e) const
{
    const CharacteristicPack &p    = pack();
    const Condition          *from = p.condition(e.from);
    const Condition          *to   = p.condition(e.to);

    QVariantMap relCell = cell(edgeTypeName(e.type));
    editable(relCell, QStringLiteral("relation"), QStringLiteral("enum"), edgeTypeName(e.type),
             relationOptions());

    // Strength is a RANKING WEIGHT and never a probability — words, never a percentage, and the
    // vocabulary comes from strengthLabel() rather than being spelled here.
    QVariantMap strengthCell = cell(strengthLabel(e.strength));
    editable(strengthCell, QStringLiteral("strength"), QStringLiteral("enum"),
             strengthName(e.strength), strengthOptions());

    QVariantMap tierCell = cell(provenanceTierLabel(e.provenance.tier),
                                e.provenance.tier == ProvenanceTier::Proposed
                                    ? QStringLiteral("warn") : QString());
    editable(tierCell, QStringLiteral("tier"), QStringLiteral("enum"),
             provenanceTierName(e.provenance.tier), tierOptions());

    QVariantList cells;
    cells.append(cell(from ? from->label : e.from));
    cells.append(cell(to ? to->label : e.to));
    cells.append(relCell);
    // A symmetric relation has no strength to state. Rendering "Moderate" on a corroboration would
    // be a confident wrong claim, which is worse than a blank.
    cells.append(e.type == EdgeType::Causes ? strengthCell : cell(tr("—"), QStringLiteral("dim")));
    cells.append(tierCell);

    QVariantMap r;
    r.insert(QStringLiteral("id"), edgeId(e.from, e.to, e.type));
    r.insert(QStringLiteral("type"), kLinks);
    r.insert(QStringLiteral("label"), tr("%1 → %2").arg(from ? from->label : e.from,
                                                        to ? to->label : e.to));
    r.insert(QStringLiteral("dot"), e.type == EdgeType::Causes
                                        ? (e.strength == Strength::Strong ? QStringLiteral("good")
                                           : e.strength == Strength::Weak ? QStringLiteral("none")
                                                                          : QStringLiteral("watch"))
                                        : QStringLiteral("none"));
    r.insert(QStringLiteral("cells"), cells);
    r.insert(QStringLiteral("fromId"), e.from);
    r.insert(QStringLiteral("toId"), e.to);

    QVariantMap keys;
    keys.insert(QStringLiteral("from"), from ? from->label : e.from);
    keys.insert(QStringLiteral("to"), to ? to->label : e.to);
    keys.insert(QStringLiteral("relation"), int(e.type));
    keys.insert(QStringLiteral("strength"), int(e.strength));
    keys.insert(QStringLiteral("evidence"), int(e.provenance.tier));
    r.insert(QStringLiteral("sortKeys"), keys);
    r.insert(QStringLiteral("searchText"),
             QStringList{ from ? from->label : e.from, to ? to->label : e.to, e.from, e.to,
                          edgeTypeName(e.type) }
                 .join(QLatin1Char(' ')));
    return r;
}

QVariantList ModelBrowser::rawRows(const QString &type) const
{
    const CharacteristicPack &p = pack();
    QVariantList              out;

    if (type == kCharacteristics) {
        for (const Condition &c : p.conditions)
            if (c.observability != Observability::Latent) out.append(conditionRow(c, false));
    } else if (type == kCauses) {
        // A cause is a condition that explains something. Faults and causes are the same TYPE — the
        // pack is explicit — so this is a view over the same registry, not a second one.
        for (const Condition &c : p.conditions)
            if (!effectsOf(p, c.id).isEmpty()) out.append(conditionRow(c, true));
    } else if (type == kMeasures) {
        for (const Measure &m : p.measures) out.append(measureRow(m));
    } else if (type == kMetrics) {
        // READ-ONLY, and every cell says so by simply not being editable. A metric is produced by
        // the pipeline, not authored: its meaning, its sign convention and what it needs are facts
        // about code, and the place to change them is the code. What this view is FOR is the join —
        // metric → the measures that read it → the corridors that judge them.
        for (const MetricDescriptor *d : m_cat.all()) {
            if (!d) continue;

            QStringList readers;
            for (const Measure &meas : p.measures)
                if (meas.metricKey == d->key) readers << measureDisplayLabel(meas);

            const QStringList needs = metricNeeds(d->requirement);

            QVariantList cells;
            cells.append(cell(d->label.isEmpty() ? d->key : d->label));
            cells.append(cell(d->key, QStringLiteral("dim"), true));
            cells.append(cell(d->unit));
            cells.append(cell(d->group));
            cells.append(cell(needs.isEmpty() ? tr("nothing extra") : needs.join(QStringLiteral(" · ")),
                              needs.isEmpty() ? QStringLiteral("dim") : QString()));
            // A metric nothing reads is not a fault — the catalogue is broader than the diagnostics
            // pack — but it IS the interesting case when you are hunting for what a corridor could
            // be built on, so it is toned rather than rendered as an ordinary zero.
            cells.append(cell(QString::number(readers.size()),
                              readers.isEmpty() ? QStringLiteral("warn") : QString(), true,
                              QStringLiteral("right")));

            QVariantMap r;
            r.insert(QStringLiteral("id"), d->key);
            r.insert(QStringLiteral("type"), kMetrics);
            r.insert(QStringLiteral("label"), d->label.isEmpty() ? d->key : d->label);
            // Planned means the catalogue describes it and nothing produces it yet — the same
            // distinction the measure roadmap draws, and worth the same dot.
            r.insert(QStringLiteral("dot"), d->planned ? QStringLiteral("watch")
                                                       : QStringLiteral("good"));
            r.insert(QStringLiteral("cells"), cells);

            QVariantMap keys;
            keys.insert(QStringLiteral("name"), d->label);
            keys.insert(QStringLiteral("key"), d->key);
            keys.insert(QStringLiteral("unit"), d->unit);
            keys.insert(QStringLiteral("group"), d->group);
            keys.insert(QStringLiteral("needs"), needs.join(QLatin1Char(' ')));
            keys.insert(QStringLiteral("readBy"), readers.size());
            r.insert(QStringLiteral("sortKeys"), keys);
            r.insert(QStringLiteral("searchText"),
                     QStringList{ d->label, d->shortLabel, d->key, d->unit, d->group,
                                  d->description, d->howToRead }
                         .join(QLatin1Char(' ')));
            out.append(r);
        }
    } else if (type == kSignals) {
        for (const Signal &s : p.signalDefs) out.append(signalRow(s));
    } else if (type == kLinks) {
        for (const Edge &e : p.edges) out.append(edgeRow(e));
    } else if (type == kScreens) {
        // From the ASSEMBLY, not the shared set: the shared one reflects the file, and every surface
        // in this panel has to show the library as it would be if you saved now.
        for (const Screen &s : m_screens.screens) {
            QStringList settles;
            for (const Condition &c : p.conditions)
                if (c.screenRef == s.id) settles << c.label;

            QVariantMap nameCell = cell(s.label);
            editable(nameCell, QStringLiteral("name"), QStringLiteral("text"), s.label);
            QVariantMap regionCell = cell(s.bodyRegion);
            editable(regionCell, QStringLiteral("region"), QStringLiteral("text"), s.bodyRegion);
            QVariantMap passCell = cell(s.passCriterion,
                                        s.passCriterion.isEmpty() ? QStringLiteral("warn") : QString());
            editable(passCell, QStringLiteral("passCriterion"), QStringLiteral("text"),
                     s.passCriterion);

            QVariantList cells;
            cells.append(nameCell);
            cells.append(regionCell);
            cells.append(passCell);
            // Settles is a JOIN, held on the condition — so it is shown and not typed into. It is
            // edited from the inspector, where the add is a type-ahead over legal candidates.
            cells.append(cell(settles.join(QStringLiteral(" · ")),
                              settles.isEmpty() ? QStringLiteral("dim") : QString()));
            cells.append(cell(QString::number(settles.size()),
                              settles.isEmpty() ? QStringLiteral("warn") : QString(), true,
                              QStringLiteral("right")));

            QVariantMap r;
            r.insert(QStringLiteral("id"), s.id);
            r.insert(QStringLiteral("type"), kScreens);
            r.insert(QStringLiteral("label"), s.label);
            // A screen that settles nothing is not a fault — it may be newly authored — but it IS
            // what an author is hunting for, so it is toned rather than left as an ordinary row.
            r.insert(QStringLiteral("dot"), settles.isEmpty() ? QStringLiteral("watch")
                                                              : QStringLiteral("good"));
            r.insert(QStringLiteral("cells"), cells);
            QVariantMap keys;
            keys.insert(QStringLiteral("name"), s.label);
            keys.insert(QStringLiteral("region"), s.bodyRegion);
            keys.insert(QStringLiteral("pass"), s.passCriterion);
            keys.insert(QStringLiteral("settles"), settles.join(QLatin1Char(' ')));
            keys.insert(QStringLiteral("settlesCount"), settles.size());
            r.insert(QStringLiteral("sortKeys"), keys);
            r.insert(QStringLiteral("searchText"),
                     QStringList{ s.label, s.id, s.bodyRegion, s.protocol, s.passCriterion }
                         .join(QLatin1Char(' ')));
            out.append(r);
        }
    } else if (type == kDrills) {
        for (const Drill &d : m_drills.drills) {
            QStringList answers;
            for (const Condition &c : p.conditions)
                if (c.drills.contains(d.id)) answers << c.label;

            QVariantMap nameCell = cell(d.label);
            editable(nameCell, QStringLiteral("name"), QStringLiteral("text"), d.label);
            QVariantMap targetsCell = cell(d.targets,
                                           d.targets.isEmpty() ? QStringLiteral("warn") : QString());
            editable(targetsCell, QStringLiteral("targets"), QStringLiteral("text"), d.targets);
            // Typed as a ` · `-joined line, split back on the way in. A chip editor in a table cell
            // would be a control nothing else in the table has, for a field most drills leave empty.
            QVariantMap equipCell = cell(d.equipment.join(QStringLiteral(" · ")),
                                         d.equipment.isEmpty() ? QStringLiteral("dim") : QString());
            editable(equipCell, QStringLiteral("equipment"), QStringLiteral("text"),
                     d.equipment.join(QStringLiteral(" · ")));

            QVariantList cells;
            cells.append(nameCell);
            cells.append(targetsCell);
            cells.append(equipCell);
            cells.append(cell(answers.join(QStringLiteral(" · ")),
                              answers.isEmpty() ? QStringLiteral("dim") : QString()));
            cells.append(cell(QString::number(answers.size()),
                              answers.isEmpty() ? QStringLiteral("warn") : QString(), true,
                              QStringLiteral("right")));

            QVariantMap r;
            r.insert(QStringLiteral("id"), d.id);
            r.insert(QStringLiteral("type"), kDrills);
            r.insert(QStringLiteral("label"), d.label);
            r.insert(QStringLiteral("dot"), answers.isEmpty() ? QStringLiteral("watch")
                                                              : QStringLiteral("good"));
            r.insert(QStringLiteral("cells"), cells);
            QVariantMap keys;
            keys.insert(QStringLiteral("name"), d.label);
            keys.insert(QStringLiteral("targets"), d.targets);
            keys.insert(QStringLiteral("equipment"), d.equipment.join(QLatin1Char(' ')));
            keys.insert(QStringLiteral("answers"), answers.join(QLatin1Char(' ')));
            keys.insert(QStringLiteral("answersCount"), answers.size());
            r.insert(QStringLiteral("sortKeys"), keys);
            r.insert(QStringLiteral("searchText"),
                     QStringList{ d.label, d.id, d.instruction, d.targets }.join(QLatin1Char(' ')));
            out.append(r);
        }
    } else if (type == kReferences) {
        for (const Reference &ref : sharedReferenceSet().references) {
            // The citation joins against ANY of the three identifiers: a handful of journals issue
            // no DOI, a book never had one, and a strict DOI match would render "cited by nothing"
            // on a paper the library leans on.
            const auto cites = [&ref](const QString &citation) {
                if (citation.isEmpty()) return false;
                return (!ref.doi.isEmpty() && citation == ref.doi)
                    || (!ref.pmid.isEmpty() && citation == ref.pmid)
                    || (!ref.isbn.isEmpty() && citation == ref.isbn);
            };

            QStringList    supports;
            ProvenanceTier best  = ProvenanceTier::Proposed;
            bool           anyClaim = false;
            for (const Edge &e : p.edges) {
                if (!cites(e.provenance.citation)) continue;
                const Condition *f = p.condition(e.from);
                const Condition *t = p.condition(e.to);
                supports << tr("%1 → %2").arg(f ? f->label : e.from, t ? t->label : e.to);
                best     = anyClaim ? std::max(best, e.provenance.tier) : e.provenance.tier;
                anyClaim = true;
            }
            for (const Condition &c : p.conditions) {
                if (!cites(c.provenance.citation)) continue;
                supports << c.label;
                best     = anyClaim ? std::max(best, c.provenance.tier) : c.provenance.tier;
                anyClaim = true;
            }

            QVariantList cells;
            cells.append(cell(ref.title.isEmpty() ? ref.id : ref.title));
            cells.append(cell(ref.identifierLabel(), QStringLiteral("dim"), true));
            cells.append(cell(supports.join(QStringLiteral(" · ")),
                              supports.isEmpty() ? QStringLiteral("dim") : QString()));
            cells.append(cell(anyClaim ? provenanceTierLabel(best)
                                       : (ref.generalReading ? tr("General reading") : tr("Uncited")),
                              anyClaim ? QString() : QStringLiteral("dim")));

            QVariantMap r;
            r.insert(QStringLiteral("id"), ref.id);
            r.insert(QStringLiteral("type"), kReferences);
            r.insert(QStringLiteral("label"), ref.title.isEmpty() ? ref.id : ref.title);
            r.insert(QStringLiteral("cells"), cells);
            QVariantMap keys;
            keys.insert(QStringLiteral("name"), ref.title);
            keys.insert(QStringLiteral("identifier"), ref.identifierLabel());
            // Ranked by how much of the library each one holds up. That ordering IS the argument:
            // the paper four claims rest on is a different kind of object from the one cited once.
            keys.insert(QStringLiteral("supports"), supports.size());
            keys.insert(QStringLiteral("tier"), anyClaim ? int(best) : -1);
            r.insert(QStringLiteral("sortKeys"), keys);
            r.insert(QStringLiteral("searchText"),
                     QStringList{ ref.title, ref.authors, ref.journal, ref.publisher, ref.id,
                                  ref.identifierLabel(), ref.establishes }
                         .join(QLatin1Char(' ')));
            out.append(r);
        }
    } else if (type == kCorridors) {
        // Every corridor in the ASSEMBLED set — shipped and yours together, because "what grades
        // this measure" is one question and splitting it by layer would make the reader do the
        // merge in their head. Which layer a row came from is the Source column's job.
        for (const Norm &n : m_norms->norms().norms) {
            const Measure *meas = p.measure(n.measureId);
            const QString  label = meas ? measureDisplayLabel(*meas) : n.measureId;

            // Rendered at the unit's own precision. Six significant figures on a quantity nobody
            // can measure past the whole number is a column of noise that reads as data.
            const int dp = corridorPrecisionFor(n.unit.isEmpty() ? (meas ? meas->unit : QString())
                                                                 : n.unit).decimals;

            QVariantMap muCell = cell(QString::number(n.mu, 'f', dp), QString(), true,
                                      QStringLiteral("right"));
            editable(muCell, QStringLiteral("mu"), QStringLiteral("number"), n.mu);

            QVariantMap loCell = cell(QString::number(n.sigmaLo, 'f', dp), QString(), true,
                                      QStringLiteral("right"));
            editable(loCell, QStringLiteral("sigmaLo"), QStringLiteral("number"), n.sigmaLo);

            QVariantMap hiCell = cell(QString::number(n.sigmaHi, 'f', dp), QString(), true,
                                      QStringLiteral("right"));
            editable(hiCell, QStringLiteral("sigmaHi"), QStringLiteral("number"), n.sigmaHi);

            QVariantMap unitCell = cell(n.unit);
            editable(unitCell, QStringLiteral("unit"), QStringLiteral("text"), n.unit);

            QVariantMap srcCell = cell(normSourceLabel(n.source));
            editable(srcCell, QStringLiteral("source"), QStringLiteral("enum"),
                     normSourceName(n.source), normSourceOptions());

            const ContextNode *node = m_norms->contexts().node(n.contextId);

            QVariantList cells;
            cells.append(cell(label));
            cells.append(cell(node ? node->label : n.contextId));
            cells.append(muCell);
            cells.append(loCell);
            cells.append(hiCell);
            cells.append(unitCell);
            cells.append(srcCell);

            QVariantMap r;
            r.insert(QStringLiteral("id"), corridorId(n.measureId, n.contextId));
            r.insert(QStringLiteral("type"), kCorridors);
            r.insert(QStringLiteral("label"), tr("%1 · %2").arg(label,
                                                                node ? node->label : n.contextId));
            r.insert(QStringLiteral("measureId"), n.measureId);
            r.insert(QStringLiteral("contextId"), n.contextId);
            // A heuristic figure is an authored starting point that everybody expects to move; a
            // seated or published one has been earned. Toning them alike would hide which corridors
            // are still guesses.
            r.insert(QStringLiteral("dot"), n.source == NormSource::Heuristic
                                                ? QStringLiteral("watch") : QStringLiteral("good"));
            r.insert(QStringLiteral("cells"), cells);

            QVariantMap keys;
            keys.insert(QStringLiteral("name"), label);
            keys.insert(QStringLiteral("context"), node ? node->label : n.contextId);
            keys.insert(QStringLiteral("mu"), n.mu);
            keys.insert(QStringLiteral("sigmaLo"), n.sigmaLo);
            keys.insert(QStringLiteral("sigmaHi"), n.sigmaHi);
            keys.insert(QStringLiteral("unit"), n.unit);
            keys.insert(QStringLiteral("source"), int(n.source));
            r.insert(QStringLiteral("sortKeys"), keys);
            r.insert(QStringLiteral("searchText"),
                     QStringList{ label, n.measureId, n.contextId, n.unit, n.citation }
                         .join(QLatin1Char(' ')));
            out.append(r);
        }
    } else if (type == kHealth) {
        // What is wrong with THE DRAFT, not with the file. That is the whole reason the working copy
        // is assembled: a validation strip that graded the saved pack would stay green through an
        // edit that broke the library.
        std::vector<ValidationIssue> issues;
        for (const ValidationIssue &i : m_assembled->report().issues) issues.push_back(i);
        if (m_norms) {
            for (const ValidationIssue &i : m_norms->report().issues)
                if (i.severity == IssueSeverity::Warning) issues.push_back(i);
            for (const ValidationIssue &i : diagnosticsHealth(p, *m_norms, m_cat))
                issues.push_back(i);
        }

        int n = 0;
        for (const ValidationIssue &i : issues) {
            const bool err = i.severity == IssueSeverity::Error;
            QVariantList cells;
            cells.append(cell(err ? tr("Error") : tr("Warning"),
                              err ? QStringLiteral("error") : QStringLiteral("warn")));
            cells.append(cell(i.code, QStringLiteral("dim"), true));
            cells.append(cell(i.subject, QString(), true));
            cells.append(cell(i.message));

            QVariantMap r;
            // Health rows are findings, not objects, so they carry a positional id. The SUBJECT is
            // what the strip filters the table by, and it travels separately for exactly that.
            r.insert(QStringLiteral("id"), QStringLiteral("health:%1").arg(n++));
            r.insert(QStringLiteral("type"), kHealth);
            r.insert(QStringLiteral("label"), i.message);
            r.insert(QStringLiteral("subject"), i.subject);
            r.insert(QStringLiteral("code"), i.code);
            r.insert(QStringLiteral("severity"), err ? QStringLiteral("error") : QStringLiteral("warning"));
            r.insert(QStringLiteral("dot"), err ? QStringLiteral("fault") : QStringLiteral("watch"));
            r.insert(QStringLiteral("cells"), cells);
            QVariantMap keys;
            keys.insert(QStringLiteral("severity"), err ? 0 : 1);
            keys.insert(QStringLiteral("code"), i.code);
            keys.insert(QStringLiteral("subject"), i.subject);
            keys.insert(QStringLiteral("message"), i.message);
            r.insert(QStringLiteral("sortKeys"), keys);
            r.insert(QStringLiteral("searchText"),
                     QStringList{ i.code, i.subject, i.message }.join(QLatin1Char(' ')));
            out.append(r);
        }
    }

    // Provenance every row carries, whatever type it is: whose content this is, and whether it is
    // unsaved. The Source column and the dirty gutter both read these, and deriving them here means
    // no type has to remember to.
    const QSet<QString> &dirtySet = dirtyIds();
    for (QVariant &v : out) {
        QVariantMap  r      = v.toMap();
        const QString id    = r.value(QStringLiteral("id")).toString();
        const QString src   = sourceOf(id);
        r.insert(QStringLiteral("source"), src);
        r.insert(QStringLiteral("sourceLabel"),
                 src == QStringLiteral("yours")   ? tr("Yours")
                 : src == QStringLiteral("both")  ? tr("Yours, over shipped")
                                                  : tr("Shipped"));
        r.insert(QStringLiteral("dirty"), dirtySet.contains(id));
        v = r;
    }
    return out;
}

QVariantList ModelBrowser::rows(const QString &type, const QVariantMap &filters) const
{
    QVariantList out = rawRows(type);

    // ── Facets ──────────────────────────────────────────────────────────────
    const QVariantMap facetFilters = filters.value(QStringLiteral("facets")).toMap();
    if (!facetFilters.isEmpty()) {
        QVariantList kept;
        for (const QVariant &v : out) {
            const QVariantMap r    = v.toMap();
            const QVariantMap keys = r.value(QStringLiteral("sortKeys")).toMap();
            bool              ok   = true;
            for (auto it = facetFilters.constBegin(); it != facetFilters.constEnd() && ok; ++it) {
                const QStringList wanted = it.value().toStringList();
                if (wanted.isEmpty()) continue;
                // Facet values are matched against the CELL TEXT of the facet's column, which is
                // what the rail counted — one source for the count and the filter, so a chip can
                // never say 12 and return 9.
                const QVariantList cells = r.value(QStringLiteral("cells")).toList();
                const QVariantList cols  = columns(type);
                QString            value;
                for (int i = 0; i < cols.size() && i < cells.size(); ++i)
                    if (cols.at(i).toMap().value(QStringLiteral("key")).toString() == it.key())
                        value = cells.at(i).toMap().value(QStringLiteral("text")).toString();
                if (value.isEmpty() && keys.contains(it.key()))
                    value = keys.value(it.key()).toString();
                ok = wanted.contains(value);
            }
            if (ok) kept.append(v);
        }
        out = kept;
    }

    // ── Explicit id set (the validation strip's "show me the offending rows") ─
    const QStringList ids = filters.value(QStringLiteral("ids")).toStringList();
    if (!ids.isEmpty()) {
        QVariantList kept;
        for (const QVariant &v : out)
            if (ids.contains(v.toMap().value(QStringLiteral("id")).toString())) kept.append(v);
        out = kept;
    }

    // ── Search ──────────────────────────────────────────────────────────────
    const QString search = filters.value(QStringLiteral("search")).toString().trimmed();
    if (!search.isEmpty()) {
        QVariantList kept;
        for (const QVariant &v : out)
            if (matches(v.toMap().value(QStringLiteral("searchText")).toString(), search))
                kept.append(v);
        out = kept;
    }

    // ── Sort ────────────────────────────────────────────────────────────────
    //
    // The default per type answers the question the author actually arrived with, rather than being
    // alphabetical for its own sake.
    QString sortKey   = filters.value(QStringLiteral("sort")).toString();
    bool    descending = filters.value(QStringLiteral("descending")).toBool();
    const bool explicitSort = !sortKey.isEmpty();

    if (!explicitSort) {
        if (type == kMeasures) {
            // Status, then least-read: this is what surfaces the 42 measures nothing reads without
            // the author having to know to ask.
            sortKey = QStringLiteral("status");
        } else if (type == kReferences) {
            sortKey = QStringLiteral("supports");
            descending = true;
        } else if (type == kHealth) {
            sortKey = QStringLiteral("severity");
        } else if (type == kCauses) {
            sortKey = QStringLiteral("explains");
            descending = true;
        } else if (type == kMetrics) {
            // By group, the way the catalogue itself is organised.
            sortKey = QStringLiteral("group");
        } else if (type == kCorridors) {
            // By measure, so a measure's five contexts sit together — the comparison an author
            // actually makes is driver against iron against wedge, not one corridor in isolation.
            sortKey = QStringLiteral("name");
        } else if (type == kScreens) {
            // ASCENDING, so the screen that settles nothing is the first row an author sees — the
            // same principle as sorting measures by least-read. It was descending, which put the
            // well-connected screens at the top and buried the ones needing work, and now that these
            // are writable that ordering would hide exactly the work the panel exists for.
            sortKey = QStringLiteral("settlesCount");
        } else if (type == kDrills) {
            sortKey = QStringLiteral("answersCount");
        } else {
            sortKey = QStringLiteral("group");
        }
    }

    const bool measuresDefault = (type == kMeasures && !explicitSort);
    const bool charsDefault    = (type == kCharacteristics && !explicitSort);

    std::stable_sort(out.begin(), out.end(), [&](const QVariant &av, const QVariant &bv) {
        const QVariantMap a = av.toMap().value(QStringLiteral("sortKeys")).toMap();
        const QVariantMap b = bv.toMap().value(QStringLiteral("sortKeys")).toMap();

        // ANY numeric type, not just Int. This tested `typeId() == Int` and nothing else, so every
        // count inserted as a container's `.size()` — which is a qsizetype, i.e. LongLong — fell
        // through to the STRING branch and sorted lexicographically: the bibliography read
        // 7, 6, 6, 4, 3, 2, 2, 12, 1 …, with the paper holding up twelve claims buried between the
        // twos and the ones. Corridor `mu` is a double and sorted the same wrong way.
        //
        // Compared as double so an int, a size and a real all order against each other correctly;
        // no sort key in this file is large enough for the mantissa to matter.
        auto numeric = [](const QVariant &v) {
            switch (v.typeId()) {
            case QMetaType::Int:      case QMetaType::UInt:
            case QMetaType::LongLong: case QMetaType::ULongLong:
            case QMetaType::Double:   case QMetaType::Float:  return true;
            default:                                          return false;
            }
        };
        auto compare = [&numeric](const QVariant &x, const QVariant &y) -> int {
            if (numeric(x) || numeric(y)) {
                const double xi = x.toDouble(), yi = y.toDouble();
                return qFuzzyCompare(xi, yi) ? 0 : (xi < yi ? -1 : 1);
            }
            return QString::compare(x.toString(), y.toString(), Qt::CaseInsensitive);
        };

        int c = compare(a.value(sortKey), b.value(sortKey));
        if (descending) c = -c;
        if (c != 0) return c < 0;

        // Tie-breaks that make the default sorts mean what they say. Least-read second, so the
        // measures column reads "status, then least-read" as a single ordering rather than as a
        // status sort with an arbitrary tail.
        if (measuresDefault) {
            const int ra = a.value(QStringLiteral("readBy")).toInt();
            const int rb = b.value(QStringLiteral("readBy")).toInt();
            if (ra != rb) return ra < rb;
        }
        // Group → axis pair → label: the two tails of one measure land adjacent instead of reading
        // as two near-duplicate faults several rows apart.
        if (charsDefault) {
            const int ax = QString::compare(a.value(QStringLiteral("axis")).toString(),
                                            b.value(QStringLiteral("axis")).toString());
            if (ax != 0) return ax < 0;
        }
        return QString::compare(a.value(QStringLiteral("name")).toString(),
                                b.value(QStringLiteral("name")).toString(),
                                Qt::CaseInsensitive) < 0;
    });

    return out;
}

// ── Facets ──────────────────────────────────────────────────────────────────

QVariantList ModelBrowser::facets(const QString &type) const
{
    // Which column each type facets on. Deliberately few: a rail of every column is a filter nobody
    // reads. These are the questions authors actually arrive with.
    QStringList keys;
    if (type == kCharacteristics)  keys = { QStringLiteral("group"), QStringLiteral("reach"),
                                            QStringLiteral("evidence") };
    else if (type == kCauses)      keys = { QStringLiteral("group"), QStringLiteral("reach"),
                                            QStringLiteral("evidence") };
    else if (type == kMeasures)    keys = { QStringLiteral("status"), QStringLiteral("unit") };
    else if (type == kMetrics)     keys = { QStringLiteral("group"), QStringLiteral("unit") };
    else if (type == kSignals)     keys = { QStringLiteral("test"), QStringLiteral("direction") };
    else if (type == kLinks)       keys = { QStringLiteral("relation"), QStringLiteral("strength"),
                                            QStringLiteral("evidence") };
    else if (type == kScreens)     keys = { QStringLiteral("region") };
    else if (type == kReferences)  keys = { QStringLiteral("tier") };
    else if (type == kCorridors)   keys = { QStringLiteral("context"), QStringLiteral("source"),
                                            QStringLiteral("unit") };
    else if (type == kHealth)      keys = { QStringLiteral("severity"), QStringLiteral("code") };
    else return {};

    const QVariantList all  = rawRows(type);
    const QVariantList cols = columns(type);

    QVariantList out;
    for (const QString &key : keys) {
        QString title;
        int     colIndex = -1;
        for (int i = 0; i < cols.size(); ++i)
            if (cols.at(i).toMap().value(QStringLiteral("key")).toString() == key) {
                title    = cols.at(i).toMap().value(QStringLiteral("title")).toString();
                colIndex = i;
            }
        if (colIndex < 0) continue;

        // Counted in first-seen order rather than sorted, so a facet over an ORDERED vocabulary
        // (groups are the swing's own order) reads in that order rather than alphabetically.
        QStringList         seen;
        QHash<QString, int> counts;
        for (const QVariant &v : all) {
            const QVariantList cells = v.toMap().value(QStringLiteral("cells")).toList();
            if (colIndex >= cells.size()) continue;
            const QString value = cells.at(colIndex).toMap().value(QStringLiteral("text")).toString();
            if (value.isEmpty()) continue;
            if (!counts.contains(value)) seen << value;
            counts[value] += 1;
        }
        if (seen.size() < 2) continue;   // a facet with one value filters nothing

        QVariantList options;
        for (const QString &value : seen) {
            QVariantMap o;
            o.insert(QStringLiteral("value"), value);
            o.insert(QStringLiteral("label"), value);
            o.insert(QStringLiteral("count"), counts.value(value));
            options.append(o);
        }

        QVariantMap f;
        f.insert(QStringLiteral("key"), key);
        f.insert(QStringLiteral("label"), title);
        f.insert(QStringLiteral("options"), options);
        out.append(f);
    }
    return out;
}

// ── Cross-type search ───────────────────────────────────────────────────────

QVariantList ModelBrowser::searchAll(const QString &query) const
{
    const QString q = query.trimmed();
    if (q.isEmpty()) return {};

    // Every registry at once. "hip" has to return characteristics, measures, signals, screens and
    // drills together — that is the whole point, and it cannot be assembled in QML because no one
    // of these lists can see the others.
    const QStringList order = { kCharacteristics, kCauses, kMetrics, kMeasures, kSignals, kLinks,
                                kScreens, kDrills, kReferences };

    QVariantList out;
    QSet<QString> seen;   // a cause is also a characteristic; one object, one row
    for (const QString &type : order) {
        for (const QVariant &v : rawRows(type)) {
            const QVariantMap r = v.toMap();
            if (!matches(r.value(QStringLiteral("searchText")).toString(), q)) continue;
            const QString id = r.value(QStringLiteral("id")).toString();
            if (seen.contains(id)) continue;
            seen.insert(id);

            // One flat shape with a Type column, per the brief's 4b: type · name · identifier ·
            // connected to · links · status. Cells are REBUILT rather than reused — the per-type
            // columns mean nothing in a list that mixes types, and carrying them over would render
            // a measure's unit under a heading saying "Connected to".
            const QVariantMap extras = searchExtras(type, id);

            QVariantList cells;
            cells.append(cell(typeLabelFor(type), QStringLiteral("dim")));
            cells.append(cell(r.value(QStringLiteral("label")).toString()));
            cells.append(cell(id, QStringLiteral("dim"), true));
            cells.append(cell(extras.value(QStringLiteral("connected")).toString(),
                              QStringLiteral("dim")));
            cells.append(cell(extras.value(QStringLiteral("links")).toString(), QString(), true,
                              QStringLiteral("right")));
            cells.append(cell(extras.value(QStringLiteral("status")).toString(),
                              extras.value(QStringLiteral("statusTone")).toString()));

            QVariantMap row = r;
            row.insert(QStringLiteral("resultType"), type);
            row.insert(QStringLiteral("cells"), cells);
            out.append(row);
        }
    }
    return out;
}

// ── Blast radius ────────────────────────────────────────────────────────────

int ModelBrowser::measureUsers(const QString &measureId) const
{
    return measureUserRows(measureId).size();
}

// The inspector row helpers, declared here rather than beside inspect() because measureUserRows()
// below is the first caller. Every row the inspector renders goes through hubRow(), so none of
// them can skip a key the delegate binds — see the row-contract case in model_browser_test.

namespace {

QVariantMap hubRow(const QString &type, const QString &id, const QString &label,
                   const QString &detail = QString(), const QString &tone = QString(),
                   bool navigable = true)
{
    QVariantMap r;
    r.insert(QStringLiteral("type"), type);
    r.insert(QStringLiteral("id"), id);
    r.insert(QStringLiteral("label"), label);
    r.insert(QStringLiteral("detail"), detail);
    r.insert(QStringLiteral("tone"), tone);
    r.insert(QStringLiteral("navigable"), navigable);
    return r;
}

QVariantMap section(const QString &title, const QVariantList &rows,
                    const QString &note = QString(), const QString &kind = QStringLiteral("list"),
                    const QString &action = QString())
{
    QVariantMap s;
    s.insert(QStringLiteral("title"), title);
    s.insert(QStringLiteral("kind"), kind);
    s.insert(QStringLiteral("note"), note);
    s.insert(QStringLiteral("rows"), rows);
    s.insert(QStringLiteral("count"), rows.size());
    // What can be ADDED to or REMOVED from this section, as a stable key. The view used to work
    // this out by comparing the translated title, which is a control that vanishes in any locale
    // but this one.
    s.insert(QStringLiteral("action"), action);
    return s;
}

// One editable FIELD, in the same grammar a table cell uses — `field`, `kind`, `value`, `options` —
// so the inspector's editors and the table's are the same three controls fed the same way, and a
// field can never be editable in one surface and inert in the other.
//
// `kind` is text | number | enum | prose. Prose is text that wants more than one line; it exists
// only here, because a paragraph has never belonged in a table cell.
QVariantMap fieldRow(const QString &field, const QString &label, const QString &kind,
                     const QVariant &value, const QVariantList &options = {},
                     const QString &hint = QString())
{
    QVariantMap r;
    r.insert(QStringLiteral("type"), QString());
    r.insert(QStringLiteral("id"), field);
    r.insert(QStringLiteral("label"), label);
    r.insert(QStringLiteral("detail"), hint);
    r.insert(QStringLiteral("tone"), QString());
    r.insert(QStringLiteral("navigable"), false);
    r.insert(QStringLiteral("field"), field);
    r.insert(QStringLiteral("kind"), kind);
    r.insert(QStringLiteral("value"), value);
    r.insert(QStringLiteral("options"), options);
    return r;
}

// One record, as a citation reads. A book, a chapter and a paper take different shapes and the
// difference is not cosmetic — a publisher rendered where a journal goes is a small, checkable lie,
// which is why the two fields were separated in the first place (reference_pack.h).
QString formatCitation(const Reference &ref)
{
    QStringList bits;
    if (!ref.authors.isEmpty()) bits << ref.authors;
    if (ref.year > 0)           bits << QStringLiteral("(%1)").arg(ref.year);
    if (!ref.title.isEmpty())   bits << ref.title + QLatin1Char('.');

    // The container: a chapter's book, else the periodical, else the publisher. Never two of them —
    // a record carrying both raises `referenceContainerConflict` at load.
    if (!ref.containerTitle.isEmpty()) {
        QString in = QObject::tr("In: %1").arg(ref.containerTitle);
        if (!ref.editor.isEmpty()) in += QObject::tr(" (%1, ed.)").arg(ref.editor);
        bits << in + QLatin1Char('.');
    } else if (!ref.journal.isEmpty()) {
        QString j = ref.journal;
        if (!ref.volume.isEmpty()) {
            j += QLatin1Char(' ') + ref.volume;
            if (!ref.issue.isEmpty()) j += QStringLiteral("(%1)").arg(ref.issue);
        }
        if (!ref.pages.isEmpty()) j += QStringLiteral(": %1").arg(ref.pages);
        bits << j + QLatin1Char('.');
    } else if (!ref.publisher.isEmpty()) {
        bits << ref.publisher + QLatin1Char('.');
    }

    if (!ref.identifierLabel().isEmpty()) bits << ref.identifierLabel();
    return bits.join(QLatin1Char(' '));
}

// A prose section — one block of text rather than a list of links.
QVariantMap prose(const QString &title, const QString &text, const QString &tone = QString())
{
    QVariantList rows;
    if (!text.isEmpty()) rows.append(hubRow(QString(), QString(), text, QString(), tone, false));
    return section(title, rows, QString(), QStringLiteral("prose"));
}

} // namespace

QVariantList ModelBrowser::measureUserRows(const QString &measureId) const
{
    const CharacteristicPack &p = pack();

    QStringList signalsUsing;
    for (const Signal &s : p.signalDefs)
        if (s.measures.contains(measureId)) signalsUsing << s.id;

    QVariantList out;
    for (const Condition &c : p.conditions) {
        bool uses = false;
        for (const QString &sid : c.detectedBy)
            if (signalsUsing.contains(sid)) uses = true;
        if (!uses) continue;

        // hubRow(), not a hand-built map. It was missing `tone`, which the inspector delegate binds
        // on every row whichever section kind is showing — invisible on screen, four warnings per
        // repaint on the console.
        out.append(hubRow(kCharacteristics, c.id, c.label, conditionGroupLabel(c.group)));
    }
    return out;
}

QVariantMap ModelBrowser::searchExtras(const QString &type, const QString &id) const
{
    const CharacteristicPack &p = pack();

    QVariantMap out;
    out.insert(QStringLiteral("connected"), QString());
    out.insert(QStringLiteral("links"), QStringLiteral("0"));
    out.insert(QStringLiteral("status"), QString());
    out.insert(QStringLiteral("statusTone"), QString());

    if (type == kCharacteristics || type == kCauses) {
        const Condition *c = p.condition(id);
        if (!c) return out;
        QStringList near;
        for (const QString &cid : causesOf(p, id))
            if (const Condition *o = p.condition(cid)) near << o->label;
        for (const QString &eid : effectsOf(p, id))
            if (const Condition *o = p.condition(eid)) near << o->label;
        out.insert(QStringLiteral("connected"), near.join(QStringLiteral(" · ")));
        out.insert(QStringLiteral("links"), QString::number(near.size()));
        out.insert(QStringLiteral("status"), reachLabel(c->confirmedBy));
    } else if (type == kMeasures) {
        const Measure *m = p.measure(id);
        if (!m) return out;
        QStringList users;
        for (const QVariant &v : measureUserRows(id))
            users << v.toMap().value(QStringLiteral("label")).toString();
        out.insert(QStringLiteral("connected"), users.join(QStringLiteral(" · ")));
        out.insert(QStringLiteral("links"), QString::number(users.size()));
        out.insert(QStringLiteral("status"), measureStatusLabel(m->status));
        out.insert(QStringLiteral("statusTone"), statusTone(m->status));
    } else if (type == kSignals) {
        QStringList users;
        for (const Condition &c : p.conditions)
            if (c.detectedBy.contains(id)) users << c.label;
        out.insert(QStringLiteral("connected"), users.join(QStringLiteral(" · ")));
        out.insert(QStringLiteral("links"), QString::number(users.size()));
    } else if (type == kLinks) {
        QString from, to; EdgeType t = EdgeType::Causes;
        if (splitEdgeId(id, from, to, t)) {
            const Condition *f = p.condition(from);
            const Condition *o = p.condition(to);
            out.insert(QStringLiteral("connected"),
                       tr("%1 → %2").arg(f ? f->label : from, o ? o->label : to));
            out.insert(QStringLiteral("links"), QStringLiteral("2"));
            out.insert(QStringLiteral("status"), edgeTypeName(t));
        }
    } else if (type == kScreens) {
        QStringList settles;
        for (const Condition &c : p.conditions)
            if (c.screenRef == id) settles << c.label;
        out.insert(QStringLiteral("connected"), settles.join(QStringLiteral(" · ")));
        out.insert(QStringLiteral("links"), QString::number(settles.size()));
    } else if (type == kDrills) {
        QStringList answers;
        for (const Condition &c : p.conditions)
            if (c.drills.contains(id)) answers << c.label;
        out.insert(QStringLiteral("connected"), answers.join(QStringLiteral(" · ")));
        out.insert(QStringLiteral("links"), QString::number(answers.size()));
    } else if (type == kMetrics) {
        const MetricDescriptor *d = m_cat.descriptor(id);
        if (!d) return out;
        QStringList readers;
        for (const Measure &meas : p.measures)
            if (meas.metricKey == id) readers << measureDisplayLabel(meas);
        out.insert(QStringLiteral("connected"), readers.join(QStringLiteral(" · ")));
        out.insert(QStringLiteral("links"), QString::number(readers.size()));
        out.insert(QStringLiteral("status"), d->planned ? tr("Planned") : tr("Live"));
        out.insert(QStringLiteral("statusTone"), d->planned ? QStringLiteral("watch")
                                                            : QStringLiteral("good"));
    } else if (type == kReferences) {
        const Reference *ref = sharedReferenceSet().reference(id);
        if (!ref) return out;
        const auto cites = [ref](const QString &citation) {
            if (citation.isEmpty()) return false;
            return (!ref->doi.isEmpty() && citation == ref->doi)
                || (!ref->pmid.isEmpty() && citation == ref->pmid)
                || (!ref->isbn.isEmpty() && citation == ref->isbn);
        };
        QStringList supports;
        for (const Edge &e : p.edges)
            if (cites(e.provenance.citation)) {
                const Condition *f = p.condition(e.from);
                const Condition *o = p.condition(e.to);
                supports << tr("%1 → %2").arg(f ? f->label : e.from, o ? o->label : e.to);
            }
        for (const Condition &c : p.conditions)
            if (cites(c.provenance.citation)) supports << c.label;
        out.insert(QStringLiteral("connected"), supports.join(QStringLiteral(" · ")));
        out.insert(QStringLiteral("links"), QString::number(supports.size()));
    }
    return out;
}

// ── The inspector: a relationship hub ───────────────────────────────────────
//
// Its job is that EVERY related object is one click away. A property sheet lists fields; this lists
// the chain — which is the thing the old panel made you leave the page to follow.



// ── Every writable field of one object ──────────────────────────────────────
//
// THE list. The inspector renders exactly these and writes them back through setField(), so the
// answer to "what can I edit" is one list per type rather than one per surface — which is how the
// panel ended up with nine fields that setField() accepted and nothing on screen could reach.
//
// Order is the order an author reads: what it is called, what kind of thing it is, then the prose
// that explains it, then the provenance that justifies it.
QVariantList ModelBrowser::fieldsOf(const QString &type, const QString &id) const
{
    const CharacteristicPack &p = pack();
    QVariantList              f;

    if (type == kCharacteristics || type == kCauses) {
        const Condition *c = p.condition(id);
        if (!c) return f;
        f.append(fieldRow(QStringLiteral("label"), tr("Name"), QStringLiteral("text"), c->label));
        f.append(fieldRow(QStringLiteral("group"), tr("Group"), QStringLiteral("enum"),
                          conditionGroupName(c->group), groupOptions()));
        f.append(fieldRow(QStringLiteral("reach"), tr("How it is reached"), QStringLiteral("enum"),
                          confirmedByName(c->confirmedBy), reachOptions(),
                          reachHint(c->confirmedBy)));
        f.append(fieldRow(QStringLiteral("state"), tr("State"), QStringLiteral("enum"),
                          conditionStateName(c->state), stateOptions()));
        f.append(fieldRow(QStringLiteral("aliases"), tr("Also called"), QStringLiteral("text"),
                          c->aliases.join(QStringLiteral(", ")),
                          {}, tr("comma separated — the words a golfer was taught")));
        f.append(fieldRow(QStringLiteral("consequence"), tr("What it costs"),
                          QStringLiteral("prose"), c->consequence.text()));
        f.append(fieldRow(QStringLiteral("injuryNote"), tr("Injury note"), QStringLiteral("prose"),
                          c->injuryNote.text()));
        f.append(fieldRow(QStringLiteral("tier"), tr("Evidence"), QStringLiteral("enum"),
                          provenanceTierName(c->provenance.tier), tierOptions(),
                          citationRequired(c->provenance.tier) ? tr("needs a citation")
                                                               : QString()));
        f.append(fieldRow(QStringLiteral("citation"), tr("Citation"), QStringLiteral("text"),
                          c->provenance.citation, {}, tr("DOI, PMID or ISBN")));

    } else if (type == kMeasures) {
        const Measure *m = p.measure(id);
        if (!m) return f;
        f.append(fieldRow(QStringLiteral("label"), tr("Name"), QStringLiteral("text"), m->label,
                          {}, m->label.isEmpty() ? tr("derived from its facets while blank")
                                                 : QString()));
        f.append(fieldRow(QStringLiteral("unit"), tr("Unit"), QStringLiteral("text"), m->unit));
        f.append(fieldRow(QStringLiteral("status"), tr("Status"), QStringLiteral("enum"),
                          measureStatusName(m->status), statusOptions()));
        f.append(fieldRow(QStringLiteral("highMeans"), tr("A high reading means"),
                          QStringLiteral("prose"), m->highMeans));
        f.append(fieldRow(QStringLiteral("gapReason"), tr("Why it cannot be read"),
                          QStringLiteral("prose"), m->gapReason, {},
                          tr("quoted by the roadmap and the detail page")));

    } else if (type == kSignals) {
        const Signal *sig = p.signal(id);
        if (!sig) return f;
        f.append(fieldRow(QStringLiteral("direction"), tr("Which tail fires"),
                          QStringLiteral("enum"),
                          sig->direction.has_value() ? directionName(*sig->direction) : QString(),
                          directionOptionList()));

    } else if (type == kLinks) {
        QString  from, to;
        EdgeType et = EdgeType::Causes;
        if (!splitEdgeId(id, from, to, et)) return f;
        const Edge *e = nullptr;
        for (const Edge &x : p.edges)
            if (x.from == from && x.to == to && x.type == et) e = &x;
        if (!e) return f;
        f.append(fieldRow(QStringLiteral("relation"), tr("Relation"), QStringLiteral("enum"),
                          edgeTypeName(e->type), relationOptions()));
        // NAMED for what it says, not for what the enum is called. Its three values read
        // "sometimes / often / usually", and a column headed Strength hid that from the one author
        // who went looking for it.
        if (e->type == EdgeType::Causes)
            f.append(fieldRow(QStringLiteral("strength"), tr("How often"), QStringLiteral("enum"),
                              strengthName(e->strength), strengthOptions(),
                              tr("how often this cause produces this effect")));
        f.append(fieldRow(QStringLiteral("tier"), tr("Evidence"), QStringLiteral("enum"),
                          provenanceTierName(e->provenance.tier), tierOptions(),
                          citationRequired(e->provenance.tier) ? tr("needs a citation")
                                                               : QString()));
        f.append(fieldRow(QStringLiteral("citation"), tr("Citation"), QStringLiteral("text"),
                          e->provenance.citation, {}, tr("DOI, PMID or ISBN")));

    } else if (type == kCorridors) {
        QString mid, ctx;
        if (!splitCorridorId(id, mid, ctx)) return f;
        const NormResolution res = m_norms->resolve(mid, ctx);
        if (!res.found()) return f;
        const Norm *n = res.norm;
        f.append(fieldRow(QStringLiteral("mu"), tr("Aspiration"), QStringLiteral("number"), n->mu));
        f.append(fieldRow(QStringLiteral("sigmaLo"), tr("Tolerance −"), QStringLiteral("number"),
                          n->sigmaLo));
        f.append(fieldRow(QStringLiteral("sigmaHi"), tr("Tolerance +"), QStringLiteral("number"),
                          n->sigmaHi));
        f.append(fieldRow(QStringLiteral("plausibleLo"), tr("Plausible low"),
                          QStringLiteral("number"),
                          n->plausibleLo.has_value() ? QString::number(*n->plausibleLo) : QString(),
                          {}, tr("blank clears the bound")));
        f.append(fieldRow(QStringLiteral("plausibleHi"), tr("Plausible high"),
                          QStringLiteral("number"),
                          n->plausibleHi.has_value() ? QString::number(*n->plausibleHi) : QString(),
                          {}, tr("blank clears the bound")));
        f.append(fieldRow(QStringLiteral("unit"), tr("Unit"), QStringLiteral("text"), n->unit));
        f.append(fieldRow(QStringLiteral("source"), tr("Source"), QStringLiteral("enum"),
                          normSourceName(n->source), normSourceOptions()));
        f.append(fieldRow(QStringLiteral("citation"), tr("Citation"), QStringLiteral("text"),
                          n->citation));

    } else if (type == kScreens) {
        const Screen *sc = m_screens.screen(id);
        if (!sc) return f;
        f.append(fieldRow(QStringLiteral("name"), tr("Name"), QStringLiteral("text"), sc->label));
        f.append(fieldRow(QStringLiteral("region"), tr("Region"), QStringLiteral("text"),
                          sc->bodyRegion));
        f.append(fieldRow(QStringLiteral("protocol"), tr("Protocol"), QStringLiteral("prose"),
                          sc->protocol, {}, tr("how to run it")));
        f.append(fieldRow(QStringLiteral("passCriterion"), tr("Passing looks like"),
                          QStringLiteral("prose"), sc->passCriterion));
        f.append(fieldRow(QStringLiteral("passAtLeast"), tr("Numeric floor"),
                          QStringLiteral("number"),
                          sc->passAtLeast.has_value() ? QString::number(*sc->passAtLeast)
                                                      : QString(),
                          {}, tr("blank for a qualitative screen")));
        f.append(fieldRow(QStringLiteral("unit"), tr("Unit"), QStringLiteral("text"), sc->unit,
                          {}, sc->passAtLeast.has_value() ? tr("required by the floor above")
                                                          : QString()));
        f.append(fieldRow(QStringLiteral("note"), tr("What it does not settle"),
                          QStringLiteral("prose"), sc->note));
        f.append(fieldRow(QStringLiteral("citation"), tr("Citation"), QStringLiteral("text"),
                          sc->citation));

    } else if (type == kDrills) {
        const Drill *d = m_drills.drill(id);
        if (!d) return f;
        f.append(fieldRow(QStringLiteral("name"), tr("Name"), QStringLiteral("text"), d->label));
        f.append(fieldRow(QStringLiteral("instruction"), tr("What the golfer does"),
                          QStringLiteral("prose"), d->instruction));
        f.append(fieldRow(QStringLiteral("targets"), tr("What it is trying to change"),
                          QStringLiteral("prose"), d->targets, {},
                          tr("intent, never a claimed effect")));
        f.append(fieldRow(QStringLiteral("equipment"), tr("Equipment"), QStringLiteral("text"),
                          d->equipment.join(QStringLiteral(" · ")), {}, tr("separated by ·")));
        f.append(fieldRow(QStringLiteral("note"), tr("When it is the wrong drill"),
                          QStringLiteral("prose"), d->note));
    }
    return f;
}

QVariantMap ModelBrowser::inspect(const QString &type, const QString &id) const
{
    const CharacteristicPack &p = pack();

    QVariantMap out;
    out.insert(QStringLiteral("found"), false);
    out.insert(QStringLiteral("type"), type);
    out.insert(QStringLiteral("id"), id);

    QVariantList sections;

    // The FIELDS come first, on every type that has any. The inspector is where an author expects to
    // see and change everything an object holds; the table is the fast path for the handful of
    // fields that fit in a column. Building both from one list is what stops them disagreeing.
    //
    // Appended after the type-specific block below fills `sections`, so it lands at the TOP — see
    // the prepend at the end of this function.
    const QVariantList editableFields = fieldsOf(type, id);

    if (type == kCharacteristics || type == kCauses) {
        const Condition *c = p.condition(id);
        if (!c) return out;

        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), c->label);
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"), QStringLiteral("%1 · %2")
                                                   .arg(c->id, conditionGroupLabel(c->group)));

        QVariantList badges;
        badges.append(hubRow(QString(), QString(), reachLabel(c->confirmedBy), reachHint(c->confirmedBy),
                             QString(), false));
        badges.append(hubRow(QString(), QString(), provenanceTierLabel(c->provenance.tier), QString(),
                             c->provenance.tier == ProvenanceTier::Proposed ? QStringLiteral("warn")
                                                                            : QString(),
                             false));
        badges.append(hubRow(QString(), QString(), conditionStateName(c->state), QString(), QString(),
                             false));
        out.insert(QStringLiteral("badges"), badges);


        // Measures, through the signals that read them. The chain the old panel made you leave the
        // page for — a metric, the measure that reads it, the corridor that judges it — starts here.
        QVariantList measures;
        for (const QString &sid : c->detectedBy) {
            const Signal *s = p.signal(sid);
            if (!s) continue;
            for (const QString &mid : s->measures) {
                const Measure *m = p.measure(mid);
                if (!m) continue;
                const QString tail = s->direction.has_value()
                                         ? directionPhrase(*s->direction, m->highMeans).sentence
                                         : QString();
                measures.append(hubRow(kMeasures, m->id, measureDisplayLabel(*m), tail,
                                       statusTone(m->status)));
            }
        }
        sections.append(section(tr("Detected by"), measures,
                                measures.isEmpty() ? tr("Nothing measures this — it is inferred "
                                                        "from what it explains.")
                                                   : QString(),
                                QStringLiteral("list"), QStringLiteral("measure")));

        QVariantList causes;
        for (const QString &cid : causesOf(p, id)) {
            const Condition *o = p.condition(cid);
            if (!o) continue;
            QString strength;
            for (const Edge &e : p.edges)
                if (e.type == EdgeType::Causes && e.from == cid && e.to == id)
                    strength = strengthLabel(e.strength);
            causes.append(hubRow(kCauses, cid, o->label, strength,
                                 o->confirmedBy == ConfirmedBy::Asserted ? QStringLiteral("dim")
                                                                         : QString()));
        }
        sections.append(section(tr("Caused by"), causes, QString(), QStringLiteral("list"),
                                QStringLiteral("cause")));

        QVariantList effects;
        for (const QString &eid : effectsOf(p, id)) {
            const Condition *o = p.condition(eid);
            if (!o) continue;
            effects.append(hubRow(kCharacteristics, eid, o->label));
        }
        sections.append(section(tr("Explains"), effects));

        // The non-causal relations. Without these a corroborates or excludes edge would be authored,
        // validated, consumed by the explanation pass, and visible nowhere.
        for (EdgeType t : { EdgeType::Corroborates, EdgeType::Excludes }) {
            QVariantList rel;
            for (const Edge &e : p.edges) {
                if (e.type != t) continue;
                const QString other = e.from == id ? e.to : (e.to == id ? e.from : QString());
                if (other.isEmpty()) continue;
                const Condition *o = p.condition(other);
                rel.append(hubRow(kCharacteristics, other, o ? o->label : other));
            }
            if (!rel.isEmpty())
                sections.append(section(t == EdgeType::Corroborates ? tr("Corroborated by")
                                                                   : tr("Excludes"),
                                        rel));
        }

        if (!c->screenRef.isEmpty()) {
            const Screen *s = m_screens.screen(c->screenRef);
            QVariantList  rows;
            // A dangling reference must read as a defect, not as a condition that happens to have
            // no screen.
            rows.append(s ? hubRow(kScreens, s->id, s->label, s->passCriterion)
                          : hubRow(kScreens, c->screenRef, c->screenRef,
                                   tr("No screen with this id"), QStringLiteral("error"), false));
            sections.append(section(tr("Settled by screen"), rows));
        }

        QVariantList drills;
        for (const QString &did : c->drills) {
            const Drill *d = m_drills.drill(did);
            drills.append(d ? hubRow(kDrills, d->id, d->label, d->targets)
                            : hubRow(kDrills, did, did, tr("No drill with this id"),
                                     QStringLiteral("error"), false));
        }
        if (!drills.isEmpty()) sections.append(section(tr("Drills"), drills));

        // Where it applies. Rows carry their own state so the control can cycle it, and the ones
        // that merely INHERIT are marked — a checkbox that cannot say whether it is stating
        // something or repeating its parent teaches the author that every row is an assertion,
        // which is the opposite of how bindings work.
        {
            QVariantList rows;
            for (const QVariant &v : bindingsOf(id)) {
                const QVariantMap b = v.toMap();
                const bool applies  = b.value(QStringLiteral("applicable")).toBool();
                const bool material = b.value(QStringLiteral("material")).toBool();
                const bool own      = b.value(QStringLiteral("own")).toBool();

                QString detail = applies ? (material ? tr("applies") : tr("not counted when ranking"))
                                         : tr("does not apply");
                if (!own) {
                    const QString from = b.value(QStringLiteral("inheritedFrom")).toString();
                    detail = from.isEmpty() ? tr("%1 · inherited").arg(detail)
                                            : tr("%1 · from %2").arg(detail, from);
                }

                QVariantMap r = hubRow(QStringLiteral("context"),
                                       b.value(QStringLiteral("id")).toString(),
                                       b.value(QStringLiteral("label")).toString(), detail,
                                       applies ? (material ? QString() : QStringLiteral("dim"))
                                               : QStringLiteral("warn"),
                                       /*navigable*/ false);
                r.insert(QStringLiteral("depth"), b.value(QStringLiteral("depth")));
                r.insert(QStringLiteral("own"), own);
                r.insert(QStringLiteral("applicable"), applies);
                r.insert(QStringLiteral("material"), material);
                rows.append(r);
            }
            sections.append(section(tr("Where it applies"), rows,
                                    tr("A row is an EXCEPTION — anything with no row of its own "
                                       "inherits from the context above it."),
                                    QStringLiteral("bindings"), QStringLiteral("binding")));
        }

        if (!c->provenance.citation.isEmpty()) {
            QVariantList rows;
            const Reference *ref = sharedReferenceSet().byCitation(c->provenance.citation);
            rows.append(ref ? hubRow(kReferences, ref->id, ref->title, ref->identifierLabel())
                            : hubRow(QString(), QString(), citationLabel(c->provenance.citation),
                                     QString(), QString(), false));
        }

    } else if (type == kMeasures) {
        const Measure *m = p.measure(id);
        if (!m) return out;

        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), measureDisplayLabel(*m));
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"),
                   m->unit.isEmpty() ? m->id : QStringLiteral("%1 · %2").arg(m->id, m->unit));

        QVariantList badges;
        badges.append(hubRow(QString(), QString(), measureStatusLabel(m->status), QString(),
                             statusTone(m->status), false));
        badges.append(hubRow(QString(), QString(), measureKindName(m->kind), QString(), QString(),
                             false));
        out.insert(QStringLiteral("badges"), badges);

        // Corridor state. A measure nothing grades is a corridor signal that can never fire, and
        // that is the single most useful thing this pane can say about it.
        // Every context that resolves, marked own or inherited — and each row NAVIGATES to the
        // corridor itself, so "what grades this" and "change what grades this" are one click apart
        // rather than in two different panels.
        QVariantList corridors;
        for (const QVariant &v : corridorContexts(id)) {
            const QVariantMap ctx = v.toMap();
            if (!ctx.value(QStringLiteral("found")).toBool()) continue;

            const bool    own  = ctx.value(QStringLiteral("own")).toBool();
            const QString from = ctx.value(QStringLiteral("inheritedFrom")).toString();
            QString detail = tr("μ %1 %2").arg(ctx.value(QStringLiteral("mu")).toDouble(), 0, 'g', 4)
                                 .arg(ctx.value(QStringLiteral("unit")).toString());
            if (!from.isEmpty()) detail = tr("%1 · from %2").arg(detail, from);
            else if (own)        detail = tr("%1 · yours").arg(detail);

            corridors.append(hubRow(kCorridors,
                                    corridorId(id, ctx.value(QStringLiteral("id")).toString()),
                                    ctx.value(QStringLiteral("label")).toString(), detail,
                                    own ? QStringLiteral("accent") : QString()));
        }
        sections.append(section(tr("Corridors"), corridors,
                                corridors.isEmpty()
                                    ? tr("No norm resolves for this measure, so a corridor signal "
                                         "on it can never fire.")
                                    : QString(),
                                QStringLiteral("list"), QStringLiteral("corridor")));


        if (!m->metricKey.isEmpty()) {
            QVariantList rows;
            // A real type key, not a singular label. It was "metric", which matched nothing and
            // landed in a toast apologising that the catalogue lived in another panel.
            const MetricDescriptor *d = m_cat.descriptor(m->metricKey);
            rows.append(hubRow(kMetrics, m->metricKey,
                               d && !d->label.isEmpty() ? d->label : m->metricKey,
                               d ? d->unit : tr("not in the catalogue")));
            sections.append(section(tr("Comes from metric"), rows));
        }

        QVariantList sigs;
        for (const Signal &s : p.signalDefs) {
            if (!s.measures.contains(id)) continue;
            const QString tail = s.direction.has_value()
                                     ? directionPhrase(*s.direction, m->highMeans).label
                                     : QString();
            sigs.append(hubRow(kSignals, s.id, s.id,
                               QStringLiteral("%1 · %2").arg(signalTestName(s.test), tail)));
        }
        sections.append(section(tr("Read by signals"), sigs));

        // The blast radius, as the LIST and not only a count: a count alone does not say what is
        // about to change, and this measure may be shared.
        const QVariantList users = measureUserRows(id);
        sections.append(section(tr("Blast radius"), users,
                                tr("characteristics that would change if this measure changed")));
    } else if (type == kMetrics) {
        const MetricDescriptor *d = m_cat.descriptor(id);
        if (!d) return out;

        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), d->label.isEmpty() ? d->key : d->label);
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"),
                   d->unit.isEmpty() ? d->key : QStringLiteral("%1 · %2").arg(d->key, d->unit));

        QVariantList badges;
        badges.append(hubRow(QString(), QString(), d->group, QString(), QString(), false));
        if (d->planned)
            badges.append(hubRow(QString(), QString(), tr("Planned"),
                                 tr("in the catalogue, no producer yet"), QStringLiteral("warn"),
                                 false));
        out.insert(QStringLiteral("badges"), badges);

        if (!d->description.isEmpty()) sections.append(prose(tr("What it means"), d->description));
        // The sign convention and when to read it. Three signals shipped inverted because a
        // convention was unstated somewhere; this is where it is stated.
        if (!d->howToRead.isEmpty())   sections.append(prose(tr("How to read it"), d->howToRead));

        const QStringList needs = metricNeeds(d->requirement);
        sections.append(prose(tr("Needs"),
                              needs.isEmpty() ? tr("Nothing beyond a swing.")
                                              : needs.join(QStringLiteral(" · "))));

        // THE JOIN, and the reason this view is here at all: a metric is a curve, and a measure is
        // that curve reduced at a phase. One metric carries several measures — sway, slide and
        // hanging back are all pelvis lateral displacement — and following that chain used to mean
        // leaving the page.
        QVariantList readers;
        for (const Measure &meas : p.measures) {
            if (meas.metricKey != id) continue;
            readers.append(hubRow(kMeasures, meas.id, measureDisplayLabel(meas),
                                  measureStatusLabel(meas.status), statusTone(meas.status)));
        }
        sections.append(section(tr("Read by measures"), readers,
                                readers.isEmpty()
                                    ? tr("No measure reads this metric, so nothing in the "
                                         "diagnostics pack is built on it yet.")
                                    : QString()));

        // Where the rest of the app uses it — hand-authored in the manifest, and the only place
        // this is visible.
        QVariantList uses;
        for (const QString &u : d->usedBy)
            uses.append(hubRow(QString(), QString(), u, QString(), QString(), false));
        if (!uses.isEmpty()) sections.append(section(tr("Used by"), uses));

    } else if (type == kSignals) {
        const Signal *s = p.signal(id);
        if (!s) return out;

        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), s->id);
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"), signalTestName(s->test));

        QVariantList measures;
        for (const QString &mid : s->measures) {
            const Measure *m = p.measure(mid);
            measures.append(m ? hubRow(kMeasures, mid, measureDisplayLabel(*m), m->unit,
                                       statusTone(m->status))
                              : hubRow(kMeasures, mid, mid, tr("No measure with this id"),
                                       QStringLiteral("error"), false));
        }
        sections.append(section(tr("Reads"), measures));

        if (s->direction.has_value()) {
            const Measure *m = s->measures.isEmpty() ? nullptr : p.measure(s->measures.first());
        }

        QVariantList users;
        for (const Condition &c : p.conditions)
            if (c.detectedBy.contains(id))
                users.append(hubRow(kCharacteristics, c.id, c.label, conditionGroupLabel(c.group)));
        sections.append(section(tr("Identifies"), users));

    } else if (type == kLinks) {
        QString from, to; EdgeType t = EdgeType::Causes;
        if (!splitEdgeId(id, from, to, t)) return out;
        const Edge *edge = nullptr;
        for (const Edge &e : p.edges)
            if (e.from == from && e.to == to && e.type == t) edge = &e;
        if (!edge) return out;

        const Condition *f = p.condition(from);
        const Condition *o = p.condition(to);

        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), tr("%1 → %2").arg(f ? f->label : from, o ? o->label : to));
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"), edgeTypeName(t));

        QVariantList ends;
        ends.append(hubRow(kCauses, from, f ? f->label : from, tr("the cause")));
        ends.append(hubRow(kCharacteristics, to, o ? o->label : to, tr("the effect")));
        sections.append(section(tr("Between"), ends));


        QVariantList ev;
        if (!edge->provenance.citation.isEmpty()) {
            const Reference *ref = sharedReferenceSet().byCitation(edge->provenance.citation);
            ev.append(ref ? hubRow(kReferences, ref->id, ref->title, ref->identifierLabel())
                          : hubRow(QString(), QString(), citationLabel(edge->provenance.citation),
                                   QString(), QString(), false));
        }

    } else if (type == kCorridors) {
        QString mid, ctx;
        if (!splitCorridorId(id, mid, ctx)) return out;

        const NormResolution res = m_norms->resolve(mid, ctx);
        if (!res.found()) return out;

        const Measure *meas = p.measure(mid);
        const ContextNode *node = m_norms->contexts().node(ctx);

        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), meas ? measureDisplayLabel(*meas) : mid);
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"), node ? node->label : ctx);

        QVariantList badges;
        badges.append(hubRow(QString(), QString(), normSourceLabel(res.norm->source), QString(),
                             res.norm->source == NormSource::Heuristic ? QStringLiteral("warn")
                                                                       : QString(),
                             false));
        // A norm that is weak says so, in the one place somebody is deciding whether to trust it.
        if (normIsWeak(*res.norm))
            badges.append(hubRow(QString(), QString(), tr("weak"), normWeakReason(*res.norm),
                                 QStringLiteral("warn"), false));
        out.insert(QStringLiteral("badges"), badges);

        QVariantList band;
        band.append(hubRow(QString(), QString(), tr("Ideal"),
                           tr("%1 to %2").arg(res.norm->mu - res.norm->sigmaLo)
                                         .arg(res.norm->mu + res.norm->sigmaHi),
                           QString(), false));
        if (res.norm->plausibleLo.has_value() || res.norm->plausibleHi.has_value())
            band.append(hubRow(QString(), QString(), tr("Believed between"),
                               tr("%1 to %2")
                                   .arg(res.norm->plausibleLo.has_value()
                                            ? QString::number(*res.norm->plausibleLo) : tr("—"))
                                   .arg(res.norm->plausibleHi.has_value()
                                            ? QString::number(*res.norm->plausibleHi) : tr("—")),
                               QString(), false));
        sections.append(section(tr("What good looks like"), band,
                                tr("Outside the believed range a reading is not graded at all — "
                                   "that asks whether the number is REAL, which is a different "
                                   "question from whether the swing is good.")));

        QVariantList meas2;
        if (meas) meas2.append(hubRow(kMeasures, mid, measureDisplayLabel(*meas), meas->unit,
                                      statusTone(meas->status)));
        sections.append(section(tr("Grades"), meas2));

        // Who this actually moves. A corridor edit has no blast radius outside its measure, and
        // every bit of one inside it.
        sections.append(section(tr("Blast radius"), measureUserRows(mid),
                                tr("characteristics graded by this corridor")));

        // The other contexts, so driver-against-iron-against-wedge is one glance rather than five
        // navigations — that comparison is the whole reason the context tree exists.
        QVariantList siblings;
        for (const QVariant &v : corridorContexts(mid)) {
            const QVariantMap c2 = v.toMap();
            if (!c2.value(QStringLiteral("found")).toBool()) continue;
            if (c2.value(QStringLiteral("id")).toString() == ctx) continue;
            siblings.append(hubRow(kCorridors,
                                   corridorId(mid, c2.value(QStringLiteral("id")).toString()),
                                   c2.value(QStringLiteral("label")).toString(),
                                   tr("μ %1").arg(c2.value(QStringLiteral("mu")).toDouble(),
                                                  0, 'g', 4)));
        }
        sections.append(section(tr("Same measure, other contexts"), siblings));

    } else if (type == kScreens) {
        // From the ASSEMBLY, so an unsaved edit is what the pane shows.
        const Screen *s = m_screens.screen(id);
        if (!s) return out;
        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), s->label);
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"), s->bodyRegion);
        // The screen's own fields — protocol, pass criterion, note — are in the Fields section at
        // the top of the pane now, as editors. They used to be repeated here as read-only prose,
        // which is what an author reached for and could not type into.
        QVariantList settles;
        for (const Condition &c : p.conditions)
            if (c.screenRef == id)
                settles.append(hubRow(kCauses, c.id, c.label,
                                      tr("explains %1").arg(coverageOf(p, c.id))));
        sections.append(section(tr("Settles"), settles,
                                settles.isEmpty()
                                    ? tr("Nothing yet — a screen that settles nothing never gets "
                                         "suggested.")
                                    : QString(),
                                QStringLiteral("list"), QStringLiteral("settles")));

    } else if (type == kDrills) {
        const Drill *d = m_drills.drill(id);
        if (!d) return out;
        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), d->label);
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"), d->id);
        QVariantList answers;
        for (const Condition &c : p.conditions)
            if (c.drills.contains(id))
                answers.append(hubRow(kCharacteristics, c.id, c.label));
        sections.append(section(tr("Answers"), answers,
                                answers.isEmpty()
                                    ? tr("Nothing yet — a drill that answers nothing is never "
                                         "offered to a golfer.")
                                    : QString(),
                                QStringLiteral("list"), QStringLiteral("answers")));

    } else if (type == kReferences) {
        const Reference *ref = sharedReferenceSet().reference(id);
        if (!ref) return out;
        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("label"), ref->title.isEmpty() ? ref->id : ref->title);
        out.insert(QStringLiteral("eyebrow"), typeLabelFor(type));
        out.insert(QStringLiteral("subtitle"),
                   QStringLiteral("%1 · %2").arg(ref->authors).arg(ref->year));

        const auto cites = [ref](const QString &citation) {
            if (citation.isEmpty()) return false;
            return (!ref->doi.isEmpty() && citation == ref->doi)
                || (!ref->pmid.isEmpty() && citation == ref->pmid)
                || (!ref->isbn.isEmpty() && citation == ref->isbn);
        };

        // The claims resting on this paper — the question a reader actually has is "why does the app
        // believe this?", and the answer is the pairing. Counted first because the badge says it.
        const QVariantList claimLinks = linksCitingReference(id);
        QVariantList       claims;
        for (const QVariant &v : claimLinks) claims.append(v);
        for (const Condition &c : p.conditions)
            if (cites(c.provenance.citation)) {
                // A condition's own provenance is not an edge, so it has no strength to set — it
                // joins the list as an ordinary navigable row and the view leaves it alone.
                QVariantMap m = hubRow(kCharacteristics, c.id, c.label,
                                       provenanceTierLabel(c.provenance.tier));
                claims.append(m);
            }

        ProvenanceTier best     = ProvenanceTier::Proposed;
        bool           anyClaim = false;
        for (const Edge &e : p.edges) {
            if (!cites(e.provenance.citation)) continue;
            best     = anyClaim ? std::max(best, e.provenance.tier) : e.provenance.tier;
            anyClaim = true;
        }
        for (const Condition &c : p.conditions) {
            if (!cites(c.provenance.citation)) continue;
            best     = anyClaim ? std::max(best, c.provenance.tier) : c.provenance.tier;
            anyClaim = true;
        }

        QVariantList badges;
        badges.append(hubRow(QString(), QString(),
                             anyClaim ? provenanceTierLabel(best)
                                      : (ref->generalReading ? tr("General reading") : tr("Uncited")),
                             QString(), anyClaim ? QString() : QStringLiteral("dim"), false));
        badges.append(hubRow(QString(), QString(),
                             tr("Holds up %n claim(s)", "", claims.size()), QString(),
                             claims.isEmpty() ? QStringLiteral("warn") : QString(), false));
        out.insert(QStringLiteral("badges"), badges);

        // The record as a citation reads, assembled HERE rather than in a delegate — which format a
        // book, a chapter and a paper each take is a rule, and a rule written in QML is a rule
        // nothing can test.
        sections.append(section(tr("Citation"),
                                QVariantList{ hubRow(QString(), QString(), formatCitation(*ref),
                                                     QString(), QString(), false) },
                                QString(), QStringLiteral("quote")));

        if (!ref->establishes.isEmpty())
            sections.append(prose(tr("What it shows"), ref->establishes));

        // The identifier, with the two things anybody actually wants to do with one.
        QVariantList idRows;
        idRows.append(hubRow(QString(), QStringLiteral("copyCitation"),
                             tr("Copy CSL-JSON"), ref->identifierLabel(), QString(), false));
        if (!ref->doi.isEmpty())
            idRows.append(hubRow(QString(), QStringLiteral("openDoi"),
                                 tr("Open DOI ↗"), ref->doi, QString(), false));
        sections.append(section(tr("Identifier"), idRows, QString(), QStringLiteral("actions")));

        // An inert pane that does not explain itself reads as a bug. Said once, in the pane, rather
        // than left for the reader to infer from cells that will not take a cursor.
        sections.append(prose(tr("Why this is not editable"),
                              tr("Imported from the bibliography and regenerated on every pack "
                                 "build, so the record itself is not editable here. What rests on "
                                 "it is."),
                              QStringLiteral("dim")));

        // …and the part that makes it a working surface. The citation is imported; the CLAIM resting
        // on it is ours, so its strength is live here and writes through the ordinary
        // setField("links", …).
        sections.append(section(tr("Supports these claims"), claims,
                                claims.isEmpty()
                                    ? tr("Nothing cites this — kept and marked rather than dropped.")
                                    : QString(),
                                QStringLiteral("claims")));
    }

    // Whose content this is, and whether it is unsaved. Every inspector header shows it, because an
    // author has to know whose content they are changing BEFORE they change it.
    out.insert(QStringLiteral("source"), sourceOf(id));
    out.insert(QStringLiteral("dirty"), dirtyIds().contains(id));
    if (!editableFields.isEmpty()) {
        QVariantList withFields;
        withFields.append(section(tr("Fields"), editableFields, QString(),
                                  QStringLiteral("fields")));
        for (const QVariant &v : sections) withFields.append(v);
        sections = withFields;
    }

    out.insert(QStringLiteral("sections"), sections);
    return out;
}

// ── The graph ───────────────────────────────────────────────────────────────

QVariantMap ModelBrowser::dag(const QString &conditionId, const QVariantMap &options) const
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

    // Over the WORKING assembly, so an unsaved edge is drawn. Every coordinate comes from
    // dag_layout.h; QML positions nothing.
    const DagLayout l = layoutDag(pack(), conditionId, opt);

    const bool hideWeak     = options.value(QStringLiteral("hideWeak")).toBool();
    const bool hideProposed = options.value(QStringLiteral("hideProposed")).toBool();

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
        m.insert(QStringLiteral("dirty"), dirtyIds().contains(n.id));
        nodes.append(m);
    }

    QVariantList edges;
    for (const DagEdge &e : l.edges) {
        if (hideWeak && e.strength == QStringLiteral("weak")) continue;

        QVariantMap m;
        m.insert(QStringLiteral("from"), e.from);
        m.insert(QStringLiteral("to"), e.to);
        m.insert(QStringLiteral("strength"), e.strength);
        m.insert(QStringLiteral("strengthLabel"), e.strengthLabel);
        m.insert(QStringLiteral("weight"), e.weight);
        m.insert(QStringLiteral("detects"), e.detects);
        m.insert(QStringLiteral("offeredOnly"), e.offeredOnly);
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

        // The row id, so clicking a line selects the same object the Causal links table does. Only
        // a real edge has one — a measure's detection line is not an edge in the pack.
        EdgeType t = EdgeType::Causes;
        if (!e.detects && edgeTypeFromName(e.relation, t)) {
            const QString rid = edgeId(e.from, e.to, t);
            if (hideProposed) {
                bool proposed = false;
                for (const Edge &raw : pack().edges)
                    if (raw.from == e.from && raw.to == e.to && raw.type == t)
                        proposed = raw.provenance.tier == ProvenanceTier::Proposed;
                if (proposed) continue;
            }
            m.insert(QStringLiteral("rowId"), rid);
            m.insert(QStringLiteral("dirty"), dirtyIds().contains(rid));
        }
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

// ── Legal targets ───────────────────────────────────────────────────────────

QVariantList ModelBrowser::linkCandidates(const QString &relation, const QString &fromId,
                                          const QString &search) const
{
    const CharacteristicPack &p = pack();

    EdgeType type = EdgeType::Causes;
    if (!edgeTypeFromName(relation, type)) return {};

    QVariantList out;
    for (const Condition &c : p.conditions) {
        // A self-edge is refused by the validator, so it is never offered.
        if (c.id == fromId) continue;

        // Already linked this way — offering it would produce a duplicate row rather than an edit.
        bool exists = false;
        for (const Edge &e : p.edges)
            if (e.type == type
                && ((e.from == fromId && e.to == c.id)
                    || (type != EdgeType::Causes && e.from == c.id && e.to == fromId)))
                exists = true;
        if (exists) continue;

        if (type == EdgeType::Causes) {
            // Acyclicity, checked BEFORE the edge can be constructed. `from` causes `to`, so the
            // cycle is the reverse path already existing.
            if (hasCausalPath(p, c.id, fromId)) continue;
        } else if (type == EdgeType::Corroborates) {
            // Corroborates is illegal between conditions that already have a causal path in either
            // direction — the pair would double-count in the confidence ranking, and the validator
            // rejects the whole library over it.
            if (hasCausalPath(p, fromId, c.id) || hasCausalPath(p, c.id, fromId)) continue;
        }

        if (!search.trimmed().isEmpty()) {
            const QString hay = QStringList{ c.label, c.id, c.aliases.join(QLatin1Char(' ')) }
                                    .join(QLatin1Char(' '));
            if (!matches(hay, search.trimmed())) continue;
        }

        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("detail"), conditionGroupLabel(c.group));
        r.insert(QStringLiteral("reachLabel"), reachLabel(c.confirmedBy));
        out.append(r);
    }

    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("label")).toString().toCaseFolded()
             < b.toMap().value(QStringLiteral("label")).toString().toCaseFolded();
    });
    return out;
}

QVariantList ModelBrowser::measureCandidates(const QString &conditionId, const QString &search) const
{
    const CharacteristicPack &p = pack();
    const Condition          *c = p.condition(conditionId);

    QSet<QString> already;
    if (c)
        for (const QString &sid : c->detectedBy)
            if (const Signal *s = p.signal(sid))
                for (const QString &mid : s->measures) already.insert(mid);

    QVariantList out;
    for (const Measure &m : p.measures) {
        if (already.contains(m.id)) continue;
        const QString label = measureDisplayLabel(m);
        if (!search.trimmed().isEmpty()) {
            const QString hay = QStringList{ label, m.id, m.unit, m.metricKey }.join(QLatin1Char(' '));
            if (!matches(hay, search.trimmed())) continue;
        }
        QVariantMap r;
        r.insert(QStringLiteral("id"), m.id);
        r.insert(QStringLiteral("label"), label);
        r.insert(QStringLiteral("detail"), measureStatusLabel(m.status));
        r.insert(QStringLiteral("tone"), statusTone(m.status));
        out.append(r);
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("label")).toString().toCaseFolded()
             < b.toMap().value(QStringLiteral("label")).toString().toCaseFolded();
    });
    return out;
}

// ── Working-copy helpers ────────────────────────────────────────────────────
//
// Copy-on-write, and the copy happens HERE rather than at each call site. Editing a field of a
// shipped object copies that object into the working pack; the shipped one is never touched. Undoing
// the first such edit removes the copy, which IS the reset — see the header block.

Condition *ModelBrowser::workingCondition(const QString &id)
{
    for (Condition &c : m_working.conditions)
        if (c.id == id) return &c;
    if (const Condition *src = pack().condition(id)) {
        m_working.conditions.push_back(*src);
        return &m_working.conditions.back();
    }
    return nullptr;
}

Measure *ModelBrowser::workingMeasure(const QString &id)
{
    for (Measure &m : m_working.measures)
        if (m.id == id) return &m;
    if (const Measure *src = pack().measure(id)) {
        m_working.measures.push_back(*src);
        return &m_working.measures.back();
    }
    return nullptr;
}

Signal *ModelBrowser::workingSignal(const QString &id)
{
    for (Signal &s : m_working.signalDefs)
        if (s.id == id) return &s;
    if (const Signal *src = pack().signal(id)) {
        m_working.signalDefs.push_back(*src);
        return &m_working.signalDefs.back();
    }
    return nullptr;
}

// Copy-on-write for the two flat sets. Reaching for a shipped screen copies it into the working
// layer, and undoing that first edit removes the copy again — which is the reset, falling out of the
// layering rather than being coded, exactly as it does on the pack side.
Screen *ModelBrowser::workingScreen(const QString &id)
{
    for (Screen &s : m_workingScreens.screens)
        if (s.id == id) return &s;
    if (const Screen *src = m_screens.screen(id)) {
        m_workingScreens.screens.push_back(*src);
        return &m_workingScreens.screens.back();
    }
    return nullptr;
}

Drill *ModelBrowser::workingDrill(const QString &id)
{
    for (Drill &d : m_workingDrills.drills)
        if (d.id == id) return &d;
    if (const Drill *src = m_drills.drill(id)) {
        m_workingDrills.drills.push_back(*src);
        return &m_workingDrills.drills.back();
    }
    return nullptr;
}

void ModelBrowser::materialiseCausesOf(const QString &conditionId)
{
    // A user pack REPLACES a condition's whole incoming causal set, so removing one shipped cause
    // means writing back every other one. Without this, deleting a single edge would silently drop
    // the rest of that condition's causes — a data-loss bug that looks like a successful edit.
    const CharacteristicPack &p = pack();
    for (const Edge &e : p.edges) {
        if (e.type != EdgeType::Causes || e.to != conditionId) continue;
        bool have = false;
        for (const Edge &w : m_working.edges)
            if (w.from == e.from && w.to == e.to && w.type == e.type) have = true;
        if (!have) m_working.edges.push_back(e);
    }
}

Edge *ModelBrowser::workingEdge(const QString &fromId, const QString &toId, EdgeType type)
{
    for (Edge &e : m_working.edges)
        if (e.from == fromId && e.to == toId && e.type == type) return &e;
    for (const Edge &e : pack().edges)
        if (e.from == fromId && e.to == toId && e.type == type) {
            if (type == EdgeType::Causes) materialiseCausesOf(toId);
            for (Edge &w : m_working.edges)
                if (w.from == fromId && w.to == toId && w.type == type) return &w;
            m_working.edges.push_back(e);
            return &m_working.edges.back();
        }
    return nullptr;
}

// ── The undo stack ──────────────────────────────────────────────────────────

void ModelBrowser::pushCommand(const QString &label, const QString &detail,
                               const CharacteristicPack &before, const NormPack &normsBefore)
{
    // The screen and drill layers as they stand ARE the before-state for every caller of this form,
    // because none of them can have touched those registries. The long form exists for the four that
    // can.
    pushCommand(label, detail, before, normsBefore, m_workingScreens, m_workingDrills);
}

void ModelBrowser::pushCommand(const QString &label, const QString &detail,
                               const CharacteristicPack &before, const NormPack &normsBefore,
                               const ScreenSet &screensBefore, const DrillSet &drillsBefore)
{
    // Anything ahead of the cursor is gone: a new edit after an undo forks history, and keeping the
    // abandoned branch would offer a redo that no longer applies to the pack in hand.
    if (m_stackIndex + 1 < int(m_stack.size()))
        m_stack.erase(m_stack.begin() + (m_stackIndex + 1), m_stack.end());

    // If the save marker was in the discarded tail, the file no longer corresponds to any position
    // on this stack. -2 is "nowhere on the stack", which is why the marker is compared rather than
    // trusted as an index.
    if (m_savedIndex > m_stackIndex) m_savedIndex = -2;

    m_stack.push_back(Command{ label, detail, before, m_working, normsBefore, m_workingNorms,
                               screensBefore, m_workingScreens, drillsBefore, m_workingDrills });
    m_stackIndex = int(m_stack.size()) - 1;
    invalidateDerived();
}

QVariantMap ModelBrowser::applyStackPosition(int newIndex)
{
    QVariantMap r;
    const int   lo = -1;
    const int   hi = int(m_stack.size()) - 1;
    if (newIndex < lo || newIndex > hi) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("Nothing to move to."));
        return r;
    }

    // The pack AT that position: the `after` of the last applied command, or the `before` of the
    // first when the cursor is below everything.
    // ALL FOUR registries move together. Restoring some and leaving others is how a stack position
    // stops describing a state anybody was ever in.
    if (newIndex >= 0) {
        const Command &c = m_stack.at(size_t(newIndex));
        m_working        = c.after;
        m_workingNorms   = c.normsAfter;
        m_workingScreens = c.screensAfter;
        m_workingDrills  = c.drillsAfter;
    } else {
        const Command &c = m_stack.front();
        m_working        = c.before;
        m_workingNorms   = c.normsBefore;
        m_workingScreens = c.screensBefore;
        m_workingDrills  = c.drillsBefore;
    }
    m_stackIndex = newIndex;
    rebuild();

    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("message"),
             newIndex >= 0 ? tr("Back to “%1”").arg(m_stack.at(size_t(newIndex)).label)
                           : tr("Back to where you started"));
    return r;
}

bool ModelBrowser::canUndo() const { return m_stackIndex >= 0; }
bool ModelBrowser::canRedo() const { return m_stackIndex + 1 < int(m_stack.size()); }

QString ModelBrowser::undoLabel() const
{
    return canUndo() ? m_stack.at(size_t(m_stackIndex)).label : QString();
}
QString ModelBrowser::redoLabel() const
{
    return canRedo() ? m_stack.at(size_t(m_stackIndex + 1)).label : QString();
}

QVariantList ModelBrowser::edits() const
{
    QVariantList out;
    for (size_t i = 0; i < m_stack.size(); ++i) {
        QVariantMap r;
        r.insert(QStringLiteral("index"), int(i));
        r.insert(QStringLiteral("label"), m_stack.at(i).label);
        r.insert(QStringLiteral("detail"), m_stack.at(i).detail);
        // Undone entries stay in the list, greyed. A history that deleted what you stepped back
        // past would make redo undiscoverable.
        r.insert(QStringLiteral("undone"), int(i) > m_stackIndex);
        // Everything up to the save marker is on disk. Shown per row rather than as one boundary
        // line because the marker can be nowhere (-2) after a forked history.
        r.insert(QStringLiteral("saved"), m_savedIndex >= 0 && int(i) <= m_savedIndex);
        out.append(r);
    }
    return out;
}

QVariantMap ModelBrowser::undo()
{
    if (!canUndo()) {
        QVariantMap r;
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("Nothing to undo."));
        return r;
    }
    const QString what = m_stack.at(size_t(m_stackIndex)).label;
    QVariantMap   r    = applyStackPosition(m_stackIndex - 1);
    if (r.value(QStringLiteral("ok")).toBool())
        r.insert(QStringLiteral("message"), tr("Undid “%1”").arg(what));
    return r;
}

QVariantMap ModelBrowser::redo()
{
    if (!canRedo()) {
        QVariantMap r;
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("Nothing to redo."));
        return r;
    }
    const QString what = m_stack.at(size_t(m_stackIndex + 1)).label;
    QVariantMap   r    = applyStackPosition(m_stackIndex + 1);
    if (r.value(QStringLiteral("ok")).toBool())
        r.insert(QStringLiteral("message"), tr("Redid “%1”").arg(what));
    return r;
}

QVariantMap ModelBrowser::undoTo(int index) { return applyStackPosition(index); }

// ── Save / revert ───────────────────────────────────────────────────────────

QVariantMap ModelBrowser::save()
{
    QVariantMap r;

    QString whyNot;
    if (!saveUserPack(m_working, &whyNot)) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), whyNot.isEmpty() ? tr("Could not save.") : whyNot);
        return r;
    }
    // The norm set is written second and reported separately. A half-save is the one outcome worth
    // naming precisely: the pack landed, the corridors did not, and an author told only "could not
    // save" would not know which half to redo.
    if (!saveUserNormPack(m_workingNorms, &whyNot)) {
        m_savedUser = m_working;   // the half that DID land is on disk; do not claim otherwise
        invalidateDerived();
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"),
                 tr("Characteristics were saved; the corridors were not. %1").arg(whyNot));
        return r;
    }

    // The screen and drill layers, written third and fourth. Reported the same way as the corridor
    // half: an author told only "could not save" would not know which registry to redo.
    if (!saveUserScreenSet(m_workingScreens, &whyNot)) {
        m_savedUser  = m_working;
        m_savedNorms = m_workingNorms;
        invalidateDerived();
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"),
                 tr("Characteristics and corridors were saved; the screens were not. %1").arg(whyNot));
        return r;
    }
    if (!saveUserDrillSet(m_workingDrills, &whyNot)) {
        m_savedUser    = m_working;
        m_savedNorms   = m_workingNorms;
        m_savedScreens = m_workingScreens;
        invalidateDerived();
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"),
                 tr("Everything but the drills was saved. %1").arg(whyNot));
        return r;
    }
    // The rest of the app reads these two through their process-wide caches, so a write that did not
    // drop them would be on disk and invisible until relaunch.
    resetSharedScreenSet();
    resetSharedDrillSet();

    const int wrote = unsavedCount();
    m_savedUser    = m_working;
    m_savedNorms   = m_workingNorms;
    m_savedScreens = m_workingScreens;
    m_savedDrills  = m_workingDrills;
    m_savedIndex   = m_stackIndex;
    // The stack is NOT cleared. Saving and immediately spotting the mistake is the common case, and
    // an undo that stopped at the last save would be an undo the author could not rely on.
    invalidateDerived();

    // Every other façade in the app caches its provider, so an edit written here is on disk and
    // invisible until relaunch unless they re-take.
    emit libraryChanged();

    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("message"), tr("Saved %n change(s)", "", wrote));
    return r;
}

QVariantMap ModelBrowser::revert()
{
    QVariantMap r;
    if (!dirty()) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("Nothing to revert."));
        return r;
    }

    const int                before        = unsavedCount();
    const CharacteristicPack prior         = m_working;
    const NormPack           priorNorms    = m_workingNorms;
    const ScreenSet          priorScreens  = m_workingScreens;
    const DrillSet           priorDrills   = m_workingDrills;
    m_working        = m_savedUser;
    m_workingNorms   = m_savedNorms;
    m_workingScreens = m_savedScreens;
    m_workingDrills  = m_savedDrills;
    rebuild();
    // Revert is itself a command. Discarding an afternoon's work with no way back would be the one
    // unrecoverable action in a panel whose rule is that there are none.
    pushCommand(tr("Revert"), tr("%n unsaved change(s) discarded", "", before), prior, priorNorms,
                priorScreens, priorDrills);

    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("message"), tr("Reverted %n change(s)", "", before));
    return r;
}

namespace {

// Every write in this file answers in the author's own terms, never with a log line — so the two
// shapes an answer can take are written once. Declared here rather than beside the editing block
// because the reset below is the first caller.
QVariantMap refuse(const QString &message)
{
    QVariantMap r;
    r.insert(QStringLiteral("ok"), false);
    r.insert(QStringLiteral("message"), message);
    return r;
}
QVariantMap accept(const QString &message)
{
    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("message"), message);
    return r;
}

// Copy a user layer aside before it is replaced. COPY rather than rename: the write that follows
// goes through saveUserPack(), which writes a temporary and renames it over the original — and an
// original that had already been moved away would leave a window with no file at all.
//
// A missing file is a success with nothing to do. That is the ordinary case for an install that has
// only ever edited one of the two registries.
bool copyAside(const QString &path, const QString &stamp, QStringList *written, QString *whyNot)
{
    if (path.isEmpty() || !QFile::exists(path)) return true;

    QFileInfo     info(path);
    const QString backup = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
                           + QStringLiteral("-") + stamp + QStringLiteral(".backup.")
                           + info.suffix();
    QFile::remove(backup);
    if (!QFile::copy(path, backup)) {
        if (whyNot)
            *whyNot = QObject::tr("Could not write the backup %1, so nothing was reset.").arg(backup);
        return false;
    }
    if (written) written->append(backup);
    return true;
}

} // namespace

QVariantMap ModelBrowser::resetToStandard()
{
    const int overridden = overriddenCount();
    const int authored   = authoredCount();
    if (overridden + authored == 0)
        return refuse(tr("This install is already on the standard model."));

    // The copies come FIRST. If the backup cannot be written the reset does not happen at all — a
    // reset that half-succeeded would have destroyed the thing the backup existed to protect.
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QStringList   backups;
    QString       whyNot;
    if (!copyAside(userPackPath(), stamp, &backups, &whyNot))      return refuse(whyNot);
    if (!copyAside(userNormPath(), stamp, &backups, &whyNot))      return refuse(whyNot);
    if (!copyAside(userScreenSetPath(), stamp, &backups, &whyNot)) return refuse(whyNot);
    if (!copyAside(userDrillSetPath(), stamp, &backups, &whyNot))  return refuse(whyNot);

    const CharacteristicPack prior        = m_working;
    const NormPack           priorNorms   = m_workingNorms;
    const ScreenSet          priorScreens = m_workingScreens;
    const DrillSet           priorDrills  = m_workingDrills;

    // Empty user layers, not deleted files: an absent layer and an empty one assemble identically,
    // and one that exists is one the next save can write to without re-deciding what its header says.
    CharacteristicPack blank;
    blank.id            = QStringLiteral("user");
    blank.version       = QStringLiteral("1");
    blank.schemaVersion = kPackSchemaVersion;
    NormPack blankNorms;
    blankNorms.id      = QStringLiteral("user");
    blankNorms.version = QStringLiteral("1");
    ScreenSet blankScreens;
    blankScreens.id      = QStringLiteral("user");
    blankScreens.version = QStringLiteral("1");
    DrillSet blankDrills;
    blankDrills.id      = QStringLiteral("user");
    blankDrills.version = QStringLiteral("1");

    m_working        = blank;
    m_workingNorms   = blankNorms;
    m_workingScreens = blankScreens;
    m_workingDrills  = blankDrills;
    m_resetCorridors.clear();
    rebuild();

    // A command like every other write, so ⌘Z holds the whole draft for the session. The label is
    // what the Edits list will show, and it has to be unmistakable next to "Revert".
    pushCommand(tr("Reset to the standard model"),
                tr("%n local change(s) removed", "", overridden + authored), prior, priorNorms,
                priorScreens, priorDrills);

    // Written through immediately. A reset that only cleared the draft would leave the files it just
    // backed up still on disk and the panel reading "n unsaved" — which is not what "reset" means to
    // anybody who clicked it.
    if (!saveUserPack(m_working, &whyNot) || !saveUserNormPack(m_workingNorms, &whyNot)
        || !saveUserScreenSet(m_workingScreens, &whyNot)
        || !saveUserDrillSet(m_workingDrills, &whyNot)) {
        // The draft is already the standard model and the stack still holds the way back, so this is
        // a failure to PERSIST rather than a failure to reset. Say exactly that.
        QVariantMap r = refuse(tr("Reset here, but it could not be written to disk. %1").arg(whyNot));
        r.insert(QStringLiteral("backups"), backups);
        return r;
    }
    resetSharedScreenSet();
    resetSharedDrillSet();
    m_savedUser    = m_working;
    m_savedNorms   = m_workingNorms;
    m_savedScreens = m_workingScreens;
    m_savedDrills  = m_workingDrills;
    m_savedIndex   = m_stackIndex;
    invalidateDerived();

    // Everybody else caches their provider, so without this the app keeps grading against the model
    // that was just reset away until relaunch.
    emit libraryChanged();

    QVariantMap r = accept(backups.isEmpty()
                               ? tr("Back on the standard model.")
                               : tr("Back on the standard model. Your version was copied to %1.")
                                     .arg(QFileInfo(backups.first()).fileName()));
    r.insert(QStringLiteral("backups"), backups);
    return r;
}

// ── Validation ──────────────────────────────────────────────────────────────

QVariantList ModelBrowser::validation() const
{
    // The health rows of the DRAFT, which is what makes the strip worth having: it grades what you
    // are about to save, not what is already on disk. Codes and their "what to DO" come from the
    // validator and diagnostics_health.h rather than being invented here.
    return rows(kHealth);
}

int ModelBrowser::validationErrorCount() const
{
    int n = 0;
    for (const QVariant &v : validation())
        if (v.toMap().value(QStringLiteral("severity")).toString() == QStringLiteral("error")) ++n;
    return n;
}

int ModelBrowser::validationWarningCount() const
{
    int n = 0;
    for (const QVariant &v : validation())
        if (v.toMap().value(QStringLiteral("severity")).toString() == QStringLiteral("warning")) ++n;
    return n;
}

// ── Editing ─────────────────────────────────────────────────────────────────

QVariantMap ModelBrowser::setField(const QString &type, const QString &id, const QString &field,
                                   const QVariant &value)
{
    const CharacteristicPack before        = m_working;
    const NormPack           normsBefore   = m_workingNorms;
    const ScreenSet          screensBefore = m_workingScreens;
    const DrillSet           drillsBefore  = m_workingDrills;
    const QString            text          = value.toString();

    // A refusal must leave NOTHING behind. The working-copy helpers below are copy-on-write, so
    // simply reaching for a shipped row copies it into the working pack — and a refusal that
    // returned without undoing that would leave an override nobody asked for, byte-identical to the
    // shipped row and marked as unsaved work. It is invisible until the author saves, and then it
    // is a permanent override of content they never edited. That applies to screens and drills for
    // exactly the same reason, so all four layers roll back together.
    auto reject = [&](const QString &why) {
        m_working        = before;
        m_workingNorms   = normsBefore;
        m_workingScreens = screensBefore;
        m_workingDrills  = drillsBefore;
        return refuse(why);
    };

    QString what;    // for the undo label
    QString subject; // for the Edits list detail line

    if (type == kCharacteristics || type == kCauses) {
        Condition *c = workingCondition(id);
        if (!c) return reject(tr("No characteristic with id %1.").arg(id));
        subject = c->label.isEmpty() ? c->id : c->label;

        if (field == QStringLiteral("label")) {
            // The id is NOT re-minted on rename: every edge pointing at it would break, and an edge
            // that points at nothing is not a rename, it is data loss.
            c->label = text;
            what     = tr("Name → %1").arg(text);
        } else if (field == QStringLiteral("group")) {
            ConditionGroup g{};
            if (!conditionGroupFromName(text, g)) return reject(tr("%1 is not a group.").arg(text));
            c->group = g;
            what     = tr("Group → %1").arg(conditionGroupLabel(g));
        } else if (field == QStringLiteral("reach")) {
            ConfirmedBy r{};
            if (!confirmedByFromName(text, r)) return reject(tr("%1 is not a reach.").arg(text));
            c->confirmedBy = r;
            what           = tr("Reach → %1").arg(reachLabel(r));
        } else if (field == QStringLiteral("tier")) {
            ProvenanceTier t{};
            if (!provenanceTierFromName(text, t)) return reject(tr("%1 is not a tier.").arg(text));
            // A tier that CLAIMS a source must name one. Refused before the write, in the author's
            // own terms, rather than letting the assembled-library validator fail the whole pack.
            if (citationRequired(t) && c->provenance.citation.isEmpty())
                return reject(tr("“%1” claims the literature supports this, so it needs a citation "
                                 "first.").arg(provenanceTierLabel(t)));
            c->provenance.tier = t;
            what               = tr("Evidence → %1").arg(provenanceTierLabel(t));
        } else if (field == QStringLiteral("consequence")) {
            c->consequence.byLocale.insert(QStringLiteral("en"), text);
            what = tr("Consequence edited");
        } else if (field == QStringLiteral("aliases")) {
            // One comma-separated line rather than a list editor: an author types "flip, flipping,
            // breakdown through impact" in one go, and a per-row add/remove UI would make the common
            // case the slow one. Trimmed and de-duplicated so the draft never carries a blank term
            // into the duplicate-alias lint.
            QStringList terms;
            for (const QString &raw : text.split(QLatin1Char(','))) {
                const QString t = raw.trimmed();
                if (!t.isEmpty() && !terms.contains(t, Qt::CaseInsensitive)) terms << t;
            }
            // Two conditions may not claim one coach term: search resolves to whichever came first
            // in the file, so the term silently leads to the wrong page. Refused before the write.
            for (const Condition &other : pack().conditions) {
                if (other.id == c->id) continue;
                for (const QString &t : terms)
                    if (other.aliases.contains(t, Qt::CaseInsensitive))
                        return reject(tr("“%1” already leads to %2.").arg(t, other.label));
            }
            c->aliases = terms;
            what       = tr("Also called → %1").arg(terms.join(QStringLiteral(", ")));
        } else if (field == QStringLiteral("injuryNote")) {
            c->injuryNote.byLocale.insert(QStringLiteral("en"), text);
            what = tr("Injury note edited");
        } else if (field == QStringLiteral("citation")) {
            c->provenance.citation = text;
            what                   = tr("Citation → %1").arg(text);
        } else if (field == QStringLiteral("state")) {
            ConditionState s{};
            if (!conditionStateFromName(text, s)) return reject(tr("%1 is not a state.").arg(text));
            c->state = s;
            what     = tr("State → %1").arg(text);
        } else {
            return reject(tr("%1 cannot be edited here.").arg(field));
        }

    } else if (type == kMeasures) {
        Measure *m = workingMeasure(id);
        if (!m) return reject(tr("No measure with id %1.").arg(id));
        subject = measureDisplayLabel(*m);

        if (field == QStringLiteral("label")) {
            m->label = text;
            what     = tr("Name → %1").arg(text.isEmpty() ? measureDisplayLabel(*m) : text);
        } else if (field == QStringLiteral("unit")) {
            m->unit = text;
            what    = tr("Unit → %1").arg(text);
        } else if (field == QStringLiteral("status")) {
            MeasureStatus s{};
            if (!measureStatusFromName(text, s)) return reject(tr("%1 is not a status.").arg(text));
            // Both of these statuses say something is permanently in the way, and only the reason
            // says what. Two surfaces quote that reason, so an empty one is refused rather than
            // shipped as a blank explanation.
            if ((s == MeasureStatus::NotCapturable || s == MeasureStatus::ExternalDevice)
                && m->gapReason.isEmpty())
                return reject(tr("“%1” needs a reason — it is what the roadmap and the detail page "
                                 "both quote.").arg(measureStatusLabel(s)));
            m->status = s;
            what      = tr("Status → %1").arg(measureStatusLabel(s));
        } else if (field == QStringLiteral("highMeans")) {
            m->highMeans = text;
            what         = tr("High means edited");
        } else if (field == QStringLiteral("gapReason")) {
            m->gapReason = text;
            what         = tr("Reason edited");
        } else {
            return reject(tr("%1 cannot be edited here.").arg(field));
        }

        // A measure is shared. The blast radius is stated in the result so the surface can say what
        // else just changed, rather than leaving the author to discover it.
        const int users = measureUsers(id);
        if (users > 1) subject = tr("%1 · %n characteristic(s) affected", "", users).arg(subject);

    } else if (type == kSignals) {
        Signal *s = workingSignal(id);
        if (!s) return reject(tr("No signal with id %1.").arg(id));
        subject = s->id;

        if (field == QStringLiteral("direction")) {
            Direction d{};
            if (!directionFromName(text, d)) return reject(tr("%1 is not a direction.").arg(text));
            // A tail the measure's SHAPE leaves ungraded can never fire, whatever the swing does.
            const Measure *m = s->measures.isEmpty() ? nullptr : pack().measure(s->measures.first());
            if (m && shapeIsOneSided(m->shape)) {
                const bool dead = (m->shape == Shape::Floor && d == Direction::High)
                               || (m->shape == Shape::Ceiling && d == Direction::Low);
                if (dead)
                    return reject(tr("%1 only grades one tail, so a signal on this side could never "
                                     "fire.").arg(measureDisplayLabel(*m)));
            }
            s->direction = d;
            what = tr("Direction → %1").arg(directionPhrase(d, m ? m->highMeans : QString()).label);
        } else {
            return reject(tr("%1 cannot be edited here.").arg(field));
        }

    } else if (type == kLinks) {
        QString from, to; EdgeType t = EdgeType::Causes;
        if (!splitEdgeId(id, from, to, t)) return reject(tr("Not a link id."));
        Edge *e = workingEdge(from, to, t);
        if (!e) return reject(tr("No such link."));

        const Condition *f = pack().condition(from);
        const Condition *o = pack().condition(to);
        subject = tr("%1 → %2").arg(f ? f->label : from, o ? o->label : to);

        if (field == QStringLiteral("strength")) {
            if (e->type != EdgeType::Causes)
                return reject(tr("A %1 relation has no strength.").arg(edgeTypeName(e->type)));
            Strength s{};
            if (!strengthFromName(text, s)) return reject(tr("%1 is not a strength.").arg(text));
            e->strength = s;
            what        = tr("Strength → %1").arg(strengthLabel(s));
        } else if (field == QStringLiteral("relation")) {
            EdgeType nt{};
            if (!edgeTypeFromName(text, nt)) return reject(tr("%1 is not a relation.").arg(text));
            if (nt == e->type) return accept(tr("Already %1.").arg(text));
            // Retyping to corroborates is illegal where a causal path already exists — the pair
            // would double-count in the confidence ranking and the validator rejects the library.
            //
            // Asked of the ASSEMBLED pack WITHOUT this edge, which is the whole subtlety: a causal
            // edge IS a causal path, so testing with it still in place would refuse every retype
            // including the ones that are fine. What has to be true is that the pair is still
            // connected once this edge is gone.
            if (nt == EdgeType::Corroborates) {
                CharacteristicPack probe = pack();
                for (auto it = probe.edges.begin(); it != probe.edges.end(); ++it)
                    if (it->from == from && it->to == to && it->type == e->type) {
                        probe.edges.erase(it);
                        break;
                    }
                if (hasCausalPath(probe, from, to) || hasCausalPath(probe, to, from))
                    return reject(tr("%1 and %2 already have a causal path, so they cannot also "
                                     "corroborate each other.")
                                      .arg(f ? f->label : from, o ? o->label : to));
            }
            e->type = nt;
            what    = tr("Relation → %1").arg(text);
        } else if (field == QStringLiteral("tier")) {
            ProvenanceTier t2{};
            if (!provenanceTierFromName(text, t2)) return reject(tr("%1 is not a tier.").arg(text));
            if (citationRequired(t2) && e->provenance.citation.isEmpty())
                return reject(tr("“%1” claims the literature supports this link, so it needs a "
                                 "citation first.").arg(provenanceTierLabel(t2)));
            e->provenance.tier = t2;
            what               = tr("Evidence → %1").arg(provenanceTierLabel(t2));
        } else if (field == QStringLiteral("citation")) {
            e->provenance.citation = text;
            what                   = tr("Citation → %1").arg(text);
        } else {
            return reject(tr("%1 cannot be edited here.").arg(field));
        }

    } else if (type == kCorridors) {
        QString mid, ctx;
        if (!splitCorridorId(id, mid, ctx)) return reject(tr("Not a corridor id."));

        Norm *n = workingNorm(mid, ctx);
        if (!n) return reject(tr("No corridor resolves at %1.").arg(ctx));

        const Measure *meas = pack().measure(mid);
        subject = tr("%1 at %2").arg(meas ? measureDisplayLabel(*meas) : mid, ctx);

        bool         numberOk = false;
        const double num      = value.toDouble(&numberOk);

        // Snapped in whatever terms the author stated it: an edge as an edge, a tolerance as a
        // tolerance. Snapping the derived value instead would land the thing being dragged between
        // two stops, which is the opposite of what a quantum is for.
        const CorridorPrecision prec = corridorPrecisionFor(n->unit.isEmpty()
                                                                ? (meas ? meas->unit : QString())
                                                                : n->unit);
        auto snap = [&](double v) { return snapCorridorValue(v, n->unit.isEmpty()
                                                                    ? (meas ? meas->unit : QString())
                                                                    : n->unit); };
        Q_UNUSED(prec)

        if (field == QStringLiteral("mu")) {
            if (!numberOk) return reject(tr("%1 is not a number.").arg(text));
            n->mu = snap(num);
            what  = tr("Aspiration → %1").arg(n->mu);
        } else if (field == QStringLiteral("idealLo") || field == QStringLiteral("idealHi")) {
            // What a HANDLE drags. The ideal edge is `mu ∓ idealMaxZ * sigma`, so an edge has to be
            // divided back through the policy before it can be stored — otherwise a corridor
            // authored under Strict comes out three times tighter than it was drawn, and under
            // Lenient three times looser. Storing the edge itself is not an option: the edge moves
            // with the policy, and freezing one policy's arithmetic into the content is the bug
            // norm.h spends a paragraph on.
            if (!numberOk) return reject(tr("%1 is not a number.").arg(text));
            const double z    = std::max(1e-9, gradePolicyByName(m_policyName).idealMaxZ);
            const double edge = snap(num);
            const double half = std::abs(edge - n->mu) / z;
            if (field == QStringLiteral("idealLo")) n->sigmaLo = half;
            else                                    n->sigmaHi = half;
            what = tr("Corridor edge → %1").arg(edge);
        } else if (field == QStringLiteral("sigmaLo") || field == QStringLiteral("sigmaHi")) {
            if (!numberOk) return reject(tr("%1 is not a number.").arg(text));
            // A tolerance is a half-width, so it cannot be negative: a negative one inverts the
            // band and grades the middle of the corridor as a fault.
            if (num < 0) return reject(tr("A tolerance cannot be negative."));
            const double t = snap(num);
            if (field == QStringLiteral("sigmaLo")) n->sigmaLo = t;
            else                                    n->sigmaHi = t;
            what = tr("Tolerance → %1").arg(t);
        } else if (field == QStringLiteral("plausibleLo")) {
            if (text.isEmpty())      n->plausibleLo.reset();
            else if (!numberOk)      return reject(tr("%1 is not a number.").arg(text));
            else                     n->plausibleLo = num;
            what = tr("Plausible low → %1").arg(text.isEmpty() ? tr("none") : text);
        } else if (field == QStringLiteral("plausibleHi")) {
            if (text.isEmpty())      n->plausibleHi.reset();
            else if (!numberOk)      return reject(tr("%1 is not a number.").arg(text));
            else                     n->plausibleHi = num;
            what = tr("Plausible high → %1").arg(text.isEmpty() ? tr("none") : text);
        } else if (field == QStringLiteral("unit")) {
            // The loader REFUSES a norm whose unit disagrees with its measure, so allowing one to be
            // typed here would author a set that cannot be read back in.
            if (meas && !meas->unit.isEmpty() && text != meas->unit)
                return reject(tr("%1 is measured in %2, so its corridor has to be too.")
                                  .arg(measureDisplayLabel(*meas), meas->unit));
            n->unit = text;
            what    = tr("Unit → %1").arg(text);
        } else if (field == QStringLiteral("source")) {
            NormSource src{};
            if (!normSourceFromName(text, src)) return reject(tr("%1 is not a source.").arg(text));
            n->source = src;
            what      = tr("Source → %1").arg(text);
        } else if (field == QStringLiteral("citation")) {
            n->citation = text;
            what        = tr("Citation → %1").arg(text);
        } else {
            return reject(tr("%1 cannot be edited here.").arg(field));
        }

        // Nothing outside this measure moves, but plenty inside it does: every characteristic whose
        // signal grades against this corridor now grades differently. Stated in the result so the
        // surface can report it rather than leaving the author to discover it.
        const int corridorUsers = measureUsers(mid);
        if (corridorUsers > 0)
            subject = tr("%1 · %n characteristic(s) affected", "", corridorUsers).arg(subject);

    } else if (type == kScreens) {
        Screen *s = workingScreen(id);
        if (!s) return reject(tr("No screen with id %1.").arg(id));
        subject = s->label.isEmpty() ? s->id : s->label;

        if (field == QStringLiteral("name")) {
            s->label = text;
            what     = tr("Name → %1").arg(text);
            subject  = text;
        } else if (field == QStringLiteral("region")) {
            s->bodyRegion = text;
            what          = tr("Region → %1").arg(text);
        } else if (field == QStringLiteral("protocol")) {
            s->protocol = text;
            what        = tr("Protocol edited");
        } else if (field == QStringLiteral("passCriterion")) {
            s->passCriterion = text;
            what             = tr("Passing looks like → %1").arg(text);
        } else if (field == QStringLiteral("note")) {
            s->note = text;
            what    = tr("What it does not settle edited");
        } else if (field == QStringLiteral("unit")) {
            // A numeric pass floor with no unit is an ERROR in validateScreenSet(), so clearing the
            // unit out from under one would author a set that cannot be read back in.
            if (text.isEmpty() && s->passAtLeast.has_value())
                return reject(tr("“%1” states a pass floor of %2, so it needs a unit.")
                                  .arg(subject).arg(*s->passAtLeast));
            s->unit = text;
            what    = tr("Unit → %1").arg(text);
        } else if (field == QStringLiteral("passAtLeast")) {
            // Empty CLEARS it — several screens are qualitative ("achieves neutral extension") and
            // inventing a number for those is worse than letting the words do the work.
            if (text.trimmed().isEmpty()) {
                s->passAtLeast.reset();
                what = tr("Pass floor cleared");
            } else {
                bool         ok  = false;
                const double num = text.toDouble(&ok);
                if (!ok) return reject(tr("%1 is not a number.").arg(text));
                if (s->unit.isEmpty())
                    return reject(tr("A pass floor needs a unit, or the number is unreadable."));
                s->passAtLeast = num;
                what           = tr("Pass floor → %1 %2").arg(text, s->unit);
            }
        } else if (field == QStringLiteral("citation")) {
            s->citation = text;
            what        = tr("Citation → %1").arg(text);
        } else {
            return reject(tr("%1 cannot be edited here.").arg(field));
        }

    } else if (type == kDrills) {
        Drill *d = workingDrill(id);
        if (!d) return reject(tr("No drill with id %1.").arg(id));
        subject = d->label.isEmpty() ? d->id : d->label;

        if (field == QStringLiteral("name")) {
            d->label = text;
            what     = tr("Name → %1").arg(text);
            subject  = text;
        } else if (field == QStringLiteral("instruction")) {
            d->instruction = text;
            what           = tr("What the golfer does edited");
        } else if (field == QStringLiteral("targets")) {
            d->targets = text;
            what       = tr("Trying to change → %1").arg(text);
        } else if (field == QStringLiteral("equipment")) {
            // The cell shows a ` · `-joined line and this splits it back. Blank entries are dropped
            // rather than stored: "mat · " would render as a phantom second item.
            QStringList items;
            for (const QString &raw : text.split(QStringLiteral("·"))) {
                const QString t = raw.trimmed();
                if (!t.isEmpty() && !items.contains(t, Qt::CaseInsensitive)) items << t;
            }
            d->equipment = items;
            what = items.isEmpty() ? tr("Equipment cleared")
                                   : tr("Equipment → %1").arg(items.join(QStringLiteral(" · ")));
        } else if (field == QStringLiteral("note")) {
            d->note = text;
            what    = tr("When it is the wrong drill edited");
        } else {
            return reject(tr("%1 cannot be edited here.").arg(field));
        }

    } else {
        // References are IMPORTED — regenerated on every pack build — so editing one here would be
        // overwritten. Saying so is better than a control that looks live and does nothing.
        return reject(tr("%1 are read-only in this panel.").arg(typeLabelFor(type)));
    }

    rebuild();
    // `normsBefore`, never m_workingNorms — that is the state AFTER this edit, and passing it would
    // make a corridor edit unable to undo itself while looking perfectly correct on the pack side.
    pushCommand(what, subject, before, normsBefore, screensBefore, drillsBefore);
    return accept(what);
}

QVariantMap ModelBrowser::setFieldOnAll(const QString &type, const QStringList &ids,
                                        const QString &field, const QVariant &value)
{
    if (ids.isEmpty()) return refuse(tr("Nothing selected."));

    // ONE command for the whole selection. Re-tiering twelve links has to undo as one action —
    // twelve undos to reverse one gesture is a stack that punishes the shortcut it exists to enable.
    const CharacteristicPack before = m_working;

    int     done = 0;
    QString firstRefusal;
    QString what;
    for (const QString &id : ids) {
        const QVariantMap r = setField(type, id, field, value);
        if (r.value(QStringLiteral("ok")).toBool()) {
            ++done;
            if (what.isEmpty()) what = r.value(QStringLiteral("message")).toString();
        } else if (firstRefusal.isEmpty()) {
            firstRefusal = r.value(QStringLiteral("message")).toString();
        }
    }

    // Collapse the per-row commands those calls pushed back into one. They were pushed against
    // intermediate states, so dropping them and re-pushing from `before` is what makes the whole
    // selection a single undo step.
    if (done > 0) {
        m_stack.erase(m_stack.begin() + (m_stackIndex + 1 - done), m_stack.begin() + (m_stackIndex + 1));
        m_stackIndex -= done;
        pushCommand(what, tr("%n row(s)", "", done), before, m_workingNorms);
    }

    if (done == 0) return refuse(firstRefusal.isEmpty() ? tr("Nothing changed.") : firstRefusal);
    if (!firstRefusal.isEmpty())
        return accept(tr("%n row(s) changed. %1", "", done).arg(firstRefusal));
    return accept(tr("%n row(s) changed", "", done));
}

QVariantMap ModelBrowser::linkLegality(const QString &fromId, const QString &toId,
                                       const QString &relation) const
{
    const CharacteristicPack &p = pack();

    auto no = [](const QString &why) {
        QVariantMap r;
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("reason"), why);
        return r;
    };

    EdgeType type{};
    if (!edgeTypeFromName(relation, type)) return no(tr("%1 is not a relation.").arg(relation));

    const Condition *f = p.condition(fromId);
    const Condition *o = p.condition(toId);
    if (!f || !o) return no(tr("Both ends have to exist."));

    if (fromId == toId) return no(tr("A condition cannot cause itself."));
    for (const Edge &e : p.edges)
        if (e.type == type && e.from == fromId && e.to == toId)
            return no(tr("%1 already %2 %3.").arg(f->label, relation, o->label));
    if (type == EdgeType::Causes && hasCausalPath(p, toId, fromId))
        return no(tr("%1 already leads to %2, so this would create a cycle.")
                      .arg(o->label, f->label));
    if (type == EdgeType::Corroborates
        && (hasCausalPath(p, fromId, toId) || hasCausalPath(p, toId, fromId)))
        return no(tr("%1 and %2 already have a causal path, so they cannot also corroborate "
                     "each other.").arg(f->label, o->label));

    QVariantMap r;
    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("reason"), tr("%1 %2 %3").arg(f->label, relation, o->label));
    return r;
}

QVariantMap ModelBrowser::addLink(const QString &fromId, const QString &toId,
                                  const QString &relation, const QString &strength)
{
    const CharacteristicPack &p = pack();

    EdgeType type{};
    if (!edgeTypeFromName(relation, type)) return refuse(tr("%1 is not a relation.").arg(relation));
    Strength s{};
    if (!strengthFromName(strength, s)) return refuse(tr("%1 is not a strength.").arg(strength));

    // Every refusal happens BEFORE anything is written, in the author's own terms — and through the
    // SAME function the graph drag consults, so a drag can never say yes to something this then
    // rejects. A graph edit that half-lands is not recoverable by a reader.
    const QVariantMap legal = linkLegality(fromId, toId, relation);
    if (!legal.value(QStringLiteral("ok")).toBool())
        return refuse(legal.value(QStringLiteral("reason")).toString());

    // The LABELS, taken now — not the pointers. rebuild() destroys the assembly these came out of,
    // so reading `f->label` after it is a read of freed memory. It crashed inside QString::arg()
    // rather than at the dereference, which is what makes this shape worth naming: the stack blames
    // the formatting, and the fault is three lines earlier.
    const Condition *f = p.condition(fromId);
    const Condition *o = p.condition(toId);
    const QString    fromLabel = f ? f->label : fromId;
    const QString    toLabel   = o ? o->label : toId;

    const CharacteristicPack before = m_working;
    if (type == EdgeType::Causes) materialiseCausesOf(toId);

    Edge e;
    e.from     = fromId;
    e.to       = toId;
    e.type     = type;
    e.strength = s;
    // A new claim nobody has been to the literature for is `proposed`, and the UI badges it. Filing
    // it as anything else would be the overclaim the tier ladder exists to prevent.
    e.provenance.tier = ProvenanceTier::Proposed;
    m_working.edges.push_back(e);

    rebuild();
    const QString what = tr("%1 %2 %3").arg(fromLabel, relation, toLabel);
    pushCommand(tr("Link added"), what, before, m_workingNorms);
    return accept(what);
}

QVariantMap ModelBrowser::removeLink(const QString &fromId, const QString &toId,
                                     const QString &relation)
{
    EdgeType type{};
    if (!edgeTypeFromName(relation, type)) return refuse(tr("%1 is not a relation.").arg(relation));

    const CharacteristicPack &p = pack();
    const Condition          *f = p.condition(fromId);
    const Condition          *o = p.condition(toId);
    // Resolved to strings now: rebuild() below frees the assembly these point into.
    const QString fromLabel = f ? f->label : fromId;
    const QString toLabel   = o ? o->label : toId;

    bool exists = false;
    for (const Edge &e : p.edges)
        if (e.type == type && e.from == fromId && e.to == toId) exists = true;
    if (!exists) return refuse(tr("No such link."));

    const CharacteristicPack before = m_working;

    // The whole incoming causal set has to be written back, or removing one shipped cause would
    // silently drop the others when the override replaces the set.
    if (type == EdgeType::Causes) materialiseCausesOf(toId);
    for (auto it = m_working.edges.begin(); it != m_working.edges.end(); ++it)
        if (it->from == fromId && it->to == toId && it->type == type) {
            m_working.edges.erase(it);
            break;
        }

    // A SYMMETRIC edge belongs to neither end, so "absent from my list" does not remove it — the
    // user pack has to tombstone it explicitly or the shipped one comes straight back.
    if (type != EdgeType::Causes) {
        Edge t;
        t.from = fromId;
        t.to   = toId;
        t.type = type;
        m_working.retiredEdges.push_back(t);
    }

    rebuild();
    const QString what = tr("%1 %2 %3").arg(fromLabel, relation, toLabel);
    pushCommand(tr("Link removed"), what, before, m_workingNorms);
    return accept(tr("Removed: %1").arg(what));
}

QVariantMap ModelBrowser::addMeasureTo(const QString &conditionId, const QString &measureId,
                                       const QString &direction)
{
    const CharacteristicPack &p = pack();

    Direction d{};
    if (!directionFromName(direction, d)) return refuse(tr("%1 is not a direction.").arg(direction));
    const Measure *m = p.measure(measureId);
    if (!m) return refuse(tr("No measure with id %1.").arg(measureId));
    if (!p.condition(conditionId)) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    // The derived label, resolved while the pack it comes from is still alive.
    const QString measureLabel = measureDisplayLabel(*m);

    if (shapeIsOneSided(m->shape)) {
        const bool dead = (m->shape == Shape::Floor && d == Direction::High)
                       || (m->shape == Shape::Ceiling && d == Direction::Low);
        if (dead)
            return refuse(tr("%1 only grades one tail, so a signal on this side could never fire.")
                              .arg(measureDisplayLabel(*m)));
    }

    const CharacteristicPack before = m_working;

    // Same minted id as the old editor's attachMeasure(), deliberately: two spellings of one signal
    // id would let the same measure be attached twice at one tail.
    Signal s;
    s.id        = QStringLiteral("sig_%1_%2").arg(measureId, direction);
    s.test      = SignalTest::OutsideCorridor;   // authors no numbers; inherits the corridor
    s.measures  = { measureId };
    s.direction = d;

    bool haveSignal = false;
    for (const Signal &x : m_working.signalDefs)
        if (x.id == s.id) haveSignal = true;
    if (!haveSignal && !p.signal(s.id)) m_working.signalDefs.push_back(s);

    Condition *c = workingCondition(conditionId);
    if (!c) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    // The label as a STRING, while `c` is known good. Restoring the working pack below assigns over
    // the vector `c` points into, and rebuild() at the end is the other end of the same hazard —
    // this one copy covers both.
    const QString condLabel = c->label;
    if (c->detectedBy.contains(s.id)) {
        m_working = before;   // same reason as setField's reject(): leave nothing behind
        return refuse(tr("%1 already reads that measure on that side.").arg(condLabel));
    }
    c->detectedBy << s.id;

    // Carry the measure into the working pack too: a signal on a measure the user pack does not
    // contain would dangle after an app update.
    bool haveMeasure = false;
    for (const Measure &x : m_working.measures)
        if (x.id == measureId) haveMeasure = true;
    if (!haveMeasure) m_working.measures.push_back(*m);

    // Both halves are strings taken while their owners were alive — rebuild() below frees the pack
    // `m` came out of, and the restore path above assigns over the vector `c` points into.
    rebuild();
    const QString what = tr("%1 reads %2").arg(condLabel, measureLabel);
    pushCommand(tr("Measure added"), what, before, m_workingNorms);
    return accept(what);
}

QVariantMap ModelBrowser::removeMeasureFrom(const QString &conditionId, const QString &measureId)
{
    const CharacteristicPack &p = pack();
    const Measure            *m = p.measure(measureId);
    // Resolved now, because rebuild() below frees the pack `m` points into.
    const QString measureLabel = m ? measureDisplayLabel(*m) : measureId;

    // Captured before the first copy-on-write, not after: workingCondition() below copies a shipped
    // condition into the working pack just by being asked for it, and a refusal past that point has
    // to put the pack back exactly as it found it.
    const CharacteristicPack before = m_working;

    Condition *c = workingCondition(conditionId);
    if (!c) {
        m_working = before;
        return refuse(tr("No characteristic with id %1.").arg(conditionId));
    }
    // As a string, while `c` is known good: the restore below assigns over the vector it points
    // into, and rebuild() at the end frees nothing of m_working but is the same hazard's other end.
    const QString condLabel = c->label;

    QStringList drop;
    for (const QString &sid : c->detectedBy)
        if (const Signal *s = p.signal(sid))
            if (s->measures.contains(measureId)) drop << sid;
    if (drop.isEmpty()) {
        m_working = before;   // workingCondition() above may have copied it; undo that
        return refuse(tr("%1 does not read that measure.").arg(condLabel));
    }

    for (const QString &sid : drop) c->detectedBy.removeAll(sid);

    rebuild();
    const QString what = tr("%1 no longer reads %2").arg(condLabel, measureLabel);
    pushCommand(tr("Measure removed"), what, before, m_workingNorms);
    return accept(what);
}

namespace {

// The house id-minting rule, in one place: readable, stable, never reused. Two paths inventing two
// conventions is how one library ends up with `hip_turn_copy` and `hip-turn-2`.
QString mintId(const QString &fromLabel, const QString &prefix, const QString &fallback,
               const std::function<bool(const QString &)> &taken)
{
    QString base;
    for (const QChar &ch : fromLabel.toLower())
        base += ch.isLetterOrNumber() ? ch : QLatin1Char('_');
    while (base.contains(QStringLiteral("__"))) base.replace(QStringLiteral("__"), QStringLiteral("_"));
    base = base.mid(0, 48);
    while (base.endsWith(QLatin1Char('_'))) base.chop(1);
    while (base.startsWith(QLatin1Char('_'))) base.remove(0, 1);
    if (base.isEmpty()) base = fallback;

    QString id = prefix + base;
    int     n  = 2;
    while (taken(id)) id = QStringLiteral("%1%2_%3").arg(prefix, base).arg(n++);
    return id;
}

} // namespace

QVariantMap ModelBrowser::duplicate(const QString &type, const QString &id)
{
    const CharacteristicPack &p = pack();

    // Screens and drills duplicate the same way everything else does — the whole point of duplicate
    // is that it beats blank, and it is at its most useful on a screen protocol somebody has already
    // written half of.
    if (type == kScreens) {
        const Screen *src = m_screens.screen(id);
        if (!src) return refuse(tr("No screen with id %1.").arg(id));

        const ScreenSet screensBefore = m_workingScreens;
        Screen          s             = *src;
        s.label = tr("%1 (copy)").arg(src->label);
        s.id    = mintId(s.label, QStringLiteral("screen."), QStringLiteral("screen"),
                         [this](const QString &x) { return m_screens.screen(x) != nullptr; });
        // The citation is NOT copied: the paper was cited for the original protocol, and a copy
        // somebody is about to rewrite has not earned it.
        s.citation.clear();
        m_workingScreens.screens.push_back(s);

        rebuild();
        pushCommand(tr("Duplicated"), s.label, m_working, m_workingNorms, screensBefore,
                    m_workingDrills);
        QVariantMap r = accept(tr("Created %1").arg(s.label));
        r.insert(QStringLiteral("id"), s.id);
        r.insert(QStringLiteral("type"), kScreens);
        return r;
    }
    if (type == kDrills) {
        const Drill *src = m_drills.drill(id);
        if (!src) return refuse(tr("No drill with id %1.").arg(id));

        const DrillSet drillsBefore = m_workingDrills;
        Drill          d            = *src;
        d.label = tr("%1 (copy)").arg(src->label);
        d.id    = mintId(d.label, QStringLiteral("drill."), QStringLiteral("drill"),
                         [this](const QString &x) { return m_drills.drill(x) != nullptr; });
        m_workingDrills.drills.push_back(d);

        rebuild();
        pushCommand(tr("Duplicated"), d.label, m_working, m_workingNorms, m_workingScreens,
                    drillsBefore);
        QVariantMap r = accept(tr("Created %1").arg(d.label));
        r.insert(QStringLiteral("id"), d.id);
        r.insert(QStringLiteral("type"), kDrills);
        return r;
    }

    if (type != kCharacteristics && type != kCauses)
        return refuse(tr("%1 cannot be duplicated here.").arg(typeLabelFor(type)));

    const Condition *src = p.condition(id);
    if (!src) return refuse(tr("No characteristic with id %1.").arg(id));

    const CharacteristicPack before = m_working;

    Condition c = *src;
    // Readable, stable, never reused — the same minting rule the old editor uses, so two paths
    // cannot produce two id conventions in one library.
    QString base;
    for (const QChar &ch : QStringLiteral("%1 copy").arg(src->label).toLower())
        base += ch.isLetterOrNumber() ? ch : QLatin1Char('_');
    while (base.contains(QStringLiteral("__"))) base.replace(QStringLiteral("__"), QStringLiteral("_"));
    base = base.mid(0, 48);
    while (base.endsWith(QLatin1Char('_'))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("characteristic");

    QString newId = base;
    int     n     = 2;
    while (p.condition(newId) != nullptr) newId = QStringLiteral("%1_%2").arg(base).arg(n++);

    c.id    = newId;
    c.label = tr("%1 (copy)").arg(src->label);
    // Aliases are NOT copied. Two conditions answering to one coach term is a load warning
    // (`duplicateAlias`) and search would silently resolve to whichever came first in the file.
    c.aliases.clear();
    // Nor is the axis: an axis joins the two tails of ONE measure, and a third condition claiming it
    // makes the pair meaningless.
    c.axis.clear();
    // A copy is somebody's draft until they say otherwise, and it inherits no evidence — the source
    // material was cited for the original claim, not for this one.
    c.state            = ConditionState::Draft;
    c.provenance.tier  = ProvenanceTier::Proposed;
    c.provenance.citation.clear();
    m_working.conditions.push_back(c);

    // The causal edges INTO it are copied, because "like this one" means the same explanation. Edges
    // out of it are not: the copy explains nothing yet, and claiming it did would be a claim the
    // author never made.
    for (const Edge &e : p.edges)
        if (e.type == EdgeType::Causes && e.to == id) {
            Edge n2 = e;
            n2.to   = newId;
            m_working.edges.push_back(n2);
        }

    rebuild();
    pushCommand(tr("Duplicated"), c.label, before, m_workingNorms);

    QVariantMap r = accept(tr("Created %1").arg(c.label));
    r.insert(QStringLiteral("id"), newId);
    r.insert(QStringLiteral("type"), kCharacteristics);
    return r;
}

QVariantMap ModelBrowser::removeObject(const QString &type, const QString &id)
{
    // A link is addressed by its composed row id everywhere else in the panel, so it is addressed
    // that way here too and unpacked once. Without this every caller had to know that links are
    // removed through a different function with three arguments — which the inspector's Delete
    // button, acting on whatever is selected, has no way to know.
    if (type == kLinks) {
        QString  from, to;
        EdgeType et = EdgeType::Causes;
        if (!splitEdgeId(id, from, to, et)) return refuse(tr("Not a link id: %1.").arg(id));
        return removeLink(from, to, edgeTypeName(et));
    }

    // Screens and drills, on the same rule the pack side uses: a SHIPPED row cannot be removed —
    // dropping the user's override restores it — and one of the author's own goes for good, along
    // with every condition's reference to it, because a `screenRef` pointing at nothing is a
    // dangling join rather than a loose end.
    if (type == kScreens || type == kDrills) {
        const bool isScreen  = type == kScreens;
        const bool isShipped = isScreen ? coreScreenSet().screen(id) != nullptr
                                        : coreDrillSet().drill(id) != nullptr;
        const bool inWorking = isScreen ? m_workingScreens.screen(id) != nullptr
                                        : m_workingDrills.drill(id) != nullptr;
        if (!inWorking)
            return refuse(isShipped
                              ? tr("This is shipped content. Edit it and the change becomes yours; "
                                   "there is nothing of yours here to remove.")
                              : tr("Nothing here with id %1.").arg(id));

        const CharacteristicPack before        = m_working;
        const ScreenSet          screensBefore = m_workingScreens;
        const DrillSet           drillsBefore  = m_workingDrills;

        QString label;
        if (isScreen) {
            for (auto it = m_workingScreens.screens.begin(); it != m_workingScreens.screens.end(); ++it)
                if (it->id == id) { label = it->label; m_workingScreens.screens.erase(it); break; }
        } else {
            for (auto it = m_workingDrills.drills.begin(); it != m_workingDrills.drills.end(); ++it)
                if (it->id == id) { label = it->label; m_workingDrills.drills.erase(it); break; }
        }

        // Only for a row that is going entirely. Removing an OVERRIDE restores the shipped screen,
        // which every condition still legitimately points at.
        int detached = 0;
        if (!isShipped) {
            for (const Condition &c : pack().conditions) {
                if (isScreen ? (c.screenRef != id) : (!c.drills.contains(id))) continue;
                Condition *w = workingCondition(c.id);
                if (!w) continue;
                if (isScreen) w->screenRef.clear();
                else          w->drills.removeAll(id);
                ++detached;
            }
        }

        rebuild();
        pushCommand(isShipped ? tr("Override removed") : tr("Deleted"),
                    detached > 0 ? tr("%1 · detached from %n characteristic(s)", "", detached).arg(label)
                                 : label,
                    before, m_workingNorms, screensBefore, drillsBefore);
        return accept(isShipped ? tr("Restored the shipped %1").arg(label)
                                : tr("Moved %1 to trash").arg(label));
    }

    if (type != kCharacteristics && type != kCauses)
        return refuse(tr("%1 cannot be removed here.").arg(typeLabelFor(type)));

    // Removing a SHIPPED characteristic is not something this panel can do: the shipped pack is
    // never modified, and an override that merely hid it would leave every edge pointing at it
    // dangling. Dropping the user's own row restores the shipped definition, which is a different
    // action with a different meaning, and it is what happens below.
    const bool isShipped = m_core->pack().condition(id) != nullptr;

    bool inWorking = false;
    for (const Condition &c : m_working.conditions)
        if (c.id == id) inWorking = true;
    if (!inWorking)
        return refuse(isShipped ? tr("This is shipped content. Edit it and the change becomes "
                                     "yours; there is nothing of yours here to remove.")
                                : tr("No characteristic with id %1.").arg(id));

    const CharacteristicPack before = m_working;
    QString label;
    for (auto it = m_working.conditions.begin(); it != m_working.conditions.end(); ++it)
        if (it->id == id) { label = it->label; m_working.conditions.erase(it); break; }

    // Its edges go too — an edge to a condition that no longer exists is an `unknownCondition`
    // error that fails the whole library, not a loose end.
    for (auto it = m_working.edges.begin(); it != m_working.edges.end();)
        if (it->from == id || it->to == id) it = m_working.edges.erase(it);
        else                                ++it;

    rebuild();
    pushCommand(isShipped ? tr("Override removed") : tr("Deleted"), label, before, m_workingNorms);
    return accept(isShipped ? tr("Restored the shipped %1").arg(label)
                            : tr("Deleted %1").arg(label));
}

// ── What a screen settles, and what a drill answers ─────────────────────────
//
// Both relationships are stored on the CONDITION — `Condition::screenRef` and `Condition::drills` —
// which is the join every reader in the app already uses. So all four of these write the pack, not
// the screen or drill set, and the ordinary copy-on-write applies: pointing a shipped condition at a
// screen makes an override of that condition, and undoing the first such edit removes it again.
//
// They live on this façade rather than being expressed as setField("characteristics", …) because the
// question an author has open is "what does THIS screen settle" — the object they are looking at is
// the screen, and an API that made them address the condition would be an API that matched the file
// format rather than the work.

QVariantMap ModelBrowser::addScreenSettles(const QString &screenId, const QString &conditionId)
{
    const Screen *s = m_screens.screen(screenId);
    if (!s) return refuse(tr("No screen with id %1.").arg(screenId));
    const Condition *target = pack().condition(conditionId);
    if (!target) return refuse(tr("No characteristic with id %1.").arg(conditionId));

    // ONE screen per condition — `screenRef` is a single field, not a list. Reassigning is a real
    // edit an author may well mean, so it is allowed and NAMED rather than silently done.
    const Screen *previous = target->screenRef.isEmpty() ? nullptr
                                                         : m_screens.screen(target->screenRef);
    if (target->screenRef == screenId)
        return refuse(tr("“%1” already settles %2.").arg(s->label, target->label));

    // Both labels as STRINGS: rebuild() reassigns m_screens, so `s` and `previous` are freed with
    // the assembly they came out of. `c` below points into m_working, which rebuild() leaves alone.
    const QString screenLabel   = s->label;
    const QString previousLabel = previous ? previous->label : QString();

    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    Condition *c = workingCondition(conditionId);
    if (!c) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    c->screenRef = screenId;

    rebuild();
    pushCommand(tr("Settles %1").arg(c->label),
                previous ? tr("%1 replaces %2").arg(screenLabel, previousLabel) : screenLabel,
                before, normsBefore);
    return accept(previous
                      ? tr("“%1” now settles %2, in place of “%3”").arg(screenLabel, c->label,
                                                                        previousLabel)
                      : tr("“%1” now settles %2").arg(screenLabel, c->label));
}

QVariantMap ModelBrowser::removeScreenSettles(const QString &screenId, const QString &conditionId)
{
    const Condition *target = pack().condition(conditionId);
    if (!target) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    if (target->screenRef != screenId)
        return refuse(tr("This screen does not settle %1.").arg(target->label));

    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    Condition *c = workingCondition(conditionId);
    if (!c) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    c->screenRef.clear();

    rebuild();
    pushCommand(tr("No longer settles %1").arg(c->label), c->label, before, normsBefore);
    return accept(tr("No longer settles %1").arg(c->label));
}

QVariantMap ModelBrowser::addDrillAnswers(const QString &drillId, const QString &conditionId)
{
    const Drill *d = m_drills.drill(drillId);
    if (!d) return refuse(tr("No drill with id %1.").arg(drillId));
    const Condition *target = pack().condition(conditionId);
    if (!target) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    if (target->drills.contains(drillId))
        return refuse(tr("“%1” already answers %2.").arg(d->label, target->label));
    // As a string, for the same reason: rebuild() reassigns m_drills.
    const QString drillLabel = d->label;

    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    Condition *c = workingCondition(conditionId);
    if (!c) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    c->drills << drillId;

    rebuild();
    pushCommand(tr("Answers %1").arg(c->label), drillLabel, before, normsBefore);
    return accept(tr("“%1” now answers %2").arg(drillLabel, c->label));
}

QVariantMap ModelBrowser::removeDrillAnswers(const QString &drillId, const QString &conditionId)
{
    const Condition *target = pack().condition(conditionId);
    if (!target) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    if (!target->drills.contains(drillId))
        return refuse(tr("This drill does not answer %1.").arg(target->label));

    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    Condition *c = workingCondition(conditionId);
    if (!c) return refuse(tr("No characteristic with id %1.").arg(conditionId));
    c->drills.removeAll(drillId);

    rebuild();
    pushCommand(tr("No longer answers %1").arg(c->label), c->label, before, normsBefore);
    return accept(tr("No longer answers %1").arg(c->label));
}

namespace {

// The candidate row shape linkCandidates() returns, so ModelPicker renders all of them alike.
QVariantMap candidateRow(const QString &id, const QString &label, const QString &detail)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("label"), label);
    m.insert(QStringLiteral("detail"), detail);
    return m;
}

} // namespace

QVariantList ModelBrowser::screenCandidates(const QString &screenId, const QString &search) const
{
    QVariantList out;
    for (const Condition &c : pack().conditions) {
        // Already settled by THIS screen is not a candidate. Settled by another one still is — and
        // it says so, because reassigning is a real edit and the author should see they are taking
        // it off something.
        if (c.screenRef == screenId) continue;
        if (!search.isEmpty() && !matches(c.label + QLatin1Char(' ') + c.id, search)) continue;

        QString detail;
        if (!c.screenRef.isEmpty()) {
            const Screen *held = m_screens.screen(c.screenRef);
            detail = tr("currently settled by %1").arg(held ? held->label : c.screenRef);
        }
        out.append(candidateRow(c.id, c.label, detail));
    }
    return out;
}

QVariantList ModelBrowser::drillCandidates(const QString &drillId, const QString &search) const
{
    QVariantList out;
    for (const Condition &c : pack().conditions) {
        if (c.drills.contains(drillId)) continue;
        if (!search.isEmpty() && !matches(c.label + QLatin1Char(' ') + c.id, search)) continue;
        out.append(candidateRow(c.id, c.label,
                                c.drills.isEmpty()
                                    ? tr("nothing answers this yet")
                                    : tr("%n drill(s) already", "", int(c.drills.size()))));
    }
    return out;
}

QVariantList ModelBrowser::linksCitingReference(const QString &refId) const
{
    QVariantList out;
    const Reference *ref = sharedReferenceSet().reference(refId);
    if (!ref) return out;

    // Any of the three identifiers, as the row list joins them: a handful of journals issue no DOI,
    // a book never had one, and a strict DOI match would render "cited by nothing" on a paper the
    // library leans on.
    const auto cites = [ref](const QString &citation) {
        if (citation.isEmpty()) return false;
        return (!ref->doi.isEmpty() && citation == ref->doi)
            || (!ref->pmid.isEmpty() && citation == ref->pmid)
            || (!ref->isbn.isEmpty() && citation == ref->isbn);
    };

    const CharacteristicPack &p = pack();
    for (const Edge &e : p.edges) {
        if (!cites(e.provenance.citation)) continue;
        const Condition *f = p.condition(e.from);
        const Condition *t = p.condition(e.to);
        // Built on hubRow() rather than by hand. Every row the inspector renders is the SAME shape —
        // { type, id, label, detail, tone, navigable } — and a row that carries only the keys its own
        // section happens to read still meets every other binding in the delegate, which evaluates
        // them whether or not that section is the visible one. Three missing keys here were four
        // "Unable to assign [undefined]" warnings per repaint.
        QVariantMap m = hubRow(kLinks, edgeId(e.from, e.to, e.type),
                               tr("%1 → %2").arg(f ? f->label : e.from, t ? t->label : e.to),
                               provenanceTierLabel(e.provenance.tier));
        m.insert(QStringLiteral("relation"), edgeTypeName(e.type));
        // The strength is the editable part: the citation is imported, the claim resting on it is
        // ours. QML writes it back through the ordinary setField("links", …).
        m.insert(QStringLiteral("strength"), strengthName(e.strength));
        m.insert(QStringLiteral("strengthLabel"), strengthLabel(e.strength));
        m.insert(QStringLiteral("options"), strengthOptions());
        out.append(m);
    }
    return out;
}

QString ModelBrowser::referenceCsl(const QString &refId) const
{
    const Reference *ref = sharedReferenceSet().reference(refId);
    if (!ref) return QString();
    // A one-record set through the real exporter, so the copied text is byte-for-byte what the
    // whole-set export would have written for this entry. A second hand-rolled serialiser here would
    // drift the first time the CSL mapping changed.
    ReferenceSet one;
    one.id      = sharedReferenceSet().id;
    one.version = sharedReferenceSet().version;
    one.references.push_back(*ref);
    return QString::fromUtf8(exportReferenceSetCsl(one));
}

QString ModelBrowser::referenceDoiUrl(const QString &refId) const
{
    const Reference *ref = sharedReferenceSet().reference(refId);
    if (!ref || ref->doi.isEmpty()) return QString();
    return QStringLiteral("https://doi.org/") + ref->doi;
}

// ── Corridors ───────────────────────────────────────────────────────────────
//
// A corridor is a norm row keyed on (measureId, contextId). It edits through the same setField()
// path everything else does; what is special is only that it lands in the OTHER registry, and that
// the one undo history has to carry both.

QString ModelBrowser::corridorId(const QString &measureId, const QString &contextId)
{
    return QStringLiteral("norm:%1@%2").arg(measureId, contextId);
}

bool ModelBrowser::splitCorridorId(const QString &id, QString &measureId, QString &contextId)
{
    if (!id.startsWith(QStringLiteral("norm:"))) return false;
    const QString body = id.mid(5);
    const int     at   = body.lastIndexOf(QLatin1Char('@'));
    if (at <= 0) return false;
    measureId = body.left(at);
    contextId = body.mid(at + 1);
    return true;
}

Norm *ModelBrowser::workingNorm(const QString &measureId, const QString &contextId)
{
    for (Norm &n : m_workingNorms.norms)
        if (n.measureId == measureId && n.contextId == contextId && n.cohort.isUnqualified())
            return &n;

    // Copy-on-write, seeded from whatever RESOLVES here rather than from nothing. Editing a
    // corridor is almost always a nudge, and starting the author at zero would make the common
    // case — "this is nearly right" — the one that costs the most.
    const NormResolution res = m_norms->resolve(measureId, contextId);
    if (!res.found()) return nullptr;

    Norm copy      = *res.norm;
    copy.measureId = measureId;
    copy.contextId = contextId;   // seated HERE, even when it was inherited from an ancestor
    copy.cohort    = Cohort{};
    m_workingNorms.norms.push_back(copy);
    return &m_workingNorms.norms.back();
}

QVariantList ModelBrowser::corridorContexts(const QString &measureId) const
{
    const ContextTree &tree = m_norms->contexts();

    QVariantList out;
    for (const QString &id : tree.inOrder()) {
        const ContextNode *node = tree.node(id);
        if (!node) continue;

        const NormResolution res = m_norms->resolve(measureId, id);

        // EVERY context that resolves to something, each marked own or inherited-from — that is the
        // whole point of the tree and it has to be visible, not implied. A context resolving to
        // nothing is still listed here, because "no corridor at all" is exactly the row an author
        // comes looking for.
        bool own = false;
        for (const Norm &n : m_workingNorms.norms)
            if (n.measureId == measureId && n.contextId == id) own = true;

        QString fromLabel;
        if (res.found() && res.inherited)
            if (const ContextNode *src = tree.node(res.contextId)) fromLabel = src->label;

        QVariantMap r;
        r.insert(QStringLiteral("id"), id);
        r.insert(QStringLiteral("label"), node->label);
        r.insert(QStringLiteral("depth"), tree.depth(id));
        r.insert(QStringLiteral("found"), res.found());
        r.insert(QStringLiteral("own"), own);
        r.insert(QStringLiteral("inherited"), res.found() && res.inherited);
        r.insert(QStringLiteral("inheritedFrom"), fromLabel);
        r.insert(QStringLiteral("mu"), res.found() ? res.norm->mu : 0.0);
        r.insert(QStringLiteral("unit"), res.found() ? res.norm->unit : QString());
        out.append(r);
    }
    return out;
}

QVariantMap ModelBrowser::addCorridor(const QString &measureId, const QString &contextId)
{
    const Measure *m = pack().measure(measureId);
    if (!m) return refuse(tr("No measure with id %1.").arg(measureId));
    if (!m_norms->contexts().node(contextId))
        return refuse(tr("%1 is not a context.").arg(contextId));
    // Resolved now: rebuild() below frees the pack `m` points into.
    const QString measureLabel = measureDisplayLabel(*m);

    for (const Norm &n : m_workingNorms.norms)
        if (n.measureId == measureId && n.contextId == contextId)
            return refuse(tr("You already have a corridor here."));

    const CharacteristicPack beforePack  = m_working;
    const NormPack           beforeNorms = m_workingNorms;

    Norm n;
    n.measureId = measureId;
    n.contextId = contextId;

    // Seeded from whatever resolves here, so the author starts from the corridor they are
    // narrowing rather than from an empty row. Where nothing resolves there is nothing honest to
    // seed from, and the unit comes off the measure so the load-time unit check can pass at all.
    const NormResolution res = m_norms->resolve(measureId, contextId);
    if (res.found()) {
        n.mu      = res.norm->mu;
        n.sigmaLo = res.norm->sigmaLo;
        n.sigmaHi = res.norm->sigmaHi;
        n.unit    = res.norm->unit;
    } else {
        n.unit = m->unit;
    }
    // Whatever it was seeded from, THIS row is an authored figure until somebody seats or sources
    // it. Inheriting `Literature` from the row it was copied off would attach a citation's
    // authority to numbers the author has just changed.
    n.source = NormSource::Heuristic;
    m_workingNorms.norms.push_back(n);

    rebuild();
    const QString what = tr("%1 at %2").arg(measureLabel, contextId);
    pushCommand(tr("Corridor added"), what, beforePack, beforeNorms);

    QVariantMap r = accept(what);
    r.insert(QStringLiteral("id"), corridorId(measureId, contextId));
    r.insert(QStringLiteral("type"), kCorridors);
    return r;
}

QVariantMap ModelBrowser::adoptCorridor(const QString &measureId, const QString &contextId,
                                        const QString &fromContextId)
{
    const NormResolution src = m_norms->resolve(measureId, fromContextId);
    if (!src.found()) return refuse(tr("Nothing resolves at %1 to adopt.").arg(fromContextId));

    const CharacteristicPack beforePack  = m_working;
    const NormPack           beforeNorms = m_workingNorms;

    Norm *n = workingNorm(measureId, contextId);
    if (!n) {
        Norm fresh;
        fresh.measureId = measureId;
        fresh.contextId = contextId;
        m_workingNorms.norms.push_back(fresh);
        n = &m_workingNorms.norms.back();
    }
    n->mu      = src.norm->mu;
    n->sigmaLo = src.norm->sigmaLo;
    n->sigmaHi = src.norm->sigmaHi;
    n->unit    = src.norm->unit;
    // Adopted, and it says so: these numbers were taken from somewhere else rather than fitted or
    // cited here, and a reader deciding whether to trust them needs that distinction.
    n->source  = NormSource::Imported;

    rebuild();
    const QString what = tr("%1 ← %2").arg(contextId, fromContextId);
    pushCommand(tr("Corridor adopted"), what, beforePack, beforeNorms);
    return accept(what);
}

QVariantMap ModelBrowser::resetCorridor(const QString &measureId, const QString &contextId)
{
    bool have = false;
    for (const Norm &n : m_workingNorms.norms)
        if (n.measureId == measureId && n.contextId == contextId) have = true;
    if (!have) return refuse(tr("There is nothing of yours here to reset."));

    const CharacteristicPack beforePack  = m_working;
    const NormPack           beforeNorms = m_workingNorms;

    for (auto it = m_workingNorms.norms.begin(); it != m_workingNorms.norms.end(); ++it)
        if (it->measureId == measureId && it->contextId == contextId) {
            m_workingNorms.norms.erase(it);
            break;
        }
    rebuild();

    // "Reset to shipped" and "remove your override" are the SAME operation with different
    // outcomes, and which one happened depends on whether core carries a row at this exact key.
    // Promising the wrong one is how a destructive action gets a reassuring label.
    const Norm *shipped = m_norms->shippedNorm(measureId, contextId);
    const QString what  = shipped ? tr("Back to the shipped corridor")
                                  : tr("Removed — the parent context's corridor applies again");
    pushCommand(tr("Corridor reset"), what, beforePack, beforeNorms);
    return accept(what);
}

// ── Context bindings — where a characteristic applies ───────────────────────
//
// A binding is an EXCEPTION, not a declaration. Bindings resolve by walking UP the context tree
// exactly as norms do: the nearest row on the chain wins, and a condition with no row anywhere
// applies everywhere. That is why the shipped pack carries almost none — an author writes a row
// only where the answer differs from the parent.

QVariantList ModelBrowser::bindingsOf(const QString &conditionId) const
{
    const Condition *c = pack().condition(conditionId);
    if (!c) return {};

    const ContextTree &tree = m_norms->contexts();

    QVariantList out;
    for (const QString &id : tree.inOrder()) {
        const ContextNode *node = tree.node(id);
        if (!node) continue;

        const BindingResolution br  = resolveContextBinding(*c, tree, id);
        const bool              own = ownContextBinding(*c, id) != nullptr;

        // Naming the ancestor a row was inherited FROM, not merely that it was: "Off, inherited" is
        // an invitation to hunt for the row that says so; "Off, from Partial swing" is not.
        QString fromLabel;
        if (br.found && br.inherited)
            if (const ContextNode *src = tree.node(br.contextId)) fromLabel = src->label;

        QVariantMap r;
        r.insert(QStringLiteral("id"), node->id);
        r.insert(QStringLiteral("label"), node->label);
        // The parent, not only the depth. Depth indents the row; the parent is what says which
        // rows a cascade would reach, and a caller that had to infer it from indentation would be
        // reconstructing the tree the model already has.
        r.insert(QStringLiteral("parentId"), node->parentId);
        r.insert(QStringLiteral("depth"), tree.depth(id));
        r.insert(QStringLiteral("applicable"), br.applicable);
        r.insert(QStringLiteral("material"), br.material);
        r.insert(QStringLiteral("own"), own);
        r.insert(QStringLiteral("inherited"), br.found && br.inherited);
        r.insert(QStringLiteral("inheritedFrom"), fromLabel);
        r.insert(QStringLiteral("hasChildren"), !tree.children(id).isEmpty());
        out.append(r);
    }
    return out;
}

QVariantMap ModelBrowser::setBinding(const QString &conditionId, const QString &contextId,
                                     bool applicable, bool material)
{
    const ContextTree &tree = m_norms->contexts();
    if (!tree.node(contextId)) return refuse(tr("%1 is not a context.").arg(contextId));

    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    Condition *c = workingCondition(conditionId);
    if (!c) { m_working = before; return refuse(tr("No characteristic with id %1.").arg(conditionId)); }

    // Switching a parent OFF has to clear any descendant row that says otherwise, or the untick
    // silently does not take. That cascade is the whole reason ADDENDUM-01 insists a cascade is ONE
    // command: it touches rows the author never named, and a partial undo would leave them believing
    // they were back where they started.
    int cascaded = 0;
    if (!applicable) {
        for (auto it = c->bindings.begin(); it != c->bindings.end();) {
            const bool beneath = it->context != contextId
                                 && tree.isDescendantOf(it->context, contextId);
            if (beneath && it->applicable) { it = c->bindings.erase(it); ++cascaded; }
            else                           { ++it; }
        }
    }

    ContextBinding *own = nullptr;
    for (ContextBinding &b : c->bindings)
        if (b.context == contextId) own = &b;
    if (!own) {
        ContextBinding b;
        b.context = contextId;
        c->bindings.push_back(b);
        own = &c->bindings.back();
    }
    own->applicable = applicable;
    own->material   = material;

    // The label BEFORE rebuild(): `tree` is a reference into m_norms, and rebuild() replaces that
    // provider wholesale — so every node the old tree owned is freed with it.
    const ContextNode *node  = tree.node(contextId);
    const QString      label = node ? node->label : contextId;

    rebuild();

    const QString      what  = !applicable ? tr("Does not apply to %1").arg(label)
                             : !material   ? tr("Not counted when ranking for %1").arg(label)
                                           : tr("Applies to %1").arg(label);
    pushCommand(tr("Where it applies"),
                cascaded > 0 ? tr("%1 · %n narrower row(s) cleared", "", cascaded).arg(what) : what,
                before, normsBefore);

    QVariantMap r = accept(what);
    r.insert(QStringLiteral("cascaded"), cascaded);
    return r;
}

QVariantMap ModelBrowser::clearBinding(const QString &conditionId, const QString &contextId)
{
    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    Condition *c = workingCondition(conditionId);
    if (!c) { m_working = before; return refuse(tr("No characteristic with id %1.").arg(conditionId)); }

    bool found = false;
    for (auto it = c->bindings.begin(); it != c->bindings.end(); ++it)
        if (it->context == contextId) { c->bindings.erase(it); found = true; break; }
    if (!found) {
        m_working = before;
        return refuse(tr("There is no row here to clear — it already inherits."));
    }

    rebuild();
    const ContextNode *node = m_norms->contexts().node(contextId);
    const QString what = tr("%1 inherits again").arg(node ? node->label : contextId);
    pushCommand(tr("Binding cleared"), what, before, normsBefore);
    return accept(what);
}

// ── Minting a measure ───────────────────────────────────────────────────────

namespace {

// Two measures are the SAME measure when their series and their reducer agree — never when their
// ids match.
//
// This is the distinction measure_facets.h states and it is easy to get backwards: ids in the
// shipped pack are hand-authored (`m_thoracicCurve`), not canonical, so an id comparison finds no
// duplicate for any of them and an author can mint a second name for a number the library already
// has. The tuple is identity; the string is for humans.
bool sameReducer(const Reducer &a, const Reducer &b)
{
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case ReducerKind::At:
        return a.anchor.value_or(a.window.first) == b.anchor.value_or(b.window.first);
    case ReducerKind::Delta:
    case ReducerKind::Rate:
        return a.window == b.window;
    case ReducerKind::Extremum:
        return a.window == b.window && a.sense == b.sense;
    }
    return false;
}

// The facet map QML hands back, as a Series + Reducer. One reader, so a key typed two ways cannot
// mean two things in two places.
bool readFacets(const QVariantMap &f, Series &series, Reducer &reducer, QString &whyNot)
{
    AnatomyRole what{}, reference{};
    Quantity    quantity{};

    if (!roleFromName(f.value(QStringLiteral("what")).toString(), what)) {
        whyNot = QObject::tr("Pick what is being measured.");
        return false;
    }
    if (!quantityFromName(f.value(QStringLiteral("quantity")).toString(), quantity)) {
        whyNot = QObject::tr("Pick a quantity.");
        return false;
    }
    if (!roleFromName(f.value(QStringLiteral("reference")).toString(), reference)) {
        whyNot = QObject::tr("Pick what it is measured against.");
        return false;
    }
    series.what      = what;
    series.quantity  = quantity;
    series.reference = reference;

    Phase anchor{};
    if (!phaseFromToken(f.value(QStringLiteral("anchor"), QStringLiteral("p1")).toString(), anchor))
        anchor = Phase::Address;
    reducer.kind   = ReducerKind::At;
    reducer.anchor = anchor;
    reducer.window = { anchor, anchor };
    return true;
}

} // namespace

QVariantMap ModelBrowser::previewMeasure(const QVariantMap &facets) const
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

    // The validity table, not a guess. A measure an author can build and the loader then rejects is
    // worse than one the picker refused to offer.
    const FacetCheck check = validateSeries(series);
    out.insert(QStringLiteral("valid"), check.valid);
    out.insert(QStringLiteral("reason"), check.reason);
    if (!check.valid) return out;

    const QString id    = canonicalMeasureId(series, reducer);
    const QString label = canonicalMeasureLabel(series, reducer);
    out.insert(QStringLiteral("id"), id);
    out.insert(QStringLiteral("label"), label);

    // Structural identity is the SERIES tuple. An exact match means reuse it; one facet different
    // means a near-duplicate, and that has to be surfaced AT CREATION because after the fact nobody
    // merges them. This is the primary defence against a library that fills with almost-identical
    // measures inside a month.
    QVariantList near;
    for (const Measure &m : pack().measures) {
        if (m.kind != MeasureKind::Composed) continue;
        if (m.series == series && sameReducer(m.reducer, reducer)) {
            QVariantMap e;
            e.insert(QStringLiteral("id"), m.id);
            e.insert(QStringLiteral("label"), measureDisplayLabel(m));
            out.insert(QStringLiteral("exactMatch"), e);
            continue;
        }
        if (isNearDuplicate(m.series, series)) {
            QVariantMap e;
            e.insert(QStringLiteral("id"), m.id);
            e.insert(QStringLiteral("label"), measureDisplayLabel(m));
            e.insert(QStringLiteral("detail"), measureStatusLabel(m.status));
            near.append(e);
        }
    }
    out.insert(QStringLiteral("nearDuplicates"), near);
    return out;
}

QVariantMap ModelBrowser::mintMeasure(const QVariantMap &facets)
{
    Series  series;
    Reducer reducer;
    QString whyNot;
    if (!readFacets(facets, series, reducer, whyNot)) return refuse(whyNot);

    const FacetCheck check = validateSeries(series);
    if (!check.valid) return refuse(check.reason);

    // Structural identity, not the id — see sameReducer()'s comment. Checking the id alone would
    // let a second name be minted for every hand-authored measure in the shipped pack.
    for (const Measure &m : pack().measures)
        if (m.kind == MeasureKind::Composed && m.series == series && sameReducer(m.reducer, reducer))
            return refuse(tr("That is %1 — use it rather than making a second one.")
                              .arg(measureDisplayLabel(m)));

    const QString id = canonicalMeasureId(series, reducer);
    if (pack().measure(id))
        return refuse(tr("That measure already exists — use it rather than making a second one."));

    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    Measure m;
    m.id      = id;
    m.kind    = MeasureKind::Composed;
    m.series  = series;
    m.reducer = reducer;
    // Never blocks the author: a measure with no producer is a legitimate, expected outcome — it is
    // the roadmap's input, which is the whole reason the roadmap can be trusted as a work queue.
    m.status     = MeasureStatus::NoProducer;
    m.viewNeeded = deriveViewNeeded(series);
    m_working.measures.push_back(m);

    rebuild();
    const QString label = measureDisplayLabel(m);
    pushCommand(tr("Measure created"), label, before, normsBefore);

    QVariantMap r = accept(tr("Created %1").arg(label));
    r.insert(QStringLiteral("id"), id);
    r.insert(QStringLiteral("type"), kMeasures);
    return r;
}

QVariantMap ModelBrowser::seedFacetsFromPhrase(const QString &phrase) const
{
    // A typed phrase SEEDS the chips; it is not a query. Wrong guesses are corrected by tapping,
    // which is far easier than rephrasing a search that returned nothing.
    QVariantMap out;
    const QString p = phrase.trimmed().toLower();
    if (p.isEmpty()) return out;

    for (AnatomyRole r : allRoles()) {
        const QString label = roleLabel(r).toLower();
        if (label.isEmpty()) continue;
        if (p.contains(label)) {
            if (!out.contains(QStringLiteral("what")))      out.insert(QStringLiteral("what"),
                                                                       roleName(r));
            else if (!out.contains(QStringLiteral("reference")))
                out.insert(QStringLiteral("reference"), roleName(r));
        }
    }
    for (Quantity q : allQuantities())
        if (p.contains(quantityLabel(q).toLower()))
            out.insert(QStringLiteral("quantity"), quantityName(q));
    return out;
}

QVariantList ModelBrowser::anatomyRoles() const
{
    QVariantList l;
    for (AnatomyRole r : allRoles())
        l.append(option(roleName(r), roleLabel(r)));
    return l;
}

QVariantList ModelBrowser::quantitiesFor(const QString &whatRole) const
{
    AnatomyRole what{};
    if (!roleFromName(whatRole, what)) return {};
    QVariantList l;
    // Gated by the validity table, so the picker never offers something it will then reject.
    for (Quantity q : legalQuantitiesFor(what)) l.append(option(quantityName(q), quantityLabel(q)));
    return l;
}

QVariantList ModelBrowser::referencesFor(const QString &whatRole, const QString &quantity) const
{
    AnatomyRole what{};
    Quantity    q{};
    if (!roleFromName(whatRole, what) || !quantityFromName(quantity, q)) return {};
    QVariantList l;
    for (AnatomyRole r : legalReferencesFor(what, q))
        l.append(option(roleName(r), roleLabel(r)));
    return l;
}

QVariantList ModelBrowser::reducerKinds() const
{
    QVariantList l;
    for (ReducerKind k : { ReducerKind::At, ReducerKind::Delta, ReducerKind::Rate,
                           ReducerKind::Extremum })
        l.append(option(reducerKindName(k), reducerKindName(k)));
    return l;
}

QVariantList ModelBrowser::phases() const
{
    QVariantList l;
    // The swing's own order, spelled once. There is no allPhases() to lean on, and this is the same
    // sequence the old editor's picker used — P-positions are what coaches and the content speak in.
    for (Phase p : { Phase::Address, Phase::ShaftParallelBack, Phase::MidBackswing, Phase::Top,
                     Phase::ArmParallelDown, Phase::Delivery, Phase::Impact,
                     Phase::ShaftParallelThrough, Phase::FollowThrough })
        l.append(option(phaseToken(p), phaseLabel(p)));
    return l;
}

// ── A blank object ──────────────────────────────────────────────────────────

QVariantMap ModelBrowser::createObject(const QString &type)
{
    if (type == kScreens) {
        const ScreenSet screensBefore = m_workingScreens;

        Screen s;
        s.label = tr("New screen");
        s.id    = mintId(s.label, QStringLiteral("screen."), QStringLiteral("screen"),
                         [this](const QString &x) { return m_screens.screen(x) != nullptr; });
        m_workingScreens.screens.push_back(s);

        rebuild();
        pushCommand(tr("Created"), s.label, m_working, m_workingNorms, screensBefore,
                    m_workingDrills);
        // It lands carrying two validation warnings — no protocol, no pass criterion — and that is
        // correct: a screen nobody could run and whose answer is unrecordable is exactly what a
        // blank one is, and the health list should say so from the first second.
        QVariantMap r = accept(tr("Created %1 — give it a name").arg(s.label));
        r.insert(QStringLiteral("id"), s.id);
        r.insert(QStringLiteral("type"), kScreens);
        return r;
    }
    if (type == kDrills) {
        const DrillSet drillsBefore = m_workingDrills;

        Drill d;
        d.label = tr("New drill");
        d.id    = mintId(d.label, QStringLiteral("drill."), QStringLiteral("drill"),
                         [this](const QString &x) { return m_drills.drill(x) != nullptr; });
        m_workingDrills.drills.push_back(d);

        rebuild();
        pushCommand(tr("Created"), d.label, m_working, m_workingNorms, m_workingScreens,
                    drillsBefore);
        QVariantMap r = accept(tr("Created %1 — give it a name").arg(d.label));
        r.insert(QStringLiteral("id"), d.id);
        r.insert(QStringLiteral("type"), kDrills);
        return r;
    }

    if (type != kCharacteristics && type != kCauses)
        return refuse(tr("%1 cannot be created here.").arg(typeLabelFor(type)));

    const CharacteristicPack before      = m_working;
    const NormPack           normsBefore = m_workingNorms;

    QString id = QStringLiteral("new_characteristic");
    int     n  = 2;
    while (pack().condition(id) != nullptr)
        id = QStringLiteral("new_characteristic_%1").arg(n++);

    Condition c;
    c.id    = id;
    c.label = tr("New characteristic");
    // A draft nobody has been to the literature for. Badging it honestly from the start is what
    // stops an unsourced claim quietly acquiring the appearance of one.
    c.state           = ConditionState::Draft;
    c.provenance.tier = ProvenanceTier::Proposed;
    m_working.conditions.push_back(c);

    rebuild();
    pushCommand(tr("Created"), c.label, before, normsBefore);

    QVariantMap r = accept(tr("Created %1 — give it a name").arg(c.label));
    r.insert(QStringLiteral("id"), id);
    r.insert(QStringLiteral("type"), kCharacteristics);
    return r;
}

// ── Pack-wide settings ──────────────────────────────────────────────────────

QVariantList ModelBrowser::gradePolicies() const
{
    QVariantList l;
    for (const GradePolicyPreset &p : gradePolicyPresets()) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), QString::fromLatin1(p.name));
        m.insert(QStringLiteral("label"), QString::fromLatin1(p.label));
        m.insert(QStringLiteral("hint"), QString::fromLatin1(p.hint));
        l.append(m);
    }
    return l;
}

QVariantList ModelBrowser::normSets() const
{
    // The LAYERS, not the assembled set. "merged" is an implementation word and must never reach a
    // user: they need to see the shipped set and their own as separate things, because that is what
    // the override relationship between them means.
    QVariantList l;
    for (const NormSetInfo &s : m_norms->layers()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), s.id);
        m.insert(QStringLiteral("label"), s.label);
        m.insert(QStringLiteral("normCount"), s.normCount);
        m.insert(QStringLiteral("readOnly"), s.readOnly);
        m.insert(QStringLiteral("shipped"), s.origin == PackOrigin::Core);
        l.append(m);
    }
    return l;
}

// ── Artefacts and the surfaces that would otherwise die with Diagnostics ────
//
// These are not editing. They are here because retiring the old panel deletes them otherwise, and
// each is the only place in the app that answers its question.

QVariantList ModelBrowser::roadmap() const
{
    const CharacteristicPack &p = pack();

    // Ranked by SERIES, not by reduced measure. One producer unblocks every reducer over its series
    // — pelvis lateral sway carries sway, slide and hanging back at three phases — so it is ONE
    // piece of pipeline work worth three characteristics. Ranking reduced measures would spread
    // that across three rows of "unblocks 1" and bury the item that should rank top.
    struct Group {
        QString       label;
        QString       metricKey;
        QString       gapReason;
        MeasureStatus status  = MeasureStatus::NoProducer;
        int           samples = 0;
        QSet<QString> blocked;
    };
    QHash<QString, Group> groups;

    for (const Measure &m : p.measures) {
        if (m.status == MeasureStatus::Live) continue;
        // Capture gaps are NOT roadmap items: one row implying a producer that will never be built
        // corrupts the artefact's meaning for every other row.
        if (m.status == MeasureStatus::NotCapturable) continue;

        const QString key = m.metricKey.isEmpty() ? canonicalSeriesId(m.series) : m.metricKey;
        Group        &g   = groups[key];
        if (g.samples == 0) {
            g.metricKey = m.metricKey;
            g.status    = m.status;
            g.gapReason = m.gapReason;
            if (const MetricDescriptor *d = m_cat.descriptor(key)) g.label = d->label;
            if (g.label.isEmpty()) g.label = measureDisplayLabel(m);
        }
        ++g.samples;
        for (const QVariant &v : measureUserRows(m.id))
            g.blocked.insert(v.toMap().value(QStringLiteral("id")).toString());
    }

    QVariantList out;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const Group &g = it.value();
        QVariantMap  r;
        r.insert(QStringLiteral("id"), it.key());
        r.insert(QStringLiteral("label"), g.label);
        r.insert(QStringLiteral("metricKey"), g.metricKey);
        r.insert(QStringLiteral("statusLabel"), measureStatusLabel(g.status));
        r.insert(QStringLiteral("blocks"), g.blocked.size());
        r.insert(QStringLiteral("samples"), g.samples);
        // "Write this producer" and "talk to that device" are different kinds of work wanted by
        // different people, and one ranked list cannot say both without the reader assuming wrong.
        r.insert(QStringLiteral("integration"), g.status == MeasureStatus::ExternalDevice);
        r.insert(QStringLiteral("gapReason"), g.gapReason);
        out.append(r);
    }
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

QVariantList ModelBrowser::captureGaps() const
{
    QVariantList out;
    for (const Measure &m : pack().measures) {
        if (m.status != MeasureStatus::NotCapturable) continue;
        QVariantMap r;
        r.insert(QStringLiteral("id"), m.id);
        r.insert(QStringLiteral("label"), measureDisplayLabel(m));
        r.insert(QStringLiteral("reason"), m.gapReason);
        r.insert(QStringLiteral("blocks"), measureUsers(m.id));
        out.append(r);
    }
    return out;
}

QVariantList ModelBrowser::causeCoverage() const
{
    const CharacteristicPack &p = pack();

    QVariantList out;
    for (const Condition &c : p.conditions) {
        const int cov = coverageOf(p, c.id);
        if (cov == 0) continue;
        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("coverage"), cov);
        r.insert(QStringLiteral("reachLabel"), reachLabel(c.confirmedBy));
        r.insert(QStringLiteral("screenRef"), c.screenRef);
        // Screened and behavioural causes can never be measured by this product. That is permanent,
        // not pending, and the UI must say so rather than implying a producer is on the way.
        r.insert(QStringLiteral("outsideCaptureReach"), isOutsideCaptureReach(c.confirmedBy));
        out.append(r);
    }
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("coverage")).toInt()
             > b.toMap().value(QStringLiteral("coverage")).toInt();
    });
    return out;
}

QVariantList ModelBrowser::glossary(const QString &search) const
{
    const CharacteristicPack &p = pack();
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
        QStringList causedBy;
        for (const QString &cid : causesOf(p, c.id))
            if (const Condition *cause = p.condition(cid)) causedBy << cause->label;

        QVariantMap r;
        r.insert(QStringLiteral("id"), c.id);
        r.insert(QStringLiteral("label"), c.label);
        r.insert(QStringLiteral("aliases"), c.aliases);
        r.insert(QStringLiteral("meaning"), c.consequence.text());
        r.insert(QStringLiteral("causedBy"), causedBy);
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

QVariantMap ModelBrowser::exportRoadmap() const
{
    QVariantMap r;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty()) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("No Documents folder to write to."));
        return r;
    }

    // Generated whole rather than scraped off the view: this artefact is meant to LEAVE the app —
    // it is what prioritises pipeline work — so it cannot depend on what happens to be on screen.
    QString md;
    md += tr("# Swing diagnostics — measure roadmap\n\n");
    md += tr("Every row is work that could be picked up: a measure some characteristic needs and "
             "nothing yet produces. Ranked by how many characteristics it unblocks.\n\n");
    md += tr("| Measure | Unblocks | Status | Metric key |\n|---|---:|---|---|\n");
    for (const QVariant &v : roadmap()) {
        const QVariantMap row = v.toMap();
        md += QStringLiteral("| %1 | %2 | %3 | `%4` |\n")
                  .arg(row.value(QStringLiteral("label")).toString())
                  .arg(row.value(QStringLiteral("blocks")).toInt())
                  .arg(row.value(QStringLiteral("statusLabel")).toString(),
                       row.value(QStringLiteral("metricKey")).toString());
    }

    // A SEPARATE section, never roadmap rows. A reader has to be able to take the table above at
    // face value as a work queue, and one row nobody could ever pick up corrupts that reading for
    // every other row.
    const QVariantList gaps = captureGaps();
    if (!gaps.isEmpty()) {
        md += tr("\n## Not resolvable from current capture\n\n");
        md += tr("These are not roadmap items. No sensor this product has can resolve them, so "
                 "they need a different modality rather than a producer.\n\n");
        for (const QVariant &v : gaps) {
            const QVariantMap row = v.toMap();
            md += QStringLiteral("- **%1** — blocks %2. %3\n")
                      .arg(row.value(QStringLiteral("label")).toString())
                      .arg(row.value(QStringLiteral("blocks")).toInt())
                      .arg(row.value(QStringLiteral("reason")).toString());
        }
    }

    const QString path = dir + QStringLiteral("/pinpoint-diagnostics-roadmap.md");
    QFile         f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("Could not write to %1.").arg(path));
        return r;
    }
    f.write(md.toUtf8());
    f.close();

    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("path"), path);
    r.insert(QStringLiteral("message"), tr("Exported to %1").arg(path));
    return r;
}

QVariantMap ModelBrowser::exportReferences() const
{
    QVariantMap r;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty()) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("No Documents folder to write to."));
        return r;
    }

    // CSL-JSON because it is what Zotero imports and what pandoc consumes, so a coach who wants
    // these sources in their own library gets every citation style for free rather than us picking
    // one. The double extension is the convention and tells a human what the file is unopened.
    const QString path = dir + QStringLiteral("/pinpoint-references.csl.json");
    QFile         f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("message"), tr("Could not write to %1.").arg(path));
        return r;
    }
    f.write(exportReferenceSetCsl(sharedReferenceSet()));
    f.close();

    r.insert(QStringLiteral("ok"), true);
    r.insert(QStringLiteral("path"), path);
    r.insert(QStringLiteral("message"), tr("Exported to %1").arg(path));
    return r;
}

// ── The corridor, as a picture ──────────────────────────────────────────────

QVariantMap ModelBrowser::corridorPlot(const QString &measureId, const QString &contextId,
                                       const QVariantMap &options) const
{
    QVariantMap out;
    out.insert(QStringLiteral("found"), false);

    const NormResolution res = m_norms->resolve(measureId, contextId);
    if (!res.found()) return out;

    const Measure *meas = pack().measure(measureId);

    CorridorPlotOptions opt;
    auto num = [&options](const char *key, double def) {
        const QVariant v = options.value(QString::fromLatin1(key));
        return v.isValid() ? v.toDouble() : def;
    };
    opt.width           = num("width", opt.width);
    opt.height          = num("height", opt.height);
    opt.curveSteps      = int(num("curveSteps", opt.curveSteps));
    opt.maxSamplePoints = int(num("maxSamplePoints", opt.maxSamplePoints));
    opt.windowMin       = num("windowMin", opt.windowMin);
    opt.windowMax       = num("windowMax", opt.windowMax);

    // Only this measure's own readings. A plot drawn over another measure's scan would be a
    // confident, wrong picture, so the values are used only when the scan says they are for this
    // key — and `scanned` travels out so the view can say whether it is showing any at all.
    static const std::vector<double> kNone;
    const std::vector<double> &values =
        (m_corridorScanned == measureId) ? m_corridorValues : kNone;

    // The measure's SHAPE decides which tails grade, and it lives on the measure rather than on the
    // norm — one-sidedness is a property of the quantity, invariant across contexts.
    const Shape       shape  = meas ? meas->shape : Shape::Target;
    const GradePolicy policy = gradePolicyByName(m_policyName);

    // A drag in progress. The picture is laid out from values nobody has committed, through the
    // SAME function that lays out the real one — so what a handle shows while it moves is exactly
    // what releasing it will store, and no command reaches the undo stack until it is let go.
    Norm preview = *res.norm;
    if (options.contains(QStringLiteral("previewMu")))
        preview.mu = options.value(QStringLiteral("previewMu")).toDouble();
    const double z = std::max(1e-9, policy.idealMaxZ);
    if (options.contains(QStringLiteral("previewIdealLo")))
        preview.sigmaLo = std::abs(preview.mu
                                   - options.value(QStringLiteral("previewIdealLo")).toDouble()) / z;
    if (options.contains(QStringLiteral("previewIdealHi")))
        preview.sigmaHi = std::abs(options.value(QStringLiteral("previewIdealHi")).toDouble()
                                   - preview.mu) / z;

    const CorridorPlot plot = layoutCorridorPlot(preview, shape, values, policy, opt);

    auto points = [](const std::vector<CorridorPoint> &in) {
        QVariantList l;
        for (const CorridorPoint &pt : in) {
            QVariantMap m;
            m.insert(QStringLiteral("x"), pt.x);
            m.insert(QStringLiteral("y"), pt.y);
            l.append(m);
        }
        return l;
    };

    QVariantList bands;
    for (const CorridorBand &b : plot.bands) {
        QVariantMap m;
        // The band's own WORD, so a delegate colours by meaning rather than by position in the
        // list — the order flips on a one-sided measure and a positional mapping would silently
        // paint Action green.
        m.insert(QStringLiteral("grade"),
                 b.grade == Grade::Ideal  ? QStringLiteral("ideal")
                 : b.grade == Grade::Good ? QStringLiteral("good")
                 : b.grade == Grade::Watch ? QStringLiteral("watch")
                                           : QStringLiteral("action"));
        m.insert(QStringLiteral("x"), b.x);
        m.insert(QStringLiteral("w"), b.w);
        bands.append(m);
    }

    QVariantList rug;
    for (double x : plot.rug) rug.append(x);

    out.insert(QStringLiteral("found"), true);
    out.insert(QStringLiteral("width"), plot.width);
    out.insert(QStringLiteral("height"), plot.height);
    out.insert(QStringLiteral("xMin"), plot.xMin);
    out.insert(QStringLiteral("xMax"), plot.xMax);
    out.insert(QStringLiteral("unit"), plot.unit);
    out.insert(QStringLiteral("bands"), bands);
    out.insert(QStringLiteral("curve"), points(plot.curve));
    out.insert(QStringLiteral("samples"), points(plot.samples));
    out.insert(QStringLiteral("rug"), rug);
    out.insert(QStringLiteral("muX"), plot.muX);
    out.insert(QStringLiteral("idealLoX"), plot.idealLoX);
    out.insert(QStringLiteral("idealHiX"), plot.idealHiX);
    out.insert(QStringLiteral("watchLoX"), plot.watchLoX);
    out.insert(QStringLiteral("watchHiX"), plot.watchHiX);
    out.insert(QStringLiteral("lowOpen"), plot.lowOpen);
    out.insert(QStringLiteral("highOpen"), plot.highOpen);
    out.insert(QStringLiteral("n"), plot.n);
    out.insert(QStringLiteral("ideal"), plot.ideal);
    out.insert(QStringLiteral("good"), plot.good);
    out.insert(QStringLiteral("watch"), plot.watch);
    out.insert(QStringLiteral("action"), plot.action);
    out.insert(QStringLiteral("implausible"), plot.implausible);
    out.insert(QStringLiteral("truncated"), plot.truncated);
    out.insert(QStringLiteral("note"), plot.note);
    // The norm's own numbers, so a drag can be turned back into a value without the view holding a
    // second copy of the scale.
    out.insert(QStringLiteral("mu"), preview.mu);
    out.insert(QStringLiteral("sigmaLo"), preview.sigmaLo);
    out.insert(QStringLiteral("sigmaHi"), preview.sigmaHi);
    // The values the two edge handles are sitting on, in the measure's own units — what a readout
    // beside a handle has to say, and what releasing it commits.
    out.insert(QStringLiteral("idealLo"), preview.mu - policy.idealMaxZ * preview.sigmaLo);
    out.insert(QStringLiteral("idealHi"), preview.mu + policy.idealMaxZ * preview.sigmaHi);
    // Absent is a real state, distinct from zero — "no bound" and "a bound at 0.0" are different
    // claims about what is believable — so the flag travels beside the number rather than a
    // sentinel standing in for it.
    const CorridorPrecision prec = corridorPrecisionFor(
        preview.unit.isEmpty() ? (meas ? meas->unit : QString()) : preview.unit);
    out.insert(QStringLiteral("step"), prec.step);
    out.insert(QStringLiteral("decimals"), prec.decimals);
    out.insert(QStringLiteral("hasPlausibleLo"), preview.plausibleLo.has_value());
    out.insert(QStringLiteral("plausibleLo"), preview.plausibleLo.value_or(0.0));
    out.insert(QStringLiteral("hasPlausibleHi"), preview.plausibleHi.has_value());
    out.insert(QStringLiteral("plausibleHi"), preview.plausibleHi.value_or(0.0));
    out.insert(QStringLiteral("scanned"), m_corridorScanned == measureId);
    return out;
}

void ModelBrowser::setGradePolicy(const QString &name)
{
    // Resolved through the shared table, never stored as handed in: an unknown name must grade
    // against the default AND read as the default, or the control shows one thing while the app
    // does another.
    const QString resolved = QString::fromLatin1(gradePolicyPresetFor(name).name);
    if (resolved == m_policyName) return;
    m_policyName = resolved;
    emit modelChanged();
}

void ModelBrowser::setLibraryRoot(const QString &root)
{
    if (root == m_libraryRoot) return;
    m_libraryRoot = root;
    // A new library invalidates what the old one said. Dropped rather than kept: readings from
    // somebody else's swings under this measure's name is the worst kind of wrong picture.
    m_corridorValues.clear();
    m_corridorScanned.clear();
    emit corridorSamplesChanged();
}

void ModelBrowser::scanCorridor(const QString &measureId)
{
    if (m_corridorWatcher->isRunning() || m_libraryRoot.isEmpty()) return;

    const Measure *meas = pack().measure(measureId);
    if (!meas) return;

    // Everything the worker needs, BY VALUE. It must not touch the pack or the providers: those
    // live on this thread and a rebuild() would pull them out from under it.
    const Measure copy = *meas;
    const QString root = m_libraryRoot;

    m_corridorScanning = true;
    m_corridorScanned  = measureId;
    m_corridorValues.clear();
    emit corridorSamplesChanged();

    // A cap, not a sample size — and the plot reports when it bites. Same figure the corpus check
    // uses, so two scans over one library cannot disagree about how much of it they saw.
    constexpr int kMaxScan = 2000;

    m_corridorWatcher->setFuture(QtConcurrent::run([root, copy]() -> QVariantList {
        QVariantList out;
        const QDir   rootDir(root);
        for (const QString &athlete : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QDir aDir(rootDir.filePath(athlete));
            for (const QString &session : aDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                const QDir sDir(aDir.filePath(session));
                for (const QString &swing : sDir.entryList({ QStringLiteral("swing_*") },
                                                           QDir::Dirs, QDir::Name)) {
                    if (out.size() >= kMaxScan) return out;
                    // The expensive part is reading the phase grid, which is sidecar-cached and
                    // pack-independent — which is exactly why measure_sample caches the grid rather
                    // than measure values.
                    const SwingPhaseGrid grid = readPhaseGrid(sDir.filePath(swing),
                                                              /*writeSidecar*/ true);
                    const std::optional<double> v = reduceOverGrid(grid, copy);
                    if (v) out.append(*v);
                }
            }
        }
        return out;
    }));
}

void ModelBrowser::onCorridorScanFinished()
{
    const QVariantList res = m_corridorWatcher->future().result();
    m_corridorValues.clear();
    m_corridorValues.reserve(size_t(res.size()));
    for (const QVariant &v : res) m_corridorValues.push_back(v.toDouble());

    m_corridorScanning = false;
    emit corridorSamplesChanged();
    emit modelChanged();
}

// ── The graph around anything ───────────────────────────────────────────────

void ModelBrowser::decorateNode(QVariantMap &node, const QString &type, const QString &id) const
{
    // The type's colour key and glyph, decided here so every node of a kind agrees wherever it is
    // drawn — and so the delegate colours by MEANING rather than by which list it came out of.
    static const QHash<QString, QString> glyphs = {
        { kCharacteristics, QStringLiteral("◇") }, { kCauses,     QStringLiteral("◆") },
        { kMeasures,        QStringLiteral("▦") }, { kSignals,    QStringLiteral("≈") },
        { kScreens,         QStringLiteral("⊕") }, { kDrills,     QStringLiteral("↻") },
        { kReferences,      QStringLiteral("§") }, { kCorridors,  QStringLiteral("▭") },
        { kHealth,          QStringLiteral("⚠") }, { kLinks,      QStringLiteral("→") },
        { kMetrics,         QStringLiteral("∿") },
    };
    node.insert(QStringLiteral("nodeType"), type);
    node.insert(QStringLiteral("glyph"), glyphs.value(type));

    // One line the node can say about itself. For a measure that is its corridor: the number a
    // reader is actually asking about when they look at a measure in a graph, and until now it was
    // two navigations away.
    if (type == kMeasures) {
        const NormResolution res = m_norms->resolve(id, QStringLiteral("any"));
        if (res.found()) {
            const int dp = corridorPrecisionFor(res.norm->unit).decimals;
            node.insert(QStringLiteral("note"),
                        tr("μ %1 %2").arg(QString::number(res.norm->mu, 'f', dp), res.norm->unit));
        } else {
            // Said, not left blank: a measure nothing grades is a corridor signal that can never
            // fire, and that is the most interesting thing about it.
            node.insert(QStringLiteral("note"), tr("no corridor"));
        }
    }
}

QVariantMap ModelBrowser::neighbourhood(const QString &type, const QString &id,
                                        const QVariantMap &options) const
{
    const CharacteristicPack &p = pack();

    auto num = [&options](const char *key, double def) {
        const QVariant v = options.value(QString::fromLatin1(key));
        return v.isValid() ? v.toDouble() : def;
    };
    const double nodeH = num("nodeH", 34);
    const double gapY  = num("gapY", 14);
    const double gapX  = num("gapX", 90);
    const double colW  = num("maxW", 210);
    const double padX  = num("padX", 12);

    // What points AT it, and what it points to. Two columns, because that is the only ordering a
    // one-hop relation has — and it is the same left-to-right reading the causal DAG uses, so the
    // two pictures do not teach opposite habits.
    struct Rel { QString type, id, label, detail; int side; };
    std::vector<Rel> rels;
    QString focusLabel = id;

    auto addCondition = [&](const Condition &c, int side, const QString &detail) {
        rels.push_back({ c.observability == Observability::Latent ? kCauses : kCharacteristics,
                         c.id, c.label, detail, side });
    };

    if (type == kMeasures) {
        const Measure *m = p.measure(id);
        if (!m) return {};
        focusLabel = measureDisplayLabel(*m);
        if (!m->metricKey.isEmpty())
            if (const MetricDescriptor *d = m_cat.descriptor(m->metricKey))
                rels.push_back({ kMetrics, d->key, d->label.isEmpty() ? d->key : d->label,
                                 d->unit, -1 });
        for (const Signal &s : p.signalDefs)
            if (s.measures.contains(id))
                rels.push_back({ kSignals, s.id, s.id, signalTestName(s.test), 1 });
        for (const QVariant &v : measureUserRows(id)) {
            const QVariantMap r = v.toMap();
            if (const Condition *c = p.condition(r.value(QStringLiteral("id")).toString()))
                addCondition(*c, 1, r.value(QStringLiteral("detail")).toString());
        }
        for (const QVariant &v : corridorContexts(id)) {
            const QVariantMap c2 = v.toMap();
            if (!c2.value(QStringLiteral("found")).toBool()) continue;
            if (!c2.value(QStringLiteral("own")).toBool()
                && !c2.value(QStringLiteral("inheritedFrom")).toString().isEmpty())
                continue;   // an inherited row is the same corridor, not another one
            rels.push_back({ kCorridors, corridorId(id, c2.value(QStringLiteral("id")).toString()),
                             c2.value(QStringLiteral("label")).toString(),
                             tr("μ %1").arg(c2.value(QStringLiteral("mu")).toDouble()), -1 });
        }
    } else if (type == kMetrics) {
        const MetricDescriptor *d = m_cat.descriptor(id);
        if (!d) return {};
        focusLabel = d->label.isEmpty() ? d->key : d->label;
        for (const Measure &meas : p.measures)
            if (meas.metricKey == id)
                rels.push_back({ kMeasures, meas.id, measureDisplayLabel(meas),
                                 measureStatusLabel(meas.status), 1 });
    } else if (type == kSignals) {
        const Signal *s = p.signal(id);
        if (!s) return {};
        for (const QString &mid : s->measures)
            if (const Measure *m = p.measure(mid))
                rels.push_back({ kMeasures, mid, measureDisplayLabel(*m), m->unit, -1 });
        for (const Condition &c : p.conditions)
            if (c.detectedBy.contains(id)) addCondition(c, 1, conditionGroupLabel(c.group));
    } else if (type == kCorridors) {
        QString mid, ctx;
        if (!splitCorridorId(id, mid, ctx)) return {};
        focusLabel = ctx;
        if (const Measure *m = p.measure(mid))
            rels.push_back({ kMeasures, mid, measureDisplayLabel(*m), m->unit, -1 });
        for (const QVariant &v : measureUserRows(mid)) {
            const QVariantMap r = v.toMap();
            if (const Condition *c = p.condition(r.value(QStringLiteral("id")).toString()))
                addCondition(*c, 1, r.value(QStringLiteral("detail")).toString());
        }
    } else if (type == kScreens) {
        const Screen *s = m_screens.screen(id);
        if (!s) return {};
        focusLabel = s->label;
        for (const Condition &c : p.conditions)
            if (c.screenRef == id) addCondition(c, 1, tr("explains %1").arg(coverageOf(p, c.id)));
    } else if (type == kDrills) {
        const Drill *d = m_drills.drill(id);
        if (!d) return {};
        focusLabel = d->label;
        for (const Condition &c : p.conditions)
            if (c.drills.contains(id)) addCondition(c, -1, conditionGroupLabel(c.group));
    } else if (type == kReferences) {
        const Reference *ref = sharedReferenceSet().reference(id);
        if (!ref) return {};
        focusLabel = ref->title.isEmpty() ? ref->id : ref->title;
        const auto cites = [ref](const QString &c) {
            if (c.isEmpty()) return false;
            return (!ref->doi.isEmpty() && c == ref->doi) || (!ref->pmid.isEmpty() && c == ref->pmid)
                || (!ref->isbn.isEmpty() && c == ref->isbn);
        };
        for (const Condition &c : p.conditions)
            if (cites(c.provenance.citation))
                addCondition(c, 1, provenanceTierLabel(c.provenance.tier));
        for (const Edge &e : p.edges) {
            if (!cites(e.provenance.citation)) continue;
            const Condition *f = p.condition(e.from);
            const Condition *o = p.condition(e.to);
            rels.push_back({ kLinks, edgeId(e.from, e.to, e.type),
                             tr("%1 → %2").arg(f ? f->label : e.from, o ? o->label : e.to),
                             provenanceTierLabel(e.provenance.tier), 1 });
        }
    } else if (type == kLinks) {
        QString from, to;
        EdgeType t = EdgeType::Causes;
        if (!splitEdgeId(id, from, to, t)) return {};
        const Condition *f = p.condition(from);
        const Condition *o = p.condition(to);
        focusLabel = edgeTypeName(t);
        if (f) addCondition(*f, -1, tr("the cause"));
        if (o) addCondition(*o, 1, tr("the effect"));
    } else {
        return {};
    }

    // ── Positions ───────────────────────────────────────────────────────────
    std::vector<const Rel *> left, right;
    for (const Rel &r : rels) (r.side < 0 ? left : right).push_back(&r);

    const double rowH   = nodeH + gapY;
    const double colH   = std::max({ double(left.size()), double(right.size()), 1.0 }) * rowH;
    const double height = colH + nodeH;
    const double width  = padX * 2 + colW * 3 + gapX * 2;

    auto place = [&](const std::vector<const Rel *> &col, double x, QVariantList &nodes) {
        const double top = (height - col.size() * rowH) / 2.0;
        for (size_t i = 0; i < col.size(); ++i) {
            QVariantMap n;
            n.insert(QStringLiteral("id"), col[i]->id);
            n.insert(QStringLiteral("kind"), QStringLiteral("cause"));
            n.insert(QStringLiteral("label"), col[i]->label);
            n.insert(QStringLiteral("statusLabel"), col[i]->detail);
            n.insert(QStringLiteral("x"), x);
            n.insert(QStringLiteral("y"), top + i * rowH);
            n.insert(QStringLiteral("w"), colW);
            n.insert(QStringLiteral("h"), nodeH);
            n.insert(QStringLiteral("available"), true);
            decorateNode(n, col[i]->type, col[i]->id);
            nodes.append(n);
        }
    };

    const double leftX   = padX;
    const double centreX = padX + colW + gapX;
    const double rightX  = centreX + colW + gapX;
    const double focusY  = (height - nodeH) / 2.0;

    QVariantList nodes;
    place(left, leftX, nodes);
    place(right, rightX, nodes);

    QVariantMap focus;
    focus.insert(QStringLiteral("id"), id);
    focus.insert(QStringLiteral("kind"), QStringLiteral("focus"));
    focus.insert(QStringLiteral("label"), focusLabel);
    focus.insert(QStringLiteral("x"), centreX);
    focus.insert(QStringLiteral("y"), focusY);
    focus.insert(QStringLiteral("w"), colW);
    focus.insert(QStringLiteral("h"), nodeH);
    focus.insert(QStringLiteral("available"), true);
    decorateNode(focus, type, id);
    nodes.append(focus);

    QVariantList edges;
    for (const QVariant &v : nodes) {
        const QVariantMap n = v.toMap();
        if (n.value(QStringLiteral("kind")).toString() == QStringLiteral("focus")) continue;
        const double nx    = n.value(QStringLiteral("x")).toDouble();
        const bool   isLeft = nx < centreX;
        const double x1 = isLeft ? nx + colW : centreX;
        const double y1 = n.value(QStringLiteral("y")).toDouble() + nodeH / 2.0;
        const double x2 = isLeft ? centreX : nx;
        const double y2 = focusY + nodeH / 2.0;

        QVariantMap e;
        e.insert(QStringLiteral("from"), isLeft ? n.value(QStringLiteral("id")) : QVariant(id));
        e.insert(QStringLiteral("to"), isLeft ? QVariant(id) : n.value(QStringLiteral("id")));
        e.insert(QStringLiteral("x1"), isLeft ? x1 : x2);
        e.insert(QStringLiteral("y1"), isLeft ? y1 : y2);
        e.insert(QStringLiteral("x2"), isLeft ? x2 : x1);
        e.insert(QStringLiteral("y2"), isLeft ? y2 : y1);
        // A gentle S through the gutter, so several lines into one box stay several lines.
        e.insert(QStringLiteral("c1x"), (x1 + x2) / 2.0);
        e.insert(QStringLiteral("c1y"), isLeft ? y1 : y2);
        e.insert(QStringLiteral("c2x"), (x1 + x2) / 2.0);
        e.insert(QStringLiteral("c2y"), isLeft ? y2 : y1);
        e.insert(QStringLiteral("weight"), 1.0);
        e.insert(QStringLiteral("detects"), false);
        e.insert(QStringLiteral("symmetric"), true);   // one hop, no direction to claim
        e.insert(QStringLiteral("tip"), false);
        e.insert(QStringLiteral("label"), QString());
        e.insert(QStringLiteral("labelX"), (x1 + x2) / 2.0);
        e.insert(QStringLiteral("labelY"), (y1 + y2) / 2.0);
        edges.append(e);
    }

    QVariantMap out;
    out.insert(QStringLiteral("nodes"), nodes);
    out.insert(QStringLiteral("edges"), edges);
    out.insert(QStringLiteral("headings"), QVariantList());
    out.insert(QStringLiteral("width"), width);
    out.insert(QStringLiteral("height"), height);
    out.insert(QStringLiteral("focusX"), centreX + colW / 2.0);
    out.insert(QStringLiteral("focusY"), focusY + nodeH / 2.0);
    out.insert(QStringLiteral("truncated"), false);
    return out;
}

QVariantMap ModelBrowser::graph(const QString &type, const QString &id,
                                const QVariantMap &options) const
{
    if (id.isEmpty()) return {};

    if (type == kCharacteristics || type == kCauses) {
        QVariantMap g = dag(id, options);
        // The causal layout predates types and colour, so its nodes are decorated on the way out.
        QVariantList nodes;
        for (const QVariant &v : g.value(QStringLiteral("nodes")).toList()) {
            QVariantMap n    = v.toMap();
            const QString k  = n.value(QStringLiteral("kind")).toString();
            const QString nt = k == QStringLiteral("measure") ? kMeasures
                             : n.value(QStringLiteral("latent")).toBool() ? kCauses
                                                                          : kCharacteristics;
            decorateNode(n, nt, n.value(QStringLiteral("id")).toString());
            nodes.append(n);
        }
        g.insert(QStringLiteral("nodes"), nodes);
        return g;
    }

    if (type == kHealth) return {};   // a finding is not an object; it has no neighbourhood
    return neighbourhood(type, id, options);
}
