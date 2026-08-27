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

// ENC §2.1 / MSG §3.0 — binding streams to a link.  Erratum E1, 22 August 2026.
//
// Session 1 of this programme shipped two transports that associated a peer's
// connections two different ways: PinPointStudio grouped by the pairing the PSK
// identity resolved to and ordered channels by serialising the dialler's
// handshakes, PinPointCapture used arrival order.  Both were self-consistent
// and neither would have met the other.  E1 replaced both with an explicit
// `link_bind { link_id, channel }` as the first frame on every stream.
//
// The four tests below are the four cases the implicit rules got wrong:
//
//   1. two diallers dialling CONCURRENTLY bind into two correct links, which
//      arrival order cannot do at all;
//   2. an abandoned dial does not disturb the link that follows it — the case
//      H1 could only meet with a deadline, and meets here by construction;
//   3. a third (preview) channel is opened AFTER the link is established, which
//      both implicit rules forbade outright (2.1d);
//   4. a first frame that is not a valid `link_bind` closes that stream and
//      nothing else (2.1c).
//
// The K_tls here is the PPCP-RV §10.1 vector, as in ppcp_transport_test.cpp and
// for the same reason: it is a published specification vector, not library code.

#include "ppcp_transport.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using pp_sock = SOCKET;
#  define PP_BAD_SOCK INVALID_SOCKET
#  define pp_shut closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using pp_sock = int;
#  define PP_BAD_SOCK (-1)
#  define pp_shut ::close
#endif

using namespace Ppcp;

namespace {

std::vector<unsigned char> fromHex(const std::string &hex)
{
    std::vector<unsigned char> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// PPCP-RV §10.1 — K_tls for the vector pairing.
Key kTlsVector()
{
    const std::vector<unsigned char> b =
        fromHex("2b0c55242ac1075eef80f548a7b39976b1cc2b88fbb6d609e5f3cd20f36d7fd4");
    Key k{};
    std::memcpy(k.data(), b.data(), k.size());
    return k;
}

// PPCP-RV §10.2 — the 17 raw octets 0x01 || rn2 || tag.
PskIdentity identityVector()
{
    return fromHex("010f1e2d3c4b5a6978b355ada60b4b5aa8");
}

// One held pairing, as RV 5.3b resolves it.  ⚠ Every dialler in this file
// offers the SAME identity and therefore resolves to the SAME pairing — which
// is precisely what makes these tests evidence: under H1's withdrawn rule all
// of them would have been grouped into one link.
IdentityResolver oneKnownPairing()
{
    const PskIdentity known = identityVector();
    const Key key = kTlsVector();
    return [known, key](const unsigned char *id, std::size_t len, ResolvedPairing &out) {
        if (len != known.size() || std::memcmp(id, known.data(), len) != 0) return false;
        out.kTls = key;
        out.pairingId = "vector-pairing";
        return true;
    };
}

ConnectorConfig clientConfig(std::uint16_t port, Options opts = Options{})
{
    ConnectorConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.kTls = kTlsVector();
    cfg.identity = identityVector();
    cfg.options = opts;
    cfg.options.handshakeTimeoutMs = 5000;
    return cfg;
}

struct LogSink {
    std::mutex m;
    std::vector<std::string> lines;
    void operator()(const std::string &l)
    {
        std::lock_guard<std::mutex> g(m);
        lines.push_back(l);
    }
};

class Harness {
public:
    explicit Harness(int channelsPerPeer = 2, Options opts = Options{})
    {
        m_listener.setIdentityResolver(oneKnownPairing());
        m_listener.setChannelsPerPeer(channelsPerPeer);
        m_listener.setOptions(opts);
        m_listener.setLog([this](const std::string &l) { m_log(l); });
        std::string err;
        m_ok = m_listener.listen(0, &err);
        m_port = m_listener.port();
    }

    bool ok() const { return m_ok; }
    std::uint16_t port() const { return m_port; }
    Listener &listener() { return m_listener; }

    std::future<std::unique_ptr<PeerConnection>> acceptAsync(int timeoutMs)
    {
        return std::async(std::launch::async, [this, timeoutMs] {
            HandshakeFailure f;
            return m_listener.accept(timeoutMs, &f);
        });
    }

private:
    Listener m_listener;
    LogSink m_log;
    std::uint16_t m_port = 0;
    bool m_ok = false;
};

}  // namespace

// ── 2.1a — the token itself ────────────────────────────────────────────────
// Sixteen bytes, fresh every time.  A `link_id` that repeated would let one
// dialler's stream join another's link on a `direct` transport, which has no
// authentication to stop it (2.1f).
TEST(PpcpLinkBind, LinkIdIsSixteenFreshCsprngBytes)
{
    LinkId a{}, b{};
    ASSERT_TRUE(mintLinkId(a));
    ASSERT_TRUE(mintLinkId(b));
    EXPECT_EQ(a.size(), 16u);
    EXPECT_NE(a, b);

    LinkId zero{};
    EXPECT_NE(a, zero) << "a link_id of sixteen zero bytes means the CSPRNG did not run";
}

// ── 2.1b — CONCURRENT dials bind into the links they belong to ─────────────
// This is the case no implicit rule can meet.  Two diallers, overlapping in
// time, offering the SAME PSK identity: H1 would have grouped all four
// connections into one "peer" and numbered them 0,1,2,3 by arrival, so the
// second dialler's control channel would have become the first's channel 2.
TEST(PpcpLinkBind, ConcurrentDialsBindIntoSeparateLinks)
{
    Harness h;
    ASSERT_TRUE(h.ok());

    // Both diallers run at once, and both accepts run at once, so nothing in
    // the test imposes an order the transport could accidentally rely on.
    auto acceptA = h.acceptAsync(10000);

    std::atomic<bool> go{false};
    auto dial = [&] {
        while (!go.load()) std::this_thread::yield();
        HandshakeFailure f;
        return Connector::connect(clientConfig(h.port()), &f);
    };
    std::future<std::unique_ptr<PeerConnection>> dA = std::async(std::launch::async, dial);
    std::future<std::unique_ptr<PeerConnection>> dB = std::async(std::launch::async, dial);
    go.store(true);

    std::unique_ptr<PeerConnection> clientA = dA.get();
    std::unique_ptr<PeerConnection> clientB = dB.get();
    ASSERT_NE(clientA, nullptr);
    ASSERT_NE(clientB, nullptr);
    EXPECT_NE(clientA->linkId(), clientB->linkId());

    std::unique_ptr<PeerConnection> serverA = acceptA.get();
    ASSERT_NE(serverA, nullptr);
    HandshakeFailure f2;
    std::unique_ptr<PeerConnection> serverB = h.listener().accept(10000, &f2);
    ASSERT_NE(serverB, nullptr);

    // Each accepted link is exactly two channels — one control, one bulk — and
    // the two links are distinct.
    for (PeerConnection *s : {serverA.get(), serverB.get()}) {
        EXPECT_EQ(s->channels().size(), 2u);
        EXPECT_NE(s->channel(Channel::Control), nullptr);
        EXPECT_NE(s->channel(Channel::Bulk), nullptr);
        EXPECT_EQ(s->channel(Channel::Preview), nullptr);
    }
    EXPECT_NE(serverA->linkId(), serverB->linkId());

    // And each server link is the link the corresponding client minted.
    const bool paired =
        (serverA->linkId() == clientA->linkId() && serverB->linkId() == clientB->linkId()) ||
        (serverA->linkId() == clientB->linkId() && serverB->linkId() == clientA->linkId());
    EXPECT_TRUE(paired) << "a listener bound a link to a link_id nobody minted";
}

// ── 2.1c — an abandoned dial does not disturb the link that follows ────────
// H1 met this with a deadline: a half-built group older than the handshake
// timeout was cleared, because otherwise the next dialler's CONTROL channel
// became the stale group's BULK.  Under E1 the property holds by construction —
// the abandoned stream carries a different link_id and can never be joined —
// and the stale link is discarded on its own bind timeout, taking nothing with
// it.  NOTE there is no sleep here: the point is that no wait is needed.
TEST(PpcpLinkBind, AnAbandonedDialDoesNotDisturbTheNextLink)
{
    Options opts;
    opts.handshakeTimeoutMs = 2000;
    opts.bindTimeoutMs = 300;

    Harness h(2, opts);
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(10000);

    {
        // A dialler that gets channel 0 up and then goes away, leaving a link
        // with one stream on it and no prospect of a second.
        ConnectorConfig half = clientConfig(h.port(), opts);
        half.channels = {Channel::Control};
        HandshakeFailure f;
        std::unique_ptr<PeerConnection> orphan = Connector::connect(half, &f);
        ASSERT_NE(orphan, nullptr) << f.message;
    }   // closed here

    HandshakeFailure f;
    std::unique_ptr<PeerConnection> client = Connector::connect(clientConfig(h.port(), opts), &f);
    ASSERT_NE(client, nullptr) << f.message;

    std::unique_ptr<PeerConnection> server = accepted.get();
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(server->linkId(), client->linkId())
        << "the listener handed back the abandoned link, not the one that completed";
    EXPECT_EQ(server->channels().size(), 2u);
    EXPECT_NE(server->channel(Channel::Control), nullptr)
        << "the stale stream renumbered control into bulk";
    EXPECT_NE(server->channel(Channel::Bulk), nullptr);
}

// ── 2.1d — a third channel, opened after the link is established ───────────
// "A bulk channel MAY be opened at any later point in the session — a `preview`
// channel after the session is established is the expected case."  Both implicit
// rules forbade this outright: arrival order has no slot for it, and grouping by
// pairing cannot tell it from a whole new peer.
TEST(PpcpLinkBind, APreviewChannelCanBeBoundAfterTheLinkIsUp)
{
    Harness h;
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(10000);

    HandshakeFailure f;
    std::unique_ptr<PeerConnection> client = Connector::connect(clientConfig(h.port()), &f);
    ASSERT_NE(client, nullptr) << f.message;
    std::unique_ptr<PeerConnection> server = accepted.get();
    ASSERT_NE(server, nullptr);
    ASSERT_EQ(server->channels().size(), 2u);

    // Now, with the link already handed over, open channel 2 on it.
    auto late = std::async(std::launch::async, [&] {
        HandshakeFailure lf;
        return h.listener().acceptInto(*server, 10000, &lf);
    });

    HandshakeFailure cf;
    ASSERT_TRUE(Connector::connectAdditional(clientConfig(h.port()), *client,
                                             Channel::Preview, &cf))
        << cf.message;
    EXPECT_TRUE(late.get());

    EXPECT_EQ(client->channels().size(), 3u);
    EXPECT_NE(client->channel(Channel::Preview), nullptr);
    EXPECT_EQ(server->channels().size(), 3u);
    EXPECT_NE(server->channel(Channel::Preview), nullptr);
    EXPECT_EQ(server->linkId(), client->linkId());

    // 2.1c — and a SECOND channel 2 on the same link is refused, at the dialler
    // before it costs a socket and at the listener if one ever got that far.
    HandshakeFailure dup;
    EXPECT_FALSE(Connector::connectAdditional(clientConfig(h.port()), *client,
                                              Channel::Preview, &dup));
}

// ── ENC 2.1d, the way the APPLICATION actually takes a third channel ───────
//
// ⚠ `acceptInto()` above is not the call the app can make, and until 27 Aug the
// app made no call at all — a phone's preview channel bound and expired after
// `bindTimeoutMs`, silently, while a comment in `PpcpHostService::start()`
// claimed it was "taken through acceptInto()".  The reason is threading: this
// application hands an accepted link to the GUI thread and everything PPCP is
// single-threaded from that moment, so the accept thread cannot call a method
// that MUTATES a live PeerConnection.
//
// `acceptChannelFor()` is the seam that solves it — it returns the CHANNEL,
// which is self-contained and safe to hand across a thread, and the link's owner
// adopts it on its own.  This test is that path, and it exists because the
// preview channel is the one piece of today's work with no coverage and the same
// shape as the six defects that had none.
TEST(PpcpLinkBind, AThirdChannelIsCollectedWithoutTouchingTheLink)
{
    Harness h;
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(10000);

    HandshakeFailure f;
    std::unique_ptr<PeerConnection> client = Connector::connect(clientConfig(h.port()), &f);
    ASSERT_NE(client, nullptr) << f.message;
    std::unique_ptr<PeerConnection> server = accepted.get();
    ASSERT_NE(server, nullptr);
    ASSERT_EQ(server->channels().size(), 2u);

    // The link id is a value, so the collector needs nothing but a copy of it —
    // which is the whole point: the accept thread never sees the PeerConnection.
    const LinkId want = server->linkId();
    auto collected = std::async(std::launch::async, [&] {
        HandshakeFailure lf;
        return h.listener().acceptChannelFor(want, 10000, &lf);
    });

    HandshakeFailure cf;
    ASSERT_TRUE(Connector::connectAdditional(clientConfig(h.port()), *client,
                                             Channel::Preview, &cf))
        << cf.message;

    std::unique_ptr<TransportChannel> ch = collected.get();
    ASSERT_NE(ch, nullptr) << "the channel was not collected";
    // ⚠ NOT ADOPTED YET, AND THAT IS THE PROPERTY UNDER TEST.  Collecting must
    // leave the link untouched, or the accept thread would be mutating an
    // object the GUI thread owns — the race this seam exists to avoid.
    EXPECT_EQ(server->channels().size(), 2u);

    // The owner adopts it, on its own thread, whenever it likes.
    EXPECT_TRUE(server->adopt(std::move(ch)));
    EXPECT_EQ(server->channels().size(), 3u);
    EXPECT_NE(server->channel(Channel::Preview), nullptr);
    EXPECT_EQ(server->linkId(), client->linkId());
}

// A link nobody is collecting for is left alone: a stream binding some OTHER
// link must not be consumed by a wait for this one, or a second phone's
// connection would be eaten by the first phone's preview poll.
TEST(PpcpLinkBind, CollectingForOneLinkDoesNotSwallowAnother)
{
    Harness h;
    ASSERT_TRUE(h.ok());
    auto accepted = h.acceptAsync(10000);

    HandshakeFailure f;
    std::unique_ptr<PeerConnection> a = Connector::connect(clientConfig(h.port()), &f);
    ASSERT_NE(a, nullptr) << f.message;
    std::unique_ptr<PeerConnection> serverA = accepted.get();
    ASSERT_NE(serverA, nullptr);

    // Ask for a third channel on link A that nobody will ever dial...
    LinkId want = serverA->linkId();
    auto collecting = std::async(std::launch::async, [&] {
        HandshakeFailure lf;
        return h.listener().acceptChannelFor(want, 1500, &lf);
    });

    // ...while a SECOND phone connects.  Its two channels belong to a different
    // link and must survive the poll above rather than being drained by it.
    HandshakeFailure bf;
    std::unique_ptr<PeerConnection> b = Connector::connect(clientConfig(h.port()), &bf);
    ASSERT_NE(b, nullptr) << bf.message;

    EXPECT_EQ(collecting.get(), nullptr) << "nothing bound link A's id, so nothing is returned";

    HandshakeFailure af;
    std::unique_ptr<PeerConnection> serverB = h.listener().accept(10000, &af);
    ASSERT_NE(serverB, nullptr) << "the second phone's link was consumed by the poll";
    EXPECT_EQ(serverB->channels().size(), 2u);
    EXPECT_NE(serverB->linkId(), serverA->linkId());
}

// ── A foreign dialler, so 2.1c's refusals are reachable at all ─────────────
// Ppcp::Connector always sends a correct link_bind, which is the point of it —
// so the refusals of 2.1c cannot be provoked through it and have to come from a
// dialler this transport does not control.  That is exactly the case E1 exists
// for.  This is the same TLS 1.2 PSK probe ppcp_transport_test.cpp uses for
// RT-14 (RV 5.4b1 measured the device peer reaching nothing else), with the
// first frame under the test's control.
class ForeignDialler {
public:
    std::vector<unsigned char> firstFrame;   // empty = send nothing at all

    static int exIndex()
    {
        static const int i = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        return i;
    }

    static unsigned int pskCb(SSL *ssl, const char *, char *id, unsigned int maxId,
                              unsigned char *psk, unsigned int maxPsk)
    {
        ForeignDialler *p = static_cast<ForeignDialler *>(SSL_get_ex_data(ssl, exIndex()));
        if (!p) return 0;
        const PskIdentity ident = identityVector();
        const Key k = kTlsVector();
        if (ident.size() + 1 > maxId || k.size() > maxPsk) return 0;
        std::memcpy(id, ident.data(), ident.size());
        id[ident.size()] = '\0';
        std::memcpy(psk, k.data(), k.size());
        return static_cast<unsigned int>(k.size());
    }

    // Completes the handshake, sends `firstFrame`, holds the socket open long
    // enough for the listener to have made up its mind, then closes.
    bool run(std::uint16_t port, int holdMs = 250)
    {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return false;
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_cipher_list(ctx, "PSK-AES128-GCM-SHA256");   // 0x00A8, RFC 5487
        SSL_CTX_set_psk_client_callback(ctx, pskCb);

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
        const bool ok = (SSL_connect(ssl) == 1);
        if (ok) {
            if (!firstFrame.empty())
                SSL_write(ssl, firstFrame.data(), static_cast<int>(firstFrame.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        }
        SSL_free(ssl);
        pp_shut(s);
        SSL_CTX_free(ctx);
        return ok;
    }
};

// ── 2.1c — a first frame that is not a link_bind closes that stream ────────
// Four shapes, all of which a foreign or hostile dialler can produce.  Each
// closes ONE stream and nothing else: the listener names the refusal, does not
// hand the stream over, and the honest link dialled straight afterwards is
// unaffected.  That last half is the one that matters operationally — under
// H1's arrival-order rule, one confused stream renumbered every channel behind
// it.
TEST(PpcpLinkBind, ABadFirstFrameClosesOnlyThatStream)
{
    Options opts;
    opts.handshakeTimeoutMs = 2000;
    opts.bindTimeoutMs = 400;

    Harness h(2, opts);
    ASSERT_TRUE(h.ok());

    struct Case {
        const char *what;
        std::vector<unsigned char> frame;
        BindRejection expect;
        int holdMs;      // how long the foreign dialler keeps the socket open
    };
    std::vector<Case> cases;

    // (a) Silence after a completed handshake — no first frame at all.  Held
    // past bindTimeoutMs so the refusal is 2.1c's timeout rather than the
    // socket simply dying, which is a different thing and says so.
    cases.push_back({ "nothing at all", {}, BindRejection::BindTimeout, 700 });

    // (b) A well-formed frame carrying some other message.  `hello` is the
    // obvious one: 3.1a says it comes AFTER the link_bind, and a dialler built
    // against the pre-E1 rules sends it first.
    {
        // Encoded by hand rather than through libppcp's message writer, because
        // what is under test is the listener refusing a frame it did not expect,
        // not this test's ability to build one.  CBOR: map(2){"msg_id":1,
        // "type":"hello"} behind an 8-byte ENC §3 header on channel 0.
        const unsigned char body[] = {
            0xA2,                                            // map(2)
            0x66, 'm','s','g','_','i','d', 0x01,             // "msg_id": 1
            0x64, 't','y','p','e', 0x65, 'h','e','l','l','o' // "type": "hello"
        };
        std::vector<unsigned char> f = {
            0x00, 0x00, 0x00, static_cast<unsigned char>(sizeof body),
            0x00 /* channel */, 0x00 /* flags */, 0x00, 0x00 /* reserved */ };
        f.insert(f.end(), body, body + sizeof body);
        cases.push_back({ "a hello where the link_bind should be", f,
                          BindRejection::NotLinkBind, 100 });
    }

    // (c) A link_bind whose body `channel` disagrees with its frame header —
    // built by encoding for channel 1 and then rewriting the header byte to 0.
    {
        LinkId id{};
        ASSERT_TRUE(mintLinkId(id));
        std::vector<unsigned char> f;
        ASSERT_TRUE(encodeLinkBindFrame(id, Channel::Bulk, f));
        ASSERT_GT(f.size(), 4u);
        f[4] = 0x00;   // the header's channel byte, now contradicting the body
        cases.push_back({ "a link_bind whose channel contradicts its header", f,
                          BindRejection::ChannelMismatch, 100 });
    }

    // (d) Bytes that are not a frame at all.
    {
        std::vector<unsigned char> f = {
            0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
            0xFF, 0xFF, 0xFF, 0xFF };   // a valid header over CBOR that is not
        cases.push_back({ "a frame whose payload is not CBOR", f,
                          BindRejection::Malformed, 100 });
    }

    for (const Case &c : cases) {
        const int before = h.listener().bindRejectionCount();

        auto probe = std::async(std::launch::async, [&] {
            ForeignDialler d;
            d.firstFrame = c.frame;
            return d.run(h.port(), c.holdMs);
        });
        // Long enough for (a)'s bind timeout to expire; the others are refused
        // the moment their frame lands.
        HandshakeFailure f;
        std::unique_ptr<PeerConnection> nothing = h.listener().accept(c.holdMs + 500, &f);
        EXPECT_EQ(nothing, nullptr) << c.what << ": the listener handed a stream over";
        EXPECT_TRUE(probe.get()) << c.what << ": the probe could not even handshake";
        EXPECT_GT(h.listener().bindRejectionCount(), before) << c.what;
        EXPECT_EQ(h.listener().lastBindRejection(), c.expect) << c.what;
        // ⚠ AND IT REACHED THE CALLER.  The counters above have been right
        // since H1 and only this test ever read them; the embedding saw an
        // ordinary quiet poll and a pairing panel sat on "waiting" while a
        // phone had arrived and been refused.  accept() now reports through
        // `fail`, which is the whole point of the reporting path.
        EXPECT_EQ(f.kind, FailureKind::BindRejected) << c.what;
        EXPECT_EQ(f.bind, c.expect) << c.what;
    }

    // And the honest dialler behind all four is completely unaffected.
    auto accepted = h.acceptAsync(10000);
    HandshakeFailure f;
    std::unique_ptr<PeerConnection> client = Connector::connect(clientConfig(h.port(), opts), &f);
    ASSERT_NE(client, nullptr) << f.message;
    std::unique_ptr<PeerConnection> server = accepted.get();
    ASSERT_NE(server, nullptr) << "a bad first frame poisoned the listener";
    EXPECT_EQ(server->linkId(), client->linkId());
    EXPECT_EQ(server->channels().size(), 2u);
    EXPECT_NE(server->channel(Channel::Control), nullptr);
    EXPECT_NE(server->channel(Channel::Bulk), nullptr);
}

// ── A stream that arrives and never speaks TLS ─────────────────────────────
// The case that motivated the reporting path.  `expirePending()` runs at the
// TOP of the accept loop and erases such a stream in place, so before this it
// could reach the log and nothing else — accept() returned null exactly as it
// does for an ordinary quiet poll, and the two were indistinguishable to the
// embedding.  A phone whose TLS never got going was therefore invisible.
//
// ⚠ NOTHING UNIFORM IS OWED HERE.  RV 7.7c holds together an unknown identity
// and a wrong key — two ways of REJECTING a counterpart.  This counterpart was
// never rejected; it stopped talking, and saying so gives nothing away.
TEST(PpcpLinkBind, AStreamThatNeverHandshakesIsReportedAndNotOnlyLogged)
{
    Options opts;
    opts.handshakeTimeoutMs = 300;   // short, so the test is not a wait
    opts.bindTimeoutMs = 300;

    Harness h(2, opts);
    ASSERT_TRUE(h.ok());

    // A bare TCP connection: no TLS, no bytes, just an open socket.
    const int sock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_GE(sock, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(h.port());
    ASSERT_EQ(::connect(sock, reinterpret_cast<sockaddr *>(&a), sizeof a), 0);

    HandshakeFailure f;
    std::unique_ptr<PeerConnection> nothing = h.listener().accept(2000, &f);

    EXPECT_EQ(nothing, nullptr) << "a silent socket was handed over as a link";
    EXPECT_EQ(f.kind, FailureKind::HandshakeTimeout)
        << "the handshake timeout did not reach the caller";
    EXPECT_GT(f.elapsedMs, 0.0);

    pp_shut(sock);
}

// ── Quiet is not failure ───────────────────────────────────────────────────
// The other half of the same contract, and the one that keeps the pairing panel
// usable: an accept() that simply timed out with nobody dialling MUST leave
// `kind` as None.  Without this the panel would report a failure every 250 ms
// for the whole time it is open.
TEST(PpcpLinkBind, AnAcceptThatNobodyDialledReportsNoFailure)
{
    Harness h;
    ASSERT_TRUE(h.ok());

    HandshakeFailure f;
    std::unique_ptr<PeerConnection> nothing = h.listener().accept(150, &f);

    EXPECT_EQ(nothing, nullptr);
    EXPECT_EQ(f.kind, FailureKind::None) << "an idle poll was reported as a failure";
    EXPECT_TRUE(f.message.empty()) << "an idle poll left a message behind";
}
