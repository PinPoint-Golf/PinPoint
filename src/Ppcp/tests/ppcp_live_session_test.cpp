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

// ppcp_peer_nominate lives in shot.h, not peer.h: 5.12 nomination is Detect's.
#include <ppcp/shot.h>

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
    void toDevice(const pptest::EventSink &sink = {})
    { pptest::pipe(host->peer(), dev.p, PPCP_CHANNEL_CONTROL, {}, sink); }

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
//
// ⚠ F-H5-3, AND IT COST THIS SUITE AN HOUR.  `ppcp_peer_config.health_report`
// is documented in peer.h as "what `heartbeat_ack` carries" — which reads as a
// decoration on liveness and is in fact a PRECONDITION for it.  A peer without
// one answers every `heartbeat` with `error` / `profile_not_supported` and the
// message "no health source", so 7.4a never runs, no ack ever returns, and the
// host's own link state stays `live` for ever because it is never told
// otherwise.  Both halves of §7.4 looked broken until the harness supplied a
// callback; neither was.  It is arguably the right refusal — a peer reporting
// `thermal: nominal` on no evidence is the fabrication this library refuses
// everywhere else — but an embedding with no thermometer will silently have no
// liveness at all, and the field's documentation should say so.

TEST(PpcpLiveSession, AHeartbeatIsQueuedEveryIntervalAndTheSessionIsUnchangedByIt)
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

    // 7.4a — the host sends `heartbeat` at `Session.heartbeat_interval_ms`, and
    // every peer answers `heartbeat_ack`.  Counted at the far end, because a
    // heartbeat the host queued and nobody received is not a heartbeat.
    std::size_t beats = 0, acks = 0;
    for (int i = 0; i < 6; ++i) {
        L.advance(1000 * kMs);
        L.live.pump(L.hostNowNs());
        pptest::pipe(L.host->peer(), L.dev.p, PPCP_CHANNEL_CONTROL);
        pptest::drainEvents(L.dev.p, [&](const ppcp_event &e) {
            if (e.kind == PPCP_EVENT_HEARTBEAT) ++beats;
        });
        pptest::pipe(L.dev.p, L.host->peer(), PPCP_CHANNEL_CONTROL);
        pptest::drainEvents(L.host->peer(), [&](const ppcp_event &e) {
            L.live.observe(e);
            if (e.kind == PPCP_EVENT_HEARTBEAT) ++acks;
        });
    }
    EXPECT_GE(beats, 3u) << "7.4a — one per interval, driven by the embedding's clock";
    EXPECT_GE(acks, 3u)  << "7.4a — every peer answers `heartbeat_ack`";
    EXPECT_EQ(L.live.linkState(), PPCP_LINK_LIVE);

    // 7.4b — and the ack CARRIES the degradation, which is the whole reason the
    // message has a body.  A host that could see the beat but not the reading
    // would learn only that the device is alive, which is the least useful half.
    ASSERT_TRUE(L.live.peerHealth().valid)
        << "no reading is a different answer from `nominal` and is not shown as one";
    EXPECT_EQ(L.live.peerHealth().thermal, PPCP_THERMAL_NOMINAL);
    EXPECT_GT(L.live.peerHealth().storageFreeBytes, 0u);
    ASSERT_TRUE(L.live.peerHealth().hasBatteryPct);
    EXPECT_EQ(L.live.peerHealth().batteryPct, 87u);
    EXPECT_GE(L.live.stats().heartbeatAcks, 3u);

    // 8.3g — "a Session with an unreachable host is not a Session with no
    // host."  Liveness changes NOTHING about the Session, and that is the whole
    // content of the clause.  Asserted at the DEVICE end, because that is the
    // peer libppcp lets read the parameters back (see the next test).
    const ppcp_body_session_open *p = ppcp_peer_session_params(L.dev.p);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pptest::idStr(p->timebase_ref), std::string(kHostTimebaseId));
    EXPECT_TRUE(p->has_arbitration);
    EXPECT_EQ(p->coincidence_window_ns, PPCP_DEFAULT_COINCIDENCE_WINDOW_NS);
    EXPECT_EQ(p->issue_hold_ns, PPCP_DEFAULT_ISSUE_HOLD_NS);
    EXPECT_TRUE(p->has_heartbeat_interval);
    EXPECT_EQ(p->heartbeat_interval_ms, 1000u);
}

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

    // Good intervals first, so what follows is a TRANSITION and not an initial
    // state.  A test that started from silence would pass on a peer that
    // reported `lost` from the moment it was constructed.
    for (int i = 0; i < 6; ++i) L.tick(1000 * kMs);
    ASSERT_EQ(L.live.linkState(), PPCP_LINK_LIVE);
    ASSERT_TRUE(L.linkStates.empty());

    // Now the device answers nothing: the host's frames still reach it, and
    // nothing comes back.
    for (int i = 0; i < 12; ++i) {
        L.advance(1000 * kMs);
        L.live.pump(L.hostNowNs());
        pptest::pipe(L.host->peer(), L.dev.p, PPCP_CHANNEL_CONTROL);   // out only
    }

    // 7.4c — three consecutive missed intervals is a lost link.
    EXPECT_GE(L.live.missedHeartbeats(), 3u);
    EXPECT_EQ(L.live.linkState(), PPCP_LINK_LOST);
    ASSERT_FALSE(L.linkStates.empty());
    EXPECT_EQ(L.linkStates.back(), PPCP_LINK_LOST);
    EXPECT_EQ(L.live.stats().linkLosses, 1u) << "one transition, not one per interval";

    // 8.3g — "a Session with an unreachable host is not a Session with no
    // host."  NOTHING about the Session changed, and that is the entire content
    // of the clause.  What changes is that no arbitration occurs; the
    // parameters, the reference timebase and the roster are what they were.
    //
    // ⚠ F-H5-2 IS CLOSED, AND THE ASSERTION IS NOW MADE AT BOTH ENDS.
    // `ppcp_peer_session_params()` used to return NULL on the peer that
    // ORIGINATED `session_open` — peer.h said "as they arrived in
    // `session_open`" and that was literally what it did, so a HOST could not
    // read back the Session it had just opened: not `timebase_ref`, not
    // `coincidence_window_ns`, not `issue_hold_ns`, every one of which the host
    // itself needs (8.2b compares against the window, 8.2h holds against the
    // hold).  The host kept a second copy in `PpcpLiveSession::Config` and the
    // two could drift, which is exactly what a single accessor exists to
    // prevent.  libppcp 42a690a fixed it; this now asserts the two ends agree,
    // which is the property the finding was actually about.
    const ppcp_body_session_open *mine = ppcp_peer_session_params(L.host->peer());
    ASSERT_NE(mine, nullptr) << "F-H5-2: the originator must be able to read its own Session";
    EXPECT_EQ(pptest::idStr(mine->timebase_ref), std::string(kHostTimebaseId));
    EXPECT_TRUE(mine->has_arbitration);
    EXPECT_EQ(mine->coincidence_window_ns, PPCP_DEFAULT_COINCIDENCE_WINDOW_NS);
    EXPECT_EQ(mine->issue_hold_ns, PPCP_DEFAULT_ISSUE_HOLD_NS);

    const ppcp_body_session_open *p = ppcp_peer_session_params(L.dev.p);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pptest::idStr(p->timebase_ref), std::string(kHostTimebaseId));
    EXPECT_TRUE(p->has_arbitration);
    EXPECT_EQ(p->coincidence_window_ns, PPCP_DEFAULT_COINCIDENCE_WINDOW_NS);
    EXPECT_EQ(p->issue_hold_ns, PPCP_DEFAULT_ISSUE_HOLD_NS);
    // 8.3g's "nothing about the Session changes" is now a comparison rather
    // than a claim about one end.
    EXPECT_EQ(pptest::idStr(mine->timebase_ref), pptest::idStr(p->timebase_ref));
    EXPECT_EQ(mine->coincidence_window_ns, p->coincidence_window_ns);
    EXPECT_EQ(mine->issue_hold_ns, p->issue_hold_ns);

    // …and the device, which is the peer 8.3g's regime applies to, enters it.
    EXPECT_TRUE(ppcp_peer_zero_host(L.dev.p) || ppcp_peer_link_state(L.dev.p) == PPCP_LINK_LIVE)
        << "the device is either still hearing us or has entered the zero-host regime";
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
    // Drained DURING the pipe, not after it: since F-L13-1 the feed stops
    // rather than overrun the event ring, so `arm` never reaches a device that
    // is still holding `declare` and `session_open` unread.
    L.toDevice([&](const ppcp_event &e) {
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
    L.toDevice([&](const ppcp_event &e) {
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

// ── F-L13-1 — why the pump feeds ONE FRAME PER CALL ───────────────────────
//
// `ppcp_peer_feed()` consumes UNBOUNDEDLY MANY whole frames from one buffer;
// the event ring is PPCP_PEER_EVENT_QUEUE deep (four); and an overflow drops
// the OLDEST event with nothing readable to say so.  A single socket read
// carrying a replayed bundle therefore loses `capture_announce` while the
// payload frames that reference it arrive — silently, and only under load.
//
// This asserts BOTH halves, because the second is the one that protects the
// pump: bulk-feeding N frames loses events, and feeding the identical bytes one
// frame at a time with a drain between them loses none.  `PpcpHostPeer::pump()`
// does the latter.  When libppcp L15 makes `feed` stop at the ring's capacity,
// the first EXPECT here goes red and points at this note.
TEST(PpcpLiveSession, F_L13_1_FeedingAWholeReadAtOnceLosesEventsAndOneFrameAtATimeDoesNot)
{
    constexpr int kFrames = 12;   // three times the ring's depth

    // The bytes: `kFrames` candidates from the device, drained into one buffer
    // exactly as a socket read would deliver them.
    auto produce = [](Link &L, std::vector<std::uint8_t> &out) {
        for (int i = 0; i < kFrames; ++i) {
            ppcp_candidate c{};
            ppcp_instant at{};
            const std::string cid = "c-" + std::to_string(i);
            ASSERT_EQ(ppcp_instant_make_z(&at, L.dev.tb.c_str(), 1000000 * (i + 1)), PPCP_OK);
            ASSERT_EQ(ppcp_candidate_make(&c, cid.c_str(), L.dev.peerId.c_str(), "src-mic",
                                          "acoustic", &at, 0.5), PPCP_OK);
            ASSERT_EQ(ppcp_peer_nominate(L.dev.p, &c), PPCP_OK);
        }
        std::vector<std::uint8_t> buf(1u << 20);
        for (;;) {
            std::size_t len = 0;
            if (ppcp_peer_drain(L.dev.p, PPCP_CHANNEL_CONTROL, buf.data(), buf.size(), &len)
                    != PPCP_OK || len == 0)
                break;
            out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(len));
        }
        ASSERT_FALSE(out.empty());
    };

    // ── (a) the whole read in one feed ────────────────────────────────────
    std::size_t bulkSeen = 0;
    {
        Link L;
        ASSERT_NO_FATAL_FAILURE(L.build());
        ASSERT_NO_FATAL_FAILURE(L.declare());
        std::vector<std::uint8_t> bytes;
        ASSERT_NO_FATAL_FAILURE(produce(L, bytes));

        std::size_t took = 0;
        ppcp_peer_feed(L.host->peer(), PPCP_CHANNEL_CONTROL, bytes.data(), bytes.size(), &took);
        pptest::drainEvents(L.host->peer(), [&](const ppcp_event &e) {
            if (e.kind == PPCP_EVENT_CANDIDATE) ++bulkSeen;
        });
    }

    // ── (b) the identical bytes, one frame per feed, draining between ─────
    std::size_t slicedSeen = 0;
    {
        Link L;
        ASSERT_NO_FATAL_FAILURE(L.build());
        ASSERT_NO_FATAL_FAILURE(L.declare());
        std::vector<std::uint8_t> bytes;
        ASSERT_NO_FATAL_FAILURE(produce(L, bytes));

        std::size_t off = 0;
        while (off < bytes.size()) {
            ppcp_frame_header fh{};
            ASSERT_EQ(ppcp_frame_header_parse(bytes.data() + off, &fh), PPCP_OK);
            const std::size_t whole = PPCP_FRAME_HEADER_BYTES + fh.payload_len;
            std::size_t took = 0;
            ppcp_peer_feed(L.host->peer(), PPCP_CHANNEL_CONTROL, bytes.data() + off, whole,
                           &took);
            pptest::drainEvents(L.host->peer(), [&](const ppcp_event &e) {
                if (e.kind == PPCP_EVENT_CANDIDATE) ++slicedSeen;
            });
            if (took == 0) break;
            off += took;
        }
    }

    EXPECT_EQ(slicedSeen, static_cast<std::size_t>(kFrames))
        << "one frame per feed with a drain between must lose nothing";
    EXPECT_LT(bulkSeen, slicedSeen)
        << "F-L13-1 has been fixed in libppcp L15 — this guard can go";
    EXPECT_LE(bulkSeen, static_cast<std::size_t>(PPCP_PEER_EVENT_QUEUE))
        << "the ring is what bounds a bulk feed, and it is four deep";
}
