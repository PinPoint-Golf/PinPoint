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

// CT-S1 on the HOST path, and CT-I36a with the host as consumer.  Work
// package H4.
//
// Two peers, both real `ppcp_peer`s, both in this process, with the bytes moved
// between them one frame at a time.  Nothing here is a mock: the device peer
// declares, opens nothing and originates exactly what a capture peer
// originates, and the host peer is the one `makeHostEngine()` builds — the same
// engine the socket transport and the bundle transport get, because a test
// against a differently-configured peer would be evidence about a peer that
// does not ship.
//
// ⚠ WHY THE BYTES ARE FED ONE FRAME AT A TIME.  peer.h: an event's `msg` is
// "valid until PPCP_PEER_EVENT_QUEUE further events have been queued", the
// queue is four deep, and `payload_chunk.data` points into the buffer the
// caller fed.  A test that handed the host a whole conversation in one feed
// would be reading four-events-ago's bytes and would pass or fail on ring
// timing.  PpcpHostPeer's pump and PpcpBundleTransport both drain per frame for
// the same reason; this does what they do.

#include "VideoInputPpcp.h"
#include "ppcp_host_engine.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <ppcp/frame.h>
#include <ppcp/message.h>
#include <ppcp/peer.h>
#include <ppcp/transfer.h>
#include <ppcp/version.h>

using namespace Ppcp;

namespace {

constexpr const char *kDevPeer   = "dev-1";
constexpr const char *kSourceId  = "src-1";
constexpr const char *kSessionId = "sess-1";
constexpr const char *kDevTb     = "tb:dev";

// Computed, not hand-picked: VideoInputPpcp::start() names its Streams via
// streamIdFor() (a hash of peerId+sourceId, CORE 5.1's 64-byte PPCP_ID_MAX),
// so the id this fixture uses to look a Stream up on the host peer must be
// exactly that function's output, not a guess at its old plain-concatenation
// format.
const std::string kVideoStreamStorage =
    VideoInputPpcp::streamIdFor(QLatin1String(kDevPeer), QLatin1String(kSourceId),
                                QStringLiteral("video"))
        .toStdString();
const std::string kPreviewStreamStorage =
    VideoInputPpcp::streamIdFor(QLatin1String(kDevPeer), QLatin1String(kSourceId),
                                QStringLiteral("preview"))
        .toStdString();
const char *kVideoStream   = kVideoStreamStorage.c_str();
const char *kPreviewStream = kPreviewStreamStorage.c_str();

// A capture peer, built the way one really is: role `capture`, an AVFoundation-
// shaped camera Source (CORE §5.6.1's table) and a preview profile beside it.
struct DevicePeer {
    std::vector<std::uint8_t>         storage;
    ppcp_peer                        *p = nullptr;
    std::vector<ppcp_capture_profile> profiles;
    std::vector<ppcp_source>          sources;
    std::vector<ppcp_timebase>        timebases;
    std::vector<ppcp_id>              declared;
    ppcp_peer_desc                    desc{};

    ~DevicePeer() { if (p) ppcp_peer_free(p); }

    // `offsetNs` is CT-S1's variable: the same declaration with a different
    // `frame_start_to_exposure_offset_ns` and nothing else changed.
    void build(std::int64_t offsetNs, bool withPreview = true)
    {
        storage.assign(ppcp_peer_sizeof(), 0);

        ppcp_timebase tb{};
        ASSERT_EQ(ppcp_timebase_make(&tb, kDevTb, std::strlen(kDevTb),
                                     PPCP_TB_MONOTONIC, false, 1), PPCP_OK);
        timebases.push_back(tb);

        // The capture profile.  `nominal_frame_start` because that is what
        // every AVFoundation Source declares, which makes it the default path
        // for the whole mobile side and the one CT-S1 is written about.
        {
            ppcp_timing t{};
            ASSERT_EQ(ppcp_timing_make_nominal_frame_start(&t, offsetNs, PPCP_PROV_ASSUMED),
                      PPCP_OK);
            ppcp_geometry g{};
            ASSERT_EQ(ppcp_geometry_make_rolling_shutter(&g, 8000000, PPCP_PROV_ASSUMED,
                                                         PPCP_ROLL_TOP_TO_BOTTOM, 1080),
                      PPCP_OK);
            ppcp_capture_profile cp{};
            ASSERT_EQ(ppcp_capture_profile_make(&cp, "p-cap", &t), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_camera(&cp, &g, PPCP_INTR_PER_FRAME), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_format(&cp, "hevc", 1920, 1080, "nv12"), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_rate(&cp, 240000, 240000, 240000), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_optical(&cp, 100000, 20000000, 100, 3200), PPCP_OK);
            profiles.push_back(cp);
        }
        // 5.11m — a preview profile declares `intrinsics: none`.  That is the
        // ONLY thing marking it as preview here, and it is the only thing
        // VideoInputPpcp reads: no name, no id and no product string.
        if (withPreview) {
            ppcp_timing t{};
            ASSERT_EQ(ppcp_timing_make(&t, PPCP_CONV_MID), PPCP_OK);
            ppcp_geometry g{};
            ASSERT_EQ(ppcp_geometry_make_rolling_shutter(&g, 8000000, PPCP_PROV_ASSUMED,
                                                         PPCP_ROLL_TOP_TO_BOTTOM, 1080),
                      PPCP_OK);
            ppcp_capture_profile cp{};
            ASSERT_EQ(ppcp_capture_profile_make(&cp, "p-prev", &t), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_camera(&cp, &g, PPCP_INTR_NONE), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_format(&cp, "raw", 8, 4, "bgra"), PPCP_OK);
            ASSERT_EQ(ppcp_capture_profile_set_rate(&cp, 30000, 30000, 30000), PPCP_OK);
            profiles.push_back(cp);
        }

        ppcp_source s{};
        ASSERT_EQ(ppcp_source_make(&s, kSourceId, kDevPeer, "camera", kDevTb, true,
                                   profiles.data(), profiles.size()), PPCP_OK);
        ASSERT_EQ(ppcp_source_set_label(&s, "down-the-line"), PPCP_OK);
        sources.push_back(s);

        for (const char *n : { "core", "capture", "detect", "mint", "live", "offline" }) {
            ppcp_id id{};
            ASSERT_EQ(ppcp_id_set_z(&id, n), PPCP_OK);
            declared.push_back(id);
        }
        ASSERT_EQ(ppcp_peer_desc_make(&desc, kDevPeer, PPCP_ROLE_CAPTURE, ppcp_wire_version(),
                                      declared.data(), declared.size(),
                                      timebases.data(), timebases.size()), PPCP_OK);
        ASSERT_EQ(ppcp_peer_desc_set_sources(&desc, sources.data(), sources.size()), PPCP_OK);
        ASSERT_EQ(ppcp_peer_desc_set_product(&desc, "PinPoint", "Capture", "1.0"), PPCP_OK);

        std::vector<const char *> profileNames;
        for (const ppcp_id &id : declared) profileNames.push_back(id.v);
        ppcp_peer_config pc{};
        pc.role          = PPCP_ROLE_CAPTURE;
        pc.peer_id       = kDevPeer;
        pc.profiles      = profileNames.data();
        pc.profile_count = profileNames.size();
        pc.listener      = true;   // ENC 2.1a — no link_bind is minted here
        // ⚠ F-H5-3 IS CLOSED AND IT IS NOW A CONSTRUCTOR PRECONDITION.  This
        // repository raised it at the end of S3: `health_report` reads in
        // peer.h as a decoration on liveness and is in fact required by it, so
        // an embedding with no thermometer silently had no liveness at all.
        // libppcp now refuses ppcp_peer_new() for a peer that declares `live`
        // without one — "a peer that has no thermometer declares no Live" —
        // which is the loud version of the same rule.  This device declares
        // `live`, so it owes a reading.
        pc.health_report = [](void *, ppcp_health *out) {
            if (!out) return PPCP_ERR_INVALID;
            *out = ppcp_health{};
            out->thermal = PPCP_THERMAL_NOMINAL;
            out->storage_free_bytes = 64ull * 1024 * 1024 * 1024;
            return PPCP_OK;
        };
        ASSERT_EQ(ppcp_peer_new(storage.data(), storage.size(), &pc, &p), PPCP_OK);
    }
};

// Moves every whole frame waiting on `ch` from one engine to the other, calling
// `after` once per frame so a consumer sees the message while its bytes are
// still the ones it was handed.
void pipe(ppcp_peer *from, ppcp_peer *to, std::uint8_t ch,
          const std::function<void()> &after = {})
{
    std::vector<std::uint8_t> buf(1u << 20);
    for (;;) {
        std::size_t len = 0;
        const ppcp_result r = ppcp_peer_drain(from, ch, buf.data(), buf.size(), &len);
        if (r != PPCP_OK || len == 0) break;
        std::size_t off = 0;
        while (off < len) {
            ppcp_frame_header h{};
            const std::uint8_t *payload = nullptr;
            std::size_t consumed = 0;
            if (ppcp_frame_read(buf.data() + off, len - off, &h, &payload, &consumed) != PPCP_OK)
                break;
            std::size_t took = 0;
            ppcp_peer_feed(to, ch, buf.data() + off, consumed, &took);
            if (after) after();
            off += consumed;
        }
    }
}

// The whole fixture: a device, the real host engine, and the camera backend
// bound to it.
struct Link {
    DevicePeer                  dev;
    std::unique_ptr<PpcpEngine> host;
    VideoInputPpcp              in;
    std::vector<PpcpClip>       clips;
    int                         frames = 0;

    void build(std::int64_t offsetNs, bool withPreview = true)
    {
        dev.build(offsetNs, withPreview);
        HostEngineConfig cfg;
        cfg.peerId = "host-1";
        cfg.listener = true;
        std::string why;
        host = makeHostEngine(std::move(cfg), &why);
        ASSERT_NE(host, nullptr) << why;
        ASSERT_NE(host->peer(), nullptr);

        in.attach(host->peer(), kSessionId);
        in.prepareDevice(VideoInputPpcp::deviceIdFor(kDevPeer, kSourceId));
        QObject::connect(&in, &VideoInputPpcp::clipReady,
                         [this](const PpcpClip &c) { clips.push_back(c); });
        QObject::connect(&in, &VideoInputBase::videoFrameReady,
                         [this](const QVideoFrame &) { ++frames; });
    }

    void toHost(std::uint8_t ch = 0)
    {
        pipe(dev.p, host->peer(), ch, [this] { in.drainEvents(); });
    }
    void toDevice(std::uint8_t ch = 0) { pipe(host->peer(), dev.p, ch); }

    void declare()
    {
        ASSERT_EQ(ppcp_peer_declare(dev.p, &dev.desc), PPCP_OK);
        toHost(0);
    }

    // start() queues two `stream_open`s; the device answers, and the acks come
    // back so both ends agree the Streams exist.
    void openStreams()
    {
        ASSERT_TRUE(in.start());
        toDevice(0);
        toHost(0);
    }

    // One shot-anchored Capture on the video Stream, with its payload and its
    // per-frame timing.  `exposures` is the array form; pass one value with
    // `scalar` to send the scalar form of ENC 4.1d instead.
    void sendClip(const char *captureId,
                  const std::vector<std::int64_t> &framesNs,
                  const std::vector<std::int64_t> &exposures,
                  bool scalar = false)
    {
        ppcp_capture c{};
        ASSERT_EQ(ppcp_capture_make_shot(&c, captureId, "shot-1", kVideoStream, PPCP_COMPLETE),
                  PPCP_OK);
        ASSERT_EQ(ppcp_capture_set_transfer(&c, PPCP_TRANSFER_IN_FLIGHT), PPCP_OK);
        std::vector<std::uint8_t> bytes(512);
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 3u + 1u);
        ppcp_digest d{};
        ASSERT_EQ(ppcp_payload_digest(bytes.data(), bytes.size(), &d), PPCP_OK);
        ASSERT_EQ(ppcp_capture_set_digest(&c, &d, bytes.size()), PPCP_OK);
        ASSERT_EQ(ppcp_peer_capture_announce(dev.p, &c, false, nullptr, nullptr, 0), PPCP_OK);
        toHost(0);

        ppcp_achieved_frames af{};
        ASSERT_EQ(ppcp_achieved_frames_make(&af, kDevTb, framesNs.data(), framesNs.size()),
                  PPCP_OK);
        ppcp_per_frame_i64 e{};
        if (scalar)
            ASSERT_EQ(ppcp_per_frame_i64_scalar(&e, exposures.front()), PPCP_OK);
        else
            ASSERT_EQ(ppcp_per_frame_i64_array(&e, exposures.data(), exposures.size()), PPCP_OK);
        ASSERT_EQ(ppcp_achieved_frames_set_exposure(&af, &e,
                      scalar ? PPCP_EXP_LOCKED_CONSTANT : PPCP_EXP_PER_FRAME), PPCP_OK);

        ASSERT_EQ(ppcp_peer_payload_begin(dev.p, PPCP_CHANNEL_BULK, captureId, bytes.size(),
                                          &d, 1024, &af), PPCP_OK);
        toHost(PPCP_CHANNEL_BULK);
        ASSERT_EQ(ppcp_peer_payload_chunk(dev.p, PPCP_CHANNEL_BULK, captureId, 0, 1024,
                                          bytes.data(), bytes.size()), PPCP_OK);
        toHost(PPCP_CHANNEL_BULK);
        ASSERT_EQ(ppcp_peer_payload_end(dev.p, PPCP_CHANNEL_BULK, captureId, &d), PPCP_OK);
        toHost(PPCP_CHANNEL_BULK);
    }
};

}  // namespace

// ── The declaration IS the capability set ──────────────────────────────────

TEST(VideoInputPpcp, CapabilitiesComeFromTheDeclaredProfilesAndNothingElse)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());

    const CameraCapabilities caps = L.in.queryCapabilities();

    // 5.11l — the preview profile's 8x4 is NOT offered as a capture resolution.
    EXPECT_EQ(caps.resolution.kind, CapabilityKind::Discrete);
    ASSERT_EQ(caps.resolution.presets.size(), 1);
    EXPECT_EQ(caps.resolution.presets.first().width, 1920);
    EXPECT_EQ(caps.resolution.presets.first().height, 1080);
    // ...and neither is its rate.
    EXPECT_EQ(caps.frameRate.kind, CapabilityKind::Range);
    EXPECT_DOUBLE_EQ(caps.frameRate.range.min, 240.0);
    EXPECT_DOUBLE_EQ(caps.frameRate.range.max, 240.0);

    EXPECT_EQ(caps.pixelFormat.supported.size(), 1);
    EXPECT_EQ(caps.pixelFormat.supported.first().encoding, PixelEncoding::H265);

    // I19 — every profile's `timing`, `geometry` and `intrinsics` reach the
    // application.  A consumer that could not see the convention would be back
    // to hardcoding it, which is the whole of CT-S3.
    EXPECT_EQ(caps.extensions.value("ppcp.profile.p-cap.timing.convention").toString(),
              QStringLiteral("nominal_frame_start"));
    EXPECT_EQ(caps.extensions.value("ppcp.profile.p-cap.timing.offset_ns").toLongLong(), 120000);
    EXPECT_EQ(caps.extensions.value("ppcp.profile.p-cap.timing.offset_provenance").toString(),
              QStringLiteral("assumed"));
    EXPECT_EQ(caps.extensions.value("ppcp.profile.p-cap.geometry").toString(),
              QStringLiteral("rolling_shutter"));
    // I28 — absence of `measured` is reported as absence, never as a zero.
    EXPECT_FALSE(caps.extensions.value("ppcp.profile.p-cap.measured").toBool());
    EXPECT_TRUE(caps.extensions.value("ppcp.profile.p-prev.preview").toBool());
    EXPECT_FALSE(caps.extensions.value("ppcp.profile.p-cap.preview").toBool());

    // I1 — the Source's Timebase travels with its capabilities, so no instant
    // this camera produces is ever a bare number.
    EXPECT_EQ(caps.extensions.value("ppcp.timebase_id").toString(), QStringLiteral("tb:dev"));
    EXPECT_TRUE(caps.isVirtual);
    EXPECT_EQ(caps.connectionInterface, CameraCapabilities::Interface::Virtual);
}

TEST(VideoInputPpcp, ASourceThatIsNotACameraIsRefusedRatherThanPresented)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(0));
    // 5.6 — a peer declares every Source it owns.  Only the cameras belong
    // behind a camera factory.
    ASSERT_EQ(ppcp_id_set_z(&L.dev.sources[0].kind, "microphone"), PPCP_OK);
    ASSERT_NO_FATAL_FAILURE(L.declare());
    EXPECT_FALSE(L.in.start());
    EXPECT_EQ(L.in.state(), VideoInputBase::State::Error);
}

// ── CT-S1 on the host path ─────────────────────────────────────────────────

TEST(VideoInputPpcpCanonicalInstant, TheConversionIsAppliedWithTheProfilesOwnTiming)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());

    // nominal_frame_start:  canonical = t + offset + d/2   (CORE §6.1)
    ASSERT_NO_FATAL_FAILURE(L.sendClip("cap-1", { 1000000, 2000000, 3000000 },
                                               { 4000, 8000, 12000 }));
    ASSERT_EQ(L.clips.size(), 1u);
    const PpcpClip &c = L.clips.front();
    EXPECT_EQ(c.timebaseId, QStringLiteral("tb:dev"));
    ASSERT_EQ(c.canonicalNs.size(), 3);
    EXPECT_EQ(c.canonicalNs[0], 1000000 + 120000 + 2000);
    EXPECT_EQ(c.canonicalNs[1], 2000000 + 120000 + 4000);
    EXPECT_EQ(c.canonicalNs[2], 3000000 + 120000 + 6000);
    // The exposure the conversion used, carried out so a consumer can see it
    // was the PER-FRAME value and not the profile's range (assertion 3).
    ASSERT_EQ(c.exposureNs.size(), 3);
    EXPECT_EQ(c.exposureNs[1], 8000);
    EXPECT_EQ(L.in.counters().unconvertible, 0u);
    EXPECT_EQ(L.frames, 0);   // a clip is NOT a live frame
}

// CT-S1 assertion 2, and the plan calls it "the whole test": an implementation
// that ignores `frame_start_to_exposure_offset_ns` passes every other assertion
// in this file.
TEST(VideoInputPpcpCanonicalInstant, TheDeclaredOffsetMovesEveryInstantByExactlyItself)
{
    Link a, b;
    ASSERT_NO_FATAL_FAILURE(a.build(0));
    ASSERT_NO_FATAL_FAILURE(b.build(120000));
    ASSERT_NO_FATAL_FAILURE(a.declare());
    ASSERT_NO_FATAL_FAILURE(b.declare());
    ASSERT_NO_FATAL_FAILURE(a.openStreams());
    ASSERT_NO_FATAL_FAILURE(b.openStreams());

    const std::vector<std::int64_t> f = { 1000000, 2000000, 3000000 };
    const std::vector<std::int64_t> e = { 4000, 8000, 12000 };
    ASSERT_NO_FATAL_FAILURE(a.sendClip("cap-1", f, e));
    ASSERT_NO_FATAL_FAILURE(b.sendClip("cap-1", f, e));

    ASSERT_EQ(a.clips.size(), 1u);
    ASSERT_EQ(b.clips.size(), 1u);
    ASSERT_EQ(a.clips[0].canonicalNs.size(), b.clips[0].canonicalNs.size());
    for (int i = 0; i < a.clips[0].canonicalNs.size(); ++i)
        EXPECT_EQ(b.clips[0].canonicalNs[i] - a.clips[0].canonicalNs[i], 120000) << "frame " << i;
}

// CT-S1 assertion 3.
TEST(VideoInputPpcpCanonicalInstant, DoublingEveryExposureChangesTheConvertedInstants)
{
    Link a, b;
    ASSERT_NO_FATAL_FAILURE(a.build(120000));
    ASSERT_NO_FATAL_FAILURE(b.build(120000));
    ASSERT_NO_FATAL_FAILURE(a.declare());
    ASSERT_NO_FATAL_FAILURE(b.declare());
    ASSERT_NO_FATAL_FAILURE(a.openStreams());
    ASSERT_NO_FATAL_FAILURE(b.openStreams());

    const std::vector<std::int64_t> f = { 1000000, 2000000, 3000000 };
    ASSERT_NO_FATAL_FAILURE(a.sendClip("cap-1", f, { 4000, 8000, 12000 }));
    ASSERT_NO_FATAL_FAILURE(b.sendClip("cap-1", f, { 8000, 16000, 24000 }));

    ASSERT_EQ(a.clips.size(), 1u);
    ASSERT_EQ(b.clips.size(), 1u);
    for (int i = 0; i < a.clips[0].canonicalNs.size(); ++i)
        EXPECT_NE(a.clips[0].canonicalNs[i], b.clips[0].canonicalNs[i]) << "frame " << i;
    // ...and by exactly half the difference in exposure, which is the shape of
    // the error a bias estimator would otherwise absorb.
    EXPECT_EQ(b.clips[0].canonicalNs[0] - a.clips[0].canonicalNs[0], 2000);
}

// CT-S1 assertion 6 — the scalar form is the one the shipping product uses,
// because the application locks exposure.
TEST(VideoInputPpcpCanonicalInstant, TheScalarFormAndAConstantArrayAgreeExactly)
{
    Link a, b;
    ASSERT_NO_FATAL_FAILURE(a.build(120000));
    ASSERT_NO_FATAL_FAILURE(b.build(120000));
    ASSERT_NO_FATAL_FAILURE(a.declare());
    ASSERT_NO_FATAL_FAILURE(b.declare());
    ASSERT_NO_FATAL_FAILURE(a.openStreams());
    ASSERT_NO_FATAL_FAILURE(b.openStreams());

    const std::vector<std::int64_t> f = { 1000000, 2000000, 3000000 };
    ASSERT_NO_FATAL_FAILURE(a.sendClip("cap-1", f, { 6000, 6000, 6000 }, /*scalar=*/false));
    ASSERT_NO_FATAL_FAILURE(b.sendClip("cap-1", f, { 6000 },             /*scalar=*/true));

    ASSERT_EQ(a.clips.size(), 1u);
    ASSERT_EQ(b.clips.size(), 1u);
    EXPECT_EQ(a.clips[0].canonicalNs, b.clips[0].canonicalNs);
}

// ── The canonical instant is not yet a host timestamp ───────────────────────

TEST(VideoInputPpcpCanonicalInstant, WithNoTimebaseRelationTheHostClockAnswerIsRefused)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());
    ASSERT_NO_FATAL_FAILURE(L.sendClip("cap-1", { 1000000 }, { 4000 }));

    // The conversion happened...
    ASSERT_EQ(L.in.lastCanonicalNs().size(), 1);
    EXPECT_EQ(L.in.lastCanonicalNs().first(), 1000000 + 120000 + 2000);
    EXPECT_EQ(L.in.lastCanonicalTimebase(), QStringLiteral("tb:dev"));
    // ...and the MAPPING has not, so nothing is offered on this host's clock.
    // 0 is what CameraInstance reads as "stamp arrival yourself".
    EXPECT_FALSE(L.in.hasTimebaseMapping());
    EXPECT_EQ(L.in.lastFrameInstantUs(), 0);

    // H5's sync prober will supply the relation; when it does, the instant is
    // the canonical one moved by it and by nothing else.
    L.in.setTimebaseOffsetNs(500000000);
    EXPECT_EQ(L.in.lastFrameInstantUs(), (1122000 + 500000000) / 1000);
    L.in.clearTimebaseMapping();
    EXPECT_EQ(L.in.lastFrameInstantUs(), 0);
}

// ── CT-I36a — the host as consumer of a preview Stream ─────────────────────

TEST(VideoInputPpcpPreview, APreviewCaptureAnnouncedPendingIsRefused)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());

    // 5.11j: "a consumer therefore never sees `transfer: pending` on a preview
    // Capture".  Producing one needs a NON-CONFORMANT peer.
    //
    // ⚠ HOW THIS TEST HAD TO CHANGE, AND WHAT THE CHANGE MEANS.  Until libppcp
    // L9 the device could be made non-conformant by LYING to its own engine —
    // `ppcp_peer_capture_announce(..., is_preview=false)` on a preview Stream —
    // because the parameter was taken at face value.  L9 closed that: the
    // engine now resolves the Stream from the Capture's own `stream_id` and
    // refuses when `is_preview` disagrees with it.  So F-H4-1 IS FIXED ON THE
    // ORIGINATION SIDE, and a conformant peer can no longer be used to produce
    // the frame at all.
    //
    // The frame is therefore built and framed by hand and fed straight in,
    // which is what a genuinely non-conformant third-party peer would put on
    // the wire.  That is the only way left to ask the question this row is
    // about: does the CONSUMER catch it.
    ppcp_interval iv{};
    ASSERT_EQ(ppcp_interval_make(&iv, kDevTb, std::strlen(kDevTb), 1000000, 2000000), PPCP_OK);
    ppcp_capture c{};
    ASSERT_EQ(ppcp_capture_make_segment(&c, "cap-preview-bad", kPreviewStream,
                                        PPCP_COMPLETE, &iv), PPCP_OK);
    ASSERT_EQ(ppcp_capture_set_transfer(&c, PPCP_TRANSFER_PENDING), PPCP_OK);

    // The owner's own engine refuses to ORIGINATE it, told the truth or told a
    // lie.  8.1i on the way out — and the second of these two is the L9 change.
    EXPECT_NE(ppcp_peer_capture_announce(L.dev.p, &c, /*is_preview=*/true, nullptr, nullptr, 0),
              PPCP_OK);
    EXPECT_NE(ppcp_peer_capture_announce(L.dev.p, &c, /*is_preview=*/false, nullptr, nullptr, 0),
              PPCP_OK);

    ppcp_msg m{};
    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_CAPTURE_ANNOUNCE, 9999), PPCP_OK);
    ASSERT_EQ(ppcp_msg_set_session_id(&m, kSessionId), PPCP_OK);
    m.body.capture_announce.capture = c;
    std::vector<std::uint8_t> frame(65536);
    std::size_t wrote = 0;
    ASSERT_EQ(ppcp_msg_encode(frame.data(), frame.size(), PPCP_CHANNEL_CONTROL, &m, &wrote),
              PPCP_OK);
    std::size_t took = 0;
    ASSERT_EQ(ppcp_peer_feed(L.host->peer(), PPCP_CHANNEL_CONTROL, frame.data(), wrote, &took),
              PPCP_OK);
    L.in.drainEvents();

    // ⚠ AND THE CONSUMER HALF OF F-H4-1 IS STILL AN APPLICATION OBLIGATION.
    // `VideoInputPpcp::onCaptureAnnounce()` runs
    // `ppcp_capture_validate_in_stream()` itself and counts the refusal; the
    // engine's own transfer table did not.  If a later libppcp makes the
    // receiving side check too, this counter drops to zero and the row becomes
    // the library's — which is where CT-I36a's consumer half belongs.
    EXPECT_EQ(L.in.counters().previewPendingRefused, 1u);
    EXPECT_EQ(L.in.counters().previewCaptures, 0u);
    EXPECT_TRUE(L.clips.empty());
}

TEST(VideoInputPpcpPreview, AShedPreviewSegmentIsAnAbsentSegmentAndNotAGap)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());

    // 5.11c3 / I11 — deliberate non-retention is an `absent` segment carrying
    // its interval and its reason, never a gap.  It accounts for its span (I36)
    // and it is a normal answer, so it does not reach the live tile and it is
    // not an error.
    ppcp_interval iv{};
    ASSERT_EQ(ppcp_interval_make(&iv, kDevTb, std::strlen(kDevTb), 1000000, 2000000), PPCP_OK);
    ppcp_capture c{};
    ASSERT_EQ(ppcp_capture_make_segment(&c, "cap-preview-shed", kPreviewStream,
                                        PPCP_ABSENT, &iv), PPCP_OK);
    ASSERT_EQ(ppcp_capture_set_absent_reason(&c, PPCP_ABSENT_NOT_RETAINED), PPCP_OK);
    ASSERT_EQ(ppcp_peer_capture_announce(L.dev.p, &c, /*is_preview=*/true, nullptr, nullptr, 0),
              PPCP_OK);
    L.toHost(0);

    EXPECT_EQ(L.in.counters().absentSegments, 1u);
    EXPECT_EQ(L.in.counters().previewPendingRefused, 0u);
    EXPECT_EQ(L.in.counters().previewGapsSeen, 0u);
    EXPECT_EQ(L.frames, 0);
    EXPECT_TRUE(L.clips.empty());   // a preview absence is not a clip either
}

TEST(VideoInputPpcpPreview, APreviewSegmentReachesTheLiveTileAsAnOrdinaryFrame)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());

    ppcp_interval iv{};
    ASSERT_EQ(ppcp_interval_make(&iv, kDevTb, std::strlen(kDevTb), 1000000, 1033000), PPCP_OK);
    ppcp_capture c{};
    ASSERT_EQ(ppcp_capture_make_segment(&c, "cap-preview-1", kPreviewStream,
                                        PPCP_COMPLETE, &iv), PPCP_OK);
    ASSERT_EQ(ppcp_capture_set_transfer(&c, PPCP_TRANSFER_IN_FLIGHT), PPCP_OK);

    std::vector<std::uint8_t> bgra(8 * 4 * 4, 0x40);
    ppcp_digest d{};
    ASSERT_EQ(ppcp_payload_digest(bgra.data(), bgra.size(), &d), PPCP_OK);
    ASSERT_EQ(ppcp_capture_set_digest(&c, &d, bgra.size()), PPCP_OK);
    ASSERT_EQ(ppcp_peer_capture_announce(L.dev.p, &c, /*is_preview=*/true, nullptr, nullptr, 0),
              PPCP_OK);
    L.toHost(0);
    EXPECT_EQ(L.in.counters().previewCaptures, 1u);

    // 5.11h — preview payload wants a bulk channel of its own so it never
    // queues behind a clip.  Channel 2 is plan A6's optional third connection
    // and PeerConnection already carries it.
    const std::int64_t frames[] = { 1000000 };
    ppcp_achieved_frames af{};
    ASSERT_EQ(ppcp_achieved_frames_make(&af, kDevTb, frames, 1), PPCP_OK);
    // 5.8j — a preview Capture is exempt from 5.8d's exposure requirement, and
    // still carries `frames`, because every sample is placed in time (I1, I2).
    ASSERT_EQ(ppcp_peer_payload_begin(L.dev.p, PPCP_CHANNEL_PREVIEW, "cap-preview-1",
                                      bgra.size(), &d, 4096, &af), PPCP_OK);
    L.toHost(PPCP_CHANNEL_PREVIEW);
    ASSERT_EQ(ppcp_peer_payload_chunk(L.dev.p, PPCP_CHANNEL_PREVIEW, "cap-preview-1", 0, 4096,
                                      bgra.data(), bgra.size()), PPCP_OK);
    L.toHost(PPCP_CHANNEL_PREVIEW);
    ASSERT_EQ(ppcp_peer_payload_end(L.dev.p, PPCP_CHANNEL_PREVIEW, "cap-preview-1", &d), PPCP_OK);
    L.toHost(PPCP_CHANNEL_PREVIEW);

    EXPECT_EQ(L.frames, 1);
    EXPECT_EQ(L.in.counters().previewFrames, 1u);
    EXPECT_EQ(L.in.counters().decodeFailures, 0u);
    EXPECT_TRUE(L.clips.empty());   // a preview frame is not a clip
}

// ── Streams ────────────────────────────────────────────────────────────────

TEST(VideoInputPpcpStreams, StartOpensACaptureStreamAndAPreviewStreamWhereOneIsOffered)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());

    EXPECT_TRUE(L.in.isActive());
    ASSERT_NE(ppcp_peer_stream_find(L.host->peer(), kVideoStream), nullptr);
    ASSERT_NE(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr);
    // 5.11 — preview is ALWAYS continuous; a shot-windowed capture Stream is
    // what a host that arbitrates asks for.
    EXPECT_EQ(ppcp_peer_stream_find(L.host->peer(), kPreviewStream)->continuity, PPCP_CONTINUOUS);
    EXPECT_EQ(ppcp_peer_stream_find(L.host->peer(), kVideoStream)->continuity, PPCP_SHOT_WINDOWED);
    // 5.11l — the Stream names the profile, and a preview profile is only ever
    // named by a preview Stream.
    EXPECT_STREQ(ppcp_peer_stream_find(L.host->peer(), kVideoStream)->profile_id.v, "p-cap");
    EXPECT_STREQ(ppcp_peer_stream_find(L.host->peer(), kPreviewStream)->profile_id.v, "p-prev");

    // 5.11i / 5.11a1 — suspending the live tile closes the PREVIEW Stream and
    // leaves the capture Stream open: a consumer that stops looking must not
    // stop a camera that is waiting for a shot.
    L.in.suspend();
    L.toDevice(0);
    EXPECT_EQ(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr);
    EXPECT_NE(ppcp_peer_stream_find(L.host->peer(), kVideoStream), nullptr);
}

TEST(VideoInputPpcpStreams, APeerOfferingNoPreviewProfileOpensOnlyTheCaptureStream)
{
    // 5.11.2: "a peer that does not offer a suitable profile simply refuses,
    // and nothing else changes" — which is the conformant way to decline
    // preview entirely until the cost of a second concurrent encode is known
    // (5.8k).
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000, /*withPreview=*/false));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());

    EXPECT_TRUE(L.in.isActive());
    EXPECT_NE(ppcp_peer_stream_find(L.host->peer(), kVideoStream), nullptr);
    EXPECT_EQ(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr);
}

// ⛔ **THE CONSUMER THAT EXISTS BEFORE ANYBODY OPENS A PANEL.**
//
// We ask a phone for preview at `declare` — 5.11.2 calls setup and framing
// preview's main use — but until 27 Aug 2026 nothing on this side received the
// answer: dispatchEvent() broadcasts to LIVE instances and the only code that
// ever constructed one was the Settings crop editor.  A phone therefore
// announced segments at ~10 fps into a host with nowhere to put them, and its
// `stream_close` was dropped too, so we could not even see that preview had
// stopped.  Diagnosed on hardware after an operator saw no preview at all.
TEST(VideoInputPpcpStreams, APreviewOnlyConsumerOpensPreviewAndNoCaptureStream)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());

    ASSERT_TRUE(L.in.startPreviewOnly());
    L.toDevice(0);
    L.toHost(0);

    EXPECT_TRUE(L.in.isPreviewOnly());
    EXPECT_TRUE(L.in.isActive());
    ASSERT_NE(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr);
    EXPECT_EQ(ppcp_peer_stream_find(L.host->peer(), kPreviewStream)->continuity,
              PPCP_CONTINUOUS);
    // ⛔ THE POINT: a pair of eyes is not a recorder.  start() would open a
    // `shot_windowed` capture Stream, which for every camera on every phone the
    // moment it connects is a claim we have no business making.
    EXPECT_EQ(ppcp_peer_stream_find(L.host->peer(), kVideoStream), nullptr);
}

// ⚠ And a tile opened later does NOT evict it.  reclaimStream() stops a stale
// sibling because two instances collide on the deterministic *capture* Stream
// id — a preview-only consumer owns no capture Stream, so it collides with
// nothing.  Left in, this would have closed the host's preview the instant an
// operator opened Settings, with nothing to reopen it when they closed it.
TEST(VideoInputPpcpStreams, ATileStartingDoesNotReclaimThePreviewOnlyConsumer)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_TRUE(L.in.startPreviewOnly());
    L.toDevice(0);
    L.toHost(0);
    ASSERT_TRUE(L.in.isActive());

    // The Settings crop editor, on the SAME peer and the SAME Source.
    VideoInputPpcp tile;
    tile.attach(L.host->peer(), kSessionId);
    tile.prepareDevice(VideoInputPpcp::deviceIdFor(kDevPeer, kSourceId));
    ASSERT_TRUE(tile.start());
    L.toDevice(0);
    L.toHost(0);

    // Both alive, and both fed: onCaptureAnnounce() resolves a Capture by
    // `source_id`, not by which Streams an instance opened.
    EXPECT_TRUE(L.in.isActive());
    EXPECT_TRUE(tile.isActive());
    EXPECT_FALSE(tile.isPreviewOnly());
    // The tile adopted the preview Stream rather than opening a duplicate
    // (5.1a — a Stream's identity is fixed for its life) and added the capture
    // Stream that is genuinely its own.
    EXPECT_NE(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr);
    EXPECT_NE(ppcp_peer_stream_find(L.host->peer(), kVideoStream), nullptr);
}

// ⚠ AND A TILE CLOSING DOES NOT TAKE THE PREVIEW WITH IT.  The open side of
// "one preview Stream per Source, several consumers" was right; the close side
// was not, and `stop()` shut the shared Stream whenever ANY consumer finished
// with it.  What an operator saw: open the crop editor on a phone's camera,
// close it (or have its delegate rebuilt underneath them) and the device dutifully
// stopped previewing — reported as "the device closed the preview stream
// (not_needed)" against the host's own consumer, which had asked for nothing.
// Reported 31 Aug 2026 on a cabled phone while setting a crop.
TEST(VideoInputPpcpStreams, ATileClosingLeavesTheStreamItAdoptedOpen)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_TRUE(L.in.startPreviewOnly());        // the host's own consumer: the owner
    L.toDevice(0);
    L.toHost(0);

    VideoInputPpcp tile;                          // the crop editor
    tile.attach(L.host->peer(), kSessionId);
    tile.prepareDevice(VideoInputPpcp::deviceIdFor(kDevPeer, kSourceId));
    ASSERT_TRUE(tile.start());
    L.toDevice(0);
    L.toHost(0);
    ASSERT_NE(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr);

    // The editor closes.  Its own capture Stream goes; the preview Stream it
    // merely adopted stays, and the consumer that opened it is untouched.
    tile.stop();
    L.toDevice(0);
    L.toHost(0);

    EXPECT_NE(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr)
        << "the tile closed a preview Stream it did not open";
    EXPECT_TRUE(L.in.isActive()) << "the owner lost its preview when a tile closed";
}

// The other half, so the rule is "the opener closes it" and not "nobody does".
TEST(VideoInputPpcpStreams, TheConsumerThatOpenedThePreviewStillClosesIt)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_TRUE(L.in.startPreviewOnly());
    L.toDevice(0);
    L.toHost(0);
    ASSERT_NE(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr);

    L.in.stop();
    L.toDevice(0);
    L.toHost(0);
    EXPECT_EQ(ppcp_peer_stream_find(L.host->peer(), kPreviewStream), nullptr)
        << "the Stream's owner walked away and left it open";
}

// ── The device id ──────────────────────────────────────────────────────────

TEST(VideoInputPpcp, ADeviceIdCarriesBothHalvesOfAPpcpIdentity)
{
    // CORE 8.5c scopes ids by the MINTING PEER, so two devices may both call a
    // Source "cam-0" and a registry keyed on the Source alone would collide.
    const QString id = VideoInputPpcp::deviceIdFor("dev-1", "cam-0");
    EXPECT_EQ(id, QStringLiteral("ppcp:dev-1/cam-0"));
    QString peer, source;
    EXPECT_TRUE(VideoInputPpcp::parseDeviceId(id, &peer, &source));
    EXPECT_EQ(peer, QStringLiteral("dev-1"));
    EXPECT_EQ(source, QStringLiteral("cam-0"));
    EXPECT_FALSE(VideoInputPpcp::parseDeviceId("0000-cam", &peer, &source));
    EXPECT_FALSE(VideoInputPpcp::parseDeviceId("ppcp:dev-1", &peer, &source));
    EXPECT_FALSE(VideoInputPpcp::parseDeviceId("ppcp:/cam-0", &peer, &source));
}

// CT-S1 assertion 5 — the rolling-shutter row instant, under both directions
// and including R == 1.  A consumer of a rolling-shutter Source has to ask this
// question: the canonical instant is the FIRST ROW's (6.2c), and a clubhead
// crossing the bottom of the sensor was seen `readout_ns` later than one
// crossing the top.  The declaration says which way the sensor reads and how
// long it takes; nothing here assumes either.
TEST(VideoInputPpcpCanonicalInstant, RowInstantsFollowTheDeclaredReadoutAndDirection)
{
    Link L;
    ASSERT_NO_FATAL_FAILURE(L.build(120000));
    ASSERT_NO_FATAL_FAILURE(L.declare());
    ASSERT_NO_FATAL_FAILURE(L.openStreams());
    ASSERT_NO_FATAL_FAILURE(L.sendClip("cap-1", { 1000000 }, { 4000 }));

    // The fixture declares top_to_bottom, readout 8 ms over R = 1080.
    //   canonical_first + readout_ns x r / (R - 1)
    const qint64 first = 1000000 + 120000 + 2000;
    qint64 r0 = 0, rLast = 0, rMid = 0;
    ASSERT_TRUE(L.in.rowInstantNs(0, 0, &r0));
    ASSERT_TRUE(L.in.rowInstantNs(0, 1079, &rLast));
    ASSERT_TRUE(L.in.rowInstantNs(0, 539, &rMid));
    EXPECT_EQ(r0, first);
    EXPECT_EQ(rLast, first + 8000000);
    EXPECT_GT(rMid, r0);
    EXPECT_LT(rMid, rLast);
    // Out of range asks are refused rather than extrapolated.
    qint64 ignored = 0;
    EXPECT_FALSE(L.in.rowInstantNs(1, 0, &ignored));
    EXPECT_FALSE(L.in.rowInstantNs(-1, 0, &ignored));
}

TEST(VideoInputPpcpCanonicalInstant, BottomToTopReversesTheRowOrderAndROneIsFlat)
{
    // Same Capture, two declarations differing only in `direction`: the first
    // and last rows swap, which is the assertion that catches a consumer that
    // ignored the field.
    {
        Link L;
        ASSERT_NO_FATAL_FAILURE(L.build(120000));
        ASSERT_EQ(ppcp_geometry_make_rolling_shutter(&L.dev.profiles[0].geometry, 8000000,
                                                     PPCP_PROV_ASSUMED,
                                                     PPCP_ROLL_BOTTOM_TO_TOP, 1080), PPCP_OK);
        ASSERT_NO_FATAL_FAILURE(L.declare());
        ASSERT_NO_FATAL_FAILURE(L.openStreams());
        ASSERT_NO_FATAL_FAILURE(L.sendClip("cap-1", { 1000000 }, { 4000 }));
        const qint64 first = 1000000 + 120000 + 2000;
        qint64 r0 = 0, rLast = 0;
        ASSERT_TRUE(L.in.rowInstantNs(0, 0, &r0));
        ASSERT_TRUE(L.in.rowInstantNs(0, 1079, &rLast));
        EXPECT_EQ(r0, first + 8000000);
        EXPECT_EQ(rLast, first);
    }
    // R == 1: every row is the frame's instant, and the division by R - 1 does
    // not happen.
    {
        Link L;
        ASSERT_NO_FATAL_FAILURE(L.build(120000));
        ASSERT_EQ(ppcp_geometry_make_rolling_shutter(&L.dev.profiles[0].geometry, 8000000,
                                                     PPCP_PROV_ASSUMED,
                                                     PPCP_ROLL_TOP_TO_BOTTOM, 1), PPCP_OK);
        ASSERT_NO_FATAL_FAILURE(L.declare());
        ASSERT_NO_FATAL_FAILURE(L.openStreams());
        ASSERT_NO_FATAL_FAILURE(L.sendClip("cap-1", { 1000000 }, { 4000 }));
        qint64 r0 = 0;
        ASSERT_TRUE(L.in.rowInstantNs(0, 0, &r0));
        EXPECT_EQ(r0, 1000000 + 120000 + 2000);
    }
}
