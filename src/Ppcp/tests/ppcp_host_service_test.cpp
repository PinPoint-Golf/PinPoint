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


// The pairing code's clock — work package H6, and the three defects that were
// in it.
//
// ⚠ WHY THIS FILE EXISTS AT ALL.  `PpcpHostService` had NO test.  It was
// covered by `ppcp_app_tu_syntax` (which only proves it compiles) and by
// `ppcp_conform_host` (which builds the peer, not the service), so every
// behaviour reachable only from QML was asserted by nothing.  The countdown the
// pairing panel displays was one of them, and it had never worked:
//
//   (a) `onTick()` opened with `if (!m_link) return`, so the whole code half —
//       the reap and the countdown — ran ONLY while a phone was already
//       connected.  That is the exact complement of the window in which a code
//       is displayed, so the number a user reads never moved.
//   (b) When a link WAS up, the 20 ms timer emitted `codeChanged` fifty times a
//       second, and a QR view repainting on that signal redrew ~1681 modules
//       each time.
//   (c) Nothing cleared `m_codeLive` at expiry, so the service went on
//       reporting a live code it would in fact refuse to honour (7.3e).
//
// Every test below asserts one of those WITH NO LINK PRESENT, because that is
// the state the bugs lived in and the state a user pairing a phone is in.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>

#include "ppcp_host_service.h"

namespace {

// A real event loop, spun for a real interval.  `m_timer` is a QTimer and the
// countdown is wall-clock seconds off `QDateTime::currentSecsSinceEpoch()`, so
// there is nothing here to fake: the assertions are about time passing.
void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

// A plain connect-and-count rather than QSignalSpy, so this suite needs no
// Qt6::Test — the only thing being counted is emissions.
//
// ⚠ IT DISCONNECTS ITSELF, AND THE FIRST VERSION DID NOT.  A functor connection
// made with no context object outlives the functor's captures, and `stop()`
// closes the live code on the way down (7.3b) — which emits `codeChanged` into
// a lambda whose `this` is a destroyed stack object.  That crashed in TearDown,
// after every assertion in the test had already passed, which is the most
// misleading place a fault can land.
class Counter
{
public:
    explicit Counter(PpcpHostService *svc)
        : m_conn(QObject::connect(svc, &PpcpHostService::codeChanged, [this] { ++m_n; }))
    {
    }
    ~Counter() { QObject::disconnect(m_conn); }

    Counter(const Counter &) = delete;
    Counter &operator=(const Counter &) = delete;

    int count() const { return m_n; }

private:
    QMetaObject::Connection m_conn;
    int                     m_n = 0;
};

// Port 0 — an ephemeral port.  7788 is what the application asks for and a
// stable port matters there (a persisted pairing reconnects to an endpoint),
// but a test that took it would fail on a machine already running the app.
class HostServiceClock : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QString err;
        ASSERT_TRUE(m_svc.start(0, &err)) << err.toStdString();
        ASSERT_TRUE(m_svc.listening());
        // No link is ever accepted in this fixture.  Nothing dials the
        // listener, so `connected()` stays false throughout — which is the
        // condition every assertion below is made under.
        ASSERT_FALSE(m_svc.connected());
    }

    void TearDown() override { m_svc.stop(); }

    PpcpHostService m_svc;
};

// (a) — the defect itself.  A published code counts down while the host waits
// for a phone, which is the only time anybody is looking at it.
TEST_F(HostServiceClock, TheCountdownRunsWhileTheHostIsStillWaitingForAPhone)
{
    ASSERT_TRUE(m_svc.publishPairingCode());
    ASSERT_TRUE(m_svc.codeLive());

    const int atPublish = m_svc.codeSecondsLeft();
    EXPECT_GT(atPublish, 0);

    Counter spy(&m_svc);
    spin(2500);

    EXPECT_FALSE(m_svc.connected()) << "no link was ever dialled";
    // Strictly less: with the early return in place this stayed put.
    EXPECT_LT(m_svc.codeSecondsLeft(), atPublish);
    EXPECT_GE(spy.count(), 2) << "codeChanged never fired without a link";
}

// (b) — once a second, not once a 20 ms tick.  Bounded generously: the point is
// the order of magnitude (3-4 vs ~125), not an exact count on a loaded machine.
//
// ⚠ WEAKER THAN THE OTHER TWO, AND STATED SO RATHER THAN LEFT TO BE FOUND.  The
// negative control for this file — reinstating the 23 Aug `onTick()` — turns (a)
// and (c) red and leaves THIS ROW GREEN, because the old early return emitted
// nothing at all without a link and zero is comfortably under the bound.  What
// it guards is the path this suite can reach: an emission that goes back to
// once-per-tick in the no-link state.  The fifty-a-second storm itself needed a
// live link, and nothing here dials one.
TEST_F(HostServiceClock, TheCountdownSignalsOnceASecondAndNotOncePerTick)
{
    ASSERT_TRUE(m_svc.publishPairingCode());

    Counter spy(&m_svc);
    spin(2500);

    EXPECT_LE(spy.count(), 8)
        << "codeChanged is firing per tick; a QR view redraws ~1681 modules on each";
}

// (c) — 7.3e: the publisher holds the authoritative clock, so a code it will no
// longer honour must stop being reported as live.
TEST_F(HostServiceClock, AnExpiredCodeStopsBeingLiveWithoutAnybodyPressingAnything)
{
    m_svc.setCodeLifetimeSecondsForTest(1);
    ASSERT_TRUE(m_svc.publishPairingCode());
    ASSERT_TRUE(m_svc.codeLive());

    spin(3000);

    EXPECT_FALSE(m_svc.codeLive())
        << "the service still displays a code whose handshake it would refuse";
    EXPECT_EQ(m_svc.codeSecondsLeft(), 0);
    EXPECT_EQ(m_svc.qrSize(), 0) << "the symbol outlived the code it encoded";
}

// The panel reads `qrRows`/`qrSize` and never the URI — RV 4.4c and 7.2b, and
// the header says the URI must not become a property.  Asserted here because
// "the code is displayable" is the precondition for everything above.
TEST_F(HostServiceClock, APublishedCodeIsDisplayableAsModulesAndCarriesItsEndpoints)
{
    ASSERT_TRUE(m_svc.publishPairingCode());

    EXPECT_GT(m_svc.qrSize(), 0);
    ASSERT_EQ(m_svc.qrRows().size(), m_svc.qrSize());
    for (const QVariant &row : m_svc.qrRows())
        EXPECT_EQ(row.toString().size(), m_svc.qrSize()) << "a ragged module grid";

    // 4.3d — the addresses the code carries.  A host with no route out has
    // none, which is a legitimate state on a locked-down builder, so this is a
    // shape assertion and not a count.
    for (const QString &ep : m_svc.codeEndpoints())
        EXPECT_TRUE(ep.contains(QLatin1Char(':'))) << ep.toStdString();
}

// 7.3b — a code is invalidated when it is displaced, used or not.  Publishing
// twice must not leave two live codes behind, and the second must be a fresh
// symbol (7.3d: a fresh psk and sid every time).
TEST_F(HostServiceClock, PublishingAgainReplacesTheCodeRatherThanAddingOne)
{
    ASSERT_TRUE(m_svc.publishPairingCode());
    const QVariantList first = m_svc.qrRows();

    ASSERT_TRUE(m_svc.publishPairingCode());
    EXPECT_TRUE(m_svc.codeLive());
    EXPECT_NE(m_svc.qrRows(), first) << "the same secret was encoded twice";

    m_svc.closePairingCode();
    EXPECT_FALSE(m_svc.codeLive());
}

// ── The device rows ─────────────────────────────────────────────────────────
//
// ⚠ ONLY THE NEGATIVE CASE IS REACHABLE HERE, AND THAT IS SAID RATHER THAN
// LEFT TO LOOK LIKE COVERAGE.  A row appears for a pairing that is persisted or
// spent; persisting needs protected storage (the login keychain, which cannot
// be unlocked from a non-Aqua session — the same reason RT-12 is a review row),
// and spending one needs a phone to dial in.  What IS reachable is the rule
// that most wants guarding, because it is the one a future edit will get wrong:
// a live code is not a phone.
TEST_F(HostServiceClock, ALiveCodeIsNotAPhone)
{
    EXPECT_TRUE(m_svc.phones().isEmpty()) << "a host that has paired with nothing has no phones";

    ASSERT_TRUE(m_svc.publishPairingCode());
    ASSERT_TRUE(m_svc.codeLive());

    // The QR on screen belongs to the pairing dialog.  Nobody has scanned it,
    // so there is no phone on the other end of it and the DEVICES list must not
    // claim there is.
    EXPECT_TRUE(m_svc.phones().isEmpty())
        << "an unscanned code was listed as a device";

    // And it does not become one by being thrown away, either: 7.3b closes the
    // session, so the entry goes rather than becoming a spent pairing.
    m_svc.closePairingCode();
    EXPECT_TRUE(m_svc.phones().isEmpty());
}

}  // namespace

int main(int argc, char **argv)
{
    // A QCoreApplication and not just gtest_main: the service's pump is a
    // QTimer, and a QTimer with no event loop behind it never fires.
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
