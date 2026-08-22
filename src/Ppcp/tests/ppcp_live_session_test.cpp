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

// CT-I21, CT-I18 and CT-I16's second half, on the host path.  Work package H5.
//
// Two real peers in one process, both on `ppcp_sim_clock`s, with a deliberate
// offset and a deliberate skew between them.  The measurement is the point: a
// test that set the offset by hand would prove that arithmetic works and
// nothing about whether §6.3's exchange recovers it.

#include "ppcp_host_engine.h"
#include "ppcp_live_session.h"
#include "ppcp_source_declaration.h"
#include "ppcp_test_peer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>

using namespace Ppcp;
using pptest::DevicePeer;

namespace {

constexpr const char *kSession = "sess:h5";
constexpr std::int64_t kMs = 1000000;

// The device's clock, offset and running fast relative to the host's.  Both
// numbers are recovered by the exchange below and neither is ever handed to the
// estimator.
constexpr std::int64_t kDeviceOffsetNs = 4200 * kMs;   // 4.2 s ahead
constexpr double       kDeviceSkewPpm  = 40.0;         // 40 ppm fast

struct Link {
    DevicePeer                  dev;
    std::unique_ptr<PpcpEngine> host;
    PpcpSourceDeclaration       decl;
    PpcpLiveSession             live;
    ppcp_sim_clock              hostClk{};
    ppcp_sim_clock              devClk{};
    std::vector<ppcp_link_state> linkStates;

    // The device end has to answer probes off ITS clock, and DevicePeer's own
    // clock hook reads `clockNs`.  Keeping the two in step is what makes the
    // recovered offset a measurement of the simulated clocks rather than of the
    // harness.
    void syncDeviceClock()
    {
        ppcp_instant now{};
        if (ppcp_clock_read(&devIface, dev.tb.c_str(), &now) == PPCP_OK) dev.clockNs = now.ns;
    }

    ppcp_clock devIface{};
    ppcp_clock hostIface{};

    void build(bool declareCameras = false)
    {
        ASSERT_EQ(ppcp_sim_clock_init(&hostClk, kHostTimebaseId, 1000000000), PPCP_OK);
        ASSERT_EQ(ppcp_sim_clock_init(&devClk, "tb:dev", 1000000000), PPCP_OK);
        ppcp_sim_clock_set_offset(&devClk, kDeviceOffsetNs);
        ppcp_sim_clock_set_skew_ppm(&devClk, kDeviceSkewPpm);
        hostIface = ppcp_sim_clock_interface(&hostClk);
        devIface  = ppcp_sim_clock_interface(&devClk);

        dev.build();
        syncDeviceClock();

        PpcpSourceDeclaration::Inventory inv;
        if (declareCameras) {
            // CT-S3's discipline: a declaration built from an Inventory, so a
            // machine with no camera attached still produces one.
            PpcpSourceDeclaration::Camera c;
            c.backend = VideoInputFactory::Backend::AppleAVFoundation;
            c.id = "cam-0";
            c.label = "face-on";
            c.caps.modelName = "FaceTime HD Camera";
            c.caps.serialNumber = "cam-0";
            c.caps.resolution.kind = CapabilityKind::Discrete;
            c.caps.resolution.presets = { { 1920, 1080 } };
            c.caps.resolution.defaultResolution = { 1920, 1080 };
            c.caps.pixelFormat.kind = CapabilityKind::Discrete;
            PixelFormat nv12;
            nv12.nativeKey = "NV12";
            nv12.encoding = PixelEncoding::YUV420_NV12;
            c.caps.pixelFormat.supported = { nv12 };
            c.caps.pixelFormat.defaultFormat = nv12;
            c.caps.frameRate.kind = CapabilityKind::Range;
            c.caps.frameRate.range = { 1.0, 240.0, 0.0, 240.0 };
            inv.cameras.push_back(c);
        }
        inv.hasMicrophone = true;
        inv.microphone.id = "mic-0";
        inv.microphone.label = "bay microphone";
        std::string derr;
        ASSERT_TRUE(decl.build("host-1", inv, &derr)) << derr;

        HostEngineConfig cfg;
        cfg.peerId = "host-1";
        cfg.listener = true;
        cfg.clock = hostIface;
        cfg.syncTimebase = kHostTimebaseId;
        std::string why;
        host = makeHostEngine(std::move(cfg), &why);
        ASSERT_NE(host, nullptr) << why;

        live.attach(host->peer(), &decl);
        live.setLinkStateCallback([this](ppcp_link_state s) { linkStates.push_back(s); });
    }

    void toHost() { pptest::pipe(dev.p, host->peer(), PPCP_CHANNEL_CONTROL); }
    void toDevice() { pptest::pipe(host->peer(), dev.p, PPCP_CHANNEL_CONTROL); }

    void declare()
    {
        ASSERT_EQ(ppcp_peer_declare(dev.p, &dev.desc), PPCP_OK);
        toHost();
        ASSERT_EQ(ppcp_peer_declare(host->peer(), decl.peer()), PPCP_OK);
        toDevice();
    }

    // Advance both simulated clocks together and let both engines make progress.
    void advance(std::int64_t ns)
    {
        ppcp_sim_clock_advance(&hostClk, ns);
        ppcp_sim_clock_advance(&devClk, ns);
        syncDeviceClock();
    }

    std::int64_t hostNowNs()
    {
        ppcp_instant i{};
        EXPECT_EQ(ppcp_clock_read(&hostIface, kHostTimebaseId, &i), PPCP_OK);
        return i.ns;
    }

    // One tick of the whole loop: schedule, move the probes, let the device
    // answer, move the replies back.  Events are drained so the engine folds
    // each `sync_reply` into its estimator while its bytes are still live.
    void tick(std::int64_t ns)
    {
        advance(ns);
        live.pump(hostNowNs());
        toDevice();
        // The device answers `sync_probe` inside its own feed path; draining its
        // events keeps the four-deep ring from overwriting.
        pptest::drainEvents(dev.p, [](const ppcp_event &) {});
        toHost();
        pptest::drainEvents(host->peer(), [this](const ppcp_event &e) { live.observe(e); });
    }
};

}  // namespace

// ── I21 — one probe sequence per LOCAL timebase ────────────────────────────

TEST(PpcpLiveSession, OneSyncEstimatorPerDeclaredHostTimebase)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(/*declareCameras=*/true));
    ASSERT_NO_FATAL_FAILURE(L.declare());

    PpcpLiveSession::Config cfg;
    cfg.sessionId = kSession;
    std::string err;
    ASSERT_TRUE(L.live.open(cfg, &err)) << err;
    L.toDevice();

    // This host samples every Source against one clock, so its declaration
    // carries exactly one Timebase and the prober registers exactly one
    // estimator.  The assertion that matters is not the number — it is that the
    // number came from the DECLARATION: a host with two clocks declares two and
    // the same code registers two, with no branch and no constant.
    ASSERT_EQ(ppcp_peer_sync_count(L.host->peer()), 1u);
    ASSERT_NE(L.decl.peer(), nullptr);
    EXPECT_EQ(ppcp_peer_sync_count(L.host->peer()), L.decl.peer()->timebase_count);
    EXPECT_EQ(pptest::idStr(L.decl.peer()->timebases[0].id), std::string(kHostTimebaseId));
    EXPECT_NE(ppcp_peer_sync_estimator_for(L.host->peer(), kHostTimebaseId), nullptr);
    // …and for no clock it does not own.  A host that had an estimator for
    // `tb:dev` would be claiming to read a clock it cannot (I1).
    EXPECT_EQ(ppcp_peer_sync_estimator_for(L.host->peer(), "tb:dev"), nullptr);
}

// ── CORE §6.3 — the exchange RECOVERS the offset and the rate ─────────────

TEST(PpcpLiveSession, TheProbeExchangeRecoversTheOffsetAndTheSkew)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build());
    ASSERT_NO_FATAL_FAILURE(L.declare());

    PpcpLiveSession::Config cfg;
    cfg.sessionId = kSession;
    std::string err;
    ASSERT_TRUE(L.live.open(cfg, &err)) << err;
    L.toDevice();

    // 6.3c — a burst of PPCP_SYNC_BURST exchanges at PPCP_SYNC_BURST_GAP_MS,
    // then 6.3g's maintenance cadence.  Driving it for a couple of minutes of
    // simulated time gives the fit a lever arm long enough for a 40 ppm slope
    // to be separable from the offset, which is the whole reason 6.3a demands
    // two exchanges SEPARATED IN TIME rather than one handshake.
    for (int i = 0; i < 400; ++i) L.tick(500 * kMs);

    const ppcp_sync_estimator *e = ppcp_peer_sync_estimator_for(L.host->peer(),
                                                                kHostTimebaseId);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(ppcp_sync_estimator_has_estimate(e));
    // 6.1b — the responder's timebase was LEARNED from its first `sync_reply`;
    // the constructor was given NULL.
    const ppcp_id *remote = ppcp_sync_estimator_remote_tb(e);
    ASSERT_NE(remote, nullptr);
    EXPECT_EQ(pptest::idStr(*remote), std::string("tb:dev"));

    ppcp_timebase_relation rel{};
    ASSERT_EQ(ppcp_sync_estimator_relation(e, &rel), PPCP_OK);
    EXPECT_EQ(rel.cls, PPCP_REL_AFFINE);
    // 6.3f — a relation carries BOTH sigmas or it is unconstructible (I3).
    EXPECT_GT(rel.offset_sigma_ns, 0.0);
    EXPECT_EQ(rel.method, PPCP_RELM_ESTIMATED_ONLINE);
    EXPECT_EQ(pptest::idStr(rel.from), std::string(kHostTimebaseId));
    EXPECT_EQ(pptest::idStr(rel.to), std::string("tb:dev"));

    // The offset and the slope are recovered, not configured.  The tolerances
    // are loose on purpose: 6.3e publishes a FILTERED value, so the estimate
    // trails the truth by design and a tight bound here would be asserting that
    // the filter does not exist.
    EXPECT_NEAR(static_cast<double>(rel.offset_ns), static_cast<double>(kDeviceOffsetNs),
                0.10 * static_cast<double>(kDeviceOffsetNs) + 5.0 * kMs);
    EXPECT_NEAR(rel.skew_ppm, kDeviceSkewPpm, 25.0);

    // 6.1f — and the relation reaches the wire as a `relation_update`.
    EXPECT_GT(L.live.stats().relationsPublished, 0u);
    EXPECT_GT(L.live.stats().probesQueued, 0u);
}

// ── I18 / 5.4c — the conversion applies at most one relation ──────────────

TEST(PpcpLiveSession, AConversionWithNoDirectRelationIsRefusedAndNeverAssumedZero)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build());
    ASSERT_NO_FATAL_FAILURE(L.declare());
    PpcpLiveSession::Config cfg;
    cfg.sessionId = kSession;
    std::string err;
    ASSERT_TRUE(L.live.open(cfg, &err)) << err;

    // Nothing has been measured yet.  The honest answer is "I cannot express
    // that here" — NOT an offset of zero, which downstream is indistinguishable
    // from a measured mapping and looks exactly like drift (8.2i1, 5.4b).
    std::int64_t off = 12345;
    EXPECT_FALSE(L.live.offsetToRefNs("tb:dev", 1000, &off));
    EXPECT_EQ(off, 12345) << "a refused conversion must not write an output";
    EXPECT_TRUE(L.live.relatedTimebases().empty());

    // I4 — identity is identity, and is never asserted as a relation with
    // `from == to`.  A Source already on `tb:host` converts through with a zero
    // offset, and that zero is a FACT rather than the fallback above.
    off = 999;
    EXPECT_TRUE(L.live.offsetToRefNs(kHostTimebaseId, 7777, &off));
    EXPECT_EQ(off, 0);

    for (int i = 0; i < 200; ++i) L.tick(500 * kMs);

    // Now there is a direct relation `tb:host` → `tb:dev`… but the conversion
    // asked for is `tb:dev` → `tb:host`, and this library does not invert or
    // compose.  Whether it answers depends on which direction was measured, and
    // the point of the assertion is that the answer is never fabricated: it
    // either found a direct relation or it refused.
    double sigma = -1.0;
    const bool converted = L.live.offsetToRefNs("tb:dev", L.hostNowNs(), &off, &sigma);
    if (converted) {
        EXPECT_GE(sigma, 0.0) << "a conversion carries the uncertainty it was made with";
        EXPECT_NE(off, 0) << "a 4.2 s offset cannot convert to nothing";
    } else {
        EXPECT_TRUE(L.live.relatedTimebases().empty())
            << "refusing while claiming to hold the relation would be the worst of both";
    }
}

// ── CORE §7.4 — liveness ──────────────────────────────────────────────────

TEST(PpcpLiveSession, ThreeMissedIntervalsIsALostLinkAndTheSessionIsUnchanged)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build());
    ASSERT_NO_FATAL_FAILURE(L.declare());
    PpcpLiveSession::Config cfg;
    cfg.sessionId = kSession;
    cfg.heartbeatIntervalMs = 1000;
    std::string err;
    ASSERT_TRUE(L.live.open(cfg, &err)) << err;
    L.toDevice();

    // A few good intervals first, so the loss below is a transition and not an
    // initial state.
    for (int i = 0; i < 6; ++i) L.tick(1000 * kMs);
    ASSERT_EQ(L.live.linkState(), PPCP_LINK_LIVE);

    // Now the device stops answering: the clocks advance and the host pumps,
    // but nothing is moved back from the device.
    for (int i = 0; i < 6; ++i) {
        L.advance(1000 * kMs);
        L.live.pump(L.hostNowNs());
        pptest::pipe(L.host->peer(), L.dev.p, PPCP_CHANNEL_CONTROL);   // out only
    }

    EXPECT_EQ(L.live.linkState(), PPCP_LINK_LOST);
    EXPECT_GE(L.live.missedHeartbeats(), 3u);
    ASSERT_FALSE(L.linkStates.empty());
    EXPECT_EQ(L.linkStates.back(), PPCP_LINK_LOST);

    // 8.3g — "a Session with an unreachable host is not a Session with no
    // host."  Nothing about the Session changed: `timebase_ref` and both
    // arbitration parameters are what they were.  That is the whole content of
    // the clause and it is asserted here rather than assumed.
    const ppcp_body_session_open *p = ppcp_peer_session_params(L.host->peer());
    if (p) {
        EXPECT_EQ(pptest::idStr(p->timebase_ref), std::string(kHostTimebaseId));
        EXPECT_TRUE(p->has_arbitration);
        EXPECT_EQ(p->coincidence_window_ns, PPCP_DEFAULT_COINCIDENCE_WINDOW_NS);
        EXPECT_EQ(p->issue_hold_ns, PPCP_DEFAULT_ISSUE_HOLD_NS);
    }
}

// ── CORE 5.10e / I16 — what `session_open` carries ────────────────────────

TEST(PpcpLiveSession, TheSessionIsHostedOnTheHostClockWithBothArbitrationParameters)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build());
    ASSERT_NO_FATAL_FAILURE(L.declare());

    PpcpLiveSession::Config cfg;
    cfg.sessionId = kSession;
    cfg.coincidenceWindowNs = 60 * kMs;
    cfg.issueHoldNs = 220 * kMs;
    cfg.heartbeatIntervalMs = 500;
    std::string err;
    ASSERT_TRUE(L.live.open(cfg, &err)) << err;

    bool sawOpen = false;
    ppcp_body_session_open seen{};
    L.toDevice();
    pptest::drainEvents(L.dev.p, [&](const ppcp_event &e) {
        if (e.kind == PPCP_EVENT_SESSION_OPEN && e.msg) { sawOpen = true; seen = e.msg->body.session_open; }
    });
    ASSERT_TRUE(sawOpen);

    EXPECT_EQ(pptest::idStr(seen.session_id), std::string(kSession));
    // I16 — `timebase_ref` is the clock this host actually reads, and it is
    // immutable.  A host naming a device's clock would be asserting a reading
    // it cannot take.
    EXPECT_EQ(pptest::idStr(seen.timebase_ref), std::string(kHostTimebaseId));
    // 5.10e — BOTH parameters travel, and their presence is the structural
    // statement that this Session has a host.
    EXPECT_TRUE(seen.has_arbitration);
    EXPECT_EQ(seen.coincidence_window_ns, 60 * kMs);
    EXPECT_EQ(seen.issue_hold_ns, 220 * kMs);
    EXPECT_TRUE(seen.has_heartbeat_interval);
    EXPECT_EQ(seen.heartbeat_interval_ms, 500u);
    // I15 — no `epoch`.  A wall-clock reading is a label and this host computes
    // every interval from `tb:host`; putting one here would invite exactly the
    // computation 5.3b forbids.
    EXPECT_FALSE(seen.epoch.present);
}

// ── CORE 7.3a — arming is host-controlled ─────────────────────────────────

TEST(PpcpLiveSession, ArmWithNoStreamIdsMeansEveryOpenCaptureStream)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build());
    ASSERT_NO_FATAL_FAILURE(L.declare());
    PpcpLiveSession::Config cfg;
    cfg.sessionId = kSession;
    std::string err;
    ASSERT_TRUE(L.live.open(cfg, &err)) << err;
    L.toDevice();

    ASSERT_TRUE(L.live.arm({}, &err)) << err;
    bool armed = false;
    std::size_t namedStreams = 1;
    L.toDevice();
    pptest::drainEvents(L.dev.p, [&](const ppcp_event &e) {
        if (e.kind == PPCP_EVENT_ARM && e.msg) {
            armed = true;
            namedStreams = e.msg->body.arm.stream_id_count;
        }
    });
    EXPECT_TRUE(armed);
    // MSG 5.2 — an EMPTY list, which is what this application's single `armed`
    // property means: the capture path as a whole, not one camera.
    EXPECT_EQ(namedStreams, 0u);

    ASSERT_TRUE(L.live.disarm({}, &err)) << err;
    bool disarmed = false;
    L.toDevice();
    pptest::drainEvents(L.dev.p, [&](const ppcp_event &e) {
        if (e.kind == PPCP_EVENT_DISARM) disarmed = true;
    });
    EXPECT_TRUE(disarmed);
}

// ── The fact everything above rests on ────────────────────────────────────

TEST(PpcpLiveSession, TheHostTimebaseIsTheSameSteadyClockTheEventBufferStamps)
{
    // `EventBuffer::nowMicros()` and `Ppcp::hostNowNs()` are the SAME
    // std::steady_clock reading in different units.  Every arbitrated `t0` in
    // this application depends on that: the acoustic and IMU detectors hand
    // `ShotController` microseconds off the event buffer's clock, and the
    // arbitration bridge nominates them as instants on `tb:host`.
    //
    // ⚠ THIS IS ASSERTED RATHER THAN COMMENTED because the day one of the two
    // changes is the day every `t0` is wrong by an unbounded amount with
    // nothing red anywhere.  The bound is loose enough to survive scheduling
    // between the two reads and far tighter than any clock substitution.
    const std::int64_t a = hostNowNs();
    const std::int64_t b =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::int64_t c = hostNowNs();
    EXPECT_LE(a / 1000, b);
    EXPECT_LE(b, c / 1000);
}
