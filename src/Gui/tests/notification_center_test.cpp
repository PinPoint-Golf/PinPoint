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

// NotificationCenter — the four rules of the notification model, driven against
// a clock the test owns rather than a 7 s sleep.
//
// These are the acceptance criteria of work item H-g in
// docs/design/live_capture_collection_design.md §8, and case 1 is 1 September
// 2026 itself: ten identical save failures must become ONE notification that
// says it happened ten times, where the shipped behaviour was one toast showing
// the tenth message with nothing to say the first nine had occurred.

#include "app/notification_center.h"

#include <QObject>
#include <cstdio>

static int g_fail = 0;
static void checkInt(const char *label, long long got, long long want)
{
    const bool ok = got == want;
    std::printf("  [%s] %-52s got %4lld  want %4lld\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}
static void checkBool(const char *label, bool got, bool want)
{
    const bool ok = got == want;
    std::printf("  [%s] %-52s got %-5s want %-5s\n",
                ok ? "PASS" : "FAIL", label, got ? "true" : "false", want ? "true" : "false");
    if (!ok) ++g_fail;
}
static void checkStr(const char *label, const QString &got, const QString &want)
{
    const bool ok = got == want;
    std::printf("  [%s] %-52s got %-18s want %s\n", ok ? "PASS" : "FAIL", label,
                qPrintable(got), qPrintable(want));
    if (!ok) ++g_fail;
}

// The clock every case drives by hand.
static qint64 g_now = 1'000'000;

static NotificationCenter::Notification saveFailed()
{
    NotificationCenter::Notification n;
    n.id       = QStringLiteral("swing.save.no-directory");
    n.severity = NotificationCenter::Error;
    n.kind     = NotificationCenter::Condition;
    n.title    = QStringLiteral("Swings are not being saved");
    n.detail   = QStringLiteral("/mnt/swingdata/... could not be created");
    n.actionLabel = QStringLiteral("Open Storage settings");
    n.actionId    = QStringLiteral("settings.storage");
    return n;
}

static NotificationCenter::Notification exportSkipped()
{
    NotificationCenter::Notification n;
    n.id       = QStringLiteral("shot.export.skipped");
    n.severity = NotificationCenter::Warn;
    n.kind     = NotificationCenter::Event;
    n.title    = QStringLiteral("Export skipped");
    n.cause    = QStringLiteral("swing.save.no-directory");
    return n;
}

int main()
{
    std::printf("=== NotificationCenter — the four rules ===\n\n");

    // ── R1: ten identical posts are one notification, counted ────────────────
    {
        NotificationCenter nc;
        nc.setClockForTest([] { return g_now; });

        std::printf("-- R1: 1 September, ten times --\n");
        for (int i = 0; i < 10; ++i) {
            nc.post(saveFailed());
            g_now += 4000;            // shots arrive seconds apart
        }
        checkInt ("live notifications", nc.rowCount(), 1);
        checkInt ("count on the one row", nc.countOf(QStringLiteral("swing.save.no-directory")), 10);
        checkBool("still live", nc.isLive(QStringLiteral("swing.save.no-directory")), true);
    }

    // ── R2: a consequence whose cause is live is recorded, not shown ─────────
    {
        g_now = 1'000'000;
        NotificationCenter nc;
        nc.setClockForTest([] { return g_now; });

        std::printf("\n-- R2: consequence suppressed while its cause is on screen --\n");
        nc.post(saveFailed());
        checkInt("live after the cause", nc.rowCount(), 1);
        nc.post(exportSkipped());
        checkInt ("live after the consequence", nc.rowCount(), 1);
        checkBool("consequence not shown", nc.isLive(QStringLiteral("shot.export.skipped")), false);

        // ⚠ The inverse must hold. Suppression is a statement about what the
        // user already knows, not a permanent ranking.
        std::printf("\n-- R2 inverse: the SAME consequence with no live cause IS shown --\n");
        nc.dismiss(QStringLiteral("swing.save.no-directory"));
        g_now += NotificationCenter::kCauseGraceMs + 1000;   // outlive the grace window
        nc.post(exportSkipped());
        checkBool("consequence shown on its own merits",
                  nc.isLive(QStringLiteral("shot.export.skipped")), true);

        std::printf("\n-- R2 grace: a cause just off screen still suppresses --\n");
        NotificationCenter nc2;
        nc2.setClockForTest([] { return g_now; });
        nc2.post(saveFailed());
        nc2.dismiss(QStringLiteral("swing.save.no-directory"));
        g_now += 2000;                                        // well inside the grace window
        nc2.post(exportSkipped());
        checkBool("still suppressed inside grace",
                  nc2.isLive(QStringLiteral("shot.export.skipped")), false);
    }

    // ── R2 transitively: a cascade three deep collapses to its root ─────────
    {
        g_now = 1'000'000;
        NotificationCenter nc;
        nc.setClockForTest([] { return g_now; });

        std::printf("\n-- R2 chain: root cause -> middle -> outer, one row --\n");
        // 1 September's actual shape: the library root is unwritable, so no
        // swing folder can be made, so no shot saves and every analysis
        // degrades. Four true statements, one fault, one thing to show.
        NotificationCenter::Notification root;
        root.id       = QStringLiteral("library.unwritable");
        root.severity = NotificationCenter::Error;
        root.kind     = NotificationCenter::Condition;
        root.title    = QStringLiteral("Swings cannot be saved");
        nc.post(root);

        NotificationCenter::Notification middle;
        middle.id    = QStringLiteral("swing.save.no-directory");
        middle.kind  = NotificationCenter::Condition;
        middle.cause = QStringLiteral("library.unwritable");
        middle.title = QStringLiteral("Swings are not being saved");

        NotificationCenter::Notification outer;
        outer.id    = QStringLiteral("shot.not-saved");
        outer.kind  = NotificationCenter::Event;
        outer.cause = QStringLiteral("swing.save.no-directory");
        outer.title = QStringLiteral("Shot not saved");

        for (int shot = 0; shot < 10; ++shot) { nc.post(middle); nc.post(outer); }

        checkInt ("one row for the whole cascade", nc.rowCount(), 1);
        checkStr ("and it is the root cause",
                  nc.data(nc.index(0, 0), NotificationCenter::IdRole).toString(),
                  QStringLiteral("library.unwritable"));
        checkBool("middle link suppressed",
                  nc.isLive(QStringLiteral("swing.save.no-directory")), false);
        checkBool("outer link suppressed too (the chain is followed)",
                  nc.isLive(QStringLiteral("shot.not-saved")), false);

        std::printf("\n-- and once the root is gone, the outer stands on its own --\n");
        nc.dismiss(QStringLiteral("library.unwritable"));
        g_now += NotificationCenter::kCauseGraceMs + 1000;
        nc.sweep();
        nc.post(outer);
        checkBool("outer shown with no live cause",
                  nc.isLive(QStringLiteral("shot.not-saved")), true);
    }

    // ── R3: conditions latch, events expire ─────────────────────────────────
    {
        g_now = 1'000'000;
        NotificationCenter nc;
        nc.setClockForTest([] { return g_now; });

        std::printf("\n-- R3: a condition outlives the dwell, an event does not --\n");
        nc.post(saveFailed());                     // Condition

        NotificationCenter::Notification refused;
        refused.id       = QStringLiteral("shot.corroboration.refused");
        refused.severity = NotificationCenter::Warn;
        refused.kind     = NotificationCenter::Event;
        refused.title    = QStringLiteral("Shot from the phone was not recorded");
        nc.post(refused);
        checkInt("live before the dwell", nc.rowCount(), 2);

        g_now += NotificationCenter::kEventDwellMs + 1;
        nc.sweep();
        checkInt ("live after the dwell", nc.rowCount(), 1);
        checkBool("condition latched", nc.isLive(QStringLiteral("swing.save.no-directory")), true);
        checkBool("event expired", nc.isLive(QStringLiteral("shot.corroboration.refused")), false);

        std::printf("\n-- a condition ends when the world changes, not when a timer fires --\n");
        nc.resolve(QStringLiteral("swing.save.no-directory"));
        checkInt("live after resolve", nc.rowCount(), 0);
    }

    // ── The id is the key, and a post without one is refused ────────────────
    {
        g_now = 1'000'000;
        NotificationCenter nc;
        nc.setClockForTest([] { return g_now; });

        std::printf("\n-- separate ids are separate news --\n");
        NotificationCenter::Notification busy;
        busy.id    = QStringLiteral("shot.dropped.busy");
        busy.kind  = NotificationCenter::Event;
        busy.title = QStringLiteral("Shot from the phone was missed");

        NotificationCenter::Notification refused;
        refused.id    = QStringLiteral("shot.corroboration.refused");
        refused.kind  = NotificationCenter::Event;
        refused.title = QStringLiteral("Shot from the phone was not recorded");

        // 1 September's four refusals and two busy drops: one story each, not
        // one toast showing whichever landed last.
        for (int i = 0; i < 4; ++i) nc.post(refused);
        for (int i = 0; i < 2; ++i) nc.post(busy);
        checkInt("two rows", nc.rowCount(), 2);
        checkInt("refused count", nc.countOf(QStringLiteral("shot.corroboration.refused")), 4);
        checkInt("busy count",    nc.countOf(QStringLiteral("shot.dropped.busy")), 2);

        std::printf("\n-- a notification with no id is refused, not shown --\n");
        NotificationCenter::Notification anonymous;
        anonymous.title = QStringLiteral("something went wrong");
        nc.post(anonymous);
        checkInt("rows unchanged", nc.rowCount(), 2);
    }

    // ── The action seam ─────────────────────────────────────────────────────
    {
        g_now = 1'000'000;
        NotificationCenter nc;
        nc.setClockForTest([] { return g_now; });

        std::printf("\n-- the action names an id; the centre owns no navigation --\n");
        QString fired;
        QObject::connect(&nc, &NotificationCenter::actionRequested,
                         [&](const QString &a) { fired = a; });
        nc.post(saveFailed());
        nc.invokeAction(QStringLiteral("swing.save.no-directory"));
        checkStr("actionRequested", fired, QStringLiteral("settings.storage"));

        // Ordering: the first thing raised keeps the anchor position.
        NotificationCenter::Notification later;
        later.id    = QStringLiteral("shot.analysis.failed");
        later.kind  = NotificationCenter::Event;
        later.title = QStringLiteral("Shot analysis failed");
        nc.post(later);
        checkStr("row 0 is still the first raised",
                 nc.data(nc.index(0, 0), NotificationCenter::IdRole).toString(),
                 QStringLiteral("swing.save.no-directory"));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
