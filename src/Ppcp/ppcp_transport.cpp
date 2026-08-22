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

#include "ppcp_transport.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using pp_socket_t = SOCKET;
#  define PP_INVALID_SOCKET INVALID_SOCKET
#  define pp_close_socket closesocket
#  define pp_poll WSAPoll
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using pp_socket_t = int;
#  define PP_INVALID_SOCKET (-1)
#  define pp_close_socket ::close
#  define pp_poll ::poll
#endif

namespace Ppcp {
namespace {

// ── ASSOCIATING THE CHANNELS OF ONE PEER — a gap in the specification ───────
//
// CORE §3 requires two independently flow-controlled channels and ENC §2 numbers
// them, but NEITHER SAYS how a listener decides that two arriving TCP
// connections are the two channels of one peer, nor which of them is channel 0.
// With one connection per channel (plan A6) that decision has to be made
// somewhere, and it is not in the specification.  Reported to the orchestrator
// as a defect; the resolution here is chosen to need no bytes on the wire, so
// that whatever clause lands later can standardise it without breaking us:
//
//   * grouping — by the pairing the offered PSK identity resolved to (RV 5.3b).
//     Every channel of one peer connection carries an identity derived from the
//     same K_id, so they resolve to the same pairing and nothing else does.
//   * ordering — the dialler completes each channel's handshake BEFORE dialling
//     the next, so arrival order at the listener is the channel order and no
//     race decides which stream is control.  One extra round trip, once, at
//     connection setup.  See Connector::connect.
//
// The alternative — a channel byte in a transport preamble — would have been a
// wire change invented by one implementation, which is exactly what a second
// implementation cannot guess.  ENC 2c gives the receiving PEER a cross-check
// (the channel in each frame header must match the stream it arrived on), so a
// mis-association is caught one layer up as `error` / `malformed` rather than
// being silently wrong.

// ── Sockets ─────────────────────────────────────────────────────────────────

struct SocketLibrary {
    SocketLibrary()
    {
#ifdef _WIN32
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#endif
    }
};
void ensureSockets()
{
    static SocketLibrary once;
    (void)once;
}

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void setNonBlocking(pp_socket_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

void applyOptions(pp_socket_t s, const Options &o)
{
    if (o.tcpNoDelay) {
        int on = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
    }
    if (o.sndBufBytes > 0) {
        int v = o.sndBufBytes;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&v), sizeof v);
    }
    if (o.rcvBufBytes > 0) {
        int v = o.rcvBufBytes;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&v), sizeof v);
    }
}

// Wait for the socket to become readable/writable, bounded by a deadline.
// Returns false on timeout or error.
bool waitFor(pp_socket_t s, bool forRead, double deadlineMs)
{
    const double remain = deadlineMs - nowMs();
    if (remain <= 0) return false;
#ifdef _WIN32
    WSAPOLLFD p{};
#else
    struct pollfd p{};
#endif
    p.fd = s;
    p.events = static_cast<short>(forRead ? POLLIN : POLLOUT);
    const int r = pp_poll(&p, 1, static_cast<int>(remain));
    return r > 0;
}

// ── The uniform rejection (RV 5.3c, 7.7c) ───────────────────────────────────
// One string for every authentication outcome.  A message that named the cause
// would be the distinguisher 7.7c forbids, and it costs nothing to withhold.
constexpr const char *kUniformHandshakeFailure = "PPCP TLS handshake failed";

// ── Per-SSL state ───────────────────────────────────────────────────────────

struct SslState {
    // Client side.
    PskIdentity identity;
    Key key{};

    // Server side.
    const IdentityResolver *resolver = nullptr;
    Key dummyKey{};             // RV 5.3d — the "proceed anyway" key
    std::string pairingId;      // set only on a successful resolution
    bool resolved = false;

    // Observed on the wire, for RT-11.
    int alert = -1;
    bool alertWasSent = false;
};

int sslStateIndex()
{
    static const int idx = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return idx;
}
SslState *stateOf(SSL *ssl)
{
    return static_cast<SslState *>(SSL_get_ex_data(ssl, sslStateIndex()));
}

void msgCallback(int write_p, int /*version*/, int content_type, const void *buf, size_t len,
                 SSL *ssl, void * /*arg*/)
{
    if (content_type != SSL3_RT_ALERT || len < 2) return;
    SslState *st = stateOf(ssl);
    if (!st) return;
    const unsigned char *a = static_cast<const unsigned char *>(buf);
    // Record the FIRST alert only: a warning close_notify arriving later must not
    // overwrite the fatal alert that RT-11 compares.
    if (st->alert < 0) {
        st->alert = a[1];
        st->alertWasSent = (write_p != 0);
    }
}

// ── The synthetic PSK session (RV §8) ───────────────────────────────────────
//
// This is the whole reason the transport uses OpenSSL directly.  An EXTERNAL PSK
// at TLS 1.3 is installed as a session object carrying the key, the cipher, and
// the cipher's hash — and RV §8 warns that getting the hash wrong fails the
// handshake with no useful diagnostic, indistinguishable from a key mismatch.
// TLS_AES_128_GCM_SHA256 (0x1301) is chosen because RV 5.2d requires it and
// because its hash IS SHA-256, which is the PSK's associated hash (5.2c).  The
// two constraints are the same constraint, and that is not a coincidence.
SSL_SESSION *makePskSession(SSL *ssl, const Key &key)
{
    static const unsigned char kTlsAes128GcmSha256[2] = {0x13, 0x01};
    const SSL_CIPHER *cipher = SSL_CIPHER_find(ssl, kTlsAes128GcmSha256);
    if (!cipher) return nullptr;

    SSL_SESSION *sess = SSL_SESSION_new();
    if (!sess) return nullptr;
    if (SSL_SESSION_set1_master_key(sess, key.data(), key.size()) != 1
        || SSL_SESSION_set_cipher(sess, cipher) != 1
        || SSL_SESSION_set_protocol_version(sess, TLS1_3_VERSION) != 1) {
        SSL_SESSION_free(sess);
        return nullptr;
    }
    return sess;
}

// TLS 1.3 client: SSL_CTX_set_psk_use_session_callback.
int pskUseSessionCb(SSL *ssl, const EVP_MD *md, const unsigned char **id, size_t *idlen,
                    SSL_SESSION **sess)
{
    SslState *st = stateOf(ssl);
    if (!st) return 0;

    // OpenSSL passes the hash it wants when it is resuming a hash-bound
    // exchange.  RV 5.2c pins ours to SHA-256, so decline politely rather than
    // hand back a session that cannot work — declining is `1` with no session.
    if (md && md != EVP_sha256()) {
        *sess = nullptr;
        return 1;
    }

    SSL_SESSION *s = makePskSession(ssl, st->key);
    if (!s) return 0;

    // RV 5.3f: the identity is the raw 17 octets and is handed over as such.
    // No transcoding, no UTF-8 validation, no truncation — the bytes go on the
    // wire exactly as libppcp produced them.
    *id = st->identity.data();
    *idlen = st->identity.size();
    *sess = s;
    return 1;
}

// TLS 1.3 server: SSL_CTX_set_psk_find_session_callback.
int pskFindSessionCb(SSL *ssl, const unsigned char *id, size_t idlen, SSL_SESSION **sess)
{
    SslState *st = stateOf(ssl);
    if (!st) return 0;

    ResolvedPairing found;
    // RV 5.3b: resolution is the embedding's — one HMAC per held pairing — and
    // the raw bytes go to it untouched (5.3f).
    const bool ok = st->resolver && (*st->resolver)(id, idlen, found);

    // RV 5.3c / 5.3d.  An unresolvable identity does NOT abort here.  It
    // proceeds with a dummy key so that both this case and a resolved identity
    // with the wrong key run to the same point — Finished verification — and
    // fail with the same alert after the same work.  Aborting early would be the
    // timing oracle 5.3d asks us to close and the content difference 5.3c
    // forbids outright.  Nothing below this line branches on `ok` in a way an
    // observer could see, and nothing logs it.
    const Key &key = ok ? found.kTls : st->dummyKey;
    if (ok) {
        st->resolved = true;
        st->pairingId = found.pairingId;
    }

    SSL_SESSION *s = makePskSession(ssl, key);
    if (!s) return 0;
    *sess = s;
    return 1;
}

// TLS 1.2 client (RFC 4279 interface).
//
// WHY THIS EXISTS ALONGSIDE THE 1.3 CALLBACK.  RV §8 warns that the hint-based
// interface does not reach TLS 1.3 external PSKs — true, and it is why the
// callbacks above exist.  It is not a reason to omit the 1.2 pair: RV 5.4b1
// records a measurement on the counterpart platform showing it reaches TLS 1.2
// `0x00A8` and nothing else, so a host that installed only the 1.3 callbacks
// could never complete a handshake with the device this protocol exists for.
// 5.2a permits 1.2 exactly here, and 5.2b1 requires us to offer 1.3 anyway.
//
// ⚠ RV 5.3f IS NOT FULLY ACHIEVABLE ON THIS PATH, and it is OpenSSL's interface
// that makes it so, not a choice of ours.  The identity is delivered here as a
// `char *` and OpenSSL takes its length with strlen — on the server side too.
// The 17-octet identity of 5.3a contains 16 CSPRNG bytes, so roughly one
// connection in sixteen carries an embedded 0x00 and would be silently
// truncated at TLS 1.2.  Reported as a specification defect.  We write the full
// 17 octets regardless: truncating deliberately would be the transcoding 5.3f
// forbids, and the failure it produces is a clean handshake failure, not a
// downgrade.
unsigned int psk12ClientCb(SSL *ssl, const char * /*hint*/, char *identity,
                           unsigned int max_identity_len, unsigned char *psk,
                           unsigned int max_psk_len)
{
    SslState *st = stateOf(ssl);
    if (!st) return 0;
    if (st->identity.size() + 1 > max_identity_len) return 0;
    if (st->key.size() > max_psk_len) return 0;
    std::memcpy(identity, st->identity.data(), st->identity.size());
    identity[st->identity.size()] = '\0';
    std::memcpy(psk, st->key.data(), st->key.size());
    return static_cast<unsigned int>(st->key.size());
}

// TLS 1.2 server (RFC 4279 interface).  Same uniform-failure discipline as the
// 1.3 path: unresolved means a dummy key, not an early abort (5.3c, 5.3d).
unsigned int psk12ServerCb(SSL *ssl, const char *identity, unsigned char *psk,
                           unsigned int max_psk_len)
{
    SslState *st = stateOf(ssl);
    if (!st) return 0;
    if (st->dummyKey.size() > max_psk_len) return 0;

    ResolvedPairing found;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(identity);
    const std::size_t len = identity ? std::strlen(identity) : 0;  // see the ⚠ above
    const bool ok = st->resolver && (*st->resolver)(bytes, len, found);
    if (ok) {
        st->resolved = true;
        st->pairingId = found.pairingId;
    }
    const Key &key = ok ? found.kTls : st->dummyKey;
    std::memcpy(psk, key.data(), key.size());
    return static_cast<unsigned int>(key.size());
}

// ── Context construction ────────────────────────────────────────────────────

std::string join(const std::vector<std::string> &v, char sep)
{
    std::string out;
    for (const std::string &s : v) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

SSL_CTX *makeContext(bool server, const TlsCapabilities &caps)
{
    SSL_CTX *ctx = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
    if (!ctx) return nullptr;

    // RV 5.2a: nothing below TLS 1.2 is ever negotiated.  No maximum is set:
    // 5.2b1 says a peer offers the strongest option it has, so if the linked
    // OpenSSL can reach 1.3 it must be allowed to.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // RV 5.2e: no certificate, no PKI, no CA — and a peer MUST NOT reject a
    // counterpart for presenting none.  With an external PSK both ends are
    // authenticated by holding K_tls, which is the whole point (5.2h property 1).
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    // The offered set, from the runtime capability query — never a constant.
    // RT-17 is a *review* row and this is the line it is about.  See
    // queryTlsCapabilities() for the query itself and RV 5.2b1 for why.
    if (!caps.tls13Suites.empty())
        SSL_CTX_set_ciphersuites(ctx, join(caps.tls13Suites, ':').c_str());
    if (!caps.tls12Suites.empty())
        SSL_CTX_set_cipher_list(ctx, join(caps.tls12Suites, ':').c_str());

    // RV 5.2b: at TLS 1.3 the key-exchange mode is psk_dhe_ke and psk_ke MUST
    // NOT be used.  OpenSSL refuses a PSK-only key exchange unless
    // SSL_OP_ALLOW_NO_DHE_KEX is set, so the rule is enforced by NOT setting it
    // — deliberately, and this comment is the record of that decision.  The
    // achieved mode is checked again after the handshake, because a defaults
    // change in a future OpenSSL must fail the connection, not weaken it.

    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_msg_callback(ctx, msgCallback);

    if (server) {
        // RV 5.2h property 3 / RT-14.  `psk_identity_hint` exists in the TLS 1.2
        // PSK model and is sent in the clear, so it MUST be empty — the rule
        // binds a server-sent field as hard as a client-sent one.  Setting it to
        // the empty string sends an empty hint rather than leaving OpenSSL to
        // decide between an empty hint and no ServerKeyExchange at all.
        SSL_CTX_use_psk_identity_hint(ctx, "");
        SSL_CTX_set_psk_find_session_callback(ctx, pskFindSessionCb);
        SSL_CTX_set_psk_server_callback(ctx, psk12ServerCb);

        // RV 7.5d: no application data on an early-data path.  Early data is
        // replayable by design, and a resumed connection that accepted `arm` as
        // early data would accept a replay of it.  Zero max_early_data makes the
        // server reject the extension outright; nothing here ever calls
        // SSL_read_early_data, so there is no path that could accept it even if
        // this line were removed.  Tickets themselves stay permitted (7.5d).
        SSL_CTX_set_max_early_data(ctx, 0);
    } else {
        SSL_CTX_set_psk_use_session_callback(ctx, pskUseSessionCb);
        SSL_CTX_set_psk_client_callback(ctx, psk12ClientCb);
        // The client half of 7.5d: SSL_write_early_data is never called.
    }
    return ctx;
}

// ── Reading the outcome back (RV 5.4k) ──────────────────────────────────────

TlsOutcome outcomeOf(SSL *ssl)
{
    TlsOutcome o;
    o.version = SSL_get_version(ssl);

    const SSL_CIPHER *c = SSL_get_current_cipher(ssl);
    if (c) {
        const char *std_name = SSL_CIPHER_standard_name(c);
        o.cipher = std_name ? std_name : SSL_CIPHER_get_name(c);
    }

    const int group = SSL_get_negotiated_group(ssl);
    if (group != 0) {
        // SSL_get0_group_name is the only accessor that names a group OpenSSL
        // has no NID for — 3.6 negotiates the hybrid X25519MLKEM768 by default
        // and OBJ_nid2sn returns nothing for it.  Which is itself the RT-17
        // argument in miniature: the platform gained something, the peer used
        // it, and no constant in this file had to be edited for that to happen.
#if OPENSSL_VERSION_NUMBER >= 0x30200000L
        const char *gn = SSL_get0_group_name(ssl);
#else
        const char *gn = OBJ_nid2sn(group);
#endif
        o.group = gn ? gn : "";
    }

    if (SSL_version(ssl) >= TLS1_3_VERSION) {
        // At 1.3 with an external PSK, a negotiated group means an ephemeral
        // (EC)DHE ran alongside the PSK: that is psk_dhe_ke.  No group means
        // psk_ke, which 5.2b forbids.
        o.kexMode = group != 0 ? "psk_dhe_ke" : "psk_ke";
        o.forwardSecrecy = (group != 0);
    } else {
        // At 1.2 the suite names the mode: TLS_ECDHE_PSK_* carries an ephemeral
        // exchange, plain TLS_PSK_* does not (5.2b, 5.2d).
        const bool ecdhe = o.cipher.rfind("TLS_ECDHE_PSK", 0) == 0;
        o.kexMode = ecdhe ? "ecdhe_psk" : "psk";
        o.forwardSecrecy = ecdhe;
    }
    return o;
}

// ── Driving a non-blocking handshake to completion ──────────────────────────

bool driveHandshake(SSL *ssl, pp_socket_t s, bool server, double deadlineMs)
{
    for (;;) {
        ERR_clear_error();
        const int r = server ? SSL_accept(ssl) : SSL_connect(ssl);
        if (r == 1) return true;
        const int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ) {
            if (!waitFor(s, true, deadlineMs)) return false;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            if (!waitFor(s, false, deadlineMs)) return false;
        } else {
            return false;
        }
    }
}

}  // namespace

// ── TransportChannel ────────────────────────────────────────────────────────

struct TransportChannel::Impl {
    pp_socket_t sock = PP_INVALID_SOCKET;
    SSL *ssl = nullptr;
    SSL_CTX *ctx = nullptr;   // owned only by the connector's per-channel context
    bool ownsCtx = false;
    std::unique_ptr<SslState> state;

    ~Impl() { shut(); }

    void shut()
    {
        if (ssl) {
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx && ownsCtx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        if (sock != PP_INVALID_SOCKET) {
            pp_close_socket(sock);
            sock = PP_INVALID_SOCKET;
        }
    }
};

namespace detail {
// The only thing allowed to build a channel or a peer connection: both are
// handed out complete, or not at all.  RV 5.2f — there is no partially-built,
// not-yet-encrypted object for anything to write to by accident.
struct ChannelFactory {
    static std::unique_ptr<TransportChannel> make(std::unique_ptr<TransportChannel::Impl> impl,
                                                  Channel ch, const TlsOutcome &o)
    {
        std::unique_ptr<TransportChannel> c(new TransportChannel());
        c->m_impl = std::move(impl);
        c->m_channel = ch;
        c->m_tls = o;
        return c;
    }
    static std::unique_ptr<PeerConnection> makePeer(
        std::vector<std::unique_ptr<TransportChannel>> chans)
    {
        std::unique_ptr<PeerConnection> p(new PeerConnection());
        p->m_channels = std::move(chans);
        return p;
    }
};
}  // namespace detail

TransportChannel::TransportChannel() = default;
TransportChannel::~TransportChannel() = default;

int TransportChannel::fd() const
{
    return m_impl ? static_cast<int>(m_impl->sock) : -1;
}

bool TransportChannel::isOpen() const
{
    return m_impl && m_impl->sock != PP_INVALID_SOCKET;
}

void TransportChannel::close()
{
    if (m_impl) m_impl->shut();
}

IoStatus TransportChannel::read(void *buf, std::size_t len, std::size_t &got)
{
    got = 0;
    if (!isOpen()) return IoStatus::Error;
    std::size_t n = 0;
    ERR_clear_error();
    const int r = SSL_read_ex(m_impl->ssl, buf, len, &n);
    if (r == 1) {
        got = n;
        return IoStatus::Ok;
    }
    const int e = SSL_get_error(m_impl->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return IoStatus::WouldBlock;
    if (e == SSL_ERROR_ZERO_RETURN) return IoStatus::Closed;
    if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) return IoStatus::Closed;
    return IoStatus::Error;
}

IoStatus TransportChannel::write(const void *buf, std::size_t len, std::size_t &put)
{
    put = 0;
    if (!isOpen()) return IoStatus::Error;
    if (len == 0) return IoStatus::Ok;
    std::size_t n = 0;
    ERR_clear_error();
    // SSL_MODE_ENABLE_PARTIAL_WRITE is set, so a short write is success with a
    // smaller `put` — that is the backpressure signal of CORE T2 in its useful
    // form.  A full window gives WouldBlock, and the caller's OTHER channel,
    // being a separate connection, is unaffected (T5).
    const int r = SSL_write_ex(m_impl->ssl, buf, len, &n);
    if (r == 1) {
        put = n;
        return IoStatus::Ok;
    }
    const int e = SSL_get_error(m_impl->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return IoStatus::WouldBlock;
    if (e == SSL_ERROR_ZERO_RETURN) return IoStatus::Closed;
    return IoStatus::Error;
}

// ── PeerConnection ──────────────────────────────────────────────────────────

PeerConnection::PeerConnection() = default;
PeerConnection::~PeerConnection() = default;

TransportChannel *PeerConnection::channel(Channel c) const
{
    for (const std::unique_ptr<TransportChannel> &ch : m_channels)
        if (ch->channel() == c) return ch.get();
    return nullptr;
}

const TlsOutcome &PeerConnection::tls() const
{
    static const TlsOutcome none;
    const TransportChannel *c = channel(Channel::Control);
    return c ? c->tls() : none;
}

std::vector<Channel> PeerConnection::channels() const
{
    std::vector<Channel> out;
    out.reserve(m_channels.size());
    for (const std::unique_ptr<TransportChannel> &c : m_channels) out.push_back(c->channel());
    return out;
}

void PeerConnection::close()
{
    for (std::unique_ptr<TransportChannel> &c : m_channels) c->close();
}

// ── TlsOutcome ──────────────────────────────────────────────────────────────

std::string TlsOutcome::describe() const
{
    // RV 5.4k wants this in the diagnostic export; RV 7.2b forbids that export
    // carrying a key, a secret or a payload.  Version, suite, mode and group are
    // none of those, and the identity is deliberately absent.
    std::string s = version + " " + cipher + " " + kexMode;
    if (!group.empty()) s += " (" + group + ")";
    s += forwardSecrecy ? " [forward secrecy]" : " [no forward secrecy]";
    return s;
}

// ── The capability query (RV 5.2b1, 5.4f — the subject of RT-17) ────────────

TlsCapabilities queryTlsCapabilities()
{
    TlsCapabilities caps;
    caps.libraryVersion = OpenSSL_version(OPENSSL_VERSION);

    SSL_CTX *probe = SSL_CTX_new(TLS_method());
    if (!probe) return caps;

    // TLS 1.3.  Only SHA-256 suites are candidates, and that is not a narrowing
    // of what we offer: RV 5.2c binds the PSK's hash to SHA-256, so a SHA-384
    // suite cannot complete a handshake with this PSK at all.  Offering it would
    // not be offering more, it would be offering something unusable.
    static const char *kTls13Candidates[] = {
        "TLS_AES_128_GCM_SHA256",          // 5.2d — required, the interoperable floor
        "TLS_CHACHA20_POLY1305_SHA256",    // offered if the platform has it (5.2b1)
    };
    for (const char *name : kTls13Candidates) {
        if (SSL_CTX_set_ciphersuites(probe, name) == 1) caps.tls13Suites.push_back(name);
    }

    // TLS 1.2 PSK suites, forward-secret ones first — 5.2b1 requires the
    // strongest the platform has to be offered, and 5.2d prefers ECDHE_PSK
    // (RFC 8442) over the plain floor (RFC 5487) wherever it is available.
    // Whether any of these exist is OpenSSL's answer, not ours: a build that
    // gains a suite begins offering it with no edit here.
    static const char *kTls12Candidates[] = {
        "ECDHE-PSK-CHACHA20-POLY1305",
        "ECDHE-PSK-AES128-GCM-SHA256",     // RFC 8442
        "ECDHE-PSK-AES128-CBC-SHA256",     // RFC 5489
        "PSK-AES128-GCM-SHA256",           // RFC 5487 0x00A8 — the floor of 5.2d
    };
    for (const char *name : kTls12Candidates) {
        if (SSL_CTX_set_cipher_list(probe, name) != 1) continue;
        STACK_OF(SSL_CIPHER) *list = SSL_CTX_get_ciphers(probe);
        // set_cipher_list succeeds if ANY token matched; confirm the suite is
        // really in the resulting list before claiming the platform has it.
        bool present = false;
        for (int i = 0; list && i < sk_SSL_CIPHER_num(list); ++i) {
            const SSL_CIPHER *c = sk_SSL_CIPHER_value(list, i);
            if (SSL_CIPHER_get_name(c) && std::strcmp(SSL_CIPHER_get_name(c), name) == 0) {
                present = true;
                break;
            }
        }
        if (present) caps.tls12Suites.push_back(name);
    }

    caps.tls13Available = !caps.tls13Suites.empty()
                          && SSL_CTX_set_max_proto_version(probe, TLS1_3_VERSION) == 1;
    SSL_CTX_free(probe);
    return caps;
}

// ── Connector ───────────────────────────────────────────────────────────────

std::unique_ptr<PeerConnection> Connector::connect(const ConnectorConfig &cfg,
                                                   HandshakeFailure *fail)
{
    ensureSockets();
    const double started = nowMs();
    auto reportFail = [&](const char *msg, const SslState *st) {
        if (!fail) return;
        fail->message = msg;
        fail->elapsedMs = nowMs() - started;
        if (st) {
            fail->alert = st->alert;
            fail->alertWasSent = st->alertWasSent;
        }
    };

    if (cfg.channels.empty()) {
        reportFail("no channels requested", nullptr);
        return nullptr;
    }

    const TlsCapabilities caps = queryTlsCapabilities();
    std::vector<std::unique_ptr<TransportChannel>> built;

    // One channel at a time, each handshake completed before the next is
    // dialled — see the association note at the top of this file.  It also means
    // a failure on channel 0 costs nothing on channel 1.
    for (Channel ch : cfg.channels) {
        auto impl = std::make_unique<TransportChannel::Impl>();
        impl->state = std::make_unique<SslState>();
        impl->state->identity = cfg.identity;
        impl->state->key = cfg.kTls;

        // Resolve and dial.
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        const std::string portText = std::to_string(cfg.port);
        if (getaddrinfo(cfg.host.c_str(), portText.c_str(), &hints, &res) != 0 || !res) {
            reportFail("address not resolved", nullptr);
            return nullptr;
        }

        pp_socket_t s = PP_INVALID_SOCKET;
        const double deadline = nowMs() + cfg.options.handshakeTimeoutMs;
        for (addrinfo *ai = res; ai; ai = ai->ai_next) {
            s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == PP_INVALID_SOCKET) continue;
            applyOptions(s, cfg.options);
            setNonBlocking(s);
            const int r = ::connect(s, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
            if (r == 0) break;
            if (waitFor(s, false, deadline)) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len);
                if (err == 0) break;
            }
            pp_close_socket(s);
            s = PP_INVALID_SOCKET;
        }
        freeaddrinfo(res);
        if (s == PP_INVALID_SOCKET) {
            // Every address failed or the deadline passed.  Not an
            // authentication outcome, so it may say what it is — RV 7.7c
            // constrains the uniformity of a REJECTION, not of a dead socket.
            reportFail("no endpoint reachable", nullptr);
            return nullptr;
        }
        impl->sock = s;

        // RV 5.2g: we dialled, so we are the TLS client.
        impl->ctx = makeContext(/*server=*/false, caps);
        impl->ownsCtx = true;
        if (!impl->ctx) {
            reportFail("TLS context unavailable", nullptr);
            return nullptr;
        }
        impl->ssl = SSL_new(impl->ctx);
        if (!impl->ssl) {
            reportFail("TLS context unavailable", nullptr);
            return nullptr;
        }
        SSL_set_ex_data(impl->ssl, sslStateIndex(), impl->state.get());
        SSL_set_fd(impl->ssl, static_cast<int>(s));
        SSL_set_connect_state(impl->ssl);

        if (!driveHandshake(impl->ssl, s, /*server=*/false, deadline)) {
            reportFail(kUniformHandshakeFailure, impl->state.get());
            if (cfg.log) cfg.log(std::string(kUniformHandshakeFailure));
            return nullptr;
        }

        const TlsOutcome outcome = outcomeOf(impl->ssl);

        // RV 5.2b: psk_ke MUST NOT be used.  OpenSSL's defaults already refuse
        // it, so reaching here with psk_ke means a defaults change or a
        // downgrade — and 5.2f says a failed handshake is a failed connection,
        // never a fallback, so we drop it rather than continue on weaker terms.
        if (outcome.kexMode == "psk_ke") {
            reportFail(kUniformHandshakeFailure, impl->state.get());
            if (cfg.log) cfg.log("PPCP TLS refused: psk_ke (RV 5.2b)");
            return nullptr;
        }

        if (cfg.log)
            cfg.log("PPCP channel " + std::to_string(static_cast<int>(ch)) + " "
                    + outcome.describe());

        built.push_back(detail::ChannelFactory::make(std::move(impl), ch, outcome));
    }

    return detail::ChannelFactory::makePeer(std::move(built));
}

// ── Listener ────────────────────────────────────────────────────────────────

struct Listener::Impl {
    pp_socket_t sock = PP_INVALID_SOCKET;
    std::uint16_t boundPort = 0;
    IdentityResolver resolver;
    Options options;
    LogFn log;
    int channelsPerPeer = 2;   // CORE T2's minimum; 3 where preview is carried
    SSL_CTX *ctx = nullptr;
    TlsCapabilities caps;
    Key dummyKey{};

    // Channels accepted so far that do not yet make a complete peer, keyed by
    // the pairing their identity resolved to.  See the association note above.
    //
    // `firstArrivalMs` is not bookkeeping.  A dialler that completes channel 0
    // and then dies leaves a half-built group behind, and the NEXT peer dialling
    // on the same pairing would have its channel 0 counted as that group's
    // channel 1 — control and bulk swapped, silently, on a reconnect.  A group
    // older than the handshake deadline is therefore abandoned rather than
    // joined.  Single use (RV 7.3a, `mu` defaulting to 1) bounds what can share
    // a pairing in the first place; this bounds what can share a stale one.
    struct PendingGroup {
        double firstArrivalMs = 0.0;
        std::vector<std::unique_ptr<TransportChannel>> channels;
    };
    std::map<std::string, PendingGroup> pending;

    ~Impl()
    {
        if (ctx) SSL_CTX_free(ctx);
        if (sock != PP_INVALID_SOCKET) pp_close_socket(sock);
    }
};

Listener::Listener() : m_impl(std::make_unique<Impl>())
{
    ensureSockets();
    // RV 5.3d's dummy key.  Fresh per listener, from the library CSPRNG, and
    // never equal to any real key — its only job is to make an unresolvable
    // identity take exactly the path a wrong key takes.
    RAND_bytes(m_impl->dummyKey.data(), static_cast<int>(m_impl->dummyKey.size()));
}

Listener::~Listener() = default;

void Listener::setIdentityResolver(IdentityResolver r) { m_impl->resolver = std::move(r); }
void Listener::setOptions(const Options &o) { m_impl->options = o; }
void Listener::setLog(LogFn f) { m_impl->log = std::move(f); }
void Listener::setChannelsPerPeer(int n) { m_impl->channelsPerPeer = n; }
std::uint16_t Listener::port() const { return m_impl->boundPort; }

bool Listener::listen(std::uint16_t port, std::string *err)
{
    ensureSockets();
    pp_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == PP_INVALID_SOCKET) {
        if (err) *err = "socket() failed";
        return false;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
    applyOptions(s, m_impl->options);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        pp_close_socket(s);
        if (err) *err = "bind() failed";
        return false;
    }
    if (::listen(s, 8) != 0) {
        pp_close_socket(s);
        if (err) *err = "listen() failed";
        return false;
    }
    socklen_t len = sizeof addr;
    if (getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        m_impl->boundPort = ntohs(addr.sin_port);

    setNonBlocking(s);
    m_impl->sock = s;

    m_impl->caps = queryTlsCapabilities();
    // RV 5.2g: we listened, so we are the TLS server.
    m_impl->ctx = makeContext(/*server=*/true, m_impl->caps);
    if (!m_impl->ctx) {
        if (err) *err = "TLS context unavailable";
        return false;
    }
    return true;
}

void Listener::stop()
{
    if (m_impl->sock != PP_INVALID_SOCKET) {
        pp_close_socket(m_impl->sock);
        m_impl->sock = PP_INVALID_SOCKET;
    }
}

std::unique_ptr<PeerConnection> Listener::accept(int timeoutMs, HandshakeFailure *fail)
{
    if (fail) *fail = HandshakeFailure{};
    if (m_impl->sock == PP_INVALID_SOCKET) {
        if (fail) fail->message = "not listening";
        return nullptr;
    }

    const double deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline) {
        if (!waitFor(m_impl->sock, true, deadline)) return nullptr;  // timeout: no message

        pp_socket_t c = ::accept(m_impl->sock, nullptr, nullptr);
        if (c == PP_INVALID_SOCKET) continue;

        const double started = nowMs();
        applyOptions(c, m_impl->options);
        setNonBlocking(c);

        auto impl = std::make_unique<TransportChannel::Impl>();
        impl->sock = c;
        impl->ctx = m_impl->ctx;   // shared; the listener owns it
        impl->ownsCtx = false;
        impl->state = std::make_unique<SslState>();
        impl->state->resolver = &m_impl->resolver;
        impl->state->dummyKey = m_impl->dummyKey;

        impl->ssl = SSL_new(m_impl->ctx);
        if (!impl->ssl) {
            if (fail) fail->message = "TLS context unavailable";
            return nullptr;
        }
        SSL_set_ex_data(impl->ssl, sslStateIndex(), impl->state.get());
        SSL_set_fd(impl->ssl, static_cast<int>(c));
        SSL_set_accept_state(impl->ssl);

        const double hsDeadline =
            std::min(deadline, nowMs() + m_impl->options.handshakeTimeoutMs);
        const bool ok = driveHandshake(impl->ssl, c, /*server=*/true, hsDeadline);

        if (!ok) {
            // RV 5.3c / 7.7c.  ONE report, ONE log line, for an unresolvable
            // identity and for a wrong key alike.  Nothing here consults
            // impl->state->resolved, and nothing may be added that does: the
            // whole obligation is that these two outcomes are the same outcome.
            if (fail) {
                fail->message = kUniformHandshakeFailure;
                fail->alert = impl->state->alert;
                fail->alertWasSent = impl->state->alertWasSent;
                fail->elapsedMs = nowMs() - started;
            }
            if (m_impl->log) m_impl->log(std::string(kUniformHandshakeFailure));
            return nullptr;
        }

        const TlsOutcome outcome = outcomeOf(impl->ssl);
        if (outcome.kexMode == "psk_ke") {
            // RV 5.2b — refuse, and refuse as a handshake failure (5.2f).
            if (fail) {
                fail->message = kUniformHandshakeFailure;
                fail->elapsedMs = nowMs() - started;
            }
            if (m_impl->log) m_impl->log("PPCP TLS refused: psk_ke (RV 5.2b)");
            return nullptr;
        }

        const std::string pairing = impl->state->pairingId;
        Impl::PendingGroup &group = m_impl->pending[pairing];
        if (!group.channels.empty()
            && nowMs() - group.firstArrivalMs > m_impl->options.handshakeTimeoutMs) {
            group.channels.clear();   // an abandoned dial; see PendingGroup
        }
        if (group.channels.empty()) group.firstArrivalMs = nowMs();

        // Arrival order is channel order — the dialler serialises its handshakes
        // so that it is.  ENC 2a numbering: 0 control, then bulk, then preview.
        const Channel ch = static_cast<Channel>(group.channels.size());

        if (m_impl->log)
            m_impl->log("PPCP channel " + std::to_string(static_cast<int>(ch)) + " "
                        + outcome.describe());

        group.channels.push_back(detail::ChannelFactory::make(std::move(impl), ch, outcome));

        if (static_cast<int>(group.channels.size()) >= m_impl->channelsPerPeer) {
            std::vector<std::unique_ptr<TransportChannel>> complete = std::move(group.channels);
            m_impl->pending.erase(pairing);
            return detail::ChannelFactory::makePeer(std::move(complete));
        }
    }
    return nullptr;
}

}  // namespace Ppcp
