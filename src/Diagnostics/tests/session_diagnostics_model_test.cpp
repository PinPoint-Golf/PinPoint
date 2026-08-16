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
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>

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

// ⚠ THE STAGED swing.json's MTIME IS INPUT DATA, NOT INCIDENTAL.
// SessionDiagnosticsModel::ingest() takes each shot's timestamp from the file's mtime
// (session_diagnostics_model.cpp:331) and toJson() writes it straight into the ledger as
// `shots[].timestampMs` (diagnostic_ledger.h:1405). QFile::copy does NOT preserve mtime — each
// copy is stamped "now" — so two sessions staged from the SAME fixture used to disagree whenever
// their two copies landed in different milliseconds. That is roughly a coin flip, and it made the
// byte-identical-ledger check below fail about 40% of the time while looking like a real defect
// in cadence handling.
//
// So the mtime is pinned, deterministically and per shot: same fixture staged anywhere yields the
// same ledger bytes, and later shot ids stay later in time so any ordering that reads the stamp
// still sees a sane sequence. Fix the staging rather than relax the assertion — "cadence changes
// no evidence" is worth checking to the byte, and a comparison that skips the fields that happen
// to be awkward stops being that check.
static constexpr qint64 kStageEpochMs = 1767225600000LL;  // 2026-01-01T00:00:00Z

static bool stageShot(const QString &sessionDir, int shotId, const char *fixture)
{
    const QString dst = QDir(sessionDir).filePath(
        QStringLiteral("swing_%1").arg(shotId, 4, 10, QLatin1Char('0')));
    if (!QDir().mkpath(dst)) return false;
    const QString src = QDir(QLatin1String(PP_LIVE_SWINGS_DIR))
                            .filePath(QLatin1String(fixture) + QLatin1String("/swing.json"));
    const QString dstFile = QDir(dst).filePath(QStringLiteral("swing.json"));
    if (!QFile::copy(src, dstFile)) return false;

    QFile f(dstFile);
    if (!f.open(QIODevice::ReadWrite)) return false;
    // One second per shot id, so shot 2 is a second after shot 1 rather than merely different.
    const bool stamped = f.setFileTime(
        QDateTime::fromMSecsSinceEpoch(kStageEpochMs + qint64(shotId) * 1000, QTimeZone::UTC),
        QFileDevice::FileModificationTime);
    f.close();
    // Deliberately fatal rather than a warning. If the mtime cannot be pinned the ledger
    // comparisons below become a coin flip again, and a suite that quietly degrades into an
    // intermittent one is worse than a suite that says it cannot run here.
    if (!stamped)
        std::printf("    setFileTime failed on %s — the filesystem will not take an mtime, so the\n"
                    "    byte-identical ledger checks cannot be trusted here.\n",
                    qPrintable(dstFile));
    return stamped;
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

    // ⚠ Fail here, loudly, rather than 400 lines later with a SIGSEGV.
    // This suite reads the shipped content through PINPOINT_CORE_* — ctest supplies them via
    // set_tests_properties(... ENVIRONMENT ...), so running the binary DIRECTLY (the natural move
    // when debugging one case) leaves the pack empty. Detection then finds nothing, the assertions
    // below reach for `cards().first()` on an empty list, and the run dies in QVariant::toMap with
    // a null `this` — a crash that reads like a defect in the model and is nothing of the kind.
    if (qEnvironmentVariableIsEmpty("PINPOINT_CORE_PACK")) {
        std::printf("\nPINPOINT_CORE_PACK is unset — this suite needs the shipped content.\n"
                    "Run it through ctest, which sets the environment for you:\n"
                    "    ctest --test-dir build/tests -R session_diagnostics_model_test\n"
                    "or export PINPOINT_CORE_{PACK,NORMS,CONTEXTS,SCREENS,DRILLS,REFERENCES}\n"
                    "pointing at src/Resources/diagnostics/*.json.\n\nFAILURES\n");
        return 1;
    }

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
        } else if (drv.value(QStringLiteral("final")).toBool()) {
            // CHANGED: a finished session's footer is DEFINITIVE (§B7). The debounce is a
            // live-only device — its argument is that the pattern set may still move, and on a
            // closed session it cannot. "The driver appears once the pattern set has held
            // still for 3 shots" under a finished session is a promise about a shot that will
            // never be taken.
            check(drv.value(QStringLiteral("waitingText")).toString().isEmpty(),
                  "a finished session's footer is never waiting for a shot that will not come");
            check(drv.value(QStringLiteral("finalText")).toString()
                      .startsWith(QLatin1String("No driver")),
                  "…it says there is no driver, in as many words");
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

        // ── CHANGED: the headline's denominator is the MEASURABLE SET ────────────────
        // It used to be fired+clean — the conditions this swing answered — while the grid
        // below it drew every condition the session can measure. Two counts of the same thing
        // that do not agree read as a bug in the panel rather than as a distinction, and the
        // measurable set is the honest one because it is what is on screen.
        check(ro.value(QStringLiteral("measurableSetCount")).toInt() == int(conds.size()),
              "the measurable set is the count of what the strip publishes");
        check(ro.value(QStringLiteral("headline")).toString()
                  .contains(QStringLiteral(" of %1 conditions").arg(int(conds.size()))),
              "…and it is the headline's denominator, so headline and grid agree");

        // ── The information order, and the silent tail ───────────────────────────────
        // Fired here, then clean here, then not-readable here but a pattern or a watch this
        // session, then the tail — silent both ways. On a real capture the tail is most of the
        // set, and published in pack order it buries the handful of cells that mean something.
        {
            int phase = 0;              // 0 fired · 1 clean · 2 watched · 3 tail
            bool orderOk = true, tailOk = true;
            int tail = 0;
            for (const QVariant &cv : conds) {
                const QVariantMap c = cv.toMap();
                const QString st = c.value(QStringLiteral("state")).toString();
                const bool isTail = c.value(QStringLiteral("tail")).toBool();
                const int want = isTail ? 3
                               : st == QLatin1String("OUT") ? 0
                               : st == QLatin1String("IN")  ? 1
                                                            : 2;
                if (want < phase) orderOk = false;
                phase = want;
                if (isTail) {
                    ++tail;
                    // The tail is the CONJUNCTION of two silences, and nothing else may be in
                    // it: a condition that fired somewhere this session is what review is for.
                    if (st != QStringLiteral("—")
                        || c.value(QStringLiteral("tierTag")).toString()
                               != QLatin1String("clean all session"))
                        tailOk = false;
                }
            }
            check(orderOk, "the strip is ordered by information: fired, clean, watched, tail");
            check(tailOk, "…and the tail is only what was silent here AND all session");
            check(ro.value(QStringLiteral("tailCount")).toInt() == tail,
                  "the tail is counted for the one line that stands in for it");
            check(tail == 0 || ro.value(QStringLiteral("tailSummary")).toString()
                                   .contains(QLatin1String("clean all session")),
                  "…and that line names both silences rather than just counting them");
        }

        // ── The recorded stage, published beside the displayed one ───────────────────
        // `stage` is frozen at Closing under review and cannot say what the session achieved.
        // The panel's composition turns on that difference: a rail is the arrangement of a
        // session that EARNED a chain, and chains are published for every pattern regardless.
        check(m->headerInfo().contains(QStringLiteral("reachedEstablished")),
              "the header says whether the session ever established");
        check(m->headerInfo().value(QStringLiteral("recordedStage")).toString()
                  != QLatin1String("established")
              || m->headerInfo().value(QStringLiteral("reachedEstablished")).toBool(),
              "…and an Established recorded stage always answers yes");
        std::printf("      recorded stage %s · reachedEstablished %s (displayed %s)\n",
                    qPrintable(m->headerInfo().value(QStringLiteral("recordedStage")).toString()),
                    m->headerInfo().value(QStringLiteral("reachedEstablished")).toBool()
                        ? "true" : "false",
                    qPrintable(m->stage()));

        // ── §B7: the footer is definitive at the close ───────────────────────────────
        {
            const QVariantMap rdrv = m->driver();
            check(rdrv.value(QStringLiteral("final")).toBool(),
                  "a reviewed session's footer is in the finished tense");
            check(rdrv.value(QStringLiteral("waitingText")).toString().isEmpty(),
                  "…and never prints the live debounce's waiting line");
            check(rdrv.value(QStringLiteral("eligible")).toBool()
                      ? !rdrv.value(QStringLiteral("rootName")).toString().isEmpty()
                      : rdrv.value(QStringLiteral("finalText")).toString()
                            .startsWith(QLatin1String("No driver")),
                  "…it either names the driver or says there is not one");
        }

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

    // ── 8. The condition detail (user-requested drill-in) ────────────────────────────
    //
    // THE CLAIM UNDER TEST IS THAT IT IS A READ. conditionDetail() answers a question about a
    // condition and must not be able to change the panel, the ledger or the file — a drill-in
    // that quietly re-ranked a card or re-wrote diagnostics.json because somebody LOOKED at a
    // condition would be the same class of defect as a cadence check in the ingest path.
    //
    // The rest is honesty: a headline that names a driver when there is one, says which cause is
    // strongest when there is not, offers the screen when nothing is measurable, and says there
    // is nothing when there is nothing. On a six-shot camera capture the last two are the common
    // cases, which is exactly why they are asserted rather than the happy path alone.
    std::printf("\ncondition detail\n");
    {
        const QString dir = makeSession(tmp, "athlete_h", "session_detail");
        check(stageLayout(dir), "six swings staged");
        auto m = freshModel();
        m->activateSession(dir);

        check(!m->cards().isEmpty(), "there is a pattern to drill into");
        const QString id = m->cards().first().toMap().value(QStringLiteral("id")).toString();

        const QVariantMap d = m->conditionDetail(id);
        check(!d.isEmpty(), "conditionDetail returns a payload for a condition the pack authors");
        check(m->conditionDetail(QStringLiteral("no_such_condition")).isEmpty(),
              "…and nothing for one it does not");

        // ── the header is the panel's own card, for this condition ───────────────────
        const QVariantMap header = d.value(QStringLiteral("header")).toMap();
        check(header.value(QStringLiteral("id")).toString() == id, "the header names the condition");
        check(!header.value(QStringLiteral("name")).toString().isEmpty(), "…by name");
        check(header.value(QStringLiteral("recurrence")).toString()
                  .endsWith(QLatin1String("measurable shots")),
              "…with the SAME recurrence wording the card carries");
        check(header.value(QStringLiteral("ticks")).toList().size() == kShots,
              "…and one tick per shot, never a gap");
        check(!header.value(QStringLiteral("statePill")).toString().isEmpty(),
              "…and a state pill rather than a blank");
        {
            // The card map is shared with buildCards(), and that is the point: the same
            // condition drawn on the panel and on its own page must not be able to differ.
            QVariantMap panelCard;
            for (const QVariant &cv : m->cards())
                if (cv.toMap().value(QStringLiteral("id")).toString() == id) panelCard = cv.toMap();
            check(!panelCard.isEmpty(), "the same condition has a card on the panel");
            check(panelCard.value(QStringLiteral("recurrence"))
                      == header.value(QStringLiteral("recurrence"))
                  && panelCard.value(QStringLiteral("directionText"))
                         == header.value(QStringLiteral("directionText"))
                  && panelCard.value(QStringLiteral("ticks"))
                         == header.value(QStringLiteral("ticks")),
                  "THE PAGE AND THE PANEL CANNOT WORD THE SAME CONDITION DIFFERENTLY");
        }

        // ── the rails: the chain rail's own node and link vocabulary ─────────────────
        const QVariantList causes  = d.value(QStringLiteral("causes")).toList();
        const QVariantList effects = d.value(QStringLiteral("effects")).toList();
        std::printf("      %s: %lld cause paths, %lld effect paths\n",
                    qPrintable(id), static_cast<long long>(causes.size()),
                    static_cast<long long>(effects.size()));
        check(!causes.isEmpty() || !effects.isEmpty(),
              "a pattern-tier condition has an authored ancestry, a descent, or both");

        bool railsOk = true, primaryOk = true, orderedOk = true;
        int primaries = 0, lastLive = 1 << 30;
        const QVariantList sides[2] = { causes, effects };
        for (const QVariantList &side : sides) {
            primaries = 0;
            lastLive  = 1 << 30;
            for (int i = 0; i < side.size(); ++i) {
                const QVariantMap rail = side.at(i).toMap();
                const QVariantList nodes = rail.value(QStringLiteral("nodes")).toList();
                if (nodes.size() < 2) railsOk = false;
                if (rail.value(QStringLiteral("primary")).toBool()) {
                    ++primaries;
                    // The primary is the FIRST, because the list is published in the order the
                    // panel draws it — most-evidenced first, railBefore()'s rule.
                    if (i != 0) primaryOk = false;
                }
                // Most-evidenced first: the live count never rises as the list goes on.
                const int live = rail.value(QStringLiteral("liveCount")).toInt();
                if (live > lastLive) orderedOk = false;
                lastLive = live;

                for (const QVariant &nv : nodes) {
                    const QVariantMap n = nv.toMap();
                    const QString kind = n.value(QStringLiteral("kind")).toString();
                    if (kind != QLatin1String("live") && kind != QLatin1String("ghost")
                        && kind != QLatin1String("screenedRoot") && kind != QLatin1String("outcome"))
                        railsOk = false;
                    // Every honesty device the rail carries, carried here: a node the capture
                    // never measured SAYS SO, and every node has a pill rather than a blank.
                    if (kind != QLatin1String("live")
                        && n.value(QStringLiteral("mark")).toString().isEmpty()) railsOk = false;
                    if (n.value(QStringLiteral("statePill")).toString().isEmpty()) railsOk = false;
                }
                for (const QVariant &lv : rail.value(QStringLiteral("links")).toList()) {
                    const QVariantMap l = lv.toMap();
                    if (l.value(QStringLiteral("word")).toString().isEmpty()
                        || l.value(QStringLiteral("stroke")).toString().isEmpty()
                        || l.value(QStringLiteral("note")).toString().isEmpty()) railsOk = false;
                    const QString g = l.value(QStringLiteral("grade")).toString();
                    if (l.value(QStringLiteral("arrow")).toBool()
                        != (g == QLatin1String("movedTogether")
                            || g == QLatin1String("conditionallyDependent"))) railsOk = false;
                }
            }
            if (!side.isEmpty() && primaries != 1) primaryOk = false;
        }
        check(railsOk, "every detail node carries its kind, mark and pill; every link its word, "
                       "stroke and note — the rail's vocabulary, unchanged");
        check(primaryOk, "exactly one path per side is the primary, and it is the first");
        check(orderedOk, "…and the paths are published most-evidenced first");

        // ── the two headlines ────────────────────────────────────────────────────────
        const QString causeLine   = d.value(QStringLiteral("causeHeadline")).toString();
        const QString outcomeLine = d.value(QStringLiteral("outcomeHeadline")).toString();
        std::printf("      cause:   %s\n", qPrintable(causeLine));
        std::printf("      outcome: %s\n", qPrintable(outcomeLine));
        check(!causeLine.isEmpty() && !outcomeLine.isEmpty(),
              "both headlines are always said, never left blank");
        check(causeLine.startsWith(QLatin1String("Likely driver:"))
                  || causeLine.startsWith(QLatin1String("Strongest authored cause this session:"))
                  || causeLine.contains(QLatin1String("would anchor this chain"))
                  || causeLine == QLatin1String("No cause the capture can see today."),
              "…and the cause headline is one of the four honest answers");
        check(outcomeLine.startsWith(QLatin1String("Most likely outcome:"))
                  || outcomeLine.startsWith(QLatin1String("No authored outcome"))
                  || outcomeLine.startsWith(QLatin1String("This is the outcome")),
              "…and the outcome headline is one of the three");
        // NO PROBABILITY LANGUAGE, EVER. The only number either sentence may carry is a count,
        // and "likely" is the ordering's claim rather than an estimate — brief §5.1's rule
        // applied to the one place on this panel where a model output is closest to a forecast.
        check(!outcomeLine.contains(QLatin1String("%"))
                  && !outcomeLine.contains(QLatin1String("chance"))
                  && !outcomeLine.contains(QLatin1String("probab")),
              "…and neither invents a probability");
        // The LM-anchored form is a count over shots, and it says which shots.
        if (outcomeLine.startsWith(QLatin1String("Most likely outcome:")))
            check(outcomeLine.contains(QLatin1String("measurable shots"))
                      || outcomeLine.contains(QLatin1String("not measurable with this capture")),
                  "…an anchored outcome quotes its count; an unanchored one says it has none");

        // ── determinism ──────────────────────────────────────────────────────────────
        // The walk chooses branches by grade, then authored strength, then id, exactly as the
        // rail does. A page that reshuffled between two calls over one ledger would be a page
        // nobody could quote back.
        check(canon(d) == canon(m->conditionDetail(id)),
              "the same condition over the same ledger yields a byte-identical page");

        // ── the detail is a PURE READ ────────────────────────────────────────────────
        const QByteArray surfaceBefore = surfaceOf(*m);
        const QByteArray tiersBefore   = tiersOf(*m);
        const QByteArray ledgerBefore  = ledgerBytesOf(dir);
        const QString    stageBefore   = m->stage();

        for (const QVariant &cv : m->cards())
            m->conditionDetail(cv.toMap().value(QStringLiteral("id")).toString());
        m->openDetail(id);
        check(m->detailConditionId() == id, "openDetail publishes which condition is open");
        check(!m->detail().isEmpty(), "…and the page with it");
        m->openDetail(QStringLiteral("no_such_condition"));
        check(m->detailConditionId() == id,
              "…and declines a condition the pack does not author, silently");

        check(surfaceOf(*m) == surfaceBefore, "OPENING A DETAIL MOVES NO ZONE OF THE PANEL");
        check(tiersOf(*m) == tiersBefore,     "…no tier, no count, no corridor");
        check(ledgerBytesOf(dir) == ledgerBefore, "…and writes nothing to disk");
        check(m->stage() == stageBefore, "…and cannot move the stage ratchet");

        m->closeDetail();
        check(m->detailConditionId().isEmpty() && m->detail().isEmpty(),
              "closeDetail puts the page away");
        check(surfaceOf(*m) == surfaceBefore, "…and closing moves nothing either");

        // ── live and review are the same page ────────────────────────────────────────
        // The detail takes no cadence decision and holds no tense of its own; the only thing
        // review changes is WHICH tick is the wide one, which is ticksFor()'s job and is shared.
        m->openDetail(id);
        const QVariantMap liveDetail = m->detail();
        m->setCadence(QStringLiteral("bandwidth"));
        check(canon(m->detail()) == canon(liveDetail), "cadence cannot reach the page");
        m->setCadence(QStringLiteral("everyShot"));
        m->closeSession();
        m->setReviewing(true);
        m->setSelectedShotId(3);
        const QVariantMap reviewDetail = m->detail();
        check(!reviewDetail.isEmpty(), "the page survives the close and the review");
        {
            // The one honest difference: the reviewed shot is the wide tick, here as everywhere.
            const QVariantList ticks =
                reviewDetail.value(QStringLiteral("header")).toMap()
                            .value(QStringLiteral("ticks")).toList();
            check(ticks.size() == kShots, "…with its run intact");
            check(!ticks.isEmpty()
                      && ticks.at(2).toMap().value(QStringLiteral("selected")).toBool(),
                  "…and the reviewed shot marked in it, reusing ticksFor()");
            const QVariantList liveTicks =
                liveDetail.value(QStringLiteral("header")).toMap()
                          .value(QStringLiteral("ticks")).toList();
            bool anySelectedLive = false;
            for (const QVariant &tv : liveTicks)
                if (tv.toMap().value(QStringLiteral("selected")).toBool()) anySelectedLive = true;
            check(!anySelectedLive, "…and a LIVE page selects no tick, because nothing is picked");
        }
    }

    // ── 9. Honest when the capture can see nothing ───────────────────────────────────
    std::printf("\ncondition detail on a sparse capture\n");
    {
        // ONE sparse swing: nothing reaches pattern tier, explain() has ranked nothing, and most
        // of the pack is unmeasurable. This is the state the wording has to survive — the panel's
        // whole discipline is that an absence is said out loud rather than filled in.
        const QString dir = makeSession(tmp, "athlete_i", "session_sparse");
        check(stageShot(dir, 1, "sparse_noclub"), "one sparse swing");
        auto m = freshModel();
        m->activateSession(dir);

        // A condition the pack authors and this capture said nothing about. missCandidates() is
        // the pack's own outcome layer, so its first entry is an authored condition by
        // construction — and an outcome is the interesting case, because its page has no
        // downstream at all.
        const QVariantList outcomes = m->missCandidates();
        check(!outcomes.isEmpty(), "the pack authors outcomes to drill into");
        const QString outcomeId = outcomes.first().toMap().value(QStringLiteral("id")).toString();

        const QVariantMap d = m->conditionDetail(outcomeId);
        check(!d.isEmpty(), "a condition with no ledger still has a page");
        const QVariantMap header = d.value(QStringLiteral("header")).toMap();
        check(header.value(QStringLiteral("valueText")).toString()
                  == QLatin1String("not measurable"),
              "…whose card says it is not measurable rather than blanking");
        check(header.value(QStringLiteral("statePill")).toString()
                  == QLatin1String("NOT MEASURED"),
              "…in the pill as well");
        check(!header.value(QStringLiteral("mark")).toString().isEmpty(),
              "…and carries the honesty mark for what KIND of node it is");

        const QString causeLine   = d.value(QStringLiteral("causeHeadline")).toString();
        const QString outcomeLine = d.value(QStringLiteral("outcomeHeadline")).toString();
        std::printf("      cause:   %s\n", qPrintable(causeLine));
        std::printf("      outcome: %s\n", qPrintable(outcomeLine));
        // NOT "Likely driver": explain() ranked nothing over an empty pattern set, and a page
        // that named one anyway would be inventing the model's one conclusion.
        check(!causeLine.startsWith(QLatin1String("Likely driver:")),
              "with no pattern set there is no ranked driver to name");
        check(causeLine.contains(QLatin1String("would anchor this chain"))
                  || causeLine == QLatin1String("No cause the capture can see today.")
                  || causeLine.startsWith(QLatin1String("Strongest authored cause this session:")),
              "…so the page offers the screen, or says there is nothing to see");
        check(!outcomeLine.isEmpty(), "and the outcome line is still said");
        check(!outcomeLine.contains(QLatin1String("of 0 measurable shots")),
              "…never as a count over a population of nothing");

        check(canon(d) == canon(m->conditionDetail(outcomeId)),
              "…and it is deterministic on a sparse capture too");
    }

    std::printf("\npatterns at close: %d\n", patternsAtClose);
    std::printf("\n%s\n", g_fail == 0 ? "OK" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
