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

// PPCP-RV, host side — work package H6.  RT-5, RT-7 (browser half), RT-8,
// RT-9, RT-15, RT-16.
//
// ⚠ THE SCANNER IN THIS FILE IS libppcp AND NOTHING ELSE.  It decodes the URI
// with ppcp_rv_uri_decode(), derives with ppcp_rv_derive() and mints its
// identity with ppcp_rv_psk_identity() — the same four functions
// PinPointCapture will call.  Nothing here re-implements a derivation, which is
// the point: if the host and the device ever disagree about what `K_tls` is,
// this suite cannot be the place it is hidden, because both sides of it come
// out of one library.
//
// ⚠ AND THE HANDSHAKE IS REAL.  Every refusal below is a TLS handshake against
// H1's Listener on a loopback socket, refused by the identity resolver, and not
// an assertion about a predicate.  RV 5.2i is explicit that this class of
// requirement is settled by observed behaviour and never by an API assertion,
// and the same reasoning applies to 7.3: "the publisher invalidates the code"
// is a claim about what happens when somebody dials.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <ppcp/rv.h>

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include "ppcp_discovery.h"
#include "ppcp_rendezvous.h"
#include "ppcp_transport.h"

using namespace Ppcp;

namespace {

std::string hexOf(const std::uint8_t *b, std::size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[b[i] >> 4]);
        s.push_back(d[b[i] & 0x0f]);
    }
    return s;
}

// The peer that SCANS.  Everything it does is a libppcp call.
struct Scanner {
    std::vector<std::uint8_t> scratch;
    ppcp_rv_payload           payload{};
    ppcp_rv_keys              keys{};
    bool                      ok = false;

    explicit Scanner(const std::string &uri)
    {
        scratch.resize(PPCP_RV_MAX_PAYLOAD);
        ppcp_rv_payload_init(&payload);
        if (ppcp_rv_uri_decode(uri.c_str(), uri.size(), scratch.data(), scratch.size(),
                               &payload) != PPCP_OK)
            return;
        if (ppcp_rv_derive(payload.sid, PPCP_RV_SID_BYTES, payload.psk, payload.psk_len,
                           &keys) != PPCP_OK)
            return;
        ok = true;
    }

    // 5.3a — fresh per connection.  The test supplies rn2 explicitly so a
    // repeat can be forced where the assertion is about repetition.
    PskIdentity identity(std::uint8_t seed) const
    {
        std::uint8_t rn2[PPCP_RV_RN_BYTES];
        for (std::size_t i = 0; i < sizeof rn2; ++i)
            rn2[i] = static_cast<std::uint8_t>(seed + i * 31u);
        PskIdentity id(PPCP_RV_PSK_IDENTITY_BYTES);
        EXPECT_EQ(ppcp_rv_psk_identity(keys.k_id, rn2, id.data()), PPCP_OK);
        return id;
    }

    Key kTls() const
    {
        Key k{};
        std::memcpy(k.data(), keys.k_tls, PPCP_RV_KEY_BYTES);
        return k;
    }
};

// A publisher on loopback with its resolver installed, and an accept loop on a
// worker thread — Listener::accept() blocks, exactly as it does in the
// application.
struct Bay {
    PpcpRendezvous rv;
    Listener       listener;
    PublishedCode  code;
    std::uint16_t  port = 0;

    bool open(const PpcpRendezvous::Config &cfg = {})
    {
        std::string err;
        if (!listener.listen(0, &err)) return false;
        port = listener.port();
        listener.setChannelsPerPeer(2);
        listener.setIdentityResolver(rv.identityResolver());
        Options o;
        o.handshakeTimeoutMs = 4000;
        o.bindTimeoutMs = 4000;
        listener.setOptions(o);

        std::vector<RvEndpoint> eps{RvEndpoint{"127.0.0.1", port}};
        return rv.publish(cfg, eps, nullptr, &code, &err);
    }

    // Runs one accept() and reports whether a link arrived.  Returns the
    // pairing the transport resolved, which is what the application feeds to
    // noteLinkEstablished().
    struct Arrival {
        bool        linked = false;
        std::string pairingId;
    };

    Arrival dial(const Scanner &s, std::uint8_t seed)
    {
        Arrival a;
        std::atomic<bool> done{false};
        std::unique_ptr<PeerConnection> link;
        std::thread server([&] {
            HandshakeFailure f;
            link = listener.accept(3000, &f);
            done = true;
        });

        ConnectorConfig c;
        c.host = "127.0.0.1";
        c.port = port;
        c.kTls = s.kTls();
        c.identity = s.identity(seed);
        c.channels = {Channel::Control, Channel::Bulk};
        c.options.handshakeTimeoutMs = 4000;
        HandshakeFailure fail;
        std::unique_ptr<PeerConnection> client = Connector::connect(c, &fail);

        server.join();
        a.linked = link != nullptr && client != nullptr;
        if (link) a.pairingId = code.pairingId;
        return a;
    }
};

}  // namespace

// ── RT-12's companion: the code itself ──────────────────────────────────────
TEST(PpcpRendezvous, EveryCodeCarriesFreshSecretsAndDerivesWhatTheScannerDerives)
{
    PpcpRendezvous rv;
    std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};

    PpcpRendezvous::Config cfg;
    cfg.displayName = "Bay 3";
    cfg.codeLifetimeS = 300;

    // 7.3d — "a publisher generates fresh psk and sid for every code.  A code
    // is never regenerated with the same secret."  Sixty-four codes, and every
    // derived K_tls, K_id and Session.id distinct.  This is the only assertion
    // available about the CSPRNG from outside it, and RT-12 exists because it
    // is not a strong one — a broken generator with a long period passes it.
    // What it does catch is the failure that has actually happened to people:
    // a secret minted once and reused for every code.
    std::vector<std::string> tlsKeys, idKeys, sessions, uris;
    for (int i = 0; i < 64; ++i) {
        PublishedCode c;
        std::string err;
        ASSERT_TRUE(rv.publish(cfg, eps, nullptr, &c, &err)) << err;
        Scanner s(c.uri);
        ASSERT_TRUE(s.ok);
        tlsKeys.push_back(hexOf(s.keys.k_tls, PPCP_RV_KEY_BYTES));
        idKeys.push_back(hexOf(s.keys.k_id, PPCP_RV_KEY_BYTES));
        sessions.push_back(c.sessionId);
        uris.push_back(c.uri);

        // 4.3g / 7.2a — at least 128 bits.  This host mints 256.
        EXPECT_EQ(s.payload.psk_len, static_cast<std::size_t>(PPCP_RV_PSK_MAX));
        // 4.2a — `v` is the payload version and this draft defines 1.
        EXPECT_EQ(s.payload.v, 1u);
        // 7.3a — the default is one, and it is written rather than assumed.
        EXPECT_TRUE(s.payload.has_mu);
        EXPECT_EQ(s.payload.mu, 1u);
        // 7.3c — a code carries `exp`.
        EXPECT_TRUE(s.payload.has_exp);
        // 4.3e — Session.id is the canonical lowercase UUID text of sid, and
        // the host and the scanner agree on it because both ask libppcp.
        char sid[PPCP_RV_SESSION_ID_CHARS] = {0};
        ASSERT_EQ(ppcp_rv_sid_to_session_id(s.payload.sid, sid), PPCP_OK);
        EXPECT_EQ(c.sessionId, std::string(sid));
        // 4.1a/4.1c — the scheme is `ppcp` and is never http(s).
        EXPECT_EQ(c.uri.rfind("ppcp:", 0), 0u);
    }
    for (auto *v : {&tlsKeys, &idKeys, &sessions, &uris}) {
        std::vector<std::string> sorted = *v;
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end())
            << "a value repeated across 64 codes";
    }
}

TEST(PpcpRendezvous, ACodeWithNoReachableAddressIsNotPublishedAtAll)
{
    // 4.3 makes `ep` 1..n and 4.3c has the scanner walk it in order.  A code
    // with nothing to walk renders a perfectly scannable QR that cannot pair,
    // which is the failure mode a user cannot diagnose.
    PpcpRendezvous rv;
    PublishedCode c;
    std::string err;
    EXPECT_FALSE(rv.publish({}, {}, nullptr, &c, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(rv.codes().empty());
}

TEST(PpcpRendezvous, TheHostAdvertisesEveryAddressItIsReachableAt)
{
    // 4.3d — "a publisher lists every address it is reachable at — wired,
    // wireless, and its hotspot address where it provides one.  This is what
    // makes the code work when discovery does not."  The machine running this
    // test has at least a loopback, so the floor is one; the ordering rule is
    // what is actually asserted.
    const std::vector<RvEndpoint> eps = reachableEndpoints(7788);
    ASSERT_FALSE(eps.empty());
    EXPECT_LE(eps.size(), static_cast<std::size_t>(PPCP_RV_MAX_ENDPOINTS));
    for (const auto &e : eps) EXPECT_EQ(e.port, 7788);

    // Loopback is last, because a code that leads with 127.0.0.1 is a code
    // that only works on the machine that displayed it.
    std::size_t firstLoopback = eps.size();
    for (std::size_t i = 0; i < eps.size(); ++i)
        if (eps[i].host == "127.0.0.1" || eps[i].host == "::1") { firstLoopback = i; break; }
    for (std::size_t i = firstLoopback; i < eps.size(); ++i)
        EXPECT_TRUE(eps[i].host == "127.0.0.1" || eps[i].host == "::1")
            << "a routable address after a loopback one";
}

// ── RT-5 — a second handshake with a `mu: 1` code is refused (7.3a) ─────────
TEST(PpcpRendezvous, RT5_ASecondPairingWithASingleUseCodeIsRefusedAtTheHandshake)
{
    Bay bay;
    ASSERT_TRUE(bay.open());
    Scanner s(bay.code.uri);
    ASSERT_TRUE(s.ok);

    // The first link comes up.  ⚠ TWO TLS HANDSHAKES HAPPEN HERE, one per
    // channel, both resolving the same pairing — see F-H6-1 and the header of
    // noteLinkEstablished().
    Bay::Arrival first = bay.dial(s, 0x11);
    ASSERT_TRUE(first.linked) << "the first pairing should complete";
    EXPECT_GE(bay.rv.counters().resolved, 2u)
        << "one link is two handshakes, which is exactly why `mu` cannot count handshakes";

    bay.rv.noteLinkEstablished(first.pairingId);
    CodeStatus st;
    ASSERT_TRUE(bay.rv.status(first.pairingId, &st));
    EXPECT_EQ(st.usesRemaining, 0u);

    // 7.3a — the code is spent.  A second peer with the same code (or the same
    // peer dialling again) is refused, and refused at the handshake rather
    // than after it.
    const auto before = bay.rv.counters();
    Bay::Arrival second = bay.dial(s, 0x22);
    EXPECT_FALSE(second.linked) << "a second pairing on a mu:1 code must be refused";
    const auto after = bay.rv.counters();
    EXPECT_GT(after.refusedExhausted, before.refusedExhausted);
    EXPECT_EQ(after.resolved, before.resolved) << "nothing was authenticated";
}

// ── RT-15 — the publisher refuses a handshake past `exp` (7.3e) ─────────────
TEST(PpcpRendezvous, RT15_ThePublisherHoldsTheAuthoritativeClockAndRefusesAnExpiredCode)
{
    Bay bay;
    std::uint64_t now = 1787832000;
    bay.rv.setClock([&] { return now; });

    PpcpRendezvous::Config cfg;
    cfg.codeLifetimeS = 60;
    ASSERT_TRUE(bay.open(cfg));
    Scanner s(bay.code.uri);
    ASSERT_TRUE(s.ok);
    ASSERT_TRUE(s.payload.has_exp);
    EXPECT_EQ(s.payload.exp, now + 60);

    // Inside the window it pairs.
    Bay::Arrival ok = bay.dial(s, 0x31);
    ASSERT_TRUE(ok.linked);

    // Past it, the same code with a fresh identity is refused — by the
    // PUBLISHER, which is what 7.3e requires and what lets 4.4a1 permit a
    // device with an untrustworthy clock to attempt rather than be locked out.
    now += 61;
    const auto before = bay.rv.counters();
    Bay::Arrival late = bay.dial(s, 0x32);
    EXPECT_FALSE(late.linked);
    EXPECT_GT(bay.rv.counters().refusedExpired, before.refusedExpired);
}

// ── 7.3b — invalidated when the session closes, used or not ────────────────
TEST(PpcpRendezvous, ClosingTheSessionInvalidatesItsCodeWhetherOrNotItWasUsed)
{
    Bay bay;
    ASSERT_TRUE(bay.open());
    Scanner s(bay.code.uri);
    ASSERT_TRUE(s.ok);

    bay.rv.closeSession(bay.code.sessionId);
    CodeStatus st;
    ASSERT_TRUE(bay.rv.status(bay.code.pairingId, &st));
    EXPECT_TRUE(st.invalidated);

    const auto before = bay.rv.counters();
    EXPECT_FALSE(bay.dial(s, 0x41).linked);
    const auto after = bay.rv.counters();
    // 7.2d erased the key material with the session, so the identity no longer
    // resolves at all — a revoked peer is a stranger, not a known refusal
    // (7.7c).  Either counter moving is conformant; both being unchanged would
    // mean the resolver was never consulted.
    EXPECT_GT(after.refusedNoPairing + after.refusedInvalidated,
              before.refusedNoPairing + before.refusedInvalidated);
    EXPECT_EQ(after.resolved, before.resolved);
}

// ── 7.4d — persist() is still the explicit, low-level entry point ──────────
// E57 (25 August 2026) downgraded 7.4b's opt-in clause to a SHOULD, and the
// ordinary path is now automatic — see the tests below this one.  persist()
// itself is unchanged: it is what noteLinkEstablished() and
// adoptGuidedPairing() call under the hood, and it stays reachable directly
// for a caller (this suite) that wants to persist a pairing with no
// handshake involved.
TEST(PpcpRendezvous, PersistenceIsOptInAndRevocationIsHonouredImmediately)
{
    PpcpRendezvous rv;
    std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};
    PublishedCode c;
    std::string err;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &c, &err)) << err;

    // ⚠ With no store installed at all there is nowhere to put a key.  This
    // used to be the platform condition — 7.2c "where one exists" — and since
    // erratum E56 it is only ever a deliberate choice by the caller, because
    // makePlatformPairingStore() now returns a store everywhere.
    std::string why;
    EXPECT_FALSE(rv.persist(c.pairingId, &why));
    EXPECT_FALSE(why.empty());

    rv.setSecretStore(makeEphemeralPairingStore());
    ASSERT_TRUE(rv.persist(c.pairingId, &why)) << why;
    EXPECT_TRUE(rv.isPersisted(c.pairingId));
    ASSERT_TRUE(rv.secretStore());
    EXPECT_EQ(rv.secretStore()->list().size(), 1u);

    // 7.4d — honoured immediately by this side: the stored key goes with it.
    rv.revoke(c.pairingId);
    EXPECT_FALSE(rv.isPersisted(c.pairingId));
    EXPECT_TRUE(rv.secretStore()->list().empty());
}

// ── E57, 25 August 2026 — RV 7.4b downgraded to a SHOULD ────────────────────
// This application's answer: a pairing is remembered automatically the moment
// it completes, and Settings -> Phones' "Forget" (7.4d) is the opt-out.  The
// two producers of a COMPLETE pairing are exercised here — a scanned code via
// noteLinkEstablished(), and a guided pairing via adoptGuidedPairing() — plus
// the two conditions that still leave a pairing unremembered: no store, and a
// `mu`>1 code (7.4f, unchanged by the erratum).
TEST(PpcpRendezvous, ACompletedPairingIsRememberedAutomaticallyOnceItsLinkEstablishes)
{
    Bay bay;
    bay.rv.setSecretStore(makeEphemeralPairingStore());
    ASSERT_TRUE(bay.open());
    Scanner s(bay.code.uri);
    ASSERT_TRUE(s.ok);

    Bay::Arrival a = bay.dial(s, 0x51);
    ASSERT_TRUE(a.linked);
    EXPECT_FALSE(bay.rv.isPersisted(a.pairingId))
        << "the transport does not call noteLinkEstablished() — the application does";

    bay.rv.noteLinkEstablished(a.pairingId);
    EXPECT_TRUE(bay.rv.isPersisted(a.pairingId))
        << "E57 — no separate opt-in action was taken, and none was needed";
    ASSERT_TRUE(bay.rv.secretStore());
    EXPECT_EQ(bay.rv.secretStore()->list().size(), 1u);

    // Still individually revocable (7.4d) — the opt-out, and now the only one.
    bay.rv.revoke(a.pairingId);
    EXPECT_FALSE(bay.rv.isPersisted(a.pairingId));
    EXPECT_TRUE(bay.rv.secretStore()->list().empty());
}

TEST(PpcpRendezvous, ACompletedPairingIsNotRememberedWithNoStoreInstalled)
{
    Bay bay;
    ASSERT_TRUE(bay.open());   // no setSecretStore() — this run declined persistence
    Scanner s(bay.code.uri);
    ASSERT_TRUE(s.ok);
    Bay::Arrival a = bay.dial(s, 0x52);
    ASSERT_TRUE(a.linked);

    bay.rv.noteLinkEstablished(a.pairingId);
    EXPECT_FALSE(bay.rv.isPersisted(a.pairingId));
}

// 11.1a — "indistinguishable from one established by a scanned code", and E57
// makes that true of remembering too: adoptGuidedPairing() never builds this
// entry until 11.5g's affirm-and-verify has both happened, so there is
// nothing left to ask permission for.
TEST(PpcpRendezvous, AGuidedPairingIsRememberedAutomaticallyByTheSameRule)
{
    PpcpRendezvous rv;
    rv.setSecretStore(makeEphemeralPairingStore());

    std::uint8_t sid[PPCP_RV_SID_BYTES];
    std::uint8_t psk[PPCP_RV_PSK_MAX];
    ASSERT_TRUE(csprngBytes(sid, sizeof sid));
    ASSERT_TRUE(csprngBytes(psk, sizeof psk));
    ppcp_rv_keys keys;
    ASSERT_EQ(ppcp_rv_derive(sid, sizeof sid, psk, sizeof psk, &keys), PPCP_OK);

    std::string id, err;
    ASSERT_TRUE(rv.adoptGuidedPairing(sid, keys, "Guided phone", &id, &err)) << err;

    EXPECT_TRUE(rv.isPersisted(id));
    ASSERT_TRUE(rv.secretStore());
    EXPECT_EQ(rv.secretStore()->list().size(), 1u);
}

// ── RT-16 — no PRK from a `mu > 1` code is persisted (7.4f) ────────────────
TEST(PpcpRendezvous, RT16_APairingFromAMultiUseCodeIsSessionScopedAndCannotBePersisted)
{
    PpcpRendezvous rv;
    rv.setSecretStore(makeEphemeralPairingStore());
    std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};

    PpcpRendezvous::Config three;
    three.maxUses = 3;
    PublishedCode multi;
    std::string err;
    ASSERT_TRUE(rv.publish(three, eps, nullptr, &multi, &err)) << err;

    // 7.4f — "a pairing established from a multi-use code is session-scoped,
    // because its key material is held by every peer that scanned that code."
    // The predicate is libppcp's (ppcp_rv_may_persist) and this host does not
    // second-guess it.
    std::string why;
    EXPECT_FALSE(rv.persist(multi.pairingId, &why));
    EXPECT_NE(why.find("7.4f"), std::string::npos) << why;
    EXPECT_TRUE(rv.secretStore()->list().empty());

    // E57's automatic remembering is the same predicate, so it refuses too —
    // a link established on this pairing must not silently persist it.
    rv.noteLinkEstablished(multi.pairingId);
    EXPECT_FALSE(rv.isPersisted(multi.pairingId));
    EXPECT_TRUE(rv.secretStore()->list().empty());

    // The single-use case in the same run, so the refusal is shown to be about
    // `mu` and not about the store.
    PublishedCode single;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &single, &err)) << err;
    EXPECT_TRUE(rv.persist(single.pairingId, &why)) << why;
    EXPECT_EQ(rv.secretStore()->list().size(), 1u);

    // And a `mu: 3` code really does still admit three pairings — one of
    // which noteLinkEstablished() above already spent — so the refusal above
    // is about persistence and not about the code being quietly narrowed to
    // `mu: 1` in all but name.
    CodeStatus st;
    ASSERT_TRUE(rv.status(multi.pairingId, &st));
    EXPECT_EQ(st.maxUses, 3u);
    EXPECT_EQ(st.usesRemaining, 2u);
}

TEST(PpcpRendezvous, APersistedPairingIsReloadedFromPrkAndNeverFromThePairingSecret)
{
    // 5.1c — "a peer that persists a pairing persists PRK and derives from it,
    // never the original psk".  The store holds 32 bytes; nothing in it could
    // reconstruct a psk, and loadPersisted() proves the reloaded pairing still
    // resolves the identities the original code produced.
    std::string pairingId, uri;
    std::uint8_t prk[PPCP_RV_KEY_BYTES] = {0};
    {
        PpcpRendezvous rv;
        rv.setSecretStore(makeEphemeralPairingStore());
        std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};
        PublishedCode c;
        std::string err;
        ASSERT_TRUE(rv.publish({}, eps, nullptr, &c, &err)) << err;
        ASSERT_TRUE(rv.persist(c.pairingId, &err)) << err;
        pairingId = c.pairingId;
        uri = c.uri;
        // What the store holds is 32 bytes and nothing else: no psk, no sid,
        // no endpoints.  Lift them out here, because the store belongs to `rv`
        // and dies with it.
        ASSERT_EQ(rv.secretStore()->list().size(), 1u);
        ASSERT_TRUE(rv.secretStore()->get(pairingId, prk));
    }
    // The first PpcpRendezvous is gone, and with it every key it held (7.2d).

    PpcpRendezvous fresh;
    auto store2 = makeEphemeralPairingStore();
    ASSERT_TRUE(store2->put(pairingId, prk));
    fresh.setSecretStore(std::move(store2));
    EXPECT_EQ(fresh.loadPersisted(), 1u);

    // 3.4b — the reloaded pairing resolves the advertisement of a peer holding
    // the same key material.
    Scanner s(uri);
    ASSERT_TRUE(s.ok);
    std::uint8_t rn[PPCP_RV_RN_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::uint8_t rid[PPCP_RV_RID_BYTES];
    ASSERT_EQ(ppcp_rv_rid(s.keys.k_id, rn, rid), PPCP_OK);
    EXPECT_EQ(fresh.resolveRid(rn, rid), pairingId);

    // A persisted pairing has no expiry and no use limit: those are properties
    // of a CODE (7.4a).
    CodeStatus st;
    ASSERT_TRUE(fresh.status(pairingId, &st));
    EXPECT_EQ(st.expUnixS, 0u);
    EXPECT_TRUE(st.persisted);
}

// ── RT-8 — `rid` rotates, and resolves under the correct K_id only ─────────
TEST(PpcpRendezvous, RT8_RidChangesWithEveryRegistrationAndResolvesUnderOneKeyOnly)
{
    PpcpRendezvous rv;
    std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};
    PublishedCode mine, other;
    std::string err;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &mine, &err)) << err;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &other, &err)) << err;

    Scanner ours(mine.uri), theirs(other.uri);
    ASSERT_TRUE(ours.ok);
    ASSERT_TRUE(theirs.ok);

    // 3.4a — regenerated on every service registration.  Two registrations,
    // two `rn`, two different `rid`, both resolving to the same pairing: that
    // is the unlinkability property and the recognisability property at once.
    std::uint8_t rn1[PPCP_RV_RN_BYTES] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18};
    std::uint8_t rn2[PPCP_RV_RN_BYTES] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    std::uint8_t rid1[PPCP_RV_RID_BYTES], rid2[PPCP_RV_RID_BYTES];
    ASSERT_EQ(ppcp_rv_rid(ours.keys.k_id, rn1, rid1), PPCP_OK);
    ASSERT_EQ(ppcp_rv_rid(ours.keys.k_id, rn2, rid2), PPCP_OK);
    EXPECT_NE(hexOf(rid1, sizeof rid1), hexOf(rid2, sizeof rid2));
    EXPECT_EQ(rv.resolveRid(rn1, rid1), mine.pairingId);
    EXPECT_EQ(rv.resolveRid(rn2, rid2), mine.pairingId);

    // Under a different pairing's K_id it resolves to that pairing and never
    // to ours — one HMAC per held pairing, no ambiguity.
    std::uint8_t ridT[PPCP_RV_RID_BYTES];
    ASSERT_EQ(ppcp_rv_rid(theirs.keys.k_id, rn1, ridT), PPCP_OK);
    EXPECT_EQ(rv.resolveRid(rn1, ridT), other.pairingId);

    // 3.4c — a `rid` belonging to nobody resolves to nothing, and the browser
    // has no branch that connects anyway.
    ppcp_rv_keys stranger{};
    std::uint8_t sid[PPCP_RV_SID_BYTES] = {9}, psk[16] = {9};
    ASSERT_EQ(ppcp_rv_derive(sid, sizeof sid, psk, sizeof psk, &stranger), PPCP_OK);
    std::uint8_t ridS[PPCP_RV_RID_BYTES];
    ASSERT_EQ(ppcp_rv_rid(stranger.k_id, rn1, ridS), PPCP_OK);
    EXPECT_TRUE(rv.resolveRid(rn1, ridS).empty());
}

// ── RT-7, browser half — what the host reads, and what it never publishes ──
TEST(PpcpRendezvous, RT7_TheBrowserReadsOnlyRnRidAndPvAndNeverDialsAnUnresolvedRid)
{
    PpcpRendezvous rv;
    std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};
    PublishedCode c;
    std::string err;
    ASSERT_TRUE(rv.publish({}, eps, nullptr, &c, &err)) << err;
    Scanner s(c.uri);
    ASSERT_TRUE(s.ok);

    std::uint8_t rn[PPCP_RV_RN_BYTES] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18};
    std::uint8_t rid[PPCP_RV_RID_BYTES];
    ASSERT_EQ(ppcp_rv_rid(s.keys.k_id, rn, rid), PPCP_OK);
    char instance[PPCP_RV_INSTANCE_NAME_MAX] = {0};
    ASSERT_EQ(ppcp_rv_instance_name(rid, instance), PPCP_OK);

    auto resolver = [&](const std::uint8_t *r, const std::uint8_t *i) {
        return rv.resolveRid(r, i);
    };

    // A record carrying exactly 3.3a's keys, plus a key from a future draft
    // that 3.3a says a receiver ignores.
    auto buildTxt = [](const std::vector<std::string> &kvs) {
        std::vector<std::uint8_t> t;
        for (const auto &kv : kvs) {
            t.push_back(static_cast<std::uint8_t>(kv.size()));
            t.insert(t.end(), kv.begin(), kv.end());
        }
        return t;
    };
    const std::vector<std::uint8_t> txt = buildTxt(
        {"txtvers=1", "pv=1.0", "role=capture", "rn=" + hexOf(rn, sizeof rn),
         "rid=" + hexOf(rid, sizeof rid), "future=whatever"});

    RvAdvertisement ad;
    ad.instanceName = instance;
    ASSERT_TRUE(parseTxtRecord(txt.data(), txt.size(), &ad));
    EXPECT_EQ(ad.pv, "1.0");
    EXPECT_EQ(ad.role, "capture");
    EXPECT_TRUE(ad.hasRn);
    EXPECT_TRUE(ad.hasRid);
    // 3.3c — the record stays under 200 bytes so it fits a single response.
    EXPECT_LT(txt.size(), 200u);

    DialDecision d = decideDial(ad, 1, resolver);
    EXPECT_TRUE(d.dial) << d.why;
    EXPECT_EQ(d.pairingId, c.pairingId);

    // 3.4c — an advertisement whose `rid` resolves to nothing is NOT dialled.
    RvAdvertisement stranger = ad;
    stranger.rid[0] ^= 0xff;
    char sn[PPCP_RV_INSTANCE_NAME_MAX] = {0};
    ASSERT_EQ(ppcp_rv_instance_name(stranger.rid, sn), PPCP_OK);
    stranger.instanceName = sn;
    EXPECT_FALSE(decideDial(stranger, 1, resolver).dial);

    // 3.2a — an instance name that does not derive from `rid` is malformed.
    RvAdvertisement misnamed = ad;
    misnamed.instanceName = "Marks-iPhone";
    EXPECT_FALSE(decideDial(misnamed, 1, resolver).dial);

    // 3.3a — the browser filters on MAJOR before connecting.
    RvAdvertisement v2 = ad;
    v2.pv = "2.0";
    EXPECT_FALSE(decideDial(v2, 1, resolver).dial);
    RvAdvertisement ranged = ad;
    ranged.pv = "1.0-1.2";
    EXPECT_TRUE(decideDial(ranged, 1, resolver).dial);
    RvAdvertisement noPv = ad;
    noPv.pv.clear();
    EXPECT_FALSE(decideDial(noPv, 1, resolver).dial);

    // 3.3a — an unrecognised key is ignored and is not a parse failure; a
    // malformed length prefix is.
    std::vector<std::uint8_t> broken = txt;
    broken[0] = 0xff;
    RvAdvertisement junk;
    EXPECT_FALSE(parseTxtRecord(broken.data(), broken.size(), &junk));

    // 3.5b / RV §2 — the host BROWSES and never advertises.  There is no
    // register call in ppcp_discovery.cpp and no socket bound to 5353 in this
    // process; the browser reaches the platform responder over local IPC.  The
    // API surface is the assertion: RvBrowser has start/stop/fd/process and
    // nothing that publishes.
    std::unique_ptr<RvBrowser> b = makePlatformBrowser();
    // ⚠ Widened from `__APPLE__` when Linux gained DNS-SD via Avahi's compat
    // shim.  The condition is "a real backend is compiled in", which is what
    // the assertion is actually about — leaving it Apple-only would have meant
    // the Linux port had the capability and nothing asserted it.
#if defined(__APPLE__) || defined(PP_HAVE_DNS_SD)
    ASSERT_TRUE(b);
    EXPECT_NE(b->describe().find("browse only"), std::string::npos);
#endif
}

TEST(PpcpRendezvous, DiscoveryFailureIsSilentAndNeverAnErrorState)
{
    // 3.6a MUST NOT treat discovery failure as an error state; 3.6b makes the
    // fallback the pairing code, without user-visible failure.  The shape of
    // that obligation in code is that there is no error channel at all: start()
    // returns a bool nobody has to surface, process() returns false when the
    // browse dies, and makePlatformBrowser() returns null where there is no
    // client.  A test cannot assert the absence of a dialog, but it can assert
    // that a browser that never starts is usable and quiet.
    std::unique_ptr<RvBrowser> b = makePlatformBrowser();
    if (!b) return;   // conformant: no client on this platform
    EXPECT_EQ(b->fd(), -1) << "no socket before start()";
    EXPECT_FALSE(b->process()) << "processing a browse that was never started";
    b->stop();        // idempotent, and not an error
}

// ── RT-9 — the diagnostic export ───────────────────────────────────────────
TEST(PpcpRendezvous, RT9_ADiagnosticExportTakenRightAfterPairingCarriesNoSecretAndNoPayload)
{
    Bay bay;
    PpcpRendezvous::Config cfg;
    cfg.displayName = "Bay 3";
    ASSERT_TRUE(bay.open(cfg));
    bay.rv.setSecretStore(makeEphemeralPairingStore());

    Scanner s(bay.code.uri);
    ASSERT_TRUE(s.ok);
    Bay::Arrival a = bay.dial(s, 0x51);
    ASSERT_TRUE(a.linked);
    bay.rv.noteLinkEstablished(a.pairingId);
    ASSERT_TRUE(bay.rv.persist(a.pairingId));

    const std::string report = bay.rv.diagnosticExport();
    ASSERT_FALSE(report.empty());

    // 7.2b — no pairing secret and no derived key.
    struct Forbidden { const char *what; std::string hex; };
    const std::vector<Forbidden> secrets = {
        {"psk", hexOf(s.payload.psk, s.payload.psk_len)},
        {"sid (raw)", hexOf(s.payload.sid, PPCP_RV_SID_BYTES)},
        {"PRK", hexOf(s.keys.prk, PPCP_RV_KEY_BYTES)},
        {"K_tls", hexOf(s.keys.k_tls, PPCP_RV_KEY_BYTES)},
        {"K_id", hexOf(s.keys.k_id, PPCP_RV_KEY_BYTES)},
    };
    for (const auto &f : secrets)
        EXPECT_EQ(report.find(f.hex), std::string::npos)
            << f.what << " reached the diagnostic export";

    // 4.4c — no payload, in any form.  The URI is the payload; so is the
    // base64url body on its own, in case somebody ever strips the scheme.
    EXPECT_EQ(report.find(bay.code.uri), std::string::npos);
    EXPECT_EQ(report.find(bay.code.uri.substr(5)), std::string::npos);
    EXPECT_EQ(report.find("ppcp:"), std::string::npos);

    // What it DOES say is enough to diagnose a pairing failure: which pairings
    // are held, their state, and what the resolver did.
    EXPECT_NE(report.find(bay.code.pairingId), std::string::npos);
    EXPECT_NE(report.find("resolver:"), std::string::npos);
    EXPECT_NE(report.find("persisted=yes"), std::string::npos);
    // Session.id is a protocol identifier the peer already knows and is not a
    // secret; it is the one thing that makes a report cross-referenceable.
    EXPECT_NE(report.find(bay.code.sessionId), std::string::npos);
}

TEST(PpcpRendezvous, ReapErasesWhatCanNeverAuthenticateAgainAndKeepsWhatCan)
{
    // 7.2d — key material is erased on revocation or session close.  reap() is
    // the periodic half of that: an expired code nobody used still holds a
    // K_tls until something removes it.
    PpcpRendezvous rv;
    std::uint64_t now = 1000;
    rv.setClock([&] { return now; });
    std::vector<RvEndpoint> eps{RvEndpoint{"192.168.1.20", 7788}};

    PpcpRendezvous::Config shortLived;
    shortLived.codeLifetimeS = 10;
    PpcpRendezvous::Config longLived;
    longLived.codeLifetimeS = 10000;

    PublishedCode a, b;
    std::string err;
    ASSERT_TRUE(rv.publish(shortLived, eps, nullptr, &a, &err));
    ASSERT_TRUE(rv.publish(longLived, eps, nullptr, &b, &err));
    EXPECT_EQ(rv.codes().size(), 2u);

    now += 11;
    EXPECT_EQ(rv.reap(), 1u);
    ASSERT_EQ(rv.codes().size(), 1u);
    EXPECT_EQ(rv.codes()[0].pairingId, b.pairingId);
}

// ── Erratum E25 — `RV` 3.3d's ONE range syntax, and the case that was wrong ─
//
// A version range is `LOW` or `LOW-HIGH`, each endpoint `MAJOR.MINOR`; both
// endpoints are inclusive and SHARE a MAJOR; a bare `LOW` is `LOW-LOW`; support
// across two MAJORs is several ranges separated by a comma, most preferred
// first.  3.3e adds that the same syntax is used for `detail.supported` and is
// NOT used for `hello.versions`, which stays an ordered list.
//
// ⚠ THE COMMA CASE IS WHY THIS TEST EXISTS.  The parser this replaced read bare
// MAJORs, so `1.0-1.2` worked by accident (both endpoints parse to 1) and
// `2.0-2.1,1.4-1.6` did not: the high endpoint ran to the comma, giving 2..2,
// and a peer offering major 1 was silently refused.  Two conformant peers would
// have failed to discover each other with nothing to say why.
TEST(PpcpRendezvous, ThePvRangeSyntaxOfErratumE25)
{
    // A bare LOW is the range LOW-LOW.
    EXPECT_TRUE(pvAcceptsMajor("1.0", 1));
    EXPECT_FALSE(pvAcceptsMajor("1.0", 2));

    // LOW-HIGH within one MAJOR, inclusive at both ends.
    EXPECT_TRUE(pvAcceptsMajor("1.0-1.2", 1));
    EXPECT_FALSE(pvAcceptsMajor("1.0-1.2", 2));

    // Several ranges, comma separated, most preferred first — the case that
    // was broken.  BOTH majors are accepted, and order is preference and not
    // precedence, so it changes no answer here.
    EXPECT_TRUE(pvAcceptsMajor("2.0-2.1,1.4-1.6", 2));
    EXPECT_TRUE(pvAcceptsMajor("2.0-2.1,1.4-1.6", 1));
    EXPECT_FALSE(pvAcceptsMajor("2.0-2.1,1.4-1.6", 3));
    EXPECT_TRUE(pvAcceptsMajor("2.0,1.0", 1));

    // 3.3d — "a reader that cannot parse a range IGNORES that advertisement
    // rather than guessing", so anything malformed ANYWHERE makes the whole
    // record unusable.  Accepting on the components that did parse would be
    // guessing what the rest meant.
    EXPECT_FALSE(pvAcceptsMajor("1", 1))          << "a bare MAJOR is not an endpoint";
    EXPECT_FALSE(pvAcceptsMajor("1-2", 1))        << "10.1b endpoints are MAJOR.MINOR";
    EXPECT_FALSE(pvAcceptsMajor("1.0-2.0", 1))    << "the endpoints must share a MAJOR";
    EXPECT_FALSE(pvAcceptsMajor("1.4-1.2", 1))    << "HIGH below LOW is not a range";
    EXPECT_FALSE(pvAcceptsMajor("1.0,,2.0", 1))   << "an empty component";
    EXPECT_FALSE(pvAcceptsMajor("1.0,", 1))       << "a trailing comma";
    EXPECT_FALSE(pvAcceptsMajor("1.x", 1))        << "a non-numeric MINOR";
    EXPECT_FALSE(pvAcceptsMajor(".0", 1));
    EXPECT_FALSE(pvAcceptsMajor("1.", 1));
    EXPECT_FALSE(pvAcceptsMajor("", 1));
    // One bad component poisons the record even though the other would match.
    EXPECT_FALSE(pvAcceptsMajor("1.0-1.2,junk", 1));
}

// ── Erratum E21 — `RV` 5.3a1, and the intermittent failure it prevents ─────
//
// The PSK identity is `0x01 || rn2 || tag`, 17 octets, and it is BINARY (5.3f).
// Several widely-used TLS stacks carry a PSK identity as a C string and take
// its length with `strlen`: an embedded `0x00` truncates it, the server
// resolves nothing, and the handshake fails ROUGHLY ONE CONNECTION IN SIXTEEN —
// 17 octets each with a 1-in-256 chance of being zero.  At a driving range that
// is diagnosed as a network fault, and `ppcp_transport_test` carries the
// demonstration against OpenSSL's own TLS 1.2 PSK interface.
//
// So a live connection draws through `ppcp_rv_psk_identity_draw()`, which
// redraws `rn2` until neither it nor the tag carries a zero.  Nothing changes at
// the server: 5.3b recomputes the tag from the `rn2` it received exactly as
// before, which is what the second half of this test asserts by resolving the
// drawn identity against the publisher that issued the pairing.
TEST(PpcpRendezvous, ADrawnPskIdentityCarriesNoZeroOctet)
{
    Bay bay;
    ASSERT_TRUE(bay.open());

    // ⚠ ONE DRAW IS NOT EVIDENCE.  A zero-bearing identity appears about once
    // in sixteen, so a single clean draw says nothing at all — which is exactly
    // why the defect survived a session of manual testing.  256 draws puts the
    // chance of missing a broken implementation below 1 in 10^6.
    for (int i = 0; i < 256; ++i) {
        PskIdentity id;
        std::string why;
        ASSERT_TRUE(bay.rv.drawPskIdentity(bay.code.pairingId, &id, &why)) << why;
        ASSERT_EQ(id.size(), static_cast<std::size_t>(PPCP_RV_PSK_IDENTITY_BYTES));
        EXPECT_EQ(id[0], 0x01) << "5.3a — the identity is versioned";
        for (std::size_t b = 0; b < id.size(); ++b)
            ASSERT_NE(id[b], 0x00) << "draw " << i << " carried a zero at octet " << b;
    }

    // 5.3b — the server recomputes the tag from the `rn2` it received, so a
    // drawn identity resolves exactly as an undrawn one does.  Without this the
    // test would pass for a draw that produced 17 zero-free bytes of nonsense.
    PskIdentity id;
    ASSERT_TRUE(bay.rv.drawPskIdentity(bay.code.pairingId, &id));
    ResolvedPairing out;
    EXPECT_TRUE(bay.rv.identityResolver()(id.data(), id.size(), out));
    EXPECT_EQ(out.pairingId, bay.code.pairingId);

    // 5.3d — an unknown pairing is refused, and says nothing about which.
    PskIdentity none;
    EXPECT_FALSE(bay.rv.drawPskIdentity("pairing:never-issued", &none));
}

// ── E56 — the settings pairing store, exercised for the first time ──────────
//
// ⛔ This store had NO runtime coverage before erratum E56.  It was the macOS
// keychain, compiled here only so the suite failed at the link line if it
// stopped building, and never executed — the login keychain cannot be unlocked
// from a non-Aqua session.  On Windows and Linux there was no store at all.
// So the one thing §7.4 rests on was, on every platform, either unrun or
// absent.  It is now an ordinary INI file and runs everywhere.
//
// ⚠ EVERY TEST HERE REDIRECTS QSettings TO A TEMPORARY DIRECTORY.  ppSettings()
// is UserScope, so without this the suite would read and WRITE THE DEVELOPER'S
// REAL PinPointStudio.ini — including revoking pairings they actually hold.
class SettingsPairingStore : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_dir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
    }
    void TearDown() override
    {
        // Put it back, so a later test in this binary is not still redirected.
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    }
    QTemporaryDir m_dir;
};

// The round trip the keychain never let us assert: a PRK goes in, the same 32
// bytes come out, and `list()` names it.
TEST_F(SettingsPairingStore, APrkSurvivesTheRoundTripAndIsListed)
{
    auto store = makePlatformPairingStore("test.pairings");
    // ⛔ Never null since E56 — this is the assertion that Windows and Linux
    // now get persistence at all.
    ASSERT_TRUE(store);

    std::uint8_t in[PPCP_RV_KEY_BYTES];
    for (std::size_t i = 0; i < PPCP_RV_KEY_BYTES; ++i)
        in[i] = static_cast<std::uint8_t>(i * 7 + 1);

    ASSERT_TRUE(store->put("pairing:abc", in));

    std::uint8_t out[PPCP_RV_KEY_BYTES];
    std::memset(out, 0, sizeof out);
    ASSERT_TRUE(store->get("pairing:abc", out));
    EXPECT_EQ(0, std::memcmp(in, out, PPCP_RV_KEY_BYTES));

    const auto ids = store->list();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], "pairing:abc");
}

// 7.4d — revocation is honoured immediately by this side.  ⛔ No soft delete.
TEST_F(SettingsPairingStore, EraseRemovesItAndASecondEraseSaysSo)
{
    auto store = makePlatformPairingStore("test.pairings");
    ASSERT_TRUE(store);
    std::uint8_t prk[PPCP_RV_KEY_BYTES];
    std::memset(prk, 0xAB, sizeof prk);
    ASSERT_TRUE(store->put("pairing:abc", prk));

    EXPECT_TRUE(store->erase("pairing:abc"));
    EXPECT_TRUE(store->list().empty());

    std::uint8_t out[PPCP_RV_KEY_BYTES];
    EXPECT_FALSE(store->get("pairing:abc", out));
    // Erasing what is not there is false, not a crash and not a lie.
    EXPECT_FALSE(store->erase("pairing:abc"));
}

// A persisted pairing survives the process that wrote it — which is the whole
// point of §7.4, and what Windows and Linux could not do before E56.
TEST_F(SettingsPairingStore, APairingSurvivesAFreshStoreInstance)
{
    std::uint8_t prk[PPCP_RV_KEY_BYTES];
    std::memset(prk, 0x5A, sizeof prk);
    {
        auto writer = makePlatformPairingStore("test.pairings");
        ASSERT_TRUE(writer);
        ASSERT_TRUE(writer->put("pairing:persist", prk));
    }
    auto reader = makePlatformPairingStore("test.pairings");
    ASSERT_TRUE(reader);
    std::uint8_t out[PPCP_RV_KEY_BYTES];
    ASSERT_TRUE(reader->get("pairing:persist", out));
    EXPECT_EQ(0, std::memcmp(prk, out, PPCP_RV_KEY_BYTES));
}

// ⛔ A corrupt or short row is a FAILURE, never a zero key.  A silently zeroed
// PRK would complete a handshake with nobody and read as a revocation at the
// far end, which is the hardest kind of bug to trace from the other side.
TEST_F(SettingsPairingStore, AShortRowFailsRatherThanYieldingAZeroKey)
{
    {
        QSettings s(QSettings::IniFormat, QSettings::UserScope,
                    QStringLiteral("PinPointStudio"), QStringLiteral("PinPointStudio"));
        s.setValue(QStringLiteral("ppcp/pairings/pairing:short"), QStringLiteral("aabbcc"));
        s.sync();
    }
    auto store = makePlatformPairingStore("test.pairings");
    ASSERT_TRUE(store);
    std::uint8_t out[PPCP_RV_KEY_BYTES];
    std::memset(out, 0xEE, sizeof out);
    EXPECT_FALSE(store->get("pairing:short", out));
    // Untouched: the caller's buffer is not half-filled on failure.
    for (std::size_t i = 0; i < PPCP_RV_KEY_BYTES; ++i) EXPECT_EQ(out[i], 0xEE);
}

// RT-9 — the diagnostic export gets a name and a count, never a byte of a key.
// ⛔ And it says "NOT protected storage" out loud, so an export never implies a
// guarantee the run did not have.
TEST_F(SettingsPairingStore, DescribeCarriesNoKeyMaterialAndAdmitsWhatItIs)
{
    auto store = makePlatformPairingStore("test.pairings");
    ASSERT_TRUE(store);
    std::uint8_t prk[PPCP_RV_KEY_BYTES];
    std::memset(prk, 0x11, sizeof prk);
    ASSERT_TRUE(store->put("pairing:abc", prk));

    const std::string d = store->describe();
    EXPECT_NE(d.find("NOT protected storage"), std::string::npos);
    EXPECT_NE(d.find("1 pairing"), std::string::npos);
    // The key is 0x11 repeated; its hex must not appear anywhere in the text.
    EXPECT_EQ(d.find("1111111111"), std::string::npos);
}
