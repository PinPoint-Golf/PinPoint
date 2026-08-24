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

// PPCP-RV §3, THE HOST HALF OF THE ADVERTISEMENT — work package H9.
// RT-7 (host half), RT-8 (host half).
//
// `ppcp_rendezvous_test` already covers both rows from the BROWSER side: what
// this host reads out of somebody else's TXT record, and that it refuses to
// dial an `rid` it cannot resolve.  Neither of those touches what this host
// PUBLISHES, because until now it published nothing.  3.5e (erratum E32) makes
// it the peer that advertises — its counterpart is barred by 3.5d — and these
// are the assertions about the record that leaves this machine.
//
// ⚠ THE LAST TEST IN THIS FILE REGISTERS WITH THE REAL RESPONDER AND BROWSES
// FOR ITSELF.  It is the only one that demonstrates rather than asserts, and it
// is the answer to "how do I see it".  It needs no network: mDNSResponder
// answers a local browse for a locally-registered service over the same IPC
// socket both halves already use, so it passes on a machine with the Ethernet
// unplugged.  What it cannot prove is that the announcement crossed the link —
// 3.6a says that will fail on plenty of networks and is not an error state, so
// no test can require it.

#include <gtest/gtest.h>

#include <poll.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ppcp/rv.h>

#include "ppcp_discovery.h"
#include "ppcp_rendezvous.h"

using namespace Ppcp;

namespace {

// Enumerates the keys of a built TXT record without going through
// parseTxtRecord(), which by design IGNORES anything it does not recognise —
// exactly the behaviour that would hide a stray key from RT-7.
std::map<std::string, std::string> txtKeys(const std::vector<std::uint8_t> &txt)
{
    std::map<std::string, std::string> out;
    std::size_t i = 0;
    while (i < txt.size()) {
        const std::size_t n = txt[i];
        EXPECT_LE(i + 1 + n, txt.size());
        if (i + 1 + n > txt.size()) break;
        const std::string entry(reinterpret_cast<const char *>(txt.data() + i + 1), n);
        i += 1 + n;
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) out[entry] = std::string();
        else out[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    return out;
}

// A stand-in responder.  Records what the driver asked for so the suite can
// assert on the SHAPE of the traffic — one register, then updates — which is
// the whole of what 3.2d is about and which nothing on the wire distinguishes
// from a rename until you are looking at a packet capture.
class FakeAdvertiser final : public RvAdvertiser {
public:
    bool start(const std::string &instanceName, std::uint16_t port,
               const std::vector<std::uint8_t> &txt) override
    {
        if (refuseStart) return false;
        ++registers;
        name = instanceName;
        this->port = port;
        last = txt;
        return true;
    }
    bool updateTxt(const std::vector<std::uint8_t> &txt) override
    {
        if (refuseUpdate) return false;
        ++updates;
        last = txt;
        return true;
    }
    void stop() override { ++stops; }
    int fd() const override { return -1; }
    bool process() override { return true; }
    std::string describe() const override { return "fake"; }
    std::string registeredName() const override { return name; }

    int registers = 0, updates = 0, stops = 0;
    bool refuseStart = false, refuseUpdate = false;
    std::string name;
    std::uint16_t port = 0;
    std::vector<std::uint8_t> last;
};

// A deterministic "CSPRNG" for the registration nonce.  The nonce is not key
// material — 3.2d draws it so the name persists across nothing, not so it is
// unguessable — so pinning it is fair and makes the instance name assertable.
RvReconnectionAdvertisement::RandomFn countingRng(std::uint8_t seed)
{
    auto n = std::make_shared<std::uint8_t>(seed);
    return [n](void *out, std::size_t len) {
        auto *p = static_cast<std::uint8_t *>(out);
        for (std::size_t i = 0; i < len; ++i) p[i] = static_cast<std::uint8_t>(*n + i);
        ++(*n);
        return true;
    };
}

// A minter that mints from a fixed K_id per pairing id, so the suite can check
// what the driver put on the wire against what a browser holding that key
// would resolve.
RvReconnectionAdvertisement::RidMinter minterOver(
    const std::map<std::string, std::vector<std::uint8_t>> &keys, int *calls = nullptr)
{
    return [keys, calls](const std::string &id, std::uint8_t rn[PPCP_RV_RN_BYTES],
                         std::uint8_t rid[PPCP_RV_RID_BYTES]) {
        auto it = keys.find(id);
        if (it == keys.end()) return false;
        if (calls) ++(*calls);
        // A fresh `rn` every call (3.4a), from a counter rather than a CSPRNG
        // because the property under test is that it CHANGES.
        static std::uint8_t tick = 0;
        ++tick;
        for (std::size_t i = 0; i < PPCP_RV_RN_BYTES; ++i)
            rn[i] = static_cast<std::uint8_t>(tick + i);
        return ppcp_rv_rid(it->second.data(), rn, rid) == PPCP_OK;
    };
}

std::vector<std::uint8_t> keyOf(std::uint8_t fill)
{
    return std::vector<std::uint8_t>(PPCP_RV_KEY_BYTES, fill);
}

}  // namespace

// ── RT-7, host half — what this host publishes, and what it cannot ─────────
//
// "A TXT record contains no `Peer.id`, no device name and no session count;
// the instance name carries no persistent value (3.3b, 3.2b)."
TEST(PpcpAdvertise, RT7_TheRecordCarriesExactlyTheFiveKeysOf33aAndNothingElse)
{
    RvTxtFields f;
    for (std::size_t i = 0; i < PPCP_RV_RN_BYTES; ++i) f.rn[i] = static_cast<std::uint8_t>(0xa0 + i);
    for (std::size_t i = 0; i < PPCP_RV_RID_BYTES; ++i) f.rid[i] = static_cast<std::uint8_t>(0x10 + i);

    std::vector<std::uint8_t> txt;
    ASSERT_TRUE(buildTxtRecord(f, &txt));

    const auto keys = txtKeys(txt);
    // Exactly five, enumerated rather than spot-checked: the failure this
    // guards is a SIXTH key, and a test that only looked for the five it
    // expected would pass with a device name beside them.
    EXPECT_EQ(keys.size(), 5u);
    EXPECT_EQ(keys.count("txtvers"), 1u);
    EXPECT_EQ(keys.count("pv"), 1u);
    EXPECT_EQ(keys.count("role"), 1u);
    EXPECT_EQ(keys.count("rn"), 1u);
    EXPECT_EQ(keys.count("rid"), 1u);
    // 3.3b's own list, by name.
    EXPECT_EQ(keys.count("id"), 0u);
    EXPECT_EQ(keys.count("name"), 0u);
    EXPECT_EQ(keys.count("dn"), 0u);
    EXPECT_EQ(keys.count("sessions"), 0u);
    EXPECT_EQ(keys.count("serial"), 0u);
    // 3.3g's `bs`/`dl` belong to a BOOTSTRAP instance (3.7), which is H10 and
    // is not this record.  A receiver seeing both `bs` and `rid` on one
    // instance treats it as malformed.
    EXPECT_EQ(keys.count("bs"), 0u);
    EXPECT_EQ(keys.count("dl"), 0u);

    EXPECT_EQ(keys.at("txtvers"), "1");
    EXPECT_EQ(keys.at("pv"), "1.0");
    EXPECT_EQ(keys.at("role"), "host");     // 3.5e — the host is the advertiser
    EXPECT_EQ(keys.at("rn").size(), 16u);   // 3.3a — 16 hexadecimal characters
    EXPECT_EQ(keys.at("rid").size(), 16u);

    EXPECT_LT(txt.size(), 200u);            // 3.3c

    // And it round-trips through the browser half, which is the only reader
    // that matters: what we publish is what our counterpart parses.
    RvAdvertisement ad;
    ASSERT_TRUE(parseTxtRecord(txt.data(), txt.size(), &ad));
    EXPECT_EQ(ad.role, "host");
    EXPECT_TRUE(ad.hasRn);
    EXPECT_TRUE(ad.hasRid);
    EXPECT_EQ(std::memcmp(ad.rn, f.rn, PPCP_RV_RN_BYTES), 0);
    EXPECT_EQ(std::memcmp(ad.rid, f.rid, PPCP_RV_RID_BYTES), 0);
    EXPECT_TRUE(pvAcceptsMajor(ad.pv, 1));
}

TEST(PpcpAdvertise, RT7_ARoleOutsideTheClosedSetIsNotPublishedAtAll)
{
    RvTxtFields f;
    f.role = "PinPointStudio on Mark's Mac";   // the 3.2b failure, one field over
    std::vector<std::uint8_t> txt;
    EXPECT_FALSE(buildTxtRecord(f, &txt));
    EXPECT_TRUE(txt.empty());
}

// 3.2d — a stable per-registration name, and 3.2b binds it unchanged.
TEST(PpcpAdvertise, RT7_TheInstanceNameIsEightHexOfAPerRegistrationNonceAndNothingElse)
{
    const std::uint8_t a[4] = {0x9b, 0x1d, 0x2d, 0xf9};
    const std::uint8_t b[4] = {0x00, 0x01, 0xff, 0x10};
    std::string na, nb;
    ASSERT_TRUE(registrationInstanceName(a, &na));
    ASSERT_TRUE(registrationInstanceName(b, &nb));

    EXPECT_EQ(na, "PPCP-9B1D2DF9");   // 3.2a's own worked example, by form
    EXPECT_EQ(nb, "PPCP-0001FF10");
    EXPECT_NE(na, nb);
    EXPECT_EQ(na.size(), 13u);
    EXPECT_LT(na.size(), std::size_t(PPCP_RV_INSTANCE_NAME_MAX));

    // The whole name is derived from the four bytes handed in, so there is no
    // input from which a device name, a user name or a model could reach it.
    for (std::size_t i = 5; i < na.size(); ++i) {
        const char c = na[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) << na;
    }
}

// ── RT-8, host half — the rotation this host actually performs ─────────────
//
// "`rid` changes across re-registration and resolves under the correct `K_id`
// only (3.4a, 3.4b)."  The browser-half test asserts that against a synthetic
// rid; this asserts it against the bytes the driver put on the wire.
TEST(PpcpAdvertise, RT8_EveryRotationPublishesAFreshRidThatOnlyItsOwnKeyResolves)
{
    const auto kA = keyOf(0x11), kB = keyOf(0x22);
    FakeAdvertiser adv;
    RvReconnectionAdvertisement drv(&adv, minterOver({{"A", kA}, {"B", kB}}),
                                    countingRng(0x9b));

    ASSERT_TRUE(drv.start(7788, {"A", "B"}, 1000));
    ASSERT_EQ(adv.registers, 1);

    std::vector<std::vector<std::uint8_t>> seenRid;
    std::vector<std::string> seenOn;

    auto readBack = [&] {
        RvAdvertisement ad;
        ASSERT_TRUE(parseTxtRecord(adv.last.data(), adv.last.size(), &ad));
        ASSERT_TRUE(ad.hasRn);
        ASSERT_TRUE(ad.hasRid);
        // 3.4b, from the far side: recompute with the K_id of each pairing held
        // and select the match.  The pairing the driver says it is on must be
        // the one whose key resolves, and the other must not.
        const std::vector<std::uint8_t> &mine =
            drv.currentPairingId() == "A" ? kA : kB;
        const std::vector<std::uint8_t> &theirs =
            drv.currentPairingId() == "A" ? kB : kA;
        std::uint8_t got[PPCP_RV_RID_BYTES];
        ASSERT_EQ(ppcp_rv_rid(mine.data(), ad.rn, got), PPCP_OK);
        EXPECT_EQ(std::memcmp(got, ad.rid, PPCP_RV_RID_BYTES), 0);
        ASSERT_EQ(ppcp_rv_rid(theirs.data(), ad.rn, got), PPCP_OK);
        EXPECT_NE(std::memcmp(got, ad.rid, PPCP_RV_RID_BYTES), 0);

        seenRid.push_back(std::vector<std::uint8_t>(ad.rid, ad.rid + PPCP_RV_RID_BYTES));
        seenOn.push_back(drv.currentPairingId());
    };

    readBack();
    std::uint64_t now = 1000;
    for (int i = 0; i < 6; ++i) {
        now += drv.periodSeconds();
        ASSERT_TRUE(drv.tick(now)) << "rotation " << i;
        readBack();
    }

    // 3.4d1 — one instance, and which pairing it names cycles in a stable
    // order that wraps.
    ASSERT_EQ(seenOn.size(), 7u);
    for (std::size_t i = 0; i < seenOn.size(); ++i)
        EXPECT_EQ(seenOn[i], (i % 2 == 0) ? "A" : "B") << i;

    // Every `rid` distinct — 3.4a's "regenerated on every registration", and
    // the property 3.4e's unlinkability rests on.
    for (std::size_t i = 0; i < seenRid.size(); ++i)
        for (std::size_t j = i + 1; j < seenRid.size(); ++j)
            EXPECT_NE(seenRid[i], seenRid[j]) << i << " vs " << j;
}

// ⚠ THE CLAUSE E49 CORRECTED, ASSERTED DIRECTLY.  A rotation must be a TXT
// update and never a re-registration: 3.2a ties the name to `rid`, so a peer
// that rotated by re-registering would rename the service — a deregister,
// probe and announce cycle — at seconds-scale, on precisely the networks 3.6a
// says drop multicast.  The mitigation for a discovery problem would trigger
// the condition that breaks discovery.  Nothing on the wire tells the two
// apart after the fact, which is why it is pinned here.
TEST(PpcpAdvertise, ARotationIsOneTxtUpdateAndTheNameNeverMoves)
{
    FakeAdvertiser adv;
    RvReconnectionAdvertisement drv(&adv, minterOver({{"A", keyOf(1)}, {"B", keyOf(2)},
                                                      {"C", keyOf(3)}}),
                                    countingRng(0x40));
    ASSERT_TRUE(drv.start(7788, {"A", "B", "C"}, 0));
    const std::string name = drv.instanceName();
    ASSERT_FALSE(name.empty());
    EXPECT_EQ(adv.name, name);
    EXPECT_EQ(adv.port, 7788);

    std::uint64_t now = 0;
    for (int i = 0; i < 30; ++i) {
        now += drv.periodSeconds();
        ASSERT_TRUE(drv.tick(now));
    }
    EXPECT_EQ(adv.registers, 1);    // once, and once only
    EXPECT_EQ(adv.updates, 30);
    EXPECT_EQ(drv.instanceName(), name);
    EXPECT_EQ(adv.name, name);
}

TEST(PpcpAdvertise, ATickBeforeThePeriodElapsesDoesNothing)
{
    FakeAdvertiser adv;
    RvReconnectionAdvertisement drv(&adv, minterOver({{"A", keyOf(1)}, {"B", keyOf(2)}}),
                                    countingRng(3));
    ASSERT_TRUE(drv.start(7788, {"A", "B"}, 100));
    const unsigned p = drv.periodSeconds();
    for (unsigned t = 0; t < p; ++t) EXPECT_FALSE(drv.tick(100 + t));
    EXPECT_EQ(adv.updates, 0);
    EXPECT_TRUE(drv.tick(100 + p));
    EXPECT_EQ(adv.updates, 1);
}

// ── 3.4d3 as amended by E55 — sized on pairings HELD ───────────────────────
TEST(PpcpAdvertise, TheRotationPeriodIsSizedOnPairingsHeldAndNotOnDevicesPresent)
{
    // Nothing held is nothing to advertise, and the caller does not register.
    EXPECT_EQ(rotationPeriodSeconds(0), 0u);

    // One pairing has no wrong pairing to be on, so the wait 3.4d1 bounds is
    // zero and the only constraint left is 3.4a's MUST — at least every 15
    // minutes.  Rotating faster would be conformant and would buy nothing:
    // E49 records that faster rotation is NEUTRAL for unlinkability within one
    // registration, because the A/AAAA record and the port do not change.
    EXPECT_EQ(rotationPeriodSeconds(1), 900u);

    // 3.4d3's own worked example for a studio: "at twenty-second rotation both
    // are minutes", with three pairings.
    EXPECT_EQ(rotationPeriodSeconds(3), 20u);
    EXPECT_EQ(rotationPeriodSeconds(2), 30u);

    // ⚠ E55 IS WHY THIS KEEPS GOING PAST THREE.  7.4a gives a persisted
    // pairing no expiry — it ends on revocation (7.4d) and on nothing else —
    // so a studio host accumulates every device it has ever met.  The count
    // that sets the wait is that accumulated total, not the two or three
    // devices in the room today, and the floor is what stops a host with a
    // season of them rotating every second.
    EXPECT_EQ(rotationPeriodSeconds(6), 10u);
    EXPECT_EQ(rotationPeriodSeconds(10), 10u);
    EXPECT_EQ(rotationPeriodSeconds(200), 10u);
    for (std::size_t n = 1; n <= 200; ++n) {
        EXPECT_GE(rotationPeriodSeconds(n), kAdvertisementMinPeriodS) << n;
        EXPECT_LE(rotationPeriodSeconds(n), kAdvertisementMaxPeriodS) << n;
    }

    // And the wait 3.4d1 bounds — period x pairings held — stays in minutes
    // rather than hours across every count a studio can plausibly reach.  At
    // the 15-minute floor with ten pairings it would be two and a half hours.
    for (std::size_t n = 2; n <= 20; ++n)
        EXPECT_LE(rotationPeriodSeconds(n) * n, 300u) << n;
}

// ── The ledger changing under the advertisement ────────────────────────────
TEST(PpcpAdvertise, RevokingAPairingDoesNotRenameTheServiceAndTheLastOneWithdrawsIt)
{
    FakeAdvertiser adv;
    RvReconnectionAdvertisement drv(&adv, minterOver({{"A", keyOf(1)}, {"B", keyOf(2)},
                                                      {"C", keyOf(3)}}),
                                    countingRng(0x77));
    ASSERT_TRUE(drv.start(7788, {"A", "B", "C"}, 0));
    const std::string name = drv.instanceName();
    EXPECT_EQ(drv.periodSeconds(), 20u);

    // A pairing revoked is not a new REGISTRATION under 3.2d, so the name
    // stands and the responder is not asked to re-register.
    drv.setPairings({"A", "C"}, 5);
    EXPECT_EQ(adv.registers, 1);
    EXPECT_EQ(drv.instanceName(), name);
    EXPECT_EQ(drv.periodSeconds(), 30u);      // resized on what is held now
    EXPECT_EQ(drv.currentPairingId(), "A");   // and not moved off its pairing

    // The last one goes: the instance is withdrawn rather than left naming a
    // pairing that no longer exists.
    drv.setPairings({}, 10);
    EXPECT_FALSE(drv.active());
    EXPECT_GE(adv.stops, 1);
    EXPECT_FALSE(drv.tick(1000000));
}

TEST(PpcpAdvertise, NoPairingsHeldMeansNoAdvertisementAtAll)
{
    FakeAdvertiser adv;
    RvReconnectionAdvertisement drv(&adv, minterOver({}), countingRng(1));
    EXPECT_FALSE(drv.start(7788, {}, 0));
    EXPECT_EQ(adv.registers, 0);
    EXPECT_FALSE(drv.active());
    EXPECT_EQ(drv.describe(), "not advertising");
}

// 3.6a — a responder that will not publish is not an error state.  The
// assertion is that the driver simply stops being discoverable, and in
// particular that it does not keep hammering the responder.
TEST(PpcpAdvertise, AResponderThatRefusesIsNotAnErrorState)
{
    FakeAdvertiser adv;
    adv.refuseStart = true;
    RvReconnectionAdvertisement drv(&adv, minterOver({{"A", keyOf(1)}}), countingRng(1));
    EXPECT_FALSE(drv.start(7788, {"A"}, 0));
    EXPECT_FALSE(drv.active());
    EXPECT_FALSE(drv.tick(100000));

    // And a rotation the responder refuses leaves the previous record standing,
    // which is conformant for up to 3.4a's fifteen minutes.
    FakeAdvertiser adv2;
    RvReconnectionAdvertisement drv2(&adv2, minterOver({{"A", keyOf(1)}, {"B", keyOf(2)}}),
                                     countingRng(1));
    ASSERT_TRUE(drv2.start(7788, {"A", "B"}, 0));
    const std::vector<std::uint8_t> standing = adv2.last;
    adv2.refuseUpdate = true;
    EXPECT_FALSE(drv2.tick(drv2.periodSeconds()));
    EXPECT_TRUE(drv2.active());
    EXPECT_EQ(adv2.last, standing);
}

// 7.2b — what the diagnostic export is allowed to say.  A count, a period and
// the service type; never which pairing is on the wire, because naming that in
// a bundle is the correlation 3.4e's unlinkability argument is about.
TEST(PpcpAdvertise, TheDiagnosticLineNamesNoPairing)
{
    FakeAdvertiser adv;
    RvReconnectionAdvertisement drv(&adv, minterOver({{"pairing-secret-handle", keyOf(1)},
                                                      {"another-handle", keyOf(2)}}),
                                    countingRng(1));
    ASSERT_TRUE(drv.start(7788, {"pairing-secret-handle", "another-handle"}, 0));
    const std::string d = drv.describe();
    EXPECT_NE(d.find("_ppcp._tcp"), std::string::npos);
    EXPECT_NE(d.find("role host"), std::string::npos);
    EXPECT_EQ(d.find("pairing-secret-handle"), std::string::npos);
    EXPECT_EQ(d.find("another-handle"), std::string::npos);
}

// ── The ledger seam, end to end ────────────────────────────────────────────
//
// The `rid` this host advertises must be one this host would itself resolve
// through the browser-half resolver, because that resolver is the one
// PinPointCapture runs.  If the two ever disagree the reconnection silently
// never happens, and 3.6a guarantees nobody sees an error about it.
TEST(PpcpAdvertise, WhatTheLedgerMintsIsWhatTheResolverResolves)
{
    PpcpRendezvous rv;
    rv.setSecretStore(makeEphemeralPairingStore());
    const std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};

    // Two pairings, both persisted — 7.4a, which is the only thing §3 has to
    // reconnect to.
    PublishedCode a, b;
    std::string err, why;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &a, &err)) << err;
    ASSERT_TRUE(rv.persist(a.pairingId, &why)) << why;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &b, &err)) << err;
    ASSERT_TRUE(rv.persist(b.pairingId, &why)) << why;

    const std::vector<std::string> held = rv.advertisablePairings();
    ASSERT_EQ(held.size(), 2u);

    for (const std::string &id : held) {
        std::uint8_t rn[PPCP_RV_RN_BYTES], rid[PPCP_RV_RID_BYTES];
        ASSERT_TRUE(rv.mintRid(id, rn, rid));
        // The round trip: what we would put in the TXT record resolves back to
        // the pairing it was minted for, through the same function 3.4b makes
        // the browser use.
        EXPECT_EQ(rv.resolveRid(rn, rid), id);

        // And a second mint of the same pairing is a different `rn` and a
        // different `rid` — 3.4a.
        std::uint8_t rn2[PPCP_RV_RN_BYTES], rid2[PPCP_RV_RID_BYTES];
        ASSERT_TRUE(rv.mintRid(id, rn2, rid2));
        EXPECT_NE(std::memcmp(rn, rn2, PPCP_RV_RN_BYTES), 0);
        EXPECT_NE(std::memcmp(rid, rid2, PPCP_RV_RID_BYTES), 0);
        EXPECT_EQ(rv.resolveRid(rn2, rid2), id);
    }

    // ⚠ AN UNPERSISTED CODE IS NOT ADVERTISED.  It is dialled by the peer that
    // scanned it, at the endpoint printed in the code (4.3d); §3 is
    // reconnection convenience, and a pairing that has not happened yet has
    // nothing to reconnect to.
    PublishedCode c;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &c, &err)) << err;
    const std::vector<std::string> still = rv.advertisablePairings();
    EXPECT_EQ(still.size(), 2u);
    for (const std::string &id : still) EXPECT_NE(id, c.pairingId);

    // Revocation (7.4d) removes it from what is advertised, immediately.
    rv.revoke(a.pairingId);
    const std::vector<std::string> after = rv.advertisablePairings();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0], b.pairingId);
    std::uint8_t rn[PPCP_RV_RN_BYTES], rid[PPCP_RV_RID_BYTES];
    EXPECT_FALSE(rv.mintRid(a.pairingId, rn, rid));
}

// 3.4d1's SHOULD — "a peer SHOULD advertise a recently used pairing first,
// because the counterpart a user is standing in front of is usually the one
// they used last".
TEST(PpcpAdvertise, TheMostRecentlyUsedPairingIsAdvertisedFirst)
{
    PpcpRendezvous rv;
    rv.setSecretStore(makeEphemeralPairingStore());
    std::uint64_t clock = 1000;
    rv.setClock([&clock] { return clock; });
    const std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};

    std::string err, why;
    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) {
        PublishedCode p;
        ASSERT_TRUE(rv.publish({}, eps, nullptr, &p, &err)) << err;
        ASSERT_TRUE(rv.persist(p.pairingId, &why)) << why;
        ids.push_back(p.pairingId);
    }

    // Nothing used yet: ledger order, and stable across calls.
    EXPECT_EQ(rv.advertisablePairings(), ids);
    EXPECT_EQ(rv.advertisablePairings(), ids);

    clock = 2000;
    rv.noteLinkEstablished(ids[2]);
    clock = 3000;
    rv.noteLinkEstablished(ids[0]);

    const std::vector<std::string> order = rv.advertisablePairings();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], ids[0]);   // used last
    EXPECT_EQ(order[1], ids[2]);
    EXPECT_EQ(order[2], ids[1]);   // never used
}

// ── THE DEMONSTRATION — a real registration, seen by a real browse ─────────
//
// Everything above is an assertion about a predicate.  This one asks
// mDNSResponder to publish the record and then asks it, through the browser
// half, whether it can see it — which is the same question `dns-sd -B
// _ppcp._tcp` answers, run by hand.
//
// ⚠ IT IS ALLOWED TO FIND NOTHING, AND SAYS SO RATHER THAN FAILING.  3.6a is a
// MUST NOT: discovery failure is not an error state.  A build machine with no
// responder, or a sandbox that declines to register, is not a defect in this
// code and a red test there would be a false one.  What it MUST NOT do is see
// our instance and find the record wrong, and that is what it asserts.
TEST(PpcpAdvertise, RegisteringWithTheRealResponderIsVisibleToARealBrowse)
{
    std::unique_ptr<RvAdvertiser> adv = makePlatformAdvertiser();
    if (!adv) {
        GTEST_SKIP() << "no DNS-SD responder on this platform (CA5: Windows deferred)";
    }

    const auto kId = keyOf(0x5a);
    std::uint8_t rn[PPCP_RV_RN_BYTES], rid[PPCP_RV_RID_BYTES];
    for (std::size_t i = 0; i < PPCP_RV_RN_BYTES; ++i) rn[i] = static_cast<std::uint8_t>(0xc0 + i);
    ASSERT_EQ(ppcp_rv_rid(kId.data(), rn, rid), PPCP_OK);

    RvTxtFields f;
    std::memcpy(f.rn, rn, sizeof rn);
    std::memcpy(f.rid, rid, sizeof rid);
    std::vector<std::uint8_t> txt;
    ASSERT_TRUE(buildTxtRecord(f, &txt));

    // A port of our own so a PinPointStudio that happens to be running on this
    // machine is not confused with the test.
    if (!adv->start("PPCP-TESTA57C", 47788, txt)) {
        GTEST_SKIP() << "the responder refused the registration (3.6a: not an error)";
    }

    std::unique_ptr<RvBrowser> br = makePlatformBrowser();
    ASSERT_TRUE(br);
    bool sawOurs = false;
    RvAdvertisement seen;
    const bool started = br->start(
        [&](const RvAdvertisement &ad) {
            if (ad.instanceName.rfind("PPCP-TESTA57C", 0) != 0) return;
            sawOurs = true;
            seen = ad;
        },
        [](const std::string &) {});
    if (!started) {
        adv->stop();
        GTEST_SKIP() << "the responder refused the browse (3.6a: not an error)";
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!sawOurs && std::chrono::steady_clock::now() < deadline) {
        struct pollfd fds[2];
        int n = 0;
        const int bf = br->fd(), af = adv->fd();
        if (bf >= 0) { fds[n].fd = bf; fds[n].events = POLLIN; fds[n].revents = 0; ++n; }
        if (af >= 0) { fds[n].fd = af; fds[n].events = POLLIN; fds[n].revents = 0; ++n; }
        if (n == 0) break;
        if (::poll(fds, n, 250) <= 0) continue;
        for (int i = 0; i < n; ++i) {
            if (!(fds[i].revents & POLLIN)) continue;
            if (fds[i].fd == bf) br->process();
            else adv->process();
        }
    }

    const std::string registered = adv->registeredName();
    adv->stop();
    br->stop();

    if (!sawOurs) {
        GTEST_SKIP() << "the local responder did not return our own registration "
                        "within 10s (3.6a forbids treating that as an error); "
                        "registered name was '" << registered << "'";
    }

    // Seen — so now it must be RIGHT.  This is the host half of RT-7 and RT-8
    // read off the responder rather than out of a buffer.
    EXPECT_EQ(seen.txtvers, "1");
    EXPECT_EQ(seen.role, "host");
    EXPECT_TRUE(pvAcceptsMajor(seen.pv, 1));
    ASSERT_TRUE(seen.hasRn);
    ASSERT_TRUE(seen.hasRid);
    EXPECT_EQ(std::memcmp(seen.rn, rn, PPCP_RV_RN_BYTES), 0);
    EXPECT_EQ(std::memcmp(seen.rid, rid, PPCP_RV_RID_BYTES), 0);
    EXPECT_EQ(seen.port, 47788);

    // 3.4b from the far side, against the real record: the advertised `rid` is
    // the one this K_id produces, and no other key produces it.
    std::uint8_t check[PPCP_RV_RID_BYTES];
    ASSERT_EQ(ppcp_rv_rid(kId.data(), seen.rn, check), PPCP_OK);
    EXPECT_EQ(std::memcmp(check, seen.rid, PPCP_RV_RID_BYTES), 0);
    const auto other = keyOf(0x5b);
    ASSERT_EQ(ppcp_rv_rid(other.data(), seen.rn, check), PPCP_OK);
    EXPECT_NE(std::memcmp(check, seen.rid, PPCP_RV_RID_BYTES), 0);
}

// ── HOW TO SEE IT BY HAND ──────────────────────────────────────────────────
//
// The test above holds its registration for about a second, which is too short
// to catch with `dns-sd`.  Set `PPCP_ADVERTISE_HOLD_S` and this one keeps a
// real advertisement up for that long, rotating on the shipped schedule, so
// the two properties that matter can be watched from another terminal:
//
//   PPCP_ADVERTISE_HOLD_S=90 ctest --test-dir build/ppcp-tests \
//       -R ppcp_advertise_hold --output-on-failure
//
//   dns-sd -B _ppcp._tcp                     # the instance appears
//   dns-sd -L PPCP-XXXXXXXX _ppcp._tcp       # its TXT, and its port
//
// ⚠ WHAT TO LOOK FOR IS THE PAIR OF THEM.  The instance NAME must not change
// for the whole hold (3.2d — a rename is a deregister/probe/announce cycle,
// and at seconds-scale that is the condition 3.6a says breaks discovery),
// while `rn` and `rid` in the TXT must change every rotation (3.4a).  A build
// that rotated by re-registering would look almost identical in `dns-sd -B`
// and is the mistake E49 corrected the specification for.
//
// Unset, this test does nothing and costs nothing.
TEST(PpcpAdvertiseHold, HoldsARealAdvertisementSoItCanBeWatchedWithDnsSd)
{
    const char *env = ::getenv("PPCP_ADVERTISE_HOLD_S");
    if (!env || !*env) {
        GTEST_SKIP() << "set PPCP_ADVERTISE_HOLD_S=90 to hold a real "
                        "advertisement up for dns-sd";
    }
    int holdS = std::atoi(env);
    if (holdS <= 0) holdS = 90;
    if (holdS > 240) holdS = 240;   // the ctest TIMEOUT is 300

    std::unique_ptr<RvAdvertiser> adv = makePlatformAdvertiser();
    ASSERT_TRUE(adv) << "no DNS-SD responder on this platform";

    // Three pairings, which is the studio case 3.4d3 is written for and gives
    // the twenty-second rotation its own worked example quotes.
    const std::map<std::string, std::vector<std::uint8_t>> keys{
        {"one", keyOf(0x01)}, {"two", keyOf(0x02)}, {"three", keyOf(0x03)}};
    RvReconnectionAdvertisement drv(adv.get(), minterOver(keys), countingRng(0x11));
    ASSERT_TRUE(drv.start(47788, {"one", "two", "three"},
                          static_cast<std::uint64_t>(::time(nullptr))));

    std::fprintf(stderr,
                 "\n  advertising as '%s' on port 47788 for %ds, "
                 "rotating every %us\n"
                 "  watch it:  dns-sd -B _ppcp._tcp\n"
                 "             dns-sd -L %s _ppcp._tcp\n\n",
                 drv.instanceName().c_str(), holdS, drv.periodSeconds(),
                 drv.instanceName().c_str());

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(holdS);
    while (std::chrono::steady_clock::now() < until) {
        const int fd = adv->fd();
        if (fd >= 0) {
            struct pollfd p { fd, POLLIN, 0 };
            if (::poll(&p, 1, 200) > 0 && (p.revents & POLLIN)) adv->process();
        } else {
            struct pollfd p { -1, 0, 0 };
            ::poll(&p, 0, 200);
        }
        if (drv.tick(static_cast<std::uint64_t>(::time(nullptr))))
            std::fprintf(stderr, "  rotated -> %s\n", drv.describe().c_str());
    }

    // The name never moved, which is the assertion the eye cannot make.
    EXPECT_EQ(adv->registeredName().rfind(drv.instanceName(), 0), 0u);
    EXPECT_GE(drv.rotations(), 2u);
    drv.stop();
}
