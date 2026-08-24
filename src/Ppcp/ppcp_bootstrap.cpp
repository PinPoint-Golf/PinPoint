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

#include "ppcp_bootstrap.h"

#include <cstring>
#include <chrono>

#include <openssl/evp.h>
#include <openssl/err.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Ppcp {
namespace {

void wipe(void *p, std::size_t n)
{
    // OPENSSL_cleanse is the one this application already links and it is not
    // elided by the optimiser, which `memset` on a dying object is.
    if (p && n) OPENSSL_cleanse(p, n);
}

std::uint64_t steadyMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// 11.3e — "an attempt that has not reached 11.5f within 30 seconds is aborted,
// and one awaiting a user's affirmation is aborted after 60."  The window's own
// bound is 3.7b's 180 seconds and binds regardless; that one is the acceptor's
// to enforce and this peer respects it by simply not being slower.
constexpr std::uint64_t kExchangeTimeoutMs = 30000;
constexpr std::uint64_t kAffirmTimeoutMs   = 60000;
constexpr int           kConnectTimeoutMs  = 5000;

}  // namespace

// ── 3.3f / 3.3g ────────────────────────────────────────────────────────────

RvInstanceKind classifyInstance(const RvAdvertisement &ad)
{
    // ⛔ 3.3g, AND IT IS CHECKED FIRST BECAUSE IT IS THE HOSTILE CASE.  "A
    // receiver that sees both `bs` and `rid` on one instance treats the
    // instance as malformed and ignores it."  A bootstrap instance carries no
    // `rn` and no `rid` — it names no pairing, because it holds none — so an
    // instance claiming both is either broken or trying to be two things at
    // once.  Ordering this test after the `bs` test would classify it as a
    // bootstrap window and dial it.
    if (ad.hasBs && (ad.hasRid || ad.hasRn)) return RvInstanceKind::Malformed;

    if (ad.hasBs) {
        // 3.3f — `bs` is `1`.  Nothing else is a window; a `bs=0` is not "a
        // closed window", it is a record this reader does not understand, and
        // 3.3d's posture for that is to ignore rather than guess.
        if (ad.bs != "1") return RvInstanceKind::Malformed;
        return RvInstanceKind::Bootstrap;
    }

    // 3.3g again, from the other side: `dl` is scoped to a bootstrap instance
    // and 3.3b bars a human-readable string from a reconnection one.  A record
    // carrying `dl` without `bs` is putting an operator-set name on the network
    // outside the window that bounds the trade.
    if (ad.hasDl) return RvInstanceKind::Malformed;

    if (ad.hasRn && ad.hasRid) return RvInstanceKind::Reconnection;
    return RvInstanceKind::Malformed;
}

std::string sanitiseLabel(const std::string &dl)
{
    // 4.4d, reached through 3.3g: "escaped for display, truncated to at most
    // 64 bytes" — 3.3f is stricter and says 32 for `dl`, so 32 is the bound.
    //
    // ⚠ TRUNCATE ON THE BYTES, RENDER ONLY WHAT IS SAFE.  A `dl` is a CBOR
    // tstr and may be any UTF-8, and this string lands in a QML `Text` where a
    // right-to-left override or a zero-width joiner is a display attack rather
    // than a character.  So rather than escaping cleverly, anything outside
    // printable ASCII becomes a visible placeholder: the operator sees that
    // the label contained something they cannot read, which is the honest
    // rendering of a string a stranger chose.
    std::string out;
    out.reserve(dl.size());
    std::size_t taken = 0;
    for (unsigned char c : dl) {
        if (taken >= 32) break;
        taken++;
        if (c >= 0x20 && c < 0x7f) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('.');       // one dot per byte; no length oracle
        }
    }
    return out;
}

bool bootstrapCandidateFrom(const RvAdvertisement &ad, int wireMajor,
                            BootstrapCandidate *out)
{
    if (!out) return false;
    if (classifyInstance(ad) != RvInstanceKind::Bootstrap) return false;

    // 3.3f — "`pv` as above, AND FILTERED BEFORE CONNECTING FOR THE SAME
    // REASON".  A window we cannot speak PPCP to after pairing is a window
    // worth nothing, and the filter is cheap where the dial is an attempt.
    if (!pvAcceptsMajor(ad.pv, wireMajor)) return false;

    // 3.7f — the SRV record names the endpoint the bootstrap connection is
    // made to, and it MUST NOT be the peer's PPCP listener.  We cannot verify
    // that from here (we do not know their listener), but a port of zero is a
    // record that names no endpoint at all.
    if (ad.port == 0 || ad.host.empty()) return false;

    out->instanceName = ad.instanceName;
    out->host = ad.host;
    out->port = ad.port;
    out->pv = ad.pv;
    out->role = ad.role;
    out->hasLabel = ad.hasDl;
    out->label = ad.hasDl ? sanitiseLabel(ad.dl) : std::string();
    return true;
}

// ── 11.7 — the digits ──────────────────────────────────────────────────────

std::string formatSas(std::uint32_t sas)
{
    // 11.7a — "exactly six decimal digits with leading zeros.  `000042` is a
    // valid string and MUST be shown as six characters."  The engine has
    // already applied the modulo; this only renders.
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06u", static_cast<unsigned>(sas % 1000000u));
    // 11.7d — "both peers group them identically — `313 164`".  The grouping
    // is part of the MUST-adjacent SHOULD because two peers grouping
    // differently make an operator compare two shapes rather than two numbers.
    return std::string(buf, 3) + " " + std::string(buf + 3, 3);
}

// ── 11.9 — what the user is told ───────────────────────────────────────────

AbortAdvice adviseOnAbort(ppcp_bs_reason rc, bool declinedHere)
{
    AbortAdvice a;
    a.rc = rc;
    switch (rc) {
    case PPCP_BS_RC_UNSUPPORTED_VERSION:
        // 11.4e — "reports to its USER that the counterpart requires a newer
        // version of the application, not a generic failure.  The operator is
        // standing there and can act on it."
        a.message = "That device needs a newer version of its app before it "
                    "can pair this way.";
        // 11.9d1 — on the FIRST abort, not the second.  A second attempt is
        // guaranteed to fail identically.
        a.offerPairingCode = true;
        a.mayOfferRetry = false;
        break;

    case PPCP_BS_RC_REJECTED:
        // ⛔ 11.9c's SECOND CASE, AND THE INDISTINGUISHABILITY IS DELIBERATE.
        // 11.4f: "a user's refusal and a failed confirmation MAC are reported
        // with the SAME code, `rejected`, and are indistinguishable to the
        // counterpart."  So an incoming `rejected` may be the other operator
        // declining — benign — or that peer's MAC failing to verify, which is
        // not.  We cannot tell, and 11.9c binds the dangerous half, so the
        // message is the conservative one and no retry is offered.
        //
        // ⚠ THIS IS A READING, AND IT IS STATED RATHER THAN ASSUMED.  11.9c
        // names "a mismatch or a MAC failure"; 11.4f makes those two
        // observationally identical to a peer's ordinary refusal.  Treating
        // the union as dangerous is the only choice that does not require
        // distinguishing what 11.4f exists to make indistinguishable.
        a.message = declinedHere
            ? "The numbers did not match — do not try again until you know why."
            : "The other device did not confirm. If the numbers did not match "
              "there, do not try again until you know why.";
        a.mayOfferRetry = false;
        break;

    case PPCP_BS_RC_COMMITMENT_MISMATCH:
        // 11.5d — the revealed key did not hash to the commitment.  On an
        // honest link this cannot happen: it means the bytes were rewritten
        // between the two frames.
        a.message = "The other device's answer did not match what it promised. "
                    "Something is interfering with the connection — do not try "
                    "again until you know why.";
        a.mayOfferRetry = false;
        break;

    case PPCP_BS_RC_INVALID_KEY:
        // ⛔ TRAP 7 / 11.6b — "a rejected key is an ATTACK SIGNAL", and the
        // clause's own words: MUST NOT treat as a transport error, MUST NOT
        // retry.  The message says so and the flag enforces it.
        a.message = "The other device offered a key this pairing cannot use. "
                    "That is not a network fault — do not try again until you "
                    "know why.";
        a.mayOfferRetry = false;
        break;

    case PPCP_BS_RC_MALFORMED:
        // 11.4c/11.4c1 — an unknown frame, a wrong-typed field, a repeated
        // frame, an unrecognised map key.  Either an implementation disagrees
        // or someone is rewriting frames; neither is fixed by trying again.
        a.message = "The other device sent something this version does not "
                    "understand.";
        a.mayOfferRetry = false;
        break;

    case PPCP_BS_RC_WINDOW_CLOSED:
        // 11.3c/11.3d — no window open, or one attempt already running there.
        // "Far more likely a peer racing a window that has just closed than an
        // attacker, and it is owed a diagnostic its user can act on."
        a.message = "That device is not offering to pair right now. Ask it to "
                    "start pairing again.";
        a.mayOfferRetry = true;
        break;

    case PPCP_BS_RC_TIMEOUT:
    default:
        // 11.9c's own exemption: "a timeout or a closed connection carries no
        // such implication and MAY be reported as the ordinary failure it is."
        a.message = "The pairing timed out.";
        a.mayOfferRetry = true;
        break;
    }
    return a;
}

// ── CA1 / §11.11 — OpenSSL X25519 ──────────────────────────────────────────

namespace {

class OpenSslAgreement final : public BootstrapKeyAgreement {
public:
    ~OpenSslAgreement() override { wipe(); }

    bool generate(std::uint8_t pk[PPCP_RV_BS_KEY_BYTES]) override
    {
        // 11.5a — FRESH, from a CSPRNG, for this attempt only.  Refusing a
        // second call is what makes "never reuses" a property of the object
        // rather than a rule the caller keeps: a reused ephemeral is not
        // ephemeral, and one leaked private key would then impersonate this
        // peer at every future first pairing it ever attempts.
        if (m_key != nullptr) return false;

        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        if (ctx == nullptr) return false;
        EVP_PKEY *k = nullptr;
        const bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
                        EVP_PKEY_keygen(ctx, &k) == 1;
        EVP_PKEY_CTX_free(ctx);
        if (!ok || k == nullptr) {
            if (k) EVP_PKEY_free(k);
            ERR_clear_error();
            return false;
        }
        std::size_t n = PPCP_RV_BS_KEY_BYTES;
        if (EVP_PKEY_get_raw_public_key(k, pk, &n) != 1 ||
            n != PPCP_RV_BS_KEY_BYTES) {
            EVP_PKEY_free(k);
            ERR_clear_error();
            return false;
        }
        m_key = k;
        return true;
    }

    bool agree(const std::uint8_t peer_pk[PPCP_RV_BS_KEY_BYTES],
               std::uint8_t z[PPCP_RV_BS_KEY_BYTES]) override
    {
        // ⛔⛔ TRAP 7 LIVES ON THIS FUNCTION'S RETURN VALUE.
        //
        // E36 measured it on this exact library: "OpenSSL 3.6.3 FAILS
        // EVP_PKEY_derive for each of the five standard small-order
        // u-coordinates" rather than returning an all-zero Z.  So the
        // zero-check every implementer writes from 11.6b's original wording
        // CAN NEVER FIRE HERE, and the branch that DOES fire is this one.  If
        // it returned some generic error the caller mapped onto the transport
        // path, an attack signal would be reported as a network fault and
        // retried — and 11.11f says a boundary that did that "would make 11.6b
        // unimplementable on the far side of it".
        //
        // ⚠ AND EVERY FAILURE EXIT HERE IS THE SAME FAILURE.  A malformed peer
        // key, a context allocation failure and a rejected small-order point
        // all return false, because the caller must not be able to tell them
        // apart and act differently: 11.11f has it treat "a reported failure
        // and an all-zero Z identically".
        if (m_key == nullptr) return false;
        std::memset(z, 0, PPCP_RV_BS_KEY_BYTES);

        EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(
            EVP_PKEY_X25519, nullptr, peer_pk, PPCP_RV_BS_KEY_BYTES);
        if (peer == nullptr) { ERR_clear_error(); return false; }

        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(m_key, nullptr);
        if (ctx == nullptr) {
            EVP_PKEY_free(peer);
            ERR_clear_error();
            return false;
        }
        std::size_t n = PPCP_RV_BS_KEY_BYTES;
        const bool ok = EVP_PKEY_derive_init(ctx) == 1 &&
                        EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
                        EVP_PKEY_derive(ctx, z, &n) == 1 &&
                        n == PPCP_RV_BS_KEY_BYTES;
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peer);
        if (!ok) {
            wipeZ(z);
            ERR_clear_error();
            return false;
        }
        // 11.6b's literal check, kept even though E36 measured that it cannot
        // fire on this library.  It costs nothing, it is what the clause says,
        // and "something else may yet return zeros" (11.11f) is the case it is
        // here for — a different OpenSSL build, or this file moving to another
        // provider.  It is NOT the defence; the `ok` branch above is.
        std::uint8_t acc = 0;
        for (std::size_t i = 0; i < PPCP_RV_BS_KEY_BYTES; ++i) acc |= z[i];
        if (acc == 0) { wipeZ(z); return false; }
        return true;
    }

    void wipe() override
    {
        // 11.11h — the private scalar is erased when the handshake ends,
        // whether it succeeded or failed.  It lives only inside the EVP_PKEY,
        // which OpenSSL cleanses on free.
        if (m_key) { EVP_PKEY_free(m_key); m_key = nullptr; }
    }

    std::string describe() const override { return "OpenSSL EVP_PKEY_X25519"; }

private:
    static void wipeZ(std::uint8_t z[PPCP_RV_BS_KEY_BYTES])
    {
        Ppcp::wipe(z, PPCP_RV_BS_KEY_BYTES);
    }
    EVP_PKEY *m_key = nullptr;
};

}  // namespace

std::unique_ptr<BootstrapKeyAgreement> makeOpenSslKeyAgreement()
{
    return std::unique_ptr<BootstrapKeyAgreement>(new OpenSslAgreement());
}

// ── 11.3b — the byte stream ────────────────────────────────────────────────

namespace {

#if !defined(_WIN32)
class TcpStream final : public BootstrapStream {
public:
    ~TcpStream() override { close(); }

    bool connect(const std::string &host, std::uint16_t port, int timeoutMs,
                 std::string *err) override
    {
        close();
        char svc[16];
        std::snprintf(svc, sizeof svc, "%u", static_cast<unsigned>(port));
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        if (::getaddrinfo(host.c_str(), svc, &hints, &res) != 0 || res == nullptr) {
            if (err) *err = "cannot resolve " + host;
            return false;
        }
        for (addrinfo *a = res; a != nullptr; a = a->ai_next) {
            const int s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) continue;
            // ⛔ SET BEFORE `connect`, NOT AFTER, AND CERTAINLY NOT LAZILY.
            // See the block below for what this defends; what forces it HERE
            // is that on macOS `setsockopt(SO_NOSIGPIPE)` returns -1 once the
            // peer has closed, so a socket that is set up late is not
            // protected at exactly the moment it needs to be.  Measured, on
            // this machine, by the suite dying instead of failing.
#if defined(SO_NOSIGPIPE)
            { int on = 1; ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on); }
#endif
            if (::connect(s, a->ai_addr, a->ai_addrlen) == 0) {
                m_fd = s;
                break;
            }
            ::close(s);
        }
        ::freeaddrinfo(res);
        if (m_fd < 0) {
            if (err) *err = "cannot reach the bootstrap endpoint";
            return false;
        }
        // Five frames, none larger than PPCP_BS_MAX_FRAME, each a round trip a
        // person is waiting on.  Nagle would add 40ms to every one of them.
        int one = 1;
        ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        // ⛔ AND SIGPIPE WOULD KILL THE APPLICATION, NOT THE ATTEMPT.  A
        // counterpart that vanishes mid-exchange — 11.9a's "a closed
        // connection", which is an ORDINARY outcome here and one of the cases
        // 11.9c says may be reported as the ordinary failure it is — leaves us
        // writing `bs_abort` into a dead socket.  The default disposition of
        // SIGPIPE terminates the process, so a phone walking out of range
        // would take PinPointStudio with it.  Caught by the suite on its first
        // run, as a SIGPIPE rather than as a failed assertion.
        //
        // macOS has no MSG_NOSIGNAL; SO_NOSIGPIPE is the per-socket form and
        // is set rather than handling the signal process-wide, because this
        // subsystem has no business changing a host application's signal
        // disposition (ground rule 8's spirit, one layer out).  It is set on
        // the socket ABOVE, before connect — see the note there.
        const int fl = ::fcntl(m_fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(m_fd, F_SETFL, fl | O_NONBLOCK);
        (void)timeoutMs;
        return true;
    }

    long read(void *buf, std::size_t len) override
    {
        if (m_fd < 0) return -1;
        const ssize_t n = ::recv(m_fd, buf, len, 0);
        if (n > 0) return static_cast<long>(n);
        if (n == 0) return -1;                            // the peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 0;
        return -1;
    }

    bool writeAll(const void *buf, std::size_t len) override
    {
        if (m_fd < 0) return false;
        const std::uint8_t *p = static_cast<const std::uint8_t *>(buf);
        std::size_t left = len;
        while (left > 0) {
#if defined(MSG_NOSIGNAL)
            const ssize_t n = ::send(m_fd, p, left, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(m_fd, p, left, 0);
#endif
            if (n > 0) { p += n; left -= static_cast<std::size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                pollfd pf{m_fd, POLLOUT, 0};
                if (::poll(&pf, 1, 5000) <= 0) return false;
                continue;
            }
            return false;
        }
        return true;
    }

    void close() override
    {
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    }

    bool isOpen() const override { return m_fd >= 0; }
    int  fd() const override { return m_fd; }

private:
    int m_fd = -1;
};
#endif

}  // namespace

std::unique_ptr<BootstrapStream> makeTcpStream()
{
#if defined(_WIN32)
    // 3.6b — silent absence.  The pairing-code path is unaffected.
    return nullptr;
#else
    return std::unique_ptr<BootstrapStream>(new TcpStream());
#endif
}

// ── The attempt ────────────────────────────────────────────────────────────

GuidedAttempt::GuidedAttempt(std::unique_ptr<BootstrapStream> stream,
                             std::unique_ptr<BootstrapKeyAgreement> agree,
                             Clock clock)
    : m_stream(std::move(stream)),
      m_agree(std::move(agree)),
      m_clock(clock ? std::move(clock) : Clock(steadyMs))
{
}

GuidedAttempt::~GuidedAttempt()
{
    // 11.6f — erased when the handshake ends, WHETHER IT SUCCEEDED OR FAILED,
    // and on every exit path including this one.  ppcp_bs_engine_wipe() is
    // idempotent and covers the paths the engine's own transitions do not:
    // a dropped connection, a closed window, a user walking away, and an
    // object going out of scope mid-exchange.
    if (m_engineLive) ppcp_bs_engine_wipe(&m_engine);
    if (m_agree) m_agree->wipe();
    wipe(&m_pairing, sizeof m_pairing);
}

bool GuidedAttempt::begin(const BootstrapCandidate &c, std::string *err)
{
    if (m_phase != GuidedPhase::Idle) {
        if (err) *err = "this attempt has already run";
        return false;
    }
    m_candidate = c;
    m_phase = GuidedPhase::Dialling;
    m_startedMs = m_clock();

    if (!m_stream || !m_agree) {
        if (err) *err = "guided pairing is not available in this build";
        fail(PPCP_BS_RC_TIMEOUT, false);
        return false;
    }

    // 11.5a — the keypair is drawn BEFORE the connection, so a peer that
    // cannot draw one never opens a socket and never occupies the counterpart's
    // single-attempt window (3.7b, 11.3d).
    std::uint8_t pk[PPCP_RV_BS_KEY_BYTES];
    if (!m_agree->generate(pk)) {
        if (err) *err = "the key agreement is unavailable";
        fail(PPCP_BS_RC_INVALID_KEY, false);
        return false;
    }

    // 3.7f — the SRV endpoint of the bootstrap instance, which is NOT the
    // peer's PPCP listener: "a bootstrap connection and a PPCP link are
    // different protocols with different first frames, and separating them at
    // the port is what keeps either from having to guess which it received."
    std::string cerr;
    if (!m_stream->connect(c.host, c.port, kConnectTimeoutMs, &cerr)) {
        wipe(pk, sizeof pk);
        if (err) *err = cerr;
        // A dial that never connected is an ordinary network failure and 11.9c
        // lets it read as one.
        fail(PPCP_BS_RC_TIMEOUT, false);
        return false;
    }

    if (ppcp_bs_engine_init(&m_engine, PPCP_BS_ROLE_INITIATOR,
                            PPCP_BS_VERSION, pk) != PPCP_OK) {
        wipe(pk, sizeof pk);
        if (err) *err = "libppcp refused the engine";
        fail(PPCP_BS_RC_MALFORMED, false);
        return false;
    }
    wipe(pk, sizeof pk);
    m_engineLive = true;

    // ⛔ 11.5b — `bs_offer` carries `v` and `ct` ONLY, and does NOT carry
    // `pk_i`.  libppcp builds the frame; the reason this call is here, before
    // anything has been read, is 11.5d's other half: `pk_i` goes out in
    // `bs_reveal` and ONLY after `bs_accept` has arrived.  The relay's
    // `--probe order-initiator` measures exactly that by never replying.
    ppcp_bs_step step{};
    if (ppcp_bs_engine_start(&m_engine, &step) != PPCP_OK) {
        if (err) *err = "libppcp refused to start the exchange";
        fail(PPCP_BS_RC_MALFORMED, false);
        return false;
    }
    m_phase = GuidedPhase::Exchanging;
    if (!applyStep(step)) {
        if (err) *err = m_advice.message;
        return false;
    }
    return true;
}

bool GuidedAttempt::applyStep(const ppcp_bs_step &step)
{
    if (step.has_out && step.out_len > 0) {
        if (!m_stream->writeAll(step.out, step.out_len)) {
            // The bytes did not reach the peer.  That is a transport failure
            // and it is the ONE place in this file where that phrase is
            // correct — it is not trap 7's case, because no key agreement was
            // involved.
            fail(PPCP_BS_RC_TIMEOUT, false);
            return false;
        }
    }

    switch (step.event) {
    case PPCP_BS_EV_NEED_SECRET:
        return supplySecret(step.peer_pk);

    case PPCP_BS_EV_COMPARE:
        // 11.5e — both peers derive, display the digits, and each waits for
        // ITS OWN user.  Nothing is sent from here: `bs_confirm` goes out from
        // affirm(), which only a person reaches.
        m_phase = GuidedPhase::Comparing;
        m_comparingSinceMs = m_clock();
        return true;

    case PPCP_BS_EV_PAIRED:
        // 11.5g — affirmed at this end AND the counterpart's MAC verified.
        m_phase = GuidedPhase::Paired;
        if (ppcp_bs_engine_take_pairing(&m_engine, &m_pairing) == PPCP_OK)
            m_havePairing = true;
        // 11.5h — "the bootstrap connection is closed once both MACs have
        // verified.  It is not reused, not upgraded in place, and not held
        // open."  The peers reconnect under §5, on a fresh connection, in
        // whichever direction 11.2b puts them — which for this pair is the
        // capture peer dialling this host, because 3.5d leaves it no choice.
        m_stream->close();
        m_agree->wipe();
        return true;

    case PPCP_BS_EV_ABORTED:
        fail(step.rc, m_declinedHere);
        return false;

    case PPCP_BS_EV_NONE:
    default:
        if (step.close) {
            fail(PPCP_BS_RC_TIMEOUT, false);
            return false;
        }
        return true;
    }
}

bool GuidedAttempt::supplySecret(const std::uint8_t peer_pk[PPCP_RV_BS_KEY_BYTES])
{
    std::uint8_t z[PPCP_RV_BS_KEY_BYTES];
    const bool agreed = m_agree->agree(peer_pk, z);

    if (!agreed) {
        // ⛔⛔ TRAP 7, IN ONE BRANCH, AND IT IS THE WHOLE POINT OF THIS
        // FUNCTION EXISTING SEPARATELY.  11.6b: "a peer aborts with
        // `invalid_key` and derives nothing where the key agreement FAILS, or
        // produces an all-zero Z ... A peer MUST NOT treat such a failure as a
        // transport error and MUST NOT retry it."
        //
        // There is no `else` here that falls through to the network path, and
        // there is no loop anywhere in this file that could bring us back.  A
        // rejected key is an attack signal; a retry loop around it eats 3.7b's
        // single-attempt bound, which is what §11.8's whole argument rests on.
        wipe(z, sizeof z);
        ppcp_bs_step step{};
        ppcp_bs_engine_abort(&m_engine, PPCP_BS_RC_INVALID_KEY, &step);
        if (step.has_out && step.out_len > 0)
            m_stream->writeAll(step.out, step.out_len);
        fail(PPCP_BS_RC_INVALID_KEY, false);
        return false;
    }

    ppcp_bs_step step{};
    const ppcp_result r = ppcp_bs_engine_supply_secret(&m_engine, z, &step);
    // 11.11h — Z is erased by whichever component held it, the moment it has
    // been handed over.  libppcp has copied what it needs.
    wipe(z, sizeof z);
    if (r != PPCP_OK) {
        // The library's own zero-check fired, or the engine was in no state to
        // take it.  Either way this is 11.6b's case and not the network's.
        fail(PPCP_BS_RC_INVALID_KEY, false);
        return false;
    }
    return applyStep(step);
}

bool GuidedAttempt::poll()
{
    if (terminal()) return false;
    const std::uint64_t now = m_clock();

    // 11.3e — the two timers, and they are the embedding's obligation because
    // libppcp owns no clock (ground rule 8).
    if (m_phase == GuidedPhase::Comparing || m_phase == GuidedPhase::Confirming) {
        if (m_comparingSinceMs != 0 && now - m_comparingSinceMs > kAffirmTimeoutMs) {
            abort(PPCP_BS_RC_TIMEOUT);
            return false;
        }
    } else if (now - m_startedMs > kExchangeTimeoutMs) {
        abort(PPCP_BS_RC_TIMEOUT);
        return false;
    }

    if (!m_stream || !m_stream->isOpen()) {
        if (m_phase != GuidedPhase::Paired) {
            // 11.9a — "a closed connection" ends the attempt and leaves no
            // pairing.  11.9c lets it be reported as the ordinary failure it
            // looks like.
            fail(PPCP_BS_RC_TIMEOUT, false);
            return false;
        }
        return false;
    }

    std::uint8_t buf[PPCP_BS_MAX_FRAME * 2];
    const long n = m_stream->read(buf, sizeof buf);
    if (n < 0) {
        fail(PPCP_BS_RC_TIMEOUT, false);
        return false;
    }
    if (n > 0) m_in.insert(m_in.end(), buf, buf + n);
    wipe(buf, sizeof buf);

    // Feed whole frames only.  PPCP_ERR_TRUNCATED leaves the engine untouched,
    // which is why the carry-over buffer is safe to re-present unchanged.
    while (!m_in.empty() && !terminal()) {
        ppcp_bs_step step{};
        std::size_t consumed = 0;
        const ppcp_result r =
            ppcp_bs_engine_recv(&m_engine, m_in.data(), m_in.size(), &consumed, &step);
        if (r == PPCP_ERR_TRUNCATED) break;      // read more and retry
        if (consumed > 0)
            m_in.erase(m_in.begin(), m_in.begin() + static_cast<long>(consumed));
        if (r != PPCP_OK) {
            // The engine refused the frame outright without producing a step —
            // 11.4c's posture, and it does not attempt recovery.
            fail(PPCP_BS_RC_MALFORMED, false);
            return false;
        }
        if (!applyStep(step)) return false;
        if (step.close) break;
    }
    return !terminal();
}

bool GuidedAttempt::sas(std::uint32_t *out) const
{
    if (!out) return false;
    // ⛔ 11.7e — nothing before 11.5d has completed, and 11.7f — nothing after
    // the attempt ends.  `Confirming` is inside the window: this user has
    // affirmed and the screen still shows what they affirmed while the
    // counterpart's MAC is owed.  `Paired` and `Failed` are outside it, and
    // the engine has wiped the value in both.
    if (m_phase != GuidedPhase::Comparing && m_phase != GuidedPhase::Confirming)
        return false;
    if (!m_engineLive) return false;
    return ppcp_bs_engine_sas(&m_engine, out) == PPCP_OK;
}

std::string GuidedAttempt::sasDigits() const
{
    std::uint32_t v = 0;
    if (!sas(&v)) return std::string();
    return formatSas(v);
}

bool GuidedAttempt::affirm()
{
    // ⛔ 11.7c AND TRAP 8.  This is called from ONE place: a control a person
    // touched.  There is no path from a received frame to here, and there must
    // never be one — "a peer MUST NOT treat the arrival of the counterpart's
    // `bs_confirm` as standing in for its own user's", and a peer that
    // compared the digits itself would pass every static test in the document
    // and authenticate nothing.
    if (m_phase != GuidedPhase::Comparing) return false;
    ppcp_bs_step step{};
    if (ppcp_bs_engine_affirm(&m_engine, &step) != PPCP_OK) {
        fail(PPCP_BS_RC_MALFORMED, false);
        return false;
    }
    m_phase = GuidedPhase::Confirming;
    return applyStep(step);
}

void GuidedAttempt::decline()
{
    // 11.7d's prompt asks whether the numbers MATCH, so this is the "they do
    // not" answer — which is 11.9c's first case and the one signal this whole
    // path produces that an attack is under way.
    m_declinedHere = true;
    abort(PPCP_BS_RC_REJECTED);
}

void GuidedAttempt::abort(ppcp_bs_reason rc)
{
    if (terminal()) return;
    if (m_engineLive) {
        ppcp_bs_step step{};
        if (ppcp_bs_engine_abort(&m_engine, rc, &step) == PPCP_OK &&
            step.has_out && step.out_len > 0 && m_stream && m_stream->isOpen()) {
            // 11.4g — `bs_abort` carries `rc` and nothing else.  libppcp built
            // the frame, so there is no message, no diagnostic string and no
            // peer name in it, and no call site here that could add one.
            m_stream->writeAll(step.out, step.out_len);
        }
    }
    fail(rc, m_declinedHere);
}

void GuidedAttempt::fail(ppcp_bs_reason rc, bool declinedHere)
{
    if (m_phase == GuidedPhase::Paired) return;
    m_phase = GuidedPhase::Failed;
    m_advice = adviseOnAbort(rc, declinedHere);
    // 11.9a — the attempt ends and leaves NO pairing at either peer.  11.6f as
    // amended by E51 has the erasure include `PRK`, `K_tls`, `K_id` and `sid`
    // where they were computed, "and the digits with them" (11.7f): a peer
    // computes the whole chain the moment it holds `Z`, before either user has
    // affirmed anything, so on every abort path there is key material here for
    // a pairing that does not exist and never will.  Computing is not holding.
    if (m_engineLive) ppcp_bs_engine_wipe(&m_engine);
    wipe(&m_pairing, sizeof m_pairing);
    m_havePairing = false;
    if (m_agree) m_agree->wipe();
    if (m_stream) m_stream->close();
    // 11.10c — everything received on a bootstrap connection is spent when the
    // connection closes.
    if (!m_in.empty()) {
        wipe(m_in.data(), m_in.size());
        m_in.clear();
    }
}

bool GuidedAttempt::takePairing(ppcp_bs_pairing *out)
{
    if (!out) return false;
    if (m_phase != GuidedPhase::Paired || !m_havePairing) return false;
    std::memcpy(out, &m_pairing, sizeof(*out));
    // 11.10c — nothing from a bootstrap connection is persisted except the
    // `PRK` of 11.6e, and only after 11.5g.  It has now moved on to the
    // rendezvous ledger, so this copy goes.
    wipe(&m_pairing, sizeof m_pairing);
    m_havePairing = false;
    return true;
}

// ── 11.3d1 / TRAP 3 — the one door ─────────────────────────────────────────

GuidedPairing::GuidedPairing()
    : m_sf(makeTcpStream),
      m_af(makeOpenSslKeyAgreement),
      m_clock(steadyMs)
{
}

GuidedPairing::~GuidedPairing() = default;

void GuidedPairing::setFactories(StreamFactory sf, AgreeFactory af)
{
    if (sf) m_sf = std::move(sf);
    if (af) m_af = std::move(af);
}

void GuidedPairing::setClock(GuidedAttempt::Clock c)
{
    if (c) m_clock = std::move(c);
}

bool GuidedPairing::noteAdvertisement(const RvAdvertisement &ad, int wireMajor)
{
    BootstrapCandidate c;
    if (!bootstrapCandidateFrom(ad, wireMajor, &c)) {
        // 3.3g — a malformed instance is IGNORED, and a reconnection instance
        // is not this class's business.  Neither is an error (3.6a): it is
        // simply not a window we would dial.  Drop any candidate we were
        // holding under that name, because a window that stopped being one has
        // closed as far as we are concerned.
        return dropInstance(ad.instanceName);
    }
    for (auto &held : m_candidates) {
        if (held.instanceName == c.instanceName) {
            const bool changed = held.host != c.host || held.port != c.port ||
                                 held.label != c.label;
            held = c;
            return changed;
        }
    }
    m_candidates.push_back(c);
    return true;
}

bool GuidedPairing::dropInstance(const std::string &instanceName)
{
    for (auto it = m_candidates.begin(); it != m_candidates.end(); ++it) {
        if (it->instanceName == instanceName) {
            m_candidates.erase(it);
            return true;
        }
    }
    return false;
}

void GuidedPairing::clearCandidates() { m_candidates.clear(); }

std::vector<BootstrapCandidate> GuidedPairing::candidates() const
{
    return m_candidates;
}

bool GuidedPairing::begin(const std::string &instanceName, std::string *whyNot)
{
    // ⛔⛔ TRAP 3 (11.3d1).  THIS IS THE ONLY FUNCTION IN THIS FILE THAT OPENS
    // A SOCKET, IT TAKES ONE NAME, AND IT REFUSES WHILE AN ATTEMPT IS LIVE.
    //
    // "An INITIATOR runs at most one bootstrap attempt at a time, and MUST NOT
    // display digits for more than one attempt.  Where several bootstrap
    // instances are discovered, the user selects one BEFORE the attempt
    // begins."  An attacker advertising N windows otherwise gets N independent
    // blind draws against one honest confirmation — and worse, the operator
    // does the selecting: shown a list of numbers one of which matches the
    // phone in their hand, they tap the match and read it as success.
    //
    // 11.3d1 is explicit that the natural implementation is the one that
    // breaks this, so the refusal is here rather than in the caller.
    if (attemptInProgress()) {
        if (whyNot) *whyNot = "a pairing attempt is already running";
        return false;
    }
    // A terminal attempt still sitting here is a caller that has not called
    // endAttempt().  Refusing rather than silently replacing it keeps 11.9b's
    // "a further explicit user action" a real event.
    if (m_attempt) {
        if (whyNot) *whyNot = "the last attempt has not been dismissed";
        return false;
    }

    const BootstrapCandidate *found = nullptr;
    for (const auto &c : m_candidates)
        if (c.instanceName == instanceName) { found = &c; break; }
    if (found == nullptr) {
        if (whyNot) *whyNot = "that device is no longer offering to pair";
        return false;
    }

    auto stream = m_sf ? m_sf() : nullptr;
    auto agree  = m_af ? m_af() : nullptr;
    if (!stream || !agree) {
        if (whyNot) *whyNot = "guided pairing is not available in this build";
        return false;
    }
    m_attempt.reset(new GuidedAttempt(std::move(stream), std::move(agree), m_clock));
    std::string err;
    if (!m_attempt->begin(*found, &err)) {
        if (whyNot) *whyNot = err.empty() ? m_attempt->advice().message : err;
        // The attempt object survives so the caller can read advice(); it is
        // terminal and begin() will refuse until endAttempt().
        return false;
    }
    return true;
}

bool GuidedPairing::attemptInProgress() const
{
    return m_attempt && !m_attempt->terminal();
}

void GuidedPairing::endAttempt()
{
    if (!m_attempt) return;
    if (!m_attempt->terminal()) return;      // ended by abort/decline, not this
    if (m_attempt->phase() == GuidedPhase::Failed) {
        m_aborts++;
        const AbortAdvice &a = m_attempt->advice();
        // 11.9d1 — `unsupported_version` offers the code on the FIRST abort.
        // 11.9d — anything else on the second.
        if (a.offerPairingCode || m_aborts >= 2) m_offerCode = true;
    }
    m_attempt.reset();
}

void GuidedPairing::resetSitting()
{
    m_aborts = 0;
    m_offerCode = false;
}

}  // namespace Ppcp
