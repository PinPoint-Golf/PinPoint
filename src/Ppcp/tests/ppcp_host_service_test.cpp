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
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <ppcp/rv.h>

#include <cstring>

#include <ppcp/cbor.h>

#include <mutex>
#include <thread>

#include "ppcp_host_service.h"
#include "ppcp_rendezvous.h"
#include "ppcp_transport.h"
#include "ppcp_wired_link.h"

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

// ── Dialling this host for real ─────────────────────────────────────────────
// A phone, reduced to the part that matters here: decode a code, derive the
// keys, open a link.  Same four libppcp functions PinPointCapture uses, and the
// same `Ppcp::Connector` the conformance harness dials with — nothing about the
// connection below is simulated.
//
// It does NOT declare itself afterwards, which is deliberate: `onDeclare()` is
// what reaches `VideoInputPpcp`, and this suite links stubs for those (see
// ppcp_host_service_stubs.cpp).  Staying silent keeps the assertions about the
// thing under test — how many conversations this service is holding — instead
// of about a stub.
class Phone
{
public:
    explicit Phone(PpcpHostService *svc, std::uint64_t maxUses = 1)
    {
        Ppcp::PpcpRendezvous::Config cfg;
        cfg.displayName = "test";
        cfg.maxUses = maxUses;
        std::string err;
        m_ok = svc->rendezvous().publish(cfg, Ppcp::reachableEndpoints(svc->port()),
                                         nullptr, &m_code, &err);
        if (!m_ok) return;

        std::vector<std::uint8_t> scratch(PPCP_RV_MAX_PAYLOAD);
        ppcp_rv_payload payload;
        ppcp_rv_payload_init(&payload);
        m_ok = ppcp_rv_uri_decode(m_code.uri.c_str(), m_code.uri.size(),
                                  scratch.data(), scratch.size(), &payload) == PPCP_OK
            && ppcp_rv_derive(payload.sid, PPCP_RV_SID_BYTES,
                              payload.psk, payload.psk_len, &m_keys) == PPCP_OK;
    }

    bool ok() const { return m_ok; }
    const std::string &pairingId() const { return m_code.pairingId; }

    // 5.3a — a fresh rn2 per connection; the seed keeps two phones distinct.
    bool dial(std::uint16_t port, std::uint8_t seed)
    {
        std::uint8_t rn2[PPCP_RV_RN_BYTES];
        for (std::size_t i = 0; i < sizeof rn2; ++i)
            rn2[i] = static_cast<std::uint8_t>(seed + i * 31u);
        Ppcp::PskIdentity id(PPCP_RV_PSK_IDENTITY_BYTES);
        if (ppcp_rv_psk_identity(m_keys.k_id, rn2, id.data()) != PPCP_OK) return false;

        Ppcp::ConnectorConfig c;
        c.host = "127.0.0.1";
        c.port = port;
        std::memcpy(c.kTls.data(), m_keys.k_tls, PPCP_RV_KEY_BYTES);
        c.identity = id;
        Ppcp::HandshakeFailure f;
        // KEPT, not replaced: a `mu: 2` code is dialled twice and both links
        // have to stay up, or the second assertion would be measuring the
        // first link's destructor rather than the host's book-keeping.
        std::unique_ptr<Ppcp::PeerConnection> link = Ppcp::Connector::connect(c, &f);
        if (!link) return false;
        m_links.push_back(std::move(link));
        return true;
    }

    void hangUp() { if (!m_links.empty() && m_links.front()) m_links.front()->close(); }

private:
    bool                  m_ok = false;
    Ppcp::PublishedCode   m_code;
    ppcp_rv_keys          m_keys{};
    std::vector<std::unique_ptr<Ppcp::PeerConnection>> m_links;
};

// Port 0 — an ephemeral port.  7788 is what the application asks for and a
// stable port matters there (a persisted pairing reconnects to an endpoint),
// but a test that took it would fail on a machine already running the app.
class HostServiceClock : public ::testing::Test
{
protected:
    // ⚠ DECLARED BEFORE `m_svc`, AND THAT ORDERING IS LOAD-BEARING.  Data
    // members construct in declaration order, and `PpcpHostService`'s
    // constructor calls `loadPersisted()` immediately — before `SetUp()` ever
    // runs.  `ppSettings()` is UserScope, so without this redirect already
    // active by then, every fixture in this file would read the developer's
    // real PinPointStudio.ini at construction, and — since erratum E57 made a
    // completed pairing remembered automatically — any test below that
    // actually connects a phone (`Phone::dial()`) would WRITE a real PRK into
    // it too.  Same pattern as `SettingsPairingStore` in
    // ppcp_rendezvous_test.cpp, just fixture-ordered so it also covers the
    // read at construction.
    struct SettingsRedirect {
        QTemporaryDir dir;
        SettingsRedirect()
        {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        }
        ~SettingsRedirect()
        {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                               QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
        }
    } m_settingsRedirect;

    void SetUp() override
    {
        ASSERT_TRUE(m_settingsRedirect.dir.isValid());
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
// An expiring code RENEWS itself rather than stranding the panel behind a
// button.  It used to go dark and wait to be asked, which is a click only
// somebody already standing at this computer can make — friction bought with
// nothing, since 7.3 says plainly that `mu` and 7.3b are "clock-free and are
// the primary defence" while `exp` is "secondary rather than relied upon".
//
// ⚠ WHAT MUST STILL BE TRUE, and is the reason this is not simply a longer
// expiry: every renewal is a WHOLE new code.  7.3d — "a publisher generates
// fresh psk and sid for every code.  A code is never regenerated with the same
// secret" — so a renewed symbol MUST differ, and no individual code lives one
// second longer than it did before.
TEST_F(HostServiceClock, AnExpiringCodeRenewsItselfWithoutAnybodyPressingAnything)
{
    m_svc.setCodeLifetimeSecondsForTest(1);
    ASSERT_TRUE(m_svc.publishPairingCode());
    ASSERT_TRUE(m_svc.codeLive());
    const QVariantList first = m_svc.qrRows();
    ASSERT_FALSE(first.isEmpty());

    spin(3000);   // three lifetimes: it has to have come round at least twice

    EXPECT_TRUE(m_svc.codeLive())
        << "the panel went dark and waited to be asked";
    EXPECT_GT(m_svc.qrSize(), 0) << "the symbol outlived the code it encoded";
    EXPECT_NE(m_svc.qrRows(), first)
        << "RV 7.3d — the same secret was displayed again";
    EXPECT_GT(m_svc.codeSecondsLeft(), 0) << "the countdown did not reset";
    EXPECT_LE(m_svc.codeSecondsLeft(), 1) << "a renewal outlived its own lifetime";
}

// The other half of the same rule: renewal is tied to the PANEL, not to the
// clock running for ever.  Dismissing the code stops it dead — 7.3b — and
// nothing brings it back on its own.
TEST_F(HostServiceClock, AClosedCodeIsNotRenewed)
{
    m_svc.setCodeLifetimeSecondsForTest(1);
    ASSERT_TRUE(m_svc.publishPairingCode());
    m_svc.closePairingCode();
    ASSERT_FALSE(m_svc.codeLive());

    spin(2500);

    EXPECT_FALSE(m_svc.codeLive()) << "a dismissed code came back by itself";
    EXPECT_EQ(m_svc.qrSize(), 0);
}

// A code a phone has used is a picture of a used ticket: `mu` is 1, so it can
// pair nothing else.  The panel must not go on showing it.
//
// ⚠ AND THE REPLACEMENT MUST NOT CLOSE THE SESSION THE PHONE IS ON.
// closeSession() wipes K_tls unless the pairing was persisted, and the live
// link still needs it — 7.5a reconnects a dropped channel on it and ENC 2.1d
// opens the preview channel on it later.  7.3f is explicit that `mu` and 7.3b
// invalidate the CODE, not the pairings already established from it.  This
// fixture cannot adopt a link, so what is asserted here is the rule the
// decision is made from: a code with no live pairing behind it IS closed
// properly, keys and all.
TEST_F(HostServiceClock, ReplacingAnUnusedCodeStillInvalidatesItProperly)
{
    ASSERT_TRUE(m_svc.publishPairingCode());
    ASSERT_EQ(m_svc.outstandingCodes().size(), 1);
    const QString firstPairing =
        m_svc.outstandingCodes().first().toMap()
             .value(QStringLiteral("pairingId")).toString();
    ASSERT_FALSE(firstPairing.isEmpty());

    ASSERT_TRUE(m_svc.publishPairingCode());

    // The displaced one is either gone (reaped) or on record as invalidated.
    // Either way it must not still be offering a use.
    for (const QVariant &v : m_svc.outstandingCodes()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("pairingId")).toString() != firstPairing) continue;
        EXPECT_TRUE(m.value(QStringLiteral("invalidated")).toBool())
            << "the displaced code was left usable";
        EXPECT_EQ(m.value(QStringLiteral("usesRemaining")).toULongLong(), 0u);
    }
}

// A renewal is not an answer to "why did my phone not connect".  The failure
// text has to survive the clock coming round, or a refusal 30 seconds before
// an expiry would be deleted before anybody read it.
TEST_F(HostServiceClock, RenewalKeepsTheLastFailureButAskingForANewCodeClearsIt)
{
    m_svc.setCodeLifetimeSecondsForTest(1);
    ASSERT_TRUE(m_svc.publishPairingCode());

    Ppcp::HandshakeFailure f;
    f.kind = Ppcp::FailureKind::HandshakeTimeout;
    m_svc.noteHandshakeFailureForTest(f);
    const QString said = m_svc.lastFailureText();
    ASSERT_FALSE(said.isEmpty());

    spin(2500);   // renews underneath it

    ASSERT_TRUE(m_svc.codeLive());
    EXPECT_EQ(m_svc.lastFailureText(), said)
        << "an automatic renewal deleted the refusal nobody had read yet";

    ASSERT_TRUE(m_svc.publishPairingCode());
    EXPECT_TRUE(m_svc.lastFailureText().isEmpty())
        << "asking for a new code should start clean";
}

// ── Two phones at once ──────────────────────────────────────────────────────
// Down-the-line and face-on are two phones, so this is the core case and not an
// edge one.  It used to be impossible: adoptLink() closed any link that arrived
// while one was held, so the second angle handshook perfectly and was dropped.
//
// ⚠ WHAT MAKES IT WORK IS ONE PEER PER PHONE, and that was already the law.
// F-H8-5 found that a `ppcp_peer` is the CONVERSATION, not the application —
// one engine shared across links kept the previous device's Session and refused
// every device after the first with `ppcp_peer_session_open: invalid argument`.
// Nothing below this class ever assumed one phone: the transport assembles
// concurrent links by `link_id` (ENC 2.1, pinned by ppcp_link_bind_test) and
// `VideoInputPpcp` has always been keyed by peer id.
TEST_F(HostServiceClock, TwoPhonesConnectAtOnceAndAreHeldSeparately)
{
    Phone dtl(&m_svc), faceOn(&m_svc);
    ASSERT_TRUE(dtl.ok());
    ASSERT_TRUE(faceOn.ok());

    ASSERT_TRUE(dtl.dial(m_svc.port(), 1)) << "the first phone could not connect";
    for (int i = 0; i < 200 && m_svc.connectedCount() < 1; ++i) spin(10);
    ASSERT_EQ(m_svc.connectedCount(), 1);

    ASSERT_TRUE(faceOn.dial(m_svc.port(), 2)) << "the second phone could not connect";
    for (int i = 0; i < 200 && m_svc.connectedCount() < 2; ++i) spin(10);

    EXPECT_EQ(m_svc.connectedCount(), 2)
        << "the second angle was refused — this is the DTL + face-on case";
    EXPECT_TRUE(m_svc.connected());

    // Two DIFFERENT pairings, each held on its own.  One phone dialling twice
    // would be a reconnection and must NOT read as two.
    EXPECT_NE(dtl.pairingId(), faceOn.pairingId());
}

// One angle dropping out mid-session must not take the other with it.
TEST_F(HostServiceClock, OnePhoneLeavingLeavesTheOtherConnected)
{
    Phone a(&m_svc), b(&m_svc);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(a.dial(m_svc.port(), 3));
    ASSERT_TRUE(b.dial(m_svc.port(), 4));
    for (int i = 0; i < 200 && m_svc.connectedCount() < 2; ++i) spin(10);
    ASSERT_EQ(m_svc.connectedCount(), 2);

    a.hangUp();
    for (int i = 0; i < 400 && m_svc.connectedCount() > 1; ++i) spin(10);

    EXPECT_EQ(m_svc.connectedCount(), 1)
        << "a dropped link took the other phone down with it, or was never noticed";
    EXPECT_TRUE(m_svc.connected());
}

// ⚠ TWO PHONES ON ONE `mu: 2` CODE ARE TWO PHONES, not one that reconnected.
// They share a pairing id — one code, one pairing, several devices, which
// §7.3's rationale calls "a real workflow" — so anything that keyed a
// connection by pairing id would take the first phone down when the second
// scanned.  A pairing is not a phone; a link is.  This application only ever
// publishes `mu: 1`, so the case is not reachable from its own panel; it is
// pinned here because the tempting "dedupe by pairing" shortcut looks correct
// right up until somebody raises `mu`.
TEST_F(HostServiceClock, TwoDevicesSharingOneMultiUseCodeAreStillTwoConnections)
{
    Phone shared(&m_svc, /*maxUses=*/2);
    ASSERT_TRUE(shared.ok());

    ASSERT_TRUE(shared.dial(m_svc.port(), 7));
    for (int i = 0; i < 200 && m_svc.connectedCount() < 1; ++i) spin(10);
    ASSERT_EQ(m_svc.connectedCount(), 1);

    ASSERT_TRUE(shared.dial(m_svc.port(), 8)) << "the code's second use was refused";
    for (int i = 0; i < 200 && m_svc.connectedCount() < 2; ++i) spin(10);

    EXPECT_EQ(m_svc.connectedCount(), 2)
        << "the second device collapsed onto the first because they share a pairing";
}

// 7.3a — and the use after that is refused, `mu` being 2.  The counterpart to
// the test above: sharing a pairing must not mean sharing it for ever.
TEST_F(HostServiceClock, AMultiUseCodeIsStillSpentOnceItsUsesAreGone)
{
    Phone shared(&m_svc, /*maxUses=*/2);
    ASSERT_TRUE(shared.ok());
    ASSERT_TRUE(shared.dial(m_svc.port(), 9));
    for (int i = 0; i < 200 && m_svc.connectedCount() < 1; ++i) spin(10);
    ASSERT_TRUE(shared.dial(m_svc.port(), 10));
    for (int i = 0; i < 200 && m_svc.connectedCount() < 2; ++i) spin(10);
    ASSERT_EQ(m_svc.connectedCount(), 2);

    EXPECT_FALSE(shared.dial(m_svc.port(), 11))
        << "RV 7.3a — a code with no uses left still paired something";
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
// The rule that most wants guarding, because it is the one a future edit will
// get wrong: a live code is not a phone.  The positive case — a row DOES
// appear once a phone actually dials in, and it is remembered automatically
// (E57) — is `APhoneThatDialsInBecomesARememberedDeviceRow` below; this test
// is the negative control for it, run before any phone exists at all.
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

// The positive case this file could not exercise before erratum E57: a phone
// that actually dials in is remembered with no separate action taken, and
// "Forget" (7.4d) is what is left to opt out with.
TEST_F(HostServiceClock, APhoneThatDialsInBecomesARememberedDeviceRow)
{
    Phone p(&m_svc);
    ASSERT_TRUE(p.ok());
    ASSERT_TRUE(p.dial(m_svc.port(), 0x61));
    for (int i = 0; i < 200 && m_svc.connectedCount() < 1; ++i) spin(10);
    ASSERT_EQ(m_svc.connectedCount(), 1);

    const QString pairingId = QString::fromStdString(p.pairingId());
    QVariantList rows = m_svc.phones();
    ASSERT_EQ(rows.size(), 1);
    QVariantMap row = rows.first().toMap();
    EXPECT_EQ(row.value(QStringLiteral("pairingId")).toString(), pairingId);
    EXPECT_TRUE(row.value(QStringLiteral("persisted")).toBool())
        << "E57 — a completed pairing is remembered automatically, with no "
           "'Remember' action taken";
    EXPECT_FALSE(row.value(QStringLiteral("invalidated")).toBool());
    EXPECT_TRUE(m_svc.rendezvous().isPersisted(p.pairingId()));

    // "Forget" is still the individual opt-out 7.4b requires (7.4d) — E57
    // removed the opt-IN, not this.
    m_svc.forgetPairing(pairingId);
    EXPECT_FALSE(m_svc.rendezvous().isPersisted(p.pairingId()));
    rows = m_svc.phones();
    ASSERT_EQ(rows.size(), 1);
    row = rows.first().toMap();
    EXPECT_TRUE(row.value(QStringLiteral("invalidated")).toBool())
        << "a forgotten phone is shown as revoked, not silently dropped from the list";
}

// ── RV §3 discovery ─────────────────────────────────────────────────────────
//
// ⚠ WHAT CAN BE ASSERTED HERE IS THAT THE BROWSER IS WIRED, AND NOT THAT IT
// FINDS ANYTHING.  There is nothing on a build machine's network advertising
// `_ppcp._tcp`, which is why docs/ppcp-conformance.md §9.4 records
// `DNSServiceBrowse` as unexercised; `parseTxtRecord`, `pvAcceptsMajor`,
// `instanceNameMatchesRid` and `decideDial` are covered by
// ppcp_rendezvous_test. What was missing until now was a CALLER — the browser
// was built, tested and constructed nowhere outside that suite.
TEST_F(HostServiceClock, DiscoveryIsWiredUpAndSaysWhatItIs)
{
    const QString d = m_svc.discoveryDescription();
    EXPECT_FALSE(d.isEmpty());
#if defined(__APPLE__)
    // makePlatformBrowser() returns a BonjourBrowser here, and start() talks to
    // the system mDNSResponder over its local IPC socket.
    EXPECT_TRUE(d.contains(QStringLiteral("browse only"))) << d.toStdString();
#endif
    // 3.6a — whatever discovery did or did not do, it is not an error and does
    // not reach the user-facing status line.
    EXPECT_FALSE(m_svc.status().contains(QStringLiteral("discovery"), Qt::CaseInsensitive));
}

// RT-9 — a diagnostic export carries no secret and no payload, and the two
// fields discovery added are a build fact and a count.
TEST_F(HostServiceClock, TheDiagnosticExportGainsDiscoveryAndStillCarriesNoSecret)
{
    ASSERT_TRUE(m_svc.publishPairingCode());
    const QString dump = m_svc.diagnosticExport();

    EXPECT_TRUE(dump.contains(QStringLiteral("discovery:"))) << dump.toStdString();
    EXPECT_TRUE(dump.contains(QStringLiteral("discovered-pairings: 0")));
    // The URI is the one thing that must never leave C++ (RV 4.4c, 7.2b), and a
    // pairing code URI is a `ppcp://` one.
    EXPECT_FALSE(dump.contains(QStringLiteral("ppcp://"))) << dump.toStdString();
    EXPECT_FALSE(dump.contains(QStringLiteral("psk")));
}

// ── A phone that arrived and did not become a link ─────────────────────────
// The panel's whole failure vocabulary, driven through the test seam because
// this fixture cannot accept a link — see ppcp_host_service_stubs.cpp, which
// stubs the three `src/Video` symbols on exactly that basis.
TEST_F(HostServiceClock, AFailedArrivalIsReportedAndNamedWhereTheSpecAllows)
{
    ASSERT_TRUE(m_svc.publishPairingCode());
    EXPECT_EQ(m_svc.failureCount(), 0);
    EXPECT_TRUE(m_svc.lastFailureText().isEmpty());

    // (a) The uniform one.  It must NOT name a cause — there is none to name —
    // but it must still say a phone was here, and carry the alert, which is the
    // only thing that distinguishes one failing phone from the next.
    Ppcp::HandshakeFailure uniform;
    uniform.kind = Ppcp::FailureKind::Handshake;
    uniform.message = "PPCP TLS handshake failed";
    uniform.alert = 40;
    uniform.alertWasSent = true;
    uniform.elapsedMs = 214.0;
    m_svc.noteHandshakeFailureForTest(uniform);

    EXPECT_EQ(m_svc.failureCount(), 1);
    const QString a = m_svc.lastFailureText();
    EXPECT_FALSE(a.isEmpty());
    EXPECT_TRUE(a.contains(QStringLiteral("40"))) << a.toStdString();
    // ⚠ RV 5.3c / 7.7c.  If either of these words ever appears, somebody has
    // taught the screen to tell an unknown identity from a wrong key.
    EXPECT_FALSE(a.contains(QStringLiteral("identity"), Qt::CaseInsensitive)) << a.toStdString();
    EXPECT_FALSE(a.contains(QStringLiteral("key"), Qt::CaseInsensitive)) << a.toStdString();
    EXPECT_FALSE(a.contains(QStringLiteral("pairing"), Qt::CaseInsensitive)) << a.toStdString();

    // (b) A repeat of the SAME failure still moves the count, because the text
    // cannot change and a QML binding on the text alone would not re-evaluate.
    m_svc.noteHandshakeFailureForTest(uniform);
    EXPECT_EQ(m_svc.failureCount(), 2);
    EXPECT_EQ(m_svc.lastFailureText(), a) << "the same failure changed its words";

    // (c) The nameable ones are named.  These are policy and framing outcomes,
    // not the pair of outcomes 7.7c holds together.
    Ppcp::HandshakeFailure late;
    late.kind = Ppcp::FailureKind::HandshakeTimeout;
    m_svc.noteHandshakeFailureForTest(late);
    EXPECT_EQ(m_svc.failureCount(), 3);
    EXPECT_NE(m_svc.lastFailureText(), a) << "a timeout read as the uniform failure";
    EXPECT_TRUE(m_svc.lastFailureText().contains(QStringLiteral("time"), Qt::CaseInsensitive))
        << m_svc.lastFailureText().toStdString();

    Ppcp::HandshakeFailure fs;
    fs.kind = Ppcp::FailureKind::NotForwardSecret;
    m_svc.noteHandshakeFailureForTest(fs);
    EXPECT_TRUE(m_svc.lastFailureText().contains(QStringLiteral("forward secret"),
                                                 Qt::CaseInsensitive))
        << m_svc.lastFailureText().toStdString();

    // (d) `None` is not a failure and must not be reported as one — it is what
    // an ordinary idle accept() leaves behind, fifty times a second.
    const int before = m_svc.failureCount();
    m_svc.noteHandshakeFailureForTest(Ppcp::HandshakeFailure{});
    EXPECT_EQ(m_svc.failureCount(), before) << "an idle poll was reported as a failure";
}

// A fresh code is a fresh attempt, so the last one's refusal stops being the
// answer to "what is happening".
TEST_F(HostServiceClock, AskingForANewCodeClearsTheLastFailure)
{
    ASSERT_TRUE(m_svc.publishPairingCode());
    Ppcp::HandshakeFailure f;
    f.kind = Ppcp::FailureKind::HandshakeTimeout;
    m_svc.noteHandshakeFailureForTest(f);
    ASSERT_FALSE(m_svc.lastFailureText().isEmpty());

    ASSERT_TRUE(m_svc.publishPairingCode());
    EXPECT_TRUE(m_svc.lastFailureText().isEmpty())
        << "a new code still carried the old code's failure";
}


// ════════════════════════════════════════════════════════════════════════════
//  THE WIRED PATH — Phase 1 contracts C2, C3 and C6
// ════════════════════════════════════════════════════════════════════════════
//
// ⚠ WHAT THIS SUITE CAN AND CANNOT REACH.  The stub usbmuxd's tunnel is a byte
// echo, not a PPCP listener, so a whole-path wired dial (usbmux Connect →
// presence read → resolve → TLS) is not assertable here and is M1's job on
// hardware.  What IS assertable is every decision the host makes on its own:
// the C3 reader against each of its refusal rules, first-match-wins resolution,
// and — the one that would be silent and expensive in production — C2's rule
// that a link WE dialled must not spend a pairing code.

// ── A record builder, written independently of the reader ──────────────────
//
// ⚠ It uses PPCP_CBOR_ORDER_LITERAL rather than the writer's default, and that
// is not incidental: C3 emits `pv, role, dl, peers`, which is NOT deterministic
// key order (that would be `dl, pv, role, peers` — length first, then bytes).
// A test that could only build canonically ordered maps could not build the
// record the contract actually specifies.
struct PresenceSpec {
    std::string pv   = "1.0";
    std::string role = "capture";
    bool        hasDl = true;
    std::string dl   = "Mark's iPhone";
    bool        hasPeers = true;
    int         peers = 1;
    std::size_t identityBytes = 17;
    bool        unknownTopKey  = false;
    bool        unknownPeerKey = false;
    bool        reorder = false;   // peers, dl, role, pv — C3: "any order"
};

std::vector<unsigned char> buildPresence(const PresenceSpec &sp)
{
    std::vector<std::uint8_t> buf(16384);
    ppcp_cbor_writer w{};
    ppcp_cbor_writer_init_order(&w, buf.data(), buf.size(), PPCP_CBOR_ORDER_LITERAL);

    std::size_t fields = 2;                       // pv, role
    if (sp.hasDl)         ++fields;
    if (sp.hasPeers)      ++fields;
    if (sp.unknownTopKey) ++fields;
    ppcp_cbor_write_map(&w, fields);

    auto writePv   = [&] { ppcp_cbor_write_text_z(&w, "pv");
                           ppcp_cbor_write_text(&w, sp.pv.data(), sp.pv.size()); };
    auto writeRole = [&] { ppcp_cbor_write_text_z(&w, "role");
                           ppcp_cbor_write_text(&w, sp.role.data(), sp.role.size()); };
    auto writeDl   = [&] { if (!sp.hasDl) return;
                           ppcp_cbor_write_text_z(&w, "dl");
                           ppcp_cbor_write_text(&w, sp.dl.data(), sp.dl.size()); };
    auto writePeers = [&] {
        if (!sp.hasPeers) return;
        ppcp_cbor_write_text_z(&w, "peers");
        ppcp_cbor_write_array(&w, static_cast<std::size_t>(sp.peers));
        for (int i = 0; i < sp.peers; ++i) {
            ppcp_cbor_write_map(&w, sp.unknownPeerKey ? 3u : 2u);
            ppcp_cbor_write_text_z(&w, "port");
            ppcp_cbor_write_uint(&w, static_cast<std::uint64_t>(51000 + i));
            ppcp_cbor_write_text_z(&w, "psk_identity");
            std::vector<std::uint8_t> id(sp.identityBytes);
            for (std::size_t b = 0; b < id.size(); ++b)
                id[b] = static_cast<std::uint8_t>(b == 0 ? 0x01 : (i + 1) * 16 + b);
            ppcp_cbor_write_bytes(&w, id.data(), id.size());
            if (sp.unknownPeerKey) {
                ppcp_cbor_write_text_z(&w, "future");
                ppcp_cbor_write_uint(&w, 7);
            }
        }
    };
    auto writeUnknown = [&] {
        if (!sp.unknownTopKey) return;
        ppcp_cbor_write_text_z(&w, "zz_future");
        ppcp_cbor_write_array(&w, 2);
        ppcp_cbor_write_uint(&w, 1);
        ppcp_cbor_write_text_z(&w, "whatever");
    };

    if (sp.reorder) { writePeers(); writeUnknown(); writeDl(); writeRole(); writePv(); }
    else            { writePv(); writeRole(); writeDl(); writeUnknown(); writePeers(); }

    std::size_t n = 0;
    if (ppcp_cbor_writer_finish(&w, &n) != PPCP_OK) return {};
    return std::vector<unsigned char>(buf.begin(), buf.begin() + n);
}

bool parses(const std::vector<unsigned char> &b, Ppcp::WiredPresence *out = nullptr,
            std::string *why = nullptr)
{
    Ppcp::WiredPresence scratch;
    std::string scratchWhy;
    return Ppcp::parseWiredPresence(b.data(), b.size(), out ? out : &scratch,
                                    why ? why : &scratchWhy);
}

std::vector<unsigned char> fromHex(const std::string &hex)
{
    std::vector<unsigned char> out;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<unsigned char>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    return out;
}

// ── The cross-repo fixtures: what PinPointCapture's encoder actually emits ─
//
// Handed over by the PPC agent on 29 Aug 2026 and independently decoded before
// being handed over — not reconstructed here from the schema, which is the only
// kind of fixture that can catch two implementations agreeing with the document
// and not with each other.
//
// ⚠ C3's key order was AMENDED on 29 Aug from `pv, role, dl, peers` to
// `dl, pv, role, peers`, because the original does not sort under ENC 4e
// (bytewise over the ENCODED key: `62 64 6c` < `62 70 76` < `64 …` < `65 …`)
// and libppcp's writer enforces 4e.  ✅ It cost this reader nothing, because
// C3 always required keys to be accepted in ANY order — and fixtures 1 and 3
// below are what turns "required" into "tested".
//
// ⚠ AND FIXTURE 2 CANNOT STAND IN FOR THEM.  An unlabelled record encodes
// identically under either rule, which is precisely how the ordering bug
// survived on the device side; only 1 and 3 together exercise the requirement.

// Fixture 1 — two pairings, `dl` present, the AMENDED (ENC 4e) order.  125 bytes.
const char *kPpcFixtureDeterministic =
    "a462646c6d4d61726b2773206950686f6e6562707663312e3064726f6c6567636170747572"
    "6565706565727382a264706f727419c3506c70736b5f6964656e7469747951010f1e2d3c4b"
    "5a6978b355ada60b4b5aa8a264706f727419c0006c70736b5f6964656e746974795101ffee"
    "ddccbbaa99887766554433221100";

// Fixture 2 — the same device with no label.  68 bytes, and the map header is
// `a3`: `dl` is ABSENT, never `null` (C3, and ENC 4c would refuse a null).
const char *kPpcFixtureNoLabel =
    "a362707663312e3064726f6c65676361707475726565706565727381a264706f727419c350"
    "6c70736b5f6964656e7469747951010f1e2d3c4b5a6978b355ada60b4b5aa8";

// Fixture 3 — the OLD key order (`pv, role, dl, peers`), which this reader must
// ALSO parse.  Free proof of the order-agnostic rule, at no cost.
const char *kPpcFixtureLegacyOrder =
    "a462707663312e3064726f6c65676361707475726562646c6d4d61726b2773206950686f6e65"
    "65706565727382a264706f727419c3506c70736b5f6964656e7469747951010f1e2d3c4b5a69"
    "78b355ada60b4b5aa8a264706f727419c0006c70736b5f6964656e746974795101ffeeddccbb"
    "aa99887766554433221100";

// ⛔ RV 5.3f — the identity is 17 raw octets and is NOT text.  The first of
// these is not valid UTF-8 and the second ENDS IN A ZERO BYTE; anything that
// transcoded, validated as text or ran strlen over them would lose the last
// byte of one and refuse the other.  A hand-written all-ASCII fixture would
// never catch it, which is why these bytes are the device's and not ours.
const char *kPpcIdentityA = "010f1e2d3c4b5a6978b355ada60b4b5aa8";
const char *kPpcIdentityB = "01ffeeddccbbaa99887766554433221100";

void expectTwoPairingFixture(const char *hex, const char *which)
{
    Ppcp::WiredPresence rec;
    std::string why;
    const std::vector<unsigned char> bytes = fromHex(hex);
    ASSERT_EQ(bytes.size(), 125u) << which;
    ASSERT_TRUE(parses(bytes, &rec, &why)) << which << ": " << why;

    EXPECT_EQ(rec.pv, "1.0") << which;
    EXPECT_EQ(rec.role, "capture") << which;
    EXPECT_EQ(rec.displayLabel, "Mark's iPhone") << which;
    ASSERT_EQ(rec.peers.size(), 2u) << which;

    EXPECT_EQ(rec.peers[0].port, 50000) << which;
    EXPECT_EQ(rec.peers[1].port, 49152) << which;
    EXPECT_EQ(rec.peers[0].identity, fromHex(kPpcIdentityA)) << which;
    EXPECT_EQ(rec.peers[1].identity, fromHex(kPpcIdentityB)) << which;
    EXPECT_EQ(rec.peers[1].identity.back(), 0x00) << which;
    EXPECT_EQ(rec.peers[0].identity.front(), 0x01) << which;   // RV 5.3a
}

TEST(WiredPresenceRecord, ThePinPointCaptureFixtureParsesInEitherKeyOrder)
{
    expectTwoPairingFixture(kPpcFixtureDeterministic, "fixture 1 (ENC 4e order)");
    expectTwoPairingFixture(kPpcFixtureLegacyOrder,   "fixture 3 (C3's original order)");

    // ⚠ Deliberately NOT a substitute for the pair above — see the note on the
    // fixtures.  What it does prove is the absent-`dl` shape: a three-key map,
    // no label, and a record that is complete without one.
    const std::vector<unsigned char> plain = fromHex(kPpcFixtureNoLabel);
    ASSERT_EQ(plain.size(), 68u);
    EXPECT_EQ(plain[0], 0xa3) << "dl is absent, so the map has three keys";
    Ppcp::WiredPresence rec;
    std::string why;
    ASSERT_TRUE(parses(plain, &rec, &why)) << why;
    EXPECT_TRUE(rec.displayLabel.empty());
    ASSERT_EQ(rec.peers.size(), 1u);
    EXPECT_EQ(rec.peers[0].port, 50000);
    EXPECT_EQ(rec.peers[0].identity, fromHex(kPpcIdentityA));
}

// ── C3, the accepting cases ────────────────────────────────────────────────
TEST(WiredPresenceRecord, AGoodRecordIsRead)
{
    Ppcp::WiredPresence rec;
    ASSERT_TRUE(parses(buildPresence({}), &rec));
    EXPECT_EQ(rec.pv, "1.0");
    EXPECT_EQ(rec.role, "capture");
    ASSERT_EQ(rec.peers.size(), 1u);
    EXPECT_EQ(rec.peers[0].port, 51000);
    EXPECT_EQ(rec.peers[0].identity.size(), 17u);
    // RV 4.4d — the label is display text from an untrusted counterpart and is
    // sanitised at the boundary, never used as a key.
    EXPECT_FALSE(rec.displayLabel.empty());
}

// Forward compatibility, which is the whole reason C3 says "any order" and
// "ignore unknown": a capture app that adds a field must not stop working with
// a host that has not been rebuilt.
TEST(WiredPresenceRecord, KeysInAnUnexpectedOrderWithAnUnknownKeyAreStillRead)
{
    PresenceSpec sp;
    sp.reorder = true;
    sp.unknownTopKey = true;
    sp.unknownPeerKey = true;
    Ppcp::WiredPresence rec;
    std::string why;
    ASSERT_TRUE(parses(buildPresence(sp), &rec, &why)) << why;
    EXPECT_EQ(rec.pv, "1.0");
    ASSERT_EQ(rec.peers.size(), 1u);
    EXPECT_EQ(rec.peers[0].identity.size(), 17u);
}

// C3: `dl` is OMITTED ENTIRELY when absent — never `null`.  (A `null` would be
// refused by ENC 4c before this reader saw it, which is the belt to this brace.)
TEST(WiredPresenceRecord, ARecordWithNoLabelIsOrdinary)
{
    PresenceSpec sp;
    sp.hasDl = false;
    const std::vector<unsigned char> bytes = buildPresence(sp);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes[0], 0xa3) << "three keys, so a three-entry map";
    Ppcp::WiredPresence rec;
    std::string why;
    ASSERT_TRUE(parses(bytes, &rec, &why)) << why;
    EXPECT_TRUE(rec.displayLabel.empty());
    EXPECT_EQ(rec.peers.size(), 1u);
}

// RV 3.3a is about MAJOR, so a device on a newer MINOR is still ours.
TEST(WiredPresenceRecord, ANewerMinorIsAcceptedAndANewerMajorIsNot)
{
    PresenceSpec ok;   ok.pv = "1.7";
    EXPECT_TRUE(parses(buildPresence(ok)));

    PresenceSpec no;   no.pv = "2.0";
    std::string why;
    EXPECT_FALSE(parses(buildPresence(no), nullptr, &why));
    EXPECT_FALSE(why.empty());
}

// ── C3, the refusals.  Every one of them is SILENCE plus one log line: the
// device is treated as not wired, which is the state a charge-only cable is in
// anyway (design §6.2, RV 3.6a).
TEST(WiredPresenceRecord, PeersAbsentOrEmptyIsRefused)
{
    PresenceSpec absent; absent.hasPeers = false;
    EXPECT_FALSE(parses(buildPresence(absent)));

    PresenceSpec empty;  empty.peers = 0;
    EXPECT_FALSE(parses(buildPresence(empty)));
}

// The cap is 16 and it is checked BEFORE the entries are read, so a hostile
// record cannot make this host do the work of refusing it.
TEST(WiredPresenceRecord, SeventeenListenersIsRefusedAndSixteenIsNot)
{
    PresenceSpec sixteen;   sixteen.peers = 16;
    EXPECT_TRUE(parses(buildPresence(sixteen)));

    PresenceSpec seventeen; seventeen.peers = 17;
    EXPECT_FALSE(parses(buildPresence(seventeen)));
}

// RV 5.3f forbids transcoding or truncating an identity, so a wrong length is a
// refusal and never something to pad or cut.
TEST(WiredPresenceRecord, AnIdentityThatIsNotSeventeenBytesIsRefused)
{
    PresenceSpec shortId; shortId.identityBytes = 16;
    EXPECT_FALSE(parses(buildPresence(shortId)));

    PresenceSpec longId;  longId.identityBytes = 18;
    EXPECT_FALSE(parses(buildPresence(longId)));
}

TEST(WiredPresenceRecord, ARecordOverTheCapIsRefused)
{
    PresenceSpec big;
    big.dl = std::string(5000, 'a');
    const std::vector<unsigned char> bytes = buildPresence(big);
    ASSERT_GT(bytes.size(), Ppcp::kWiredPresenceMaxBytes);
    std::string why;
    EXPECT_FALSE(parses(bytes, nullptr, &why));
    EXPECT_FALSE(why.empty());
}

// ⛔ THERE IS NO FRAMING, so a short read is indistinguishable from a record
// and must be refused as one.  This is the case a reader that trusted its own
// buffer length would get wrong.
TEST(WiredPresenceRecord, ATruncatedReadIsRefusedAsAParseFailure)
{
    const std::vector<unsigned char> whole = buildPresence({});
    ASSERT_GT(whole.size(), 8u);
    for (std::size_t cut : {whole.size() / 2, whole.size() - 1, std::size_t(0)}) {
        std::vector<unsigned char> part(whole.begin(), whole.begin() + cut);
        EXPECT_FALSE(parses(part)) << "a " << cut << "-byte prefix was accepted";
    }
}

// ── RV 5.3b, run client-side: FIRST MATCH WINS ─────────────────────────────
TEST(WiredPresenceRecord, ResolutionTakesTheFirstEntryThatResolves)
{
    PresenceSpec sp; sp.peers = 4;
    Ppcp::WiredPresence rec;
    ASSERT_TRUE(parses(buildPresence(sp), &rec));
    ASSERT_EQ(rec.peers.size(), 4u);

    int calls = 0;
    // Entries 2 and 3 both resolve; the contract says the FIRST of them wins,
    // and that nothing after it is even offered.
    Ppcp::IdentityResolver r = [&](const unsigned char *id, std::size_t len,
                                   Ppcp::ResolvedPairing &out) {
        ++calls;
        if (len != 17) return false;
        // buildPresence() stamps byte 1 of entry i as (i + 1) * 16 + 1.
        if (id[1] != 0x31 && id[1] != 0x41) return false;   // entries 2 and 3
        out.pairingId = (id[1] == 0x31) ? "pair-two" : "pair-three";
        return true;
    };

    std::size_t which = 99;
    Ppcp::ResolvedPairing got;
    ASSERT_TRUE(Ppcp::resolveFirstWiredPeer(rec, r, &which, &got));
    EXPECT_EQ(which, 2u);
    EXPECT_EQ(got.pairingId, "pair-two");
    EXPECT_EQ(calls, 3) << "the resolver kept going after the first match";
}

// ⛔ RV 3.4c — a phone this host is not paired with is not a failure.  It is a
// phone that belongs to somebody else, and the answer is silence.
TEST(WiredPresenceRecord, NothingResolvingIsSilenceAndNotAnError)
{
    PresenceSpec sp; sp.peers = 3;
    Ppcp::WiredPresence rec;
    ASSERT_TRUE(parses(buildPresence(sp), &rec));

    Ppcp::IdentityResolver never = [](const unsigned char *, std::size_t,
                                      Ppcp::ResolvedPairing &) { return false; };
    std::size_t which = 99;
    Ppcp::ResolvedPairing got;
    EXPECT_FALSE(Ppcp::resolveFirstWiredPeer(rec, never, &which, &got));
    EXPECT_TRUE(got.pairingId.empty());
}

// The gate is off unless somebody asks for it, and the code says so out loud.
TEST(WiredPresenceRecord, TheWiredPathIsOffUnlessTheEnvironmentAsksForIt)
{
    const bool on = Ppcp::PpcpWiredLink::enabled();
    const char *v = std::getenv("PINPOINT_PPCP_WIRED");
    EXPECT_EQ(on, v != nullptr && std::string(v) == "1");
}

// ── Contract C2 — a device the host DIALS, without a cable ─────────────────
//
// A `Ppcp::Listener` standing in for the phone's per-pairing PpcpListener
// (contract C5).  What matters is not that it is a phone but that THIS HOST is
// the client: the link that comes back is a dialled one, and a dialled link's
// `pairingId()` is empty — which is the entire reason C2 exists.
class FakeCabledDevice
{
public:
    FakeCabledDevice()
    {
        for (std::size_t i = 0; i < m_kTls.size(); ++i)
            m_kTls[i] = static_cast<unsigned char>(0xA0 + i);
        m_identity.resize(17);
        m_identity[0] = 0x01;
        for (std::size_t i = 1; i < m_identity.size(); ++i)
            m_identity[i] = static_cast<unsigned char>(i * 7 + 3);

        const Ppcp::Key k = m_kTls;
        m_listener.setIdentityResolver(
            [k](const unsigned char *, std::size_t, Ppcp::ResolvedPairing &out) {
                out.kTls = k;
                // The DEVICE's own handle on the pairing.  ⚠ It never reaches
                // the host: a dialling host learns nothing from the handshake,
                // which is why it has to resolve the pairing itself first.
                out.pairingId = "device-side-handle";
                return true;
            });
        m_listener.setChannelsPerPeer(2);
        m_ok = m_listener.listen(0, nullptr);
        if (!m_ok) return;
        m_thread = std::thread([this] {
            while (!m_stop) {
                std::unique_ptr<Ppcp::PeerConnection> l = m_listener.accept(50, nullptr);
                if (!l) continue;
                std::lock_guard<std::mutex> g(m_m);
                m_accepted.push_back(std::move(l));
            }
        });
    }

    ~FakeCabledDevice()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        m_listener.stop();
    }

    bool ok() const { return m_ok; }

    // What `PpcpWiredLink` does after it has resolved a pairing: dial, with the
    // identity the presence record published.
    std::unique_ptr<Ppcp::PeerConnection> hostDials()
    {
        Ppcp::ConnectorConfig c;
        c.host = "127.0.0.1";
        c.port = m_listener.port();
        c.kTls = m_kTls;
        c.identity = m_identity;
        return Ppcp::Connector::connect(c, nullptr);
    }

private:
    Ppcp::Listener m_listener;
    Ppcp::Key      m_kTls{};
    Ppcp::PskIdentity m_identity;
    bool m_ok = false;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::mutex  m_m;
    std::vector<std::unique_ptr<Ppcp::PeerConnection>> m_accepted;
};

// ⛔ THE ASSERTION THIS WHOLE CONTRACT EXISTS FOR.  RV 7.3a's single-use
// accounting spends a PAIRING CODE; a wired reconnection resolves against a
// pairing the host already holds and spends nothing.  Calling
// noteLinkEstablished() on the wired path would burn a code no phone ever
// scanned — and, under erratum E57, silently persist a pairing as well.
//
// ⚠ The pairing used here is a fresh, unspent code rather than a persisted one,
// which a real wired reconnection would never be.  That is deliberate: an
// already-persisted pairing has nothing left to spend, so the defect would be
// invisible.  This is the shape that can still see it.
TEST_F(HostServiceClock, AWiredAdoptSpendsNothingAndStillJoinsTheLedger)
{
    FakeCabledDevice device;
    ASSERT_TRUE(device.ok());

    Ppcp::PpcpRendezvous::Config cfg;
    cfg.displayName = "wired";
    Ppcp::PublishedCode code;
    std::string err;
    ASSERT_TRUE(m_svc.rendezvous().publish(cfg, Ppcp::reachableEndpoints(m_svc.port()),
                                           nullptr, &code, &err)) << err;
    const QString pairingId = QString::fromStdString(code.pairingId);

    std::unique_ptr<Ppcp::PeerConnection> link = device.hostDials();
    ASSERT_TRUE(link) << "the host could not dial the stand-in device";
    // ⛔ The premise of C2, asserted rather than assumed: a link WE dialled
    // carries no pairing.  If this ever becomes non-empty the parameter is
    // redundant — and until it does, adopting without it is invisible to
    // phoneByPairing(), notePeerName() and m_pairedThisRun.
    EXPECT_TRUE(link->pairingId().empty());

    m_svc.adoptLinkForTest(std::move(link), pairingId);

    EXPECT_EQ(m_svc.connectedCount(), 1);

    // Rule 2 — nothing was spent, and nothing was remembered on its behalf.
    bool found = false;
    for (const QVariant &v : m_svc.outstandingCodes()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("pairingId")).toString() != pairingId) continue;
        found = true;
        EXPECT_EQ(m.value(QStringLiteral("usesRemaining")).toULongLong(), 1u)
            << "RV 7.3a — a wired reconnection spent a pairing code";
        EXPECT_FALSE(m.value(QStringLiteral("persisted")).toBool())
            << "noteLinkEstablished() ran on the wired path and persisted the pairing";
    }
    EXPECT_TRUE(found);

    // Rule 3 — and the row is there anyway, which is the half that must NOT
    // differ by transport.  `m_pairedThisRun` is the only thing putting it in
    // the list, since the pairing is not persisted.
    bool row = false;
    for (const QVariant &v : m_svc.phones()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("pairingId")).toString() != pairingId) continue;
        row = true;
        EXPECT_EQ(m.value(QStringLiteral("status")).toString(), QStringLiteral("connected"))
            << "the home screen says connected and Settings->Phones says otherwise — "
               "the empty-Phones-list bug, one transport along";
    }
    EXPECT_TRUE(row) << "a wired link was adopted and no device row appeared";
}

// The other arm, and the control for the one above: the WiFi path DOES spend
// the code, because there the phone dialled a code somebody scanned.
TEST_F(HostServiceClock, AnAcceptedLinkStillSpendsItsCodeAndIsRemembered)
{
    Phone p(&m_svc);
    ASSERT_TRUE(p.ok());
    ASSERT_TRUE(p.dial(m_svc.port(), 0x51));
    for (int i = 0; i < 200 && m_svc.connectedCount() < 1; ++i) spin(10);
    ASSERT_EQ(m_svc.connectedCount(), 1);

    const QString pairingId = QString::fromStdString(p.pairingId());
    bool found = false;
    for (const QVariant &v : m_svc.outstandingCodes()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("pairingId")).toString() != pairingId) continue;
        found = true;
        EXPECT_EQ(m.value(QStringLiteral("usesRemaining")).toULongLong(), 0u)
            << "RV 7.3a — the code the phone used was not spent";
        EXPECT_TRUE(m.value(QStringLiteral("persisted")).toBool())
            << "E57 — a completed pairing is remembered automatically";
    }
    EXPECT_TRUE(found);
}

// ⚠ AND A DIALLED LINK WITH NO PAIRING PASSED IN IS THE BUG, KEPT AS A
// NEGATIVE CONTROL.  Without C2's parameter this is what the wired path would
// have produced: a live link carrying video that no device row can see.  If
// this test ever goes green on the row assertion, somebody has taught
// adoptLink() to find the pairing another way and C2 can be simplified.
TEST_F(HostServiceClock, ADialledLinkAdoptedWithNoPairingIsLiveAndInvisible)
{
    FakeCabledDevice device;
    ASSERT_TRUE(device.ok());

    Ppcp::PpcpRendezvous::Config cfg;
    cfg.displayName = "wired";
    Ppcp::PublishedCode code;
    std::string err;
    ASSERT_TRUE(m_svc.rendezvous().publish(cfg, Ppcp::reachableEndpoints(m_svc.port()),
                                           nullptr, &code, &err)) << err;

    std::unique_ptr<Ppcp::PeerConnection> link = device.hostDials();
    ASSERT_TRUE(link);
    m_svc.adoptLinkForTest(std::move(link), QString());

    EXPECT_EQ(m_svc.connectedCount(), 1) << "the link is up";
    for (const QVariant &v : m_svc.phones()) {
        const QVariantMap m = v.toMap();
        EXPECT_NE(m.value(QStringLiteral("pairingId")).toString(),
                  QString::fromStdString(code.pairingId))
            << "a link with no pairing found its way into the ledger";
    }
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
