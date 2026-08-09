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

#pragma once

#include "../../Analysis/diagnostic_ledger.h"
#include "../../Diagnostics/characteristic_engine.h"
#include "../../Diagnostics/relation_resolver.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

namespace pinpoint::analysis {
class ICharacteristicPackProvider;
class INormProvider;
}

// THE QML FAÇADE for PpSessionDiagnosticsPanel — the one object between a recorded swing and
// the panel that says what keeps happening.
//
// It is a JOIN and an ORCHESTRATOR, and deliberately not a second place where diagnostics
// arithmetic lives. Everything it publishes is one of three things:
//
//   · a reduction from `src/Analysis/diagnostic_ledger.h` (every tier, recurrence, trend,
//     link grade, chain, stage, bookend and anti-churn ordering),
//   · a fact from the authored pack (`src/Diagnostics`: names, consequences, screens,
//     causal edges, the explanation pass), or
//   · a STRING assembled from the two, here, because "QML positions and paints, C++ decides
//     the numbers" (analysis pipeline guide §6.2) means the panel gets sentences and not
//     ingredients. There is no arithmetic in this file that diagnostic_ledger.h could have
//     done, and no wording in the QML that this file could have written.
//
// INSTANTIATED BY THE PANEL, not registered in main.cpp as a context property — the same
// decision LmSessionModel documents and for the same reason, sharpened. The session
// diagnostics panel is OFF by default (the user turns it on in View), and this model owns a
// characteristic pack, a norm provider and a worker thread that re-runs detection on every
// shot. A golfer who never opens the panel should not be paying for any of it, and a model
// that reached for the singletons itself would additionally be untestable — which matters
// more here than anywhere, because the thing under test is a claim about a golfer.
//
// ── The five decisions worth knowing before reading the .cpp ────────────────────────────
//
// 1. THREADING. ingestShot() returns immediately. Detection reads swing.json, which is up to
//    tens of megabytes, and doing that on the GUI thread would stall the app in the ten
//    seconds between balls — exactly the moment the panel exists to serve. Work runs on a
//    private single-thread QThreadPool (one thread, so shots reduce in the order they were
//    struck) and the finished ShotRecord is delivered back by a QUEUED invocation, so every
//    mutation of the row vector and every signal emission happens on the thread that owns
//    this object. setSynchronous(true) collapses that to an inline call for tests, and is
//    the only concession the design makes to being tested.
//
// 2. WHAT IS PERSISTED. diagnostics.json holds the ROWS and the intent, never the verdicts —
//    diagnostic_ledger.h's serialisation contract, unchanged, wrapped in a thin envelope
//    (schema tag, session meta, focus contract, declared miss, screen answers, which shots
//    carried launch-monitor data). Review mode re-reduces and gets the identical panel by
//    construction, and a gate that moves in a later build re-grades an old session honestly
//    rather than quoting a stale verdict. Written atomically after every ingest, so a crash
//    mid-session costs at most the shot in flight, and activateSession() reconciles what is
//    on disk against the swing_* directories beside it and back-fills the difference —
//    which is also what makes the panel safe to enable half way through a session.
//
// 3. WHAT THE FAULT PROFILE MAY TOUCH. `fault_profile.json` lives in the ATHLETE folder (the
//    session directory's parent) and is read at activation and written at close. It reaches
//    exactly two things: the Cold state's `expectations` list, and the `score` fed to
//    hystereticOrder(). It is structurally incapable of reaching a tier, a corridor or a
//    firing, because the only function that reads it — profileBias() — is called from
//    rankScores() and from nowhere else, and LedgerOptions has no field it could be poured
//    into. That is brief §5.5 made a property of the call graph rather than a promise.
//
// 4. WHEN explain() RUNS. Over the PATTERN-TIER set only, and only when that set's
//    membership changes (design §A5) — per-shot re-ranking would churn the resolver's greedy
//    set cover on every ball and reorder the footer under the golfer's eyes. The footer is
//    then debounced a second time, by kDiagDriverDebounceShots, so it appears only once the
//    pattern set has held still: better absent than flickering (§B8).
//
// 5. WHAT CADENCE GATES. `quiet` and `afterShotDelta`. Nothing else, ever. ingestShot() does
//    not consult it, the ledger does not see it, and the persisted rows are byte-identical
//    under both modes — brief §5.6. A cadence check anywhere near the ingest path is the
//    single most likely way for this panel to start lying, so there is exactly one place in
//    the .cpp where m_cadence is read and it is named surfaceThisShot().
//
// The panel comes in a later phase; every property below is already the finished shape it
// will bind to. Unit-tested standalone in
// src/Diagnostics/tests/session_diagnostics_model_test.cpp.
class SessionDiagnosticsModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // ── Inputs the panel binds, passed in rather than reached for ────────────────────
    //
    // Same rule LmSessionModel states for `gradePolicy` and `connected`: the panel already
    // has `appSettings` in scope, and a model that read the singleton itself would couple
    // the panel to main.cpp and could not be constructed in a test. Bind `cadence` to
    // AppSettings::sessionDiagnosticsCadence and `gradePolicy` to
    // AppSettings::diagnosticsGradePolicy — the latter must be the SAME policy the dashboard
    // grades against, or one reading would sit outside its corridor on one panel and inside
    // it on another.
    Q_PROPERTY(QString cadence READ cadence WRITE setCadence NOTIFY cadenceChanged)
    Q_PROPERTY(QString gradePolicy READ gradePolicy WRITE setGradePolicy NOTIFY gradePolicyChanged)

    // Whether the shots being reduced are a LOADED session's rather than the live one's —
    // LmSessionModel's `reviewing`, with one addition: review also FREEZES the ledger at
    // Closing, because a finished session's panel is its summary and must not re-open
    // because it was looked at.
    Q_PROPERTY(bool reviewing READ reviewing WRITE setReviewing NOTIFY reviewingChanged)

    // The shot the stage carousel has focused. -1 for "none". The panel reads the carousel;
    // it never owns a selection of its own (brief §8).
    Q_PROPERTY(int selectedShotId READ selectedShotId WRITE setSelectedShotId NOTIFY selectedShotIdChanged)

    // ── Session identity and lifecycle ──────────────────────────────────────────────
    Q_PROPERTY(QString sessionDir READ sessionDir NOTIFY sessionChanged)
    // "cold" | "forming" | "established" | "closing". Ratcheted one way within a session.
    Q_PROPERTY(QString stage READ stage NOTIFY surfaceChanged)
    Q_PROPERTY(bool closed READ closed NOTIFY surfaceChanged)
    Q_PROPERTY(int shotCount READ shotCount NOTIFY surfaceChanged)
    Q_PROPERTY(int patternCount READ patternCount NOTIFY surfaceChanged)
    // A detection is in flight. The panel shows the previous state while it is true — never
    // an empty one, because a blank panel between balls reads as "nothing is wrong".
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    // ── Session intent (design §A6) ─────────────────────────────────────────────────
    //
    // Both declarations shape ATTENTION and EXPERIMENT STRUCTURE and never evidence. The
    // focus contract supplies the FocusSplit that lets a link earn Moved-together honestly;
    // the declared miss pre-arms the chains upstream of the outcome. Neither is an input to
    // detection, to a corridor, or to a tier — asserted by test.
    Q_PROPERTY(QString focusConditionId READ focusConditionId NOTIFY intentChanged)
    Q_PROPERTY(int focusFromShot READ focusFromShot NOTIFY intentChanged)
    Q_PROPERTY(QString declaredMiss READ declaredMiss NOTIFY intentChanged)
    // The declared miss's authored label, so the panel's header chip can name it without
    // resolving an id against a pack it does not have. Empty when nothing is declared.
    Q_PROPERTY(QString declaredMissName READ declaredMissName NOTIFY intentChanged)
    // Everything authored upstream of the declared miss — the chains it pre-arms. Names and
    // ids, so the panel can badge a card as "on the way to your declared miss".
    Q_PROPERTY(QVariantList missChain READ missChain NOTIFY intentChanged)

    // ── The zones (design §B2, brief §4) ────────────────────────────────────────────
    //
    // One property per region of the panel, each already carrying its final strings. The
    // QML binds and positions; it computes nothing.
    Q_PROPERTY(QVariantMap headerInfo READ headerInfo NOTIFY surfaceChanged)
    Q_PROPERTY(QVariantList thisShot READ thisShot NOTIFY surfaceChanged)
    // Cadence's ONLY two outputs.
    Q_PROPERTY(bool quiet READ quiet NOTIFY surfaceChanged)
    Q_PROPERTY(QVariantMap afterShotDelta READ afterShotDelta NOTIFY surfaceChanged)
    Q_PROPERTY(QVariantList cards READ cards NOTIFY surfaceChanged)
    Q_PROPERTY(QVariantList watching READ watching NOTIFY surfaceChanged)
    Q_PROPERTY(QVariantList chains READ chains NOTIFY surfaceChanged)
    Q_PROPERTY(QVariantList unchained READ unchained NOTIFY surfaceChanged)
    Q_PROPERTY(QString unchainedLine READ unchainedLine NOTIFY surfaceChanged)
    Q_PROPERTY(QVariantMap driver READ driver NOTIFY surfaceChanged)
    Q_PROPERTY(QString coverageLine READ coverageLine NOTIFY surfaceChanged)
    // Closing only; empty otherwise.
    Q_PROPERTY(QVariantList bookends READ bookends NOTIFY surfaceChanged)
    // Cold only; empty otherwise. Expectations to TEST, never findings — the wording says so
    // and the panel must keep saying so.
    Q_PROPERTY(QVariantList expectations READ expectations NOTIFY surfaceChanged)

    // ── The condition detail (user-requested drill-in) ──────────────────────────────
    //
    // DESIGN-REVIEW: brief §9 lists "tapping a chain node through" as not designed. This is the
    // minimum honest answer to the request and it wants a design pass — see
    // docs/design/session_diagnostics_build_findings.md.
    //
    // A PROPERTY PAIR RATHER THAN AN INVOKABLE THE PANEL RE-CALLS, and the reason is the same one
    // that made shotReadout() an invokable: shotReadout answers a question about a shot the
    // CAROUSEL owns, so somebody outside this object has to say when to ask it again. The detail
    // answers a question about a condition THIS object owns the answer to, and its answer changes
    // on exactly the events the zones' answers change on — a shot lands, a screen is entered, the
    // reviewed shot moves. Published as a property, the detail is live by construction and a panel
    // that opened one mid-session watches it accumulate; published as an invokable it would need a
    // second re-fetch path in the panel, and a detail one shot stale is a detail that is lying.
    //
    // ITS OWN SIGNAL, and not surfaceChanged, because it is a projection of ONE MORE INPUT — which
    // condition is open — that no zone reads. Everything the zones share one signal for is one
    // projection of one reduction; this is that reduction seen through a second question.
    Q_PROPERTY(QString detailConditionId READ detailConditionId NOTIFY detailChanged)
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY detailChanged)

public:
    explicit SessionDiagnosticsModel(QObject *parent = nullptr);
    ~SessionDiagnosticsModel() override;

    QString cadence() const { return m_cadence; }
    void    setCadence(const QString &mode);
    QString gradePolicy() const { return m_policyName; }
    void    setGradePolicy(const QString &name);
    bool    reviewing() const { return m_reviewing; }
    void    setReviewing(bool on);
    int     selectedShotId() const { return m_selectedShotId; }
    void    setSelectedShotId(int id);

    QString sessionDir() const { return m_sessionDir; }
    QString stage() const;
    bool    closed() const { return m_closed; }
    int     shotCount() const { return int(m_shots.size()); }
    int     patternCount() const;
    bool    busy() const { return m_pending > 0; }

    QString focusConditionId() const { return m_focusConditionId; }
    int     focusFromShot() const { return m_focusFromShot; }
    QString declaredMiss() const { return m_declaredMiss; }
    QString declaredMissName() const
    { return m_declaredMiss.isEmpty() ? QString() : conditionName(m_declaredMiss); }
    QVariantList missChain() const { return m_missChain; }

    QVariantMap  headerInfo() const { return m_headerInfo; }
    QVariantList thisShot() const { return m_thisShot; }
    bool         quiet() const { return m_quiet; }
    QVariantMap  afterShotDelta() const { return m_afterShotDelta; }
    QVariantList cards() const { return m_cards; }
    QVariantList watching() const { return m_watching; }
    QVariantList chains() const { return m_chains; }
    QVariantList unchained() const { return m_unchained; }
    QString      unchainedLine() const { return m_unchainedLine; }
    QVariantMap  driver() const { return m_driver; }
    QString      coverageLine() const { return m_coverageLine; }
    QVariantList bookends() const { return m_bookends; }
    QVariantList expectations() const { return m_expectations; }
    QString      detailConditionId() const { return m_detailConditionId; }
    QVariantMap  detail() const { return m_detail; }

    // ── Session lifecycle ───────────────────────────────────────────────────────────

    // Point the model at a session directory: load any diagnostics.json already there,
    // reconcile it against the swing_* directories on disk, and back-fill the shots the
    // file does not have through the ordinary ingest path.
    //
    // This ONE call covers three situations that are the same problem: opening a finished
    // session for review, resuming a session the app crashed out of, and turning the panel
    // on half way through a live session. In all three the answer is "the rows on disk are
    // the truth, the swing directories are the truth about which shots exist, and the
    // difference is work to do".
    //
    // Also reads the athlete's fault profile from the PARENT directory (see the class
    // comment) and publishes it as `expectations`.
    Q_INVOKABLE void activateSession(const QString &sessionDir);

    // Reduce one shot into the ledger. IDEMPOTENT per shotId: a shot already ingested, or
    // already in flight, is ignored — which is what makes it safe to wire straight to
    // ShotProcessor::shotProcessed and to call again from a back-fill scan.
    //
    // NEVER GATED BY CADENCE (brief §5.6). Quiet is a statement about the panel, not about
    // the evidence.
    Q_INVOKABLE void ingestShot(int shotId, const QString &swingDir);

    // End the session: freeze at Closing, write the final diagnostics.json, and fold this
    // session's patterns into the athlete's fault profile. One-way — sessionStage() will not
    // un-close it, and neither will another shot.
    Q_INVOKABLE void closeSession();

    // ── Intent (design §A6) ─────────────────────────────────────────────────────────

    // "Working on X." Records the split point at the CURRENT shot count, so shots already
    // struck are baseline and everything after is intervention. Declaring a focus is the
    // only thing that puts the Moved-together grade on offer at all.
    Q_INVOKABLE void declareFocus(const QString &conditionId);
    Q_INVOKABLE void clearFocus();

    // The session's opening question — "what is the bad shot?". Pre-arms the chains upstream
    // of that outcome and marks it as the chain's outcome node. Verified against launch
    // monitor data where the session has any. An EMPTY id clears the declaration.
    Q_INVOKABLE void declareMiss(const QString &missId);

    // What the panel may offer as an answer to that question: every condition the pack
    // authors as an OUTCOME, as {id, name}, in pack order.
    //
    // THE CLASSIFICATION IS THE PACK'S KIND, not this file's opinion and not the ball-flight
    // GROUP. ConditionKind::Outcome is "what the ball did — where an explanation ENDS", it is
    // intrinsic (it never moves when a producer lands), and it is the same field
    // outcomeReachOf() asks when it counts what a condition reaches. The group is coextensive
    // with it over shipped content and core_pack_test asserts that in both directions, but
    // the group answers WHERE IN THE SWING and pressing it into this job is exactly the
    // conflation the kind axis was added to end. A pack that authors no kinds yields no
    // candidates, which is the honest answer: nothing there is marked as an outcome.
    Q_INVOKABLE QVariantList missCandidates() const;

    // A physical screen answered at the range counts immediately (design §A5). Feeds
    // explain()'s knownScreenResults and NodeSpec::screenEntered — which is what lets a
    // screened root stop anchoring nothing.
    Q_INVOKABLE void recordScreenResult(const QString &conditionId, bool present);

    // ── Review (brief §6, option 13a) ───────────────────────────────────────────────

    // The this-shot strip for a selected shot, read INSIDE the finished ledger: session
    // counts, session tiers, this shot's own reading. Empty map for a shot not in the ledger.
    Q_INVOKABLE QVariantMap shotReadout(int shotId) const;

    // The carousel cell's pip row — one pip per tracked condition for that shot.
    Q_INVOKABLE QVariantList pipsFor(int shotId) const;
    // The count that sits beside the pips, in the state colour.
    Q_INVOKABLE int firedCountFor(int shotId) const;

    // ── The condition detail ────────────────────────────────────────────────────────

    // What might have caused this condition, and what it most likely leads to, with this
    // session's evidence on every link. A PURE READ: it takes no lock on the panel's state,
    // consults no cadence, moves no ratchet, writes nothing to disk, and returns the same map in
    // live and in review for the same ledger. Empty for a condition the pack does not author.
    //
    // The map (every string authored here, exactly like the zones):
    //   header          one pattern-card map for the condition itself, plus `mark`/`kindText`
    //                   for a condition the capture never measured
    //   causeHeadline   the top-ranked cause in one sentence, or the honest absence of one
    //   outcomeHeadline the most likely outcome in one sentence, with its evidence
    //   causes          rails, most-evidenced first, upstream to the roots. `primary` on the first
    //   effects         rails, most-evidenced first, downstream to the outcomes
    //   rivals          parents this session could not adjudicate between — named, never ranked
    // Rails carry the SAME node and link maps the chain rail consumes, off the same helpers, so
    // the detail speaks the panel's existing visual language rather than a second dialect.
    Q_INVOKABLE QVariantMap conditionDetail(const QString &conditionId) const;

    // Open/close the published `detail`. Opening a different condition from inside the detail
    // RE-TARGETS it — there is no navigation stack, deliberately: one step back to the panel is
    // the whole of the model, and a stack would be a second place for "where am I" to be wrong.
    Q_INVOKABLE void openDetail(const QString &conditionId);
    Q_INVOKABLE void closeDetail();

    // ── Test seam ───────────────────────────────────────────────────────────────────
    //
    // Detection off the GUI thread is the right production shape and the wrong test shape:
    // a unit test asserting a stage progression should not also be asserting that an event
    // loop ran. This is the whole of the concession — the code path is otherwise identical.
    Q_INVOKABLE void setSynchronous(bool on) { m_synchronous = on; }
    Q_INVOKABLE bool synchronous() const { return m_synchronous; }
    // Spin the caller's event loop until every in-flight ingest has landed. Returns false on
    // timeout. A no-op in synchronous mode.
    Q_INVOKABLE bool waitForIdle(int msTimeout = 60000);

signals:
    void cadenceChanged();
    void gradePolicyChanged();
    void reviewingChanged();
    void selectedShotIdChanged();
    void sessionChanged();
    void intentChanged();
    void busyChanged();
    // Every derived surface, in one signal. The zones are ONE projection of ONE reduction
    // computed in rebuild(), exactly as LmSessionModel's tiles and graphics are — a signal
    // per zone would be a second chance for two regions of the same panel to disagree about
    // one shot.
    void surfaceChanged();
    // Which condition the detail is open on, and the detail itself. See the property block.
    void detailChanged();
    // One shot finished reducing. The panel's cue for the after-shot moment; carries whether
    // cadence decided to surface it, so the QML does not have to ask a second question.
    void shotIngested(int shotId, bool surfaced);

private:
    // ── The pipeline, in the order it runs ──────────────────────────────────────────

    // Worker-thread half: swing.json -> phase grid -> measures -> norms -> findings -> rows.
    // Touches no member except the pack and the norm provider, both of which are assembled
    // once in the constructor and read-only thereafter.
    struct Ingested {
        pinpoint::analysis::ShotRecord record;
        bool hasLaunchMonitor = false;
        bool ok = false;
    };
    Ingested detectShot(int shotId, const QString &swingDir) const;

    // GUI-thread half: append, re-reduce, decide the after-shot moment, persist, emit.
    void applyIngested(const Ingested &in);

    // Everything derived, from the rows and nothing else. Cheap enough to do wholesale —
    // tens of shots by ~150 conditions is arithmetic no profiler will find — so there are no
    // running totals to keep correct across a back-fill or a re-activation.
    void rebuild();

    // The pack's causal graph, marshalled into the ledger's pack-agnostic NodeSpec/EdgeSpec.
    // See the .cpp for WHICH conditions are marshalled and why it is not all of them.
    void buildGraph();

    // buildGraph()'s second half, on any node set: pack order in, NodeSpec/EdgeSpec out. Shared
    // with conditionDetail(), which needs the SAME marshalling over a different neighbourhood —
    // a detail's downstream reaches past the session's own graph, and a second marshaller would
    // be a second answer to "is this node measurable on this capture".
    void marshalGraph(const QSet<QString> &keep,
                      std::vector<pinpoint::analysis::NodeSpec> &nodes,
                      std::vector<pinpoint::analysis::EdgeSpec> &edges) const;

    // explain() over the pattern-tier set, run only when that set's membership changes.
    void refreshExplanation();

    // hystereticOrder()'s input. The ONLY function that reads the fault profile.
    std::vector<pinpoint::analysis::RankedCondition> rankScores() const;
    double profileBias(const QString &conditionId) const;

    // ── Persistence ─────────────────────────────────────────────────────────────────
    QString      diagnosticsPath() const;
    QString      faultProfilePath() const;
    QJsonObject  envelope() const;
    void         persist();
    void         loadProfile();
    void         updateProfile();

    // ── Surface builders (one per zone; all pure over the reduction) ────────────────
    void buildHeader();
    void buildThisShot();
    void buildCards();
    void buildChains();
    void buildDriver();
    void buildBookends();
    void buildDetail();

    // ── The card, node, link and rival maps — ONE marshaller each ───────────────────
    //
    // Extracted out of buildCards()/buildChains()/buildDriver() rather than copied into the
    // detail: the detail draws the same pattern card, the same node cards and the same graded
    // link strokes as the panel behind it, and two marshallers would be two chances for the same
    // link to read "Coherent" on one surface and "Present together" on the other. Everything they
    // publish is a function of (ledger, node spec, link evidence, focused shot) and nothing else.
    QVariantMap cardMap(const pinpoint::analysis::ConditionLedger &l,
                        int focusIdx, int selectedTick) const;
    QVariantMap nodeMap(const QString &id, const pinpoint::analysis::NodeSpec *ns,
                        pinpoint::analysis::ChainNodeKind kind,
                        const std::vector<pinpoint::analysis::LinkEvidence> &links,
                        int focusIdx, int selectedTick) const;
    QVariantMap linkMap(const pinpoint::analysis::LinkEvidence &e) const;
    QVariantMap rivalMap(const pinpoint::analysis::RivalParent &rp) const;
    // The node's honesty mark, in the model's words. Generalised past the declared miss: ANY
    // outcome-kind condition can be the end of a detail's downstream, and one whose rows the
    // launch monitor verified says so on the same terms the declared miss always has.
    QString markFor(const QString &id, pinpoint::analysis::ChainNodeKind kind,
                    const pinpoint::analysis::NodeSpec *ns) const;
    // One line of evidence prose: the contingency sentence where an authored parent was actually
    // tested, the pack's consequence line otherwise. Never both, never manufactured.
    QString evidenceProse(const QString &id,
                          const std::vector<pinpoint::analysis::LinkEvidence> &links) const;

    // ── Small shared formatters, so an export and the screen cannot disagree ────────
    QString conditionName(const QString &id) const;
    QString consequenceOf(const QString &id) const;
    QString measureLabelOf(const QString &measureId) const;
    QString measureUnitOf(const QString &measureId) const;
    QString measurePhaseOf(const QString &measureId) const;
    QVariantList ticksFor(const pinpoint::analysis::ConditionLedger &l, int selectedIndex) const;
    const pinpoint::analysis::ConditionLedger *ledger(const QString &id) const;
    int  indexOfShot(int shotId) const;
    // Which shot the panel is talking about: the selected one while reviewing, the newest
    // one live. One answer, in one place.
    int  focusIndex() const;
    // The stage the panel DRAWS. `m_stage` is the session's own ratcheted stage; reviewing a
    // session shows it frozen at Closing without being able to close it — see rebuild().
    pinpoint::analysis::Stage effectiveStage() const;

    // ── Owned registries ────────────────────────────────────────────────────────────
    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_packProv;
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    QString m_policyName = QStringLiteral("standard");

    // ── The evidence ────────────────────────────────────────────────────────────────
    std::vector<pinpoint::analysis::ShotRecord> m_shots;
    pinpoint::analysis::LedgerOptions           m_opt;
    QSet<int> m_ingested;      // shot ids already in the ledger or in flight
    QSet<int> m_lmShots;       // shot ids whose capture carried launch-monitor data
    QHash<int, QString> m_swingDirs;

    // ── The reduction ───────────────────────────────────────────────────────────────
    std::vector<pinpoint::analysis::ConditionLedger> m_ledgers;
    std::vector<pinpoint::analysis::NodeSpec>        m_nodes;
    std::vector<pinpoint::analysis::EdgeSpec>        m_edges;
    std::vector<pinpoint::analysis::LinkEvidence>    m_links;
    pinpoint::analysis::ChainRails                   m_rails;
    pinpoint::analysis::Coverage                     m_coverage;
    pinpoint::analysis::Explanation                  m_explanation;
    QStringList m_patternSet;      // the set explain() last ran over
    QStringList m_displayOrder;    // hystereticOrder()'s previous order — the anti-churn state

    // ── Lifecycle + intent ──────────────────────────────────────────────────────────
    QString m_sessionDir;
    pinpoint::analysis::Stage m_stage = pinpoint::analysis::Stage::Cold;
    // DID THIS SESSION EVER ACTUALLY ESTABLISH? `m_stage` cannot answer it once the session is
    // closed, because sessionStage() returns Closing outright for a closed session and the
    // Forming/Established distinction is lost with it. The panel needs the distinction: the
    // chain rail is the composition of a session that EARNED a chain, and a Forming session
    // reviewed at its close must keep the flat pattern cards it had (design 12a: "Forming,
    // flat cards, no chain because these two patterns share no authored edge"). Ratcheted the
    // same way the stage is, and persisted with it so reloading a closed session agrees.
    bool    m_reachedEstablished = false;
    bool    m_closed = false;
    bool    m_reviewing = false;
    int     m_selectedShotId = -1;
    QString m_cadence = QStringLiteral("bandwidth");
    QString m_focusConditionId;
    int     m_focusFromShot = -1;
    QString m_declaredMiss;
    QVariantList m_missChain;
    QHash<QString, bool> m_screens;

    // ── The fault profile. Read here; it reaches rankScores() and expectations(), and
    //    there is no third call site. See the class comment.
    QJsonObject  m_profile;
    QVariantList m_expectations;

    // ── Published surface ───────────────────────────────────────────────────────────
    QVariantMap  m_headerInfo;
    QVariantList m_thisShot;
    bool         m_quiet = false;
    QVariantMap  m_afterShotDelta;
    QVariantList m_cards;
    QVariantList m_watching;
    QVariantList m_chains;
    QVariantList m_unchained;
    QString      m_unchainedLine;
    QVariantMap  m_driver;
    QString      m_coverageLine;
    QVariantList m_bookends;
    // Which condition the drill-in is open on, and its map. Panel-local in every sense that
    // matters — never persisted, never in the envelope, cleared on activation: which page was
    // being read is not a fact about the session.
    QString      m_detailConditionId;
    QVariantMap  m_detail;

    // ── Threading ───────────────────────────────────────────────────────────────────
    QThreadPool m_pool;         // maxThreadCount 1: shots reduce in the order they were struck
    int         m_pending = 0;
    bool        m_synchronous = false;
};
