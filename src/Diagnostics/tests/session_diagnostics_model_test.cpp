// THE SESSION DIAGNOSTICS FAÇADE, RUN OVER REAL RECORDED SWINGS.
//
// diagnostic_ledger_test proves the arithmetic; live_measure_source_test proves the pack meets a
// real swing. Neither can fail if the ORCHESTRATION between them is wrong, and orchestration is
// where this panel's honesty actually lives: a cadence check in the ingest path, a fault profile
// leaking into a tier, an idempotence hole that counts one swing twice, a diagnostics.json that
// does not round-trip — every one of those produces a panel that looks entirely healthy and is
// lying about a golfer.
//
// So the assertions here are almost all CONTRACTS RATHER THAN VALUES:
//
//   · ingest is idempotent per shot id, whatever order the calls arrive in;
//   · the persisted rows re-reduce to a byte-identical surface (review mode is not a second
//     rendering path, it is the same one fed from disk);
//   · a cold activation over swing directories with no diagnostics.json back-fills the lot;
//   · cadence changes `quiet` and NOTHING that is stored — the two modes' ledgers are compared
//     byte for byte;
//   · the focus contract and the declared miss move no tier, no count and no corridor;
//   · the fault profile is written at close, read at the next activation, and moves no tier.
//
// The fixtures are live_measure_source_test's, staged into a temporary session so the model sees
// the swing-library shape it will see in the app (<athlete>/<session>/swing_NNNN/swing.json) — the
// fault profile lands in the ATHLETE folder, which only exists if the session has a parent.
//
//   cmake --build build/analysis-tests --target session_diagnostics_model_test
//   ctest --test-dir build/analysis-tests -R session_diagnostics_model --output-on-failure

#include "../../Gui/diagnostics/session_diagnostics_model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <memory>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

#ifndef PP_LIVE_SWINGS_DIR
#  define PP_LIVE_SWINGS_DIR "."
#endif

// ── Fixture staging ─────────────────────────────────────────────────────────────────────

// <root>/<athlete>/<session>/swing_000N/swing.json, copied out of the corpus fixtures. The
// two-deep shape is load-bearing: faultProfilePath() is the session's PARENT, so a flat
// staging would write the athlete's history inside one session and give it a sample size of one.
static QString makeSession(const QTemporaryDir &tmp, const char *athlete, const char *session)
{
    const QString dir = QDir(tmp.path()).filePath(QLatin1String(athlete) + QLatin1Char('/')
                                                  + QLatin1String(session));
    QDir().mkpath(dir);
    return dir;
}

static bool stageShot(const QString &sessionDir, int shotId, const char *fixture)
{
    const QString dst = QDir(sessionDir).filePath(
        QStringLiteral("swing_%1").arg(shotId, 4, 10, QLatin1Char('0')));
    if (!QDir().mkpath(dst)) return false;
    const QString src = QDir(QLatin1String(PP_LIVE_SWINGS_DIR))
                            .filePath(QLatin1String(fixture) + QLatin1String("/swing.json"));
    return QFile::copy(src, QDir(dst).filePath(QStringLiteral("swing.json")));
}

static QString swingDirFor(const QString &sessionDir, int shotId)
{
    return QDir(sessionDir).filePath(QStringLiteral("swing_%1").arg(shotId, 4, 10, QLatin1Char('0')));
}

// ── Snapshots, so "identical surface" is a comparison rather than a claim ────────────────

static QByteArray canon(const QVariant &v)
{
    return QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact);
}

// Sorted by `id`, because DISPLAY ORDER IS NOT PART OF THE CLAIM being checked. hystereticOrder()
// is deliberately path-dependent — a card sticks where it is unless its rank moves by more than
// the band — so a session built shot by shot and the same session rebuilt from disk in one pass
// can legitimately order two equally-ranked cards differently. Sorting compares what must not
// differ (every field of every card) without pinning what is allowed to.
static QVariantList byId(const QVariantList &in)
{
    QVariantList out = in;
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("id")).toString()
             < b.toMap().value(QStringLiteral("id")).toString();
    });
    return out;
}

// Everything the panel would draw, in one blob. Deliberately includes the prose: a round trip
// that reproduced the numbers and lost a sentence would still be a broken review mode.
static QByteArray surfaceOf(const SessionDiagnosticsModel &m)
{
    QVariantMap s;
    s[QStringLiteral("stage")]         = m.stage();
    s[QStringLiteral("shotCount")]     = m.shotCount();
    s[QStringLiteral("patternCount")]  = m.patternCount();
    s[QStringLiteral("headerInfo")]    = m.headerInfo();
    s[QStringLiteral("cards")]         = byId(m.cards());
    s[QStringLiteral("watching")]      = byId(m.watching());
    s[QStringLiteral("chains")]        = m.chains();
    s[QStringLiteral("unchained")]     = byId(m.unchained());
    s[QStringLiteral("unchainedLine")] = m.unchainedLine();
    s[QStringLiteral("driver")]        = m.driver();
    s[QStringLiteral("coverageLine")]  = m.coverageLine();
    s[QStringLiteral("bookends")]      = byId(m.bookends());
    return canon(s);
}

// The tier ledger, and ONLY the tier ledger. This is what "never alters detection or tiers" is
// asserted against — a snapshot that also carried ranking or link grades would pass when a focus
// contract legitimately changed a LINK (which it may) and fail for the wrong reason. Sorted for
// the same reason as above, and here the sort is itself load-bearing: the fault profile is
// ALLOWED to change the order and forbidden to change anything in the rows.
static QByteArray tiersOf(const SessionDiagnosticsModel &m)
{
    QVariantList out;
    // Read through the public surface, which is the only thing a panel can see.
    const QVariantMap readout = m.shotReadout(1);
    for (const QVariant &cv : readout.value(QStringLiteral("conditions")).toList()) {
        const QVariantMap c = cv.toMap();
        out.append(QVariantMap{
            { QStringLiteral("id"),         c.value(QStringLiteral("id")) },
            { QStringLiteral("tierTag"),    c.value(QStringLiteral("tierTag")) },
            { QStringLiteral("recurrence"), c.value(QStringLiteral("recurrence")) },
            { QStringLiteral("stateKind"),  c.value(QStringLiteral("stateKind")) },
            { QStringLiteral("valueText"),  c.value(QStringLiteral("valueText")) },
            { QStringLiteral("corridorText"), c.value(QStringLiteral("corridorText")) },
        });
    }
    return canon(byId(out));
}

static QByteArray ledgerBytesOf(const QString &sessionDir)
{
    QFile f(QDir(sessionDir).filePath(QStringLiteral("diagnostics.json")));
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    return QJsonDocument(root.value(QStringLiteral("ledger")).toObject())
        .toJson(QJsonDocument::Compact);
}

// The session every block below is built from: five copies of the corpus's richest capture, then
// one sparse one. Five of the same swing is a deliberate synthetic — it is the only way to get a
// condition past the pattern gate (3 assessable AND a Wilson lower bound over 0.30) out of four
// available fixtures, and the panel's whole subject is what RECURS. The sparse shot at the end is
// there so the review strip has genuinely not-assessable rows to print reasons into.
static const char *kLayout[] = { "rich_7iron", "rich_7iron", "rich_7iron",
                                 "rich_7iron", "rich_7iron", "sparse_noclub" };
static constexpr int kShots = 6;

static bool stageLayout(const QString &sessionDir)
{
    for (int i = 0; i < kShots; ++i)
        if (!stageShot(sessionDir, i + 1, kLayout[i])) return false;
    return true;
}

static std::unique_ptr<SessionDiagnosticsModel> freshModel(const QString &cadence = QStringLiteral("everyShot"))
{
    auto m = std::make_unique<SessionDiagnosticsModel>();
    m->setSynchronous(true);
    m->setCadence(cadence);
    return m;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::printf("session_diagnostics_model_test\n");

    QTemporaryDir tmp;
    check(tmp.isValid(), "a temporary swing library");
    if (!tmp.isValid()) { std::printf("\nFAILURES\n"); return 1; }

    // ── 1. Live ingest: stage progression and the ratchet ────────────────────────────
    std::printf("\nlive ingest\n");
    QByteArray liveSurface;
    QByteArray liveLedger;
    int        patternsAtClose = 0;
    {
        // The LIVE case: an empty session directory, driven shot by shot, so the stage
        // machine is watched moving rather than arrived at.
        auto m = freshModel();
        const QString stepDir = makeSession(tmp, "athlete_a", "session_step");
        m->activateSession(stepDir);
        check(m->stage() == QLatin1String("cold"), "an empty session is Cold");
        check(m->shotCount() == 0, "…with no shots");

        for (int i = 0; i < kShots; ++i) {
            check(stageShot(stepDir, i + 1, kLayout[i]), "a shot staged");
            m->ingestShot(i + 1, swingDirFor(stepDir, i + 1));
            if (i == 1)
                check(m->stage() == QLatin1String("cold"),
                      "two shots is still Cold — below the assessable floor");
        }
        check(m->shotCount() == kShots, "every shot reached the ledger");
        const QString stageAfter = m->stage();
        std::printf("      stage after %d shots: %s · %d patterns · %s\n",
                    kShots, qPrintable(stageAfter), m->patternCount(),
                    qPrintable(m->coverageLine()));
        check(stageAfter == QLatin1String("forming") || stageAfter == QLatin1String("established"),
              "six shots have left Cold");
        check(m->patternCount() >= 1, "at least one condition reached pattern tier");
        check(!m->cards().isEmpty(), "…and produced a pattern card");
        check(!m->coverageLine().isEmpty(), "the coverage line states how little is measurable");

        // The card's wording rules, checked on the surface rather than in the ledger — these
        // are the strings the panel paints and nothing else may compose them.
        const QVariantMap card = m->cards().first().toMap();
        const QString recurrence = card.value(QStringLiteral("recurrence")).toString();
        check(recurrence.endsWith(QLatin1String("measurable shots")),
              "recurrence is always \"<fired> of <assessable> measurable shots\"");
        check(!card.value(QStringLiteral("directionText")).toString().isEmpty()
                  || card.value(QStringLiteral("directionClaimed")).toBool(),
              "a card states a direction or says why it cannot");
        if (!card.value(QStringLiteral("directionClaimed")).toBool()) {
            check(card.value(QStringLiteral("directionText")).toString()
                      .contains(QLatin1String("dispersion, not a direction")),
                  "below the agreement gate the card says dispersion, not a direction");
        }
        check(card.value(QStringLiteral("ticks")).toList().size() == kShots,
              "the tick run carries one tick per shot, never a gap");

        // ── The chain rail, and the honesty devices on it ────────────────────────────
        if (stageAfter == QLatin1String("established")) {
            check(!m->chains().isEmpty(), "Established draws at least one chain rail");
            bool railOk = !m->chains().isEmpty(), sawLink = false;
            int liveNodes = 0, ghostNodes = 0;
            for (const QVariant &cv : m->chains()) {
                const QVariantMap chain = cv.toMap();
                const QVariantList nodes = chain.value(QStringLiteral("nodes")).toList();
                if (nodes.size() < 2) railOk = false;
                if (!chain.value(QStringLiteral("anyLive")).toBool()) railOk = false;
                for (const QVariant &nv : nodes) {
                    const QVariantMap n = nv.toMap();
                    const QString kind = n.value(QStringLiteral("kind")).toString();
                    if (kind == QLatin1String("live")) ++liveNodes;
                    else if (kind == QLatin1String("ghost")) ++ghostNodes;
                    else if (kind != QLatin1String("screenedRoot") && kind != QLatin1String("outcome"))
                        railOk = false;
                    // A node the capture never measured must SAY SO — a ghost drawn without
                    // its mark is a chain claiming continuity it does not have.
                    if (kind != QLatin1String("live")
                        && n.value(QStringLiteral("mark")).toString().isEmpty()) railOk = false;
                    if (n.value(QStringLiteral("statePill")).toString().isEmpty()) railOk = false;
                }
                for (const QVariant &lv : chain.value(QStringLiteral("links")).toList()) {
                    const QVariantMap l = lv.toMap();
                    sawLink = true;
                    // The word, the stroke and the note under it are all decided in C++ —
                    // brief §4.1. A link with no note is a claim with no evidence beside it.
                    if (l.value(QStringLiteral("word")).toString().isEmpty()
                        || l.value(QStringLiteral("stroke")).toString().isEmpty()
                        || l.value(QStringLiteral("note")).toString().isEmpty()) railOk = false;
                    // Only the two tested grades carry an arrowhead.
                    const QString g = l.value(QStringLiteral("grade")).toString();
                    const bool arrow = l.value(QStringLiteral("arrow")).toBool();
                    if (arrow != (g == QLatin1String("movedTogether")
                                  || g == QLatin1String("conditionallyDependent"))) railOk = false;
                }
            }
            check(railOk, "every rail node carries its kind, mark and pill; every link its word, "
                          "stroke and note");
            check(sawLink, "…and the rail actually joins its nodes");
            std::printf("      rail: %d live nodes, %d ghosts across %lld chains\n",
                        liveNodes, ghostNodes, static_cast<long long>(m->chains().size()));
        }

        // ── The driver footer, debounced by pattern-set stability ────────────────────
        const QVariantMap drv = m->driver();
        if (drv.value(QStringLiteral("eligible")).toBool()) {
            check(!drv.value(QStringLiteral("rootName")).toString().isEmpty(),
                  "an eligible driver footer names its root");
            check(drv.value(QStringLiteral("whyText")).toString()
                      .contains(QLatin1String("would explain")),
                  "…and says why, as a count of what it accounts for");
            std::printf("      driver: %s\n",
                        qPrintable(drv.value(QStringLiteral("whyText")).toString()));
        } else {
            check(!drv.value(QStringLiteral("waitingText")).toString().isEmpty(),
                  "an ineligible driver footer says it is waiting, rather than showing nothing");
        }

        // A pattern with no authored edge is reported on its own line, never dropped.
        if (!m->unchained().isEmpty())
            check(!m->unchainedLine().isEmpty(),
                  "an unchained pattern gets its own line rather than being dropped");

        // Idempotence — the same shot again, and the same shot from a different directory.
        m->ingestShot(3, swingDirFor(stepDir, 3));
        m->ingestShot(3, swingDirFor(stepDir, 1));
        check(m->shotCount() == kShots, "re-ingesting a shot id is a no-op");

        // Closing freezes and produces the bookends strip.
        m->closeSession();
        check(m->stage() == QLatin1String("closing"), "closeSession freezes at Closing");
        check(!m->bookends().isEmpty(), "the closing panel offers session bookends");
        const QVariantMap bk = m->bookends().first().toMap();
        check(bk.contains(QStringLiteral("representativeShotId")),
              "…including the most representative shot, not just best and worst");
        m->ingestShot(99, swingDirFor(stepDir, 1));
        check(m->stage() == QLatin1String("closing") && m->shotCount() == kShots,
              "a closed session is frozen: no re-open, and no further shots");

        patternsAtClose = m->patternCount();
        liveSurface = surfaceOf(*m);
        liveLedger  = ledgerBytesOf(stepDir);
        check(!liveLedger.isEmpty(), "diagnostics.json was written into the session folder");
    }

    // ── 2. Persistence round trip ────────────────────────────────────────────────────
    std::printf("\npersistence round trip\n");
    {
        const QString stepDir = makeSession(tmp, "athlete_a", "session_step");
        auto m = freshModel();
        m->activateSession(stepDir);
        check(m->shotCount() == kShots, "a re-activated session re-reads its rows");
        check(m->stage() == QLatin1String("closing"), "…and remembers that it was closed");
        check(surfaceOf(*m) == liveSurface,
              "the surface rebuilt from disk is byte-identical to the live one");
        check(ledgerBytesOf(stepDir) == liveLedger,
              "…and activation left the evidence on disk untouched");
    }

    // ── 3. Cold back-fill: swing dirs on disk, no diagnostics.json ───────────────────
    std::printf("\ncold back-fill\n");
    {
        const QString coldDir = makeSession(tmp, "athlete_b", "session_cold");
        check(stageLayout(coldDir), "six swings staged with no diagnostics.json beside them");
        auto m = freshModel();
        m->activateSession(coldDir);
        check(m->shotCount() == kShots, "activation back-filled every swing directory");
        check(QFile::exists(QDir(coldDir).filePath(QStringLiteral("diagnostics.json"))),
              "…and wrote the file it did not find");

        // Enable-mid-session: one more swing appears, activation picks up only the new one.
        check(stageShot(coldDir, 7, "lm_7iron"), "a seventh swing arrives");
        auto m2 = freshModel();
        m2->activateSession(coldDir);
        check(m2->shotCount() == kShots + 1, "re-activation reconciles and back-fills the difference");
    }

    // ── 4. Cadence gates surfacing, never ingest ─────────────────────────────────────
    std::printf("\ncadence\n");
    {
        // ONE shot, and a sparse one: no pattern exists yet and there is no previous shot for a
        // clean streak to break, so bandwidth has nothing to surface and every-shot still does.
        const QString aDir = makeSession(tmp, "athlete_c", "session_bandwidth");
        const QString bDir = makeSession(tmp, "athlete_c", "session_everyshot");
        check(stageShot(aDir, 1, "sparse_noclub") && stageShot(bDir, 1, "sparse_noclub"),
              "one sparse swing in each of two sessions");

        auto a = freshModel(QStringLiteral("bandwidth"));
        a->activateSession(aDir);
        auto b = freshModel(QStringLiteral("everyShot"));
        b->activateSession(bDir);

        check(a->shotCount() == 1 && b->shotCount() == 1, "both modes ingested the shot");
        check(a->quiet(), "bandwidth is quiet when nothing pattern-tier fired");
        check(!b->quiet(), "every-shot always surfaces");
        check(ledgerBytesOf(aDir) == ledgerBytesOf(bDir),
              "the two modes persist byte-identical evidence — cadence gates surfacing only");
        check(a->afterShotDelta().value(QStringLiteral("surfaced")).toBool() == false
                  && b->afterShotDelta().value(QStringLiteral("surfaced")).toBool() == true,
              "the after-shot delta reports which mode surfaced it");
        check(!a->afterShotDelta().value(QStringLiteral("headline")).toString().isEmpty(),
              "…and the delta itself is computed either way, quiet or not");

        // Flipping the mode changes the surface and not a row.
        const QByteArray before = ledgerBytesOf(aDir);
        a->setCadence(QStringLiteral("everyShot"));
        check(!a->quiet(), "switching to every-shot surfaces the same shot");
        check(ledgerBytesOf(aDir) == before, "…and rewrites no evidence");
    }

    // ── 5. Focus contract and declared miss: persisted, and inert on the evidence ────
    std::printf("\nfocus contract and declared miss\n");
    {
        const QString dir = makeSession(tmp, "athlete_d", "session_intent");
        check(stageLayout(dir), "six swings staged");

        auto m = freshModel();
        m->activateSession(dir);
        const QByteArray tiersBefore = tiersOf(*m);
        check(!tiersBefore.isEmpty(), "there are tiers to compare");

        const QString focusId = m->cards().isEmpty()
                                    ? QString()
                                    : m->cards().first().toMap().value(QStringLiteral("id")).toString();
        m->declareFocus(focusId);
        check(m->focusConditionId() == focusId, "the focus contract is recorded");
        check(m->focusFromShot() == kShots,
              "…splitting the session after the shots already struck");
        m->declareMiss(QStringLiteral("slice"));
        check(m->declaredMiss() == QLatin1String("slice"), "the declared miss is recorded");

        check(tiersOf(*m) == tiersBefore,
              "NEITHER DECLARATION MOVES A TIER, A COUNT OR A CORRIDOR");

        // Both survive a round trip.
        auto m2 = freshModel();
        m2->activateSession(dir);
        check(m2->focusConditionId() == focusId && m2->focusFromShot() == kShots,
              "the focus contract round-trips through diagnostics.json");
        check(m2->declaredMiss() == QLatin1String("slice"),
              "…and so does the declared miss");
        check(tiersOf(*m2) == tiersBefore, "…still without moving a tier");

        m2->clearFocus();
        check(m2->focusConditionId().isEmpty(), "the focus contract can be withdrawn");
        check(tiersOf(*m2) == tiersBefore, "…which also moves no tier");
    }

    // ── 6. The fault profile: written at close, read at the next activation, inert ───
    std::printf("\nfault profile\n");
    {
        const QString first  = makeSession(tmp, "athlete_e", "session_one");
        const QString second = makeSession(tmp, "athlete_e", "session_two");
        check(stageLayout(first) && stageLayout(second), "two sessions for one athlete");

        auto m1 = freshModel();
        m1->activateSession(first);
        check(m1->expectations().isEmpty(), "a first session has no history to expect");
        m1->closeSession();

        const QString profilePath =
            QDir(QDir(first).filePath(QStringLiteral(".."))).absoluteFilePath(
                QStringLiteral("fault_profile.json"));
        check(QFile::exists(profilePath),
              "closing writes fault_profile.json into the ATHLETE folder, not the session");

        auto m2 = freshModel();
        m2->activateSession(second);
        const QByteArray tiersWithProfile = tiersOf(*m2);
        check(!m2->expectations().isEmpty(),
              "the next session opens with the athlete's usual patterns");
        const QVariantMap exp = m2->expectations().first().toMap();
        check(exp.value(QStringLiteral("text")).toString()
                  .contains(QLatin1String("of your last")),
              "…phrased as a count of sessions, never a percentage");
        check(exp.value(QStringLiteral("caveat")).toString()
                  .contains(QLatin1String("not a finding")),
              "…and labelled an expectation to test");

        // The structural claim: the same rows under a profile and under none produce the same
        // tiers. The profile reaches ranking and the Cold list; there is no code path from it
        // to a reduction, and this is the assertion that keeps it that way.
        const QString bare = makeSession(tmp, "athlete_f", "session_two");
        check(stageLayout(bare), "the same six swings under an athlete with no history");
        auto m3 = freshModel();
        m3->activateSession(bare);
        check(m3->expectations().isEmpty(), "no history, no expectations");
        check(tiersOf(*m3) == tiersWithProfile,
              "IDENTICAL ROWS PRODUCE IDENTICAL TIERS, profile or no profile");

        // Closing twice must not double-count the session.
        auto m4 = freshModel();
        m4->activateSession(first);
        m4->closeSession();
        QFile pf(profilePath);
        check(pf.open(QIODevice::ReadOnly), "the profile is readable");
        const QJsonObject prof = QJsonDocument::fromJson(pf.readAll()).object();
        pf.close();
        const QJsonObject conds = prof.value(QStringLiteral("conditions")).toObject();
        bool countsSane = !conds.isEmpty();
        for (auto it = conds.constBegin(); it != conds.constEnd(); ++it) {
            const QJsonObject c = it.value().toObject();
            if (c.value(QStringLiteral("sessionsSeen")).toInt() != 1) countsSane = false;
        }
        check(countsSane, "closing the same session twice counts it once");
        std::printf("      profile carries %lld conditions after one session\n",
                    static_cast<long long>(conds.size()));
    }

    // ── 7. Review payloads ───────────────────────────────────────────────────────────
    std::printf("\nreview payloads (13a)\n");
    {
        const QString dir = makeSession(tmp, "athlete_g", "session_review");
        check(stageLayout(dir), "six swings staged");
        auto m = freshModel();
        m->activateSession(dir);
        m->closeSession();
        m->setReviewing(true);
        m->setSelectedShotId(3);

        check(m->stage() == QLatin1String("closing"), "review holds the final state");
        check(m->headerInfo().value(QStringLiteral("reviewBadge")).toString()
                  == QStringLiteral("REVIEWING · shot 3 of 6"),
              "the header badges the reviewed shot inside the finished ledger");
        check(m->headerInfo().value(QStringLiteral("countLine")).toString()
                  .contains(QLatin1String("counted over all 6 shots")),
              "…and states that the counts are session totals");

        const QVariantMap ro = m->shotReadout(3);
        check(!ro.isEmpty(), "shotReadout returns a payload for a shot in the ledger");
        check(m->shotReadout(4242).isEmpty(), "…and nothing for one that is not");
        check(ro.value(QStringLiteral("headline")).toString()
                  .contains(QLatin1String("fired on this swing")),
              "the strip headline is one population");
        const QVariantList conds = ro.value(QStringLiteral("conditions")).toList();
        check(!conds.isEmpty(), "the strip lists this session's measurable conditions");

        bool shapeOk = true, minusOk = true, selectedOk = true;
        for (const QVariant &cv : conds) {
            const QVariantMap c = cv.toMap();
            const QString state = c.value(QStringLiteral("state")).toString();
            if (state != QLatin1String("OUT") && state != QLatin1String("IN")
                && state != QStringLiteral("—")) shapeOk = false;
            if (c.value(QStringLiteral("valueText")).toString().isEmpty()) shapeOk = false;
            // Never a blank in the corridor slot — a not-assessable row prints THE REASON there.
            if (c.value(QStringLiteral("corridorText")).toString().isEmpty()) shapeOk = false;
            if (c.value(QStringLiteral("tierTag")).toString().isEmpty()) shapeOk = false;
            if (state == QStringLiteral("—")
                && c.value(QStringLiteral("valueText")).toString() != QLatin1String("not measurable"))
                shapeOk = false;
            // A true minus (U+2212), never a hyphen, wherever a number is negative.
            const QString v = c.value(QStringLiteral("valueText")).toString();
            if (v.contains(QLatin1Char('-'))) minusOk = false;
            const QVariantList ticks = c.value(QStringLiteral("ticks")).toList();
            if (ticks.size() != kShots) shapeOk = false;
            if (!ticks.isEmpty() && !ticks.at(2).toMap().value(QStringLiteral("selected")).toBool())
                selectedOk = false;
            if (!c.contains(QStringLiteral("firingsAfterText"))) shapeOk = false;
        }
        check(shapeOk, "every row carries state, value, corridor-or-reason, tier and a tick run");
        check(minusOk, "negative values use a true minus, matching the corridor text");
        check(selectedOk, "the selected shot's tick is marked in every run");

        // The sparse capture is the one that has to explain itself: conditions the session
        // measured elsewhere and could not measure HERE. The corridor slot then carries the
        // REASON, and a blank there would read as a bug rather than as "we did not look".
        const QVariantList sparse = m->shotReadout(kShots)
                                        .value(QStringLiteral("conditions")).toList();
        int na = 0;
        bool reasonsOk = true;
        for (const QVariant &cv : sparse) {
            const QVariantMap c = cv.toMap();
            if (c.value(QStringLiteral("state")).toString() != QStringLiteral("—")) continue;
            ++na;
            if (c.value(QStringLiteral("valueText")).toString() != QLatin1String("not measurable")
                || c.value(QStringLiteral("reason")).toString().isEmpty())
                reasonsOk = false;
        }
        std::printf("      the sparse shot leaves %d of %lld conditions not assessable\n",
                    na, static_cast<long long>(sparse.size()));
        check(na > 0, "the sparse capture leaves genuinely not-assessable rows");
        check(reasonsOk, "…and every one of them prints WHY, never a blank");
        check(m->shotReadout(kShots).value(QStringLiteral("note")).toString()
                  .contains(QLatin1String("not assessable on this capture")),
              "…and the strip says how many, in words");

        const QVariantList pips = m->pipsFor(3);
        check(pips.size() == conds.size(),
              "the carousel pip row is the same population as the strip");
        bool pipsOk = !pips.isEmpty();
        for (const QVariant &pv : pips) {
            const QString s = pv.toMap().value(QStringLiteral("state")).toString();
            if (s != QLatin1String("fired") && s != QLatin1String("clean")
                && s != QLatin1String("notAssessable")) pipsOk = false;
        }
        check(pipsOk, "…and every pip is one of the three states");
        check(m->firedCountFor(3) >= 0 && m->pipsFor(4242).isEmpty(),
              "the fired count is offered per shot, and an unknown shot has no pips");
    }

    std::printf("\npatterns at close: %d\n", patternsAtClose);
    std::printf("\n%s\n", g_fail == 0 ? "OK" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
