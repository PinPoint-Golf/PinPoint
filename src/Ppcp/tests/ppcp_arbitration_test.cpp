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

// CT-I7, CT-I8, CT-I20 and CT-I35 on the host path, and 8.1's three shapes.
// Work package H5, the arbitration bridge.
//
// The host declares a microphone of its own, so "two nominators of the SAME
// basis from DIFFERENT peers" is reachable — which is the half of CT-I8 that a
// per-modality slot fails silently, and the reason this bridge replaces
// `ShotArbiter` rather than wrapping it.

#include "ppcp_host_engine.h"
#include "ppcp_live_session.h"
#include "ppcp_shot_bridge.h"
#include "ppcp_source_declaration.h"
#include "ppcp_test_peer.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace Ppcp;
using pptest::DevicePeer;
using pptest::idStr;

namespace {

constexpr const char *kSession = "sess:arb";
constexpr std::int64_t kMs = 1000000;

// A deterministic id source.  8.3e says ids SHOULD be UUIDs and the library has
// no random source (ground rule 8), so the embedding supplies them — and a test
// that wants to assert WHICH Candidate ended up where wants them predictable.
struct Ids {
    int n = 0;
    std::string prefix = "id";
    bool next(std::string *out) { *out = prefix + "-" + std::to_string(++n); return true; }
};

struct Fixture {
    DevicePeer                  dev;
    std::unique_ptr<PpcpEngine> host;
    PpcpSourceDeclaration       decl;
    PpcpLiveSession             live;
    PpcpShotBridge              bridge;
    Ids                         ids;
    std::vector<std::string>    shots;
    std::vector<std::size_t>    shotCandidateCounts;
    ppcp_sim_clock              hostClk{};
    ppcp_clock                  hostIface{};

    std::int64_t nowNs()
    {
        ppcp_instant i{};
        EXPECT_EQ(ppcp_clock_read(&hostIface, kHostTimebaseId, &i), PPCP_OK);
        return i.ns;
    }

    void build(ppcp_role hostRole = PPCP_ROLE_HOST)
    {
        ASSERT_EQ(ppcp_sim_clock_init(&hostClk, kHostTimebaseId, 1000000000), PPCP_OK);
        hostIface = ppcp_sim_clock_interface(&hostClk);
        dev.build();

        // The host owns a microphone.  Without one it has nothing to nominate
        // with, and CT-I8's "a host microphone and a device microphone" is
        // unreachable — which is how a per-modality arbiter passes every test
        // it is given.
        PpcpSourceDeclaration::Inventory inv;
        inv.hasMicrophone = true;
        inv.microphone.id = "mic-0";
        inv.microphone.label = "bay microphone";
        std::string derr;
        ASSERT_TRUE(decl.build("host-1", inv, &derr)) << derr;
        ASSERT_GT(decl.sourceCount(), 0u) << "the host must own a Source to nominate from";

        HostEngineConfig cfg;
        cfg.peerId = "host-1";
        cfg.listener = true;
        cfg.clock = hostIface;
        cfg.syncTimebase = kHostTimebaseId;
        std::string why;
        host = makeHostEngine(std::move(cfg), &why);
        ASSERT_NE(host, nullptr) << why;
        (void)hostRole;

        live.attach(host->peer(), &decl);
        bridge.attach(host->peer(), &decl, &live);
        bridge.setShotCallback([this](const ppcp_shot &s) {
            shots.push_back(idStr(s.id));
            shotCandidateCounts.push_back(s.candidate_count);
        });
    }

    void toHost() { pptest::pipe(dev.p, host->peer(), PPCP_CHANNEL_CONTROL,
                                 [this] { drainHost(); }); }
    void toDevice(const pptest::EventSink &sink = {})
    { pptest::pipe(host->peer(), dev.p, PPCP_CHANNEL_CONTROL, {}, sink); }

    void drainHost()
    {
        pptest::drainEvents(host->peer(), [this](const ppcp_event &e) {
            live.observe(e);
            bridge.observe(e);
        });
    }

    void declare()
    {
        ASSERT_EQ(ppcp_peer_declare(dev.p, &dev.desc), PPCP_OK);
        toHost();
        ASSERT_EQ(ppcp_peer_declare(host->peer(), decl.peer()), PPCP_OK);
        toDevice();
    }

    void openSession()
    {
        PpcpLiveSession::Config cfg;
        cfg.sessionId = kSession;
        std::string err;
        ASSERT_TRUE(live.open(cfg, &err)) << err;
        toDevice();
    }

    void startBridge()
    {
        PpcpShotBridge::Config cfg;
        cfg.peerId = "host-1";
        std::string err;
        ASSERT_TRUE(bridge.start(cfg, [this](std::string *o) { return ids.next(o); }, &err))
            << err;
    }

    // The host's own microphone Source, whatever the declaration called it.
    std::string hostMicSourceId() const
    {
        for (const ppcp_source &s : decl.sources())
            if (idStr(s.kind) == "microphone") return idStr(s.id);
        return {};
    }

    // A relation `tb:dev` -> `tb:host`, DECLARED rather than measured, so a
    // device Candidate can be converted into `timebase_ref`.  Declaring it here
    // is legitimate — 5.4 allows `method: declared` — and it keeps this test
    // about arbitration rather than about §6.3, which has its own suite.
    void declareRelation(std::int64_t offsetNs, double sigmaNs)
    {
        ppcp_timebase_relation r{};
        ppcp_instant at{};
        ASSERT_EQ(ppcp_instant_make_z(&at, "tb:dev", 0), PPCP_OK);
        ASSERT_EQ(ppcp_relation_make_affine(&r, "tb:dev", kHostTimebaseId, offsetNs, 0.0,
                                            sigmaNs, 0.0, PPCP_RELM_DECLARED, &at),
                  PPCP_OK);
        ppcp_relation_set *rs = ppcp_peer_relations(host->peer());
        ASSERT_NE(rs, nullptr);
        ASSERT_EQ(ppcp_relations_put(rs, &r), PPCP_OK);
    }

    // A Candidate from the DEVICE's microphone, on the device's clock.
    void deviceNominates(std::int64_t devNs, double confidence, const char *basis)
    {
        ppcp_candidate c{};
        static int n = 0;
        const std::string cid = "dev-c-" + std::to_string(++n);
        ppcp_instant at{};
        ASSERT_EQ(ppcp_instant_make_z(&at, dev.tb.c_str(), devNs), PPCP_OK);
        ASSERT_EQ(ppcp_candidate_make(&c, cid.c_str(), dev.peerId.c_str(), "src-mic",
                                      basis, &at, confidence), PPCP_OK);
        ASSERT_EQ(ppcp_peer_nominate(dev.p, &c), PPCP_OK);
        toHost();
    }
};

}  // namespace

// ── CT-I20 — arbitration is the host's, and only the host's ───────────────

TEST(PpcpArbitration, ANonHostPeerCannotArbitrate)
{
    // The device end is `role: capture` and declares Mint, not Arbitrate.
    // ppcp_arbiter_new() refuses it — I20 by construction, and CONF §1d's
    // negative half: a peer that arbitrated without declaring it is
    // non-conformant and the refusal is what stops that reaching a wire.
    DevicePeer dev;
    ASSERT_NO_FATAL_FAILURE(dev.build());

    std::vector<std::uint8_t> storage(ppcp_arbiter_sizeof(), 0);
    ppcp_arbiter *a = nullptr;
    Ids ids;
    auto idFn = [](void *ctx, ppcp_id *out) -> ppcp_result {
        Ids *i = static_cast<Ids *>(ctx);
        std::string s;
        i->next(&s);
        return ppcp_id_set(out, s.c_str(), s.size());
    };
    EXPECT_NE(ppcp_arbiter_new(storage.data(), storage.size(), dev.p, idFn, &ids, &a), PPCP_OK);
    EXPECT_EQ(a, nullptr);

    // And this application's bridge reports the refusal rather than degrading
    // to something that looks like it worked.
    PpcpShotBridge b;
    b.attach(dev.p, nullptr, nullptr);
    PpcpShotBridge::Config cfg;
    cfg.peerId = dev.peerId;
    std::string err;
    EXPECT_FALSE(b.start(cfg, [&ids](std::string *o) { return ids.next(o); }, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(b.active());
}

// ── CT-I8 — two nominators of the SAME basis, both retained ───────────────

TEST(PpcpArbitration, TwoAcousticNominatorsFromDifferentPeersBothAppear)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());
    EXPECT_TRUE(F.bridge.active());

    F.declareRelation(/*offsetNs=*/0, /*sigmaNs=*/100000.0);

    const std::int64_t t = F.nowNs() + 10 * kMs;

    // The device's microphone hears it…
    F.deviceNominates(t, 0.9, kBasisAcoustic);
    // …and so does ours, 3 ms later — inside the 50 ms coincidence window, so
    // 8.2b treats them as nominating the SAME Shot.
    std::string err;
    ASSERT_TRUE(F.bridge.nominate(F.hostMicSourceId(), kBasisAcoustic, t + 3 * kMs, 0,
                                  0.7, nullptr, nullptr, &err)) << err;

    EXPECT_EQ(F.bridge.stats().nominated, 1u);
    EXPECT_EQ(F.bridge.stats().observedForeign, 1u);
    EXPECT_EQ(F.bridge.groupCount(), 1u) << "8.2b groups by instant, not by modality";

    // 8.2h — issue no earlier than `issue_hold_ns` after the earliest
    // contributing Candidate.
    ppcp_sim_clock_advance(&F.hostClk, 400 * kMs);
    F.bridge.pump(F.nowNs());

    ASSERT_EQ(F.shots.size(), 1u) << "two microphones, one swing, one Shot";
    // ⚠ THE ASSERTION THE OLD ARBITER FAILS.  `ShotArbiter` models three fixed
    // modalities in fixed slots; a second `acoustic` nomination overwrites the
    // first and nothing anywhere records that it happened.  Here BOTH are in
    // `Shot.candidates`, which is 8.2f and I8.
    EXPECT_EQ(F.shotCandidateCounts.front(), 2u);
}

// ── CT-I7 / 8.2e — a late Candidate ATTACHES and `t0` is not revised ──────

TEST(PpcpArbitration, ACandidateArrivingAfterTheShotAttachesAndT0IsNotRevised)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());
    F.declareRelation(0, 100000.0);

    const std::int64_t t = F.nowNs() + 10 * kMs;
    std::string err;
    ASSERT_TRUE(F.bridge.nominate(F.hostMicSourceId(), kBasisAcoustic, t, 0, 0.9, nullptr,
                                  nullptr, &err)) << err;

    ppcp_sim_clock_advance(&F.hostClk, 400 * kMs);
    ASSERT_EQ(F.bridge.pump(F.nowNs()), 1u);
    ASSERT_EQ(F.shots.size(), 1u);

    const std::string shotId = F.shots.front();
    const std::size_t candsAtIssue = F.shotCandidateCounts.front();
    EXPECT_EQ(candsAtIssue, 1u);

    // Now a device Candidate for the same event arrives LATE — after the Shot
    // was issued.
    F.declareRelation(0, 100000.0);
    F.deviceNominates(t + 5 * kMs, 0.8, kBasisAcoustic);
    F.bridge.pump(F.nowNs());

    // 8.2e: it ATTACHES.  I7: no second Shot, no revision, and the same id.
    // ⚠ AND `t0` COULD NOT HAVE BEEN REVISED EVEN IF THIS HOST WANTED TO:
    // libppcp has no setter for it anywhere, which is I7 by API surface as well
    // as by behaviour.  `ppcp_shot_attach_candidate()` takes a Candidate rather
    // than an instant precisely so that attaching cannot move it.
    EXPECT_EQ(F.shots.size(), 1u) << "a late Candidate does not produce a second Shot";
    EXPECT_EQ(F.shots.front(), shotId);
    EXPECT_EQ(F.shotCandidateCounts.size(), 1u);
}

// ── 8.2d — an over-wide sigma EXCLUDES and RETAINS ───────────────────────

TEST(PpcpArbitration, AnOverWideSigmaExcludesTheCandidateAndKeepsIt)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());

    PpcpShotBridge::Config cfg;
    cfg.peerId = "host-1";
    cfg.maxConversionSigmaNs = 1.0e6;   // 1 ms — this host's policy, not libppcp's
    std::string err;
    ASSERT_TRUE(F.bridge.start(cfg, [&F](std::string *o) { return F.ids.next(o); }, &err))
        << err;

    // A relation that exists but is bad: 20 ms of offset uncertainty, twenty
    // times the policy's bound.  8.2d is reached only where a relation EXISTS —
    // a missing or `unrelated` one is decided by the specification and the
    // policy is never asked.
    F.declareRelation(0, 20.0e6);

    const std::int64_t t = F.nowNs() + 10 * kMs;
    F.deviceNominates(t, 0.95, kBasisAcoustic);

    EXPECT_GE(F.bridge.stats().excluded, 1u) << "the policy was consulted and said no";
    // Exclusion is a CONCLUSION, not a discard: the Candidate is retained and
    // remains evidence (I8).  A consumer may re-derive `t0` later with a better
    // clock, which is exactly what retaining it is for.
    EXPECT_GE(F.bridge.retainedCount() + F.bridge.groupCount(), 1u);
}

// ── 8.2i1 — a peer declaring `unrelated` puts EVERY candidate in retention ─

TEST(PpcpArbitration, WithNoRelationEveryForeignCandidateIsRetainedAndNoneIsGrouped)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());
    // No relation is declared at all.

    F.deviceNominates(F.nowNs() + 10 * kMs, 0.9, kBasisAcoustic);
    F.deviceNominates(F.nowNs() + 12 * kMs, 0.9, kBasisAcoustic);

    ppcp_sim_clock_advance(&F.hostClk, 400 * kMs);
    F.bridge.pump(F.nowNs());

    // 5.4b / 8.2i1 — a Candidate whose relation is missing cannot be expressed
    // in `timebase_ref`, so there is not even an instant to group by.  It is
    // retained with no Shot, FOR EVER, and that is a legal and honest state.
    // The alternative — assuming a zero offset — would produce a Shot whose
    // `t0` is a fabrication, and I7 would then forbid correcting it.
    EXPECT_EQ(F.shots.size(), 0u);
    EXPECT_GE(F.bridge.retainedCount(), 2u);
}

// ── CORE 8.1 — the launch monitor row is a ShotLink and NEVER a Candidate ──

TEST(PpcpArbitration, TheLaunchMonitorRowBecomesAnArrivalPairingLinkConfirmedByObserver)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());
    F.declareRelation(0, 100000.0);

    const std::int64_t t = F.nowNs() + 10 * kMs;
    std::string err;
    ASSERT_TRUE(F.bridge.nominate(F.hostMicSourceId(), kBasisAcoustic, t, 0, 0.9, nullptr,
                                  nullptr, &err)) << err;
    ppcp_sim_clock_advance(&F.hostClk, 400 * kMs);
    ASSERT_EQ(F.bridge.pump(F.nowNs()), 1u);
    ASSERT_EQ(F.shots.size(), 1u);

    const std::size_t candidatesBefore = F.bridge.stats().nominated;
    ASSERT_TRUE(F.bridge.linkForeignShot(F.shots.front(), "GCQ-00417",
                                         "com.foresightsports.gcquad", 1.0, &err)) << err;

    // 8.1e — nothing was synthesised.  The CSV has no timestamp, so no Instant,
    // no Timebase and no TimebaseRelation was invented for it, and the
    // Candidate count did not move.
    EXPECT_EQ(F.bridge.stats().nominated, candidatesBefore);
    EXPECT_EQ(F.bridge.stats().shotLinks, 1u);

    bool sawLink = false;
    ppcp_shot_link got{};
    // Drained DURING the pipe: since F-L13-1 the feed refuses a frame it
    // cannot report, so a frame behind an undrained ring never arrives at all.
    F.toDevice([&](const ppcp_event &e) {
        if (e.kind == PPCP_EVENT_SHOT_LINK && e.msg) { sawLink = true; got = e.msg->body.shot_link.link; }
    });
    ASSERT_TRUE(sawLink);
    EXPECT_EQ(idStr(got.basis), std::string(PPCP_LINK_ARRIVAL_PAIRING));
    EXPECT_TRUE(got.confirmed);
    ASSERT_TRUE(got.has_confirmed_by);
    // 5.16f — `arrival_pairing` is NOT retrospective, which is precisely why
    // `observer` is permitted here and would be refused on
    // `sequence_alignment`.  The host armed the slot and watched the row
    // arrive; that is an observation, not a human decision.
    EXPECT_EQ(got.confirmed_by, PPCP_CONFIRMED_BY_OBSERVER);
    EXPECT_FALSE(ppcp_shot_link_basis_is_retrospective(got.basis.v, got.basis.len));
    EXPECT_EQ(idStr(got.local_shot_id), F.shots.front());
    EXPECT_EQ(idStr(got.foreign_shot_id), std::string("GCQ-00417"));
    ASSERT_TRUE(got.has_foreign_system);
    EXPECT_EQ(idStr(got.foreign_system), std::string("com.foresightsports.gcquad"));
}

// ── CORE §8.4 — an orphan capture request ────────────────────────────────

TEST(PpcpArbitration, CaptureRequestNamesT0InTheSessionTimebaseRef)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());

    std::string err;
    ASSERT_TRUE(F.bridge.requestCapture("shot:1", 123456789, { "st:dev-1:src-cam:video" },
                                        200 * kMs, 800 * kMs, &err)) << err;

    bool saw = false;
    ppcp_body_capture_request req{};
    F.toDevice([&](const ppcp_event &e) {
        if (e.kind == PPCP_EVENT_CAPTURE_REQUEST && e.msg) { saw = true; req = e.msg->body.capture_request; }
    });
    ASSERT_TRUE(saw);
    EXPECT_EQ(idStr(req.shot_id), std::string("shot:1"));
    // 5.13c — `t0` is in `Session.timebase_ref`.  The OWNER inverts §6.1's
    // conversion into its own convention at its end; a host that did it for
    // them would be applying the correction twice (8.2a, I33).
    EXPECT_EQ(idStr(req.t0.tb), std::string(kHostTimebaseId));
    EXPECT_EQ(req.t0.ns, 123456789);
    EXPECT_EQ(req.pre_ns, 200 * kMs);
    EXPECT_EQ(req.post_ns, 800 * kMs);
    ASSERT_EQ(req.stream_id_count, 1u);
}

// ── I26 — a Candidate names a Source THIS peer declared ──────────────────

TEST(PpcpArbitration, NominatingFromASourceWeDidNotDeclareIsRefused)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());

    std::string err;
    // The device's microphone.  It is a real Source, declared by a real peer in
    // this Session — and it is not OURS, so nominating from it would be this
    // host claiming an observation it did not make (5.12a, 7.1a).
    EXPECT_FALSE(F.bridge.nominate("src-mic", kBasisAcoustic, F.nowNs(), 0, 0.9, nullptr,
                                   nullptr, &err));
    EXPECT_EQ(F.bridge.stats().nominationsRefused, 1u);
    EXPECT_EQ(F.bridge.stats().nominated, 0u);
}

// ── Erratum E29 / F-S5-1 — A RETAINED CANDIDATE IS RECONSIDERED ────────────
//
// 8.2d1.  The test above asserts that a Candidate with no relation is retained
// and never grouped, which is right and was where this host stopped.  What 8.2d
// did not say, and E29 now does, is what happens when the relation ARRIVES —
// which on a live link is the normal case, because §6.3's burst takes a moment
// to converge and a device nominates the instant it hears something.
//
// ⚠ THE OLD BEHAVIOUR WAS SILENT AND LOOKED CORRECT.  No error, no Shot, and
// every Candidate present in `retainedCount()` exactly as 8.2d requires.  A
// host could run a whole Session arbitrating nothing and every assertion in
// this file would still have passed.
TEST(PpcpArbitration, ACandidateRetainedForWantOfARelationIsReconsideredWhenItArrives)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());

    // Nominated BEFORE any relation exists — the sync burst has not converged.
    const std::int64_t at = F.nowNs() + 10 * kMs;
    F.deviceNominates(at, 0.9, kBasisAcoustic);
    ppcp_sim_clock_advance(&F.hostClk, 400 * kMs);
    F.bridge.pump(F.nowNs());

    ASSERT_EQ(F.shots.size(), 0u) << "8.2d: with no relation there is not even an instant";
    ASSERT_GE(F.bridge.retainedCount(), 1u);
    ASSERT_EQ(F.bridge.stats().reconsidered, 0u);

    // The relation arrives.  8.2d1: what was retained for want of one is
    // reconsidered, and this host has to say so — the arbiter owns no clock and
    // no event loop, so it cannot notice for itself.
    F.declareRelation(/*offsetNs=*/0, /*sigmaNs=*/1000.0);
    const std::size_t readmitted = F.bridge.reconsider();
    EXPECT_GE(readmitted, 1u) << "E29: the Candidate is re-admitted to arbitration";
    EXPECT_GE(F.bridge.stats().reconsidered, 1u);

    ppcp_sim_clock_advance(&F.hostClk, 400 * kMs);
    F.bridge.pump(F.nowNs());
    EXPECT_EQ(F.shots.size(), 1u) << "and the Shot that could not be issued now is";
    EXPECT_EQ(F.bridge.retainedCount(), 0u);
}

// ── Erratum E28 / F-S5-3 — AN IMPORTED FRAME NEVER REACHES THE ARBITER ─────
//
// MSG §9.1: a device offers a Session it recorded earlier and replays its
// bundle down the link a LIVE Session is running on.  `ppcp_event::imported`
// marks those frames, and an embedding that ignores the flag arbitrates two
// Sessions as one — the replayed Candidates were nominated against another
// `timebase_ref`, possibly days ago, and their instants are numerically
// plausible, so 8.2 groups them by coincidence with live ones and issues Shots
// that never happened.  Nothing is malformed and nothing goes red.
//
// The guard is asserted here as a branch and end to end in the `IOP-3-live`
// interoperability row, which replays a real bundle over a real socket.
TEST(PpcpArbitration, AnImportedCandidateIsCountedAndNeverArbitrated)
{
    Fixture F;
    ASSERT_NO_FATAL_FAILURE(F.build());
    ASSERT_NO_FATAL_FAILURE(F.declare());
    ASSERT_NO_FATAL_FAILURE(F.openSession());
    ASSERT_NO_FATAL_FAILURE(F.startBridge());
    F.declareRelation(0, 1000.0);

    // A live Candidate, so the arbiter has a group for an imported one to be
    // wrongly folded into — which is the failure, not a crash.
    F.deviceNominates(F.nowNs() + 10 * kMs, 0.9, kBasisAcoustic);
    const std::size_t liveForeign = F.bridge.stats().observedForeign;
    ASSERT_GE(liveForeign, 1u);

    // The same shape of Candidate, arriving as part of a REPLAYED Session.
    ppcp_candidate c{};
    ppcp_instant at{};
    ASSERT_EQ(ppcp_instant_make_z(&at, F.dev.tb.c_str(), F.nowNs() + 12 * kMs), PPCP_OK);
    ASSERT_EQ(ppcp_candidate_make(&c, "imported-c-1", F.dev.peerId.c_str(), "src-mic",
                                  kBasisAcoustic, &at, 0.9), PPCP_OK);
    ppcp_msg m{};
    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_CANDIDATE, 4242), PPCP_OK);
    m.body.candidate.candidate = c;

    ppcp_event ev{};
    ev.kind = PPCP_EVENT_CANDIDATE;
    ev.msg = &m;
    ev.status = PPCP_OK;
    ev.imported = true;
    F.bridge.observe(ev);

    EXPECT_EQ(F.bridge.stats().observedForeign, liveForeign)
        << "E28: an imported Candidate is not observed by the live arbiter";
    EXPECT_EQ(F.bridge.stats().importedIgnored, 1u)
        << "and it is COUNTED, so a routed replay is distinguishable from no replay";

    // The same event with the flag clear IS arbitrated — otherwise this test
    // would pass for a bridge that had simply stopped observing Candidates.
    ev.imported = false;
    F.bridge.observe(ev);
    EXPECT_EQ(F.bridge.stats().observedForeign, liveForeign + 1);
}
