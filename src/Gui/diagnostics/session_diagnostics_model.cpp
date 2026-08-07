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

#include "session_diagnostics_model.h"

#include "../../Diagnostics/live_measure_source.h"
#include "../../Diagnostics/measure_facets.h"
#include "../../Diagnostics/norm_provider.h"
#include "../../Diagnostics/pack_io.h"
#include "../../Diagnostics/pack_provider.h"
#include "../../Diagnostics/screen_pack.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace pinpoint::analysis;

namespace {

// The envelope's own schema tag, separate from kDiagSchemaVersion.
//
// TWO VERSIONS BECAUSE THERE ARE TWO CONTRACTS. kDiagSchemaVersion tags the ROW shape, which
// diagnostic_ledger.h owns and which a corpus tool reads without knowing this class exists.
// This one tags what the MODEL wraps around it — the focus contract, the declared miss, the
// screen answers, the session meta. They change on completely different schedules, and
// sharing one number would force a bump on every consumer whenever a panel gained a field.
constexpr int kEnvelopeVersion = 1;

const QString kDiagnosticsFile = QStringLiteral("diagnostics.json");
const QString kProfileFile     = QStringLiteral("fault_profile.json");

// The true minus, U+2212. Brief §6: the value slot and the corridor slot must agree, and a
// hyphen-minus beside a typographic one in the same cell is the kind of thing a golfer reads
// as a bug in the numbers.
const QChar kMinus = QChar(0x2212);

QString fmtNumber(double v, int decimals = 1)
{
    if (!std::isfinite(v)) return QStringLiteral("—");
    QString s = QString::number(std::fabs(v), 'f', decimals);
    return (v < 0.0) ? QString(kMinus) + s : s;
}

QString withUnit(const QString &s, const QString &unit)
{
    return unit.isEmpty() ? s : s + unit;
}

QString stageName(Stage s)
{
    switch (s) {
    case Stage::Cold:        return QStringLiteral("cold");
    case Stage::Forming:     return QStringLiteral("forming");
    case Stage::Established: return QStringLiteral("established");
    case Stage::Closing:     break;
    }
    return QStringLiteral("closing");
}

Stage stageFromName(const QString &s)
{
    if (s == QLatin1String("forming"))     return Stage::Forming;
    if (s == QLatin1String("established")) return Stage::Established;
    if (s == QLatin1String("closing"))     return Stage::Closing;
    return Stage::Cold;
}

QString shotStateKind(ShotState s)
{
    switch (s) {
    case ShotState::Fired: return QStringLiteral("fired");
    case ShotState::Clean: return QStringLiteral("clean");
    case ShotState::NotAssessable: break;
    }
    return QStringLiteral("notAssessable");
}

QString tierTag(Tier t)
{
    switch (t) {
    case Tier::Pattern:  return QStringLiteral("pattern");
    case Tier::Watching: return QStringLiteral("watching");
    case Tier::Clean:    break;
    }
    return QStringLiteral("clean all session");
}

// The link's word and its stroke, straight off the brief's §4.1 table. ONE place, because the
// word and the stroke encode the same claim and a surface that could pick them independently
// would eventually draw "Coherent" with an arrowhead.
struct LinkStyle {
    const char *word;
    const char *stroke;   // "solidAccent" | "solid" | "dashed" | "dotted" | "dottedFaint"
    double      width;
    bool        arrow;
    double      opacity;
};

LinkStyle styleOf(LinkGrade g)
{
    switch (g) {
    case LinkGrade::MovedTogether:          return { "Moved together",           "solidAccent", 2.0, true,  1.0 };
    case LinkGrade::ConditionallyDependent: return { "Conditionally dependent",  "solid",       1.5, true,  1.0 };
    case LinkGrade::Coherent:               return { "Coherent",                 "dashed",      1.5, false, 1.0 };
    case LinkGrade::PresentTogether:        return { "Present together",         "dotted",      1.0, false, 1.0 };
    case LinkGrade::Unanchored:             break;
    }
    return { "Unanchored", "dottedFaint", 1.0, false, 0.5 };
}

QString gradeName(LinkGrade g)
{
    switch (g) {
    case LinkGrade::MovedTogether:          return QStringLiteral("movedTogether");
    case LinkGrade::ConditionallyDependent: return QStringLiteral("conditionallyDependent");
    case LinkGrade::Coherent:               return QStringLiteral("coherent");
    case LinkGrade::PresentTogether:        return QStringLiteral("presentTogether");
    case LinkGrade::Unanchored:             break;
    }
    return QStringLiteral("unanchored");
}

QString chainNodeKindName(ChainNodeKind k)
{
    switch (k) {
    case ChainNodeKind::LiveCard:     return QStringLiteral("live");
    case ChainNodeKind::Ghost:        return QStringLiteral("ghost");
    case ChainNodeKind::ScreenedRoot: return QStringLiteral("screenedRoot");
    case ChainNodeKind::Outcome:      break;
    }
    return QStringLiteral("outcome");
}

} // namespace

// ── Construction ────────────────────────────────────────────────────────────────────────

SessionDiagnosticsModel::SessionDiagnosticsModel(QObject *parent)
    : QObject(parent)
    , m_packProv(makeCharacteristicPackProvider())
    , m_norms(sharedNormProvider())
{
    // ONE worker thread, not "a worker thread". Two would reduce shot 8 before shot 7 on an
    // unlucky schedule, and while conditionLedgers() sorts by nothing that would care, the
    // TICK RUN is drawn in vector order and the run is the panel's most literal claim about
    // what happened when. A pool of one is also the honest expression of the load: one swing
    // arrives every twenty seconds and takes well under a second to reduce.
    m_pool.setMaxThreadCount(1);
    rebuild();
}

SessionDiagnosticsModel::~SessionDiagnosticsModel()
{
    // The pool holds lambdas that capture `this`. Draining before the members go is not
    // optional, and there is no cancellation to offer — a detection in flight is a second of
    // work, so waiting is cheaper than a cancellation flag every worker would have to check.
    m_pool.waitForDone();
}

// ── Simple inputs ───────────────────────────────────────────────────────────────────────

void SessionDiagnosticsModel::setCadence(const QString &mode)
{
    // The same fall-back regime AppSettings applies: anything unrecognised is bandwidth, the
    // quieter of the two. See app_settings.h for why that is the safe direction.
    const QString m = (mode == QLatin1String("everyShot")) ? mode : QStringLiteral("bandwidth");
    if (m_cadence == m) return;
    m_cadence = m;
    emit cadenceChanged();
    // Cadence changes what is SURFACED and nothing that is stored, so this rebuilds the
    // surface and never touches a row.
    rebuild();
}

void SessionDiagnosticsModel::setGradePolicy(const QString &name)
{
    if (m_policyName == name) return;
    m_policyName = name;
    emit gradePolicyChanged();
    // Deliberately NOT a re-detection of the shots already in the ledger. The rows record
    // what a corridor said at the moment the swing was graded; re-grading history under a
    // policy the golfer chose mid-session would silently rewrite the ticks they have already
    // been shown. The new policy applies from the next shot, and a full re-read is what
    // activateSession() on a fresh model is for.
}

void SessionDiagnosticsModel::setReviewing(bool on)
{
    if (m_reviewing == on) return;
    m_reviewing = on;
    emit reviewingChanged();
    rebuild();
}

void SessionDiagnosticsModel::setSelectedShotId(int id)
{
    if (m_selectedShotId == id) return;
    m_selectedShotId = id;
    emit selectedShotIdChanged();
    rebuild();
}

Stage SessionDiagnosticsModel::effectiveStage() const
{
    return (m_closed || m_reviewing) ? Stage::Closing : m_stage;
}

QString SessionDiagnosticsModel::stage() const { return stageName(effectiveStage()); }

int SessionDiagnosticsModel::patternCount() const
{
    int n = 0;
    for (const ConditionLedger &l : m_ledgers)
        if (l.tier == Tier::Pattern) ++n;
    return n;
}

// ── Ingest ──────────────────────────────────────────────────────────────────────────────

void SessionDiagnosticsModel::ingestShot(int shotId, const QString &swingDir)
{
    // IDEMPOTENCE IS DECIDED HERE, ON THE CALLING THREAD, AND BEFORE ANY WORK IS QUEUED.
    // Doing it in applyIngested() would still produce one row per shot, but it would parse
    // the same 30 MB document twice when a back-fill scan raced a live shotProcessed — which
    // is exactly the moment the app can least afford it.
    if (shotId < 0 || swingDir.isEmpty()) return;
    // A CLOSED SESSION IS FROZEN. Closing is the one lifecycle transition that is an event
    // rather than a threshold, and the panel it produces is the session's summary — a shot
    // arriving afterwards belongs to the next session, not to the one whose bookends and
    // fault-profile contribution have already been written.
    if (m_closed) return;
    if (m_ingested.contains(shotId)) return;
    m_ingested.insert(shotId);
    m_swingDirs.insert(shotId, swingDir);

    if (m_synchronous) {
        applyIngested(detectShot(shotId, swingDir));
        return;
    }

    ++m_pending;
    emit busyChanged();
    // QThreadPool::start() rather than QtConcurrent::run(): there is no QFuture to wait on
    // here — the result comes back through the queued invocation below — and run()'s future
    // is [[nodiscard]] precisely so that a caller who drops it says why.
    m_pool.start([this, shotId, swingDir]() {
        const Ingested in = detectShot(shotId, swingDir);
        // Queued, so the row vector and every signal below are only ever touched by the
        // thread that owns this object. The worker holds no reference to anything mutable.
        QMetaObject::invokeMethod(this, [this, in]() { applyIngested(in); }, Qt::QueuedConnection);
    });
}

SessionDiagnosticsModel::Ingested SessionDiagnosticsModel::detectShot(int shotId,
                                                                     const QString &swingDir) const
{
    Ingested out;
    if (!m_packProv || !m_norms) return out;

    const CharacteristicPack &pack = m_packProv->pack();

    // writeSidecar true: the phase grid is cached beside the swing, so a re-activation of the
    // same session costs a small read rather than a second fat parse of every document.
    const LiveMeasureSource src(swingDir, pack);
    const LiveDetection     d = detectForSwing(src, pack, m_norms, gradePolicyByName(m_policyName));

    ShotRecord rec;
    rec.shotId    = shotId;
    rec.club      = d.club;
    rec.contextId = d.contextId;
    rec.timestampMs =
        QFileInfo(QDir(swingDir).filePath(QStringLiteral("swing.json"))).lastModified()
            .toMSecsSinceEpoch();
    // Warm-up is the FIRST-N rule only, applied inside shotWeight(). A declared warm-up flag
    // has no producer yet and inventing one from the shot index would double-count the rule.
    rec.warmUp = false;

    rec.rows.reserve(d.result.findings.size());
    for (const Finding &f : d.result.findings) {
        ConditionRow r;
        r.conditionId      = f.conditionId;
        r.confidence       = f.confidence;
        r.material         = f.material;
        r.contextId        = d.contextId;
        // LiveMeasureSource::club() applies the house-wide DRIVER stub for a shot that
        // declares none, so this seam cannot tell an undeclared club from a declared driver
        // and never trips the inferred-context demotion. Stated rather than faked: a `true`
        // here would demote every shot's ranking on a fact we do not have.
        r.contextInferred  = false;

        switch (f.state) {
        case FindingState::Fired:
            r.state     = ShotState::Fired;
            r.direction = (f.direction == Direction::High) ? 1 : -1;
            break;
        case FindingState::NotFired:
            r.state     = ShotState::Clean;
            r.direction = 0;
            break;
        case FindingState::Unavailable:
            r.state = ShotState::NotAssessable;
            // Never blank (ConditionRow's contract): the review strip prints this where a
            // corridor would go. The source knows WHY for the measure that failed; where a
            // condition names several, the first is the one that stopped it.
            if (!f.missingMeasures.isEmpty())
                r.notAssessableReason = src.missingReason(f.missingMeasures.first());
            if (r.notAssessableReason.isEmpty())
                r.notAssessableReason = missingReasonText(MissingKind::MetricNotProduced);
            break;
        }

        if (f.evidence.hasEvidence) {
            r.drivingMeasureId = f.evidence.drivingMeasureId;
            r.value            = f.evidence.value;
            r.corridorLo       = f.evidence.corridorLo;
            r.corridorHi       = f.evidence.corridorHi;
            r.z                = f.evidence.z;
        }
        rec.rows.push_back(std::move(r));
    }

    out.record           = std::move(rec);
    out.hasLaunchMonitor = src.hasLaunchMonitor();
    out.ok               = true;
    return out;
}

void SessionDiagnosticsModel::applyIngested(const Ingested &in)
{
    if (!m_synchronous) {
        m_pending = std::max(0, m_pending - 1);
        emit busyChanged();
    }
    if (!in.ok) {
        // A swing that could not be read is not a shot with no findings — it is a shot we
        // never looked at, and putting an all-NotAssessable row set in the ledger for it
        // would draw a full column of outlined ticks for a capture that may be fine. Drop
        // the reservation so a later re-activation can try again.
        m_ingested.remove(in.record.shotId);
        return;
    }

    // Shots are held in the order they were struck. The pool is single-threaded so arrivals
    // are already ordered, but a back-fill can interleave with a live shot, and the tick run
    // IS shot order — so the insertion point is found rather than assumed.
    const int id = in.record.shotId;
    auto pos = std::lower_bound(m_shots.begin(), m_shots.end(), id,
                                [](const ShotRecord &s, int v) { return s.shotId < v; });
    if (pos != m_shots.end() && pos->shotId == id) return;   // belt and braces
    m_shots.insert(pos, in.record);
    if (in.hasLaunchMonitor) m_lmShots.insert(id);

    rebuild();
    persist();
    emit shotIngested(id, !m_quiet);
}

bool SessionDiagnosticsModel::waitForIdle(int msTimeout)
{
    if (m_synchronous) return true;
    QDeadlineTimer deadline(msTimeout);
    while (m_pending > 0 && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        m_pool.waitForDone(20);
    }
    // One last drain: the worker may have finished while the queued delivery was still in the
    // event queue.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return m_pending == 0;
}

// ── Session lifecycle ───────────────────────────────────────────────────────────────────

void SessionDiagnosticsModel::activateSession(const QString &sessionDir)
{
    m_pool.waitForDone();

    m_sessionDir = sessionDir;
    m_shots.clear();
    m_ingested.clear();
    m_lmShots.clear();
    m_swingDirs.clear();
    m_displayOrder.clear();
    m_patternSet.clear();
    m_explanation = Explanation();
    m_screens.clear();
    m_focusConditionId.clear();
    m_focusFromShot = -1;
    m_declaredMiss.clear();
    m_stage  = Stage::Cold;
    m_closed = false;
    m_profile = QJsonObject();
    m_expectations.clear();

    emit sessionChanged();

    if (sessionDir.isEmpty()) {
        rebuild();
        emit intentChanged();
        return;
    }

    // ── 1. The file, if there is one ────────────────────────────────────────────────
    QFile f(diagnosticsPath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();

        // The ledger half is diagnostic_ledger.h's own, read through its own reader — the
        // gates travel with the file so a session records what it was read under.
        const QJsonObject ledgerObj = root.value(QStringLiteral("ledger")).toObject();
        m_shots = fromJson(ledgerObj, &m_opt);
        for (const ShotRecord &s : m_shots) m_ingested.insert(s.shotId);

        const QJsonObject session = root.value(QStringLiteral("session")).toObject();
        m_closed = session.value(QStringLiteral("closed")).toBool(false);
        m_stage  = stageFromName(session.value(QStringLiteral("stage")).toString());
        for (const QJsonValue &v : session.value(QStringLiteral("lmShots")).toArray())
            m_lmShots.insert(v.toInt());
        const QJsonObject dirs = session.value(QStringLiteral("swingDirs")).toObject();
        for (auto it = dirs.constBegin(); it != dirs.constEnd(); ++it)
            m_swingDirs.insert(it.key().toInt(), it.value().toString());

        const QJsonObject intent = root.value(QStringLiteral("intent")).toObject();
        m_focusConditionId = intent.value(QStringLiteral("focusConditionId")).toString();
        m_focusFromShot    = intent.value(QStringLiteral("focusFromShot")).toInt(-1);
        m_declaredMiss     = intent.value(QStringLiteral("declaredMiss")).toString();

        const QJsonObject screens = root.value(QStringLiteral("screens")).toObject();
        for (auto it = screens.constBegin(); it != screens.constEnd(); ++it)
            m_screens.insert(it.key(), it.value().toBool());
    }

    // ── 2. The athlete's history ────────────────────────────────────────────────────
    loadProfile();

    // ── 3. Reconcile against what is actually on disk ───────────────────────────────
    //
    // The swing directories are the truth about which shots EXIST; diagnostics.json is the
    // truth about which have been reduced. The difference is work, and doing it here is what
    // makes three separate situations one code path: a crash mid-session, a session the panel
    // was switched on half way through, and a past session opened for review that predates
    // this feature entirely (no file at all — every shot back-fills).
    //
    // Shot ids come from the directory name (swing_0007 -> 7) rather than from a re-parse of
    // each document: the id has to agree with the carousel's, the carousel takes it from the
    // same place, and reading 30 MB to learn a number that is in the filename is not a trade
    // worth making.
    rebuild();          // publish what the file gave us before the back-fill starts

    const QDir dir(sessionDir);
    QStringList swings = dir.entryList(QStringList{ QStringLiteral("swing_*") },
                                       QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : swings) {
        bool ok = false;
        const int id = name.mid(name.lastIndexOf(QLatin1Char('_')) + 1).toInt(&ok);
        if (!ok) continue;
        if (m_ingested.contains(id)) continue;
        ingestShot(id, dir.filePath(name));
    }

    emit intentChanged();
    rebuild();
}

void SessionDiagnosticsModel::closeSession()
{
    if (m_closed) return;
    m_pool.waitForDone();
    m_closed = true;
    rebuild();
    persist();
    // The profile is updated ONCE, at close, and from the frozen ledger. Updating it per shot
    // would let a session that ended after four swings claim the same standing as one that
    // ran to thirty, and the whole value of the profile is that a "pattern in 8 of your last
    // 10 sessions" line is countable in sessions rather than in moments.
    updateProfile();
    emit surfaceChanged();
}

// ── Intent ──────────────────────────────────────────────────────────────────────────────

void SessionDiagnosticsModel::declareFocus(const QString &conditionId)
{
    if (conditionId.isEmpty()) { clearFocus(); return; }
    m_focusConditionId = conditionId;
    // The split is AFTER the shots already struck: FocusSplit::baselineEnd is the index of
    // the last baseline shot, and the shot count is exactly one past it.
    m_focusFromShot = int(m_shots.size());
    emit intentChanged();
    rebuild();
    persist();
}

void SessionDiagnosticsModel::clearFocus()
{
    if (m_focusConditionId.isEmpty() && m_focusFromShot < 0) return;
    m_focusConditionId.clear();
    m_focusFromShot = -1;
    emit intentChanged();
    rebuild();
    persist();
}

void SessionDiagnosticsModel::declareMiss(const QString &missId)
{
    if (m_declaredMiss == missId) return;
    m_declaredMiss = missId;
    emit intentChanged();
    rebuild();
    persist();
}

void SessionDiagnosticsModel::recordScreenResult(const QString &conditionId, bool present)
{
    if (conditionId.isEmpty()) return;
    m_screens.insert(conditionId, present);
    // A screen answered mid-session counts immediately (§A5) — the whole point of a screened
    // root is that one thirty-second test settles what no number of swings can.
    m_patternSet.clear();       // force explain() to re-run: knownScreenResults changed
    rebuild();
    persist();
}

// ── The reduction ───────────────────────────────────────────────────────────────────────

void SessionDiagnosticsModel::rebuild()
{
    m_ledgers  = conditionLedgers(m_shots, m_opt);
    m_coverage = sessionCoverage(m_ledgers, m_packProv ? int(m_packProv->pack().conditions.size()) : 0);

    buildGraph();

    FocusSplit focus;
    focus.declared    = !m_focusConditionId.isEmpty() && m_focusFromShot >= 0;
    focus.baselineEnd = m_focusFromShot - 1;

    m_links = gradeLinks(m_nodes, m_edges, m_ledgers, focus, m_opt);
    m_rails = extractChains(m_nodes, m_edges, m_ledgers, m_links, m_opt);

    // The stage ratchet. `m_stage` is both the input and the output — sessionStage() is a
    // pure function of (previous, evidence, closed), so the one-way behaviour lives in the
    // header rather than in a state machine here.
    //
    // REVIEW IS NOT PASSED AS `sessionClosed`, deliberately. Looking at a session must not be
    // able to close it: the ratchet is permanent, so a live model that showed a past session
    // and then went back to live would be stuck at Closing forever. Review freezes the
    // DISPLAYED stage instead — effectiveStage() — which is the same picture and a reversible
    // fact.
    m_stage = sessionStage(m_stage, m_ledgers, m_links, m_closed, m_opt);

    refreshExplanation();

    // Display order, with rank-band hysteresis, over the pattern set. The previous order IS
    // the state — hystereticOrder() holds none, by design.
    {
        const std::vector<QString> prev(m_displayOrder.constBegin(), m_displayOrder.constEnd());
        const std::vector<QString> next = hystereticOrder(rankScores(), prev, m_opt.rankBandWidth);
        m_displayOrder.clear();
        for (const QString &id : next) m_displayOrder.push_back(id);
    }

    // buildThisShot() BEFORE buildHeader(): it is the one function that reads cadence, and
    // the header quotes its verdict ("BANDWIDTH · QUIET").
    buildThisShot();
    buildHeader();
    buildCards();
    buildChains();
    buildDriver();
    buildBookends();

    emit surfaceChanged();
}

// WHICH CONDITIONS ARE MARSHALLED INTO THE GRAPH, and why it is not all of them.
//
// extractChains() treats every unmeasurable node as PARTICIPATING — that is the honesty
// device: a chain that quietly omitted its unmeasurable middle would claim a continuity it
// does not have. Handing it all ~150 pack conditions would therefore hand it ~150
// participating nodes and every authored edge between them, and the rail would fill with
// paths that have nothing to do with this session.
//
// So the node set is the SESSION'S OWN NEIGHBOURHOOD, defined by three rules and nothing
// else:
//   · every pattern-tier condition (the session's assertions), plus the declared miss;
//   · everything authored UPSTREAM of those, transitively — this is what puts Chain B's
//     screened root and its two ghosts on the rail, three hops above the one live node;
//   · everything on a directed path from one of those down to the declared miss, so the
//     outcome node is reachable rather than orphaned.
// Nothing else. A condition that neither reached pattern tier nor lies between one and an
// outcome is not part of this session's picture, and drawing it would be inventing context
// rather than reporting it.
void SessionDiagnosticsModel::buildGraph()
{
    m_nodes.clear();
    m_edges.clear();
    m_missChain.clear();
    if (!m_packProv) return;

    const CharacteristicPack &pack = m_packProv->pack();

    QSet<QString> seeds;
    for (const ConditionLedger &l : m_ledgers)
        if (l.tier == Tier::Pattern) seeds.insert(l.id);
    if (!m_declaredMiss.isEmpty() && pack.condition(m_declaredMiss)) seeds.insert(m_declaredMiss);

    QSet<QString> keep = seeds;
    for (const QString &id : std::as_const(seeds))
        keep.unite(causalClosure(pack, id, /*downstream*/ false));

    if (!m_declaredMiss.isEmpty() && pack.condition(m_declaredMiss)) {
        const QSet<QString> missAncestors = causalClosure(pack, m_declaredMiss, false);
        for (const QString &id : std::as_const(seeds)) {
            const QSet<QString> below = causalClosure(pack, id, /*downstream*/ true);
            for (const QString &d : below)
                if (missAncestors.contains(d) || d == m_declaredMiss) keep.insert(d);
        }
        // The pre-armed chain, published for the panel: everything authored upstream of the
        // declared outcome, whether or not the capture can see it. Intent shapes ATTENTION.
        QStringList arm(missAncestors.constBegin(), missAncestors.constEnd());
        std::sort(arm.begin(), arm.end());
        for (const QString &id : std::as_const(arm)) {
            const ConditionLedger *l = ledger(id);
            m_missChain.append(QVariantMap{
                { QStringLiteral("id"),         id },
                { QStringLiteral("name"),       conditionName(id) },
                { QStringLiteral("measurable"), l && l->assessable >= 1 },
                { QStringLiteral("tier"),       l ? tierTag(l->tier) : QStringLiteral("clean all session") },
            });
        }
    }

    // Pack order, never hash order — the rail must not reshuffle between launches.
    for (const Condition &c : pack.conditions) {
        if (!keep.contains(c.id)) continue;
        const ConditionLedger *l = ledger(c.id);

        NodeSpec n;
        n.id = c.id;
        // MEASURABLE MEANS "THIS CAPTURE ANSWERED IT", not "the pack authored a measure".
        // A condition with a signal whose producer never lands is exactly as unanswerable as
        // one with no signal at all, and the session's own rows are the only honest evidence
        // of which is which on THIS capture.
        n.measurable    = l && l->assessable >= 1;
        // The three ways a node can be unmeasurable, and they are read off the pack's own
        // epistemics rather than guessed from the rows: ConfirmedBy is the field that says
        // HOW a condition can be established today.
        n.screened      = c.confirmedBy == ConfirmedBy::Screened;
        n.screenEntered = m_screens.contains(c.id);
        n.asserted      = c.confirmedBy == ConfirmedBy::Asserted;
        if (!m_declaredMiss.isEmpty() && c.id == m_declaredMiss) n.outcomeId = m_declaredMiss;
        // TEMPORAL ORDER IS APPROXIMATED BY CONDITION GROUP, which is gradeLink()'s own
        // stated approximation: the group order IS the swing (setup, posture, lateral, arms
        // and club, release, sequence, impact, finish, ball flight), and it is the finest
        // ordering available until the detector timestamps a finding within a shot.
        const auto &groups = allConditionGroups();
        n.phaseOrder = int(std::find(groups.begin(), groups.end(), c.group) - groups.begin());
        m_nodes.push_back(std::move(n));
    }

    for (const Edge &e : pack.edges) {
        if (e.type != EdgeType::Causes) continue;         // rule 3: only authored causation
        if (!keep.contains(e.from) || !keep.contains(e.to)) continue;
        EdgeSpec s;
        s.from = e.from;
        s.to   = e.to;
        // The pack's Edge carries no directional SENSE — it says A causes B, not "A high
        // implies B high". Type 0 is the honest marshalling: gradeLink() then rests
        // coherence on temporal order alone rather than on a sign nobody authored.
        s.type     = 0;
        s.strength = int(e.strength);
        m_edges.push_back(std::move(s));
    }
}

void SessionDiagnosticsModel::refreshExplanation()
{
    QStringList patterns;
    for (const ConditionLedger &l : m_ledgers)
        if (l.tier == Tier::Pattern) patterns.push_back(l.id);
    patterns.sort();

    if (patterns == m_patternSet) return;      // membership unchanged: no re-rank (§A5)
    m_patternSet = patterns;

    if (!m_packProv) { m_explanation = Explanation(); return; }

    // explain() takes a DetectionResult, and what it is handed here is deliberately not one
    // shot's. It is the SESSION's assertion — every pattern-tier condition presented as
    // Fired, every other assessed condition as NotFired — so the greedy set cover runs over
    // what the session believes rather than over what the last ball happened to do. That is
    // the whole of §A5's "over the pattern-tier fired set, not per shot".
    DetectionResult det;
    for (const ConditionLedger &l : m_ledgers) {
        if (l.tier == Tier::Pattern) {
            Finding f;
            f.conditionId = l.id;
            f.state       = FindingState::Fired;
            f.confidence  = 1.0f;
            det.findings.push_back(std::move(f));
        } else if (l.assessable >= 1) {
            Finding f;
            f.conditionId = l.id;
            f.state       = FindingState::NotFired;
            det.findings.push_back(std::move(f));
        }
    }
    m_explanation = explain(m_packProv->pack(), det, m_screens);
}

// THE ONLY READER OF THE FAULT PROFILE. hystereticOrder()'s comment names this as the one
// place a prevalence bias may legitimately enter, and the reason it is safe is structural:
// this function returns a display SCORE, the score reaches nothing but an ordering, and there
// is no path from an ordering to a tier, a corridor or a firing.
std::vector<RankedCondition> SessionDiagnosticsModel::rankScores() const
{
    std::vector<RankedCondition> out;
    for (const ConditionLedger &l : m_ledgers) {
        if (l.tier != Tier::Pattern) continue;
        RankedCondition r;
        r.id = l.id;
        // Evidence weight first — the Wilson bound is the ordinal the tier gate itself uses,
        // so the card at the top is the one the session is most sure about.
        r.score = l.wilsonLower;
        // Then history, at a tenth of the weight: enough to break a tie between two equally
        // evidenced patterns in favour of the one this golfer keeps producing, never enough
        // to lift a weakly evidenced one over a strongly evidenced one.
        r.score += 0.10 * profileBias(l.id);
        // And the two demotions the pack already carries. Immateriality is RANKING ONLY by
        // ContextBinding's own contract, and this is where it is allowed to act.
        const ConditionRow *latest = m_shots.empty() ? nullptr : rowFor(m_shots.back(), l.id);
        if (latest && !latest->material)        r.score -= 0.05;
        if (latest && latest->contextInferred)  r.score -= 0.05;
        out.push_back(std::move(r));
    }
    return out;
}

double SessionDiagnosticsModel::profileBias(const QString &conditionId) const
{
    const QJsonObject conds = m_profile.value(QStringLiteral("conditions")).toObject();
    const QJsonObject c     = conds.value(conditionId).toObject();
    const double seen    = c.value(QStringLiteral("sessionsSeen")).toDouble(0.0);
    const double pattern = c.value(QStringLiteral("sessionsPattern")).toDouble(0.0);
    if (!(seen > 0.0)) return 0.0;
    return std::clamp(pattern / seen, 0.0, 1.0);
}

// ── Persistence ─────────────────────────────────────────────────────────────────────────

QString SessionDiagnosticsModel::diagnosticsPath() const
{
    return m_sessionDir.isEmpty() ? QString() : QDir(m_sessionDir).filePath(kDiagnosticsFile);
}

QString SessionDiagnosticsModel::faultProfilePath() const
{
    // The ATHLETE folder, which is the session directory's parent —
    // <athleteLibraryPath>/<athlete>/<session>/. A profile written inside the session folder
    // would be a profile with a sample size of one, which is the one thing it must not be.
    if (m_sessionDir.isEmpty()) return QString();
    QDir d(m_sessionDir);
    if (!d.cdUp()) return QString();
    return d.filePath(kProfileFile);
}

QJsonObject SessionDiagnosticsModel::envelope() const
{
    QJsonArray lm;
    QList<int> lmIds(m_lmShots.constBegin(), m_lmShots.constEnd());
    std::sort(lmIds.begin(), lmIds.end());
    for (int id : std::as_const(lmIds)) lm.append(id);

    QJsonObject dirs;
    for (auto it = m_swingDirs.constBegin(); it != m_swingDirs.constEnd(); ++it)
        dirs.insert(QString::number(it.key()), it.value());

    QJsonObject session;
    session[QStringLiteral("sessionDir")] = m_sessionDir;
    session[QStringLiteral("sessionId")]  = QFileInfo(m_sessionDir).fileName();
    session[QStringLiteral("closed")]     = m_closed;
    session[QStringLiteral("stage")]      = stageName(m_stage);
    session[QStringLiteral("shotCount")]  = int(m_shots.size());
    session[QStringLiteral("gradePolicy")]= m_policyName;
    session[QStringLiteral("lmShots")]    = lm;
    session[QStringLiteral("swingDirs")]  = dirs;
    session[QStringLiteral("writtenAtMs")]= double(QDateTime::currentMSecsSinceEpoch());

    QJsonObject intent;
    intent[QStringLiteral("focusConditionId")] = m_focusConditionId;
    intent[QStringLiteral("focusFromShot")]    = m_focusFromShot;
    intent[QStringLiteral("declaredMiss")]     = m_declaredMiss;

    QJsonObject screens;
    for (auto it = m_screens.constBegin(); it != m_screens.constEnd(); ++it)
        screens.insert(it.key(), it.value());

    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = kEnvelopeVersion;
    root[QStringLiteral("session")]       = session;
    root[QStringLiteral("intent")]        = intent;
    root[QStringLiteral("screens")]       = screens;
    // The ledger is nested rather than merged, so diagnostic_ledger.h's reader takes exactly
    // the object its writer produced — including ITS schemaVersion, which is a different
    // number about a different contract.
    root[QStringLiteral("ledger")]        = toJson(m_shots, m_opt);
    return root;
}

void SessionDiagnosticsModel::persist()
{
    const QString path = diagnosticsPath();
    if (path.isEmpty()) return;
    // The house's write-to-temp-then-rename helper (pack_io.h), not a bare QFile: an
    // interrupted write must not truncate the session's evidence, and a half-written
    // diagnostics.json would load, report what survived, and look exactly like a session in
    // which nothing happened after shot 6.
    atomicWrite(path, QJsonDocument(envelope()).toJson(QJsonDocument::Compact));
}

void SessionDiagnosticsModel::loadProfile()
{
    m_profile = QJsonObject();
    m_expectations.clear();

    const QString path = faultProfilePath();
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    m_profile = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    // The Cold state's "usually yours" list. EXPECTATIONS TO TEST, and the wording has to
    // keep saying so — these are dashed cards in the design precisely because they are the
    // one thing on the panel that is not evidence from this session.
    const QJsonObject conds = m_profile.value(QStringLiteral("conditions")).toObject();
    struct Row { QString id; int seen; int pattern; QString last; QString trend; };
    std::vector<Row> rows;
    for (auto it = conds.constBegin(); it != conds.constEnd(); ++it) {
        const QJsonObject c = it.value().toObject();
        Row r;
        r.id      = it.key();
        r.seen    = c.value(QStringLiteral("sessionsSeen")).toInt();
        r.pattern = c.value(QStringLiteral("sessionsPattern")).toInt();
        r.last    = c.value(QStringLiteral("lastSeenSession")).toString();
        r.trend   = c.value(QStringLiteral("trend")).toString();
        if (r.seen < 1 || r.pattern < 1) continue;    // nothing to expect
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        const double pa = double(a.pattern) / double(a.seen);
        const double pb = double(b.pattern) / double(b.seen);
        if (pa != pb) return pa > pb;
        if (a.pattern != b.pattern) return a.pattern > b.pattern;
        return a.id < b.id;
    });

    for (const Row &r : rows) {
        m_expectations.append(QVariantMap{
            { QStringLiteral("id"),   r.id },
            { QStringLiteral("name"), conditionName(r.id) },
            // A COUNT over sessions, for the same reason recurrence is a count over shots: at
            // this n a percentage is fabricated precision.
            { QStringLiteral("text"), QStringLiteral("pattern in %1 of your last %2 sessions")
                                          .arg(r.pattern).arg(r.seen) },
            { QStringLiteral("trend"), r.trend },
            { QStringLiteral("lastSeenSession"), r.last },
            // The sentence the design puts on the panel and the implementation has to keep
            // true. It ships as data so no surface can drop it.
            { QStringLiteral("caveat"),
              QStringLiteral("an expectation to test, not a finding") },
        });
    }
}

void SessionDiagnosticsModel::updateProfile()
{
    const QString path = faultProfilePath();
    if (path.isEmpty()) return;

    const QString sessionId = QFileInfo(m_sessionDir).fileName();
    QJsonObject conds = m_profile.value(QStringLiteral("conditions")).toObject();

    for (const ConditionLedger &l : m_ledgers) {
        // Only conditions this session could actually SEE update their history. A condition
        // the capture never assessed is not evidence that it was absent, and counting it as a
        // session-seen-but-not-pattern would quietly drive its prevalence to zero every time
        // the camera could not answer it.
        if (l.assessable < 1) continue;

        QJsonObject c = conds.value(l.id).toObject();
        const int seenBefore    = c.value(QStringLiteral("sessionsSeen")).toInt();
        const int patternBefore = c.value(QStringLiteral("sessionsPattern")).toInt();
        const QString lastSession = c.value(QStringLiteral("lastSeenSession")).toString();
        if (lastSession == sessionId) continue;        // idempotent: closing twice counts once

        const int seen    = seenBefore + 1;
        const int pattern = patternBefore + (l.tier == Tier::Pattern ? 1 : 0);

        // A three-valued trend and nothing finer. "rising"/"falling"/"steady" over the
        // prevalence before and after this session is all two numbers can support, and a
        // slope over a handful of sessions dressed as a percentage would be the same
        // fabricated precision the recurrence caption refuses.
        QString trend = QStringLiteral("steady");
        if (seenBefore > 0) {
            const double before = double(patternBefore) / double(seenBefore);
            const double after  = double(pattern) / double(seen);
            if (after > before + 1e-9)      trend = QStringLiteral("rising");
            else if (after < before - 1e-9) trend = QStringLiteral("falling");
        }

        c[QStringLiteral("sessionsSeen")]    = seen;
        c[QStringLiteral("sessionsPattern")] = pattern;
        c[QStringLiteral("trend")]           = trend;
        if (l.tier == Tier::Pattern) c[QStringLiteral("lastPatternSession")] = sessionId;
        c[QStringLiteral("lastSeenSession")] = sessionId;
        conds.insert(l.id, c);
    }

    QJsonObject root = m_profile;
    root[QStringLiteral("schemaVersion")] = kEnvelopeVersion;
    root[QStringLiteral("conditions")]    = conds;
    root[QStringLiteral("updatedAtMs")]   = double(QDateTime::currentMSecsSinceEpoch());
    m_profile = root;
    atomicWrite(path, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// ── Lookups and formatters ──────────────────────────────────────────────────────────────

const ConditionLedger *SessionDiagnosticsModel::ledger(const QString &id) const
{
    return ledgerFor(m_ledgers, id);
}

QString SessionDiagnosticsModel::conditionName(const QString &id) const
{
    if (!m_packProv) return id;
    const Condition *c = m_packProv->pack().condition(id);
    return (c && !c->label.isEmpty()) ? c->label : id;
}

QString SessionDiagnosticsModel::consequenceOf(const QString &id) const
{
    if (!m_packProv) return QString();
    const Condition *c = m_packProv->pack().condition(id);
    return c ? c->consequence.text() : QString();
}

QString SessionDiagnosticsModel::measureLabelOf(const QString &measureId) const
{
    if (measureId.isEmpty() || !m_packProv) return QString();
    const Measure *m = m_packProv->pack().measure(measureId);
    return m ? measureDisplayLabel(*m) : measureId;
}

QString SessionDiagnosticsModel::measureUnitOf(const QString &measureId) const
{
    if (measureId.isEmpty() || !m_packProv) return QString();
    const Measure *m = m_packProv->pack().measure(measureId);
    return m ? m->unit : QString();
}

QString SessionDiagnosticsModel::measurePhaseOf(const QString &measureId) const
{
    if (measureId.isEmpty() || !m_packProv) return QString();
    const Measure *m = m_packProv->pack().measure(measureId);
    if (!m) return QString();
    // Where the reduction LANDS, which is the phase a card should name: the anchor for an
    // `at`, the window's end for a delta or a rate. An extremum names its span's end too —
    // the peak is somewhere inside it and the pack does not record where.
    if (m->reducer.kind == ReducerKind::At && m->reducer.anchor)
        return phaseLabel(*m->reducer.anchor);
    return phaseLabel(m->reducer.window.second);
}

int SessionDiagnosticsModel::indexOfShot(int shotId) const
{
    for (int i = 0; i < int(m_shots.size()); ++i)
        if (m_shots[size_t(i)].shotId == shotId) return i;
    return -1;
}

int SessionDiagnosticsModel::focusIndex() const
{
    if (m_shots.empty()) return -1;
    if (m_reviewing && m_selectedShotId >= 0) {
        const int i = indexOfShot(m_selectedShotId);
        if (i >= 0) return i;
    }
    return int(m_shots.size()) - 1;
}

QVariantList SessionDiagnosticsModel::ticksFor(const ConditionLedger &l, int selectedIndex) const
{
    QVariantList out;
    for (int i = 0; i < int(l.run.size()); ++i) {
        out.append(QVariantMap{
            { QStringLiteral("state"),  shotStateKind(l.run[size_t(i)]) },
            { QStringLiteral("shotId"), i < int(m_shots.size()) ? m_shots[size_t(i)].shotId : -1 },
            // The selected shot's tick is drawn wide and outlined (brief §6). Which tick that
            // is, is decided here rather than by the QML comparing ids in a delegate.
            { QStringLiteral("selected"), i == selectedIndex },
        });
    }
    return out;
}

// ── Zone 1: the header ──────────────────────────────────────────────────────────────────

void SessionDiagnosticsModel::buildHeader()
{
    const int n        = int(m_shots.size());
    const int patterns = patternCount();
    const int fi       = focusIndex();
    const Stage st     = effectiveStage();

    QVariantMap h;
    h[QStringLiteral("stage")]        = stageName(st);
    h[QStringLiteral("stageLabel")]   = stageName(st).toUpper();
    h[QStringLiteral("shotCount")]    = n;
    h[QStringLiteral("patternCount")] = patterns;
    h[QStringLiteral("reviewing")]    = m_reviewing;
    h[QStringLiteral("closed")]       = m_closed;

    h[QStringLiteral("shotLabel")] =
        n == 0 ? QStringLiteral("no shots yet")
               : (m_reviewing && fi >= 0
                      ? QStringLiteral("shot %1 of %2").arg(fi + 1).arg(n)
                      : QStringLiteral("shot %1").arg(n));

    // A COUNT over all shots, and it says so — the review header's whole job is stating the
    // tense, because the panel deliberately does NOT rewind to what it knew at the selected
    // shot (brief §6).
    h[QStringLiteral("countLine")] =
        QStringLiteral("%1 %2 · counted over all %3 %4")
            .arg(patterns)
            .arg(patterns == 1 ? QStringLiteral("pattern") : QStringLiteral("patterns"))
            .arg(n)
            .arg(n == 1 ? QStringLiteral("shot") : QStringLiteral("shots"));

    h[QStringLiteral("reviewBadge")] =
        m_reviewing && fi >= 0 ? QStringLiteral("REVIEWING · shot %1 of %2").arg(fi + 1).arg(n)
                               : QString();
    h[QStringLiteral("reviewNote")] =
        m_reviewing ? QStringLiteral("final session state · this shot read inside the finished ledger")
                    : QString();
    h[QStringLiteral("cadenceNote")] = m_quiet ? QStringLiteral("BANDWIDTH · QUIET") : QString();

    // The Cold body line. One sentence, and it is the em-dash philosophy: say the n is too
    // small, do not fill the space with something that looks like a finding.
    h[QStringLiteral("coldLine")] =
        st == Stage::Cold ? QStringLiteral("Too few swings to call a pattern.") : QString();
    // The Forming body line — the one that stops a reader assuming the model simply failed to
    // find a chain. It did not fail; it authors no edge between these two.
    h[QStringLiteral("formingLine")] =
        st == Stage::Forming && patterns >= 2
            ? QStringLiteral("No chain is drawn: the model authors no edge between these patterns.")
            : QString();
    h[QStringLiteral("closingLine")] =
        st == Stage::Closing
            ? QStringLiteral("Counts are session totals; this shot is the wide tick. "
                             "Recurrence is a count, not a rate at this n.")
            : QString();

    m_headerInfo = h;
}

// ── Zone 1: the this-shot strip, and the ONE place cadence is read ──────────────────────

void SessionDiagnosticsModel::buildThisShot()
{
    m_thisShot.clear();
    m_afterShotDelta = QVariantMap();
    m_quiet = false;

    const int fi = focusIndex();
    if (fi < 0) return;

    const ShotRecord &shot = m_shots[size_t(fi)];

    int fired = 0, clean = 0, na = 0, firedPatterns = 0;
    QVariantList firedList, newPatterns;
    for (const ConditionRow &r : shot.rows) {
        const ConditionLedger *l = ledger(r.conditionId);
        switch (r.state) {
        case ShotState::Fired: {
            ++fired;
            const bool pattern = l && l->tier == Tier::Pattern;
            if (pattern) ++firedPatterns;
            QVariantMap chip{
                { QStringLiteral("id"),    r.conditionId },
                { QStringLiteral("name"),  conditionName(r.conditionId) },
                { QStringLiteral("kind"),  QStringLiteral("fired") },
                { QStringLiteral("tier"),  l ? tierTag(l->tier) : QStringLiteral("watching") },
                { QStringLiteral("focus"), r.conditionId == m_focusConditionId },
            };
            firedList.append(chip);
            m_thisShot.append(chip);
            if (l && l->freshThisShot) {
                newPatterns.append(QVariantMap{ { QStringLiteral("id"), r.conditionId },
                                                { QStringLiteral("name"), conditionName(r.conditionId) } });
            }
            break;
        }
        case ShotState::Clean:         ++clean; break;
        case ShotState::NotAssessable: ++na;    break;
        }
    }

    // An explicit CLEAN chip, never an empty strip. "Nothing fired" and "the panel has not
    // caught up" look identical on an empty row, and only one of them is good news.
    if (fired == 0 && clean > 0) {
        m_thisShot.append(QVariantMap{ { QStringLiteral("kind"), QStringLiteral("clean") },
                                       { QStringLiteral("name"), QStringLiteral("clean on every measurable condition") } });
    }
    if (na > 0) {
        m_thisShot.append(QVariantMap{
            { QStringLiteral("kind"), QStringLiteral("notAssessable") },
            { QStringLiteral("name"), QStringLiteral("%1 not assessable on this capture").arg(na) },
        });
    }

    // ── The clean-streak test, defined here because it has to be defined somewhere ──
    //
    // A CLEAN STREAK is a run of consecutive shots on which NOTHING fired; it BREAKS on the
    // first shot that fires anything after at least one such shot. Not "a pattern's own run
    // of clean shots ended" — that fires on every ordinary shot in a session with six
    // patterns and would make bandwidth mode indistinguishable from every-shot. The streak
    // is a property of the SHOT, which is what the golfer between balls is actually asking
    // about: "was that one different?".
    bool streakBroken = false;
    if (fired > 0 && fi > 0) {
        const ShotRecord &prev = m_shots[size_t(fi - 1)];
        bool prevFiredAny = false;
        for (const ConditionRow &r : prev.rows)
            if (r.state == ShotState::Fired) { prevFiredAny = true; break; }
        streakBroken = !prevFiredAny;
    }

    // THE ONE PLACE m_cadence IS READ. Bandwidth surfaces the after-shot moment only when a
    // pattern-tier condition fired on this shot or a clean streak broke; every-shot always
    // surfaces. Post-session there is no gating at all (brief §6) — a finished session is
    // read, not fed back.
    const bool everyShot = m_cadence == QLatin1String("everyShot");
    const bool surfaced  = m_reviewing || m_closed || everyShot
                        || firedPatterns > 0 || streakBroken;
    m_quiet = !surfaced;

    QVariantMap d;
    d[QStringLiteral("shotId")]              = shot.shotId;
    d[QStringLiteral("shotIndex")]           = fi;
    d[QStringLiteral("firedCount")]          = fired;
    d[QStringLiteral("cleanCount")]          = clean;
    d[QStringLiteral("notAssessableCount")]  = na;
    d[QStringLiteral("firedPatternCount")]   = firedPatterns;
    d[QStringLiteral("fired")]               = firedList;
    d[QStringLiteral("newPatterns")]         = newPatterns;
    d[QStringLiteral("streakBroken")]        = streakBroken;
    d[QStringLiteral("surfaced")]            = surfaced;
    // ONE POPULATION in the headline, per brief §6: how many of the conditions this capture
    // could speak to fired, and how many of those the session already calls patterns.
    d[QStringLiteral("headline")] =
        QStringLiteral("%1 of %2 conditions fired on this swing · %3 of them %4")
            .arg(fired).arg(fired + clean).arg(firedPatterns)
            .arg(firedPatterns == 1 ? QStringLiteral("pattern") : QStringLiteral("patterns"));
    d[QStringLiteral("note")] =
        na > 0 ? QStringLiteral("%1 %2 not assessable on this capture")
                     .arg(na).arg(na == 1 ? QStringLiteral("measure") : QStringLiteral("measures"))
               : QString();

    // Under a focus contract the focused node's marker is the headline of the moment (§B3).
    if (!m_focusConditionId.isEmpty()) {
        const ConditionRow *r = rowFor(shot, m_focusConditionId);
        const QString state = r ? shotStateKind(r->state) : QStringLiteral("notAssessable");
        d[QStringLiteral("focusId")]    = m_focusConditionId;
        d[QStringLiteral("focusState")] = state;
        d[QStringLiteral("focusHeadline")] =
            QStringLiteral("%1 — %2").arg(conditionName(m_focusConditionId),
                                          state == QLatin1String("fired")   ? QStringLiteral("fired here")
                                        : state == QLatin1String("clean")   ? QStringLiteral("clean here")
                                                                            : QStringLiteral("not measured here"));
    }

    m_afterShotDelta = d;
}

// ── Zone 2: pattern cards and the watching row ──────────────────────────────────────────

void SessionDiagnosticsModel::buildCards()
{
    m_cards.clear();
    m_watching.clear();
    m_coverageLine.clear();

    const int fi = focusIndex();
    const int selectedTick = (m_reviewing || m_closed) ? fi : -1;

    // Display order is hystereticOrder()'s, not the ledger's first-seen order: which
    // conditions exist and which one goes first churn on completely different schedules.
    QStringList ordered = m_displayOrder;
    for (const ConditionLedger &l : m_ledgers)
        if (l.tier == Tier::Pattern && !ordered.contains(l.id)) ordered.push_back(l.id);

    for (const QString &id : std::as_const(ordered)) {
        const ConditionLedger *l = ledger(id);
        if (!l || l->tier != Tier::Pattern) continue;

        const QString unit = measureUnitOf(l->drivingMeasureId);

        QVariantMap c;
        c[QStringLiteral("id")]          = id;
        c[QStringLiteral("name")]        = conditionName(id);
        c[QStringLiteral("consequence")] = consequenceOf(id);
        c[QStringLiteral("tier")]        = tierTag(l->tier);
        c[QStringLiteral("measureId")]   = l->drivingMeasureId;
        c[QStringLiteral("measure")]     = measureLabelOf(l->drivingMeasureId);
        c[QStringLiteral("phase")]       = measurePhaseOf(l->drivingMeasureId);
        // ONE wording for recurrence, and it comes out of the ledger header so an export and
        // the screen cannot disagree about what recurrence looks like.
        c[QStringLiteral("recurrence")]  = l->recurrence;
        c[QStringLiteral("fired")]       = l->fired;
        c[QStringLiteral("assessable")]  = l->assessable;
        c[QStringLiteral("fresh")]       = l->freshThisShot;
        c[QStringLiteral("resolving")]   = l->resolving;
        c[QStringLiteral("focused")]     = id == m_focusConditionId;
        c[QStringLiteral("ticks")]       = ticksFor(*l, selectedTick);

        // DIRECTION OR DISPERSION. Below the agreement gate the condition is still a pattern —
        // inconsistency is a finding — but the direction claim is suppressed, and the sentence
        // that says so is fixed by the brief to the character.
        c[QStringLiteral("directionClaimed")] = l->directionClaimed;
        c[QStringLiteral("directionAgreement")] = l->directionAgreement;
        c[QStringLiteral("directionText")] =
            l->directionClaimed
                ? (l->modalDirection == 0
                       ? QString()
                       : QStringLiteral("Consistent direction: %1 on %2% of its firings.")
                             .arg(l->modalDirection > 0 ? QStringLiteral("the high side")
                                                        : QStringLiteral("the low side"))
                             .arg(int(std::lround(l->directionAgreement * 100.0))))
                : QStringLiteral("Direction agreement %1%, below the %2% gate: dispersion, not a direction.")
                      .arg(int(std::lround(l->directionAgreement * 100.0)))
                      .arg(int(std::lround(m_opt.directionAgreementGate * 100.0)));

        // TREND + RECENCY. `resolving` wins over the plain recency line (brief §3.2).
        c[QStringLiteral("trendKnown")] = l->trendKnown;
        c[QStringLiteral("trend")]      = l->trendKnown ? (l->trend == Trend::Improving
                                                               ? QStringLiteral("improving")
                                                               : QStringLiteral("worsening"))
                                                        : QStringLiteral("stable");
        c[QStringLiteral("trendArrow")] = l->trendKnown ? (l->trend == Trend::Improving
                                                               ? QStringLiteral("↓")
                                                               : QStringLiteral("↑"))
                                                        : QString();
        c[QStringLiteral("trendText")] =
            l->trendKnown ? (l->trend == Trend::Improving ? QStringLiteral("improving")
                                                          : QStringLiteral("worsening"))
                          : (l->trendPoints < m_opt.minPointsForTrend
                                 ? QStringLiteral("trend after %1 measurable shots").arg(m_opt.minPointsForTrend)
                                 : QStringLiteral("no clear trend"));
        c[QStringLiteral("recencyText")] =
            l->resolving
                ? QStringLiteral("resolving · none in the last %1").arg(l->sinceLastFiring)
                : (l->sinceLastFiring < 0
                       ? QStringLiteral("not seen this session")
                       : l->sinceLastFiring == 0
                             ? QStringLiteral("fired on the last measurable shot")
                             : QStringLiteral("last fired %1 measurable shots ago").arg(l->sinceLastFiring));

        // This shot's marker on the card, and the review extras.
        const ConditionRow *here = (fi >= 0) ? rowFor(m_shots[size_t(fi)], id) : nullptr;
        const QString hereState = here ? shotStateKind(here->state) : QStringLiteral("notAssessable");
        c[QStringLiteral("thisShot")] = hereState;
        c[QStringLiteral("statePill")] =
            (m_reviewing || m_closed)
                ? (hereState == QLatin1String("fired") ? QStringLiteral("FIRED HERE")
                 : hereState == QLatin1String("clean") ? QStringLiteral("CLEAN HERE")
                                                       : QStringLiteral("NOT MEASURED"))
                : (hereState == QLatin1String("fired") ? QStringLiteral("FIRED")
                 : hereState == QLatin1String("clean") ? QStringLiteral("CLEAN")
                                                       : QStringLiteral("NOT MEASURED"));
        if (here && here->state != ShotState::NotAssessable) {
            c[QStringLiteral("valueText")]    = withUnit(fmtNumber(here->value), unit);
            c[QStringLiteral("corridorText")] = here->corridorHi > here->corridorLo
                ? QStringLiteral("pass %1 to %2")
                      .arg(fmtNumber(here->corridorLo), withUnit(fmtNumber(here->corridorHi), unit))
                : QStringLiteral("no corridor authored");
        } else {
            c[QStringLiteral("valueText")]    = QStringLiteral("not measurable");
            c[QStringLiteral("corridorText")] = here ? here->notAssessableReason : QString();
        }
        if (m_reviewing || m_closed) {
            int after = 0;
            for (int i = fi + 1; i < int(l->run.size()); ++i)
                if (l->run[size_t(i)] == ShotState::Fired) ++after;
            c[QStringLiteral("firingsAfter")] = after;
            c[QStringLiteral("firingsAfterText")] =
                after == 0 ? QStringLiteral("no firings after this shot")
                           : QStringLiteral("%1 more %2 after this shot")
                                 .arg(after).arg(after == 1 ? QStringLiteral("firing")
                                                            : QStringLiteral("firings"));
        }

        // ONE LINE OF EVIDENCE PROSE (§4.2). The contingency sentence when an authored parent
        // was actually tested — which is the design's own coach sentence — and the pack's
        // consequence line otherwise. Never both, and never a manufactured one.
        QString prose;
        for (const LinkEvidence &e : m_links) {
            if (e.to != id || !e.fisherTested) continue;
            prose = QStringLiteral("%1 on %2 of the %3 swings where %4 fired — on %5 of the %6 where it did not.")
                        .arg(conditionName(id))
                        .arg(e.a).arg(e.a + e.b)
                        .arg(conditionName(e.from))
                        .arg(e.c).arg(e.c + e.d);
            break;
        }
        c[QStringLiteral("evidence")] = prose.isEmpty() ? consequenceOf(id) : prose;

        m_cards.append(c);
    }

    // The collapsed Watching row: seen, not yet evidence, never headlines.
    for (const ConditionLedger &l : m_ledgers) {
        if (l.tier != Tier::Watching) continue;
        m_watching.append(QVariantMap{
            { QStringLiteral("id"),         l.id },
            { QStringLiteral("name"),       conditionName(l.id) },
            { QStringLiteral("recurrence"), l.recurrence },
        });
    }

    // The coverage line — the quietest and most load-bearing sentence on the panel.
    if (m_coverage.total > 0) {
        m_coverageLine = QStringLiteral("%1 of %2 characteristics measurable with current capture")
                             .arg(m_coverage.measurable).arg(m_coverage.total);
        // Named only when it actually contributed. "Launch monitor" on a camera-only session
        // would be the panel claiming a device that is not there.
        if (!m_lmShots.isEmpty())
            m_coverageLine += QStringLiteral(" · launch monitor contributing on %1 of %2 shots")
                                  .arg(m_lmShots.size()).arg(int(m_shots.size()));
    }
}

// ── Zone 2: the chain rail ──────────────────────────────────────────────────────────────

void SessionDiagnosticsModel::buildChains()
{
    m_chains.clear();
    m_unchained.clear();
    m_unchainedLine.clear();

    const int fi = focusIndex();
    const int selectedTick = (m_reviewing || m_closed) ? fi : -1;

    for (const Chain &chain : m_rails.chains) {
        QVariantList nodes, links;
        for (size_t k = 0; k < chain.nodes.size(); ++k) {
            const ChainNode &cn = chain.nodes[k];
            const ConditionLedger *l = ledger(cn.id);
            const NodeSpec *ns = nodeFor(m_nodes, cn.id);

            QVariantMap n;
            n[QStringLiteral("id")]      = cn.id;
            n[QStringLiteral("name")]    = conditionName(cn.id);
            n[QStringLiteral("kind")]    = chainNodeKindName(cn.kind);
            n[QStringLiteral("measure")] = measureLabelOf(l ? l->drivingMeasureId : QString());
            n[QStringLiteral("phase")]   = measurePhaseOf(l ? l->drivingMeasureId : QString());
            n[QStringLiteral("focused")] = cn.id == m_focusConditionId;

            // THE MARK IS THE HONESTY DEVICE. Each of the four says a different thing and
            // they must not be blurred: a screened root is not a ghost, and a ghost the pack
            // ASSERTS is not a ghost nobody has written a measure for.
            switch (cn.kind) {
            case ChainNodeKind::ScreenedRoot:
                n[QStringLiteral("mark")] = ns && ns->screenEntered
                                                ? QStringLiteral("screened root · screen entered")
                                                : QStringLiteral("screened root · needs a physical screen");
                break;
            case ChainNodeKind::Ghost:
                n[QStringLiteral("mark")] = (ns && ns->asserted)
                                                ? QStringLiteral("ghost · authored as asserted, no measure")
                                                : QStringLiteral("ghost · measure planned");
                break;
            case ChainNodeKind::Outcome:
                n[QStringLiteral("mark")] = m_lmShots.isEmpty()
                                                ? QStringLiteral("declared miss")
                                                : QStringLiteral("declared miss · launch-monitor verified");
                break;
            case ChainNodeKind::LiveCard:
                n[QStringLiteral("mark")] = QString();
                break;
            }

            if (l) {
                n[QStringLiteral("recurrence")] = l->recurrence;
                n[QStringLiteral("ticks")]      = ticksFor(*l, selectedTick);
                n[QStringLiteral("trend")]      = l->trendKnown
                                                     ? (l->trend == Trend::Improving
                                                            ? QStringLiteral("improving")
                                                            : QStringLiteral("worsening"))
                                                     : QStringLiteral("stable");
                n[QStringLiteral("trendArrow")] = l->trendKnown
                                                     ? (l->trend == Trend::Improving ? QStringLiteral("↓")
                                                                                     : QStringLiteral("↑"))
                                                     : QString();
                const ConditionRow *here = (fi >= 0) ? rowFor(m_shots[size_t(fi)], cn.id) : nullptr;
                const QString hs = here ? shotStateKind(here->state) : QStringLiteral("notAssessable");
                n[QStringLiteral("state")] = hs;
                n[QStringLiteral("statePill")] =
                    (m_reviewing || m_closed)
                        ? (hs == QLatin1String("fired") ? QStringLiteral("FIRED HERE")
                         : hs == QLatin1String("clean") ? QStringLiteral("CLEAN HERE")
                                                        : QStringLiteral("NOT MEASURED"))
                        : (hs == QLatin1String("fired") ? QStringLiteral("FIRED")
                         : hs == QLatin1String("clean") ? QStringLiteral("CLEAN")
                                                        : QStringLiteral("NOT MEASURED"));
            } else {
                // A node with no ledger was never assessed at all — no recurrence, no ticks,
                // and the pill says so rather than showing an empty count.
                n[QStringLiteral("recurrence")] = QString();
                n[QStringLiteral("ticks")]      = QVariantList();
                n[QStringLiteral("state")]      = QStringLiteral("notAssessable");
                n[QStringLiteral("statePill")]  = QStringLiteral("NOT MEASURED");
                n[QStringLiteral("trend")]      = QStringLiteral("stable");
                n[QStringLiteral("trendArrow")] = QString();
            }

            QString prose;
            for (const LinkEvidence &e : m_links) {
                if (e.to != cn.id || !e.fisherTested) continue;
                prose = QStringLiteral("%1 on %2 of the %3 swings where %4 fired — on %5 of the %6 where it did not.")
                            .arg(conditionName(cn.id))
                            .arg(e.a).arg(e.a + e.b)
                            .arg(conditionName(e.from))
                            .arg(e.c).arg(e.c + e.d);
                break;
            }
            n[QStringLiteral("evidence")] = prose.isEmpty() ? consequenceOf(cn.id) : prose;
            nodes.append(n);

            if (!cn.hasLink) continue;
            const LinkEvidence &e = cn.link;
            const LinkStyle     st = styleOf(e.grade);

            // THE NOTE UNDER THE LINK. The brief fixes four of these; the fifth (a Coherent
            // link that is NOT range restricted) it does not, because in the mock every
            // Coherent link happens to be range restricted. Saying "no variance to test"
            // there would be a claim about the data that is simply untrue, so the honest
            // answer is the count that stopped the test.
            QString note;
            switch (e.grade) {
            case LinkGrade::MovedTogether:
                note = QStringLiteral("baseline %1 · %2 since focus").arg(e.baselineN).arg(e.interventionN);
                break;
            case LinkGrade::ConditionallyDependent:
                note = QStringLiteral("Fisher exact · %1 paired shots").arg(e.pairs);
                break;
            case LinkGrade::Coherent:
                note = e.rangeRestricted
                           ? QStringLiteral("no variance to test")
                           : (e.pairs < m_opt.minPairsForDependence
                                  ? QStringLiteral("%1 paired shots · too few to test").arg(e.pairs)
                                  : QStringLiteral("tested · not dependent"));
                break;
            case LinkGrade::PresentTogether: {
                const ConditionLedger *lf = ledger(e.from);
                const ConditionLedger *lt = ledger(e.to);
                const bool observedBoth = lf && lt && lf->assessable >= 1 && lt->assessable >= 1;
                note = observedBoth ? QStringLiteral("no coherent order")
                                    : QStringLiteral("neither measured");
                break;
            }
            case LinkGrade::Unanchored:
                note = e.screenOutstanding ? QStringLiteral("screen would confirm")
                                           : QStringLiteral("not established");
                break;
            }

            links.append(QVariantMap{
                { QStringLiteral("from"),    e.from },
                { QStringLiteral("to"),      e.to },
                { QStringLiteral("grade"),   gradeName(e.grade) },
                { QStringLiteral("word"),    QString::fromLatin1(st.word) },
                { QStringLiteral("stroke"),  QString::fromLatin1(st.stroke) },
                { QStringLiteral("width"),   st.width },
                { QStringLiteral("arrow"),   st.arrow },
                { QStringLiteral("opacity"), st.opacity },
                { QStringLiteral("note"),    note },
                { QStringLiteral("pairs"),   e.pairs },
            });
        }

        int live = 0;
        for (const ChainNode &cn : chain.nodes)
            if (cn.kind == ChainNodeKind::LiveCard) ++live;

        m_chains.append(QVariantMap{
            { QStringLiteral("nodes"),     nodes },
            { QStringLiteral("links"),     links },
            { QStringLiteral("anyLive"),   chain.anyLive },
            { QStringLiteral("liveCount"), live },
        });
    }

    // MOST-EVIDENCED RAIL FIRST, and the ordering is decided here rather than left to the
    // panel. A session's authored neighbourhood can yield more rails than a 1168 px panel can
    // draw — this one yields eighteen over six shots — so something has to choose, and a
    // panel that took "the first few" off an arbitrary order would be showing a different
    // chain each launch. The key is the count of LIVE cards (how much of this rail the
    // session actually measured), then length, then the first node's id so ties are stable.
    // How many to draw, and what the "more" affordance is, remains the panel's decision.
    std::sort(m_chains.begin(), m_chains.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const int lx = x.value(QStringLiteral("liveCount")).toInt();
        const int ly = y.value(QStringLiteral("liveCount")).toInt();
        if (lx != ly) return lx > ly;
        const QVariantList nx = x.value(QStringLiteral("nodes")).toList();
        const QVariantList ny = y.value(QStringLiteral("nodes")).toList();
        if (nx.size() != ny.size()) return nx.size() > ny.size();
        const QString ix = nx.isEmpty() ? QString() : nx.first().toMap().value(QStringLiteral("id")).toString();
        const QString iy = ny.isEmpty() ? QString() : ny.first().toMap().value(QStringLiteral("id")).toString();
        return ix < iy;
    });

    // A pattern with no authored edge to any other pattern gets its OWN line — reported,
    // never forced onto a chain to tidy the picture (brief §5.3).
    QStringList names;
    for (const QString &id : m_rails.unchainedPatterns) {
        const ConditionLedger *l = ledger(id);
        m_unchained.append(QVariantMap{
            { QStringLiteral("id"),         id },
            { QStringLiteral("name"),       conditionName(id) },
            { QStringLiteral("recurrence"), l ? l->recurrence : QString() },
        });
        names << conditionName(id);
    }
    if (!names.isEmpty()) {
        m_unchainedLine =
            names.size() == 1
                ? QStringLiteral("%1 is a pattern the model authors no edge for this session — "
                                 "reported on its own.").arg(names.first())
                : QStringLiteral("%1 are patterns the model authors no edge between this session — "
                                 "reported on their own.").arg(names.join(QStringLiteral(", ")));
    }
}

// ── Zone 3: the driver footer ───────────────────────────────────────────────────────────

void SessionDiagnosticsModel::buildDriver()
{
    m_driver = QVariantMap();

    // Debounced by pattern-set stability. Better absent than flickering (§B8) — and the
    // eligibility test lives in the ledger header so the footer and a replay agree.
    const bool eligible = driverFooterEligible(m_shots, m_opt);
    m_driver[QStringLiteral("eligible")] = eligible;
    if (!eligible || m_explanation.roots.empty()) {
        m_driver[QStringLiteral("waitingText")] =
            QStringLiteral("the driver appears once the pattern set has held still for %1 shots")
                .arg(m_opt.driverDebounceShots);
        return;
    }

    const RankedCause &top = m_explanation.roots.front();
    m_driver[QStringLiteral("rootId")]   = top.conditionId;
    m_driver[QStringLiteral("rootName")] = conditionName(top.conditionId);
    m_driver[QStringLiteral("coverage")] = top.coverage;
    // The one sentence that says WHY it is the driver, and it is a count of what it accounts
    // for rather than a score — the score is ordinal and publishing it would imply a
    // probability it is not.
    m_driver[QStringLiteral("whyText")] =
        QStringLiteral("%1 — would explain %2 of your %3")
            .arg(conditionName(top.conditionId))
            .arg(top.coverage)
            .arg(top.coverage == 1 ? QStringLiteral("pattern") : QStringLiteral("patterns"));

    QVariantList explains;
    for (const QString &id : top.explains)
        explains.append(QVariantMap{ { QStringLiteral("id"), id },
                                     { QStringLiteral("name"), conditionName(id) } });
    m_driver[QStringLiteral("explains")] = explains;

    // THE SCREEN CTA, with its reason. The highest-value output of the whole model and the
    // only one that costs no hardware: a thirty-second physical test that anchors a chain.
    if (!m_explanation.recommendations.empty()) {
        const TestRecommendation &rec = m_explanation.recommendations.front();
        const Screen *sc = sharedScreenSet().screen(rec.screenRef);
        m_driver[QStringLiteral("screenConditionId")] = rec.conditionId;
        m_driver[QStringLiteral("screenRef")]         = rec.screenRef;
        m_driver[QStringLiteral("screenLabel")]       = sc ? sc->label : rec.screenRef;
        m_driver[QStringLiteral("screenCta")] =
            QStringLiteral("%1 would anchor this chain").arg(sc ? sc->label : conditionName(rec.conditionId));
        m_driver[QStringLiteral("screenReason")] =
            QStringLiteral("it would explain %1 of your %2")
                .arg(rec.coverage)
                .arg(rec.coverage == 1 ? QStringLiteral("pattern") : QStringLiteral("patterns"));
        m_driver[QStringLiteral("screenProtocol")] = sc ? sc->protocol : QString();
    }

    // THE RIVAL-PARENT DISCLOSURE. Named, and explicitly NOT adjudicated when the rival is
    // unmeasurable or under-paired — the panel shows the most consistent chain and never
    // asserts uniqueness (§A4).
    if (!m_rails.rivals.empty()) {
        const RivalParent &rp = m_rails.rivals.front();
        m_driver[QStringLiteral("rival")] = QVariantMap{
            { QStringLiteral("childId"),        rp.childId },
            { QStringLiteral("childName"),      conditionName(rp.childId) },
            { QStringLiteral("chosenParentId"), rp.chosenParentId },
            { QStringLiteral("chosenName"),     conditionName(rp.chosenParentId) },
            { QStringLiteral("rivalParentId"),  rp.rivalParentId },
            { QStringLiteral("rivalName"),      conditionName(rp.rivalParentId) },
            { QStringLiteral("adjudicated"),    rp.adjudicated },
            { QStringLiteral("text"),
              rp.adjudicated
                  ? QStringLiteral("%1 could also explain %2; this session's evidence favours %3.")
                        .arg(conditionName(rp.rivalParentId), conditionName(rp.childId),
                             conditionName(rp.chosenParentId))
                  : QStringLiteral("%1 could also explain %2 — not adjudicated: it is not measurable "
                                   "on this capture.")
                        .arg(conditionName(rp.rivalParentId), conditionName(rp.childId)) },
        };
    }

    // Asserted causes are OFFERED and phrased as questions, never concluded (relation_resolver
    // rule 2). The question mark is not decoration — it is the whole epistemic status.
    QVariantList offered;
    for (const RankedCause &o : m_explanation.offered) {
        offered.append(QVariantMap{
            { QStringLiteral("id"),   o.conditionId },
            { QStringLiteral("name"), conditionName(o.conditionId) },
            { QStringLiteral("question"),
              QStringLiteral("%1?").arg(conditionName(o.conditionId)) },
            { QStringLiteral("coverage"), o.coverage },
        });
    }
    m_driver[QStringLiteral("offered")] = offered;
}

// ── Closing: session bookends (§B7) ─────────────────────────────────────────────────────

void SessionDiagnosticsModel::buildBookends()
{
    m_bookends.clear();
    if (effectiveStage() != Stage::Closing) return;   // the closing strip replaces this-shot

    for (const ConditionLedger &l : m_ledgers) {
        if (l.tier != Tier::Pattern) continue;
        const Bookends bk = conditionBookends(m_shots, l.id);
        if (!bk.has) continue;
        m_bookends.append(QVariantMap{
            { QStringLiteral("id"),   l.id },
            { QStringLiteral("name"), conditionName(l.id) },
            { QStringLiteral("worstShotId"),  bk.worstShotId },
            { QStringLiteral("bestShotId"),   bk.bestShotId },
            { QStringLiteral("representativeShotId"), bk.representativeShotId },
            { QStringLiteral("worstText"),
              QStringLiteral("worst · shot %1").arg(bk.worstShotId) },
            { QStringLiteral("bestText"),
              QStringLiteral("best · shot %1").arg(bk.bestShotId) },
            // The one that is the point: shown their most representative swing, a golfer
            // learns what their swing IS rather than what its range is.
            { QStringLiteral("representativeText"),
              QStringLiteral("most representative · shot %1").arg(bk.representativeShotId) },
        });
    }
}

// ── Review payloads (brief §6 / option 13a) ─────────────────────────────────────────────

QVariantMap SessionDiagnosticsModel::shotReadout(int shotId) const
{
    const int idx = indexOfShot(shotId);
    if (idx < 0) return QVariantMap();
    const ShotRecord &shot = m_shots[size_t(idx)];

    int fired = 0, clean = 0, na = 0, firedPatterns = 0;
    QVariantList conditions;

    // The POPULATION is the session's measurable set — conditions this capture answered at
    // least once. Listing all ~150 pack conditions would bury the nine that mean something
    // under a hundred and forty rows of "not measurable", which is not honesty, it is noise
    // wearing honesty's clothes. The coverage line is where the other hundred and forty are
    // accounted for, in one sentence, as the design intends.
    QStringList ordered = m_displayOrder;
    for (const ConditionLedger &l : m_ledgers)
        if (l.assessable >= 1 && !ordered.contains(l.id)) ordered.push_back(l.id);

    for (const QString &id : std::as_const(ordered)) {
        const ConditionLedger *l = ledger(id);
        if (!l || l->assessable < 1) continue;

        const ConditionRow *r = rowFor(shot, id);
        const ShotState st = r ? r->state : ShotState::NotAssessable;
        if (st == ShotState::Fired) {
            ++fired;
            if (l->tier == Tier::Pattern) ++firedPatterns;
        } else if (st == ShotState::Clean) {
            ++clean;
        } else {
            ++na;
        }

        const QString unit = measureUnitOf(l->drivingMeasureId);
        int after = 0;
        for (int i = idx + 1; i < int(l->run.size()); ++i)
            if (l->run[size_t(i)] == ShotState::Fired) ++after;

        QVariantMap c;
        c[QStringLiteral("id")]        = id;
        c[QStringLiteral("name")]      = conditionName(id);
        c[QStringLiteral("stateKind")] = shotStateKind(st);
        // OUT / IN / — in the state colour. The em dash is the third state and it is not a
        // blank: "we did not look" is a fact the golfer is owed.
        c[QStringLiteral("state")] = st == ShotState::Fired ? QStringLiteral("OUT")
                                   : st == ShotState::Clean ? QStringLiteral("IN")
                                                            : QStringLiteral("—");
        if (st != ShotState::NotAssessable && r) {
            c[QStringLiteral("valueText")] = withUnit(fmtNumber(r->value), unit);
            c[QStringLiteral("corridorText")] =
                r->corridorHi > r->corridorLo
                    ? QStringLiteral("pass %1 to %2")
                          .arg(fmtNumber(r->corridorLo), withUnit(fmtNumber(r->corridorHi), unit))
                    : QStringLiteral("no corridor authored");
            c[QStringLiteral("reason")] = QString();
        } else {
            // Never a blank in either slot: the value reads "not measurable" and the corridor
            // slot carries THE REASON. A blank there reads as a bug.
            c[QStringLiteral("valueText")]    = QStringLiteral("not measurable");
            c[QStringLiteral("corridorText")] = r ? r->notAssessableReason
                                                  : missingReasonText(MissingKind::MetricNotProduced);
            c[QStringLiteral("reason")]       = c[QStringLiteral("corridorText")];
        }
        c[QStringLiteral("tierTag")]      = tierTag(l->tier);
        c[QStringLiteral("recurrence")]   = l->recurrence;
        c[QStringLiteral("ticks")]        = ticksFor(*l, idx);
        c[QStringLiteral("selectedIndex")]= idx;
        c[QStringLiteral("firingsAfter")] = after;
        c[QStringLiteral("firingsAfterText")] =
            after == 0 ? QStringLiteral("no firings after this shot")
                       : QStringLiteral("%1 more %2 after this shot")
                             .arg(after).arg(after == 1 ? QStringLiteral("firing")
                                                        : QStringLiteral("firings"));
        conditions.append(c);
    }

    QVariantMap out;
    out[QStringLiteral("shotId")]     = shotId;
    out[QStringLiteral("shotIndex")]  = idx;
    out[QStringLiteral("shotCount")]  = int(m_shots.size());
    out[QStringLiteral("club")]       = shot.club;
    out[QStringLiteral("contextId")]  = shot.contextId;
    out[QStringLiteral("firedCount")] = fired;
    out[QStringLiteral("cleanCount")] = clean;
    out[QStringLiteral("notAssessableCount")] = na;
    out[QStringLiteral("headline")] =
        QStringLiteral("%1 of %2 conditions fired on this swing · %3 of them %4")
            .arg(fired).arg(fired + clean).arg(firedPatterns)
            .arg(firedPatterns == 1 ? QStringLiteral("pattern") : QStringLiteral("patterns"));
    out[QStringLiteral("note")] =
        QStringLiteral("%1 %2 not assessable on this capture")
            .arg(na).arg(na == 1 ? QStringLiteral("measure") : QStringLiteral("measures"));
    out[QStringLiteral("conditions")] = conditions;
    return out;
}

QVariantList SessionDiagnosticsModel::pipsFor(int shotId) const
{
    QVariantList out;
    const int idx = indexOfShot(shotId);
    if (idx < 0) return out;
    const ShotRecord &shot = m_shots[size_t(idx)];

    // One pip per TRACKED condition, in the same order and over the same population as the
    // review strip — the pip row and the strip are the same nine facts at two sizes, and a
    // carousel that ordered them differently would make them impossible to read together.
    QStringList ordered = m_displayOrder;
    for (const ConditionLedger &l : m_ledgers)
        if (l.assessable >= 1 && !ordered.contains(l.id)) ordered.push_back(l.id);

    for (const QString &id : std::as_const(ordered)) {
        const ConditionLedger *l = ledger(id);
        if (!l || l->assessable < 1) continue;
        const ConditionRow *r = rowFor(shot, id);
        out.append(QVariantMap{
            { QStringLiteral("id"),    id },
            { QStringLiteral("state"), shotStateKind(r ? r->state : ShotState::NotAssessable) },
        });
    }
    return out;
}

int SessionDiagnosticsModel::firedCountFor(int shotId) const
{
    const int idx = indexOfShot(shotId);
    if (idx < 0) return 0;
    int n = 0;
    for (const ConditionRow &r : m_shots[size_t(idx)].rows)
        if (r.state == ShotState::Fired) ++n;
    return n;
}
