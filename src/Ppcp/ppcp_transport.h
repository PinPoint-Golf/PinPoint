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

#pragma once

// PPCP transport — two (optionally three) TCP connections per peer, each its own
// TLS session keyed by the same K_tls.  This is work package H1 of
// libppcp/docs/implementation/implementation-plan.md.
//
// WHAT THIS LAYER OWES THE REST OF THE PROGRAM, and nothing more: two ordered,
// reliable, independently flow-controlled byte streams per peer — CORE T1, T2,
// T5.  Message boundaries (T3) are PPCP's framing, read out of these bytes by
// libppcp, not by us.  Addressing, discovery and authentication are explicitly
// NOT part of the transport contract (T4); the pairing code and the derivation
// that produces K_tls are PPCP-RV, and land in H6.
//
// WHY TWO CONNECTIONS AND NOT A MULTIPLEXER (CORE 3.1, plan decision A6).  A
// shot event must reach the host while a 25 MB capture is in flight.  One TCP
// connection carrying both head-of-line blocks the second shot behind the first
// shot's video.  Two connections give per-channel flow control for free and
// make ENC 2c's "header channel matches the stream it arrived on" checkable.
//
// WHY OpenSSL DIRECTLY AND NOT QSslSocket (RV §8).  Qt's PSK interface is the
// RFC 4279 one — identity hints, TLS 1.2 ciphersuite selection — and it does not
// reach a TLS 1.3 EXTERNAL PSK at all.  RV §8 names that trap in as many words.
// We therefore drive OpenSSL's external-PSK session callbacks ourselves and
// build the synthetic SSL_SESSION with the cipher and, critically, the hash of
// RV 5.2c bound to it.  Getting the hash wrong produces a handshake failure with
// no useful diagnostic, indistinguishable from a key mismatch — RV §8 again.
//
// Deliberately Qt-free.  This compiles and tests without a Qt event loop so the
// conformance harness (H8) can drive it headless, and so the sockets can be
// pumped from whichever thread H2 chooses.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Ppcp {

namespace detail { struct ChannelFactory; }

// ── Channels ────────────────────────────────────────────────────────────────
// ENC 2a: channel 0 is control, 1 and above are bulk, 255 is reserved.  Channel
// 2 carries preview where a device offers one (plan A6).  The numbering is the
// protocol's; the transport only has to keep the streams apart and tell the
// caller which is which.
enum class Channel : std::uint8_t {
    Control = 0,
    Bulk    = 1,
    Preview = 2,
};

// ── The link, and the token that names it (ENC §2.1, erratum E1) ────────────
//
// A LINK is the set of TCP connections between two peers that together carry
// one PPCP session — one connection per channel here (plan A6).  ENC 2.1 exists
// because a listener cannot infer from the transport which arriving connections
// belong to one peer, nor which of them is channel 0: arrival order breaks the
// moment a dialler opens channels concurrently or abandons a dial, an address
// is shared by every peer behind one NAT, and a TLS identity exists only on the
// rendezvous path.  Session 1 of this programme grouped by the resolved PSK
// pairing and ordered by serialised handshakes; PinPointCapture used arrival
// order.  Both were self-consistent and neither would have met the other, which
// is what erratum E1 records and what this type now settles.
//
// 2.1a: the DIALLER mints 16 CSPRNG bytes per link and sends
// `link_bind { link_id, channel }` as the FIRST frame on every stream it opens,
// on every channel including 0.  The listener binds by `link_id` and takes the
// channel from the frame header (2.1b).  2.1f: it is a transport-binding token
// and nothing else — not `Peer.id`, never persisted, never reused.
using LinkId = std::array<unsigned char, 16>;

// 16 bytes from the CSPRNG (2.1a).  Returns false if the RNG failed, which is
// fatal to the dial: a predictable link_id would let a stranger's stream join
// somebody else's link on a `direct` transport that has no authentication.
bool mintLinkId(LinkId &out);

// Why a listener refused a stream under 2.1c.  This is NOT an authentication
// outcome — the handshake has already succeeded by the time any of these can
// happen — so RV 7.7c's uniformity does not reach it and the reason may be
// named, logged and asserted on.
enum class BindRejection {
    None = 0,
    NotLinkBind,       // 2.1c: the first frame was not a `link_bind`
    ChannelMismatch,   // 2.1c: body `channel` disagreed with the frame header
    DuplicateChannel,  // 2.1c: that link already holds that channel
    Malformed,         // ENC §4/§8: the frame or its CBOR did not decode
    BindTimeout,       // 2.1c: no `link_bind` arrived inside the bind timeout
};
const char *describe(BindRejection r);

// ENC 2.1a / MSG 3.0 — the `link_bind` frame for one stream, encoded, header
// and all.  Public because a dialler that is not this Connector still owes the
// frame: the `direct` path of CORE §3.2 is a tunnel or a socket an embedding
// hands in, and the synthetic peer of CONF §2c has to be able to produce both a
// correct one and a wrong one.  Returns false only if libppcp refuses the
// encode, which would be a bug in this file rather than in the caller.
bool encodeLinkBindFrame(const LinkId &id, Channel ch, std::vector<unsigned char> &out);

// The 32-byte external pre-shared key of RV 5.1 — HKDF-Expand(PRK, "ppcp1
// tls-psk", 32).  Used for the TLS handshake and for nothing else (5.1a).
using Key = std::array<unsigned char, 32>;

// The PSK identity of RV 5.3a: the raw 17 octets 0x01 || rn2 || tag.  It is
// BINARY (5.3f) — a peer MUST NOT transcode it, validate it as text, or
// truncate it.  Hence std::vector<unsigned char> and not std::string: nothing in
// this file may be tempted to call strlen on it.
using PskIdentity = std::vector<unsigned char>;

// ── The negotiated outcome, surfaced to the caller (RV 5.4k) ────────────────
// 5.4k is a MUST: the achieved TLS version and key-exchange mode are made
// available to the application layer and recorded in the diagnostic export.
// Forward secrecy is a per-connection outcome since the 5.4.3 relaxation, not a
// property of the protocol, so a peer that cannot say which it got cannot apply
// a policy to it and cannot honestly tell a user what "encrypted" means here.
// RV 7.2b forbids that export carrying keys or payloads; none of this is either.
struct TlsOutcome {
    std::string version;        // "TLSv1.3" / "TLSv1.2"
    std::string cipher;         // OpenSSL's name for the negotiated suite
    std::string kexMode;        // "psk_dhe_ke" | "psk_ke" | "ecdhe_psk" | "psk"
    std::string group;          // negotiated FFDHE/ECDHE group, empty if none
    bool forwardSecrecy = false;  // true iff the mode carries an ephemeral DH

    // One line, safe for a log or a diagnostic export.  No identity, no key.
    std::string describe() const;
};

// ── Failure, uniformly (RV 5.3c, 7.7c) ──────────────────────────────────────
// A server that resolves no pairing aborts with the SAME alert it would send for
// a resolved identity and a wrong key (5.3c), and the rejection is uniform in
// what it returns AND in how long it takes (7.7c).  So this struct carries no
// field that could tell the two apart, `message` is drawn from a fixed set that
// does not name the cause, and the resolver's verdict is never logged.
//
// ⚠ THE UNIFORMITY BINDS THE WIRE, NOT THE HOST'S OWN SCREEN.  7.7c is about
// what the COUNTERPART can observe — "in what it returns or in how long it
// takes" — and 7.7b about what is disclosed "to an unauthenticated
// counterpart".  Neither reaches what this host shows its own operator, and RV
// 4.2b and §8 ask in terms for a failure a user can act on.  So `kind` below
// exists to let the embedding SAY SOMETHING, and the one kind that must stay
// silent about its cause is the one that always was.
enum class FailureKind {
    None = 0,          // nothing failed.  A poll that timed out with no dial is
                       // NOT a failure and leaves this None — that is how a
                       // caller tells "quiet" from "refused".
    // ⚠ UNIFORM, AND THE REASON IS NOT AVAILABLE HERE BY DESIGN.  An
    // unresolvable identity and a wrong key both arrive as this, with the same
    // `message` and the same `alert`.  Nothing may be added to this struct that
    // tells them apart, and nothing that fills it may consult the resolver's
    // verdict: that IS 5.3c/7.7c, and 5.3c1 records that the wrong-key branch
    // is unreachable anyway while K_tls and K_id share one PRK.
    Handshake,
    NotForwardSecret,  // 5.2b — psk_ke, refused AFTER a completed handshake, so
                       // it is a policy outcome and not an authentication one.
    HandshakeTimeout,  // no handshake completed inside handshakeTimeoutMs.  The
                       // stream never authenticated, so there is nothing to be
                       // uniform ABOUT: it is a dead socket, not a rejection.
    BindRejected,      // ENC 2.1c — see `bind` for which.  Post-handshake.
};
// Plain words for a kind, safe for a log and for a screen.  ⚠ `Handshake`'s
// text must never grow a cause.
const char *describe(FailureKind k);

struct HandshakeFailure {
    std::string message;        // uniform text; never says which check failed
    int alert = -1;             // TLS alert seen on the wire, -1 if none
    bool alertWasSent = false;  // true: we sent it; false: the peer sent it
    double elapsedMs = 0.0;     // for the timing half of 7.7c / RT-11

    // Which of the outcomes above this is.  `None` means nothing failed.
    FailureKind kind = FailureKind::None;
    // Only meaningful when `kind == BindRejected`; `None` otherwise.
    BindRejection bind = BindRejection::None;
};

// ── One channel: one TCP connection, one TLS session ────────────────────────
//
// Non-blocking throughout.  `read`/`write` return WouldBlock rather than
// stalling, which is what makes the backpressure of CORE T2 visible to the
// caller: a bulk channel that has filled its window says so, and the control
// channel on its own connection carries on regardless.  `fd()` is exposed so
// H2's pump can poll several channels at once without this class owning a
// thread — the same sans-I/O discipline libppcp itself keeps (ground rule 7).
enum class IoStatus {
    Ok,         // progress made; `got`/`put` says how much
    WouldBlock, // no progress now; poll and retry
    Closed,     // orderly shutdown by the peer
    Error,      // fatal; the channel is dead
};

class TransportChannel {
public:
    ~TransportChannel();
    TransportChannel(const TransportChannel &) = delete;
    TransportChannel &operator=(const TransportChannel &) = delete;

    IoStatus read(void *buf, std::size_t len, std::size_t &got);
    IoStatus write(const void *buf, std::size_t len, std::size_t &put);

    Channel channel() const { return m_channel; }
    const TlsOutcome &tls() const { return m_tls; }
    int fd() const;
    bool isOpen() const;
    void close();

    // RV 7.5d: a peer MUST NOT accept application data on an early-data path,
    // because TLS 1.3 early data is replayable by design and a resumed
    // connection that took `arm` as early data would take a replay of it.  This
    // is true by construction here — see the .cpp — and the accessor exists so a
    // test can assert it rather than trust the comment.
    bool acceptedEarlyData() const { return false; }

    // ⚠ F-H6-2 — WHICH PAIRING AUTHENTICATED THIS STREAM, AND IT HAD NO WAY
    // OUT UNTIL H6.  `ResolvedPairing::pairingId` below is documented as "the
    // embedding's handle on WHICH pairing authenticated a stream", the listener
    // has held it since H1, and there was no accessor — so an embedding that
    // had to act on it (RV 7.3a's "invalidate once `mu` handshakes have
    // completed" is the whole of the single-use defence) could not find out
    // which code had just been used.  Empty on the dialling side, which never
    // resolves anything.
    const std::string &pairingId() const;

    // The socket and the SSL* behind this channel.  Named here and DEFINED
    // ONLY IN ppcp_transport.cpp: nothing outside that file can do anything
    // with it but pass it along, which is what the connector's dial slots and
    // the listener's pending streams do while a channel is being built.  Public
    // because those are free functions in the .cpp rather than members, and an
    // incomplete type is not an encapsulation the header was keeping.
    struct Impl;

private:
    friend struct detail::ChannelFactory;
    friend class Connector;
    friend class Listener;
    TransportChannel();

    std::unique_ptr<Impl> m_impl;
    Channel m_channel = Channel::Control;
    TlsOutcome m_tls;
};

// ── One link: the streams that ENC 2.1 bound together ───────────────────────
// Named PeerConnection since H1 and kept for the call sites; ENC 2.1 calls the
// same thing a LINK, and `linkId()` is the name it was bound under.
class PeerConnection {
public:
    ~PeerConnection();
    PeerConnection(const PeerConnection &) = delete;
    PeerConnection &operator=(const PeerConnection &) = delete;

    // Null if this link does not carry that channel.  Channel 2 is optional
    // (plan A6); 0 and 1 are the CORE T2 minimum and are always here.
    TransportChannel *channel(Channel c) const;

    // The control channel's outcome.  Every channel of one link is a separate
    // TLS session over the same K_tls, so in practice they agree; the
    // per-channel value is on the channel for the case where they do not.
    const TlsOutcome &tls() const;

    // ENC 2.1a/2.1b — the token the dialler minted and both ends bound by.
    const LinkId &linkId() const { return m_linkId; }

    // F-H6-2 — the pairing the LISTENER resolved for this link, read off the
    // control channel.  Every stream of one link resolves the same pairing
    // (they are separate TLS sessions over the same K_tls), so the link-level
    // answer is well defined and is the one RV 7.3a needs: `mu` counts links,
    // never handshakes — see PpcpRendezvous::noteLinkEstablished().
    const std::string &pairingId() const;

    std::vector<Channel> channels() const;
    void close();

    // ENC 2.1d: a bulk channel MAY be opened at any later point in the session
    // — a `preview` channel after the session is established is the expected
    // case — by a further stream carrying `link_bind` with the SAME link_id.
    // Both ends reach that through here: the dialler via
    // Connector::connectAdditional, the listener via Listener::acceptInto.
    // Returns false if this link already holds that channel (2.1c).
    bool adopt(std::unique_ptr<TransportChannel> c);

private:
    friend struct detail::ChannelFactory;
    PeerConnection();

    LinkId m_linkId{};
    std::vector<std::unique_ptr<TransportChannel>> m_channels;
};

// ── Tuning that is ours, not the protocol's ─────────────────────────────────
// No threshold from PPCP appears here.  These are socket and deadline knobs
// belonging to this transport: PPCP carries no transport quality requirement at
// all (CORE 3.2) and no timing constant of the protocol is expressible as a
// timeout on a handshake.
struct Options {
    int handshakeTimeoutMs = 10000;
    // ENC 2.1c: "a link that has not bound channel 0 within the listener's own
    // timeout is discarded with every stream it holds; THE TIMEOUT IS THE
    // EMBEDDING'S POLICY".  So it is a knob here and not a constant in libppcp,
    // for the same reason the 120 fps ingest floor is (I14).
    int bindTimeoutMs = 10000;
    int sndBufBytes = 0;   // 0 = leave the OS default. Tests set it small so a
    int rcvBufBytes = 0;   // stalled channel is reachable in bounded time.
    bool tcpNoDelay = true;
};

// Diagnostic sink.  RV 5.4k wants the negotiated mode recorded; RV 7.2b forbids
// a secret, a derived key or a decoded payload reaching a log or a diagnostic
// export, so nothing handed to this callback contains one — not the identity,
// not K_tls, and on failure not the reason (7.7c).
using LogFn = std::function<void(const std::string &line)>;

// ── Dialling ────────────────────────────────────────────────────────────────
// RV 5.2g: the peer that dialled is the TLS client.  On the pairing-code path
// the scanner dials, so the host is the client only on the discovery path (RV
// §2) — which is why this class exists at all on a peer that mostly listens.
struct ConnectorConfig {
    std::string host;
    std::uint16_t port = 0;
    Key kTls{};
    PskIdentity identity;                       // the raw 17 octets of RV 5.3a
    std::vector<Channel> channels{Channel::Control, Channel::Bulk};
    Options options;
    LogFn log;
};

class Connector {
public:
    // Returns null on any failure, with `fail` filled in.  There is no second
    // return value that could carry an unencrypted connection: RV 5.2f forbids
    // falling back to one under ANY circumstance, including a handshake
    // failure, a timeout or a user instruction, and the way to be sure of that
    // is to have no code path that produces one.
    //
    // Mints one `link_id` for the whole call (ENC 2.1a) and sends `link_bind`
    // as the first frame on every stream.  The TLS handshakes run CONCURRENTLY
    // and in no particular order — H1 serialised them so that arrival order at
    // the listener would be the channel order, and E1 withdrew the rule that
    // made that necessary.  Nothing downstream may depend on the order again.
    static std::unique_ptr<PeerConnection> connect(const ConnectorConfig &cfg,
                                                   HandshakeFailure *fail = nullptr);

    // ENC 2.1d — one more stream on an established link, carrying `link_bind`
    // with that link's existing `link_id`.  This is how the preview channel is
    // opened after the session is up.  On success the channel is adopted into
    // `link` and the function returns true.
    static bool connectAdditional(const ConnectorConfig &cfg, PeerConnection &link,
                                  Channel ch, HandshakeFailure *fail = nullptr);
};

// ── Listening ───────────────────────────────────────────────────────────────
// The server side of RV 5.3: it resolves an offered identity by recomputing the
// tag with the K_id of each pairing it holds (5.3b) and selecting the match.
// That computation is libppcp's (plan A7/A8 — the library provides K_tls, K_id,
// the identity and its resolver, and nothing that touches a socket), so this
// transport takes it as a callback and knows nothing about how it is done.
struct ResolvedPairing {
    Key kTls{};
    // Opaque, stable for the life of one pairing, never sent anywhere.  It is
    // the embedding's handle on WHICH pairing authenticated a stream — nothing
    // more.  ⚠ It is NOT how the channels of one peer are associated: H1 used
    // it for exactly that and erratum E1 withdrew the rule.  Association is
    // `link_id`, on the wire, and nothing else (ENC 2.1b).
    std::string pairingId;
};

// Return false for "no pairing resolves this identity".  Do NOT throw, do not
// log, and do not vary in cost by outcome: 5.3d asks the two failing paths to be
// indistinguishable in timing as well as in content, and this callback is on
// the timed path.  The identity is handed over as raw bytes and MUST NOT be
// transcoded or validated as text (5.3f).
using IdentityResolver =
    std::function<bool(const unsigned char *identity, std::size_t len, ResolvedPairing &out)>;

class Listener {
public:
    Listener();
    ~Listener();
    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;

    // port 0 binds an ephemeral port; read it back with `port()`.
    bool listen(std::uint16_t port, std::string *err = nullptr);
    std::uint16_t port() const;
    void stop();

    void setIdentityResolver(IdentityResolver r);
    void setOptions(const Options &o);
    void setLog(LogFn f);

    // How many bound channels make a link ready to hand over.  CORE T2's
    // minimum is two; a third arrives later under ENC 2.1d and is taken through
    // acceptInto(), not counted here.  The listener has to be told because
    // `link_bind` says which channel a stream is, never how many are coming.
    void setChannelsPerPeer(int n);

#if defined(PP_PPCP_PLAINTEXT_HARNESS)
    // ══════════════════════════════════════════════════════════════════════
    // ⚠⚠⚠  THIS ACCEPTS CONNECTIONS WITH NO TLS AND NO AUTHENTICATION.  ⚠⚠⚠
    //
    // IT MUST NEVER BE COMPILED INTO A SHIPPING BUILD.  The whole declaration
    // — and every line that implements it — is inside
    // `#if defined(PP_PPCP_PLAINTEXT_HARNESS)`, and that macro is defined by
    // exactly one thing: the CMake option `PP_PPCP_PLAINTEXT_HARNESS`, which
    // is `OFF` by default and is turned on ONLY by `src/Ppcp/tests`.  The
    // application's own CMakeLists.txt never sets it, so in a release build
    // there is no plaintext code path at all — which is the same discipline
    // `Connector::connect()` keeps for RV 5.2f, applied to the listener.
    //
    // WHY IT EXISTS.  `PPCP-CONF` §2c requires the peer under test to be
    // driven from OUTSIDE, by the synthetic peer both implementations develop
    // against.  `tools/ppcp-sim` has no TLS transport — its `--psk-ke-only`
    // mode is a hand-built ClientHello for RT-4 and speaks no application
    // data — so a conformance harness cannot reach this host over the
    // rendezvous transport.  `PPCP-RV` erratum E4 (RV 2c1) settles what that
    // means: the rendezvous *paths* are what 2c constrains, and a test
    // harness socket is not one of them.  `CORE` §3.2's `direct` transport is
    // conformant plaintext, and this is a `direct` listener.
    //
    // WHAT IT DOES NOT CHANGE.  ENC §2.1 link binding is identical — the
    // dialler still mints a `link_id` and sends `link_bind` first on every
    // stream, and every 2.1c refusal still applies.  The identity resolver is
    // never consulted, because there is no identity: a plaintext listener
    // authenticates nobody and says so through `TlsOutcome::version` being
    // the literal string "plaintext-harness".
    void setPlaintextHarness(bool on);
#endif

    // Blocks up to `timeoutMs` for a link that has bound channelsPerPeer
    // channels.  Returns null on timeout (with `fail->message` empty) or on a
    // failed handshake (with `fail` filled in uniformly — 7.7c).
    //
    // Concurrent dials, abandoned dials and out-of-order channels are all
    // ordinary here: streams are bound by `link_id` (ENC 2.1b) and a stream
    // that never binds is discarded on its own without disturbing any other
    // link (2.1c).
    std::unique_ptr<PeerConnection> accept(int timeoutMs, HandshakeFailure *fail = nullptr);

    // ENC 2.1d — waits for ONE further stream whose `link_bind` names `link`'s
    // existing link_id, and adopts it.  Streams that bind other links are kept
    // for a later accept() rather than dropped.  Returns false on timeout.
    bool acceptInto(PeerConnection &link, int timeoutMs, HandshakeFailure *fail = nullptr);

    // ⚠ THE SAME THING, WITHOUT TOUCHING THE PeerConnection — WHICH IS WHAT AN
    // EMBEDDING WITH AN ACCEPT THREAD ACTUALLY NEEDS.
    //
    // `acceptInto()` above mutates the link, and in this application a link
    // belongs to the GUI thread the moment `accept()` hands it over: everything
    // PPCP is single-threaded from that point, deliberately.  So the accept
    // thread cannot call `acceptInto()` on a live phone's link, and the GUI
    // thread cannot call it either without driving this listener's sockets from
    // a second thread.  Both roads lead to a data race for want of a way to
    // collect the channel and adopt it somewhere else.
    //
    // This is that way: the accept thread waits for a stream binding `want` and
    // returns the CHANNEL, which is a self-contained object safe to hand across
    // threads; the owner of the link then calls `PeerConnection::adopt()` on
    // its own thread.  Null on timeout, which is the ordinary quiet case.
    std::unique_ptr<TransportChannel> acceptChannelFor(const LinkId &want, int timeoutMs,
                                                       HandshakeFailure *fail = nullptr);

    // 2.1c diagnostics.  Not an authentication outcome (see BindRejection), so
    // naming it breaches nothing and a test can assert on it.
    BindRejection lastBindRejection() const;
    int           bindRejectionCount() const;

private:
    // The one loop behind accept() and acceptInto().  `want` is null for the
    // first and names a link for the second; a stream that binds some other
    // link is kept for a later accept() either way.
    std::unique_ptr<PeerConnection> acceptImpl(int timeoutMs, HandshakeFailure *fail,
                                               const LinkId *want,
                                               std::unique_ptr<TransportChannel> *out_one);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ── The capability query behind RT-17 ───────────────────────────────────────
// RV 5.2b1 is a MUST: a peer offers the strongest option it has and MUST NOT
// propose a weaker mode than its platform supports, because the weakest end sets
// the outcome and no observer can tell a limitation from a choice.  RT-17 is a
// *review* row precisely because that is unobservable from outside, and it says
// what the reviewer is to check: that the offered set is derived from a platform
// capability query rather than from a constant, and that it is re-read whenever
// this path or the OpenSSL version is touched.
//
// So: these functions ask the linked OpenSSL what it can actually do, at
// runtime, and the answer is what gets offered.  A build against a newer OpenSSL
// that gains a suite begins offering it with no edit here — which is the point
// (5.4f: a platform that gains TLS 1.3 external PSK silently restores forward
// secrecy for an implementation that asks rather than assumes).
struct TlsCapabilities {
    std::vector<std::string> tls13Suites;  // IANA names, strongest first
    std::vector<std::string> tls12Suites;  // OpenSSL names, forward-secret first
    bool tls13Available = false;
    std::string libraryVersion;            // e.g. "OpenSSL 3.6.3"
};
TlsCapabilities queryTlsCapabilities();

}  // namespace Ppcp
