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

// Loopback conformance evidence for the PPCP transport (work package H1).
// Each test names the RV clause and the matrix row it stands for.
//
//   RT-4   strongest mode negotiated, never plaintext, outcome surfaced
//   RT-10  no application bytes before a completed handshake
//   RT-11  unknown identity and wrong key indistinguishable
//   RT-14  the §10.2 identity, unaltered, at whatever version is negotiated
//   RT-17  the offered set comes from a capability query, not a constant
//
// The K_tls here is the PPCP-RV §10.1 vector, hardcoded IN THIS TEST ONLY.  The
// production path derives it with HKDF-SHA256 from the pairing code (RV 5.1),
// which is libppcp work package L12 and is not app code — see the note on
// `kTlsVector` below.

#include "ppcp_transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Ppcp;

namespace {

std::vector<unsigned char> fromHex(const std::string &hex)
{
    std::vector<unsigned char> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// PPCP-RV §10.1 — K_tls = HKDF-Expand(PRK, "ppcp1 tls-psk", 32) for the vector
// pairing.  HARDCODED HERE ONLY, and only until libppcp's L12 derivation API
// lands: the app must never carry a key it did not derive.  Ground rule 1 —
// nothing is copied between repositories, and this is a published test vector
// from the specification, not library code.
Key kTlsVector()
{
    const std::vector<unsigned char> b =
        fromHex("2b0c55242ac1075eef80f548a7b39976b1cc2b88fbb6d609e5f3cd20f36d7fd4");
    Key k{};
    std::memcpy(k.data(), b.data(), k.size());
    return k;
}

// PPCP-RV §10.2 — identity = 0x01 || rn2 || tag, the 17 octets.  Note it is NOT
// valid UTF-8 (0xb3, 0xad, 0xa6 …), which is the point of 5.3f: a peer MUST NOT
// transcode, validate as text, or truncate an identity, and this one completing
// a handshake unchanged is the assertion.
PskIdentity identityVector()
{
    return fromHex("010f1e2d3c4b5a6978b355ada60b4b5aa8");
}

Key otherKey()
{
    Key k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<unsigned char>(0xA0 + i);
    return k;
}

// A resolver that knows exactly one pairing — the §10.2 identity — and returns
// the §10.1 K_tls for it.  RV 5.3b in miniature; the real one recomputes the tag
// under each held pairing's K_id inside libppcp.
IdentityResolver oneKnownPairing(const Key &key)
{
    const PskIdentity known = identityVector();
    return [known, key](const unsigned char *id, std::size_t len, ResolvedPairing &out) {
        if (len != known.size() || std::memcmp(id, known.data(), len) != 0) return false;
        out.kTls = key;
        out.pairingId = "vector-pairing";
        return true;
    };
}

struct LogSink {
    std::mutex m;
    std::vector<std::string> lines;
    void operator()(const std::string &l)
    {
        std::lock_guard<std::mutex> g(m);
        lines.push_back(l);
    }
    std::vector<std::string> snapshot()
    {
        std::lock_guard<std::mutex> g(m);
        return lines;
    }
};

// A listener on an ephemeral loopback port, accepting in its own thread.
class Harness {
public:
    explicit Harness(IdentityResolver r, int channelsPerPeer = 2, Options opts = Options{})
    {
        m_listener.setIdentityResolver(std::move(r));
        m_listener.setChannelsPerPeer(channelsPerPeer);
        m_listener.setOptions(opts);
        m_listener.setLog([this](const std::string &l) { m_log(l); });
        std::string err;
        m_ok = m_listener.listen(0, &err);
        m_port = m_listener.port();
    }

    bool ok() const { return m_ok; }
    std::uint16_t port() const { return m_port; }
    LogSink &log() { return m_log; }

    // Runs accept() on a worker and hands back the future.
    std::future<std::unique_ptr<PeerConnection>> acceptAsync(int timeoutMs)
    {
        return std::async(std::launch::async, [this, timeoutMs] {
            return m_listener.accept(timeoutMs, &m_fail);
        });
    }

    const HandshakeFailure &lastFailure() const { return m_fail; }

private:
    Listener m_listener;
    LogSink m_log;
    HandshakeFailure m_fail;
    std::uint16_t m_port = 0;
    bool m_ok = false;
};

ConnectorConfig clientConfig(std::uint16_t port, const Key &k, Options opts = Options{})
{
    ConnectorConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.kTls = k;
    cfg.identity = identityVector();
    cfg.options = opts;
    cfg.options.handshakeTimeoutMs = 5000;
    return cfg;
}

}  // namespace

// ── RT-17 — the offered set is a capability query, not a constant ───────────
// The review row asks a reader to confirm that this is what the code does.  The
// test cannot prove the negative ("no constant anywhere"), which is exactly why
// RT-17 is `review` and not `injected` — but it can prove the query runs, reads
// back what the linked OpenSSL actually has, and includes the floor RV 5.2d
// requires.  A build against an OpenSSL that gained a suite offers it with no
// edit to the source, and this test would show the longer list.
TEST(PpcpTransportCapabilities, OfferedSuitesComeFromTheLibraryNotAConstant)
{
    const TlsCapabilities caps = queryTlsCapabilities();

    EXPECT_FALSE(caps.libraryVersion.empty());

    // RV 5.2d: TLS_AES_128_GCM_SHA256 is supported at TLS 1.3.
    ASSERT_TRUE(caps.tls13Available) << "RV 5.2a wants 1.3 wherever both peers can reach it";
    EXPECT_NE(std::find(caps.tls13Suites.begin(), caps.tls13Suites.end(),
                        std::string("TLS_AES_128_GCM_SHA256")),
              caps.tls13Suites.end());

    // RV 5.2d floor at TLS 1.2.  Its presence is OpenSSL's answer, not ours; if
    // a future OpenSSL drops PSK suites this list shortens and the peer stops
    // offering them, which is 5.2b1 working rather than failing.
    EXPECT_FALSE(caps.tls12Suites.empty())
        << "no TLS 1.2 PSK suite offered — the device peer of RV 5.4b1 reaches "
           "nothing else, so this pairing could never complete";
}

// ── RT-4 / RT-14 — handshake completes, and the outcome is surfaced ─────────
TEST(PpcpTransport, HandshakeCompletesWithMatchingKtlsAndReportsTheMode)
{
    Harness h(oneKnownPairing(kTlsVector()));
    ASSERT_TRUE(h.ok());

    auto accepted = h.acceptAsync(5000);

    LogSink clientLog;
    ConnectorConfig cfg = clientConfig(h.port(), kTlsVector());
    cfg.log = [&clientLog](const std::string &l) { clientLog(l); };

    HandshakeFailure fail;
    std::unique_ptr<PeerConnection> client = Connector::connect(cfg, &fail);
    ASSERT_NE(client, nullptr) << fail.message;

    std::unique_ptr<PeerConnection> server = accepted.get();
    ASSERT_NE(server, nullptr);

    // CORE T2: two channels, and they are the two the protocol numbers (ENC 2a).
    ASSERT_NE(client->channel(Channel::Control), nullptr);
    ASSERT_NE(client->channel(Channel::Bulk), nullptr);
    ASSERT_NE(server->channel(Channel::Control), nullptr);
    ASSERT_NE(server->channel(Channel::Bulk), nullptr);
    EXPECT_EQ(client->channel(Channel::Preview), nullptr);

    // RV 5.4k: the achieved version and key-exchange mode reach the caller.
    const TlsOutcome &out = client->tls();
    EXPECT_TRUE(out.version == "TLSv1.3" || out.version == "TLSv1.2")
        << "RV 5.2a — nothing below TLS 1.2 is ever negotiated; got " << out.version;

    if (out.version == "TLSv1.3") {
        // RV 5.2b: at 1.3 the mode is psk_dhe_ke and psk_ke MUST NOT be used.
        EXPECT_EQ(out.kexMode, "psk_dhe_ke");
        EXPECT_TRUE(out.forwardSecrecy);
        EXPECT_FALSE(out.group.empty());
        // RV 5.2d.
        EXPECT_EQ(out.cipher, "TLS_AES_128_GCM_SHA256");
    } else {
        EXPECT_TRUE(out.kexMode == "ecdhe_psk" || out.kexMode == "psk");
    }

    // Both ends agree, and both ends can say so — 5.4k binds the server too.
    EXPECT_EQ(server->tls().version, out.version);
    EXPECT_EQ(server->tls().kexMode, out.kexMode);

    // The diagnostic line carries the mode and NOT the identity or the key
    // (RV 5.4k with RV 7.2b).
    const std::vector<std::string> lines = clientLog.snapshot();
    ASSERT_FALSE(lines.empty());
    bool sawMode = false;
    for (const std::string &l : lines) {
        if (l.find(out.kexMode) != std::string::npos) sawMode = true;
        EXPECT_EQ(l.find("2b0c5524"), std::string::npos) << "K_tls in a log line — RV 7.2b";
        EXPECT_EQ(l.find("b355ada6"), std::string::npos) << "identity in a log line — RV 7.2b";
    }
    EXPECT_TRUE(sawMode) << "RV 5.4k — the negotiated mode must be recorded";

    // RV 7.5d: nothing was taken on an early-data path.
    EXPECT_FALSE(client->channel(Channel::Control)->acceptedEarlyData());
    EXPECT_FALSE(server->channel(Channel::Control)->acceptedEarlyData());

    // The channels carry bytes, in order, both ways.  That is all PPCP asks of a
    // transport (CORE T1) — framing is libppcp's, above this line.
    const std::string msg = "hello";
    std::size_t put = 0;
    ASSERT_EQ(client->channel(Channel::Control)->write(msg.data(), msg.size(), put),
              IoStatus::Ok);
    EXPECT_EQ(put, msg.size());

    char buf[64] = {};
    std::size_t got = 0;
    for (int i = 0; i < 200 && got == 0; ++i) {
        const IoStatus st = server->channel(Channel::Control)->read(buf, sizeof buf, got);
        if (st == IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        else ASSERT_EQ(st, IoStatus::Ok);
    }
    EXPECT_EQ(std::string(buf, got), msg);
}

// ── RT-14 — the identity crosses as 17 raw octets (RV 5.3f) ────────────────
TEST(PpcpTransport, IdentityIsRawOctetsAndIsNotValidatedAsText)
{
    // The resolver is handed the bytes and compares them byte-for-byte.  If
    // anything in the path transcoded, truncated or UTF-8-validated the
    // identity, this resolver would not match and the handshake would fail.
    std::atomic<int> seenLen{0};
    std::atomic<bool> byteExact{false};
    const PskIdentity expect = identityVector();

    Harness h([&](const unsigned char *id, std::size_t len, ResolvedPairing &out) {
        seenLen = static_cast<int>(len);
        byteExact = (len == expect.size() && std::memcmp(id, expect.data(), len) == 0);
        if (!byteExact) return false;
        out.kTls = kTlsVector();
        out.pairingId = "vector-pairing";
        return true;
    });
    ASSERT_TRUE(h.ok());

    auto accepted = h.acceptAsync(5000);
    HandshakeFailure fail;
    std::unique_ptr<PeerConnection> client =
        Connector::connect(clientConfig(h.port(), kTlsVector()), &fail);
    ASSERT_NE(client, nullptr) << fail.message;
    ASSERT_NE(accepted.get(), nullptr);

    EXPECT_EQ(seenLen.load(), 17) << "RV 5.3a — the identity is 17 octets";
    EXPECT_TRUE(byteExact.load()) << "RV 5.3f — no transcoding, no truncation";
}

// ── RT-4 — a wrong K_tls fails, and fails closed (RV 5.2f) ─────────────────
TEST(PpcpTransport, HandshakeFailsWithMismatchedKtlsAndNeverFallsBack)
{
    Harness h(oneKnownPairing(kTlsVector()));
    ASSERT_TRUE(h.ok());

    auto accepted = h.acceptAsync(5000);

    HandshakeFailure fail;
    std::unique_ptr<PeerConnection> client =
        Connector::connect(clientConfig(h.port(), otherKey()), &fail);

    // RV 5.2f: a failed handshake is a failed connection.  There is no second
    // return value, no plaintext object, and nothing to write bytes into.
    EXPECT_EQ(client, nullptr);
    EXPECT_EQ(fail.message, "PPCP TLS handshake failed");

    EXPECT_EQ(accepted.get(), nullptr);
}

// ── RT-11 — unknown identity and wrong key are the same failure ────────────
// RV 5.3c (same alert), 5.3d (same timing, via the dummy key), 7.7c (uniform in
// content AND in how long it takes).  This is the row RV §9 calls one of the two
// most likely to be skipped, so it is asserted on both halves.
TEST(PpcpTransport, UnknownIdentityIsIndistinguishableFromAWrongKey)
{
    // Case A — the identity resolves, the key is wrong.
    HandshakeFailure aClient;
    std::vector<std::string> aServerLog;
    HandshakeFailure aServer;
    {
        Harness h(oneKnownPairing(kTlsVector()));
        ASSERT_TRUE(h.ok());
        auto accepted = h.acceptAsync(5000);
        EXPECT_EQ(Connector::connect(clientConfig(h.port(), otherKey()), &aClient), nullptr);
        EXPECT_EQ(accepted.get(), nullptr);
        aServer = h.lastFailure();
        aServerLog = h.log().snapshot();
    }

    // Case B — no pairing resolves the identity at all.
    HandshakeFailure bClient;
    std::vector<std::string> bServerLog;
    HandshakeFailure bServer;
    {
        Harness h([](const unsigned char *, std::size_t, ResolvedPairing &) { return false; });
        ASSERT_TRUE(h.ok());
        auto accepted = h.acceptAsync(5000);
        EXPECT_EQ(Connector::connect(clientConfig(h.port(), kTlsVector()), &bClient), nullptr);
        EXPECT_EQ(accepted.get(), nullptr);
        bServer = h.lastFailure();
        bServerLog = h.log().snapshot();
    }

    // Content — what the counterpart sees on the wire.
    EXPECT_EQ(aClient.message, bClient.message);
    EXPECT_EQ(aClient.alert, bClient.alert) << "RV 5.3c — the same alert for both";
    EXPECT_EQ(aClient.alertWasSent, bClient.alertWasSent);

    // Content — what we report and what we log.  A log line that named the cause
    // would not cross the wire, but it is the distinguisher a support engineer
    // would read out loud, and 7.7c does not carve out diagnostics.
    EXPECT_EQ(aServer.message, bServer.message);
    EXPECT_EQ(aServerLog, bServerLog) << "RV 7.7c — no distinguishing log";

    // ⚠ AND THE KIND IS UNIFORM TOO.  `FailureKind` exists so the embedding can
    // put a failed arrival on the screen, and it is the obvious place for a
    // future maintainer to add "unknownIdentity" / "wrongKey" as a convenience.
    // That would defeat this whole row without touching a byte on the wire, so
    // it is asserted here rather than left to the comment in the header.
    EXPECT_EQ(aServer.kind, bServer.kind) << "RV 7.7c — the kind named the cause";
    EXPECT_EQ(aServer.kind, FailureKind::Handshake);
    EXPECT_EQ(aClient.kind, bClient.kind);
    EXPECT_EQ(aServer.bind, BindRejection::None)
        << "an authentication failure is not a bind rejection";

    // Timing — 5.3d is a SHOULD and the technique it names is the dummy key,
    // which this transport uses: both cases run to Finished verification.  The
    // bound is deliberately loose because a unit test on a loaded machine cannot
    // measure a side channel; what it can catch is the structural regression
    // where an unresolvable identity aborts early and returns in a fraction of
    // the time.
    const double a = aServer.elapsedMs, b = bServer.elapsedMs;
    const double ratio = (a > b) ? (a / std::max(b, 0.001)) : (b / std::max(a, 0.001));
    EXPECT_LT(ratio, 10.0) << "RV 5.3d — an early abort would show up here: "
                           << a << "ms vs " << b << "ms";
}

// ── CORE T2/T5 — the channels are independent ──────────────────────────────
// The reason two connections is not negotiable (CORE 3.1): a 25 MB capture in
// flight on bulk must not delay the next shot's event on control.  Here the
// server never reads channel 1, so channel 1's window fills and its writes say
// WouldBlock — and channel 0 keeps working, both ways, throughout.
TEST(PpcpTransport, StalledBulkChannelDoesNotBlockControl)
{
    // Small socket buffers so "the window is full" is reachable in bounded time.
    // Nothing protocol-derived here — CORE 3.2 mandates no transport quality at
    // all; these are this test's knobs.
    Options opts;
    opts.sndBufBytes = 16 * 1024;
    opts.rcvBufBytes = 16 * 1024;

    Harness h(oneKnownPairing(kTlsVector()), 2, opts);
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(5000);

    HandshakeFailure fail;
    std::unique_ptr<PeerConnection> client =
        Connector::connect(clientConfig(h.port(), kTlsVector(), opts), &fail);
    ASSERT_NE(client, nullptr) << fail.message;
    std::unique_ptr<PeerConnection> server = accepted.get();
    ASSERT_NE(server, nullptr);

    // Fill channel 1 until it pushes back.  The server never reads it.
    std::vector<char> block(64 * 1024, 'x');
    std::size_t total = 0;
    bool stalled = false;
    for (int i = 0; i < 512 && !stalled; ++i) {
        std::size_t put = 0;
        const IoStatus st = client->channel(Channel::Bulk)->write(block.data(), block.size(), put);
        if (st == IoStatus::WouldBlock) stalled = true;
        else {
            ASSERT_EQ(st, IoStatus::Ok);
            total += put;
        }
    }
    ASSERT_TRUE(stalled) << "channel 1 never applied backpressure after " << total
                         << " bytes — CORE T2 wants per-channel flow control";

    // …and channel 0 is untouched by it (CORE T5).
    const std::string ping = "shot";
    std::size_t put = 0;
    ASSERT_EQ(client->channel(Channel::Control)->write(ping.data(), ping.size(), put),
              IoStatus::Ok);
    EXPECT_EQ(put, ping.size());

    char buf[32] = {};
    std::size_t got = 0;
    for (int i = 0; i < 400 && got == 0; ++i) {
        const IoStatus st = server->channel(Channel::Control)->read(buf, sizeof buf, got);
        if (st == IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        else ASSERT_EQ(st, IoStatus::Ok);
    }
    EXPECT_EQ(std::string(buf, got), ping)
        << "control blocked behind a stalled bulk channel — CORE 3.1";

    // And the reply comes back the other way while bulk is still stalled.
    ASSERT_EQ(server->channel(Channel::Control)->write("ack", 3, put), IoStatus::Ok);
    got = 0;
    std::memset(buf, 0, sizeof buf);
    for (int i = 0; i < 400 && got == 0; ++i) {
        const IoStatus st = client->channel(Channel::Control)->read(buf, sizeof buf, got);
        if (st == IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        else ASSERT_EQ(st, IoStatus::Ok);
    }
    EXPECT_EQ(std::string(buf, got), "ack");
}

// ── Plan A6 — the optional third channel ───────────────────────────────────
TEST(PpcpTransport, PreviewChannelIsCarriedWhenAskedFor)
{
    Harness h(oneKnownPairing(kTlsVector()), 3);
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(8000);

    ConnectorConfig cfg = clientConfig(h.port(), kTlsVector());
    cfg.channels = {Channel::Control, Channel::Bulk, Channel::Preview};

    HandshakeFailure fail;
    std::unique_ptr<PeerConnection> client = Connector::connect(cfg, &fail);
    ASSERT_NE(client, nullptr) << fail.message;
    std::unique_ptr<PeerConnection> server = accepted.get();
    ASSERT_NE(server, nullptr);

    EXPECT_EQ(client->channels().size(), 3u);
    ASSERT_NE(server->channel(Channel::Preview), nullptr);
    // Each channel is its own TLS session over the same K_tls (plan A6).
    EXPECT_EQ(server->channel(Channel::Preview)->tls().version,
              server->channel(Channel::Control)->tls().version);
}

// ── RT-10 (transport half) — nothing crosses before the handshake ──────────
// RV 7.5b requires `session_resume` to be refused on a connection that did not
// complete the handshake, and RV 7.7a forbids ANY PPCP message before it.  At
// this layer the guarantee is structural: a connect() that did not complete
// returns nothing to write to, and the peer engine that would parse
// `session_resume` is only ever fed from a TransportChannel that exists.  The
// message-level half of RT-10 lands with H2, which is where a `session_resume`
// can first be recognised at all.
TEST(PpcpTransport, NoChannelExistsUntilTheHandshakeCompletes)
{
    Harness h([](const unsigned char *, std::size_t, ResolvedPairing &) { return false; });
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(5000);

    HandshakeFailure fail;
    std::unique_ptr<PeerConnection> client =
        Connector::connect(clientConfig(h.port(), kTlsVector()), &fail);

    ASSERT_EQ(client, nullptr);
    EXPECT_EQ(accepted.get(), nullptr) << "a peer connection was handed out without a handshake";
}

// ─────────────────────────────────────────────────────────────────────────────
// An instrumented TLS 1.2-only counterpart.
//
// RV 5.2i says compliance with the clauses a platform may not expose is
// demonstrated by OBSERVED HANDSHAKE — a wire capture, or a counterpart
// instrumented to refuse what the clause forbids — never by an API assertion.
// This is that counterpart, in-process: a hand-built OpenSSL client pinned to
// TLS 1.2 and to the RFC 5487 floor, watching the wire.
//
// It exists because RV 5.4b1 measured the device peer reaching TLS 1.2 `0x00A8`
// and nothing else on the target hardware.  That leg is not hypothetical, it is
// the ordinary case, and two obligations only bite there: the `psk_identity_hint`
// must be EMPTY (5.2h property 3, RT-14) and the negotiated result must be
// reported honestly as carrying no forward secrecy (5.4k).
// ─────────────────────────────────────────────────────────────────────────────

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

struct Tls12Probe {
    Key key = kTlsVector();
    PskIdentity identity = identityVector();

    // ENC 2.1a — the first frame on the stream.  A probe that sent nothing here
    // would complete its handshake and then be discarded on the listener's bind
    // timeout, because since erratum E1 an unbound stream is not a channel.
    // The default is a correct `link_bind` on channel 0; a caller that wants to
    // exercise 2.1c's refusals overwrites it.
    LinkId linkId = [] { LinkId l{}; mintLinkId(l); return l; }();
    Channel channel = Channel::Control;
    std::vector<unsigned char> firstFrame;

    bool completed = false;
    bool sawServerKeyExchange = false;
    std::vector<unsigned char> hint;     // the psk_identity_hint as sent
    std::string version;
    std::string cipher;

    static int exIndex()
    {
        static const int i = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        return i;
    }

    static unsigned int pskCb(SSL *ssl, const char *, char *id, unsigned int maxId,
                              unsigned char *psk, unsigned int maxPsk)
    {
        Tls12Probe *p = static_cast<Tls12Probe *>(SSL_get_ex_data(ssl, exIndex()));
        if (!p || p->identity.size() + 1 > maxId || p->key.size() > maxPsk) return 0;
        std::memcpy(id, p->identity.data(), p->identity.size());
        id[p->identity.size()] = '\0';
        std::memcpy(psk, p->key.data(), p->key.size());
        return static_cast<unsigned int>(p->key.size());
    }

    static void msgCb(int write_p, int, int content_type, const void *buf, size_t len, SSL *ssl,
                      void *)
    {
        if (write_p || content_type != SSL3_RT_HANDSHAKE || len < 4) return;
        const unsigned char *b = static_cast<const unsigned char *>(buf);
        if (b[0] != 12 /* server_key_exchange */) return;   // RFC 5246 §7.4.3
        Tls12Probe *p = static_cast<Tls12Probe *>(SSL_get_ex_data(ssl, exIndex()));
        if (!p) return;
        p->sawServerKeyExchange = true;
        // For a PSK suite the body is: 2-byte length, then the hint (RFC 4279).
        if (len >= 6) {
            const std::size_t n = (static_cast<std::size_t>(b[4]) << 8) | b[5];
            p->hint.assign(b + 6, b + 6 + std::min(n, len - 6));
        }
    }

    // Returns true if the handshake completed.
    bool run(std::uint16_t port)
    {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return false;
        SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_cipher_list(ctx, "PSK-AES128-GCM-SHA256");   // 0x00A8, RFC 5487
        SSL_CTX_set_psk_client_callback(ctx, pskCb);
        SSL_CTX_set_msg_callback(ctx, msgCb);

        const int s = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
            SSL_CTX_free(ctx);
            return false;
        }

        SSL *ssl = SSL_new(ctx);
        SSL_set_ex_data(ssl, exIndex(), this);
        SSL_set_fd(ssl, s);
        completed = (SSL_connect(ssl) == 1);
        if (completed) {
            version = SSL_get_version(ssl);
            const SSL_CIPHER *c = SSL_get_current_cipher(ssl);
            if (c) cipher = SSL_CIPHER_standard_name(c) ? SSL_CIPHER_standard_name(c) : "";

            // ENC 2.1a — bind the stream, or send whatever the caller asked for
            // in its place.
            std::vector<unsigned char> frame = firstFrame;
            if (frame.empty()) encodeLinkBindFrame(linkId, channel, frame);
            if (!frame.empty())
                SSL_write(ssl, frame.data(), static_cast<int>(frame.size()));

            // Leave the connection up long enough for the listener to hand back
            // its channel, then close.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        SSL_free(ssl);
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
        SSL_CTX_free(ctx);
        return completed;
    }
};

}  // namespace

// ── RT-14 — the TLS 1.2 leg: empty hint, honest report ─────────────────────
TEST(PpcpTransportTls12, EmptyPskIdentityHintAndAnHonestForwardSecrecyReport)
{
    // One connection only: this is a handshake probe, not a PPCP session, so it
    // does not carry the two channels CORE T2 requires of a real peer.
    Harness h(oneKnownPairing(kTlsVector()), 1);
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(8000);

    Tls12Probe probe;
    const bool ok = probe.run(h.port());
    std::unique_ptr<PeerConnection> server = accepted.get();

    ASSERT_TRUE(ok) << "the host could not complete a TLS 1.2 PSK handshake — RV 5.4b1 "
                       "measured the device peer reaching nothing else, so this leg is "
                       "the ordinary case, not a fallback";
    EXPECT_EQ(probe.version, "TLSv1.2");
    EXPECT_EQ(probe.cipher, "TLS_PSK_WITH_AES_128_GCM_SHA256");   // 0x00A8, RV 5.4b1

    // RV 5.2h property 3 / RT-14: the hint is sent in the clear and MUST be
    // empty.  Observed on the wire, not asserted through an API — RV 5.2i.
    ASSERT_TRUE(probe.sawServerKeyExchange)
        << "no ServerKeyExchange: the hint was omitted rather than sent empty, which "
           "RT-14 distinguishes";
    EXPECT_TRUE(probe.hint.empty()) << "RV 5.2h property 3 — psk_identity_hint must be empty";

    // RV 5.4k: the outcome reaches the application, and it tells the truth about
    // what §5.4.3 gave up on this leg.
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(server->tls().version, "TLSv1.2");
    EXPECT_EQ(server->tls().kexMode, "psk");
    EXPECT_FALSE(server->tls().forwardSecrecy);
    EXPECT_NE(server->tls().describe().find("no forward secrecy"), std::string::npos);
}

// ── A DEFECT, characterised rather than papered over ───────────────────────
// RV 5.3f: the identity is 17 binary octets and a peer MUST NOT transcode,
// validate as text, or TRUNCATE it.  On the TLS 1.2 path that is not achievable
// through OpenSSL: both its client and its server PSK callbacks pass the
// identity as a `char *` and take its length with strlen.  The 16 CSPRNG bytes
// of 5.3a carry an embedded 0x00 about one connection in sixteen, and that
// connection fails — silently, and only sometimes, which is the worst shape a
// bug can have.
//
// This test pins the behaviour so that the day it changes, somebody is told.
// Reported for erratum: either 5.3a excludes 0x00 from rn2 (a one-line change
// to a generator that is already rejecting nothing), or 5.3f acknowledges the
// TLS 1.2 path cannot carry an arbitrary octet string.  It is NOT worked around
// here: truncating on purpose would be the transcoding 5.3f forbids.
TEST(PpcpTransportTls12, IdentityWithAnEmbeddedNulCannotSurviveTheTls12Path)
{
    PskIdentity withNul = identityVector();
    withNul[9] = 0x00;   // as a CSPRNG would produce roughly 6% of the time

    Harness h([&](const unsigned char *id, std::size_t len, ResolvedPairing &out) {
        if (len != withNul.size() || std::memcmp(id, withNul.data(), len) != 0) return false;
        out.kTls = kTlsVector();
        out.pairingId = "vector-pairing";
        return true;
    }, 1);
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(5000);

    Tls12Probe probe;
    probe.identity = withNul;
    const bool ok = probe.run(h.port());

    EXPECT_FALSE(ok) << "OpenSSL's TLS 1.2 PSK path now carries an embedded NUL — the "
                        "defect reported against RV 5.3f is fixed and this test should "
                        "become an assertion that it works";
    EXPECT_EQ(accepted.get(), nullptr);

    // The same identity at TLS 1.3, where the external-PSK session callbacks
    // take a pointer and a length, crosses intact.  The defect is the 1.2
    // interface, not the identity.
    Harness h13([&](const unsigned char *id, std::size_t len, ResolvedPairing &out) {
        if (len != withNul.size() || std::memcmp(id, withNul.data(), len) != 0) return false;
        out.kTls = kTlsVector();
        out.pairingId = "vector-pairing";
        return true;
    }, 2);
    ASSERT_TRUE(h13.ok());
    auto accepted13 = h13.acceptAsync(5000);
    ConnectorConfig cfg = clientConfig(h13.port(), kTlsVector());
    cfg.identity = withNul;
    HandshakeFailure fail;
    std::unique_ptr<PeerConnection> client = Connector::connect(cfg, &fail);
    ASSERT_NE(client, nullptr) << fail.message;
    std::unique_ptr<PeerConnection> server = accepted13.get();
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(server->tls().version, "TLSv1.3");
}

// ⚠ The abandoned-dial test that stood here has MOVED to
// ppcp_link_bind_test.cpp.  It was written against H1's rule that arrival order
// within a resolved pairing is channel order; erratum E1 withdrew that rule, so
// the property it guarded ("a stale half-built group must not renumber the next
// peer's channels") is now guarded by `link_id` instead of by a deadline, and
// the test that proves it belongs with the rest of the ENC §2.1 evidence.
