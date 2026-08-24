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

// PPCP-RV §11 — RV-6 guided pairing, the INITIATOR half.  Work package H10.
//
// This host browses for open bootstrap windows (3.3f, §3.7), the user picks
// ONE, we dial its SRV endpoint (3.7f) and drive libppcp's five-frame engine
// (CA2) to six digits the user compares against the phone in their hand.
//
// ⛔⛔ THE THREE TRAPS THAT LIVE IN THIS FILE, AND WHY EACH IS INVISIBLE.
//
// TRAP 3 (11.3d1) — DIALLING SEVERAL WINDOWS TO SHOW A LIST OF NUMBERS.
//   3.3f's `dl` exists so a browser seeing four open windows can tell them
//   apart, and a list of discovered windows is the obvious host interface.  So
//   the obvious next step is to dial them all and show the operator the
//   candidate numbers.  ⛔ THAT GIVES AN ATTACKER ADVERTISING N WINDOWS N
//   BLIND DRAWS AGAINST ONE CONFIRMATION, AND THE OPERATOR FINDS THE COLLISION
//   FOR THEM: shown a list of numbers one of which matches the phone in their
//   hand, an operator taps the match and reads it as success.  At twenty bays
//   that is a factor of twenty.
//
//   The defence is structural rather than a check.  `GuidedPairing` below
//   holds candidates that NOTHING dials, and `begin()` — which takes ONE
//   instance name — is the only function in this file that opens a socket.  It
//   refuses while an attempt is live, so `attempt()` is a single object and
//   there is no container anywhere that could hold two sets of digits.  A call
//   site cannot introduce the trap without adding a loop that the type system
//   makes obviously wrong.
//
// TRAP 7 (11.6b, 11.11f) — A FAILED KEY AGREEMENT REPORTED AS A TRANSPORT
//   ERROR, AND RETRIED.  OpenSSL fails `EVP_PKEY_derive` for each of the five
//   standard small-order u-coordinates rather than returning zeros, so the
//   branch that actually fires is the one that lands on the generic error path
//   — where it reads as a network fault and gets a retry.  ⛔ A REJECTED KEY IS
//   AN ATTACK SIGNAL, and a retry loop around it eats 3.7b's single-attempt
//   bound, which is what §11.8's whole argument rests on.  So
//   `BootstrapKeyAgreement::agree()` returns a bool that is the failure signal
//   (11.11f), `Attempt::supplySecret()` maps a false to
//   PPCP_BS_RC_INVALID_KEY and to nothing else, and there is no retry anywhere
//   in this file — an attempt that ends is over.
//
// TRAP 8 (11.1d) — COMPARING THE DIGITS IN SOFTWARE.  There is no function
//   here that takes the counterpart's digits, and there cannot be: the
//   comparison has value only because it crosses a channel the attacker is not
//   on, and the only such channel is a person looking at two screens.  A peer
//   that matched them itself, or accepted the counterpart's assertion that
//   they matched, would pass every static test in the document and
//   authenticate nothing.  `affirm()` is called from one place — a control a
//   person touched — and `sas()` hands six digits to a screen.
//
// ⚠ AND THE TWO UX MUSTs, WHICH ARE NOT CODE AND ARE MUSTs ANYWAY (11.7d,
//   11.9c).  The affirmative control is not pre-selected and not where a stray
//   tap lands, and the prompt asks whether the numbers MATCH rather than
//   whether to trust or continue.  A mismatch or a MAC failure is NOT reported
//   in terms that invite a retry.  `AbortAdvice` below carries the second one
//   out of C++ and into the dialogue, so the QML cannot get it wrong by
//   omission.
//
// ⚠ CA1 — X25519 IS A PARAMETER, NOT A DEPENDENCY AND NOT A CALLBACK.  libppcp
//   has no curve in it and never will (ground rule 13).  This file computes
//   `pk` and `Z` with the OpenSSL this application already links and passes 32
//   octets in; 11.11d makes those two values the ONLY things that cross the
//   boundary, and `BK`, `sas_raw`, the digits, `K_c`, the MACs, `sid` and `PRK`
//   stay on libppcp's side of it.
//
// ⚠ NO Qt AND NO CLOCK.  Like ppcp_discovery.h, this is injectable all the way
//   down: the byte stream is an interface, the clock is a parameter, the key
//   agreement is an interface.  11.3e's 30- and 60-second timers are the
//   embedding's obligation and they are here, driven from `poll(nowMs)`.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ppcp/bootstrap.h>
#include <ppcp/rv.h>

#include "ppcp_discovery.h"

namespace Ppcp {

// ── 3.3f / 3.3g — a bootstrap instance is a different TXT record ───────────
//
// The two forms are told apart by the presence of `bs`, and 3.3g is a hard
// rule with teeth: "a receiver that sees both `bs` and `rid` on one instance
// treats the instance as malformed and ignores it".  A bootstrap instance
// names no pairing because it holds none, so an instance carrying both is
// either broken or trying to be two things at once, and neither is dialled.
enum class RvInstanceKind {
    Reconnection,   // 3.3a — carries rn and rid, resolvable under 3.4b
    Bootstrap,      // 3.3f — carries bs=1, and optionally dl
    Malformed       // 3.3g — both bs and rid, or neither shape
};

RvInstanceKind classifyInstance(const RvAdvertisement &ad);

// One discovered open window.  It is a CANDIDATE and not a connection:
// nothing in this struct has been dialled and nothing about it is trusted.
struct BootstrapCandidate {
    std::string   instanceName;   // 3.2c — derived from `bn`, meaningless
    std::string   host;
    std::uint16_t port = 0;       // 3.7f — NOT the peer's PPCP listener
    std::string   pv;
    std::string   role;

    // ⛔ 3.3f/3.3g/4.4d — UNTRUSTED DISPLAY TEXT.  `dl` is shown before
    // anything has been authenticated, so it is whatever a stranger put on the
    // wire.  It is escaped for display and truncated, and it MUST NOT be used
    // as an identifier, a trust signal or a storage key.  Nothing in this
    // application keys anything on it: `instanceName` is what `begin()` takes.
    std::string   label;          // already sanitised — see sanitiseLabel()
    bool          hasLabel = false;
};

// 4.4d in full, applied to `dl` by 3.3g's reference.  Escapes control
// characters and anything that is not printable ASCII (a `dl` is a tstr and
// may be any UTF-8, and this host renders it into a QML string where a lone
// surrogate or a bidi override is a display attack), and truncates to 3.3f's
// 32 bytes.  Never returns a string a caller could mistake for a key.
std::string sanitiseLabel(const std::string &dl);

// Fills `out` from an advertisement that classifyInstance() called Bootstrap.
// False for any other kind — there is no branch here that dials a malformed
// instance or a reconnection one.
bool bootstrapCandidateFrom(const RvAdvertisement &ad, int wireMajor,
                            BootstrapCandidate *out);

// ── CA1 / §11.11 — the key agreement seam ──────────────────────────────────
//
// ⛔ 11.11d: ONLY `pk` AND `Z` CROSS THIS BOUNDARY.  The boundary is BELOW the
// derivation, not inside it.
class BootstrapKeyAgreement {
public:
    virtual ~BootstrapKeyAgreement() = default;

    // 11.5a — a FRESH keypair from a CSPRNG for every attempt, used for that
    // attempt only, never reused and never persisted.  A reused ephemeral is
    // not ephemeral.  False means no attempt begins.
    virtual bool generate(std::uint8_t pk[PPCP_RV_BS_KEY_BYTES]) = 0;

    // ⛔ 11.11f — THE RETURN VALUE IS THE FAILURE SIGNAL AND IT IS THE WHOLE
    // POINT OF THIS SIGNATURE.  "Whoever performs the agreement reports failure
    // distinguishably from success, and the caller treats a reported failure
    // and an all-zero Z identically, aborting with `invalid_key`."  A boundary
    // that dropped this signal, or reported it as a transport error, would make
    // 11.6b unimplementable on the far side of it.
    //
    // 11.11g — constant-time with respect to the private scalar.  That is an
    // obligation on the supplier that nothing downstream can check; OpenSSL's
    // X25519 satisfies it.
    virtual bool agree(const std::uint8_t peer_pk[PPCP_RV_BS_KEY_BYTES],
                       std::uint8_t z[PPCP_RV_BS_KEY_BYTES]) = 0;

    // 11.11h — the private scalar is erased by whoever holds them, when the
    // handshake ends, whether it succeeded or failed.  Idempotent.
    virtual void wipe() = 0;

    virtual std::string describe() const = 0;
};

// OpenSSL 3 `EVP_PKEY_X25519`.  The private scalar never leaves the EVP_PKEY,
// so 11.11e's clamping question does not arise here — OpenSSL clamps
// internally and we never see the scalar.
std::unique_ptr<BootstrapKeyAgreement> makeOpenSslKeyAgreement();

// ── The byte stream ────────────────────────────────────────────────────────
//
// 11.3b — "one reliable, ordered byte stream ... it carries one bootstrap
// attempt and nothing else".  Injectable so the suite can drive an attempt
// over a socketpair with no network at all, and so the gate can drive one at
// `ppcp-relay`.
class BootstrapStream {
public:
    virtual ~BootstrapStream() = default;

    // Non-blocking connect is not worth the complexity for one short-lived
    // socket a person is waiting on.  False leaves nothing open.
    virtual bool connect(const std::string &host, std::uint16_t port,
                         int timeoutMs, std::string *err) = 0;

    // >0 bytes, 0 for "nothing right now", -1 for a closed or broken stream.
    virtual long read(void *buf, std::size_t len) = 0;
    // Writes all of `len` or returns false.  A bootstrap frame is at most
    // PPCP_BS_MAX_FRAME bytes and never needs partial-write bookkeeping.
    virtual bool writeAll(const void *buf, std::size_t len) = 0;

    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual int  fd() const = 0;    // for an event loop to watch, or -1
};

std::unique_ptr<BootstrapStream> makeTcpStream();

// ── 11.9 — what the user is told, and what they are NOT offered ────────────
//
// ⛔ 11.9c IS A MUST NOT AND IT IS A UX RULE, SO IT IS CARRIED OUT OF C++ IN A
// STRUCT RATHER THAN LEFT TO THE DIALOGUE TO REMEMBER.  "A peer MUST NOT
// report an abort to its user in terms that invite a retry as the obvious next
// step where the cause was a mismatch or a MAC failure.  Those two mean either
// an implementation is wrong or someone is on the link."  A mismatch is the
// ONE signal this path produces that an attack is under way, and a dialogue
// whose reflex is *try again* converts a one-shot bound into an unbounded one
// by way of the operator's muscle memory.
struct AbortAdvice {
    ppcp_bs_reason rc = PPCP_BS_RC_MALFORMED;

    // The honest message.  11.9c's own words for the dangerous case: "the
    // numbers did not match — do not retry until you know why".
    std::string    message;

    // ⛔ FALSE MEANS THE DIALOGUE SHOWS NO RETRY AFFORDANCE AT ALL.  Not a
    // greyed-out one, not one behind a confirmation — none.  True only where
    // 11.9c says the cause "carries no such implication": a timeout or a
    // closed connection, which are the ordinary failures they look like.
    bool           mayOfferRetry = false;

    // 11.9d1 (erratum E45) — a peer aborting with `unsupported_version` offers
    // the pairing code on the FIRST abort rather than the second, because a
    // second attempt is guaranteed to fail identically: 11.4h has the initiator
    // offer the highest `v` it implements and forbids proposing lower.  11.9d's
    // two-abort threshold would spend an operator's attempt on a certainty.
    //
    // ⚠ AND THIS IS THE DIRECTION THAT ACTUALLY BREAKS.  RV-6 offers `v` as a
    // single value, so a newer INITIATOR meeting an older ACCEPTOR cannot
    // complete a guided pairing at all — and 11.9d1 notes that on this
    // deployment the host is the initiator and updates whenever the user
    // likes, while the capture peer is the acceptor and its rollout lags.
    // "Host updates first, phones catch up over the following weeks" is exactly
    // the case that breaks, and it is the default way these two ship.  So this
    // flag is not a nicety here; it is the whole fallback.
    bool           offerPairingCode = false;
};

// `declinedHere` is true when THIS user said the numbers do not match, which
// is a mismatch rather than a peer's refusal and is 11.9c's first case.
AbortAdvice adviseOnAbort(ppcp_bs_reason rc, bool declinedHere);

// 11.7a/11.7d — exactly six decimal digits with leading zeros, grouped `435
// 948`.  `000042` is a valid string and MUST be shown as six characters.  Both
// peers group identically, which is why this is a function and not a format
// string at each call site.
std::string formatSas(std::uint32_t sas);

// ── The attempt ────────────────────────────────────────────────────────────
//
// ONE ATTEMPT, ONE ENGINE, ONE KEYPAIR, ONE CONNECTION.  11.5a wants a fresh
// keypair per attempt, 11.3d1 allows one attempt at a time, 11.9b forbids
// reopening without a further explicit user action — so this object is
// single-shot and a second attempt is a second object.
enum class GuidedPhase {
    Idle,        // nothing has been dialled
    Dialling,
    Exchanging,  // frames in flight; NO digits exist and none are shown (11.7e)
    Comparing,   // 11.5e — the digits exist; THIS user is being asked
    Confirming,  // this user affirmed; the counterpart's MAC is owed
    Paired,      // 11.5g — affirmed here AND the counterpart's MAC verified
    Failed
};

class GuidedAttempt {
public:
    using Clock = std::function<std::uint64_t()>;   // milliseconds, monotonic

    GuidedAttempt(std::unique_ptr<BootstrapStream> stream,
                  std::unique_ptr<BootstrapKeyAgreement> agree,
                  Clock clock);
    ~GuidedAttempt();
    GuidedAttempt(const GuidedAttempt &) = delete;
    GuidedAttempt &operator=(const GuidedAttempt &) = delete;

    // Dials 3.7f's endpoint and sends `bs_offer` — `v` and `ct` only, never
    // `pk_i` (11.5b).  False leaves the attempt Failed and `advice()` set.
    bool begin(const BootstrapCandidate &c, std::string *err);

    // Drive from an event loop, as often as convenient.  Reads what the stream
    // has, feeds the engine, writes what it produced, and applies 11.3e's two
    // timers.  Returns false once the attempt is terminal.
    bool poll();

    // ⛔ 11.7e — refuses before the exchange has completed 11.5d.  "A peer MUST
    // NOT display any part of the digits, or any control that affirms them,
    // before it has completed 11.5d.  There is nothing to compare before then,
    // and a progressive display would leak the value to whichever side an
    // attacker reached first."  11.7f then forbids reusing, caching or
    // re-showing them once the attempt ends, so this refuses after too.
    bool sas(std::uint32_t *out) const;
    std::string sasDigits() const;   // "" outside the window above

    // ⛔ 11.7c / TRAP 8 — CALL THIS ONLY FOR AN AFFIRMATIVE ACT BY A PERSON AT
    // THIS END.  A single affirmation at one end does not establish a pairing
    // at the other, and a peer MUST NOT treat the arrival of the counterpart's
    // `bs_confirm` as standing in for its own user's.  The engine cannot tell a
    // real affirmation from a synthesised one.
    bool affirm();

    // The user said the numbers do not match.  11.9a ends the attempt; 11.9c
    // then governs what they are told, and `advice().mayOfferRetry` is false.
    void decline();

    // Any other end: a closed window, a timeout the caller noticed, the user
    // walking away.  Safe at any point and idempotent.
    void abort(ppcp_bs_reason rc);

    GuidedPhase phase() const { return m_phase; }
    bool terminal() const { return m_phase == GuidedPhase::Paired ||
                                   m_phase == GuidedPhase::Failed; }
    const AbortAdvice &advice() const { return m_advice; }
    const BootstrapCandidate &candidate() const { return m_candidate; }

    // 11.5g — valid only in Paired, and taking it ERASES IT FROM THE ENGINE
    // (trap 6; libppcp does the dangerous half).  False otherwise, and in
    // particular false for a handshake that computed the whole chain and then
    // aborted, where 11.6f as amended by E51 has already wiped `PRK`.
    bool takePairing(ppcp_bs_pairing *out);

private:
    void fail(ppcp_bs_reason rc, bool declinedHere);
    bool applyStep(const ppcp_bs_step &step);
    bool supplySecret(const std::uint8_t peer_pk[PPCP_RV_BS_KEY_BYTES]);

    std::unique_ptr<BootstrapStream>       m_stream;
    std::unique_ptr<BootstrapKeyAgreement> m_agree;
    Clock                                  m_clock;

    ppcp_bs_engine     m_engine{};
    bool               m_engineLive = false;
    GuidedPhase        m_phase = GuidedPhase::Idle;
    AbortAdvice        m_advice;
    BootstrapCandidate m_candidate;

    std::vector<std::uint8_t> m_in;     // partial frame carry-over
    std::uint64_t m_startedMs = 0;
    std::uint64_t m_comparingSinceMs = 0;
    bool          m_declinedHere = false;
    bool          m_havePairing = false;
    ppcp_bs_pairing m_pairing{};
};

// ── 11.3d1 / TRAP 3 — the browse half, and the one door to an attempt ──────
//
// ⛔ NOTHING IN THIS CLASS DIALS A CANDIDATE.  Candidates arrive from the
// browser, are held, and are shown.  `begin()` is the ONLY function that opens
// a socket, it takes ONE instance name, and it refuses while an attempt is
// live.  "The user selects one BEFORE the attempt begins" is therefore a
// property of the type rather than a rule a call site has to remember.
class GuidedPairing {
public:
    using StreamFactory = std::function<std::unique_ptr<BootstrapStream>()>;
    using AgreeFactory  = std::function<std::unique_ptr<BootstrapKeyAgreement>()>;

    GuidedPairing();
    ~GuidedPairing();

    // Defaults are makeTcpStream / makeOpenSslKeyAgreement / a steady clock.
    void setFactories(StreamFactory sf, AgreeFactory af);
    void setClock(GuidedAttempt::Clock c);

    // Feed the browser's results here.  A malformed instance (3.3g) and a
    // reconnection instance are both ignored, silently — 3.6a makes discovery
    // failure not an error state, and a record we will not dial is not one
    // either.  Returns true when the candidate list changed.
    bool noteAdvertisement(const RvAdvertisement &ad, int wireMajor);
    bool dropInstance(const std::string &instanceName);
    void clearCandidates();

    std::vector<BootstrapCandidate> candidates() const;

    // ⛔ THE ONE DOOR.  Refuses — false, with `whyNot` set — when an attempt is
    // already live (11.3d1) or the name is not a held candidate.  There is no
    // overload that takes a list, and adding one would be the trap.
    bool begin(const std::string &instanceName, std::string *whyNot);

    bool attemptInProgress() const;
    GuidedAttempt *attempt() { return m_attempt.get(); }
    const GuidedAttempt *attempt() const { return m_attempt.get(); }

    // Drops a terminal attempt so a further explicit user action can start a
    // new one (11.9b).  Does nothing while one is live: an attempt is ended by
    // abort(), decline() or completion, never by being forgotten.
    void endAttempt();

    // 11.9d1 / 11.9d — how many aborts this sitting has seen, and whether the
    // pairing code should be offered instead.  An `unsupported_version` sets it
    // on the FIRST abort; anything else on the second.
    bool shouldOfferPairingCode() const { return m_offerCode; }
    std::size_t abortsThisSitting() const { return m_aborts; }
    void resetSitting();

private:
    StreamFactory m_sf;
    AgreeFactory  m_af;
    GuidedAttempt::Clock m_clock;
    std::vector<BootstrapCandidate> m_candidates;
    std::unique_ptr<GuidedAttempt>  m_attempt;
    std::size_t m_aborts = 0;
    bool        m_offerCode = false;
};

}  // namespace Ppcp
