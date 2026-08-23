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

// The offer list, at the CONTROLLER level: an offer in becomes a row, and
// accepting a row puts a `session_accept` on the wire carrying the digests this
// host holds.  Work package H5 (the offer list); MSG §9.1–9.2.
//
// ⚠ TESTED WITHOUT QML ON PURPOSE.  The deliverable is a QAbstractListModel and
// a QML delegate; the delegate is presentation and the model is the protocol
// obligation.  Asserting the model means the evidence survives a redesign of
// the screen, which the screen's own history says is likely.

#include "ppcp_host_engine.h"
#include "ppcp_import_ledger.h"
#include "ppcp_offer_controller.h"
#include "ppcp_test_peer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace Ppcp;
using pptest::DevicePeer;
using pptest::idStr;

namespace {

constexpr const char *kSession = "sess:offered";

struct Fixture {
    DevicePeer                  dev;
    std::unique_ptr<PpcpEngine> host;
    PpcpOfferController         offers;
    PpcpImportLedger            ledger;

    void build()
    {
        dev.build();
        HostEngineConfig cfg;
        cfg.peerId = "host-1";
        cfg.listener = true;
        std::string why;
        host = makeHostEngine(std::move(cfg), &why);
        ASSERT_NE(host, nullptr) << why;
        offers.attach(host->peer(), QStringLiteral("dev-1"));
        offers.setLedger(&ledger);

        ASSERT_EQ(ppcp_peer_declare(dev.p, &dev.desc), PPCP_OK);
        pptest::pipe(dev.p, host->peer(), PPCP_CHANNEL_CONTROL);
        pptest::drainEvents(host->peer(), [](const ppcp_event &) {});
    }

    void deviceOffers(const char *sessionId, ppcp_completeness c, std::uint64_t bytes,
                      bool withEpoch = true)
    {
        ppcp_body_session_offer o{};
        ASSERT_EQ(ppcp_id_set_z(&o.session_id, sessionId), PPCP_OK);
        ASSERT_EQ(ppcp_id_set_z(&o.minting_peer_id, dev.peerId.c_str()), PPCP_OK);
        o.completeness = c;
        o.has_bytes_estimate = true;
        o.bytes_estimate = bytes;
        if (withEpoch) {
            // I15 — a LABEL.  It reaches the row as a formatted string and is
            // never subtracted from anything.
            o.epoch.present = true;
            o.epoch.wall_utc_ns = 1755820800000000000LL;   // 22 Aug 2026, UTC
            ASSERT_EQ(ppcp_instant_make_z(&o.epoch.at, dev.tb.c_str(), 1000), PPCP_OK);
        }
        ASSERT_EQ(ppcp_peer_session_offer(dev.p, &o), PPCP_OK);
        pptest::pipe(dev.p, host->peer(), PPCP_CHANNEL_CONTROL,
                     [this] {
                         pptest::drainEvents(host->peer(),
                                             [this](const ppcp_event &e) { offers.observe(QStringLiteral("dev-1"), e); });
                     });
    }

    // What the device saw come back.
    bool lastAccept(ppcp_body_session_accept *out, std::uint64_t *replyTo)
    {
        bool saw = false;
        pptest::pipe(host->peer(), dev.p, PPCP_CHANNEL_CONTROL);
        pptest::drainEvents(dev.p, [&](const ppcp_event &e) {
            if (e.kind == PPCP_EVENT_SESSION_ACCEPT && e.msg) {
                saw = true;
                *out = e.msg->body.session_accept;
                if (replyTo) *replyTo = e.msg->env.has_reply_to ? e.msg->env.reply_to : 0;
            }
        });
        return saw;
    }

    QString value(int row, int role) const
    {
        return offers.data(offers.index(row), role).toString();
    }
};

}  // namespace

TEST(PpcpOfferList, AnOfferBecomesARowCarryingWhatTheDeviceAsserted)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_EQ(F.offers.rowCount(), 0);

    ASSERT_NO_FATAL_FAILURE(F.deviceOffers(kSession, PPCP_PARTIAL, 734003200ull));

    ASSERT_EQ(F.offers.rowCount(), 1);
    EXPECT_EQ(F.value(0, PpcpOfferController::SessionIdRole), QString(kSession));
    EXPECT_EQ(F.value(0, PpcpOfferController::MintingPeerIdRole), QStringLiteral("dev-1"));
    // I10 — completeness is ASSERTED by the owner and never inferred.  A
    // `partial` session is shown as partial; nothing here upgrades it on the
    // strength of what happens to arrive later.
    EXPECT_EQ(F.value(0, PpcpOfferController::CompletenessRole), QStringLiteral("partial"));
    EXPECT_TRUE(F.offers.data(F.offers.index(0), PpcpOfferController::HasBytesRole).toBool());
    EXPECT_TRUE(F.offers.data(F.offers.index(0), PpcpOfferController::HasEpochRole).toBool());
    EXPECT_FALSE(F.value(0, PpcpOfferController::EpochLabelRole).isEmpty());
    EXPECT_FALSE(F.offers.data(F.offers.index(0),
                               PpcpOfferController::AlreadyHeldRole).toBool());
    EXPECT_EQ(F.offers.offersSeen(), 1u);
}

TEST(PpcpOfferList, ASecondOfferForTheSameSessionReplacesTheRowRatherThanAddingOne)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.deviceOffers(kSession, PPCP_PARTIAL, 100));
    ASSERT_NO_FATAL_FAILURE(F.deviceOffers(kSession, PPCP_COMPLETE, 200));

    // MSG 3.3a's shape applied to 9.1: the device is stating what it holds NOW.
    // Two rows for one Session, with different sizes, would present a choice
    // that does not exist.
    ASSERT_EQ(F.offers.rowCount(), 1);
    EXPECT_EQ(F.value(0, PpcpOfferController::CompletenessRole), QStringLiteral("complete"));
    EXPECT_EQ(F.offers.data(F.offers.index(0), PpcpOfferController::BytesEstimateRole)
                  .toULongLong(), 200u);
    EXPECT_EQ(F.offers.offersSeen(), 2u);
}

TEST(PpcpOfferList, AcceptingPutsSessionAcceptOnTheWireWithTheDigestsWeHold)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());

    // Two Captures already imported from this device and Session.  One has a
    // digest; one does not, because 8.1e of MSG permits an announce before the
    // digest is computed and an `absent` Capture never has one.
    PpcpImportLedger::CaptureRecord withDigest;
    withDigest.key = { "dev-1", kSession, "cap-1" };
    withDigest.digestHex = std::string(64, 'a');
    ASSERT_EQ(F.ledger.admit(withDigest), PpcpImportLedger::Admission::Recorded);

    PpcpImportLedger::CaptureRecord noDigest;
    noDigest.key = { "dev-1", kSession, "cap-2" };
    noDigest.completeness = Completeness::Absent;
    ASSERT_EQ(F.ledger.admit(noDigest), PpcpImportLedger::Admission::Recorded);

    ASSERT_NO_FATAL_FAILURE(F.deviceOffers(kSession, PPCP_PARTIAL, 1000));
    ASSERT_TRUE(F.offers.acceptOffer(0));

    ppcp_body_session_accept acc{};
    std::uint64_t replyTo = 0;
    ASSERT_TRUE(F.lastAccept(&acc, &replyTo));
    EXPECT_EQ(idStr(acc.session_id), std::string(kSession));
    EXPECT_EQ(acc.verdict, PPCP_OFFER_ACCEPT);
    // 9.1a — the payloads we already hold are named so they are not replayed.
    // ONE digest, not two: the `absent` Capture has no payload to skip and
    // naming it would be naming a hash that does not exist.
    ASSERT_EQ(acc.have_digest_count, 1u);
    EXPECT_EQ(F.offers.lastAcceptDigests(), 1u);
    EXPECT_EQ(F.offers.lastAcceptDigestsDropped(), 0u);
    EXPECT_TRUE(acc.have_digests[0].present);
    // ENC 5b — every response carries `reply_to`.  Without it a device offering
    // several Sessions cannot tell which offer was answered.
    EXPECT_NE(replyTo, 0u);
}

TEST(PpcpOfferList, ASessionWeAlreadyHoldSaysSoAndIsRefusedAsAlreadyHeld)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    F.ledger.noteSession("dev-1", kSession, /*asserted=*/true, Completeness::Complete,
                         /*bundleTruncated=*/false);

    ASSERT_NO_FATAL_FAILURE(F.deviceOffers(kSession, PPCP_COMPLETE, 500));
    ASSERT_EQ(F.offers.rowCount(), 1);
    // I34 — a re-import is a no-op, and the row says so BEFORE the user presses
    // rather than after a replay that changed nothing.
    EXPECT_TRUE(F.offers.data(F.offers.index(0),
                              PpcpOfferController::AlreadyHeldRole).toBool());

    ASSERT_TRUE(F.offers.refuseOffer(0));
    ppcp_body_session_accept acc{};
    ASSERT_TRUE(F.lastAccept(&acc, nullptr));
    // `already_held` is not a refusal: it is the truthful verdict, and it saves
    // the device a replay it would have thrown away.
    EXPECT_EQ(acc.verdict, PPCP_OFFER_ALREADY_HELD);
    EXPECT_EQ(acc.have_digest_count, 0u);
}

TEST(PpcpOfferList, DetachingClearsTheRowsBecauseTheyWereFactsAboutALink)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.deviceOffers(kSession, PPCP_COMPLETE, 500));
    ASSERT_EQ(F.offers.rowCount(), 1);
    F.offers.detach(QStringLiteral("dev-1"));
    EXPECT_EQ(F.offers.rowCount(), 0);
    EXPECT_FALSE(F.offers.acceptOffer(0));
}

int main(int argc, char **argv)
{
    // QLocale and QDateTime are used to format the row labels, so a Qt
    // application object has to exist.  QCoreApplication and not QGuiApplication:
    // nothing here draws.
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
