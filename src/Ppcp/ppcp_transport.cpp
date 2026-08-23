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

// ENC §2.1 / §3 / §5 are libppcp's, not ours.  The transport does not hand-roll
// a frame header, a CBOR map or an envelope: it calls the library that both
// ends of the protocol are built from, which is the whole reason ground rule 1
// makes libppcp the only shared artefact.  These four headers are all real in
// libppcp.a today (L1); nothing here reaches into planned.h.
#include <ppcp/cbor.h>
#include <ppcp/common.h>
#include <ppcp/envelope.h>
#include <ppcp/frame.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

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

// ── ASSOCIATING THE STREAMS OF ONE LINK — settled by erratum E1 ─────────────
//
// H1 shipped with a gap: CORE §3 requires two independently flow-controlled
// channels and ENC §2 numbers them, but nothing said how a listener decides
// that two arriving TCP connections are two channels of one peer, nor which is
// channel 0.  H1 grouped by the pairing the offered PSK identity resolved to
// and ordered by the dialler completing each handshake before dialling the
// next.  PinPointCapture took arrival order.  Both worked against themselves,
// which is the definition of an interoperability failure, and BOTH ARE NOW
// WITHDRAWN — ENC §2.1 / MSG §3.0, erratum E1 of 22 August 2026.
//
// What replaces them, and nothing else may be reintroduced beside it:
//
//   * the DIALLER mints a `link_id` — 16 CSPRNG bytes, fresh per link — and
//     sends `link_bind { link_id, channel }` as the FIRST frame on every stream
//     it opens, on every channel including 0, with the frame header carrying
//     that same channel (2.1a).
//   * the LISTENER associates streams into a link by `link_id` and takes each
//     stream's channel from the header (2.1b).  It MUST NOT infer either from
//     arrival order, from the transport address, or from a rendezvous identity.
//   * a stream whose first frame is not `link_bind`, whose `channel` disagrees
//     with its header, or whose `link_id` already holds that channel is closed
//     (2.1c); a link that has not bound channel 0 inside the listener's own
//     timeout is discarded with every stream it holds.
//
// Two consequences worth stating because H1's design forbade them.  Channels
// may be dialled CONCURRENTLY — the connector below does — and a third channel
// may be opened at ANY later point in the session with the same link_id (2.1d),
// which is what the preview Stream of CORE §5.11.2 wants.
//
// The pairing that RV 5.3b resolves is still surfaced to the embedding as
// ResolvedPairing::pairingId.  It is no longer consulted for association, and
// it must not be: `link_bind` is what makes this transport meet a foreign one,
// and the `direct` path has no PSK identity to group by at all.

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
#ifdef SO_NOSIGPIPE
    // A peer that walks away mid-session is ordinary here — ENC 2.1c makes an
    // abandoned dial an expected event, not an error — and writing to the
    // socket it left behind raises SIGPIPE and kills the process.  A transport
    // must not do that to its embedding, and it must not fix it by changing the
    // process's signal disposition either.  Where the platform has the
    // per-socket option (Apple, the BSDs) that is the right place for it; where
    // it does not, the listener never writes to a stream it is refusing — see
    // Impl::abandon() below.
    int nosig = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&nosig),
               sizeof nosig);
#endif
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


// ── One handshake step, non-blocking (E1 wants them concurrent) ─────────────
// H1 drove each handshake to completion before starting the next, because
// arrival order was load-bearing.  It no longer is, so the connector runs every
// channel's handshake at once and the listener runs every pending stream's at
// once — which means neither can be held up by one peer that stalls.
enum class Step { Done, Again, Failed };

Step stepHandshake(SSL *ssl, bool server, short &wantEvents)
{
    ERR_clear_error();
    const int r = server ? SSL_accept(ssl) : SSL_connect(ssl);
    if (r == 1) return Step::Done;
    const int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ)  { wantEvents = POLLIN;  return Step::Again; }
    if (e == SSL_ERROR_WANT_WRITE) { wantEvents = POLLOUT; return Step::Again; }
    return Step::Failed;
}

// ── link_bind, encoded and decoded (ENC §2.1, §3, §5; MSG §3.0) ─────────────
//
//   link_bind { link_id: bytes(16), channel: uint }
//
// Deterministic key order (ENC 4e) is the writer's job, not ours: the encoded
// keys sort `type` < `msg_id` < `channel` < `link_id` and
// ppcp_envelope_before() interleaves the reserved keys with the body's.  We
// declare the two body fields in that order and the writer refuses us if we
// are wrong, which is why this is not a comment that can rot.
//
// ⚠ `msg_id`.  MSG §3.0 does not say what msg_id a `link_bind` carries, and it
// matters here because the TRANSPORT mints this frame while the PEER ENGINE
// owns the sender's per-connection msg_id sequence (ENC 5c).  We send 1 and the
// engine will also start at 1 for `hello`.  That is safe — 3.0c says link_bind
// requires no response, so no `reply_to` ever names it, and the listener
// consumes the frame before the engine sees a byte of the stream — but it is a
// silence in the specification and is reported as such.

struct LinkBindBody {
    const LinkId *id;
    uint8_t       channel;
};

ppcp_result writeLinkBind(ppcp_cbor_writer *w, ppcp_envelope_writer *ew, void *ctx)
{
    const LinkBindBody *b = static_cast<const LinkBindBody *>(ctx);
    ppcp_result rc = ppcp_envelope_before(w, ew, "channel", 7);
    if (rc != PPCP_OK) return rc;
    rc = ppcp_cbor_write_text_z(w, "channel");
    if (rc != PPCP_OK) return rc;
    rc = ppcp_cbor_write_uint(w, b->channel);
    if (rc != PPCP_OK) return rc;

    rc = ppcp_envelope_before(w, ew, "link_id", 7);
    if (rc != PPCP_OK) return rc;
    rc = ppcp_cbor_write_text_z(w, "link_id");
    if (rc != PPCP_OK) return rc;
    return ppcp_cbor_write_bytes(w, b->id->data(), b->id->size());
}

// A link_bind frame is roughly forty bytes (ENC §2.1's own estimate).  This
// bound is a BUFFER SIZE, not a protocol threshold: a first frame that does not
// fit in it cannot be a link_bind, so the stream is refused under 2.1c without
// reading a megabyte to find that out.
constexpr std::size_t kLinkBindMaxFrame = 256;

bool encodeLinkBindImpl(const LinkId &id, Channel ch, std::vector<unsigned char> &out)
{
    ppcp_envelope e;
    if (ppcp_envelope_init(&e, "link_bind", 1) != PPCP_OK) return false;

    LinkBindBody body{ &id, static_cast<uint8_t>(ch) };
    unsigned char buf[kLinkBindMaxFrame];
    std::size_t written = 0;
    const ppcp_result rc = ppcp_message_encode(buf, sizeof buf, static_cast<uint8_t>(ch),
                                               &e, /*body_fields=*/2, writeLinkBind,
                                               &body, &written);
    if (rc != PPCP_OK) return false;
    out.assign(buf, buf + written);
    return true;
}

// Decodes one complete link_bind frame.  The channel this stream will carry is
// the frame header's (ENC 2b/2c), so the caller learns it from `out_channel`
// rather than telling us.
//
// Returns None with `out_consumed == 0` for "not a whole frame yet, poll again"
// and None with `out_consumed > 0` for a stream bound successfully.  Every
// other value is a 2.1c refusal.
BindRejection decodeLinkBind(const unsigned char *buf, std::size_t len,
                             LinkId &out_id, uint8_t &out_channel,
                             std::size_t &out_consumed)
{
    out_consumed = 0;
    ppcp_frame_header hdr{};
    const uint8_t *payload = nullptr;
    const ppcp_result fr = ppcp_frame_read(buf, len, &hdr, &payload, &out_consumed);
    if (fr == PPCP_ERR_TRUNCATED) { out_consumed = 0; return BindRejection::None; }
    if (fr != PPCP_OK) return BindRejection::Malformed;

    const ppcp_cbor_limits lim = ppcp_cbor_limits_for_channel(hdr.channel);

    // ENC §4 and §8 in one pass before any field is read (I13 skipping included).
    std::size_t consumed = 0;
    if (ppcp_cbor_validate(payload, hdr.payload_len, lim, &consumed) != PPCP_OK)
        return BindRejection::Malformed;

    ppcp_envelope env;
    uint32_t pairs = 0;
    if (ppcp_envelope_decode(payload, hdr.payload_len, lim, &env, &pairs) != PPCP_OK)
        return BindRejection::Malformed;
    if (std::strcmp(env.type, "link_bind") != 0) return BindRejection::NotLinkBind;

    ppcp_cbor_reader r;
    ppcp_cbor_reader_init(&r, payload, hdr.payload_len, lim);
    ppcp_cbor_item it;
    if (ppcp_cbor_read(&r, &it) != PPCP_OK || it.type != PPCP_CBOR_MAP)
        return BindRejection::Malformed;

    bool haveId = false, haveChannel = false;
    uint8_t bodyChannel = 0;
    for (uint32_t i = 0; i < it.count; ++i) {
        const char *k = nullptr;
        std::size_t klen = 0;
        if (ppcp_cbor_read_key(&r, &k, &klen) != PPCP_OK) return BindRejection::Malformed;
        if (ppcp_cbor_key_is(k, klen, "link_id")) {
            ppcp_cbor_item v;
            if (ppcp_cbor_read(&r, &v) != PPCP_OK) return BindRejection::Malformed;
            if (v.type != PPCP_CBOR_BYTES || v.len != out_id.size())
                return BindRejection::Malformed;   // 2.1a: sixteen bytes, exactly
            std::memcpy(out_id.data(), v.bytes, out_id.size());
            haveId = true;
        } else if (ppcp_cbor_key_is(k, klen, "channel")) {
            ppcp_cbor_item v;
            if (ppcp_cbor_read(&r, &v) != PPCP_OK) return BindRejection::Malformed;
            if (v.type != PPCP_CBOR_UINT || v.i < 0 || v.i > 255)
                return BindRejection::Malformed;
            bodyChannel = static_cast<uint8_t>(v.i);
            haveChannel = true;
        } else if (ppcp_cbor_skip(&r) != PPCP_OK) {
            return BindRejection::Malformed;       // I13: skip what we do not know
        }
    }
    if (!haveId || !haveChannel) return BindRejection::Malformed;

    // 2.1a/2.1c: the body `channel` MUST equal the header's.  ENC 2c already
    // requires every later frame on this stream to match it too.
    if (bodyChannel != hdr.channel) return BindRejection::ChannelMismatch;
    if (ppcp_channel_validate(hdr.channel) != PPCP_OK) return BindRejection::Malformed;

    out_channel = hdr.channel;
    return BindRejection::None;
}

#if defined(PP_PPCP_PLAINTEXT_HARNESS)
// ══════════════════════════════════════════════════════════════════════════
// ⚠⚠⚠  EVERY LINE UNDER THIS GUARD MOVES BYTES WITH NO TLS.  ⚠⚠⚠
//
// Compiled ONLY when the CMake option `PP_PPCP_PLAINTEXT_HARNESS` is ON, which
// is `OFF` by default and set by `src/Ppcp/tests` and by nothing else.  See the
// banner on `Listener::setPlaintextHarness()` in the header for why the
// conformance harness needs it and why RV erratum E4 permits it.

IoStatus plaintextRead(pp_socket_t s, void *buf, std::size_t len, std::size_t &got)
{
    got = 0;
    const auto n = ::recv(s, static_cast<char *>(buf), static_cast<int>(len), 0);
    if (n > 0) {
        got = static_cast<std::size_t>(n);
        return IoStatus::Ok;
    }
    if (n == 0) return IoStatus::Closed;
#ifdef _WIN32
    const int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return IoStatus::WouldBlock;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IoStatus::WouldBlock;
#endif
    return IoStatus::Error;
}

IoStatus plaintextWrite(pp_socket_t s, const void *buf, std::size_t len, std::size_t &put)
{
    put = 0;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const auto n = ::send(s, static_cast<const char *>(buf), static_cast<int>(len), flags);
    if (n > 0) {
        // A short send is success with a smaller `put` — CORE T2's backpressure
        // in the same shape SSL_MODE_ENABLE_PARTIAL_WRITE gives it above, so
        // the pump above this layer cannot tell the two transports apart.
        put = static_cast<std::size_t>(n);
        return IoStatus::Ok;
    }
    if (n == 0) return IoStatus::WouldBlock;
#ifdef _WIN32
    const int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return IoStatus::WouldBlock;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IoStatus::WouldBlock;
    if (errno == EPIPE || errno == ECONNRESET) return IoStatus::Closed;
#endif
    return IoStatus::Error;
}

// RV 5.4k asks for the achieved version and key-exchange mode to be made
// available to the application layer.  There is no handshake here, so the
// honest answer names itself: nothing downstream can mistake this for TLS, and
// a log line carrying it reads as the warning it is.
TlsOutcome plaintextOutcome()
{
    TlsOutcome o;
    o.version = "plaintext-harness";
    o.cipher = "none";
    o.kexMode = "none";
    o.forwardSecrecy = false;
    return o;
}
#endif  // PP_PPCP_PLAINTEXT_HARNESS

}  // namespace

// ── TransportChannel ────────────────────────────────────────────────────────

struct TransportChannel::Impl {
    pp_socket_t sock = PP_INVALID_SOCKET;
    SSL *ssl = nullptr;
    SSL_CTX *ctx = nullptr;   // owned only by the connector's per-channel context
    bool ownsCtx = false;
    std::unique_ptr<SslState> state;

    // ── F-H8-3 — BYTES THE LISTENER READ PAST `link_bind`, AND USED TO DROP ──
    //
    // ENC 2.1a makes `link_bind` the FIRST frame on a stream.  It does not make
    // it the ONLY thing in the first read, and a dialler that queues
    // `link_bind` and `hello` together — which `tools/ppcp-sim` does, and which
    // is the obvious thing for a peer engine with an outbound queue to do —
    // puts both in one TCP segment.  The bind loop reads whatever is available,
    // decodes the first frame and hands the CHANNEL over; everything after
    // `consumed` in that buffer used to go out of scope with it.  The `hello`
    // was simply gone, no error anywhere, and the link sat there until it timed
    // out.
    //
    // This is the defect `PPCP-CONF` §2c exists to find: PinPointStudio's own
    // `Connector` writes `link_bind` and then nothing until its engine is
    // pumped, so this repository talking to itself never produced the case.
    // The first counterpart that was not us produced it on the first row.
    std::vector<unsigned char> carry;
    std::size_t carryOff = 0;

    ~Impl() { shut(); }

    // ENC 2.1c — "a listener closes a stream whose first frame is not
    // link_bind".  It closes it; it does not answer it.  Suppressing the TLS
    // close_notify is therefore the right shape as well as the safe one: a
    // stream being refused is told nothing, which is also what RV 7.7c's habit
    // of mind asks for, and nothing is written to a socket whose peer may
    // already be gone.
    void abandon()
    {
        if (ssl) SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
        shut();
    }

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
    // ENC 2.1b — the link_id both ends bound by.  Set once, by whichever side
    // learned it: the dialler when it minted it, the listener when the first
    // link_bind on the link arrived.
    static void setLinkId(PeerConnection &p, const LinkId &id) { p.m_linkId = id; }
    // Drops a built channel without a close_notify — see Impl::abandon().
    static void abandon(TransportChannel &c) { if (c.m_impl) c.m_impl->abandon(); }
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

    // F-H8-3 — whatever the bind loop read past `link_bind` is the first thing
    // on this stream, ahead of anything still in the socket.  One read at a
    // time: the caller loops, and mixing the two sources in one call would
    // reorder the byte stream, which is the one thing CORE T1 does not allow.
    if (m_impl->carryOff < m_impl->carry.size()) {
        const std::size_t have = m_impl->carry.size() - m_impl->carryOff;
        const std::size_t take = (len < have) ? len : have;
        std::memcpy(buf, m_impl->carry.data() + m_impl->carryOff, take);
        m_impl->carryOff += take;
        if (m_impl->carryOff >= m_impl->carry.size()) {
            m_impl->carry.clear();
            m_impl->carryOff = 0;
        }
        got = take;
        return IoStatus::Ok;
    }
#if defined(PP_PPCP_PLAINTEXT_HARNESS)
    // ⚠ HARNESS ONLY — see Listener::setPlaintextHarness().  A channel with no
    // SSL* can only have come from a plaintext harness listener; in a shipping
    // build this branch does not exist and `m_impl->ssl` is never null.
    if (!m_impl->ssl) return plaintextRead(m_impl->sock, buf, len, got);
#endif
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
#if defined(PP_PPCP_PLAINTEXT_HARNESS)
    // ⚠ HARNESS ONLY — see read() above.
    if (!m_impl->ssl) return plaintextWrite(m_impl->sock, buf, len, put);
#endif
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

const std::string &TransportChannel::pairingId() const
{
    static const std::string kNone;
    return (m_impl && m_impl->state) ? m_impl->state->pairingId : kNone;
}

const std::string &PeerConnection::pairingId() const
{
    static const std::string kNone;
    const TransportChannel *c = channel(Channel::Control);
    return c ? c->pairingId() : kNone;
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

bool PeerConnection::adopt(std::unique_ptr<TransportChannel> c)
{
    if (!c) return false;
    // ENC 2.1c — a link_bind naming a link that already holds that channel is
    // refused.  Enforced here as well as in the listener so the dialler's own
    // side cannot build a link with two channel 1s either.
    if (channel(c->channel())) return false;
    m_channels.push_back(std::move(c));
    return true;
}

// ── The link token (ENC 2.1a) ───────────────────────────────────────────────

bool mintLinkId(LinkId &out)
{
    // 2.1a: sixteen bytes from a CSPRNG, fresh per link.  RAND_bytes is
    // OpenSSL's, which is already linked for the TLS half; a failure is
    // reported rather than papered over, because a predictable link_id would
    // let a stranger's stream join somebody else's link on a `direct`
    // transport that has no authentication to stop it (2.1f).
    return RAND_bytes(out.data(), static_cast<int>(out.size())) == 1;
}

bool encodeLinkBindFrame(const LinkId &id, Channel ch, std::vector<unsigned char> &out)
{
    return encodeLinkBindImpl(id, ch, out);
}

const char *describe(BindRejection r)
{
    switch (r) {
    case BindRejection::None:             return "bound";
    case BindRejection::NotLinkBind:      return "first frame was not link_bind (ENC 2.1c)";
    case BindRejection::ChannelMismatch:  return "link_bind channel disagrees with its header (ENC 2.1c)";
    case BindRejection::DuplicateChannel: return "link already holds that channel (ENC 2.1c)";
    case BindRejection::Malformed:        return "malformed first frame (ENC 4, 8)";
    case BindRejection::BindTimeout:      return "no link_bind inside the bind timeout (ENC 2.1c)";
    }
    return "unknown";
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

// ── Connector ─────────────────────────────────────────────────────────────────────

namespace {

// One channel being dialled.  The three phases are separated so all channels
// can be in flight at once: TCP connect, TLS handshake, link_bind write.
struct DialSlot {
    Channel channel = Channel::Control;
    std::unique_ptr<TransportChannel::Impl> impl;
    bool     handshaken = false;
    short    want = POLLOUT;
    std::vector<unsigned char> bind;   // the link_bind frame, not yet sent
    std::size_t sent = 0;
    TlsOutcome outcome;
};

// Dials one TCP connection, bounded by `deadline`.  Address fallback is here
// and nowhere else.
pp_socket_t dialTcp(const ConnectorConfig &cfg, double deadline)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    const std::string portText = std::to_string(cfg.port);
    if (getaddrinfo(cfg.host.c_str(), portText.c_str(), &hints, &res) != 0 || !res)
        return PP_INVALID_SOCKET;

    pp_socket_t s = PP_INVALID_SOCKET;
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
    return s;
}

// Writes the whole link_bind frame, bounded by `deadline`.  It is the first
// frame on the stream (2.1a), so nothing has been written before it and a short
// write can only mean the socket buffer, never interleaving.
bool sendLinkBind(DialSlot &slot, double deadline)
{
    while (slot.sent < slot.bind.size()) {
        ERR_clear_error();
        const int n = SSL_write(slot.impl->ssl, slot.bind.data() + slot.sent,
                                static_cast<int>(slot.bind.size() - slot.sent));
        if (n > 0) { slot.sent += static_cast<std::size_t>(n); continue; }
        const int e = SSL_get_error(slot.impl->ssl, n);
        if (e == SSL_ERROR_WANT_READ) {
            if (!waitFor(slot.impl->sock, true, deadline)) return false;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            if (!waitFor(slot.impl->sock, false, deadline)) return false;
        } else {
            return false;
        }
    }
    return true;
}

// Dials `channels` streams on one link_id, running every TLS handshake
// CONCURRENTLY, and writes link_bind first on each (ENC 2.1a/2.1d).  Returns
// the built channels, or an empty vector with `fail` filled in.
std::vector<std::unique_ptr<TransportChannel>> dialLink(const ConnectorConfig &cfg,
                                                        const LinkId &linkId,
                                                        const std::vector<Channel> &channels,
                                                        HandshakeFailure *fail,
                                                        double started)
{
    auto reportFail = [&](const char *msg, const SslState *st) {
        if (!fail) return;
        fail->message = msg;
        fail->elapsedMs = nowMs() - started;
        if (st) {
            fail->alert = st->alert;
            fail->alertWasSent = st->alertWasSent;
        }
    };

    const TlsCapabilities caps = queryTlsCapabilities();
    const double deadline = nowMs() + cfg.options.handshakeTimeoutMs;

    std::vector<DialSlot> slots;
    slots.reserve(channels.size());

    for (Channel ch : channels) {
        DialSlot slot;
        slot.channel = ch;
        slot.impl = std::make_unique<TransportChannel::Impl>();
        slot.impl->state = std::make_unique<SslState>();
        slot.impl->state->identity = cfg.identity;
        slot.impl->state->key = cfg.kTls;

        // ENC 2.1a — the frame is built BEFORE the socket, so a failure to encode
        // it (an impossibility that would still be a bug) never leaves a stream
        // open with no binding on it.
        if (!encodeLinkBindFrame(linkId, ch, slot.bind)) {
            reportFail("link_bind could not be encoded", nullptr);
            return {};
        }

        const pp_socket_t s = dialTcp(cfg, deadline);
        if (s == PP_INVALID_SOCKET) {
            // Not an authentication outcome, so it may say what it is — RV 7.7c
            // constrains the uniformity of a REJECTION, not of a dead socket.
            reportFail("no endpoint reachable", nullptr);
            return {};
        }
        slot.impl->sock = s;

        // RV 5.2g: we dialled, so we are the TLS client.
        slot.impl->ctx = makeContext(/*server=*/false, caps);
        slot.impl->ownsCtx = true;
        if (!slot.impl->ctx) {
            reportFail("TLS context unavailable", nullptr);
            return {};
        }
        slot.impl->ssl = SSL_new(slot.impl->ctx);
        if (!slot.impl->ssl) {
            reportFail("TLS context unavailable", nullptr);
            return {};
        }
        SSL_set_ex_data(slot.impl->ssl, sslStateIndex(), slot.impl->state.get());
        SSL_set_fd(slot.impl->ssl, static_cast<int>(s));
        SSL_set_connect_state(slot.impl->ssl);
        slots.push_back(std::move(slot));
    }

    // Every handshake at once.  H1 completed each before starting the next so
    // that arrival order at the listener would be the channel order; E1
    // withdrew that rule, and running them together is how this file stops
    // being able to depend on it again by accident.
    std::size_t remaining = slots.size();
    while (remaining > 0) {
        if (nowMs() >= deadline) {
            reportFail(kUniformHandshakeFailure, nullptr);
            return {};
        }
#ifdef _WIN32
        std::vector<WSAPOLLFD> fds;
#else
        std::vector<struct pollfd> fds;
#endif
        std::vector<std::size_t> idx;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].handshaken) continue;
            const Step st = stepHandshake(slots[i].impl->ssl, /*server=*/false, slots[i].want);
            if (st == Step::Done) {
                slots[i].handshaken = true;
                --remaining;
                continue;
            }
            if (st == Step::Failed) {
                reportFail(kUniformHandshakeFailure, slots[i].impl->state.get());
                if (cfg.log) cfg.log(std::string(kUniformHandshakeFailure));
                return {};
            }
#ifdef _WIN32
            WSAPOLLFD p{};
#else
            struct pollfd p{};
#endif
            p.fd = slots[i].impl->sock;
            p.events = slots[i].want;
            fds.push_back(p);
            idx.push_back(i);
        }
        if (remaining == 0) break;
        if (!fds.empty()) {
            const double remain = deadline - nowMs();
            if (remain <= 0) {
                reportFail(kUniformHandshakeFailure, nullptr);
                return {};
            }
            pp_poll(fds.data(), static_cast<unsigned long>(fds.size()),
                    static_cast<int>(remain));
        }
    }

    // Outcomes, then the binding frame.
    std::vector<std::unique_ptr<TransportChannel>> built;
    for (DialSlot &slot : slots) {
        slot.outcome = outcomeOf(slot.impl->ssl);

        // RV 5.2b: psk_ke MUST NOT be used.  OpenSSL's defaults already refuse
        // it, so reaching here with psk_ke means a defaults change or a
        // downgrade — and 5.2f says a failed handshake is a failed connection,
        // never a fallback, so we drop it rather than continue on weaker terms.
        if (slot.outcome.kexMode == "psk_ke") {
            reportFail(kUniformHandshakeFailure, slot.impl->state.get());
            if (cfg.log) cfg.log("PPCP TLS refused: psk_ke (RV 5.2b)");
            return {};
        }
        if (!sendLinkBind(slot, deadline)) {
            reportFail("link_bind could not be sent", slot.impl->state.get());
            return {};
        }
        if (cfg.log)
            cfg.log("PPCP channel " + std::to_string(static_cast<int>(slot.channel)) + " "
                    + slot.outcome.describe());
        built.push_back(detail::ChannelFactory::make(std::move(slot.impl), slot.channel,
                                                     slot.outcome));
    }
    return built;
}

}  // namespace

std::unique_ptr<PeerConnection> Connector::connect(const ConnectorConfig &cfg,
                                                   HandshakeFailure *fail)
{
    ensureSockets();
    const double started = nowMs();

    if (cfg.channels.empty()) {
        if (fail) fail->message = "no channels requested";
        return nullptr;
    }

    // ENC 2.1a — ONE link_id for the whole link, minted here, fresh, never
    // reused and never persisted (2.1f).
    LinkId linkId{};
    if (!mintLinkId(linkId)) {
        if (fail) fail->message = "CSPRNG unavailable for link_id";
        return nullptr;
    }

    std::vector<std::unique_ptr<TransportChannel>> built =
        dialLink(cfg, linkId, cfg.channels, fail, started);
    if (built.empty()) return nullptr;

    std::unique_ptr<PeerConnection> peer = detail::ChannelFactory::makePeer(std::move(built));
    detail::ChannelFactory::setLinkId(*peer, linkId);
    return peer;
}

bool Connector::connectAdditional(const ConnectorConfig &cfg, PeerConnection &link,
                                  Channel ch, HandshakeFailure *fail)
{
    ensureSockets();
    // ENC 2.1c — refuse before dialling: a second stream for a channel the link
    // already holds is exactly what the listener would close.
    if (link.channel(ch)) {
        if (fail) fail->message = "link already holds that channel";
        return false;
    }
    std::vector<std::unique_ptr<TransportChannel>> built =
        dialLink(cfg, link.linkId(), { ch }, fail, nowMs());
    if (built.size() != 1) return false;
    return link.adopt(std::move(built.front()));
}

// ── Listener ────────────────────────────────────────────────────────────────
//
// Streams arrive one at a time and are bound into links by `link_id` (ENC
// 2.1b).  Nothing here looks at arrival order, at the transport address, or at
// the resolved pairing — 2.1b forbids all three, and E1 exists because two
// implementations each chose one of them.
//
// The loop is stage-based rather than one-stream-at-a-time because 2.1c makes
// an unbound stream something that must NOT hold up anybody else: a dialler
// that completes its TLS handshake and then dies is expected, and the link it
// left half-built is discarded on its own timeout while every other link
// carries on binding.

struct Listener::Impl {
    pp_socket_t sock = PP_INVALID_SOCKET;
    std::uint16_t boundPort = 0;
    IdentityResolver resolver;
    Options options;
    LogFn log;
    int channelsPerPeer = 2;   // CORE T2's minimum; a third arrives under 2.1d
#if defined(PP_PPCP_PLAINTEXT_HARNESS)
    // ⚠ HARNESS ONLY.  See Listener::setPlaintextHarness() in the header.
    bool plaintext = false;
#else
    // In a shipping build the flag does not exist and every read of it below is
    // the compile-time constant `false`, so there is no plaintext code path to
    // reach even by mistake.
    static constexpr bool plaintext = false;
#endif
    SSL_CTX *ctx = nullptr;
    TlsCapabilities caps;
    Key dummyKey{};
    BindRejection lastRejection = BindRejection::None;
    int rejections = 0;

    // A stream that has been accepted but not yet bound: still handshaking, or
    // handshaken and waiting for its link_bind frame.
    struct Pending {
        std::unique_ptr<TransportChannel::Impl> impl;
        bool   handshaken = false;
        short  want = POLLIN;
        double deadline = 0.0;     // handshake, then bind
        std::vector<unsigned char> rx;
        double started = 0.0;
    };
    std::vector<std::unique_ptr<Pending>> pending;

    // A link being assembled.  `firstBindMs` starts at the FIRST link_bind for
    // this link_id, and 2.1c discards the link and every stream it holds if
    // channel 0 has not bound by `bindTimeoutMs` after it.
    struct Link {
        double firstBindMs = 0.0;
        std::vector<std::unique_ptr<TransportChannel>> channels;
    };
    std::map<LinkId, Link> links;

    ~Impl()
    {
        if (ctx) SSL_CTX_free(ctx);
        if (sock != PP_INVALID_SOCKET) pp_close_socket(sock);
    }

    void reject(BindRejection why)
    {
        lastRejection = why;
        ++rejections;
        if (log) log(std::string("PPCP stream closed: ") + describe(why));
    }

    bool linkHasChannel(const Link &l, uint8_t ch) const
    {
        for (const std::unique_ptr<TransportChannel> &c : l.channels)
            if (static_cast<uint8_t>(c->channel()) == ch) return true;
        return false;
    }

    // 2.1c — a link that has not bound channel 0 inside the timeout is
    // discarded with every stream it holds.  A link that HAS bound channel 0 is
    // left alone: 2.1d lets a bulk channel arrive at any later point.
    void expireLinks()
    {
        const double now = nowMs();
        for (auto it = links.begin(); it != links.end();) {
            const bool haveControl = linkHasChannel(it->second, PPCP_CHANNEL_CONTROL);
            if (!haveControl && now - it->second.firstBindMs > options.bindTimeoutMs) {
                reject(BindRejection::BindTimeout);
                for (std::unique_ptr<TransportChannel> &c : it->second.channels)
                    detail::ChannelFactory::abandon(*c);
                it = links.erase(it);
            } else {
                ++it;
            }
        }
    }

    void expirePending()
    {
        const double now = nowMs();
        for (auto it = pending.begin(); it != pending.end();) {
            if (now > (*it)->deadline) {
                // A stream that handshook and then owed us a link_bind is a
                // 2.1c refusal and is counted as one.  A stream that never got
                // through the handshake is a dead socket, not a bind failure,
                // and RV 7.7c has already had its say about naming those.
                if ((*it)->handshaken) reject(BindRejection::BindTimeout);
                else if (log) log("PPCP stream closed: handshake did not complete");
                if ((*it)->impl) (*it)->impl->abandon();
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
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
#if defined(PP_PPCP_PLAINTEXT_HARNESS)
void Listener::setPlaintextHarness(bool on) { m_impl->plaintext = on; }
#endif
std::uint16_t Listener::port() const { return m_impl->boundPort; }
BindRejection Listener::lastBindRejection() const { return m_impl->lastRejection; }
int Listener::bindRejectionCount() const { return m_impl->rejections; }

namespace {

// Why a socket call failed, in words, on both platforms.  `bind()` reports
// through WSAGetLastError() on Windows and leaves `errno` untouched, so the
// obvious std::strerror(errno) would print "Undefined error: 0" on exactly the
// machine where a port clash is most likely.
std::string sockErrText()
{
#ifdef _WIN32
    return "WSA error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

}  // namespace

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
    // ⚠ INADDR_ANY, AND IT MUST BE.  This was INADDR_LOOPBACK from H1 until
    // 23 Aug, which meant no phone ever reached this host: RV 4.3d has
    // `reachableEndpoints()` print the ROUTABLE addresses into the pairing
    // code (loopback deliberately last, "a code that leads with 127.0.0.1
    // works only on the machine that displayed it"), so the QR sent the
    // scanner to 192.168.x.y:7788 while the socket answered only on
    // 127.0.0.1:7788.  The dial was refused by the kernel — no firewall, no
    // handshake, nothing on any log.
    //
    // ⚠ AND NO TEST CAN CATCH THE REGRESSION.  Every peer in `ppcp-tests`,
    // the conformance harness and `tools/ppcp-sim` included, runs on this
    // machine and connects over loopback, so the suite passed throughout and
    // would pass again the moment somebody narrows this line.  What guards it
    // is the endpoint the code advertises, not a test.
    //
    // Listening wide is not a widening of TRUST: RV 5.2g makes this side the
    // TLS server, `m_impl->resolver` authenticates every dial against the
    // pairing ledger, and 5.3d gives an unresolvable identity the same path a
    // wrong key takes.  A stranger who reaches the port gets a failed
    // handshake, which is what the port is for.
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        // The reason, not just the fact.  On a wide bind EADDRINUSE is a real
        // possibility (another copy of the application, or anything else on
        // 7788) where on loopback it never was, and `start()` answers a bare
        // failure by silently retrying on an ephemeral port — so without the
        // errno there is nothing to tell "somebody has the port" apart from
        // "the network stack refused us".
        const std::string why = sockErrText();
        pp_close_socket(s);
        if (err) *err = "bind() failed: " + why;
        return false;
    }
    if (::listen(s, 8) != 0) {
        const std::string why = sockErrText();
        pp_close_socket(s);
        if (err) *err = "listen() failed: " + why;
        return false;
    }
    socklen_t len = sizeof addr;
    if (getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        m_impl->boundPort = ntohs(addr.sin_port);

    setNonBlocking(s);
    m_impl->sock = s;

    if (m_impl->plaintext) {
        // ⚠ HARNESS ONLY.  No context, no resolver, no handshake — and it is
        // said out loud on the log so a run that reached this by accident is
        // not a quiet one.
        if (m_impl->log)
            m_impl->log("PPCP listener is PLAINTEXT (conformance harness, CONF 2c / RV 2c1) "
                        "— no TLS, no authentication");
        return true;
    }

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

namespace {

// Reads whatever is available into `rx` without blocking.  Returns false when
// the stream died; "nothing to read right now" is a true with no growth.
// True once `rx` holds at least one whole frame — header plus its declared
// payload.  F-H8-3's other half: without this the read loop below kept going
// until the socket was empty, and a dialler that queued several frames at once
// could push `rx` past the buffer cap and have its stream refused as malformed
// when nothing was wrong with it.  One whole frame is all the bind decision
// needs; everything after it is carried, not re-read.
bool haveWholeFrame(const std::vector<unsigned char> &rx)
{
    ppcp_frame_header hdr{};
    const uint8_t *payload = nullptr;
    std::size_t consumed = 0;
    return ppcp_frame_read(rx.data(), rx.size(), &hdr, &payload, &consumed) == PPCP_OK
           && consumed > 0;
}

bool drainAvailable(TransportChannel::Impl &impl, std::vector<unsigned char> &rx)
{
    if (haveWholeFrame(rx)) return true;
    for (;;) {
        unsigned char buf[512];
#if defined(PP_PPCP_PLAINTEXT_HARNESS)
        if (!impl.ssl) {
            std::size_t got = 0;
            const IoStatus st = plaintextRead(impl.sock, buf, sizeof buf, got);
            if (st == IoStatus::Ok && got > 0) {
                rx.insert(rx.end(), buf, buf + got);
                if (haveWholeFrame(rx)) return true;
                if (rx.size() > kLinkBindMaxFrame * 4) return false;
                continue;
            }
            return st == IoStatus::WouldBlock;
        }
#endif
        ERR_clear_error();
        const int n = SSL_read(impl.ssl, buf, static_cast<int>(sizeof buf));
        if (n > 0) {
            rx.insert(rx.end(), buf, buf + n);
            if (haveWholeFrame(rx)) return true;
            if (rx.size() > kLinkBindMaxFrame * 4) return false;   // see kLinkBindMaxFrame
            continue;
        }
        const int e = SSL_get_error(impl.ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return true;
        return false;
    }
}

}  // namespace

// The one loop behind accept() and acceptInto().  `want` is null for accept()
// and names a link for acceptInto().
std::unique_ptr<PeerConnection> Listener::acceptImpl(int timeoutMs, HandshakeFailure *fail,
                                                     const LinkId *want,
                                                     std::unique_ptr<TransportChannel> *out_one)
{
    if (fail) *fail = HandshakeFailure{};
    if (m_impl->sock == PP_INVALID_SOCKET) {
        if (fail) fail->message = "not listening";
        return nullptr;
    }

    Impl *impl = m_impl.get();
    const double deadline = nowMs() + timeoutMs;

    while (nowMs() < deadline) {
        impl->expirePending();
        impl->expireLinks();

        // Drive every pending stream one step, then poll everything at once.
#ifdef _WIN32
        std::vector<WSAPOLLFD> fds;
#else
        std::vector<struct pollfd> fds;
#endif
        for (auto it = impl->pending.begin(); it != impl->pending.end();) {
            Impl::Pending &p = **it;
            bool drop = false;

            if (!p.handshaken) {
                const Step st = stepHandshake(p.impl->ssl, /*server=*/true, p.want);
                if (st == Step::Failed) {
                    // RV 5.3c / 7.7c.  ONE report, ONE log line, for an
                    // unresolvable identity and for a wrong key alike.  Nothing
                    // here consults p.impl->state->resolved, and nothing may be
                    // added that does: the whole obligation is that these two
                    // outcomes are the same outcome.
                    if (fail) {
                        fail->message = kUniformHandshakeFailure;
                        fail->alert = p.impl->state->alert;
                        fail->alertWasSent = p.impl->state->alertWasSent;
                        fail->elapsedMs = nowMs() - p.started;
                    }
                    if (impl->log) impl->log(std::string(kUniformHandshakeFailure));
                    impl->pending.erase(it);
                    return nullptr;
                }
                if (st == Step::Done) {
                    const TlsOutcome outcome = outcomeOf(p.impl->ssl);
                    if (outcome.kexMode == "psk_ke") {
                        // RV 5.2b — refuse, and refuse as a handshake failure (5.2f).
                        if (fail) {
                            fail->message = kUniformHandshakeFailure;
                            fail->elapsedMs = nowMs() - p.started;
                        }
                        if (impl->log) impl->log("PPCP TLS refused: psk_ke (RV 5.2b)");
                        impl->pending.erase(it);
                        return nullptr;
                    }
                    p.handshaken = true;
                    p.want = POLLIN;
                    // 2.1c's timeout starts here: the stream is up and owes us a
                    // link_bind.
                    p.deadline = nowMs() + impl->options.bindTimeoutMs;
                }
            }

            if (p.handshaken && !drainAvailable(*p.impl, p.rx)) {
                impl->reject(BindRejection::Malformed);
                drop = true;
            }

            if (!drop && p.handshaken && !p.rx.empty()) {
                LinkId id{};
                uint8_t ch = 0;
                std::size_t consumed = 0;
                const BindRejection why = decodeLinkBind(p.rx.data(), p.rx.size(),
                                                         id, ch, consumed);
                if (why != BindRejection::None) {
                    impl->reject(why);
                    drop = true;
                } else if (consumed > 0) {
                    // ENC 2.1b — bound.  Everything after this frame belongs to
                    // the peer engine; there is never a partial frame left over,
                    // because 2.1a makes link_bind the FIRST frame and the
                    // dialler writes nothing else before hello.
                    Impl::Link &link = impl->links[id];
                    if (link.channels.empty()) link.firstBindMs = nowMs();

                    if (impl->linkHasChannel(link, ch)) {
                        impl->reject(BindRejection::DuplicateChannel);
                        drop = true;
                    } else {
#if defined(PP_PPCP_PLAINTEXT_HARNESS)
                        const TlsOutcome outcome =
                            p.impl->ssl ? outcomeOf(p.impl->ssl) : plaintextOutcome();
#else
                        const TlsOutcome outcome = outcomeOf(p.impl->ssl);
#endif
                        if (impl->log)
                            impl->log("PPCP channel " + std::to_string(static_cast<int>(ch))
                                      + " bound " + outcome.describe());
                        // F-H8-3 — the rest of what was read belongs to the
                        // peer engine, and it travels with the channel.
                        if (consumed < p.rx.size())
                            p.impl->carry.assign(p.rx.begin()
                                                     + static_cast<std::ptrdiff_t>(consumed),
                                                 p.rx.end());
                        link.channels.push_back(detail::ChannelFactory::make(
                            std::move(p.impl), static_cast<Channel>(ch), outcome));
                        it = impl->pending.erase(it);

                        if (want && out_one && id == *want) {
                            *out_one = std::move(link.channels.back());
                            link.channels.pop_back();
                            if (link.channels.empty()) impl->links.erase(id);
                            return nullptr;   // the caller reads *out_one
                        }
                        if (!want && static_cast<int>(link.channels.size())
                                     >= impl->channelsPerPeer) {
                            std::vector<std::unique_ptr<TransportChannel>> done =
                                std::move(link.channels);
                            impl->links.erase(id);
                            std::unique_ptr<PeerConnection> peer =
                                detail::ChannelFactory::makePeer(std::move(done));
                            detail::ChannelFactory::setLinkId(*peer, id);
                            return peer;
                        }
                        continue;   // `it` was advanced by erase
                    }
                }
            }

            if (drop) {
                if (p.impl) p.impl->abandon();   // 2.1c: closed, not answered
                it = impl->pending.erase(it);
                continue;
            }

#ifdef _WIN32
            WSAPOLLFD pf{};
#else
            struct pollfd pf{};
#endif
            pf.fd = p.impl->sock;
            pf.events = p.want;
            fds.push_back(pf);
            ++it;
        }

        // The listen socket last, so a burst of new dials never starves the
        // streams already waiting to bind.
#ifdef _WIN32
        WSAPOLLFD lf{};
#else
        struct pollfd lf{};
#endif
        lf.fd = impl->sock;
        lf.events = POLLIN;
        fds.push_back(lf);

        double slice = deadline - nowMs();
        if (slice <= 0) break;
        // Bounded so expireLinks() and expirePending() run even while nothing
        // is readable — 2.1c's timeout is a deadline, not an event.
        if (slice > 50) slice = 50;
        const int pr = pp_poll(fds.data(), static_cast<unsigned long>(fds.size()),
                               static_cast<int>(slice));
        if (pr <= 0) continue;

        if (fds.back().revents & POLLIN) {
            pp_socket_t c = ::accept(impl->sock, nullptr, nullptr);
            if (c != PP_INVALID_SOCKET) {
                applyOptions(c, impl->options);
                setNonBlocking(c);

                auto ps = std::make_unique<Impl::Pending>();
                ps->started = nowMs();
                ps->deadline = nowMs() + impl->options.handshakeTimeoutMs;
                ps->impl = std::make_unique<TransportChannel::Impl>();
                ps->impl->sock = c;
#if defined(PP_PPCP_PLAINTEXT_HARNESS)
                if (impl->plaintext) {
                    // ⚠ HARNESS ONLY.  No SSL object is built, so there is
                    // nothing to hand a key to and no resolver is called.  The
                    // stream is "handshaken" the moment it is accepted and owes
                    // us a `link_bind` from that instant — ENC 2.1c's timeout is
                    // the SAME one, because binding is a protocol obligation and
                    // has nothing to do with how the bytes are protected.
                    ps->handshaken = true;
                    ps->want = POLLIN;
                    ps->deadline = nowMs() + impl->options.bindTimeoutMs;
                    impl->pending.push_back(std::move(ps));
                    continue;
                }
#endif
                ps->impl->ctx = impl->ctx;   // shared; the listener owns it
                ps->impl->ownsCtx = false;
                ps->impl->state = std::make_unique<SslState>();
                ps->impl->state->resolver = &impl->resolver;
                ps->impl->state->dummyKey = impl->dummyKey;
                ps->impl->ssl = SSL_new(impl->ctx);
                if (!ps->impl->ssl) {
                    if (fail) fail->message = "TLS context unavailable";
                    return nullptr;
                }
                SSL_set_ex_data(ps->impl->ssl, sslStateIndex(), ps->impl->state.get());
                SSL_set_fd(ps->impl->ssl, static_cast<int>(c));
                SSL_set_accept_state(ps->impl->ssl);
                impl->pending.push_back(std::move(ps));
            }
        }
    }
    return nullptr;   // timeout: no message (fail->message stays empty)
}

std::unique_ptr<PeerConnection> Listener::accept(int timeoutMs, HandshakeFailure *fail)
{
    return acceptImpl(timeoutMs, fail, nullptr, nullptr);
}

bool Listener::acceptInto(PeerConnection &link, int timeoutMs, HandshakeFailure *fail)
{
    // ENC 2.1d — one more stream, same link_id.  A stream that binds a
    // DIFFERENT link stays in this listener's link table and is handed out by a
    // later accept(); it is not dropped, because the dialler behind it did
    // nothing wrong.
    std::unique_ptr<TransportChannel> one;
    acceptImpl(timeoutMs, fail, &link.linkId(), &one);
    if (!one) return false;
    return link.adopt(std::move(one));
}

}  // namespace Ppcp
