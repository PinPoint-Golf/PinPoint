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

// CT-I14 (thresholds live in the application, not the protocol) and the host
// peer adapter's own obligations under CORE 5.15 and MSG 3.4.  Work package H2.
//
// CT-I14's method is `static`: "grep the implementation's protocol layer for a
// frame-rate, resolution, quality or confidence constant.  Assert every such
// threshold lives in a policy layer above it."  The grep is at the bottom of
// this file; everything above it is the behaviour the threshold produces, which
// a grep cannot see.

#include "ppcp_host_peer.h"
#include "ppcp_ingest_policy.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Ppcp;

namespace {

// A counterpart declaration, built the way a capture peer's would arrive: a
// camera Source with two profiles at named rates.
struct FakeCounterpart {
    std::vector<ppcp_capture_profile> profiles;
    std::vector<ppcp_source>          sources;
    std::vector<ppcp_timebase>        timebases;
    std::vector<ppcp_id>              declaredProfiles;
    ppcp_peer_desc                    peer{};

    // `rates` in fps; 0 means "declares no rate at all", which CORE 5.7 permits
    // and which is therefore a policy question rather than a malformed one.
    void build(const std::vector<double> &rates, const char *kind = "camera")
    {
        ppcp_timebase tb{};
        ppcp_timebase_make(&tb, "tb:dev", 6, PPCP_TB_MONOTONIC, false, 1);
        timebases.push_back(tb);

        profiles.reserve(rates.size());
        static std::vector<std::string> ids;
        ids.clear();
        for (std::size_t i = 0; i < rates.size(); ++i) ids.push_back("p" + std::to_string(i));

        for (std::size_t i = 0; i < rates.size(); ++i) {
            // A capture peer is AVFoundation-shaped, per CORE 5.6.1's table.
            ppcp_timing timing{};
            ppcp_timing_make_nominal_frame_start(&timing, 0, PPCP_PROV_ASSUMED);
            ppcp_geometry g{};
            ppcp_geometry_make_rolling_shutter(&g, 0, PPCP_PROV_ASSUMED,
                                               PPCP_ROLL_TOP_TO_BOTTOM, 1080);
            ppcp_capture_profile p{};
            ppcp_capture_profile_make(&p, ids[i].c_str(), &timing);
            if (std::strcmp(kind, "camera") == 0)
                ppcp_capture_profile_set_camera(&p, &g, PPCP_INTR_PER_FRAME);
            if (rates[i] > 0.0) {
                const int64_t mhz = static_cast<int64_t>(rates[i] * 1000.0 + 0.5);
                ppcp_capture_profile_set_rate(&p, mhz, mhz, mhz);
            }
            profiles.push_back(p);
        }

        ppcp_source s{};
        ppcp_source_make(&s, "src-1", "dev-1", kind, "tb:dev", true,
                         profiles.data(), profiles.size());
        sources.push_back(s);

        for (const char *n : { "core", "capture", "detect", "mint", "live", "offline" }) {
            ppcp_id id{};
            ppcp_id_set_z(&id, n);
            declaredProfiles.push_back(id);
        }

        ppcp_peer_desc_make(&peer, "dev-1", PPCP_ROLE_CAPTURE, ppcp_wire_version(),
                            declaredProfiles.data(), declaredProfiles.size(),
                            timebases.data(), timebases.size());
        ppcp_peer_desc_set_sources(&peer, sources.data(), sources.size());
    }
};

// A sink that records what the pump handed it and offers a scripted reply.
class RecordingEngine final : public PpcpEngine {
public:
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> fed;
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> toSend;

    ppcp_result feed(std::uint8_t channel, const std::uint8_t *b, std::size_t n) override
    {
        fed.emplace_back(channel, std::vector<std::uint8_t>(b, b + n));
        return PPCP_OK;
    }
    ppcp_result drain(std::uint8_t channel, std::uint8_t *out, std::size_t cap,
                      std::size_t *len) override
    {
        *len = 0;
        for (auto it = toSend.begin(); it != toSend.end(); ++it) {
            if (it->first != channel) continue;
            if (it->second.size() > cap) return PPCP_ERR_NOSPACE;
            std::memcpy(out, it->second.data(), it->second.size());
            *len = it->second.size();
            toSend.erase(it);
            return PPCP_OK;
        }
        return PPCP_OK;   // nothing waiting on this channel
    }
};

}  // namespace

// ── I14 / MSG 3.4b — the threshold rejects, and does not close ─────────────
TEST(PpcpIngestPolicy, ASlowPeerIsRejectedWithAReasonAndTheConnectionSurvives)
{
    FakeCounterpart c;
    c.build({ 60.0, 30.0 });   // both under this host's 120 fps floor

    PpcpIngestPolicy policy;
    const IngestVerdict v = policy.evaluate(c.peer);

    EXPECT_FALSE(v.accepted);
    // MSG 3.4a — machine-readable, and specifically NOT a free-text sentence:
    // the peer has to be able to act on it.
    EXPECT_FALSE(v.reason.empty());
    EXPECT_EQ(v.reason, "insufficient_rate");

    // 3.4a again — a rejection "does NOT close the connection". There is no
    // API here that could: evaluate() returns a verdict and touches no socket,
    // which is the shape that makes the rule unbreakable rather than merely
    // stated.
    EXPECT_EQ(v.notes.size(), 2u);
    for (const IngestVerdict::Note &n : v.notes) EXPECT_FALSE(n.accepted);
}

// ── MSG 3.4c — per-profile rejection inside an accepted declaration ────────
// "A peer MAY reject individual profiles in `notes` while accepting the
// declaration as a whole.  A profile rejected in `notes` MUST NOT be activated
// by a later `stream_open`."
TEST(PpcpIngestPolicy, TheSlowProfileIsRefusedWhileThePeerIsAccepted)
{
    FakeCounterpart c;
    c.build({ 240.0, 30.0 });

    const IngestVerdict v = PpcpIngestPolicy{}.evaluate(c.peer);
    EXPECT_TRUE(v.accepted) << "one usable profile is enough to accept a peer";
    ASSERT_EQ(v.notes.size(), 1u);
    EXPECT_EQ(v.notes[0].profileId, "p1");
    EXPECT_FALSE(v.notes[0].accepted);
    EXPECT_EQ(v.notes[0].reason, "insufficient_rate");

    // The note is the thing a later stream_open is checked against.
    EXPECT_TRUE(v.profileAccepted("src-1", "p0"));
    EXPECT_FALSE(v.profileAccepted("src-1", "p1"));
}

// ── CORE 5.6.1 — a peer with no cameras is not judged on frame rate ────────
// "Source count is not a role marker."  A host that rejected a microphone-only
// nominator, or a peer with no Sources at all, would have turned an ingest
// policy about video into a rule about who may connect.
TEST(PpcpIngestPolicy, APeerWithNoCameraSourcesIsAcceptedWithoutComment)
{
    FakeCounterpart mic;
    mic.build({ 0.0 }, "microphone");
    const IngestVerdict v = PpcpIngestPolicy{}.evaluate(mic.peer);
    EXPECT_TRUE(v.accepted);
    EXPECT_TRUE(v.notes.empty());

    FakeCounterpart bare;
    bare.build({});
    // build({}) still makes a Source with zero profiles; strip it so the case
    // under test is genuinely "declares no Sources" (MSG 3.3d from the other
    // side of the wire).
    ppcp_peer_desc_set_sources(&bare.peer, nullptr, 0);
    EXPECT_TRUE(PpcpIngestPolicy{}.evaluate(bare.peer).accepted);
}

// ── The threshold is a knob, which is what makes it policy ────────────────
// If 120 were a protocol constant it could not move. It is a field, the host
// owns it, and a different host would ship a different one — which is exactly
// what I14 means by "acceptance is host policy, expressed outside the protocol".
TEST(PpcpIngestPolicy, TheFloorIsAFieldAndMovingItChangesTheVerdict)
{
    FakeCounterpart c;
    c.build({ 60.0 });

    EXPECT_FALSE(PpcpIngestPolicy{}.evaluate(c.peer).accepted);

    PpcpIngestPolicy::Limits lower;
    lower.minCameraRateMhz = 30000;   // 30 fps
    EXPECT_TRUE(PpcpIngestPolicy(lower).evaluate(c.peer).accepted);
}

// ── CORE 5.15 — Readiness is a measurement, and blocking has a reason ─────
TEST(PpcpHostPeerReadiness, StoragePressureRefusesToArmRatherThanDroppingSwings)
{
    PpcpHostPeer::Config cfg;
    cfg.peerId = "host-1";
    cfg.storageFloorBytes = 1024;

    PpcpHostPeer host(cfg);
    PpcpSourceDeclaration::Inventory inv;
    inv.hasMicrophone = true;
    inv.microphone.id = "mic0";
    std::string err;
    ASSERT_TRUE(host.declareSelf(inv, &err)) << err;

    host.setStorage([](std::uint64_t *free) { *free = 10; return true; });
    ppcp_readiness r{};
    ASSERT_EQ(host.readiness(&r), PPCP_OK);
    EXPECT_FALSE(r.settled);
    EXPECT_EQ(ppcp_readiness_validate(&r), PPCP_OK);
    EXPECT_STREQ(r.blocked_reason.v, "storage_full");

    // With room, and nothing else wrong, the host is ready.
    host.setStorage([](std::uint64_t *free) { *free = 100ull * 1024 * 1024 * 1024; return true; });
    ppcp_readiness ok{};
    ASSERT_EQ(host.readiness(&ok), PPCP_OK);
    EXPECT_TRUE(ok.settled);
    EXPECT_EQ(ppcp_readiness_validate(&ok), PPCP_OK);
}

TEST(PpcpHostPeerReadiness, ThermalCriticalBlocksAndNoStateMachineNameCrossesTheWire)
{
    PpcpHostPeer::Config cfg;
    cfg.peerId = "host-1";
    PpcpHostPeer host(cfg);
    PpcpSourceDeclaration::Inventory inv;
    inv.hasMicrophone = true;
    std::string err;
    ASSERT_TRUE(host.declareSelf(inv, &err)) << err;

    host.setThermal([](ppcp_thermal_level *l) { *l = PPCP_THERMAL_CRITICAL; return true; });
    ppcp_readiness r{};
    ASSERT_EQ(host.readiness(&r), PPCP_OK);
    EXPECT_STREQ(r.blocked_reason.v, "thermal_limit");

    // 5.15a — "a device state-machine name (`cold`, `warm`, `armed` or any
    // equivalent) MUST NOT cross the wire". Readiness is settled/not-settled
    // plus a reason, and there is nowhere in the struct to write a state name.
    for (const char *banned : { "cold", "warm", "armed", "ready", "idle" })
        EXPECT_STRNE(r.blocked_reason.v, banned);

    // `elevated` is not `critical`: a peer that blocked on warmth would be
    // refusing to work exactly when a range session gets going.
    host.setThermal([](ppcp_thermal_level *l) { *l = PPCP_THERMAL_ELEVATED; return true; });
    ppcp_readiness warm{};
    ASSERT_EQ(host.readiness(&warm), PPCP_OK);
    EXPECT_TRUE(warm.settled);
}

// ── The pump (ground rule 7: the embedding moves the bytes) ────────────────
TEST(PpcpHostPeerPump, BytesGoBothWaysOnTheChannelTheyBelongTo)
{
    // No socket: the pump is exercised against a link it cannot have, which is
    // the point of the engine being an interface. What is asserted here is the
    // part that is this application's — that a pump with nothing attached is
    // inert rather than undefined.
    PpcpHostPeer::Config cfg;
    cfg.peerId = "host-1";
    PpcpHostPeer host(cfg);

    RecordingEngine engine;
    EXPECT_FALSE(host.pump()) << "a pump with no link must be inert, not undefined";

    host.attach(nullptr, &engine);
    EXPECT_FALSE(host.pump());
    EXPECT_EQ(host.stats().bytesIn, 0u);
    EXPECT_EQ(host.stats().bytesOut, 0u);
}

// ── L6 is not here, and this says so out loud ─────────────────────────────
// planned.h: an application that CALLS an unimplemented symbol should fail at
// BUILD time naming the function, "which is a better diagnostic than a stub
// returning PPCP_ERR_UNIMPLEMENTED at runtime". So the adapter returns null and
// names the package rather than shipping something that behaves like an engine.
//
// ⚠ WHEN L6 LANDS, THIS TEST INVERTS. That is deliberate: a red test is how the
// deferral gets noticed rather than quietly becoming permanent.
TEST(PpcpHostPeer, TheEngineBindingIsDeferredAndSaysWhichSymbolIsMissing)
{
    PpcpHostPeer::Config cfg;
    cfg.peerId = "host-1";
    PpcpHostPeer host(cfg);

    std::string why;
    std::unique_ptr<PpcpEngine> e = host.makeLibppcpEngine(&why);
#if defined(PPCP_HAVE_PEER)
    EXPECT_NE(e, nullptr) << why;
#else
    EXPECT_EQ(e, nullptr);
    EXPECT_NE(why.find("L6"), std::string::npos);
    EXPECT_NE(why.find("ppcp_peer_new"), std::string::npos);
#endif
}

// ── CT-I14 — the static half, which is a grep ─────────────────────────────
//
// "Grep the implementation's protocol layer for a frame-rate, resolution,
// quality or confidence constant.  Assert every such threshold lives in a
// policy layer above it."
//
// The protocol layer in this repository is src/Ppcp minus the policy file: the
// transport, the declaration builder and the peer adapter. None of them may
// carry a number that decides whether a peer is good enough.
TEST(PpcpIngestPolicy, NoThresholdLivesInTheProtocolLayer)
{
#ifndef PP_PPCP_SRC_DIR
    GTEST_SKIP() << "PP_PPCP_SRC_DIR not set — the static half of CT-I14 cannot run";
#else
    static const char *kProtocolLayer[] = {
        "ppcp_transport.cpp", "ppcp_transport.h",
        "ppcp_source_declaration.cpp", "ppcp_source_declaration.h",
        "ppcp_host_peer.cpp",
    };
    // The tokens a frame-rate, resolution or confidence threshold would be
    // spelled with. Deliberately crude: a threshold that dodged all of these
    // would have had to be obfuscated on purpose.
    static const char *kThresholdSpellings[] = {
        "minCameraRateMhz", "minRateMhz", "minFps", "maxFps",
        "kMinFrameRate", "kMinConfidence", "minConfidence",
        "120000", "minResolution",
    };

    for (const char *name : kProtocolLayer) {
        const std::string path = std::string(PP_PPCP_SRC_DIR) + "/" + name;
        std::ifstream in(path);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();

        // Comments are how the rule is explained and must stay legal; a
        // threshold hiding in one would be a curiosity, not a bug. Strip
        // whole-line // comments before looking.
        std::string code;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            const std::size_t c = line.find("//");
            code += (c == std::string::npos) ? line : line.substr(0, c);
            code += '\n';
        }

        for (const char *tok : kThresholdSpellings) {
            EXPECT_EQ(code.find(tok), std::string::npos)
                << name << " carries `" << tok << "` — every frame-rate, resolution, "
                   "quality and confidence threshold belongs in ppcp_ingest_policy.h "
                   "(I14, CT-I14)";
        }
    }

    // And the policy file really does carry it, so the grep above is evidence
    // of a threshold that MOVED rather than one that was never written.
    std::ifstream pol(std::string(PP_PPCP_SRC_DIR) + "/ppcp_ingest_policy.h");
    ASSERT_TRUE(pol.good());
    std::stringstream ps;
    ps << pol.rdbuf();
    EXPECT_NE(ps.str().find("minCameraRateMhz"), std::string::npos);
#endif
}
