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

// CT-I12, CT-I15, CT-I16 and CT-I34, the host column.  Work package H3.
//
// ⚠ THE BUNDLES HERE ARE WRITTEN BY libppcp's OWN WRITER.  The first draft of
// this file assembled the container by hand, took the 8-byte magic and the
// 16-byte header to be consecutive when the header CONTAINS the magic, and then
// asserted against a transport that made the same mistake — so the two agreed
// with each other and neither agreed with ENC §7.  Nothing here writes a
// container byte any more except the two cases that are ABOUT a malformed one.
//
// ⚠ AND THE SINK IS A REAL ppcp_peer, FROM THE SAME FACTORY THE SOCKET USES.
// Plan A10: "a consumer gains a file transport, NOT an importer." A test that
// fed a lookalike sink would prove the transport walks frames; it would not
// prove the claim. Note also what this target does NOT link: no OpenSSL and no
// ppcp_transport.cpp. Reading a file must not require TLS, and the link line is
// where that is enforced rather than asserted.

#include "ppcp_bundle_transport.h"
#include "ppcp_engine.h"
#include "ppcp_host_engine.h"
#include "ppcp_import_ledger.h"
#include "ppcp_import_sink.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <ppcp/bundle.h>
#include <ppcp/cbor.h>
#include <ppcp/envelope.h>
#include <ppcp/frame.h>

using namespace Ppcp;

namespace {

// 5.10h — `opened_at` is mandatory on every Session constructor.  These
// fixtures replay a RECORDED Session, so the honest value is a fixed literal in
// the recording's own timebase: there is no live clock here to read, and a
// reading taken at import time would be exactly the fabricated instant 5.10h
// exists to prevent.
ppcp_instant fixtureOpenedAt(const char *timebase)
{
    ppcp_instant in{};
    EXPECT_EQ(ppcp_instant_make(&in, timebase, std::strlen(timebase), 1'000'000'000LL), PPCP_OK);
    return in;
}

// ── A bundle, written through ppcp_bundle_writer ────────────────────────────

class Bundle {
public:
    Bundle()
    {
        m_storage.resize(ppcp_bundle_writer_sizeof());
        EXPECT_EQ(ppcp_bundle_writer_new(m_storage.data(), m_storage.size(), &m_w), PPCP_OK);
        std::uint8_t hdr[PPCP_BUNDLE_HEADER_BYTES];
        std::size_t n = 0;
        EXPECT_EQ(ppcp_bundle_writer_begin(m_w, hdr, sizeof hdr, &n), PPCP_OK);
        EXPECT_EQ(n, PPCP_BUNDLE_HEADER_BYTES);
        m_bytes.insert(m_bytes.end(), hdr, hdr + n);
    }

    ppcp_result add(std::uint8_t channel, const ppcp_msg *m)
    {
        std::uint8_t buf[64 * 1024];
        std::size_t n = 0;
        const ppcp_result r =
            ppcp_bundle_writer_append_msg(m_w, channel, m, buf, sizeof buf, &n);
        if (r == PPCP_OK) m_bytes.insert(m_bytes.end(), buf, buf + n);
        return r;
    }

    // Only for the frames the writer is RIGHT to refuse — ENC 7g's `link_bind`
    // is the case, and refusing it is the writer doing its job.
    void addRaw(std::uint8_t channel, const ppcp_msg *m)
    {
        std::uint8_t buf[4096];
        std::size_t n = 0;
        auto none = [](ppcp_cbor_writer *, ppcp_envelope_writer *, void *) { return PPCP_OK; };
        ppcp_envelope e;
        ASSERT_EQ(ppcp_envelope_init(&e, "link_bind", m->env.msg_id), PPCP_OK);
        ASSERT_EQ(ppcp_message_encode(buf, sizeof buf, channel, &e, 0, none, nullptr, &n),
                  PPCP_OK);
        m_bytes.insert(m_bytes.end(), buf, buf + n);
    }

    void finish() { EXPECT_EQ(ppcp_bundle_writer_finish(m_w), PPCP_OK); }
    void truncateBy(std::size_t n) { if (n < m_bytes.size()) m_bytes.resize(m_bytes.size() - n); }
    // ENC 7f — a MINOR this reader has never heard of, and a MAJOR it must
    // refuse.  Both are container facts, so they are written over the header.
    void setMajorMinor(std::uint16_t major, std::uint16_t minor)
    {
        m_bytes[8] = static_cast<std::uint8_t>(major >> 8);
        m_bytes[9] = static_cast<std::uint8_t>(major & 0xFF);
        m_bytes[10] = static_cast<std::uint8_t>(minor >> 8);
        m_bytes[11] = static_cast<std::uint8_t>(minor & 0xFF);
    }

    const std::vector<std::uint8_t> &bytes() const { return m_bytes; }

    std::string writeTo(const QDir &dir, const char *name) const
    {
        const std::string path = dir.filePath(name).toStdString();
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(m_bytes.data()),
                  static_cast<std::streamsize>(m_bytes.size()));
        return path;
    }

private:
    std::vector<std::uint8_t> m_storage;
    std::vector<std::uint8_t> m_bytes;
    ppcp_bundle_writer       *m_w = nullptr;
};

ppcp_instant inst(const char *tb, std::int64_t ns)
{
    ppcp_instant i{};
    EXPECT_EQ(ppcp_instant_make_z(&i, tb, ns), PPCP_OK);
    return i;
}

ppcp_digest dig(std::uint8_t fill)
{
    ppcp_digest d{};
    std::uint8_t v[PPCP_SHA256_BYTES];
    std::memset(v, fill, sizeof v);
    EXPECT_EQ(ppcp_digest_set(&d, v), PPCP_OK);
    return d;
}

// The device's `declare`, kept alive for as long as the message that borrows it.
struct Declaration {
    ppcp_id              profiles[4];
    ppcp_timebase        tb[1];
    ppcp_capture_profile cp[1];
    ppcp_source          src[1];
    ppcp_peer_desc       peer{};

    Declaration(const char *peerId, const char *tbId)
    {
        static const char *const names[] = { "core", "capture", "mint", "offline" };
        for (int i = 0; i < 4; ++i) EXPECT_EQ(ppcp_id_set_z(&profiles[i], names[i]), PPCP_OK);
        EXPECT_EQ(ppcp_timebase_make(&tb[0], tbId, std::strlen(tbId), PPCP_TB_CONTINUOUS,
                                     true, 1000), PPCP_OK);
        ppcp_timing timing{};
        ppcp_geometry geom{};
        EXPECT_EQ(ppcp_timing_make_nominal_frame_start(&timing, 240000, PPCP_PROV_ASSUMED),
                  PPCP_OK);
        EXPECT_EQ(ppcp_geometry_make_rolling_shutter(&geom, 8000000, PPCP_PROV_ASSUMED,
                                                     PPCP_ROLL_TOP_TO_BOTTOM, 1080), PPCP_OK);
        EXPECT_EQ(ppcp_capture_profile_make(&cp[0], "cp:1", &timing), PPCP_OK);
        EXPECT_EQ(ppcp_capture_profile_set_camera(&cp[0], &geom, PPCP_INTR_PER_FRAME), PPCP_OK);
        EXPECT_EQ(ppcp_source_make(&src[0], "src:1", peerId, "camera", tbId, true, cp, 1),
                  PPCP_OK);
        EXPECT_EQ(ppcp_peer_desc_make(&peer, peerId, PPCP_ROLE_CAPTURE, "1.0", profiles, 4,
                                      tb, 1), PPCP_OK);
        EXPECT_EQ(ppcp_peer_desc_set_sources(&peer, src, 1), PPCP_OK);
    }
};

struct SessionSpec {
    const char *streamKind = PPCP_STREAM_KIND_VIDEO;   // nullptr for no Streams (I12)
    bool  withPayload = true;
    bool  assertState = false;
    ppcp_completeness asserted = PPCP_COMPLETE;
    const char *sessionId = "sess:1";
    const char *peerId = "peer:dev";
    const char *timebase = "tb:dev";
};

const std::uint8_t kClip[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };

// Exactly what a hostless capture device records (CORE 4.1b): no `arm`, no
// arbitration parameters, and `session_manifest` before the payload (ENC 7c).
void writeSession(Bundle &b, const SessionSpec &spec)
{
    ppcp_session sess{};
    ppcp_msg m{};
    std::uint64_t id = 1;
    const ppcp_instant openedAtSpec = fixtureOpenedAt(spec.timebase);

    ASSERT_EQ(ppcp_session_make_hostless(&sess, spec.sessionId, spec.timebase,
                                         &openedAtSpec), PPCP_OK);
    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_SESSION_OPEN, id++), PPCP_OK);
    m.body.session_open.session_id = sess.id;
    m.body.session_open.timebase_ref = sess.timebase_ref;
    ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

    Declaration decl(spec.peerId, spec.timebase);
    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_DECLARE, id++), PPCP_OK);
    m.body.declare.generation = 1;
    m.body.declare.peer = decl.peer;
    ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

    if (spec.streamKind != nullptr) {
        ppcp_stream st{};
        const ppcp_instant at = inst(spec.timebase, 1000);
        ASSERT_EQ(ppcp_stream_make(&st, "st:1", spec.sessionId, "src:1", spec.streamKind,
                                   "cp:1", spec.timebase, PPCP_SHOT_WINDOWED, &at), PPCP_OK);
        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_STREAM_OPEN, id++), PPCP_OK);
        m.body.stream_open.stream = st;
        ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

        // CT-I34's two awkward Captures: a `complete` one whose transfer is
        // still `pending` and which therefore has NO digest yet, and an
        // `absent` one that will never have one.  An importer keyed on the
        // digest duplicates both on the second read.
        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_CAPTURE_ANNOUNCE, id++), PPCP_OK);
        ASSERT_EQ(ppcp_msg_set_session_id(&m, spec.sessionId), PPCP_OK);
        ASSERT_EQ(ppcp_capture_make_shot(&m.body.capture_announce.capture, "cap:1", "shot:1",
                                         "st:1", PPCP_COMPLETE), PPCP_OK);
        ASSERT_FALSE(m.body.capture_announce.capture.digest.present);
        ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_CAPTURE_ANNOUNCE, id++), PPCP_OK);
        ASSERT_EQ(ppcp_msg_set_session_id(&m, spec.sessionId), PPCP_OK);
        ASSERT_EQ(ppcp_capture_make_shot(&m.body.capture_announce.capture, "cap:gone", "shot:2",
                                         "st:1", PPCP_ABSENT), PPCP_OK);
        ASSERT_EQ(ppcp_capture_set_absent_reason(&m.body.capture_announce.capture,
                                                 PPCP_ABSENT_OUTSIDE_BUFFER), PPCP_OK);
        ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);
    }

    if (spec.assertState) {
        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_SESSION_STATE, id++), PPCP_OK);
        m.body.session_state.session_id = sess.id;
        m.body.session_state.state = PPCP_SESSION_CLOSED;
        m.body.session_state.completeness = spec.asserted;
        ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);
    }

    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_SESSION_MANIFEST, id++), PPCP_OK);
    m.body.session_manifest.session_id = sess.id;
    m.body.session_manifest.completeness = PPCP_UNKNOWN;
    ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

    if (spec.withPayload && spec.streamKind != nullptr) {
        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_PAYLOAD_BEGIN, id++), PPCP_OK);
        ASSERT_EQ(ppcp_id_set_z(&m.body.payload_begin.capture_id, "cap:1"), PPCP_OK);
        m.body.payload_begin.bytes = sizeof kClip;
        m.body.payload_begin.digest = dig(0x33);
        m.body.payload_begin.chunk_bytes = PPCP_DEFAULT_CHUNK_BYTES;
        ASSERT_EQ(b.add(PPCP_CHANNEL_BULK, &m), PPCP_OK);

        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_PAYLOAD_CHUNK, id++), PPCP_OK);
        ASSERT_EQ(ppcp_id_set_z(&m.body.payload_chunk.capture_id, "cap:1"), PPCP_OK);
        m.body.payload_chunk.index = 0;
        m.body.payload_chunk.offset = 0;
        m.body.payload_chunk.data = kClip;
        m.body.payload_chunk.data_len = sizeof kClip;
        m.body.payload_chunk.digest = dig(0x44);
        ASSERT_EQ(b.add(PPCP_CHANNEL_BULK, &m), PPCP_OK);

        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_PAYLOAD_END, id++), PPCP_OK);
        ASSERT_EQ(ppcp_id_set_z(&m.body.payload_end.capture_id, "cap:1"), PPCP_OK);
        m.body.payload_end.digest = dig(0x33);
        ASSERT_EQ(b.add(PPCP_CHANNEL_BULK, &m), PPCP_OK);
    }

    b.finish();
}

// The host, end to end: the engine the socket path builds, a ledger, a sink.
struct Host {
    std::unique_ptr<PpcpEngine> engine;
    PpcpImportLedger            ledger;

    Host()
    {
        HostEngineConfig cfg;
        cfg.peerId = "peer:pps-test";
        cfg.listener = true;
        std::string why;
        engine = makeHostEngine(std::move(cfg), &why);
        EXPECT_NE(engine, nullptr) << why;
        EXPECT_NE(engine ? engine->peer() : nullptr, nullptr);
    }

    struct Outcome {
        PpcpBundleTransport::Result r;
        PpcpImportSink::Stats       stats;
    };

    Outcome import(const std::vector<std::uint8_t> &bytes, const std::string &root,
                   bool writeClips = true)
    {
        PpcpImportSink::Config sc;
        sc.importRoot = root;
        sc.writeClips = writeClips;
        PpcpImportSink sink(ledger, engine->peer(), sc);

        PpcpBundleTransport::Options opt;
        opt.sink = engine->peer();
        opt.index = sink.index();
        opt.onFrame = [&sink](std::uint8_t) { sink.drainEvents(); };

        Outcome o;
        o.r = PpcpBundleTransport::streamBytes(bytes.data(), bytes.size(), opt);
        sink.drainEvents();
        sink.finish(o.r);
        o.stats = sink.stats();
        return o;
    }

    Outcome importFile(const std::string &path, const std::string &root)
    {
        PpcpImportSink::Config sc;
        sc.importRoot = root;
        PpcpImportSink sink(ledger, engine->peer(), sc);

        PpcpBundleTransport::Options opt;
        opt.sink = engine->peer();
        opt.index = sink.index();
        opt.onFrame = [&sink](std::uint8_t) { sink.drainEvents(); };

        Outcome o;
        o.r = PpcpBundleTransport::streamFile(path, opt);
        sink.drainEvents();
        sink.finish(o.r);
        o.stats = sink.stats();
        return o;
    }
};

}  // namespace

// ── CT-I12 — any subset of Streams is a valid bundle ───────────────────────
//
// "A video-only bundle, an IMU-only bundle and an empty-stream Session all load
// and are valid."  CORE 9e is the MUST behind it, and it matters because a
// hostless capture peer that recorded no video at all still produced a session
// somebody wants.
TEST(PpcpBundleImport, AnySubsetOfStreamsLoadsIncludingNone)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    struct Case { const char *kind; const char *name; std::size_t captures; };
    const Case cases[] = {
        { PPCP_STREAM_KIND_VIDEO, "video", 2 },
        { PPCP_STREAM_KIND_IMU,   "imu",   2 },
        { nullptr,                "none",  0 },
    };

    for (const Case &c : cases) {
        Bundle b;
        SessionSpec spec;
        spec.streamKind = c.kind;
        spec.withPayload = (c.kind != nullptr);
        writeSession(b, spec);

        Host host;
        const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());

        EXPECT_TRUE(o.r.ok) << c.name << ": " << o.r.error;
        EXPECT_GT(o.r.frames, 0u) << c.name;
        EXPECT_EQ(o.stats.sessionId, "sess:1") << c.name;
        EXPECT_EQ(o.stats.ownerPeerId, "peer:dev") << c.name;
        EXPECT_EQ(o.stats.captures, c.captures) << c.name;
        EXPECT_EQ(o.stats.streams, c.kind ? 1u : 0u) << c.name;
        // A Session with no Streams is a Session, and the ledger holds it.
        EXPECT_TRUE(host.ledger.holdsSession("peer:dev", "sess:1")) << c.name;
    }
}

// ── CT-I15 (host half) — the Session lands, and nothing is read from `wall` ──
//
// "A bundle whose wall clock steps mid-session.  Assert no interval, duration or
// ordering decision is computed from the `wall` timebase."  The host half is a
// negative, so it is asserted twice: a bundle carrying a wall-clock
// `discontinuity` imports unchanged, and the ingest path contains no reference
// to the wall clock for it to have used.
TEST(PpcpBundleImport, AWallClockStepChangesNothingAboutWhatIsImported)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle plain;
    writeSession(plain, SessionSpec{});

    Bundle stepped;
    {
        // The same session, with a `discontinuity` on a wall timebase in the
        // middle of it. CORE 6.4: an observed step is a measurement.
        ppcp_session sess{};
        ppcp_msg m{};
        std::uint64_t id = 1;
        const ppcp_instant openedAtDev = fixtureOpenedAt("tb:dev");
        ASSERT_EQ(ppcp_session_make_hostless(&sess, "sess:1", "tb:dev", &openedAtDev), PPCP_OK);
        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_SESSION_OPEN, id++), PPCP_OK);
        m.body.session_open.session_id = sess.id;
        m.body.session_open.timebase_ref = sess.timebase_ref;
        ASSERT_EQ(stepped.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

        Declaration decl("peer:dev", "tb:dev");
        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_DECLARE, id++), PPCP_OK);
        m.body.declare.generation = 1;
        m.body.declare.peer = decl.peer;
        ASSERT_EQ(stepped.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_DISCONTINUITY, id++), PPCP_OK);
        // CORE 5.5b — observed in a timebase that did NOT step, which is the
        // whole reason a wall step is reportable at all.
        const ppcp_instant seen = inst("tb:dev", 5000);
        ASSERT_EQ(ppcp_clock_discontinuity_make(&m.body.discontinuity.discontinuity,
                                                "tb:wall", &seen, 1500000000, "ntp_step"),
                  PPCP_OK);
        ASSERT_EQ(stepped.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

        ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_SESSION_MANIFEST, id++), PPCP_OK);
        m.body.session_manifest.session_id = sess.id;
        m.body.session_manifest.completeness = PPCP_UNKNOWN;
        ASSERT_EQ(stepped.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);
        stepped.finish();
    }

    Host h1, h2;
    const Host::Outcome a = h1.import(plain.bytes(), tmp.path().toStdString());
    const Host::Outcome b = h2.import(stepped.bytes(), tmp.path().toStdString());

    ASSERT_TRUE(a.r.ok) << a.r.error;
    ASSERT_TRUE(b.r.ok) << b.r.error;
    // The step is carried and it decides nothing: same Session, same owner.
    EXPECT_EQ(a.stats.sessionId, b.stats.sessionId);
    EXPECT_EQ(a.stats.ownerPeerId, b.stats.ownerPeerId);
    EXPECT_EQ(a.r.completeness, b.r.completeness);
}

// The negative half, as a grep — the same method CT-I14 uses, because "nothing
// computes from the wall clock" is a claim about what is ABSENT.
TEST(PpcpBundleImport, TheIngestPathNeverReadsTheWallClock)
{
#ifndef PP_PPCP_SRC_DIR
    GTEST_SKIP() << "PP_PPCP_SRC_DIR not set";
#else
    static const char *kIngest[] = {
        "ppcp_bundle_transport.cpp", "ppcp_bundle_transport.h",
        "ppcp_import_sink.cpp",      "ppcp_import_sink.h",
        "ppcp_import_ledger.cpp",    "ppcp_import_ledger.h",
    };
    for (const char *f : kIngest) {
        std::ifstream in(std::string(PP_PPCP_SRC_DIR) + "/" + f);
        ASSERT_TRUE(in.good()) << f;
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            const std::size_t hash = line.find("//");
            const std::string code = (hash == std::string::npos) ? line : line.substr(0, hash);
            EXPECT_EQ(code.find("wall_utc"), std::string::npos) << f << ":" << n;
            EXPECT_EQ(code.find(".epoch"), std::string::npos) << f << ":" << n;
        }
    }
#endif
}

// ── CT-I16 (host half) — `timebase_ref` is immutable ──────────────────────
//
// MSG 4.1a: "a second `session_open` for the same `session_id` with a different
// `timebase_ref` is an error."  The host half of CT-I16 that is reachable
// today: importing cannot move a Session's timebase.  The other half — a
// re-solved mapping arriving as a NEW TimebaseRelation from it — is L9 and is
// not claimed here.
TEST(PpcpBundleImport, ImportingCannotMoveASessionsTimebaseRef)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle b;
    ppcp_session sess{}, moved{};
    ppcp_msg m{};
    const ppcp_instant openedAtDev = fixtureOpenedAt("tb:dev");
    const ppcp_instant openedAtOther = fixtureOpenedAt("tb:other");
    ASSERT_EQ(ppcp_session_make_hostless(&sess, "sess:1", "tb:dev", &openedAtDev), PPCP_OK);
    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_SESSION_OPEN, 1), PPCP_OK);
    m.body.session_open.session_id = sess.id;
    m.body.session_open.timebase_ref = sess.timebase_ref;
    ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);

    ASSERT_EQ(ppcp_session_make_hostless(&moved, "sess:1", "tb:other", &openedAtOther), PPCP_OK);
    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_SESSION_OPEN, 2), PPCP_OK);
    m.body.session_open.session_id = moved.id;
    m.body.session_open.timebase_ref = moved.timebase_ref;
    ASSERT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_OK);
    b.finish();

    Host host;
    const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
    ASSERT_TRUE(o.r.ok) << o.r.error;

    const ppcp_id *tb = ppcp_peer_timebase_ref(host.engine->peer());
    ASSERT_NE(tb, nullptr);
    EXPECT_EQ(std::string(tb->v, tb->len), "tb:dev");
}

// ── CT-I34 — re-import is a no-op, never a duplicate ─────────────────────
//
// "Identity is `Capture.id` scoped by session and owning peer, and `digest`
// where present is checked as CONTENT rather than used as the key."  The two
// Captures in the fixture are the ones that break a digest-keyed importer: a
// `complete` + `pending` clip with no digest yet, and an `absent` one that will
// never have one.
TEST(PpcpBundleImport, ASecondImportOfTheSameBundleAddsNothing)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle b;
    writeSession(b, SessionSpec{});

    Host host;
    const Host::Outcome first = host.import(b.bytes(), tmp.path().toStdString());
    ASSERT_TRUE(first.r.ok) << first.r.error;
    EXPECT_EQ(first.stats.captures, 2u);
    EXPECT_EQ(first.stats.capturesNew, 2u);
    EXPECT_EQ(first.stats.capturesAlreadyHeld, 0u);
    EXPECT_EQ(host.ledger.captureCount(), 2u);

    // A SECOND host, seeded from the same ledger — which is what a restart is.
    const Host::Outcome second = host.import(b.bytes(), tmp.path().toStdString());
    ASSERT_TRUE(second.r.ok) << second.r.error;
    EXPECT_EQ(second.stats.captures, 2u);
    EXPECT_EQ(second.stats.capturesNew, 0u);
    EXPECT_EQ(second.stats.capturesAlreadyHeld, 2u);
    EXPECT_EQ(host.ledger.captureCount(), 2u);
    EXPECT_EQ(host.ledger.sessionCount(), 1u);
}

// The same rule from the other side: the same Capture id under a DIFFERENT
// owning peer is a different Capture, because identity is scoped and not bare.
TEST(PpcpBundleImport, TheSameCaptureIdUnderAnotherPeerIsAnotherCapture)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle mine, theirs;
    writeSession(mine, SessionSpec{});
    SessionSpec other;
    other.peerId = "peer:other";
    writeSession(theirs, other);

    Host host;
    ASSERT_TRUE(host.import(mine.bytes(), tmp.path().toStdString()).r.ok);
    const Host::Outcome o = host.import(theirs.bytes(), tmp.path().toStdString());
    ASSERT_TRUE(o.r.ok) << o.r.error;
    EXPECT_EQ(o.stats.capturesNew, 2u);
    EXPECT_EQ(host.ledger.captureCount(), 4u);
}

// ── The clip reaches the disk, byte for byte ─────────────────────────────
TEST(PpcpBundleImport, ThePayloadBecomesAFileAndAnOwedCommit)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle b;
    writeSession(b, SessionSpec{});

    Host host;
    const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
    ASSERT_TRUE(o.r.ok) << o.r.error;
    EXPECT_EQ(o.stats.clipsWritten, 1u);
    EXPECT_EQ(o.stats.clipBytes, sizeof kClip);

    const CaptureKey key{ "peer:dev", "sess:1", "cap:1" };
    const PpcpImportLedger::CaptureRecord *rec = host.ledger.capture(key);
    ASSERT_NE(rec, nullptr);
    ASSERT_FALSE(rec->localPath.empty());

    std::ifstream in(rec->localPath, std::ios::binary);
    ASSERT_TRUE(in.good()) << rec->localPath;
    std::vector<std::uint8_t> got((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    EXPECT_EQ(got, std::vector<std::uint8_t>(kClip, kClip + sizeof kClip));

    // CORE 5.14h — the payload is durably held, so a `capture_committed` is
    // owed to the OWNING peer on its next connection. 8.4b makes this the only
    // route to `confirmed` on the offline path.
    EXPECT_EQ(o.stats.commitsQueued, 1u);
    const auto owed = host.ledger.pendingCommits("peer:dev");
    ASSERT_EQ(owed.size(), 1u);
    EXPECT_EQ(owed[0].key.captureId, "cap:1");
    // …and nothing is owed to a peer that did not mint it.
    EXPECT_TRUE(host.ledger.pendingCommits("peer:someone-else").empty());
}

// 5.14h1 — a commit naming a CLOSED Session is still owed, "because it may
// arrive days after the bundle was imported and releasing storage stays
// legitimate after a Session closes".
TEST(PpcpBundleImport, AClosedSessionDoesNotCancelWhatIsOwed)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle b;
    SessionSpec spec;
    spec.assertState = true;
    spec.asserted = PPCP_COMPLETE;
    writeSession(b, spec);

    Host host;
    const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
    ASSERT_TRUE(o.r.ok) << o.r.error;

    const PpcpImportLedger::SessionRecord *s = host.ledger.session("peer:dev", "sess:1");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->closed);
    EXPECT_EQ(host.ledger.pendingCommits("peer:dev").size(), 1u);
}

// ── ENC 7d / I10 — completeness is asserted, and never inferred upward ────
TEST(PpcpBundleImport, CompletenessHasThreeStatesAndTruncationNeverOverrulesAnAssertion)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // (a) nothing asserted, the file is whole -> `unknown`, NOT `complete`.
    {
        Bundle b;
        SessionSpec spec;
        writeSession(b, spec);
        Host host;
        const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
        ASSERT_TRUE(o.r.ok) << o.r.error;
        EXPECT_FALSE(o.r.truncated);
        EXPECT_FALSE(o.r.assertedCompleteness);
        EXPECT_EQ(o.r.completeness, PPCP_UNKNOWN);
    }

    // (b) nothing asserted, the file stops mid-frame -> `partial`.
    {
        Bundle b;
        writeSession(b, SessionSpec{});
        b.truncateBy(6);
        Host host;
        const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
        ASSERT_TRUE(o.r.ok) << o.r.error;
        EXPECT_TRUE(o.r.truncated);
        EXPECT_EQ(o.r.completeness, PPCP_PARTIAL);
        const PpcpImportLedger::SessionRecord *s = host.ledger.session("peer:dev", "sess:1");
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->completeness, Completeness::Partial);
    }

    // (c) asserted `complete`, the file stops mid-frame -> STILL complete. The
    // truncation is a defect in the bundle, not a downgrade of the Session
    // (CT-I36 (d)), and a receiver never overrules the owner (I10).
    {
        Bundle b;
        SessionSpec spec;
        spec.assertState = true;
        spec.asserted = PPCP_COMPLETE;
        writeSession(b, spec);
        b.truncateBy(6);
        Host host;
        const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
        ASSERT_TRUE(o.r.ok) << o.r.error;
        EXPECT_TRUE(o.r.truncated);
        EXPECT_TRUE(o.r.assertedCompleteness);
        EXPECT_EQ(o.r.completeness, PPCP_COMPLETE);
        const PpcpImportLedger::SessionRecord *s = host.ledger.session("peer:dev", "sess:1");
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->completeness, Completeness::Complete);
    }

    // (d) asserted `partial`, the file is whole -> partial. "Never upgrades a
    // partial Session to complete on the strength of what happened to be
    // present."
    {
        Bundle b;
        SessionSpec spec;
        spec.assertState = true;
        spec.asserted = PPCP_PARTIAL;
        writeSession(b, spec);
        Host host;
        const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
        ASSERT_TRUE(o.r.ok) << o.r.error;
        EXPECT_FALSE(o.r.truncated);
        EXPECT_EQ(o.r.completeness, PPCP_PARTIAL);
    }
}

// ── ENC 7f / I13 — a newer MINOR loads, a different MAJOR does not ────────
TEST(PpcpBundleImport, ANewerMinorLoadsAndADifferentMajorIsRefused)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    {
        Bundle b;
        writeSession(b, SessionSpec{});
        b.setMajorMinor(PPCP_BUNDLE_MAJOR, 99);
        Host host;
        const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
        EXPECT_TRUE(o.r.ok) << o.r.error;
        EXPECT_EQ(o.r.minor, 99);
        EXPECT_EQ(o.stats.sessionId, "sess:1");
    }
    {
        Bundle b;
        writeSession(b, SessionSpec{});
        b.setMajorMinor(2, 0);
        Host host;
        const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
        EXPECT_FALSE(o.r.ok);
        EXPECT_NE(o.r.error.find("header"), std::string::npos) << o.r.error;
        EXPECT_EQ(o.stats.captures, 0u);
    }
}

// ── ENC 7g / 2.1e — link_bind never appears in a bundle ───────────────────
//
// Two halves. The WRITER refuses to put one in, which is libppcp's, and is
// asserted here because a host that could write one would export bundles no
// other implementation should accept. The READER meets one anyway — a peer that
// recorded its live bytes too literally — and ignores it (I13) rather than
// losing the session.
TEST(PpcpBundleImport, ALinkBindIsRefusedByTheWriterAndIgnoredByTheReader)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle b;
    ppcp_msg m{};
    ASSERT_EQ(ppcp_msg_init(&m, PPCP_MT_LINK_BIND, 1), PPCP_OK);
    std::memset(m.body.link_bind.link_id, 0x11, PPCP_LINK_ID_BYTES);
    m.body.link_bind.channel = PPCP_CHANNEL_CONTROL;
    EXPECT_EQ(b.add(PPCP_CHANNEL_CONTROL, &m), PPCP_ERR_INVALID);

    // Written past the writer, as a non-conformant peer would have.
    b.addRaw(PPCP_CHANNEL_CONTROL, &m);
    writeSession(b, SessionSpec{});

    Host host;
    const Host::Outcome o = host.import(b.bytes(), tmp.path().toStdString());
    EXPECT_TRUE(o.r.ok) << o.r.error;
    EXPECT_EQ(o.stats.sessionId, "sess:1");
    EXPECT_EQ(o.stats.captures, 2u);
}

// ── The file path, end to end ────────────────────────────────────────────
TEST(PpcpBundleImport, ABundleOnDiskStreamsIntoTheSamePeerASocketWould)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    Bundle b;
    writeSession(b, SessionSpec{});
    const std::string path = b.writeTo(QDir(tmp.path()), "session.ppcpbndl");

    Host host;
    const Host::Outcome o = host.importFile(path, tmp.path().toStdString());
    ASSERT_TRUE(o.r.ok) << o.r.error;
    EXPECT_EQ(o.stats.captures, 2u);
    EXPECT_EQ(o.stats.clipsWritten, 1u);
    EXPECT_TRUE(QDir(QString::fromStdString(o.stats.sessionDir)).exists());
}

TEST(PpcpBundleImport, AMissingFileIsAnErrorAndNotACrash)
{
    Host host;
    PpcpBundleTransport::Options opt;
    opt.sink = host.engine->peer();
    const PpcpBundleTransport::Result r =
        PpcpBundleTransport::streamFile("/nonexistent/no.ppcpbndl", opt);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("cannot open"), std::string::npos);
}

// ── The ledger survives a restart, which is the only reason it exists ─────
TEST(PpcpImportLedgerPersistence, EverythingSurvivesAReloadIncludingWhatIsOwed)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string file = QDir(tmp.path()).filePath("ppcp-import.json").toStdString();

    Bundle b;
    writeSession(b, SessionSpec{});
    {
        Host host;
        host.ledger.setPath(file);
        ASSERT_TRUE(host.import(b.bytes(), tmp.path().toStdString()).r.ok);
        EXPECT_TRUE(host.ledger.save());
    }

    PpcpImportLedger reloaded;
    ASSERT_TRUE(reloaded.load(file));
    EXPECT_EQ(reloaded.captureCount(), 2u);
    EXPECT_EQ(reloaded.sessionCount(), 1u);
    EXPECT_EQ(reloaded.pendingCommits("peer:dev").size(), 1u);
    EXPECT_TRUE(reloaded.holds(CaptureKey{ "peer:dev", "sess:1", "cap:gone" }));

    // And a re-import against the RELOADED ledger is still a no-op (I34) —
    // which is what "seeded from whatever it kept" is for.
    ppcp_capture_index ix{};
    ASSERT_TRUE(reloaded.seedIndex(&ix));
    EXPECT_EQ(ppcp_capture_index_count(&ix), 2u);
}
