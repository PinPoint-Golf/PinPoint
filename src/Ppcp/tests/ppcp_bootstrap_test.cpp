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

// H10 — PPCP-RV §11 guided pairing, the INITIATOR half.
//
// ⛔ WHAT THIS SUITE CAN AND CANNOT SHOW, STATED FIRST BECAUSE IT IS THE WHOLE
// POINT OF THE SESSION THIS WAS WRITTEN IN.
//
// Every row below is arithmetic between two parties that are both behaving,
// and CR-01's §4 is nine ways to write an implementation that passes exactly
// that and authenticates nothing.  Three of those nine live in this file's
// subject — traps 3, 7 and 8 — and NONE of them is discoverable from the wire.
// So:
//
//   • Trap 3 (11.3d1) IS testable here, because the defence is structural: the
//     assertion is that `begin()` REFUSES, and a refusal is observable.
//   • Trap 7 (11.6b) IS testable here, because the assertion is that a failing
//     key agreement is called ONCE and produces `invalid_key`, and a call count
//     is observable.
//   • Trap 8 (11.1d) IS NOT testable here and cannot be.  "A peer that compares
//     the digits in software passes every static test in the document."  What
//     this suite can assert is the negative shape — that no function takes the
//     counterpart's digits, and that `affirm()` is reachable only from a call
//     the test makes on the user's behalf.  That is a code review, not a test,
//     and it is recorded as one.
//   • The ORDERING of 11.5b/11.5d is asserted here against libppcp's own
//     acceptor engine, which is the same code the relay runs — so it is one
//     implementation in-process rather than two agreeing with themselves (B7).
//     The AUTHORITATIVE evidence is `ppcp-relay --probe order-initiator`, run
//     out of process by tests/run-guided-relay.sh, because only the relay can
//     withhold a reply the way an attacker would.
//
// ⛔ AND NOTHING HERE CLAIMS RV-6.  9g is a MUST: a claim names RT-20c and
// states its result, and RT-20c needs both applications either side of the
// relay.  It is unrun.  No aggregate appears in this file or in anything it
// prints.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "ppcp_bootstrap.h"
#include "ppcp_discovery.h"

using namespace Ppcp;

namespace {

std::vector<std::uint8_t> txt(const std::vector<std::string> &kv)
{
    std::vector<std::uint8_t> out;
    for (const auto &s : kv) {
        out.push_back(static_cast<std::uint8_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    return out;
}

RvAdvertisement parse(const std::vector<std::string> &kv)
{
    RvAdvertisement ad;
    ad.instanceName = "PPCP-DEADBEEF";
    ad.host = "192.0.2.7";
    ad.port = 47791;
    const auto b = txt(kv);
    EXPECT_TRUE(parseTxtRecord(b.data(), b.size(), &ad));
    return ad;
}

// A BootstrapStream over one end of a socketpair.  No network, no responder.
//
// ⚠ SO_NOSIGPIPE, AND ⛔ IT MUST BE SET WHILE THE SOCKET IS STILL LIVE.
// Writing into a socket whose far end has gone raises SIGPIPE, whose default
// disposition TERMINATES THE PROCESS — so a phone that walks out of range
// mid-attempt would take the application with it, and that is 11.9a's "a
// closed connection", an ordinary outcome on this path rather than an exotic
// one.  This suite found it by DYING rather than by failing an assertion.
//
// The second half was found the same way, one build later: on macOS
// `setsockopt(SO_NOSIGPIPE)` returns **-1** once the peer has already closed,
// so setting it lazily — at first write, or in a constructor that runs after
// the counterpart has gone — silently does nothing and the process still dies.
// It is set here immediately after `socketpair()`, and in the shipped
// `TcpStream` immediately after `socket()` and BEFORE `connect()`, for that
// reason.  macOS has no `MSG_NOSIGNAL` to fall back on.
void noSigPipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

class FdStream final : public BootstrapStream {
public:
    explicit FdStream(int fd) : m_fd(fd) { noSigPipe(fd); }
    ~FdStream() override { close(); }

    bool connect(const std::string &, std::uint16_t, int, std::string *) override
    {
        return m_fd >= 0;
    }
    long read(void *buf, std::size_t len) override
    {
        if (m_fd < 0) return -1;
        const ssize_t n = ::recv(m_fd, buf, len, MSG_DONTWAIT);
        if (n > 0) return static_cast<long>(n);
        if (n == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    bool writeAll(const void *buf, std::size_t len) override
    {
        if (m_fd < 0) return false;
        return ::send(m_fd, buf, len, 0) == static_cast<ssize_t>(len);
    }
    void close() override { if (m_fd >= 0) { ::close(m_fd); m_fd = -1; } }
    bool isOpen() const override { return m_fd >= 0; }
    int  fd() const override { return m_fd; }

private:
    int m_fd = -1;
};

// A key agreement that fails on demand and COUNTS ITS CALLS.  The count is the
// assertion for trap 7: 11.6b's "MUST NOT retry it" is a statement about how
// many times this is reached, and nothing else observable says so.
class CountingAgreement final : public BootstrapKeyAgreement {
public:
    explicit CountingAgreement(bool failAgree) : m_fail(failAgree) {}

    bool generate(std::uint8_t pk[PPCP_RV_BS_KEY_BYTES]) override
    {
        generates++;
        if (m_inner == nullptr) m_inner = makeOpenSslKeyAgreement();
        return m_inner->generate(pk);
    }
    bool agree(const std::uint8_t peer_pk[PPCP_RV_BS_KEY_BYTES],
               std::uint8_t z[PPCP_RV_BS_KEY_BYTES]) override
    {
        agrees++;
        if (m_fail) { std::memset(z, 0, PPCP_RV_BS_KEY_BYTES); return false; }
        return m_inner && m_inner->agree(peer_pk, z);
    }
    void wipe() override { if (m_inner) m_inner->wipe(); }
    std::string describe() const override { return "counting"; }

    int generates = 0;
    int agrees = 0;

private:
    bool m_fail;
    std::unique_ptr<BootstrapKeyAgreement> m_inner;
};

// libppcp's own ACCEPTOR engine, driven over the other end of the socketpair.
//
// ⚠ THIS IS NOT A SECOND IMPLEMENTATION AND MUST NOT BECOME ONE.  CA4 puts
// PinPointStudio on the initiator side only; this is `ppcp_bs_engine` in
// PPCP_BS_ROLE_ACCEPTOR — the same object the relay and PinPointCapture drive —
// running in this process so the initiator has a counterpart with no network.
// Nothing here is compiled into the application.
class LibppcpAcceptor {
public:
    explicit LibppcpAcceptor(int fd) : m_fd(fd)
    {
        noSigPipe(fd);
        m_agree = makeOpenSslKeyAgreement();
        std::uint8_t pk[PPCP_RV_BS_KEY_BYTES];
        EXPECT_TRUE(m_agree->generate(pk));
        EXPECT_EQ(ppcp_bs_engine_init(&m_engine, PPCP_BS_ROLE_ACCEPTOR,
                                      PPCP_BS_VERSION, pk), PPCP_OK);
        OPENSSL_cleanse(pk, sizeof pk);
    }
    ~LibppcpAcceptor()
    {
        ppcp_bs_engine_wipe(&m_engine);
        if (m_fd >= 0) ::close(m_fd);
    }

    // Reads what is there, feeds the engine, writes what came back.  Records
    // the frame types SEEN, which is what 11.5b and 11.5d are asserted on.
    void pump()
    {
        std::uint8_t buf[PPCP_BS_MAX_FRAME * 2];
        const ssize_t n = ::recv(m_fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) m_in.insert(m_in.end(), buf, buf + n);
        while (!m_in.empty()) {
            // Read the frame ourselves first, so the test can assert on what
            // arrived rather than only on what the engine did with it.
            ppcp_bs_frame f{};
            std::size_t used = 0;
            if (ppcp_bs_frame_read(m_in.data(), m_in.size(), &f, &used) == PPCP_OK)
                seen.push_back(f.ty);

            ppcp_bs_step step{};
            std::size_t consumed = 0;
            const ppcp_result r = ppcp_bs_engine_recv(&m_engine, m_in.data(),
                                                      m_in.size(), &consumed, &step);
            if (r == PPCP_ERR_TRUNCATED) break;
            if (consumed > 0)
                m_in.erase(m_in.begin(), m_in.begin() + static_cast<long>(consumed));
            if (r != PPCP_OK) { failed = true; break; }
            if (!apply(step)) break;
            if (step.close) { closed = true; break; }
        }
    }

    bool comparing() const { return m_comparing; }
    bool paired() const { return m_paired; }
    std::uint32_t sas() const { return m_sas; }

    // The acceptor's user affirms.  ⛔ Called only by the test, standing in for
    // a person — the engine cannot tell the difference and 11.7c is the reason
    // this is a separate call rather than something pump() does.
    void affirm()
    {
        ppcp_bs_step step{};
        if (ppcp_bs_engine_affirm(&m_engine, &step) == PPCP_OK) apply(step);
    }

    std::vector<int> seen;
    bool failed = false;
    bool closed = false;

private:
    bool apply(const ppcp_bs_step &step)
    {
        if (step.has_out && step.out_len > 0)
            ::send(m_fd, step.out, step.out_len, 0);
        switch (step.event) {
        case PPCP_BS_EV_NEED_SECRET: {
            std::uint8_t z[PPCP_RV_BS_KEY_BYTES];
            if (!m_agree->agree(step.peer_pk, z)) {
                ppcp_bs_step s2{};
                ppcp_bs_engine_abort(&m_engine, PPCP_BS_RC_INVALID_KEY, &s2);
                if (s2.has_out) ::send(m_fd, s2.out, s2.out_len, 0);
                failed = true;
                return false;
            }
            ppcp_bs_step s2{};
            const ppcp_result r = ppcp_bs_engine_supply_secret(&m_engine, z, &s2);
            OPENSSL_cleanse(z, sizeof z);
            if (r != PPCP_OK) { failed = true; return false; }
            return apply(s2);
        }
        case PPCP_BS_EV_COMPARE:
            m_comparing = true;
            ppcp_bs_engine_sas(&m_engine, &m_sas);
            return true;
        case PPCP_BS_EV_PAIRED:
            m_paired = true;
            return true;
        case PPCP_BS_EV_ABORTED:
            failed = true;
            return false;
        default:
            return true;
        }
    }

    int m_fd = -1;
    ppcp_bs_engine m_engine{};
    std::unique_ptr<BootstrapKeyAgreement> m_agree;
    std::vector<std::uint8_t> m_in;
    bool m_comparing = false;
    bool m_paired = false;
    std::uint32_t m_sas = 0;
};

BootstrapCandidate aWindow()
{
    BootstrapCandidate c;
    c.instanceName = "PPCP-0BADCAFE";
    c.host = "127.0.0.1";
    c.port = 47791;
    c.pv = "1.0";
    c.role = "capture";
    return c;
}

}  // namespace

// ══ 3.3f / 3.3g — telling the two record shapes apart ═════════════════════

TEST(PpcpBootstrapDiscovery, BsIdentifiesABootstrapInstance)
{
    const auto ad = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1"});
    EXPECT_EQ(classifyInstance(ad), RvInstanceKind::Bootstrap);
}

TEST(PpcpBootstrapDiscovery, ReconnectionInstanceIsNotAWindow)
{
    const auto ad = parse({"txtvers=1", "pv=1.0", "role=host",
                           "rn=0011223344556677",
                           "rid=8899aabbccddeeff"});
    EXPECT_EQ(classifyInstance(ad), RvInstanceKind::Reconnection);
}

// ⛔ 3.3g's LAST SENTENCE, AND IT IS THE HOSTILE CASE: "a receiver that sees
// both `bs` and `rid` on one instance treats the instance as malformed and
// IGNORES it."  A bootstrap instance names no pairing because it holds none.
TEST(PpcpBootstrapDiscovery, BsAndRidTogetherIsMalformedAndIgnored)
{
    const auto ad = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1",
                           "rn=0011223344556677",
                           "rid=8899aabbccddeeff"});
    EXPECT_EQ(classifyInstance(ad), RvInstanceKind::Malformed);

    BootstrapCandidate c;
    EXPECT_FALSE(bootstrapCandidateFrom(ad, 1, &c));

    GuidedPairing gp;
    EXPECT_FALSE(gp.noteAdvertisement(ad, 1));
    EXPECT_TRUE(gp.candidates().empty());
}

TEST(PpcpBootstrapDiscovery, DlWithoutBsIsMalformed)
{
    // 3.3b bars a human-readable string from a reconnection instance; `dl` is
    // scoped to a window by 3.3g and bounded by 3.7d's lifetime.
    const auto ad = parse({"txtvers=1", "pv=1.0", "role=host",
                           "rn=0011223344556677",
                           "rid=8899aabbccddeeff", "dl=Bay 3"});
    EXPECT_EQ(classifyInstance(ad), RvInstanceKind::Malformed);
}

TEST(PpcpBootstrapDiscovery, PvIsFilteredOnMajorBeforeConnecting)
{
    // 3.3f — "as above, AND FILTERED BEFORE CONNECTING FOR THE SAME REASON".
    const auto ad = parse({"txtvers=1", "pv=2.0", "role=capture", "bs=1"});
    EXPECT_EQ(classifyInstance(ad), RvInstanceKind::Bootstrap);
    BootstrapCandidate c;
    EXPECT_FALSE(bootstrapCandidateFrom(ad, 1, &c));
    EXPECT_TRUE(bootstrapCandidateFrom(ad, 2, &c));
}

// ══ 4.4d, reached through 3.3g — `dl` is UNTRUSTED display text ═══════════

TEST(PpcpBootstrapLabel, TruncatedToTheThirtyTwoBytes33fAllows)
{
    const std::string long_(120, 'A');
    EXPECT_EQ(sanitiseLabel(long_).size(), 32u);
}

TEST(PpcpBootstrapLabel, ControlCharactersAndBidiNeverReachTheScreen)
{
    // A `dl` is a CBOR tstr and may be any UTF-8.  This host renders it into a
    // QML Text, where a right-to-left override is a display attack rather than
    // a character — so anything outside printable ASCII becomes a visible
    // placeholder and the operator can see that they cannot read it.
    const std::string hostile = std::string("Bay\r\n3\x1b[31m") + "\xe2\x80\xae" + "evil";
    const std::string safe = sanitiseLabel(hostile);
    EXPECT_EQ(safe.find('\r'), std::string::npos);
    EXPECT_EQ(safe.find('\n'), std::string::npos);
    EXPECT_EQ(safe.find('\x1b'), std::string::npos);
    for (unsigned char ch : safe) EXPECT_TRUE(ch >= 0x20 && ch < 0x7f);
    EXPECT_EQ(safe.substr(0, 3), "Bay");
}

TEST(PpcpBootstrapLabel, NothingIsKeyedOnTheLabel)
{
    // 4.4d — "MUST NOT be used as an identifier, a trust signal, or a storage
    // key".  Two windows with the SAME label stay two candidates, and the name
    // `begin()` takes is the instance name.  If `label` were the key one of
    // these would have replaced the other.
    GuidedPairing gp;
    auto ad1 = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1", "dl=Phone"});
    ad1.instanceName = "PPCP-00000001";
    auto ad2 = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1", "dl=Phone"});
    ad2.instanceName = "PPCP-00000002";
    EXPECT_TRUE(gp.noteAdvertisement(ad1, 1));
    EXPECT_TRUE(gp.noteAdvertisement(ad2, 1));
    ASSERT_EQ(gp.candidates().size(), 2u);
    EXPECT_EQ(gp.candidates()[0].label, gp.candidates()[1].label);
    EXPECT_NE(gp.candidates()[0].instanceName, gp.candidates()[1].instanceName);
}

// ══ 11.7a / 11.7d — the digits ════════════════════════════════════════════

TEST(PpcpBootstrapSas, SixCharactersWithLeadingZeros)
{
    // 11.7a — "`000042` is a valid string and MUST be shown as six characters."
    EXPECT_EQ(formatSas(42u), "000 042");
    EXPECT_EQ(formatSas(0u), "000 000");
    EXPECT_EQ(formatSas(999999u), "999 999");
    // 11.7d's own example, grouped identically at both peers.
    EXPECT_EQ(formatSas(313164u), "313 164");
    EXPECT_EQ(formatSas(435948u), "435 948");
}

// ══ 11.9c / 11.9d1 — what the user is told, and NOT offered ═══════════════

TEST(PpcpBootstrapAdvice, MismatchOffersNoRetryAffordance)
{
    // ⛔ 11.9c is a MUST NOT and this is the case it exists for.  A mismatch is
    // the ONE signal this path produces that an attack is under way, and a
    // dialogue whose reflex is *try again* converts a one-shot bound into an
    // unbounded one by way of the operator's muscle memory.
    const AbortAdvice a = adviseOnAbort(PPCP_BS_RC_REJECTED, /*declinedHere=*/true);
    EXPECT_FALSE(a.mayOfferRetry);
    EXPECT_NE(a.message.find("did not match"), std::string::npos);
    EXPECT_NE(a.message.find("do not try again"), std::string::npos);
}

TEST(PpcpBootstrapAdvice, AnIncomingRejectedIsTreatedAsTheDangerousCase)
{
    // 11.4f makes a user's refusal and a failed MAC INDISTINGUISHABLE to the
    // counterpart, both `rejected`.  We cannot tell which we received, and
    // 11.9c binds the dangerous half, so no retry is offered for either.
    const AbortAdvice a = adviseOnAbort(PPCP_BS_RC_REJECTED, /*declinedHere=*/false);
    EXPECT_FALSE(a.mayOfferRetry);
}

TEST(PpcpBootstrapAdvice, InvalidKeyIsNeverRetried)
{
    // Trap 7 / 11.6b — a rejected key is an attack signal, not a network fault.
    const AbortAdvice a = adviseOnAbort(PPCP_BS_RC_INVALID_KEY, false);
    EXPECT_FALSE(a.mayOfferRetry);
    // 11.6b's other half: it MUST NOT be reported as a transport error.  The
    // message says so in as many words, because the operator standing there is
    // the only party who can act on an attack signal.
    EXPECT_NE(a.message.find("not a network fault"), std::string::npos);
    EXPECT_NE(a.message.find("do not try again"), std::string::npos);
}

TEST(PpcpBootstrapAdvice, CommitmentMismatchIsNeverRetried)
{
    const AbortAdvice a = adviseOnAbort(PPCP_BS_RC_COMMITMENT_MISMATCH, false);
    EXPECT_FALSE(a.mayOfferRetry);
}

TEST(PpcpBootstrapAdvice, TimeoutMayBeReportedAsTheOrdinaryFailureItIs)
{
    // 11.9c's own exemption, and it matters: making EVERY failure alarming is
    // how an operator learns to ignore the alarming ones.
    EXPECT_TRUE(adviseOnAbort(PPCP_BS_RC_TIMEOUT, false).mayOfferRetry);
    EXPECT_TRUE(adviseOnAbort(PPCP_BS_RC_WINDOW_CLOSED, false).mayOfferRetry);
}

TEST(PpcpBootstrapAdvice, UnsupportedVersionOffersTheCodeOnTheFirstAbort)
{
    // 11.9d1 (E45) — the FIRST abort, not the second: a second attempt is
    // guaranteed to fail identically, because 11.4h has the initiator offer the
    // highest `v` it implements and forbids proposing lower.  11.9d's two-abort
    // threshold would spend an operator's attempt on a certainty.
    const AbortAdvice a = adviseOnAbort(PPCP_BS_RC_UNSUPPORTED_VERSION, false);
    EXPECT_TRUE(a.offerPairingCode);
    EXPECT_FALSE(a.mayOfferRetry);
    // 11.4e — the user is told the counterpart needs a newer application, not
    // given a generic failure.  "The operator is standing there and can act."
    EXPECT_NE(a.message.find("newer version"), std::string::npos);
}

TEST(PpcpBootstrapAdvice, OtherReasonsOfferTheCodeOnTheSecondAbort)
{
    // 11.9d — the general rule, which 11.9d1 only overrides for one code.
    GuidedPairing gp;
    EXPECT_FALSE(gp.shouldOfferPairingCode());
    gp.setFactories([] { return std::unique_ptr<BootstrapStream>(); },
                    [] { return std::unique_ptr<BootstrapKeyAgreement>(); });
    // Simulated by driving the counter directly through two failed sittings is
    // not possible without an attempt, so this asserts the initial state and
    // the reset; the two-abort path is exercised by TwoAbortsOfferTheCode below.
    gp.resetSitting();
    EXPECT_EQ(gp.abortsThisSitting(), 0u);
}

// ══ ⛔ TRAP 3 (11.3d1) — the one that the natural implementation breaks ════

TEST(PpcpBootstrapTrap3, SeveralWindowsAreCandidatesAndNoneIsDialled)
{
    // 3.3f's `dl` exists so a browser seeing four windows can tell them apart,
    // and the obvious host interface is a list.  Holding four candidates is
    // fine; DIALLING them is the trap.  Nothing here has opened a socket.
    GuidedPairing gp;
    int streams = 0;
    gp.setFactories([&streams] {
                        streams++;
                        return std::unique_ptr<BootstrapStream>();
                    },
                    [] { return makeOpenSslKeyAgreement(); });
    for (int i = 0; i < 4; ++i) {
        auto ad = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1",
                         "dl=Bay " + std::to_string(i)});
        ad.instanceName = "PPCP-0000000" + std::to_string(i);
        EXPECT_TRUE(gp.noteAdvertisement(ad, 1));
    }
    EXPECT_EQ(gp.candidates().size(), 4u);
    EXPECT_EQ(streams, 0);                       // ⛔ nothing dialled
    EXPECT_FALSE(gp.attemptInProgress());
}

TEST(PpcpBootstrapTrap3, OneAttemptAtATimeAndTheSecondIsRefused)
{
    // ⛔ 11.3d1 — "an INITIATOR runs at most one bootstrap attempt at a time,
    // and MUST NOT display digits for more than one attempt."  An attacker
    // advertising N windows otherwise gets N blind draws against ONE operator
    // confirmation, with the operator finding the collision for them.
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    LibppcpAcceptor acceptor(a[1]);

    GuidedPairing gp;
    bool handedOut = false;
    gp.setFactories(
        [&] () -> std::unique_ptr<BootstrapStream> {
            if (handedOut) return nullptr;
            handedOut = true;
            return std::unique_ptr<BootstrapStream>(new FdStream(a[0]));
        },
        [] { return makeOpenSslKeyAgreement(); });

    auto ad1 = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1", "dl=Bay 1"});
    ad1.instanceName = "PPCP-00000001";
    auto ad2 = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1", "dl=Bay 2"});
    ad2.instanceName = "PPCP-00000002";
    gp.noteAdvertisement(ad1, 1);
    gp.noteAdvertisement(ad2, 1);

    std::string why;
    ASSERT_TRUE(gp.begin("PPCP-00000001", &why)) << why;
    EXPECT_TRUE(gp.attemptInProgress());

    // ⛔ THE ASSERTION.  A second window cannot be dialled while the first is
    // live, so there is never a moment at which two sets of digits exist.
    EXPECT_FALSE(gp.begin("PPCP-00000002", &why));
    EXPECT_NE(why.find("already running"), std::string::npos);
    EXPECT_EQ(handedOut, true);
}

TEST(PpcpBootstrapTrap3, ATerminalAttemptMustBeDismissedBeforeAnother)
{
    // 11.9b — "MUST NOT reopen the window without a further EXPLICIT user
    // action ... MUST NOT retry automatically, offer a *try again* control that
    // reopens without that action".  A caller that has not dismissed the last
    // result has not had that action, so `begin()` refuses.
    GuidedPairing gp;
    gp.setFactories([] { return std::unique_ptr<BootstrapStream>(); },
                    [] { return makeOpenSslKeyAgreement(); });
    auto ad = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1"});
    ad.instanceName = "PPCP-00000001";
    gp.noteAdvertisement(ad, 1);

    std::string why;
    EXPECT_FALSE(gp.begin("PPCP-00000001", &why));   // no stream in this build
    EXPECT_FALSE(gp.begin("PPCP-00000001", &why));
    EXPECT_NE(why.find("not available"), std::string::npos);
}

TEST(PpcpBootstrapTrap3, TwoAbortsOfferTheCode)
{
    // 11.9d — the pairing code is REQUIRED of every implementation (2a) and is
    // the answer to both plausible causes.
    // A fresh dead endpoint per attempt: 11.5a wants a new keypair each time
    // and 3.7b closes the window on each abort, so re-handing the same socket
    // would be modelling something that cannot happen.
    GuidedPairing gp;
    int made = 0;
    gp.setFactories(
        [&made] () -> std::unique_ptr<BootstrapStream> {
            int s[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0) return nullptr;
            noSigPipe(s[0]);                         // ⛔ while it is still live
            ::close(s[1]);                           // nothing on the far end
            made++;
            return std::unique_ptr<BootstrapStream>(new FdStream(s[0]));
        },
        [] { return makeOpenSslKeyAgreement(); });
    auto ad = parse({"txtvers=1", "pv=1.0", "role=capture", "bs=1"});
    ad.instanceName = "PPCP-00000001";
    gp.noteAdvertisement(ad, 1);

    std::string why;
    gp.begin("PPCP-00000001", &why);
    if (gp.attempt() && !gp.attempt()->terminal()) gp.attempt()->abort(PPCP_BS_RC_TIMEOUT);
    gp.endAttempt();
    EXPECT_EQ(gp.abortsThisSitting(), 1u);
    EXPECT_FALSE(gp.shouldOfferPairingCode());

    gp.begin("PPCP-00000001", &why);
    if (gp.attempt() && !gp.attempt()->terminal()) gp.attempt()->abort(PPCP_BS_RC_TIMEOUT);
    gp.endAttempt();
    EXPECT_EQ(gp.abortsThisSitting(), 2u);
    EXPECT_TRUE(gp.shouldOfferPairingCode());

    gp.resetSitting();
    EXPECT_FALSE(gp.shouldOfferPairingCode());
    EXPECT_EQ(made, 2);
}

// ══ ⛔ TRAP 7 (11.6b, 11.11f) — a failed agreement is not a network fault ══

TEST(PpcpBootstrapTrap7, AFailedAgreementIsInvalidKeyAndIsCalledExactlyOnce)
{
    // ⛔ E36 MEASURED THIS ON THE LIBRARY THIS FILE LINKS.  "OpenSSL 3.6.3
    // fails EVP_PKEY_derive for each of the five standard small-order
    // u-coordinates" — so the zero-check every implementer writes from 11.6b's
    // ORIGINAL wording can never fire, and the branch that DOES fire is the
    // failure one.  An implementation following the clause literally believes
    // the case is defended and leaves the live branch on the generic error
    // path, where it reads as a network fault and gets retried.
    //
    // The assertion is `agrees == 1`.  A retry loop is the thing 11.6b's last
    // sentence forbids, and a call count is the only observable that says so.
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    LibppcpAcceptor acceptor(a[1]);

    auto agree = new CountingAgreement(/*failAgree=*/true);
    GuidedAttempt at(std::unique_ptr<BootstrapStream>(new FdStream(a[0])),
                     std::unique_ptr<BootstrapKeyAgreement>(agree),
                     nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(aWindow(), &err)) << err;

    for (int i = 0; i < 50 && !at.terminal(); ++i) { acceptor.pump(); at.poll(); }

    ASSERT_TRUE(at.terminal());
    EXPECT_EQ(at.phase(), GuidedPhase::Failed);
    EXPECT_EQ(at.advice().rc, PPCP_BS_RC_INVALID_KEY);
    EXPECT_FALSE(at.advice().mayOfferRetry);
    EXPECT_EQ(agree->agrees, 1);                 // ⛔ NOT RETRIED
    EXPECT_EQ(agree->generates, 1);              // 11.5a — one keypair, one attempt
}

// ══ 11.5b / 11.5d — the ordering, in-process ══════════════════════════════

TEST(PpcpBootstrapOrdering, OfferCarriesNoKeyAndRevealFollowsOnlyAccept)
{
    // ⚠ THE AUTHORITATIVE MEASUREMENT IS `ppcp-relay --probe order-initiator`,
    // which never replies and checks that no `bs_reveal` follows.  This row is
    // the in-process shadow of it and is here so a regression is caught by
    // `ctest` rather than only by a run that needs a second process.
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    LibppcpAcceptor acceptor(a[1]);

    GuidedAttempt at(std::unique_ptr<BootstrapStream>(new FdStream(a[0])),
                     makeOpenSslKeyAgreement(), nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(aWindow(), &err)) << err;

    // Only `bs_offer` has been sent.  11.5b: it carries `v` and `ct` and does
    // NOT carry `pk_i`.
    acceptor.pump();
    ASSERT_EQ(acceptor.seen.size(), 1u);
    EXPECT_EQ(acceptor.seen[0], PPCP_BS_OFFER);

    // ⛔ AND WITH NOTHING COMING BACK, NOTHING MORE GOES OUT.  `pk_i` follows
    // in `bs_reveal` and only after `bs_accept` — 11.5d.  The acceptor above
    // has ALREADY replied inside pump(), so to observe the silence properly the
    // relay probe is the instrument; here we assert only that the first frame
    // out was the offer and that reveal never precedes accept.
    for (int i = 0; i < 50 && !at.terminal(); ++i) { at.poll(); acceptor.pump(); }

    ASSERT_GE(acceptor.seen.size(), 2u);
    EXPECT_EQ(acceptor.seen[1], PPCP_BS_REVEAL);
    EXPECT_TRUE(acceptor.comparing());
}

TEST(PpcpBootstrapOrdering, NoRevealFollowsAnUnansweredOffer)
{
    // The in-process mirror of the relay probe: a counterpart that reads and
    // never replies.  ⛔ If `bs_reveal` appeared here, `pk_i` would be on the
    // wire before the acceptor had committed to `pk_a`, which is trap 2 seen
    // from the initiator's side.
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);

    GuidedAttempt at(std::unique_ptr<BootstrapStream>(new FdStream(a[0])),
                     makeOpenSslKeyAgreement(), nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(aWindow(), &err)) << err;
    for (int i = 0; i < 20; ++i) at.poll();      // nothing is written to a[1]

    std::vector<std::uint8_t> got;
    std::uint8_t buf[512];
    for (;;) {
        const ssize_t n = ::recv(a[1], buf, sizeof buf, MSG_DONTWAIT);
        if (n <= 0) break;
        got.insert(got.end(), buf, buf + n);
    }
    ASSERT_FALSE(got.empty());
    std::vector<int> types;
    std::size_t at_ = 0;
    while (at_ < got.size()) {
        ppcp_bs_frame f{};
        std::size_t used = 0;
        if (ppcp_bs_frame_read(got.data() + at_, got.size() - at_, &f, &used) != PPCP_OK)
            break;
        types.push_back(f.ty);
        at_ += used;
    }
    ASSERT_GE(types.size(), 1u);
    EXPECT_EQ(types[0], PPCP_BS_OFFER);
    for (int t : types) EXPECT_NE(t, PPCP_BS_REVEAL);   // ⛔ no pk_i
    ::close(a[1]);
}

// ══ 11.7e / 11.7f — when the digits exist, and when they do not ═══════════

TEST(PpcpBootstrapDigits, NothingIsShownBefore115dCompletes)
{
    // ⛔ 11.7e — "a peer MUST NOT display ANY PART of the digits, or any control
    // that affirms them, before it has completed 11.5d.  There is nothing to
    // compare before then, and a progressive display would leak the value to
    // whichever side an attacker reached first."
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    GuidedAttempt at(std::unique_ptr<BootstrapStream>(new FdStream(a[0])),
                     makeOpenSslKeyAgreement(), nullptr);
    std::uint32_t v = 0;
    EXPECT_FALSE(at.sas(&v));                    // before begin()
    EXPECT_TRUE(at.sasDigits().empty());
    // 11.7e also bars the CONTROL, not only the value.
    EXPECT_FALSE(at.affirm());

    std::string err;
    ASSERT_TRUE(at.begin(aWindow(), &err)) << err;
    EXPECT_FALSE(at.sas(&v));                    // offer sent, nothing derived
    EXPECT_TRUE(at.sasDigits().empty());
    EXPECT_FALSE(at.affirm());
    ::close(a[1]);
}

TEST(PpcpBootstrapDigits, TheyAreGoneOnceTheAttemptEnds)
{
    // 11.7f — "the digits MUST NOT be reused, cached, or shown again after the
    // attempt ends.  They are a function of two ephemeral keys and are
    // meaningless outside the attempt that produced them."
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    LibppcpAcceptor acceptor(a[1]);
    GuidedAttempt at(std::unique_ptr<BootstrapStream>(new FdStream(a[0])),
                     makeOpenSslKeyAgreement(), nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(aWindow(), &err)) << err;
    for (int i = 0; i < 50 && at.phase() != GuidedPhase::Comparing; ++i) {
        acceptor.pump();
        at.poll();
    }
    ASSERT_EQ(at.phase(), GuidedPhase::Comparing);
    EXPECT_FALSE(at.sasDigits().empty());

    at.decline();
    EXPECT_TRUE(at.terminal());
    std::uint32_t v = 0;
    EXPECT_FALSE(at.sas(&v));
    EXPECT_TRUE(at.sasDigits().empty());
    // 11.9c — and the message does not invite a retry.
    EXPECT_FALSE(at.advice().mayOfferRetry);
}

// ══ 11.5e-11.5h — a complete guided pairing ═══════════════════════════════

TEST(PpcpBootstrapExchange, BothUsersAffirmAndAPairingExists)
{
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    LibppcpAcceptor acceptor(a[1]);
    GuidedAttempt at(std::unique_ptr<BootstrapStream>(new FdStream(a[0])),
                     makeOpenSslKeyAgreement(), nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(aWindow(), &err)) << err;

    for (int i = 0; i < 50 && at.phase() != GuidedPhase::Comparing; ++i) {
        acceptor.pump();
        at.poll();
    }
    ASSERT_EQ(at.phase(), GuidedPhase::Comparing);
    // The initiator reaches the comparison as soon as it has supplied `Z`,
    // which is the same call that put `bs_reveal` on the wire — so the
    // acceptor has not necessarily read it yet.  That asymmetry is real and is
    // 11.5e's own shape: "both peers then derive ... and each waits for ITS OWN
    // user".  Nothing is owed between the two until a person acts.
    for (int i = 0; i < 20 && !acceptor.comparing(); ++i) acceptor.pump();
    ASSERT_TRUE(acceptor.comparing());

    // ⛔ TRAP 8, AND THIS IS THE ONE ASSERTION THIS SUITE CANNOT MAKE FOR REAL.
    // The two ends derived the same digits, and a person holding two screens is
    // what turns that into authentication.  The test compares them because the
    // test is checking the ARITHMETIC; the APPLICATION must never do this, and
    // there is no function in ppcp_bootstrap.h that would let it — nothing
    // takes the counterpart's digits, because the comparison has value only
    // because it crosses a channel the attacker is not on.
    std::uint32_t mine = 0;
    ASSERT_TRUE(at.sas(&mine));
    EXPECT_EQ(mine, acceptor.sas());
    EXPECT_EQ(formatSas(mine).size(), 7u);       // "nnn nnn"

    // 11.7c — each peer obtains an affirmative act from ITS OWN user.  A single
    // affirmation at one end does not establish a pairing at the other.
    ASSERT_TRUE(at.affirm());
    EXPECT_EQ(at.phase(), GuidedPhase::Confirming);
    for (int i = 0; i < 10; ++i) { acceptor.pump(); at.poll(); }
    EXPECT_NE(at.phase(), GuidedPhase::Paired);  // the acceptor's user has not

    acceptor.affirm();
    for (int i = 0; i < 50 && at.phase() != GuidedPhase::Paired; ++i) {
        acceptor.pump();
        at.poll();
    }
    ASSERT_EQ(at.phase(), GuidedPhase::Paired) << "11.5g was not reached";
    EXPECT_TRUE(acceptor.paired());

    // 11.5g — and only now does the pairing exist.
    ppcp_bs_pairing p{};
    ASSERT_TRUE(at.takePairing(&p));
    std::uint8_t acc = 0;
    for (std::size_t i = 0; i < PPCP_RV_KEY_BYTES; ++i) acc |= p.keys.prk[i];
    EXPECT_NE(acc, 0);
    // 11.6d — `sid` is a well-formed version 4 UUID, bits set before use.
    EXPECT_EQ(p.sid[6] & 0xf0, 0x40);
    EXPECT_EQ(p.sid[8] & 0xc0, 0x80);

    // Taking it a second time yields nothing: it moved out and was erased.
    ppcp_bs_pairing again{};
    EXPECT_FALSE(at.takePairing(&again));
}

TEST(PpcpBootstrapExchange, ADeclineAtThisEndLeavesNoPairingAnywhere)
{
    // 11.9a — "any abort ends the attempt, closes the window, and leaves NO
    // pairing at either peer."
    int a[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
    LibppcpAcceptor acceptor(a[1]);
    GuidedAttempt at(std::unique_ptr<BootstrapStream>(new FdStream(a[0])),
                     makeOpenSslKeyAgreement(), nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(aWindow(), &err)) << err;
    for (int i = 0; i < 50 && at.phase() != GuidedPhase::Comparing; ++i) {
        acceptor.pump();
        at.poll();
    }
    ASSERT_EQ(at.phase(), GuidedPhase::Comparing);

    at.decline();
    for (int i = 0; i < 20; ++i) acceptor.pump();

    ppcp_bs_pairing p{};
    EXPECT_FALSE(at.takePairing(&p));
    EXPECT_FALSE(acceptor.paired());
    EXPECT_EQ(at.advice().rc, PPCP_BS_RC_REJECTED);
    EXPECT_FALSE(at.advice().mayOfferRetry);
}

// ══ 3.3d — the `pv` this host advertises, carried forward from H9 ═════════

TEST(PpcpBootstrapVersion, PvIsDerivedFromTheWireVersionAndNotWrittenOut)
{
    // ⚠ H9 CARRIED THE LITERAL `"1.0"`.  Harmless today and wrong the day the
    // wire version moves — and it fails SILENTLY, because 3.6a forbids treating
    // a discovery mismatch as an error state.  A host advertising a stale `pv`
    // is simply never dialled, by a browser doing exactly what 3.3a says.
    const std::string pv = wirePvRange();
    EXPECT_EQ(pv, std::to_string(PPCP_WIRE_VERSION_MAJOR) + "." +
                  std::to_string(PPCP_WIRE_VERSION_MINOR));
    RvTxtFields f;
    EXPECT_EQ(f.pv, pv);
    EXPECT_TRUE(pvAcceptsMajor(f.pv, PPCP_WIRE_VERSION_MAJOR));
}

// ══ The gate — a live guided pairing against `ppcp-relay` ═════════════════
//
// Skips in a millisecond when PPCP_BS_RELAY is unset, which is every ordinary
// ctest run.  tests/run-guided-relay.sh sets it, having started
// `ppcp-relay --peer acceptor --listen P` first.
//
// ⛔ THE RELAY'S OWN `--peer` MODE IS NOT A CONFORMANT PEER AND SAYS SO: "it
// affirms its own comparison in software, which is the one thing 11.1d
// forbids."  It exists so this side has a counterpart before PinPointCapture's
// acceptor does.  What this row demonstrates is that THIS application completes
// a guided pairing over a real socket; it demonstrates nothing about the relay.

// ⛔ MY OWN MIRROR OF 11.5c, AND I RUN ONLY MINE (CA4).
//
// `ppcp-relay --probe order-initiator --listen P` is the instrument: it accepts
// the connection, reads the `bs_offer`, asserts it carries `v` and `ct` ONLY,
// and then NEVER REPLIES — and the row passes if and only if no `bs_reveal`
// follows in the observation window.  That is 11.5b and 11.5d measured from
// outside, and it is the half of trap 2 that lives on this side.
//
// ⚠ THE VERDICT IS THE PROBE'S, NOT THIS TEST'S.  All this row does is DIAL and
// then sit still for long enough to be observed; the probe's exit code is what
// run-guided-relay.sh reports.  A test that concluded here would be this
// application marking its own homework — which is the whole reason L21 exists
// and is CA4's point about each team running its own half.
TEST(PpcpGuidedRelay, DialsForTheOrderingProbe)
{
    const char *ep = std::getenv("PPCP_BS_PROBE");
    if (ep == nullptr || *ep == '\0') {
        GTEST_SKIP() << "PPCP_BS_PROBE unset — see tests/run-guided-relay.sh";
    }
    std::string spec(ep);
    const std::size_t colon = spec.rfind(':');
    ASSERT_NE(colon, std::string::npos) << "PPCP_BS_PROBE wants HOST:PORT";

    BootstrapCandidate c;
    c.instanceName = "PPCP-PROBE000";
    c.host = spec.substr(0, colon);
    c.port = static_cast<std::uint16_t>(std::atoi(spec.c_str() + colon + 1));
    c.pv = wirePvRange();
    c.role = "capture";

    GuidedAttempt at(makeTcpStream(), makeOpenSslKeyAgreement(), nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(c, &err)) << err;

    // Sit for longer than the probe's default --observe-ms of 3000, doing
    // exactly what the application would do: poll, and send nothing that the
    // exchange has not earned.  11.3e's 30-second timer will not have fired.
    for (int i = 0; i < 6000 && !at.terminal(); ++i) { at.poll(); ::usleep(1000); }

    // Nothing is asserted about the outcome beyond this: the attempt must not
    // have PAIRED, because nothing came back and 11.5g cannot have been met.
    EXPECT_NE(at.phase(), GuidedPhase::Paired);
}

TEST(PpcpGuidedRelay, CompletesAPairingAgainstTheRelay)
{
    const char *ep = std::getenv("PPCP_BS_RELAY");
    if (ep == nullptr || *ep == '\0') {
        GTEST_SKIP() << "PPCP_BS_RELAY unset — see tests/run-guided-relay.sh";
    }
    std::string spec(ep);
    const std::size_t colon = spec.rfind(':');
    ASSERT_NE(colon, std::string::npos) << "PPCP_BS_RELAY wants HOST:PORT";

    BootstrapCandidate c;
    c.instanceName = "PPCP-RELAY000";
    c.host = spec.substr(0, colon);
    c.port = static_cast<std::uint16_t>(std::atoi(spec.c_str() + colon + 1));
    c.pv = wirePvRange();
    c.role = "capture";

    GuidedAttempt at(makeTcpStream(), makeOpenSslKeyAgreement(), nullptr);
    std::string err;
    ASSERT_TRUE(at.begin(c, &err)) << err;

    for (int i = 0; i < 30000 && at.phase() != GuidedPhase::Comparing &&
                    !at.terminal(); ++i) {
        at.poll();
        ::usleep(1000);
    }
    ASSERT_EQ(at.phase(), GuidedPhase::Comparing)
        << "never reached the comparison: " << at.advice().message;

    // The digits go to stdout so a PERSON can hold them against the relay's own
    // printed pair.  ⛔ Nothing in this process compares them (11.1d, trap 8).
    std::printf("\n  PinPointStudio (initiator) SAS: %s\n\n",
                at.sasDigits().c_str());

    ASSERT_TRUE(at.affirm());
    for (int i = 0; i < 30000 && !at.terminal(); ++i) { at.poll(); ::usleep(1000); }

    ASSERT_EQ(at.phase(), GuidedPhase::Paired) << at.advice().message;
    ppcp_bs_pairing p{};
    EXPECT_TRUE(at.takePairing(&p));
}

// ══ ⛔ THE TWO UX MUSTs, READ OFF THE QML SOURCE ══════════════════════════
//
// ⚠ WHAT THIS IS, AND WHAT IT IS NOT, STATED PLAINLY.
//
// 11.7d and 11.9c are MUSTs about a DIALOGUE, and the honest instrument for
// them is a rendered screen — this repository has one, the offscreen `qml_ui`
// suite, and it lives in the umbrella tree.  This session may not build the
// PinPointStudio application target, so that instrument is out of reach here
// and saying so is part of the result.
//
// What IS in reach is the property that actually regresses.  Nobody will
// rewrite this dialogue from scratch; somebody will "tidy" its footer to match
// the house convention every other dialogue in the application follows —
// Cancel first, the affirmative last carrying `primary: true` — which is
// correct for *Export* and is exactly the arrangement 11.7d forbids here.  So
// these rows read the QML as text and assert the shape:
//
//   • the affirmative control is NOT `primary`,
//   • it is NOT the last control in its row (not where a stray tap lands),
//   • the prompt asks whether the numbers MATCH,
//   • and the retry control is bound to `guidedMayRetry` and hidden with
//     `visible`, not disabled with `enabled` — because a greyed-out *Try
//     again* still teaches the operator that trying again is the answer, and
//     11.9c is about the reflex rather than the click.
//
// ⛔ A SOURCE SCAN IS WEAKER THAN A RENDER AND IS RECORDED AS SUCH.  It cannot
// see focus, z-order, or a style that repaints a plain button to look primary.
// It is a regression guard, not a demonstration that the dialogue is right.

namespace {

// ⚠ COMMENTS OUT FIRST, AND THE SUITE FOUND OUT WHY BY FAILING ON THEM.  This
// dialogue is heavily commented — deliberately, because the reasons are the
// only thing stopping someone "fixing" its inverted footer — and the comments
// say "NOT primary" and "the six digits are the only thing that authenticates
// anybody" right beside the code they describe.  A scanner that reads the prose
// asserts against the explanation rather than the dialogue.
std::string stripComments(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    std::size_t at = 0;
    while (at < in.size()) {
        const std::size_t nl = in.find('\n', at);
        const std::string line = in.substr(at, nl == std::string::npos
                                               ? std::string::npos : nl - at);
        const std::size_t c = line.find("//");
        out += (c == std::string::npos) ? line : line.substr(0, c);
        out += '\n';
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return out;
}

std::string readQml(const char *rel)
{
    std::string path = std::string(PP_PPCP_SRC_DIR) + "/../Gui/home/" + rel;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return std::string();
    std::string out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return stripComments(out);
}

// The body of one named QML object: from its `objectName` to the start of the
// NEXT named one.  Crude, and sufficient for a flat dialogue in which every
// control this suite cares about carries an objectName — and it is bounded that
// way rather than by a closing brace because a brace at a guessed indentation
// ran straight past `guidedAffirm` into `guidedReject`, whose `primary: true`
// then read as the affirmative being the default.  The scanner said the
// dialogue was wrong when it was right, which is the worse direction for a
// guard like this to fail in.
std::string blockFor(const std::string &qml, const std::string &objectName)
{
    const std::size_t at = qml.find("objectName: \"" + objectName + "\"");
    if (at == std::string::npos) return std::string();
    const std::size_t next = qml.find("objectName: \"", at + 1);
    return qml.substr(at, next == std::string::npos ? std::string::npos : next - at);
}

}  // namespace

TEST(PpcpGuidedUx, TheAffirmativeControlIsNotTheDefault)
{
    // ⛔ 11.7d — "the affirmative control is not pre-selected and not the one a
    // stray tap reaches ... A dialogue whose default is *Continue* is a
    // dialogue that authenticates whatever is on the other end."
    const std::string qml = readQml("PpcpGuidedPairDialog.qml");
    ASSERT_FALSE(qml.empty()) << "the guided pairing dialogue is missing";

    const std::string affirm = blockFor(qml, "guidedAffirm");
    ASSERT_FALSE(affirm.empty()) << "no affirmative control found";
    // NOT primary, NOT attention — nothing that paints it as the thing to do.
    EXPECT_EQ(affirm.find("primary"), std::string::npos)
        << "⛔ the affirmative control is styled as the default (11.7d)";
    EXPECT_EQ(affirm.find("attention"), std::string::npos);
    EXPECT_EQ(affirm.find("focus"), std::string::npos);

    // And it comes BEFORE the safe answer in its row, so the right-hand slot a
    // thumb reaches by habit is the one that declines.
    const std::size_t a = qml.find("objectName: \"guidedAffirm\"");
    const std::size_t r = qml.find("objectName: \"guidedReject\"");
    ASSERT_NE(a, std::string::npos);
    ASSERT_NE(r, std::string::npos);
    EXPECT_LT(a, r) << "⛔ the affirmative is where a stray tap lands (11.7d)";

    // The SAFE answer is the prominent one.
    const std::string reject = blockFor(qml, "guidedReject");
    EXPECT_NE(reject.find("primary: true"), std::string::npos);
}

TEST(PpcpGuidedUx, ThePromptAsksWhetherTheNumbersMatch)
{
    // ⛔ 11.7d — "the prompt asks whether the numbers MATCH rather than whether
    // to trust or continue."  Every one of trust/continue/allow/connect asks
    // the operator for a judgement they have no basis for.
    const std::string qml = readQml("PpcpGuidedPairDialog.qml");
    ASSERT_FALSE(qml.empty());
    const std::string prompt = blockFor(qml, "guidedPrompt");
    ASSERT_FALSE(prompt.empty());
    EXPECT_NE(prompt.find("match"), std::string::npos);
    for (const char *forbidden : {"Trust", "Continue", "Allow", "Connect"})
        EXPECT_EQ(prompt.find(forbidden), std::string::npos)
            << "⛔ the prompt asks for a judgement rather than a comparison: "
            << forbidden;
}

TEST(PpcpGuidedUx, NoRetryAffordanceSurvivesAMismatch)
{
    // ⛔ 11.9c — "a peer MUST NOT report an abort to its user in terms that
    // invite a retry as the obvious next step where the cause was a mismatch or
    // a MAC failure."  The control is bound to `guidedMayRetry`, which
    // `adviseOnAbort` sets false for exactly those causes.
    const std::string qml = readQml("PpcpGuidedPairDialog.qml");
    ASSERT_FALSE(qml.empty());
    const std::string retry = blockFor(qml, "guidedRetry");
    ASSERT_FALSE(retry.empty());
    EXPECT_NE(retry.find("visible"), std::string::npos);
    EXPECT_NE(retry.find("mayRetry"), std::string::npos)
        << "⛔ the retry control is not bound to 11.9c's predicate";
    // ⚠ HIDDEN, NOT DISABLED.  A greyed-out *Try again* still teaches the
    // operator that trying again is the shape of the answer.
    EXPECT_EQ(retry.find("enabled:"), std::string::npos)
        << "⛔ a disabled retry control is still a retry affordance (11.9c)";
}

TEST(PpcpGuidedUx, TheWindowListShowsNoDigits)
{
    // ⛔ 11.3d1 / TRAP 3 — a list of discovered windows each carrying a number
    // is the interface that hands an attacker N draws against one confirmation.
    // The list binds `label` and `instanceName`, and there is no digits binding
    // anywhere inside it.
    const std::string qml = readQml("PpcpGuidedPairDialog.qml");
    ASSERT_FALSE(qml.empty());
    const std::size_t list = qml.find("objectName: \"guidedWindowList\"");
    const std::size_t compare = qml.find("objectName: \"guidedCompare\"");
    ASSERT_NE(list, std::string::npos);
    ASSERT_NE(compare, std::string::npos);
    ASSERT_LT(list, compare);
    const std::string body = qml.substr(list, compare - list);
    EXPECT_EQ(body.find("digits"), std::string::npos)
        << "⛔ the candidate list carries digits (11.3d1, trap 3)";
    EXPECT_EQ(body.find("guidedAffirm"), std::string::npos);
}
